#pragma once

#include "double_page_model.h"

namespace jpegview_linux {

struct PresentationNavigationState {
	std::size_t selectedIndex = 0;
	bool selectedImageCommitted = false;
	bool currentHeaderPending = false;
	bool spreadActive = false;
	bool anchorTextureReady = false;
	bool partnerTextureReady = false;
};

enum class SpreadPreparationAction {
	UseSinglePage,
	AwaitDimensions,
	CapturePageDimensions,
	UseSinglePageAfterPartnerFailure,
	PrepareSpread,
};

struct SpreadPreparationSnapshot {
	std::size_t selectedIndex = 0;
	std::size_t pageCount = 0;
	bool selectedImageCommitted = false;
	bool cacheAvailable = false;
	bool anchorPixelsAvailable = false;
	bool pageDimensionsCaptured = false;
	std::optional<PageDimensions> anchorDimensions;
	std::optional<PageDimensions> partnerDimensions;
	bool partnerDecodeFailed = false;
	int anchorRotationQuarterTurns = 0;
	DoublePageModeState modes;
};

struct SpreadPreparationDecision {
	SpreadPreparationAction action = SpreadPreparationAction::UseSinglePage;
	std::optional<DoublePageSpread> layout;
	bool preserveDeferredDisplay = false;
};

enum class SpreadRequestAction {
	ReturnToSinglePage,
	KeepFailedSpreadDeferred,
	StartSpreadRequests,
};

struct SpreadRequestSnapshot {
	bool partnerRequestAvailable = false;
	std::size_t anchorTextureBytes = 0;
	std::size_t partnerTextureBytes = 0;
	std::size_t cacheBudgetBytes = 0;
	bool pairPreviouslyFailed = false;
};

struct SpreadRequestDecision {
	SpreadRequestAction action = SpreadRequestAction::ReturnToSinglePage;
	bool preserveDeferredDisplay = false;
};

// Adds presentation-level decisions to the existing pure spread state model.
// The renderer reports texture readiness and frame presentation as values; it
// never passes SDL objects into this controller.
class PresentationController : public DoublePagePresentationModel {
public:
	SpreadPreparationDecision PlanSpreadPreparation(
		const SpreadPreparationSnapshot& snapshot) const;
	SpreadRequestDecision PlanSpreadRequests(
		const SpreadRequestSnapshot& snapshot) const;
	bool AcknowledgeFramePresented(std::size_t selectedIndex,
		bool spreadTexturesReady);
	bool CanRepeatNavigation(const PresentationNavigationState& state) const;
};

} // namespace jpegview_linux
