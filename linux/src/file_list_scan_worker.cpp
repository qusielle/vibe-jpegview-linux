#include "file_list_scan_worker.h"
#include "event_loop_model.h"
#include "source_work_coordinator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <utility>
#include <unistd.h>

namespace jpegview_linux {

struct FileListScanWorker::Impl {
	struct Task {
		FileList::ScanRequest request;
		std::uint64_t generation = 0;
	};

	~Impl() {
		Stop();
	}

	void Stop() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping.store(true);
			generation.fetch_add(1);
		}
		condition.notify_one();
		SourceWorkCoordinator::Global().NotifyWaiters();
		if (worker.joinable()) worker.join();
	}

	bool IsCurrent(std::uint64_t requestedGeneration) const {
		return !stopping.load() && generation.load() == requestedGeneration;
	}

	bool ShouldContinue(std::uint64_t requestedGeneration,
		bool& interruptedForForeground) {
		if (!IsCurrent(requestedGeneration)) return false;
		if (foregroundPending.load() ||
			SourceWorkCoordinator::Global().Snapshot().foregroundPending) {
			interruptedForForeground = true;
			yieldingForForeground.store(true);
			return false;
		}
		yieldingForForeground.store(false);
		return IsCurrent(requestedGeneration);
	}

	void Run() {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
		while (true) {
			Task task;
			std::vector<FileListScanResult> releaseResults;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] {
					return stopping.load() || pending.has_value() || !retiredResults.empty();
				});
				if (stopping.load()) {
					pending.reset();
					ready.reset();
					releaseResults.swap(retiredResults);
					lock.unlock();
					return;
				}
				if (!retiredResults.empty()) {
					releaseResults.swap(retiredResults);
				} else if (pending) {
					task = std::move(*pending);
					pending.reset();
				}
			}
			if (!releaseResults.empty()) continue;

			FileListScanResult result;
			result.generation = task.generation;
			for (;;) {
				bool interruptedForForeground = false;
				const auto shouldContinue = [this,
					requestedGeneration = task.generation, &interruptedForForeground] {
					return ShouldContinue(requestedGeneration, interruptedForForeground);
				};
				WorkContext workContext;
				workContext.sourcePriority = SourceWorkPriority::Metadata;
				workContext.shouldContinue = shouldContinue;
				workContext.onForegroundYield = [this, &interruptedForForeground] {
					interruptedForForeground = true;
					yieldingForForeground.store(true);
				};
				try {
					ScopedWorkContext activeContext(workContext);
					result.prepared = FileList::PrepareScan(task.request, shouldContinue);
				} catch (const std::exception& error) {
					result.prepared.operation = task.request.operation;
					result.prepared.expectedRevision = task.request.expectedRevision;
					result.error = error.what();
				} catch (...) {
					result.prepared.operation = task.request.operation;
					result.prepared.expectedRevision = task.request.expectedRevision;
					result.error = "unknown directory scan error";
				}
				if (!result.error.empty() || !IsCurrent(task.generation)) {
					yieldingForForeground.store(false);
					break;
				}
				if (!interruptedForForeground && result.prepared.completed) break;
				if (!interruptedForForeground) break;
				yieldingForForeground.store(true);
				(void)SourceWorkCoordinator::Global().WaitForSnapshot(
					[this, requestedGeneration = task.generation](
						const SourceWorkSnapshot& snapshot) {
						return !IsCurrent(requestedGeneration) || !snapshot.foregroundPending;
					}, std::chrono::hours(24));
				yieldingForForeground.store(false);
				if (!IsCurrent(task.generation)) break;
				result = {};
				result.generation = task.generation;
			}
			if (!IsCurrent(task.generation) ||
				(!result.prepared.completed && result.error.empty())) continue;

			bool published = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (IsCurrent(task.generation)) {
					ready = std::move(result);
					published = true;
				}
			}
			if (published) UiCompletionWakeup().Notify();
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::optional<Task> pending;
	std::optional<FileListScanResult> ready;
	std::vector<FileListScanResult> retiredResults;
	std::atomic<std::uint64_t> generation{0};
	std::atomic<bool> stopping{false};
	std::atomic<bool> foregroundPending{false};
	std::atomic<bool> yieldingForForeground{false};
	std::thread worker;
};

FileListScanWorker::FileListScanWorker() : impl_(std::make_unique<Impl>()) {}
FileListScanWorker::~FileListScanWorker() = default;

void FileListScanWorker::Stop() {
	impl_->Stop();
}

std::uint64_t FileListScanWorker::Request(FileList::ScanRequest request) {
	std::uint64_t nextGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->stopping.load()) return 0;
		nextGeneration = impl_->generation.fetch_add(1) + 1;
		if (impl_->ready) impl_->retiredResults.push_back(std::move(*impl_->ready));
		impl_->ready.reset();
		impl_->pending = Impl::Task{std::move(request), nextGeneration};
		impl_->StartWorkerLocked();
	}
	impl_->condition.notify_one();
	SourceWorkCoordinator::Global().NotifyWaiters();
	return nextGeneration;
}

void FileListScanWorker::Clear() {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->generation.fetch_add(1);
		impl_->pending.reset();
		if (impl_->ready) impl_->retiredResults.push_back(std::move(*impl_->ready));
		impl_->ready.reset();
	}
	impl_->condition.notify_one();
	SourceWorkCoordinator::Global().NotifyWaiters();
}

void FileListScanWorker::SetForegroundPending(bool pending) {
	SourceWorkCoordinator& coordinator = SourceWorkCoordinator::Global();
	if (pending) {
		coordinator.SetForegroundPending(true);
		impl_->foregroundPending.store(true);
	} else {
		impl_->foregroundPending.store(false);
		coordinator.SetForegroundPending(false);
	}
}

bool FileListScanWorker::IsYieldingForForeground() const {
	return impl_->yieldingForForeground.load();
}

std::vector<FileListScanResult> FileListScanWorker::TakeReady() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<FileListScanResult> results;
	if (impl_->ready.has_value()) {
		results.push_back(std::move(*impl_->ready));
		impl_->ready.reset();
	}
	return results;
}

} // namespace jpegview_linux
