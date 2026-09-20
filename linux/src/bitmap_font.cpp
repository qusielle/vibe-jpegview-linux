#include "bitmap_font.h"
#include "bitmap_font_data.h"

#include <algorithm>

namespace jpegview_linux {

const BitmapFontGlyph& Terminus9Glyph(unsigned char character) {
	constexpr unsigned char firstCharacter = 32;
	constexpr unsigned char lastCharacter = 126;
	constexpr unsigned char fallbackCharacter = '?';
	const unsigned char available = character >= firstCharacter && character <= lastCharacter ?
		character : fallbackCharacter;
	return kEmbeddedBitmapFontGlyphs[available - firstCharacter];
}

const std::uint8_t* Terminus9GlyphPixels(const BitmapFontGlyph& glyph) {
	return kEmbeddedBitmapFontPixels.data() + glyph.pixelOffset;
}

bool Terminus9CanRender(std::string_view text) {
	return std::all_of(text.begin(), text.end(), [](unsigned char character) {
		return character >= 32 && character <= 126;
	});
}

int Terminus9TextWidth(std::string_view text, int scale) {
	if (text.empty() || scale <= 0) return 0;
	int width = 0;
	for (unsigned char character : text) width += Terminus9Glyph(character).advance * scale;
	return width;
}

int Terminus9Ascent() {
	return kEmbeddedBitmapFontAscent;
}

int Terminus9LineHeight() {
	return kEmbeddedBitmapFontLineHeight;
}

} // namespace jpegview_linux
