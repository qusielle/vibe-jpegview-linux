#include "thumbnail_panel_model.h"

#include <algorithm>
#include <cmath>

namespace jpegview_linux {

ThumbnailPanelLayout CalculateThumbnailPanelLayout(int windowWidth, int windowHeight,
	bool panelVisible, int preferredPanelWidth) {
	const int width = std::max(0, windowWidth);
	const int height = std::max(0, windowHeight);
	const int panelWidth = panelVisible && width > 1 && preferredPanelWidth > 0 ?
		std::min(preferredPanelWidth, width - 1) : 0;
	return {panelWidth, panelWidth, width - panelWidth, height};
}

int ThumbnailRowHeight(int panelWidth, int verticalMargin) {
	if (panelWidth <= 0) return 0;
	const int margin = std::max(0, verticalMargin);
	const int imageHeight = std::max(1, static_cast<int>(std::round(panelWidth * 2.0 / 3.0)));
	return imageHeight + margin * 2 + 1;
}

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

ThumbnailRect ThumbnailImageRect(int sourceWidth, int sourceHeight,
	int panelWidth, int rowY, int rowHeight, int verticalMargin) {
	const int margin = std::max(0, verticalMargin);
	const int availableHeight = rowHeight - margin * 2 - 1;
	const ThumbnailSize size = FitThumbnailSize(sourceWidth, sourceHeight,
		panelWidth, availableHeight);
	if (size.width <= 0 || size.height <= 0) return {};
	return {
		(panelWidth - size.width) / 2,
		rowY + margin + (availableHeight - size.height) / 2,
		size.width,
		size.height
	};
}

} // namespace jpegview_linux
