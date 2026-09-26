#pragma once

#include <cstdint>
#include <string>

namespace jpegview_linux {

enum class TransparencyPattern {
	Black,
	White,
	Checkerboard,
};

inline constexpr int kTransparencyCheckerCellSize = 16;

struct TransparencyPatternColor {
	std::uint8_t red = 0;
	std::uint8_t green = 0;
	std::uint8_t blue = 0;
};

bool ParseTransparencyPattern(const std::string& value, TransparencyPattern& pattern);
const char* TransparencyPatternSettingName(TransparencyPattern pattern);
TransparencyPatternColor TransparencyPatternTileColor(
	TransparencyPattern pattern, int tileX, int tileY);

} // namespace jpegview_linux
