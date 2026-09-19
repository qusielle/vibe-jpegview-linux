#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jpegview_linux {

inline constexpr int kSpectrumBinCount = 256;
inline constexpr int kSpectrumGraphWidth = 256;
inline constexpr int kSpectrumGraphHeight = 50;
inline constexpr int kSpectrumBottomGap = 10;
inline constexpr int kSpectrumButtonSize = 18;

using GrayscaleSpectrum = std::array<std::uint64_t, kSpectrumBinCount>;

// Builds the same weighted grayscale histogram shown by Windows JPEGView.
// Large images are sampled on a regular grid to bound the work per frame.
GrayscaleSpectrum BuildGrayscaleSpectrum(const std::vector<std::uint8_t>& bgra,
	int width, int height);

std::array<int, kSpectrumBinCount> GrayscaleSpectrumBarHeights(
	const GrayscaleSpectrum& spectrum, int maximumHeight);

} // namespace jpegview_linux
