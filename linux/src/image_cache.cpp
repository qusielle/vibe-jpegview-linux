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

	explicit Impl(std::size_t budget, Decoder decode,
		std::shared_ptr<SharedCacheBudget> shared, std::size_t workerCount)
		: byteBudget(budget), decoder(std::move(decode)), sharedBudget(std::move(shared)) {
		if (!decoder) decoder = DecodeImage;
		workers.reserve(std::max<std::size_t>(1, workerCount));
		for (std::size_t index = 0; index < std::max<std::size_t>(1, workerCount); ++index) {
			workers.emplace_back([this] { Run(); });
		}
		retirementWorker = std::thread([this] { Retire(); });
	}

	~Impl() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			++generation;
			queue.clear();
		}
		workAvailable.notify_all();
		retirementAvailable.notify_all();
		for (std::thread& worker : workers) {
			if (worker.joinable()) worker.join();
		}
		if (retirementWorker.joinable()) retirementWorker.join();
		if (sharedBudget) sharedBudget->Release(cachedBytes);
	}

	void Erase(std::unordered_map<std::string, Entry>::iterator entry) {
		const std::size_t bytes = entry->second.bytes;
		ImagePtr retiredImage = std::move(entry->second.image);
		entries.erase(entry);
		cachedBytes -= bytes;
		if (sharedBudget) sharedBudget->Release(bytes);
		if (retiredImage) {
			retired.push_back(std::move(retiredImage));
			retirementAvailable.notify_one();
		}
	}

	std::unordered_map<std::string, Entry>::iterator Oldest() {
		if (entries.empty()) return entries.end();
		auto oldest = entries.begin();
		for (auto candidate = std::next(entries.begin()); candidate != entries.end(); ++candidate) {
			if (candidate->second.lastUsed < oldest->second.lastUsed) oldest = candidate;
		}
		return oldest;
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
			Erase(Oldest());
		}
		if (bytes > byteBudget - cachedBytes) return false;
		if (sharedBudget) {
			while (!sharedBudget->TryReserve(bytes)) {
				if (!mayEvict || entries.empty()) return false;
				Erase(Oldest());
			}
		}
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
				queuedKeys.erase(work.key);
				inFlightKeys.insert(work.key);
				++activeWorkers;
			}

			auto image = std::make_shared<DecodedImage>();
			std::string errorMessage;
			const bool decoded = decoder(work.filename, *image, errorMessage) && !image->frames.empty();
			const FileIdentity currentIdentity = decoded ? Identify(work.filename) : FileIdentity{};

			ImagePtr completedImage;
			Completion completion;
			{
				std::lock_guard<std::mutex> lock(mutex);
				inFlightKeys.erase(work.key);
				const bool foreground = foregroundKeys.erase(work.key) != 0;
				if (!stopping && decoded && currentIdentity == work.identity) {
					// Speculative work must never evict an image the user has
					// already viewed. Stop this generation once the free budget
					// cannot hold its next nearest neighbor.
					if (!Insert(work.key, work.identity, image, foreground)) {
						queue.clear();
						queuedKeys.clear();
					} else {
						completedImage = image;
						const auto desired = desiredWork.find(work.key);
						if (desired != desiredWork.end() && desired->second.identity == work.identity) {
							completion = desired->second.completion;
						}
					}
				}
				--activeWorkers;
				idle.notify_all();
			}
			if (completion && completedImage) completion(work.filename, completedImage);
		}
	}

	void Retire() {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
		for (;;) {
			ImagePtr image;
			{
				std::unique_lock<std::mutex> lock(mutex);
				retirementAvailable.wait(lock, [this] { return stopping || !retired.empty(); });
				if (stopping) return;
				image = std::move(retired.front());
				retired.pop_front();
			}
			image.reset();
		}
	}

	const std::size_t byteBudget;
	Decoder decoder;
	std::shared_ptr<SharedCacheBudget> sharedBudget;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable retirementAvailable;
	std::condition_variable idle;
	std::vector<std::thread> workers;
	std::thread retirementWorker;
	std::unordered_map<std::string, Entry> entries;
	std::deque<Work> queue;
	std::deque<ImagePtr> retired;
	std::unordered_set<std::string> queuedKeys;
	std::unordered_set<std::string> inFlightKeys;
	std::unordered_set<std::string> foregroundKeys;
	std::unordered_map<std::string, Work> desiredWork;
	std::size_t cachedBytes = 0;
	std::size_t activeWorkers = 0;
	std::uint64_t useCounter = 0;
	std::uint64_t generation = 0;
	bool stopping = false;
};

DecodedImageCache::DecodedImageCache(std::size_t byteBudget, Decoder decoder,
	std::shared_ptr<SharedCacheBudget> sharedBudget, std::size_t workerCount)
	: impl_(std::make_unique<Impl>(byteBudget, std::move(decoder), std::move(sharedBudget),
		workerCount)) {}

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

DecodedImageCache::ImagePtr DecodedImageCache::FindOrWait(const fs::path& filename) {
	const std::string key = CacheKey(filename);
	const FileIdentity identity = Identify(filename);
	std::unique_lock<std::mutex> lock(impl_->mutex);
	const auto findValid = [&]() -> ImagePtr {
		const auto found = impl_->entries.find(key);
		if (found == impl_->entries.end()) return {};
		if (!(found->second.identity == identity)) {
			impl_->Erase(found);
			return {};
		}
		found->second.lastUsed = ++impl_->useCounter;
		return found->second.image;
	};
	if (ImagePtr cached = findValid()) return cached;

	const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
		[&key](const Impl::Work& work) { return work.key == key; });
	const bool inFlight = impl_->inFlightKeys.find(key) != impl_->inFlightKeys.end();
	if (queued == impl_->queue.end() && !inFlight) return {};
	impl_->foregroundKeys.insert(key);
	if (queued != impl_->queue.end() && queued != impl_->queue.begin()) {
		Impl::Work promoted = std::move(*queued);
		impl_->queue.erase(queued);
		impl_->queue.push_front(std::move(promoted));
	}
	impl_->workAvailable.notify_one();
	impl_->idle.wait(lock, [&] {
		return impl_->stopping || impl_->entries.find(key) != impl_->entries.end() ||
			(impl_->queuedKeys.find(key) == impl_->queuedKeys.end() &&
			 impl_->inFlightKeys.find(key) == impl_->inFlightKeys.end());
	});
	return findValid();
}

void DecodedImageCache::Store(const fs::path& filename,
	const std::shared_ptr<DecodedImage>& image) {
	const std::string key = CacheKey(filename);
	const FileIdentity identity = Identify(filename);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->Insert(key, identity, image, true);
	}
	impl_->workAvailable.notify_one();
}

void DecodedImageCache::Prefetch(const std::vector<fs::path>& files,
	std::size_t currentIndex, int preferredDirection, std::size_t maximumCount,
	Completion completion, Filter filter) {
	std::vector<Impl::Work> prepared;
	std::vector<std::pair<fs::path, ImagePtr>> alreadyCached;
	for (const std::size_t index : ImagePrefetchOrder(files.size(), currentIndex,
		preferredDirection, maximumCount)) {
		Impl::Work work;
		work.filename = files[index];
		if (filter && !filter(work.filename)) continue;
		work.key = CacheKey(work.filename);
		work.identity = Identify(work.filename);
		work.completion = completion;
		if (work.identity.valid) prepared.push_back(std::move(work));
	}
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		++impl_->generation;
		impl_->queue.clear();
		impl_->queuedKeys.clear();
		impl_->desiredWork.clear();
		for (Impl::Work& work : prepared) {
			work.generation = impl_->generation;
			impl_->desiredWork[work.key] = work;
			const auto cached = impl_->entries.find(work.key);
			if (cached != impl_->entries.end() && cached->second.identity == work.identity) {
				if (completion) alreadyCached.emplace_back(work.filename, cached->second.image);
				continue;
			}
			if (cached != impl_->entries.end()) impl_->Erase(cached);
			if (impl_->inFlightKeys.find(work.key) != impl_->inFlightKeys.end()) continue;
			impl_->queue.push_back(std::move(work));
			impl_->queuedKeys.insert(impl_->queue.back().key);
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
	impl_->queuedKeys.clear();
	impl_->desiredWork.clear();
	impl_->foregroundKeys.clear();
	while (!impl_->entries.empty()) impl_->Erase(impl_->entries.begin());
	impl_->idle.notify_all();
	impl_->workAvailable.notify_one();
}

std::size_t DecodedImageCache::CachedBytes() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->cachedBytes;
}

std::size_t DecodedImageCache::CachedImages() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->entries.size();
}

std::size_t DecodedImageCache::EvictLeastRecentlyUsed() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto oldest = impl_->Oldest();
	if (oldest == impl_->entries.end()) return 0;
	const std::size_t bytes = oldest->second.bytes;
	impl_->Erase(oldest);
	impl_->workAvailable.notify_one();
	return bytes;
}

bool DecodedImageCache::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(impl_->mutex);
	return impl_->idle.wait_for(lock, timeout, [this] {
		return impl_->queue.empty() && impl_->activeWorkers == 0;
	});
}

} // namespace jpegview_linux
