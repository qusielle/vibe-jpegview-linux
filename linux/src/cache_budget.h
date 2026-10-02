#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace jpegview_linux {

inline constexpr std::size_t kDefaultCacheSizeMiB = 1024;
inline constexpr std::size_t kMaximumCacheSizeMiB = 64 * 1024;

std::size_t CacheBytesFromMiB(std::size_t mebibytes);

enum class CacheMemoryCategory {
	RetainedDecodedPixels,
	RetainedPreparedFrames,
	RetainedImageTextures,
	TemporaryUploadStaging,
	ActiveWorkingData
};

enum class CacheProtectionTier {
	DistantSpeculation,
	Neighbor,
	Active
};

struct CacheBudgetSnapshot {
	std::uint64_t releaseRevision = 0;
	std::uint64_t retainedCapacityRevision = 0;
	std::size_t capacityBytes = 0;
	std::size_t retainedBytes = 0;
	std::size_t decodedPixelBytes = 0;
	std::size_t preparedFrameBytes = 0;
	std::size_t imageTextureBytes = 0;
	std::size_t uploadStagingBytes = 0;
	std::size_t activeWorkingBytes = 0;
};

namespace detail {
struct CacheBudgetState;
struct CacheBudgetCharge;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
enum class CacheBudgetTestHookPoint {
	ReservationResetUnlocked,
	ExistingChargeLocked
};
using CacheBudgetTestHook = void (*)(CacheBudgetTestHookPoint, void*);
#endif
}

// Move-only ownership of one accounted allocation. ShareAlias makes another
// move-only handle to the same allocation without charging its bytes twice.
class CacheReservation {
public:
	CacheReservation() = default;
	~CacheReservation();

	CacheReservation(const CacheReservation&) = delete;
	CacheReservation& operator=(const CacheReservation&) = delete;
	CacheReservation(CacheReservation&& other) noexcept;
	CacheReservation& operator=(CacheReservation&& other) noexcept;

	explicit operator bool() const;
	std::size_t Bytes() const;
	CacheMemoryCategory Category() const;
	CacheReservation ShareAlias() const;
	bool Reclassify(CacheMemoryCategory category);
	// A cache entry that moves into retirement is no longer a retained owner,
	// but its charge stays in its current category until another owner transfers it.
	void RelinquishRetainedOwnership();
	// Transfer an ownerless allocation without changing its single shared charge.
	bool ReclassifyIfNoRetainedOwners(CacheMemoryCategory category);
	// Active decoded owners can share their reservation with cache aliases. Their
	// source pixels are working data while the selected image still needs them.
	bool ReclassifyForActiveUse();
	void Reset();

private:
	struct AdoptHandle {};
	CacheReservation(std::shared_ptr<detail::CacheBudgetCharge> charge, AdoptHandle,
		bool retainedOwner = false);
	std::shared_ptr<detail::CacheBudgetCharge> charge_;
	bool retainedOwner_ = false;

	friend class SharedCacheBudget;
};

// One configured limit covers retained decoded pixels, prepared frames, and
// renderer textures. Temporary upload staging and active working data are
// reported separately and do not reduce retained-cache capacity.
class SharedCacheBudget {
public:
	explicit SharedCacheBudget(std::size_t capacityBytes);
	~SharedCacheBudget();

	SharedCacheBudget(const SharedCacheBudget&) = delete;
	SharedCacheBudget& operator=(const SharedCacheBudget&) = delete;

	CacheReservation TryReserve(std::size_t bytes, CacheMemoryCategory category,
		std::shared_ptr<const void> allocationOwner = {});
	CacheReservation TrackTemporary(std::size_t bytes, CacheMemoryCategory category,
		std::shared_ptr<const void> allocationOwner = {});
	// All caches sharing this budget register a retired allocation here. One
	// worker owns the sole-reference wait and releases attached charges with it.
	void RetireAllocation(std::shared_ptr<const void> allocation,
		CacheReservation reservation = {});
	void SetCapacity(std::size_t capacityBytes);

	std::size_t Capacity() const;
	std::size_t Used() const;
	std::size_t Available() const;
	std::uint64_t ReleaseRevision() const;
	std::uint64_t RetainedCapacityRevision() const;
	CacheBudgetSnapshot Snapshot() const;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	void SetTestHookForTesting(detail::CacheBudgetTestHook hook, void* context);
#endif

private:
	CacheReservation ReserveInternal(std::size_t bytes, CacheMemoryCategory category,
		std::shared_ptr<const void> allocationOwner, bool retained);
	std::shared_ptr<detail::CacheBudgetState> state_;
	std::thread retirementWorker_;
};

} // namespace jpegview_linux
