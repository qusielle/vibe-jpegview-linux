#pragma once

#include "image_processing.h"

#include <filesystem>
#include <string>

namespace jpegview_linux {

struct ProcessingRecipe {
	std::string name;
	ImageProcessingPreset preset;
};

bool SerializeProcessingRecipeJson(const ProcessingRecipe& recipe,
	std::string& json, std::string& errorMessage);
bool ParseProcessingRecipeJson(const std::string& json,
	ProcessingRecipe& recipe, std::string& errorMessage);
bool ReadProcessingRecipeJson(const std::filesystem::path& filename,
	ProcessingRecipe& recipe, std::string& errorMessage);
bool WriteProcessingRecipeJson(const std::filesystem::path& filename,
	const ProcessingRecipe& recipe, bool overwriteConfirmed,
	std::string& errorMessage);

} // namespace jpegview_linux
