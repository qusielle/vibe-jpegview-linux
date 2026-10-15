#pragma once

#include "image_processing.h"

#include <filesystem>
#include <map>
#include <string>

namespace jpegview_linux {

using ImageProcessingStore = std::map<std::string, ImageProcessingPreset>;

std::filesystem::path ImageProcessingStorePath();
bool LoadImageProcessingStore(const std::filesystem::path& filename,
	ImageProcessingStore& store);
bool SaveImageProcessingStore(const std::filesystem::path& filename,
	const ImageProcessingStore& store, bool replaceExisting = true);
bool BackupImageProcessingStore(const std::filesystem::path& liveDatabase,
	const std::filesystem::path& backupPath, bool replaceExisting,
	std::string& errorMessage);

} // namespace jpegview_linux
