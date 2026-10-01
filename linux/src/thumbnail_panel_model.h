#pragma once

#include "archive_source.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jpegview_linux {

struct ThumbnailSlot {
	std::size_t fileIndex = 0;
	int y = 0;
	bool current = false;
	bool doublePagePartner = false;
	bool marked = false;
};

struct ThumbnailSize {
	int width = 0;
	int height = 0;
};

struct ThumbnailRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct ThumbnailPanelLayout {
	int panelWidth = 0;
	int imageX = 0;
	int imageWidth = 0;
	int imageHeight = 0;
};

// Splits the client area into a left thumbnail strip and the image viewport.
// At least one pixel remains available to the image in very narrow windows.
ThumbnailPanelLayout CalculateThumbnailPanelLayout(int windowWidth, int windowHeight,
	bool panelVisible, int preferredPanelWidth);

// Uses a 3:2 thumbnail cell so row height follows the adjustable panel width.
// Vertical margins and the one-pixel separator are added around the image.
int ThumbnailRowHeight(int panelWidth, int verticalMargin);

// Returns visible rows in file-list order. The current file's row is centered
// vertically; rows outside the window are omitted rather than wrapping.
std::vector<ThumbnailSlot> ThumbnailPanelSlots(std::size_t fileCount,
	std::size_t currentIndex, int windowHeight, int rowHeight,
	std::optional<std::size_t> markedIndex = std::nullopt,
	std::optional<std::size_t> doublePagePartnerIndex = std::nullopt);

// Whether a file-list index intersects the visible strip, including partially
// clipped rows at the top and bottom of the panel.
bool ThumbnailIndexVisible(std::size_t fileCount, std::size_t currentIndex,
	std::size_t fileIndex, int windowHeight, int rowHeight);

// Returns file indices nearest to the current file first. Equal-distance
// entries prefer the preceding file, matching their top-to-bottom placement.
std::vector<std::size_t> ThumbnailPreloadOrder(std::size_t fileCount,
	std::size_t currentIndex, std::size_t maximumCount);

// Calculates the number of full-panel thumbnail surfaces that fit within a
// pixel budget. A non-empty cache keeps at least one entry and never exceeds
// maximumEntries.
std::size_t ThumbnailCacheCapacity(int panelWidth, int rowHeight,
	int verticalMargin, std::size_t pixelBudget, std::size_t maximumEntries);

// Aspect-fit dimensions for a thumbnail. Images are never enlarged.
ThumbnailSize FitThumbnailSize(int sourceWidth, int sourceHeight,
	int maximumWidth, int maximumHeight);

// Fits a thumbnail into a row with no forced horizontal inset. The bottom
// pixel is reserved for the row separator in addition to the vertical margin.
ThumbnailRect ThumbnailImageRect(int sourceWidth, int sourceHeight,
	int panelWidth, int rowY, int rowHeight, int verticalMargin);

struct ThumbnailLoadRequest {
	std::size_t fileIndex = 0;
	SourceKey key;
	std::uint64_t catalogRevision = 0;
	std::uint64_t geometryRevision = 0;
	int maximumWidth = 0;
	int maximumHeight = 0;
};

struct ThumbnailSchedulerOperationCounts {
	std::size_t catalogEntriesVisited = 0;
	std::size_t currentEntriesVisited = 0;
	std::size_t currentUpdates = 0;
	std::size_t trimEntriesVisited = 0;
};

struct ThumbnailCacheEvictionCounts {
	std::size_t keyLookups = 0;
	std::size_t entriesErased = 0;
};

template <typename Cache, typename OnErase>
ThumbnailCacheEvictionCounts EraseThumbnailCacheEntries(
	Cache& cache, const std::vector<SourceKey>& keys, OnErase&& onErase) {
	ThumbnailCacheEvictionCounts counts;
	for (const SourceKey& key : keys) {
		++counts.keyLookups;
		const auto cached = cache.find(key);
		if (cached == cache.end()) continue;
		onErase(cached->second);
		cache.erase(cached);
		++counts.entriesErased;
	}
	return counts;
}

// Tracks both per-instance FileList mutations and whole-list owner replacement.
class ThumbnailCatalogRevisionTracker {
public:
	bool NeedsUpdate(std::uint64_t mutationRevision,
		std::uint64_t descriptorRevision = 0) const;
	void MarkUpdated(std::uint64_t mutationRevision,
		std::uint64_t descriptorRevision = 0);
	void NoteReplacement();
	void Reset();

private:
	std::uint64_t ownerRevision_ = 0;
	std::uint64_t appliedOwnerRevision_ = 0;
	std::uint64_t appliedMutationRevision_ = 0;
	std::uint64_t appliedDescriptorRevision_ = 0;
	bool hasApplied_ = false;
};

// Owns nearest-first work, cancellation generations, LRU usage, and cache
// capacity. Pixel decoding and renderer textures stay in the SDL adapter.
class ThumbnailCacheScheduler {
public:
	std::vector<std::string> SetCatalog(std::vector<std::string> fileKeys);
	std::vector<std::string> SetCatalog(std::vector<std::string> fileKeys,
		std::size_t capacity);
	std::vector<SourceKey> SetSourceCatalog(std::vector<SourceKey> sourceKeys);
	std::vector<SourceKey> SetSourceCatalog(std::vector<SourceKey> sourceKeys,
		std::size_t capacity);
	std::vector<std::string> SetCurrent(std::size_t currentIndex);
	std::vector<SourceKey> SetSourceCurrent(std::size_t currentIndex);
	std::vector<std::string> SetGeometry(int maximumWidth, int maximumHeight);
	std::vector<ThumbnailLoadRequest> TakeNext(std::uint32_t now,
		std::size_t maximumCount = 1);
	std::vector<ThumbnailLoadRequest> TakeNext(std::uint32_t now,
		std::size_t maximumCount,
		const std::function<bool(const ThumbnailLoadRequest&)>& permitted);
	std::vector<std::string> Complete(const ThumbnailLoadRequest& request,
		std::uint32_t now, std::uint32_t delayMs = 25);
	void Fail(const ThumbnailLoadRequest& request, std::uint32_t now,
		std::uint32_t delayMs = 25);
	void Retry(const ThumbnailLoadRequest& request);
	// Records pixels prepared outside the sequential idle-time loader.
	std::vector<std::string> Store(const std::string& key);
	std::vector<SourceKey> Store(const SourceKey& key);
	void Touch(const std::string& key);
	void Touch(const SourceKey& key);
	void Clear();

	bool IsCached(const std::string& key) const;
	bool IsCached(const SourceKey& key) const;
	bool IsFailed(const std::string& key) const;
	bool IsFailed(const SourceKey& key) const;
	bool IsCurrent(const ThumbnailLoadRequest& request) const;
	std::size_t CacheSize() const { return cache_.size(); }
	std::size_t PendingCount() const { return pendingIndices_.size(); }
	std::uint64_t CatalogRevision() const { return catalogRevision_; }
	std::uint64_t GeometryRevision() const { return geometryRevision_; }
	ThumbnailSchedulerOperationCounts OperationCounts() const { return operationCounts_; }

private:
	bool IsEligible(std::size_t fileIndex) const;
	void RebuildWorkingSet();
	std::optional<std::size_t> NearestPendingIndex() const;
	void RebuildPendingIndices();
	std::vector<SourceKey> Trim();

	struct CacheRecord {
		std::uint64_t lastUsed = 0;
	};
	std::vector<SourceKey> catalog_;
	std::unordered_map<SourceKey, std::size_t, SourceKeyHash> catalogIndices_;
	std::vector<std::size_t> catalogPositions_;
	std::set<std::size_t> workingIndices_;
	std::set<std::size_t> pendingIndices_;
	std::unordered_map<SourceKey, CacheRecord, SourceKeyHash> cache_;
	std::unordered_set<SourceKey, SourceKeyHash> failed_;
	std::unordered_set<SourceKey, SourceKeyHash> inFlight_;
	SourceKey protectedKey_;
	std::size_t currentIndex_ = 0;
	std::size_t capacity_ = 0;
	bool wholeCatalogWorking_ = false;
	std::uint64_t useCounter_ = 0;
	std::uint64_t catalogRevision_ = 0;
	std::uint64_t geometryRevision_ = 0;
	int maximumWidth_ = 0;
	int maximumHeight_ = 0;
	std::uint32_t nextLoadTick_ = 0;
	ThumbnailSchedulerOperationCounts operationCounts_;
};

} // namespace jpegview_linux
