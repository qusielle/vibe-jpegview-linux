#include "exif_orientation.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>

namespace jpegview_linux {
namespace {

constexpr std::uint64_t kMaximumPixels = 100ull * 1024ull * 1024ull;
constexpr int kCancellationRowBatch = 16;

bool Continue(const std::function<bool()>& shouldContinue) {
	if (!shouldContinue) return true;
	try {
		return shouldContinue();
	} catch (...) {
		return false;
	}
}

void MapPixel(int orientation, int sourceWidth, int sourceHeight,
	int sourceX, int sourceY, int& targetX, int& targetY) {
	switch (orientation) {
	case 2:
		targetX = sourceWidth - 1 - sourceX;
		targetY = sourceY;
		break;
	case 3:
		targetX = sourceWidth - 1 - sourceX;
		targetY = sourceHeight - 1 - sourceY;
		break;
	case 4:
		targetX = sourceX;
		targetY = sourceHeight - 1 - sourceY;
		break;
	case 5:
		targetX = sourceY;
		targetY = sourceX;
		break;
	case 6:
		targetX = sourceHeight - 1 - sourceY;
		targetY = sourceX;
		break;
	case 7:
		targetX = sourceHeight - 1 - sourceY;
		targetY = sourceWidth - 1 - sourceX;
		break;
	case 8:
		targetX = sourceY;
		targetY = sourceWidth - 1 - sourceX;
		break;
	default:
		targetX = sourceX;
		targetY = sourceY;
		break;
	}
}

} // namespace

bool IsValidExifOrientation(int orientation) {
	return orientation >= 1 && orientation <= 8;
}

bool ExifOrientationSwapsAxes(int orientation) {
	return orientation >= 5 && orientation <= 8;
}

bool ApplyExifOrientation(int orientation, int& width, int& height,
	std::vector<std::uint8_t>& bgra, std::string& errorMessage,
	const std::function<bool()>& shouldContinue) {
	errorMessage.clear();
	if (!IsValidExifOrientation(orientation)) {
		errorMessage = "invalid EXIF orientation";
		return false;
	}
	if (width <= 0 || height <= 0 || width > 65535 || height > 65535 ||
		static_cast<std::uint64_t>(width) * height > kMaximumPixels ||
		static_cast<std::uint64_t>(width) * height >
			std::numeric_limits<std::size_t>::max() / 4 ||
		bgra.size() != static_cast<std::size_t>(width) * height * 4) {
		errorMessage = "invalid EXIF orientation pixel buffer";
		return false;
	}
	if (!Continue(shouldContinue)) {
		errorMessage = "EXIF orientation transform was cancelled";
		return false;
	}
	if (orientation == 1) return true;

	const int outputWidth = ExifOrientationSwapsAxes(orientation) ? height : width;
	const int outputHeight = ExifOrientationSwapsAxes(orientation) ? width : height;
	std::vector<std::uint8_t> transformed;
	try {
		transformed.resize(bgra.size());
	} catch (const std::exception&) {
		errorMessage = "out of memory while applying EXIF orientation";
		return false;
	}
	for (int y = 0; y < height; ++y) {
		if (y % kCancellationRowBatch == 0 && !Continue(shouldContinue)) {
			errorMessage = "EXIF orientation transform was cancelled";
			return false;
		}
		for (int x = 0; x < width; ++x) {
			int targetX = 0;
			int targetY = 0;
			MapPixel(orientation, width, height, x, y, targetX, targetY);
			const std::size_t sourceOffset =
				(static_cast<std::size_t>(y) * width + x) * 4;
			const std::size_t targetOffset =
				(static_cast<std::size_t>(targetY) * outputWidth + targetX) * 4;
			std::copy_n(bgra.data() + sourceOffset, 4,
				transformed.data() + targetOffset);
		}
	}
	if (!Continue(shouldContinue)) {
		errorMessage = "EXIF orientation transform was cancelled";
		return false;
	}
	bgra.swap(transformed);
	width = outputWidth;
	height = outputHeight;
	return true;
}

} // namespace jpegview_linux
