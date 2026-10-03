#pragma once

#include <string>

namespace jpegview_linux {

inline bool IsDisplayTexturePinned(const std::string& key,
	const std::string& transitionKey, const std::string& pendingTransitionKey,
	const std::string& captureKey, const std::string& lastPresentedKey,
	bool lastPresentedMatchesCurrentSource,
	const std::string& activeSpreadAnchorKey = {},
	const std::string& activeSpreadPartnerKey = {}) {
	return !key.empty() && (key == transitionKey || key == pendingTransitionKey ||
		key == captureKey || key == activeSpreadAnchorKey ||
		key == activeSpreadPartnerKey ||
		(key == lastPresentedKey && lastPresentedMatchesCurrentSource));
}

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
