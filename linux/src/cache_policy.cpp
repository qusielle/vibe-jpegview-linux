#include "cache_policy.h"

#include "perf_diagnostics.h"

#include <algorithm>
#include <array>

namespace jpegview_linux {

namespace {

bool StrongerProtection(CacheProtectionTier left, CacheProtectionTier right) {
	return static_cast<unsigned>(left) > static_cast<unsigned>(right);
}

} // namespace

CacheProtectionTier CacheProtectionForWorkClass(PerfWorkClass workClass,
	bool foreground) {
	if (foreground || workClass == PerfWorkClass::ActiveImageSpread) {
		return CacheProtectionTier::Active;
	}
	return workClass == PerfWorkClass::NearestNavigationNeighbor ||
		workClass == PerfWorkClass::FocusedPreview ? CacheProtectionTier::Neighbor :
		CacheProtectionTier::DistantSpeculation;
}

void AddDecodedProtection(CacheProtectionSnapshot& snapshot, const SourceKey& source,
	CacheProtectionTier protection) {
	if (!source.Valid()) return;
	const auto existing = std::find_if(snapshot.decoded.begin(), snapshot.decoded.end(),
		[&source](const auto& candidate) { return candidate.first == source; });
	if (existing == snapshot.decoded.end()) snapshot.decoded.emplace_back(source, protection);
	else if (StrongerProtection(protection, existing->second)) existing->second = protection;
}

void AddPreparedProtection(CacheProtectionSnapshot& snapshot,
	const DisplayImageCacheKey& key, CacheProtectionTier protection) {
	if (!key.Valid()) return;
	const auto existing = std::find_if(snapshot.prepared.begin(), snapshot.prepared.end(),
		[&key](const auto& candidate) { return candidate.first == key; });
	if (existing == snapshot.prepared.end()) snapshot.prepared.emplace_back(key, protection);
	else if (StrongerProtection(protection, existing->second)) existing->second = protection;
}

void AddTextureProtection(CacheProtectionSnapshot& snapshot, const std::string& key,
	CacheProtectionTier protection) {
	if (key.empty()) return;
	const auto existing = std::find_if(snapshot.textures.begin(), snapshot.textures.end(),
		[&key](const auto& candidate) { return candidate.first == key; });
	if (existing == snapshot.textures.end()) snapshot.textures.emplace_back(key, protection);
	else if (StrongerProtection(protection, existing->second)) existing->second = protection;
}

void AddDisplayRequestProtection(CacheProtectionSnapshot& snapshot,
	const DisplayImageRequest& request, CacheProtectionTier protection) {
	if (!request.source.Valid() || !request.cacheKey.Valid()) return;
	AddDecodedProtection(snapshot, request.source.Key(), protection);
	AddPreparedProtection(snapshot, request.cacheKey, protection);
	AddTextureProtection(snapshot, request.key, protection);
}

void AddMagnifyingGlassProtection(CacheProtectionSnapshot& snapshot,
	const std::optional<DisplayImageRequest>& request) {
	if (request.has_value()) {
		AddDisplayRequestProtection(snapshot, *request, CacheProtectionTier::Neighbor);
	}
}

CacheAdmissionPolicy::CacheAdmissionPolicy(SharedCacheBudget& budget)
	: budget_(budget) {}

CacheReservation CacheAdmissionPolicy::Reserve(std::size_t bytes,
	CacheMemoryCategory category, std::shared_ptr<const void> allocationOwner,
	CacheProtectionTier incomingProtection, const EvictOne& evictOne,
	bool mayEvict) const {
	const auto reserve = [&]() {
		CacheReservation result = budget_.TryReserve(bytes, category, allocationOwner);
		if (result && category == CacheMemoryCategory::RetainedImageTextures) {
			const CacheBudgetSnapshot snapshot = budget_.Snapshot();
			if (snapshot.uploadStagingBytes != 0) {
				PerfDiagnostics::Instance().RecordText(PerfMetric::CacheSnapshot,
					bytes, snapshot.capacityBytes, snapshot.retainedBytes,
					snapshot.uploadStagingBytes, snapshot.activeWorkingBytes,
					snapshot.releaseRevision, "upload_overlap");
			}
		}
		return result;
	};
	CacheReservation reservation = reserve();
	if (reservation || !evictOne || !mayEvict || bytes == 0 ||
		incomingProtection == CacheProtectionTier::DistantSpeculation) return reservation;
	const std::array<CacheProtectionTier, 2> evictionOrder{{
		CacheProtectionTier::DistantSpeculation,
		CacheProtectionTier::Neighbor}};
	const std::size_t evictionCount = incomingProtection == CacheProtectionTier::Active ?
		evictionOrder.size() : 1;
	for (std::size_t index = 0; index < evictionCount; ++index) {
		const CacheProtectionTier tier = evictionOrder[index];
		const std::uint64_t beforeReleaseRevision = budget_.RetainedCapacityRevision();
		if (evictOne(tier)) {
			// The removal callback may enqueue CPU pixels for asynchronous retirement.
			// Retry only when the reservation charge was actually released here.
			if (budget_.RetainedCapacityRevision() == beforeReleaseRevision) return {};
			reservation = reserve();
			return reservation;
		}
	}
	return {};
}

} // namespace jpegview_linux
