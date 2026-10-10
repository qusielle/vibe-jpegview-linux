#include "presentation_controller.h"

namespace jpegview_linux {

bool AnimationFramePresentationModel::SameOwner(
	const AnimationFramePresentationIdentity& left,
	const AnimationFramePresentationIdentity& right) {
	return left.source == right.source &&
		left.ownerGeneration == right.ownerGeneration &&
		left.documentRevision == right.documentRevision &&
		left.frameIndex == right.frameIndex;
}

void AnimationFramePresentationModel::SetCommitted(
	const AnimationFramePresentationIdentity& identity) {
	committed_ = identity;
	if (target_.has_value() && !SameOwner(target_->owner, identity)) {
		target_.reset();
	}
}

std::uint64_t AnimationFramePresentationModel::BeginTarget(
	const AnimationFramePresentationIdentity& owner, std::size_t frameIndex,
	int width, int height, bool hasTransparency) {
	if (!committed_.has_value() || !SameOwner(*committed_, owner) ||
		width <= 0 || height <= 0) return 0;
	++nextTargetGeneration_;
	if (nextTargetGeneration_ == 0) ++nextTargetGeneration_;
	target_ = AnimationFramePresentationTarget{owner, nextTargetGeneration_,
		frameIndex, width, height, hasTransparency, {}, false};
	return nextTargetGeneration_;
}

bool AnimationFramePresentationModel::SetTargetRequestKey(
	std::uint64_t targetGeneration, const std::string& requestKey) {
	if (!target_.has_value() || target_->targetGeneration != targetGeneration ||
		requestKey.empty()) return false;
	target_->requestKey = requestKey;
	target_->ready = false;
	return true;
}

bool AnimationFramePresentationModel::TargetMatches(
	std::uint64_t targetGeneration,
	const AnimationFramePresentationIdentity& owner,
	const std::string& requestKey) const {
	return target_.has_value() && committed_.has_value() &&
		target_->targetGeneration == targetGeneration &&
		SameOwner(target_->owner, owner) && SameOwner(*committed_, owner) &&
		!requestKey.empty() && target_->requestKey == requestKey;
}

bool AnimationFramePresentationModel::MarkTargetReady(
	std::uint64_t targetGeneration,
	const AnimationFramePresentationIdentity& owner,
	const std::string& requestKey) {
	if (!TargetMatches(targetGeneration, owner, requestKey)) return false;
	target_->ready = true;
	return true;
}

bool AnimationFramePresentationModel::CanCommitTarget(
	std::uint64_t targetGeneration,
	const AnimationFramePresentationIdentity& owner,
	const std::string& requestKey) const {
	return TargetMatches(targetGeneration, owner, requestKey) && target_->ready;
}

bool AnimationFramePresentationModel::CommitTarget(
	std::uint64_t targetGeneration,
	const AnimationFramePresentationIdentity& owner,
	const std::string& requestKey, std::uint64_t documentRevision) {
	if (!CanCommitTarget(targetGeneration, owner, requestKey)) return false;
	committed_ = AnimationFramePresentationIdentity{owner.source,
		owner.ownerGeneration, documentRevision, target_->frameIndex};
	target_.reset();
	return true;
}

bool AnimationFramePresentationModel::FailTarget(
	std::uint64_t targetGeneration,
	const AnimationFramePresentationIdentity& owner,
	const std::string& requestKey) {
	if (!TargetMatches(targetGeneration, owner, requestKey)) return false;
	target_.reset();
	return true;
}

void AnimationFramePresentationModel::CancelTarget() {
	target_.reset();
}

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

bool PresentationController::CanPresentSelection(
	const PresentationNavigationState& state) const {
	if (!state.selectedImageCommitted || state.currentHeaderPending ||
		SuppressSinglePage(state.selectedIndex) || !state.anchorTextureReady) return false;
	return !state.spreadActive ||
		(SpreadReady(state.selectedIndex) && state.partnerTextureReady);
}

} // namespace jpegview_linux
