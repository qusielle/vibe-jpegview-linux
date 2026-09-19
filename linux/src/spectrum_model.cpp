#include "spectrum_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jpegview_linux {

GrayscaleSpectrum BuildGrayscaleSpectrum(const std::vector<std::uint8_t>& bgra,
	int width, int height) {
	GrayscaleSpectrum spectrum{};
	if (width <= 0 || height <= 0) return spectrum;

	const std::size_t imageWidth = static_cast<std::size_t>(width);
	const std::size_t imageHeight = static_cast<std::size_t>(height);
	if (imageWidth > std::numeric_limits<std::size_t>::max() / imageHeight / 4 ||
		bgra.size() < imageWidth * imageHeight * 4) {
		return spectrum;
	}

	const std::size_t pixelCount = imageWidth * imageHeight;
	const std::size_t grid = std::max<std::size_t>(1, static_cast<std::size_t>(
		0.5 + std::sqrt(1.0 + static_cast<double>(pixelCount / 50000))));
	const std::size_t pixelsPerLine = std::max<std::size_t>(1, imageWidth / grid);
	const std::size_t lines = std::max<std::size_t>(1, imageHeight / grid);
	for (std::size_t row = 0; row < lines; ++row) {
		const std::size_t y = row * grid;
		for (std::size_t column = 0; column < pixelsPerLine; ++column) {
			const std::size_t x = column * grid;
			const std::size_t offset = (y * imageWidth + x) * 4;
			const unsigned int blue = bgra[offset];
			const unsigned int green = bgra[offset + 1];
			const unsigned int red = bgra[offset + 2];
			const std::size_t luminance = (blue * 128 + green * 640 + red * 256) >> 10;
			++spectrum[luminance];
		}
	}
	return spectrum;
}

std::array<int, kSpectrumBinCount> GrayscaleSpectrumBarHeights(
	const GrayscaleSpectrum& spectrum, int maximumHeight) {
	std::array<int, kSpectrumBinCount> heights{};
	if (maximumHeight <= 0) return heights;
	const std::uint64_t maximum = *std::max_element(spectrum.begin(), spectrum.end());
	if (maximum == 0) return heights;

	const double scale = maximumHeight / std::sqrt(static_cast<double>(maximum));
	for (std::size_t index = 0; index < spectrum.size(); ++index) {
		heights[index] = static_cast<int>(std::sqrt(static_cast<double>(spectrum[index])) * scale + 0.5);
	}
	return heights;
}

} // namespace jpegview_linux
