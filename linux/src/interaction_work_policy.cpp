#include "interaction_work_policy.h"

#include <algorithm>
#include <utility>

namespace jpegview_linux {

bool ForegroundSourceWorkPending(const ForegroundSourceWorkState& state) {
	return state.displayQueued || state.displayActive || state.decodedQueued ||
		state.decodedActive || state.activeSpreadQueued || state.activeSpreadActive ||
		state.spreadPresentationPending;
}

bool InteractionWorkPlan::Allows(PerfWorkClass workClass) const {
	return permittedWorkClasses.find(workClass) != permittedWorkClasses.end();
}

bool InteractionWorkPlan::AllowsThumbnail(std::size_t fileIndex) const {
	return Allows(PerfWorkClass::VisibleThumbnail) &&
		std::find(visibleThumbnailIndices.begin(), visibleThumbnailIndices.end(), fileIndex) !=
			visibleThumbnailIndices.end();
}

InteractionWorkPolicy::InteractionWorkPolicy(Clock clock)
	: clock_(std::move(clock)) {
	if (!clock_) clock_ = [] { return std::chrono::steady_clock::now(); };
}

std::chrono::steady_clock::time_point InteractionWorkPolicy::Now() const {
	return clock_();
}

void InteractionWorkPolicy::NotifyActivity(InteractionActivity activity) {
	(void)activity;
	lastActivity_ = Now();
	hasActivity_ = true;
}

void InteractionWorkPolicy::SetCaptureActive(bool active) {
	if (captureActive_ == active) return;
	captureActive_ = active;
	if (!active) {
		lastActivity_ = Now();
		hasActivity_ = true;
	}
}

InteractionWorkPlan InteractionWorkPolicy::Plan(bool foregroundPending,
	std::vector<std::size_t> visibleThumbnailIndices) const {
	const auto now = Now();
	const bool withinIdleDelay = hasActivity_ &&
		(now < lastActivity_ || now - lastActivity_ < IdleDelay());
	const bool interactionActive = captureActive_ || withinIdleDelay;
	InteractionWorkPlan plan;
	plan.visibleThumbnailIndices = std::move(visibleThumbnailIndices);
	plan.interactionActive = interactionActive;
	plan.foregroundPending = foregroundPending;
	plan.cancelQueuedSpeculation = interactionActive || foregroundPending;
	plan.requestActiveSpeculationCancellation = interactionActive || foregroundPending;
	plan.suspendSpeculativeUploads = interactionActive || foregroundPending;
	plan.permittedWorkClasses.insert(PerfWorkClass::ActiveImageSpread);
	plan.permittedWorkClasses.insert(PerfWorkClass::FocusedPreview);
	if (!foregroundPending) {
		plan.permittedWorkClasses.insert(PerfWorkClass::VisibleThumbnail);
	}
	if (!interactionActive && !foregroundPending) {
		plan.permittedWorkClasses.insert(PerfWorkClass::NearestNavigationNeighbor);
		plan.permittedWorkClasses.insert(PerfWorkClass::DistantSpeculation);
	}
	return plan;
}

std::optional<std::chrono::steady_clock::time_point>
InteractionWorkPolicy::NextIdleDeadline() const {
	if (!hasActivity_ || captureActive_) return std::nullopt;
	const auto deadline = lastActivity_ + IdleDelay();
	return Now() < deadline ? std::optional<std::chrono::steady_clock::time_point>(deadline) :
		std::nullopt;
}

} // namespace jpegview_linux
