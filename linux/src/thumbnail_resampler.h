#pragma once

#include "display_image_cache.h"
#include "perf_diagnostics.h"

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
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
	std::vector<std::uint8_t>& target,
	const std::function<bool()>& shouldContinue = {});

// Bounds the display-sized source retained while a background thumbnail is
// derived, including overflow-safe handling of invalid dimensions.
bool CanReuseDisplayPixelsForThumbnail(int width, int height,
	std::size_t maximumPixels);

struct ThumbnailPreparationRequest {
	SourceKey key;
	DisplayImageCache::ImagePtr source;
	int maximumWidth = 0;
	int maximumHeight = 0;
	std::size_t priority = 0;
	PerfWorkClass workClass = PerfWorkClass::VisibleThumbnail;
	std::filesystem::path logicalSource;
	std::uint64_t catalogRevision = 0;
	std::uint64_t geometryRevision = 0;
	std::shared_ptr<std::atomic<bool>> cancellation;
	std::size_t fileIndex = 0;
	SourceDescriptor sourceDescriptor;

	bool Valid() const;
};

struct PreparedThumbnailImage {
	SourceKey key;
	SourceDescriptor sourceDescriptor;
	int width = 0;
	int height = 0;
	bool hasTransparency = false;
	PerfWorkClass workClass = PerfWorkClass::VisibleThumbnail;
	std::vector<std::uint8_t> bgra;
};

struct ThumbnailPreparationResult {
	SourceKey key;
	SourceDescriptor observedSource;
	std::size_t fileIndex = 0;
	std::uint64_t catalogRevision = 0;
	std::uint64_t geometryRevision = 0;
	int maximumWidth = 0;
	int maximumHeight = 0;
	PerfWorkClass workClass = PerfWorkClass::VisibleThumbnail;
	std::shared_ptr<const PreparedThumbnailImage> image;
	bool cancelled = false;
	WorkerFailure failure;
};

// One farther queued request may be displaced when an admitted request enters
// the bounded worker queue. The caller returns that identity to its scheduler.
struct ThumbnailPreparationAdmission {
	bool accepted = false;
	std::optional<ThumbnailPreparationResult> displaced;

	operator bool() const { return accepted; }
};

// Verifies that a completed result still belongs to the active preload plan.
bool ThumbnailPreparationResultMatches(const ThumbnailPreparationResult& result,
	std::uint64_t catalogRevision, std::uint64_t geometryRevision,
	std::size_t fileIndex, const SourceKey& sourceKey,
	int maximumWidth, int maximumHeight);
bool ThumbnailPreparationResultMatches(const ThumbnailPreparationResult& result,
	std::uint64_t catalogRevision, std::uint64_t geometryRevision,
	std::size_t fileIndex, const std::string& sourceKey,
	int maximumWidth, int maximumHeight);

// Keeps an allocation-failed completion available for a later retention
// attempt. A retry may be the same object already held by the optional.
void PreserveThumbnailPreparationRetry(
	std::optional<ThumbnailPreparationResult>& pending,
	ThumbnailPreparationResult& failedResult);

struct ThumbnailPreparationDiagnostics {
	std::size_t queued = 0;
	std::size_t active = 0;
	std::size_t completedResults = 0;
	std::size_t completedBytes = 0;
	std::size_t completedImages = 0;
	std::size_t reservedCompletionBytes = 0;
	std::size_t reservedCompletionImages = 0;
	std::size_t retiredSourceBytes = 0;
	std::size_t retiredSources = 0;
	std::size_t retiredThumbnailBytes = 0;
	std::size_t retiredThumbnailImages = 0;
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
	static constexpr std::size_t kMaximumCompletedResults = 2;
	static constexpr std::size_t kMaximumCompletedBytes = 16u * 1024u * 1024u;

	explicit ThumbnailPreparationWorker(Processor processor = {});
	~ThumbnailPreparationWorker();
	ThumbnailPreparationWorker(const ThumbnailPreparationWorker&) = delete;
	ThumbnailPreparationWorker& operator=(const ThumbnailPreparationWorker&) = delete;

	ThumbnailPreparationAdmission Request(const ThumbnailPreparationRequest& request);
	std::vector<Result> TakeCompleted(std::size_t maximumCount);
	std::vector<Result> TakeCompleted(std::size_t maximumCount,
		const std::set<PerfWorkClass>& permittedWorkClasses);
	std::vector<Result> Cancel(const std::set<PerfWorkClass>& workClasses);
	void Retire(const ImagePtr& image);
	void Clear();
	bool HasPendingWork() const;
	ThumbnailPreparationDiagnostics GetDiagnostics() const;
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
