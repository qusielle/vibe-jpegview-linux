#pragma once

#include <filesystem>

namespace jpegview_linux {

bool IsSupportedImagePath(const std::filesystem::path& path);

} // namespace jpegview_linux
