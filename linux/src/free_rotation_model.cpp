#include "free_rotation_model.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr double kMinimumRotationDegrees = -180.0;
constexpr double kMaximumRotationDegrees = 180.0;
constexpr double kAngleEqualityTolerance = 1e-9;

} // namespace

bool FreeRotationDialogController::Open(int sourceWidth, int sourceHeight) {
	if (sourceWidth <= 0 || sourceHeight <= 0) return false;
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

void FreeRotationDialogController::Close() {
	phase_ = Phase::Closed;
	message_.clear();
}

bool FreeRotationDialogController::SetAngleDegrees(double degrees) {
	if (!CanEdit() || !std::isfinite(degrees)) return false;
	degrees = std::clamp(degrees, kMinimumRotationDegrees, kMaximumRotationDegrees);
	if (std::abs(parameters_.clockwiseDegrees - degrees) <= kAngleEqualityTolerance) {
		return false;
	}
	parameters_.clockwiseDegrees = degrees;
	message_.clear();
	AdvancePreviewRevision();
	return true;
}

bool FreeRotationDialogController::NudgeAngle(double deltaDegrees) {
	if (!CanEdit() || !std::isfinite(deltaDegrees)) return false;
	return SetAngleDegrees(parameters_.clockwiseDegrees + deltaDegrees);
}

bool FreeRotationDialogController::SetAutoCrop(bool enabled) {
	if (!CanEdit() || parameters_.autoCrop == enabled) return false;
	parameters_.autoCrop = enabled;
	message_.clear();
	AdvancePreviewRevision();
	return true;
}

bool FreeRotationDialogController::SetPreserveAspectRatio(bool enabled) {
	if (!CanEdit() || parameters_.preserveAspectRatio == enabled) return false;
	parameters_.preserveAspectRatio = enabled;
	message_.clear();
	AdvancePreviewRevision();
	return true;
}

bool FreeRotationDialogController::SetGridVisible(bool visible) {
	if (!CanEdit() || parameters_.showGrid == visible) return false;
	parameters_.showGrid = visible;
	message_.clear();
	AdvancePreviewRevision();
	return true;
}

bool FreeRotationDialogController::BeginApply() {
	if (!CanEdit()) return false;
	phase_ = Phase::Applying;
	message_ = "Applying rotation";
	return true;
}

void FreeRotationDialogController::ResumeEditing(std::string message) {
	if (phase_ != Phase::Applying) return;
	phase_ = Phase::Editing;
	message_ = std::move(message);
}

void FreeRotationDialogController::CompleteApply() {
	if (phase_ == Phase::Applying) Close();
}

void FreeRotationDialogController::SetMessage(std::string message) {
	if (IsOpen()) message_ = std::move(message);
}

bool FreeRotationDialogController::MatchesPreview(std::uint64_t sessionId,
	std::uint64_t revision) const {
	return phase_ == Phase::Editing && sessionId_ == sessionId &&
		previewRevision_ == revision;
}

void FreeRotationDialogController::AdvancePreviewRevision() {
	++previewRevision_;
	if (previewRevision_ == 0) previewRevision_ = 1;
}

} // namespace jpegview_linux
