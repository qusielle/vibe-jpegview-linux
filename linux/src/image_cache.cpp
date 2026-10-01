#include "image_cache.h"
#include "archive_source.h"

#include <algorithm>
#include <atomic>
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

struct DecodedImageWorkKey {
	SourceKey source;
	bool dimensionsOnly = false;
};

bool operator==(const DecodedImageWorkKey& left, const DecodedImageWorkKey& right) {
	return left.dimensionsOnly == right.dimensionsOnly && left.source == right.source;
}

struct DecodedImageWorkKeyHash {
	std::size_t operator()(const DecodedImageWorkKey& key) const {
	return SourceKeyHash{}(key.source) ^
		(static_cast<std::size_t>(key.dimensionsOnly) + 0x9e3779b9u);
	}
};

DecodedImageWorkKey WorkKey(const SourceDescriptor& source, bool dimensionsOnly = false) {
	return DecodedImageWorkKey{source.Key(), dimensionsOnly};
}

void RecordQueuedCancellation(PerfWorkClass workClass,
	PerfExecution execution = PerfExecution::EventThread) {
	PerfContextScope context(workClass, execution);
	PerfDiagnostics::Instance().Record(PerfMetric::Cancellation);
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
		std::size_t bytes = 0;
		std::uint64_t lastUsed = 0;
	};
	using EntryMap = std::unordered_map<SourceKey, Entry, SourceKeyHash>;

	struct Work {
		fs::path filename;
		DecodedImageWorkKey key;
		SourceDescriptor source;
		std::uint64_t generation = 0;
		PerfWorkClass workClass = PerfWorkClass::Unspecified;
		bool dimensionsOnly = false;
		Completion completion;
		DimensionsCompletion dimensionsCompletion;
		std::shared_ptr<std::atomic<bool>> cancellation;
	};

	explicit Impl(std::size_t budget, Decoder decode,
		std::shared_ptr<SharedCacheBudget> shared, std::size_t workerCount,
		DimensionsReader readDimensions)
		: byteBudget(budget), decoder(std::move(decode)),
		  dimensionsReader(std::move(readDimensions)), sharedBudget(std::move(shared)) {
		if (!decoder) decoder = DecodeImage;
		if (!dimensionsReader) {
			dimensionsReader = [](const fs::path& filename, int& width, int& height,
				std::string& errorMessage) {
				return ReadJpegDimensions(filename, width, height, errorMessage);
			};
		}
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
			for (const auto& active : inFlightCancellation) {
				if (const auto cancellation = active.second.lock()) cancellation->store(true);
			}
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

	void Erase(EntryMap::iterator entry) {
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

	void EraseOtherSourceIdentities(const SourceKey& current) {
		if (current.logicalPath.empty()) return;
		for (auto entry = entries.begin(); entry != entries.end();) {
			if (entry->first.logicalPath == current.logicalPath && entry->first != current) {
				auto stale = entry++;
				Erase(stale);
			} else {
				++entry;
			}
		}
	}

	EntryMap::iterator Oldest() {
		if (entries.empty()) return entries.end();
		auto oldest = entries.begin();
		for (auto candidate = std::next(entries.begin()); candidate != entries.end(); ++candidate) {
			if (candidate->second.lastUsed < oldest->second.lastUsed) oldest = candidate;
		}
		return oldest;
	}

	bool Insert(const SourceKey& key,
		const std::shared_ptr<DecodedImage>& image, bool mayEvict) {
		if (!image || image->frames.empty() || !key.Valid()) return false;
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
		entries.emplace(key, Entry{image, bytes, ++useCounter});
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
				inFlightCancellation[work.key] = work.cancellation;
				inFlightWorkClasses[work.key] = work.workClass;
				++activeWorkers;
			}

			PerfContextScope context(work.workClass, PerfExecution::WorkerThread);
			std::shared_ptr<DecodedImage> image;
			std::string errorMessage;
			int sourceWidth = 0;
			int sourceHeight = 0;
			bool sourceChanged = false;
			const bool sourceCurrentBeforeOpen = (!work.cancellation ||
				!work.cancellation->load()) && IsImageSourceCurrent(work.source);
			sourceChanged = !sourceCurrentBeforeOpen &&
				(!work.cancellation || !work.cancellation->load());
			bool completed = sourceCurrentBeforeOpen;
			if (completed && work.dimensionsOnly) {
				completed = dimensionsReader(work.filename, sourceWidth, sourceHeight,
					errorMessage) && sourceWidth > 0 && sourceHeight > 0 &&
					(!work.cancellation || !work.cancellation->load());
			} else if (completed) {
				image = std::make_shared<DecodedImage>();
				completed = decoder(work.filename, *image, errorMessage) &&
					!image->frames.empty() && (!work.cancellation || !work.cancellation->load());
			}
			const bool cancelledAfterWork = work.cancellation && work.cancellation->load();
			const bool currentSource = !cancelledAfterWork && IsImageSourceCurrent(work.source);
			if (!cancelledAfterWork && !currentSource) sourceChanged = true;
			SourceDescriptor changedSource;
			if (sourceChanged) changedSource = DescribeImageSource(work.source.LogicalPath());
			const SourceChangeNotice sourceChange{work.source.Key(), changedSource};

			ImagePtr completedImage;
			Completion completion;
			DimensionsCompletion dimensionsCompletion;
			PerfWorkClass effectiveWorkClass = work.workClass;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (sourceChanged) {
					const auto existing = std::find_if(changedSources.begin(), changedSources.end(),
					[&sourceChange](const SourceChangeNotice& notice) {
							return notice.previous == sourceChange.previous;
						});
					if (existing == changedSources.end()) changedSources.push_back(sourceChange);
					else *existing = sourceChange;
				}
				inFlightKeys.erase(work.key);
				const auto activeWorkClass = inFlightWorkClasses.find(work.key);
				if (activeWorkClass != inFlightWorkClasses.end()) {
					effectiveWorkClass = activeWorkClass->second;
					inFlightWorkClasses.erase(activeWorkClass);
				}
				const bool foreground = foregroundKeys.erase(work.key) != 0;
				inFlightCancellation.erase(work.key);
				const auto desired = desiredWork.find(work.key);
				const bool stillDesired = desired != desiredWork.end() &&
					desired->second.generation >= work.generation &&
					desired->second.source.Key() == work.source.Key();
				if (!stopping && (foreground || stillDesired) && completed &&
					currentSource && !work.dimensionsOnly) {
					// Speculative work must never evict an image the user has
					// already viewed. Stop this generation once the free budget
					// cannot hold its next nearest neighbor.
					const bool mayEvict = foreground ||
						effectiveWorkClass == PerfWorkClass::ActiveImageSpread;
					if (!Insert(work.key.source, image, mayEvict)) {
						for (auto queued = queue.begin(); queued != queue.end();) {
							if (queued->workClass == PerfWorkClass::ActiveImageSpread ||
								foregroundKeys.find(queued->key) != foregroundKeys.end()) {
								++queued;
								continue;
							}
							RecordQueuedCancellation(queued->workClass,
								PerfExecution::WorkerThread);
							if (queued->cancellation) queued->cancellation->store(true);
							queuedKeys.erase(queued->key);
							const auto desiredQueued = desiredWork.find(queued->key);
							if (desiredQueued != desiredWork.end() &&
								desiredQueued->second.generation == queued->generation &&
								desiredQueued->second.source.Key() == queued->source.Key()) {
								desiredWork.erase(desiredQueued);
							}
							queued = queue.erase(queued);
						}
					} else {
						completedImage = image;
					}
				}
				if (!stopping && stillDesired) {
					if (work.dimensionsOnly) {
						dimensionsCompletion = desired->second.dimensionsCompletion;
					} else {
						completion = desired->second.completion;
					}
				}
				--activeWorkers;
				idle.notify_all();
			}
			if (completion) completion(work.filename, completedImage);
			if (dimensionsCompletion) dimensionsCompletion(work.filename,
				completed && currentSource, sourceWidth, sourceHeight);
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
	DimensionsReader dimensionsReader;
	std::shared_ptr<SharedCacheBudget> sharedBudget;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable retirementAvailable;
	std::condition_variable idle;
	std::vector<std::thread> workers;
	std::thread retirementWorker;
	EntryMap entries;
	std::deque<Work> queue;
	std::deque<ImagePtr> retired;
	std::vector<SourceChangeNotice> changedSources;
	std::unordered_set<DecodedImageWorkKey, DecodedImageWorkKeyHash> queuedKeys;
	std::unordered_set<DecodedImageWorkKey, DecodedImageWorkKeyHash> inFlightKeys;
	std::unordered_map<DecodedImageWorkKey, std::weak_ptr<std::atomic<bool>>,
		DecodedImageWorkKeyHash> inFlightCancellation;
	std::unordered_map<DecodedImageWorkKey, PerfWorkClass, DecodedImageWorkKeyHash> inFlightWorkClasses;
	std::unordered_set<DecodedImageWorkKey, DecodedImageWorkKeyHash> foregroundKeys;
	std::unordered_map<DecodedImageWorkKey, Work, DecodedImageWorkKeyHash> desiredWork;
	std::size_t cachedBytes = 0;
	std::size_t activeWorkers = 0;
	std::uint64_t useCounter = 0;
	std::uint64_t generation = 0;
	bool stopping = false;
};

DecodedImageCache::DecodedImageCache(std::size_t byteBudget, Decoder decoder,
	std::shared_ptr<SharedCacheBudget> sharedBudget, std::size_t workerCount,
	DimensionsReader dimensionsReader)
	: impl_(std::make_unique<Impl>(byteBudget, std::move(decoder), std::move(sharedBudget),
		workerCount, std::move(dimensionsReader))) {}

DecodedImageCache::~DecodedImageCache() = default;

DecodedImageCache::ImagePtr DecodedImageCache::Find(const fs::path& filename) {
	return Find(DescribeImageSource(filename));
}

DecodedImageCache::ImagePtr DecodedImageCache::Find(const SourceDescriptor& source) {
	const SourceKey key = source.Key();
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->EraseOtherSourceIdentities(key);
	if (!key.Valid()) return {};
	const auto found = impl_->entries.find(key);
	if (found == impl_->entries.end()) return {};
	found->second.lastUsed = ++impl_->useCounter;
	return found->second.image;
}

DecodedImageCache::ImagePtr DecodedImageCache::FindOrWait(const fs::path& filename) {
	return FindOrWait(DescribeImageSource(filename));
}

DecodedImageCache::ImagePtr DecodedImageCache::FindOrWait(const SourceDescriptor& source) {
	const SourceKey key = source.Key();
	std::unique_lock<std::mutex> lock(impl_->mutex);
	impl_->EraseOtherSourceIdentities(key);
	if (!key.Valid()) return {};
	const DecodedImageWorkKey workKey = WorkKey(source);
	const auto findValid = [&]() -> ImagePtr {
		const auto found = impl_->entries.find(key);
		if (found == impl_->entries.end()) return {};
		found->second.lastUsed = ++impl_->useCounter;
		return found->second.image;
	};
	if (ImagePtr cached = findValid()) return cached;

	const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
		[&workKey](const Impl::Work& work) { return work.key == workKey; });
	const bool inFlight = impl_->inFlightKeys.find(workKey) != impl_->inFlightKeys.end();
	if (queued == impl_->queue.end() && !inFlight) return {};
	impl_->foregroundKeys.insert(workKey);
	if (queued != impl_->queue.end()) {
		queued->workClass = PerfWorkClass::ActiveImageSpread;
		if (queued->cancellation) queued->cancellation->store(false);
	}
	if (inFlight) {
		impl_->inFlightWorkClasses[workKey] = PerfWorkClass::ActiveImageSpread;
		const auto cancellation = impl_->inFlightCancellation.find(workKey);
		if (cancellation != impl_->inFlightCancellation.end()) {
			if (const auto token = cancellation->second.lock()) token->store(false);
		}
	}
	if (queued != impl_->queue.end() && queued != impl_->queue.begin()) {
			Impl::Work promoted = std::move(*queued);
			impl_->queue.erase(queued);
			promoted.workClass = PerfWorkClass::ActiveImageSpread;
			if (promoted.cancellation) promoted.cancellation->store(false);
			impl_->queue.push_front(std::move(promoted));
	}
	impl_->workAvailable.notify_one();
	impl_->idle.wait(lock, [&] {
		return impl_->stopping || impl_->entries.find(key) != impl_->entries.end() ||
			(impl_->queuedKeys.find(workKey) == impl_->queuedKeys.end() &&
			 impl_->inFlightKeys.find(workKey) == impl_->inFlightKeys.end());
	});
	return findValid();
}

void DecodedImageCache::Store(const fs::path& filename,
	const std::shared_ptr<DecodedImage>& image) {
	Store(DescribeImageSource(filename), image);
}

void DecodedImageCache::Store(const SourceDescriptor& source,
	const std::shared_ptr<DecodedImage>& image) {
	const SourceKey key = source.Key();
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->EraseOtherSourceIdentities(key);
		impl_->Insert(key, image, true);
	}
	impl_->workAvailable.notify_one();
}

void DecodedImageCache::RequestBackground(const fs::path& filename,
	Completion completion, PerfWorkClass workClass) {
	RequestBackground(DescribeImageSource(filename), std::move(completion), workClass);
}

void DecodedImageCache::RequestBackground(const SourceDescriptor& source,
	Completion completion, PerfWorkClass workClass) {
	Impl::Work work;
	work.filename = source.LogicalPath();
	work.key = WorkKey(source);
	work.source = source;
	work.workClass = workClass;
	work.completion = std::move(completion);
	work.cancellation = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->EraseOtherSourceIdentities(work.key.source);
	}
	if (!work.key.source.Valid()) return;

	ImagePtr alreadyCached;
	bool queuedWork = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->stopping) return;
		work.generation = impl_->generation;
		const auto cached = impl_->entries.find(work.key.source);
		if (cached != impl_->entries.end()) {
			cached->second.lastUsed = ++impl_->useCounter;
			alreadyCached = cached->second.image;
		} else {
			const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
				[&work](const Impl::Work& candidate) { return candidate.key == work.key; });
			if (queued != impl_->queue.end()) {
				if (queued->cancellation) queued->cancellation->store(false);
				queued->generation = work.generation;
				queued->workClass = work.workClass;
				queued->completion = work.completion;
				queued->cancellation = work.cancellation;
				work = *queued;
			} else if (impl_->inFlightKeys.find(work.key) != impl_->inFlightKeys.end()) {
				impl_->inFlightWorkClasses[work.key] = work.workClass;
				const auto cancellation = impl_->inFlightCancellation.find(work.key);
				if (cancellation != impl_->inFlightCancellation.end()) {
					if (const auto token = cancellation->second.lock()) token->store(false);
				}
			} else {
				impl_->queue.push_front(work);
				impl_->queuedKeys.insert(work.key);
				queuedWork = true;
			}
			impl_->desiredWork[work.key] = work;
		}
	}
	if (queuedWork) impl_->workAvailable.notify_one();
	if (alreadyCached && work.completion) work.completion(work.filename, alreadyCached);
}

void DecodedImageCache::RequestJpegDimensions(const fs::path& filename,
	DimensionsCompletion completion, PerfWorkClass workClass) {
	RequestJpegDimensions(DescribeImageSource(filename), std::move(completion), workClass);
}

void DecodedImageCache::RequestJpegDimensions(const SourceDescriptor& source,
	DimensionsCompletion completion, PerfWorkClass workClass) {
	if (!completion || !IsJpegPath(source.LogicalPath())) return;
	Impl::Work work;
	work.filename = source.LogicalPath();
	work.key = WorkKey(source, true);
	work.source = source;
	work.workClass = workClass;
	work.dimensionsOnly = true;
	work.dimensionsCompletion = std::move(completion);
	work.cancellation = std::make_shared<std::atomic<bool>>(false);
	if (!work.key.source.Valid()) return;

	bool queuedWork = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->stopping) return;
		work.generation = impl_->generation;
		const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
			[&work](const Impl::Work& candidate) { return candidate.key == work.key; });
		if (queued != impl_->queue.end()) {
			if (queued->cancellation) queued->cancellation->store(false);
			queued->generation = work.generation;
			queued->workClass = work.workClass;
			queued->dimensionsCompletion = work.dimensionsCompletion;
			queued->cancellation = work.cancellation;
			work = *queued;
		} else if (impl_->inFlightKeys.find(work.key) != impl_->inFlightKeys.end()) {
			impl_->inFlightWorkClasses[work.key] = work.workClass;
			const auto cancellation = impl_->inFlightCancellation.find(work.key);
			if (cancellation != impl_->inFlightCancellation.end()) {
				if (const auto token = cancellation->second.lock()) token->store(false);
			}
		} else {
			impl_->queue.push_front(work);
			impl_->queuedKeys.insert(work.key);
			queuedWork = true;
		}
		impl_->desiredWork[work.key] = work;
	}
	if (queuedWork) impl_->workAvailable.notify_one();
}

bool DecodedImageCache::CancelActiveSpreadRequest(const fs::path& filename,
	bool dimensionsOnly) {
	return CancelActiveSpreadRequest(DescribeImageSource(filename), dimensionsOnly);
}

bool DecodedImageCache::CancelActiveSpreadRequest(const SourceDescriptor& source,
	bool dimensionsOnly) {
	const DecodedImageWorkKey key = WorkKey(source, dimensionsOnly);
	bool canceled = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->foregroundKeys.find(key) != impl_->foregroundKeys.end()) return false;

		const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
			[&key](const Impl::Work& work) { return work.key == key; });
		if (queued != impl_->queue.end() &&
			queued->workClass == PerfWorkClass::ActiveImageSpread) {
			if (queued->cancellation) queued->cancellation->store(true);
			RecordQueuedCancellation(queued->workClass);
			impl_->queuedKeys.erase(key);
			impl_->queue.erase(queued);
			impl_->desiredWork.erase(key);
			canceled = true;
		}

		const auto inFlight = impl_->inFlightCancellation.find(key);
		const auto workClass = impl_->inFlightWorkClasses.find(key);
		if (inFlight != impl_->inFlightCancellation.end() &&
			workClass != impl_->inFlightWorkClasses.end() &&
			workClass->second == PerfWorkClass::ActiveImageSpread) {
			if (const auto cancellation = inFlight->second.lock()) {
				cancellation->store(true);
			}
			canceled = true;
			impl_->inFlightWorkClasses.erase(workClass);
			impl_->desiredWork.erase(key);
		}
		if (canceled) impl_->idle.notify_all();
	}
	if (canceled) impl_->workAvailable.notify_all();
	return canceled;
}

void DecodedImageCache::Prefetch(const std::vector<fs::path>& files,
	std::size_t currentIndex, int preferredDirection, std::size_t maximumCount,
	Completion completion, Filter filter, std::size_t nearestCount,
	DescriptorProvider descriptorProvider) {
	std::vector<Impl::Work> prepared;
	std::vector<std::pair<fs::path, ImagePtr>> alreadyCached;
	const std::vector<std::size_t> order = ImagePrefetchOrder(files.size(), currentIndex,
		preferredDirection, maximumCount);
	for (std::size_t position = 0; position < order.size(); ++position) {
		const std::size_t index = order[position];
		Impl::Work work;
		const SourceDescriptor* suppliedSource = descriptorProvider ?
			descriptorProvider(index) : nullptr;
		work.source = suppliedSource != nullptr ? *suppliedSource :
			DescribeImageSource(files[index]);
		work.filename = work.source.LogicalPath();
		if (filter && !filter(work.filename)) continue;
		work.key = WorkKey(work.source);
		work.workClass = position < nearestCount ?
			PerfWorkClass::NearestNavigationNeighbor : PerfWorkClass::DistantSpeculation;
		work.completion = completion;
		work.cancellation = std::make_shared<std::atomic<bool>>(false);
		if (work.key.source.Valid()) prepared.push_back(std::move(work));
	}
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		++impl_->generation;
		const std::uint64_t generation = impl_->generation;
		for (auto queued = impl_->queue.begin(); queued != impl_->queue.end();) {
			if (queued->workClass == PerfWorkClass::ActiveImageSpread ||
				impl_->foregroundKeys.find(queued->key) != impl_->foregroundKeys.end()) {
				queued->generation = generation;
				if (queued->cancellation) queued->cancellation->store(false);
				++queued;
			} else {
				RecordQueuedCancellation(queued->workClass);
				if (queued->cancellation) queued->cancellation->store(true);
				queued = impl_->queue.erase(queued);
			}
		}
		for (const auto& active : impl_->inFlightCancellation) {
			const auto workClass = impl_->inFlightWorkClasses.find(active.first);
			if (impl_->foregroundKeys.find(active.first) != impl_->foregroundKeys.end() ||
				(workClass != impl_->inFlightWorkClasses.end() &&
					workClass->second == PerfWorkClass::ActiveImageSpread)) {
				if (const auto cancellation = active.second.lock()) cancellation->store(false);
				continue;
			}
			const bool remainsDesired = std::any_of(prepared.begin(), prepared.end(),
				[&active](const Impl::Work& work) { return work.key == active.first; });
			if (const auto cancellation = active.second.lock()) {
				cancellation->store(!remainsDesired);
			}
		}
		impl_->queuedKeys.clear();
		std::unordered_map<DecodedImageWorkKey, Impl::Work,
			DecodedImageWorkKeyHash> preservedSpreadWork;
		for (const auto& active : impl_->inFlightWorkClasses) {
			if (active.second != PerfWorkClass::ActiveImageSpread) continue;
			const auto desired = impl_->desiredWork.find(active.first);
			if (desired != impl_->desiredWork.end()) {
				Impl::Work work = desired->second;
				work.generation = generation;
				preservedSpreadWork.emplace(active.first, std::move(work));
			}
		}
		for (Impl::Work& work : impl_->queue) {
			impl_->queuedKeys.insert(work.key);
			if (work.workClass == PerfWorkClass::ActiveImageSpread) {
				preservedSpreadWork[work.key] = work;
			}
		}
		impl_->queue.erase(std::remove_if(impl_->queue.begin(), impl_->queue.end(),
			[&](const Impl::Work& work) {
				return work.workClass != PerfWorkClass::ActiveImageSpread &&
					impl_->foregroundKeys.find(work.key) == impl_->foregroundKeys.end();
			}), impl_->queue.end());
		impl_->queuedKeys.clear();
		for (const Impl::Work& work : impl_->queue) impl_->queuedKeys.insert(work.key);
		impl_->desiredWork.clear();
		for (auto& retained : preservedSpreadWork) {
			impl_->desiredWork.emplace(retained.first, std::move(retained.second));
		}
		for (Impl::Work& work : prepared) {
			work.generation = impl_->generation;
			impl_->desiredWork[work.key] = work;
			const auto cached = impl_->entries.find(work.key.source);
			if (cached != impl_->entries.end()) {
				if (completion) alreadyCached.emplace_back(work.filename, cached->second.image);
				continue;
			}
			if (impl_->inFlightKeys.find(work.key) != impl_->inFlightKeys.end()) {
				const auto cancellation = impl_->inFlightCancellation.find(work.key);
				if (cancellation != impl_->inFlightCancellation.end()) {
					if (const auto token = cancellation->second.lock()) token->store(false);
				}
				continue;
			}
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
	for (const auto& active : impl_->inFlightCancellation) {
		if (const auto cancellation = active.second.lock()) cancellation->store(true);
	}
	for (const Impl::Work& work : impl_->queue) {
		RecordQueuedCancellation(work.workClass);
	}
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

DecodedImageCacheDiagnostics DecodedImageCache::GetDiagnostics() const {
	DecodedImageCacheDiagnostics diagnostics;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	diagnostics.cachedBytes = impl_->cachedBytes;
	diagnostics.cachedImages = impl_->entries.size();
	for (const Impl::Work& work : impl_->queue) {
		if (work.workClass == PerfWorkClass::ActiveImageSpread) {
			++diagnostics.activeSpreadQueued;
		}
		if (impl_->foregroundKeys.find(work.key) != impl_->foregroundKeys.end()) {
			++diagnostics.foregroundQueued;
		} else {
			++diagnostics.backgroundQueued;
		}
	}
	for (const DecodedImageWorkKey& key : impl_->inFlightKeys) {
		const auto workClass = impl_->inFlightWorkClasses.find(key);
		if (workClass != impl_->inFlightWorkClasses.end() &&
			workClass->second == PerfWorkClass::ActiveImageSpread) {
			++diagnostics.activeSpreadActive;
		}
		if (impl_->foregroundKeys.find(key) != impl_->foregroundKeys.end()) {
			++diagnostics.foregroundActive;
		} else {
			++diagnostics.backgroundActive;
		}
	}
	diagnostics.retiredImages = impl_->retired.size();
	for (const ImagePtr& image : impl_->retired) {
		if (image) diagnostics.retiredBytes += DecodedImageBytes(*image);
	}
	return diagnostics;
}

std::vector<SourceChangeNotice> DecodedImageCache::TakeChangedSources() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<SourceChangeNotice> changed;
	changed.swap(impl_->changedSources);
	return changed;
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
