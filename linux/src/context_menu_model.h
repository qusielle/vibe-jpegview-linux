#pragma once

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

std::vector<MenuItem> CompactMenuItems(const std::vector<MenuItem>& items,
	int showAdvancedCommand, const char* showAdvancedLabel);

// Finds the next actionable item, wrapping in either direction. Returns -1
// when the menu has no enabled command.
int NextMenuSelection(const std::vector<MenuItem>& items, int current, int direction);

} // namespace jpegview_linux
