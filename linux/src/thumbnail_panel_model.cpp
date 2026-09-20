#include "thumbnail_panel_model.h"

#include <algorithm>
#include <cmath>

namespace jpegview_linux {

ThumbnailPanelLayout CalculateThumbnailPanelLayout(int windowWidth, int windowHeight,
	bool panelVisible, int preferredPanelWidth) {
	const int width = std::max(0, windowWidth);
	const int height = std::max(0, windowHeight);
	const int panelWidth = panelVisible && width > 1 && preferredPanelWidth > 0 ?
		std::min(preferredPanelWidth, width - 1) : 0;
	return {panelWidth, panelWidth, width - panelWidth, height};
}

int ThumbnailRowHeight(int panelWidth, int verticalMargin) {
	if (panelWidth <= 0) return 0;
	const int margin = std::max(0, verticalMargin);
	const int imageHeight = std::max(1, static_cast<int>(std::round(panelWidth * 2.0 / 3.0)));
	return imageHeight + margin * 2 + 1;
}

std::vector<ThumbnailSlot> ThumbnailPanelSlots(std::size_t fileCount,
	std::size_t currentIndex, int windowHeight, int rowHeight) {
	std::vector<ThumbnailSlot> slots;
	if (fileCount == 0 || currentIndex >= fileCount || windowHeight <= 0 || rowHeight <= 0) return slots;
	const int currentY = (windowHeight - rowHeight) / 2;
	const int maximumOffset = windowHeight / rowHeight + 2;
	for (int offset = -maximumOffset; offset <= maximumOffset; ++offset) {
		const long long index = static_cast<long long>(currentIndex) + offset;
		if (index < 0 || index >= static_cast<long long>(fileCount)) continue;
		const int y = currentY + offset * rowHeight;
		if (y >= windowHeight || y + rowHeight <= 0) continue;
		slots.push_back({static_cast<std::size_t>(index), y, offset == 0});
	}
	return slots;
}

std::vector<std::size_t> ThumbnailPreloadOrder(std::size_t fileCount,
	std::size_t currentIndex, std::size_t maximumCount) {
	std::vector<std::size_t> order;
	if (fileCount == 0 || currentIndex >= fileCount || maximumCount == 0) return order;
	order.reserve(std::min(fileCount, maximumCount));
	order.push_back(currentIndex);
	for (std::size_t distance = 1; order.size() < maximumCount && order.size() < fileCount; ++distance) {
		if (distance <= currentIndex) order.push_back(currentIndex - distance);
		if (order.size() >= maximumCount || order.size() >= fileCount) break;
		if (distance < fileCount - currentIndex) order.push_back(currentIndex + distance);
	}
	return order;
}

std::size_t ThumbnailCacheCapacity(int panelWidth, int rowHeight,
	int verticalMargin, std::size_t pixelBudget, std::size_t maximumEntries) {
	if (panelWidth <= 0 || rowHeight <= 0 || pixelBudget == 0 || maximumEntries == 0) return 0;
	const int margin = std::max(0, verticalMargin);
	const int imageHeight = rowHeight - margin * 2 - 1;
	if (imageHeight <= 0) return 0;
	const std::size_t pixelsPerThumbnail = static_cast<std::size_t>(panelWidth) *
		static_cast<std::size_t>(imageHeight);
	return std::clamp(pixelBudget / pixelsPerThumbnail,
		static_cast<std::size_t>(1), maximumEntries);
}

ThumbnailSize FitThumbnailSize(int sourceWidth, int sourceHeight,
	int maximumWidth, int maximumHeight) {
	if (sourceWidth <= 0 || sourceHeight <= 0 || maximumWidth <= 0 || maximumHeight <= 0) return {};
	const double scale = std::min({1.0, static_cast<double>(maximumWidth) / sourceWidth,
		static_cast<double>(maximumHeight) / sourceHeight});
	return {
		std::max(1, static_cast<int>(std::floor(sourceWidth * scale + 0.5))),
		std::max(1, static_cast<int>(std::floor(sourceHeight * scale + 0.5)))
	};
}

ThumbnailRect ThumbnailImageRect(int sourceWidth, int sourceHeight,
	int panelWidth, int rowY, int rowHeight, int verticalMargin) {
	const int margin = std::max(0, verticalMargin);
	const int availableHeight = rowHeight - margin * 2 - 1;
	const ThumbnailSize size = FitThumbnailSize(sourceWidth, sourceHeight,
		panelWidth, availableHeight);
	if (size.width <= 0 || size.height <= 0) return {};
	return {
		(panelWidth - size.width) / 2,
		rowY + margin + (availableHeight - size.height) / 2,
		size.width,
		size.height
	};
}

std::vector<std::string> ThumbnailCacheScheduler::Prepare(
	const std::vector<std::string>& fileKeys, std::size_t currentIndex,
	std::size_t capacity) {
	++generation_;
	queue_.clear();
	queuePosition_ = 0;
	capacity_ = capacity;
	protectedKey_ = currentIndex < fileKeys.size() ? fileKeys[currentIndex] : std::string();
	const std::vector<std::size_t> order = ThumbnailPreloadOrder(
		fileKeys.size(), currentIndex, capacity);
	queue_.reserve(order.size());
	for (const std::size_t index : order) {
		queue_.push_back({index, fileKeys[index], generation_});
	}
	nextLoadTick_ = 0;
	return Trim();
}

std::optional<ThumbnailLoadRequest> ThumbnailCacheScheduler::Next(std::uint32_t now) {
	if (nextLoadTick_ != 0 && static_cast<std::int32_t>(now - nextLoadTick_) < 0) return std::nullopt;
	while (queuePosition_ < queue_.size()) {
		const ThumbnailLoadRequest request = queue_[queuePosition_++];
		if (IsCached(request.key)) {
			Touch(request.key);
			continue;
		}
		return request;
	}
	return std::nullopt;
}

std::vector<std::string> ThumbnailCacheScheduler::Complete(
	const ThumbnailLoadRequest& request, std::uint32_t now, std::uint32_t delayMs) {
	if (request.generation != generation_) return {};
	cache_[request.key].lastUsed = ++useCounter_;
	nextLoadTick_ = now + delayMs;
	return Trim();
}

std::vector<std::string> ThumbnailCacheScheduler::Store(const std::string& key) {
	if (key.empty()) return {};
	cache_[key].lastUsed = ++useCounter_;
	return Trim();
}

void ThumbnailCacheScheduler::Touch(const std::string& key) {
	const auto found = cache_.find(key);
	if (found != cache_.end()) found->second.lastUsed = ++useCounter_;
}

void ThumbnailCacheScheduler::Clear() {
	++generation_;
	queue_.clear();
	queuePosition_ = 0;
	cache_.clear();
	protectedKey_.clear();
	capacity_ = 0;
	nextLoadTick_ = 0;
}

bool ThumbnailCacheScheduler::IsCached(const std::string& key) const {
	return cache_.find(key) != cache_.end();
}

std::vector<std::string> ThumbnailCacheScheduler::Trim() {
	std::vector<std::string> evicted;
	while (cache_.size() > capacity_) {
		auto oldest = cache_.end();
		for (auto candidate = cache_.begin(); candidate != cache_.end(); ++candidate) {
			if (candidate->first == protectedKey_) continue;
			if (oldest == cache_.end() || candidate->second.lastUsed < oldest->second.lastUsed) {
				oldest = candidate;
			}
		}
		if (oldest == cache_.end()) {
			if (capacity_ != 0 || cache_.empty()) break;
			oldest = cache_.begin();
		}
		evicted.push_back(oldest->first);
		cache_.erase(oldest);
	}
	return evicted;
}

} // namespace jpegview_linux
