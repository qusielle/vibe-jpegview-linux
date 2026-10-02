#pragma once

#include "cache_budget.h"
#include "display_image_cache.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jpegview_linux {

struct CacheProtectionSnapshot {
	std::vector<std::pair<SourceKey, CacheProtectionTier>> decoded;
	std::vector<std::pair<DisplayImageCacheKey, CacheProtectionTier>> prepared;
	std::vector<std::pair<std::string, CacheProtectionTier>> textures;
};

CacheProtectionTier CacheProtectionForWorkClass(PerfWorkClass workClass,
	bool foreground = false);
void AddDecodedProtection(CacheProtectionSnapshot& snapshot, const SourceKey& source,
	CacheProtectionTier protection);
void AddPreparedProtection(CacheProtectionSnapshot& snapshot,
	const DisplayImageCacheKey& key, CacheProtectionTier protection);
void AddTextureProtection(CacheProtectionSnapshot& snapshot, const std::string& key,
	CacheProtectionTier protection);
void AddDisplayRequestProtection(CacheProtectionSnapshot& snapshot,
	const DisplayImageRequest& request, CacheProtectionTier protection);
void AddMagnifyingGlassProtection(CacheProtectionSnapshot& snapshot,
	const std::optional<DisplayImageRequest>& request);

// Central retained-cache admission for renderer-thread work. Owners provide
// one-victim-at-a-time eviction; callbacks run outside the budget mutex.
class CacheAdmissionPolicy {
public:
	using EvictOne = std::function<bool(CacheProtectionTier)>;

	explicit CacheAdmissionPolicy(SharedCacheBudget& budget);

	CacheReservation Reserve(std::size_t bytes, CacheMemoryCategory category,
		std::shared_ptr<const void> allocationOwner,
		CacheProtectionTier incomingProtection, const EvictOne& evictOne,
		bool mayEvict = true) const;

private:
	SharedCacheBudget& budget_;
};

} // namespace jpegview_linux
