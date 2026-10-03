#include "thumbnail_resampler.h"

#include "image_decoder.h"
#include "perf_diagnostics.h"
#include "source_work_coordinator.h"
#include "thumbnail_panel_model.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <utility>

namespace jpegview_linux {

namespace {

void RecordThumbnailCancellation(PerfWorkClass workClass,
	PerfExecution execution = PerfExecution::EventThread) {
	PerfContextScope context(workClass, execution);
	PerfDiagnostics::Instance().Record(PerfMetric::Cancellation);
}

template <typename Left, typename Right>
bool SameSharedOwnership(const Left& left, const Right& right) {
	return !left.owner_before(right) && !right.owner_before(left);
}

bool EstimateThumbnailBytes(const ThumbnailPreparationRequest& request,
	std::size_t& bytes) {
	bytes = 0;
	if (request.maximumWidth <= 0 || request.maximumHeight <= 0) return false;
	const std::size_t width = static_cast<std::size_t>(request.maximumWidth);
	const std::size_t height = static_cast<std::size_t>(request.maximumHeight);
	const std::size_t maximum = std::numeric_limits<std::size_t>::max();
	if (width > maximum / height) return false;
	const std::size_t pixels = width * height;
	if (pixels > maximum / 4) return false;
	bytes = pixels * 4;
	return true;
}

} // namespace

bool DownsampleThumbnailBgra(const std::vector<std::uint8_t>& source,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	std::vector<std::uint8_t>& target,
	const std::function<bool()>& shouldContinue) {
	PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Resampling,
		static_cast<std::uint64_t>(sourceWidth) * static_cast<std::uint64_t>(sourceHeight),
		static_cast<std::uint64_t>(targetWidth) * static_cast<std::uint64_t>(targetHeight));
	if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0 ||
		targetWidth > sourceWidth || targetHeight > sourceHeight) return false;
	const std::size_t maximum = std::numeric_limits<std::size_t>::max();
	const std::size_t sourcePixels = static_cast<std::size_t>(sourceWidth) * sourceHeight;
	const std::size_t targetPixels = static_cast<std::size_t>(targetWidth) * targetHeight;
	if (sourcePixels > maximum / 4 || targetPixels > maximum / 4 || source.size() < sourcePixels * 4) {
		return false;
	}
	try {
		if (sourceWidth == targetWidth && sourceHeight == targetHeight) {
			if (shouldContinue && !shouldContinue()) return false;
			target.assign(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(sourcePixels * 4));
			return true;
		}
		target.assign(targetPixels * 4, 0);
	} catch (const std::exception&) {
		return false;
	}

	const double horizontalScale = static_cast<double>(sourceWidth) / targetWidth;
	const double verticalScale = static_cast<double>(sourceHeight) / targetHeight;
	const double coveredArea = horizontalScale * verticalScale;
	for (int targetY = 0; targetY < targetHeight; ++targetY) {
		if (shouldContinue && !shouldContinue()) {
			target.clear();
			return false;
		}
		const double sourceTop = targetY * verticalScale;
		const double sourceBottom = (targetY + 1) * verticalScale;
		const int firstY = static_cast<int>(std::floor(sourceTop));
		const int lastY = std::min(sourceHeight - 1,
			static_cast<int>(std::ceil(sourceBottom)) - 1);
		for (int targetX = 0; targetX < targetWidth; ++targetX) {
			const double sourceLeft = targetX * horizontalScale;
			const double sourceRight = (targetX + 1) * horizontalScale;
			const int firstX = static_cast<int>(std::floor(sourceLeft));
			const int lastX = std::min(sourceWidth - 1,
				static_cast<int>(std::ceil(sourceRight)) - 1);
			double weightedAlpha = 0.0;
			double weightedBlue = 0.0;
			double weightedGreen = 0.0;
			double weightedRed = 0.0;
			for (int sourceY = firstY; sourceY <= lastY; ++sourceY) {
				const double verticalCoverage = std::max(0.0,
					std::min(sourceBottom, sourceY + 1.0) - std::max(sourceTop, static_cast<double>(sourceY)));
				for (int sourceX = firstX; sourceX <= lastX; ++sourceX) {
					const double horizontalCoverage = std::max(0.0,
						std::min(sourceRight, sourceX + 1.0) - std::max(sourceLeft, static_cast<double>(sourceX)));
					const double coverage = horizontalCoverage * verticalCoverage;
					const std::size_t offset =
						(static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4;
					const double alpha = source[offset + 3];
					const double alphaCoverage = alpha * coverage;
					weightedAlpha += alphaCoverage;
					weightedBlue += source[offset] * alphaCoverage;
					weightedGreen += source[offset + 1] * alphaCoverage;
					weightedRed += source[offset + 2] * alphaCoverage;
				}
			}
			const std::size_t output =
				(static_cast<std::size_t>(targetY) * targetWidth + targetX) * 4;
			target[output + 3] = static_cast<std::uint8_t>(std::clamp(
				std::lround(weightedAlpha / coveredArea), 0l, 255l));
			if (weightedAlpha > 0.0) {
				target[output] = static_cast<std::uint8_t>(std::clamp(
					std::lround(weightedBlue / weightedAlpha), 0l, 255l));
				target[output + 1] = static_cast<std::uint8_t>(std::clamp(
					std::lround(weightedGreen / weightedAlpha), 0l, 255l));
				target[output + 2] = static_cast<std::uint8_t>(std::clamp(
					std::lround(weightedRed / weightedAlpha), 0l, 255l));
			}
		}
	}
	return true;
}

bool CanReuseDisplayPixelsForThumbnail(int width, int height,
	std::size_t maximumPixels) {
	if (width <= 0 || height <= 0 || maximumPixels == 0) return false;
	const std::size_t sourceWidth = static_cast<std::size_t>(width);
	const std::size_t sourceHeight = static_cast<std::size_t>(height);
	return sourceWidth <= maximumPixels / sourceHeight;
}

bool ThumbnailPreparationRequest::Valid() const {
	const bool validDisplaySource = source && source->width > 0 && source->height > 0;
	const bool validFileSource = !logicalSource.empty();
	return !key.Empty() && (validDisplaySource || validFileSource) &&
		maximumWidth > 0 && maximumHeight > 0;
}

bool ThumbnailPreparationResultMatches(const ThumbnailPreparationResult& result,
	std::uint64_t catalogRevision, std::uint64_t geometryRevision,
	std::size_t fileIndex, const SourceKey& sourceKey,
	int maximumWidth, int maximumHeight) {
	return result.catalogRevision == catalogRevision &&
		result.geometryRevision == geometryRevision && result.fileIndex == fileIndex &&
		result.key == sourceKey &&
		result.maximumWidth == maximumWidth && result.maximumHeight == maximumHeight;
}

bool ThumbnailPreparationResultMatches(const ThumbnailPreparationResult& result,
	std::uint64_t catalogRevision, std::uint64_t geometryRevision,
	std::size_t fileIndex, const std::string& sourceKey,
	int maximumWidth, int maximumHeight) {
	return ThumbnailPreparationResultMatches(result, catalogRevision,
		geometryRevision, fileIndex, SourceKey(sourceKey), maximumWidth,
		maximumHeight);
}

void PreserveThumbnailPreparationRetry(
	std::optional<ThumbnailPreparationResult>& pending,
	ThumbnailPreparationResult& failedResult) {
	if (pending && &*pending == &failedResult) return;
	pending = std::move(failedResult);
}

namespace {

bool ValidateOrRefreshThumbnailSource(const SourceDescriptor& requested,
	const std::function<bool()>& shouldContinue, SourceDescriptor& observed) {
	const std::filesystem::path path = requested.LogicalPath();
	if (path.empty()) return false;
	WorkContext context = requested.Valid() ?
		MakeWorkContext(requested, SourceWorkPriority::Metadata, shouldContinue) :
		MakePathWorkContext(path, SourceWorkPriority::Metadata, shouldContinue);
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
		context, path);
	if (!admission || !context.Continue()) return false;
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	bool current = false;
	{
		ScopedWorkContext activeContext(context);
		current = requested.Valid() && context.Continue() &&
			IsImageSourceCurrent(requested);
	}
	context.sourceAccessAlreadyAdmitted = false;
	context.cpuProcessingAlreadyAdmitted = false;
	admission.Reset();
	if (!current && shouldContinue()) {
		WorkContext refreshContext = MakePathWorkContext(path,
			SourceWorkPriority::Metadata, shouldContinue);
		SourceCpuWorkLease refreshAdmission =
			SourceWorkCoordinator::Global().AcquireSourceAndCpu(refreshContext, path);
		if (refreshAdmission && refreshContext.Continue()) {
			refreshContext.sourcePriority = refreshContext.Priority();
			refreshContext.sourceAccessAlreadyAdmitted = true;
			refreshContext.cpuProcessingAlreadyAdmitted = true;
			{
				ScopedWorkContext activeContext(refreshContext);
				observed = DescribeImageSource(path);
			}
			refreshContext.sourceAccessAlreadyAdmitted = false;
			refreshContext.cpuProcessingAlreadyAdmitted = false;
			refreshAdmission.Reset();
			if (!requested.Valid() && !observed.Valid()) observed = SourceDescriptor{};
		}
	}
	return current;
}

ThumbnailPreparationWorker::ImagePtr PrepareThumbnail(
	const ThumbnailPreparationRequest& request) {
	if (!request.Valid()) return {};
	const auto shouldContinue = [&request] {
		return !request.cancellation || !request.cancellation->load();
	};
	if (!shouldContinue()) return {};
	WorkContext workContext = request.sourceDescriptor.Valid() ?
		MakeWorkContext(request.sourceDescriptor, SourceWorkPriority::Speculative) :
		MakePathWorkContext(request.logicalSource, SourceWorkPriority::Speculative);
	workContext.source = request.key;
	workContext.shouldContinue = shouldContinue;
	if (request.sourceDescriptor.Valid()) {
		SourceDescriptor observed;
		if (!ValidateOrRefreshThumbnailSource(request.sourceDescriptor,
			shouldContinue, observed)) return {};
	}
	DecodedImage decoded;
	const std::vector<std::uint8_t>* sourcePixels = nullptr;
	int sourceWidth = 0;
	int sourceHeight = 0;
	bool hasTransparency = false;
	ThumbnailSize size;
	if (request.source) {
		sourceWidth = request.source->width;
		sourceHeight = request.source->height;
		hasTransparency = request.source->hasTransparency;
		sourcePixels = &request.source->bgra;
		size = FitThumbnailSize(sourceWidth, sourceHeight,
			request.maximumWidth, request.maximumHeight);
	} else {
		std::string errorMessage;
		if (IsJpegPath(request.logicalSource)) {
			int jpegWidth = 0;
			int jpegHeight = 0;
			if (!ReadJpegDimensions(request.logicalSource, jpegWidth, jpegHeight,
				errorMessage, workContext) || !shouldContinue()) return {};
			size = FitThumbnailSize(jpegWidth, jpegHeight,
				request.maximumWidth, request.maximumHeight);
			int decodedSourceWidth = 0;
			int decodedSourceHeight = 0;
			if (size.width <= 0 || size.height <= 0 ||
				!DecodeJpegForDisplay(request.logicalSource, size.width, size.height,
					decoded, decodedSourceWidth, decodedSourceHeight, errorMessage,
					workContext) ||
				decoded.frames.empty() || !shouldContinue()) return {};
		} else if (!DecodeImage(request.logicalSource, decoded, errorMessage, workContext) ||
			decoded.frames.empty() || !shouldContinue()) {
			return {};
		}
		const DecodedFrame& frame = decoded.frames.front();
		sourceWidth = frame.width;
		sourceHeight = frame.height;
		hasTransparency = frame.hasTransparency;
		sourcePixels = &frame.bgra;
		if (size.width <= 0 || size.height <= 0) {
			size = FitThumbnailSize(sourceWidth, sourceHeight,
				request.maximumWidth, request.maximumHeight);
		}
	}
	if (size.width <= 0 || size.height <= 0) return {};
	if (!shouldContinue()) return {};
	auto prepared = std::make_shared<PreparedThumbnailImage>();
	prepared->key = request.key;
	prepared->sourceDescriptor = request.sourceDescriptor;
	prepared->width = size.width;
	prepared->height = size.height;
	prepared->hasTransparency = hasTransparency;
	prepared->workClass = request.workClass;
	CpuWorkLease cpuLease = SourceWorkCoordinator::Global().AcquireCpu(workContext);
	if (!cpuLease || !shouldContinue()) return {};
	if (!DownsampleThumbnailBgra(*sourcePixels, sourceWidth, sourceHeight,
		size.width, size.height, prepared->bgra,
		[&workContext, &shouldContinue] {
			return workContext.Continue() && shouldContinue();
		})) return {};
	return prepared;
}

} // namespace

struct ThumbnailPreparationWorker::Impl {
	using ImagePtr = ThumbnailPreparationWorker::ImagePtr;
	using Result = ThumbnailPreparationWorker::Result;

	struct Work {
		ThumbnailPreparationRequest request;
		std::uint64_t generation = 0;
		std::size_t reservedBytes = 0;
	};

	struct Completion {
		Result result;
		std::size_t reservedBytes = 0;
	};

	struct RetirementState {
		std::mutex mutex;
		std::condition_variable available;
		std::deque<ImagePtr> images;
		std::weak_ptr<const PreparedThumbnailImage> retiringOwner;
		bool stopping = false;
	};

	explicit Impl(Processor prepare) : processor(std::move(prepare)) {
		if (!processor) processor = PrepareThumbnail;
	}

	~Impl() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			++generation;
			if (activeCancellation) activeCancellation->store(true);
		}
		workAvailable.notify_one();
		if (worker.joinable()) worker.join();
		bool externallyOwnedRetirement = false;
		{
			std::lock_guard<std::mutex> lock(retirementState->mutex);
			for (const ImagePtr& image : retirementState->images) {
				if (image.use_count() > 1) externallyOwnedRetirement = true;
			}
			retirementState->stopping = true;
		}
		retirementState->available.notify_all();
		if (retirementWorker.joinable()) {
			if (externallyOwnedRetirement) retirementWorker.detach();
			else retirementWorker.join();
		}
	}

	static void Retire(const std::shared_ptr<RetirementState>& state) {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
		for (;;) {
			ImagePtr image;
			{
				std::unique_lock<std::mutex> lock(state->mutex);
				const auto ready = std::find_if(state->images.begin(), state->images.end(),
					[](const ImagePtr& candidate) { return candidate.use_count() == 1; });
				if (ready == state->images.end()) {
					if (state->images.empty()) {
						if (state->stopping) return;
						state->available.wait(lock, [&state] {
							return state->stopping || !state->images.empty();
						});
					} else {
						state->available.wait_for(lock, std::chrono::milliseconds(2));
					}
					continue;
				}
				image = std::move(*ready);
				state->images.erase(ready);
				state->retiringOwner = image;
			}
			image.reset();
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				state->retiringOwner.reset();
			}
		}
	}

	void QueueRetirement(ImagePtr image) noexcept {
		if (!image) return;
		const PreparedThumbnailImage* identity = image.get();
		try {
			{
				std::lock_guard<std::mutex> lock(retirementState->mutex);
				if (SameSharedOwnership(image, retirementState->retiringOwner) ||
					std::any_of(retirementState->images.begin(), retirementState->images.end(),
						[identity](const ImagePtr& queued) { return queued.get() == identity; })) return;
				if (!retirementWorker.joinable()) {
					retirementWorker = std::thread([state = retirementState] { Retire(state); });
				}
				retirementState->images.push_back(image);
			}
			retirementState->available.notify_one();
		} catch (...) {
			// Keep allocation failures from killing a worker. The source queue lock
			// has been released before this last-owner fallback runs.
			image.reset();
		}
	}

	bool CanStart(const Work& work) const {
		std::size_t bytes = 0;
		return EstimateThumbnailBytes(work.request, bytes) &&
			reservedCompletionBytes <= ThumbnailPreparationWorker::kMaximumCompletedBytes &&
			bytes <= ThumbnailPreparationWorker::kMaximumCompletedBytes -
				reservedCompletionBytes &&
			reservedCompletionImages < ThumbnailPreparationWorker::kMaximumCompletedResults;
	}

	bool HasStartableWork() const {
		return std::any_of(queue.begin(), queue.end(), [this](const Work& work) {
			return CanStart(work);
		});
	}

	std::deque<Work>::iterator FindStartableWork() {
		auto best = queue.end();
		for (auto work = queue.begin(); work != queue.end(); ++work) {
			if (CanStart(*work) && (best == queue.end() ||
				work->request.priority < best->request.priority)) best = work;
		}
		return best;
	}

	void ReleaseReservation(std::size_t bytes, bool releaseImage) {
		reservedCompletionBytes -= std::min(reservedCompletionBytes, bytes);
		if (releaseImage && reservedCompletionImages != 0) --reservedCompletionImages;
	}

	void Run() {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
		for (;;) {
			Work work;
			DisplayImageCache::ImagePtr retiredSource;
			DisplayImageCache::ImagePtr completedSource;
			std::deque<Work> abandonedWork;
			std::deque<Completion> abandonedCompletions;
			std::deque<DisplayImageCache::ImagePtr> abandonedSources;
			bool stopNow = false;
			bool hasWork = false;
			{
				std::unique_lock<std::mutex> lock(mutex);
				workAvailable.wait(lock, [this] {
					return stopping || !retired.empty() || HasStartableWork();
				});
				if (stopping) {
					stopNow = true;
					abandonedWork.swap(queue);
					abandonedCompletions.swap(completed);
					abandonedSources.swap(retired);
					for (Completion& pending : abandonedCompletions) {
						ReleaseReservation(pending.reservedBytes, true);
					}
					idle.notify_all();
				} else {
					if (!retired.empty()) {
						retiredSource = std::move(retired.front());
						retired.pop_front();
					}
					if (!retiredSource) {
						auto selected = FindStartableWork();
						if (selected == queue.end()) continue;
						std::size_t reservedBytes = 0;
						if (!EstimateThumbnailBytes(selected->request, reservedBytes) ||
							reservedBytes > ThumbnailPreparationWorker::kMaximumCompletedBytes ||
							!CanStart(*selected)) continue;
						work = std::move(*selected);
						queue.erase(selected);
						hasWork = true;
						work.reservedBytes = reservedBytes;
						reservedCompletionBytes += work.reservedBytes;
						++reservedCompletionImages;
						activeCancellation = work.request.cancellation;
						activeWorkClass = work.request.workClass;
						activeRequest = &work.request;
						activeCancellationReported = false;
						activeSource = work.request.source;
						++activeWorkers;
					}
				}
			}
			if (stopNow) {
				for (auto& source : abandonedSources) source.reset();
				for (auto& pending : abandonedWork) pending.request.source.reset();
				for (auto& pending : abandonedCompletions) {
					QueueRetirement(std::move(pending.result.image));
				}
				return;
			}
			retiredSource.reset();
			if (!hasWork) continue;

			PerfContextScope context(work.request.workClass, PerfExecution::WorkerThread);
			ImagePtr image;
			SourceDescriptor observedSource;
			WorkerFailure processingFailure;
			const auto shouldContinue = [this, generation = work.generation,
				cancellation = work.request.cancellation] {
				if (cancellation && cancellation->load()) return false;
				std::lock_guard<std::mutex> lock(mutex);
				return !stopping && generation == this->generation;
			};
			try {
				const SourceDescriptor& sourceDescriptor = work.request.sourceDescriptor;
				const bool hasDescriptorPath = !sourceDescriptor.LogicalPath().empty();
				if (hasDescriptorPath) {
					if (ValidateOrRefreshThumbnailSource(sourceDescriptor,
						shouldContinue, observedSource)) {
						image = processor(work.request);
						if (sourceDescriptor.Valid() &&
							!ValidateOrRefreshThumbnailSource(sourceDescriptor,
								shouldContinue, observedSource)) image.reset();
					}
				} else {
					image = processor(work.request);
				}
			} catch (const std::exception& error) {
				processingFailure = {WorkerFailureKind::Exception, error.what()};
				image.reset();
			} catch (...) {
				processingFailure = {WorkerFailureKind::Exception,
					"unknown thumbnail preparation failure"};
				image.reset();
			}
			work.request.source.reset();
			retiredSource.reset();
			ImagePtr retiredResult;
			Result result;
			result.observedSource = std::move(observedSource);
			result.fileIndex = work.request.fileIndex;
			result.catalogRevision = work.request.catalogRevision;
			result.geometryRevision = work.request.geometryRevision;
			result.maximumWidth = work.request.maximumWidth;
			result.maximumHeight = work.request.maximumHeight;
			result.workClass = work.request.workClass;
			result.failure = std::move(processingFailure);
			{
				std::lock_guard<std::mutex> lock(mutex);
				--activeWorkers;
				const bool wasCancelled = work.request.cancellation &&
					work.request.cancellation->load();
				if (!stopping && work.generation == generation) {
					if (!wasCancelled || !activeCancellationReported) {
						result.cancelled = wasCancelled;
						if (!wasCancelled && image && image->key == work.request.key) {
							const std::size_t actualBytes = image->bgra.size();
							const std::size_t extraBytes = actualBytes > work.reservedBytes ?
								actualBytes - work.reservedBytes : 0;
							const bool fitsReservation = actualBytes <=
								ThumbnailPreparationWorker::kMaximumCompletedBytes &&
								extraBytes <= ThumbnailPreparationWorker::kMaximumCompletedBytes -
									reservedCompletionBytes;
							if (fitsReservation) {
								reservedCompletionBytes += extraBytes;
								reservedCompletionBytes -= work.reservedBytes -
									std::min(work.reservedBytes, actualBytes);
								work.reservedBytes = actualBytes;
								result.image = std::move(image);
							} else {
								retiredResult = std::move(image);
								ReleaseReservation(work.reservedBytes, false);
								work.reservedBytes = 0;
							}
						} else {
							if (image) retiredResult = std::move(image);
							ReleaseReservation(work.reservedBytes, false);
							work.reservedBytes = 0;
						}
						activeRequest = nullptr;
						result.key = std::move(work.request.key);
						ImagePtr publicationGuard = result.image;
						try {
							completed.push_back({std::move(result), work.reservedBytes});
						} catch (...) {
							ReleaseReservation(work.reservedBytes, true);
							retiredResult = std::move(publicationGuard);
							work.reservedBytes = 0;
						}
					} else {
						ReleaseReservation(work.reservedBytes, true);
						retiredResult = std::move(image);
					}
					if (wasCancelled) RecordThumbnailCancellation(work.request.workClass,
						PerfExecution::WorkerThread);
				} else if (!stopping && work.generation != generation) {
					RecordThumbnailCancellation(work.request.workClass,
						PerfExecution::WorkerThread);
					ReleaseReservation(work.reservedBytes, true);
					retiredResult = std::move(image);
				} else {
					ReleaseReservation(work.reservedBytes, true);
					retiredResult = std::move(image);
				}
				activeCancellation.reset();
				activeRequest = nullptr;
				activeCancellationReported = false;
				completedSource = std::move(activeSource);
				idle.notify_all();
			}
			QueueRetirement(std::move(retiredResult));
			completedSource.reset();
			workAvailable.notify_all();
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	static constexpr std::size_t kMaximumQueuedSources = 2;
	Processor processor;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable idle;
	std::deque<Work> queue;
	std::deque<Completion> completed;
	std::deque<DisplayImageCache::ImagePtr> retired;
	std::shared_ptr<RetirementState> retirementState =
		std::make_shared<RetirementState>();
	DisplayImageCache::ImagePtr activeSource;
	std::shared_ptr<std::atomic<bool>> activeCancellation;
	PerfWorkClass activeWorkClass = PerfWorkClass::Unspecified;
	const ThumbnailPreparationRequest* activeRequest = nullptr;
	bool activeCancellationReported = false;
	std::size_t activeWorkers = 0;
	std::size_t reservedCompletionBytes = 0;
	std::size_t reservedCompletionImages = 0;
	std::uint64_t generation = 0;
	bool stopping = false;
	std::thread worker;
	std::thread retirementWorker;
};

ThumbnailPreparationWorker::ThumbnailPreparationWorker(Processor processor)
	: impl_(std::make_unique<Impl>(std::move(processor))) {}

ThumbnailPreparationWorker::~ThumbnailPreparationWorker() = default;

ThumbnailPreparationAdmission ThumbnailPreparationWorker::Request(
	const ThumbnailPreparationRequest& request) {
	ThumbnailPreparationAdmission admission;
	if (!request.Valid()) return admission;
	std::size_t estimatedBytes = 0;
	if (!EstimateThumbnailBytes(request, estimatedBytes) || estimatedBytes >
		kMaximumCompletedBytes) return admission;
	ThumbnailPreparationRequest prepared = request;
	if (!prepared.cancellation || prepared.cancellation->load()) {
		prepared.cancellation = std::make_shared<std::atomic<bool>>(false);
	}
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->stopping || (impl_->activeRequest != nullptr &&
			impl_->activeRequest->key == request.key) ||
			std::any_of(impl_->completed.begin(), impl_->completed.end(),
			[&request](const Impl::Completion& completion) {
				const Result& result = completion.result;
					return result.key == request.key &&
						result.catalogRevision == request.catalogRevision &&
						result.geometryRevision == request.geometryRevision &&
						result.fileIndex == request.fileIndex;
			})) {
			return admission;
		}
		const auto queued = std::find_if(impl_->queue.begin(), impl_->queue.end(),
			[&request](const Impl::Work& work) { return work.request.key == request.key; });
		if (queued != impl_->queue.end()) {
			if (queued->request.source != request.source) {
				if (queued->request.source) {
					impl_->retired.push_back(queued->request.source);
				}
				queued->request.source = request.source;
			}
			queued->request.priority = request.priority;
			queued->request.workClass = request.workClass;
			queued->request.catalogRevision = request.catalogRevision;
			queued->request.geometryRevision = request.geometryRevision;
			queued->request.fileIndex = request.fileIndex;
			queued->request.maximumWidth = request.maximumWidth;
			queued->request.maximumHeight = request.maximumHeight;
			queued->request.logicalSource = request.logicalSource;
			queued->request.cancellation = prepared.cancellation;
			admission.accepted = true;
			impl_->workAvailable.notify_one();
			return admission;
		}
		if (impl_->queue.size() >= Impl::kMaximumQueuedSources) {
			const auto farthest = std::max_element(impl_->queue.begin(), impl_->queue.end(),
				[](const Impl::Work& left, const Impl::Work& right) {
					return left.request.priority < right.request.priority;
			});
			if (farthest == impl_->queue.end() ||
				farthest->request.priority <= request.priority) return admission;
			ThumbnailPreparationResult displaced;
			displaced.key = farthest->request.key;
			displaced.fileIndex = farthest->request.fileIndex;
			displaced.catalogRevision = farthest->request.catalogRevision;
			displaced.geometryRevision = farthest->request.geometryRevision;
			displaced.maximumWidth = farthest->request.maximumWidth;
			displaced.maximumHeight = farthest->request.maximumHeight;
			displaced.workClass = farthest->request.workClass;
			displaced.cancelled = true;
			admission.displaced = std::move(displaced);
			RecordThumbnailCancellation(farthest->request.workClass);
			impl_->retired.push_back(std::move(farthest->request.source));
			impl_->queue.erase(farthest);
		}
		impl_->StartWorkerLocked();
		impl_->queue.push_back({std::move(prepared), impl_->generation});
		admission.accepted = true;
	}
	impl_->workAvailable.notify_one();
	return admission;
}

std::vector<ThumbnailPreparationWorker::Result>
ThumbnailPreparationWorker::TakeCompleted(std::size_t maximumCount) {
	static const std::set<PerfWorkClass> allWorkClasses{
		PerfWorkClass::Unspecified,
		PerfWorkClass::ActiveImageSpread,
		PerfWorkClass::FocusedPreview,
		PerfWorkClass::VisibleThumbnail,
		PerfWorkClass::NearestNavigationNeighbor,
		PerfWorkClass::DistantSpeculation};
	return TakeCompleted(maximumCount, allWorkClasses);
}

std::vector<ThumbnailPreparationWorker::Result>
ThumbnailPreparationWorker::TakeCompleted(std::size_t maximumCount,
	const std::set<PerfWorkClass>& permittedWorkClasses) {
	std::vector<Result> images;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	images.reserve(std::min(maximumCount, impl_->completed.size()));
	while (images.size() < maximumCount) {
		const auto next = std::find_if(impl_->completed.begin(), impl_->completed.end(),
			[&permittedWorkClasses](const Impl::Completion& completion) {
				return permittedWorkClasses.find(completion.result.workClass) !=
					permittedWorkClasses.end();
			});
		if (next == impl_->completed.end()) break;
		images.push_back(std::move(next->result));
		impl_->ReleaseReservation(next->reservedBytes, true);
		impl_->completed.erase(next);
	}
	impl_->idle.notify_all();
	impl_->workAvailable.notify_all();
	return images;
}

std::vector<ThumbnailPreparationWorker::Result>
ThumbnailPreparationWorker::Cancel(const std::set<PerfWorkClass>& workClasses) {
	std::vector<Result> cancelled;
	cancelled.reserve(Impl::kMaximumQueuedSources +
		ThumbnailPreparationWorker::kMaximumCompletedResults + 1);
	std::unique_lock<std::mutex> lock(impl_->mutex);
	for (auto queued = impl_->queue.begin(); queued != impl_->queue.end();) {
		if (workClasses.find(queued->request.workClass) == workClasses.end()) {
			++queued;
			continue;
		}
		if (queued->request.source) impl_->retired.push_back(queued->request.source);
		Result result;
		result.key = std::move(queued->request.key);
		result.fileIndex = queued->request.fileIndex;
		result.catalogRevision = queued->request.catalogRevision;
		result.geometryRevision = queued->request.geometryRevision;
		result.maximumWidth = queued->request.maximumWidth;
		result.maximumHeight = queued->request.maximumHeight;
		result.workClass = queued->request.workClass;
		result.cancelled = true;
		cancelled.push_back(std::move(result));
		RecordThumbnailCancellation(queued->request.workClass);
		queued = impl_->queue.erase(queued);
	}
	for (auto completed = impl_->completed.begin(); completed != impl_->completed.end();) {
		if (workClasses.find(completed->result.workClass) == workClasses.end()) {
			++completed;
			continue;
		}
		completed->result.cancelled = true;
		cancelled.push_back(std::move(completed->result));
		impl_->ReleaseReservation(completed->reservedBytes, true);
		completed = impl_->completed.erase(completed);
	}
	if (impl_->activeCancellation && impl_->activeRequest != nullptr &&
		workClasses.find(impl_->activeWorkClass) != workClasses.end()) {
		impl_->activeCancellation->store(true);
		if (!impl_->activeCancellationReported) {
			Result result;
			try {
				result.key = impl_->activeRequest->key;
				result.fileIndex = impl_->activeRequest->fileIndex;
				result.catalogRevision = impl_->activeRequest->catalogRevision;
				result.geometryRevision = impl_->activeRequest->geometryRevision;
				result.maximumWidth = impl_->activeRequest->maximumWidth;
				result.maximumHeight = impl_->activeRequest->maximumHeight;
				result.workClass = impl_->activeRequest->workClass;
				result.cancelled = true;
				cancelled.push_back(std::move(result));
				impl_->activeCancellationReported = true;
				RecordThumbnailCancellation(impl_->activeWorkClass);
			} catch (...) {
				// If reporting needs memory that is unavailable, the worker emits the
				// cancellation result later by moving its original key.
			}
		}
	}
	impl_->idle.notify_all();
	impl_->workAvailable.notify_all();
	lock.unlock();
	for (Result& result : cancelled) {
		impl_->QueueRetirement(std::move(result.image));
	}
	return cancelled;
}

void ThumbnailPreparationWorker::Retire(const ImagePtr& image) {
	impl_->QueueRetirement(image);
}

void ThumbnailPreparationWorker::Clear() {
	std::deque<Impl::Work> abandonedWork;
	std::deque<Impl::Completion> abandonedCompletions;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		for (const Impl::Work& pending : impl_->queue) {
			if (pending.request.source) impl_->retired.push_back(pending.request.source);
		}
		++impl_->generation;
		if (impl_->activeCancellation) impl_->activeCancellation->store(true);
		for (const Impl::Work& pending : impl_->queue) {
			RecordThumbnailCancellation(pending.request.workClass);
		}
		abandonedWork.swap(impl_->queue);
		abandonedCompletions.swap(impl_->completed);
		for (const Impl::Completion& completion : abandonedCompletions) {
			impl_->ReleaseReservation(completion.reservedBytes, true);
		}
		// Each queued source also has an owner in impl_->retired, so dropping
		// these local copies cannot run a large destructor under the mutex.
		for (Impl::Work& pending : abandonedWork) pending.request.source.reset();
		impl_->idle.notify_all();
	}
	for (Impl::Completion& completion : abandonedCompletions) {
		impl_->QueueRetirement(std::move(completion.result.image));
	}
	impl_->workAvailable.notify_all();
}

bool ThumbnailPreparationWorker::HasPendingWork() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return !impl_->queue.empty() || impl_->activeWorkers != 0 || !impl_->completed.empty();
}

ThumbnailPreparationDiagnostics ThumbnailPreparationWorker::GetDiagnostics() const {
	ThumbnailPreparationDiagnostics diagnostics;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	diagnostics.queued = impl_->queue.size();
	diagnostics.active = impl_->activeWorkers;
	diagnostics.completedResults = impl_->completed.size();
	diagnostics.reservedCompletionBytes = impl_->reservedCompletionBytes;
	diagnostics.reservedCompletionImages = impl_->reservedCompletionImages;
	for (const Impl::Completion& completion : impl_->completed) {
		if (completion.result.image) {
			diagnostics.completedBytes += completion.result.image->bgra.size();
			++diagnostics.completedImages;
		}
	}
	diagnostics.retiredSources = impl_->retired.size();
	for (const DisplayImageCache::ImagePtr& source : impl_->retired) {
		if (source) diagnostics.retiredSourceBytes += PreparedDisplayImageBytes(*source);
	}
	{
		std::lock_guard<std::mutex> retirementLock(impl_->retirementState->mutex);
		for (const ImagePtr& image : impl_->retirementState->images) {
			if (image) {
				diagnostics.retiredThumbnailBytes += image->bgra.size();
				++diagnostics.retiredThumbnailImages;
			}
		}
	}
	std::unordered_set<const PreparedDisplayImage*> retainedSources;
	for (const Impl::Work& work : impl_->queue) {
		if (work.request.source && retainedSources.insert(work.request.source.get()).second) {
			diagnostics.retainedSourceBytes += PreparedDisplayImageBytes(*work.request.source);
		}
	}
	if (impl_->activeSource && retainedSources.insert(impl_->activeSource.get()).second) {
		diagnostics.retainedSourceBytes += PreparedDisplayImageBytes(*impl_->activeSource);
	}
	return diagnostics;
}

bool ThumbnailPreparationWorker::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(impl_->mutex);
	return impl_->idle.wait_for(lock, timeout, [this] {
		return impl_->queue.empty() && impl_->activeWorkers == 0;
	});
}

} // namespace jpegview_linux
