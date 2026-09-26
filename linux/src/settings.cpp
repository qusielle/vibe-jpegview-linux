#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

struct ProcessingSettingKey {
	const char* key;
	LevelControl control;
};

constexpr ProcessingSettingKey kDefaultProcessingSettingKeys[] = {
	{"default_contrast", LevelControl::Contrast},
	{"default_gamma", LevelControl::Brightness},
	{"default_saturation", LevelControl::Saturation},
	{"default_cyan_red", LevelControl::CyanRed},
	{"default_magenta_green", LevelControl::MagentaGreen},
	{"default_yellow_blue", LevelControl::YellowBlue},
	{"default_lighten_shadows", LevelControl::LightenShadows},
	{"default_darken_highlights", LevelControl::DarkenHighlights},
	{"default_deep_shadows", LevelControl::DeepShadows},
	{"default_color_correction", LevelControl::ColorCorrection},
	{"default_contrast_correction", LevelControl::ContrastCorrection},
	{"default_sharpen", LevelControl::Sharpen},
};

std::string Trim(std::string value) {
	const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
		return std::isspace(character) != 0;
	});
	const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
		return std::isspace(character) != 0;
	}).base();
	if (first >= last) return {};
	return std::string(first, last);
}

bool ParseBool(const std::string& value) {
	return value == "1" || value == "true";
}

bool ParseBoolStrict(const std::string& value, bool& parsed) {
	if (value == "1" || value == "true") {
		parsed = true;
		return true;
	}
	if (value == "0" || value == "false") {
		parsed = false;
		return true;
	}
	return false;
}

bool ParseInt(const std::string& value, int& parsed) {
	try {
		std::size_t parsedCharacters = 0;
		parsed = std::stoi(value, &parsedCharacters);
		return parsedCharacters == value.size();
	} catch (const std::exception&) {
		return false;
	}
}

} // namespace

fs::path ViewerSettingsPath() {
	if (const char* configHome = std::getenv("XDG_CONFIG_HOME"); configHome != nullptr && *configHome != '\0') {
		return fs::path(configHome) / "jpegview-linux" / "settings.conf";
	}
	if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
		return fs::path(home) / ".config" / "jpegview-linux" / "settings.conf";
	}
	return {};
}

bool LoadViewerSettings(const fs::path& filename, ViewerSettings& settings) {
	std::ifstream input(filename);
	if (!input) return false;

	ViewerSettings loaded;
	std::string line;
	while (std::getline(input, line)) {
		if (line.empty() || line[0] == '#') continue;
		const std::size_t separator = line.find('=');
		if (separator == std::string::npos) continue;
		const std::string key = Trim(line.substr(0, separator));
		const std::string value = Trim(line.substr(separator + 1));
		if (key == "scale_mode") {
			loaded.scaleMode = value;
		} else if (key == "sort_mode") {
			loaded.sortMode = value;
		} else if (key == "sort_ascending") {
			loaded.sortAscending = ParseBool(value);
		} else if (key == "copy_rename_pattern") {
			loaded.copyRenamePattern = value;
		} else if (key == "manual_zoom") {
			try {
				std::size_t parsedCharacters = 0;
				const double parsedZoom = std::stod(value, &parsedCharacters);
				if (parsedCharacters == value.size() && std::isfinite(parsedZoom)) {
					loaded.manualZoom = std::clamp(parsedZoom, kMinimumZoom, kMaximumZoom);
					loaded.manualZoomSet = true;
				}
			} catch (const std::exception&) {
				// Ignore malformed settings and retain the built-in default.
			}
		} else if (key == "maximized") {
			loaded.maximized = ParseBool(value);
		} else if (key == "navigation_panel_enabled") {
			loaded.navigationPanelEnabled = ParseBool(value);
		} else if (key == "navigation_panel_auto_reveal") {
			loaded.navigationPanelAutoReveal = ParseBool(value);
		} else if (key == "thumbnail_panel_visible") {
			loaded.thumbnailPanelVisible = ParseBool(value);
		} else if (key == "show_zoom_navigator") {
			bool parsed = false;
			if (ParseBoolStrict(value, parsed)) loaded.showZoomNavigator = parsed;
		} else if (key == "transparency_pattern") {
			(void)ParseTransparencyPattern(value, loaded.transparencyPattern);
		} else if (key == "thumbnail_panel_width") {
			try {
				std::size_t parsedCharacters = 0;
				const int parsedWidth = std::stoi(value, &parsedCharacters);
				if (parsedCharacters == value.size()) {
					loaded.thumbnailPanelWidth = std::clamp(parsedWidth,
						kMinimumThumbnailPanelWidth, kMaximumThumbnailPanelWidth);
				}
			} catch (const std::exception&) {
				// Ignore malformed settings and retain the built-in default.
			}
		} else if (key == "file_dialog_width" || key == "file_dialog_height") {
			try {
				std::size_t parsedCharacters = 0;
				const int parsedDimension = std::stoi(value, &parsedCharacters);
				if (parsedCharacters == value.size()) {
					const bool width = key == "file_dialog_width";
					const int minimum = width ? kMinimumFileDialogWidth : kMinimumFileDialogHeight;
					const int dimension = std::clamp(parsedDimension, minimum,
						kMaximumFileDialogDimension);
					if (width) loaded.fileDialogWidth = dimension;
					else loaded.fileDialogHeight = dimension;
				}
			} catch (const std::exception&) {
				// Ignore malformed settings and retain the built-in default.
			}
		} else if (key == "file_dialog_preview_ratio") {
			try {
				std::size_t parsedCharacters = 0;
				const double parsedRatio = std::stod(value, &parsedCharacters);
				if (parsedCharacters == value.size() && std::isfinite(parsedRatio)) {
					loaded.fileDialogPreviewRatio = std::clamp(parsedRatio, 0.0, 0.8);
				}
			} catch (const std::exception&) {
				// Ignore malformed settings and retain the built-in default.
			}
		} else if (key == "fixed_crop_width" || key == "fixed_crop_height") {
			int parsed = 0;
			if (ParseInt(value, parsed)) {
				const int dimension = std::clamp(parsed, kMinimumFixedCropDimension,
					kMaximumFixedCropDimension);
				if (key == "fixed_crop_width") loaded.fixedCropWidth = dimension;
				else loaded.fixedCropHeight = dimension;
			}
		} else if (key == "fixed_crop_screen_pixels") {
			bool parsed = false;
			if (ParseBoolStrict(value, parsed)) loaded.fixedCropScreenPixels = parsed;
		} else if (key == "user_crop_aspect_width" || key == "user_crop_aspect_height") {
			int parsed = 0;
			if (ParseInt(value, parsed) && parsed > 0) {
				const int dimension = std::min(parsed, kMaximumFixedCropDimension);
				if (key == "user_crop_aspect_width") loaded.userCropAspectWidth = dimension;
				else loaded.userCropAspectHeight = dimension;
			}
		} else if (key == "selection_mode_enabled") {
			bool parsed = false;
			if (ParseBoolStrict(value, parsed)) loaded.selectionModeEnabled = parsed;
		} else if (key == "info_visible") {
			loaded.infoVisible = ParseBool(value);
		} else if (key == "show_histogram") {
			loaded.showHistogram = ParseBool(value);
		} else if (key == "show_filename") {
			loaded.showFilename = ParseBool(value);
		} else if (key == "auto_contrast") {
			loaded.autoContrast = ParseBool(value);
		} else if (key == "keep_picture_levels") {
			loaded.keepPictureLevels = ParseBool(value);
		} else if (key == "default_local_density") {
			loaded.defaultImageProcessing.localDensityEnabled = ParseBool(value);
		} else if (key == "unsharp_mask_radius" || key == "unsharp_mask_amount" ||
			key == "unsharp_mask_threshold") {
			try {
				std::size_t parsedCharacters = 0;
				const double parsed = std::stod(value, &parsedCharacters);
				if (parsedCharacters == value.size() && std::isfinite(parsed)) {
					if (key == "unsharp_mask_radius") loaded.unsharpMaskRadius = std::clamp(parsed, 0.0, 5.0);
					else if (key == "unsharp_mask_amount") loaded.unsharpMaskAmount = std::clamp(parsed, 0.0, 10.0);
					else loaded.unsharpMaskThreshold = std::clamp(parsed, 0.0, 20.0);
				}
			} catch (const std::exception&) {
				// Ignore malformed settings and retain the built-in default.
			}
		} else if (key == "cache_size_mb") {
			try {
				std::size_t parsedCharacters = 0;
				const unsigned long long parsedSize = std::stoull(value, &parsedCharacters);
				if (parsedCharacters == value.size()) {
					loaded.cacheSizeMiB = static_cast<std::size_t>(std::min<unsigned long long>(
						parsedSize, kMaximumCacheSizeMiB));
				}
			} catch (const std::exception&) {
				// Ignore malformed settings and retain the built-in default.
			}
		} else {
			for (const ProcessingSettingKey& setting : kDefaultProcessingSettingKeys) {
				if (key != setting.key) continue;
				try {
					std::size_t parsedCharacters = 0;
					const double parsed = std::stod(value, &parsedCharacters);
					if (parsedCharacters == value.size() && std::isfinite(parsed)) {
						SetLevelControlValue(loaded.defaultImageProcessing, setting.control, parsed);
					}
				} catch (const std::exception&) {
					// Ignore malformed settings and retain the built-in default.
				}
				break;
			}
		}
	}
	if (!input.eof() && input.fail()) return false;
	settings = std::move(loaded);
	return true;
}

bool SaveViewerSettings(const fs::path& filename, const ViewerSettings& settings) {
	if (filename.empty()) return false;
	const double fileDialogPreviewRatio = std::isfinite(settings.fileDialogPreviewRatio) ?
		std::clamp(settings.fileDialogPreviewRatio, 0.0, 0.8) : 0.0;

	std::error_code error;
	if (!filename.parent_path().empty()) {
		fs::create_directories(filename.parent_path(), error);
		if (error) return false;
	}

	fs::path temporary = filename;
	temporary += ".tmp";
	{
		std::ofstream output(temporary, std::ios::trunc);
		if (!output) return false;
		output << "# JPEGView Linux display, dialog, and batch-operation settings\n"
		       << "scale_mode=" << settings.scaleMode << '\n'
		       << "sort_mode=" << settings.sortMode << '\n'
		       << "sort_ascending=" << (settings.sortAscending ? 1 : 0) << '\n'
		       << std::setprecision(17) << "manual_zoom=" << settings.manualZoom << '\n'
		       << "maximized=" << (settings.maximized ? 1 : 0) << '\n'
		       << "navigation_panel_enabled=" << (settings.navigationPanelEnabled ? 1 : 0) << '\n'
		       << "navigation_panel_auto_reveal=" << (settings.navigationPanelAutoReveal ? 1 : 0) << '\n'
		       << "thumbnail_panel_visible=" << (settings.thumbnailPanelVisible ? 1 : 0) << '\n'
		       << "show_zoom_navigator=" << (settings.showZoomNavigator ? 1 : 0) << '\n'
		       << "transparency_pattern=" << TransparencyPatternSettingName(settings.transparencyPattern) << '\n'
		       << "thumbnail_panel_width=" << settings.thumbnailPanelWidth << '\n'
		       << "file_dialog_width=" << std::clamp(settings.fileDialogWidth,
			kMinimumFileDialogWidth, kMaximumFileDialogDimension) << '\n'
		       << "file_dialog_height=" << std::clamp(settings.fileDialogHeight,
			kMinimumFileDialogHeight, kMaximumFileDialogDimension) << '\n'
		       << "file_dialog_preview_ratio=" << fileDialogPreviewRatio << '\n'
		       << "fixed_crop_width=" << std::clamp(settings.fixedCropWidth,
			kMinimumFixedCropDimension, kMaximumFixedCropDimension) << '\n'
		       << "fixed_crop_height=" << std::clamp(settings.fixedCropHeight,
			kMinimumFixedCropDimension, kMaximumFixedCropDimension) << '\n'
		       << "fixed_crop_screen_pixels=" << (settings.fixedCropScreenPixels ? 1 : 0) << '\n'
		       << "user_crop_aspect_width=" << std::clamp(settings.userCropAspectWidth,
			kMinimumFixedCropDimension, kMaximumFixedCropDimension) << '\n'
		       << "user_crop_aspect_height=" << std::clamp(settings.userCropAspectHeight,
			kMinimumFixedCropDimension, kMaximumFixedCropDimension) << '\n'
		       << "selection_mode_enabled=" << (settings.selectionModeEnabled ? 1 : 0) << '\n'
		       << "info_visible=" << (settings.infoVisible ? 1 : 0) << '\n'
		       << "show_histogram=" << (settings.showHistogram ? 1 : 0) << '\n'
		       << "show_filename=" << (settings.showFilename ? 1 : 0) << '\n'
		       << "auto_contrast=" << (settings.autoContrast ? 1 : 0) << '\n'
		       << "keep_picture_levels=" << (settings.keepPictureLevels ? 1 : 0) << '\n'
		       << "unsharp_mask_radius=" << std::clamp(settings.unsharpMaskRadius, 0.0, 5.0) << '\n'
		       << "unsharp_mask_amount=" << std::clamp(settings.unsharpMaskAmount, 0.0, 10.0) << '\n'
		       << "unsharp_mask_threshold=" << std::clamp(settings.unsharpMaskThreshold, 0.0, 20.0) << '\n'
		       << "default_local_density=" << (settings.defaultImageProcessing.localDensityEnabled ? 1 : 0) << '\n'
		       << "cache_size_mb=" << std::min(settings.cacheSizeMiB, kMaximumCacheSizeMiB) << '\n'
		       << "copy_rename_pattern=" << settings.copyRenamePattern << '\n';
		output << std::setprecision(17);
		for (const ProcessingSettingKey& setting : kDefaultProcessingSettingKeys) {
			output << setting.key << '=' << GetLevelControlValue(settings.defaultImageProcessing,
				setting.control) << '\n';
		}
		if (!output) {
			output.close();
			fs::remove(temporary, error);
			return false;
		}
	}

	error.clear();
	fs::rename(temporary, filename, error);
	if (error) {
		fs::remove(temporary, error);
		return false;
	}
	return true;
}

} // namespace jpegview_linux
