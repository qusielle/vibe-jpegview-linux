#include "file_list.h"
#include "archive_source.h"
#include "image_formats.h"
#include "source_work_coordinator.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fcntl.h>
#include <functional>
#include <linux/stat.h>
#include <numeric>
#include <system_error>
#include <sys/syscall.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace jpegview_linux {

namespace {

bool ContinueScan(const std::function<bool()>& shouldContinue) {
	return !shouldContinue || shouldContinue();
}

bool SameSourceMetadata(const SourceMetadata& left, const SourceMetadata& right) {
	return left.fileSize == right.fileSize &&
		left.modificationTimeNanoseconds == right.modificationTimeNanoseconds &&
		left.creationTimeNanoseconds == right.creationTimeNanoseconds &&
		left.width == right.width && left.height == right.height &&
		left.hasFileSize == right.hasFileSize &&
		left.hasModificationTime == right.hasModificationTime &&
		left.hasCreationTime == right.hasCreationTime &&
		left.hasDimensions == right.hasDimensions &&
		left.hasTransparency == right.hasTransparency &&
		left.transparencyKnown == right.transparencyKnown &&
		left.archiveMember == right.archiveMember &&
		left.archiveMemberEncrypted == right.archiveMemberEncrypted;
}

constexpr std::size_t kDirectoryEnumerationBatchSize = 32;

bool AcquireDirectoryEnumerationLease(const fs::path& directory,
	const std::function<bool()>& shouldContinue, bool& interruptedForForeground,
	WorkContext& context, SourceWorkLease& lease) {
	WorkContext supplied;
	supplied.shouldContinue = shouldContinue;
	context = ResolveWorkContext(directory, SourceWorkPriority::Metadata, supplied);
	const WorkContext directoryContext = MakePathWorkContext(directory,
		SourceWorkPriority::Metadata);
	context.source = directoryContext.source;
	context.sourcePriority = SourceWorkPriority::Metadata;
	context.sourceAccessAlreadyAdmitted = false;
	context.cpuProcessingAlreadyAdmitted = false;
	const std::function<bool()> inheritedContinue = context.shouldContinue;
	const std::function<void()> inheritedYield = context.onForegroundYield;
	const std::function<SourceWorkPriority()> inheritedPriority = context.currentPriority;
	context.onForegroundYield = [&interruptedForForeground, inheritedYield] {
		interruptedForForeground = true;
		if (inheritedYield) inheritedYield();
	};
	context.shouldContinue = [&interruptedForForeground, inheritedContinue,
		inheritedPriority, priority = context.sourcePriority, inheritedYield] {
		if (inheritedContinue && !inheritedContinue()) return false;
		const SourceWorkPriority currentPriority = inheritedPriority ?
			inheritedPriority() : priority;
		if (currentPriority != SourceWorkPriority::Foreground &&
			SourceWorkCoordinator::Global().Snapshot().foregroundPending) {
			interruptedForForeground = true;
			if (inheritedYield) inheritedYield();
			return false;
		}
		return true;
	};
	lease = SourceWorkCoordinator::Global().Acquire(context, directory);
	if (!lease || !context.Continue()) {
		if (lease) lease.Reset();
		return false;
	}
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	return true;
}

void ReleaseDirectoryEnumerationLease(WorkContext& context, SourceWorkLease& lease) {
	context.sourceAccessAlreadyAdmitted = false;
	lease.Reset();
}

bool ClassifyArchiveDirectory(const fs::path& directory,
	const std::function<bool()>& shouldContinue, bool& interruptedForForeground,
	bool& archiveLocation) {
	archiveLocation = false;
	WorkContext context;
	SourceWorkLease lease;
	if (!AcquireDirectoryEnumerationLease(directory, shouldContinue,
		interruptedForForeground, context, lease)) return false;
	{
		ScopedWorkContext activeContext(context);
		if (context.Continue()) archiveLocation = IsArchiveLocation(directory);
		ReleaseDirectoryEnumerationLease(context, lease);
	}
	return !interruptedForForeground && ContinueScan(shouldContinue);
}

enum class InputPathKind {
	Unsupported,
	ArchiveContainer,
	ArchiveMember,
	Directory,
	RegularFile,
};

bool ClassifyInputPath(const fs::path& path,
	const std::function<bool()>& shouldContinue, InputPathKind& kind) {
	kind = InputPathKind::Unsupported;
	bool interruptedForForeground = false;
	WorkContext context;
	SourceWorkLease lease;
	if (!AcquireDirectoryEnumerationLease(path, shouldContinue,
		interruptedForForeground, context, lease)) return false;
	{
		ScopedWorkContext activeContext(context);
		if (!context.Continue()) {
			ReleaseDirectoryEnumerationLease(context, lease);
			return false;
		}
		if (IsArchiveContainerFile(path)) {
			kind = InputPathKind::ArchiveContainer;
		} else if (IsArchiveMemberLocation(path)) {
			kind = InputPathKind::ArchiveMember;
		} else {
			std::error_code error;
			if (fs::is_directory(path, error) && !error) {
				kind = InputPathKind::Directory;
			} else if (!error && fs::is_regular_file(path, error) && !error) {
				kind = InputPathKind::RegularFile;
			}
		}
		ReleaseDirectoryEnumerationLease(context, lease);
	}
	return !interruptedForForeground && ContinueScan(shouldContinue);
}

} // namespace

FileList::FileList(const std::vector<std::string>& inputs, SortMode sortMode, bool sortAscending,
	bool wrapAroundFolder)
	: inputs_(std::make_shared<const std::vector<std::string>>(inputs)), sortMode_(sortMode),
	  sortAscending_(sortAscending), wrapAroundFolder_(wrapAroundFolder) {
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

FileList::Entry FileList::DescribeFile(const fs::path& path,
	const std::function<bool()>& shouldContinue) {
	Entry result;
	result.path = Normalize(path);
	result.randomOrder = std::hash<std::string>{}(result.path.string());
	if (!ContinueScan(shouldContinue)) return result;
	WorkContext suppliedContext;
	suppliedContext.shouldContinue = shouldContinue;
	WorkContext context = ResolveWorkContext(result.path,
		SourceWorkPriority::Metadata, suppliedContext);
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(context,
		result.path);
	if (!admission || !context.Continue()) return result;
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	ScopedWorkContext activeContext(context);
	result.source = DescribeImageSource(result.path, context);
	if (!context.Continue()) return Entry{};
	const SourceMetadata& metadata = result.source.Metadata();
	result.lastModificationTime = metadata.hasModificationTime ?
		metadata.modificationTimeNanoseconds : 0;
	result.creationTime = metadata.hasCreationTime ?
		metadata.creationTimeNanoseconds : result.lastModificationTime;
	result.fileSize = metadata.hasFileSize ? metadata.fileSize : 0;
	result.archiveMember = metadata.archiveMember;
	return result;
}

std::vector<FileList::Entry> FileList::ScanDirectory(const fs::path& directory,
	const std::function<bool()>& shouldContinue) {
	std::vector<Entry> result;
	if (!ContinueScan(shouldContinue)) return result;
	bool interruptedForForeground = false;
	bool archiveLocation = false;
	if (!ClassifyArchiveDirectory(directory, shouldContinue,
		interruptedForForeground, archiveLocation)) return result;
	if (archiveLocation) {
		std::vector<ArchiveEntryInfo> archiveEntries;
		std::string errorMessage;
		if (!ListArchiveDirectoryCancellable(directory, archiveEntries,
			[&shouldContinue] { return ContinueScan(shouldContinue); }, errorMessage)) return result;
		for (const ArchiveEntryInfo& archiveEntry : archiveEntries) {
			if (!ContinueScan(shouldContinue)) return {};
			if (!archiveEntry.directory && IsSupportedImagePath(archiveEntry.path)) {
				Entry entry;
				entry.source = DescribeArchiveMember(archiveEntry.path,
					archiveEntry.backingIdentity, archiveEntry.size,
					archiveEntry.modificationTime, archiveEntry.encrypted);
				entry.path = entry.source.LogicalPath();
				entry.lastModificationTime = entry.source.Metadata().modificationTimeNanoseconds;
				entry.creationTime = entry.lastModificationTime;
				entry.fileSize = archiveEntry.size;
				entry.randomOrder = std::hash<std::string>{}(entry.path.string());
				entry.archiveMember = true;
				result.push_back(std::move(entry));
			}
		}
		return result;
	}
	std::error_code error;
	fs::directory_iterator iterator;
	const fs::directory_iterator end;
	bool iteratorStarted = false;
	while (!iteratorStarted || (iterator != end && !error)) {
		std::vector<fs::path> imagePaths;
		WorkContext context;
		SourceWorkLease lease;
		if (!AcquireDirectoryEnumerationLease(directory, shouldContinue,
			interruptedForForeground, context, lease)) return {};
		{
			ScopedWorkContext activeContext(context);
			if (!iteratorStarted) {
				iterator = fs::directory_iterator(directory, error);
				iteratorStarted = true;
			}
			std::size_t batchEntries = 0;
			while (iterator != end && !error && batchEntries < kDirectoryEnumerationBatchSize) {
				if (!context.Continue()) break;
				const fs::directory_entry entry = *iterator;
				std::error_code statusError;
				if (entry.is_regular_file(statusError) && !statusError &&
					IsSupportedImagePath(entry.path())) {
					imagePaths.push_back(entry.path());
				}
				iterator.increment(error);
				++batchEntries;
			}
			ReleaseDirectoryEnumerationLease(context, lease);
		}
		if (interruptedForForeground || !ContinueScan(shouldContinue)) return {};
		for (const fs::path& imagePath : imagePaths) {
			if (!ContinueScan(shouldContinue)) return {};
			result.push_back(DescribeFile(imagePath, shouldContinue));
			if (!ContinueScan(shouldContinue)) return {};
		}
	}
	return result;
}

std::vector<fs::path> FileList::ChildDirectories(const fs::path& directory,
	const std::function<bool()>& shouldContinue) {
	std::vector<fs::path> result;
	if (!ContinueScan(shouldContinue)) return result;
	bool interruptedForForeground = false;
	bool archiveLocation = false;
	if (!ClassifyArchiveDirectory(directory, shouldContinue,
		interruptedForForeground, archiveLocation)) return result;
	if (archiveLocation) {
		std::vector<ArchiveEntryInfo> archiveEntries;
		std::string errorMessage;
		if (ListArchiveDirectoryCancellable(directory, archiveEntries,
			[&shouldContinue] { return ContinueScan(shouldContinue); }, errorMessage)) {
			for (const ArchiveEntryInfo& entry : archiveEntries) {
				if (!ContinueScan(shouldContinue)) return {};
				if (entry.directory) result.push_back(Normalize(entry.path));
			}
		}
		std::sort(result.begin(), result.end(), [](const fs::path& left, const fs::path& right) {
			const int nameComparison = CompareLogicalNames(left.filename().string(), right.filename().string());
			return nameComparison == 0 ? left.string() < right.string() : nameComparison < 0;
		});
		return result;
	}
	std::error_code error;
	fs::directory_iterator iterator;
	const fs::directory_iterator end;
	bool iteratorStarted = false;
	while (!iteratorStarted || (iterator != end && !error)) {
		WorkContext context;
		SourceWorkLease lease;
		if (!AcquireDirectoryEnumerationLease(directory, shouldContinue,
			interruptedForForeground, context, lease)) return {};
		{
			ScopedWorkContext activeContext(context);
			if (!iteratorStarted) {
				iterator = fs::directory_iterator(directory, error);
				iteratorStarted = true;
			}
			std::size_t batchEntries = 0;
			while (iterator != end && !error && batchEntries < kDirectoryEnumerationBatchSize) {
				if (!context.Continue()) break;
				const fs::directory_entry entry = *iterator;
				std::error_code statusError;
				std::error_code symlinkError;
				if (!entry.is_symlink(symlinkError) && !symlinkError &&
					entry.is_directory(statusError) && !statusError) {
					result.push_back(Normalize(entry.path()));
				}
				iterator.increment(error);
				++batchEntries;
			}
			ReleaseDirectoryEnumerationLease(context, lease);
		}
		if (interruptedForForeground || !ContinueScan(shouldContinue)) return {};
	}
	std::sort(result.begin(), result.end(), [](const fs::path& left, const fs::path& right) {
		const int nameComparison = CompareLogicalNames(left.filename().string(), right.filename().string());
		return nameComparison == 0 ? left.string() < right.string() : nameComparison < 0;
	});
	return result;
}

void FileList::CollectDescendantDirectories(const fs::path& directory,
	std::vector<fs::path>& result, const std::function<bool()>& shouldContinue) {
	std::vector<fs::path> pending;
	std::vector<fs::path> firstLevel = ChildDirectories(directory, shouldContinue);
	for (auto child = firstLevel.rbegin(); child != firstLevel.rend(); ++child) {
		pending.push_back(std::move(*child));
	}
	while (!pending.empty() && ContinueScan(shouldContinue)) {
		fs::path current = std::move(pending.back());
		pending.pop_back();
		result.push_back(current);
		std::vector<fs::path> children = ChildDirectories(current, shouldContinue);
		for (auto child = children.rbegin(); child != children.rend(); ++child) {
			if (!ContinueScan(shouldContinue)) return;
			pending.push_back(std::move(*child));
		}
	}
}

void FileList::Initialize(const std::vector<std::string>& inputs,
	const std::function<bool()>& shouldContinue) {
	entries_.clear();
	paths_.clear();
	pathIndices_.clear();
	markedIndex_.reset();
	previousFolders_.clear();
	nextFolders_.clear();
	currentIndex_ = 0;
	multipleInputMode_ = false;
	browseLocationOnEmpty_.clear();

	if (inputs.empty()) {
		std::error_code error;
		const fs::path directory = Normalize(fs::current_path(error));
		rootDirectory_ = directory;
		currentDirectory_ = directory;
		entries_ = ScanDirectory(directory, shouldContinue);
		SortEntries();
		currentIndex_ = 0;
		RebuildPaths();
		return;
	}

	if (inputs.size() == 1) {
		const fs::path input = Normalize(inputs.front());
		InputPathKind inputKind = InputPathKind::Unsupported;
		if (!ClassifyInputPath(input, shouldContinue, inputKind)) return;
		if (inputKind == InputPathKind::ArchiveContainer) {
			rootDirectory_ = input;
			currentDirectory_ = input;
			browseLocationOnEmpty_ = input;
			entries_ = ScanDirectory(input, shouldContinue);
			SortEntries();
			currentIndex_ = 0;
			RebuildPaths();
			return;
		}
		if (inputKind == InputPathKind::ArchiveMember && IsSupportedImagePath(input)) {
			rootDirectory_ = input.parent_path();
			currentDirectory_ = input.parent_path();
			entries_ = ScanDirectory(currentDirectory_, shouldContinue);
			SortEntries();
			currentIndex_ = FindEntry(input);
			RebuildPaths();
			return;
		}
		if (inputKind == InputPathKind::Directory) {
			rootDirectory_ = input;
			currentDirectory_ = input;
			browseLocationOnEmpty_ = input;
			entries_ = ScanDirectory(input, shouldContinue);
			SortEntries();
			currentIndex_ = 0;
			RebuildPaths();
			return;
		}
		if (inputKind == InputPathKind::RegularFile && IsSupportedImagePath(input)) {
			rootDirectory_ = input.parent_path();
			currentDirectory_ = input.parent_path();
			entries_ = ScanDirectory(currentDirectory_, shouldContinue);
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
		if (!ContinueScan(shouldContinue)) return;
		const fs::path input = Normalize(inputString);
		InputPathKind inputKind = InputPathKind::Unsupported;
		if (!ClassifyInputPath(input, shouldContinue, inputKind)) return;
		if (inputKind == InputPathKind::ArchiveContainer) {
			const std::vector<Entry> archiveEntries = ScanDirectory(input, shouldContinue);
			entries_.insert(entries_.end(), archiveEntries.begin(), archiveEntries.end());
		} else if (inputKind == InputPathKind::ArchiveMember &&
			IsSupportedImagePath(input)) {
			entries_.push_back(DescribeFile(input, shouldContinue));
		} else if (inputKind == InputPathKind::Directory) {
			const std::vector<Entry> directoryEntries = ScanDirectory(input, shouldContinue);
			entries_.insert(entries_.end(), directoryEntries.begin(), directoryEntries.end());
		} else if (inputKind == InputPathKind::RegularFile && IsSupportedImagePath(input)) {
			entries_.push_back(DescribeFile(input, shouldContinue));
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

FileList::ScanRequest FileList::InitialScanRequest(const std::vector<std::string>& inputs,
	SortMode sortMode, bool sortAscending, bool wrapAroundFolder, NavigationMode navigationMode) {
	ScanRequest request;
	request.operation = ScanOperation::Initialize;
	request.inputs = std::make_shared<const std::vector<std::string>>(inputs);
	request.sortMode = sortMode;
	request.sortAscending = sortAscending;
	request.wrapAroundFolder = wrapAroundFolder;
	request.navigationMode = navigationMode;
	return request;
}

FileList::ScanRequest FileList::MakeScanRequest(ScanOperation operation, int direction) const {
	ScanRequest request;
	request.operation = operation;
	request.inputs = inputs_;
	request.currentDirectory = currentDirectory_;
	request.rootDirectory = rootDirectory_;
	request.selectedPath = Current();
	request.sortMode = sortMode_;
	request.navigationMode = navigationMode_;
	request.expectedRevision = mutationRevision_;
	request.expectedDescriptorRevision = descriptorRevision_;
	request.sortAscending = sortAscending_;
	request.wrapAroundFolder = wrapAroundFolder_;
	request.multipleInputMode = multipleInputMode_;
	request.direction = direction;
	return request;
}

FileListPreparedScan FileList::PrepareScan(const ScanRequest& request,
	const std::function<bool()>& shouldContinue) {
	FileListPreparedScan result;
	result.operation = request.operation;
	result.expectedRevision = request.expectedRevision;
	result.expectedDescriptorRevision = request.expectedDescriptorRevision;
	result.sourceSelectedPath = request.selectedPath;
	FileList& replacement = result.replacement;
	replacement.inputs_ = request.inputs ? request.inputs :
		std::make_shared<const std::vector<std::string>>();
	replacement.sortMode_ = request.sortMode;
	replacement.sortAscending_ = request.sortAscending;
	replacement.wrapAroundFolder_ = request.wrapAroundFolder;
	replacement.navigationMode_ = request.navigationMode;
	replacement.mutationRevision_ = request.expectedRevision;

	const auto setDirectoryEntries = [&replacement](const fs::path& directory,
		std::vector<Entry> entries, const fs::path& selected) {
		replacement.currentDirectory_ = Normalize(directory);
		replacement.rootDirectory_ = replacement.currentDirectory_;
		replacement.entries_ = std::move(entries);
		replacement.currentIndex_ = 0;
		replacement.SortEntries();
		replacement.currentIndex_ = selected.empty() ? 0 : replacement.FindEntry(selected);
		replacement.RebuildPaths();
	};

	if (!ContinueScan(shouldContinue)) return result;
	switch (request.operation) {
	case ScanOperation::Initialize: {
		replacement.Initialize(request.inputs ? *request.inputs : std::vector<std::string>(),
			shouldContinue);
		if (!ContinueScan(shouldContinue)) return result;
		replacement.navigationMode_ = request.navigationMode;
		replacement.wrapAroundFolder_ = request.wrapAroundFolder;
		result.completed = true;
		result.targetFound = true;
		return result;
	}
	case ScanOperation::Reload: {
		if (request.multipleInputMode) {
			replacement.Initialize(request.inputs ? *request.inputs : std::vector<std::string>(),
				shouldContinue);
			if (!ContinueScan(shouldContinue)) return result;
			replacement.navigationMode_ = request.navigationMode;
			result.targetFound = true;
		} else {
			const fs::path directory = request.currentDirectory;
			std::vector<Entry> entries = ScanDirectory(directory, shouldContinue);
			if (!ContinueScan(shouldContinue)) return result;
			setDirectoryEntries(directory, std::move(entries), request.selectedPath);
			result.targetFound = true;
		}
		result.completed = true;
		return result;
	}
	case ScanOperation::PrepareDirectoryScope: {
		const fs::path selected = request.selectedPath;
		const fs::path directory = selected.parent_path();
		if (directory.empty()) return result;
		std::vector<Entry> entries = ScanDirectory(directory, shouldContinue);
		if (!ContinueScan(shouldContinue)) return result;
		setDirectoryEntries(directory, std::move(entries), selected);
		result.completed = true;
		result.targetFound = true;
		return result;
	}
	case ScanOperation::MarkedToggleTarget: {
		const fs::path target = request.selectedPath;
		if (target.empty() || !IsSupportedImagePath(target)) return result;
		const fs::path directory = target.parent_path();
		std::vector<Entry> entries = ScanDirectory(directory, shouldContinue);
		if (!ContinueScan(shouldContinue)) return result;
		setDirectoryEntries(directory, std::move(entries), target);
		result.completed = true;
		result.targetFound = !replacement.entries_.empty() &&
			replacement.entries_[replacement.currentIndex_].path == Normalize(target);
		return result;
	}
	case ScanOperation::PreviousSibling:
	case ScanOperation::NextSibling: {
		if (request.direction != -1 && request.direction != 1) return result;
		const fs::path parent = request.currentDirectory.parent_path();
		if (parent.empty() || parent == request.currentDirectory) {
			result.completed = true;
			return result;
		}
		const std::vector<fs::path> siblings = ChildDirectories(parent, shouldContinue);
		if (!ContinueScan(shouldContinue)) return result;
		const auto current = std::find(siblings.begin(), siblings.end(), request.currentDirectory);
		if (current == siblings.end()) {
			result.completed = true;
			return result;
		}
		const std::ptrdiff_t currentIndex = std::distance(siblings.begin(), current);
		for (std::ptrdiff_t index = currentIndex + request.direction;
			index >= 0 && index < static_cast<std::ptrdiff_t>(siblings.size()); index += request.direction) {
			if (!ContinueScan(shouldContinue)) return result;
			const fs::path& candidate = siblings[static_cast<std::size_t>(index)];
			std::vector<Entry> entries = ScanDirectory(candidate, shouldContinue);
			if (!ContinueScan(shouldContinue)) return result;
			if (entries.empty()) continue;
			setDirectoryEntries(candidate, std::move(entries), {});
			result.targetFound = true;
			break;
		}
		result.completed = true;
		return result;
	}
	case ScanOperation::ForwardBoundary: {
		std::vector<fs::path> directories;
		if (request.navigationMode == NavigationMode::LoopSameDirectoryLevel) {
			const fs::path parent = request.currentDirectory.parent_path();
			const std::vector<fs::path> siblings = ChildDirectories(parent, shouldContinue);
			if (!ContinueScan(shouldContinue)) return result;
			const auto current = std::find(siblings.begin(), siblings.end(), request.currentDirectory);
			if (current != siblings.end()) directories.assign(current + 1, siblings.end());
		} else if (request.navigationMode == NavigationMode::LoopSubDirectories) {
			CollectDescendantDirectories(request.rootDirectory, directories, shouldContinue);
			if (!ContinueScan(shouldContinue)) return result;
			std::size_t start = 0;
			if (request.currentDirectory != request.rootDirectory) {
				const auto current = std::find(directories.begin(), directories.end(), request.currentDirectory);
				if (current == directories.end()) {
					result.completed = true;
					return result;
				}
				start = static_cast<std::size_t>(std::distance(directories.begin(), current)) + 1;
			}
			directories.erase(directories.begin(), directories.begin() + static_cast<std::ptrdiff_t>(start));
		} else {
			result.completed = true;
			return result;
		}
		for (const fs::path& directory : directories) {
			if (!ContinueScan(shouldContinue)) return result;
			std::vector<Entry> entries = ScanDirectory(directory, shouldContinue);
			if (!ContinueScan(shouldContinue)) return result;
			if (entries.empty()) continue;
			setDirectoryEntries(directory, std::move(entries), {});
			if (request.operation == ScanOperation::ForwardBoundary) {
				replacement.rootDirectory_ = request.rootDirectory;
			}
			result.targetFound = true;
			break;
		}
		result.completed = true;
		return result;
	}
	}
	return result;
}

bool FileList::ApplyPreparedScan(FileListPreparedScan&& scan,
	const fs::path& preferredPathAtApply) {
	if (!scan.completed || scan.expectedRevision != mutationRevision_ ||
		scan.expectedDescriptorRevision != descriptorRevision_) return false;
	const ScanOperation operation = scan.operation;
	if (operation == ScanOperation::PrepareDirectoryScope &&
		!preferredPathAtApply.empty() &&
		preferredPathAtApply.parent_path() != scan.replacement.currentDirectory_) return false;
	if (operation != ScanOperation::Initialize && operation != ScanOperation::Reload &&
		!scan.targetFound) return true;
	FileList& replacement = scan.replacement;
	const bool keepPreparedSelection = operation == ScanOperation::ForwardBoundary ||
		operation == ScanOperation::PreviousSibling || operation == ScanOperation::NextSibling ||
		operation == ScanOperation::MarkedToggleTarget;
	const fs::path selected = keepPreparedSelection ? replacement.Current() :
		(!preferredPathAtApply.empty() ? preferredPathAtApply :
		(!replacement.Current().empty() ? replacement.Current() : replacement.emptyPath_));

	if (operation == ScanOperation::Initialize) {
		// A settings edit can happen while startup scanning is in flight. Keep
		// the live navigation preference instead of restoring the request snapshot.
		replacement.wrapAroundFolder_ = wrapAroundFolder_;
		if (!preferredPathAtApply.empty() && !replacement.entries_.empty()) {
			const std::size_t selectedIndex = replacement.FindEntry(preferredPathAtApply);
			if (replacement.entries_[selectedIndex].path == Normalize(preferredPathAtApply)) {
				replacement.currentIndex_ = selectedIndex;
			}
		}
		replacement.mutationRevision_ = mutationRevision_ + 1;
		*this = std::move(replacement);
		return true;
	}

	const auto moveActiveEntries = [this, &replacement, &selected] {
		entries_ = std::move(replacement.entries_);
		paths_ = std::move(replacement.paths_);
		pathIndices_ = std::move(replacement.pathIndices_);
		currentDirectory_ = std::move(replacement.currentDirectory_);
		currentIndex_ = selected.empty() ? replacement.currentIndex_ : FindEntry(selected);
		if (currentIndex_ >= entries_.size()) currentIndex_ = entries_.empty() ? 0 : entries_.size() - 1;
		UpdateMarkedIndex();
	};

	if (operation == ScanOperation::Reload && multipleInputMode_) {
		const fs::path markedFile = markedFile_;
		const fs::path markedCurrent = markedFileCurrent_;
		const int markedToggle = markedToggleIndex_;
		replacement.navigationMode_ = navigationMode_;
		replacement.wrapAroundFolder_ = wrapAroundFolder_;
		replacement.markedFile_ = markedFile;
		replacement.markedFileCurrent_ = markedCurrent;
		replacement.markedToggleIndex_ = markedToggle;
		replacement.mutationRevision_ = mutationRevision_ + 1;
		*this = std::move(replacement);
		if (!selected.empty()) currentIndex_ = FindEntry(selected);
		if (currentIndex_ >= entries_.size()) currentIndex_ = entries_.empty() ? 0 : entries_.size() - 1;
		UpdateMarkedIndex();
		return true;
	}

	std::optional<FolderState> previousFolder;
	if (operation == ScanOperation::ForwardBoundary) {
		previousFolder = FolderState{currentDirectory_, std::move(entries_),
			std::move(paths_), std::move(pathIndices_), currentIndex_};
	}
	moveActiveEntries();
	if (operation == ScanOperation::Reload) {
		nextFolders_.clear();
	} else if (operation == ScanOperation::PrepareDirectoryScope) {
		rootDirectory_ = currentDirectory_;
		previousFolders_.clear();
		nextFolders_.clear();
		multipleInputMode_ = false;
	} else if (operation == ScanOperation::ForwardBoundary) {
		previousFolders_.push_back(std::move(*previousFolder));
		nextFolders_.clear();
		multipleInputMode_ = false;
	} else if (operation == ScanOperation::PreviousSibling ||
		operation == ScanOperation::NextSibling) {
		rootDirectory_ = currentDirectory_.parent_path();
		previousFolders_.clear();
		nextFolders_.clear();
		multipleInputMode_ = false;
	} else if (operation == ScanOperation::MarkedToggleTarget) {
		rootDirectory_ = currentDirectory_;
		previousFolders_.clear();
		nextFolders_.clear();
		multipleInputMode_ = false;
	}

	++mutationRevision_;
	return true;
}

bool FileList::EntryLess(const Entry& left, const Entry& right,
	SortMode sortMode, bool sortAscending) {
	int comparison = 0;
	switch (sortMode) {
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
	return sortAscending ? comparison < 0 : comparison > 0;
}

void FileList::SortEntries() {
	const fs::path selected = currentIndex_ < entries_.size() ? entries_[currentIndex_].path : emptyPath_;
	std::stable_sort(entries_.begin(), entries_.end(), [this](const Entry& left, const Entry& right) {
		return EntryLess(left, right, sortMode_, sortAscending_);
	});
	RebuildPathIndex();
	if (!selected.empty()) {
		currentIndex_ = FindEntry(selected);
	} else if (currentIndex_ >= entries_.size()) {
		currentIndex_ = entries_.empty() ? 0 : entries_.size() - 1;
	}
}

void FileList::RebuildPathIndex() {
	pathIndices_.resize(entries_.size());
	std::iota(pathIndices_.begin(), pathIndices_.end(), std::size_t{0});
	std::sort(pathIndices_.begin(), pathIndices_.end(), [this](std::size_t left, std::size_t right) {
		return entries_[left].path.native() < entries_[right].path.native();
	});
}

void FileList::SetProvisionalInputs(const std::vector<std::string>& inputs) {
	inputs_ = std::make_shared<const std::vector<std::string>>(inputs);
	entries_.clear();
	paths_.clear();
	pathIndices_.clear();
	previousFolders_.clear();
	nextFolders_.clear();
	markedIndex_.reset();
	browseLocationOnEmpty_.clear();
	currentIndex_ = 0;
	multipleInputMode_ = inputs.size() > 1;
	currentDirectory_.clear();
	rootDirectory_.clear();

	if (inputs.size() != 1) {
		++mutationRevision_;
		return;
	}
	for (const std::string& inputString : inputs) {
		const fs::path input = Normalize(fs::path(inputString));
		if (IsSupportedImagePath(input)) {
			Entry provisional;
			provisional.path = input;
			SourceIdentity identity;
			(void)CaptureImageSourceIdentity(input, identity);
			provisional.source = SourceDescriptor(input, identity, {});
			provisional.randomOrder = std::hash<std::string>{}(input.string());
			provisional.archiveMember = IsArchiveMemberLocation(input);
			entries_.push_back(std::move(provisional));
			currentDirectory_ = input.parent_path();
			rootDirectory_ = currentDirectory_;
			break;
		}
		if (inputs.size() == 1) {
			currentDirectory_ = input;
			rootDirectory_ = input;
			break;
		}
	}
	SortEntries();
	RebuildPaths();
	++mutationRevision_;
}

void FileList::RebuildPaths() {
	paths_.clear();
	paths_.reserve(entries_.size());
	for (const Entry& entry : entries_) paths_.push_back(entry.path);
	if (currentIndex_ >= paths_.size()) currentIndex_ = paths_.empty() ? 0 : paths_.size() - 1;
	UpdateMarkedIndex();
}

bool FileList::RefreshSourceDescriptor(const fs::path& path) {
	if (entries_.empty()) return false;
	const fs::path normalized = Normalize(path);
	return RefreshSourceDescriptor(DescribeImageSource(normalized));
}

bool FileList::RefreshSourceDescriptor(const SourceDescriptor& refreshed) {
	if (entries_.empty() || refreshed.LogicalPath().empty()) return false;
	const fs::path normalized = Normalize(refreshed.LogicalPath());
	const std::size_t index = FindEntry(normalized);
	if (entries_[index].path != normalized) return false;
	Entry& entry = entries_[index];
	if (entry.source.Key() == refreshed.Key() &&
		SameSourceMetadata(entry.source.Metadata(), refreshed.Metadata())) return false;

	const SourceMetadata metadata = refreshed.Metadata();
	const bool sortKeyChanged =
		(sortMode_ == SortMode::LastModificationTime && entry.lastModificationTime !=
			(metadata.hasModificationTime ? metadata.modificationTimeNanoseconds : 0)) ||
		(sortMode_ == SortMode::CreationTime && entry.creationTime !=
			(metadata.hasCreationTime ? metadata.creationTimeNanoseconds : 0)) ||
		(sortMode_ == SortMode::FileSize && entry.fileSize !=
			(metadata.hasFileSize ? metadata.fileSize : 0));
	const fs::path selected = Current();
	entry.source = refreshed;
	entry.archiveMember = metadata.archiveMember;
	entry.lastModificationTime = metadata.hasModificationTime ?
		metadata.modificationTimeNanoseconds : 0;
	entry.creationTime = metadata.hasCreationTime ?
		metadata.creationTimeNanoseconds : entry.lastModificationTime;
	entry.fileSize = metadata.hasFileSize ? metadata.fileSize : 0;
	++descriptorRevision_;
	if (sortKeyChanged) {
		const std::vector<fs::path> previousOrder = paths_;
		SortEntries();
		RebuildPaths();
		if (paths_ != previousOrder) ++mutationRevision_;
	}
	return true;
}

bool FileList::RefreshSourceDescriptor(const SourceKey& expected,
	const SourceDescriptor& observed) {
	if (expected.logicalPath.empty() || expected.logicalPath != observed.Key().logicalPath) return false;
	const fs::path normalized = Normalize(observed.LogicalPath());
	if (entries_.empty()) return false;
	const std::size_t index = FindEntry(normalized);
	if (entries_[index].path != normalized || entries_[index].source.Key() != expected) return false;
	return RefreshSourceDescriptor(observed);
}

SourceRefreshOutcome RefreshFileListSource(FileList& files,
	const SourceKey& expected, const SourceDescriptor& observed) {
	SourceRefreshOutcome outcome;
	if (files.Empty()) return outcome;
	const std::filesystem::path selectedPath = files.Current();
	const std::size_t selectedIndexBefore = files.CurrentIndex();
	outcome.previousIndex = files.IndexOf(observed.LogicalPath());
	const std::uint64_t mutationRevision = files.MutationRevision();
	if (!files.RefreshSourceDescriptor(expected, observed)) return outcome;
	outcome.applied = true;
	outcome.orderChanged = files.MutationRevision() != mutationRevision;
	outcome.currentIndex = files.IndexOf(observed.LogicalPath());
	outcome.selectedSourceChanged = outcome.previousIndex.has_value() &&
		*outcome.previousIndex == selectedIndexBefore && !files.Empty() &&
		files.Current() == selectedPath;
	return outcome;
}

SourceRefreshDisplayAction ResolveSourceRefreshDisplayAction(
	const SourceRefreshOutcome& refresh, bool preserveCurrentPixels) {
	if (!refresh.applied || !refresh.selectedSourceChanged) {
		return SourceRefreshDisplayAction::NoCurrentChange;
	}
	return preserveCurrentPixels ? SourceRefreshDisplayAction::PreserveCurrentPixels :
		SourceRefreshDisplayAction::ReloadCurrent;
}

void FileList::UpdateMarkedIndex() {
	markedIndex_.reset();
	if (!markedFile_.empty() && !entries_.empty()) {
		const std::size_t marked = FindEntry(markedFile_);
		if (entries_[marked].path == Normalize(markedFile_)) markedIndex_ = marked;
	}
}

std::size_t FileList::FindEntry(const fs::path& path) const {
	const fs::path normalized = Normalize(path);
	const auto found = std::lower_bound(pathIndices_.begin(), pathIndices_.end(), normalized,
		[this](std::size_t index, const fs::path& target) {
			return entries_[index].path.native() < target.native();
		});
	if (found != pathIndices_.end() && entries_[*found].path == normalized) {
		return *found;
	}
	return entries_.empty() ? 0 : entries_.size() - 1;
}

bool FileList::SelectPath(const fs::path& path) {
	const fs::path normalized = Normalize(path);
	if (!IsSupportedImagePath(normalized)) return false;
	if (IsArchiveMemberLocation(normalized)) {
		ArchiveMemberInfo info;
		std::string errorMessage;
		if (!GetArchiveMemberInfo(normalized, info, errorMessage)) return false;
	} else {
		std::error_code error;
		if (!fs::is_regular_file(normalized, error) || error) return false;
	}

	const std::size_t existing = FindEntry(normalized);
	if (!entries_.empty() && entries_[existing].path == normalized) {
		currentIndex_ = existing;
		return true;
	}

	const fs::path directory = normalized.parent_path();
	std::vector<Entry> replacement = ScanDirectory(directory);
	const auto selected = std::find_if(replacement.begin(), replacement.end(), [&normalized](const Entry& entry) {
		return entry.path == normalized;
	});
	if (selected == replacement.end()) return false;
	const std::size_t selectedIndex = static_cast<std::size_t>(
		std::distance(replacement.begin(), selected));

	entries_ = std::move(replacement);
	currentDirectory_ = directory;
	rootDirectory_ = directory;
	currentIndex_ = selectedIndex;
	previousFolders_.clear();
	nextFolders_.clear();
	multipleInputMode_ = false;
	SortEntries();
	currentIndex_ = FindEntry(normalized);
	RebuildPaths();
	++mutationRevision_;
	return !Empty() && Current() == normalized;
}

void FileList::LoadDirectory(const fs::path& directory, const fs::path& selected) {
	currentDirectory_ = Normalize(directory);
	entries_ = ScanDirectory(currentDirectory_);
	currentIndex_ = 0;
	SortEntries();
	if (!selected.empty()) currentIndex_ = FindEntry(selected);
	RebuildPaths();
}

bool FileList::EnterDirectory(const fs::path& directory, bool rememberCurrentFolder) {
	std::vector<Entry> nextEntries = ScanDirectory(directory);
	if (nextEntries.empty()) return false;
	if (rememberCurrentFolder) {
		previousFolders_.push_back(FolderState{currentDirectory_, std::move(entries_),
			std::move(paths_), std::move(pathIndices_), currentIndex_});
	} else {
		previousFolders_.clear();
	}
	entries_ = std::move(nextEntries);
	currentDirectory_ = Normalize(directory);
	currentIndex_ = 0;
	SortEntries();
	currentIndex_ = 0;
	nextFolders_.clear();
	multipleInputMode_ = false;
	RebuildPaths();
	++mutationRevision_;
	return true;
}

bool FileList::NavigateSiblingDirectory(int direction) {
	if (direction != -1 && direction != 1) return false;
	if (multipleInputMode_) PrepareDirectoryNavigation();
	if (entries_.empty() || currentDirectory_.empty()) return false;
	const fs::path parent = currentDirectory_.parent_path();
	if (parent.empty() || parent == currentDirectory_) return false;
	const std::vector<fs::path> siblings = ChildDirectories(parent);
	const auto current = std::find(siblings.begin(), siblings.end(), currentDirectory_);
	if (current == siblings.end()) return false;
	const std::ptrdiff_t currentIndex = std::distance(siblings.begin(), current);
	for (std::ptrdiff_t index = currentIndex + direction;
		index >= 0 && index < static_cast<std::ptrdiff_t>(siblings.size()); index += direction) {
		const fs::path& candidate = siblings[static_cast<std::size_t>(index)];
		if (ScanDirectory(candidate).empty()) continue;
		if (!EnterDirectory(candidate, false)) continue;
		rootDirectory_ = parent;
		return true;
	}
	return false;
}

bool FileList::RestorePreviousFolder() {
	if (previousFolders_.empty()) return false;
	nextFolders_.push_back(FolderState{currentDirectory_, std::move(entries_),
		std::move(paths_), std::move(pathIndices_), currentIndex_});
	FolderState state = std::move(previousFolders_.back());
	previousFolders_.pop_back();
	currentDirectory_ = std::move(state.directory);
	entries_ = std::move(state.entries);
	paths_ = std::move(state.paths);
	pathIndices_ = std::move(state.pathIndices);
	currentIndex_ = state.index;
	multipleInputMode_ = false;
	UpdateMarkedIndex();
	++mutationRevision_;
	return true;
}

bool FileList::RestoreNextFolder() {
	if (nextFolders_.empty()) return false;
	previousFolders_.push_back(FolderState{currentDirectory_, std::move(entries_),
		std::move(paths_), std::move(pathIndices_), currentIndex_});
	FolderState state = std::move(nextFolders_.back());
	nextFolders_.pop_back();
	currentDirectory_ = std::move(state.directory);
	entries_ = std::move(state.entries);
	paths_ = std::move(state.paths);
	pathIndices_ = std::move(state.pathIndices);
	currentIndex_ = state.index;
	multipleInputMode_ = false;
	UpdateMarkedIndex();
	++mutationRevision_;
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
	++mutationRevision_;
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

bool FileList::Select(std::size_t index) {
	if (index >= entries_.size()) return false;
	currentIndex_ = index;
	return true;
}

FileList::LoadedNavigationResult FileList::NextLoaded() {
	if (entries_.empty()) return LoadedNavigationResult::NoMove;
	if (currentIndex_ + 1 < entries_.size()) {
		++currentIndex_;
		return LoadedNavigationResult::Moved;
	}
	if (RestoreNextFolder()) return LoadedNavigationResult::Moved;
	if (navigationMode_ == NavigationMode::LoopDirectory || multipleInputMode_) {
		if (!wrapAroundFolder_) return LoadedNavigationResult::NoMove;
		currentIndex_ = 0;
		return LoadedNavigationResult::Moved;
	}
	return LoadedNavigationResult::NeedsDirectoryScan;
}

FileList::LoadedNavigationResult FileList::PreviousLoaded() {
	if (entries_.empty()) return LoadedNavigationResult::NoMove;
	if (currentIndex_ > 0) {
		--currentIndex_;
		return LoadedNavigationResult::Moved;
	}
	if (RestorePreviousFolder()) return LoadedNavigationResult::Moved;
	if (navigationMode_ == NavigationMode::LoopDirectory || multipleInputMode_) {
		if (!wrapAroundFolder_) return LoadedNavigationResult::NoMove;
		currentIndex_ = entries_.size() - 1;
		return LoadedNavigationResult::Moved;
	}
	return LoadedNavigationResult::NoMove;
}

bool FileList::MarkCurrentForToggle() {
	if (Empty() || Current().empty()) return false;
	if (IsArchiveMemberLocation(Current())) {
		ArchiveMemberInfo info;
		std::string errorMessage;
		if (!GetArchiveMemberInfo(Current(), info, errorMessage)) return false;
	} else {
		std::error_code error;
		if (!fs::is_regular_file(Current(), error) || error) return false;
	}
	markedFile_ = Current();
	markedFileCurrent_.clear();
	markedIndex_ = currentIndex_;
	markedToggleIndex_ = -1;
	return true;
}

bool FileList::ToggleBetweenMarkedAndCurrent() {
	const fs::path target = MarkedToggleTarget();
	if (target.empty()) return false;
	const fs::path current = Current();
	if (!SelectPath(target)) return false;
	return CompleteMarkedToggle(current);
}

fs::path FileList::MarkedToggleTarget() const {
	if (markedFile_.empty() || Empty()) return {};
	const int targetIndex = markedToggleIndex_ < 0 ? 0 : markedToggleIndex_;
	if (targetIndex != 0 && markedFileCurrent_.empty()) return {};
	return targetIndex == 0 ? markedFile_ : markedFileCurrent_;
}

bool FileList::ContainsPath(const fs::path& path) const {
	return IndexOf(path).has_value();
}

std::optional<std::size_t> FileList::IndexOf(const fs::path& path) const {
	if (Empty() || path.empty()) return std::nullopt;
	const fs::path normalized = Normalize(path);
	const auto found = std::lower_bound(pathIndices_.begin(), pathIndices_.end(), normalized,
		[this](std::size_t index, const fs::path& target) {
			return entries_[index].path.native() < target.native();
		});
	if (found == pathIndices_.end() || entries_[*found].path != normalized) return std::nullopt;
	return *found;
}

bool FileList::CompleteMarkedToggle(const fs::path& previousPath) {
	const fs::path target = MarkedToggleTarget();
	if (target.empty() || Current() != Normalize(target) || previousPath.empty()) return false;
	const int targetIndex = markedToggleIndex_ < 0 ? 0 : markedToggleIndex_;
	if (targetIndex == 0) markedFileCurrent_ = Normalize(previousPath);
	markedToggleIndex_ = (targetIndex + 1) & 1;
	return true;
}

std::optional<std::size_t> FileList::MarkedIndex() const {
	return markedIndex_;
}

void FileList::First() {
	if (!entries_.empty()) currentIndex_ = 0;
}

void FileList::Last() {
	if (!entries_.empty()) currentIndex_ = entries_.size() - 1;
}

bool FileList::Reload() {
	return Reload(Current());
}

bool FileList::Reload(const fs::path& preferredPath) {
	const fs::path selected = preferredPath;
	if (selected.empty()) return false;
	if (multipleInputMode_) {
		Initialize(inputs_ ? *inputs_ : std::vector<std::string>());
		if (Current().empty()) return false;
		currentIndex_ = FindEntry(selected);
		RebuildPaths();
		++mutationRevision_;
		return true;
	}
	LoadDirectory(currentDirectory_, selected);
	nextFolders_.clear();
	++mutationRevision_;
	return !entries_.empty();
}

void FileList::SetSorting(SortMode sortMode, bool sortAscending) {
	const fs::path selected = Current();
	sortMode_ = sortMode;
	sortAscending_ = sortAscending;
	SortEntries();
	if (!selected.empty()) currentIndex_ = FindEntry(selected);
	RebuildPaths();
	++mutationRevision_;
}

bool FileList::SetNavigationMode(NavigationMode navigationMode) {
	if (navigationMode_ == navigationMode) return false;
	navigationMode_ = navigationMode;
	previousFolders_.clear();
	nextFolders_.clear();
	++mutationRevision_;
	if (multipleInputMode_ && navigationMode_ != NavigationMode::LoopDirectory) return true;
	if (navigationMode_ != NavigationMode::LoopDirectory) {
		if (!currentDirectory_.empty()) rootDirectory_ = currentDirectory_;
	}
	return false;
}

bool FileList::PreviousSiblingDirectory() {
	return NavigateSiblingDirectory(-1);
}

bool FileList::NextSiblingDirectory() {
	return NavigateSiblingDirectory(1);
}

} // namespace jpegview_linux
