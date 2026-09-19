#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace jpegview_linux {

struct FileDialogEntry {
	std::filesystem::path path;
	bool directory = false;
	bool parent = false;
};

// Applies a case-insensitive filename substring filter. The parent-directory
// entry is always retained so filtering never traps the user in a directory.
std::vector<FileDialogEntry> FilterFileDialogEntries(
	const std::vector<FileDialogEntry>& entries, std::string_view filter);

} // namespace jpegview_linux
