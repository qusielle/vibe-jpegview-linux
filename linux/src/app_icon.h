#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jpegview_linux {

struct ApplicationIcon {
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> bgra;
};

// Decodes the largest uncompressed BMP frame embedded from JPEGView.ico.
bool DecodeApplicationIcon(ApplicationIcon& icon, std::string& errorMessage);

} // namespace jpegview_linux
