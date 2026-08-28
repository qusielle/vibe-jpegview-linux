#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace jpegview_linux {

struct ImageWriteOptions {
	int jpegQuality = 85;
	int webpQuality = 85;
	bool webpLossless = false;
};

// Writes a top-to-bottom BGRA pixel buffer. The output format is selected from
// the filename extension, matching the format selection in Windows SaveImage.
bool WriteImage(const std::filesystem::path& filename, const std::uint8_t* bgra,
	int width, int height, const ImageWriteOptions& options, std::string& errorMessage);

} // namespace jpegview_linux
