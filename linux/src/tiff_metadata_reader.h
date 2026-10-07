#pragma once

#include "exif_reader.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace jpegview_linux {

// Parses a TIFF header embedded in a bounded metadata payload.
bool ReadTiffMetadata(const std::uint8_t* bytes, std::size_t bytesLength,
	std::size_t tiffOffset, ExifInfo& info, const WorkContext& context = {});

// The callback must read exactly the requested range or return false. The
// parser validates ranges and checks the work context before and after reads.
using TiffMetadataReadAt = std::function<bool(std::size_t, std::uint8_t*, std::size_t)>;

// Reads only the TIFF header, directories, and referenced metadata values.
bool ReadTiffMetadataFromSource(std::size_t bytesLength,
	const TiffMetadataReadAt& readAt, std::size_t tiffOffset,
	ExifInfo& info, const WorkContext& context = {});

bool ReadTiffMetadataFile(const std::filesystem::path& filename,
	ExifInfo& info, const WorkContext& context = {});

} // namespace jpegview_linux
