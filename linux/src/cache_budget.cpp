#include "cache_budget.h"

#include <algorithm>
#include <limits>
#include <mutex>

namespace jpegview_linux {

std::size_t CacheBytesFromMiB(std::size_t mebibytes) {
	constexpr std::size_t bytesPerMiB = 1024u * 1024u;
	if (mebibytes > std::numeric_limits<std::size_t>::max() / bytesPerMiB) {
		return std::numeric_limits<std::size_t>::max();
	}
	return mebibytes * bytesPerMiB;
}

struct SharedCacheBudget::State {
	explicit State(std::size_t capacityBytes) : capacity(capacityBytes) {}

	mutable std::mutex mutex;
	std::size_t capacity = 0;
	std::size_t used = 0;
};

SharedCacheBudget::SharedCacheBudget(std::size_t capacityBytes)
	: state_(std::make_unique<State>(capacityBytes)) {}

SharedCacheBudget::~SharedCacheBudget() = default;

bool SharedCacheBudget::TryReserve(std::size_t bytes) {
	if (bytes == 0) return true;
	std::lock_guard<std::mutex> lock(state_->mutex);
	if (bytes > state_->capacity - std::min(state_->used, state_->capacity)) return false;
	state_->used += bytes;
	return true;
}

void SharedCacheBudget::Release(std::size_t bytes) {
	std::lock_guard<std::mutex> lock(state_->mutex);
	state_->used -= std::min(bytes, state_->used);
}

void SharedCacheBudget::SetCapacity(std::size_t capacityBytes) {
	std::lock_guard<std::mutex> lock(state_->mutex);
	state_->capacity = capacityBytes;
}

std::size_t SharedCacheBudget::Capacity() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->capacity;
}

std::size_t SharedCacheBudget::Used() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->used;
}

std::size_t SharedCacheBudget::Available() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->capacity > state_->used ? state_->capacity - state_->used : 0;
}

} // namespace jpegview_linux
