#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace jpegview_linux {

enum class FrameInvalidationReason : std::uint32_t {
	None = 0,
	Input = 1u << 0,
	Viewport = 1u << 1,
	ImageResource = 1u << 2,
	Overlay = 1u << 3,
	Dialog = 1u << 4,
	Animation = 1u << 5,
	Transition = 1u << 6,
	WindowExposure = 1u << 7,
};

constexpr std::uint32_t FrameInvalidationBit(FrameInvalidationReason reason) {
	return static_cast<std::uint32_t>(reason);
}

// Event-thread-only dirty state. Reasons accumulate until a frame is built,
// which makes idle presentation an explicit no-op instead of a polling draw.
class FrameInvalidator {
public:
	void Mark(FrameInvalidationReason reason) {
		reasons_ |= FrameInvalidationBit(reason);
	}
	bool NeedsRender() const { return reasons_ != 0; }
	std::uint32_t PendingReasons() const { return reasons_; }
	std::uint32_t Consume() {
		const std::uint32_t reasons = reasons_;
		reasons_ = 0;
		return reasons;
	}

private:
	std::uint32_t reasons_ = 0;
};

// Worker threads use one notification event for all completion queues. The
// callback only wakes the event loop; payload ownership stays in each queue.
class CoalescedCompletionWakeup {
public:
	using PostFunction = std::function<bool()>;

	void SetPostFunction(PostFunction post);
	bool Notify() noexcept;
	void Consume() noexcept;
	bool Pending() const noexcept { return pending_.load(std::memory_order_acquire); }

private:
	mutable std::mutex mutex_;
	std::condition_variable postsIdle_;
	PostFunction post_;
	std::size_t activePosts_ = 0;
	std::atomic<bool> pending_{false};
};

CoalescedCompletionWakeup& UiCompletionWakeup();

// Returns a bounded wait for SDL_WaitEventTimeout. Deadlines use SDL's
// wrapping 32-bit tick clock and are compared with signed modular arithmetic.
int EventWaitTimeoutMs(std::uint32_t now,
	const std::vector<std::uint32_t>& deadlines, int fallbackMs);
std::optional<std::uint32_t> DisplayUploadContinuationDeadline(
	std::uint32_t now, bool eligibleBandedUpload);

struct MouseMotionSample {
	std::int32_t x = 0;
	std::int32_t y = 0;
	std::int32_t xrel = 0;
	std::int32_t yrel = 0;
	std::uint32_t state = 0;
};

// Only adjacent motion events with the same button state may be merged. The
// caller must stop at every non-motion event so button, key, crop, and modal
// routing boundaries retain their order.
bool CoalesceMouseMotion(MouseMotionSample& accumulated,
	const MouseMotionSample& next);

} // namespace jpegview_linux
