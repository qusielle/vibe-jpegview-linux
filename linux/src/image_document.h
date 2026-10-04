#pragma once

#include "archive_source.h"
#include "cache_budget.h"
#include "image.h"
#include "image_decoder.h"
#include "work_context.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace jpegview_linux {

enum class ImageOperationKind {
	Materialize,
	Transform,
	Crop,
	Resize,
	Reprocess,
	PrepareOutput,
	CopySelection,
};

enum class ImageTransformKind {
	RotateClockwise,
	RotateCounterClockwise,
	MirrorHorizontal,
	MirrorVertical,
};

struct ImageDocumentSnapshot {
	SourceKey source;
	std::uint64_t ownerGeneration = 0;
	std::uint64_t revision = 0;
	std::size_t frameIndex = 0;
	bool animated = false;
	std::shared_ptr<const Image> sourcePixels;
	std::shared_ptr<const Image> presentationPixels;
	ImageProcessingParams processing;
	ImageProcessingParams materializedProcessing;
	bool autoContrast = false;
	bool materializedAutoContrast = false;
	bool modified = false;
	bool detached = false;
	int rotationQuarterTurns = 0;
};

struct ImageOperationSpec {
	ImageOperationKind kind = ImageOperationKind::Materialize;
	ImageTransformKind transform = ImageTransformKind::RotateClockwise;
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
	int width = 0;
	int height = 0;
	int resizeFilter = 3;
};

struct ImageOperationRequest {
	ImageDocumentSnapshot document;
	std::shared_ptr<const DecodedImage> decoded;
	ImageOperationSpec operation;
};

struct ImageOperationResult {
	ImageDocumentSnapshot expected;
	std::uint64_t requestGeneration = 0;
	ImageOperationKind kind = ImageOperationKind::Materialize;
	bool updatesDocument = false;
	bool success = false;
	bool modified = false;
	bool flattenAnimation = false;
	bool detached = false;
	int rotationQuarterTurns = 0;
	std::shared_ptr<const Image> sourcePixels;
	std::shared_ptr<const Image> presentationPixels;
	std::shared_ptr<const Image> outputPixels;
	CacheReservation sourceReservation;
	CacheReservation presentationReservation;
	CacheReservation outputReservation;
	WorkerFailure failure;
};

struct RetiredImageBuffers {
	std::shared_ptr<const Image> sourcePixels;
	std::shared_ptr<const Image> presentationPixels;
	std::shared_ptr<const Image> outputPixels;
	std::shared_ptr<const DecodedImage> decoded;
	CacheReservation sourceReservation;
	CacheReservation presentationReservation;
	CacheReservation outputReservation;
};

// The document is the immutable pixel owner for the current edit. Presentation
// pixels remain separate from the source base so processing can be reapplied
// without decoding again. Ordinary viewing keeps both pointers empty.
class ImageDocument {
public:
	const Image& Presentation() const;
	const std::shared_ptr<const Image>& PresentationPixels() const {
		return presentationPixels_;
	}
	const std::shared_ptr<const Image>& SourcePixels() const { return sourcePixels_; }
	bool HasMaterializedPixels() const {
		return sourcePixels_ != nullptr && presentationPixels_ != nullptr;
	}

	ImageDocumentSnapshot Snapshot() const;
	bool Matches(const ImageDocumentSnapshot& snapshot) const;
	bool CanApply(const ImageOperationResult& result) const;
	bool Apply(ImageOperationResult& result, RetiredImageBuffers& retired);

	RetiredImageBuffers BeginSelection(const SourceKey& source,
		std::uint64_t ownerGeneration, bool clearPixels);
	RetiredImageBuffers SetFrame(std::size_t frameIndex, bool animated,
		int width, int height, bool hasTransparency);
	void SetDimensions(int width, int height, bool hasTransparency);
	void UpdateProcessing(const ImageProcessingParams& processing, bool autoContrast);
	RetiredImageBuffers ClearPixels();

	void MarkDetached();
	void MarkDocumentChanged();
	void SetFrameIndex(std::size_t frameIndex);
	void SetAnimation(bool animated);

	std::uint64_t Revision() const { return revision_; }
	std::uint64_t OwnerGeneration() const { return ownerGeneration_; }
	const SourceKey& Source() const { return source_; }
	std::size_t FrameIndex() const { return frameIndex_; }
	bool Animated() const { return animated_; }
	bool Modified() const { return modified_; }
	bool Detached() const { return detached_; }
	int RotationQuarterTurns() const { return rotationQuarterTurns_; }
	const ImageProcessingParams& Processing() const { return processing_; }
	bool AutoContrast() const { return autoContrast_; }
	const ImageProcessingParams& MaterializedProcessing() const {
		return materializedProcessing_;
	}
	bool MaterializedAutoContrast() const { return materializedAutoContrast_; }

private:
	void AdvanceRevision();

	SourceKey source_;
	std::uint64_t ownerGeneration_ = 0;
	std::uint64_t revision_ = 0;
	std::size_t frameIndex_ = 0;
	bool animated_ = false;
	std::shared_ptr<const Image> sourcePixels_;
	std::shared_ptr<const Image> presentationPixels_;
	CacheReservation sourceReservation_;
	CacheReservation presentationReservation_;
	Image metadata_;
	ImageProcessingParams processing_;
	ImageProcessingParams materializedProcessing_;
	bool autoContrast_ = false;
	bool materializedAutoContrast_ = false;
	bool modified_ = false;
	bool detached_ = false;
	int rotationQuarterTurns_ = 0;
};

} // namespace jpegview_linux
