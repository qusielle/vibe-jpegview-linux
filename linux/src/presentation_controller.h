#pragma once

#include "archive_source.h"
#include "double_page_model.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace jpegview_linux {

struct PresentationNavigationState {
	std::size_t selectedIndex = 0;
	bool selectedImageCommitted = false;
	bool currentHeaderPending = false;
	bool spreadActive = false;
	bool anchorTextureReady = false;
	bool partnerTextureReady = false;
};

struct AnimationFramePresentationIdentity {
	SourceKey source;
	std::uint64_t ownerGeneration = 0;
	std::uint64_t documentRevision = 0;
	std::size_t frameIndex = 0;
};

struct AnimationFramePresentationTarget {
	AnimationFramePresentationIdentity owner;
	std::uint64_t targetGeneration = 0;
	std::size_t frameIndex = 0;
	int width = 0;
	int height = 0;
	bool hasTransparency = false;
	std::string requestKey;
	bool ready = false;
};

// Tracks a committed animation frame separately from the frame whose display
// preparation is in flight. It owns only value identities, never pixels or
// renderer resources.
class AnimationFramePresentationModel {
public:
	void SetCommitted(const AnimationFramePresentationIdentity& identity);
	std::uint64_t BeginTarget(const AnimationFramePresentationIdentity& owner,
		std::size_t frameIndex, int width, int height, bool hasTransparency);
	bool SetTargetRequestKey(std::uint64_t targetGeneration,
		const std::string& requestKey);
	bool MarkTargetReady(std::uint64_t targetGeneration,
		const AnimationFramePresentationIdentity& owner,
		const std::string& requestKey);
	bool CanCommitTarget(std::uint64_t targetGeneration,
		const AnimationFramePresentationIdentity& owner,
		const std::string& requestKey) const;
	bool CommitTarget(std::uint64_t targetGeneration,
		const AnimationFramePresentationIdentity& owner,
		const std::string& requestKey, std::uint64_t documentRevision);
	bool FailTarget(std::uint64_t targetGeneration,
		const AnimationFramePresentationIdentity& owner,
		const std::string& requestKey);
	void CancelTarget();
	const std::optional<AnimationFramePresentationIdentity>& Committed() const {
		return committed_;
	}
	const std::optional<AnimationFramePresentationTarget>& Target() const {
		return target_;
	}

private:
	bool TargetMatches(std::uint64_t targetGeneration,
		const AnimationFramePresentationIdentity& owner,
		const std::string& requestKey) const;
	static bool SameOwner(const AnimationFramePresentationIdentity& left,
		const AnimationFramePresentationIdentity& right);

	std::optional<AnimationFramePresentationIdentity> committed_;
	std::optional<AnimationFramePresentationTarget> target_;
	std::uint64_t nextTargetGeneration_ = 0;
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
	bool CanPresentSelection(const PresentationNavigationState& state) const;
	bool CanRepeatNavigation(const PresentationNavigationState& state) const;
};

} // namespace jpegview_linux
