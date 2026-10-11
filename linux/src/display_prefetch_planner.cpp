#include "display_prefetch_planner.h"

#include "archive_source.h"
#include "event_loop_model.h"
#include "image_cache.h"
#include "image_decoder.h"
#include "perf_diagnostics.h"
#include "source_work_coordinator.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace jpegview_linux {

std::optional<DisplayImageRequest> MakeFittedDisplayPrefetchRequest(
	const DisplayPrefetchCandidate& candidate,
	const DisplayPrefetchPlannerRequest& request,
	int sourceWidth, int sourceHeight) {
	if (sourceWidth <= 0 || sourceHeight <= 0 || request.imageAreaWidth <= 0 ||
		request.imageAreaHeight <= 0) return std::nullopt;
	ViewportSnapshot fittedViewport = request.viewport;
	fittedViewport.fitToWindow = true;
	fittedViewport.zoom = 1.0;
	fittedViewport.relativeZoom = 1.0;
	Viewport viewport;
	viewport.Restore(fittedViewport, sourceWidth, sourceHeight,
		request.imageAreaWidth, request.imageAreaHeight);
	const ViewportRect target = viewport.Destination(sourceWidth, sourceHeight,
		request.imageAreaWidth, request.imageAreaHeight);
	const bool svg = IsSvgPath(candidate.filename);
	const DisplayImageTarget resolution = svg ?
		BoundSvgDisplayTarget(target.width, target.height) :
		ClampDisplayImageTarget(sourceWidth, sourceHeight,
			target.width, target.height);
	DisplayImageRequest displayRequest = candidate.jpeg ?
		MakeJpegDisplayImageRequest(candidate.source, sourceWidth, sourceHeight,
			resolution.width, resolution.height, candidate.autoContrast,
			candidate.priority, candidate.processing, 0, request.includeSpectrum) : svg ?
		MakeSvgDisplayImageRequest(candidate.source, sourceWidth, sourceHeight,
			resolution.width, resolution.height, candidate.autoContrast,
			candidate.priority, candidate.processing, 0, request.includeSpectrum) :
		MakeRasterDisplayImageRequest(candidate.source, sourceWidth, sourceHeight,
			resolution.width, resolution.height, candidate.autoContrast,
			candidate.priority, candidate.processing, 0, request.includeSpectrum);
	displayRequest.workClass = candidate.priority <= 2 ?
		PerfWorkClass::NearestNavigationNeighbor : PerfWorkClass::DistantSpeculation;
	if (!displayRequest.Valid()) return std::nullopt;
	return displayRequest;
}

namespace {

DisplayPrefetchPlannerResult Plan(const DisplayPrefetchPlannerRequest& request,
	std::uint64_t generation, const std::shared_ptr<std::atomic<bool>>& canceled,
	const DisplayPrefetchPlannerWorker::DimensionsReader& dimensionsReader) {
	DisplayPrefetchPlannerResult result;
	result.generation = generation;
	result.catalogRevision = request.catalogRevision;
	result.descriptorRevision = request.descriptorRevision;
	result.viewportRevision = request.viewportRevision;
	result.currentIndex = request.currentIndex;
	result.preferredDirection = request.preferredDirection;
	result.viewport = request.viewport;
	result.imageAreaWidth = request.imageAreaWidth;
	result.imageAreaHeight = request.imageAreaHeight;
	result.includeSpectrum = request.includeSpectrum;
	const std::size_t count = std::min(request.maximumCount, request.neighbors.size());
	result.requests.reserve(count);
	result.reusableRequests.reserve(count);
	result.protectedTextureKeys.reserve(count);
	result.protectedTextureCacheKeys.reserve(count);
	result.dimensions.reserve(count);
	const auto shouldContinue = [&] {
		return !canceled || !canceled->load(std::memory_order_relaxed);
	};
	std::size_t estimatedPreparedBytes = 0;
	for (std::size_t position = 0; position < count && shouldContinue(); ++position) {
		const DisplayPrefetchCandidate& candidate = request.neighbors[position];
		const bool admissionBudgetAvailable =
			request.maximumPreparedBytes - estimatedPreparedBytes >= 4;
		if (!admissionBudgetAvailable && candidate.priority > 2) continue;
		const bool svg = IsSvgPath(candidate.filename);
		if (!candidate.jpeg && !svg) continue;
		PerfContextScope perfContext(candidate.priority <= 2 ?
			PerfWorkClass::NearestNavigationNeighbor :
			PerfWorkClass::DistantSpeculation, PerfExecution::WorkerThread);
		int sourceWidth = 0;
		int sourceHeight = 0;
		if (candidate.source.Metadata().hasDimensions) {
			sourceWidth = candidate.source.Metadata().width;
			sourceHeight = candidate.source.Metadata().height;
		} else {
			if (!candidate.source.Valid() || !shouldContinue()) continue;
			std::string errorMessage;
			WorkContext context = MakeWorkContext(candidate.source,
				SourceWorkPriority::Metadata, shouldContinue);
			SourceCpuWorkLease admission =
				SourceWorkCoordinator::Global().AcquireSourceAndCpu(
					context, candidate.source.LogicalPath());
			if (!admission) continue;
			context.sourcePriority = context.Priority();
			context.sourceAccessAlreadyAdmitted = true;
			context.cpuProcessingAlreadyAdmitted = true;
			bool dimensionsRead = false;
			bool sourceCurrentAfterRead = false;
			{
				ScopedWorkContext activeContext(context);
				dimensionsRead = context.Continue() && IsImageSourceCurrent(candidate.source) &&
					dimensionsReader(candidate.source, sourceWidth, sourceHeight,
						errorMessage, shouldContinue);
				sourceCurrentAfterRead = dimensionsRead && context.Continue() &&
					IsImageSourceCurrent(candidate.source);
			}
			context.sourceAccessAlreadyAdmitted = false;
			context.cpuProcessingAlreadyAdmitted = false;
			admission.Reset();
			if (!dimensionsRead || !sourceCurrentAfterRead || !shouldContinue()) continue;
		}
		if (sourceWidth <= 0 || sourceHeight <= 0) continue;
		result.dimensions.push_back({candidate.index, candidate.source.Key(),
			sourceWidth, sourceHeight});

		std::optional<DisplayImageRequest> fittedRequest =
			MakeFittedDisplayPrefetchRequest(candidate, request, sourceWidth, sourceHeight);
		if (!fittedRequest.has_value()) continue;
		DisplayImageRequest displayRequest = std::move(*fittedRequest);
		const RetainedDisplayTexture* retainedTexture =
			FindReusableRetainedDisplayTexture(displayRequest.cacheKey,
				request.retainedTextures);
		if (retainedTexture != nullptr) {
			if (candidate.priority <= 2) {
				result.protectedTextureKeys.push_back(retainedTexture->key);
				result.protectedTextureCacheKeys.push_back(retainedTexture->cacheKey);
			}
			// This renderer-thread snapshot may be stale by the time planning
			// completes, so ask the owner to verify residency before resolving it.
			result.reusableRequests.push_back(std::move(displayRequest));
			continue;
		}
		if (!admissionBudgetAvailable) continue;

		if (request.doublePageMode.enabled &&
			candidate.index == request.currentIndex + 1 &&
			request.currentPageDimensions.has_value() &&
			BuildDoublePageSpread(request.currentIndex, request.pageCount,
				*request.currentPageDimensions,
				std::optional<PageDimensions>{{sourceWidth, sourceHeight}},
				request.doublePageMode).has_value()) {
			continue;
		}
		std::size_t displayBytes = 0;
		if (!EstimateDisplayImageBytes(displayRequest, displayBytes) ||
			displayBytes > request.maximumPreparedBytes -
				std::min(request.maximumPreparedBytes, estimatedPreparedBytes)) continue;
		estimatedPreparedBytes += displayBytes;
		if (candidate.priority <= 2) {
			result.protectedTextureKeys.push_back(displayRequest.key);
			result.protectedTextureCacheKeys.push_back(displayRequest.cacheKey);
		}
		result.requests.push_back(std::move(displayRequest));
	}
	return result;
}

} // namespace

std::size_t DisplayPrefetchPlanningBudgetBytes(const CacheBudgetSnapshot& budget) {
	// Retained entries can be evicted by priority-aware admission. Planning
	// against only currently free bytes would prevent the nearest request from
	// ever reaching that admission path when the cache is full.
	return budget.capacityBytes;
}

DisplayPrefetchPlannerPhases SplitDisplayPrefetchPlannerRequest(
	const DisplayPrefetchPlannerRequest& request, std::size_t nearestCount) {
	DisplayPrefetchPlannerPhases phases;
	phases.nearest = request;
	phases.nearest.neighbors.clear();
	phases.nearest.maximumCount = 0;
	phases.secondary = request;
	phases.secondary.neighbors.clear();
	phases.secondary.maximumCount = 0;
	DisplayPrefetchPlannerRequest distant = request;
	distant.neighbors.clear();
	distant.maximumCount = 0;
	for (const DisplayPrefetchCandidate& candidate : request.neighbors) {
		if (candidate.priority <= nearestCount) {
			phases.nearest.neighbors.push_back(candidate);
		} else if (candidate.priority == nearestCount + 1) {
			phases.secondary.neighbors.push_back(candidate);
		} else {
			distant.neighbors.push_back(candidate);
		}
	}
	phases.nearest.maximumCount = phases.nearest.neighbors.size();
	phases.secondary.maximumCount = phases.secondary.neighbors.size();
	distant.maximumCount = distant.neighbors.size();
	if (phases.secondary.neighbors.empty()) phases.secondary.maximumCount = 0;
	if (!distant.neighbors.empty()) phases.distant = std::move(distant);
	return phases;
}

DisplayPrefetchContinuation::DisplayPrefetchContinuation(
	std::vector<SourceKey> nearestSources,
	std::vector<SourceKey> secondarySources,
	std::optional<DisplayPrefetchPlannerRequest> secondaryRequest,
	std::optional<DisplayPrefetchPlannerRequest> distantRequest,
	std::size_t maximumPreparedBytes, bool nearestPlannerPending,
	bool distantWorkPending)
	: secondarySources_(std::move(secondarySources)),
	  secondaryRequest_(std::move(secondaryRequest)),
	  distantRequest_(std::move(distantRequest)),
	  maximumPreparedBytes_(maximumPreparedBytes),
	  remainingPreparedBytes_(maximumPreparedBytes),
	  nearestPlannerPending_(nearestPlannerPending),
	  distantWorkPending_(distantWorkPending) {
	for (SourceKey& source : nearestSources) {
		if (source.Valid()) nearestSources_.insert(std::move(source));
	}
	secondarySources_.erase(std::remove_if(secondarySources_.begin(),
		secondarySources_.end(), [](const SourceKey& source) {
			return !source.Valid();
		}), secondarySources_.end());
}

void DisplayPrefetchContinuation::TrackNearestRequest(
	const DisplayImageRequest& request) {
	TrackNearestRequest(request, request.key);
}

void DisplayPrefetchContinuation::TrackNearestRequest(
	const DisplayImageRequest& request, const std::string& completionKey) {
	if (completionKey.empty() || !request.source.Valid()) return;
	std::size_t preparedBytes = 0;
	if (!EstimateDisplayImageBytes(request, preparedBytes)) preparedBytes = 0;
	std::lock_guard<std::mutex> lock(mutex_);
	if (canceled_ || nearestSources_.find(request.source.Key()) == nearestSources_.end() ||
		nearestRequests_.find(completionKey) != nearestRequests_.end()) return;
	const std::size_t reservedBytes = std::min(remainingPreparedBytes_, preparedBytes);
	remainingPreparedBytes_ -= reservedBytes;
	nearestRequests_.emplace(completionKey,
		RequestReservation{request.source.Key(), reservedBytes});
}

void DisplayPrefetchContinuation::ResolveNearestSource(const SourceKey& source) {
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (canceled_) return;
		changed = nearestSources_.erase(source) != 0;
		for (auto request = nearestRequests_.begin(); request != nearestRequests_.end();) {
			if (request->second.source != source) {
				++request;
				continue;
			}
			const std::size_t available = maximumPreparedBytes_ -
				std::min(maximumPreparedBytes_, remainingPreparedBytes_);
			remainingPreparedBytes_ += std::min(available,
				request->second.preparedBytes);
			request = nearestRequests_.erase(request);
			changed = true;
		}
	}
	if (changed) UiCompletionWakeup().Notify();
}

void DisplayPrefetchContinuation::ResolveNearestRequest(
	const std::string& requestKey, bool retained) {
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (canceled_) return;
		const auto request = nearestRequests_.find(requestKey);
		if (request == nearestRequests_.end()) return;
		nearestSources_.erase(request->second.source);
		if (!retained) {
			const std::size_t available = maximumPreparedBytes_ -
				std::min(maximumPreparedBytes_, remainingPreparedBytes_);
			remainingPreparedBytes_ += std::min(available,
				request->second.preparedBytes);
		}
		nearestRequests_.erase(request);
		changed = true;
	}
	if (changed) UiCompletionWakeup().Notify();
}

void DisplayPrefetchContinuation::MarkNearestPlannerFinished() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (canceled_ || !nearestPlannerPending_) return;
		nearestPlannerPending_ = false;
	}
	UiCompletionWakeup().Notify();
}

std::optional<DisplayPrefetchContinuationPhase>
DisplayPrefetchContinuation::TakeNextPhaseIfReady() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (canceled_ || nearestPlannerPending_ || !nearestSources_.empty()) {
		return std::nullopt;
	}
	if (!secondaryPhaseStarted_) {
		secondaryPhaseStarted_ = true;
		for (const SourceKey& source : secondarySources_) {
			nearestSources_.insert(source);
		}
		secondarySources_.clear();
		nearestPlannerPending_ = secondaryRequest_.has_value();
		std::optional<DisplayPrefetchPlannerRequest> request =
			std::move(secondaryRequest_);
		secondaryRequest_.reset();
		if (request.has_value()) request->maximumPreparedBytes = remainingPreparedBytes_;
		if (!nearestSources_.empty() || nearestPlannerPending_) {
			return DisplayPrefetchContinuationPhase{2, std::move(request)};
		}
	}
	if (nearestPlannerPending_ || !nearestSources_.empty() || distantPhaseStarted_ ||
		!distantWorkPending_) return std::nullopt;
	distantPhaseStarted_ = true;
	distantWorkPending_ = false;
	std::optional<DisplayPrefetchPlannerRequest> request = std::move(distantRequest_);
	distantRequest_.reset();
	if (request.has_value()) request->maximumPreparedBytes = remainingPreparedBytes_;
	return DisplayPrefetchContinuationPhase{3, std::move(request)};
}

void DisplayPrefetchContinuation::Cancel() {
	std::lock_guard<std::mutex> lock(mutex_);
	canceled_ = true;
	secondarySources_.clear();
	secondaryRequest_.reset();
	distantRequest_.reset();
	distantWorkPending_ = false;
	nearestSources_.clear();
	nearestRequests_.clear();
}

const RetainedDisplayTexture* FindReusableRetainedDisplayTexture(
	const DisplayImageCacheKey& requested,
	const std::vector<RetainedDisplayTexture>& retainedTextures) {
	for (const RetainedDisplayTexture& retained : retainedTextures) {
		if (CanReuseDisplayImageRepresentation(requested, retained.cacheKey)) {
			return &retained;
		}
	}
	return nullptr;
}

bool ShouldRetainPendingDisplayUpload(const SourceKey& uploadSource,
	const SourceKey& selectedSource, const std::vector<SourceKey>& neighborSources) {
	if (!uploadSource.Valid()) return false;
	if (uploadSource == selectedSource) return true;
	return std::find(neighborSources.begin(), neighborSources.end(), uploadSource) !=
		neighborSources.end();
}

bool SameViewportSnapshot(const ViewportSnapshot& left,
	const ViewportSnapshot& right) {
	return left.fitToWindow == right.fitToWindow &&
		left.fillWithCrop == right.fillWithCrop && left.noEnlarge == right.noEnlarge &&
		left.zoom == right.zoom && left.relativeZoom == right.relativeZoom;
}

bool ShouldDeactivateDisplayPrefetchBatch(DisplayPrefetchBatchOwner owner,
	DisplayPrefetchBatchInvalidation invalidation, bool activeSpreadRequestStillCurrent) {
	return invalidation != DisplayPrefetchBatchInvalidation::ViewportChanged ||
		owner != DisplayPrefetchBatchOwner::ActiveSpread || !activeSpreadRequestStillCurrent;
}

std::shared_ptr<const DecodedImage> RetainDisplayPrefetchDecodedImage(
	DisplayPrefetchBatchOwner owner,
	const std::shared_ptr<const DecodedImage>& decoded) {
	return owner == DisplayPrefetchBatchOwner::ActiveSpread ? decoded :
		std::shared_ptr<const DecodedImage>{};
}

void AppendDisplayPrefetchRequests(std::vector<DisplayImageRequest>& requests,
	const std::vector<DisplayImageRequest>& additions) {
	requests.insert(requests.end(), additions.begin(), additions.end());
}

bool MatchesDisplayPrefetchSnapshot(const DisplayPrefetchPlannerResult& result,
	std::uint64_t expectedGeneration, std::uint64_t catalogRevision,
	std::uint64_t descriptorRevision, std::uint64_t viewportRevision,
	std::size_t currentIndex, int preferredDirection,
	const ViewportSnapshot& viewport, int imageAreaWidth, int imageAreaHeight,
	bool includeSpectrum) {
	return result.generation == expectedGeneration &&
		result.catalogRevision == catalogRevision &&
		result.descriptorRevision == descriptorRevision &&
		result.viewportRevision == viewportRevision &&
		result.currentIndex == currentIndex &&
		result.preferredDirection == preferredDirection &&
		SameViewportSnapshot(result.viewport, viewport) &&
		result.imageAreaWidth == imageAreaWidth &&
		result.imageAreaHeight == imageAreaHeight &&
		result.includeSpectrum == includeSpectrum;
}

struct DisplayPrefetchPlannerWorker::Impl {
	struct Work {
		DisplayPrefetchPlannerRequest request;
		std::uint64_t generation = 0;
		std::shared_ptr<std::atomic<bool>> canceled;
	};

	explicit Impl(DimensionsReader readDimensions)
		: dimensionsReader(std::move(readDimensions)) {
		if (!dimensionsReader) {
			dimensionsReader = [](const SourceDescriptor& source, int& width, int& height,
				std::string& errorMessage, const Continue& shouldContinue) {
			if (!shouldContinue()) return false;
			WorkContext context = ResolveWorkContext(source.LogicalPath(),
				SourceWorkPriority::Metadata);
			context.shouldContinue = shouldContinue;
			const bool read = IsSvgPath(source.LogicalPath()) ?
				ReadSvgDimensions(source.LogicalPath(), width, height, errorMessage, context) :
				ReadJpegDimensions(source.LogicalPath(), width, height, errorMessage, context);
			return read && shouldContinue();
		};
		}
		worker = std::thread([this] { Run(); });
	}

	~Impl() { Stop(); }

	void Run() {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
		for (;;) {
			Work work;
			{
				std::unique_lock<std::mutex> lock(mutex);
				available.wait(lock, [this] { return stopping || pending.has_value(); });
				if (stopping) return;
				work = std::move(*pending);
				pending.reset();
				activeCancellation = work.canceled;
				active = true;
			}

			DisplayPrefetchPlannerResult result;
			try {
				result = Plan(work.request, work.generation, work.canceled,
					dimensionsReader);
			} catch (const std::exception& error) {
				result.failure = {WorkerFailureKind::Exception, error.what()};
				result.requests.clear();
				result.reusableRequests.clear();
				result.protectedTextureKeys.clear();
				result.protectedTextureCacheKeys.clear();
				result.dimensions.clear();
				result.generation = work.generation;
				result.catalogRevision = work.request.catalogRevision;
				result.descriptorRevision = work.request.descriptorRevision;
				result.viewportRevision = work.request.viewportRevision;
				result.currentIndex = work.request.currentIndex;
			} catch (...) {
				result.failure = {WorkerFailureKind::Exception,
					"unknown display prefetch planning failure"};
				result.requests.clear();
				result.reusableRequests.clear();
				result.protectedTextureKeys.clear();
				result.protectedTextureCacheKeys.clear();
				result.dimensions.clear();
				result.generation = work.generation;
				result.catalogRevision = work.request.catalogRevision;
				result.descriptorRevision = work.request.descriptorRevision;
				result.viewportRevision = work.request.viewportRevision;
				result.currentIndex = work.request.currentIndex;
			}
			bool published = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				active = false;
				activeCancellation.reset();
				if (!stopping && !work.canceled->load(std::memory_order_relaxed) &&
					work.generation == generation) {
					ready.clear();
					ready.push_back(std::move(result));
					published = true;
				}
				idle.notify_all();
			}
			if (published) UiCompletionWakeup().Notify();
		}
	}

	std::uint64_t Request(DisplayPrefetchPlannerRequest request) {
		if (request.neighbors.size() > request.maximumCount) {
			request.neighbors.resize(request.maximumCount);
		}
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping) return generation;
		++generation;
		if (activeCancellation) activeCancellation->store(true, std::memory_order_relaxed);
		if (pending && pending->canceled) pending->canceled->store(true, std::memory_order_relaxed);
		ready.clear();
		pending = Work{std::move(request), generation,
			std::make_shared<std::atomic<bool>>(false)};
		available.notify_one();
		return generation;
	}

	void Cancel() {
		std::lock_guard<std::mutex> lock(mutex);
		++generation;
		if (activeCancellation) activeCancellation->store(true, std::memory_order_relaxed);
		if (pending && pending->canceled) pending->canceled->store(true, std::memory_order_relaxed);
		pending.reset();
		ready.clear();
		idle.notify_all();
	}

	void Stop() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping) return;
			stopping = true;
			++generation;
			if (activeCancellation) activeCancellation->store(true, std::memory_order_relaxed);
			pending.reset();
			ready.clear();
		}
		available.notify_all();
		if (worker.joinable()) worker.join();
	}

	std::vector<DisplayPrefetchPlannerResult> TakeReady() {
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<DisplayPrefetchPlannerResult> result;
		result.swap(ready);
		return result;
	}

	bool WaitUntilIdle(std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(mutex);
		return idle.wait_for(lock, timeout, [this] {
			return !pending.has_value() && !active;
		});
	}

	DimensionsReader dimensionsReader;
	std::mutex mutex;
	std::condition_variable available;
	std::condition_variable idle;
	std::thread worker;
	std::optional<Work> pending;
	std::shared_ptr<std::atomic<bool>> activeCancellation;
	std::vector<DisplayPrefetchPlannerResult> ready;
	std::uint64_t generation = 0;
	bool active = false;
	bool stopping = false;
};

DisplayPrefetchPlannerWorker::DisplayPrefetchPlannerWorker(DimensionsReader reader)
	: impl_(std::make_unique<Impl>(std::move(reader))) {}

DisplayPrefetchPlannerWorker::~DisplayPrefetchPlannerWorker() = default;

std::uint64_t DisplayPrefetchPlannerWorker::Request(DisplayPrefetchPlannerRequest request) {
	return impl_->Request(std::move(request));
}

void DisplayPrefetchPlannerWorker::Cancel() { impl_->Cancel(); }

void DisplayPrefetchPlannerWorker::Stop() { impl_->Stop(); }

std::vector<DisplayPrefetchPlannerResult> DisplayPrefetchPlannerWorker::TakeReady() {
	return impl_->TakeReady();
}

bool DisplayPrefetchPlannerWorker::WaitUntilIdle(std::chrono::milliseconds timeout) {
	return impl_->WaitUntilIdle(timeout);
}

} // namespace jpegview_linux
