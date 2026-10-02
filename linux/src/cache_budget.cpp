#include "cache_budget.h"

#include "perf_diagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace jpegview_linux {
namespace {

constexpr std::size_t CategoryCount = 5;

std::size_t CategoryIndex(CacheMemoryCategory category) {
	return static_cast<std::size_t>(category);
}

bool IsRetained(CacheMemoryCategory category) {
	return category == CacheMemoryCategory::RetainedDecodedPixels ||
		category == CacheMemoryCategory::RetainedPreparedFrames ||
		category == CacheMemoryCategory::RetainedImageTextures;
}

} // namespace

std::size_t CacheBytesFromMiB(std::size_t mebibytes) {
	constexpr std::size_t bytesPerMiB = 1024u * 1024u;
	if (mebibytes > std::numeric_limits<std::size_t>::max() / bytesPerMiB) {
		return std::numeric_limits<std::size_t>::max();
	}
	return mebibytes * bytesPerMiB;
}

namespace detail {

struct CacheBudgetState {
	explicit CacheBudgetState(std::size_t configuredCapacity) : capacity(configuredCapacity) {}
	struct RetiredAllocation {
		std::shared_ptr<const void> allocation;
		std::weak_ptr<const void> identity;
		std::vector<CacheReservation> reservations;
	};

	mutable std::mutex mutex;
	std::size_t capacity = 0;
	std::size_t retainedBytes = 0;
	std::uint64_t releaseRevision = 0;
	std::uint64_t retainedCapacityRevision = 0;
	std::array<std::size_t, CategoryCount> categoryBytes{};
	using AllocationOwner = std::weak_ptr<const void>;
	std::map<AllocationOwner, std::weak_ptr<CacheBudgetCharge>,
		std::owner_less<AllocationOwner>> allocations;
	std::mutex retirementMutex;
	std::condition_variable retirementAvailable;
	std::deque<std::shared_ptr<RetiredAllocation>> retiredAllocations;
	std::map<AllocationOwner, std::weak_ptr<RetiredAllocation>,
		std::owner_less<AllocationOwner>> retirementOwners;
	std::shared_ptr<RetiredAllocation> retiringAllocation;
	bool stoppingRetirement = false;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	CacheBudgetTestHook testHook = nullptr;
	void* testHookContext = nullptr;
#endif
};

struct CacheBudgetCharge {
	CacheBudgetCharge(std::shared_ptr<CacheBudgetState> budgetState,
		std::size_t allocationBytes, CacheMemoryCategory allocationCategory,
		std::shared_ptr<const void> allocationOwner)
		: state(std::move(budgetState)), bytes(allocationBytes), category(allocationCategory),
		  identity(std::move(allocationOwner)), hasIdentity(!identity.expired()) {}

	~CacheBudgetCharge() {
		std::shared_ptr<CacheBudgetCharge> active;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			std::size_t& categoryUsage = state->categoryBytes[CategoryIndex(category)];
			categoryUsage -= std::min(bytes, categoryUsage);
			if (IsRetained(category)) {
				state->retainedBytes -= std::min(bytes, state->retainedBytes);
				++state->retainedCapacityRevision;
			}
			++state->releaseRevision;
			if (hasIdentity) {
				const auto found = state->allocations.find(identity);
				if (found != state->allocations.end()) {
					active = found->second.lock();
					if (!active || active.get() == this) state->allocations.erase(found);
				}
			}
		}
		// A promoted map entry may itself be the final charge owner. Drop it only
		// after releasing the mutex that another charge destructor takes.
		active.reset();
	}

	std::shared_ptr<CacheBudgetState> state;
	const std::size_t bytes;
	CacheMemoryCategory category;
	std::weak_ptr<const void> identity;
	const bool hasIdentity = false;
	std::size_t handles = 0;
	std::size_t retainedOwners = 0;
};

} // namespace detail

namespace {

void ChangeChargeCategory(detail::CacheBudgetCharge& charge,
	CacheMemoryCategory category) {
	if (charge.category == category) return;
	const bool wasRetained = IsRetained(charge.category);
	const bool becomesRetained = IsRetained(category);
	std::size_t& oldUsage = charge.state->categoryBytes[CategoryIndex(charge.category)];
	oldUsage -= std::min(charge.bytes, oldUsage);
	charge.state->categoryBytes[CategoryIndex(category)] += charge.bytes;
	if (wasRetained) {
		charge.state->retainedBytes -= std::min(charge.bytes,
			charge.state->retainedBytes);
	}
	if (becomesRetained) charge.state->retainedBytes += charge.bytes;
	if (wasRetained && !becomesRetained) {
		++charge.state->releaseRevision;
		++charge.state->retainedCapacityRevision;
	}
	charge.category = category;
}

void RetireAllocations(const std::shared_ptr<detail::CacheBudgetState>& state) {
	(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
	for (;;) {
		std::shared_ptr<detail::CacheBudgetState::RetiredAllocation> retired;
		std::shared_ptr<const void> allocation;
		{
			std::unique_lock<std::mutex> lock(state->retirementMutex);
			for (;;) {
				auto ready = std::find_if(state->retiredAllocations.begin(),
					state->retiredAllocations.end(), [](const auto& candidate) {
						return candidate && candidate->allocation &&
							candidate->allocation.use_count() == 1;
					});
				if (ready != state->retiredAllocations.end()) {
					retired = std::move(*ready);
					state->retiredAllocations.erase(ready);
					state->retiringAllocation = retired;
					allocation = std::move(retired->allocation);
					break;
				}
				if (state->stoppingRetirement && state->retiredAllocations.empty()) return;
				if (state->retiredAllocations.empty()) {
					state->retirementAvailable.wait(lock, [&state] {
						return state->stoppingRetirement ||
							!state->retiredAllocations.empty();
					});
				} else {
					state->retirementAvailable.wait_for(lock, std::chrono::milliseconds(2));
				}
			}
		}
		allocation.reset();
		std::vector<CacheReservation> reservations;
		{
			std::lock_guard<std::mutex> lock(state->retirementMutex);
			reservations.swap(retired->reservations);
			const auto owner = state->retirementOwners.find(retired->identity);
			if (owner != state->retirementOwners.end()) {
				const auto active = owner->second.lock();
				if (!active || active.get() == retired.get()) {
					state->retirementOwners.erase(owner);
				}
			}
			state->retiringAllocation.reset();
		}
		reservations.clear();
	}
}

} // namespace

CacheReservation::CacheReservation(
	std::shared_ptr<detail::CacheBudgetCharge> charge, AdoptHandle, bool retainedOwner)
	: charge_(std::move(charge)), retainedOwner_(retainedOwner) {}

CacheReservation::~CacheReservation() {
	Reset();
}

CacheReservation::CacheReservation(CacheReservation&& other) noexcept
	: charge_(std::move(other.charge_)),
	  retainedOwner_(std::exchange(other.retainedOwner_, false)) {}

CacheReservation& CacheReservation::operator=(CacheReservation&& other) noexcept {
	if (this == &other) return *this;
	Reset();
	charge_ = std::move(other.charge_);
	retainedOwner_ = std::exchange(other.retainedOwner_, false);
	return *this;
}

CacheReservation::operator bool() const {
	return static_cast<bool>(charge_);
}

std::size_t CacheReservation::Bytes() const {
	return charge_ ? charge_->bytes : 0;
}

CacheMemoryCategory CacheReservation::Category() const {
	if (!charge_) return CacheMemoryCategory::ActiveWorkingData;
	std::lock_guard<std::mutex> lock(charge_->state->mutex);
	return charge_->category;
}

CacheReservation CacheReservation::ShareAlias() const {
	if (!charge_) return {};
	std::lock_guard<std::mutex> lock(charge_->state->mutex);
	++charge_->handles;
	if (retainedOwner_) ++charge_->retainedOwners;
	return CacheReservation(charge_, AdoptHandle{}, retainedOwner_);
}

bool CacheReservation::Reclassify(CacheMemoryCategory category) {
	if (!charge_) return false;
	std::lock_guard<std::mutex> lock(charge_->state->mutex);
	if (charge_->handles != 1) return false;
	const bool becomesRetained = IsRetained(category);
	if (!IsRetained(charge_->category) && becomesRetained &&
		charge_->bytes > charge_->state->capacity -
		std::min(charge_->state->retainedBytes, charge_->state->capacity)) return false;
	if (charge_->category != category) ChangeChargeCategory(*charge_, category);
	if (becomesRetained != retainedOwner_) {
		if (becomesRetained) {
			++charge_->retainedOwners;
		} else {
			charge_->retainedOwners -= std::min<std::size_t>(1,
				charge_->retainedOwners);
		}
		retainedOwner_ = becomesRetained;
	}
	return true;
}

void CacheReservation::RelinquishRetainedOwnership() {
	if (!charge_ || !retainedOwner_) return;
	std::lock_guard<std::mutex> lock(charge_->state->mutex);
	if (retainedOwner_) {
		charge_->retainedOwners -= std::min<std::size_t>(1, charge_->retainedOwners);
		retainedOwner_ = false;
	}
}

bool CacheReservation::ReclassifyIfNoRetainedOwners(CacheMemoryCategory category) {
	if (!charge_ || IsRetained(category)) return false;
	std::lock_guard<std::mutex> lock(charge_->state->mutex);
	if (retainedOwner_ || charge_->retainedOwners != 0) return false;
	ChangeChargeCategory(*charge_, category);
	return true;
}

bool CacheReservation::ReclassifyForActiveUse() {
	if (!charge_) return false;
	std::lock_guard<std::mutex> lock(charge_->state->mutex);
	if (retainedOwner_) {
		charge_->retainedOwners -= std::min<std::size_t>(1, charge_->retainedOwners);
		retainedOwner_ = false;
	}
	if (charge_->retainedOwners == 0) {
		ChangeChargeCategory(*charge_, CacheMemoryCategory::ActiveWorkingData);
	}
	return true;
}

void CacheReservation::Reset() {
	if (!charge_) return;
	auto charge = std::move(charge_);
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	detail::CacheBudgetTestHook testHook = nullptr;
	void* testHookContext = nullptr;
#endif
	{
		std::lock_guard<std::mutex> lock(charge->state->mutex);
		if (charge->handles != 0) --charge->handles;
		if (retainedOwner_) {
			charge->retainedOwners -= std::min<std::size_t>(1, charge->retainedOwners);
			retainedOwner_ = false;
		}
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
		testHook = charge->state->testHook;
		testHookContext = charge->state->testHookContext;
#endif
	}
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	if (testHook) testHook(detail::CacheBudgetTestHookPoint::ReservationResetUnlocked,
		testHookContext);
#endif
	charge.reset();
}

SharedCacheBudget::SharedCacheBudget(std::size_t capacityBytes)
	: state_(std::make_shared<detail::CacheBudgetState>(capacityBytes)),
	  retirementWorker_([state = state_] { RetireAllocations(state); }) {}

SharedCacheBudget::~SharedCacheBudget() {
	bool externalOwner = false;
	{
		std::lock_guard<std::mutex> lock(state_->retirementMutex);
		state_->stoppingRetirement = true;
		if (state_->retiringAllocation && state_->retiringAllocation->allocation &&
			state_->retiringAllocation->allocation.use_count() > 1) externalOwner = true;
		for (const auto& retired : state_->retiredAllocations) {
			if (retired && retired->allocation && retired->allocation.use_count() > 1) {
				externalOwner = true;
			}
		}
	}
	state_->retirementAvailable.notify_all();
	if (retirementWorker_.joinable()) {
		if (externalOwner) retirementWorker_.detach();
		else retirementWorker_.join();
	}
}

CacheReservation SharedCacheBudget::ReserveInternal(std::size_t bytes,
	CacheMemoryCategory category, std::shared_ptr<const void> allocationOwner,
	bool retained) {
	if (bytes == 0) return {};
	// Keep a promoted weak reference alive until after unlocking. If a concurrent
	// Reset drops its reservation in this window, this can become the final owner.
	std::shared_ptr<detail::CacheBudgetCharge> charge;
	std::unique_lock<std::mutex> lock(state_->mutex);
	if (allocationOwner) {
		const std::weak_ptr<const void> ownerKey(allocationOwner);
		const auto existing = state_->allocations.find(ownerKey);
		if (existing != state_->allocations.end()) {
			charge = existing->second.lock();
			if (charge) {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				if (state_->testHook) {
					state_->testHook(detail::CacheBudgetTestHookPoint::ExistingChargeLocked,
						state_->testHookContext);
				}
#endif
				if (charge->bytes != bytes) return {};
				if (retained && !IsRetained(charge->category)) {
					if (bytes > state_->capacity -
						std::min(state_->retainedBytes, state_->capacity)) return {};
					ChangeChargeCategory(*charge, category);
				} else if (!retained && IsRetained(charge->category) &&
					charge->retainedOwners == 0) {
					// The allocation may still have a retirement reservation, but once
					// every live cache entry relinquishes ownership its remaining pixels
					// are temporary work rather than retained cache capacity.
					ChangeChargeCategory(*charge, category);
				}
				++charge->handles;
				if (retained) ++charge->retainedOwners;
				return CacheReservation(std::move(charge), CacheReservation::AdoptHandle{},
					retained);
			}
			state_->allocations.erase(existing);
		}
	}
	if (retained && bytes > state_->capacity -
		std::min(state_->retainedBytes, state_->capacity)) return {};
	charge = std::make_shared<detail::CacheBudgetCharge>(state_, bytes,
		category, allocationOwner);
	charge->handles = 1;
	charge->retainedOwners = retained ? 1 : 0;
	state_->categoryBytes[CategoryIndex(category)] += bytes;
	if (IsRetained(category)) state_->retainedBytes += bytes;
	if (allocationOwner) state_->allocations[allocationOwner] = charge;
	return CacheReservation(std::move(charge), CacheReservation::AdoptHandle{}, retained);
}

CacheReservation SharedCacheBudget::TryReserve(std::size_t bytes,
	CacheMemoryCategory category, std::shared_ptr<const void> allocationOwner) {
	if (!IsRetained(category)) return {};
	return ReserveInternal(bytes, category, std::move(allocationOwner), true);
}

CacheReservation SharedCacheBudget::TrackTemporary(std::size_t bytes,
	CacheMemoryCategory category, std::shared_ptr<const void> allocationOwner) {
	if (IsRetained(category)) return {};
	CacheReservation reservation = ReserveInternal(bytes, category,
		std::move(allocationOwner), false);
	if (reservation && category == CacheMemoryCategory::ActiveWorkingData &&
		reservation.Category() == category) {
		const CacheBudgetSnapshot snapshot = Snapshot();
		PerfDiagnostics::Instance().RecordText(PerfMetric::CacheSnapshot,
			bytes, snapshot.retainedBytes, snapshot.uploadStagingBytes,
			snapshot.activeWorkingBytes, snapshot.capacityBytes,
			snapshot.releaseRevision, "active_working_charge");
	}
	return reservation;
}

void SharedCacheBudget::RetireAllocation(std::shared_ptr<const void> allocation,
	CacheReservation reservation) {
	if (!allocation) return;
	const std::weak_ptr<const void> identity(allocation);
	{
		std::lock_guard<std::mutex> lock(state_->retirementMutex);
		const auto existing = state_->retirementOwners.find(identity);
		if (existing != state_->retirementOwners.end()) {
			if (const auto retired = existing->second.lock()) {
				if (reservation) retired->reservations.push_back(std::move(reservation));
				return;
			}
			state_->retirementOwners.erase(existing);
		}
		auto retired = std::make_shared<detail::CacheBudgetState::RetiredAllocation>();
		retired->allocation = std::move(allocation);
		retired->identity = identity;
		if (reservation) retired->reservations.push_back(std::move(reservation));
		state_->retirementOwners[identity] = retired;
		state_->retiredAllocations.push_back(std::move(retired));
	}
	state_->retirementAvailable.notify_one();
}

void SharedCacheBudget::SetCapacity(std::size_t capacityBytes) {
	std::lock_guard<std::mutex> lock(state_->mutex);
	if (capacityBytes > state_->capacity) ++state_->releaseRevision;
	if (capacityBytes > state_->capacity) ++state_->retainedCapacityRevision;
	state_->capacity = capacityBytes;
}

std::size_t SharedCacheBudget::Capacity() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->capacity;
}

std::size_t SharedCacheBudget::Used() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->retainedBytes;
}

std::size_t SharedCacheBudget::Available() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->capacity > state_->retainedBytes ?
		state_->capacity - state_->retainedBytes : 0;
}

std::uint64_t SharedCacheBudget::ReleaseRevision() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->releaseRevision;
}

std::uint64_t SharedCacheBudget::RetainedCapacityRevision() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->retainedCapacityRevision;
}

CacheBudgetSnapshot SharedCacheBudget::Snapshot() const {
	std::lock_guard<std::mutex> lock(state_->mutex);
	return {state_->releaseRevision, state_->retainedCapacityRevision,
		state_->capacity, state_->retainedBytes,
		state_->categoryBytes[CategoryIndex(CacheMemoryCategory::RetainedDecodedPixels)],
		state_->categoryBytes[CategoryIndex(CacheMemoryCategory::RetainedPreparedFrames)],
		state_->categoryBytes[CategoryIndex(CacheMemoryCategory::RetainedImageTextures)],
		state_->categoryBytes[CategoryIndex(CacheMemoryCategory::TemporaryUploadStaging)],
		state_->categoryBytes[CategoryIndex(CacheMemoryCategory::ActiveWorkingData)]};
}

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
void SharedCacheBudget::SetTestHookForTesting(detail::CacheBudgetTestHook hook,
	void* context) {
	std::lock_guard<std::mutex> lock(state_->mutex);
	state_->testHook = hook;
	state_->testHookContext = context;
}
#endif

} // namespace jpegview_linux
