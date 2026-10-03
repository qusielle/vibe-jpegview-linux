#pragma once

#include "archive_source.h"

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

namespace jpegview_linux {

enum class SourceWorkPriority {
	Foreground,
	Speculative,
	Metadata,
};

enum class WorkerFailureKind {
	None,
	Cancelled,
	SourceUnavailable,
	ProcessingFailed,
	Exception,
};

struct WorkerFailure {
	WorkerFailureKind kind = WorkerFailureKind::None;
	std::string message;

	bool Failed() const { return kind != WorkerFailureKind::None; }
};

// Captures source ownership and cancellation at the request boundary. Source
// identity is the backing file identity, so archive members share admission.
struct WorkContext {
	SourceKey source;
	SourceWorkPriority sourcePriority = SourceWorkPriority::Foreground;
	std::function<SourceWorkPriority()> currentPriority;
	std::function<bool()> shouldContinue;
	std::function<void()> onForegroundYield;
	bool sourceAccessAlreadyAdmitted = false;
	bool cpuProcessingAlreadyAdmitted = false;

	bool Continue() const {
		if (!shouldContinue) return true;
		try {
			return shouldContinue();
		} catch (...) {
			return false;
		}
	}

	void MarkForegroundYield() const {
		if (!onForegroundYield) return;
		try {
			onForegroundYield();
		} catch (...) {
		}
	}

	SourceWorkPriority Priority() const {
		if (!currentPriority) return sourcePriority;
		try {
			return currentPriority();
		} catch (...) {
			return sourcePriority;
		}
	}
};

inline WorkContext MakeWorkContext(const SourceDescriptor& source,
	SourceWorkPriority priority, std::function<bool()> shouldContinue = {}) {
	WorkContext context;
	context.source = source.Key();
	context.sourcePriority = priority;
	context.shouldContinue = std::move(shouldContinue);
	return context;
}

inline WorkContext MakePathWorkContext(const std::filesystem::path& path,
	SourceWorkPriority priority, std::function<bool()> shouldContinue = {}) {
	WorkContext context;
	context.sourcePriority = priority;
	context.shouldContinue = std::move(shouldContinue);
	context.source.logicalPath = path.string();
	return context;
}

inline const WorkContext*& ActiveWorkContextSlot() {
	static thread_local const WorkContext* context = nullptr;
	return context;
}

class ScopedWorkContext {
public:
	explicit ScopedWorkContext(const WorkContext& context)
		: previous_(ActiveWorkContextSlot()) {
		ActiveWorkContextSlot() = &context;
	}
	~ScopedWorkContext() { ActiveWorkContextSlot() = previous_; }
	ScopedWorkContext(const ScopedWorkContext&) = delete;
	ScopedWorkContext& operator=(const ScopedWorkContext&) = delete;

private:
	const WorkContext* previous_;
};

inline WorkContext ResolveWorkContext(const std::filesystem::path& path,
	SourceWorkPriority defaultPriority, const WorkContext& supplied = {}) {
	WorkContext context = supplied;
	if (context.source.Empty()) {
		const WorkContext* active = ActiveWorkContextSlot();
		if (active != nullptr) {
			const WorkContext local = supplied;
			context = *active;
			if (local.shouldContinue && active->shouldContinue) {
				const auto localContinue = local.shouldContinue;
				const auto activeContinue = active->shouldContinue;
				context.shouldContinue = [activeContinue, localContinue] {
					return activeContinue() && localContinue();
				};
			} else if (local.shouldContinue) {
				context.shouldContinue = local.shouldContinue;
			}
			if (local.onForegroundYield && active->onForegroundYield) {
				const auto localYield = local.onForegroundYield;
				const auto activeYield = active->onForegroundYield;
				context.onForegroundYield = [activeYield, localYield] {
					try {
						activeYield();
					} catch (...) {
					}
				try {
						localYield();
					} catch (...) {
					}
				};
			} else if (local.onForegroundYield) {
				context.onForegroundYield = local.onForegroundYield;
			}
			if (local.currentPriority && active->currentPriority) {
				const auto localPriority = local.currentPriority;
				const auto activePriority = active->currentPriority;
				context.currentPriority = [activePriority, localPriority] {
					const SourceWorkPriority activeValue = activePriority();
					const SourceWorkPriority localValue = localPriority();
					return static_cast<int>(activeValue) <= static_cast<int>(localValue) ?
						activeValue : localValue;
				};
			} else if (local.currentPriority) {
				context.currentPriority = local.currentPriority;
			}
			context.sourceAccessAlreadyAdmitted = active->sourceAccessAlreadyAdmitted;
			context.cpuProcessingAlreadyAdmitted = active->cpuProcessingAlreadyAdmitted;
		}
		if (context.source.Empty()) {
			const auto resolvedContinue = context.shouldContinue;
			const auto resolvedPriority = context.currentPriority;
			const auto resolvedForegroundYield = context.onForegroundYield;
			const bool sourceAlreadyAdmitted = context.sourceAccessAlreadyAdmitted;
			const bool cpuAlreadyAdmitted = context.cpuProcessingAlreadyAdmitted;
			context = MakePathWorkContext(path, defaultPriority, resolvedContinue);
			context.currentPriority = resolvedPriority;
			context.onForegroundYield = resolvedForegroundYield;
			context.sourceAccessAlreadyAdmitted = sourceAlreadyAdmitted;
			context.cpuProcessingAlreadyAdmitted = cpuAlreadyAdmitted;
		}
	}
	if (!supplied.source.Empty()) {
		context.source = supplied.source;
		context.sourcePriority = supplied.sourcePriority;
		if (supplied.shouldContinue) context.shouldContinue = supplied.shouldContinue;
		if (supplied.currentPriority) context.currentPriority = supplied.currentPriority;
		context.sourceAccessAlreadyAdmitted = supplied.sourceAccessAlreadyAdmitted;
		context.cpuProcessingAlreadyAdmitted = supplied.cpuProcessingAlreadyAdmitted;
	}
	return context;
}

} // namespace jpegview_linux
