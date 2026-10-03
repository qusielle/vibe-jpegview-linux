#include "exif_metadata_worker.h"

#include "perf_diagnostics.h"
#include "source_work_coordinator.h"

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
		if (!reader) {
			reader = [](const std::filesystem::path& filename, ExifInfo& metadata,
				std::string& comment) {
				return ReadJpegMetadata(filename, metadata, comment);
			};
		}
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
			bool sourceCurrentForPublication = false;
			bool sourceUnavailableFailure = false;
			bool exceptionFailure = false;
			for (;;) {
				if (!IsCurrent(work)) break;
				const auto foregroundYielded =
					std::make_shared<std::atomic<bool>>(false);
				const auto cancellation = work.canceled;
				WorkContext context = MakeWorkContext(work.source,
					SourceWorkPriority::Metadata, [cancellation, foregroundYielded] {
						if (cancellation->load(std::memory_order_relaxed)) return false;
						if (SourceWorkCoordinator::Global().Snapshot().foregroundPending) {
							foregroundYielded->store(true, std::memory_order_relaxed);
							return false;
						}
						return true;
					});
				context.onForegroundYield = [foregroundYielded] {
					foregroundYielded->store(true, std::memory_order_relaxed);
				};
				bool retryAfterForeground = false;
				try {
					SourceCpuWorkLease admission =
						SourceWorkCoordinator::Global().AcquireSourceAndCpu(
							context, work.source.LogicalPath());
					const bool admitted = static_cast<bool>(admission);
					if (admitted) {
						context.sourcePriority = context.Priority();
						context.sourceAccessAlreadyAdmitted = true;
						context.cpuProcessingAlreadyAdmitted = true;
						{
							ScopedWorkContext activeContext(context);
							const auto sourceIsCurrent = [&] {
								if (!context.Continue()) return false;
								const bool current = sourceValidator(work.source);
								return context.Continue() && current;
							};
							if (!context.Continue()) {
								retryAfterForeground = foregroundYielded->load(
									std::memory_order_relaxed);
							} else if (!sourceIsCurrent()) {
								if (foregroundYielded->load(std::memory_order_relaxed)) {
									retryAfterForeground = true;
								} else if (!work.canceled->load(std::memory_order_relaxed)) {
									result.failure = {WorkerFailureKind::SourceUnavailable,
										"image source was unavailable before metadata was read"};
									sourceUnavailableFailure = true;
								}
							} else if (context.Continue()) {
								result.metadataAvailable = reader(work.source.LogicalPath(),
									result.metadata, result.jpegComment);
								if (foregroundYielded->load(std::memory_order_relaxed) ||
									!context.Continue()) {
									retryAfterForeground = foregroundYielded->load(
										std::memory_order_relaxed);
								} else if (!sourceIsCurrent()) {
									if (foregroundYielded->load(std::memory_order_relaxed)) {
										retryAfterForeground = true;
									} else if (!work.canceled->load(std::memory_order_relaxed)) {
										result.metadataAvailable = false;
										result.failure = {WorkerFailureKind::SourceUnavailable,
											"image source changed while metadata was read"};
										sourceUnavailableFailure = true;
									}
								} else {
									if (!sourceIsCurrent()) {
										if (foregroundYielded->load(std::memory_order_relaxed)) {
											retryAfterForeground = true;
										} else if (!work.canceled->load(
											std::memory_order_relaxed)) {
											result.metadataAvailable = false;
											result.failure = {WorkerFailureKind::SourceUnavailable,
												"image source changed before metadata could be published"};
											sourceUnavailableFailure = true;
										}
									} else {
										sourceCurrentForPublication = true;
									}
								}
							}
						}
						context.sourceAccessAlreadyAdmitted = false;
						context.cpuProcessingAlreadyAdmitted = false;
					}
					admission.Reset();
					if (!admitted && !work.canceled->load(std::memory_order_relaxed) &&
						!foregroundYielded->load(std::memory_order_relaxed)) {
						result.failure = {WorkerFailureKind::Cancelled,
							"metadata source admission was cancelled"};
					}
				} catch (const std::exception& error) {
					result.metadata = {};
					result.jpegComment.clear();
					result.metadataAvailable = false;
					result.failure = {WorkerFailureKind::Exception, error.what()};
					exceptionFailure = true;
				} catch (...) {
					result.metadata = {};
					result.jpegComment.clear();
					result.metadataAvailable = false;
					result.failure = {WorkerFailureKind::Exception,
						"unknown EXIF metadata worker failure"};
					exceptionFailure = true;
				}
				if (exceptionFailure || sourceUnavailableFailure) break;
				if (!IsCurrent(work)) break;
				if (!retryAfterForeground &&
					foregroundYielded->load(std::memory_order_relaxed)) {
					retryAfterForeground = true;
				}
				if (!retryAfterForeground) break;

				result.metadata = {};
				result.jpegComment.clear();
				result.metadataAvailable = false;
				result.failure = {};
				sourceCurrentForPublication = false;
				(void)SourceWorkCoordinator::Global().WaitForSnapshot(
					[this, &work](const SourceWorkSnapshot& snapshot) {
						return !IsCurrent(work) || !snapshot.foregroundPending;
					}, std::chrono::hours(24));
				if (!IsCurrent(work)) break;
			}
			if (work.canceled->load(std::memory_order_relaxed)) {
				result.metadataAvailable = false;
				result.failure = {WorkerFailureKind::Cancelled, "metadata request was cancelled"};
			} else if (!result.metadataAvailable && !result.failure.Failed()) {
				result.failure = {WorkerFailureKind::ProcessingFailed,
					"JPEG metadata was unavailable"};
			}
			if (!result.metadataAvailable) {
				result.metadata = {};
				result.jpegComment.clear();
			}

			{
				std::lock_guard<std::mutex> lock(mutex);
				active = false;
				activeCancellation.reset();
				if (!stopping && !work.canceled->load(std::memory_order_relaxed) &&
					work.generation == generation && result.source == work.source.Key() &&
					(sourceCurrentForPublication || exceptionFailure ||
						sourceUnavailableFailure)) {
					ready.clear();
					ready.push_back(std::move(result));
				}
				idle.notify_all();
			}
		}
	}

	std::uint64_t Request(SourceDescriptor source) {
		std::uint64_t requestedGeneration = 0;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping) return generation;
			requestedGeneration = ++generation;
			if (activeCancellation) activeCancellation->store(true, std::memory_order_relaxed);
			if (pending && pending->canceled) {
				pending->canceled->store(true, std::memory_order_relaxed);
			}
			pending.reset();
			ready.clear();
			if (!source.Valid()) {
				idle.notify_all();
			} else {
				pending = Work{std::move(source), generation,
					std::make_shared<std::atomic<bool>>(false)};
				available.notify_one();
			}
		}
		SourceWorkCoordinator::Global().NotifyWaiters();
		return requestedGeneration;
	}

	void Cancel() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			++generation;
			if (activeCancellation) activeCancellation->store(true, std::memory_order_relaxed);
			if (pending && pending->canceled) pending->canceled->store(true, std::memory_order_relaxed);
			pending.reset();
			ready.clear();
			idle.notify_all();
		}
		SourceWorkCoordinator::Global().NotifyWaiters();
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
		SourceWorkCoordinator::Global().NotifyWaiters();
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

	bool IsCurrent(const Work& work) {
		if (work.canceled->load(std::memory_order_relaxed)) return false;
		std::lock_guard<std::mutex> lock(mutex);
		return !stopping && work.generation == generation;
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
