#include "image_info_model.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace jpegview_linux {

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

} // namespace jpegview_linux
