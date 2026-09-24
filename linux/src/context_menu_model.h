#pragma once

#include "file_list.h"
#include "playback_scheduler.h"
#include "crop_selection_model.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jpegview_linux {

constexpr int kContextMenuShowAdvanced = -1;
constexpr int kToggleNavigationPanelAutoReveal = -3;
constexpr int kCommandEditPictureLevels = -7;
constexpr int kCommandToggleZoomNavigator = -9;

struct MenuItem {
	std::string label;
	int command = 0;
	bool separator = false;
	bool checked = false;
	bool enabled = true;
	std::string shortcut;
	bool advanced = false;

	MenuItem(const char* itemLabel = nullptr, int itemCommand = 0,
		bool itemSeparator = false, bool itemChecked = false,
		bool itemEnabled = true, const char* itemShortcut = nullptr,
		bool itemAdvanced = false)
		: label(itemLabel == nullptr ? "" : itemLabel), command(itemCommand),
		  separator(itemSeparator), checked(itemChecked), enabled(itemEnabled),
		  shortcut(itemShortcut == nullptr ? "" : itemShortcut), advanced(itemAdvanced) {}
};

struct MenuColumn {
	std::size_t begin = 0;
	std::size_t end = 0;
	int height = 0;
};

struct ContextMenuState {
	PlaybackMode playbackMode = PlaybackMode::None;
	bool animationPlaying = false;
	bool animationAvailable = false;
	double movieFramesPerSecond = 25.0;
	bool infoVisible = false;
	bool filenameVisible = false;
	bool navigationPanelEnabled = true;
	bool navigationPanelAutoReveal = true;
	bool thumbnailPanelVisible = false;
	bool showZoomNavigator = true;
	bool selectionModeEnabled = false;
	FileList::NavigationMode navigationMode = FileList::NavigationMode::LoopDirectory;
	FileList::SortMode sortMode = FileList::SortMode::FileName;
	bool sortAscending = true;
	bool imageAvailable = false;
	bool losslessJpegAvailable = false;
	bool cropContextMenu = false;
	bool cropSelectionAvailable = false;
	bool losslessJpegCropAvailable = false;
	CropSelectionMode cropMode = CropSelectionMode::Free;
	int cropAspectWidth = 1;
	int cropAspectHeight = 1;
	int userCropAspectWidth = 1;
	int userCropAspectHeight = 1;
	bool autoCorrectionEnabled = false;
	bool pictureLevelsAvailable = false;
	bool localDensityEnabled = false;
	bool keepPictureLevels = false;
	bool pictureLevelsSaved = false;
	bool parameterDatabaseAvailable = true;
	bool fitToWindow = true;
	bool fillWithCrop = false;
	bool noEnlarge = true;
	double zoom = 1.0;
	bool fullscreen = false;
	bool borderless = false;
	bool alwaysOnTop = false;
	int transitionEffect = 0;
	std::uint32_t transitionDurationMs = 500;
	std::vector<std::string> openWithApplicationNames;
};

std::vector<MenuItem> BuildContextMenu(const ContextMenuState& state,
	bool advancedOptions);

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
