#include "image_cache.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <mutex>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
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

bool operator==(const FileIdentity& left, const FileIdentity& right) {
	return left.valid && right.valid && left.device == right.device &&
		left.inode == right.inode && left.size == right.size &&
		left.modifiedSeconds == right.modifiedSeconds &&
		left.modifiedNanoseconds == right.modifiedNanoseconds;
}

FileIdentity Identify(const fs::path& filename) {
	struct stat status{};
	if (::stat(filename.c_str(), &status) != 0 || status.st_size < 0) return {};
	FileIdentity result;
	result.device = static_cast<std::uint64_t>(status.st_dev);
	result.inode = static_cast<std::uint64_t>(status.st_ino);
	result.size = static_cast<std::uint64_t>(status.st_size);
#if defined(__linux__)
	result.modifiedSeconds = status.st_mtim.tv_sec;
	result.modifiedNanoseconds = status.st_mtim.tv_nsec;
#else
	result.modifiedSeconds = status.st_mtime;
#endif
	result.valid = true;
	return result;
}

std::string CacheKey(const fs::path& filename) {
	std::error_code error;
	const fs::path absolute = fs::absolute(filename, error);
	return (error ? filename : absolute).lexically_normal().string();
}

} // namespace

std::size_t DecodedImageBytes(const DecodedImage& image) {
	std::size_t bytes = 0;
	for (const DecodedFrame& frame : image.frames) {
		if (frame.bgra.size() > static_cast<std::size_t>(-1) - bytes) {
			return static_cast<std::size_t>(-1);
		}
		bytes += frame.bgra.size();
	}
	return bytes;
}

std::vector<std::size_t> ImagePrefetchOrder(std::size_t fileCount,
	std::size_t currentIndex, int preferredDirection, std::size_t maximumCount) {
	std::vector<std::size_t> result;
	if (fileCount < 2 || currentIndex >= fileCount || maximumCount == 0) return result;
	result.reserve(std::min(maximumCount, fileCount - 1));
	std::vector<bool> visited(fileCount, false);
	visited[currentIndex] = true;
	const int direction = preferredDirection < 0 ? -1 : 1;
	for (std::size_t distance = 1; result.size() < maximumCount && result.size() + 1 < fileCount;
		++distance) {
		for (const int sign : {direction, -direction}) {
			const std::size_t offset = distance % fileCount;
			const std::size_t candidate = sign > 0 ?
				(currentIndex + offset) % fileCount :
				(currentIndex + fileCount - offset) % fileCount;
			if (visited[candidate]) continue;
			visited[candidate] = true;
			result.push_back(candidate);
			if (result.size() >= maximumCount || result.size() + 1 >= fileCount) break;
		}
	}
	return result;
}

struct DecodedImageCache::Impl {
	struct Entry {
		ImagePtr image;
		FileIdentity identity;
		std::size_t bytes = 0;
		std::uint64_t lastUsed = 0;
	};

	struct Work {
		fs::path filename;
		std::string key;
		FileIdentity identity;
		std::uint64_t generation = 0;
		Completion completion;
	};

	explicit Impl(std::size_t budget, Decoder decode)
		: byteBudget(budget), decoder(std::move(decode)) {
		if (!decoder) decoder = DecodeImage;
		worker = std::thread([this] { Run(); });
	}

	~Impl() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			++generation;
			queue.clear();
		}
		workAvailable.notify_all();
		if (worker.joinable()) worker.join();
	}

	void Erase(std::unordered_map<std::string, Entry>::iterator entry) {
		cachedBytes -= entry->second.bytes;
		entries.erase(entry);
	}

	bool Insert(const std::string& key, const FileIdentity& identity,
		const std::shared_ptr<DecodedImage>& image, bool mayEvict) {
		if (!image || image->frames.empty() || !identity.valid) return false;
		const std::size_t bytes = DecodedImageBytes(*image);
		if (bytes == 0 || bytes > byteBudget) return false;
		const auto existing = entries.find(key);
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
		entries.emplace(key, Entry{image, identity, bytes, ++useCounter});
		cachedBytes += bytes;
		return true;
	}

	void Run() {
		// Decoding should consume otherwise-idle CPU without competing equally
		// with input, painting, or the synchronous decoder on the UI thread.
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
		for (;;) {
			Work work;
			{
				std::unique_lock<std::mutex> lock(mutex);
				workAvailable.wait(lock, [this] { return stopping || !queue.empty(); });
				if (stopping) return;
				work = std::move(queue.front());
				queue.pop_front();
				decoding = true;
			}

			auto image = std::make_shared<DecodedImage>();
			std::string errorMessage;
			const bool decoded = decoder(work.filename, *image, errorMessage) && !image->frames.empty();
			const FileIdentity currentIdentity = decoded ? Identify(work.filename) : FileIdentity{};

			ImagePtr completedImage;
			Completion completion;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (!stopping && decoded && work.generation == generation &&
					currentIdentity == work.identity) {
					// Speculative work must never evict an image the user has
					// already viewed. Stop this generation once the free budget
					// cannot hold its next nearest neighbor.
					if (!Insert(work.key, work.identity, image, false)) {
						queue.clear();
					} else {
						completedImage = image;
						completion = std::move(work.completion);
					}
				}
			}
			if (completion && completedImage) completion(work.filename, completedImage);
			{
				std::lock_guard<std::mutex> lock(mutex);
				decoding = false;
				idle.notify_all();
			}
		}
	}

	const std::size_t byteBudget;
	Decoder decoder;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable idle;
	std::thread worker;
	std::unordered_map<std::string, Entry> entries;
	std::deque<Work> queue;
	std::size_t cachedBytes = 0;
	std::uint64_t useCounter = 0;
	std::uint64_t generation = 0;
	bool decoding = false;
	bool stopping = false;
};

DecodedImageCache::DecodedImageCache(std::size_t byteBudget, Decoder decoder)
	: impl_(std::make_unique<Impl>(byteBudget, std::move(decoder))) {}

DecodedImageCache::~DecodedImageCache() = default;

DecodedImageCache::ImagePtr DecodedImageCache::Find(const fs::path& filename) {
	const std::string key = CacheKey(filename);
	const FileIdentity identity = Identify(filename);
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto found = impl_->entries.find(key);
	if (found == impl_->entries.end()) return {};
	if (!(found->second.identity == identity)) {
		impl_->Erase(found);
		return {};
	}
	found->second.lastUsed = ++impl_->useCounter;
	return found->second.image;
}

void DecodedImageCache::Store(const fs::path& filename,
	const std::shared_ptr<DecodedImage>& image) {
	const std::string key = CacheKey(filename);
	const FileIdentity identity = Identify(filename);
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->Insert(key, identity, image, true);
}

void DecodedImageCache::Prefetch(const std::vector<fs::path>& files,
	std::size_t currentIndex, int preferredDirection, std::size_t maximumCount,
	Completion completion) {
	std::vector<Impl::Work> prepared;
	std::vector<std::pair<fs::path, ImagePtr>> alreadyCached;
	for (const std::size_t index : ImagePrefetchOrder(files.size(), currentIndex,
		preferredDirection, maximumCount)) {
		Impl::Work work;
		work.filename = files[index];
		work.key = CacheKey(work.filename);
		work.identity = Identify(work.filename);
		work.completion = completion;
		if (work.identity.valid) prepared.push_back(std::move(work));
	}
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		++impl_->generation;
		impl_->queue.clear();
		for (Impl::Work& work : prepared) {
			const auto cached = impl_->entries.find(work.key);
			if (cached != impl_->entries.end() && cached->second.identity == work.identity) {
				if (completion) alreadyCached.emplace_back(work.filename, cached->second.image);
				continue;
			}
			work.generation = impl_->generation;
			impl_->queue.push_back(std::move(work));
		}
		impl_->idle.notify_all();
	}
	impl_->workAvailable.notify_one();
	if (completion) {
		for (const auto& cached : alreadyCached) completion(cached.first, cached.second);
	}
}

void DecodedImageCache::Clear() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	++impl_->generation;
	impl_->queue.clear();
	impl_->entries.clear();
	impl_->cachedBytes = 0;
	impl_->idle.notify_all();
}

std::size_t DecodedImageCache::CachedBytes() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->cachedBytes;
}

std::size_t DecodedImageCache::CachedImages() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->entries.size();
}

bool DecodedImageCache::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(impl_->mutex);
	return impl_->idle.wait_for(lock, timeout, [this] {
		return impl_->queue.empty() && !impl_->decoding;
	});
}

} // namespace jpegview_linux
