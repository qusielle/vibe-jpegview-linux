#include "display_image_cache.h"

#include "image.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <iomanip>
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
	std::size_t frameIndex, int width, int height, bool autoContrast,
	const ImageProcessingParams& processing) {
	if (!identity.valid) return {};
	const bool localDensity = processing.localDensityEnabled &&
		(processing.lightenShadows > 0.0 || processing.darkenHighlights > 0.0);
	const bool unsharp = processing.unsharpRadius > 0.0 && processing.unsharpAmount > 0.0;
	std::ostringstream key;
	key << std::setprecision(17);
	key << NormalizedPath(filename) << '\n'
		<< identity.device << ':' << identity.inode << ':' << identity.size << ':'
		<< identity.modifiedSeconds << ':' << identity.modifiedNanoseconds << '\n'
		<< frameIndex << ':' << width << 'x' << height << ':' << autoContrast << ':'
		<< processing.contrast << ':' << processing.gamma << ':' << processing.saturation << ':'
		<< processing.cyanRed << ':' << processing.magentaGreen << ':' << processing.yellowBlue << ':'
		<< (localDensity ? processing.lightenShadows : 0.0) << ':' <<
		(localDensity ? processing.darkenHighlights : 0.0) << ':' <<
		(localDensity ? processing.deepShadows : 0.0) << ':' <<
		(autoContrast ? processing.colorCorrection : 0.0) << ':' <<
		(autoContrast ? processing.contrastCorrection : 0.0) << ':' << processing.sharpen << ':' <<
		(unsharp ? processing.unsharpRadius : 0.0) << ':' <<
		(unsharp ? processing.unsharpAmount : 0.0) << ':' <<
		(unsharp ? processing.unsharpThreshold : 0.0) << ':' << localDensity;
	return key.str();
}

DisplayImageCache::ImagePtr PrepareDisplayImage(const DisplayImageRequest& request) {
	if (!request.Valid()) return {};
	DecodedImage displayDecoded;
	const DecodedFrame* decodedFrame = nullptr;
	if (request.decoded) {
		decodedFrame = &request.decoded->frames[request.frameIndex];
	} else {
		int sourceWidth = 0;
		int sourceHeight = 0;
		std::string errorMessage;
		if (!DecodeJpegForDisplay(request.filename, request.targetWidth, request.targetHeight,
			displayDecoded, sourceWidth, sourceHeight, errorMessage) ||
			displayDecoded.frames.empty() || sourceWidth != request.sourceWidth ||
			sourceHeight != request.sourceHeight) return {};
		decodedFrame = &displayDecoded.frames.front();
	}
	Image image;
	if (request.decoded) {
		if (!image.StoreBGRA(decodedFrame->bgra.data(), decodedFrame->width,
			decodedFrame->height, decodedFrame->hasTransparency)) return {};
	} else {
		// File-backed JPEG pixels are private to this worker. Move them into the
		// resize stage instead of copying the reduced decode a second time.
		DecodedFrame& ownedFrame = displayDecoded.frames.front();
		image.width = image.originalWidth = ownedFrame.width;
		image.height = image.originalHeight = ownedFrame.height;
		image.bgra = std::move(ownedFrame.bgra);
		image.hasTransparency = ownedFrame.hasTransparency;
	}
	if (!image.ApplyProcessing(request.processing, request.autoContrast)) return {};
	if ((request.targetWidth < image.width || request.targetHeight < image.height) &&
		!image.Resize(request.targetWidth, request.targetHeight)) return {};

	auto prepared = std::make_shared<PreparedDisplayImage>();
	prepared->filename = request.filename;
	prepared->key = request.key;
	prepared->width = image.width;
	prepared->height = image.height;
	prepared->hasTransparency = image.hasTransparency;
	prepared->bgra = std::move(image.bgra);
	prepared->priority = request.priority;
	return prepared;
}

} // namespace

bool DisplayImageRequest::Valid() const {
	if (key.empty() ||
		targetWidth <= 0 || targetHeight <= 0) return false;
	if (!decoded) return sourceWidth > 0 && sourceHeight > 0 && IsJpegPath(filename);
	if (frameIndex >= decoded->frames.size()) return false;
	const DecodedFrame& frame = decoded->frames[frameIndex];
	return frame.width > 0 && frame.height > 0 && !frame.bgra.empty();
}

DisplayImageRequest MakeDisplayImageRequest(const fs::path& filename,
	const std::shared_ptr<const DecodedImage>& decoded, std::size_t frameIndex,
	int targetWidth, int targetHeight, bool autoContrast, std::size_t priority,
	const ImageProcessingParams& processing) {
	DisplayImageRequest request;
	request.filename = filename;
	request.decoded = decoded;
	request.frameIndex = frameIndex;
	request.targetWidth = targetWidth;
	request.targetHeight = targetHeight;
	request.autoContrast = autoContrast;
	request.processing = processing;
	request.priority = priority;
	if (!decoded || frameIndex >= decoded->frames.size() || targetWidth <= 0 || targetHeight <= 0) {
		return request;
	}
	request.sourceWidth = decoded->frames[frameIndex].width;
	request.sourceHeight = decoded->frames[frameIndex].height;
	request.key = RequestKey(filename, Identify(filename), frameIndex,
		targetWidth, targetHeight, autoContrast, processing);
	return request;
}

DisplayImageRequest MakeJpegDisplayImageRequest(const fs::path& filename,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	bool autoContrast, std::size_t priority, const ImageProcessingParams& processing) {
	DisplayImageRequest request;
	request.filename = filename;
	request.sourceWidth = sourceWidth;
	request.sourceHeight = sourceHeight;
	request.targetWidth = targetWidth;
	request.targetHeight = targetHeight;
	request.autoContrast = autoContrast;
	request.processing = processing;
	request.priority = priority;
	if (!IsJpegPath(filename) || sourceWidth <= 0 || sourceHeight <= 0 ||
		targetWidth <= 0 || targetHeight <= 0) return request;
	request.key = RequestKey(filename, Identify(filename), 0,
		targetWidth, targetHeight, autoContrast, processing);
	return request;
}

std::size_t PreparedDisplayImageBytes(const PreparedDisplayImage& image) {
	return image.bgra.size();
}

std::size_t DisplayPrefetchCount(std::size_t cacheBytes, int viewportWidth,
	int viewportHeight, std::size_t fileCount) {
	constexpr std::size_t maximumSpeculativeFiles = 512;
	if (cacheBytes == 0 || viewportWidth <= 0 || viewportHeight <= 0 || fileCount < 2) return 0;
	const std::size_t width = static_cast<std::size_t>(viewportWidth);
	const std::size_t height = static_cast<std::size_t>(viewportHeight);
	if (width > std::numeric_limits<std::size_t>::max() / height ||
		width * height > std::numeric_limits<std::size_t>::max() / 4) return 0;
	const std::size_t textureBytes = width * height * 4;
	const std::size_t textureSlots = cacheBytes / textureBytes;
	if (textureSlots < 2) return 0;
	return std::min({textureSlots - 1, fileCount - 1, maximumSpeculativeFiles});
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

	struct Completion {
		ImagePtr image;
		std::size_t priority = 0;
	};

	explicit Impl(std::size_t budget, std::size_t requestedWorkers, Processor prepare,
		std::shared_ptr<SharedCacheBudget> shared)
		: byteBudget(budget), processor(std::move(prepare)), sharedBudget(std::move(shared)) {
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
		retirementWorker = std::thread([this] { Retire(); });
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

	bool Insert(const ImagePtr& image, bool mayEvict) {
		if (!image || image->key.empty()) return false;
		const std::size_t bytes = PreparedDisplayImageBytes(*image);
		if (bytes == 0 || bytes > byteBudget) return false;
		const auto existing = entries.find(image->key);
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
				inFlightPriorities[work.request.key] =
					work.foreground ? 0 : work.request.priority;
				++activeWorkers;
			}

			ImagePtr image = processor(work.request);
			const std::string currentKey = RequestKey(work.request.filename,
				Identify(work.request.filename), work.request.frameIndex,
				work.request.targetWidth, work.request.targetHeight,
				work.request.autoContrast, work.request.processing);

			{
				std::lock_guard<std::mutex> lock(mutex);
				inFlightKeys.erase(work.request.key);
				inFlightPriorities.erase(work.request.key);
				--activeWorkers;
				const bool foreground = work.foreground ||
					foregroundKeys.erase(work.request.key) != 0;
				const bool stillCurrentForeground = !foreground ||
					work.request.key == latestForegroundKey;
				const auto desiredPriority = desiredPrefetchPriorities.find(work.request.key);
				const std::size_t completionPriority = foreground ? 0 :
					(desiredPriority == desiredPrefetchPriorities.end() ? work.request.priority :
						desiredPriority->second);
				if (!stopping && stillCurrentForeground && work.epoch == epoch && image && image->key == currentKey &&
					(foreground || desiredPrefetchKeys.find(work.request.key) !=
						desiredPrefetchKeys.end())) {
					// A finished worker frame remains eligible for immediate SDL upload
					// even if retained-cache space is currently occupied by decoded data.
					// The completion queue is transient and bounded by worker throughput.
					Insert(image, foreground);
					completed.push_back({image, completionPriority});
				}
				idle.notify_all();
			}
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
	Processor processor;
	std::shared_ptr<SharedCacheBudget> sharedBudget;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable retirementAvailable;
	std::condition_variable idle;
	std::vector<std::thread> workers;
	std::thread retirementWorker;
	std::unordered_map<std::string, Entry> entries;
	std::deque<Work> queue;
	std::deque<Completion> completed;
	std::deque<ImagePtr> retired;
	std::unordered_set<std::string> queuedKeys;
	std::unordered_set<std::string> inFlightKeys;
	std::unordered_map<std::string, std::size_t> inFlightPriorities;
	std::unordered_set<std::string> desiredPrefetchKeys;
	std::unordered_map<std::string, std::size_t> desiredPrefetchPriorities;
	std::unordered_set<std::string> foregroundKeys;
	std::string latestForegroundKey;
	std::size_t cachedBytes = 0;
	std::size_t activeWorkers = 0;
	std::uint64_t useCounter = 0;
	std::uint64_t generation = 0;
	std::uint64_t epoch = 0;
	bool stopping = false;
};

DisplayImageCache::DisplayImageCache(std::size_t byteBudget,
	std::size_t workerCount, Processor processor,
	std::shared_ptr<SharedCacheBudget> sharedBudget)
	: impl_(std::make_unique<Impl>(byteBudget, workerCount, std::move(processor),
		std::move(sharedBudget))) {}

DisplayImageCache::~DisplayImageCache() = default;

DisplayImageCache::ImagePtr DisplayImageCache::Find(const DisplayImageRequest& request) {
	if (!request.Valid()) return {};
	if (request.key != RequestKey(request.filename, Identify(request.filename),
		request.frameIndex, request.targetWidth, request.targetHeight,
		request.autoContrast, request.processing)) return {};
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto found = impl_->entries.find(request.key);
	if (found != impl_->entries.end()) {
		found->second.lastUsed = ++impl_->useCounter;
		return found->second.image;
	}
	const auto completed = std::find_if(impl_->completed.begin(), impl_->completed.end(),
		[&request](const Impl::Completion& completion) {
			return completion.image && completion.image->key == request.key;
		});
	return completed == impl_->completed.end() ? ImagePtr{} : completed->image;
}

void DisplayImageCache::Request(const DisplayImageRequest& request) {
	if (!request.Valid()) return;
	bool removedQueuedForeground = false;
	bool hasQueuedWork = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->latestForegroundKey != request.key) {
			impl_->latestForegroundKey = request.key;
			for (auto queued = impl_->queue.begin(); queued != impl_->queue.end();) {
				if (queued->foreground && queued->request.key != request.key) {
					impl_->queuedKeys.erase(queued->request.key);
					impl_->foregroundKeys.erase(queued->request.key);
					queued = impl_->queue.erase(queued);
					removedQueuedForeground = true;
				} else {
					++queued;
				}
			}
			for (auto completed = impl_->completed.begin(); completed != impl_->completed.end();) {
				if (completed->priority == 0 && completed->image && completed->image->key != request.key) {
					impl_->retired.push_back(std::move(completed->image));
					completed = impl_->completed.erase(completed);
				} else {
					++completed;
				}
			}
			if (!impl_->retired.empty()) impl_->retirementAvailable.notify_one();
		}
		const auto completed = std::find_if(impl_->completed.begin(), impl_->completed.end(),
			[&request](const Impl::Completion& completion) {
				return completion.image && completion.image->key == request.key;
			});
		if (completed != impl_->completed.end()) {
			completed->priority = 0;
		} else if (const auto cached = impl_->entries.find(request.key); cached != impl_->entries.end()) {
			// A speculative frame can become the foreground between worker
			// completion and renderer upload. Promote the shared completion object
			// so it cannot wait behind any neighboring frame.
			impl_->completed.push_front({cached->second.image, 0});
		} else if (impl_->inFlightKeys.find(request.key) != impl_->inFlightKeys.end()) {
			impl_->foregroundKeys.insert(request.key);
			impl_->inFlightPriorities[request.key] = 0;
		} else if (impl_->queuedKeys.find(request.key) != impl_->queuedKeys.end()) {
			const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
				[&request](const Impl::Work& work) { return work.request.key == request.key; });
			if (queued != impl_->queue.end()) {
				Impl::Work promoted = std::move(*queued);
				impl_->queue.erase(queued);
				promoted.foreground = true;
				promoted.epoch = impl_->epoch;
				impl_->queue.push_front(std::move(promoted));
				hasQueuedWork = true;
			}
		} else {
			impl_->queue.push_front(Impl::Work{request, 0, impl_->epoch, true});
			impl_->queuedKeys.insert(request.key);
			hasQueuedWork = true;
		}
	}
	if (removedQueuedForeground) impl_->idle.notify_all();
	if (hasQueuedWork) impl_->workAvailable.notify_one();
}

DisplayImageCache::ImagePtr DisplayImageCache::RequestAndWait(
	const DisplayImageRequest& request) {
	if (!request.Valid()) return {};
	Request(request);
	std::unique_lock<std::mutex> lock(impl_->mutex);
	const auto findCompleted = [&]() -> ImagePtr {
		const auto cached = impl_->entries.find(request.key);
		if (cached != impl_->entries.end()) {
			cached->second.lastUsed = ++impl_->useCounter;
			return cached->second.image;
		}
		const auto completed = std::find_if(impl_->completed.begin(), impl_->completed.end(),
			[&request](const Impl::Completion& completion) {
				return completion.image && completion.image->key == request.key;
			});
		return completed == impl_->completed.end() ? ImagePtr{} : completed->image;
	};
	if (ImagePtr ready = findCompleted()) return ready;
	impl_->idle.wait(lock, [&] {
		if (impl_->stopping || impl_->entries.find(request.key) != impl_->entries.end()) return true;
		if (std::any_of(impl_->completed.begin(), impl_->completed.end(),
			[&request](const Impl::Completion& completion) {
				return completion.image && completion.image->key == request.key;
			})) return true;
		return impl_->queuedKeys.find(request.key) == impl_->queuedKeys.end() &&
			impl_->inFlightKeys.find(request.key) == impl_->inFlightKeys.end();
	});
	return findCompleted();
}

void DisplayImageCache::Prefetch(const std::vector<DisplayImageRequest>& requests) {
	std::vector<DisplayImageRequest> prioritizedRequests = requests;
	std::stable_sort(prioritizedRequests.begin(), prioritizedRequests.end(),
		[](const DisplayImageRequest& left, const DisplayImageRequest& right) {
			return left.priority < right.priority;
		});
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		++impl_->generation;
		if (requests.empty()) {
			while (!impl_->completed.empty()) {
				impl_->retired.push_back(std::move(impl_->completed.front().image));
				impl_->completed.pop_front();
			}
			impl_->retirementAvailable.notify_one();
		}
		impl_->queue.erase(std::remove_if(impl_->queue.begin(), impl_->queue.end(),
			[](const Impl::Work& work) { return !work.foreground; }), impl_->queue.end());
		impl_->queuedKeys.clear();
		impl_->desiredPrefetchKeys.clear();
		impl_->desiredPrefetchPriorities.clear();
		for (const Impl::Work& work : impl_->queue) impl_->queuedKeys.insert(work.request.key);
		for (const DisplayImageRequest& request : prioritizedRequests) {
			if (!request.Valid()) continue;
			impl_->desiredPrefetchKeys.insert(request.key);
			impl_->desiredPrefetchPriorities[request.key] = request.priority;
			const auto retained = impl_->entries.find(request.key);
			if (retained != impl_->entries.end()) {
				const auto completion = std::find_if(impl_->completed.begin(),
					impl_->completed.end(), [&request](const Impl::Completion& completion) {
						return completion.image && completion.image->key == request.key;
					});
				if (completion == impl_->completed.end()) {
					impl_->completed.push_back({retained->second.image, request.priority});
				} else {
					completion->priority = request.priority;
				}
				continue;
			}
			if (impl_->queuedKeys.find(request.key) != impl_->queuedKeys.end()) continue;
			if (impl_->inFlightKeys.find(request.key) != impl_->inFlightKeys.end()) {
				impl_->inFlightPriorities[request.key] = request.priority;
				continue;
			}
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
	result.reserve(std::min(maximumCount, impl_->completed.size()));
	while (result.size() < maximumCount && !impl_->completed.empty()) {
		const auto next = std::min_element(impl_->completed.begin(), impl_->completed.end(),
			[](const Impl::Completion& left, const Impl::Completion& right) {
				return left.priority < right.priority;
			});
		std::size_t outstandingPriority = std::numeric_limits<std::size_t>::max();
		for (const Impl::Work& work : impl_->queue) {
			outstandingPriority = std::min(outstandingPriority,
				work.foreground ? 0 : work.request.priority);
		}
		for (const auto& inFlight : impl_->inFlightPriorities) {
			outstandingPriority = std::min(outstandingPriority, inFlight.second);
		}
		if (outstandingPriority < next->priority) break;
		result.push_back(std::move(next->image));
		impl_->completed.erase(next);
	}
	return result;
}

void DisplayImageCache::Release(const std::string& key) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto found = impl_->entries.find(key);
	if (found != impl_->entries.end()) impl_->Erase(found);
	impl_->workAvailable.notify_one();
}

void DisplayImageCache::Retire(const ImagePtr& image) {
	if (!image) return;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->retired.push_back(image);
	}
	impl_->retirementAvailable.notify_one();
}

void DisplayImageCache::Clear() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	++impl_->generation;
	++impl_->epoch;
	impl_->queue.clear();
	while (!impl_->completed.empty()) {
		impl_->retired.push_back(std::move(impl_->completed.front().image));
		impl_->completed.pop_front();
	}
	impl_->queuedKeys.clear();
	impl_->desiredPrefetchKeys.clear();
	impl_->desiredPrefetchPriorities.clear();
	impl_->foregroundKeys.clear();
	impl_->latestForegroundKey.clear();
	while (!impl_->entries.empty()) impl_->Erase(impl_->entries.begin());
	impl_->idle.notify_all();
	impl_->workAvailable.notify_all();
	impl_->retirementAvailable.notify_all();
}

std::size_t DisplayImageCache::CachedBytes() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->cachedBytes;
}

std::size_t DisplayImageCache::CachedImages() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->entries.size();
}

bool DisplayImageCache::HasPendingWork() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return !impl_->queue.empty() || impl_->activeWorkers != 0 || !impl_->completed.empty();
}

bool DisplayImageCache::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(impl_->mutex);
	return impl_->idle.wait_for(lock, timeout, [this] {
		return impl_->queue.empty() && impl_->activeWorkers == 0;
	});
}

} // namespace jpegview_linux
