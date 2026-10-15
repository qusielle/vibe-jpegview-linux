#include "image_processing_store.h"

#include "atomic_file.h"
#include "settings.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace jpegview_linux {

fs::path ImageProcessingStorePath() {
	fs::path settings = ViewerSettingsPath();
	if (settings.empty()) return {};
	settings.replace_filename("picture-levels.db");
	return settings;
}

bool LoadImageProcessingStore(const fs::path& filename, ImageProcessingStore& store) {
	std::ifstream input(filename);
	if (!input) return false;
	ImageProcessingStore loaded;
	std::string line;
	while (std::getline(input, line)) {
		if (line.empty() || line[0] == '#') continue;
		std::istringstream row(line);
		std::string key;
		ImageProcessingPreset preset;
		ImageProcessingParams& params = preset.processing;
		bool localDensityEnabled = false;
		if (!(row >> std::quoted(key) >> preset.autoContrast >> localDensityEnabled)) return false;
		if (!(row >> params.contrast >> params.gamma >> params.saturation >>
			params.cyanRed >> params.magentaGreen >> params.yellowBlue >>
			params.lightenShadows >> params.darkenHighlights >> params.deepShadows >>
			params.colorCorrection >> params.contrastCorrection >> params.sharpen)) {
			return false;
		}
		params.localDensityEnabled = localDensityEnabled;
		bool valid = true;
		for (std::size_t i = 0; i < static_cast<std::size_t>(LevelControl::Count); ++i) {
			const LevelControl control = static_cast<LevelControl>(i);
			const double value = GetLevelControlValue(params, control);
			const LevelControlInfo& info = GetLevelControlInfo(control);
			if (!std::isfinite(value) || value < info.minimum || value > info.maximum) {
				valid = false;
				break;
			}
		}
		if (!valid || key.empty()) return false;
		loaded[key] = preset;
	}
	if (!input.eof()) return false;
	store = std::move(loaded);
	return true;
}

bool SaveImageProcessingStore(const fs::path& filename, const ImageProcessingStore& store,
	bool replaceExisting) {
	if (filename.empty()) return false;
	AtomicFileWriter atomicOutput(filename);
	if (!atomicOutput) return false;
	{
		std::ostream& output = atomicOutput.Stream();
		output << "# JPEGView Linux per-image picture levels, version 2\n";
		output << std::setprecision(17);
		for (const auto& entry : store) {
			const ImageProcessingPreset& preset = entry.second;
			const ImageProcessingParams& params = preset.processing;
			output << std::quoted(entry.first) << ' ' << (preset.autoContrast ? 1 : 0) << ' '
				<< (params.localDensityEnabled ? 1 : 0) << ' '
				<< params.contrast << ' ' << params.gamma << ' ' << params.saturation << ' '
				<< params.cyanRed << ' ' << params.magentaGreen << ' ' << params.yellowBlue << ' '
				<< params.lightenShadows << ' ' << params.darkenHighlights << ' '
				<< params.deepShadows << ' ' << params.colorCorrection << ' '
				<< params.contrastCorrection << ' ' << params.sharpen << '\n';
		}
	}
	return atomicOutput.Commit(replaceExisting);
}

bool BackupImageProcessingStore(const fs::path& liveDatabase, const fs::path& backupPath,
	bool replaceExisting, std::string& errorMessage) {
	errorMessage.clear();
	if (backupPath.empty()) {
		errorMessage = "backup destination is empty";
		return false;
	}
	std::error_code statusError;
	const bool sourceExists = fs::exists(liveDatabase, statusError);
	if (statusError) {
		errorMessage = "could not inspect the live parameter database";
		return false;
	}
	ImageProcessingStore backupContents;
	if (sourceExists && !LoadImageProcessingStore(liveDatabase, backupContents)) {
		errorMessage = "the live parameter database is unreadable";
		return false;
	}
	if (!SaveImageProcessingStore(backupPath, backupContents, replaceExisting)) {
		errorMessage = "could not write the parameter database backup";
		return false;
	}
	return true;
}

} // namespace jpegview_linux
