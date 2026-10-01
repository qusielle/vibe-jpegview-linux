#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jpegview_linux {

struct FileListPreparedScan;

// Linux counterpart of the Windows CFileList.  The viewer keeps image data
// separate from this class, just as the Windows file list is separate from
// CJPEGImage/CJPEGProvider.
class FileList {
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

	void SetSorting(SortMode sortMode, bool sortAscending);
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
		std::int64_t lastModificationTime = 0;
		std::int64_t creationTime = 0;
		std::uintmax_t fileSize = 0;
		std::size_t randomOrder = 0;
		bool archiveMember = false;
	};

	struct FolderState {
		std::filesystem::path directory;
		std::vector<Entry> entries;
		std::vector<std::filesystem::path> paths;
		std::vector<std::size_t> pathIndices;
		std::size_t index = 0;
	};

	static std::filesystem::path Normalize(const std::filesystem::path& path);
	static Entry DescribeFile(const std::filesystem::path& path,
		const std::function<bool()>& shouldContinue = {});
	static std::vector<Entry> ScanDirectory(const std::filesystem::path& directory,
		const std::function<bool()>& shouldContinue = {});
	static std::vector<std::filesystem::path> ChildDirectories(
		const std::filesystem::path& directory, const std::function<bool()>& shouldContinue = {});
	static void CollectDescendantDirectories(const std::filesystem::path& directory,
		std::vector<std::filesystem::path>& result,
		const std::function<bool()>& shouldContinue = {});
	static std::string Lower(std::string value);
	static int CompareLogicalNames(const std::string& left, const std::string& right);
	static bool EntryLess(const Entry& left, const Entry& right,
		SortMode sortMode, bool sortAscending);

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

	std::vector<Entry> entries_;
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
	std::filesystem::path emptyPath_;
};

// A complete, already sorted replacement list. The worker builds this object
// privately; Viewer accepts it by moving its vectors after checking the source
// revision, so no active-list copy or sorting occurs on the event thread.
struct FileListPreparedScan {
	FileList::ScanOperation operation = FileList::ScanOperation::Reload;
	std::uint64_t expectedRevision = 0;
	std::filesystem::path sourceSelectedPath;
	FileList replacement;
	bool completed = false;
	bool targetFound = false;
};

} // namespace jpegview_linux
