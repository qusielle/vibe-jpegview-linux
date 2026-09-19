#include "image_info_model.h"

#include <array>
#include <iomanip>
#include <sstream>

namespace jpegview_linux {

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
