#include "perspective_correction_model.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr double kFractionEqualityTolerance = 1e-12;

} // namespace

bool PerspectiveCorrectionDialogController::Open(int sourceWidth, int sourceHeight) {
	ImageTransformGeometry geometry;
	if (!BuildPerspectiveGeometry(sourceWidth, sourceHeight,
		PerspectiveCorrectionParameters{}, geometry)) return false;
	sourceWidth_ = sourceWidth;
	sourceHeight_ = sourceHeight;
	parameters_ = {};
	message_.clear();
	phase_ = Phase::Editing;
	sessionId_ = nextSessionId_++;
	if (nextSessionId_ == 0) nextSessionId_ = 1;
	previewRevision_ = 1;
	return true;
}

void PerspectiveCorrectionDialogController::Close() {
	phase_ = Phase::Closed;
	message_.clear();
}

bool PerspectiveCorrectionDialogController::SetLeftDeltaFraction(double fraction) {
	return SetFraction(fraction, parameters_.transform.leftDeltaFraction);
}

bool PerspectiveCorrectionDialogController::SetRightDeltaFraction(double fraction) {
	return SetFraction(fraction, parameters_.transform.rightDeltaFraction);
}

bool PerspectiveCorrectionDialogController::NudgeLeftDelta(double deltaFraction) {
	if (!CanEdit() || !std::isfinite(deltaFraction)) return false;
	return SetLeftDeltaFraction(parameters_.transform.leftDeltaFraction + deltaFraction);
}

bool PerspectiveCorrectionDialogController::NudgeRightDelta(double deltaFraction) {
	if (!CanEdit() || !std::isfinite(deltaFraction)) return false;
	return SetRightDeltaFraction(parameters_.transform.rightDeltaFraction + deltaFraction);
}

bool PerspectiveCorrectionDialogController::SetAutoCrop(bool enabled) {
	if (!CanEdit() || parameters_.transform.autoCrop == enabled) return false;
	parameters_.transform.autoCrop = enabled;
	message_.clear();
	AdvancePreviewRevision();
	return true;
}

bool PerspectiveCorrectionDialogController::SetPreserveAspectRatio(bool enabled) {
	if (!CanEdit() || parameters_.transform.preserveAspectRatio == enabled) return false;
	parameters_.transform.preserveAspectRatio = enabled;
	message_.clear();
	AdvancePreviewRevision();
	return true;
}

bool PerspectiveCorrectionDialogController::SetGridVisible(bool visible) {
	if (!CanEdit() || parameters_.showGrid == visible) return false;
	parameters_.showGrid = visible;
	message_.clear();
	AdvancePreviewRevision();
	return true;
}

bool PerspectiveCorrectionDialogController::BeginApply() {
	if (!CanEdit()) return false;
	phase_ = Phase::Applying;
	message_ = "Applying perspective correction";
	return true;
}

void PerspectiveCorrectionDialogController::ResumeEditing(std::string message) {
	if (phase_ != Phase::Applying) return;
	phase_ = Phase::Editing;
	message_ = std::move(message);
}

void PerspectiveCorrectionDialogController::CompleteApply() {
	if (phase_ == Phase::Applying) Close();
}

void PerspectiveCorrectionDialogController::SetMessage(std::string message) {
	if (IsOpen()) message_ = std::move(message);
}

bool PerspectiveCorrectionDialogController::MatchesPreview(std::uint64_t sessionId,
	std::uint64_t revision) const {
	return phase_ == Phase::Editing && sessionId_ == sessionId &&
		previewRevision_ == revision;
}

bool PerspectiveCorrectionDialogController::SetFraction(double fraction,
	double& current) {
	if (!CanEdit() || !std::isfinite(fraction)) return false;
	fraction = std::clamp(fraction, -kMaximumPerspectiveCorrectionFraction,
		kMaximumPerspectiveCorrectionFraction);
	if (std::abs(current - fraction) <= kFractionEqualityTolerance) return false;
	current = fraction;
	message_.clear();
	AdvancePreviewRevision();
	return true;
}

void PerspectiveCorrectionDialogController::AdvancePreviewRevision() {
	++previewRevision_;
	if (previewRevision_ == 0) previewRevision_ = 1;
}

} // namespace jpegview_linux
