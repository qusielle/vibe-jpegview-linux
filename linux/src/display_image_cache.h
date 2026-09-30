#pragma once

#include "image_decoder.h"
#include "cache_budget.h"
#include "image_processing.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jpegview_linux {

// Identifies one renderer-ready bitmap. The key includes the source file
// identity as well as every setting that changes the resulting pixels.
struct DisplayImageRequest {
	std::filesystem::path filename;
	std::shared_ptr<const DecodedImage> decoded;
	std::size_t frameIndex = 0;
	int sourceWidth = 0;
	int sourceHeight = 0;
	int targetWidth = 0;
	int targetHeight = 0;
	int rotationQuarterTurns = 0;
	bool autoContrast = false;
	ImageProcessingParams processing;
	std::size_t priority = 0;
	std::string key;

	bool Valid() const;
};

struct PreparedDisplayImage {
	std::filesystem::path filename;
	std::string key;
	int width = 0;
	int height = 0;
	bool hasTransparency = false;
	std::vector<std::uint8_t> bgra;
	std::size_t priority = 0;
	int rotationQuarterTurns = 0;
};

// Captures the source identity at request time. An invalid request is returned
// for a missing file, invalid frame, or invalid target dimensions.
DisplayImageRequest MakeDisplayImageRequest(const std::filesystem::path& filename,
	const std::shared_ptr<const DecodedImage>& decoded, std::size_t frameIndex,
	int targetWidth, int targetHeight, bool autoContrast, std::size_t priority = 0,
	const ImageProcessingParams& processing = {}, int rotationQuarterTurns = 0);

DisplayImageRequest MakeJpegDisplayImageRequest(const std::filesystem::path& filename,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	bool autoContrast, std::size_t priority = 0,
	const ImageProcessingParams& processing = {}, int rotationQuarterTurns = 0);

std::size_t PreparedDisplayImageBytes(const PreparedDisplayImage& image);

// Conservative number of neighboring full-viewport textures that fit beside
// the current image. The cap prevents a very large configured budget from
// scheduling an unbounded directory in one speculative batch.
std::size_t DisplayPrefetchCount(std::size_t cacheBytes, int viewportWidth,
	int viewportHeight, std::size_t fileCount);

// Threaded CPU-side display-frame cache. Workers perform correction, copying,
// and high-quality scaling. SDL texture creation deliberately remains outside
// this class because SDL_Renderer is confined to its owning thread.
class DisplayImageCache {
public:
	using ImagePtr = std::shared_ptr<const PreparedDisplayImage>;
	using Processor = std::function<ImagePtr(const DisplayImageRequest&)>;

	explicit DisplayImageCache(std::size_t byteBudget,
		std::size_t workerCount = 0, Processor processor = {},
		std::shared_ptr<SharedCacheBudget> sharedBudget = {});
	~DisplayImageCache();

	DisplayImageCache(const DisplayImageCache&) = delete;
	DisplayImageCache& operator=(const DisplayImageCache&) = delete;

	ImagePtr Find(const DisplayImageRequest& request);
	// Reports whether this exact key is retained, queued, in-flight, or waiting
	// for renderer-thread consumption. A false result after a request was queued
	// means preparation failed or became obsolete.
	bool HasPendingOrCached(const std::string& key) const;
	void Request(const DisplayImageRequest& request);
	// Adds optional work without replacing the foreground request or the current
	// neighbor-prefetch set. Lower-priority work is scheduled after nearer items.
	void RequestBackground(const DisplayImageRequest& request);
	// Enqueues related background requests under one lock before waking workers.
	// This lets a multi-image presentation start both sides without a sequential
	// request-publication gap, while preserving ordinary foreground work.
	void RequestBackgroundBatch(const std::vector<DisplayImageRequest>& requests);
	// Removes queued/completed background work for a key. In-flight work is
	// allowed to finish but its result is discarded unless another request needs it.
	void CancelBackground(const std::string& key);
	ImagePtr RequestAndWait(const DisplayImageRequest& request);
	void Prefetch(const std::vector<DisplayImageRequest>& requests);
	std::vector<ImagePtr> TakeCompleted(std::size_t maximumCount);
	void Release(const std::string& key);
	void Retire(const ImagePtr& image);
	void Clear();

	std::size_t CachedBytes() const;
	std::size_t CachedImages() const;
	bool HasPendingWork() const;
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
