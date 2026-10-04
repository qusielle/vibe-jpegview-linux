#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace jpegview_linux {

bool CopyTextToClipboard(const std::string& text, std::string& errorMessage);
bool CopyImageToClipboard(const std::uint8_t* bgra, int width, int height,
	std::string& errorMessage, pid_t& clipboardOwnerChild,
	const std::function<bool()>& shouldContinue = {});
bool PasteImageFromClipboard(std::vector<std::uint8_t>& encodedPng,
	std::string& errorMessage, const std::function<bool()>& shouldContinue = {});

} // namespace jpegview_linux
