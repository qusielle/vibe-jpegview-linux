#pragma once

#include "archive_source.h"
#include "image_decoder.h"
#include "cache_budget.h"
#include "image_processing.h"
#include "perf_diagnostics.h"

#include <chrono>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jpegview_linux {

// Exact identity for a renderer-ready bitmap. Optional processing values are
// normalized when this key is built, so disabled controls do not split cache
// entries while active pixel-affecting controls remain distinct.
struct DisplayImageCacheKey {
	SourceKey source;
	std::size_t frameIndex = 0;
	int targetWidth = 0;
	int targetHeight = 0;
	int rotationQuarterTurns = 0;
	bool autoContrast = false;
	ImageProcessingParams processing;

	bool Valid() const;
};

bool operator==(const DisplayImageCacheKey& left, const DisplayImageCacheKey& right);
bool operator!=(const DisplayImageCacheKey& left, const DisplayImageCacheKey& right);

struct DisplayImageCacheKeyHash {
	std::size_t operator()(const DisplayImageCacheKey& key) const;
};

// Identifies one renderer-ready bitmap. The key includes the source file
// identity as well as every setting that changes the resulting pixels.
struct DisplayImageRequest {
	std::filesystem::path filename;
	SourceDescriptor source;
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
	PerfWorkClass workClass = PerfWorkClass::Unspecified;
	DisplayImageCacheKey cacheKey;
	std::string key;
	std::shared_ptr<std::atomic<bool>> cancellation;
	WorkContext workContext;

	bool Valid() const;
};

struct PreparedDisplayImage {
	std::filesystem::path filename;
	SourceDescriptor source;
	DisplayImageCacheKey cacheKey;
	std::string key;
	int width = 0;
	int height = 0;
	bool hasTransparency = false;
	std::vector<std::uint8_t> bgra;
	std::size_t priority = 0;
	PerfWorkClass workClass = PerfWorkClass::Unspecified;
	int rotationQuarterTurns = 0;
};

// Carries the cache's current scheduling classification with a prepared frame.
// A frame can be promoted after preparation, so its payload fields may describe
// the original request rather than the work currently being scheduled.
struct DisplayImageCompletionInfo {
	std::shared_ptr<const PreparedDisplayImage> image;
	std::size_t priority = 0;
	PerfWorkClass workClass = PerfWorkClass::Unspecified;
};

// Captures the source identity at request time. An invalid request is returned
// for a missing file, invalid frame, or invalid target dimensions.
DisplayImageRequest MakeDisplayImageRequest(const std::filesystem::path& filename,
	const std::shared_ptr<const DecodedImage>& decoded, std::size_t frameIndex,
	int targetWidth, int targetHeight, bool autoContrast, std::size_t priority = 0,
	const ImageProcessingParams& processing = {}, int rotationQuarterTurns = 0);
DisplayImageRequest MakeDisplayImageRequest(const SourceDescriptor& source,
	const std::shared_ptr<const DecodedImage>& decoded, std::size_t frameIndex,
	int targetWidth, int targetHeight, bool autoContrast, std::size_t priority = 0,
	const ImageProcessingParams& processing = {}, int rotationQuarterTurns = 0);

DisplayImageRequest MakeJpegDisplayImageRequest(const std::filesystem::path& filename,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	bool autoContrast, std::size_t priority = 0,
	const ImageProcessingParams& processing = {}, int rotationQuarterTurns = 0);
DisplayImageRequest MakeJpegDisplayImageRequest(const SourceDescriptor& source,
	int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
	bool autoContrast, std::size_t priority = 0,
	const ImageProcessingParams& processing = {}, int rotationQuarterTurns = 0);

std::size_t PreparedDisplayImageBytes(const PreparedDisplayImage& image);

struct DisplayImageCacheDiagnostics {
	std::size_t cachedBytes = 0;
	std::size_t cachedImages = 0;
	std::size_t foregroundQueued = 0;
	std::size_t backgroundQueued = 0;
	std::size_t foregroundActive = 0;
	std::size_t backgroundActive = 0;
	std::size_t preparedBytes = 0;
	std::size_t preparedImages = 0;
	std::size_t speculativePreparedBytes = 0;
	std::size_t speculativePreparedImages = 0;
	std::size_t speculativeReservedBytes = 0;
	std::size_t speculativeReservedImages = 0;
	std::size_t borrowedBytes = 0;
	std::size_t borrowedImages = 0;
	std::size_t retiredBytes = 0;
	std::size_t retiredImages = 0;
	std::size_t activeRetiredBytes = 0;
	WorkerFailure lastWorkerFailure;
};

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
	static constexpr std::size_t kMaximumSpeculativeCompletions = 2;
	static constexpr std::size_t kMaximumSpeculativeCompletionBytes = 64u * 1024u * 1024u;

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
	// Removes queued/completed background work for a key and retires its active
	// background classification. In-flight work may finish, but its result is
	// discarded unless a later explicit request needs it; empty prefetch will not
	// revive it.
	void CancelBackground(const std::string& key);
	ImagePtr RequestAndWait(const DisplayImageRequest& request);
	void Prefetch(const std::vector<DisplayImageRequest>& requests);
	std::vector<ImagePtr> TakeCompleted(std::size_t maximumCount);
	std::vector<ImagePtr> TakeCompleted(std::size_t maximumCount,
		const std::set<PerfWorkClass>& permittedWorkClasses);
	std::vector<DisplayImageCompletionInfo> TakeCompletedWithMetadata(
		std::size_t maximumCount);
	std::vector<DisplayImageCompletionInfo> TakeCompletedWithMetadata(
		std::size_t maximumCount,
		const std::set<PerfWorkClass>& permittedWorkClasses);
	void Release(const std::string& key);
	// Removes the entry while tracking its CPU pixels as temporary upload staging
	// until the final shared owner releases them on the retirement worker.
	void ReleaseForUpload(const ImagePtr& image);
	// Transfers a set of prepared frames before renderer admission processes the
	// selected uploads, so unselected aliases release retained capacity in time.
	void ReleaseForUpload(const std::vector<ImagePtr>& images);
	void ReleaseForActiveUse(const ImagePtr& image);
	void Retire(const ImagePtr& image);
	void SetProtectionSnapshot(
		const std::vector<std::pair<DisplayImageCacheKey, CacheProtectionTier>>& protections);
	std::size_t EvictLeastRecentlyUsed(
		CacheProtectionTier maximumTier = CacheProtectionTier::Neighbor);
	void Clear();

	std::size_t CachedBytes() const;
	std::size_t CachedImages() const;
	DisplayImageCacheDiagnostics GetDiagnostics() const;
	std::vector<SourceChangeNotice> TakeChangedSources();
	bool HasPendingWork() const;
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// Owns every result returned by TakeCompleted until the renderer explicitly
// transfers it to a texture or a deferred-upload queue. Unclaimed pixels are
// retired by the cache worker when the batch leaves scope.
class DisplayImageCompletionBatch {
public:
	using ImagePtr = std::shared_ptr<const PreparedDisplayImage>;
	using CompletionInfo = DisplayImageCompletionInfo;

	DisplayImageCompletionBatch(DisplayImageCache& cache, std::vector<ImagePtr> images);
	DisplayImageCompletionBatch(DisplayImageCache& cache,
		std::vector<CompletionInfo> completions);
	~DisplayImageCompletionBatch();
	DisplayImageCompletionBatch(const DisplayImageCompletionBatch&) = delete;
	DisplayImageCompletionBatch& operator=(const DisplayImageCompletionBatch&) = delete;

	std::size_t Size() const;
	const ImagePtr& At(std::size_t index) const;
	const CompletionInfo& CompletionAt(std::size_t index) const;
	ImagePtr Take(std::size_t index);
	CompletionInfo TakeCompletion(std::size_t index);

private:
	DisplayImageCache& cache_;
	std::vector<CompletionInfo> completions_;
};

} // namespace jpegview_linux
