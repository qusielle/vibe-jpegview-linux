#include "presentation_controller.h"

namespace jpegview_linux {

SpreadPreparationDecision PresentationController::PlanSpreadPreparation(
	const SpreadPreparationSnapshot& snapshot) const {
	if (snapshot.pageCount == 0 || snapshot.selectedIndex >= snapshot.pageCount) return {};
	if (!snapshot.selectedImageCommitted) {
		const bool potentialAnchor = snapshot.modes.enabled && snapshot.cacheAvailable &&
			snapshot.selectedIndex > 0 && snapshot.selectedIndex < snapshot.pageCount - 1;
		return {potentialAnchor ? SpreadPreparationAction::AwaitDimensions :
			SpreadPreparationAction::UseSinglePage, std::nullopt, false};
	}
	if (!snapshot.modes.enabled || !snapshot.cacheAvailable ||
		snapshot.selectedIndex == 0 || snapshot.selectedIndex >= snapshot.pageCount - 1 ||
		!snapshot.anchorPixelsAvailable) return {};
	if (!snapshot.pageDimensionsCaptured) {
		return {SpreadPreparationAction::CapturePageDimensions, std::nullopt, false};
	}
	if (!snapshot.anchorDimensions.has_value() ||
		!CanAnchorDoublePageSpread(snapshot.selectedIndex, snapshot.pageCount,
			*snapshot.anchorDimensions, true)) return {};
	if (!snapshot.partnerDimensions.has_value()) {
		return {snapshot.partnerDecodeFailed ?
			SpreadPreparationAction::UseSinglePageAfterPartnerFailure :
			SpreadPreparationAction::AwaitDimensions, std::nullopt,
			snapshot.partnerDecodeFailed};
	}
	const std::optional<DoublePageSpread> layout = BuildDoublePageSpread(
		snapshot.selectedIndex, snapshot.pageCount, *snapshot.anchorDimensions,
		snapshot.partnerDimensions, snapshot.modes, true,
		snapshot.anchorRotationQuarterTurns);
	if (!layout.has_value()) {
		return {SpreadPreparationAction::UseSinglePageAfterPartnerFailure,
			std::nullopt, true};
	}
	return {SpreadPreparationAction::PrepareSpread, layout, false};
}

SpreadRequestDecision PresentationController::PlanSpreadRequests(
	const SpreadRequestSnapshot& snapshot) const {
	if (!snapshot.partnerRequestAvailable) {
		return {SpreadRequestAction::ReturnToSinglePage, true};
	}
	if (snapshot.anchorTextureBytes > snapshot.cacheBudgetBytes ||
		snapshot.partnerTextureBytes >
			snapshot.cacheBudgetBytes - snapshot.anchorTextureBytes) {
		return {SpreadRequestAction::ReturnToSinglePage, false};
	}
	if (snapshot.pairPreviouslyFailed) {
		return {SpreadRequestAction::KeepFailedSpreadDeferred, true};
	}
	return {SpreadRequestAction::StartSpreadRequests, false};
}

bool PresentationController::AcknowledgeFramePresented(
	std::size_t selectedIndex, bool spreadTexturesReady) {
	return spreadTexturesReady && MarkSpreadPresented(selectedIndex);
}

bool PresentationController::CanRepeatNavigation(
	const PresentationNavigationState& state) const {
	if (!state.selectedImageCommitted || state.currentHeaderPending ||
		SuppressSinglePage(state.selectedIndex)) return false;
	if (!state.spreadActive) return true;
	return SpreadReady(state.selectedIndex) && SpreadPresented(state.selectedIndex) &&
		state.anchorTextureReady && state.partnerTextureReady;
}

} // namespace jpegview_linux
