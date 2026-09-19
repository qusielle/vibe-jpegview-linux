#include "thumbnail_panel_model.h"

#include <algorithm>
#include <cmath>

namespace jpegview_linux {

std::vector<ThumbnailSlot> ThumbnailPanelSlots(std::size_t fileCount,
	std::size_t currentIndex, int windowHeight, int rowHeight) {
	std::vector<ThumbnailSlot> slots;
	if (fileCount == 0 || currentIndex >= fileCount || windowHeight <= 0 || rowHeight <= 0) return slots;
	const int currentY = (windowHeight - rowHeight) / 2;
	const int maximumOffset = windowHeight / rowHeight + 2;
	for (int offset = -maximumOffset; offset <= maximumOffset; ++offset) {
		const long long index = static_cast<long long>(currentIndex) + offset;
		if (index < 0 || index >= static_cast<long long>(fileCount)) continue;
		const int y = currentY + offset * rowHeight;
		if (y >= windowHeight || y + rowHeight <= 0) continue;
		slots.push_back({static_cast<std::size_t>(index), y, offset == 0});
	}
	return slots;
}

std::vector<std::size_t> ThumbnailPreloadOrder(std::size_t fileCount,
	std::size_t currentIndex, std::size_t maximumCount) {
	std::vector<std::size_t> order;
	if (fileCount == 0 || currentIndex >= fileCount || maximumCount == 0) return order;
	order.reserve(std::min(fileCount, maximumCount));
	order.push_back(currentIndex);
	for (std::size_t distance = 1; order.size() < maximumCount && order.size() < fileCount; ++distance) {
		if (distance <= currentIndex) order.push_back(currentIndex - distance);
		if (order.size() >= maximumCount || order.size() >= fileCount) break;
		if (distance < fileCount - currentIndex) order.push_back(currentIndex + distance);
	}
	return order;
}

ThumbnailSize FitThumbnailSize(int sourceWidth, int sourceHeight,
	int maximumWidth, int maximumHeight) {
	if (sourceWidth <= 0 || sourceHeight <= 0 || maximumWidth <= 0 || maximumHeight <= 0) return {};
	const double scale = std::min({1.0, static_cast<double>(maximumWidth) / sourceWidth,
		static_cast<double>(maximumHeight) / sourceHeight});
	return {
		std::max(1, static_cast<int>(std::floor(sourceWidth * scale + 0.5))),
		std::max(1, static_cast<int>(std::floor(sourceHeight * scale + 0.5)))
	};
}

} // namespace jpegview_linux
