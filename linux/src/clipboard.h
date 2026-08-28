#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jpegview_linux {

bool CopyTextToClipboard(const std::string& text, std::string& errorMessage);
bool CopyImageToClipboard(const std::uint8_t* bgra, int width, int height, std::string& errorMessage);
bool PasteImageFromClipboard(std::vector<std::uint8_t>& encodedPng, std::string& errorMessage);

} // namespace jpegview_linux
