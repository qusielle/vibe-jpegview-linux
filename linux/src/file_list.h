#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jpegview_linux {

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

	FileList(const std::vector<std::string>& inputs,
		SortMode sortMode = SortMode::LastModificationTime,
		bool sortAscending = true,
		bool wrapAroundFolder = true);

	bool Empty() const { return entries_.empty(); }
	std::size_t Size() const { return entries_.size(); }
	std::size_t CurrentIndex() const { return currentIndex_; }
	const std::filesystem::path& Current() const;
	const std::vector<std::filesystem::path>& Files() const { return paths_; }

	bool Next();
	bool Previous();
	bool Select(std::size_t index);
	void First();
	void Last();
	bool Reload();
	bool Reload(const std::filesystem::path& preferredPath);

	void SetSorting(SortMode sortMode, bool sortAscending);
	SortMode GetSorting() const { return sortMode_; }
	bool IsSortedAscending() const { return sortAscending_; }
	bool WrapAroundFolder() const { return wrapAroundFolder_; }

	void SetNavigationMode(NavigationMode navigationMode);
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
	};

	struct FolderState {
		std::filesystem::path directory;
		std::vector<Entry> entries;
		std::size_t index = 0;
	};

	static std::filesystem::path Normalize(const std::filesystem::path& path);
	static Entry DescribeFile(const std::filesystem::path& path);
	static std::vector<Entry> ScanDirectory(const std::filesystem::path& directory);
	static std::vector<std::filesystem::path> ChildDirectories(const std::filesystem::path& directory);
	static void CollectDescendantDirectories(const std::filesystem::path& directory,
		std::vector<std::filesystem::path>& result);
	static std::string Lower(std::string value);
	static int CompareLogicalNames(const std::string& left, const std::string& right);

	void Initialize(const std::vector<std::string>& inputs);
	void SortEntries();
	void RebuildPaths();
	void LoadDirectory(const std::filesystem::path& directory, const std::filesystem::path& selected);
	bool EnterDirectory(const std::filesystem::path& directory);
	bool NavigateSiblingDirectory(int direction);
	bool RestorePreviousFolder();
	bool RestoreNextFolder();
	std::filesystem::path FindNextFolder() const;
	void PrepareDirectoryNavigation();
	std::size_t FindEntry(const std::filesystem::path& path) const;

	std::vector<Entry> entries_;
	std::vector<std::filesystem::path> paths_;
	std::vector<FolderState> previousFolders_;
	std::vector<FolderState> nextFolders_;
	std::vector<std::string> inputs_;
	std::filesystem::path currentDirectory_;
	std::filesystem::path rootDirectory_;
	std::size_t currentIndex_ = 0;
	SortMode sortMode_ = SortMode::LastModificationTime;
	bool sortAscending_ = true;
	bool wrapAroundFolder_ = true;
	NavigationMode navigationMode_ = NavigationMode::LoopDirectory;
	bool multipleInputMode_ = false;
	std::filesystem::path emptyPath_;
};

} // namespace jpegview_linux
