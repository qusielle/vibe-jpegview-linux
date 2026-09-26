#include "transparency_pattern.h"

namespace jpegview_linux {

bool ParseTransparencyPattern(const std::string& value, TransparencyPattern& pattern) {
	if (value == "black") pattern = TransparencyPattern::Black;
	else if (value == "white") pattern = TransparencyPattern::White;
	else if (value == "checkerboard") pattern = TransparencyPattern::Checkerboard;
	else return false;
	return true;
}

const char* TransparencyPatternSettingName(TransparencyPattern pattern) {
	switch (pattern) {
	case TransparencyPattern::White: return "white";
	case TransparencyPattern::Checkerboard: return "checkerboard";
	case TransparencyPattern::Black:
	default: return "black";
	}
}

TransparencyPatternColor TransparencyPatternTileColor(
	TransparencyPattern pattern, int tileX, int tileY) {
	switch (pattern) {
	case TransparencyPattern::White:
		return {255, 255, 255};
	case TransparencyPattern::Checkerboard: {
		const std::uint8_t shade = ((tileX + tileY) & 1) == 0 ? 208 : 144;
		return {shade, shade, shade};
	}
	case TransparencyPattern::Black:
	default:
		return {0, 0, 0};
	}
}

} // namespace jpegview_linux
