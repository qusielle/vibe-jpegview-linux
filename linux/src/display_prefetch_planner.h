#pragma once

#include "display_image_cache.h"
#include "double_page_model.h"
#include "viewport.h"
#include "work_context.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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

struct RetainedDisplayTexture {
	std::string key;
	DisplayImageCacheKey cacheKey;
};

const RetainedDisplayTexture* FindReusableRetainedDisplayTexture(
	const DisplayImageCacheKey& requested,
	const std::vector<RetainedDisplayTexture>& retainedTextures);

// A prepared frame already transferred to renderer upload staging is still a
// usable neighbor-cache entry. Keep it only while its source remains in the
// newly selected image's immediate navigation window.
bool ShouldRetainPendingDisplayUpload(const SourceKey& uploadSource,
	const SourceKey& selectedSource, const std::vector<SourceKey>& neighborSources);

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
	// Capture the overlay's source-spectrum requirement with the pixel plan.
	bool includeSpectrum = false;
	std::size_t maximumPreparedBytes = std::numeric_limits<std::size_t>::max();
	DoublePageModeState doublePageMode;
	std::optional<PageDimensions> currentPageDimensions;
	std::size_t maximumCount = 0;
	std::vector<DisplayPrefetchCandidate> neighbors;
	std::vector<RetainedDisplayTexture> retainedTextures;
};

// Build the exact fitted display request used by planning. The viewer also uses
// it to protect only existing neighbor representations that can satisfy the plan.
std::optional<DisplayImageRequest> MakeFittedDisplayPrefetchRequest(
	const DisplayPrefetchCandidate& candidate,
	const DisplayPrefetchPlannerRequest& request,
	int sourceWidth, int sourceHeight);

struct DisplayPrefetchPlannerPhases {
	DisplayPrefetchPlannerRequest nearest;
	DisplayPrefetchPlannerRequest secondary;
	std::optional<DisplayPrefetchPlannerRequest> distant;
};

// Planning may use the configured retained-cache capacity; actual admission
// evicts lower-priority entries and is still enforced by the shared budget.
std::size_t DisplayPrefetchPlanningBudgetBytes(const CacheBudgetSnapshot& budget);

// Plan only the direction-preferred immediate neighbor first. The other
// immediate neighbor and farther sources wait for separate later phases.
DisplayPrefetchPlannerPhases SplitDisplayPrefetchPlannerRequest(
	const DisplayPrefetchPlannerRequest& request, std::size_t nearestCount = 1);

struct DisplayPrefetchContinuationPhase {
	std::size_t priority = 0;
	std::optional<DisplayPrefetchPlannerRequest> plannerRequest;
};

// The viewer acknowledges sources as display requests are published or
// skipped. A later phase starts only after every source in the current phase
// reaches a terminal state.
class DisplayPrefetchContinuation {
public:
	DisplayPrefetchContinuation(std::vector<SourceKey> nearestSources,
		std::vector<SourceKey> secondarySources,
		std::optional<DisplayPrefetchPlannerRequest> secondaryRequest,
		std::optional<DisplayPrefetchPlannerRequest> distantRequest,
		std::size_t maximumPreparedBytes, bool nearestPlannerPending,
		bool distantWorkPending);

	void TrackNearestRequest(const DisplayImageRequest& request);
	void TrackNearestRequest(const DisplayImageRequest& request,
		const std::string& completionKey);
	void ResolveNearestSource(const SourceKey& source);
	void ResolveNearestRequest(const std::string& requestKey, bool retained);
	void MarkNearestPlannerFinished();
	std::optional<DisplayPrefetchContinuationPhase> TakeNextPhaseIfReady();
	void Cancel();

private:
	struct RequestReservation {
		SourceKey source;
		std::size_t preparedBytes = 0;
	};

	std::mutex mutex_;
	std::unordered_set<SourceKey, SourceKeyHash> nearestSources_;
	std::unordered_map<std::string, RequestReservation> nearestRequests_;
	std::vector<SourceKey> secondarySources_;
	std::optional<DisplayPrefetchPlannerRequest> secondaryRequest_;
	std::optional<DisplayPrefetchPlannerRequest> distantRequest_;
	std::size_t maximumPreparedBytes_ = 0;
	std::size_t remainingPreparedBytes_ = 0;
	bool nearestPlannerPending_ = false;
	bool secondaryPhaseStarted_ = false;
	bool distantPhaseStarted_ = false;
	bool distantWorkPending_ = false;
	bool canceled_ = false;
};

struct DisplayPrefetchPlannedDimensions {
	std::size_t index = 0;
	SourceKey source;
	int width = 0;
	int height = 0;
};

struct DisplayPrefetchPlannerResult {
	WorkerFailure failure;
	std::uint64_t generation = 0;
	std::uint64_t catalogRevision = 0;
	std::uint64_t descriptorRevision = 0;
	std::uint64_t viewportRevision = 0;
	std::size_t currentIndex = 0;
	int preferredDirection = 0;
	ViewportSnapshot viewport;
	int imageAreaWidth = 0;
	int imageAreaHeight = 0;
	bool includeSpectrum = false;
	std::vector<DisplayImageRequest> requests;
	// A matching texture existed when the worker captured its snapshot. The
	// renderer thread must revalidate these before deciding that the source is ready.
	std::vector<DisplayImageRequest> reusableRequests;
	std::vector<std::string> protectedTextureKeys;
	std::vector<DisplayImageCacheKey> protectedTextureCacheKeys;
	std::vector<DisplayPrefetchPlannedDimensions> dimensions;
};

bool MatchesDisplayPrefetchSnapshot(const DisplayPrefetchPlannerResult& result,
	std::uint64_t expectedGeneration, std::uint64_t catalogRevision,
	std::uint64_t descriptorRevision, std::uint64_t viewportRevision,
	std::size_t currentIndex, int preferredDirection,
	const ViewportSnapshot& viewport, int imageAreaWidth, int imageAreaHeight,
	bool includeSpectrum = false);

// One cancellable worker probes JPEGs and SVG/SVGZ in a bounded, captured neighbor
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
