#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace jpegview_linux {

inline constexpr char kDefaultWindowTitlePattern[] = "[%p] %f (%m) - %a";
inline constexpr std::size_t kMaximumWindowTitlePatternBytes = 1024;
inline constexpr char kWindowTitlePatternHelpLine1[] =
	"Title codes: %p visible position/total; %i one-based image index; %n total images; %f filename+extension; %F filename stem (no extension); %e extension (no dot)";
inline constexpr char kWindowTitlePatternHelpLine2[] =
	"%P full path; %D containing folder; %w/%h original pixel width/height; %s readable size; %b exact bytes";
inline constexpr char kWindowTitlePatternHelpLine3[] =
	"%m dimensions+readable size; %a app name; %v build version; %% literal percent";

struct WindowTitleContext {
	std::string position;
	std::size_t currentIndex = 0;
	std::size_t imageCount = 0;
	std::string filename;
	std::string filenameStem;
	std::string extension;
	std::string fullPath;
	std::string directory;
	int width = 0;
	int height = 0;
	std::optional<std::uintmax_t> fileSize;
	std::string applicationName = "JPEGView";
	std::string applicationVersion;
};

// Surrounding whitespace is discarded. An empty pattern resolves to the
// built-in format; unknown or incomplete %-codes are invalid.
std::string NormalizeWindowTitlePattern(std::string_view pattern);
bool ValidateWindowTitlePattern(std::string_view pattern, std::string* error = nullptr);
std::string FormatWindowTitle(std::string_view pattern, const WindowTitleContext& context);

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
