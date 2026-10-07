#pragma once

#include "exif_reader.h"

#include <filesystem>

namespace jpegview_linux::detail {

constexpr std::uint64_t kMaximumEmbeddedExifBytes = 64u * 1024u * 1024u;

bool ReadPngMetadata(const std::filesystem::path& filename, ExifInfo& info,
	const WorkContext& context);

} // namespace jpegview_linux::detail
