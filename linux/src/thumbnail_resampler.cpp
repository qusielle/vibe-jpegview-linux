#include "thumbnail_resampler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>

namespace jpegview_linux {

bool DownsampleThumbnailBgra(const std::vector<std::uint8_t>& source,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	std::vector<std::uint8_t>& target) {
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

} // namespace jpegview_linux
