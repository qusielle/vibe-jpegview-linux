#pragma once

#include "cache_budget.h"
#include "image_document.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace jpegview_linux {

class ImageOperationWorker {
public:
	using Processor = std::function<ImageOperationResult(const ImageOperationRequest&,
		const std::function<bool()>&, SharedCacheBudget&)>;

	explicit ImageOperationWorker(std::shared_ptr<SharedCacheBudget> budget,
		Processor processor = {});
	~ImageOperationWorker();
	ImageOperationWorker(const ImageOperationWorker&) = delete;
	ImageOperationWorker& operator=(const ImageOperationWorker&) = delete;

	std::uint64_t Request(ImageOperationRequest request);
	void Cancel();
	void Stop();
	std::optional<ImageOperationResult> TakeReady();
	void Retire(ImageOperationResult&& result);
	void Retire(RetiredImageBuffers&& buffers);
	bool WaitUntilIdle(std::chrono::milliseconds timeout);
	std::uint64_t Generation() const;

private:
	struct Work {
		ImageOperationRequest request;
		std::uint64_t generation = 0;
		std::shared_ptr<std::atomic<bool>> canceled;
	};

	bool IsCurrent(const Work& work) const;
	void Run();
	void RetireResultBuffers(ImageOperationResult& result);
	void RetireBuffers(RetiredImageBuffers& buffers);

	std::shared_ptr<SharedCacheBudget> budget_;
	Processor processor_;
	mutable std::mutex mutex_;
	std::condition_variable available_;
	std::condition_variable idle_;
	std::thread worker_;
	std::optional<Work> pending_;
	std::deque<ImageOperationResult> retiredResults_;
	std::deque<RetiredImageBuffers> retiredBuffers_;
	std::optional<ImageOperationResult> ready_;
	std::shared_ptr<std::atomic<bool>> activeCancellation_;
	std::uint64_t generation_ = 0;
	bool active_ = false;
	bool retiring_ = false;
	bool stopping_ = false;
};

ImageOperationResult ProcessImageOperation(const ImageOperationRequest& request,
	const std::function<bool()>& shouldContinue, SharedCacheBudget& budget);

} // namespace jpegview_linux
