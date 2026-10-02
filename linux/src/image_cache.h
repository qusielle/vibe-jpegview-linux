#pragma once

#include "archive_source.h"
#include "image_decoder.h"
#include "cache_budget.h"
#include "perf_diagnostics.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jpegview_linux {

// Cache identity for a decoded-image or header-only dimensions request.
// Current-image loads may take ownership of an active spread request with the
// same identity, so later spread replacement cannot cancel the promoted read.
struct DecodedImageRequestIdentity {
	SourceKey source;
	bool dimensionsOnly = false;
};

bool SameDecodedImageRequest(const DecodedImageRequestIdentity& left,
	const DecodedImageRequestIdentity& right);
bool CanTransferActiveSpreadRequestToCurrentImage(
	const DecodedImageRequestIdentity& spreadRequest,
	const DecodedImageRequestIdentity& currentImageRequest);
bool operator==(const DecodedImageRequestIdentity& left,
	const DecodedImageRequestIdentity& right);
bool operator!=(const DecodedImageRequestIdentity& left,
	const DecodedImageRequestIdentity& right);

// Pixel memory retained by a decoded image. Container bookkeeping is small
// compared with the BGRA buffers and is intentionally excluded from the
// configured cache budget.
std::size_t DecodedImageBytes(const DecodedImage& image);

// A JPEG header result remains usable across catalog/descriptor revisions
// while its load generation and backing source identity are still current.
bool IsCurrentJpegDimensionsResult(std::uint64_t resultGeneration,
	const SourceKey& resultSource, std::uint64_t expectedGeneration,
	const SourceKey& expectedSource);

struct DecodedImageCacheDiagnostics {
	std::size_t cachedBytes = 0;
	std::size_t cachedImages = 0;
	std::size_t foregroundQueued = 0;
	std::size_t backgroundQueued = 0;
	std::size_t foregroundActive = 0;
	std::size_t backgroundActive = 0;
	std::size_t activeSpreadQueued = 0;
	std::size_t activeSpreadActive = 0;
	std::size_t retiredBytes = 0;
	std::size_t retiredImages = 0;
};

// Returns neighboring indices in likely-use order. The preferred direction
// is visited first at each distance, and folder-loop wraparound is included.
std::vector<std::size_t> ImagePrefetchOrder(std::size_t fileCount,
	std::size_t currentIndex, int preferredDirection, std::size_t maximumCount);

// Thread-safe decoded-pixel LRU with a single background decoder. Cache hits
// avoid file decoding while texture creation remains on SDL's main thread.
class DecodedImageCache {
public:
	using ImagePtr = std::shared_ptr<const DecodedImage>;
	using Decoder = std::function<bool(const std::filesystem::path&, DecodedImage&, std::string&)>;
	using DimensionsReader = std::function<bool(const std::filesystem::path&, int&, int&,
		std::string&)>;
	// Called when this generation's requested decode completes. A null image
	// reports a decode failure or a result that could not be retained.
	using Completion = std::function<void(const std::filesystem::path&, const ImagePtr&)>;
	using DimensionsCompletion = std::function<void(const std::filesystem::path&,
		bool, int, int)>;
	using Filter = std::function<bool(const std::filesystem::path&)>;

	explicit DecodedImageCache(std::size_t byteBudget, Decoder decoder = {},
		std::shared_ptr<SharedCacheBudget> sharedBudget = {},
		std::size_t workerCount = 1, DimensionsReader dimensionsReader = {});
	~DecodedImageCache();

	DecodedImageCache(const DecodedImageCache&) = delete;
	DecodedImageCache& operator=(const DecodedImageCache&) = delete;

	ImagePtr Find(const std::filesystem::path& filename);
	ImagePtr Find(const SourceDescriptor& source);
	// If the requested file is already queued or being decoded, promote that
	// work and join it. Returns immediately on a cache hit or when no matching
	// work exists, allowing the caller to perform its normal foreground load.
	ImagePtr FindOrWait(const std::filesystem::path& filename);
	ImagePtr FindOrWait(const SourceDescriptor& source);
	void Store(const std::filesystem::path& filename,
		const std::shared_ptr<DecodedImage>& image);
	void Store(const SourceDescriptor& source,
		const std::shared_ptr<DecodedImage>& image);
	// Adds one non-speculative decode without replacing the current neighbor set.
	void RequestBackground(const std::filesystem::path& filename,
		Completion completion, PerfWorkClass workClass = PerfWorkClass::ActiveImageSpread);
	void RequestBackground(const SourceDescriptor& source,
		Completion completion, PerfWorkClass workClass = PerfWorkClass::ActiveImageSpread);
	// Reads only JPEG header dimensions on the existing worker, without retaining
	// or materializing full-resolution decoded pixels.
	void RequestJpegDimensions(const std::filesystem::path& filename,
		DimensionsCompletion completion,
		PerfWorkClass workClass = PerfWorkClass::ActiveImageSpread);
	void RequestJpegDimensions(const SourceDescriptor& source,
		DimensionsCompletion completion,
		PerfWorkClass workClass = PerfWorkClass::ActiveImageSpread);
	// Cancels only a matching active-spread source request. Foreground requests
	// promoted by FindOrWait are protected, even when they share the same key.
	bool CancelActiveSpreadRequest(const std::filesystem::path& filename,
		bool dimensionsOnly = false);
	bool CancelActiveSpreadRequest(const SourceDescriptor& source,
		bool dimensionsOnly = false);
	using DescriptorProvider = std::function<const SourceDescriptor*(std::size_t)>;
	void Prefetch(const std::vector<std::filesystem::path>& files,
		std::size_t currentIndex, int preferredDirection,
		std::size_t maximumCount, Completion completion = {}, Filter filter = {},
		std::size_t nearestCount = 2, DescriptorProvider descriptorProvider = {});
	void Clear();

	std::size_t CachedBytes() const;
	std::size_t CachedImages() const;
	DecodedImageCacheDiagnostics GetDiagnostics() const;
	std::vector<SourceChangeNotice> TakeChangedSources();
	std::size_t EvictLeastRecentlyUsed();
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
