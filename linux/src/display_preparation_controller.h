#pragma once

#include "display_prefetch_planner.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace jpegview_linux {

inline constexpr std::size_t kMaximumDisplayPreparationCompletions = 512;

// A worker callback publishes only an owned request value. The SDL adapter
// drains it on the event thread and applies it to DisplayImageCache there.
class DisplayPreparationRequestChannel {
public:
	bool Publish(std::uint64_t generation, DisplayImageRequest request);
	std::vector<DisplayImageRequest> Take(std::uint64_t generation);
	void Deactivate(std::uint64_t generation);
	void Shutdown();

private:
	friend class DisplayPreparationController;
	void Activate(std::uint64_t generation);

	std::mutex mutex_;
	std::vector<DisplayImageRequest> ready_;
	std::uint64_t activeGeneration_ = 0;
	bool active_ = false;
	bool shutdown_ = false;
};

// Owns planner lifetime, request generations, and a bounded completion channel.
// Worker callbacks retain the channel as a service object rather than retaining
// a raw cache pointer or calling back into the Viewer.
class DisplayPreparationController {
public:
	explicit DisplayPreparationController(
		DisplayPrefetchPlannerWorker::DimensionsReader reader = {});
	~DisplayPreparationController();
	DisplayPreparationController(const DisplayPreparationController&) = delete;
	DisplayPreparationController& operator=(const DisplayPreparationController&) = delete;

	std::uint64_t RequestPlan(DisplayPrefetchPlannerRequest request);
	void CancelPlan();
	std::vector<DisplayPrefetchPlannerResult> TakePlanResults();
	bool WaitUntilPlanIdle(std::chrono::milliseconds timeout);
	std::uint64_t ActivePlanGeneration() const { return activePlanGeneration_; }
	void StopPlanWorker();

	std::uint64_t AdvanceViewportRevision();
	std::uint64_t ViewportRevision() const { return viewportRevision_; }
	std::uint64_t BeginRequestBatch();
	void CancelRequestBatch(std::uint64_t generation);
	std::vector<DisplayImageRequest> TakeRequestBatch(std::uint64_t generation);
	std::shared_ptr<DisplayPreparationRequestChannel> RequestChannel() const {
		return requestChannel_;
	}
	void Shutdown();

private:
	DisplayPrefetchPlannerWorker plannerWorker_;
	std::shared_ptr<DisplayPreparationRequestChannel> requestChannel_;
	std::uint64_t activePlanGeneration_ = 0;
	std::uint64_t viewportRevision_ = 0;
	std::uint64_t requestBatchGeneration_ = 0;
	bool shutdown_ = false;
};

} // namespace jpegview_linux
