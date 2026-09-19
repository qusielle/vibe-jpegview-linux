#pragma once

#include <cstddef>
#include <vector>

namespace jpegview_linux {

struct MenuItem {
	const char* label = nullptr;
	int command = 0;
	bool separator = false;
	bool checked = false;
	bool enabled = true;
	const char* shortcut = nullptr;
	bool advanced = false;
};

struct MenuColumn {
	std::size_t begin = 0;
	std::size_t end = 0;
	int height = 0;
};

std::vector<MenuItem> CompactMenuItems(const std::vector<MenuItem>& items,
	int showAdvancedCommand, const char* showAdvancedLabel);

// Finds the next actionable item, wrapping in either direction. Returns -1
// when the menu has no enabled command.
int NextMenuSelection(const std::vector<MenuItem>& items, int current, int direction);

// Splits menu items into sequential columns that fit within the available
// content height. Separators consume separatorHeight; all other rows consume
// itemHeight. An empty menu still produces one empty column.
std::vector<MenuColumn> LayoutMenuColumns(const std::vector<MenuItem>& items,
	int maximumContentHeight, int itemHeight, int separatorHeight);

// Finds the next actionable item, wrapping only inside the specified column.
int NextMenuSelectionInColumn(const std::vector<MenuItem>& items,
	const MenuColumn& column, int current, int direction);

// Moves to the adjacent column's actionable item nearest the current row.
// With no current selection, right starts at the first column and left at the
// last. Returns -1 if no adjacent column has an actionable item.
int AdjacentMenuSelection(const std::vector<MenuItem>& items,
	const std::vector<MenuColumn>& columns, int current, int direction,
	int itemHeight, int separatorHeight);

} // namespace jpegview_linux
