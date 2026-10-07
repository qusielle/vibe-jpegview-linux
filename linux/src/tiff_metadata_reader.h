#pragma once

#include "exif_reader.h"

#include <cstddef>
#include <cstdint>

namespace jpegview_linux {

// Parses a TIFF header embedded in a bounded metadata payload.
bool ReadTiffMetadata(const std::uint8_t* bytes, std::size_t bytesLength,
	std::size_t tiffOffset, ExifInfo& info);

} // namespace jpegview_linux
