#include "display_preparation_controller.h"

#include <limits>
#include <utility>

namespace jpegview_linux {

void DisplayPreparationRequestChannel::Activate(std::uint64_t generation) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (shutdown_) return;
	activeGeneration_ = generation;
	active_ = generation != 0;
	ready_.clear();
}

bool DisplayPreparationRequestChannel::Publish(std::uint64_t generation,
	DisplayImageRequest request) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (shutdown_ || !active_ || generation == 0 ||
		generation != activeGeneration_ ||
		ready_.size() >= kMaximumDisplayPreparationCompletions) return false;
	ready_.push_back(std::move(request));
	return true;
}

std::vector<DisplayImageRequest> DisplayPreparationRequestChannel::Take(
	std::uint64_t generation) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (shutdown_ || !active_ || generation == 0 ||
		generation != activeGeneration_) return {};
	std::vector<DisplayImageRequest> ready;
	ready.swap(ready_);
	return ready;
}

void DisplayPreparationRequestChannel::Deactivate(std::uint64_t generation) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (generation == 0 || generation != activeGeneration_) return;
	active_ = false;
	activeGeneration_ = 0;
	ready_.clear();
}

void DisplayPreparationRequestChannel::Shutdown() {
	std::lock_guard<std::mutex> lock(mutex_);
	shutdown_ = true;
	active_ = false;
	activeGeneration_ = 0;
	ready_.clear();
}

DisplayPreparationController::DisplayPreparationController(
	DisplayPrefetchPlannerWorker::DimensionsReader reader)
	: plannerWorker_(std::move(reader)),
	  requestChannel_(std::make_shared<DisplayPreparationRequestChannel>()) {}

DisplayPreparationController::~DisplayPreparationController() {
	Shutdown();
}

std::uint64_t DisplayPreparationController::RequestPlan(
	DisplayPrefetchPlannerRequest request) {
	if (shutdown_) return 0;
	activePlanGeneration_ = plannerWorker_.Request(std::move(request));
	return activePlanGeneration_;
}

void DisplayPreparationController::CancelPlan() {
	activePlanGeneration_ = 0;
	plannerWorker_.Cancel();
}

std::vector<DisplayPrefetchPlannerResult>
DisplayPreparationController::TakePlanResults() {
	return plannerWorker_.TakeReady();
}

bool DisplayPreparationController::WaitUntilPlanIdle(
	std::chrono::milliseconds timeout) {
	return plannerWorker_.WaitUntilIdle(timeout);
}

void DisplayPreparationController::StopPlanWorker() {
	activePlanGeneration_ = 0;
	plannerWorker_.Stop();
}

std::uint64_t DisplayPreparationController::AdvanceViewportRevision() {
	if (viewportRevision_ == std::numeric_limits<std::uint64_t>::max()) {
		viewportRevision_ = 1;
	} else {
		++viewportRevision_;
	}
	return viewportRevision_;
}

std::uint64_t DisplayPreparationController::BeginRequestBatch() {
	if (shutdown_) return 0;
	if (requestBatchGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
		requestBatchGeneration_ = 1;
	} else {
		++requestBatchGeneration_;
	}
	requestChannel_->Activate(requestBatchGeneration_);
	return requestBatchGeneration_;
}

void DisplayPreparationController::CancelRequestBatch(std::uint64_t generation) {
	requestChannel_->Deactivate(generation);
}

std::vector<DisplayImageRequest> DisplayPreparationController::TakeRequestBatch(
	std::uint64_t generation) {
	return requestChannel_->Take(generation);
}

void DisplayPreparationController::Shutdown() {
	if (shutdown_) return;
	shutdown_ = true;
	activePlanGeneration_ = 0;
	requestChannel_->Shutdown();
	plannerWorker_.Stop();
}

} // namespace jpegview_linux
