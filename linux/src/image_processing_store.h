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
	const ImageProcessingStore& store);

} // namespace jpegview_linux
