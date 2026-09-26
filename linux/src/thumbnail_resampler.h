#pragma once

#include "display_image_cache.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
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

	bool Valid() const;
};

struct PreparedThumbnailImage {
	std::string key;
	int width = 0;
	int height = 0;
	bool hasTransparency = false;
	std::vector<std::uint8_t> bgra;
};

// Derives thumbnails from display-ready neighbors on one very-low-priority
// worker. This reuses pixels already decoded for navigation and never performs
// file I/O. The bounded queue protects the display cache from accumulating
// large source buffers when thumbnail preparation falls behind.
class ThumbnailPreparationWorker {
public:
	using ImagePtr = std::shared_ptr<const PreparedThumbnailImage>;
	using Processor = std::function<ImagePtr(const ThumbnailPreparationRequest&)>;

	explicit ThumbnailPreparationWorker(Processor processor = {});
	~ThumbnailPreparationWorker();
	ThumbnailPreparationWorker(const ThumbnailPreparationWorker&) = delete;
	ThumbnailPreparationWorker& operator=(const ThumbnailPreparationWorker&) = delete;

	bool Request(const ThumbnailPreparationRequest& request);
	std::vector<ImagePtr> TakeCompleted(std::size_t maximumCount);
	void Clear();
	bool HasPendingWork() const;
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
