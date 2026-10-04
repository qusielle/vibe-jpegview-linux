#include "image_document.h"

#include "image_processing.h"

#include <utility>

namespace jpegview_linux {

const Image& ImageDocument::Presentation() const {
	return presentationPixels_ ? *presentationPixels_ : metadata_;
}

ImageDocumentSnapshot ImageDocument::Snapshot() const {
	return {source_, ownerGeneration_, revision_, frameIndex_, animated_, sourcePixels_,
		presentationPixels_, processing_, materializedProcessing_, autoContrast_,
		materializedAutoContrast_, modified_, detached_, rotationQuarterTurns_};
}

bool ImageDocument::Matches(const ImageDocumentSnapshot& snapshot) const {
	return source_ == snapshot.source && ownerGeneration_ == snapshot.ownerGeneration &&
		revision_ == snapshot.revision && frameIndex_ == snapshot.frameIndex;
}

bool ImageDocument::CanApply(const ImageOperationResult& result) const {
	return result.success && result.updatesDocument && Matches(result.expected) &&
		result.sourcePixels != nullptr && result.presentationPixels != nullptr;
}

bool ImageDocument::Apply(ImageOperationResult& result, RetiredImageBuffers& retired) {
	if (!CanApply(result)) return false;

	if (sourcePixels_ == result.sourcePixels) {
		result.sourcePixels.reset();
		result.sourceReservation.Reset();
	} else {
		retired.sourcePixels = std::move(sourcePixels_);
		retired.sourceReservation = std::move(sourceReservation_);
		sourcePixels_ = std::move(result.sourcePixels);
		sourceReservation_ = std::move(result.sourceReservation);
	}
	if (presentationPixels_ == result.presentationPixels) {
		result.presentationPixels.reset();
		result.presentationReservation.Reset();
	} else {
		retired.presentationPixels = std::move(presentationPixels_);
		retired.presentationReservation = std::move(presentationReservation_);
		presentationPixels_ = std::move(result.presentationPixels);
		presentationReservation_ = std::move(result.presentationReservation);
	}

	metadata_ = {};
	metadata_.width = presentationPixels_->width;
	metadata_.height = presentationPixels_->height;
	metadata_.originalWidth = presentationPixels_->originalWidth;
	metadata_.originalHeight = presentationPixels_->originalHeight;
	metadata_.hasTransparency = presentationPixels_->hasTransparency;
	modified_ = result.modified;
	detached_ = result.detached;
	if (result.flattenAnimation) animated_ = false;
	rotationQuarterTurns_ = result.rotationQuarterTurns;
	materializedProcessing_ = processing_;
	materializedAutoContrast_ = autoContrast_;
	AdvanceRevision();
	return true;
}

RetiredImageBuffers ImageDocument::BeginSelection(const SourceKey& source,
	std::uint64_t ownerGeneration, bool clearPixels) {
	RetiredImageBuffers retired;
	if (clearPixels) {
		retired = ClearPixels();
		frameIndex_ = 0;
		animated_ = false;
		modified_ = false;
		detached_ = false;
		rotationQuarterTurns_ = 0;
		materializedProcessing_ = {};
		materializedAutoContrast_ = false;
	}
	source_ = source;
	ownerGeneration_ = ownerGeneration;
	AdvanceRevision();
	return retired;
}

RetiredImageBuffers ImageDocument::SetFrame(std::size_t frameIndex, bool animated,
	int width, int height, bool hasTransparency) {
	RetiredImageBuffers retired = ClearPixels();
	frameIndex_ = frameIndex;
	animated_ = animated;
	modified_ = false;
	detached_ = false;
	rotationQuarterTurns_ = 0;
	materializedProcessing_ = {};
	materializedAutoContrast_ = false;
	SetDimensions(width, height, hasTransparency);
	AdvanceRevision();
	return retired;
}

void ImageDocument::SetDimensions(int width, int height, bool hasTransparency) {
	if (metadata_.width == width && metadata_.originalWidth == width &&
		metadata_.height == height && metadata_.originalHeight == height &&
		metadata_.hasTransparency == hasTransparency) return;
	metadata_ = {};
	metadata_.width = metadata_.originalWidth = width;
	metadata_.height = metadata_.originalHeight = height;
	metadata_.hasTransparency = hasTransparency;
	AdvanceRevision();
}

void ImageDocument::UpdateProcessing(const ImageProcessingParams& processing,
	bool autoContrast) {
	if (jpegview_linux::EqualImageProcessing(processing_, processing) &&
		autoContrast_ == autoContrast) return;
	processing_ = processing;
	autoContrast_ = autoContrast;
	AdvanceRevision();
}

RetiredImageBuffers ImageDocument::ClearPixels() {
	RetiredImageBuffers retired;
	retired.sourcePixels = std::move(sourcePixels_);
	retired.presentationPixels = std::move(presentationPixels_);
	retired.sourceReservation = std::move(sourceReservation_);
	retired.presentationReservation = std::move(presentationReservation_);
	metadata_ = {};
	materializedProcessing_ = {};
	materializedAutoContrast_ = false;
	AdvanceRevision();
	return retired;
}

void ImageDocument::MarkDetached() {
	if (detached_) return;
	detached_ = true;
	AdvanceRevision();
}

void ImageDocument::MarkDocumentChanged() {
	modified_ = true;
	AdvanceRevision();
}

void ImageDocument::SetFrameIndex(std::size_t frameIndex) {
	if (frameIndex_ == frameIndex) return;
	frameIndex_ = frameIndex;
	AdvanceRevision();
}

void ImageDocument::SetAnimation(bool animated) {
	if (animated_ == animated) return;
	animated_ = animated;
	AdvanceRevision();
}

void ImageDocument::AdvanceRevision() {
	++revision_;
	if (revision_ == 0) ++revision_;
}

} // namespace jpegview_linux
