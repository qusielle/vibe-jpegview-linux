#pragma once

#include "archive_source.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jpegview_linux {

struct FileListPreparedScan;
struct FileListSortRequest;
struct FileListPreparedSort;

// Linux counterpart of the Windows CFileList.  The viewer keeps image data
// separate from this class, just as the Windows file list is separate from
// CJPEGImage/CJPEGProvider.
class FileList {
	friend class FileListSortWorker;
	friend struct FileListSortRequest;
	friend struct FileListPreparedSort;
	friend struct FileListPreparedScan;
public:
	enum class SortMode {
		LastModificationTime,
		CreationTime,
		FileName,
		Random,
		FileSize,
	};

	enum class NavigationMode {
		LoopDirectory,
		LoopSubDirectories,
		LoopSameDirectoryLevel,
	};

	FileList() = default;
	FileList(const std::vector<std::string>& inputs,
		SortMode sortMode = SortMode::LastModificationTime,
		bool sortAscending = true,
		bool wrapAroundFolder = true);

	bool Empty() const { return entries_.empty(); }
	std::size_t Size() const { return entries_.size(); }
	std::size_t CurrentIndex() const { return currentIndex_; }
	const std::filesystem::path& Current() const;
	const std::filesystem::path& BrowseLocationOnEmpty() const { return browseLocationOnEmpty_; }
	const std::vector<std::filesystem::path>& Files() const { return paths_; }
	const SourceDescriptor* DescriptorAt(std::size_t index) const {
		return index < entries_.size() ? &entries_[index].source : nullptr;
	}
	bool IsArchiveMember(std::size_t index) const {
		return index < entries_.size() && entries_[index].archiveMember;
	}

	bool Next();
	bool Previous();
	bool Select(std::size_t index);
	bool MarkCurrentForToggle();
	bool ToggleBetweenMarkedAndCurrent();
	std::filesystem::path MarkedToggleTarget() const;
	std::optional<std::size_t> IndexOf(const std::filesystem::path& path) const;
	bool ContainsPath(const std::filesystem::path& path) const;
	bool CompleteMarkedToggle(const std::filesystem::path& previousPath);
	bool HasMarkedFile() const { return !markedFile_.empty(); }
	std::optional<std::size_t> MarkedIndex() const;
	void First();
	void Last();
	bool Reload();
	bool Reload(const std::filesystem::path& preferredPath);
	std::filesystem::path CurrentDirectory() const { return currentDirectory_; }

	enum class ScanOperation {
		Initialize,
		Reload,
		ForwardBoundary,
		PreviousSibling,
		NextSibling,
		PrepareDirectoryScope,
		MarkedToggleTarget,
	};

	enum class LoadedNavigationResult {
		Moved,
		NeedsDirectoryScan,
		NoMove,
	};

	struct ScanRequest {
		ScanOperation operation = ScanOperation::Reload;
		std::shared_ptr<const std::vector<std::string>> inputs;
		std::filesystem::path currentDirectory;
		std::filesystem::path rootDirectory;
		std::filesystem::path selectedPath;
		SortMode sortMode = SortMode::LastModificationTime;
		NavigationMode navigationMode = NavigationMode::LoopDirectory;
		std::uint64_t expectedRevision = 0;
		std::uint64_t expectedDescriptorRevision = 0;
		bool sortAscending = true;
		bool wrapAroundFolder = true;
		bool multipleInputMode = false;
		int direction = 0;
	};

	static ScanRequest InitialScanRequest(const std::vector<std::string>& inputs,
		SortMode sortMode, bool sortAscending, bool wrapAroundFolder,
		NavigationMode navigationMode);
	ScanRequest MakeScanRequest(ScanOperation operation, int direction = 0) const;
	static FileListPreparedScan PrepareScan(const ScanRequest& request,
		const std::function<bool()>& shouldContinue);
	bool ApplyPreparedScan(FileListPreparedScan&& scan,
		const std::filesystem::path& preferredPathAtApply = {});
	void SetProvisionalInputs(const std::vector<std::string>& inputs);
	LoadedNavigationResult NextLoaded();
	LoadedNavigationResult PreviousLoaded();
	std::uint64_t MutationRevision() const { return mutationRevision_; }
	std::uint64_t DescriptorRevision() const { return descriptorRevision_; }
	// Refreshes one descriptor after an application-owned write or a worker
	// reports that the backing source changed. The list order is rebuilt only
	// when the active metadata sort key changed.
	bool RefreshSourceDescriptor(const std::filesystem::path& path);
	bool RefreshSourceDescriptor(const SourceDescriptor& source, bool deferSort = false,
		bool* sortKeyChanged = nullptr);
	bool RefreshSourceDescriptor(const SourceKey& expected,
		const SourceDescriptor& observed, bool deferSort = false,
		bool* sortKeyChanged = nullptr);

	void SetSorting(SortMode sortMode, bool sortAscending);
	FileListSortRequest MakeSortRequest(SortMode sortMode, bool sortAscending);
	static FileListPreparedSort PrepareSort(const FileListSortRequest& request,
		const std::function<bool()>& shouldContinue);
	bool ApplyPreparedSort(FileListPreparedSort& prepared);
	SortMode GetSorting() const { return sortMode_; }
	bool IsSortedAscending() const { return sortAscending_; }
	bool WrapAroundFolder() const { return wrapAroundFolder_; }
	void SetWrapAroundFolder(bool enabled) { wrapAroundFolder_ = enabled; }

	// Returns true when a multiple-input list needs an off-thread directory scan
	// before it can take on the selected image's folder scope.
	bool SetNavigationMode(NavigationMode navigationMode);
	NavigationMode GetNavigationMode() const { return navigationMode_; }
	bool PreviousSiblingDirectory();
	bool NextSiblingDirectory();

private:
	struct Entry {
		std::filesystem::path path;
		std::string foldedFilename;
		SourceDescriptor source;
		std::int64_t lastModificationTime = 0;
		std::int64_t creationTime = 0;
		std::uintmax_t fileSize = 0;
		std::size_t randomOrder = 0;
		bool archiveMember = false;
	};

	class EntryStorage {
	public:
		EntryStorage() : entries_(std::make_shared<std::vector<Entry>>()) {}
		explicit EntryStorage(std::vector<Entry>&& entries)
			: entries_(std::make_shared<std::vector<Entry>>(std::move(entries))) {}
		explicit EntryStorage(std::shared_ptr<std::vector<Entry>> entries)
			: entries_(entries ? std::move(entries) : std::make_shared<std::vector<Entry>>()) {}

		EntryStorage& operator=(std::vector<Entry>&& entries) {
			entries_ = std::make_shared<std::vector<Entry>>(std::move(entries));
			return *this;
		}
		std::size_t size() const { return entries_->size(); }
		bool empty() const { return entries_->empty(); }
		const Entry& operator[](std::size_t index) const { return (*entries_)[index]; }
		Entry& operator[](std::size_t index) { EnsureUnique(); return (*entries_)[index]; }
		const Entry& front() const { return entries_->front(); }
		Entry& front() { EnsureUnique(); return entries_->front(); }
		auto begin() const { return entries_->begin(); }
		auto end() const { return entries_->end(); }
		auto begin() { EnsureUnique(); return entries_->begin(); }
		auto end() { EnsureUnique(); return entries_->end(); }
		void clear() { EnsureUnique(); entries_->clear(); }
		void push_back(const Entry& entry) { EnsureUnique(); entries_->push_back(entry); }
		void push_back(Entry&& entry) { EnsureUnique(); entries_->push_back(std::move(entry)); }
		template<class PositionIterator, class InputIterator>
		void insert(PositionIterator position, InputIterator first, InputIterator last) {
			const auto offset = std::distance(entries_->begin(), position);
			EnsureUnique();
			entries_->insert(entries_->begin() + offset, first, last);
		}
		template<class Iterator>
		Iterator erase(Iterator first, Iterator last) {
			const auto offset = std::distance(entries_->begin(), first);
			const auto count = std::distance(first, last);
			EnsureUnique();
			return entries_->erase(entries_->begin() + offset,
				entries_->begin() + offset + count);
		}
		std::shared_ptr<const std::vector<Entry>> Snapshot() const { return entries_; }
		std::shared_ptr<std::vector<Entry>> ShareMutable() const { return entries_; }

	private:
		void EnsureUnique() {
			if (!entries_.unique()) entries_ = std::make_shared<std::vector<Entry>>(*entries_);
		}
		std::shared_ptr<std::vector<Entry>> entries_;
	};

	struct FolderState {
		std::filesystem::path directory;
		EntryStorage entries;
		std::vector<std::filesystem::path> paths;
		std::vector<std::size_t> pathIndices;
		std::size_t index = 0;
	};

	static std::filesystem::path Normalize(const std::filesystem::path& path);
	static Entry DescribeFile(const std::filesystem::path& path,
		const std::function<bool()>& shouldContinue = {});
	static std::vector<Entry> ScanDirectory(const std::filesystem::path& directory,
		const std::function<bool()>& shouldContinue = {});
	static bool IncludeExplicitContentFile(std::vector<Entry>& entries,
		const std::filesystem::path& path,
		const std::function<bool()>& shouldContinue = {});
	static std::vector<std::filesystem::path> ChildDirectories(
		const std::filesystem::path& directory, const std::function<bool()>& shouldContinue = {});
	static void CollectDescendantDirectories(const std::filesystem::path& directory,
		std::vector<std::filesystem::path>& result,
		const std::function<bool()>& shouldContinue = {});
	static std::string Lower(std::string value);
	static int CompareLogicalNames(const std::string& left, const std::string& right);
	static int CompareFoldedLogicalNames(const std::string& left, const std::string& right);
	static bool EntryLess(const Entry& left, const Entry& right,
		SortMode sortMode, bool sortAscending);
	static void PrepareEntrySortKey(Entry& entry);

	void Initialize(const std::vector<std::string>& inputs,
		const std::function<bool()>& shouldContinue = {});
	void SortEntries();
	void RebuildPathIndex();
	void RebuildPaths();
	void UpdateMarkedIndex();
	void LoadDirectory(const std::filesystem::path& directory, const std::filesystem::path& selected);
	bool EnterDirectory(const std::filesystem::path& directory, bool rememberCurrentFolder = true);
	bool NavigateSiblingDirectory(int direction);
	bool RestorePreviousFolder();
	bool RestoreNextFolder();
	std::filesystem::path FindNextFolder() const;
	void PrepareDirectoryNavigation();
	std::size_t FindEntry(const std::filesystem::path& path) const;
	bool SelectPath(const std::filesystem::path& path);

	EntryStorage entries_;
	std::vector<std::filesystem::path> paths_;
	std::vector<std::size_t> pathIndices_;
	std::vector<FolderState> previousFolders_;
	std::vector<FolderState> nextFolders_;
	std::shared_ptr<const std::vector<std::string>> inputs_;
	std::filesystem::path currentDirectory_;
	std::filesystem::path rootDirectory_;
	std::filesystem::path browseLocationOnEmpty_;
	std::filesystem::path markedFile_;
	std::filesystem::path markedFileCurrent_;
	std::optional<std::size_t> markedIndex_;
	std::size_t currentIndex_ = 0;
	int markedToggleIndex_ = -1;
	SortMode sortMode_ = SortMode::LastModificationTime;
	bool sortAscending_ = true;
	bool wrapAroundFolder_ = true;
	NavigationMode navigationMode_ = NavigationMode::LoopDirectory;
	bool multipleInputMode_ = false;
	std::uint64_t mutationRevision_ = 0;
	std::uint64_t descriptorRevision_ = 0;
	std::filesystem::path emptyPath_;
};

struct SourceRefreshOutcome {
	bool applied = false;
	bool selectedSourceChanged = false;
	bool sortKeyChanged = false;
	bool orderChanged = false;
	std::optional<std::size_t> previousIndex;
	std::optional<std::size_t> currentIndex;
};

enum class SourceRefreshDisplayAction {
	NoCurrentChange,
	ReloadCurrent,
	PreserveCurrentPixels,
};

// Applies an exact-key worker/application refresh and reports the effects that
// determine whether renderer work tied to a list index or selected source must
// be rebuilt.
SourceRefreshOutcome RefreshFileListSource(FileList& files,
	const SourceKey& expected, const SourceDescriptor& observed,
	bool deferSort = false);
SourceRefreshDisplayAction ResolveSourceRefreshDisplayAction(
	const SourceRefreshOutcome& refresh, bool preserveCurrentPixels);

// A complete, already sorted replacement list. The worker builds this object
// privately; Viewer accepts it by moving its vectors after checking the list
// and descriptor revisions, so no active-list copy or sorting occurs on the
// event thread.
struct FileListPreparedScan {
	FileList::ScanOperation operation = FileList::ScanOperation::Reload;
	std::uint64_t expectedRevision = 0;
	std::uint64_t expectedDescriptorRevision = 0;
	std::filesystem::path sourceSelectedPath;
	FileList replacement;
	std::shared_ptr<FileList> retiredFileList;
	std::shared_ptr<std::vector<FileList::Entry>> retiredEntries;
	std::shared_ptr<std::vector<std::filesystem::path>> retiredPaths;
	std::shared_ptr<std::vector<std::size_t>> retiredPathIndices;
	bool completed = false;
	bool targetFound = false;
};

// The active list shares this immutable storage with the sorting worker. The
// worker copies and orders entries off-thread; navigation remains on the old
// order until ApplyPreparedSort accepts the complete result.
struct FileListSortRequest {
	std::shared_ptr<const std::vector<FileList::Entry>> entries;
	FileList::SortMode sortMode = FileList::SortMode::LastModificationTime;
	std::uint64_t expectedRevision = 0;
	std::uint64_t expectedDescriptorRevision = 0;
	bool sortAscending = true;
};

struct FileListPreparedSort {
	std::shared_ptr<std::vector<FileList::Entry>> entries;
	std::vector<std::filesystem::path> paths;
	std::vector<std::size_t> pathIndices;
	std::shared_ptr<std::vector<FileList::Entry>> retiredEntries;
	std::shared_ptr<std::vector<std::filesystem::path>> retiredPaths;
	std::shared_ptr<std::vector<std::size_t>> retiredPathIndices;
	std::uint64_t expectedRevision = 0;
	std::uint64_t expectedDescriptorRevision = 0;
	FileList::SortMode sortMode = FileList::SortMode::LastModificationTime;
	bool sortAscending = true;
	bool completed = false;
};

} // namespace jpegview_linux
