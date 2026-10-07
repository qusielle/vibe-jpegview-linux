#pragma once

#include "exif_reader.h"

#include <filesystem>
#include <string>

namespace jpegview_linux {

// Selects the metadata reader from the file's content, on a worker thread.
bool ReadImageMetadata(const std::filesystem::path& filename, ExifInfo& info,
	std::string& imageComment, const WorkContext& context = {});

} // namespace jpegview_linux
