#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace jpegview_linux {

const std::vector<std::string>& DefaultViewerMimeTypes();
std::string DefaultViewerDesktopEntry(const std::filesystem::path& executable);
bool RegisterDefaultViewer(const std::filesystem::path& executable,
	const std::filesystem::path& dataHome, const std::filesystem::path& configHome,
	std::string& errorMessage);

} // namespace jpegview_linux
