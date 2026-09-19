#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace jpegview_linux {

std::string FormatFileSize(std::uintmax_t size);

// Produces the compact EXIF-popup summary: "W X H, Size". An unavailable file
// size is omitted without leaving punctuation behind.
std::string FormatImageDimensionsAndSize(int width, int height,
	std::string_view formattedFileSize);

std::string FormatModificationDateLine(std::string_view date);

} // namespace jpegview_linux
