#pragma once

#include <mutex>
#include <utility>

namespace jpegview_linux {

// Serializes late completion publication with batch cancellation. Once
// Deactivate returns, no callback can enqueue work from this batch.
class WorkBatchGate {
public:
	template <typename Callback>
	bool Publish(Callback&& callback) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!active_) return false;
		std::forward<Callback>(callback)();
		return true;
	}

	void Deactivate() {
		std::lock_guard<std::mutex> lock(mutex_);
		active_ = false;
	}

private:
	std::mutex mutex_;
	bool active_ = true;
};

} // namespace jpegview_linux
