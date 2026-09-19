#pragma once

#include <cstddef>
#include <vector>

namespace jpegview_linux {

struct ThumbnailSlot {
	std::size_t fileIndex = 0;
	int y = 0;
	bool current = false;
};

struct ThumbnailSize {
	int width = 0;
	int height = 0;
};

// Returns visible rows in file-list order. The current file's row is centered
// vertically; rows outside the window are omitted rather than wrapping.
std::vector<ThumbnailSlot> ThumbnailPanelSlots(std::size_t fileCount,
	std::size_t currentIndex, int windowHeight, int rowHeight);

// Returns file indices nearest to the current file first. Equal-distance
// entries prefer the preceding file, matching their top-to-bottom placement.
std::vector<std::size_t> ThumbnailPreloadOrder(std::size_t fileCount,
	std::size_t currentIndex, std::size_t maximumCount);

// Aspect-fit dimensions for a thumbnail. Images are never enlarged.
ThumbnailSize FitThumbnailSize(int sourceWidth, int sourceHeight,
	int maximumWidth, int maximumHeight);

} // namespace jpegview_linux
