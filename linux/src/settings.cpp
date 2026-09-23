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
		       << "thumbnail_panel_width=" << settings.thumbnailPanelWidth << '\n'
		       << "file_dialog_width=" << std::clamp(settings.fileDialogWidth,
			kMinimumFileDialogWidth, kMaximumFileDialogDimension) << '\n'
		       << "file_dialog_height=" << std::clamp(settings.fileDialogHeight,
			kMinimumFileDialogHeight, kMaximumFileDialogDimension) << '\n'
		       << "file_dialog_preview_ratio=" << fileDialogPreviewRatio << '\n'
		       << "info_visible=" << (settings.infoVisible ? 1 : 0) << '\n'
		       << "show_histogram=" << (settings.showHistogram ? 1 : 0) << '\n'
		       << "show_filename=" << (settings.showFilename ? 1 : 0) << '\n'
		       << "auto_contrast=" << (settings.autoContrast ? 1 : 0) << '\n'
		       << "keep_picture_levels=" << (settings.keepPictureLevels ? 1 : 0) << '\n'
		       << "unsharp_mask_radius=" << std::clamp(settings.unsharpMaskRadius, 0.0, 5.0) << '\n'
		       << "unsharp_mask_amount=" << std::clamp(settings.unsharpMaskAmount, 0.0, 10.0) << '\n'
		       << "unsharp_mask_threshold=" << std::clamp(settings.unsharpMaskThreshold, 0.0, 20.0) << '\n'
		       << "cache_size_mb=" << std::min(settings.cacheSizeMiB, kMaximumCacheSizeMiB) << '\n'
		       << "copy_rename_pattern=" << settings.copyRenamePattern << '\n';
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
