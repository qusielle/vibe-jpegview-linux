#pragma once

#include "cache_budget.h"
#include "image_processing.h"

#include <filesystem>
#include <string>

namespace jpegview_linux {

inline constexpr double kMinimumZoom = 0.01;
inline constexpr double kMaximumZoom = 32.0;
inline constexpr int kDefaultThumbnailPanelWidth = 164;
inline constexpr int kMinimumThumbnailPanelWidth = 48;
inline constexpr int kMaximumThumbnailPanelWidth = 1024;
inline constexpr int kDefaultFileDialogWidth = 900;
inline constexpr int kDefaultFileDialogHeight = 650;
inline constexpr int kMinimumFileDialogWidth = 320;
inline constexpr int kMinimumFileDialogHeight = 260;
inline constexpr int kMaximumFileDialogDimension = 16384;
inline constexpr int kDefaultFixedCropWidth = 320;
inline constexpr int kDefaultFixedCropHeight = 200;
inline constexpr int kMinimumFixedCropDimension = 1;
inline constexpr int kMaximumFixedCropDimension = 65535;
inline constexpr int kDefaultUserCropAspectWidth = 14;
inline constexpr int kDefaultUserCropAspectHeight = 11;

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
	bool showZoomNavigator = true;
	int thumbnailPanelWidth = kDefaultThumbnailPanelWidth;
	int fileDialogWidth = kDefaultFileDialogWidth;
	int fileDialogHeight = kDefaultFileDialogHeight;
	// Zero keeps the responsive default preview width; non-zero stores the
	// preview's fraction of the dialog's available list/preview width.
	double fileDialogPreviewRatio = 0.0;
	int fixedCropWidth = kDefaultFixedCropWidth;
	int fixedCropHeight = kDefaultFixedCropHeight;
	bool fixedCropScreenPixels = true;
	int userCropAspectWidth = kDefaultUserCropAspectWidth;
	int userCropAspectHeight = kDefaultUserCropAspectHeight;
	bool defaultSelectionMode = true;
	bool infoVisible = false;
	bool showHistogram = false;
	bool showFilename = false;
	bool autoContrast = false;
	bool keepPictureLevels = false;
	ImageProcessingParams defaultImageProcessing;
	double unsharpMaskRadius = 1.0;
	double unsharpMaskAmount = 0.0;
	double unsharpMaskThreshold = 4.0;
	std::size_t cacheSizeMiB = kDefaultCacheSizeMiB;
	std::string copyRenamePattern;
};

// Missing or unreadable files leave settings unchanged and return false.
bool LoadViewerSettings(const std::filesystem::path& filename, ViewerSettings& settings);

// Writes settings through a temporary file and returns whether the final rename succeeded.
bool SaveViewerSettings(const std::filesystem::path& filename, const ViewerSettings& settings);

} // namespace jpegview_linux
