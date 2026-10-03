#include "image_spectrum_worker.h"

#include "event_loop_model.h"
#include "perf_diagnostics.h"
#include "source_work_coordinator.h"

#include <atomic>
#include <exception>
#include <limits>
#include <utility>

namespace jpegview_linux {

bool operator==(const ImageSpectrumKey& left, const ImageSpectrumKey& right) {
	return left.source == right.source &&
		left.documentRevision == right.documentRevision &&
		left.frameIndex == right.frameIndex &&
		EqualEffectiveImageProcessingParams(left.processing, right.processing) &&
		left.autoContrast == right.autoContrast &&
		left.rotationQuarterTurns == right.rotationQuarterTurns;
}

bool operator!=(const ImageSpectrumKey& left, const ImageSpectrumKey& right) {
	return !(left == right);
}

bool IsCurrentImageSpectrumResult(const ImageSpectrumResult& result,
	std::uint64_t expectedGeneration, const ImageSpectrumKey& expectedKey) {
	return result.generation == expectedGeneration && result.key == expectedKey;
}

ImageSpectrumWorker::ImageSpectrumWorker(Computer computer)
	: computer_(std::move(computer)) {
	if (!computer_) {
		computer_ = [](const Image& image, const std::function<bool()>& shouldContinue) {
			return TryBuildGrayscaleSpectrum(image.bgra, image.width, image.height,
				shouldContinue);
		};
	}
	worker_ = std::thread([this] { Run(); });
}

ImageSpectrumWorker::~ImageSpectrumWorker() {
	Stop();
}

bool ImageSpectrumWorker::HasPixels(const Image& image) {
	if (image.width <= 0 || image.height <= 0) return false;
	const std::size_t width = static_cast<std::size_t>(image.width);
	const std::size_t height = static_cast<std::size_t>(image.height);
	return width <= std::numeric_limits<std::size_t>::max() / height / 4 &&
		image.bgra.size() >= width * height * 4;
}

std::uint64_t ImageSpectrumWorker::Request(const Image& image, ImageSpectrumKey key) {
	if (!key.Valid() || !HasPixels(image)) return 0;
	std::shared_ptr<std::atomic<bool>> canceled;
	ImageSpectrumKey stateKey;
	ImageSpectrumKey workKey;
	try {
		canceled = std::make_shared<std::atomic<bool>>(false);
		stateKey = key;
		workKey = std::move(key);
	} catch (...) {
		return 0;
	}
	std::uint64_t requestedGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_) return 0;
		if (currentKey_.has_value() && *currentKey_ == stateKey) return generation_;
		++generation_;
		if (activeCancellation_) activeCancellation_->store(true, std::memory_order_relaxed);
		if (pending_ && pending_->canceled) {
			pending_->canceled->store(true, std::memory_order_relaxed);
		}
		pending_.reset();
		ready_.reset();
		currentKey_ = std::move(stateKey);
		requestedGeneration = generation_;
		pending_ = Work{&image, std::move(workKey), requestedGeneration,
			std::move(canceled)};
		available_.notify_one();
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
	return requestedGeneration;
}

void ImageSpectrumWorker::Cancel() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_) return;
		++generation_;
		currentKey_.reset();
		if (activeCancellation_) activeCancellation_->store(true, std::memory_order_relaxed);
		if (pending_ && pending_->canceled) {
			pending_->canceled->store(true, std::memory_order_relaxed);
		}
		pending_.reset();
		ready_.reset();
		if (!active_) idle_.notify_all();
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
}

void ImageSpectrumWorker::CancelAndWait() {
	Cancel();
	std::unique_lock<std::mutex> lock(mutex_);
	idle_.wait(lock, [this] { return !active_ && !pending_.has_value(); });
}

void ImageSpectrumWorker::Stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!stopping_) {
			stopping_ = true;
			++generation_;
			currentKey_.reset();
			if (activeCancellation_) activeCancellation_->store(true, std::memory_order_relaxed);
			if (pending_ && pending_->canceled) {
				pending_->canceled->store(true, std::memory_order_relaxed);
			}
			pending_.reset();
			ready_.reset();
			available_.notify_all();
			idle_.notify_all();
		}
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
	if (worker_.joinable()) worker_.join();
}

std::optional<ImageSpectrumResult> ImageSpectrumWorker::TakeReady() {
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<ImageSpectrumResult> result = std::move(ready_);
	ready_.reset();
	return result;
}

bool ImageSpectrumWorker::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(mutex_);
	return idle_.wait_for(lock, timeout,
		[this] { return !active_ && !pending_.has_value(); });
}

std::uint64_t ImageSpectrumWorker::Generation() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return generation_;
}

bool ImageSpectrumWorker::IsPendingFor(const ImageSpectrumKey& key) const {
	std::lock_guard<std::mutex> lock(mutex_);
	return currentKey_.has_value() && *currentKey_ == key &&
		(active_ || pending_.has_value() || ready_.has_value());
}

bool ImageSpectrumWorker::IsCurrent(const Work& work) const {
	if (!work.canceled || work.canceled->load(std::memory_order_relaxed)) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	return !stopping_ && work.generation == generation_ &&
		currentKey_.has_value() && *currentKey_ == work.key;
}

void ImageSpectrumWorker::Run() {
	for (;;) {
		Work work;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			available_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
			if (stopping_) return;
			work = std::move(*pending_);
			pending_.reset();
			activeCancellation_ = work.canceled;
			active_ = true;
		}

		ImageSpectrumResult result;
		result.generation = work.generation;
		PerfContextScope perfContext(PerfWorkClass::ActiveImageSpread,
			PerfExecution::WorkerThread);
		try {
			result.key = work.key;
			WorkContext context;
			context.source = work.key.source;
			context.sourcePriority = SourceWorkPriority::Foreground;
			context.shouldContinue = [this, &work] { return IsCurrent(work); };
			CpuWorkLease cpu = SourceWorkCoordinator::Global().AcquireCpu(context);
			if (!cpu) {
				if (IsCurrent(work)) {
					result.failure = {WorkerFailureKind::Cancelled,
						"histogram CPU admission was cancelled"};
				}
			} else if (IsCurrent(work)) {
				PerfScopedTimer timer(PerfDiagnostics::Instance(), PerfMetric::Processing);
				result.spectrum = computer_(*work.image, context.shouldContinue);
				if (!result.spectrum && IsCurrent(work)) {
					result.failure = {WorkerFailureKind::ProcessingFailed,
						"image histogram could not be computed"};
				}
			}
		} catch (const std::exception& error) {
			result.failure = {WorkerFailureKind::Exception, error.what()};
		} catch (...) {
			result.failure = {WorkerFailureKind::Exception,
				"unknown image histogram worker failure"};
		}

		bool published = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			active_ = false;
			activeCancellation_.reset();
			if (!stopping_ && work.canceled &&
				!work.canceled->load(std::memory_order_relaxed) &&
				work.generation == generation_ && currentKey_.has_value() &&
				*currentKey_ == work.key && (result.spectrum.has_value() ||
					result.failure.Failed())) {
				try {
					ready_ = std::move(result);
					published = ready_.has_value();
				} catch (...) {
					ready_.reset();
				}
			}
			idle_.notify_all();
		}
		if (published) UiCompletionWakeup().Notify();
	}
}

} // namespace jpegview_linux
