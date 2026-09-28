#include "double_page_model.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jpegview_linux {
namespace {

bool IsPortrait(const PageDimensions& page) {
	return page.width > 0 && page.height > 0 && page.height > page.width;
}

int ScaledWidthAtHeight(const PageDimensions& page, int height) {
	const double width = static_cast<double>(page.width) * height / page.height;
	if (!std::isfinite(width) || width < 1.0 ||
		width > static_cast<double>(std::numeric_limits<int>::max())) return 0;
	return std::max(1, static_cast<int>(std::lround(width)));
}

} // namespace

bool IsDoublePagePair(std::size_t firstIndex, std::size_t pageCount,
	const PageDimensions& first, const PageDimensions& second, bool coverSingle) {
	if (pageCount < 2 || firstIndex >= pageCount - 1 ||
		(coverSingle && firstIndex == 0)) return false;
	return IsPortrait(first) && IsPortrait(second);
}

std::optional<DoublePageSpread> BuildDoublePageSpread(std::size_t currentIndex,
	std::size_t pageCount, const PageDimensions& current,
	const std::optional<PageDimensions>& next, const DoublePageModeState& modes,
	bool coverSingle) {
	if (!modes.enabled || !next.has_value() ||
		!IsDoublePagePair(currentIndex, pageCount, current, *next, coverSingle)) {
		return std::nullopt;
	}
	const int canvasHeight = std::max(current.height, next->height);
	const int currentWidth = ScaledWidthAtHeight(current, canvasHeight);
	const int nextWidth = ScaledWidthAtHeight(*next, canvasHeight);
	if (currentWidth <= 0 || nextWidth <= 0 ||
		currentWidth > std::numeric_limits<int>::max() - nextWidth) return std::nullopt;

	DoublePageSpread spread;
	spread.firstIndex = currentIndex;
	spread.secondIndex = currentIndex + 1;
	spread.canvasWidth = currentWidth + nextWidth;
	spread.canvasHeight = canvasHeight;
	if (modes.mangaReadingOrder) {
		spread.currentPage = {nextWidth, 0, currentWidth, canvasHeight};
		spread.nextPage = {0, 0, nextWidth, canvasHeight};
	} else {
		spread.currentPage = {0, 0, currentWidth, canvasHeight};
		spread.nextPage = {currentWidth, 0, nextWidth, canvasHeight};
	}
	return spread;
}

int DoublePageNavigationStep(int direction, std::size_t currentIndex,
	std::size_t pageCount, bool enabled,
	const std::optional<PageDimensions>& current,
	const std::optional<PageDimensions>& next,
	const std::optional<PageDimensions>& previousSpreadFirst,
	const std::optional<PageDimensions>& previousSpreadSecond,
	bool coverSingle) {
	if (!enabled || pageCount < 2) return 1;
	if (direction > 0) {
		if (current.has_value() && next.has_value() &&
			IsDoublePagePair(currentIndex, pageCount, *current, *next, coverSingle)) return 2;
	} else if (direction < 0 && currentIndex >= 2 &&
		previousSpreadFirst.has_value() && previousSpreadSecond.has_value() &&
		IsDoublePagePair(currentIndex - 2, pageCount, *previousSpreadFirst,
			*previousSpreadSecond, coverSingle)) {
		return 2;
	}
	return 1;
}

int LogicalDirectionForPhysicalKey(int physicalDirection, bool mangaReadingOrder,
	bool invertLeftRightInMangaMode) {
	if (physicalDirection != -1 && physicalDirection != 1) return 0;
	return mangaReadingOrder && invertLeftRightInMangaMode ?
		-physicalDirection : physicalDirection;
}

} // namespace jpegview_linux
