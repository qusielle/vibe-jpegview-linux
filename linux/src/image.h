#pragma once

#include "image_processing.h"

#include <cstdint>
#include <vector>

namespace jpegview_linux {

// Mutable top-to-bottom BGRA8 image used by the viewer after decoding.
// originalWidth/originalHeight retain the source dimensions while width/height
// track non-destructive transforms and explicit resizes.
class Image {
public:
	int width = 0;
	int height = 0;
	int originalWidth = 0;
	int originalHeight = 0;
	std::vector<std::uint8_t> bgra;

	bool StoreBGRA(const std::uint8_t* bgraPixels, int imageWidth, int imageHeight);
	bool Rotate(bool clockwise);
	bool Mirror(bool horizontal);

	// Filters match the resize dialog: 0 point, 1 Lanczos, 2 sharpen-low,
	// and 3 sharpen-medium.
	bool Resize(int newWidth, int newHeight, int filter = 3);

	// Applies JPEGView's histogram-derived automatic correction in place.
	bool AutoContrast(double colorCorrection = 0.0, double contrastCorrection = 0.0);

	// Applies the picture-level controls to the current pixels. Automatic
	// correction remains independently switchable and is applied first.
	bool ApplyProcessing(const ImageProcessingParams& params, bool autoContrast);
};

} // namespace jpegview_linux
