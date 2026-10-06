#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jpegview_linux {

struct PixelColorRgba {
	std::uint8_t red = 0;
	std::uint8_t green = 0;
	std::uint8_t blue = 0;
	std::uint8_t alpha = 0;
};

// Reads one top-to-bottom BGRA8 document pixel and returns channels in RGBA order.
std::optional<PixelColorRgba> SampleBgraPixel(const std::vector<std::uint8_t>& bgra,
	int width, int height, int x, int y);

// Formats the color as CSS-style hexadecimal in #RRGGBBAA order.
std::string FormatPixelColorRgba(const PixelColorRgba& color);

} // namespace jpegview_linux
