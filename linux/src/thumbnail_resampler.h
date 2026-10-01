#pragma once

#include "display_image_cache.h"
#include "perf_diagnostics.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jpegview_linux {

// Downscales straight-alpha BGRA pixels with a source-area box filter. Every
// source pixel covered by a destination pixel contributes proportionally,
// preventing high-frequency details from aliasing in the thumbnail panel.
// Color channels are accumulated with premultiplied alpha to avoid colored
// fringes around transparent image edges.
bool DownsampleThumbnailBgra(const std::vector<std::uint8_t>& source,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	std::vector<std::uint8_t>& target);

// Bounds the display-sized source retained while a background thumbnail is
// derived, including overflow-safe handling of invalid dimensions.
bool CanReuseDisplayPixelsForThumbnail(int width, int height,
	std::size_t maximumPixels);

struct ThumbnailPreparationRequest {
	std::string key;
	DisplayImageCache::ImagePtr source;
	int maximumWidth = 0;
	int maximumHeight = 0;
	std::size_t priority = 0;
	PerfWorkClass workClass = PerfWorkClass::VisibleThumbnail;
	std::filesystem::path logicalSource;
	std::uint64_t requestGeneration = 0;
	std::uint64_t fileListIdentity = 0;

	bool Valid() const;
};

struct PreparedThumbnailImage {
	std::string key;
	int width = 0;
	int height = 0;
	bool hasTransparency = false;
	PerfWorkClass workClass = PerfWorkClass::VisibleThumbnail;
	std::vector<std::uint8_t> bgra;
};

struct ThumbnailPreparationResult {
	std::string key;
	std::uint64_t requestGeneration = 0;
	std::uint64_t fileListIdentity = 0;
	int maximumWidth = 0;
	int maximumHeight = 0;
	PerfWorkClass workClass = PerfWorkClass::VisibleThumbnail;
	std::shared_ptr<const PreparedThumbnailImage> image;
};

// Verifies that a completed result still belongs to the active preload plan.
bool ThumbnailPreparationResultMatches(const ThumbnailPreparationResult& result,
	std::uint64_t requestGeneration, std::uint64_t fileListIdentity,
	const std::string& sourceKey, int maximumWidth, int maximumHeight);

struct ThumbnailPreparationDiagnostics {
	std::size_t queued = 0;
	std::size_t active = 0;
	std::size_t completedBytes = 0;
	std::size_t completedImages = 0;
	std::size_t retiredSourceBytes = 0;
	std::size_t retiredSources = 0;
	std::size_t retainedSourceBytes = 0;
};

// Prepares thumbnails from display-ready neighbors or file sources on one
// very-low-priority worker. The bounded queue protects the display cache from
// accumulating large source buffers when thumbnail preparation falls behind.
class ThumbnailPreparationWorker {
public:
	using ImagePtr = std::shared_ptr<const PreparedThumbnailImage>;
	using Result = ThumbnailPreparationResult;
	using Processor = std::function<ImagePtr(const ThumbnailPreparationRequest&)>;

	explicit ThumbnailPreparationWorker(Processor processor = {});
	~ThumbnailPreparationWorker();
	ThumbnailPreparationWorker(const ThumbnailPreparationWorker&) = delete;
	ThumbnailPreparationWorker& operator=(const ThumbnailPreparationWorker&) = delete;

	bool Request(const ThumbnailPreparationRequest& request);
	std::vector<Result> TakeCompleted(std::size_t maximumCount);
	void Clear();
	bool HasPendingWork() const;
	ThumbnailPreparationDiagnostics GetDiagnostics() const;
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
