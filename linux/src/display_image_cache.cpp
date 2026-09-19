#include "display_image_cache.h"

#include "image.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

struct FileIdentity {
	std::uint64_t device = 0;
	std::uint64_t inode = 0;
	std::uint64_t size = 0;
	std::int64_t modifiedSeconds = 0;
	std::int64_t modifiedNanoseconds = 0;
	bool valid = false;
};

FileIdentity Identify(const fs::path& filename) {
	struct stat status{};
	if (::stat(filename.c_str(), &status) != 0 || status.st_size < 0) return {};
	FileIdentity result;
	result.device = static_cast<std::uint64_t>(status.st_dev);
	result.inode = static_cast<std::uint64_t>(status.st_ino);
	result.size = static_cast<std::uint64_t>(status.st_size);
	result.modifiedSeconds = status.st_mtim.tv_sec;
	result.modifiedNanoseconds = status.st_mtim.tv_nsec;
	result.valid = true;
	return result;
}

std::string NormalizedPath(const fs::path& filename) {
	std::error_code error;
	const fs::path absolute = fs::absolute(filename, error);
	return (error ? filename : absolute).lexically_normal().string();
}

std::string RequestKey(const fs::path& filename, const FileIdentity& identity,
	std::size_t frameIndex, int width, int height, bool autoContrast) {
	if (!identity.valid) return {};
	std::ostringstream key;
	key << NormalizedPath(filename) << '\n'
		<< identity.device << ':' << identity.inode << ':' << identity.size << ':'
		<< identity.modifiedSeconds << ':' << identity.modifiedNanoseconds << '\n'
		<< frameIndex << ':' << width << 'x' << height << ':' << autoContrast;
	return key.str();
}

DisplayImageCache::ImagePtr PrepareDisplayImage(const DisplayImageRequest& request) {
	if (!request.Valid()) return {};
	const DecodedFrame& decodedFrame = request.decoded->frames[request.frameIndex];
	Image image;
	if (!image.StoreBGRA(decodedFrame.bgra.data(), decodedFrame.width, decodedFrame.height)) return {};
	if (request.autoContrast && !image.AutoContrast()) return {};
	if ((request.targetWidth < image.width || request.targetHeight < image.height) &&
		!image.Resize(request.targetWidth, request.targetHeight)) return {};

	auto prepared = std::make_shared<PreparedDisplayImage>();
	prepared->key = request.key;
	prepared->width = image.width;
	prepared->height = image.height;
	prepared->bgra = std::move(image.bgra);
	return prepared;
}

} // namespace

bool DisplayImageRequest::Valid() const {
	if (key.empty() || !decoded || frameIndex >= decoded->frames.size() ||
		targetWidth <= 0 || targetHeight <= 0) return false;
	const DecodedFrame& frame = decoded->frames[frameIndex];
	return frame.width > 0 && frame.height > 0 && !frame.bgra.empty();
}

DisplayImageRequest MakeDisplayImageRequest(const fs::path& filename,
	const std::shared_ptr<const DecodedImage>& decoded, std::size_t frameIndex,
	int targetWidth, int targetHeight, bool autoContrast) {
	DisplayImageRequest request;
	request.filename = filename;
	request.decoded = decoded;
	request.frameIndex = frameIndex;
	request.targetWidth = targetWidth;
	request.targetHeight = targetHeight;
	request.autoContrast = autoContrast;
	if (!decoded || frameIndex >= decoded->frames.size() || targetWidth <= 0 || targetHeight <= 0) {
		return request;
	}
	request.key = RequestKey(filename, Identify(filename), frameIndex,
		targetWidth, targetHeight, autoContrast);
	return request;
}

std::size_t PreparedDisplayImageBytes(const PreparedDisplayImage& image) {
	return image.bgra.size();
}

struct DisplayImageCache::Impl {
	struct Entry {
		ImagePtr image;
		std::size_t bytes = 0;
		std::uint64_t lastUsed = 0;
	};

	struct Work {
		DisplayImageRequest request;
		std::uint64_t generation = 0;
		std::uint64_t epoch = 0;
		bool foreground = false;
	};

	explicit Impl(std::size_t budget, std::size_t requestedWorkers, Processor prepare)
		: byteBudget(budget), processor(std::move(prepare)) {
		if (!processor) processor = PrepareDisplayImage;
		std::size_t count = requestedWorkers;
		if (count == 0) {
			const unsigned int hardwareThreads = std::thread::hardware_concurrency();
			count = hardwareThreads > 2 ? std::min<std::size_t>(4, hardwareThreads - 1) : 1;
		}
		count = std::max<std::size_t>(1, count);
		workers.reserve(count);
		for (std::size_t index = 0; index < count; ++index) {
			workers.emplace_back([this] { Run(); });
		}
	}

	~Impl() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			++generation;
			++epoch;
			queue.clear();
			queuedKeys.clear();
		}
		workAvailable.notify_all();
		for (std::thread& worker : workers) {
			if (worker.joinable()) worker.join();
		}
	}

	void Erase(std::unordered_map<std::string, Entry>::iterator entry) {
		cachedBytes -= entry->second.bytes;
		entries.erase(entry);
	}

	bool Insert(const ImagePtr& image, bool mayEvict) {
		if (!image || image->key.empty()) return false;
		const std::size_t bytes = PreparedDisplayImageBytes(*image);
		if (bytes == 0 || bytes > byteBudget) return false;
		const auto existing = entries.find(image->key);
		if (existing != entries.end()) Erase(existing);
		if (!mayEvict && bytes > byteBudget - cachedBytes) return false;
		while (bytes > byteBudget - cachedBytes && !entries.empty()) {
			auto oldest = entries.begin();
			for (auto candidate = std::next(entries.begin()); candidate != entries.end(); ++candidate) {
				if (candidate->second.lastUsed < oldest->second.lastUsed) oldest = candidate;
			}
			Erase(oldest);
		}
		if (bytes > byteBudget - cachedBytes) return false;
		entries.emplace(image->key, Entry{image, bytes, ++useCounter});
		cachedBytes += bytes;
		return true;
	}

	void Run() {
		// Keep speculative scaling below the UI and decoder threads in scheduler
		// priority while still allowing several independent frames to use CPUs.
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
		for (;;) {
			Work work;
			{
				std::unique_lock<std::mutex> lock(mutex);
				workAvailable.wait(lock, [this] { return stopping || !queue.empty(); });
				if (stopping) return;
				work = std::move(queue.front());
				queue.pop_front();
				queuedKeys.erase(work.request.key);
				inFlightKeys.insert(work.request.key);
				++activeWorkers;
			}

			ImagePtr image = processor(work.request);
			const std::string currentKey = RequestKey(work.request.filename,
				Identify(work.request.filename), work.request.frameIndex,
				work.request.targetWidth, work.request.targetHeight,
				work.request.autoContrast);

			{
				std::lock_guard<std::mutex> lock(mutex);
				inFlightKeys.erase(work.request.key);
				--activeWorkers;
				const bool foreground = work.foreground ||
					foregroundKeys.erase(work.request.key) != 0;
				if (!stopping && work.epoch == epoch && image && image->key == currentKey &&
					(foreground || desiredPrefetchKeys.find(work.request.key) !=
						desiredPrefetchKeys.end()) &&
					Insert(image, foreground)) {
					completed.push_back(image);
				}
				idle.notify_all();
			}
		}
	}

	const std::size_t byteBudget;
	Processor processor;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable idle;
	std::vector<std::thread> workers;
	std::unordered_map<std::string, Entry> entries;
	std::deque<Work> queue;
	std::deque<ImagePtr> completed;
	std::unordered_set<std::string> queuedKeys;
	std::unordered_set<std::string> inFlightKeys;
	std::unordered_set<std::string> desiredPrefetchKeys;
	std::unordered_set<std::string> foregroundKeys;
	std::size_t cachedBytes = 0;
	std::size_t activeWorkers = 0;
	std::uint64_t useCounter = 0;
	std::uint64_t generation = 0;
	std::uint64_t epoch = 0;
	bool stopping = false;
};

DisplayImageCache::DisplayImageCache(std::size_t byteBudget,
	std::size_t workerCount, Processor processor)
	: impl_(std::make_unique<Impl>(byteBudget, workerCount, std::move(processor))) {}

DisplayImageCache::~DisplayImageCache() = default;

DisplayImageCache::ImagePtr DisplayImageCache::Find(const DisplayImageRequest& request) {
	if (!request.Valid()) return {};
	if (request.key != RequestKey(request.filename, Identify(request.filename),
		request.frameIndex, request.targetWidth, request.targetHeight,
		request.autoContrast)) return {};
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto found = impl_->entries.find(request.key);
	if (found == impl_->entries.end()) return {};
	found->second.lastUsed = ++impl_->useCounter;
	return found->second.image;
}

void DisplayImageCache::Request(const DisplayImageRequest& request) {
	if (!request.Valid()) return;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->entries.find(request.key) != impl_->entries.end()) return;
		if (impl_->inFlightKeys.find(request.key) != impl_->inFlightKeys.end()) {
			impl_->foregroundKeys.insert(request.key);
			return;
		}
		if (impl_->queuedKeys.find(request.key) != impl_->queuedKeys.end()) {
			const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
				[&request](const Impl::Work& work) { return work.request.key == request.key; });
			if (queued != impl_->queue.end()) {
				Impl::Work promoted = std::move(*queued);
				impl_->queue.erase(queued);
				promoted.foreground = true;
				promoted.epoch = impl_->epoch;
				impl_->queue.push_front(std::move(promoted));
			}
			return;
		}
		impl_->queue.push_front(Impl::Work{request, 0, impl_->epoch, true});
		impl_->queuedKeys.insert(request.key);
	}
	impl_->workAvailable.notify_one();
}

void DisplayImageCache::Prefetch(const std::vector<DisplayImageRequest>& requests) {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		++impl_->generation;
		impl_->queue.erase(std::remove_if(impl_->queue.begin(), impl_->queue.end(),
			[](const Impl::Work& work) { return !work.foreground; }), impl_->queue.end());
		impl_->queuedKeys.clear();
		impl_->desiredPrefetchKeys.clear();
		for (const Impl::Work& work : impl_->queue) impl_->queuedKeys.insert(work.request.key);
		for (const DisplayImageRequest& request : requests) {
			if (!request.Valid()) continue;
			impl_->desiredPrefetchKeys.insert(request.key);
			if (impl_->entries.find(request.key) != impl_->entries.end() ||
				impl_->queuedKeys.find(request.key) != impl_->queuedKeys.end() ||
				impl_->inFlightKeys.find(request.key) != impl_->inFlightKeys.end()) continue;
			impl_->queue.push_back(Impl::Work{request, impl_->generation, impl_->epoch, false});
			impl_->queuedKeys.insert(request.key);
		}
		impl_->idle.notify_all();
	}
	impl_->workAvailable.notify_all();
}

std::vector<DisplayImageCache::ImagePtr> DisplayImageCache::TakeCompleted(std::size_t maximumCount) {
	std::vector<ImagePtr> result;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const std::size_t count = std::min(maximumCount, impl_->completed.size());
	result.reserve(count);
	for (std::size_t index = 0; index < count; ++index) {
		result.push_back(std::move(impl_->completed.front()));
		impl_->completed.pop_front();
	}
	return result;
}

void DisplayImageCache::Release(const std::string& key) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto found = impl_->entries.find(key);
	if (found != impl_->entries.end()) impl_->Erase(found);
}

void DisplayImageCache::Clear() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	++impl_->generation;
	++impl_->epoch;
	impl_->queue.clear();
	impl_->completed.clear();
	impl_->queuedKeys.clear();
	impl_->desiredPrefetchKeys.clear();
	impl_->foregroundKeys.clear();
	impl_->entries.clear();
	impl_->cachedBytes = 0;
	impl_->idle.notify_all();
}

std::size_t DisplayImageCache::CachedBytes() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->cachedBytes;
}

std::size_t DisplayImageCache::CachedImages() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->entries.size();
}

bool DisplayImageCache::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(impl_->mutex);
	return impl_->idle.wait_for(lock, timeout, [this] {
		return impl_->queue.empty() && impl_->activeWorkers == 0;
	});
}

} // namespace jpegview_linux
