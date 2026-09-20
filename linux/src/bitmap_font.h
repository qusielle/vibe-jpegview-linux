#pragma once

#include <cstdint>
#include <string_view>

namespace jpegview_linux {

struct BitmapFontGlyph {
	std::uint16_t pixelOffset = 0;
	std::uint8_t width = 0;
	std::uint8_t height = 0;
	std::int8_t bearingLeft = 0;
	std::int8_t bearingTop = 0;
	std::uint8_t advance = 0;
};

const BitmapFontGlyph& Terminus9Glyph(unsigned char character);
const std::uint8_t* Terminus9GlyphPixels(const BitmapFontGlyph& glyph);
bool Terminus9CanRender(std::string_view text);
int Terminus9TextWidth(std::string_view text, int scale = 1);
int Terminus9Ascent();
int Terminus9LineHeight();

} // namespace jpegview_linux
