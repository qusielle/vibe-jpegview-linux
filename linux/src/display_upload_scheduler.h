#pragma once

#include "perf_diagnostics.h"

#include <cstddef>
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
	std::size_t maximumCount);

bool DisplayUploadHasHigherPriority(const DisplayUploadPriority& left,
	const DisplayUploadPriority& right);

// Preserves the strongest effective scheduling metadata when two owners have
// the same prepared-frame key, such as a deferred upload and a promoted refill.
bool MergeDisplayUploadPriority(DisplayUploadPriority& existing,
	const DisplayUploadPriority& incoming);

} // namespace jpegview_linux
