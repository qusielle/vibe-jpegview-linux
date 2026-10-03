#pragma once

#include "perf_diagnostics.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <vector>

namespace jpegview_linux {

enum class InteractionActivity {
	Pan,
	Zoom,
	Resize,
	CropDrag,
	NavigatorDrag,
	HeldNavigation,
	Wheel,
};

struct ForegroundSourceWorkState {
	bool displayQueued = false;
	bool displayActive = false;
	bool decodedQueued = false;
	bool decodedActive = false;
	bool activeSpreadQueued = false;
	bool activeSpreadActive = false;
	bool spreadPresentationPending = false;
};

bool ForegroundSourceWorkPending(const ForegroundSourceWorkState& state);

struct InteractionWorkPlan {
	std::set<PerfWorkClass> permittedWorkClasses;
	std::vector<std::size_t> visibleThumbnailIndices;
	bool interactionActive = false;
	bool foregroundPending = false;
	bool cancelQueuedSpeculation = false;
	bool requestActiveSpeculationCancellation = false;
	bool suspendSpeculativeUploads = false;

	bool Allows(PerfWorkClass workClass) const;
	bool AllowsThumbnail(std::size_t fileIndex) const;
};

// Keeps low-priority image work paused until interaction has been idle for
// 250 ms. The clock seam lets tests exercise activity bursts without sleeping.
class InteractionWorkPolicy {
public:
	using Clock = std::function<std::chrono::steady_clock::time_point()>;

	explicit InteractionWorkPolicy(Clock clock = {});

	void NotifyActivity(InteractionActivity activity);
	void SetCaptureActive(bool active);
	InteractionWorkPlan Plan(bool foregroundPending,
		std::vector<std::size_t> visibleThumbnailIndices) const;
	std::optional<std::chrono::steady_clock::time_point> NextIdleDeadline() const;

	static constexpr std::chrono::milliseconds IdleDelay() {
		return std::chrono::milliseconds(250);
	}

private:
	std::chrono::steady_clock::time_point Now() const;

	Clock clock_;
	std::chrono::steady_clock::time_point lastActivity_{};
	bool hasActivity_ = false;
	bool captureActive_ = false;
};

} // namespace jpegview_linux
