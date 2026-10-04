#include "file_list_sort_worker.h"

#include "event_loop_model.h"
#include "source_work_coordinator.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace jpegview_linux {

struct FileListSortWorker::Impl {
	struct Task {
		FileListSortRequest request;
		std::uint64_t generation = 0;
	};

	~Impl() { Stop(); }

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

	void Run() {
		while (true) {
			Task task;
			std::optional<Task> abandonedPending;
			std::optional<FileListSortResult> abandonedReady;
			std::vector<Task> releaseTasks;
			std::vector<FileListPreparedSort> releaseResults;
			std::vector<std::shared_ptr<const void>> releaseStorage;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] {
					return stopping.load() || pending.has_value() || !retiredTasks.empty() ||
						!retiredResults.empty() || !retiredStorage.empty();
				});
				if (stopping.load()) {
					if (pending) abandonedPending = std::move(pending);
					pending.reset();
					if (ready) abandonedReady = std::move(ready);
					ready.reset();
					releaseTasks.swap(retiredTasks);
					releaseResults.swap(retiredResults);
					releaseStorage.swap(retiredStorage);
					lock.unlock();
					return;
				}
				if (!retiredTasks.empty() || !retiredResults.empty() || !retiredStorage.empty()) {
					releaseTasks.swap(retiredTasks);
					releaseResults.swap(retiredResults);
					releaseStorage.swap(retiredStorage);
				} else if (pending) {
					task = std::move(*pending);
					pending.reset();
				}
			}
			if (!releaseTasks.empty() || !releaseResults.empty() || !releaseStorage.empty()) {
				continue;
			}

			FileListSortResult result;
			result.generation = task.generation;
			WorkContext context;
			context.sourcePriority = SourceWorkPriority::Foreground;
			context.shouldContinue = [this, requestedGeneration = task.generation] {
				return IsCurrent(requestedGeneration);
			};
			try {
				CpuWorkLease cpu = SourceWorkCoordinator::Global().AcquireCpu(context);
				if (cpu && context.Continue()) {
					result.prepared = FileList::PrepareSort(task.request,
						context.shouldContinue);
				}
			} catch (const std::exception& error) {
				result.error = error.what();
			} catch (...) {
				result.error = "unknown file-list sorting error";
			}
			if (!IsCurrent(task.generation) ||
				(!result.prepared.completed && result.error.empty())) continue;

			bool published = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (IsCurrent(task.generation)) {
					if (!ready) {
						ready = std::move(result);
						published = true;
					}
				}
			}
			if (published) UiCompletionWakeup().Notify();
		}
	}

	void RetirePrepared(FileListPreparedSort&& prepared) {
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping.load()) return;
		retiredResults.push_back(std::move(prepared));
		StartWorkerLocked();
		condition.notify_one();
	}

	void RetireStorage(std::shared_ptr<const void> storage) {
		if (!storage) return;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping.load()) return;
			retiredStorage.push_back(std::move(storage));
			StartWorkerLocked();
		}
		condition.notify_one();
	}

	void Clear() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			generation.fetch_add(1);
			if (pending) retiredTasks.push_back(std::move(*pending));
			pending.reset();
			if (ready) retiredResults.push_back(std::move(ready->prepared));
			ready.reset();
			StartWorkerLocked();
		}
		condition.notify_one();
		SourceWorkCoordinator::Global().NotifyWaiters();
	}

	std::uint64_t Request(FileListSortRequest request) {
		std::uint64_t nextGeneration = 0;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping.load()) return 0;
			nextGeneration = generation.fetch_add(1) + 1;
			if (pending) retiredTasks.push_back(std::move(*pending));
			if (ready) retiredResults.push_back(std::move(ready->prepared));
			ready.reset();
			pending = Task{std::move(request), nextGeneration};
			StartWorkerLocked();
		}
		condition.notify_one();
		SourceWorkCoordinator::Global().NotifyWaiters();
		return nextGeneration;
	}

	void StartWorkerLocked() {
		if (!worker.joinable() && !stopping.load()) worker = std::thread([this] { Run(); });
	}

	std::vector<FileListSortResult> TakeReady() {
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<FileListSortResult> results;
		if (ready) {
			results.push_back(std::move(*ready));
			ready.reset();
		}
		return results;
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::optional<Task> pending;
	std::optional<FileListSortResult> ready;
	std::vector<Task> retiredTasks;
	std::vector<FileListPreparedSort> retiredResults;
	std::vector<std::shared_ptr<const void>> retiredStorage;
	std::atomic<std::uint64_t> generation{0};
	std::atomic<bool> stopping{false};
	std::thread worker;
};

FileListSortWorker::FileListSortWorker() : impl_(std::make_unique<Impl>()) {}
FileListSortWorker::~FileListSortWorker() = default;

void FileListSortWorker::Stop() { impl_->Stop(); }

std::uint64_t FileListSortWorker::Request(FileListSortRequest request) {
	return impl_->Request(std::move(request));
}

void FileListSortWorker::Clear() { impl_->Clear(); }

std::vector<FileListSortResult> FileListSortWorker::TakeReady() {
	return impl_->TakeReady();
}

void FileListSortWorker::Retire(FileListPreparedSort&& prepared) {
	impl_->RetirePrepared(std::move(prepared));
}

void FileListSortWorker::Retire(std::shared_ptr<const void> storage) {
	impl_->RetireStorage(std::move(storage));
}

} // namespace jpegview_linux
