#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace jpegview_linux {

struct ThumbnailSlot {
	std::size_t fileIndex = 0;
	int y = 0;
	bool current = false;
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
	std::size_t currentIndex, int windowHeight, int rowHeight);

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
	std::string key;
	std::uint64_t generation = 0;
};

// Owns nearest-first work, cancellation generations, LRU usage, and cache
// capacity. Pixel decoding and renderer textures stay in the SDL adapter.
class ThumbnailCacheScheduler {
public:
	std::vector<std::string> Prepare(const std::vector<std::string>& fileKeys,
		std::size_t currentIndex, std::size_t capacity);
	std::optional<ThumbnailLoadRequest> Next(std::uint32_t now);
	std::vector<std::string> Complete(const ThumbnailLoadRequest& request,
		std::uint32_t now, std::uint32_t delayMs = 25);
	// Records pixels prepared outside the sequential idle-time loader.
	std::vector<std::string> Store(const std::string& key);
	void Touch(const std::string& key);
	void Clear();

	bool IsCached(const std::string& key) const;
	std::size_t CacheSize() const { return cache_.size(); }
	std::size_t PendingCount() const { return queue_.size() - queuePosition_; }
	std::uint64_t Generation() const { return generation_; }

private:
	std::vector<std::string> Trim();

	struct CacheRecord {
		std::uint64_t lastUsed = 0;
	};
	std::vector<ThumbnailLoadRequest> queue_;
	std::size_t queuePosition_ = 0;
	std::unordered_map<std::string, CacheRecord> cache_;
	std::string protectedKey_;
	std::size_t capacity_ = 0;
	std::uint64_t useCounter_ = 0;
	std::uint64_t generation_ = 0;
	std::uint32_t nextLoadTick_ = 0;
};

} // namespace jpegview_linux
