#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

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
	int clockwiseQuarterTurns = 0;
	SpreadPagePlacement currentPage;
	SpreadPagePlacement nextPage;
};

// YACReader pairs only two adjacent portrait pages. Page zero is treated as a
// standalone cover by default, so page one starts the first spread.
bool CanAnchorDoublePageSpread(std::size_t firstIndex, std::size_t pageCount,
	const PageDimensions& first, bool coverSingle = true);

bool IsDoublePagePair(std::size_t firstIndex, std::size_t pageCount,
	const PageDimensions& first, const PageDimensions& second,
	bool coverSingle = true);

std::optional<DoublePageSpread> BuildDoublePageSpread(std::size_t currentIndex,
	std::size_t pageCount, const PageDimensions& current,
	const std::optional<PageDimensions>& next, const DoublePageModeState& modes,
	bool coverSingle = true, int clockwiseQuarterTurns = 0);

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

// Physical left/right keys can reverse direction in manga reading mode, whether
// or not two-page display is currently enabled. The inversion preference defaults
// to the existing behavior and can be disabled through viewer settings.
int LogicalDirectionForPhysicalKey(int physicalDirection, bool mangaReadingOrder,
	bool invertLeftRightInMangaMode);

enum class DoublePagePresentationPhase {
	SinglePage,
	AwaitingDimensions,
	PreparingSpread,
	ReadySpread,
	FailedSpread,
};

// A spread is a single presentation unit: while its partner dimensions or
// either final-size frame are pending, the anchor page must not be shown alone.
// Texture creation and ownership remain in the SDL adapter; this model only
// tracks the generation and exact keys that may be published together.
class DoublePagePresentationModel {
public:
	void AwaitDimensions(std::size_t anchorIndex, std::size_t partnerIndex);
	void UseSinglePage(std::size_t anchorIndex);
	bool BeginSpread(std::size_t anchorIndex, std::size_t partnerIndex,
		std::string anchorTextureKey, std::string partnerTextureKey);
	bool MarkTextureReady(const std::string& textureKey);
	bool MarkTextureFailed(const std::string& textureKey);
	bool MarkSpreadPresented(std::size_t anchorIndex);
	bool SpreadFailed(std::size_t anchorIndex, const std::string& anchorTextureKey,
		const std::string& partnerTextureKey) const;

	DoublePagePresentationPhase Phase() const { return phase_; }
	std::uint64_t Generation() const { return generation_; }
	std::size_t AnchorIndex() const { return anchorIndex_; }
	std::size_t PartnerIndex() const { return partnerIndex_; }
	const std::string& AnchorTextureKey() const { return anchorTextureKey_; }
	const std::string& PartnerTextureKey() const { return partnerTextureKey_; }
	bool SuppressSinglePage(std::size_t anchorIndex) const;
	bool SpreadReady(std::size_t anchorIndex) const;
	bool SpreadPresented(std::size_t anchorIndex) const;

private:
	void Reset(std::size_t anchorIndex, std::size_t partnerIndex,
		DoublePagePresentationPhase phase);

	DoublePagePresentationPhase phase_ = DoublePagePresentationPhase::SinglePage;
	std::size_t anchorIndex_ = 0;
	std::size_t partnerIndex_ = 0;
	std::uint64_t generation_ = 0;
	std::string anchorTextureKey_;
	std::string partnerTextureKey_;
	bool anchorReady_ = false;
	bool partnerReady_ = false;
	bool spreadPresented_ = false;
};

} // namespace jpegview_linux
