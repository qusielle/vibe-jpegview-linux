#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace jpegview_linux {

std::string FormatFileSize(std::uintmax_t size);

// Formats a zero-based current image index and optional visible spread partner
// as a one-based position such as "1/123" or "1-2/123".
std::string FormatImagePosition(std::size_t currentIndex, std::size_t imageCount,
	std::optional<std::size_t> spreadPartnerIndex = std::nullopt);

// Produces the compact EXIF-popup summary: "W X H, Size". An unavailable file
// size is omitted without leaving punctuation behind.
std::string FormatImageDimensionsAndSize(int width, int height,
	std::string_view formattedFileSize);

std::string FormatModificationDateLine(std::string_view date);

} // namespace jpegview_linux
