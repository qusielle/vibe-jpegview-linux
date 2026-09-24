#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jpegview_linux {

// A decoded frame is always a top-to-bottom, straight-alpha BGRA8 bitmap.
// The delay is the display time used by the animation timer.  Static images
// are returned as a one-frame sequence with a zero delay.
struct DecodedFrame {
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> bgra;
	int delayMs = 0;
};

struct DecodedImage {
	std::vector<DecodedFrame> frames;
	bool animation = false;
	int loopCount = 0; // zero means loop forever, as in GIF/WebP.
};

bool DecodeImage(const std::filesystem::path& filename, DecodedImage& image,
	std::string& errorMessage);

bool IsJpegPath(const std::filesystem::path& filename);

// Reads only the JPEG header. This is used to calculate a stable fitted
// viewport before committing CPU time and memory to pixel decompression.
bool ReadJpegDimensions(const std::filesystem::path& filename, int& width, int& height,
	std::string& errorMessage);

// Returns the source JPEG's minimum-coded-unit dimensions from its component
// sampling factors. Lossless crop origins and interior boundaries use these.
bool ReadJpegMcuSize(const std::filesystem::path& filename, int& width, int& height,
	std::string& errorMessage);

// Uses libjpeg's native DCT scaling to decode the smallest available image
// that is still at least the requested size. sourceWidth/sourceHeight always
// report the full JPEG dimensions; the returned frame can be smaller.
bool DecodeJpegForDisplay(const std::filesystem::path& filename,
	int minimumWidth, int minimumHeight, DecodedImage& image,
	int& sourceWidth, int& sourceHeight, std::string& errorMessage);

} // namespace jpegview_linux
