#pragma once

#include "work_context.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>

namespace jpegview_linux {

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
namespace detail {
enum class SourceWorkTestHookPoint {
	InitialSourceQueueRegistration,
	InitialCpuQueueRegistration,
	PromotedSourceQueueRegistration,
	PromotedCpuQueueRegistration,
	ActiveSourceRegistration,
	ActiveCpuRegistration,
};
using SourceWorkTestHook = void (*)(SourceWorkTestHookPoint, void*);
} // namespace detail
#endif

struct SourceWorkSnapshot {
	std::size_t activeForeground = 0;
	std::size_t activeSpeculative = 0;
	std::size_t waitingForeground = 0;
	std::size_t waitingSpeculative = 0;
	std::size_t activeCpu = 0;
	std::size_t waitingCpu = 0;
	bool foregroundPending = false;
};

class SourceWorkCoordinator;

class CpuWorkLease {
public:
	CpuWorkLease() = default;
	~CpuWorkLease();
	CpuWorkLease(CpuWorkLease&& other) noexcept;
	CpuWorkLease& operator=(CpuWorkLease&& other) noexcept;
	CpuWorkLease(const CpuWorkLease&) = delete;
	CpuWorkLease& operator=(const CpuWorkLease&) = delete;

	explicit operator bool() const { return coordinator_ != nullptr; }
	void Reset();

private:
	friend class SourceWorkCoordinator;
	CpuWorkLease(SourceWorkCoordinator* coordinator, std::size_t ticket);

	SourceWorkCoordinator* coordinator_ = nullptr;
	std::size_t ticket_ = 0;
};

class SourceWorkLease {
public:
	SourceWorkLease() = default;
	~SourceWorkLease();
	SourceWorkLease(SourceWorkLease&& other) noexcept;
	SourceWorkLease& operator=(SourceWorkLease&& other) noexcept;
	SourceWorkLease(const SourceWorkLease&) = delete;
	SourceWorkLease& operator=(const SourceWorkLease&) = delete;

	explicit operator bool() const { return coordinator_ != nullptr; }
	void Reset();

private:
	friend class SourceWorkCoordinator;
	SourceWorkLease(SourceWorkCoordinator* coordinator, std::size_t ticket);

	SourceWorkCoordinator* coordinator_ = nullptr;
	std::size_t ticket_ = 0;
};

class SourceCpuWorkLease {
public:
	explicit operator bool() const { return admitted_; }
	void Reset() {
		source_.Reset();
		cpu_.Reset();
		admitted_ = false;
	}

private:
	friend class SourceWorkCoordinator;
	CpuWorkLease cpu_;
	SourceWorkLease source_;
	bool admitted_ = false;
};

// Coordinates the source-consuming portions of otherwise independent worker
// pools. Foreground work has its own lane; speculative and metadata work share
// one lane and wait while foreground work is pending.
class SourceWorkCoordinator {
public:
	SourceWorkCoordinator();
	~SourceWorkCoordinator();
	SourceWorkCoordinator(const SourceWorkCoordinator&) = delete;
	SourceWorkCoordinator& operator=(const SourceWorkCoordinator&) = delete;

	static SourceWorkCoordinator& Global();

	SourceWorkLease Acquire(const WorkContext& context,
		const std::filesystem::path& fallbackSource = {});
	CpuWorkLease AcquireCpu(const WorkContext& context);
	SourceCpuWorkLease AcquireSourceAndCpu(const WorkContext& context,
		const std::filesystem::path& fallbackSource = {});
	void SetForegroundPending(bool pending);
	void NotifyWaiters();
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	void SetTestHookForTesting(detail::SourceWorkTestHook hook, void* context);
#endif
	SourceWorkSnapshot Snapshot() const;
	bool WaitForSnapshot(const std::function<bool(const SourceWorkSnapshot&)>& predicate,
		std::chrono::milliseconds timeout) const;

private:
	friend class SourceWorkLease;
	friend class CpuWorkLease;
	struct Impl;
	std::unique_ptr<Impl> impl_;
	void Release(std::size_t ticket);
	void ReleaseCpu(std::size_t ticket);
};

std::size_t HardwareAwareCpuWorkerCount(unsigned int hardwareConcurrency = 0);
std::size_t ClampCpuWorkerCount(std::size_t requested,
	unsigned int hardwareConcurrency = 0);

} // namespace jpegview_linux
