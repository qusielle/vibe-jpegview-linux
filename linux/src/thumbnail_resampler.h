#pragma once

#include <cstdint>
#include <vector>

namespace jpegview_linux {

// Downscales straight-alpha BGRA pixels with a source-area box filter. Every
// source pixel covered by a destination pixel contributes proportionally,
// preventing high-frequency details from aliasing in the thumbnail panel.
// Color channels are accumulated with premultiplied alpha to avoid colored
// fringes around transparent image edges.
bool DownsampleThumbnailBgra(const std::vector<std::uint8_t>& source,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	std::vector<std::uint8_t>& target);

} // namespace jpegview_linux
