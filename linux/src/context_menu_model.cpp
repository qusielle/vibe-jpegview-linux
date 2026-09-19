#include "context_menu_model.h"

namespace jpegview_linux {

std::vector<MenuItem> CompactMenuItems(const std::vector<MenuItem>& items,
	int showAdvancedCommand, const char* showAdvancedLabel) {
	std::vector<MenuItem> compactItems;
	compactItems.reserve(items.size());
	bool advancedOptionAdded = false;
	for (const MenuItem& item : items) {
		if (item.advanced) {
			if (!advancedOptionAdded) {
				compactItems.push_back({showAdvancedLabel, showAdvancedCommand});
				advancedOptionAdded = true;
			}
			continue;
		}
		compactItems.push_back(item);
	}
	return compactItems;
}

int NextMenuSelection(const std::vector<MenuItem>& items, int current, int direction) {
	if (items.empty() || direction == 0) return -1;
	int candidate = current;
	for (std::size_t tries = 0; tries < items.size(); ++tries) {
		candidate += direction < 0 ? -1 : 1;
		if (candidate < 0) candidate = static_cast<int>(items.size()) - 1;
		if (candidate >= static_cast<int>(items.size())) candidate = 0;
		const MenuItem& item = items[static_cast<std::size_t>(candidate)];
		if (!item.separator && item.command != 0 && item.enabled) return candidate;
	}
	return -1;
}

} // namespace jpegview_linux
