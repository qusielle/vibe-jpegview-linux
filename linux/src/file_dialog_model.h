#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
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

struct DirectorySummary {
	std::size_t imageCount = 0;
	std::size_t subdirectoryCount = 0;
};

// Counts only direct children. Files in subdirectories are deliberately not
// visited, matching what the open dialog will show after entering the folder.
DirectorySummary CountImmediateDirectoryContents(const std::filesystem::path& directory);
std::string FormatDirectorySummary(const DirectorySummary& summary);

struct DirectorySummaryResult {
	std::filesystem::path directory;
	std::uint64_t generation = 0;
	DirectorySummary summary;
};

// Serial background scanner used by the file dialog. A new request replaces
// all queued work so navigating to another directory cannot publish stale
// summaries into the current listing.
class DirectorySummaryLoader {
public:
	DirectorySummaryLoader();
	~DirectorySummaryLoader();
	DirectorySummaryLoader(const DirectorySummaryLoader&) = delete;
	DirectorySummaryLoader& operator=(const DirectorySummaryLoader&) = delete;

	void Request(const std::vector<std::filesystem::path>& directories, std::uint64_t generation);
	std::vector<DirectorySummaryResult> TakeReady();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
