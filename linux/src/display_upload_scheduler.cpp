#include "display_upload_scheduler.h"

#include <algorithm>
#include <limits>

namespace jpegview_linux {
namespace {

std::size_t WorkClassPriority(PerfWorkClass workClass) {
	switch (workClass) {
		case PerfWorkClass::ActiveImageSpread: return 0;
		case PerfWorkClass::FocusedPreview: return 1;
		case PerfWorkClass::NearestNavigationNeighbor: return 2;
		case PerfWorkClass::VisibleThumbnail: return 3;
		case PerfWorkClass::DistantSpeculation: return 4;
		case PerfWorkClass::Unspecified: return 5;
	}
	return 5;
}

bool IsSpeculative(PerfWorkClass workClass) {
	return workClass == PerfWorkClass::NearestNavigationNeighbor ||
		workClass == PerfWorkClass::DistantSpeculation;
}

} // namespace

bool DisplayUploadHasHigherPriority(const DisplayUploadPriority& left,
	const DisplayUploadPriority& right) {
	const std::size_t leftClass = WorkClassPriority(left.workClass);
	const std::size_t rightClass = WorkClassPriority(right.workClass);
	if (leftClass != rightClass) return leftClass < rightClass;
	return left.requestPriority < right.requestPriority;
}

bool MergeDisplayUploadPriority(DisplayUploadPriority& existing,
	const DisplayUploadPriority& incoming) {
	if (!DisplayUploadHasHigherPriority(incoming, existing)) return false;
	existing = incoming;
	return true;
}

std::vector<std::size_t> PlanDisplayTextureUploads(
	const std::vector<DisplayUploadPriority>& candidates,
	const std::set<PerfWorkClass>& permittedWorkClasses,
	std::size_t maximumCount, std::size_t maximumSpeculativeCount) {
	std::vector<std::size_t> order;
	if (maximumCount == 0) return order;
	std::vector<std::size_t> permitted;
	permitted.reserve(candidates.size());
	for (std::size_t index = 0; index < candidates.size(); ++index) {
		if (permittedWorkClasses.find(candidates[index].workClass) !=
			permittedWorkClasses.end()) permitted.push_back(index);
	}
	std::stable_sort(permitted.begin(), permitted.end(), [&candidates](std::size_t left,
		std::size_t right) {
		return DisplayUploadHasHigherPriority(candidates[left], candidates[right]);
	});
	order.reserve(std::min(maximumCount, permitted.size()));
	std::size_t speculativeCount = 0;
	for (const std::size_t index : permitted) {
		if (order.size() >= maximumCount) break;
		if (IsSpeculative(candidates[index].workClass)) {
			if (speculativeCount >= maximumSpeculativeCount) continue;
			++speculativeCount;
		}
		order.push_back(index);
	}
	return order;
}

std::size_t SpeculativeDisplayTextureBudgetBytes(std::size_t sharedBudgetBytes) {
	constexpr std::size_t maximumSpeculativeBytes = 256u * 1024u * 1024u;
	return std::min(sharedBudgetBytes / 2, maximumSpeculativeBytes);
}

bool CanRetainSpeculativeDisplayTexture(std::size_t retainedSpeculativeBytes,
	std::size_t incomingBytes, std::size_t speculativeBudgetBytes) {
	return incomingBytes <= speculativeBudgetBytes &&
		retainedSpeculativeBytes <= speculativeBudgetBytes - incomingBytes;
}

std::size_t DisplayTextureRetirementsForTick(std::size_t queuedRetirements,
	bool interactionActive, std::size_t maximumPerTick,
	std::size_t interactionPressureThreshold) {
	if (queuedRetirements == 0 || maximumPerTick == 0) return 0;
	if (interactionActive && queuedRetirements < interactionPressureThreshold) return 0;
	return std::min(queuedRetirements, maximumPerTick);
}

DisplayTextureUploadPlan::DisplayTextureUploadPlan(int width, int height,
	std::size_t pixelBytes, std::size_t synchronousByteLimit,
	std::size_t bandByteLimit)
	: width_(width), height_(height) {
	if (width <= 0 || height <= 0) return;
	const std::size_t unsignedWidth = static_cast<std::size_t>(width);
	const std::size_t unsignedHeight = static_cast<std::size_t>(height);
	if (unsignedWidth > std::numeric_limits<std::size_t>::max() / 4) return;
	const std::size_t rowBytes = unsignedWidth * 4;
	if (rowBytes > std::numeric_limits<std::size_t>::max() / unsignedHeight ||
		rowBytes * unsignedHeight != pixelBytes) return;
	valid_ = true;
	if (pixelBytes <= synchronousByteLimit) {
		bandRows_ = height;
		return;
	}
	const std::size_t maximumRows = std::max<std::size_t>(1,
		bandByteLimit / rowBytes);
	bandRows_ = static_cast<int>(std::min(unsignedHeight, maximumRows));
}

std::optional<DisplayTextureUploadBand> DisplayTextureUploadPlan::CurrentBand() const {
	if (!valid_ || failed_ || cancelled_ || nextY_ >= height_ || bandRows_ <= 0) {
		return std::nullopt;
	}
	return DisplayTextureUploadBand{nextY_, std::min(bandRows_, height_ - nextY_)};
}

bool DisplayTextureUploadPlan::MarkCurrentBandUploaded() {
	const std::optional<DisplayTextureUploadBand> band = CurrentBand();
	if (!band.has_value()) return false;
	nextY_ += band->height;
	return true;
}

void DisplayTextureUploadPlan::Fail() {
	if (!Complete() && !cancelled_) failed_ = true;
}

void DisplayTextureUploadPlan::Cancel() {
	if (!Complete()) cancelled_ = true;
}

} // namespace jpegview_linux
