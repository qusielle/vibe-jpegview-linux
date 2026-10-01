#pragma once

#include <cstdint>
#include <memory>

namespace jpegview_linux {

enum class PerfWorkClass : std::uint8_t {
	Unspecified,
	ActiveImageSpread,
	FocusedPreview,
	VisibleThumbnail,
	NearestNavigationNeighbor,
	DistantSpeculation,
};

enum class PerfExecution : std::uint8_t {
	Unspecified,
	EventThread,
	WorkerThread,
};

struct PerfContext {
	PerfWorkClass workClass = PerfWorkClass::Unspecified;
	PerfExecution execution = PerfExecution::Unspecified;
};

// A stack-only context passed through existing synchronous and worker paths.
// Its thread-local state is two enums, so disabled diagnostics add no allocation.
class PerfContextScope {
public:
	PerfContextScope(PerfWorkClass workClass, PerfExecution execution);
	~PerfContextScope();
	PerfContextScope(const PerfContextScope&) = delete;
	PerfContextScope& operator=(const PerfContextScope&) = delete;

private:
	PerfContext previous_;
};

PerfContext CurrentPerfContext();
const char* PerfWorkClassName(PerfWorkClass workClass);
const char* PerfExecutionName(PerfExecution execution);

// Keeps the first input that has not yet been followed by a presentation.
// The pure timestamp seam makes event-queue coalescing deterministic to test.
class PendingInputPresentation {
public:
	void Mark(std::uint64_t timestampUs);
	std::uint64_t TakeLatency(std::uint64_t presentationUs);
	std::uint64_t PendingSince() const;

private:
	std::uint64_t pendingSinceUs_ = 0;
	bool pending_ = false;
};

enum class PerfMetric : std::uint8_t {
	EventHandling,
	FrameBuild,
	Present,
	InputToPresent,
	Metadata,
	SourceRead,
	SourceMap,
	Decode,
	Processing,
	Resampling,
	TextureUpload,
	Cancellation,
	CacheSnapshot,
	QueueSnapshot,
	Renderer,
	DroppedRecords,
};

// Opt-in, bounded CSV diagnostics. With JPEGVIEW_PERF_TRACE unset or empty,
// timers return immediately and no writer thread or trace storage is created.
class PerfDiagnostics {
public:
	static PerfDiagnostics& Instance();

	PerfDiagnostics(const PerfDiagnostics&) = delete;
	PerfDiagnostics& operator=(const PerfDiagnostics&) = delete;
	~PerfDiagnostics();

	bool Enabled() const;
	std::uint64_t Begin() const;
	void End(PerfMetric metric, std::uint64_t start,
		std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
		std::uint64_t d = 0, std::uint64_t e = 0, std::uint64_t f = 0);
	void Record(PerfMetric metric, std::uint64_t durationUs = 0,
		std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
		std::uint64_t d = 0, std::uint64_t e = 0, std::uint64_t f = 0);
	void RecordText(PerfMetric metric, std::uint64_t a, std::uint64_t b,
		std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f,
		const char* text);
	void RecordText(PerfMetric metric, std::uint64_t durationUs,
		std::uint64_t a, std::uint64_t b, std::uint64_t c,
		std::uint64_t d, std::uint64_t e, std::uint64_t f, const char* text);
	void MarkInput();
	void RecordPresentation();

private:
	PerfDiagnostics();
	struct State;
	std::unique_ptr<State> state_;
	PendingInputPresentation pendingInput_;
};

class PerfScopedTimer {
public:
	PerfScopedTimer(PerfDiagnostics& diagnostics, PerfMetric metric,
		std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0);
	~PerfScopedTimer();

	PerfScopedTimer(const PerfScopedTimer&) = delete;
	PerfScopedTimer& operator=(const PerfScopedTimer&) = delete;

private:
	PerfDiagnostics& diagnostics_;
	PerfMetric metric_;
	std::uint64_t start_;
	std::uint64_t a_;
	std::uint64_t b_;
	std::uint64_t c_;
};

} // namespace jpegview_linux
