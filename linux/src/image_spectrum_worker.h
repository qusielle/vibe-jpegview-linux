#pragma once

#include "archive_source.h"
#include "image.h"
#include "image_processing.h"
#include "spectrum_model.h"
#include "work_context.h"

#include <chrono>
#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace jpegview_linux {

struct ImageSpectrumKey {
	SourceKey source;
	std::uint64_t documentRevision = 0;
	std::size_t frameIndex = 0;
	ImageProcessingParams processing;
	bool autoContrast = false;
	int rotationQuarterTurns = 0;

	bool Valid() const { return source.Valid(); }
};

bool operator==(const ImageSpectrumKey& left, const ImageSpectrumKey& right);
bool operator!=(const ImageSpectrumKey& left, const ImageSpectrumKey& right);

struct ImageSpectrumResult {
	std::uint64_t generation = 0;
	ImageSpectrumKey key;
	std::optional<GrayscaleSpectrum> spectrum;
	WorkerFailure failure;
};

bool IsCurrentImageSpectrumResult(const ImageSpectrumResult& result,
	std::uint64_t expectedGeneration, const ImageSpectrumKey& expectedKey);

// Requests retain immutable image storage. Replacing a document cancels the
// generation; the worker drops its final image reference off the event thread.
class ImageSpectrumWorker {
public:
	using Computer = std::function<std::optional<GrayscaleSpectrum>(const Image&,
		const std::function<bool()>&)>;

	explicit ImageSpectrumWorker(Computer computer = {});
	~ImageSpectrumWorker();
	ImageSpectrumWorker(const ImageSpectrumWorker&) = delete;
	ImageSpectrumWorker& operator=(const ImageSpectrumWorker&) = delete;

	std::uint64_t Request(std::shared_ptr<const Image> image, ImageSpectrumKey key);
	void Cancel();
	void CancelAndWait();
	void Stop();
	std::optional<ImageSpectrumResult> TakeReady();
	bool WaitUntilIdle(std::chrono::milliseconds timeout);
	std::uint64_t Generation() const;
	bool IsPendingFor(const ImageSpectrumKey& key) const;

private:
	struct Work {
		std::shared_ptr<const Image> image;
		ImageSpectrumKey key;
		std::uint64_t generation = 0;
		std::shared_ptr<std::atomic<bool>> canceled;
	};

	void Run();
	bool IsCurrent(const Work& work) const;
	static bool HasPixels(const Image& image);

	Computer computer_;
	mutable std::mutex mutex_;
	std::condition_variable available_;
	std::condition_variable idle_;
	std::thread worker_;
	std::optional<Work> pending_;
	std::shared_ptr<std::atomic<bool>> activeCancellation_;
	std::optional<ImageSpectrumKey> currentKey_;
	std::optional<ImageSpectrumResult> ready_;
	std::uint64_t generation_ = 0;
	bool active_ = false;
	bool stopping_ = false;
};

} // namespace jpegview_linux
