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

} // namespace jpegview_linux
