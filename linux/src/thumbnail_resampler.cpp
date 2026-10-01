#include "thumbnail_resampler.h"

#include "image_decoder.h"
#include "perf_diagnostics.h"
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
	return !key.empty() && (validDisplaySource || validFileSource) &&
		maximumWidth > 0 && maximumHeight > 0;
}

bool ThumbnailPreparationResultMatches(const ThumbnailPreparationResult& result,
	std::uint64_t catalogRevision, std::uint64_t geometryRevision,
	std::size_t fileIndex, const std::string& sourceKey,
	int maximumWidth, int maximumHeight) {
	return result.catalogRevision == catalogRevision &&
		result.geometryRevision == geometryRevision && result.fileIndex == fileIndex &&
		result.key == sourceKey &&
		result.maximumWidth == maximumWidth && result.maximumHeight == maximumHeight;
}

namespace {

ThumbnailPreparationWorker::ImagePtr PrepareThumbnail(
	const ThumbnailPreparationRequest& request) {
	if (!request.Valid()) return {};
	const auto shouldContinue = [&request] {
		return !request.cancellation || !request.cancellation->load();
	};
	if (!shouldContinue()) return {};
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
				errorMessage) || !shouldContinue()) return {};
			size = FitThumbnailSize(jpegWidth, jpegHeight,
				request.maximumWidth, request.maximumHeight);
			int decodedSourceWidth = 0;
			int decodedSourceHeight = 0;
			if (size.width <= 0 || size.height <= 0 ||
				!DecodeJpegForDisplay(request.logicalSource, size.width, size.height,
					decoded, decodedSourceWidth, decodedSourceHeight, errorMessage) ||
				decoded.frames.empty() || !shouldContinue()) return {};
		} else if (!DecodeImage(request.logicalSource, decoded, errorMessage) ||
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
	prepared->width = size.width;
	prepared->height = size.height;
	prepared->hasTransparency = hasTransparency;
	prepared->workClass = request.workClass;
	if (!DownsampleThumbnailBgra(*sourcePixels, sourceWidth, sourceHeight,
		size.width, size.height, prepared->bgra, shouldContinue)) return {};
	return prepared;
}

} // namespace

struct ThumbnailPreparationWorker::Impl {
	struct Work {
		ThumbnailPreparationRequest request;
		std::uint64_t generation = 0;
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
	}

	void Run() {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
		for (;;) {
			Work work;
			DisplayImageCache::ImagePtr retiredSource;
			DisplayImageCache::ImagePtr completedSource;
			{
				std::unique_lock<std::mutex> lock(mutex);
				workAvailable.wait(lock, [this] {
					return stopping || !queue.empty() || !retired.empty();
				});
				if (stopping) {
					queue.clear();
					retired.clear();
					completed.clear();
					return;
				}
				if (!retired.empty()) {
					retiredSource = std::move(retired.front());
					retired.pop_front();
				}
				if (queue.empty()) continue;
				const auto nearest = std::min_element(queue.begin(), queue.end(),
					[](const Work& left, const Work& right) {
						return left.request.priority < right.request.priority;
					});
				work = std::move(*nearest);
				queue.erase(nearest);
				inFlightKeys.insert(work.request.key);
				activeCancellation = work.request.cancellation;
				activeWorkClass = work.request.workClass;
				activeRequest = work.request;
				activeCancellationReported = false;
				activeSource = work.request.source;
				++activeWorkers;
			}

			PerfContextScope context(work.request.workClass, PerfExecution::WorkerThread);
			ImagePtr image;
			try {
				image = processor(work.request);
			} catch (const std::exception&) {
				image.reset();
			}
			work.request.source.reset();
			retiredSource.reset();
			{
				std::lock_guard<std::mutex> lock(mutex);
				inFlightKeys.erase(work.request.key);
				--activeWorkers;
				const bool wasCancelled = work.request.cancellation &&
					work.request.cancellation->load();
				if (!stopping && work.generation == generation) {
					if (!wasCancelled || !activeCancellationReported) {
						Result result;
						result.key = work.request.key;
						result.fileIndex = work.request.fileIndex;
						result.catalogRevision = work.request.catalogRevision;
						result.geometryRevision = work.request.geometryRevision;
						result.maximumWidth = work.request.maximumWidth;
						result.maximumHeight = work.request.maximumHeight;
						result.workClass = work.request.workClass;
						result.cancelled = wasCancelled;
						if (!wasCancelled && image && image->key == work.request.key) {
							result.image = std::move(image);
						}
						completed.push_back(std::move(result));
					}
					if (wasCancelled) RecordThumbnailCancellation(work.request.workClass,
						PerfExecution::WorkerThread);
				} else if (!stopping && work.generation != generation) {
					RecordThumbnailCancellation(work.request.workClass,
						PerfExecution::WorkerThread);
				}
				activeCancellation.reset();
				activeRequest = {};
				activeCancellationReported = false;
				completedSource = std::move(activeSource);
				idle.notify_all();
			}
			completedSource.reset();
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
	std::deque<Result> completed;
	std::deque<DisplayImageCache::ImagePtr> retired;
	std::unordered_set<std::string> inFlightKeys;
	DisplayImageCache::ImagePtr activeSource;
	std::shared_ptr<std::atomic<bool>> activeCancellation;
	PerfWorkClass activeWorkClass = PerfWorkClass::Unspecified;
	ThumbnailPreparationRequest activeRequest;
	bool activeCancellationReported = false;
	std::size_t activeWorkers = 0;
	std::uint64_t generation = 0;
	bool stopping = false;
	std::thread worker;
};

ThumbnailPreparationWorker::ThumbnailPreparationWorker(Processor processor)
	: impl_(std::make_unique<Impl>(std::move(processor))) {}

ThumbnailPreparationWorker::~ThumbnailPreparationWorker() = default;

ThumbnailPreparationAdmission ThumbnailPreparationWorker::Request(
	const ThumbnailPreparationRequest& request) {
	ThumbnailPreparationAdmission admission;
	if (!request.Valid()) return admission;
	ThumbnailPreparationRequest prepared = request;
	if (!prepared.cancellation || prepared.cancellation->load()) {
		prepared.cancellation = std::make_shared<std::atomic<bool>>(false);
	}
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->stopping || impl_->inFlightKeys.find(request.key) != impl_->inFlightKeys.end() ||
			std::any_of(impl_->completed.begin(), impl_->completed.end(),
			[&request](const Result& result) {
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
				DisplayImageCache::ImagePtr retiredSource =
					std::move(queued->request.source);
				queued->request.source = request.source;
				if (retiredSource) impl_->retired.push_back(std::move(retiredSource));
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
			[&permittedWorkClasses](const Result& result) {
				return permittedWorkClasses.find(result.workClass) != permittedWorkClasses.end();
			});
		if (next == impl_->completed.end()) break;
		images.push_back(std::move(*next));
		impl_->completed.erase(next);
	}
	impl_->idle.notify_all();
	return images;
}

std::vector<ThumbnailPreparationWorker::Result>
ThumbnailPreparationWorker::Cancel(const std::set<PerfWorkClass>& workClasses) {
	std::vector<Result> cancelled;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	for (auto queued = impl_->queue.begin(); queued != impl_->queue.end();) {
		if (workClasses.find(queued->request.workClass) == workClasses.end()) {
			++queued;
			continue;
		}
		Result result;
		result.key = queued->request.key;
		result.fileIndex = queued->request.fileIndex;
		result.catalogRevision = queued->request.catalogRevision;
		result.geometryRevision = queued->request.geometryRevision;
		result.maximumWidth = queued->request.maximumWidth;
		result.maximumHeight = queued->request.maximumHeight;
		result.workClass = queued->request.workClass;
		result.cancelled = true;
		cancelled.push_back(std::move(result));
		RecordThumbnailCancellation(queued->request.workClass);
		impl_->retired.push_back(std::move(queued->request.source));
		queued = impl_->queue.erase(queued);
	}
	for (auto completed = impl_->completed.begin(); completed != impl_->completed.end();) {
		if (workClasses.find(completed->workClass) == workClasses.end()) {
			++completed;
			continue;
		}
		completed->image.reset();
		completed->cancelled = true;
		cancelled.push_back(std::move(*completed));
		completed = impl_->completed.erase(completed);
	}
	if (impl_->activeCancellation &&
		workClasses.find(impl_->activeWorkClass) != workClasses.end()) {
		impl_->activeCancellation->store(true);
		if (!impl_->activeCancellationReported) {
			Result result;
			result.key = impl_->activeRequest.key;
			result.fileIndex = impl_->activeRequest.fileIndex;
			result.catalogRevision = impl_->activeRequest.catalogRevision;
			result.geometryRevision = impl_->activeRequest.geometryRevision;
			result.maximumWidth = impl_->activeRequest.maximumWidth;
			result.maximumHeight = impl_->activeRequest.maximumHeight;
			result.workClass = impl_->activeRequest.workClass;
			result.cancelled = true;
			cancelled.push_back(std::move(result));
			impl_->activeCancellationReported = true;
			RecordThumbnailCancellation(impl_->activeWorkClass);
		}
	}
	impl_->idle.notify_all();
	impl_->workAvailable.notify_one();
	return cancelled;
}

void ThumbnailPreparationWorker::Clear() {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		++impl_->generation;
		if (impl_->activeCancellation) impl_->activeCancellation->store(true);
		while (!impl_->queue.empty()) {
			RecordThumbnailCancellation(impl_->queue.front().request.workClass);
			impl_->retired.push_back(std::move(impl_->queue.front().request.source));
			impl_->queue.pop_front();
		}
		impl_->completed.clear();
		impl_->idle.notify_all();
	}
	impl_->workAvailable.notify_one();
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
	for (const Result& result : impl_->completed) {
		if (result.image) {
			diagnostics.completedBytes += result.image->bgra.size();
			++diagnostics.completedImages;
		}
	}
	diagnostics.retiredSources = impl_->retired.size();
	for (const DisplayImageCache::ImagePtr& source : impl_->retired) {
		if (source) diagnostics.retiredSourceBytes += PreparedDisplayImageBytes(*source);
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
