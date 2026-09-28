#pragma once

#include <cstddef>
#include <optional>

namespace jpegview_linux {

struct DoublePageModeState {
	bool enabled = false;
	bool mangaReadingOrder = false;
};

struct PageDimensions {
	int width = 0;
	int height = 0;
};

struct SpreadPagePlacement {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct DoublePageSpread {
	std::size_t firstIndex = 0;
	std::size_t secondIndex = 0;
	int canvasWidth = 0;
	int canvasHeight = 0;
	SpreadPagePlacement currentPage;
	SpreadPagePlacement nextPage;
};

// YACReader pairs only two adjacent portrait pages. Page zero is treated as a
// standalone cover by default, so page one starts the first spread.
bool IsDoublePagePair(std::size_t firstIndex, std::size_t pageCount,
	const PageDimensions& first, const PageDimensions& second,
	bool coverSingle = true);

std::optional<DoublePageSpread> BuildDoublePageSpread(std::size_t currentIndex,
	std::size_t pageCount, const PageDimensions& current,
	const std::optional<PageDimensions>& next, const DoublePageModeState& modes,
	bool coverSingle = true);

// Returns the number of source-list entries to move. Forward movement skips
// the visible partner; backward movement skips a preceding spread, matching
// YACReader's anchor-page navigation rule. Unknown neighbor dimensions safely
// fall back to a single entry until their metadata is available.
int DoublePageNavigationStep(int direction, std::size_t currentIndex,
	std::size_t pageCount, bool enabled,
	const std::optional<PageDimensions>& current,
	const std::optional<PageDimensions>& next,
	const std::optional<PageDimensions>& previousSpreadFirst,
	const std::optional<PageDimensions>& previousSpreadSecond,
	bool coverSingle = true);

// Physical left/right keys reverse direction in manga reading mode, whether
// or not two-page display is currently enabled.
int LogicalDirectionForPhysicalKey(int physicalDirection, bool mangaReadingOrder);

} // namespace jpegview_linux
