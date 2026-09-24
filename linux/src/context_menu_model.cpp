#include "context_menu_model.h"

#include "input_commands.h"
#include "sort_mode.h"

#include "../../src/JPEGView/resource.h"

#include <algorithm>
#include <cmath>
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

std::vector<MenuItem> BuildContextMenu(const ContextMenuState& state,
	bool advancedOptions) {
	if (state.cropContextMenu) {
		std::vector<MenuItem> cropItems = {
			{"Crop Selection", IDM_CROP_SEL, false, false, state.cropSelectionAvailable, nullptr, false},
			{"Crop Selection Lossless...", IDM_LOSSLESS_CROP_SEL, false, false,
				state.cropSelectionAvailable && state.losslessJpegCropAvailable, nullptr, false},
			{"Copy Selection to Clipboard", IDM_COPY_SEL, false, false,
				state.cropSelectionAvailable, nullptr, false},
			{nullptr, 0, true},
			{"Crop Mode", 0, false, false, false, nullptr, false},
			{"  Free", IDM_CROPMODE_FREE, false,
				state.cropMode == CropSelectionMode::Free, true, nullptr, false},
			{"  Same as Image", IDM_CROPMODE_IMAGE, false,
				state.cropMode == CropSelectionMode::ImageAspect, true, nullptr, false},
			{"  Fixed size...", IDM_CROPMODE_FIXED_SIZE, false,
				state.cropMode == CropSelectionMode::FixedSize, true, nullptr, false},
			{nullptr, 0, true},
			{"  1 : 1", IDM_CROPMODE_1_1, false,
				state.cropMode == CropSelectionMode::FixedAspect &&
				state.cropAspectWidth == 1 && state.cropAspectHeight == 1, true, nullptr, false},
			{"  5 : 4", IDM_CROPMODE_5_4, false,
				state.cropMode == CropSelectionMode::FixedAspect &&
				state.cropAspectWidth == 5 && state.cropAspectHeight == 4, true, nullptr, false},
			{"  4 : 3", IDM_CROPMODE_4_3, false,
				state.cropMode == CropSelectionMode::FixedAspect &&
				state.cropAspectWidth == 4 && state.cropAspectHeight == 3, true, nullptr, false},
			{"  7 : 5", IDM_CROPMODE_7_5, false,
				state.cropMode == CropSelectionMode::FixedAspect &&
				state.cropAspectWidth == 7 && state.cropAspectHeight == 5, true, nullptr, false},
			{"  3 : 2", IDM_CROPMODE_3_2, false,
				state.cropMode == CropSelectionMode::FixedAspect &&
				state.cropAspectWidth == 3 && state.cropAspectHeight == 2, true, nullptr, false},
			{"  16 : 10", IDM_CROPMODE_16_10, false,
				state.cropMode == CropSelectionMode::FixedAspect &&
				state.cropAspectWidth == 16 && state.cropAspectHeight == 10, true, nullptr, false},
			{"  16 : 9", IDM_CROPMODE_16_9, false,
				state.cropMode == CropSelectionMode::FixedAspect &&
				state.cropAspectWidth == 16 && state.cropAspectHeight == 9, true, nullptr, false},
			{nullptr, 0, true},
			{"User aspect", IDM_CROPMODE_USER, false,
				state.cropMode == CropSelectionMode::FixedAspect &&
				state.cropAspectWidth == state.userCropAspectWidth &&
				state.cropAspectHeight == state.userCropAspectHeight, true, nullptr, false},
			{nullptr, 0, true},
			{"Zoom to Selection", IDM_ZOOM_SEL, false, false,
				state.cropSelectionAvailable, nullptr, false},
		};
		for (MenuItem& item : cropItems) {
			if (item.command == IDM_CROPMODE_USER) {
				item.label = "  User aspect (" + std::to_string(state.userCropAspectWidth) +
					" : " + std::to_string(state.userCropAspectHeight) + ")";
				break;
			}
		}
		return cropItems;
	}
	const std::string sortingLabel = "Current order: " +
		std::string(SortModeShortLabel(state.sortMode)) + " (" +
		SortModeDescription(state.sortMode) + ")";
	std::vector<MenuItem> items = {
		// This is a flattened rendering of the complete Windows PopupMenu
		// resource.  Indented entries are the portable equivalent of its
		// submenus. Unsupported Windows-only commands remain visible but
		// disabled instead of silently doing nothing.
		{"Stop slide show/movie", IDM_STOP_MOVIE, false, false,
			state.playbackMode != PlaybackMode::None || state.animationPlaying, "Esc", true},
		{nullptr, 0, true},
		{"Open image...", IDM_OPEN, false, false, true, "Ctrl+O"},
		{"Open image with", 0, false, false, true, nullptr, true},
		{"  (no configured applications)", 0, false, false, false, nullptr, true},
		{"Save processed image...", IDM_SAVE, false, false, true, "Ctrl+S"},
		{"Save displayed image...", IDM_SAVE_SCREEN, false, false, true, "Ctrl+Shift+S"},
		{"Reload image", IDM_RELOAD, false, false, true, "Ctrl+R"},
		{"Open containing folder", IDM_EXPLORE, false, false, true, "W"},
		{"Print image...", IDM_PRINT, false, false, true, "Ctrl+P", true},
		{"Batch rename/copy...", IDM_BATCH_COPY, false, false, true, nullptr, true},
		{"Set modification date", 0, false, false, true, nullptr, true},
		{"  To current date", IDM_TOUCH_IMAGE, false, false, true, "Ctrl+Shift+M", true},
		{"  To EXIF date", IDM_TOUCH_IMAGE_EXIF, false, false, true, "Ctrl+Shift+E", true},
		{"  To EXIF date all files in folder", IDM_TOUCH_IMAGE_EXIF_FOLDER, false, false, true, nullptr, true},
		{"Set as desktop wallpaper", 0, false, false, true, nullptr, true},
		{"  Use original image", IDM_SET_WALLPAPER_ORIG, false, false, true, nullptr, true},
		{"  Use processed image as displayed", IDM_SET_WALLPAPER_DISPLAY, false, false, true, nullptr, true},
		{nullptr, 0, true},
		{"Copy original size image", IDM_COPY_FULL, false, false, true, "Ctrl+C"},
		{"Copy file path", IDM_COPY_PATH, false, false, true, "Ctrl+Shift+C"},
		{"Paste from clipboard", IDM_PASTE, false, false, true, "Ctrl+V"},
		{nullptr, 0, true},
		{"Show picture info (EXIF)", IDM_SHOW_FILEINFO, false, state.infoVisible, true, "F2"},
		{"Show filename", IDM_SHOW_FILENAME, false, state.filenameVisible, true, "Shift+N / Ctrl+F2"},
		{"Show navigation panel", IDM_SHOW_NAVPANEL, false, state.navigationPanelEnabled, true, "Ctrl+N"},
		{"Show navigation panel on bottom hover", kToggleNavigationPanelAutoReveal, false,
			state.navigationPanelAutoReveal, true},
		{"Show thumbnail panel", jpegview_linux::kCommandToggleThumbnailPanel,
			false, state.thumbnailPanelVisible, true, "Ctrl+T"},
		{"Show zoom navigator", jpegview_linux::kCommandToggleZoomNavigator,
			false, state.showZoomNavigator, true},
		{nullptr, 0, true},
		{"Next image", IDM_NEXT, false, false, true, "Right/PgDn"},
		{"Previous image", IDM_PREV, false, false, true, "Left/PgUp"},
		{"First image", IDM_FIRST, false, false, true, "Home"},
		{"Last image", IDM_LAST, false, false, true, "End"},
		{nullptr, 0, true},
		{"Navigation", 0, false, false, true, nullptr, true},
		{"  Loop folder", IDM_LOOP_FOLDER, false,
			state.navigationMode == jpegview_linux::FileList::NavigationMode::LoopDirectory, true, "F7", true},
		{"  Loop recursively", IDM_LOOP_RECURSIVELY, false,
			state.navigationMode == jpegview_linux::FileList::NavigationMode::LoopSubDirectories, true, "F8", true},
		{"  Loop siblings", IDM_LOOP_SIBLINGS, false,
			state.navigationMode == jpegview_linux::FileList::NavigationMode::LoopSameDirectoryLevel, true, "F9", true},
		{"  Previous sibling folder", kCommandPreviousSiblingFolder, false, false, true, "Alt+Left", true},
		{"  Next sibling folder", kCommandNextSiblingFolder, false, false, true, "Alt+Right", true},
		{"Display order", 0, false, false, true, nullptr, true},
		{sortingLabel.c_str(), 0, false, false, true, nullptr, true},
		{"  Modification date", IDM_SORT_MOD_DATE, false,
			state.sortMode == jpegview_linux::FileList::SortMode::LastModificationTime, true, "M", true},
		{"  Creation date", IDM_SORT_CREATION_DATE, false,
			state.sortMode == jpegview_linux::FileList::SortMode::CreationTime, true, "C", true},
		{"  File name", IDM_SORT_NAME, false,
			state.sortMode == jpegview_linux::FileList::SortMode::FileName, true, "N", true},
		{"  File size", IDM_SORT_SIZE, false,
			state.sortMode == jpegview_linux::FileList::SortMode::FileSize, true, nullptr, true},
		{"  Random", IDM_SORT_RANDOM, false,
			state.sortMode == jpegview_linux::FileList::SortMode::Random, true, "Z", true},
		{"  Ascending", IDM_SORT_ASCENDING, false, state.sortAscending, true, nullptr, true},
		{"  Descending", IDM_SORT_DESCENDING, false, !state.sortAscending, true, nullptr, true},
		{nullptr, 0, true},
		{"Transform image", 0, false, false, true, nullptr, true},
		{"  Rotate +90", IDM_ROTATE_90, false, false, true, "Down", true},
		{"  Rotate -90", IDM_ROTATE_270, false, false, true, "Up", true},
		{"  Rotate...", IDM_ROTATE, false, false, false, nullptr, true},
		{"  Change size...", IDM_CHANGESIZE, false, false, state.imageAvailable, "Ctrl+Shift+R", true},
		{"  Perspective correction...", IDM_PERSPECTIVE, false, false, false, nullptr, true},
		{"  Mirror horizontally", IDM_MIRROR_H, false, false, true, nullptr, true},
		{"  Mirror vertically", IDM_MIRROR_V, false, false, true, nullptr, true},
		{"Lossless JPEG transformations", 0, false, false, true, nullptr, true},
		{"  Rotate +90", IDM_ROTATE_90_LOSSLESS, false, false, state.losslessJpegAvailable, "R", true},
		{"  Rotate -90", IDM_ROTATE_270_LOSSLESS, false, false, state.losslessJpegAvailable, "T", true},
		{"  Rotate 180", IDM_ROTATE_180_LOSSLESS, false, false, state.losslessJpegAvailable, nullptr, true},
		{"  Mirror horizontally", IDM_MIRROR_H_LOSSLESS, false, false, state.losslessJpegAvailable, nullptr, true},
		{"  Mirror vertically", IDM_MIRROR_V_LOSSLESS, false, false, state.losslessJpegAvailable, nullptr, true},
		{"Auto correction", IDM_AUTO_CORRECTION, false, state.autoCorrectionEnabled, state.imageAvailable, "F5", true},
		{"Edit picture levels...", kCommandEditPictureLevels, false, false, state.pictureLevelsAvailable},
		{"Local density correction", IDM_LDC, false, state.localDensityEnabled, state.pictureLevelsAvailable, nullptr, true},
		{"Keep parameters between images", IDM_KEEP_PARAMETERS, false, state.keepPictureLevels, true, nullptr, true},
		{"Save parameters to DB", IDM_SAVE_PARAM_DB, false, false,
			state.pictureLevelsAvailable && !state.keepPictureLevels, nullptr, true},
		{"Clear parameters from DB", IDM_CLEAR_PARAM_DB, false, state.pictureLevelsSaved,
			state.pictureLevelsSaved && !state.keepPictureLevels, nullptr, true},
		{nullptr, 0, true},
		{"Scale / zoom", 0},
		{"  Fit to screen", IDM_FIT_TO_SCREEN, false, state.fitToWindow && !state.fillWithCrop, true, "Return/0"},
		{"  Fill with crop", IDM_FILL_WITH_CROP, false,
			state.fitToWindow && state.fillWithCrop && !state.noEnlarge, true, "Ctrl+Return", true},
		{"  Span all screens", IDM_SPAN_SCREENS, false, state.fullscreen, true, "F12", true},
		{"  400 %", IDM_ZOOM_400, false, false, true, nullptr, true},
		{"  200 %", IDM_ZOOM_200, false, false, true, nullptr, true},
		{"  Actual size (100 %)", IDM_ZOOM_100, false, !state.fitToWindow && std::abs(state.zoom - 1.0) < 0.01, true, "Space"},
		{"  50 %", IDM_ZOOM_50, false, false, true, nullptr, true},
		{"  25 %", IDM_ZOOM_25, false, false, true, nullptr, true},
		{"  Full screen mode", IDM_FULL_SCREEN_MODE, false, state.fullscreen, true, "F11/F"},
		{"  Fit window to image", IDM_FIT_WINDOW_TO_IMAGE, false, false, true, "Ctrl+F11"},
		{"  Hide window title bar", IDM_HIDE_TITLE_BAR, false, state.borderless, true, "Shift+F11", true},
		{"  Set window always on top", IDM_ALWAYS_ON_TOP, false, state.alwaysOnTop, true, "Shift+F12", true},
		{"Auto zoom mode", 0, false, false, true, nullptr, true},
		{"  Fit to screen no zoom", IDM_AUTO_ZOOM_FIT_NO_ZOOM,
			false, state.fitToWindow && !state.fillWithCrop && state.noEnlarge, true, nullptr, true},
		{"  Fill with crop no zoom", IDM_AUTO_ZOOM_FILL_NO_ZOOM,
			false, state.fitToWindow && state.fillWithCrop && state.noEnlarge, true, nullptr, true},
		{"  Fit to screen", IDM_AUTO_ZOOM_FIT,
			false, state.fitToWindow && !state.fillWithCrop && !state.noEnlarge, true, nullptr, true},
		{"  Fill with crop", IDM_AUTO_ZOOM_FILL,
			false, state.fitToWindow && state.fillWithCrop && !state.noEnlarge, true, nullptr, true},
		{nullptr, 0, true},
		{"Play folder as slideshow/movie", 0, false, false, true, nullptr, true},
		{state.playbackMode == PlaybackMode::Slideshow ? "  Stop slideshow" : "  Slideshow",
			state.playbackMode == PlaybackMode::Slideshow ? IDM_STOP_MOVIE : IDM_SLIDESHOW_START,
			false, false, true, "1-9", true},
		{"  Waiting time 1 sec", IDM_SLIDESHOW_1, false, false, true, "1", true},
		{"  Waiting time 2 sec", IDM_SLIDESHOW_2, false, false, true, "2", true},
		{"  Waiting time 3 sec", IDM_SLIDESHOW_3, false, false, true, "3", true},
		{"  Waiting time 4 sec", IDM_SLIDESHOW_4, false, false, true, "4", true},
		{"  Waiting time 5 sec", IDM_SLIDESHOW_5, false, false, true, "5", true},
		{"  Waiting time 7 sec", IDM_SLIDESHOW_7, false, false, true, "7", true},
		{"  Waiting time 10 sec", IDM_SLIDESHOW_10, false, false, true, nullptr, true},
		{"  Waiting time 20 sec", IDM_SLIDESHOW_20, false, false, true, nullptr, true},
		{"  Transition effect", 0, false, false, true, nullptr, true},
		{"    None", IDM_EFFECT_NONE, false, state.transitionEffect == IDM_EFFECT_NONE, true, nullptr, true},
		{"    Blend", IDM_EFFECT_BLEND, false, state.transitionEffect == IDM_EFFECT_BLEND, true, nullptr, true},
		{"    Slide from right", IDM_EFFECT_SLIDE_RL, false, state.transitionEffect == IDM_EFFECT_SLIDE_RL, true, nullptr, true},
		{"    Slide from left", IDM_EFFECT_SLIDE_LR, false, state.transitionEffect == IDM_EFFECT_SLIDE_LR, true, nullptr, true},
		{"    Slide from top", IDM_EFFECT_SLIDE_TB, false, state.transitionEffect == IDM_EFFECT_SLIDE_TB, true, nullptr, true},
		{"    Slide from bottom", IDM_EFFECT_SLIDE_BT, false, state.transitionEffect == IDM_EFFECT_SLIDE_BT, true, nullptr, true},
		{"    Roll from right", IDM_EFFECT_ROLL_RL, false, state.transitionEffect == IDM_EFFECT_ROLL_RL, true, nullptr, true},
		{"    Roll from left", IDM_EFFECT_ROLL_LR, false, state.transitionEffect == IDM_EFFECT_ROLL_LR, true, nullptr, true},
		{"    Roll from top", IDM_EFFECT_ROLL_TB, false, state.transitionEffect == IDM_EFFECT_ROLL_TB, true, nullptr, true},
		{"    Roll from bottom", IDM_EFFECT_ROLL_BT, false, state.transitionEffect == IDM_EFFECT_ROLL_BT, true, nullptr, true},
		{"    Scroll from right", IDM_EFFECT_SCROLL_RL, false, state.transitionEffect == IDM_EFFECT_SCROLL_RL, true, nullptr, true},
		{"    Scroll from left", IDM_EFFECT_SCROLL_LR, false, state.transitionEffect == IDM_EFFECT_SCROLL_LR, true, nullptr, true},
		{"    Scroll from top", IDM_EFFECT_SCROLL_TB, false, state.transitionEffect == IDM_EFFECT_SCROLL_TB, true, nullptr, true},
		{"    Scroll from bottom", IDM_EFFECT_SCROLL_BT, false, state.transitionEffect == IDM_EFFECT_SCROLL_BT, true, nullptr, true},
		{"  Transition speed", 0, false, false, true, nullptr, true},
		{"    Very fast", IDM_EFFECTTIME_VERY_FAST, false, state.transitionDurationMs == 100, true, nullptr, true},
		{"    Fast", IDM_EFFECTTIME_FAST, false, state.transitionDurationMs == 250, true, nullptr, true},
		{"    Normal", IDM_EFFECTTIME_NORMAL, false, state.transitionDurationMs == 500, true, nullptr, true},
		{"    Slow", IDM_EFFECTTIME_SLOW, false, state.transitionDurationMs == 1000, true, nullptr, true},
		{"    Very slow", IDM_EFFECTTIME_VERY_SLOW, false, state.transitionDurationMs == 2000, true, nullptr, true},
		{"  Resume playback", IDM_SLIDESHOW_RESUME, false, false,
			(!state.animationPlaying &&
				(state.playbackMode != PlaybackMode::None || state.animationAvailable)), "Alt+R", true},
		{"  Movie", IDM_MOVIE_START_FPS, false, state.playbackMode == PlaybackMode::Movie &&
			std::abs(state.movieFramesPerSecond - 25.0) < 0.01, true, "25 fps", true},
		{"  Playback speed 5 fps", IDM_MOVIE_5_FPS, false, state.playbackMode == PlaybackMode::Movie &&
			std::abs(state.movieFramesPerSecond - 5.0) < 0.01, true, "5", true},
		{"  Playback speed 10 fps", IDM_MOVIE_10_FPS, false, state.playbackMode == PlaybackMode::Movie &&
			std::abs(state.movieFramesPerSecond - 10.0) < 0.01, true, "10", true},
		{"  Playback speed 25 fps", IDM_MOVIE_25_FPS, false, state.playbackMode == PlaybackMode::Movie &&
			std::abs(state.movieFramesPerSecond - 25.0) < 0.01, true, "25", true},
		{"  Playback speed 30 fps", IDM_MOVIE_30_FPS, false, state.playbackMode == PlaybackMode::Movie &&
			std::abs(state.movieFramesPerSecond - 30.0) < 0.01, true, "30", true},
		{"  Playback speed 50 fps", IDM_MOVIE_50_FPS, false, state.playbackMode == PlaybackMode::Movie &&
			std::abs(state.movieFramesPerSecond - 50.0) < 0.01, true, "50", true},
		{"  Playback speed 100 fps", IDM_MOVIE_100_FPS, false, state.playbackMode == PlaybackMode::Movie &&
			std::abs(state.movieFramesPerSecond - 100.0) < 0.01, true, "100", true},
		{nullptr, 0, true},
		{"Settings Admin", 0, false, false, true, nullptr, true},
		{"  Edit global settings...", IDM_EDIT_GLOBAL_CONFIG, false, false, false, nullptr, true},
		{"  Edit user settings...", IDM_EDIT_USER_CONFIG, false, false, false, nullptr, true},
		{"  Update user settings...", IDM_UPDATE_USER_CONFIG, false, false, false, nullptr, true},
		{"  Manage Open image with menu...", IDM_MANAGE_OPEN_WITH_MENU, false, false, false, nullptr, true},
		{"  Set current parameters as default...", IDM_SAVE_PARAMETERS, false, false,
			state.pictureLevelsAvailable, nullptr, true},
		{"  Set as default viewer...", IDM_SET_AS_DEFAULT_VIEWER, false, false, true, nullptr, true},
		{"  Backup parameter DB...", IDM_BACKUP_PARAMDB, false, false,
			state.parameterDatabaseAvailable, nullptr, true},
		{"  Restore parameter DB...", IDM_RESTORE_PARAMDB, false, false,
			state.parameterDatabaseAvailable, nullptr, true},
		{"User commands", 0, false, false, true, nullptr, true},
		{"  (none configured)", 0, false, false, false, nullptr, true},
		{nullptr, 0, true},
		{"Help...", IDM_HELP, false, false, true, "F1"},
		{"About JPEGView...", IDM_ABOUT},
		{nullptr, 0, true},
		{"Exit", IDM_EXIT, false, false, true, "Q/Esc"},
	};

	const auto openWithHeader = std::find_if(items.begin(), items.end(), [](const MenuItem& item) {
		return item.label == "Open image with";
	});
	if (openWithHeader != items.end() && !state.openWithApplicationNames.empty()) {
		const std::size_t headerIndex = static_cast<std::size_t>(std::distance(items.begin(), openWithHeader));
		items.erase(items.begin() + static_cast<std::ptrdiff_t>(headerIndex + 1));
		for (std::size_t index = 0; index < state.openWithApplicationNames.size(); ++index) {
			const std::string label = "  " + state.openWithApplicationNames[index];
			items.insert(items.begin() + static_cast<std::ptrdiff_t>(headerIndex + 1 + index),
				MenuItem{label.c_str(), static_cast<int>(IDM_FIRST_OPENWITH_CMD + index),
					false, false, true, nullptr, true});
		}
	}
	if (!advancedOptions) {
		items = CompactMenuItems(items, kContextMenuShowAdvanced, "Show Advanced Options");
	}
	return items;
}


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
