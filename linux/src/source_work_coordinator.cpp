#include "source_work_coordinator.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace jpegview_linux {
namespace {

struct BackingSource {
	SourceIdentity identity;
	std::string fallbackPath;

	bool Same(const BackingSource& other) const {
		if (identity.valid && other.identity.valid) {
			return identity.device == other.identity.device &&
				identity.inode == other.identity.inode;
		}
		return !fallbackPath.empty() && fallbackPath == other.fallbackPath;
	}
};

BackingSource ResolveBackingSource(const WorkContext& context,
	const std::filesystem::path& fallbackSource) {
	BackingSource backing;
	if (context.source.backingIdentity.valid) {
		backing.identity = context.source.backingIdentity;
	}
	std::filesystem::path path = fallbackSource;
	if (path.empty() && !context.source.logicalPath.empty()) path = context.source.logicalPath;
	if (path.empty()) return backing;
	path = ArchiveBackingFileForAdmission(path).lexically_normal();
	std::error_code error;
	const std::filesystem::path absolute = std::filesystem::absolute(path, error);
	backing.fallbackPath = (error ? path : absolute).lexically_normal().string();
	return backing;
}

} // namespace

struct SourceWorkCoordinator::Impl {
	struct Waiting {
		std::size_t ticket = 0;
		BackingSource source;
	};
	struct Active {
		std::size_t ticket = 0;
		SourceWorkPriority priority = SourceWorkPriority::Foreground;
		BackingSource source;
	};
	struct CpuActive {
		std::size_t ticket = 0;
		SourceWorkPriority priority = SourceWorkPriority::Foreground;
	};

	mutable std::mutex mutex;
	mutable std::condition_variable changed;
	std::deque<Waiting> foregroundQueue;
	std::deque<Waiting> speculativeQueue;
	std::vector<Active> active;
	std::deque<std::size_t> cpuForegroundQueue;
	std::deque<std::size_t> cpuBackgroundQueue;
	std::vector<CpuActive> activeCpu;
	const std::size_t cpuLimit = HardwareAwareCpuWorkerCount();
	std::size_t nextTicket = 1;
	bool foregroundPending = false;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	detail::SourceWorkTestHook testHook = nullptr;
	void* testHookContext = nullptr;

	void InvokeTestHook(detail::SourceWorkTestHookPoint point) {
		if (testHook != nullptr) testHook(point, testHookContext);
	}
#endif
};

SourceWorkCoordinator::SourceWorkCoordinator() : impl_(std::make_unique<Impl>()) {}
SourceWorkCoordinator::~SourceWorkCoordinator() = default;

SourceWorkLease::SourceWorkLease(SourceWorkCoordinator* coordinator, std::size_t ticket)
	: coordinator_(coordinator), ticket_(ticket) {}

SourceWorkLease::~SourceWorkLease() { Reset(); }

SourceWorkLease::SourceWorkLease(SourceWorkLease&& other) noexcept
	: coordinator_(other.coordinator_), ticket_(other.ticket_) {
	other.coordinator_ = nullptr;
	other.ticket_ = 0;
}

SourceWorkLease& SourceWorkLease::operator=(SourceWorkLease&& other) noexcept {
	if (this == &other) return *this;
	Reset();
	coordinator_ = other.coordinator_;
	ticket_ = other.ticket_;
	other.coordinator_ = nullptr;
	other.ticket_ = 0;
	return *this;
}

void SourceWorkLease::Reset() {
	if (coordinator_ == nullptr) return;
	SourceWorkCoordinator* coordinator = coordinator_;
	const std::size_t ticket = ticket_;
	coordinator_ = nullptr;
	ticket_ = 0;
	coordinator->Release(ticket);
}

CpuWorkLease::CpuWorkLease(SourceWorkCoordinator* coordinator, std::size_t ticket)
	: coordinator_(coordinator), ticket_(ticket) {}

CpuWorkLease::~CpuWorkLease() { Reset(); }

CpuWorkLease::CpuWorkLease(CpuWorkLease&& other) noexcept
	: coordinator_(other.coordinator_), ticket_(other.ticket_) {
	other.coordinator_ = nullptr;
	other.ticket_ = 0;
}

CpuWorkLease& CpuWorkLease::operator=(CpuWorkLease&& other) noexcept {
	if (this == &other) return *this;
	Reset();
	coordinator_ = other.coordinator_;
	ticket_ = other.ticket_;
	other.coordinator_ = nullptr;
	other.ticket_ = 0;
	return *this;
}

void CpuWorkLease::Reset() {
	if (coordinator_ == nullptr) return;
	SourceWorkCoordinator* coordinator = coordinator_;
	const std::size_t ticket = ticket_;
	coordinator_ = nullptr;
	ticket_ = 0;
	coordinator->ReleaseCpu(ticket);
}

SourceWorkCoordinator& SourceWorkCoordinator::Global() {
	static SourceWorkCoordinator coordinator;
	return coordinator;
}

SourceWorkLease SourceWorkCoordinator::Acquire(const WorkContext& context,
	const std::filesystem::path& fallbackSource) {
	if (!context.Continue()) return {};
	const BackingSource source = ResolveBackingSource(context, fallbackSource);
	const std::size_t ticket = [&] {
		std::lock_guard<std::mutex> lock(impl_->mutex);
		return impl_->nextTicket++;
	}();
	bool foreground = context.Priority() == SourceWorkPriority::Foreground;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		auto& queue = foreground ? impl_->foregroundQueue : impl_->speculativeQueue;
		try {
			queue.push_back({ticket, source});
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
			impl_->InvokeTestHook(detail::SourceWorkTestHookPoint::InitialSourceQueueRegistration);
#endif
		} catch (...) {
			if (!queue.empty() && queue.back().ticket == ticket) queue.pop_back();
			impl_->changed.notify_all();
			throw;
		}
		impl_->changed.notify_all();
	}

	std::unique_lock<std::mutex> lock(impl_->mutex);
	const auto removeWaiting = [&] {
		auto& queue = foreground ? impl_->foregroundQueue : impl_->speculativeQueue;
		const auto found = std::find_if(queue.begin(), queue.end(), [ticket](const auto& item) {
			return item.ticket == ticket;
		});
		if (found != queue.end()) queue.erase(found);
		impl_->changed.notify_all();
	};
	try {
	for (;;) {
		lock.unlock();
		const bool shouldContinue = context.Continue();
		const SourceWorkPriority priority = context.Priority();
		lock.lock();
		const bool nowForeground = priority == SourceWorkPriority::Foreground;
		if (nowForeground != foreground) {
			// Register in the new lane first. If allocation fails, the old lane
			// still owns the request and the outer handler can remove it cleanly.
			auto& destination = nowForeground ? impl_->foregroundQueue :
				impl_->speculativeQueue;
			destination.push_back({ticket, source});
			try {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				impl_->InvokeTestHook(
					detail::SourceWorkTestHookPoint::PromotedSourceQueueRegistration);
#endif
			} catch (...) {
				destination.pop_back();
				throw;
			}
			auto& previous = foreground ? impl_->foregroundQueue :
				impl_->speculativeQueue;
			const auto previousEntry = std::find_if(previous.begin(), previous.end(),
				[ticket](const auto& item) { return item.ticket == ticket; });
			if (previousEntry != previous.end()) previous.erase(previousEntry);
			foreground = nowForeground;
			impl_->changed.notify_all();
		}
		if (!shouldContinue) {
			removeWaiting();
			return {};
		}
		const auto& queue = foreground ? impl_->foregroundQueue : impl_->speculativeQueue;
		const bool firstInLane = !queue.empty() && queue.front().ticket == ticket;
		const bool sourceBusy = std::any_of(impl_->active.begin(), impl_->active.end(),
			[&source](const Impl::Active& active) { return source.Same(active.source); });
		const bool laneBusy = std::any_of(impl_->active.begin(), impl_->active.end(),
			[foreground](const Impl::Active& active) {
				return foreground ? active.priority == SourceWorkPriority::Foreground :
					active.priority != SourceWorkPriority::Foreground;
			});
		const bool foregroundWaiting = impl_->foregroundPending ||
			!impl_->foregroundQueue.empty() || std::any_of(impl_->active.begin(),
				impl_->active.end(), [](const Impl::Active& active) {
					return active.priority == SourceWorkPriority::Foreground;
				});
		if (firstInLane && !laneBusy && !sourceBusy &&
			(foreground || !foregroundWaiting)) {
			// Keep the queue record until active registration is complete. This
			// way allocation failure cannot strand either an active permit or an
			// untracked request.
			try {
				impl_->active.push_back({ticket, priority, source});
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				impl_->InvokeTestHook(detail::SourceWorkTestHookPoint::ActiveSourceRegistration);
#endif
			} catch (...) {
				if (!impl_->active.empty() && impl_->active.back().ticket == ticket) {
					impl_->active.pop_back();
				}
				throw;
			}
			auto& mutableQueue = foreground ? impl_->foregroundQueue : impl_->speculativeQueue;
			mutableQueue.pop_front();
			impl_->changed.notify_all();
			return SourceWorkLease(this, ticket);
		}
		impl_->changed.wait_for(lock, std::chrono::milliseconds(10));
	}
	} catch (...) {
		removeWaiting();
		throw;
	}
}

CpuWorkLease SourceWorkCoordinator::AcquireCpu(const WorkContext& context) {
	if (!context.Continue()) return {};
	const std::size_t ticket = [&] {
		std::lock_guard<std::mutex> lock(impl_->mutex);
		return impl_->nextTicket++;
	}();
	bool foreground = context.Priority() == SourceWorkPriority::Foreground;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		auto& queue = foreground ? impl_->cpuForegroundQueue : impl_->cpuBackgroundQueue;
		try {
			queue.push_back(ticket);
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
			impl_->InvokeTestHook(detail::SourceWorkTestHookPoint::InitialCpuQueueRegistration);
#endif
		} catch (...) {
			if (!queue.empty() && queue.back() == ticket) queue.pop_back();
			impl_->changed.notify_all();
			throw;
		}
	}
	impl_->changed.notify_all();

	std::unique_lock<std::mutex> lock(impl_->mutex);
	const auto removeWaiting = [&] {
		auto& queue = foreground ? impl_->cpuForegroundQueue : impl_->cpuBackgroundQueue;
		const auto found = std::find(queue.begin(), queue.end(), ticket);
		if (found != queue.end()) queue.erase(found);
		impl_->changed.notify_all();
	};
	try {
	for (;;) {
		lock.unlock();
		const bool shouldContinue = context.Continue();
		const SourceWorkPriority priority = context.Priority();
		lock.lock();
		const bool nowForeground = priority == SourceWorkPriority::Foreground;
		if (nowForeground != foreground) {
			auto& destination = nowForeground ? impl_->cpuForegroundQueue :
				impl_->cpuBackgroundQueue;
			destination.push_back(ticket);
			try {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				impl_->InvokeTestHook(
					detail::SourceWorkTestHookPoint::PromotedCpuQueueRegistration);
#endif
			} catch (...) {
				destination.pop_back();
				throw;
			}
			auto& previous = foreground ? impl_->cpuForegroundQueue :
				impl_->cpuBackgroundQueue;
			const auto previousEntry = std::find(previous.begin(), previous.end(), ticket);
			if (previousEntry != previous.end()) previous.erase(previousEntry);
			foreground = nowForeground;
			impl_->changed.notify_all();
		}
		if (!shouldContinue) {
			removeWaiting();
			return {};
		}
		const auto& queue = foreground ? impl_->cpuForegroundQueue : impl_->cpuBackgroundQueue;
		const bool firstInLane = !queue.empty() && queue.front() == ticket;
		const bool backgroundMayStart = foreground ||
			(impl_->cpuForegroundQueue.empty() && !impl_->foregroundPending);
		if (firstInLane && backgroundMayStart &&
			impl_->activeCpu.size() < impl_->cpuLimit) {
			try {
				impl_->activeCpu.push_back({ticket, priority});
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				impl_->InvokeTestHook(detail::SourceWorkTestHookPoint::ActiveCpuRegistration);
#endif
			} catch (...) {
				if (!impl_->activeCpu.empty() && impl_->activeCpu.back().ticket == ticket) {
					impl_->activeCpu.pop_back();
				}
				throw;
			}
			auto& mutableQueue = foreground ? impl_->cpuForegroundQueue :
				impl_->cpuBackgroundQueue;
			mutableQueue.pop_front();
			impl_->changed.notify_all();
			return CpuWorkLease(this, ticket);
		}
		impl_->changed.wait_for(lock, std::chrono::milliseconds(10));
	}
	} catch (...) {
		removeWaiting();
		throw;
	}
}

SourceCpuWorkLease SourceWorkCoordinator::AcquireSourceAndCpu(
	const WorkContext& context, const std::filesystem::path& fallbackSource) {
	SourceCpuWorkLease result;
	if (!context.Continue()) return result;
	if (context.sourceAccessAlreadyAdmitted != context.cpuProcessingAlreadyAdmitted) {
		return result;
	}
	if (context.sourceAccessAlreadyAdmitted || context.cpuProcessingAlreadyAdmitted) {
		result.admitted_ = true;
		return result;
	}

	const BackingSource source = ResolveBackingSource(context, fallbackSource);
	const std::size_t ticket = [&] {
		std::lock_guard<std::mutex> lock(impl_->mutex);
		return impl_->nextTicket++;
	}();
	bool foreground = context.Priority() == SourceWorkPriority::Foreground;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		auto& sourceQueue = foreground ? impl_->foregroundQueue :
			impl_->speculativeQueue;
		auto& cpuQueue = foreground ? impl_->cpuForegroundQueue :
			impl_->cpuBackgroundQueue;
		sourceQueue.push_back({ticket, source});
		bool cpuQueued = false;
		try {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
			impl_->InvokeTestHook(
				detail::SourceWorkTestHookPoint::InitialSourceQueueRegistration);
#endif
			cpuQueue.push_back(ticket);
			cpuQueued = true;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
			impl_->InvokeTestHook(
				detail::SourceWorkTestHookPoint::InitialCpuQueueRegistration);
#endif
		} catch (...) {
			if (cpuQueued) cpuQueue.pop_back();
			sourceQueue.pop_back();
			impl_->changed.notify_all();
			throw;
		}
	}
	impl_->changed.notify_all();

	std::unique_lock<std::mutex> lock(impl_->mutex);
	const auto removeWaitingFromLane = [&](bool foregroundLane) {
		auto& sourceQueue = foregroundLane ? impl_->foregroundQueue :
			impl_->speculativeQueue;
		const auto sourceFound = std::find_if(sourceQueue.begin(), sourceQueue.end(),
			[ticket](const auto& item) { return item.ticket == ticket; });
		if (sourceFound != sourceQueue.end()) sourceQueue.erase(sourceFound);
		auto& cpuQueue = foregroundLane ? impl_->cpuForegroundQueue :
			impl_->cpuBackgroundQueue;
		const auto cpuFound = std::find(cpuQueue.begin(), cpuQueue.end(), ticket);
		if (cpuFound != cpuQueue.end()) cpuQueue.erase(cpuFound);
	};
	const auto removeWaiting = [&] {
		removeWaitingFromLane(foreground);
		impl_->changed.notify_all();
	};
	try {
	for (;;) {
		lock.unlock();
		const bool shouldContinue = context.Continue();
		const SourceWorkPriority priority = context.Priority();
		lock.lock();
		const bool nowForeground = priority == SourceWorkPriority::Foreground;
		if (nowForeground != foreground) {
			auto& destinationSource = nowForeground ? impl_->foregroundQueue :
				impl_->speculativeQueue;
			auto& destinationCpu = nowForeground ? impl_->cpuForegroundQueue :
				impl_->cpuBackgroundQueue;
			destinationSource.push_back({ticket, source});
			bool destinationCpuQueued = false;
			try {
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				impl_->InvokeTestHook(
					detail::SourceWorkTestHookPoint::PromotedSourceQueueRegistration);
#endif
				destinationCpu.push_back(ticket);
				destinationCpuQueued = true;
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				impl_->InvokeTestHook(
					detail::SourceWorkTestHookPoint::PromotedCpuQueueRegistration);
#endif
			} catch (...) {
				if (destinationCpuQueued) destinationCpu.pop_back();
				destinationSource.pop_back();
				throw;
			}
			removeWaitingFromLane(foreground);
			foreground = nowForeground;
			impl_->changed.notify_all();
		}
		if (!shouldContinue) {
			removeWaiting();
			return result;
		}

		const auto& sourceQueue = foreground ? impl_->foregroundQueue : impl_->speculativeQueue;
		const auto& cpuQueue = foreground ? impl_->cpuForegroundQueue : impl_->cpuBackgroundQueue;
		const bool firstInSourceLane = !sourceQueue.empty() &&
			sourceQueue.front().ticket == ticket;
		const bool firstInCpuLane = !cpuQueue.empty() && cpuQueue.front() == ticket;
		const bool sourceBusy = std::any_of(impl_->active.begin(), impl_->active.end(),
			[&source](const Impl::Active& active) { return source.Same(active.source); });
		const bool sourceLaneBusy = std::any_of(impl_->active.begin(), impl_->active.end(),
			[foreground](const Impl::Active& active) {
				return foreground ? active.priority == SourceWorkPriority::Foreground :
					active.priority != SourceWorkPriority::Foreground;
			});
		const bool foregroundWaiting = impl_->foregroundPending ||
			!impl_->foregroundQueue.empty() || std::any_of(impl_->active.begin(),
				impl_->active.end(), [](const Impl::Active& active) {
					return active.priority == SourceWorkPriority::Foreground;
				});
		const bool cpuPriorityAvailable = foreground ||
			(impl_->cpuForegroundQueue.empty() && !impl_->foregroundPending);
		if (firstInSourceLane && firstInCpuLane && !sourceBusy && !sourceLaneBusy &&
			(foreground || !foregroundWaiting) && cpuPriorityAvailable &&
			impl_->activeCpu.size() < impl_->cpuLimit) {
			try {
				impl_->active.push_back({ticket, priority, source});
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				impl_->InvokeTestHook(
					detail::SourceWorkTestHookPoint::ActiveSourceRegistration);
#endif
				impl_->activeCpu.push_back({ticket, priority});
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
				impl_->InvokeTestHook(
					detail::SourceWorkTestHookPoint::ActiveCpuRegistration);
#endif
			} catch (...) {
				if (!impl_->activeCpu.empty() && impl_->activeCpu.back().ticket == ticket) {
					impl_->activeCpu.pop_back();
				}
				if (!impl_->active.empty() && impl_->active.back().ticket == ticket) {
					impl_->active.pop_back();
				}
				throw;
			}
			auto& mutableSourceQueue = foreground ? impl_->foregroundQueue :
				impl_->speculativeQueue;
			mutableSourceQueue.pop_front();
			if (foreground) impl_->cpuForegroundQueue.pop_front();
			else impl_->cpuBackgroundQueue.pop_front();
			impl_->changed.notify_all();
			result.cpu_ = CpuWorkLease(this, ticket);
			result.source_ = SourceWorkLease(this, ticket);
			result.admitted_ = true;
			return result;
		}
		impl_->changed.wait_for(lock, std::chrono::milliseconds(10));
	}
	} catch (...) {
		removeWaiting();
		throw;
	}
}

void SourceWorkCoordinator::SetForegroundPending(bool pending) {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->foregroundPending = pending;
	}
	impl_->changed.notify_all();
}

void SourceWorkCoordinator::NotifyWaiters() {
	impl_->changed.notify_all();
}

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
void SourceWorkCoordinator::SetTestHookForTesting(
	detail::SourceWorkTestHook hook, void* context) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->testHook = hook;
	impl_->testHookContext = context;
}
#endif

SourceWorkSnapshot SourceWorkCoordinator::Snapshot() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	SourceWorkSnapshot snapshot;
	for (const Impl::Active& active : impl_->active) {
		if (active.priority == SourceWorkPriority::Foreground) ++snapshot.activeForeground;
		else ++snapshot.activeSpeculative;
	}
	snapshot.waitingForeground = impl_->foregroundQueue.size();
	snapshot.waitingSpeculative = impl_->speculativeQueue.size();
	snapshot.activeCpu = impl_->activeCpu.size();
	snapshot.waitingCpu = impl_->cpuForegroundQueue.size() + impl_->cpuBackgroundQueue.size();
	snapshot.foregroundPending = impl_->foregroundPending ||
		!impl_->foregroundQueue.empty() || snapshot.activeForeground != 0;
	return snapshot;
}

bool SourceWorkCoordinator::WaitForSnapshot(
	const std::function<bool(const SourceWorkSnapshot&)>& predicate,
	std::chrono::milliseconds timeout) const {
	if (!predicate) return false;
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	for (;;) {
		const SourceWorkSnapshot snapshot = Snapshot();
		if (predicate(snapshot)) return true;
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) return predicate(Snapshot());
		std::unique_lock<std::mutex> lock(impl_->mutex);
		impl_->changed.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(10)));
	}
}

void SourceWorkCoordinator::Release(std::size_t ticket) {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		const auto found = std::find_if(impl_->active.begin(), impl_->active.end(),
			[ticket](const Impl::Active& active) { return active.ticket == ticket; });
		if (found != impl_->active.end()) impl_->active.erase(found);
	}
	impl_->changed.notify_all();
}

void SourceWorkCoordinator::ReleaseCpu(std::size_t ticket) {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		const auto found = std::find_if(impl_->activeCpu.begin(), impl_->activeCpu.end(),
			[ticket](const Impl::CpuActive& active) { return active.ticket == ticket; });
		if (found != impl_->activeCpu.end()) impl_->activeCpu.erase(found);
	}
	impl_->changed.notify_all();
}

std::size_t HardwareAwareCpuWorkerCount(unsigned int hardwareConcurrency) {
	if (hardwareConcurrency == 0) hardwareConcurrency = std::thread::hardware_concurrency();
	if (hardwareConcurrency <= 2) return 1;
	return std::min<std::size_t>(4, static_cast<std::size_t>(hardwareConcurrency - 1));
}

std::size_t ClampCpuWorkerCount(std::size_t requested,
	unsigned int hardwareConcurrency) {
	const std::size_t target = requested == 0 ?
		HardwareAwareCpuWorkerCount(hardwareConcurrency) : requested;
	return std::clamp<std::size_t>(target, 1, 4);
}

} // namespace jpegview_linux
