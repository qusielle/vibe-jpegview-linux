#pragma once

#include <string>
#include <string_view>

namespace jpegview_linux {

// Renderer-thread views of the current owners. Build and consume this snapshot
// synchronously before any key owner mutates; string_view avoids copying keys in
// the per-entry eviction and active-working cleanup paths.
struct DisplayTexturePinSnapshot {
	std::string_view activeTransition;
	std::string_view pendingTransition;
	std::string_view temporaryCapture;
	std::string_view animationTarget;
	std::string_view activeSpreadAnchor;
	std::string_view activeSpreadPartner;
	std::string_view retainedSpreadAnchor;
	std::string_view retainedSpreadPartner;
	std::string_view lastPresented;
	bool lastPresentedMatchesCurrentSource = false;

	bool IsPinned(std::string_view key) const {
		return !key.empty() && (key == activeTransition || key == pendingTransition ||
			key == temporaryCapture || key == animationTarget ||
			key == activeSpreadAnchor || key == activeSpreadPartner ||
			key == retainedSpreadAnchor || key == retainedSpreadPartner ||
			(key == lastPresented && lastPresentedMatchesCurrentSource));
	}

	// These roles were already forced to the active cache tier independently of
	// ordinary request priorities. The animation target remains pinned for cleanup
	// but keeps the tier assigned by its request, matching existing policy.
	template <typename Callback>
	void ForEachAlwaysActiveRole(Callback&& callback) const {
		if (!activeTransition.empty()) callback(activeTransition);
		if (!pendingTransition.empty()) callback(pendingTransition);
		if (!temporaryCapture.empty()) callback(temporaryCapture);
		if (!retainedSpreadAnchor.empty()) callback(retainedSpreadAnchor);
		if (!retainedSpreadPartner.empty()) callback(retainedSpreadPartner);
		if (lastPresentedMatchesCurrentSource && !lastPresented.empty()) {
			callback(lastPresented);
		}
	}
};

// Temporarily keeps a borrowed outgoing display texture alive while the prior
// transition is cleared and ownership is transferred to its replacement.
class DisplayTexturePinHandoff {
public:
	DisplayTexturePinHandoff(std::string& captureKey, const std::string& borrowedKey)
		: captureKey_(captureKey), previousCaptureKey_(captureKey) {
		if (!borrowedKey.empty()) {
			std::string replacementKey(borrowedKey);
			captureKey_.swap(replacementKey);
		}
	}

	DisplayTexturePinHandoff(const DisplayTexturePinHandoff&) = delete;
	DisplayTexturePinHandoff& operator=(const DisplayTexturePinHandoff&) = delete;

	~DisplayTexturePinHandoff() { Restore(); }

	void Restore() {
		if (restored_) return;
		captureKey_.swap(previousCaptureKey_);
		restored_ = true;
	}

private:
	std::string& captureKey_;
	std::string previousCaptureKey_;
	bool restored_ = false;
};

} // namespace jpegview_linux
