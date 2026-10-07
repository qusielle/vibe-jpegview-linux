#pragma once

#include "image.h"
#include "image_transform_geometry.h"

#include <functional>

namespace jpegview_linux {

enum class ImageTransformSampling {
	PreviewBilinear,
	FinalBicubic,
};

// Resamples a validated BGRA image into the geometry's output canvas. Pixels
// outside the source are transparent; color interpolation is alpha-aware.
// The caller's output is left untouched on failure or cancellation.
bool ResampleImageTransform(const Image& source,
	const ImageTransformGeometry& geometry, ImageTransformSampling sampling,
	Image& output, const std::function<bool()>& shouldContinue = {});

} // namespace jpegview_linux
