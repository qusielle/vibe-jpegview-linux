#pragma once

#include "image_transform_geometry.h"

#include <cstdint>
#include <string>

namespace jpegview_linux {

struct PerspectiveCorrectionDialogParameters {
	PerspectiveCorrectionParameters transform;
	bool showGrid = true;
};

// Owns the editable parameters and revision identity for one perspective
// correction dialog. SDL input, preview pixels, and document ownership remain
// with the Viewer.
class PerspectiveCorrectionDialogController {
public:
	enum class Phase {
		Closed,
		Editing,
		Applying,
	};

	bool Open(int sourceWidth, int sourceHeight);
	void Close();
	bool SetLeftDeltaFraction(double fraction);
	bool SetRightDeltaFraction(double fraction);
	bool NudgeLeftDelta(double deltaFraction);
	bool NudgeRightDelta(double deltaFraction);
	bool SetAutoCrop(bool enabled);
	bool SetPreserveAspectRatio(bool enabled);
	bool SetGridVisible(bool visible);
	bool BeginApply();
	void ResumeEditing(std::string message = {});
	void CompleteApply();
	void SetMessage(std::string message);

	bool IsOpen() const { return phase_ != Phase::Closed; }
	bool IsApplying() const { return phase_ == Phase::Applying; }
	Phase CurrentPhase() const { return phase_; }
	int SourceWidth() const { return sourceWidth_; }
	int SourceHeight() const { return sourceHeight_; }
	const PerspectiveCorrectionDialogParameters& Parameters() const { return parameters_; }
	const std::string& Message() const { return message_; }
	std::uint64_t SessionId() const { return sessionId_; }
	std::uint64_t PreviewRevision() const { return previewRevision_; }
	bool MatchesPreview(std::uint64_t sessionId, std::uint64_t revision) const;

private:
	bool CanEdit() const { return phase_ == Phase::Editing; }
	bool SetFraction(double fraction, double& current);
	void AdvancePreviewRevision();

	Phase phase_ = Phase::Closed;
	int sourceWidth_ = 0;
	int sourceHeight_ = 0;
	PerspectiveCorrectionDialogParameters parameters_;
	std::string message_;
	std::uint64_t nextSessionId_ = 1;
	std::uint64_t sessionId_ = 0;
	std::uint64_t previewRevision_ = 0;
};

} // namespace jpegview_linux
