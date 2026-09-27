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
	bool archiveContainer = false;
	bool archiveMember = false;
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

// Applies a case-insensitive filename substring filter, or a full-path
// substring filter when requested. The parent-directory entry is always
// retained so filtering never traps the user in a directory.
std::vector<FileDialogEntry> FilterFileDialogEntries(
	const std::vector<FileDialogEntry>& entries, std::string_view filter,
	bool matchFullPath = false);

struct DirectorySummary {
	std::size_t imageCount = 0;
	std::size_t subdirectoryCount = 0;
};

// Counts only direct children. Supported archive containers count as
// subdirectories because the open dialog browses them like folders. Files in
// subdirectories are deliberately not visited.
DirectorySummary CountImmediateDirectoryContents(const std::filesystem::path& directory);
std::string FormatDirectorySummary(const DirectorySummary& summary);

// Resolves a directory selection to the first supported image in the order
// used by the open dialog. Returns an empty path if no image is available.
std::filesystem::path FirstImageInDirectory(
	const std::filesystem::path& directory, FileDialogSortMode mode);

struct FileDialogPreviewSize {
	int width = 0;
	int height = 0;
};

struct FileDialogScrollbarGeometry {
	int trackY = 0;
	int trackHeight = 0;
	int thumbY = 0;
	int thumbHeight = 0;
	int maximumScroll = 0;
	bool scrollable = false;
};

// Calculates a proportional scrollbar thumb while enforcing a usable minimum
// size. The supplied track coordinates and scroll offset are clamped safely.
FileDialogScrollbarGeometry CalculateFileDialogScrollbarGeometry(
	int entryCount, int visibleRows, int scroll, int trackY, int trackHeight,
	int minimumThumbHeight);

// Maps a requested thumb position back to a clamped row offset. Track clicks
// can then page the model while thumb drags use this exact inverse mapping.
int FileDialogScrollForThumbPosition(const FileDialogScrollbarGeometry& geometry,
	int requestedThumbY);

// Returns the preview image area inside a pane, accounting for its label,
// frame inset, and reserved filename footer.
FileDialogPreviewSize FileDialogPreviewImageSize(int paneWidth, int paneHeight);

class FileDialogModel {
public:
	void Begin(bool saveDialog);
	void Clear();
	void SetEntries(std::vector<FileDialogEntry> entries);
	void SetEntriesInOrder(std::vector<FileDialogEntry> entries, bool matchFullPath = false);

	void AppendFilter(std::string_view text);
	bool BackspaceFilter();
	void ClearFilter();
	void ToggleSortMode(int visibleRows);

	void MoveSelection(int direction, int visibleRows);
	void MoveSelectionByPage(int direction, int visibleRows);
	void ScrollBy(int rows, int visibleRows);
	void ScrollTo(int rows, int visibleRows);
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
	bool matchFullPath_ = false;
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

struct FileDialogPreviewResult {
	std::uint64_t generation = 0;
	std::filesystem::path source;
	int width = 0;
	int height = 0;
	bool hasTransparency = false;
	std::vector<std::uint8_t> bgra;
	std::string error;
};

// Decodes just the latest requested preview on one background worker. Preview
// pixels are returned to the UI and are deliberately not added to image caches.
class FileDialogPreviewLoader {
public:
	FileDialogPreviewLoader();
	~FileDialogPreviewLoader();
	FileDialogPreviewLoader(const FileDialogPreviewLoader&) = delete;
	FileDialogPreviewLoader& operator=(const FileDialogPreviewLoader&) = delete;

	std::uint64_t Request(const std::filesystem::path& path, bool directory,
		FileDialogSortMode mode, int maximumWidth, int maximumHeight);
	void Clear();
	std::vector<FileDialogPreviewResult> TakeReady();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
