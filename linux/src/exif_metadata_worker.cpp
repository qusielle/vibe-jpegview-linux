#include "exif_metadata_worker.h"

#include "perf_diagnostics.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace jpegview_linux {

bool IsCurrentExifMetadataResult(const ExifMetadataResult& result,
	std::uint64_t expectedGeneration, const SourceKey& expectedSource) {
	return result.generation == expectedGeneration && result.source == expectedSource;
}

struct ExifMetadataWorker::Impl {
	struct Work {
		SourceDescriptor source;
		std::uint64_t generation = 0;
		std::shared_ptr<std::atomic<bool>> canceled;
	};

	explicit Impl(Reader readMetadata, SourceValidator validateSource)
		: reader(std::move(readMetadata)), sourceValidator(std::move(validateSource)) {
		if (!reader) reader = ReadJpegMetadata;
		if (!sourceValidator) sourceValidator = IsImageSourceCurrent;
		worker = std::thread([this] { Run(); });
	}

	~Impl() { Stop(); }

	void Run() {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
		for (;;) {
			Work work;
			{
				std::unique_lock<std::mutex> lock(mutex);
				available.wait(lock, [this] { return stopping || pending.has_value(); });
				if (stopping) return;
				work = std::move(*pending);
				pending.reset();
				activeCancellation = work.canceled;
				active = true;
			}

			ExifMetadataResult result;
			result.generation = work.generation;
			result.source = work.source.Key();
			PerfContextScope workContext(PerfWorkClass::ActiveImageSpread,
				PerfExecution::WorkerThread);
			if (!work.canceled->load(std::memory_order_relaxed) &&
				sourceValidator(work.source)) {
				result.metadataAvailable = reader(work.source.LogicalPath(),
					result.metadata, result.jpegComment);
				if (!sourceValidator(work.source)) result.metadataAvailable = false;
			}

			// Source validation performs filesystem or archive I/O. Keep it outside
			// the mutex used by event-facing Request, Cancel, and TakeReady calls.
			const bool sourceCurrentForPublication =
				!work.canceled->load(std::memory_order_relaxed) &&
				sourceValidator(work.source);

			{
				std::lock_guard<std::mutex> lock(mutex);
				active = false;
				activeCancellation.reset();
				if (!stopping && !work.canceled->load(std::memory_order_relaxed) &&
					work.generation == generation && result.source == work.source.Key() &&
					sourceCurrentForPublication) {
					ready.clear();
					ready.push_back(std::move(result));
				}
				idle.notify_all();
			}
		}
	}

	std::uint64_t Request(SourceDescriptor source) {
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping) return generation;
		++generation;
		if (activeCancellation) activeCancellation->store(true, std::memory_order_relaxed);
		if (pending && pending->canceled) pending->canceled->store(true, std::memory_order_relaxed);
		pending.reset();
		ready.clear();
		if (!source.Valid()) {
			idle.notify_all();
			return generation;
		}
		pending = Work{std::move(source), generation,
			std::make_shared<std::atomic<bool>>(false)};
		available.notify_one();
		return generation;
	}

	void Cancel() {
		std::lock_guard<std::mutex> lock(mutex);
		++generation;
		if (activeCancellation) activeCancellation->store(true, std::memory_order_relaxed);
		if (pending && pending->canceled) pending->canceled->store(true, std::memory_order_relaxed);
		pending.reset();
		ready.clear();
		idle.notify_all();
	}

	void Stop() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping) return;
			stopping = true;
			++generation;
			if (activeCancellation) activeCancellation->store(true, std::memory_order_relaxed);
			pending.reset();
			ready.clear();
		}
		available.notify_all();
		if (worker.joinable()) worker.join();
	}

	std::vector<ExifMetadataResult> TakeReady() {
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<ExifMetadataResult> result;
		result.swap(ready);
		return result;
	}

	bool WaitUntilIdle(std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(mutex);
		return idle.wait_for(lock, timeout, [this] {
			return !pending.has_value() && !active;
		});
	}

	Reader reader;
	SourceValidator sourceValidator;
	std::mutex mutex;
	std::condition_variable available;
	std::condition_variable idle;
	std::thread worker;
	std::vector<ExifMetadataResult> ready;
	std::shared_ptr<std::atomic<bool>> activeCancellation;
	std::optional<Work> pending;
	std::uint64_t generation = 0;
	bool active = false;
	bool stopping = false;
};

ExifMetadataWorker::ExifMetadataWorker(Reader reader, SourceValidator sourceValidator)
	: impl_(std::make_unique<Impl>(std::move(reader), std::move(sourceValidator))) {}

ExifMetadataWorker::~ExifMetadataWorker() = default;

std::uint64_t ExifMetadataWorker::Request(SourceDescriptor source) {
	return impl_->Request(std::move(source));
}

void ExifMetadataWorker::Cancel() { impl_->Cancel(); }

void ExifMetadataWorker::Stop() { impl_->Stop(); }

std::vector<ExifMetadataResult> ExifMetadataWorker::TakeReady() {
	return impl_->TakeReady();
}

bool ExifMetadataWorker::WaitUntilIdle(std::chrono::milliseconds timeout) {
	return impl_->WaitUntilIdle(timeout);
}

} // namespace jpegview_linux
