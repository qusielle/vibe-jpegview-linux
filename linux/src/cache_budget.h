#pragma once

#include <cstddef>
#include <memory>

namespace jpegview_linux {

inline constexpr std::size_t kDefaultCacheSizeMiB = 1024;
inline constexpr std::size_t kMaximumCacheSizeMiB = 64 * 1024;

std::size_t CacheBytesFromMiB(std::size_t mebibytes);

// Thread-safe accounting shared by decoded pixels, prepared display frames,
// and renderer textures. Owners reserve retained bytes and release them when
// entries are evicted. In-flight worker buffers are temporary, not cache.
class SharedCacheBudget {
public:
	explicit SharedCacheBudget(std::size_t capacityBytes);
	~SharedCacheBudget();

	bool TryReserve(std::size_t bytes);
	void Release(std::size_t bytes);
	void SetCapacity(std::size_t capacityBytes);

	std::size_t Capacity() const;
	std::size_t Used() const;
	std::size_t Available() const;

private:
	struct State;
	std::unique_ptr<State> state_;
};

} // namespace jpegview_linux
