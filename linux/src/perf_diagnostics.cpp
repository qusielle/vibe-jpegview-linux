#include "perf_diagnostics.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

namespace jpegview_linux {
namespace {

constexpr std::size_t kTraceCapacity = 4096;
constexpr std::size_t kTextLength = 64;

std::uint64_t MonotonicMicros() {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

thread_local PerfContext currentPerfContext;

std::uint64_t CurrentThreadNumber() {
	return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

const char* MetricName(PerfMetric metric) {
	switch (metric) {
	case PerfMetric::EventHandling: return "event_handling";
	case PerfMetric::FrameBuild: return "frame_build";
	case PerfMetric::Present: return "present";
	case PerfMetric::InputToPresent: return "input_to_present";
	case PerfMetric::Metadata: return "metadata";
	case PerfMetric::SourceRead: return "source_read";
	case PerfMetric::SourceMap: return "source_map";
	case PerfMetric::Decode: return "decode";
	case PerfMetric::Processing: return "processing";
	case PerfMetric::Resampling: return "resampling";
	case PerfMetric::TextureUpload: return "texture_upload";
	case PerfMetric::TextureDestroy: return "texture_destroy";
	case PerfMetric::Cancellation: return "cancellation";
	case PerfMetric::CacheSnapshot: return "cache_snapshot";
	case PerfMetric::QueueSnapshot: return "queue_snapshot";
	case PerfMetric::Renderer: return "renderer";
	case PerfMetric::DroppedRecords: return "trace_dropped";
	}
	return "unknown";
}

struct TraceRecord {
	std::uint64_t monotonicUs = 0;
	std::uint64_t durationUs = 0;
	std::uint64_t threadId = 0;
	std::uint64_t values[6]{};
	PerfMetric metric = PerfMetric::EventHandling;
	PerfWorkClass workClass = PerfWorkClass::Unspecified;
	PerfExecution execution = PerfExecution::Unspecified;
	char text[kTextLength]{};
};

void WriteCsvText(std::FILE* output, const char* text) {
	std::fputc('"', output);
	for (const char* cursor = text; *cursor != '\0'; ++cursor) {
		if (*cursor == '"') std::fputc('"', output);
		std::fputc(*cursor, output);
	}
	std::fputc('"', output);
}

void WriteRecord(std::FILE* output, const TraceRecord& record) {
	std::fprintf(output, "%llu,%s,%llu,%s,%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu,",
		static_cast<unsigned long long>(record.monotonicUs), MetricName(record.metric),
		static_cast<unsigned long long>(record.durationUs), PerfExecutionName(record.execution),
		static_cast<unsigned long long>(record.threadId), PerfWorkClassName(record.workClass),
		static_cast<unsigned long long>(record.values[0]),
		static_cast<unsigned long long>(record.values[1]),
		static_cast<unsigned long long>(record.values[2]),
		static_cast<unsigned long long>(record.values[3]),
		static_cast<unsigned long long>(record.values[4]),
		static_cast<unsigned long long>(record.values[5]));
	WriteCsvText(output, record.text);
	std::fputc('\n', output);
}

} // namespace

PerfContextScope::PerfContextScope(PerfWorkClass workClass, PerfExecution execution)
	: previous_(currentPerfContext) {
	currentPerfContext = {workClass, execution};
}

PerfContextScope::~PerfContextScope() {
	currentPerfContext = previous_;
}

PerfContext CurrentPerfContext() {
	return currentPerfContext;
}

const char* PerfWorkClassName(PerfWorkClass workClass) {
	switch (workClass) {
	case PerfWorkClass::Unspecified: return "unspecified";
	case PerfWorkClass::ActiveImageSpread: return "active_image_spread";
	case PerfWorkClass::FocusedPreview: return "focused_preview";
	case PerfWorkClass::VisibleThumbnail: return "visible_thumbnail";
	case PerfWorkClass::NearestNavigationNeighbor: return "nearest_navigation_neighbor";
	case PerfWorkClass::DistantSpeculation: return "distant_speculation";
	}
	return "unspecified";
}

const char* PerfExecutionName(PerfExecution execution) {
	switch (execution) {
	case PerfExecution::Unspecified: return "unspecified";
	case PerfExecution::EventThread: return "event_thread";
	case PerfExecution::WorkerThread: return "worker_thread";
	}
	return "unspecified";
}

void PendingInputPresentation::Mark(std::uint64_t timestampUs) {
	if (pending_) return;
	pendingSinceUs_ = timestampUs;
	pending_ = true;
}

std::uint64_t PendingInputPresentation::TakeLatency(std::uint64_t presentationUs) {
	if (!pending_) return 0;
	const std::uint64_t latency = presentationUs >= pendingSinceUs_ ?
		presentationUs - pendingSinceUs_ : 0;
	pending_ = false;
	pendingSinceUs_ = 0;
	return latency;
}

std::uint64_t PendingInputPresentation::PendingSince() const {
	return pending_ ? pendingSinceUs_ : 0;
}

struct PerfDiagnostics::State {
	explicit State(std::FILE* outputFile) : output(outputFile) {
		writer = std::thread([this] { WriteLoop(); });
	}

	~State() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		available.notify_one();
		if (writer.joinable()) writer.join();
		std::fclose(output);
	}

	void Push(const TraceRecord& record) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (count == records.size()) {
				++dropped;
				return;
			}
			records[tail] = record;
			tail = (tail + 1) % records.size();
			++count;
		}
		available.notify_one();
	}

	void WriteLoop() {
		std::fprintf(output,
			"monotonic_us,metric,duration_us,execution,thread_id,work_class,"
			"value_a,value_b,value_c,value_d,value_e,value_f,detail\n");
		std::array<TraceRecord, kTraceCapacity> batch{};
		for (;;) {
			std::size_t batchSize = 0;
			std::uint64_t droppedCount = 0;
			bool finish = false;
			{
				std::unique_lock<std::mutex> lock(mutex);
				available.wait_for(lock, std::chrono::milliseconds(50), [this] {
					return stopping;
				});
				while (count != 0 && batchSize < batch.size()) {
					batch[batchSize++] = records[head];
					head = (head + 1) % records.size();
					--count;
				}
				droppedCount = dropped;
				dropped = 0;
				finish = stopping && count == 0;
			}
			for (std::size_t index = 0; index < batchSize; ++index) {
				WriteRecord(output, batch[index]);
			}
			if (droppedCount != 0) {
				TraceRecord droppedRecord;
				droppedRecord.monotonicUs = MonotonicMicros();
				droppedRecord.threadId = CurrentThreadNumber();
				droppedRecord.metric = PerfMetric::DroppedRecords;
				droppedRecord.values[0] = droppedCount;
				WriteRecord(output, droppedRecord);
			}
			if (batchSize != 0 || droppedCount != 0 || finish) std::fflush(output);
			if (finish) return;
		}
	}

	std::FILE* output;
	std::mutex mutex;
	std::condition_variable available;
	std::array<TraceRecord, kTraceCapacity> records{};
	std::size_t head = 0;
	std::size_t tail = 0;
	std::size_t count = 0;
	std::uint64_t dropped = 0;
	bool stopping = false;
	std::thread writer;
};

PerfDiagnostics& PerfDiagnostics::Instance() {
	static PerfDiagnostics diagnostics;
	return diagnostics;
}

PerfDiagnostics::PerfDiagnostics() {
	const char* tracePath = std::getenv("JPEGVIEW_PERF_TRACE");
	if (tracePath == nullptr || *tracePath == '\0') return;
	std::FILE* output = std::fopen(tracePath, "w");
	if (output == nullptr) {
		std::fprintf(stderr, "Cannot open JPEGVIEW_PERF_TRACE output: %s\n", tracePath);
		return;
	}
	state_ = std::make_unique<State>(output);
}

PerfDiagnostics::~PerfDiagnostics() = default;

bool PerfDiagnostics::Enabled() const {
	return state_ != nullptr;
}

std::uint64_t PerfDiagnostics::Begin() const {
	return state_ == nullptr ? 0 : MonotonicMicros();
}

void PerfDiagnostics::End(PerfMetric metric, std::uint64_t start,
	std::uint64_t a, std::uint64_t b, std::uint64_t c,
	std::uint64_t d, std::uint64_t e, std::uint64_t f) {
	if (start == 0 || state_ == nullptr) return;
	const std::uint64_t finish = MonotonicMicros();
	TraceRecord record;
	record.monotonicUs = finish;
	record.durationUs = finish - start;
	record.threadId = CurrentThreadNumber();
	record.workClass = CurrentPerfContext().workClass;
	record.execution = CurrentPerfContext().execution;
	record.values[0] = a;
	record.values[1] = b;
	record.values[2] = c;
	record.values[3] = d;
	record.values[4] = e;
	record.values[5] = f;
	record.metric = metric;
	state_->Push(record);
}

void PerfDiagnostics::Record(PerfMetric metric, std::uint64_t durationUs,
	std::uint64_t a, std::uint64_t b, std::uint64_t c,
	std::uint64_t d, std::uint64_t e, std::uint64_t f) {
	if (state_ == nullptr) return;
	TraceRecord record;
	record.monotonicUs = MonotonicMicros();
	record.durationUs = durationUs;
	record.threadId = CurrentThreadNumber();
	record.workClass = CurrentPerfContext().workClass;
	record.execution = CurrentPerfContext().execution;
	record.values[0] = a;
	record.values[1] = b;
	record.values[2] = c;
	record.values[3] = d;
	record.values[4] = e;
	record.values[5] = f;
	record.metric = metric;
	state_->Push(record);
}

void PerfDiagnostics::RecordText(PerfMetric metric, std::uint64_t a,
	std::uint64_t b, std::uint64_t c, std::uint64_t d,
	std::uint64_t e, std::uint64_t f, const char* text) {
	RecordText(metric, 0, a, b, c, d, e, f, text);
}

void PerfDiagnostics::RecordText(PerfMetric metric, std::uint64_t durationUs,
	std::uint64_t a, std::uint64_t b, std::uint64_t c,
	std::uint64_t d, std::uint64_t e, std::uint64_t f, const char* text) {
	if (state_ == nullptr) return;
	TraceRecord record;
	record.monotonicUs = MonotonicMicros();
	record.durationUs = durationUs;
	record.threadId = CurrentThreadNumber();
	record.metric = metric;
	record.workClass = CurrentPerfContext().workClass;
	record.execution = CurrentPerfContext().execution;
	record.values[0] = a;
	record.values[1] = b;
	record.values[2] = c;
	record.values[3] = d;
	record.values[4] = e;
	record.values[5] = f;
	if (text != nullptr) {
		std::strncpy(record.text, text, sizeof(record.text) - 1);
		record.text[sizeof(record.text) - 1] = '\0';
	}
	state_->Push(record);
}

void PerfDiagnostics::MarkInput() {
	if (state_ == nullptr) return;
	pendingInput_.Mark(MonotonicMicros());
}

void PerfDiagnostics::RecordPresentation() {
	if (state_ == nullptr) return;
	const std::uint64_t now = MonotonicMicros();
	const std::uint64_t latency = pendingInput_.TakeLatency(now);
	if (latency != 0) Record(PerfMetric::InputToPresent, latency);
}

PerfScopedTimer::PerfScopedTimer(PerfDiagnostics& diagnostics,
	PerfMetric metric, std::uint64_t a, std::uint64_t b, std::uint64_t c)
	: diagnostics_(diagnostics), metric_(metric), start_(diagnostics.Begin()),
	  a_(a), b_(b), c_(c) {}

PerfScopedTimer::~PerfScopedTimer() {
	diagnostics_.End(metric_, start_, a_, b_, c_);
}

} // namespace jpegview_linux
