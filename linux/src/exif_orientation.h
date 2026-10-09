#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jpegview_linux {

bool IsValidExifOrientation(int orientation);
bool ExifOrientationSwapsAxes(int orientation);

// Transforms top-to-bottom BGRA pixels according to EXIF orientations 1-8.
// Width, height, and pixels change only after a complete, successful transform.
bool ApplyExifOrientation(int orientation, int& width, int& height,
	std::vector<std::uint8_t>& bgra, std::string& errorMessage,
	const std::function<bool()>& shouldContinue = {});

} // namespace jpegview_linux
