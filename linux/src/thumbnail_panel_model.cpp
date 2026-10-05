#include "thumbnail_panel_model.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace jpegview_linux {

namespace {

std::vector<std::string> SourceKeyPaths(const std::vector<SourceKey>& keys) {
	std::vector<std::string> paths;
	paths.reserve(keys.size());
	for (const SourceKey& key : keys) paths.push_back(key.logicalPath);
	return paths;
}

} // namespace

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
	std::size_t currentIndex, int windowHeight, int rowHeight,
	std::optional<std::size_t> markedIndex,
	std::optional<std::size_t> doublePagePartnerIndex) {
	std::vector<ThumbnailSlot> slots;
	if (fileCount == 0 || currentIndex >= fileCount || windowHeight <= 0 || rowHeight <= 0) return slots;
	const int currentY = (windowHeight - rowHeight) / 2;
	const int maximumOffset = windowHeight / rowHeight + 2;
	for (int offset = -maximumOffset; offset <= maximumOffset; ++offset) {
		const long long index = static_cast<long long>(currentIndex) + offset;
		if (index < 0 || index >= static_cast<long long>(fileCount)) continue;
		const int y = currentY + offset * rowHeight;
		if (y >= windowHeight || y + rowHeight <= 0) continue;
		const std::size_t fileIndex = static_cast<std::size_t>(index);
		const bool isCurrent = offset == 0;
		const bool isDoublePagePartner = !isCurrent && doublePagePartnerIndex &&
			*doublePagePartnerIndex == fileIndex;
		slots.push_back({fileIndex, y, isCurrent, isDoublePagePartner,
			markedIndex && *markedIndex == fileIndex});
	}
	return slots;
}

bool ThumbnailIndexVisible(std::size_t fileCount, std::size_t currentIndex,
	std::size_t fileIndex, int windowHeight, int rowHeight) {
	if (fileCount == 0 || currentIndex >= fileCount || fileIndex >= fileCount ||
		windowHeight <= 0 || rowHeight <= 0) return false;
	const std::size_t maximumOffset =
		static_cast<std::size_t>(windowHeight / rowHeight) + 2;
	const bool afterCurrent = fileIndex >= currentIndex;
	const std::size_t distance = afterCurrent ? fileIndex - currentIndex : currentIndex - fileIndex;
	if (distance > maximumOffset) return false;
	const std::int64_t offset = static_cast<std::int64_t>(distance) * (afterCurrent ? 1 : -1);
	const std::int64_t currentY = (windowHeight - rowHeight) / 2;
	const std::int64_t rowY = currentY + offset * rowHeight;
	return rowY < windowHeight && rowY + rowHeight > 0;
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

std::vector<std::size_t> ThumbnailTextureWindowIndices(std::size_t fileCount,
	std::size_t currentIndex, int windowHeight, int rowHeight, bool panelVisible,
	std::optional<std::size_t> pinnedIndex, std::size_t extraViewports) {
	std::vector<std::size_t> indices;
	if (panelVisible && fileCount != 0 && currentIndex < fileCount &&
		windowHeight > 0 && rowHeight > 0) {
		const std::vector<ThumbnailSlot> visible = ThumbnailPanelSlots(
			fileCount, currentIndex, windowHeight, rowHeight);
		if (!visible.empty()) {
			std::size_t firstVisible = fileCount;
			std::size_t lastVisible = 0;
			for (const ThumbnailSlot& slot : visible) {
				firstVisible = std::min(firstVisible, slot.fileIndex);
				lastVisible = std::max(lastVisible, slot.fileIndex);
			}
			const std::size_t height = static_cast<std::size_t>(windowHeight);
			const std::size_t row = static_cast<std::size_t>(rowHeight);
			const std::size_t viewportRows = std::max<std::size_t>(1,
				height / row + (height % row == 0 ? 0 : 1));
			const std::size_t overscanRows = extraViewports > fileCount / viewportRows ?
				fileCount : std::min(fileCount, extraViewports * viewportRows);
			const std::size_t first = firstVisible > overscanRows ?
				firstVisible - overscanRows : 0;
			const std::size_t remainingAfterLast = fileCount - 1 - lastVisible;
			const std::size_t last = overscanRows > remainingAfterLast ?
				fileCount - 1 : lastVisible + overscanRows;
			indices.reserve(last - first + 1 + (pinnedIndex ? 1 : 0));
			for (std::size_t index = first; index <= last; ++index) {
				indices.push_back(index);
			}
		}
	}
	if (pinnedIndex && *pinnedIndex < fileCount) indices.push_back(*pinnedIndex);
	std::sort(indices.begin(), indices.end());
	indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
	return indices;
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

bool ThumbnailCatalogRevisionTracker::NeedsUpdate(std::uint64_t mutationRevision,
	std::uint64_t descriptorRevision) const {
	return !hasApplied_ || appliedOwnerRevision_ != ownerRevision_ ||
		appliedMutationRevision_ != mutationRevision ||
		appliedDescriptorRevision_ != descriptorRevision;
}

void ThumbnailCatalogRevisionTracker::MarkUpdated(std::uint64_t mutationRevision,
	std::uint64_t descriptorRevision) {
	appliedOwnerRevision_ = ownerRevision_;
	appliedMutationRevision_ = mutationRevision;
	appliedDescriptorRevision_ = descriptorRevision;
	hasApplied_ = true;
}

void ThumbnailCatalogRevisionTracker::NoteReplacement() {
	++ownerRevision_;
	if (ownerRevision_ == 0) ++ownerRevision_;
}

void ThumbnailCatalogRevisionTracker::Reset() {
	hasApplied_ = false;
}

std::vector<std::string> ThumbnailCacheScheduler::SetCatalog(
	std::vector<std::string> fileKeys) {
	const std::size_t capacity = fileKeys.size();
	return SetCatalog(std::move(fileKeys), capacity);
}

std::vector<std::string> ThumbnailCacheScheduler::SetCatalog(
	std::vector<std::string> fileKeys, std::size_t capacity) {
	std::vector<SourceKey> sourceKeys;
	sourceKeys.reserve(fileKeys.size());
	for (std::string& fileKey : fileKeys) sourceKeys.emplace_back(std::move(fileKey));
	return SourceKeyPaths(SetSourceCatalog(std::move(sourceKeys), capacity));
}

std::vector<SourceKey> ThumbnailCacheScheduler::SetSourceCatalog(
	std::vector<SourceKey> sourceKeys) {
	const std::size_t capacity = sourceKeys.size();
	return SetSourceCatalog(std::move(sourceKeys), capacity);
}

std::vector<SourceKey> ThumbnailCacheScheduler::SetSourceCatalog(
	std::vector<SourceKey> sourceKeys, std::size_t capacity) {
	if (catalog_ == sourceKeys) {
		if (capacity_ == capacity) return {};
		capacity_ = capacity;
		RebuildWorkingSet();
		RebuildPendingIndices();
		return Trim();
	}
	++catalogRevision_;
	if (catalogRevision_ == 0) ++catalogRevision_;
	const SourceKey previousCurrent = protectedKey_;
	catalog_ = std::move(sourceKeys);
	capacity_ = capacity;
	catalogIndices_.clear();
	catalogIndices_.reserve(catalog_.size());
	catalogPositions_.clear();
	catalogPositions_.reserve(catalog_.size());
	for (std::size_t index = 0; index < catalog_.size(); ++index) {
		++operationCounts_.catalogEntriesVisited;
		if (!catalog_[index].Empty() &&
			catalogIndices_.emplace(catalog_[index], index).second) {
			catalogPositions_.push_back(index);
		}
	}

	std::vector<SourceKey> evicted;
	for (auto cached = cache_.begin(); cached != cache_.end();) {
		if (catalogIndices_.find(cached->first) == catalogIndices_.end()) {
			evicted.push_back(cached->first);
			cached = cache_.erase(cached);
		} else {
			++cached;
		}
	}
	for (auto failed = failed_.begin(); failed != failed_.end();) {
		if (catalogIndices_.find(*failed) == catalogIndices_.end()) failed = failed_.erase(failed);
		else ++failed;
	}
	inFlight_.clear();
	const auto activeCurrent = catalogIndices_.find(previousCurrent);
	if (activeCurrent == catalogIndices_.end()) {
		currentIndex_ = catalog_.size();
		protectedKey_ = {};
	} else {
		currentIndex_ = activeCurrent->second;
		protectedKey_ = previousCurrent;
	}
	RebuildWorkingSet();
	RebuildPendingIndices();
	nextLoadTick_ = 0;
	const std::vector<SourceKey> capacityEvictions = Trim();
	evicted.insert(evicted.end(), capacityEvictions.begin(), capacityEvictions.end());
	return evicted;
}

std::vector<std::string> ThumbnailCacheScheduler::SetCurrent(std::size_t currentIndex) {
	return SourceKeyPaths(SetSourceCurrent(currentIndex));
}

std::vector<SourceKey> ThumbnailCacheScheduler::SetSourceCurrent(std::size_t currentIndex) {
	++operationCounts_.currentUpdates;
	const std::size_t previousIndex = currentIndex_;
	if (currentIndex >= catalog_.size()) {
		currentIndex_ = catalog_.size();
		protectedKey_ = {};
	} else {
		currentIndex_ = currentIndex;
		protectedKey_ = catalog_[currentIndex];
	}
	if (currentIndex_ != previousIndex) {
		RebuildWorkingSet();
		if (!wholeCatalogWorking_) RebuildPendingIndices();
	}
	return Trim();
}

std::vector<std::string> ThumbnailCacheScheduler::SetGeometry(
	int maximumWidth, int maximumHeight) {
	maximumWidth = std::max(0, maximumWidth);
	maximumHeight = std::max(0, maximumHeight);
	if (maximumWidth_ == maximumWidth && maximumHeight_ == maximumHeight) return {};
	maximumWidth_ = maximumWidth;
	maximumHeight_ = maximumHeight;
	++geometryRevision_;
	if (geometryRevision_ == 0) ++geometryRevision_;
	std::vector<std::string> evicted;
	evicted.reserve(cache_.size());
	for (const auto& cached : cache_) evicted.push_back(cached.first.logicalPath);
	cache_.clear();
	failed_.clear();
	inFlight_.clear();
	RebuildPendingIndices();
	nextLoadTick_ = 0;
	return evicted;
}

std::vector<ThumbnailLoadRequest> ThumbnailCacheScheduler::TakeNext(
	std::uint32_t now, std::size_t maximumCount) {
	return TakeNext(now, maximumCount, {});
}

std::vector<ThumbnailLoadRequest> ThumbnailCacheScheduler::TakeNext(
	std::uint32_t now, std::size_t maximumCount,
	const std::function<bool(const ThumbnailLoadRequest&)>& permitted) {
	std::vector<ThumbnailLoadRequest> requests;
	if (maximumCount == 0 || maximumWidth_ <= 0 || maximumHeight_ <= 0 ||
		(nextLoadTick_ != 0 && static_cast<std::int32_t>(now - nextLoadTick_) < 0)) return requests;
	requests.reserve(std::min(maximumCount, pendingIndices_.size()));
	while (requests.size() < maximumCount) {
		const std::optional<std::size_t> nextIndex = NearestPendingIndex();
		if (!nextIndex.has_value()) break;
		const std::size_t index = *nextIndex;
		ThumbnailLoadRequest request{index, catalog_[index], catalogRevision_,
			geometryRevision_, maximumWidth_, maximumHeight_};
		if (permitted && !permitted(request)) break;
		pendingIndices_.erase(index);
		inFlight_.insert(request.key);
		requests.push_back(std::move(request));
	}
	return requests;
}

std::vector<std::string> ThumbnailCacheScheduler::Complete(
	const ThumbnailLoadRequest& request, std::uint32_t now, std::uint32_t delayMs) {
	if (!IsCurrent(request)) return {};
	inFlight_.erase(request.key);
	failed_.erase(request.key);
	pendingIndices_.erase(request.fileIndex);
	if (!IsEligible(request.fileIndex)) return {};
	cache_[request.key].lastUsed = ++useCounter_;
	nextLoadTick_ = now + delayMs;
	return SourceKeyPaths(Trim());
}

void ThumbnailCacheScheduler::Fail(const ThumbnailLoadRequest& request,
	std::uint32_t now, std::uint32_t delayMs) {
	if (!IsCurrent(request)) return;
	inFlight_.erase(request.key);
	pendingIndices_.erase(request.fileIndex);
	failed_.insert(request.key);
	nextLoadTick_ = now + delayMs;
}

void ThumbnailCacheScheduler::Retry(const ThumbnailLoadRequest& request) {
	if (!IsCurrent(request) || IsCached(request.key) || IsFailed(request.key)) return;
	inFlight_.erase(request.key);
	if (IsEligible(request.fileIndex)) pendingIndices_.insert(request.fileIndex);
}

std::vector<std::string> ThumbnailCacheScheduler::Store(const std::string& key) {
	return SourceKeyPaths(Store(SourceKey(key)));
}

std::vector<SourceKey> ThumbnailCacheScheduler::Store(const SourceKey& key) {
	if (key.Empty()) return {};
	const auto index = catalogIndices_.find(key);
	if (index == catalogIndices_.end() || !IsEligible(index->second)) return {};
	failed_.erase(key);
	inFlight_.erase(key);
	pendingIndices_.erase(index->second);
	cache_[key].lastUsed = ++useCounter_;
	return Trim();
}

void ThumbnailCacheScheduler::Touch(const std::string& key) {
	Touch(SourceKey(key));
}

void ThumbnailCacheScheduler::Touch(const SourceKey& key) {
	const auto found = cache_.find(key);
	if (found != cache_.end()) found->second.lastUsed = ++useCounter_;
}

void ThumbnailCacheScheduler::Clear() {
	++catalogRevision_;
	if (catalogRevision_ == 0) ++catalogRevision_;
	++geometryRevision_;
	if (geometryRevision_ == 0) ++geometryRevision_;
	catalog_.clear();
	catalogIndices_.clear();
	catalogPositions_.clear();
	workingIndices_.clear();
	pendingIndices_.clear();
	cache_.clear();
	failed_.clear();
	inFlight_.clear();
	protectedKey_ = {};
	currentIndex_ = 0;
	capacity_ = 0;
	wholeCatalogWorking_ = false;
	maximumWidth_ = 0;
	maximumHeight_ = 0;
	nextLoadTick_ = 0;
}

bool ThumbnailCacheScheduler::IsCached(const std::string& key) const {
	return IsCached(SourceKey(key));
}

bool ThumbnailCacheScheduler::IsCached(const SourceKey& key) const {
	return cache_.find(key) != cache_.end();
}

bool ThumbnailCacheScheduler::IsFailed(const std::string& key) const {
	return IsFailed(SourceKey(key));
}

bool ThumbnailCacheScheduler::IsFailed(const SourceKey& key) const {
	return failed_.find(key) != failed_.end();
}

bool ThumbnailCacheScheduler::IsCurrent(const ThumbnailLoadRequest& request) const {
	if (request.catalogRevision != catalogRevision_ ||
		request.geometryRevision != geometryRevision_ ||
		request.maximumWidth != maximumWidth_ ||
		request.maximumHeight != maximumHeight_ ||
		request.fileIndex >= catalog_.size() || catalog_[request.fileIndex] != request.key) return false;
	const auto found = catalogIndices_.find(request.key);
	return found != catalogIndices_.end() && found->second == request.fileIndex;
}

std::optional<std::size_t> ThumbnailCacheScheduler::NearestPendingIndex() const {
	if (pendingIndices_.empty()) return std::nullopt;
	if (currentIndex_ >= catalog_.size()) return *pendingIndices_.begin();
	auto after = pendingIndices_.lower_bound(currentIndex_);
	if (after != pendingIndices_.end() && *after == currentIndex_) return *after;
	if (after == pendingIndices_.begin()) return *after;
	if (after == pendingIndices_.end()) return *std::prev(after);
	const auto before = std::prev(after);
	const std::size_t beforeDistance = currentIndex_ - *before;
	const std::size_t afterDistance = *after - currentIndex_;
	return beforeDistance <= afterDistance ? *before : *after;
}

bool ThumbnailCacheScheduler::IsEligible(std::size_t fileIndex) const {
	if (fileIndex >= catalog_.size()) return false;
	const auto found = catalogIndices_.find(catalog_[fileIndex]);
	if (found == catalogIndices_.end() || found->second != fileIndex) return false;
	return wholeCatalogWorking_ || workingIndices_.find(fileIndex) != workingIndices_.end();
}

void ThumbnailCacheScheduler::RebuildWorkingSet() {
	workingIndices_.clear();
	wholeCatalogWorking_ = capacity_ >= catalogPositions_.size();
	if (wholeCatalogWorking_ || capacity_ == 0 || catalogPositions_.empty()) return;

	const std::size_t workingCount = std::min(capacity_, catalogPositions_.size());
	if (currentIndex_ >= catalog_.size()) {
		for (std::size_t offset = 0; offset < workingCount; ++offset) {
			++operationCounts_.catalogEntriesVisited;
			workingIndices_.insert(catalogPositions_[offset]);
		}
		return;
	}

	auto after = std::lower_bound(catalogPositions_.begin(), catalogPositions_.end(), currentIndex_);
	auto before = after;
	bool hasBefore = before != catalogPositions_.begin();
	if (hasBefore) --before;
	for (std::size_t selected = 0; selected < workingCount; ++selected) {
		bool selectBefore = false;
		if (!hasBefore) {
			if (after == catalogPositions_.end()) break;
		} else if (after == catalogPositions_.end()) {
			selectBefore = true;
		} else {
			const std::size_t beforeDistance = currentIndex_ - *before;
			const std::size_t afterDistance = *after - currentIndex_;
			selectBefore = beforeDistance <= afterDistance;
		}
		if (selectBefore) {
			++operationCounts_.catalogEntriesVisited;
			workingIndices_.insert(*before);
			if (before == catalogPositions_.begin()) hasBefore = false;
			else --before;
		} else {
			++operationCounts_.catalogEntriesVisited;
			workingIndices_.insert(*after);
			++after;
		}
	}
}

void ThumbnailCacheScheduler::RebuildPendingIndices() {
	pendingIndices_.clear();
	const auto addPending = [this](std::size_t index) {
		++operationCounts_.catalogEntriesVisited;
		const SourceKey& key = catalog_[index];
		if (!IsCached(key) && !IsFailed(key) && inFlight_.find(key) == inFlight_.end()) {
			pendingIndices_.insert(index);
		}
	};
	if (wholeCatalogWorking_) {
		for (const std::size_t index : catalogPositions_) addPending(index);
	} else {
		for (const std::size_t index : workingIndices_) addPending(index);
	}
}

std::vector<SourceKey> ThumbnailCacheScheduler::Trim() {
	std::vector<SourceKey> evicted;
	if (wholeCatalogWorking_ && cache_.size() <= capacity_) return evicted;
	while (true) {
		auto oldest = cache_.end();
		bool foundIneligible = false;
		for (auto candidate = cache_.begin(); candidate != cache_.end(); ++candidate) {
			++operationCounts_.trimEntriesVisited;
			const auto active = catalogIndices_.find(candidate->first);
			const bool ineligible = active == catalogIndices_.end() || !IsEligible(active->second);
			if (ineligible) {
				if (!foundIneligible || candidate->second.lastUsed < oldest->second.lastUsed) {
					oldest = candidate;
					foundIneligible = true;
				}
				continue;
			}
			if (foundIneligible || cache_.size() <= capacity_) continue;
			if (candidate->first == protectedKey_) continue;
			if (oldest == cache_.end() || candidate->second.lastUsed < oldest->second.lastUsed) {
				oldest = candidate;
			}
		}
		if (oldest == cache_.end()) break;
		const SourceKey key = oldest->first;
		const auto active = catalogIndices_.find(key);
		if (active != catalogIndices_.end() && IsEligible(active->second) && !IsFailed(key) &&
			inFlight_.find(key) == inFlight_.end()) pendingIndices_.insert(active->second);
		else if (active != catalogIndices_.end()) pendingIndices_.erase(active->second);
		evicted.push_back(key);
		cache_.erase(oldest);
	}
	return evicted;
}

} // namespace jpegview_linux
