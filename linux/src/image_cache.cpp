#include "image_cache.h"
#include "archive_source.h"
#include "cache_policy.h"
#include "source_work_coordinator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <list>
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

bool SameDecodedImageRequest(const DecodedImageRequestIdentity& left,
	const DecodedImageRequestIdentity& right) {
	return left.dimensionsOnly == right.dimensionsOnly && left.source == right.source;
}

bool CanTransferActiveSpreadRequestToCurrentImage(
	const DecodedImageRequestIdentity& spreadRequest,
	const DecodedImageRequestIdentity& currentImageRequest) {
	return SameDecodedImageRequest(spreadRequest, currentImageRequest);
}

bool operator==(const DecodedImageRequestIdentity& left,
	const DecodedImageRequestIdentity& right) {
	return SameDecodedImageRequest(left, right);
}

bool operator!=(const DecodedImageRequestIdentity& left,
	const DecodedImageRequestIdentity& right) {
	return !(left == right);
}

namespace {

using DecodedImageWorkKey = DecodedImageRequestIdentity;

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

std::shared_ptr<const void> DecodedAllocationIdentity(
	const std::shared_ptr<const DecodedImage>& image) {
	return image ? std::shared_ptr<const void>(image, image.get()) :
		std::shared_ptr<const void>{};
}

template <typename Left, typename Right>
bool SameSharedOwnership(const Left& left, const Right& right) {
	return !left.owner_before(right) && !right.owner_before(left);
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

bool IsCurrentJpegDimensionsResult(std::uint64_t resultGeneration,
	const SourceKey& resultSource, std::uint64_t expectedGeneration,
	const SourceKey& expectedSource) {
	return resultGeneration == expectedGeneration && resultSource == expectedSource;
}

std::vector<std::size_t> ImagePrefetchOrder(std::size_t fileCount,
	std::size_t currentIndex, int preferredDirection, std::size_t maximumCount) {
	std::vector<std::size_t> result;
	if (fileCount < 2 || currentIndex >= fileCount || maximumCount == 0) return result;
	result.reserve(std::min(maximumCount, fileCount - 1));
	const int direction = preferredDirection < 0 ? -1 : 1;
	for (std::size_t distance = 1;
		result.size() < maximumCount && result.size() + 1 < fileCount;
		++distance) {
		const std::size_t offset = distance % fileCount;
		if (offset == 0) break;
		const std::size_t forward = currentIndex >= fileCount - offset ?
			currentIndex - (fileCount - offset) : currentIndex + offset;
		const std::size_t backward = currentIndex >= offset ?
			currentIndex - offset : fileCount - (offset - currentIndex);
		const std::size_t first = direction > 0 ? forward : backward;
		const std::size_t second = direction > 0 ? backward : forward;
		result.push_back(first);
		if (second != first && result.size() < maximumCount && result.size() + 1 < fileCount) {
			result.push_back(second);
		}
	}
	return result;
}

struct DecodedImageCache::Impl {
	using ImagePtr = DecodedImageCache::ImagePtr;
	using LruList = std::list<SourceKey>;

	struct Entry {
		ImagePtr image;
		std::size_t bytes = 0;
		CacheProtectionTier protection = CacheProtectionTier::DistantSpeculation;
		CacheReservation reservation;
		std::list<SourceKey>::iterator lru;
	};
	using EntryMap = std::unordered_map<SourceKey, Entry, SourceKeyHash>;
	struct RetiredImage {
		ImagePtr image;
		CacheReservation reservation;
	};
	struct RetirementState {
		std::mutex mutex;
		std::condition_variable available;
		std::deque<RetiredImage> retired;
		std::weak_ptr<const void> retiringOwner;
		bool retiring = false;
		bool stopping = false;
	};

	struct Work {
		fs::path filename;
		DecodedImageWorkKey key;
		SourceDescriptor source;
		std::uint64_t generation = 0;
		PerfWorkClass workClass = PerfWorkClass::Unspecified;
		bool dimensionsOnly = false;
		Completion completion;
		DetailedCompletion detailedCompletion;
		DimensionsCompletion dimensionsCompletion;
		std::shared_ptr<std::atomic<bool>> cancellation;
		bool foregroundAtStart = false;
	};

	static std::size_t TierIndex(CacheProtectionTier tier) {
		return static_cast<std::size_t>(tier);
	}

	void Touch(EntryMap::iterator entry) {
		LruList& list = lru[TierIndex(entry->second.protection)];
		list.splice(list.end(), list, entry->second.lru);
	}

	bool SetProtection(EntryMap::iterator entry, CacheProtectionTier protection) {
		if (sharedBudget && entry->second.reservation) {
			if (protection == CacheProtectionTier::Active) {
				if (!entry->second.reservation.ReclassifyForActiveUse()) return false;
			} else if (entry->second.reservation.Category() ==
				CacheMemoryCategory::ActiveWorkingData &&
				!entry->second.reservation.Reclassify(
					CacheMemoryCategory::RetainedDecodedPixels)) {
				return false;
			}
		}
		if (entry->second.protection != protection) {
			lru[TierIndex(entry->second.protection)].erase(entry->second.lru);
			LruList& list = lru[TierIndex(protection)];
			list.push_back(entry->first);
			entry->second.lru = std::prev(list.end());
			entry->second.protection = protection;
		}
		return true;
	}

	explicit Impl(std::size_t budget, Decoder decode,
		std::shared_ptr<SharedCacheBudget> shared, std::size_t workerCount,
		DimensionsReader readDimensions)
		: byteBudget(budget), decoder(std::move(decode)),
		  dimensionsReader(std::move(readDimensions)), sharedBudget(std::move(shared)) {
		if (!decoder) {
			decoder = [](const fs::path& filename, DecodedImage& image,
				std::string& errorMessage) {
				return DecodeImage(filename, image, errorMessage);
			};
		}
		if (!dimensionsReader) {
			dimensionsReader = [](const fs::path& filename, int& width, int& height,
				std::string& errorMessage) {
				return ReadJpegDimensions(filename, width, height, errorMessage);
			};
		}
		retirementState = std::make_shared<RetirementState>();
		const std::size_t boundedWorkerCount = ClampCpuWorkerCount(workerCount);
		workers.reserve(boundedWorkerCount);
		for (std::size_t index = 0; index < boundedWorkerCount; ++index) {
			workers.emplace_back([this] { Run(); });
		}
		retirementWorker = std::thread([state = retirementState] { Retire(state); });
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
			while (!entries.empty()) Erase(entries.begin());
		}
		workAvailable.notify_all();
		for (std::thread& worker : workers) {
			if (worker.joinable()) worker.join();
		}
		bool externalRetirementOwner = false;
		{
			std::lock_guard<std::mutex> lock(retirementState->mutex);
			retirementState->stopping = true;
			if (retirementState->retiring) {
				const auto owner = retirementState->retiringOwner.lock();
				externalRetirementOwner = owner && owner.use_count() > 2;
			}
			for (const RetiredImage& retiredImage : retirementState->retired) {
				if (retiredImage.image && retiredImage.image.use_count() > 1) {
					externalRetirementOwner = true;
				}
			}
		}
		retirementState->available.notify_all();
		if (retirementWorker.joinable()) {
			if (externalRetirementOwner) retirementWorker.detach();
			else retirementWorker.join();
		}
	}

	void Erase(EntryMap::iterator entry) {
		const std::size_t bytes = entry->second.bytes;
		ImagePtr retiredImage = std::move(entry->second.image);
		CacheReservation reservation = std::move(entry->second.reservation);
		lru[TierIndex(entry->second.protection)].erase(entry->second.lru);
		entries.erase(entry);
		cachedBytes -= bytes;
		if (retiredImage) {
			QueueRetirement(std::move(retiredImage), std::move(reservation));
		}
	}

	void QueueRetirement(ImagePtr image, CacheReservation reservation = {}) {
		if (!image) return;
		reservation.RelinquishRetainedOwnership();
		if (!reservation && sharedBudget) {
			reservation = sharedBudget->TrackTemporary(DecodedImageBytes(*image),
				CacheMemoryCategory::ActiveWorkingData,
				DecodedAllocationIdentity(image));
		}
		if (sharedBudget) {
			sharedBudget->RetireAllocation(DecodedAllocationIdentity(image),
				std::move(reservation));
			return;
		}
		std::lock_guard<std::mutex> lock(retirementState->mutex);
		if (SameSharedOwnership(image, retirementState->retiringOwner) ||
			std::any_of(retirementState->retired.begin(), retirementState->retired.end(),
				[&image](const RetiredImage& candidate) {
					return SameSharedOwnership(image, candidate.image);
				})) return;
		retirementState->retired.push_back({std::move(image), std::move(reservation)});
		retirementState->available.notify_one();
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

	EntryMap::iterator Oldest(CacheProtectionTier maximumTier) {
		for (std::size_t tier = 0; tier <= TierIndex(maximumTier); ++tier) {
			if (lru[tier].empty()) continue;
			const auto entry = entries.find(lru[tier].front());
			if (entry != entries.end()) return entry;
		}
		return entries.end();
	}

	bool Insert(const SourceKey& key,
		const std::shared_ptr<DecodedImage>& image, bool mayEvict,
		CacheProtectionTier protection = CacheProtectionTier::Active,
		bool activeUse = false) {
		if (!image || image->frames.empty() || !key.Valid()) return false;
		const std::size_t bytes = DecodedImageBytes(*image);
		if (bytes == 0 || bytes > byteBudget) return false;
		const auto existing = entries.find(key);
		if (existing != entries.end()) Erase(existing);
		if (!mayEvict && bytes > byteBudget - cachedBytes) return false;
		while (bytes > byteBudget - cachedBytes && !entries.empty()) {
			auto oldest = Oldest(CacheProtectionTier::DistantSpeculation);
			if (oldest == entries.end() && mayEvict &&
				protection == CacheProtectionTier::Active) {
				oldest = Oldest(CacheProtectionTier::Neighbor);
			}
			if (oldest == entries.end()) return false;
			Erase(oldest);
		}
		if (bytes > byteBudget - cachedBytes) return false;
		CacheReservation reservation;
		if (sharedBudget) {
			const std::shared_ptr<const void> allocation = DecodedAllocationIdentity(image);
			reservation = activeUse ?
				sharedBudget->TrackTemporary(bytes,
					CacheMemoryCategory::ActiveWorkingData, allocation) :
				sharedBudget->TryReserve(bytes,
					CacheMemoryCategory::RetainedDecodedPixels, allocation);
			if (reservation && activeUse &&
				!reservation.ReclassifyForActiveUse()) return false;
			while (!reservation) {
				if (!mayEvict || activeUse) return false;
				auto oldest = Oldest(CacheProtectionTier::DistantSpeculation);
				if (oldest == entries.end() && protection == CacheProtectionTier::Active) {
					oldest = Oldest(CacheProtectionTier::Neighbor);
				}
				if (oldest == entries.end()) return false;
				const std::uint64_t releaseRevision =
					sharedBudget->RetainedCapacityRevision();
				Erase(oldest);
				if (sharedBudget->RetainedCapacityRevision() == releaseRevision) return false;
				reservation = sharedBudget->TryReserve(bytes,
					CacheMemoryCategory::RetainedDecodedPixels, allocation);
				if (!reservation) return false;
			}
		}
		LruList& list = lru[TierIndex(protection)];
		list.push_back(key);
		entries.emplace(key, Entry{image, bytes, protection, std::move(reservation),
			std::prev(list.end())});
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
				work.foregroundAtStart = foregroundKeys.find(work.key) != foregroundKeys.end();
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
			const bool activeSpread =
				work.workClass == PerfWorkClass::ActiveImageSpread;
			const SourceWorkPriority sourcePriority = work.foregroundAtStart || activeSpread ?
				SourceWorkPriority::Foreground : work.dimensionsOnly ?
				SourceWorkPriority::Metadata : SourceWorkPriority::Speculative;
			const auto cancellation = work.cancellation;
			WorkContext sourceContext = MakeWorkContext(work.source, sourcePriority,
				[cancellation] { return !cancellation || !cancellation->load(); });
			sourceContext.currentPriority = [this, key = work.key, sourcePriority] {
				std::lock_guard<std::mutex> lock(mutex);
				if (foregroundKeys.find(key) != foregroundKeys.end()) {
					return SourceWorkPriority::Foreground;
				}
				const auto activeClass = inFlightWorkClasses.find(key);
				return activeClass != inFlightWorkClasses.end() &&
					activeClass->second == PerfWorkClass::ActiveImageSpread ?
					SourceWorkPriority::Foreground : sourcePriority;
			};
			bool sourceCurrentBeforeOpen = false;
			bool completed = false;
			bool sourceAdmissionSucceeded = false;
			bool currentSource = false;
			bool sourceCheckedForPublication = false;
			bool sourceRefreshNeeded = false;
			SourceDescriptor changedSource;
			WorkerFailure workerFailure;
			{
				try {
					SourceCpuWorkLease admission =
						SourceWorkCoordinator::Global().AcquireSourceAndCpu(
							sourceContext, work.filename);
					sourceAdmissionSucceeded = static_cast<bool>(admission);
					if (admission) {
						sourceContext.sourcePriority = sourceContext.Priority();
						sourceContext.sourceAccessAlreadyAdmitted = true;
						sourceContext.cpuProcessingAlreadyAdmitted = true;
						ScopedWorkContext activeContext(sourceContext);
						sourceCurrentBeforeOpen = (!work.cancellation ||
							!work.cancellation->load()) && IsImageSourceCurrent(work.source);
						if (sourceCurrentBeforeOpen && sourceContext.Continue() &&
							work.dimensionsOnly) {
							completed = dimensionsReader(work.filename, sourceWidth, sourceHeight,
								errorMessage) && sourceWidth > 0 && sourceHeight > 0 &&
								sourceContext.Continue();
						} else if (sourceCurrentBeforeOpen && sourceContext.Continue()) {
							image = std::make_shared<DecodedImage>();
							completed = decoder(work.filename, *image, errorMessage) &&
								!image->frames.empty() && sourceContext.Continue();
						}
						const bool cancelledAfterWork = work.cancellation &&
							work.cancellation->load();
						if (sourceCurrentBeforeOpen && !cancelledAfterWork &&
							workerFailure.kind != WorkerFailureKind::Exception &&
							sourceContext.Continue()) {
							currentSource = IsImageSourceCurrent(work.source);
							sourceCheckedForPublication = true;
						}
						if (!cancelledAfterWork && sourceCheckedForPublication && !currentSource) {
							sourceChanged = true;
							sourceRefreshNeeded = true;
						}
						sourceContext.sourceAccessAlreadyAdmitted = false;
						sourceContext.cpuProcessingAlreadyAdmitted = false;
					}
				} catch (const std::exception& error) {
					image.reset();
					errorMessage = error.what();
					workerFailure = {WorkerFailureKind::Exception, error.what()};
					completed = false;
				} catch (...) {
					image.reset();
					errorMessage = "unknown decoded-image worker failure";
					workerFailure = {WorkerFailureKind::Exception,
						"unknown decoded-image worker failure"};
					completed = false;
				}
			}
			if (sourceAdmissionSucceeded && !sourceCurrentBeforeOpen &&
				!workerFailure.Failed() && (!work.cancellation || !work.cancellation->load())) {
				sourceChanged = true;
				sourceRefreshNeeded = true;
			}
			if (sourceRefreshNeeded && !workerFailure.Failed() &&
				(!work.cancellation || !work.cancellation->load())) {
				try {
					WorkContext refreshContext = MakePathWorkContext(
						work.source.LogicalPath(),
						SourceWorkPriority::Metadata, [cancellation] {
							return !cancellation || !cancellation->load();
						});
					refreshContext.currentPriority = sourceContext.currentPriority;
					SourceCpuWorkLease refreshAdmission =
						SourceWorkCoordinator::Global().AcquireSourceAndCpu(
							refreshContext, work.filename);
					if (refreshAdmission && refreshContext.Continue()) {
						refreshContext.sourcePriority = refreshContext.Priority();
						refreshContext.sourceAccessAlreadyAdmitted = true;
						refreshContext.cpuProcessingAlreadyAdmitted = true;
						{
							ScopedWorkContext activeContext(refreshContext);
							changedSource = DescribeImageSource(work.source.LogicalPath());
						}
						refreshContext.sourceAccessAlreadyAdmitted = false;
						refreshContext.cpuProcessingAlreadyAdmitted = false;
					}
				} catch (const std::exception& error) {
					workerFailure = {WorkerFailureKind::Exception, error.what()};
					sourceChanged = false;
				} catch (...) {
					workerFailure = {WorkerFailureKind::Exception,
						"unknown decoded-image source refresh failure"};
					sourceChanged = false;
				}
			}
			const bool cancelledAfterWork = work.cancellation && work.cancellation->load();
			if (cancelledAfterWork) {
				workerFailure = {WorkerFailureKind::Cancelled, "decoded-image request was cancelled"};
			} else if (sourceChanged) {
				workerFailure = {WorkerFailureKind::SourceUnavailable,
					"image source changed while it was being decoded"};
			} else if (!completed && !workerFailure.Failed()) {
				workerFailure = {WorkerFailureKind::ProcessingFailed,
					errorMessage.empty() ? "image decoding failed" : errorMessage};
			}
			const SourceChangeNotice sourceChange{work.source.Key(), changedSource};

			ImagePtr completedImage;
			Completion completion;
			DetailedCompletion detailedCompletion;
			DimensionsCompletion dimensionsCompletion;
			PerfWorkClass effectiveWorkClass = work.workClass;
			bool retryQueued = false;
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
				if (workerFailure.Failed()) lastWorkerFailure = workerFailure;
				inFlightKeys.erase(work.key);
				const auto activeWorkClass = inFlightWorkClasses.find(work.key);
				if (activeWorkClass != inFlightWorkClasses.end()) {
					effectiveWorkClass = activeWorkClass->second;
					inFlightWorkClasses.erase(activeWorkClass);
				}
				const bool foreground = foregroundKeys.erase(work.key) != 0;
				inFlightCancellation.erase(work.key);
				const auto desired = desiredWork.find(work.key);
				const bool sameDesiredRequest = desired != desiredWork.end() &&
					desired->second.generation >= work.generation &&
					desired->second.source.Key() == work.source.Key();
				const bool stillDesired = sameDesiredRequest &&
					desired->second.cancellation == work.cancellation;
				if (!stopping && workerFailure.kind == WorkerFailureKind::Cancelled &&
					sameDesiredRequest &&
					queuedKeys.find(work.key) == queuedKeys.end()) {
					Work retry = desired->second;
					retry.cancellation = std::make_shared<std::atomic<bool>>(false);
					queue.push_front(std::move(retry));
					desired->second = queue.front();
					queuedKeys.insert(work.key);
					retryQueued = true;
				}
				if (!stopping && (foreground || stillDesired) && completed &&
					currentSource && !work.dimensionsOnly) {
					// Speculative work must never evict an image the user has
					// already viewed. Stop this generation once the free budget
					// cannot hold its next nearest neighbor.
					const bool mayEvict = foreground ||
						effectiveWorkClass == PerfWorkClass::ActiveImageSpread ||
						effectiveWorkClass == PerfWorkClass::NearestNavigationNeighbor;
					const CacheProtectionTier protection =
						CacheProtectionForWorkClass(effectiveWorkClass, foreground);
					if (!Insert(work.key.source, image, mayEvict, protection,
						foreground || effectiveWorkClass == PerfWorkClass::ActiveImageSpread)) {
						QueueRetirement(image);
						if (sharedBudget) {
							const CacheBudgetSnapshot snapshot = sharedBudget->Snapshot();
							PerfDiagnostics::Instance().RecordText(PerfMetric::CacheSnapshot,
								DecodedImageBytes(*image), snapshot.retainedBytes,
								snapshot.activeWorkingBytes, snapshot.capacityBytes,
								static_cast<std::uint64_t>(effectiveWorkClass), 0,
								"decoded_retention_denied");
						}
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
					}
					completedImage = image;
				}
				if (!stopping && stillDesired) {
					if (work.dimensionsOnly) {
						dimensionsCompletion = desired->second.dimensionsCompletion;
					} else {
						completion = desired->second.completion;
						detailedCompletion = desired->second.detailedCompletion;
					}
				}
				--activeWorkers;
				idle.notify_all();
			}
			if (retryQueued) workAvailable.notify_one();
			try {
				if (completion) completion(work.filename, completedImage);
				if (detailedCompletion) {
					detailedCompletion(work.source, completedImage, workerFailure);
				}
			} catch (const std::exception& error) {
				std::lock_guard<std::mutex> lock(mutex);
				lastWorkerFailure = {WorkerFailureKind::Exception, error.what()};
			} catch (...) {
				std::lock_guard<std::mutex> lock(mutex);
				lastWorkerFailure = {WorkerFailureKind::Exception,
					"unknown decoded-image completion callback failure"};
			}
			try {
				if (dimensionsCompletion) dimensionsCompletion(work.filename,
					completed && currentSource, sourceWidth, sourceHeight);
			} catch (const std::exception& error) {
				std::lock_guard<std::mutex> lock(mutex);
				lastWorkerFailure = {WorkerFailureKind::Exception, error.what()};
			} catch (...) {
				std::lock_guard<std::mutex> lock(mutex);
				lastWorkerFailure = {WorkerFailureKind::Exception,
					"unknown JPEG dimensions callback failure"};
			}
		}
	}

	static void Retire(const std::shared_ptr<RetirementState>& state) {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
		for (;;) {
			RetiredImage retiredImage;
			{
				std::unique_lock<std::mutex> lock(state->mutex);
				for (;;) {
					auto ready = std::find_if(state->retired.begin(), state->retired.end(),
						[](const RetiredImage& candidate) {
							return candidate.image && candidate.image.use_count() == 1;
						});
					if (ready != state->retired.end()) {
						retiredImage = std::move(*ready);
						state->retired.erase(ready);
						state->retiringOwner = retiredImage.image;
						state->retiring = true;
						break;
					}
					if (state->stopping && state->retired.empty()) return;
					if (state->retired.empty()) {
						state->available.wait(lock, [&state] {
							return state->stopping || !state->retired.empty();
						});
					} else {
						state->available.wait_for(lock, std::chrono::milliseconds(2));
					}
				}
			}
			retiredImage.image.reset();
			retiredImage.reservation.Reset();
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				state->retiringOwner.reset();
				state->retiring = false;
			}
		}
	}

	const std::size_t byteBudget;
	Decoder decoder;
	DimensionsReader dimensionsReader;
	std::shared_ptr<SharedCacheBudget> sharedBudget;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable idle;
	std::vector<std::thread> workers;
	std::thread retirementWorker;
	std::shared_ptr<RetirementState> retirementState;
	EntryMap entries;
	std::array<LruList, 3> lru;
	std::deque<Work> queue;
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
	WorkerFailure lastWorkerFailure;
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
	impl_->Touch(found);
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
		impl_->Touch(found);
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
	if (!impl_->Insert(key, image, true, CacheProtectionTier::Active)) {
		impl_->QueueRetirement(image);
	}
	}
	impl_->workAvailable.notify_one();
}

void DecodedImageCache::PromoteToActiveUse(const SourceKey& source) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto entry = impl_->entries.find(source);
	if (entry == impl_->entries.end()) return;
	if (!impl_->SetProtection(entry, CacheProtectionTier::Active)) {
		impl_->Erase(entry);
		impl_->workAvailable.notify_one();
	}
}

void DecodedImageCache::SetProtectionSnapshot(
	const std::vector<std::pair<SourceKey, CacheProtectionTier>>& protections) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::unordered_map<SourceKey, CacheProtectionTier, SourceKeyHash> strongest;
	for (const auto& protection : protections) {
		auto inserted = strongest.emplace(protection.first, protection.second);
		if (!inserted.second && static_cast<unsigned>(protection.second) >
			static_cast<unsigned>(inserted.first->second)) {
			inserted.first->second = protection.second;
		}
	}
	std::vector<std::pair<SourceKey, CacheProtectionTier>> changes;
	for (std::size_t tier = 0; tier < impl_->lru.size(); ++tier) {
		const std::vector<SourceKey> keys(impl_->lru[tier].begin(),
			impl_->lru[tier].end());
		for (const SourceKey& key : keys) {
			const auto entry = impl_->entries.find(key);
			if (entry == impl_->entries.end()) continue;
			const auto desired = strongest.find(key);
			const CacheProtectionTier protection = desired == strongest.end() ?
				CacheProtectionTier::DistantSpeculation : desired->second;
			changes.emplace_back(key, protection);
		}
	}
	// Release retained ownership from all active entries before restoring
	// retained charges for entries moving to lower tiers. This keeps an
	// unchanged active entry from temporarily blocking a neighbor transition.
	std::stable_partition(changes.begin(), changes.end(),
		[](const auto& change) {
			return change.second == CacheProtectionTier::Active;
		});
	for (const auto& change : changes) {
		const auto entry = impl_->entries.find(change.first);
		if (entry != impl_->entries.end() &&
			!impl_->SetProtection(entry, change.second)) {
			impl_->Erase(entry);
		}
	}
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
			impl_->Touch(cached);
			impl_->SetProtection(cached,
				CacheProtectionForWorkClass(work.workClass));
			alreadyCached = cached->second.image;
		} else {
			const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
				[&work](const Impl::Work& candidate) { return candidate.key == work.key; });
			if (queued != impl_->queue.end()) {
				if (queued->cancellation) queued->cancellation->store(false);
				queued->generation = work.generation;
				queued->workClass = work.workClass;
				queued->completion = work.completion;
				queued->detailedCompletion = work.detailedCompletion;
				queued->cancellation = work.cancellation;
				work = *queued;
			} else if (impl_->inFlightKeys.find(work.key) != impl_->inFlightKeys.end()) {
				impl_->inFlightWorkClasses[work.key] = work.workClass;
				const auto cancellation = impl_->inFlightCancellation.find(work.key);
				if (cancellation != impl_->inFlightCancellation.end()) {
					if (const auto token = cancellation->second.lock()) {
						if (!token->load()) work.cancellation = token;
					}
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

void DecodedImageCache::RequestSelectedSource(const SourceDescriptor& source,
	DetailedCompletion completion, PerfWorkClass workClass) {
	Impl::Work work;
	work.filename = source.LogicalPath();
	work.key = WorkKey(source);
	work.source = source;
	work.workClass = workClass;
	work.detailedCompletion = std::move(completion);
	work.cancellation = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->EraseOtherSourceIdentities(work.key.source);
	}
	if (!work.key.source.Valid()) {
		if (work.detailedCompletion) {
			work.detailedCompletion(source, {}, WorkerFailure{
				WorkerFailureKind::SourceUnavailable, "image source identity is unavailable"});
		}
		return;
	}

	ImagePtr alreadyCached;
	bool queuedWork = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->stopping) return;
		work.generation = impl_->generation;
		const auto cached = impl_->entries.find(work.key.source);
		if (cached != impl_->entries.end()) {
			impl_->Touch(cached);
			impl_->SetProtection(cached,
				CacheProtectionForWorkClass(work.workClass));
			alreadyCached = cached->second.image;
		} else {
			const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
				[&work](const Impl::Work& candidate) { return candidate.key == work.key; });
			if (queued != impl_->queue.end()) {
				if (queued->cancellation) queued->cancellation->store(false);
				queued->generation = work.generation;
				queued->workClass = work.workClass;
				queued->completion = {};
				queued->detailedCompletion = work.detailedCompletion;
				queued->cancellation = work.cancellation;
				work = *queued;
			} else if (impl_->inFlightKeys.find(work.key) != impl_->inFlightKeys.end()) {
				impl_->inFlightWorkClasses[work.key] = work.workClass;
				const auto cancellation = impl_->inFlightCancellation.find(work.key);
				if (cancellation != impl_->inFlightCancellation.end()) {
					if (const auto token = cancellation->second.lock()) {
						if (!token->load()) work.cancellation = token;
					}
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
	if (alreadyCached && work.detailedCompletion) {
		work.detailedCompletion(source, alreadyCached, {});
	}
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
				if (const auto token = cancellation->second.lock()) {
					if (!token->load()) work.cancellation = token;
				}
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
				continue;
			}
			const bool remainsDesired = std::any_of(prepared.begin(), prepared.end(),
				[&active](const Impl::Work& work) { return work.key == active.first; });
			if (!remainsDesired) {
				if (const auto cancellation = active.second.lock()) cancellation->store(true);
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
				impl_->Touch(cached);
					impl_->SetProtection(cached,
						CacheProtectionForWorkClass(work.workClass));
				if (completion) alreadyCached.emplace_back(work.filename, cached->second.image);
				continue;
			}
			if (impl_->inFlightKeys.find(work.key) != impl_->inFlightKeys.end()) {
				const auto cancellation = impl_->inFlightCancellation.find(work.key);
				if (cancellation != impl_->inFlightCancellation.end()) {
					if (const auto token = cancellation->second.lock()) {
						if (!token->load()) {
							work.cancellation = token;
							impl_->desiredWork[work.key] = work;
						}
					}
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
	diagnostics.lastWorkerFailure = impl_->lastWorkerFailure;
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
	{
		std::lock_guard<std::mutex> retirementLock(impl_->retirementState->mutex);
		diagnostics.retiredImages = impl_->retirementState->retired.size();
		for (const Impl::RetiredImage& retired : impl_->retirementState->retired) {
			if (retired.image) diagnostics.retiredBytes += DecodedImageBytes(*retired.image);
		}
	}
	return diagnostics;
}

std::vector<SourceChangeNotice> DecodedImageCache::TakeChangedSources() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<SourceChangeNotice> changed;
	changed.swap(impl_->changedSources);
	return changed;
}

std::size_t DecodedImageCache::EvictLeastRecentlyUsed(CacheProtectionTier maximumTier) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto oldest = impl_->Oldest(maximumTier);
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
