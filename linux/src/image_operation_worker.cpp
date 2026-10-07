#include "image_operation_worker.h"

#include "event_loop_model.h"
#include "image_processing.h"
#include "image_transform_geometry.h"
#include "image_transform_pixels.h"
#include "perf_diagnostics.h"
#include "source_work_coordinator.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr int kMaximumFreeRotationPreviewDimension = 2048;
constexpr std::uint64_t kMaximumFreeRotationPreviewPixels = 4ull * 1024ull * 1024ull;

bool HasPixels(const Image& image) {
	if (image.width <= 0 || image.height <= 0) return false;
	const std::size_t width = static_cast<std::size_t>(image.width);
	const std::size_t height = static_cast<std::size_t>(image.height);
	return width <= std::numeric_limits<std::size_t>::max() / height / 4 &&
		image.bgra.size() == width * height * 4;
}

std::shared_ptr<const Image> OwnImage(Image&& image, SharedCacheBudget& budget,
	CacheReservation& reservation) {
	auto owned = std::make_shared<Image>(std::move(image));
	std::shared_ptr<const Image> pixels = std::move(owned);
	reservation = budget.TrackTemporary(pixels->bgra.size(),
		CacheMemoryCategory::ActiveWorkingData,
		std::static_pointer_cast<const void>(pixels));
	return pixels;
}

std::shared_ptr<const Image> ShareImage(std::shared_ptr<const Image> image,
	SharedCacheBudget& budget, CacheReservation& reservation) {
	if (!image || image->bgra.empty()) return {};
	reservation = budget.TrackTemporary(image->bgra.size(),
		CacheMemoryCategory::ActiveWorkingData,
		std::static_pointer_cast<const void>(image));
	return reservation ? std::move(image) : std::shared_ptr<const Image>{};
}

std::shared_ptr<const Image> CopyDecodedFrame(
	const std::shared_ptr<const DecodedImage>& decoded, std::size_t frameIndex,
	SharedCacheBudget& budget, CacheReservation& reservation,
	const std::function<bool()>& shouldContinue) {
	if (!decoded || frameIndex >= decoded->frames.size() || !shouldContinue()) return {};
	const DecodedFrame& frame = decoded->frames[frameIndex];
	Image image;
	if (!image.StoreBGRA(frame.bgra.data(), frame.width, frame.height,
		frame.hasTransparency) || !shouldContinue()) return {};
	return OwnImage(std::move(image), budget, reservation);
}

std::shared_ptr<const Image> GetSourcePixels(const ImageOperationRequest& request,
	SharedCacheBudget& budget, CacheReservation& reservation,
	const std::function<bool()>& shouldContinue) {
	if (request.document.sourcePixels) return request.document.sourcePixels;
	return CopyDecodedFrame(request.decoded, request.document.frameIndex, budget,
		reservation, shouldContinue);
}

bool TransformCopy(const Image& source, ImageTransformKind transform, Image& output,
	const std::function<bool()>& shouldContinue) {
	if (!HasPixels(source) || !shouldContinue()) return false;
	const bool rotate = transform == ImageTransformKind::RotateClockwise ||
		transform == ImageTransformKind::RotateCounterClockwise;
	const int newWidth = rotate ? source.height : source.width;
	const int newHeight = rotate ? source.width : source.height;
	Image result;
	result.width = newWidth;
	result.height = newHeight;
	result.originalWidth = source.originalWidth;
	result.originalHeight = source.originalHeight;
	result.hasTransparency = source.hasTransparency;
	try {
		result.bgra.resize(source.bgra.size());
	} catch (const std::exception&) {
		return false;
	}
	for (int sourceY = 0; sourceY < source.height; ++sourceY) {
		if ((sourceY % 16) == 0 && !shouldContinue()) return false;
		for (int sourceX = 0; sourceX < source.width; ++sourceX) {
			int targetX = sourceX;
			int targetY = sourceY;
			switch (transform) {
			case ImageTransformKind::RotateClockwise:
				targetX = source.height - sourceY - 1;
				targetY = sourceX;
				break;
			case ImageTransformKind::RotateCounterClockwise:
				targetX = sourceY;
				targetY = source.width - sourceX - 1;
				break;
			case ImageTransformKind::MirrorHorizontal:
				targetX = source.width - sourceX - 1;
				break;
			case ImageTransformKind::MirrorVertical:
				targetY = source.height - sourceY - 1;
				break;
			}
			const std::size_t sourceOffset =
				(static_cast<std::size_t>(sourceY) * source.width + sourceX) * 4;
			const std::size_t targetOffset =
				(static_cast<std::size_t>(targetY) * newWidth + targetX) * 4;
			std::copy_n(source.bgra.data() + sourceOffset, 4,
				result.bgra.data() + targetOffset);
		}
	}
	if (!shouldContinue()) return false;
	output = std::move(result);
	return true;
}

bool CropCopy(const Image& source, const ImageOperationSpec& operation,
	Image& output, const std::function<bool()>& shouldContinue) {
	return source.CopyCrop(operation.left, operation.top, operation.right,
		operation.bottom, output, shouldContinue);
}

bool BuildProcessed(const Image& source, const ImageDocumentSnapshot& document,
	Image& output, const std::function<bool()>& shouldContinue) {
	try {
		output = source;
	} catch (const std::exception&) {
		return false;
	}
	return output.ApplyProcessing(document.processing, document.autoContrast,
		shouldContinue);
}

bool ProcessingChangesPixels(const ImageDocumentSnapshot& document) {
	return document.autoContrast ||
		!jpegview_linux::IsDefaultImageProcessing(document.processing);
}

std::shared_ptr<const Image> PreparePresentation(
	const std::shared_ptr<const Image>& source,
	const ImageDocumentSnapshot& document, SharedCacheBudget& budget,
	CacheReservation& reservation, const std::function<bool()>& shouldContinue) {
	if (!source || !shouldContinue()) return {};
	if (!ProcessingChangesPixels(document)) {
		return ShareImage(source, budget, reservation);
	}
	Image processed;
	if (!BuildProcessed(*source, document, processed, shouldContinue)) return {};
	return OwnImage(std::move(processed), budget, reservation);
}

bool ResizeFreeRotationPreviewBase(std::shared_ptr<Image>& previewBase,
	int width, int height, SharedCacheBudget& budget,
	CacheReservation& previewReservation,
	const std::function<bool()>& shouldContinue) {
	if (previewBase->width == width && previewBase->height == height) {
		return !shouldContinue || shouldContinue();
	}
	const std::size_t targetBytes = static_cast<std::size_t>(width) * height * 4;
	const std::size_t resizePeakBytes = std::max(targetBytes, previewBase->bgra.size());
	CacheReservation resizeStaging = budget.TrackTemporary(resizePeakBytes,
		CacheMemoryCategory::ActiveWorkingData);
	if (!resizeStaging || !previewBase->Resize(width, height, 2, shouldContinue)) return false;
	previewReservation.Reset();
	previewReservation = budget.TrackTemporary(previewBase->bgra.size(),
		CacheMemoryCategory::ActiveWorkingData,
		std::static_pointer_cast<const void>(previewBase));
	return previewReservation && (!shouldContinue || shouldContinue());
}

bool PrepareFreeRotationPreview(const Image& source,
	const ImageDocumentSnapshot& document, const ImageOperationSpec& operation,
	SharedCacheBudget& budget, const std::function<bool()>& shouldContinue,
	Image& output, CacheReservation& outputStaging) {
	ImageTransformGeometry sourceGeometry;
	if (!BuildFreeRotationGeometry(source.width, source.height,
		operation.clockwiseDegrees, operation.autoCrop,
		operation.preserveAspectRatio, sourceGeometry)) return false;
	const std::uint64_t expandedPixels = static_cast<std::uint64_t>(
		sourceGeometry.outputWidth) * sourceGeometry.outputHeight;
	const double maximumDimension = std::max(source.width, source.height);
	const int maximumExpandedDimension = std::max(sourceGeometry.outputWidth,
		sourceGeometry.outputHeight);
	double scale = std::min(1.0,
		static_cast<double>(kMaximumFreeRotationPreviewDimension) / maximumDimension);
	scale = std::min(scale, static_cast<double>(
		kMaximumFreeRotationPreviewDimension) / maximumExpandedDimension);
	scale = std::min(scale, std::sqrt(static_cast<double>(
		kMaximumFreeRotationPreviewPixels) / expandedPixels));
	int previewWidth = std::max(1, static_cast<int>(std::llround(source.width * scale)));
	int previewHeight = std::max(1, static_cast<int>(std::llround(source.height * scale)));
	const bool processingMatches = document.presentationPixels &&
		jpegview_linux::EqualImageProcessing(document.materializedProcessing,
			document.processing) &&
		document.materializedAutoContrast == document.autoContrast &&
		document.presentationPixels->width == source.width &&
		document.presentationPixels->height == source.height;
	const Image& input = processingMatches ? *document.presentationPixels : source;
	std::shared_ptr<Image> previewBase;
	try {
		previewBase = std::make_shared<Image>(input);
	} catch (const std::exception&) {
		return false;
	}
	CacheReservation previewReservation = budget.TrackTemporary(previewBase->bgra.size(),
		CacheMemoryCategory::ActiveWorkingData,
		std::static_pointer_cast<const void>(previewBase));
	if (!previewReservation || (shouldContinue && !shouldContinue()) ||
		!ResizeFreeRotationPreviewBase(previewBase, previewWidth, previewHeight,
			budget, previewReservation, shouldContinue)) return false;
	ImageTransformGeometry previewGeometry;
	if (!BuildFreeRotationGeometry(previewBase->width, previewBase->height,
		operation.clockwiseDegrees, operation.autoCrop,
		operation.preserveAspectRatio, previewGeometry)) return false;
	while (static_cast<std::uint64_t>(previewGeometry.outputWidth) *
		previewGeometry.outputHeight > kMaximumFreeRotationPreviewPixels ||
		std::max(previewGeometry.outputWidth, previewGeometry.outputHeight) >
			kMaximumFreeRotationPreviewDimension) {
		double shrink = std::sqrt(static_cast<double>(
			kMaximumFreeRotationPreviewPixels) /
			(static_cast<double>(previewGeometry.outputWidth) *
				previewGeometry.outputHeight)) * 0.99;
		shrink = std::min(shrink, 0.99 * static_cast<double>(
			kMaximumFreeRotationPreviewDimension) /
			std::max(previewGeometry.outputWidth, previewGeometry.outputHeight));
		previewWidth = std::max(1, static_cast<int>(
			std::floor(previewBase->width * shrink)));
		previewHeight = std::max(1, static_cast<int>(
			std::floor(previewBase->height * shrink)));
		if (previewWidth == previewBase->width && previewHeight == previewBase->height) {
			if (previewWidth >= previewHeight && previewWidth > 1) --previewWidth;
			else if (previewHeight > 1) --previewHeight;
			else return false;
		}
		if (!ResizeFreeRotationPreviewBase(previewBase, previewWidth, previewHeight,
			budget, previewReservation, shouldContinue) ||
			!BuildFreeRotationGeometry(previewBase->width, previewBase->height,
				operation.clockwiseDegrees, operation.autoCrop,
				operation.preserveAspectRatio, previewGeometry)) return false;
	}
	if (!processingMatches && ProcessingChangesPixels(document) &&
		!previewBase->ApplyProcessing(document.processing, document.autoContrast,
			shouldContinue)) return false;
	const std::size_t outputBytes = static_cast<std::size_t>(previewGeometry.outputWidth) *
		previewGeometry.outputHeight * 4;
	outputStaging = budget.TrackTemporary(outputBytes,
		CacheMemoryCategory::ActiveWorkingData);
	if (!outputStaging || (shouldContinue && !shouldContinue())) return false;
	return ResampleFreeRotation(*previewBase, previewGeometry,
		ImageTransformSampling::PreviewBilinear, output, shouldContinue);
}

} // namespace

ImageOperationResult ProcessImageOperation(const ImageOperationRequest& request,
	const std::function<bool()>& shouldContinue, SharedCacheBudget& budget) {
	ImageOperationResult result;
	result.expected = request.document;
	result.kind = request.operation.kind;
	result.rotationQuarterTurns = request.document.rotationQuarterTurns;
	result.modified = request.document.modified;
	result.detached = request.document.detached;
	result.updatesDocument = request.operation.kind == ImageOperationKind::Materialize ||
		request.operation.kind == ImageOperationKind::Transform ||
		request.operation.kind == ImageOperationKind::FreeRotate ||
		request.operation.kind == ImageOperationKind::Crop ||
		request.operation.kind == ImageOperationKind::Resize ||
		request.operation.kind == ImageOperationKind::Reprocess ||
		(request.operation.kind == ImageOperationKind::PrepareOutput &&
			request.operation.preserveDocumentPixels);

	CacheReservation newlyDecodedReservation;
	const std::shared_ptr<const Image> source = GetSourcePixels(request, budget,
		newlyDecodedReservation, shouldContinue);
	if (!source || !HasPixels(*source) || !shouldContinue()) {
		result.failure = {WorkerFailureKind::ProcessingFailed,
			"source pixels could not be prepared"};
		return result;
	}
	const auto setSource = [&result, &request, &source,
		&newlyDecodedReservation](bool sourceWasCreated) {
		result.sourcePixels = source;
		if (sourceWasCreated) {
			result.sourceReservation = std::move(newlyDecodedReservation);
		} else if (request.document.sourcePixels == source) {
			result.sourceReservation.Reset();
		}
	};
	const auto makePresentation = [&result, &budget](Image&& image) {
		result.presentationPixels = OwnImage(std::move(image), budget,
			result.presentationReservation);
		return result.presentationPixels != nullptr;
	};
	const auto sharePresentation = [&result, &budget](
		const std::shared_ptr<const Image>& image) {
		result.presentationPixels = ShareImage(image, budget,
			result.presentationReservation);
		return result.presentationPixels != nullptr;
	};
	const auto makeOutput = [&result, &budget](Image&& image) {
		result.outputPixels = OwnImage(std::move(image), budget,
			result.outputReservation);
		return result.outputPixels != nullptr;
	};
	bool sourceCreated = request.document.sourcePixels == nullptr;
	switch (request.operation.kind) {
	case ImageOperationKind::Materialize: {
		result.presentationPixels = PreparePresentation(source, request.document,
			budget, result.presentationReservation, shouldContinue);
		if (!result.presentationPixels) break;
		setSource(sourceCreated);
		result.success = true;
		break;
	}
	case ImageOperationKind::Transform: {
		Image transformedSource;
		if (!TransformCopy(*source, request.operation.transform,
			transformedSource, shouldContinue)) break;
		result.sourcePixels = OwnImage(std::move(transformedSource), budget,
			result.sourceReservation);
		if (!result.sourcePixels) break;
		if (!ProcessingChangesPixels(request.document)) {
			if (!sharePresentation(result.sourcePixels)) break;
		} else {
			Image currentPresentation;
			Image transformedPresentation;
			const bool currentProcessingMatches = request.document.presentationPixels &&
				jpegview_linux::EqualImageProcessing(
					request.document.materializedProcessing, request.document.processing) &&
				request.document.materializedAutoContrast == request.document.autoContrast;
			const Image* presentationToTransform = request.document.presentationPixels.get();
			if (!currentProcessingMatches) {
				if (!BuildProcessed(*source, request.document, currentPresentation,
					shouldContinue)) break;
				presentationToTransform = &currentPresentation;
			}
			if (presentationToTransform == nullptr ||
				!TransformCopy(*presentationToTransform, request.operation.transform,
					transformedPresentation, shouldContinue) ||
				!makePresentation(std::move(transformedPresentation))) break;
		}
		if (request.operation.transform == ImageTransformKind::RotateClockwise) {
			result.rotationQuarterTurns = (result.rotationQuarterTurns + 1) % 4;
		} else if (request.operation.transform == ImageTransformKind::RotateCounterClockwise) {
			result.rotationQuarterTurns = (result.rotationQuarterTurns + 3) % 4;
		}
		result.modified = true;
		result.flattenAnimation = request.document.animated;
		result.success = true;
		break;
	}
	case ImageOperationKind::FreeRotate: {
		ImageTransformGeometry geometry;
		if (!BuildFreeRotationGeometry(source->width, source->height,
			request.operation.clockwiseDegrees, request.operation.autoCrop,
			request.operation.preserveAspectRatio, geometry)) break;
		Image rotatedSource;
		if (!ResampleFreeRotation(*source, geometry,
			ImageTransformSampling::FinalBicubic, rotatedSource, shouldContinue)) break;
		result.sourcePixels = OwnImage(std::move(rotatedSource), budget,
			result.sourceReservation);
		if (!result.sourcePixels) break;
		if (!ProcessingChangesPixels(request.document)) {
			if (!sharePresentation(result.sourcePixels)) break;
		} else {
			Image currentPresentation;
			Image rotatedPresentation;
			const bool currentProcessingMatches = request.document.presentationPixels &&
				jpegview_linux::EqualImageProcessing(
					request.document.materializedProcessing, request.document.processing) &&
				request.document.materializedAutoContrast == request.document.autoContrast;
			const Image* presentationToRotate = request.document.presentationPixels.get();
			if (!currentProcessingMatches) {
				if (!BuildProcessed(*source, request.document, currentPresentation,
					shouldContinue)) break;
				presentationToRotate = &currentPresentation;
			}
			if (presentationToRotate == nullptr ||
				!ResampleFreeRotation(*presentationToRotate, geometry,
					ImageTransformSampling::FinalBicubic, rotatedPresentation,
					shouldContinue) ||
				!makePresentation(std::move(rotatedPresentation))) break;
		}
		result.rotationQuarterTurns = 0;
		result.modified = true;
		result.flattenAnimation = request.document.animated;
		result.success = true;
		break;
	}
	case ImageOperationKind::FreeRotatePreview: {
		Image preview;
		CacheReservation previewOutputStaging;
		if (!PrepareFreeRotationPreview(*source, request.document,
			request.operation, budget, shouldContinue, preview, previewOutputStaging) ||
			!makeOutput(std::move(preview))) break;
		result.success = true;
		break;
	}
	case ImageOperationKind::Crop: {
		Image croppedSource;
		if (!CropCopy(*source, request.operation, croppedSource, shouldContinue)) break;
		croppedSource.originalWidth = croppedSource.width;
		croppedSource.originalHeight = croppedSource.height;
		result.sourcePixels = OwnImage(std::move(croppedSource), budget,
			result.sourceReservation);
		if (!result.sourcePixels) break;
		if (ProcessingChangesPixels(request.document)) {
			Image croppedPresentation;
			if (!BuildProcessed(*result.sourcePixels, request.document,
				croppedPresentation, shouldContinue) ||
				!makePresentation(std::move(croppedPresentation))) break;
		} else if (!sharePresentation(result.sourcePixels)) {
			break;
		}
		result.modified = true;
		result.flattenAnimation = request.document.animated;
		result.success = true;
		break;
	}
	case ImageOperationKind::Resize: {
		Image resizedSource;
		try {
			resizedSource = *source;
		} catch (const std::exception&) {
			break;
		}
		if (!resizedSource.Resize(request.operation.width, request.operation.height,
			request.operation.resizeFilter, shouldContinue)) break;
		resizedSource.originalWidth = request.operation.width;
		resizedSource.originalHeight = request.operation.height;
		result.sourcePixels = OwnImage(std::move(resizedSource), budget,
			result.sourceReservation);
		if (!result.sourcePixels) break;
		if (ProcessingChangesPixels(request.document)) {
			Image resizedPresentation;
			if (!BuildProcessed(*result.sourcePixels, request.document,
				resizedPresentation, shouldContinue) ||
				!makePresentation(std::move(resizedPresentation))) break;
		} else if (!sharePresentation(result.sourcePixels)) {
			break;
		}
		result.modified = true;
		result.flattenAnimation = request.document.animated;
		result.success = true;
		break;
	}
	case ImageOperationKind::Reprocess: {
		result.presentationPixels = PreparePresentation(source, request.document,
			budget, result.presentationReservation, shouldContinue);
		if (!result.presentationPixels) break;
		setSource(sourceCreated);
		result.success = true;
		break;
	}
	case ImageOperationKind::PrepareOutput: {
		const bool currentPresentation = request.document.presentationPixels &&
			jpegview_linux::EqualImageProcessing(
				request.document.materializedProcessing, request.document.processing) &&
			request.document.materializedAutoContrast == request.document.autoContrast;
		if (request.operation.preserveDocumentPixels) {
			result.presentationPixels = currentPresentation ?
				ShareImage(request.document.presentationPixels, budget,
					result.presentationReservation) :
				PreparePresentation(source, request.document, budget,
					result.presentationReservation, shouldContinue);
			if (!result.presentationPixels) break;
			setSource(sourceCreated);
		}
		if (request.operation.preserveDocumentPixels &&
			request.operation.width <= 0 && request.operation.height <= 0) {
			result.outputPixels = result.presentationPixels;
			result.outputReservation = result.presentationReservation.ShareAlias();
		} else if (request.operation.width <= 0 && request.operation.height <= 0 &&
			currentPresentation) {
			result.outputPixels = request.document.presentationPixels;
		} else if (request.operation.width <= 0 && request.operation.height <= 0 &&
			!ProcessingChangesPixels(request.document) &&
			!request.operation.preserveDocumentPixels) {
			result.outputPixels = ShareImage(source, budget, result.outputReservation);
		} else {
			Image presentation;
			if (request.operation.preserveDocumentPixels) {
				try {
					presentation = *result.presentationPixels;
				} catch (const std::exception&) {
					break;
				}
			} else if (currentPresentation) {
				try {
					presentation = *request.document.presentationPixels;
				} catch (const std::exception&) {
					break;
				}
			} else if (!BuildProcessed(*source, request.document, presentation,
				shouldContinue)) {
				break;
			}
			if (request.operation.width > 0 && request.operation.height > 0 &&
				!presentation.Resize(request.operation.width, request.operation.height,
					request.operation.resizeFilter, shouldContinue)) break;
			if (!makeOutput(std::move(presentation))) break;
		}
		result.success = true;
		break;
	}
	case ImageOperationKind::CopySelection: {
		Image selection;
		const bool currentPresentation = request.document.presentationPixels &&
			jpegview_linux::EqualImageProcessing(
				request.document.materializedProcessing, request.document.processing) &&
			request.document.materializedAutoContrast == request.document.autoContrast;
		if (currentPresentation) {
			if (!CropCopy(*request.document.presentationPixels, request.operation,
				selection, shouldContinue)) break;
		} else {
			const std::shared_ptr<const Image> presentation = PreparePresentation(source,
				request.document, budget, result.presentationReservation, shouldContinue);
			if (!presentation || !CropCopy(*presentation, request.operation,
				selection, shouldContinue)) break;
		}
		if (!makeOutput(std::move(selection))) break;
		result.success = true;
		break;
	}
	}

	if (!result.success && !result.failure.Failed()) {
		result.failure = {shouldContinue() ? WorkerFailureKind::ProcessingFailed :
			WorkerFailureKind::Cancelled,
			"image operation could not produce a complete result"};
	}
	return result;
}

ImageOperationWorker::ImageOperationWorker(std::shared_ptr<SharedCacheBudget> budget,
	Processor processor)
	: budget_(std::move(budget)), processor_(std::move(processor)) {
	if (!budget_) budget_ = std::make_shared<SharedCacheBudget>(0);
	if (!processor_) processor_ = ProcessImageOperation;
	worker_ = std::thread([this] { Run(); });
}

ImageOperationWorker::~ImageOperationWorker() {
	Stop();
}

std::uint64_t ImageOperationWorker::Request(ImageOperationRequest request) {
	std::shared_ptr<std::atomic<bool>> canceled;
	try {
		canceled = std::make_shared<std::atomic<bool>>(false);
	} catch (...) {
		return 0;
	}
	std::uint64_t requestedGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_) return 0;
		++generation_;
		if (generation_ == 0) ++generation_;
		if (activeCancellation_) activeCancellation_->store(true, std::memory_order_relaxed);
		if (pending_) {
			pending_->canceled->store(true, std::memory_order_relaxed);
			pending_.reset();
		}
		if (ready_) {
			retiredResults_.push_back(std::move(*ready_));
			ready_.reset();
		}
		requestedGeneration = generation_;
		pending_ = Work{std::move(request), requestedGeneration, std::move(canceled)};
		available_.notify_all();
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
	return requestedGeneration;
}

void ImageOperationWorker::Cancel() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_) return;
		++generation_;
		if (generation_ == 0) ++generation_;
		if (activeCancellation_) activeCancellation_->store(true, std::memory_order_relaxed);
		if (pending_) {
			pending_->canceled->store(true, std::memory_order_relaxed);
			pending_.reset();
		}
		if (ready_) {
			retiredResults_.push_back(std::move(*ready_));
			ready_.reset();
		}
		if (!active_) idle_.notify_all();
		available_.notify_all();
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
}

void ImageOperationWorker::Stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!stopping_) {
			stopping_ = true;
			++generation_;
			if (activeCancellation_) activeCancellation_->store(true,
				std::memory_order_relaxed);
			if (pending_) pending_->canceled->store(true, std::memory_order_relaxed);
			available_.notify_all();
			idle_.notify_all();
		}
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
	if (worker_.joinable()) worker_.join();
}

std::optional<ImageOperationResult> ImageOperationWorker::TakeReady() {
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<ImageOperationResult> result = std::move(ready_);
	ready_.reset();
	return result;
}

void ImageOperationWorker::Retire(ImageOperationResult&& result) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!stopping_) {
			retiredResults_.push_back(std::move(result));
			available_.notify_all();
			return;
		}
	}
	RetireResultBuffers(result);
}

void ImageOperationWorker::Retire(RetiredImageBuffers&& buffers) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!stopping_) {
			retiredBuffers_.push_back(std::move(buffers));
			available_.notify_all();
			return;
		}
	}
	RetireBuffers(buffers);
}

bool ImageOperationWorker::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(mutex_);
	return idle_.wait_for(lock, timeout,
		[this] {
			return !active_ && !pending_.has_value() && retiredResults_.empty() &&
				retiredBuffers_.empty() && !retiring_;
		});
}

std::uint64_t ImageOperationWorker::Generation() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return generation_;
}

bool ImageOperationWorker::IsCurrent(const Work& work) const {
	if (!work.canceled || work.canceled->load(std::memory_order_relaxed)) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	return !stopping_ && work.generation == generation_;
}

void ImageOperationWorker::Run() {
	for (;;) {
		std::optional<Work> work;
		std::optional<ImageOperationResult> retired;
		std::optional<RetiredImageBuffers> retiredBuffers;
		std::optional<ImageOperationResult> finalReady;
		std::optional<Work> finalPending;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			available_.wait(lock, [this] {
				return stopping_ || pending_.has_value() || !retiredResults_.empty() ||
					!retiredBuffers_.empty();
			});
			if (!retiredResults_.empty()) {
				retired = std::move(retiredResults_.front());
				retiredResults_.pop_front();
				retiring_ = true;
			} else if (!retiredBuffers_.empty()) {
				retiredBuffers = std::move(retiredBuffers_.front());
				retiredBuffers_.pop_front();
				retiring_ = true;
			} else if (stopping_) {
				if (pending_) finalPending = std::move(pending_);
				pending_.reset();
				if (ready_) finalReady = std::move(ready_);
				ready_.reset();
				retiring_ = finalReady.has_value();
				lock.unlock();
				if (finalReady) RetireResultBuffers(*finalReady);
				finalPending.reset();
				{
					std::lock_guard<std::mutex> finishLock(mutex_);
					retiring_ = false;
					idle_.notify_all();
				}
				return;
			} else if (pending_) {
				work = std::move(pending_);
				pending_.reset();
				activeCancellation_ = work->canceled;
				active_ = true;
			}
		}
		if (retired) {
			RetireResultBuffers(*retired);
			{
				std::lock_guard<std::mutex> lock(mutex_);
				retiring_ = false;
				idle_.notify_all();
			}
			continue;
		}
		if (retiredBuffers) {
			RetireBuffers(*retiredBuffers);
			{
				std::lock_guard<std::mutex> lock(mutex_);
				retiring_ = false;
				idle_.notify_all();
			}
			continue;
		}
		if (!work) continue;

		ImageOperationResult result;
		result.expected = work->request.document;
		result.kind = work->request.operation.kind;
		result.requestGeneration = work->generation;
		PerfContextScope perfContext(PerfWorkClass::ActiveImageSpread,
			PerfExecution::WorkerThread);
		try {
			WorkContext context;
			context.source = work->request.document.source;
			context.sourcePriority = SourceWorkPriority::Foreground;
			context.shouldContinue = [this, &work] { return IsCurrent(*work); };
			CpuWorkLease cpu = SourceWorkCoordinator::Global().AcquireCpu(context);
			if (cpu && context.Continue()) {
				PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Processing);
				result = processor_(work->request, context.shouldContinue, *budget_);
				result.expected = work->request.document;
				result.kind = work->request.operation.kind;
				result.requestGeneration = work->generation;
			} else if (IsCurrent(*work)) {
				result.failure = {WorkerFailureKind::Cancelled,
					"image-operation CPU admission was cancelled"};
			}
		} catch (const std::exception& error) {
			result.failure = {WorkerFailureKind::Exception, error.what()};
		} catch (...) {
			result.failure = {WorkerFailureKind::Exception,
				"unknown image-operation worker failure"};
		}

		bool published = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			active_ = false;
			activeCancellation_.reset();
			if (!stopping_ && work->canceled &&
				!work->canceled->load(std::memory_order_relaxed) &&
				work->generation == generation_ &&
				(result.success || result.failure.Failed())) {
				if (ready_) retiredResults_.push_back(std::move(*ready_));
				ready_ = std::move(result);
				published = true;
			}
			idle_.notify_all();
			available_.notify_all();
		}
		if (published) UiCompletionWakeup().Notify();
	}
}

void ImageOperationWorker::RetireResultBuffers(ImageOperationResult& result) {
	auto retire = [this](std::shared_ptr<const Image>& pixels,
		CacheReservation& reservation) {
		if (pixels) budget_->RetireAllocation(std::static_pointer_cast<const void>(pixels),
			std::move(reservation));
		pixels.reset();
		reservation.Reset();
	};
	retire(result.sourcePixels, result.sourceReservation);
	retire(result.presentationPixels, result.presentationReservation);
	retire(result.outputPixels, result.outputReservation);
}

void ImageOperationWorker::RetireBuffers(RetiredImageBuffers& buffers) {
	auto retireImage = [this](std::shared_ptr<const Image>& pixels,
		CacheReservation& reservation) {
		if (pixels) budget_->RetireAllocation(std::static_pointer_cast<const void>(pixels),
			std::move(reservation));
		pixels.reset();
		reservation.Reset();
	};
	retireImage(buffers.sourcePixels, buffers.sourceReservation);
	retireImage(buffers.presentationPixels, buffers.presentationReservation);
	retireImage(buffers.outputPixels, buffers.outputReservation);
	if (buffers.decoded) {
		budget_->RetireAllocation(std::static_pointer_cast<const void>(buffers.decoded));
		buffers.decoded.reset();
	}
}

} // namespace jpegview_linux
