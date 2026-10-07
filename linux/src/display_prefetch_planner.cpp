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
	const std::size_t count = std::min(request.maximumCount, request.neighbors.size());
	result.requests.reserve(count);
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

		Viewport viewport;
		ViewportSnapshot fittedViewport = request.viewport;
		fittedViewport.fitToWindow = true;
		fittedViewport.zoom = 1.0;
		fittedViewport.relativeZoom = 1.0;
		viewport.Restore(fittedViewport, sourceWidth, sourceHeight,
			request.imageAreaWidth, request.imageAreaHeight);
		const ViewportRect target = viewport.Destination(sourceWidth, sourceHeight,
			request.imageAreaWidth, request.imageAreaHeight);
		const DisplayImageTarget resolution = svg ?
			BoundSvgDisplayTarget(target.width, target.height) :
			ClampDisplayImageTarget(sourceWidth, sourceHeight, target.width, target.height);
		DisplayImageRequest displayRequest = svg ?
			MakeSvgDisplayImageRequest(candidate.source, sourceWidth, sourceHeight,
				resolution.width, resolution.height, candidate.autoContrast,
				candidate.priority, candidate.processing) :
			MakeJpegDisplayImageRequest(candidate.source, sourceWidth, sourceHeight,
				resolution.width, resolution.height, candidate.autoContrast,
				candidate.priority, candidate.processing);
		displayRequest.workClass = candidate.priority <= 2 ?
			PerfWorkClass::NearestNavigationNeighbor : PerfWorkClass::DistantSpeculation;
		if (!displayRequest.Valid()) continue;
		const RetainedDisplayTexture* retainedTexture =
			FindReusableRetainedDisplayTexture(displayRequest.cacheKey,
				request.retainedTextures);
		if (retainedTexture != nullptr) {
			if (candidate.priority <= 2) {
				result.protectedTextureKeys.push_back(retainedTexture->key);
				result.protectedTextureCacheKeys.push_back(retainedTexture->cacheKey);
			}
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
	const ViewportSnapshot& viewport, int imageAreaWidth, int imageAreaHeight) {
	return result.generation == expectedGeneration &&
		result.catalogRevision == catalogRevision &&
		result.descriptorRevision == descriptorRevision &&
		result.viewportRevision == viewportRevision &&
		result.currentIndex == currentIndex &&
		result.preferredDirection == preferredDirection &&
		SameViewportSnapshot(result.viewport, viewport) &&
		result.imageAreaWidth == imageAreaWidth &&
		result.imageAreaHeight == imageAreaHeight;
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
