#pragma once

#include "archive_source.h"
#include "work_context.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace jpegview_linux {

struct FileDialogEntry {
	FileDialogEntry(std::filesystem::path sourcePath = {}, bool isDirectory = false,
		bool isParent = false, std::filesystem::file_time_type modified = {},
		bool archive = false, bool member = false, bool isEncrypted = false,
		std::uintmax_t size = 0, bool sizeKnown = false,
		SourceDescriptor source = {})
		: path(std::move(sourcePath)), directory(isDirectory), parent(isParent),
		  modificationTime(modified), archiveContainer(archive), archiveMember(member),
		  encrypted(isEncrypted), fileSize(size), fileSizeKnown(sizeKnown),
		  sourceDescriptor(std::move(source)) {}

	std::filesystem::path path;
	bool directory = false;
	bool parent = false;
	std::filesystem::file_time_type modificationTime{};
	bool archiveContainer = false;
	bool archiveMember = false;
	bool encrypted = false;
	std::uintmax_t fileSize = 0;
	bool fileSizeKnown = false;
	SourceDescriptor sourceDescriptor;
};

enum class FileDialogSortMode {
	Name,
	ModificationDate,
};

struct FileDialogEntrySortOrders {
	std::vector<std::size_t> name;
	std::vector<std::size_t> modificationDate;
	bool completed = false;
};

// Builds both row orders without moving or copying the authoritative
// entries. Directory workers can cancel between bounded batches and must not
// publish an incomplete order.
FileDialogEntrySortOrders BuildFileDialogEntrySortOrders(
	const std::vector<FileDialogEntry>& entries,
	const std::function<bool()>& shouldContinue = {});

const std::string& FileDialogListingLoadingMessage();
void UpdateFileDialogListingMessage(std::string& message,
	const std::string& listingMessage);
bool FileDialogShouldClearSelectionAfterListing(bool saveDialog,
	bool includeNonImageFiles, bool activationPending);

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

struct FileDialogPreviewFooterLayout {
	int filenameWidth = 0;
	int detailsWidth = 0;
	int detailsOffsetX = 0;
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
// frame inset, and combined filename/details footer.
FileDialogPreviewSize FileDialogPreviewImageSize(int paneWidth, int paneHeight);

// Shares one footer row between the left-aligned filename and right-aligned
// image details, reserving at least half the content width for the filename.
FileDialogPreviewFooterLayout CalculateFileDialogPreviewFooterLayout(
	int contentWidth, int filenameTextWidth, int detailsTextWidth, int gap = 8);

// A preview completion needs a source refresh only when the descriptor for the
// requested image no longer matches the identity observed by its worker.
bool ShouldRefreshFileDialogPreviewSource(const SourceKey& requested,
	const SourceDescriptor& observed);

class FileDialogModel {
public:
	class EntryView {
	public:
		class const_iterator {
		public:
			using iterator_category = std::forward_iterator_tag;
			using value_type = FileDialogEntry;
			using difference_type = std::ptrdiff_t;
			using pointer = const FileDialogEntry*;
			using reference = const FileDialogEntry&;

			const_iterator() = default;
			const_iterator(const std::vector<FileDialogEntry>* entries,
				const std::vector<std::size_t>* indices, std::size_t position)
				: entries_(entries), indices_(indices), position_(position) {}
			reference operator*() const { return (*entries_)[(*indices_)[position_]]; }
			pointer operator->() const { return &operator*(); }
			const_iterator& operator++() { ++position_; return *this; }
			const_iterator operator++(int) { const_iterator copy = *this; ++*this; return copy; }
			bool operator==(const const_iterator& other) const {
				return entries_ == other.entries_ && indices_ == other.indices_ &&
					position_ == other.position_;
			}
			bool operator!=(const const_iterator& other) const { return !(*this == other); }

		private:
			const std::vector<FileDialogEntry>* entries_ = nullptr;
			const std::vector<std::size_t>* indices_ = nullptr;
			std::size_t position_ = 0;
		};

		EntryView(const std::vector<FileDialogEntry>& entries,
			const std::vector<std::size_t>& indices)
			: entries_(&entries), indices_(&indices) {}
		std::size_t size() const { return indices_->size(); }
		bool empty() const { return indices_->empty(); }
		const FileDialogEntry& operator[](std::size_t index) const {
			return (*entries_)[(*indices_)[index]];
		}
		const_iterator begin() const { return {entries_, indices_, 0}; }
		const_iterator end() const { return {entries_, indices_, indices_->size()}; }

	private:
		const std::vector<FileDialogEntry>* entries_;
		const std::vector<std::size_t>* indices_;
	};

	void Begin(bool saveDialog);
	void Clear();
	void SetEntries(std::vector<FileDialogEntry> entries);
	void SetEntriesInOrder(std::vector<FileDialogEntry> entries, bool matchFullPath = false);
	void SetEntriesWithPreparedOrder(std::vector<FileDialogEntry> entries,
		FileDialogEntrySortOrders orders);

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
	bool MarkEncrypted(const std::filesystem::path& path);
	bool SetFileSize(const std::filesystem::path& path, std::uintmax_t size);
	bool RefreshSourceDescriptor(const SourceKey& expected,
		const SourceDescriptor& observed);
	void ClearSelection();

	const std::vector<FileDialogEntry>& AllEntries() const { return allEntries_; }
	EntryView Entries() const { return EntryView(allEntries_, visibleIndices_); }
	const FileDialogEntry* SelectedEntry() const;
	const std::string& Filter() const { return filter_; }
	FileDialogSortMode SortMode() const { return sortMode_; }
	int SelectedIndex() const { return selected_; }
	int Scroll() const { return scroll_; }
	bool SaveDialog() const { return saveDialog_; }

private:
	static std::string PathIndexKey(const std::filesystem::path& path);
	void RebuildPathIndex();
	void ApplyFilter();
	void EnsureSelectionVisible(int visibleRows);

	std::vector<FileDialogEntry> allEntries_;
	FileDialogEntrySortOrders sortOrders_;
	std::vector<std::size_t> visibleIndices_;
	std::vector<int> visiblePositionByEntry_;
	std::unordered_map<std::string, std::size_t> entryIndexByPath_;
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
	WorkerFailure failure;
};

struct FileDialogListingPolicy {
	bool saveDialog = false;
	bool includeNonImageFiles = false;
	bool includeArchives = true;
	std::unordered_set<std::string> encryptedArchivePaths;
};

struct FileDialogDirectoryResult {
	std::filesystem::path directory;
	std::uint64_t generation = 0;
	FileDialogListingPolicy policy;
	std::vector<FileDialogEntry> entries;
	FileDialogEntrySortOrders sortOrders;
	std::string error;
	ArchiveErrorKind errorKind = ArchiveErrorKind::None;
	bool archiveLocation = false;
	bool containsEncryptedEntries = false;
};

// Replaceable one-at-a-time directory listing for Browse. Filesystem metadata,
// archive catalogs, and both row orders are prepared away from the UI thread.
class FileDialogDirectoryLoader {
public:
	using Enumerator = std::function<FileDialogDirectoryResult(
		const std::filesystem::path&, FileDialogListingPolicy,
		const std::function<bool()>&)>;

	explicit FileDialogDirectoryLoader(Enumerator enumerator = {});
	~FileDialogDirectoryLoader();
	FileDialogDirectoryLoader(const FileDialogDirectoryLoader&) = delete;
	FileDialogDirectoryLoader& operator=(const FileDialogDirectoryLoader&) = delete;

	void Request(const std::filesystem::path& directory, std::uint64_t generation,
		FileDialogListingPolicy policy);
	void Clear(std::uint64_t generation);
	std::vector<FileDialogDirectoryResult> TakeReady();
	void Shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct FileDialogFileSizeResult {
	std::uint64_t generation = 0;
	std::filesystem::path path;
	std::uintmax_t size = 0;
	SourceDescriptor requestedSource;
	SourceDescriptor observedSource;
	WorkerFailure failure;
};

// Resolves ordinary-file and archive-member sizes away from the SDL event
// thread. Replacing a request drops queued work and prevents stale results
// from being published into a newer dialog listing.
class FileDialogFileSizeLoader {
public:
	using SourceCapture = std::function<SourceDescriptor(const SourceDescriptor&)>;

	explicit FileDialogFileSizeLoader(SourceCapture sourceCapture = {});
	~FileDialogFileSizeLoader();
	FileDialogFileSizeLoader(const FileDialogFileSizeLoader&) = delete;
	FileDialogFileSizeLoader& operator=(const FileDialogFileSizeLoader&) = delete;

	void Request(const std::vector<std::filesystem::path>& paths, std::uint64_t generation);
	void RequestSources(const std::vector<SourceDescriptor>& sources,
		std::uint64_t generation);
	void Clear(std::uint64_t generation);
	std::vector<FileDialogFileSizeResult> TakeReady(std::size_t maximumResults = 128);
	void Shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct ArchiveDirectoryResult {
	std::filesystem::path directory;
	std::uint64_t generation = 0;
	std::vector<ArchiveEntryInfo> entries;
	std::string error;
	ArchiveErrorKind errorKind = ArchiveErrorKind::None;
	bool containsEncryptedEntries = false;
	bool passwordValidation = false;
};

// Cold archive indexes, especially gzip TAR streams, are built away from the
// event thread. New directory requests cancel obsolete scans and results.
class ArchiveDirectoryLoader {
public:
	ArchiveDirectoryLoader();
	~ArchiveDirectoryLoader();
	ArchiveDirectoryLoader(const ArchiveDirectoryLoader&) = delete;
	ArchiveDirectoryLoader& operator=(const ArchiveDirectoryLoader&) = delete;

	void Request(const std::filesystem::path& directory, std::uint64_t generation);
	void RequestPasswordValidation(const std::filesystem::path& archive,
		const std::string& password, std::uint64_t generation);
	void Clear(std::uint64_t generation);
	std::vector<ArchiveDirectoryResult> TakeReady();
	void Shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
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
	bool IsYieldingForForeground() const;
	std::vector<DirectorySummaryResult> TakeReady(std::size_t maximumResults = 64);
	void Shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct FileDialogPreviewResult {
	std::uint64_t generation = 0;
	std::filesystem::path source;
	SourceDescriptor requestedSourceDescriptor;
	SourceDescriptor sourceDescriptor;
	SourceDescriptor observedSource;
	int width = 0;
	int height = 0;
	int sourceWidth = 0;
	int sourceHeight = 0;
	std::uintmax_t fileSize = 0;
	bool fileSizeKnown = false;
	bool hasTransparency = false;
	std::vector<std::uint8_t> bgra;
	std::string error;
	ArchiveErrorKind errorKind = ArchiveErrorKind::None;
	bool encryptedArchive = false;
};

// Decodes just the latest requested preview on one background worker. Preview
// pixels are returned to the UI and are deliberately not added to image caches.
class FileDialogPreviewLoader {
public:
	using Processor = std::function<FileDialogPreviewResult(
		const std::filesystem::path&, bool, FileDialogSortMode, int, int,
		const std::function<bool()>&)>;
	using SourceCapture = std::function<SourceDescriptor(const SourceDescriptor&)>;

	explicit FileDialogPreviewLoader(Processor processor = {},
		SourceCapture sourceCapture = {});
	~FileDialogPreviewLoader();
	FileDialogPreviewLoader(const FileDialogPreviewLoader&) = delete;
	FileDialogPreviewLoader& operator=(const FileDialogPreviewLoader&) = delete;

	std::uint64_t Request(const std::filesystem::path& path, bool directory,
		FileDialogSortMode mode, int maximumWidth, int maximumHeight,
		SourceDescriptor source = {});
	void Clear();
	std::vector<FileDialogPreviewResult> TakeReady();
	void Shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace jpegview_linux
