#include "context_menu_model.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace jpegview_linux {

namespace {

bool IsSelectable(const MenuItem& item) {
	return !item.separator && item.command != 0 && item.enabled;
}

int ItemHeight(const MenuItem& item, int itemHeight, int separatorHeight) {
	return std::max(1, item.separator ? separatorHeight : itemHeight);
}

} // namespace

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
		if (item.separator && (compactItems.empty() || compactItems.back().separator)) continue;
		compactItems.push_back(item);
	}
	if (!compactItems.empty() && compactItems.back().separator) compactItems.pop_back();
	return compactItems;
}

int NextMenuSelection(const std::vector<MenuItem>& items, int current, int direction) {
	if (items.empty() || direction == 0) return -1;
	int candidate = current;
	for (std::size_t tries = 0; tries < items.size(); ++tries) {
		candidate += direction < 0 ? -1 : 1;
		if (candidate < 0) candidate = static_cast<int>(items.size()) - 1;
		if (candidate >= static_cast<int>(items.size())) candidate = 0;
		if (IsSelectable(items[static_cast<std::size_t>(candidate)])) return candidate;
	}
	return -1;
}

std::vector<MenuColumn> LayoutMenuColumns(const std::vector<MenuItem>& items,
	int maximumContentHeight, int itemHeight, int separatorHeight) {
	const int maximumRowHeight = std::max({1, itemHeight, separatorHeight});
	const int contentLimit = std::max(maximumRowHeight, maximumContentHeight);
	std::vector<MenuColumn> columns;
	std::size_t columnBegin = 0;
	int columnHeight = 0;
	for (std::size_t index = 0; index < items.size(); ++index) {
		const int rowHeight = ItemHeight(items[index], itemHeight, separatorHeight);
		if (columnHeight > 0 && columnHeight + rowHeight > contentLimit) {
			columns.push_back({columnBegin, index, columnHeight});
			columnBegin = index;
			columnHeight = 0;
		}
		columnHeight += rowHeight;
	}
	if (columnBegin < items.size() || columns.empty()) {
		columns.push_back({columnBegin, items.size(), columnHeight});
	}
	return columns;
}

int NextMenuSelectionInColumn(const std::vector<MenuItem>& items,
	const MenuColumn& column, int current, int direction) {
	if (direction == 0 || column.begin >= column.end || column.end > items.size()) return -1;
	int candidate = current;
	if (candidate < static_cast<int>(column.begin) || candidate >= static_cast<int>(column.end)) {
		candidate = direction > 0 ? static_cast<int>(column.begin) - 1 : static_cast<int>(column.end);
	}
	for (std::size_t tries = 0; tries < column.end - column.begin; ++tries) {
		candidate += direction > 0 ? 1 : -1;
		if (candidate < static_cast<int>(column.begin)) candidate = static_cast<int>(column.end) - 1;
		if (candidate >= static_cast<int>(column.end)) candidate = static_cast<int>(column.begin);
		if (IsSelectable(items[static_cast<std::size_t>(candidate)])) return candidate;
	}
	return -1;
}

int AdjacentMenuSelection(const std::vector<MenuItem>& items,
	const std::vector<MenuColumn>& columns, int current, int direction,
	int itemHeight, int separatorHeight) {
	if (direction == 0 || columns.empty()) return -1;
	int currentColumn = -1;
	int currentCenter = 0;
	if (current >= 0 && static_cast<std::size_t>(current) < items.size()) {
		for (std::size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex) {
			const MenuColumn& column = columns[columnIndex];
			if (static_cast<std::size_t>(current) < column.begin ||
				static_cast<std::size_t>(current) >= column.end || column.end > items.size()) continue;
			currentColumn = static_cast<int>(columnIndex);
			int itemTop = 0;
			for (std::size_t index = column.begin; index < static_cast<std::size_t>(current); ++index) {
				itemTop += ItemHeight(items[index], itemHeight, separatorHeight);
			}
			currentCenter = itemTop + ItemHeight(items[static_cast<std::size_t>(current)],
				itemHeight, separatorHeight) / 2;
			break;
		}
	}

	int candidateColumn = currentColumn < 0
		? (direction > 0 ? 0 : static_cast<int>(columns.size()) - 1)
		: currentColumn + (direction > 0 ? 1 : -1);
	while (candidateColumn >= 0 && candidateColumn < static_cast<int>(columns.size())) {
		const MenuColumn& column = columns[static_cast<std::size_t>(candidateColumn)];
		if (column.end > items.size() || column.begin > column.end) return -1;
		int itemTop = 0;
		int nearestIndex = -1;
		int nearestDistance = std::numeric_limits<int>::max();
		for (std::size_t index = column.begin; index < column.end; ++index) {
			const int rowHeight = ItemHeight(items[index], itemHeight, separatorHeight);
			if (IsSelectable(items[index])) {
				const int itemCenter = itemTop + rowHeight / 2;
				const int distance = currentColumn < 0 ? 0 : std::abs(itemCenter - currentCenter);
				if (distance < nearestDistance) {
					nearestDistance = distance;
					nearestIndex = static_cast<int>(index);
				}
			}
			itemTop += rowHeight;
		}
		if (nearestIndex >= 0) return nearestIndex;
		candidateColumn += direction > 0 ? 1 : -1;
	}
	return -1;
}

} // namespace jpegview_linux
