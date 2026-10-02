#pragma once

#include "display_image_cache.h"
#include "double_page_model.h"
#include "viewport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace jpegview_linux {

enum class DisplayPrefetchBatchOwner {
	NeighborPlanner,
	ActiveSpread,
};

enum class DisplayPrefetchBatchInvalidation {
	ViewportChanged,
	OwnerChanged,
};

bool ShouldDeactivateDisplayPrefetchBatch(DisplayPrefetchBatchOwner owner,
	DisplayPrefetchBatchInvalidation invalidation, bool activeSpreadRequestStillCurrent);

// Speculative planner bookkeeping must not keep decoded allocations alive after
// the display cache accepts their request. Active spreads may retain a source
// fallback while their partner becomes displayable.
std::shared_ptr<const DecodedImage> RetainDisplayPrefetchDecodedImage(
	DisplayPrefetchBatchOwner owner,
	const std::shared_ptr<const DecodedImage>& decoded);

// An immutable entry from the bounded neighbor window captured by the viewer.
// Processing and source identity are resolved before the worker sees it.
struct DisplayPrefetchCandidate {
	std::filesystem::path filename;
	SourceDescriptor source;
	std::size_t index = 0;
	std::size_t priority = 0;
	ImageProcessingParams processing;
	bool autoContrast = false;
	bool jpeg = false;
};

struct DisplayPrefetchPlannerRequest {
	std::uint64_t catalogRevision = 0;
	std::uint64_t descriptorRevision = 0;
	std::uint64_t viewportRevision = 0;
	std::size_t currentIndex = 0;
	std::size_t pageCount = 0;
	int preferredDirection = 0;
	ViewportSnapshot viewport;
	int imageAreaWidth = 0;
	int imageAreaHeight = 0;
	DoublePageModeState doublePageMode;
	std::optional<PageDimensions> currentPageDimensions;
	std::size_t maximumCount = 0;
	std::vector<DisplayPrefetchCandidate> neighbors;
	std::unordered_set<std::string> retainedTextureKeys;
};

struct DisplayPrefetchPlannedDimensions {
	std::size_t index = 0;
	SourceKey source;
	int width = 0;
	int height = 0;
};

struct DisplayPrefetchPlannerResult {
	std::uint64_t generation = 0;
	std::uint64_t catalogRevision = 0;
	std::uint64_t descriptorRevision = 0;
	std::uint64_t viewportRevision = 0;
	std::size_t currentIndex = 0;
	int preferredDirection = 0;
	ViewportSnapshot viewport;
	int imageAreaWidth = 0;
	int imageAreaHeight = 0;
	std::vector<DisplayImageRequest> requests;
	std::vector<std::string> protectedTextureKeys;
	std::vector<DisplayImageCacheKey> protectedTextureCacheKeys;
	std::vector<DisplayPrefetchPlannedDimensions> dimensions;
};

bool MatchesDisplayPrefetchSnapshot(const DisplayPrefetchPlannerResult& result,
	std::uint64_t expectedGeneration, std::uint64_t catalogRevision,
	std::uint64_t descriptorRevision, std::uint64_t viewportRevision,
	std::size_t currentIndex, int preferredDirection,
	const ViewportSnapshot& viewport, int imageAreaWidth, int imageAreaHeight);

// One cancellable worker probes only the JPEGs in a bounded, captured neighbor
// window and turns their dimensions into renderer-ready request descriptors.
// It never touches SDL objects or mutable viewer state.
class DisplayPrefetchPlannerWorker {
public:
	using Continue = std::function<bool()>;
	using DimensionsReader = std::function<bool(const SourceDescriptor&, int&, int&,
		std::string&, const Continue&)>;

	explicit DisplayPrefetchPlannerWorker(DimensionsReader reader = {});
	~DisplayPrefetchPlannerWorker();
	DisplayPrefetchPlannerWorker(const DisplayPrefetchPlannerWorker&) = delete;
	DisplayPrefetchPlannerWorker& operator=(const DisplayPrefetchPlannerWorker&) = delete;

	std::uint64_t Request(DisplayPrefetchPlannerRequest request);
	void Cancel();
	void Stop();
	std::vector<DisplayPrefetchPlannerResult> TakeReady();
	bool WaitUntilIdle(std::chrono::milliseconds timeout);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

bool SameViewportSnapshot(const ViewportSnapshot& left,
	const ViewportSnapshot& right);

void AppendDisplayPrefetchRequests(std::vector<DisplayImageRequest>& requests,
	const std::vector<DisplayImageRequest>& additions);

} // namespace jpegview_linux
