#pragma once

#include "perf_diagnostics.h"

#include <cstddef>
#include <optional>
#include <set>
#include <vector>

namespace jpegview_linux {

struct DisplayUploadPriority {
	PerfWorkClass workClass = PerfWorkClass::Unspecified;
	std::size_t requestPriority = 0;
};

// Orders a mixed set of completed and deferred display uploads. The returned
// indices refer to the input vector and include only currently permitted work.
std::vector<std::size_t> PlanDisplayTextureUploads(
	const std::vector<DisplayUploadPriority>& candidates,
	const std::set<PerfWorkClass>& permittedWorkClasses,
	std::size_t maximumCount,
	std::size_t maximumSpeculativeCount = static_cast<std::size_t>(-1));

bool DisplayUploadHasHigherPriority(const DisplayUploadPriority& left,
	const DisplayUploadPriority& right);

// Preserves the strongest effective scheduling metadata when two owners have
// the same prepared-frame key, such as a deferred upload and a promoted refill.
bool MergeDisplayUploadPriority(DisplayUploadPriority& existing,
	const DisplayUploadPriority& incoming);

// Speculative display textures use a bounded share of the existing shared
// cache budget. Active presentation textures continue to use normal admission.
std::size_t SpeculativeDisplayTextureBudgetBytes(std::size_t sharedBudgetBytes);
bool CanRetainSpeculativeDisplayTexture(std::size_t retainedSpeculativeBytes,
	std::size_t incomingBytes, std::size_t speculativeBudgetBytes);

// During interaction, avoid routine renderer destruction until obsolete
// textures build up enough to represent meaningful residency pressure.
std::size_t DisplayTextureRetirementsForTick(std::size_t queuedRetirements,
	bool interactionActive, std::size_t maximumPerTick,
	std::size_t interactionPressureThreshold);

struct DisplayTextureUploadBand {
	int y = 0;
	int height = 0;
};

// Pure progress model for a private renderer texture assembled over maintenance
// opportunities. It cannot become publishable after cancellation or a failed
// band, so callers never expose partially uploaded pixels.
class DisplayTextureUploadPlan {
public:
	DisplayTextureUploadPlan() = default;
	DisplayTextureUploadPlan(int width, int height, std::size_t pixelBytes,
		std::size_t synchronousByteLimit, std::size_t bandByteLimit);

	bool Valid() const { return valid_; }
	bool IsBanded() const { return bandRows_ > 0 && bandRows_ < height_; }
	bool Complete() const { return valid_ && !failed_ && !cancelled_ && nextY_ >= height_; }
	bool Failed() const { return failed_; }
	bool Cancelled() const { return cancelled_; }
	std::optional<DisplayTextureUploadBand> CurrentBand() const;
	bool MarkCurrentBandUploaded();
	void Fail();
	void Cancel();

private:
	int width_ = 0;
	int height_ = 0;
	int nextY_ = 0;
	int bandRows_ = 0;
	bool valid_ = false;
	bool failed_ = false;
	bool cancelled_ = false;
};

} // namespace jpegview_linux
