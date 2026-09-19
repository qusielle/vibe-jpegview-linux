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
	std::filesystem::file_time_type modificationTime{};
};

enum class FileDialogSortMode {
	Name,
	ModificationDate,
};

// Keeps the parent entry first and directories before files. Names are sorted
// case-insensitively in ascending order; modification dates are newest first,
// with names providing deterministic ordering for equal timestamps.
void SortFileDialogEntries(std::vector<FileDialogEntry>& entries, FileDialogSortMode mode);

// Removes one complete UTF-8 code point from the end. Invalid trailing byte
// sequences are still removed without touching preceding valid text.
bool EraseLastUtf8CodePoint(std::string& text);

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

class FileDialogModel {
public:
	void Begin(bool saveDialog);
	void Clear();
	void SetEntries(std::vector<FileDialogEntry> entries);

	void AppendFilter(std::string_view text);
	bool BackspaceFilter();
	void ClearFilter();
	void ToggleSortMode(int visibleRows);

	void MoveSelection(int direction, int visibleRows);
	void MoveSelectionByPage(int direction, int visibleRows);
	void SelectFirst(int visibleRows);
	void SelectLast(int visibleRows);
	void Select(int index, int visibleRows);
	bool Focus(const std::filesystem::path& path, int visibleRows);
	void ClearSelection();

	const std::vector<FileDialogEntry>& AllEntries() const { return allEntries_; }
	const std::vector<FileDialogEntry>& Entries() const { return entries_; }
	const FileDialogEntry* SelectedEntry() const;
	const std::string& Filter() const { return filter_; }
	FileDialogSortMode SortMode() const { return sortMode_; }
	int SelectedIndex() const { return selected_; }
	int Scroll() const { return scroll_; }
	bool SaveDialog() const { return saveDialog_; }

private:
	void ApplyFilter();
	void EnsureSelectionVisible(int visibleRows);

	std::vector<FileDialogEntry> allEntries_;
	std::vector<FileDialogEntry> entries_;
	std::string filter_;
	FileDialogSortMode sortMode_ = FileDialogSortMode::Name;
	int selected_ = -1;
	int scroll_ = 0;
	bool saveDialog_ = false;
};

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
