#include "image_info_model.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr std::size_t kMaximumFormattedWindowTitleBytes = 16384;

bool IsWindowTitleToken(char token) {
	switch (token) {
	case '%': case 'p': case 'i': case 'n': case 'f': case 'F': case 'e':
	case 'P': case 'D': case 'w': case 'h': case 's': case 'b': case 'm':
	case 'a': case 'v':
		return true;
	default:
		return false;
	}
}

void SetWindowTitlePatternError(std::string* error, std::string message) {
	if (error != nullptr) *error = std::move(message);
}

bool AppendWindowTitleText(std::string& output, std::string_view text) {
	if (text.empty()) return true;
	const std::size_t remaining = kMaximumFormattedWindowTitleBytes - output.size();
	if (text.size() <= remaining) {
		output.append(text.data(), text.size());
		return true;
	}
	std::size_t count = remaining;
	while (count > 0 && count < text.size() &&
		(static_cast<unsigned char>(text[count]) & 0xc0u) == 0x80u) --count;
	output.append(text.data(), count);
	return false;
}

std::string WindowTitleTokenValue(char token, const WindowTitleContext& context) {
	switch (token) {
	case 'p': return context.position;
	case 'i':
		return context.currentIndex < context.imageCount ?
			std::to_string(context.currentIndex + 1) : std::string();
	case 'n': return std::to_string(context.imageCount);
	case 'f': return context.filename;
	case 'F': return context.filenameStem;
	case 'e': return context.extension;
	case 'P': return context.fullPath;
	case 'D': return context.directory;
	case 'w': return context.width > 0 ? std::to_string(context.width) : std::string();
	case 'h': return context.height > 0 ? std::to_string(context.height) : std::string();
	case 's': return context.fileSize.has_value() ? FormatFileSize(*context.fileSize) : std::string();
	case 'b': return context.fileSize.has_value() ? std::to_string(*context.fileSize) : std::string();
	case 'm': {
		std::string details;
		if (context.width > 0 && context.height > 0) {
			details = std::to_string(context.width) + "x" + std::to_string(context.height);
		}
		if (context.fileSize.has_value()) {
			if (!details.empty()) details += ", ";
			details += FormatFileSize(*context.fileSize);
		}
		return details;
	}
	case 'a': return context.applicationName;
	case 'v': return context.applicationVersion;
	default: return {};
	}
}

} // namespace

std::string NormalizeWindowTitlePattern(std::string_view pattern) {
	std::size_t first = 0;
	while (first < pattern.size() &&
		std::isspace(static_cast<unsigned char>(pattern[first])) != 0) ++first;
	std::size_t last = pattern.size();
	while (last > first && std::isspace(static_cast<unsigned char>(pattern[last - 1])) != 0) --last;
	if (first == last) return kDefaultWindowTitlePattern;
	return std::string(pattern.substr(first, last - first));
}

bool ValidateWindowTitlePattern(std::string_view pattern, std::string* error) {
	if (pattern.size() > kMaximumWindowTitlePatternBytes) {
		SetWindowTitlePatternError(error, "Window title pattern exceeds 1024 bytes.");
		return false;
	}
	for (const char character : pattern) {
		if (character == '\n' || character == '\r' || character == '\0') {
			SetWindowTitlePatternError(error, "Window title pattern must fit on one line.");
			return false;
		}
	}
	for (std::size_t index = 0; index < pattern.size(); ++index) {
		if (pattern[index] != '%') continue;
		if (index + 1 >= pattern.size()) {
			SetWindowTitlePatternError(error, "A trailing % is incomplete; use %% for a literal percent.");
			return false;
		}
		const char token = pattern[++index];
		if (!IsWindowTitleToken(token)) {
			SetWindowTitlePatternError(error,
				std::string("Unknown window-title code %") + token + ".");
			return false;
		}
	}
	if (error != nullptr) error->clear();
	return true;
}

std::string FormatWindowTitle(std::string_view pattern, const WindowTitleContext& context) {
	std::string normalized = NormalizeWindowTitlePattern(pattern);
	if (!ValidateWindowTitlePattern(normalized)) normalized = kDefaultWindowTitlePattern;
	std::string output;
	output.reserve(std::min(normalized.size() + 128, kMaximumFormattedWindowTitleBytes));
	for (std::size_t index = 0; index < normalized.size();) {
		if (normalized[index] != '%') {
			const std::size_t start = index++;
			while (index < normalized.size() && normalized[index] != '%') ++index;
			if (!AppendWindowTitleText(output, std::string_view(normalized).substr(start, index - start))) break;
			continue;
		}
		const char token = normalized[++index];
		++index;
		if (token == '%') {
			if (!AppendWindowTitleText(output, "%")) break;
		} else if (!AppendWindowTitleText(output, WindowTitleTokenValue(token, context))) {
			break;
		}
	}
	return output;
}

std::string FormatImagePosition(std::size_t currentIndex, std::size_t imageCount,
	std::optional<std::size_t> spreadPartnerIndex) {
	if (imageCount == 0 || currentIndex >= imageCount) return {};

	std::size_t firstIndex = currentIndex;
	std::size_t lastIndex = currentIndex;
	if (spreadPartnerIndex.has_value() && *spreadPartnerIndex < imageCount &&
		*spreadPartnerIndex != currentIndex) {
		firstIndex = std::min(currentIndex, *spreadPartnerIndex);
		lastIndex = std::max(currentIndex, *spreadPartnerIndex);
	}

	std::string position = std::to_string(firstIndex + 1);
	if (lastIndex != firstIndex) position += "-" + std::to_string(lastIndex + 1);
	return position + "/" + std::to_string(imageCount);
}

std::string FormatFileSize(std::uintmax_t size) {
	static constexpr std::array<const char*, 4> suffixes = {"B", "KB", "MB", "GB"};
	double value = static_cast<double>(size);
	std::size_t suffix = 0;
	while (value >= 1024.0 && suffix + 1 < suffixes.size()) {
		value /= 1024.0;
		++suffix;
	}
	std::ostringstream stream;
	if (suffix == 0) {
		stream << size << ' ' << suffixes[suffix];
	} else {
		stream << std::fixed << std::setprecision(value >= 10.0 ? 0 : 1)
			<< value << ' ' << suffixes[suffix];
	}
	return stream.str();
}

std::string FormatImageDimensionsAndSize(int width, int height,
	std::string_view formattedFileSize) {
	std::string line = std::to_string(width) + " X " + std::to_string(height);
	if (!formattedFileSize.empty()) {
		line += ", ";
		line += formattedFileSize;
	}
	return line;
}

std::string FormatModificationDateLine(std::string_view date) {
	return std::string(date);
}

std::string FormatAnimationPlaybackStatus(bool animationPlaying,
	bool manuallyPaused) {
	if (manuallyPaused) return "frozen";
	return animationPlaying ? "playing" : "paused";
}

const std::string& WindowTitleFormatCache::GetOrBuild(const std::string& key,
	const std::function<std::string()>& builder) {
	if (!valid_ || key_ != key) {
		key_ = key;
		value_ = builder ? builder() : std::string();
		valid_ = true;
	}
	return value_;
}

void WindowTitleFormatCache::Clear() {
	key_.clear();
	value_.clear();
	valid_ = false;
}

const std::vector<std::string>& ImageInfoLineCache::GetOrBuild(const std::string& key,
	const std::function<std::vector<std::string>()>& builder) {
	if (!valid_ || key_ != key) {
		key_ = key;
		lines_ = builder ? builder() : std::vector<std::string>();
		valid_ = true;
	}
	return lines_;
}

void ImageInfoLineCache::Clear() {
	key_.clear();
	lines_.clear();
	valid_ = false;
}

bool AppliedWindowTitle::Update(std::string title) {
	if (valid_ && title_ == title) return false;
	title_ = std::move(title);
	valid_ = true;
	return true;
}

void AppliedWindowTitle::Clear() {
	title_.clear();
	valid_ = false;
}

} // namespace jpegview_linux
