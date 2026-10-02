#include "display_upload_scheduler.h"

#include <algorithm>

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
	std::size_t maximumCount) {
	std::vector<std::size_t> order;
	if (maximumCount == 0) return order;
	order.reserve(std::min(maximumCount, candidates.size()));
	for (std::size_t index = 0; index < candidates.size(); ++index) {
		if (permittedWorkClasses.find(candidates[index].workClass) !=
			permittedWorkClasses.end()) order.push_back(index);
	}
	std::stable_sort(order.begin(), order.end(), [&candidates](std::size_t left,
		std::size_t right) {
		return DisplayUploadHasHigherPriority(candidates[left], candidates[right]);
	});
	if (order.size() > maximumCount) order.resize(maximumCount);
	return order;
}

} // namespace jpegview_linux
