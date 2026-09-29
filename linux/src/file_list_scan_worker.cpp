#include "file_list_scan_worker.h"

#include <atomic>
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
		stopping.store(true);
		generation.fetch_add(1);
		condition.notify_one();
		if (worker.joinable()) worker.join();
	}

	bool IsCurrent(std::uint64_t requestedGeneration) const {
		return !stopping.load() && generation.load() == requestedGeneration;
	}

	void Run() {
		(void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
		while (!stopping.load()) {
			Task task;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] { return stopping.load() || pending.has_value(); });
				if (stopping.load()) return;
				task = std::move(*pending);
				pending.reset();
			}

			FileListScanResult result;
			result.generation = task.generation;
			try {
				result.prepared = FileList::PrepareScan(task.request,
					[this, requestedGeneration = task.generation] {
						return IsCurrent(requestedGeneration);
					});
			} catch (const std::exception& error) {
				result.prepared.operation = task.request.operation;
				result.prepared.expectedRevision = task.request.expectedRevision;
				result.error = error.what();
			} catch (...) {
				result.prepared.operation = task.request.operation;
				result.prepared.expectedRevision = task.request.expectedRevision;
				result.error = "unknown directory scan error";
			}
			if (!IsCurrent(task.generation) ||
				(!result.prepared.completed && result.error.empty())) continue;

			std::lock_guard<std::mutex> lock(mutex);
			if (IsCurrent(task.generation)) ready = std::move(result);
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::optional<Task> pending;
	std::optional<FileListScanResult> ready;
	std::atomic<std::uint64_t> generation{0};
	std::atomic<bool> stopping{false};
	std::thread worker;
};

FileListScanWorker::FileListScanWorker() : impl_(std::make_unique<Impl>()) {}
FileListScanWorker::~FileListScanWorker() = default;

std::uint64_t FileListScanWorker::Request(FileList::ScanRequest request) {
	std::uint64_t nextGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		nextGeneration = impl_->generation.fetch_add(1) + 1;
		impl_->ready.reset();
		impl_->pending = Impl::Task{std::move(request), nextGeneration};
		impl_->StartWorkerLocked();
	}
	impl_->condition.notify_one();
	return nextGeneration;
}

void FileListScanWorker::Clear() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->generation.fetch_add(1);
	impl_->pending.reset();
	impl_->ready.reset();
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
