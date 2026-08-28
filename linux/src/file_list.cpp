#include "file_list.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fcntl.h>
#include <functional>
#include <linux/stat.h>
#include <system_error>
#include <sys/syscall.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace jpegview_linux {

FileList::FileList(const std::vector<std::string>& inputs, SortMode sortMode, bool sortAscending,
	bool wrapAroundFolder)
	: inputs_(inputs), sortMode_(sortMode), sortAscending_(sortAscending), wrapAroundFolder_(wrapAroundFolder) {
	Initialize(inputs);
}

const fs::path& FileList::Current() const {
	if (currentIndex_ < paths_.size()) return paths_[currentIndex_];
	return emptyPath_;
}

std::string FileList::Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

int FileList::CompareLogicalNames(const std::string& left, const std::string& right) {
	std::size_t leftPosition = 0;
	std::size_t rightPosition = 0;
	const std::string leftLower = Lower(left);
	const std::string rightLower = Lower(right);
	while (leftPosition < leftLower.size() && rightPosition < rightLower.size()) {
		const unsigned char leftCharacter = static_cast<unsigned char>(leftLower[leftPosition]);
		const unsigned char rightCharacter = static_cast<unsigned char>(rightLower[rightPosition]);
		if (std::isdigit(leftCharacter) && std::isdigit(rightCharacter)) {
			const std::size_t leftEnd = leftPosition;
			const std::size_t rightEnd = rightPosition;
			while (leftPosition < leftLower.size() && std::isdigit(static_cast<unsigned char>(leftLower[leftPosition]))) ++leftPosition;
			while (rightPosition < rightLower.size() && std::isdigit(static_cast<unsigned char>(rightLower[rightPosition]))) ++rightPosition;

			std::size_t leftSignificant = leftEnd;
			std::size_t rightSignificant = rightEnd;
			while (leftSignificant + 1 < leftPosition && leftLower[leftSignificant] == '0') ++leftSignificant;
			while (rightSignificant + 1 < rightPosition && rightLower[rightSignificant] == '0') ++rightSignificant;
			const std::size_t leftDigits = leftPosition - leftSignificant;
			const std::size_t rightDigits = rightPosition - rightSignificant;
			if (leftDigits != rightDigits) return leftDigits < rightDigits ? -1 : 1;
			const int numberComparison = leftLower.compare(leftSignificant, leftDigits, rightLower, rightSignificant, rightDigits);
			if (numberComparison != 0) return numberComparison < 0 ? -1 : 1;
			continue;
		}
		if (leftLower[leftPosition] != rightLower[rightPosition]) {
			return leftLower[leftPosition] < rightLower[rightPosition] ? -1 : 1;
		}
		++leftPosition;
		++rightPosition;
	}
	if (leftLower.size() != rightLower.size()) return leftLower.size() < rightLower.size() ? -1 : 1;
	return 0;
}

fs::path FileList::Normalize(const fs::path& path) {
	std::error_code error;
	const fs::path absolute = fs::absolute(path, error);
	return (error ? path : absolute).lexically_normal();
}

bool FileList::IsImageFile(const fs::path& path) {
	static const std::vector<std::string> extensions = {
		".jpg", ".jpeg", ".jpe", ".png", ".gif", ".bmp", ".tga",
		".psd", ".pnm", ".ppm", ".pgm", ".pic", ".webp"
	};
	const std::string extension = Lower(path.extension().string());
	return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

FileList::Entry FileList::DescribeFile(const fs::path& path) {
	Entry result;
	result.path = Normalize(path);
	result.randomOrder = std::hash<std::string>{}(result.path.string());
	std::error_code error;
	const fs::file_time_type fallbackModificationTime = fs::last_write_time(result.path, error);
	if (!error) {
		result.lastModificationTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
			fallbackModificationTime.time_since_epoch()).count();
	}
	result.creationTime = result.lastModificationTime;

	// CFileDesc stores both timestamps.  Linux's statx API exposes the birth
	// time on filesystems that support it; older kernels/filesystems fall back
	// to the modification timestamp, which is the only portable C++17 option.
	struct statx status{};
	const int statxMask = STATX_BTIME | STATX_MTIME | STATX_SIZE;
	if (syscall(SYS_statx, AT_FDCWD, result.path.c_str(), AT_STATX_SYNC_AS_STAT,
		statxMask, &status) == 0) {
		result.lastModificationTime = static_cast<std::int64_t>(status.stx_mtime.tv_sec) * 1000000000ll + status.stx_mtime.tv_nsec;
		if ((status.stx_mask & STATX_BTIME) != 0) {
			result.creationTime = static_cast<std::int64_t>(status.stx_btime.tv_sec) * 1000000000ll + status.stx_btime.tv_nsec;
		}
		result.fileSize = status.stx_size;
		return result;
	}
	result.creationTime = result.lastModificationTime;
	result.fileSize = fs::file_size(result.path, error);
	if (error) result.fileSize = 0;
	return result;
}

std::vector<FileList::Entry> FileList::ScanDirectory(const fs::path& directory) {
	std::vector<Entry> result;
	std::error_code error;
	fs::directory_iterator iterator(directory, error);
	const fs::directory_iterator end;
	while (iterator != end && !error) {
		const fs::directory_entry entry = *iterator;
		std::error_code statusError;
		if (entry.is_regular_file(statusError) && !statusError && IsImageFile(entry.path())) {
			result.push_back(DescribeFile(entry.path()));
		}
		iterator.increment(error);
	}
	return result;
}

std::vector<fs::path> FileList::ChildDirectories(const fs::path& directory) {
	std::vector<fs::path> result;
	std::error_code error;
	fs::directory_iterator iterator(directory, error);
	const fs::directory_iterator end;
	while (iterator != end && !error) {
		const fs::directory_entry entry = *iterator;
		std::error_code statusError;
		std::error_code symlinkError;
		if (!entry.is_symlink(symlinkError) && !symlinkError && entry.is_directory(statusError) && !statusError) {
			result.push_back(Normalize(entry.path()));
		}
		iterator.increment(error);
	}
	std::sort(result.begin(), result.end(), [](const fs::path& left, const fs::path& right) {
		const int nameComparison = CompareLogicalNames(left.filename().string(), right.filename().string());
		return nameComparison == 0 ? left.string() < right.string() : nameComparison < 0;
	});
	return result;
}

void FileList::CollectDescendantDirectories(const fs::path& directory, std::vector<fs::path>& result) {
	for (const fs::path& child : ChildDirectories(directory)) {
		result.push_back(child);
		CollectDescendantDirectories(child, result);
	}
}

void FileList::Initialize(const std::vector<std::string>& inputs) {
	entries_.clear();
	paths_.clear();
	previousFolders_.clear();
	nextFolders_.clear();
	currentIndex_ = 0;
	multipleInputMode_ = false;

	if (inputs.empty()) {
		std::error_code error;
		const fs::path directory = Normalize(fs::current_path(error));
		rootDirectory_ = directory;
		currentDirectory_ = directory;
		entries_ = ScanDirectory(directory);
		SortEntries();
		currentIndex_ = 0;
		RebuildPaths();
		return;
	}

	if (inputs.size() == 1) {
		const fs::path input = Normalize(inputs.front());
		std::error_code error;
		if (fs::is_directory(input, error)) {
			rootDirectory_ = input;
			currentDirectory_ = input;
			entries_ = ScanDirectory(input);
			SortEntries();
			currentIndex_ = 0;
			RebuildPaths();
			return;
		}
		if (fs::is_regular_file(input, error) && IsImageFile(input)) {
			rootDirectory_ = input.parent_path();
			currentDirectory_ = input.parent_path();
			entries_ = ScanDirectory(currentDirectory_);
			SortEntries();
			currentIndex_ = FindEntry(input);
			RebuildPaths();
			return;
		}
		return;
	}

	// Multiple command-line paths are a Linux extension.  Preserve the useful
	// existing behavior as one list, while switching to directory-local lists
	// when a Windows-style recursive/sibling navigation mode is selected.
	multipleInputMode_ = true;
	for (const std::string& inputString : inputs) {
		const fs::path input = Normalize(inputString);
		std::error_code error;
		if (fs::is_directory(input, error)) {
			const std::vector<Entry> directoryEntries = ScanDirectory(input);
			entries_.insert(entries_.end(), directoryEntries.begin(), directoryEntries.end());
		} else if (fs::is_regular_file(input, error) && IsImageFile(input)) {
			entries_.push_back(DescribeFile(input));
		}
	}
	std::sort(entries_.begin(), entries_.end(), [](const Entry& left, const Entry& right) {
		return left.path.string() < right.path.string();
	});
	entries_.erase(std::unique(entries_.begin(), entries_.end(), [](const Entry& left, const Entry& right) {
		return left.path == right.path;
	}), entries_.end());
	if (!entries_.empty()) {
		currentDirectory_ = entries_.front().path.parent_path();
		rootDirectory_ = currentDirectory_;
	}
	SortEntries();
	currentIndex_ = 0;
	RebuildPaths();
}

void FileList::SortEntries() {
	const fs::path selected = currentIndex_ < entries_.size() ? entries_[currentIndex_].path : emptyPath_;
	std::stable_sort(entries_.begin(), entries_.end(), [this](const Entry& left, const Entry& right) {
		int comparison = 0;
		switch (sortMode_) {
		case SortMode::LastModificationTime:
			comparison = left.lastModificationTime < right.lastModificationTime ? -1 :
				left.lastModificationTime > right.lastModificationTime ? 1 : 0;
			break;
		case SortMode::CreationTime:
			comparison = left.creationTime < right.creationTime ? -1 :
				left.creationTime > right.creationTime ? 1 : 0;
			break;
		case SortMode::Random:
			comparison = left.randomOrder < right.randomOrder ? -1 : left.randomOrder > right.randomOrder ? 1 : 0;
			break;
		case SortMode::FileSize:
			comparison = left.fileSize < right.fileSize ? -1 : left.fileSize > right.fileSize ? 1 : 0;
			break;
		case SortMode::FileName:
			break;
		}
		if (comparison == 0) comparison = CompareLogicalNames(left.path.filename().string(), right.path.filename().string());
		if (comparison == 0 && left.path != right.path) comparison = left.path.string() < right.path.string() ? -1 : 1;
		return sortAscending_ ? comparison < 0 : comparison > 0;
	});
	if (!selected.empty()) {
		currentIndex_ = FindEntry(selected);
	} else if (currentIndex_ >= entries_.size()) {
		currentIndex_ = entries_.empty() ? 0 : entries_.size() - 1;
	}
}

void FileList::RebuildPaths() {
	paths_.clear();
	paths_.reserve(entries_.size());
	for (const Entry& entry : entries_) paths_.push_back(entry.path);
	if (currentIndex_ >= paths_.size()) currentIndex_ = paths_.empty() ? 0 : paths_.size() - 1;
}

std::size_t FileList::FindEntry(const fs::path& path) const {
	const fs::path normalized = Normalize(path);
	for (std::size_t index = 0; index < entries_.size(); ++index) {
		if (entries_[index].path == normalized) return index;
	}
	return entries_.empty() ? 0 : entries_.size() - 1;
}

void FileList::LoadDirectory(const fs::path& directory, const fs::path& selected) {
	currentDirectory_ = Normalize(directory);
	entries_ = ScanDirectory(currentDirectory_);
	currentIndex_ = 0;
	SortEntries();
	if (!selected.empty()) currentIndex_ = FindEntry(selected);
	RebuildPaths();
}

bool FileList::EnterDirectory(const fs::path& directory) {
	std::vector<Entry> nextEntries = ScanDirectory(directory);
	if (nextEntries.empty()) return false;
	previousFolders_.push_back(FolderState{currentDirectory_, std::move(entries_), currentIndex_});
	entries_ = std::move(nextEntries);
	currentDirectory_ = Normalize(directory);
	currentIndex_ = 0;
	SortEntries();
	currentIndex_ = 0;
	nextFolders_.clear();
	multipleInputMode_ = false;
	RebuildPaths();
	return true;
}

bool FileList::RestorePreviousFolder() {
	if (previousFolders_.empty()) return false;
	nextFolders_.push_back(FolderState{currentDirectory_, std::move(entries_), currentIndex_});
	FolderState state = std::move(previousFolders_.back());
	previousFolders_.pop_back();
	currentDirectory_ = std::move(state.directory);
	entries_ = std::move(state.entries);
	currentIndex_ = state.index;
	multipleInputMode_ = false;
	RebuildPaths();
	return true;
}

bool FileList::RestoreNextFolder() {
	if (nextFolders_.empty()) return false;
	previousFolders_.push_back(FolderState{currentDirectory_, std::move(entries_), currentIndex_});
	FolderState state = std::move(nextFolders_.back());
	nextFolders_.pop_back();
	currentDirectory_ = std::move(state.directory);
	entries_ = std::move(state.entries);
	currentIndex_ = state.index;
	multipleInputMode_ = false;
	RebuildPaths();
	return true;
}

fs::path FileList::FindNextFolder() const {
	if (navigationMode_ == NavigationMode::LoopSameDirectoryLevel) {
		const fs::path parent = currentDirectory_.parent_path();
		const std::vector<fs::path> siblings = ChildDirectories(parent);
		bool foundCurrent = false;
		for (const fs::path& sibling : siblings) {
			if (sibling == currentDirectory_) {
				foundCurrent = true;
				continue;
			}
			if (foundCurrent && !ScanDirectory(sibling).empty()) return sibling;
		}
		return emptyPath_;
	}

	std::vector<fs::path> descendants;
	CollectDescendantDirectories(rootDirectory_, descendants);
	std::size_t start = 0;
	if (currentDirectory_ != rootDirectory_) {
		const auto current = std::find(descendants.begin(), descendants.end(), currentDirectory_);
		if (current == descendants.end()) return emptyPath_;
		start = static_cast<std::size_t>(std::distance(descendants.begin(), current)) + 1;
	}
	for (std::size_t index = start; index < descendants.size(); ++index) {
		if (!ScanDirectory(descendants[index]).empty()) return descendants[index];
	}
	return emptyPath_;
}

void FileList::PrepareDirectoryNavigation() {
	if (!multipleInputMode_ || Current().empty()) return;
	const fs::path selected = Current();
	const fs::path directory = selected.parent_path();
	multipleInputMode_ = false;
	previousFolders_.clear();
	nextFolders_.clear();
	rootDirectory_ = directory;
	LoadDirectory(directory, selected);
}

bool FileList::Next() {
	if (entries_.empty()) return false;
	if (currentIndex_ + 1 < entries_.size()) {
		++currentIndex_;
		return true;
	}
	if (RestoreNextFolder()) return true;
	if (navigationMode_ == NavigationMode::LoopDirectory || multipleInputMode_) {
		if (!wrapAroundFolder_) return false;
		currentIndex_ = 0;
		return true;
	}
	const fs::path nextDirectory = FindNextFolder();
	return nextDirectory.empty() ? false : EnterDirectory(nextDirectory);
}

bool FileList::Previous() {
	if (entries_.empty()) return false;
	if (currentIndex_ > 0) {
		--currentIndex_;
		return true;
	}
	if (RestorePreviousFolder()) return true;
	if (navigationMode_ == NavigationMode::LoopDirectory || multipleInputMode_) {
		if (!wrapAroundFolder_) return false;
		currentIndex_ = entries_.size() - 1;
		return true;
	}
	return false;
}

void FileList::First() {
	if (!entries_.empty()) currentIndex_ = 0;
}

void FileList::Last() {
	if (!entries_.empty()) currentIndex_ = entries_.size() - 1;
}

bool FileList::Reload() {
	const fs::path selected = Current();
	if (selected.empty()) return false;
	if (multipleInputMode_) {
		Initialize(inputs_);
		if (Current().empty()) return false;
		currentIndex_ = FindEntry(selected);
		RebuildPaths();
		return true;
	}
	LoadDirectory(currentDirectory_, selected);
	nextFolders_.clear();
	return !entries_.empty();
}

void FileList::SetSorting(SortMode sortMode, bool sortAscending) {
	const fs::path selected = Current();
	sortMode_ = sortMode;
	sortAscending_ = sortAscending;
	SortEntries();
	if (!selected.empty()) currentIndex_ = FindEntry(selected);
	RebuildPaths();
}

void FileList::SetNavigationMode(NavigationMode navigationMode) {
	if (navigationMode_ == navigationMode) return;
	navigationMode_ = navigationMode;
	previousFolders_.clear();
	nextFolders_.clear();
	if (navigationMode_ != NavigationMode::LoopDirectory) {
		PrepareDirectoryNavigation();
		if (!currentDirectory_.empty()) rootDirectory_ = currentDirectory_;
	}
}

} // namespace jpegview_linux
