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
		} else if (key == "info_visible") {
			loaded.infoVisible = ParseBool(value);
		} else if (key == "show_filename") {
			loaded.showFilename = ParseBool(value);
		} else if (key == "auto_contrast") {
			loaded.autoContrast = ParseBool(value);
		}
	}
	if (!input.eof() && input.fail()) return false;
	settings = std::move(loaded);
	return true;
}

bool SaveViewerSettings(const fs::path& filename, const ViewerSettings& settings) {
	if (filename.empty()) return false;

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
		output << "# JPEGView Linux display and batch-operation settings\n"
		       << "scale_mode=" << settings.scaleMode << '\n'
		       << "sort_mode=" << settings.sortMode << '\n'
		       << "sort_ascending=" << (settings.sortAscending ? 1 : 0) << '\n'
		       << std::setprecision(17) << "manual_zoom=" << settings.manualZoom << '\n'
		       << "maximized=" << (settings.maximized ? 1 : 0) << '\n'
		       << "navigation_panel_enabled=" << (settings.navigationPanelEnabled ? 1 : 0) << '\n'
		       << "navigation_panel_auto_reveal=" << (settings.navigationPanelAutoReveal ? 1 : 0) << '\n'
		       << "info_visible=" << (settings.infoVisible ? 1 : 0) << '\n'
		       << "show_filename=" << (settings.showFilename ? 1 : 0) << '\n'
		       << "auto_contrast=" << (settings.autoContrast ? 1 : 0) << '\n'
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
