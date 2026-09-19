#pragma once

#include "cache_budget.h"

#include <filesystem>
#include <string>

namespace jpegview_linux {

inline constexpr double kMinimumZoom = 0.01;
inline constexpr double kMaximumZoom = 32.0;
inline constexpr int kDefaultThumbnailPanelWidth = 164;
inline constexpr int kMinimumThumbnailPanelWidth = 48;
inline constexpr int kMaximumThumbnailPanelWidth = 1024;

std::filesystem::path ViewerSettingsPath();

struct ViewerSettings {
	std::string scaleMode = "fit_no_enlarge";
	std::string sortMode = "modification_date";
	bool sortAscending = true;
	double manualZoom = 1.0;
	bool manualZoomSet = false;
	bool maximized = false;
	bool navigationPanelEnabled = true;
	bool navigationPanelAutoReveal = true;
	bool thumbnailPanelVisible = false;
	int thumbnailPanelWidth = kDefaultThumbnailPanelWidth;
	bool infoVisible = false;
	bool showHistogram = false;
	bool showFilename = false;
	bool autoContrast = false;
	std::size_t cacheSizeMiB = kDefaultCacheSizeMiB;
	std::string copyRenamePattern;
};

// Missing or unreadable files leave settings unchanged and return false.
bool LoadViewerSettings(const std::filesystem::path& filename, ViewerSettings& settings);

// Writes settings through a temporary file and returns whether the final rename succeeded.
bool SaveViewerSettings(const std::filesystem::path& filename, const ViewerSettings& settings);

} // namespace jpegview_linux
