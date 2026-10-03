#include "event_loop_model.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace jpegview_linux {

void CoalescedCompletionWakeup::SetPostFunction(PostFunction post) {
	const bool configured = static_cast<bool>(post);
	{
		std::unique_lock<std::mutex> lock(mutex_);
		post_ = std::move(post);
		if (!configured) postsIdle_.wait(lock, [this] { return activePosts_ == 0; });
	}
	if (!configured) Consume();
}

bool CoalescedCompletionWakeup::Notify() noexcept {
	if (pending_.exchange(true, std::memory_order_acq_rel)) return true;
	PostFunction post;
	try {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!post_) return false;
		post = post_;
		++activePosts_;
	} catch (...) {
		return false;
	}
	bool posted = false;
	try {
		posted = post();
	} catch (...) {
		posted = false;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (activePosts_ > 0 && --activePosts_ == 0) postsIdle_.notify_all();
	}
	// Keep the logical pending bit set when SDL refuses the event. The event
	// loop's finite fallback wait observes it and drains the completion queues.
	return posted;
}

void CoalescedCompletionWakeup::Consume() noexcept {
	pending_.store(false, std::memory_order_release);
}

CoalescedCompletionWakeup& UiCompletionWakeup() {
	static CoalescedCompletionWakeup wakeup;
	return wakeup;
}

int EventWaitTimeoutMs(std::uint32_t now,
	const std::vector<std::uint32_t>& deadlines, int fallbackMs) {
	int timeout = std::max(1, fallbackMs);
	for (const std::uint32_t deadline : deadlines) {
		const std::int32_t remaining = static_cast<std::int32_t>(deadline - now);
		if (remaining <= 0) return 0;
		timeout = std::min(timeout, remaining);
	}
	return timeout;
}

std::optional<std::uint32_t> DisplayUploadContinuationDeadline(
	std::uint32_t now, bool eligibleBandedUpload) {
	if (!eligibleBandedUpload) return std::nullopt;
	return now + 8u;
}

namespace {

std::int32_t SaturatingAdd(std::int32_t left, std::int32_t right) {
	const std::int64_t sum = static_cast<std::int64_t>(left) + right;
	return static_cast<std::int32_t>(std::clamp(sum,
		static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
		static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

} // namespace

bool CoalesceMouseMotion(MouseMotionSample& accumulated,
	const MouseMotionSample& next) {
	if (accumulated.state != next.state) return false;
	accumulated.x = next.x;
	accumulated.y = next.y;
	accumulated.xrel = SaturatingAdd(accumulated.xrel, next.xrel);
	accumulated.yrel = SaturatingAdd(accumulated.yrel, next.yrel);
	return true;
}

} // namespace jpegview_linux
