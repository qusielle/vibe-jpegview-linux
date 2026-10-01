#include "double_page_model.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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

SpreadPagePlacement RotatePlacement(const SpreadPagePlacement& page,
	int canvasWidth, int canvasHeight, int clockwiseQuarterTurns) {
	switch (clockwiseQuarterTurns) {
	case 1:
		return {canvasHeight - page.y - page.height, page.x, page.height, page.width};
	case 2:
		return {canvasWidth - page.x - page.width,
			canvasHeight - page.y - page.height, page.width, page.height};
	case 3:
		return {page.y, canvasWidth - page.x - page.width, page.height, page.width};
	default:
		return page;
	}
}

} // namespace

bool CanAnchorDoublePageSpread(std::size_t firstIndex, std::size_t pageCount,
	const PageDimensions& first, bool coverSingle) {
	if (pageCount < 2 || firstIndex >= pageCount - 1 ||
		(coverSingle && firstIndex == 0)) return false;
	return IsPortrait(first);
}

bool IsDoublePagePair(std::size_t firstIndex, std::size_t pageCount,
	const PageDimensions& first, const PageDimensions& second, bool coverSingle) {
	return CanAnchorDoublePageSpread(firstIndex, pageCount, first, coverSingle) &&
		IsPortrait(second);
}

std::optional<DoublePageSpread> BuildDoublePageSpread(std::size_t currentIndex,
	std::size_t pageCount, const PageDimensions& current,
	const std::optional<PageDimensions>& next, const DoublePageModeState& modes,
	bool coverSingle, int clockwiseQuarterTurns) {
	if (!modes.enabled || !next.has_value() ||
		!IsDoublePagePair(currentIndex, pageCount, current, *next, coverSingle)) {
		return std::nullopt;
	}
	clockwiseQuarterTurns %= 4;
	if (clockwiseQuarterTurns < 0) clockwiseQuarterTurns += 4;
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
	spread.clockwiseQuarterTurns = clockwiseQuarterTurns;
	if (clockwiseQuarterTurns != 0) {
		const int originalCanvasWidth = spread.canvasWidth;
		const int originalCanvasHeight = spread.canvasHeight;
		spread.currentPage = RotatePlacement(spread.currentPage, originalCanvasWidth,
			originalCanvasHeight, clockwiseQuarterTurns);
		spread.nextPage = RotatePlacement(spread.nextPage, originalCanvasWidth,
			originalCanvasHeight, clockwiseQuarterTurns);
		if ((clockwiseQuarterTurns & 1) != 0) {
			spread.canvasWidth = originalCanvasHeight;
			spread.canvasHeight = originalCanvasWidth;
		}
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

void DoublePagePresentationModel::Reset(std::size_t anchorIndex,
	std::size_t partnerIndex, DoublePagePresentationPhase phase) {
	++generation_;
	phase_ = phase;
	anchorIndex_ = anchorIndex;
	partnerIndex_ = partnerIndex;
	anchorTextureKey_.clear();
	partnerTextureKey_.clear();
	anchorReady_ = false;
	partnerReady_ = false;
	spreadPresented_ = false;
}

void DoublePagePresentationModel::AwaitDimensions(std::size_t anchorIndex,
	std::size_t partnerIndex) {
	if (phase_ == DoublePagePresentationPhase::AwaitingDimensions &&
		anchorIndex_ == anchorIndex && partnerIndex_ == partnerIndex) return;
	Reset(anchorIndex, partnerIndex, DoublePagePresentationPhase::AwaitingDimensions);
}

void DoublePagePresentationModel::UseSinglePage(std::size_t anchorIndex) {
	if (phase_ == DoublePagePresentationPhase::SinglePage &&
		anchorIndex_ == anchorIndex) return;
	Reset(anchorIndex, anchorIndex, DoublePagePresentationPhase::SinglePage);
}

bool DoublePagePresentationModel::BeginSpread(std::size_t anchorIndex,
	std::size_t partnerIndex, std::string anchorTextureKey,
	std::string partnerTextureKey) {
	if (anchorTextureKey.empty() || partnerTextureKey.empty() ||
		anchorTextureKey == partnerTextureKey) return false;
	if ((phase_ == DoublePagePresentationPhase::PreparingSpread ||
		phase_ == DoublePagePresentationPhase::ReadySpread ||
		phase_ == DoublePagePresentationPhase::FailedSpread) &&
		anchorIndex_ == anchorIndex && partnerIndex_ == partnerIndex &&
		anchorTextureKey_ == anchorTextureKey && partnerTextureKey_ == partnerTextureKey) {
		return false;
	}
	Reset(anchorIndex, partnerIndex, DoublePagePresentationPhase::PreparingSpread);
	anchorTextureKey_ = std::move(anchorTextureKey);
	partnerTextureKey_ = std::move(partnerTextureKey);
	return true;
}

bool DoublePagePresentationModel::MarkTextureReady(const std::string& textureKey) {
	if (phase_ != DoublePagePresentationPhase::PreparingSpread &&
		phase_ != DoublePagePresentationPhase::ReadySpread) return false;
	if (textureKey == anchorTextureKey_) anchorReady_ = true;
	else if (textureKey == partnerTextureKey_) partnerReady_ = true;
	else return false;
	if (anchorReady_ && partnerReady_) phase_ = DoublePagePresentationPhase::ReadySpread;
	return true;
}

bool DoublePagePresentationModel::MarkTextureFailed(const std::string& textureKey) {
	if ((phase_ != DoublePagePresentationPhase::PreparingSpread &&
		phase_ != DoublePagePresentationPhase::ReadySpread) ||
		(textureKey != anchorTextureKey_ && textureKey != partnerTextureKey_)) return false;
	phase_ = DoublePagePresentationPhase::FailedSpread;
	anchorReady_ = false;
	partnerReady_ = false;
	return true;
}

bool DoublePagePresentationModel::MarkSpreadPresented(std::size_t anchorIndex) {
	if (phase_ != DoublePagePresentationPhase::ReadySpread ||
		anchorIndex_ != anchorIndex) return false;
	spreadPresented_ = true;
	return true;
}

bool DoublePagePresentationModel::SpreadFailed(std::size_t anchorIndex,
	const std::string& anchorTextureKey, const std::string& partnerTextureKey) const {
	return phase_ == DoublePagePresentationPhase::FailedSpread &&
		anchorIndex_ == anchorIndex && anchorTextureKey_ == anchorTextureKey &&
		partnerTextureKey_ == partnerTextureKey;
}

bool DoublePagePresentationModel::SuppressSinglePage(std::size_t anchorIndex) const {
	return anchorIndex_ == anchorIndex &&
		(phase_ == DoublePagePresentationPhase::AwaitingDimensions ||
			phase_ == DoublePagePresentationPhase::PreparingSpread);
}

bool DoublePagePresentationModel::SpreadReady(std::size_t anchorIndex) const {
	return anchorIndex_ == anchorIndex &&
		phase_ == DoublePagePresentationPhase::ReadySpread;
}

bool DoublePagePresentationModel::SpreadPresented(std::size_t anchorIndex) const {
	return anchorIndex_ == anchorIndex && spreadPresented_;
}

} // namespace jpegview_linux
