#include "file_dialog_model.h"
#include "event_loop_model.h"
#include "archive_source.h"
#include "image_formats.h"
#include "image_decoder.h"
#include "perf_diagnostics.h"
#include "source_work_coordinator.h"
#include "thumbnail_resampler.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace jpegview_linux {
namespace {

constexpr std::size_t kDirectoryEnumerationBatchSize = 32;
constexpr std::size_t kFileDialogSortPreparationBatchSize = 128;
constexpr std::size_t kFileDialogSortCancellationBatchSize = 256;

struct FileDialogSortCancelled {};

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

bool AcquireDirectoryEnumerationLease(const std::filesystem::path& directory,
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

std::filesystem::path NormalizeDialogPath(const std::filesystem::path& path) {
	std::error_code error;
	const std::filesystem::path absolute = std::filesystem::absolute(path, error);
	return (error ? path : absolute).lexically_normal();
}

FileDialogDirectoryResult EnumerateFileDialogDirectory(
	const std::filesystem::path& directory, FileDialogListingPolicy policy,
	const std::function<bool()>& shouldContinue, bool& interruptedForForeground) {
	FileDialogDirectoryResult result;
	result.directory = directory;
	const std::filesystem::path parent = directory.parent_path();
	if (!parent.empty() && parent != directory) {
		result.entries.emplace_back(parent, true, true);
	}

	bool archiveLocation = false;
	WorkContext classificationContext;
	SourceWorkLease classificationLease;
	if (!AcquireDirectoryEnumerationLease(directory, shouldContinue,
		interruptedForForeground, classificationContext, classificationLease)) {
		return result;
	}
	{
		ScopedWorkContext activeContext(classificationContext);
		if (classificationContext.Continue()) archiveLocation = IsArchiveLocation(directory);
		ReleaseDirectoryEnumerationLease(classificationContext, classificationLease);
	}
	if (interruptedForForeground || !shouldContinue()) return result;
	result.archiveLocation = archiveLocation;

	if (archiveLocation) {
		std::vector<ArchiveEntryInfo> archiveEntries;
		WorkContext archiveContext = MakePathWorkContext(directory,
			SourceWorkPriority::Metadata, shouldContinue);
		archiveContext.onForegroundYield = [&interruptedForForeground] {
			interruptedForForeground = true;
		};
		ScopedWorkContext activeContext(archiveContext);
		if (!ListArchiveDirectoryCancellable(directory, archiveEntries, shouldContinue,
			result.error, &result.errorKind, &result.containsEncryptedEntries)) {
			return result;
		}
		for (const ArchiveEntryInfo& archiveEntry : archiveEntries) {
			if (!shouldContinue()) break;
			if (!archiveEntry.directory && !IsSupportedImagePath(archiveEntry.path)) continue;
			const std::filesystem::file_time_type modified = ArchiveFileModificationTime(
				archiveEntry.modificationTime);
			FileDialogEntry entry{archiveEntry.path, archiveEntry.directory, false, modified,
				false, true, archiveEntry.encrypted,
				archiveEntry.directory ? 0 : archiveEntry.size, !archiveEntry.directory};
			if (!archiveEntry.directory) {
				entry.sourceDescriptor = DescribeArchiveMember(archiveEntry.path,
					archiveEntry.backingIdentity, archiveEntry.size,
					archiveEntry.modificationTime, archiveEntry.encrypted);
			}
			result.entries.push_back(std::move(entry));
		}
		if (!shouldContinue()) return result;
		return result;
	}

	std::error_code iteratorError;
	std::filesystem::directory_iterator iterator;
	const std::filesystem::directory_iterator end;
	bool iteratorStarted = false;
	while (!iteratorStarted || (!iteratorError && iterator != end)) {
		WorkContext context;
		SourceWorkLease sourceLease;
		if (!AcquireDirectoryEnumerationLease(directory, shouldContinue,
			interruptedForForeground, context, sourceLease)) break;
		{
			ScopedWorkContext activeContext(context);
			if (!iteratorStarted) {
				iterator = std::filesystem::directory_iterator(directory, iteratorError);
				iteratorStarted = true;
			}
			std::size_t batchEntries = 0;
			while (!iteratorError && iterator != end &&
				batchEntries < kDirectoryEnumerationBatchSize) {
				if (!context.Continue()) break;
				const std::filesystem::path entryPath = iterator->path();
				std::error_code statusError;
				const bool isDirectory = iterator->is_directory(statusError);
				const bool regularFile = !statusError && !isDirectory &&
					iterator->is_regular_file(statusError) && !statusError;
				const bool archiveCandidate = regularFile &&
					IsArchiveContainerName(entryPath);
				const bool archive = archiveCandidate && policy.includeArchives &&
					!policy.saveDialog && !policy.includeNonImageFiles;
				const bool showFile = regularFile && (archive || (!archiveCandidate &&
					(policy.includeNonImageFiles || IsSupportedImagePath(entryPath))));
				if (!statusError && (isDirectory || showFile)) {
					std::error_code modificationError;
					const std::filesystem::file_time_type modificationTime =
						iterator->last_write_time(modificationError);
					const std::filesystem::path normalizedPath = NormalizeDialogPath(entryPath);
					FileDialogEntry entry{normalizedPath, isDirectory || archive, false,
						modificationError ? std::filesystem::file_time_type{} : modificationTime,
						archive, false, archive &&
							policy.encryptedArchivePaths.count(normalizedPath.string()) != 0};
					if (regularFile && !archive && !policy.includeNonImageFiles &&
						IsSupportedImagePath(entryPath)) {
						entry.sourceDescriptor = SourceDescriptor(normalizedPath, {}, {});
						std::error_code sizeError;
						entry.fileSize = iterator->file_size(sizeError);
						entry.fileSizeKnown = !sizeError;
					}
					result.entries.push_back(std::move(entry));
				}
				iterator.increment(iteratorError);
				++batchEntries;
			}
			ReleaseDirectoryEnumerationLease(context, sourceLease);
		}
		if (interruptedForForeground || !shouldContinue()) break;
	}
	if (!shouldContinue()) return result;
	if (iteratorError) {
		result.error = iteratorError.message();
		return result;
	}
	if (interruptedForForeground) return result;
	if (!shouldContinue()) return result;
	return result;
}

bool CapturePreviewSource(const SourceDescriptor& requested,
	const std::filesystem::path& path,
	const FileDialogPreviewLoader::SourceCapture& sourceCapture,
	const std::function<bool()>& shouldContinue, SourceDescriptor& observed) {
	WorkContext context = requested.Valid() ?
		MakeWorkContext(requested, SourceWorkPriority::Metadata, shouldContinue) :
		MakePathWorkContext(path, SourceWorkPriority::Metadata, shouldContinue);
	context.sourceAccessAlreadyAdmitted = false;
	context.cpuProcessingAlreadyAdmitted = false;
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
		context, path);
	if (!admission || !context.Continue()) return false;
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	{
		ScopedWorkContext activeContext(context);
		observed = sourceCapture(requested.Valid() ? requested : SourceDescriptor(path, {}, {}));
	}
	context.sourceAccessAlreadyAdmitted = false;
	context.cpuProcessingAlreadyAdmitted = false;
	admission.Reset();
	return context.Continue();
}

bool ValidateAndRefreshPreviewSource(const SourceDescriptor& requested,
	const FileDialogPreviewLoader::SourceCapture& sourceCapture,
	const std::function<bool()>& shouldContinue, SourceDescriptor& observed) {
	if (!requested.Valid()) return false;
	WorkContext context = MakeWorkContext(requested,
		SourceWorkPriority::Metadata, shouldContinue);
	context.sourceAccessAlreadyAdmitted = false;
	context.cpuProcessingAlreadyAdmitted = false;
	SourceCpuWorkLease admission = SourceWorkCoordinator::Global().AcquireSourceAndCpu(
		context, requested.LogicalPath());
	if (!admission || !context.Continue()) return false;
	context.sourcePriority = context.Priority();
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	bool current = false;
	{
		ScopedWorkContext activeContext(context);
		current = IsImageSourceCurrent(requested);
	}
	context.sourceAccessAlreadyAdmitted = false;
	context.cpuProcessingAlreadyAdmitted = false;
	admission.Reset();
	if (!current && context.Continue()) {
		(void)CapturePreviewSource(SourceDescriptor{}, requested.LogicalPath(),
			sourceCapture, shouldContinue, observed);
	}
	return current && context.Continue();
}

template<typename Continue>
DirectorySummary CountImmediateDirectoryContentsWhile(
	const std::filesystem::path& directory, Continue shouldContinue,
	bool* completed = nullptr, std::string* errorMessage = nullptr) {
	if (completed != nullptr) *completed = true;
	if (errorMessage != nullptr) errorMessage->clear();
	DirectorySummary summary;
	bool interruptedForForeground = false;
	bool archiveLocation = false;
	WorkContext classificationContext;
	SourceWorkLease classificationLease;
	if (!AcquireDirectoryEnumerationLease(directory,
		[&shouldContinue] { return shouldContinue(); }, interruptedForForeground,
		classificationContext, classificationLease)) {
		if (completed != nullptr) *completed = false;
		return summary;
	}
	{
		ScopedWorkContext activeContext(classificationContext);
		if (classificationContext.Continue()) {
			archiveLocation = IsArchiveLocation(directory);
		}
		ReleaseDirectoryEnumerationLease(classificationContext, classificationLease);
	}
	if (interruptedForForeground || !shouldContinue()) {
		if (completed != nullptr) *completed = false;
		return summary;
	}
	if (archiveLocation) {
		std::vector<ArchiveEntryInfo> entries;
		std::string localError;
		std::string& archiveError = errorMessage == nullptr ? localError : *errorMessage;
		if (!ListArchiveDirectoryCancellable(directory, entries,
			[&shouldContinue] { return shouldContinue(); },
			archiveError)) {
			if (completed != nullptr) *completed = false;
			return summary;
		}
		for (const ArchiveEntryInfo& entry : entries) {
			if (!shouldContinue()) {
				if (completed != nullptr) *completed = false;
				break;
			}
			if (entry.directory) ++summary.subdirectoryCount;
			else if (IsSupportedImagePath(entry.path)) ++summary.imageCount;
		}
		return summary;
	}
	std::error_code iteratorError;
	std::filesystem::directory_iterator iterator;
	const std::filesystem::directory_iterator end;
	bool iteratorStarted = false;
	while (!iteratorStarted || (!iteratorError && iterator != end)) {
		WorkContext context;
		SourceWorkLease sourceLease;
		if (!AcquireDirectoryEnumerationLease(directory,
			[&shouldContinue] { return shouldContinue(); }, interruptedForForeground,
			context, sourceLease)) {
			if (completed != nullptr) *completed = false;
			break;
		}
		{
			ScopedWorkContext activeContext(context);
			if (!iteratorStarted) {
				iterator = std::filesystem::directory_iterator(directory, iteratorError);
				iteratorStarted = true;
			}
			std::size_t batchEntries = 0;
			while (!iteratorError && iterator != end &&
				batchEntries < kDirectoryEnumerationBatchSize) {
				if (!context.Continue()) break;
				const std::filesystem::path entryPath = iterator->path();
				std::error_code statusError;
				if (iterator->is_directory(statusError)) {
					if (!statusError) ++summary.subdirectoryCount;
				} else if (!statusError && iterator->is_regular_file(statusError) &&
					!statusError) {
					if (IsArchiveContainerName(entryPath)) {
						++summary.subdirectoryCount;
					} else if (IsSupportedImagePath(entryPath)) {
						++summary.imageCount;
					}
				}
				iterator.increment(iteratorError);
				++batchEntries;
			}
			ReleaseDirectoryEnumerationLease(context, sourceLease);
		}
		if (interruptedForForeground || !shouldContinue()) {
			if (completed != nullptr) *completed = false;
			break;
		}
	}
	if (iteratorError && errorMessage != nullptr) *errorMessage = iteratorError.message();
	if ((iteratorError || interruptedForForeground) && completed != nullptr) {
		*completed = false;
	}
	return summary;
}

std::filesystem::path FirstImageInDirectoryWhile(
	const std::filesystem::path& directory, FileDialogSortMode mode,
	const std::function<bool()>& shouldContinue, std::string* errorMessage = nullptr,
	ArchiveErrorKind* errorKind = nullptr, bool* containsEncryptedEntries = nullptr) {
	std::vector<FileDialogEntry> images;
	bool interruptedForForeground = false;
	bool archiveLocation = false;
	std::string archiveFormat;
	WorkContext classificationContext;
	SourceWorkLease classificationLease;
	if (!AcquireDirectoryEnumerationLease(directory, shouldContinue,
		interruptedForForeground, classificationContext, classificationLease)) return {};
	{
		ScopedWorkContext activeContext(classificationContext);
		if (classificationContext.Continue()) {
			archiveLocation = IsArchiveLocation(directory);
			if (archiveLocation) archiveFormat = ArchiveFormatName(directory);
		}
		ReleaseDirectoryEnumerationLease(classificationContext, classificationLease);
	}
	if (interruptedForForeground || !shouldContinue()) return {};
	if (archiveLocation) {
		const bool promptsForArchivePassword = archiveFormat == ".7Z" || archiveFormat == "RAR";
		std::vector<ArchiveEntryInfo> archiveEntries;
		std::string localErrorMessage;
		ArchiveErrorKind localErrorKind = ArchiveErrorKind::None;
		bool localContainsEncryptedEntries = false;
		if (!ListArchiveDirectoryCancellable(directory, archiveEntries,
			shouldContinue, localErrorMessage, &localErrorKind,
			&localContainsEncryptedEntries)) {
			if (errorMessage != nullptr) *errorMessage = std::move(localErrorMessage);
			if (errorKind != nullptr) *errorKind = localErrorKind;
			if (containsEncryptedEntries != nullptr) {
				*containsEncryptedEntries = promptsForArchivePassword && localContainsEncryptedEntries;
			}
			return {};
		}
		if (containsEncryptedEntries != nullptr) {
			*containsEncryptedEntries = promptsForArchivePassword && localContainsEncryptedEntries;
		}
		for (const ArchiveEntryInfo& entry : archiveEntries) {
			if (!shouldContinue()) return {};
			if (!entry.directory && IsSupportedImagePath(entry.path)) {
				FileDialogEntry image{entry.path, false, false,
					ArchiveFileModificationTime(entry.modificationTime), false, true,
					entry.encrypted, entry.size, true};
				image.sourceDescriptor = DescribeArchiveMember(entry.path,
					entry.backingIdentity, entry.size, entry.modificationTime,
					entry.encrypted);
				images.push_back(std::move(image));
			}
		}
		SortFileDialogEntries(images, mode);
		if (errorMessage != nullptr) errorMessage->clear();
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
		WorkContext passwordContext;
		passwordContext.sourcePriority = SourceWorkPriority::Metadata;
		passwordContext.shouldContinue = shouldContinue;
		if (promptsForArchivePassword && !images.empty() && images.front().encrypted &&
			!HasSessionArchivePassword(directory, passwordContext)) {
			if (errorMessage != nullptr) {
				*errorMessage = "password required for encrypted archive image";
			}
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::PasswordRequired;
			return {};
		}
		return images.empty() ? std::filesystem::path{} : images.front().path;
	}
	std::error_code iteratorError;
	std::filesystem::directory_iterator iterator;
	const std::filesystem::directory_iterator end;
	bool iteratorStarted = false;
	while (!iteratorStarted || (iterator != end && !iteratorError)) {
		WorkContext context;
		SourceWorkLease lease;
		if (!AcquireDirectoryEnumerationLease(directory, shouldContinue,
			interruptedForForeground, context, lease)) return {};
		{
			ScopedWorkContext activeContext(context);
			if (!iteratorStarted) {
				iterator = std::filesystem::directory_iterator(directory, iteratorError);
				iteratorStarted = true;
			}
			std::size_t batchEntries = 0;
			while (!iteratorError && iterator != end &&
				batchEntries < kDirectoryEnumerationBatchSize) {
				if (!context.Continue()) break;
				std::error_code statusError;
				if (iterator->is_regular_file(statusError) && !statusError &&
					IsSupportedImagePath(iterator->path())) {
					std::error_code modificationError;
					const std::filesystem::file_time_type modificationTime =
						iterator->last_write_time(modificationError);
					images.emplace_back(iterator->path(), false, false,
						modificationError ? std::filesystem::file_time_type{} : modificationTime);
				}
				iterator.increment(iteratorError);
				++batchEntries;
			}
			ReleaseDirectoryEnumerationLease(context, lease);
		}
		if (interruptedForForeground || !shouldContinue()) return {};
	}
	if (!shouldContinue()) return {};
	SortFileDialogEntries(images, mode);
	return images.empty() ? std::filesystem::path{} : images.front().path;
}

} // namespace

void SortFileDialogEntries(std::vector<FileDialogEntry>& entries, FileDialogSortMode mode) {
	const FileDialogEntrySortOrders orders = BuildFileDialogEntrySortOrders(entries);
	if (!orders.completed) return;
	const std::vector<std::size_t>& order = mode == FileDialogSortMode::Name ?
		orders.name : orders.modificationDate;
	std::vector<FileDialogEntry> sorted;
	sorted.reserve(entries.size());
	for (std::size_t index : order) sorted.push_back(std::move(entries[index]));
	entries = std::move(sorted);
}

FileDialogEntrySortOrders BuildFileDialogEntrySortOrders(
	const std::vector<FileDialogEntry>& entries,
	const std::function<bool()>& shouldContinue) {
	FileDialogEntrySortOrders orders;
	const auto continueWork = [&shouldContinue] {
		return !shouldContinue || shouldContinue();
	};
	if (!continueWork()) return orders;
	orders.name.resize(entries.size());
	orders.modificationDate.resize(entries.size());
	std::vector<std::string> foldedNames;
	std::vector<std::string> fullPaths;
	foldedNames.reserve(entries.size());
	fullPaths.reserve(entries.size());
	for (std::size_t index = 0; index < entries.size(); ++index) {
		if ((index % kFileDialogSortPreparationBatchSize) == 0 && !continueWork()) {
			return {};
		}
		const FileDialogEntry& entry = entries[index];
		orders.name[index] = index;
		orders.modificationDate[index] = index;
		foldedNames.push_back(Lower(entry.path.filename().string()));
		fullPaths.push_back(entry.path.string());
	}
	const auto less = [&entries, &foldedNames, &fullPaths](FileDialogSortMode mode,
		std::size_t leftIndex, std::size_t rightIndex) {
		const FileDialogEntry& left = entries[leftIndex];
		const FileDialogEntry& right = entries[rightIndex];
		if (left.parent != right.parent) return left.parent;
		if (left.directory != right.directory) return left.directory;
		if (mode == FileDialogSortMode::ModificationDate &&
			left.modificationTime != right.modificationTime) {
			return left.modificationTime > right.modificationTime;
		}
		return foldedNames[leftIndex] == foldedNames[rightIndex] ?
			fullPaths[leftIndex] < fullPaths[rightIndex] :
			foldedNames[leftIndex] < foldedNames[rightIndex];
	};
	std::size_t comparisons = 0;
	const auto cancellableLess = [&less, &continueWork, &comparisons](
		FileDialogSortMode mode, std::size_t left, std::size_t right) {
		if ((++comparisons % kFileDialogSortCancellationBatchSize) == 0 &&
			!continueWork()) throw FileDialogSortCancelled{};
		return less(mode, left, right);
	};
	try {
		std::sort(orders.name.begin(), orders.name.end(), [&cancellableLess](
			std::size_t left, std::size_t right) {
			return cancellableLess(FileDialogSortMode::Name, left, right);
		});
		std::sort(orders.modificationDate.begin(), orders.modificationDate.end(),
			[&cancellableLess](std::size_t left, std::size_t right) {
				return cancellableLess(FileDialogSortMode::ModificationDate, left, right);
			});
	} catch (const FileDialogSortCancelled&) {
		return {};
	}
	if (!continueWork()) return {};
	orders.completed = true;
	return orders;
}

const std::string& FileDialogListingLoadingMessage() {
	static const std::string message = "Reading contents…";
	return message;
}

void UpdateFileDialogListingMessage(std::string& message,
	const std::string& listingMessage) {
	if (message == FileDialogListingLoadingMessage()) message = listingMessage;
}

bool FileDialogShouldClearSelectionAfterListing(bool saveDialog,
	bool includeNonImageFiles, bool activationPending) {
	return saveDialog || (includeNonImageFiles && !activationPending);
}

bool EraseLastUtf8CodePoint(std::string& text) {
	if (text.empty()) return false;
	const std::size_t last = text.size() - 1;
	std::size_t position = last;
	while (position > 0 &&
		(static_cast<unsigned char>(text[position]) & 0xc0u) == 0x80u) --position;
	const unsigned char lead = static_cast<unsigned char>(text[position]);
	const std::size_t expectedBytes = (lead & 0x80u) == 0 ? 1 :
		(lead & 0xe0u) == 0xc0u ? 2 : (lead & 0xf0u) == 0xe0u ? 3 :
		(lead & 0xf8u) == 0xf0u ? 4 : 0;
	if (expectedBytes == text.size() - position) {
		text.erase(position);
	} else {
		// Keep preceding valid text intact if the input ends in a malformed sequence.
		text.erase(last);
	}
	return true;
}

std::vector<FileDialogEntry> FilterFileDialogEntries(
	const std::vector<FileDialogEntry>& entries, std::string_view filter, bool matchFullPath) {
	if (filter.empty()) return entries;
	const std::string loweredFilter = Lower(std::string(filter));
	std::vector<FileDialogEntry> filtered;
	filtered.reserve(entries.size());
	for (const FileDialogEntry& entry : entries) {
		const std::string candidate = matchFullPath ? entry.path.string() :
			entry.path.filename().string();
		if (entry.parent || Lower(candidate).find(loweredFilter) != std::string::npos) {
			filtered.push_back(entry);
		}
	}
	return filtered;
}

DirectorySummary CountImmediateDirectoryContents(const std::filesystem::path& directory) {
	return CountImmediateDirectoryContentsWhile(directory, [] { return true; });
}

std::string FormatDirectorySummary(const DirectorySummary& summary) {
	std::ostringstream text;
	text << summary.imageCount << (summary.imageCount == 1 ? " image, " : " images, ")
		<< summary.subdirectoryCount << (summary.subdirectoryCount == 1 ? " dir" : " dirs");
	return text.str();
}

std::filesystem::path FirstImageInDirectory(
	const std::filesystem::path& directory, FileDialogSortMode mode) {
	return FirstImageInDirectoryWhile(directory, mode, [] { return true; });
}

FileDialogPreviewSize FileDialogPreviewImageSize(int paneWidth, int paneHeight) {
	return FileDialogPreviewSize{
		std::max(1, paneWidth - 16),
		std::max(1, paneHeight - 64),
	};
}

FileDialogPreviewFooterLayout CalculateFileDialogPreviewFooterLayout(
	int contentWidth, int filenameTextWidth, int detailsTextWidth, int gap) {
	FileDialogPreviewFooterLayout layout;
	const int width = std::max(0, contentWidth);
	layout.filenameWidth = width;
	layout.detailsOffsetX = width;
	if (width == 0 || detailsTextWidth <= 0) return layout;

	gap = std::clamp(gap, 0, width);
	const int minimumFilenameWidth = std::min(std::max(0, filenameTextWidth),
		std::max(0, (width - gap) / 2));
	const int maximumDetailsWidth = std::max(0, width - gap - minimumFilenameWidth);
	layout.detailsWidth = std::min(detailsTextWidth, maximumDetailsWidth);
	if (layout.detailsWidth <= 0) return layout;
	layout.filenameWidth = std::max(0, width - gap - layout.detailsWidth);
	layout.detailsOffsetX = width - layout.detailsWidth;
	return layout;
}

bool ShouldRefreshFileDialogPreviewSource(const SourceKey& requested,
	const SourceDescriptor& observed) {
	return requested != observed.Key();
}

FileDialogScrollbarGeometry CalculateFileDialogScrollbarGeometry(
	int entryCount, int visibleRows, int scroll, int trackY, int trackHeight,
	int minimumThumbHeight) {
	FileDialogScrollbarGeometry geometry;
	geometry.trackY = trackY;
	geometry.trackHeight = std::max(0, trackHeight);
	visibleRows = std::max(1, visibleRows);
	entryCount = std::max(0, entryCount);
	geometry.maximumScroll = std::max(0, entryCount - visibleRows);
	geometry.scrollable = geometry.maximumScroll > 0;
	if (geometry.trackHeight == 0) {
		geometry.thumbY = trackY;
		return geometry;
	}
	if (!geometry.scrollable) {
		geometry.thumbHeight = geometry.trackHeight;
		geometry.thumbY = trackY;
		return geometry;
	}
	const std::int64_t proportionalHeight =
		static_cast<std::int64_t>(geometry.trackHeight) * visibleRows / entryCount;
	const int minimum = std::clamp(minimumThumbHeight, 1, geometry.trackHeight);
	geometry.thumbHeight = static_cast<int>(std::clamp<std::int64_t>(
		std::max<std::int64_t>(minimum, proportionalHeight), 1, geometry.trackHeight));
	const int scrollRange = geometry.trackHeight - geometry.thumbHeight;
	const int clampedScroll = std::clamp(scroll, 0, geometry.maximumScroll);
	const std::int64_t thumbOffset = (static_cast<std::int64_t>(clampedScroll) * scrollRange +
		geometry.maximumScroll / 2) / geometry.maximumScroll;
	geometry.thumbY = trackY + static_cast<int>(thumbOffset);
	return geometry;
}

int FileDialogScrollForThumbPosition(const FileDialogScrollbarGeometry& geometry,
	int requestedThumbY) {
	if (!geometry.scrollable || geometry.maximumScroll <= 0) return 0;
	const int scrollRange = geometry.trackHeight - geometry.thumbHeight;
	if (scrollRange <= 0) return 0;
	const int thumbOffset = std::clamp(requestedThumbY - geometry.trackY, 0, scrollRange);
	const std::int64_t scaledOffset = static_cast<std::int64_t>(thumbOffset) *
		geometry.maximumScroll + scrollRange / 2;
	return static_cast<int>(scaledOffset / scrollRange);
}

void FileDialogModel::Begin(bool saveDialog) {
	saveDialog_ = saveDialog;
	filter_.clear();
	allEntries_.clear();
	sortOrders_ = {};
	visibleIndices_.clear();
	visiblePositionByEntry_.clear();
	entryIndexByPath_.clear();
	selected_ = -1;
	scroll_ = 0;
	matchFullPath_ = false;
}

void FileDialogModel::Clear() {
	Begin(false);
}

void FileDialogModel::SetEntries(std::vector<FileDialogEntry> entries) {
	FileDialogEntrySortOrders orders = BuildFileDialogEntrySortOrders(entries);
	SetEntriesWithPreparedOrder(std::move(entries), std::move(orders));
}

void FileDialogModel::SetEntriesInOrder(std::vector<FileDialogEntry> entries, bool matchFullPath) {
	allEntries_ = std::move(entries);
	sortOrders_.name.resize(allEntries_.size());
	std::iota(sortOrders_.name.begin(), sortOrders_.name.end(), 0);
	sortOrders_.modificationDate = sortOrders_.name;
	sortOrders_.completed = true;
	matchFullPath_ = matchFullPath;
	RebuildPathIndex();
	ApplyFilter();
}

void FileDialogModel::SetEntriesWithPreparedOrder(std::vector<FileDialogEntry> entries,
	FileDialogEntrySortOrders orders) {
	const auto isPermutation = [size = entries.size()](
		const std::vector<std::size_t>& order) {
		if (order.size() != size) return false;
		std::vector<bool> seen(size, false);
		for (std::size_t index : order) {
			if (index >= size || seen[index]) return false;
			seen[index] = true;
		}
		return true;
	};
	if (!orders.completed || !isPermutation(orders.name) ||
		!isPermutation(orders.modificationDate)) {
		orders = BuildFileDialogEntrySortOrders(entries);
	}
	allEntries_ = std::move(entries);
	sortOrders_ = std::move(orders);
	matchFullPath_ = false;
	RebuildPathIndex();
	ApplyFilter();
}

void FileDialogModel::AppendFilter(std::string_view text) {
	if (saveDialog_ || text.empty()) return;
	filter_.append(text.data(), text.size());
	ApplyFilter();
}

bool FileDialogModel::BackspaceFilter() {
	if (saveDialog_ || !EraseLastUtf8CodePoint(filter_)) return false;
	ApplyFilter();
	return true;
}

void FileDialogModel::ClearFilter() {
	if (filter_.empty()) return;
	filter_.clear();
	ApplyFilter();
}

void FileDialogModel::ToggleSortMode(int visibleRows) {
	if (saveDialog_) return;
	std::filesystem::path selectedPath;
	if (const FileDialogEntry* selected = SelectedEntry()) selectedPath = selected->path;
	sortMode_ = sortMode_ == FileDialogSortMode::Name ?
		FileDialogSortMode::ModificationDate : FileDialogSortMode::Name;
	ApplyFilter();
	if (!selectedPath.empty()) Focus(selectedPath, visibleRows);
}

void FileDialogModel::MoveSelection(int direction, int visibleRows) {
	if (visibleIndices_.empty()) return;
	selected_ = std::clamp(selected_ + direction, 0,
		static_cast<int>(visibleIndices_.size()) - 1);
	EnsureSelectionVisible(visibleRows);
}

void FileDialogModel::MoveSelectionByPage(int direction, int visibleRows) {
	MoveSelection(direction * std::max(1, visibleRows), visibleRows);
}

void FileDialogModel::ScrollBy(int rows, int visibleRows) {
	visibleRows = std::max(1, visibleRows);
	const int maximumScroll = std::max(0, static_cast<int>(visibleIndices_.size()) - visibleRows);
	const std::int64_t nextScroll = static_cast<std::int64_t>(scroll_) + rows;
	scroll_ = static_cast<int>(std::clamp<std::int64_t>(nextScroll, 0, maximumScroll));
}

void FileDialogModel::ScrollTo(int rows, int visibleRows) {
	visibleRows = std::max(1, visibleRows);
	const int maximumScroll = std::max(0, static_cast<int>(visibleIndices_.size()) - visibleRows);
	scroll_ = std::clamp(rows, 0, maximumScroll);
}

void FileDialogModel::SelectFirst(int visibleRows) {
	Select(0, visibleRows);
}

void FileDialogModel::SelectLast(int visibleRows) {
	Select(static_cast<int>(visibleIndices_.size()) - 1, visibleRows);
}

void FileDialogModel::Select(int index, int visibleRows) {
	if (index < 0 || index >= static_cast<int>(visibleIndices_.size())) return;
	selected_ = index;
	EnsureSelectionVisible(visibleRows);
}

bool FileDialogModel::Focus(const std::filesystem::path& path, int visibleRows) {
	const auto found = entryIndexByPath_.find(PathIndexKey(path));
	if (found == entryIndexByPath_.end()) return false;
	const std::size_t entryIndex = found->second;
	if (entryIndex >= visiblePositionByEntry_.size() ||
		visiblePositionByEntry_[entryIndex] < 0) return false;
	Select(visiblePositionByEntry_[entryIndex], visibleRows);
	return true;
}

bool FileDialogModel::MarkEncrypted(const std::filesystem::path& path) {
	const auto found = entryIndexByPath_.find(PathIndexKey(path));
	if (found == entryIndexByPath_.end()) return false;
	allEntries_[found->second].encrypted = true;
	return true;
}

bool FileDialogModel::SetFileSize(const std::filesystem::path& path, std::uintmax_t size) {
	const auto found = entryIndexByPath_.find(PathIndexKey(path));
	if (found == entryIndexByPath_.end()) return false;
	FileDialogEntry& entry = allEntries_[found->second];
	entry.fileSize = size;
	entry.fileSizeKnown = true;
	return true;
}

bool FileDialogModel::RefreshSourceDescriptor(const SourceKey& expected,
	const SourceDescriptor& observed) {
	const std::filesystem::path normalizedPath = observed.LogicalPath().lexically_normal();
	const auto found = entryIndexByPath_.find(PathIndexKey(normalizedPath));
	if (found == entryIndexByPath_.end()) return false;
	FileDialogEntry& entry = allEntries_[found->second];
	if (entry.sourceDescriptor.Key() != expected) return false;
	entry.sourceDescriptor = observed;
	entry.archiveMember = observed.Metadata().archiveMember;
	entry.encrypted = observed.Metadata().archiveMemberEncrypted;
	entry.fileSizeKnown = observed.Metadata().hasFileSize;
	entry.fileSize = observed.Metadata().hasFileSize ?
		static_cast<std::uintmax_t>(observed.Metadata().fileSize) : 0;
	return true;
}

void FileDialogModel::ClearSelection() {
	selected_ = -1;
}

const FileDialogEntry* FileDialogModel::SelectedEntry() const {
	return selected_ >= 0 && selected_ < static_cast<int>(visibleIndices_.size()) ?
		&allEntries_[visibleIndices_[static_cast<std::size_t>(selected_)]] : nullptr;
}

std::string FileDialogModel::PathIndexKey(const std::filesystem::path& path) {
	return path.lexically_normal().string();
}

void FileDialogModel::RebuildPathIndex() {
	entryIndexByPath_.clear();
	entryIndexByPath_.reserve(allEntries_.size());
	for (std::size_t index = 0; index < allEntries_.size(); ++index) {
		entryIndexByPath_.emplace(PathIndexKey(allEntries_[index].path), index);
	}
}

void FileDialogModel::ApplyFilter() {
	visibleIndices_.clear();
	visiblePositionByEntry_.assign(allEntries_.size(), -1);
	const std::vector<std::size_t>& order =
		(saveDialog_ || sortMode_ == FileDialogSortMode::Name) ?
		sortOrders_.name : sortOrders_.modificationDate;
	const std::string loweredFilter = saveDialog_ ? std::string() : Lower(filter_);
	visibleIndices_.reserve(order.size());
	for (std::size_t entryIndex : order) {
		const FileDialogEntry& entry = allEntries_[entryIndex];
		const std::string candidate = matchFullPath_ ? entry.path.string() :
			entry.path.filename().string();
		if (!entry.parent && !loweredFilter.empty() &&
			Lower(candidate).find(loweredFilter) == std::string::npos) continue;
		visiblePositionByEntry_[entryIndex] = static_cast<int>(visibleIndices_.size());
		visibleIndices_.push_back(entryIndex);
	}
	scroll_ = 0;
	if (visibleIndices_.empty()) {
		selected_ = -1;
		return;
	}
	selected_ = 0;
	if (saveDialog_) return;
	const auto firstChild = std::find_if(visibleIndices_.begin(), visibleIndices_.end(),
		[this](std::size_t index) { return !allEntries_[index].parent; });
	if (firstChild != visibleIndices_.end()) {
		selected_ = static_cast<int>(std::distance(visibleIndices_.begin(), firstChild));
	} else if (!filter_.empty()) {
		selected_ = -1;
	}
}

void FileDialogModel::EnsureSelectionVisible(int visibleRows) {
	visibleRows = std::max(1, visibleRows);
	if (selected_ < scroll_) scroll_ = selected_;
	if (selected_ >= scroll_ + visibleRows) scroll_ = selected_ - visibleRows + 1;
	const int maximumScroll = std::max(0, static_cast<int>(visibleIndices_.size()) - visibleRows);
	scroll_ = std::clamp(scroll_, 0, maximumScroll);
}

struct FileDialogDirectoryLoader::Impl {
	struct Task {
		std::filesystem::path directory;
		std::uint64_t generation = 0;
		FileDialogListingPolicy policy;
	};

	explicit Impl(FileDialogDirectoryLoader::Enumerator customEnumerator)
		: enumerator(std::move(customEnumerator)) {}

	~Impl() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping.store(true);
			currentGeneration.fetch_add(1);
		}
		condition.notify_all();
		SourceWorkCoordinator::Global().NotifyWaiters();
		if (worker.joinable()) worker.join();
	}

	bool IsCurrent(std::uint64_t generation) const {
		return !stopping.load() && currentGeneration.load() == generation;
	}

	void Run() {
		while (!stopping.load()) {
			Task task;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] { return stopping.load() || !pending.empty(); });
				if (stopping.load()) return;
				task = std::move(pending.front());
				pending.pop_front();
			}

			try {
				FileDialogDirectoryResult result;
				bool interruptedForForeground = false;
				const auto shouldContinue = [this, generation = task.generation] {
					return IsCurrent(generation);
				};
				for (;;) {
					interruptedForForeground = false;
					result = {};
					try {
						result.directory = task.directory;
						if (enumerator) {
							result = enumerator(task.directory, task.policy, shouldContinue);
						} else {
							result = EnumerateFileDialogDirectory(task.directory, task.policy,
								shouldContinue, interruptedForForeground);
						}
					} catch (const std::exception& error) {
						result.entries.clear();
						result.error = error.what();
					} catch (...) {
						result.entries.clear();
						result.error = "listing failed";
					}
					if (!IsCurrent(task.generation)) break;
					if (!interruptedForForeground) break;
					result = {};
					SourceWorkCoordinator& coordinator = SourceWorkCoordinator::Global();
					(void)coordinator.WaitForSnapshot(
						[this, generation = task.generation](const SourceWorkSnapshot& snapshot) {
							return !IsCurrent(generation) || !snapshot.foregroundPending;
						}, std::chrono::hours(24));
					if (!IsCurrent(task.generation)) break;
				}
				if (!IsCurrent(task.generation)) continue;
				result.directory = task.directory;
				result.generation = task.generation;
				result.policy = task.policy;
				try {
					SourceWorkCoordinator& coordinator = SourceWorkCoordinator::Global();
					for (;;) {
						bool interruptedForForeground = false;
						WorkContext sortContext;
						sortContext.sourcePriority = SourceWorkPriority::Metadata;
						sortContext.shouldContinue = [this, &coordinator,
							&interruptedForForeground, generation = task.generation] {
							if (!IsCurrent(generation)) return false;
							if (coordinator.Snapshot().foregroundPending) {
								interruptedForForeground = true;
								return false;
							}
							return true;
						};
						{
							CpuWorkLease cpu = coordinator.AcquireCpu(sortContext);
							if (cpu && sortContext.Continue()) {
								result.sortOrders = BuildFileDialogEntrySortOrders(result.entries,
									sortContext.shouldContinue);
							}
						}
						if (result.sortOrders.completed || !IsCurrent(task.generation)) break;
						if (!interruptedForForeground) {
							result.error = "sort failed";
							break;
						}
						(void)coordinator.WaitForSnapshot(
							[this, generation = task.generation](
								const SourceWorkSnapshot& snapshot) {
								return !IsCurrent(generation) || !snapshot.foregroundPending;
							}, std::chrono::hours(24));
						if (!IsCurrent(task.generation)) break;
					}
					if (!IsCurrent(task.generation)) continue;
					if (!result.sortOrders.completed) {
						result.entries.clear();
						result.sortOrders = {};
					}
				} catch (...) {
					if (!IsCurrent(task.generation)) continue;
					result.entries.clear();
					result.sortOrders = {};
					result.error = "sort failed";
				}

				bool published = false;
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (IsCurrent(task.generation)) {
						ready.push_back(std::move(result));
						published = true;
					}
				}
				if (published) UiCompletionWakeup().Notify();
			}
			catch (...) {
				// Keep the worker alive when completion assembly or delivery fails.
			}
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	std::mutex mutex;
	std::condition_variable condition;
	FileDialogDirectoryLoader::Enumerator enumerator;
	std::deque<Task> pending;
	std::vector<FileDialogDirectoryResult> ready;
	std::atomic<std::uint64_t> currentGeneration{0};
	std::atomic<bool> stopping{false};
	std::thread worker;
};

FileDialogDirectoryLoader::FileDialogDirectoryLoader(Enumerator enumerator)
	: impl_(std::make_unique<Impl>(std::move(enumerator))) {}
FileDialogDirectoryLoader::~FileDialogDirectoryLoader() = default;

void FileDialogDirectoryLoader::Request(const std::filesystem::path& directory,
	std::uint64_t generation, FileDialogListingPolicy policy) {
	impl_->currentGeneration.store(generation);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->pending.clear();
		impl_->ready.clear();
		impl_->StartWorkerLocked();
		impl_->pending.push_back(Impl::Task{directory, generation, policy});
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
	impl_->condition.notify_one();
}

void FileDialogDirectoryLoader::Clear(std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->pending.clear();
		impl_->ready.clear();
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
}

std::vector<FileDialogDirectoryResult> FileDialogDirectoryLoader::TakeReady() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<FileDialogDirectoryResult> results;
	results.swap(impl_->ready);
	return results;
}

struct ArchiveDirectoryLoader::Impl {
	struct Task {
		std::filesystem::path directory;
		std::uint64_t generation = 0;
		std::string password;
		bool validatePassword = false;
	};

	Impl() = default;

	~Impl() {
		stopping.store(true);
		currentGeneration.fetch_add(1);
		condition.notify_one();
		SourceWorkCoordinator::Global().NotifyWaiters();
		if (worker.joinable()) worker.join();
	}

	void Run() {
		while (!stopping.load()) {
			Task task;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] { return stopping.load() || !pending.empty(); });
				if (stopping.load()) return;
				task = std::move(pending.front());
				pending.pop_front();
			}

			ArchiveDirectoryResult result;
			result.directory = task.directory;
			result.generation = task.generation;
			result.passwordValidation = task.validatePassword;
			const auto isCurrent = [this, generation = task.generation] {
				return !stopping.load() && currentGeneration.load() == generation;
			};
			WorkContext workContext = MakePathWorkContext(task.directory,
				SourceWorkPriority::Metadata, isCurrent);
			if (task.validatePassword) {
				bool foregroundYieldObserved = false;
				WorkContext validationContext = MakePathWorkContext(task.directory,
					SourceWorkPriority::Metadata,
					[this, generation = task.generation, &foregroundYieldObserved] {
						if (stopping.load() || currentGeneration.load() != generation) {
							return false;
						}
						if (SourceWorkCoordinator::Global().Snapshot().foregroundPending) {
							foregroundYieldObserved = true;
							return false;
						}
						return true;
					});
				for (;;) {
					foregroundYieldObserved = false;
					try {
						if (isCurrent()) {
							(void)ValidateArchivePassword(task.directory, task.password,
								result.error, &result.errorKind, validationContext);
						}
					} catch (const std::exception& error) {
						result.error = error.what();
					} catch (...) {
						result.error = "unknown archive password validation failure";
					}
					if (!isCurrent() || !foregroundYieldObserved) break;
					result.error.clear();
					result.errorKind = ArchiveErrorKind::None;
					(void)SourceWorkCoordinator::Global().WaitForSnapshot(
						[&isCurrent](const SourceWorkSnapshot& snapshot) {
							return !isCurrent() || !snapshot.foregroundPending;
						}, std::chrono::hours(24));
					if (!isCurrent()) break;
				}
				std::fill(task.password.begin(), task.password.end(), '\0');
			} else {
				ScopedWorkContext activeContext(workContext);
				for (;;) {
					try {
						(void)ListArchiveDirectoryCancellable(task.directory, result.entries,
							isCurrent, result.error, &result.errorKind,
							&result.containsEncryptedEntries);
					} catch (const std::exception& error) {
						result.error = error.what();
					} catch (...) {
						result.error = "unknown archive directory worker failure";
					}
					if (!isCurrent() || result.error.find("cancelled") == std::string::npos) break;
					result.error.clear();
					result.errorKind = ArchiveErrorKind::None;
					result.entries.clear();
					(void)SourceWorkCoordinator::Global().WaitForSnapshot(
						[&isCurrent](const SourceWorkSnapshot& snapshot) {
							return !isCurrent() || !snapshot.foregroundPending;
						}, std::chrono::hours(24));
				}
			}
			if (!isCurrent()) continue;

			bool published = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (isCurrent()) {
					ready.push_back(std::move(result));
					published = true;
				}
			}
			if (published) UiCompletionWakeup().Notify();
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::deque<Task> pending;
	std::vector<ArchiveDirectoryResult> ready;
	std::atomic<std::uint64_t> currentGeneration{0};
	std::atomic<bool> stopping{false};
	std::thread worker;
};

ArchiveDirectoryLoader::ArchiveDirectoryLoader() : impl_(std::make_unique<Impl>()) {}
ArchiveDirectoryLoader::~ArchiveDirectoryLoader() = default;

void ArchiveDirectoryLoader::Request(const std::filesystem::path& directory,
	std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->pending.clear();
		impl_->ready.clear();
		impl_->StartWorkerLocked();
		impl_->pending.push_back(Impl::Task{directory, generation, {}, false});
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
	impl_->condition.notify_one();
}

void ArchiveDirectoryLoader::RequestPasswordValidation(const std::filesystem::path& archive,
	const std::string& password, std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->pending.clear();
		impl_->ready.clear();
		impl_->StartWorkerLocked();
		impl_->pending.push_back(Impl::Task{archive, generation, password, true});
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
	impl_->condition.notify_one();
}

void ArchiveDirectoryLoader::Clear(std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->pending.clear();
		impl_->ready.clear();
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
}

std::vector<ArchiveDirectoryResult> ArchiveDirectoryLoader::TakeReady() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<ArchiveDirectoryResult> results;
	results.swap(impl_->ready);
	return results;
}

struct FileDialogFileSizeLoader::Impl {
	struct Task {
		std::vector<SourceDescriptor> sources;
		std::uint64_t generation = 0;
	};

	explicit Impl(FileDialogFileSizeLoader::SourceCapture capture)
		: sourceCapture(std::move(capture)) {
		if (!sourceCapture) {
			sourceCapture = [](const SourceDescriptor& requested) {
				return DescribeImageSource(requested.LogicalPath());
			};
		}
	}

	~Impl() {
		stopping.store(true);
		currentGeneration.fetch_add(1);
		condition.notify_one();
		SourceWorkCoordinator::Global().NotifyWaiters();
		if (worker.joinable()) worker.join();
	}

	bool IsCurrent(std::uint64_t requestedGeneration) const {
		return !stopping.load() && currentGeneration.load() == requestedGeneration;
	}

	void Publish(std::vector<FileDialogFileSizeResult>& batch, std::uint64_t requestedGeneration) {
		if (batch.empty() || !IsCurrent(requestedGeneration)) return;
		bool published = false;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (IsCurrent(requestedGeneration)) {
				ready.insert(ready.end(), std::make_move_iterator(batch.begin()),
					std::make_move_iterator(batch.end()));
				published = true;
			}
		}
		batch.clear();
		if (published) UiCompletionWakeup().Notify();
	}

	void Run() {
		while (!stopping.load()) {
			Task task;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] { return stopping.load() || pending.has_value(); });
				if (stopping.load()) return;
				task = std::move(*pending);
				pending.reset();
			}

			std::vector<FileDialogFileSizeResult> batch;
			batch.reserve(32);
			for (const SourceDescriptor& requestedSource : task.sources) {
				if (!IsCurrent(task.generation)) break;
				FileDialogFileSizeResult result;
				result.generation = task.generation;
				result.path = requestedSource.LogicalPath();
				result.requestedSource = requestedSource;
				const auto shouldContinue = [this, generation = task.generation] {
					return IsCurrent(generation);
				};
				for (;;) {
					bool interruptedForForeground = false;
					WorkContext context = MakePathWorkContext(result.path,
						SourceWorkPriority::Metadata, shouldContinue);
					context.onForegroundYield = [&interruptedForForeground] {
						interruptedForForeground = true;
					};
					try {
						SourceCpuWorkLease admission =
							SourceWorkCoordinator::Global().AcquireSourceAndCpu(
								context, result.path);
						if (admission && context.Continue()) {
							context.sourcePriority = context.Priority();
							context.sourceAccessAlreadyAdmitted = true;
							context.cpuProcessingAlreadyAdmitted = true;
							ScopedWorkContext activeContext(context);
							result.observedSource = sourceCapture(requestedSource);
							result.size = result.observedSource.Metadata().fileSize;
						} else if (IsCurrent(task.generation)) {
							result.failure = {WorkerFailureKind::Cancelled,
								"file-size source admission was cancelled"};
						}
					} catch (const std::exception& error) {
						result.failure = {WorkerFailureKind::Exception, error.what()};
					} catch (...) {
						result.failure = {WorkerFailureKind::Exception,
							"unknown file-size worker failure"};
					}
					if (!interruptedForForeground || !IsCurrent(task.generation)) break;
					SourceWorkCoordinator& coordinator = SourceWorkCoordinator::Global();
					(void)coordinator.WaitForSnapshot(
						[this, generation = task.generation](
							const SourceWorkSnapshot& snapshot) {
							return !IsCurrent(generation) || !snapshot.foregroundPending;
						}, std::chrono::hours(24));
					if (!IsCurrent(task.generation)) break;
					result = {};
					result.generation = task.generation;
					result.path = requestedSource.LogicalPath();
					result.requestedSource = requestedSource;
				}
				if (!IsCurrent(task.generation)) break;
				batch.push_back(std::move(result));
				if (batch.size() >= 32) Publish(batch, task.generation);
			}
			Publish(batch, task.generation);
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	std::mutex mutex;
	std::condition_variable condition;
	FileDialogFileSizeLoader::SourceCapture sourceCapture;
	std::optional<Task> pending;
	std::deque<FileDialogFileSizeResult> ready;
	std::atomic<std::uint64_t> currentGeneration{0};
	std::atomic<bool> stopping{false};
	std::thread worker;
};

FileDialogFileSizeLoader::FileDialogFileSizeLoader(SourceCapture sourceCapture)
	: impl_(std::make_unique<Impl>(std::move(sourceCapture))) {}
FileDialogFileSizeLoader::~FileDialogFileSizeLoader() = default;

void FileDialogFileSizeLoader::Request(
	const std::vector<std::filesystem::path>& paths, std::uint64_t generation) {
	std::vector<SourceDescriptor> sources;
	sources.reserve(paths.size());
	for (const std::filesystem::path& path : paths) {
		sources.emplace_back(path, SourceIdentity{}, SourceMetadata{});
	}
	RequestSources(sources, generation);
}

void FileDialogFileSizeLoader::RequestSources(
	const std::vector<SourceDescriptor>& sources, std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->pending.reset();
		impl_->ready.clear();
		if (!sources.empty()) {
			impl_->StartWorkerLocked();
			impl_->pending = Impl::Task{sources, generation};
		}
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
	if (!sources.empty()) impl_->condition.notify_one();
}

void FileDialogFileSizeLoader::Clear(std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->pending.reset();
		impl_->ready.clear();
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
}

std::vector<FileDialogFileSizeResult> FileDialogFileSizeLoader::TakeReady(
	std::size_t maximumResults) {
	std::vector<FileDialogFileSizeResult> results;
	if (maximumResults == 0) return results;
	bool hasMore = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		results.reserve(std::min(maximumResults, impl_->ready.size()));
		while (results.size() < maximumResults && !impl_->ready.empty()) {
			results.push_back(std::move(impl_->ready.front()));
			impl_->ready.pop_front();
		}
		hasMore = !impl_->ready.empty();
	}
	if (hasMore) UiCompletionWakeup().Notify();
	return results;
}

struct DirectorySummaryLoader::Impl {
	struct Task {
		std::filesystem::path directory;
		std::uint64_t generation = 0;
	};

	Impl() = default;

	~Impl() {
		stopping.store(true);
		currentGeneration.fetch_add(1);
		condition.notify_one();
		SourceWorkCoordinator::Global().NotifyWaiters();
		if (worker.joinable()) worker.join();
	}

	void Run() {
		while (!stopping.load()) {
			Task task;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] { return stopping.load() || !pending.empty(); });
				if (stopping.load()) return;
				task = std::move(pending.front());
				pending.pop_front();
			}

			DirectorySummary summary;
			WorkerFailure failure;
			for (;;) {
				bool scanComplete = false;
				bool interruptedForForeground = false;
				std::string attemptError;
				const auto shouldContinue = [this, generation = task.generation,
					&interruptedForForeground] {
					if (stopping.load() || currentGeneration.load() != generation) return false;
					SourceWorkCoordinator& coordinator = SourceWorkCoordinator::Global();
					if (!coordinator.Snapshot().foregroundPending) return true;
					interruptedForForeground = true;
					yieldingForForeground.store(true);
					return false;
				};
				WorkContext workContext;
				workContext.sourcePriority = SourceWorkPriority::Metadata;
				workContext.shouldContinue = shouldContinue;
				workContext.onForegroundYield = [this, &interruptedForForeground] {
					interruptedForForeground = true;
					yieldingForForeground.store(true);
				};
				try {
					ScopedWorkContext activeContext(workContext);
					summary = CountImmediateDirectoryContentsWhile(task.directory,
						shouldContinue, &scanComplete, &attemptError);
				} catch (const std::exception& error) {
					failure = {WorkerFailureKind::Exception, error.what()};
				} catch (...) {
					failure = {WorkerFailureKind::Exception,
						"unknown directory summary worker failure"};
				}
				if (stopping.load() || currentGeneration.load() != task.generation ||
					failure.Failed()) {
					yieldingForForeground.store(false);
					break;
				}
				if (!interruptedForForeground && scanComplete) break;
				if (!interruptedForForeground) {
					failure = {WorkerFailureKind::ProcessingFailed,
						attemptError.empty() ? "directory summary scan did not complete" :
							attemptError};
					break;
				}
				SourceWorkCoordinator& coordinator = SourceWorkCoordinator::Global();
				yieldingForForeground.store(true);
				(void)coordinator.WaitForSnapshot(
					[this, generation = task.generation](const SourceWorkSnapshot& snapshot) {
						return stopping.load() || currentGeneration.load() != generation ||
							!snapshot.foregroundPending;
					}, std::chrono::hours(24));
				yieldingForForeground.store(false);
				if (stopping.load() || currentGeneration.load() != task.generation) break;
				summary = {};
				failure = {};
			}
			if (stopping.load() || currentGeneration.load() != task.generation) continue;

			bool published = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (currentGeneration.load() == task.generation) {
					ready.push_back(DirectorySummaryResult{task.directory, task.generation,
						summary, failure});
					published = true;
				}
			}
			if (published) UiCompletionWakeup().Notify();
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::deque<Task> pending;
	std::deque<DirectorySummaryResult> ready;
	std::atomic<std::uint64_t> currentGeneration{0};
	std::atomic<bool> stopping{false};
	std::atomic<bool> yieldingForForeground{false};
	std::thread worker;
};

DirectorySummaryLoader::DirectorySummaryLoader() : impl_(std::make_unique<Impl>()) {}
DirectorySummaryLoader::~DirectorySummaryLoader() = default;

void DirectorySummaryLoader::Request(
	const std::vector<std::filesystem::path>& directories, std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->pending.clear();
		impl_->ready.clear();
		if (!directories.empty()) {
			impl_->StartWorkerLocked();
			for (const std::filesystem::path& directory : directories) {
				impl_->pending.push_back(Impl::Task{directory, generation});
			}
		}
	}
	SourceWorkCoordinator::Global().NotifyWaiters();
	if (!directories.empty()) impl_->condition.notify_one();
}

bool DirectorySummaryLoader::IsYieldingForForeground() const {
	return impl_->yieldingForForeground.load();
}

std::vector<DirectorySummaryResult> DirectorySummaryLoader::TakeReady(
	std::size_t maximumResults) {
	std::vector<DirectorySummaryResult> results;
	if (maximumResults == 0) return results;
	bool hasMore = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		results.reserve(std::min(maximumResults, impl_->ready.size()));
		while (results.size() < maximumResults && !impl_->ready.empty()) {
			results.push_back(std::move(impl_->ready.front()));
			impl_->ready.pop_front();
		}
		hasMore = !impl_->ready.empty();
	}
	if (hasMore) UiCompletionWakeup().Notify();
	return results;
}

namespace {

enum class PreviewCancellationReason : std::uint64_t {
	PendingReplaced = 1,
	ReadyReplaced = 2,
	StaleWorkerResult = 3,
	PendingShutdown = 4,
	ReadyShutdown = 5,
};

void RecordPreviewCancellation(std::uint64_t generation,
	PreviewCancellationReason reason, PerfExecution execution) {
	PerfContextScope workContext(PerfWorkClass::FocusedPreview, execution);
	PerfDiagnostics::Instance().RecordText(PerfMetric::Cancellation,
		generation, static_cast<std::uint64_t>(reason), 0, 0, 0, 0,
		"file_dialog_preview");
}

FileDialogPreviewResult LoadFileDialogPreview(
	const std::filesystem::path& path, bool directory, FileDialogSortMode mode,
	int maximumWidth, int maximumHeight, const std::function<bool()>& shouldContinue,
	const SourceDescriptor& providedSource,
	const FileDialogPreviewLoader::SourceCapture& sourceCapture) {
	FileDialogPreviewResult result;
	result.source = directory ? FirstImageInDirectoryWhile(path, mode, shouldContinue,
		&result.error, &result.errorKind, &result.encryptedArchive) : path;
	if (directory && result.source.empty() &&
		result.errorKind == ArchiveErrorKind::PasswordRequired) {
		result.source = path;
	}
	if (!shouldContinue()) return result;
	if (!result.error.empty()) {
		// Preserve archive errors for the UI; previews never request credentials.
	} else if (result.source.empty()) {
		result.error = "No images in this folder";
	} else {
		const bool providedPathMatches = providedSource.Valid() &&
			!providedSource.LogicalPath().empty() &&
			providedSource.LogicalPath().lexically_normal() == result.source.lexically_normal();
		result.sourceDescriptor = providedPathMatches ? providedSource :
			SourceDescriptor(result.source, {}, {});
		if (!providedPathMatches && !CapturePreviewSource(result.sourceDescriptor,
			result.source, sourceCapture, shouldContinue, result.sourceDescriptor)) return result;
		if (!result.sourceDescriptor.Valid()) {
			SourceDescriptor recaptured;
			if (!CapturePreviewSource(result.sourceDescriptor, result.source,
				sourceCapture, shouldContinue, recaptured)) return result;
			if (recaptured.Valid() && recaptured.LogicalPath().lexically_normal() ==
				result.source.lexically_normal()) {
				result.sourceDescriptor = recaptured;
			} else {
				result.observedSource = recaptured;
				result.error = "Image changed while preparing preview";
				return result;
			}
		}
		if (result.sourceDescriptor.Valid() &&
			!ValidateAndRefreshPreviewSource(result.sourceDescriptor, sourceCapture,
				shouldContinue, result.observedSource)) {
			if (!shouldContinue()) return result;
			result.error = "Image changed while preparing preview";
			return result;
		}
		const SourceMetadata& metadata = result.sourceDescriptor.Metadata();
		result.fileSize = metadata.fileSize;
		result.fileSizeKnown = metadata.hasFileSize;
		WorkContext sourceContext = MakeWorkContext(result.sourceDescriptor,
			SourceWorkPriority::Speculative, shouldContinue);
		DecodedImage decoded;
		bool success = false;
		if (IsJpegPath(result.source)) {
			success = DecodeJpegForDisplay(result.source, maximumWidth,
				maximumHeight, decoded, result.sourceWidth,
				result.sourceHeight, result.error, sourceContext);
		} else {
			success = DecodeImage(result.source, decoded, result.error, sourceContext);
		}
		if (success && !decoded.frames.empty()) {
			DecodedFrame frame = std::move(decoded.frames.front());
			if (result.sourceWidth <= 0 || result.sourceHeight <= 0) {
				result.sourceWidth = frame.width;
				result.sourceHeight = frame.height;
			}
			result.hasTransparency = frame.hasTransparency;
			const double scale = std::min({1.0,
				static_cast<double>(maximumWidth) / frame.width,
				static_cast<double>(maximumHeight) / frame.height});
			const int width = std::max(1,
				static_cast<int>(std::floor(frame.width * scale + 0.5)));
			const int height = std::max(1,
				static_cast<int>(std::floor(frame.height * scale + 0.5)));
			CpuWorkLease cpuLease = SourceWorkCoordinator::Global().AcquireCpu(sourceContext);
			if (!DownsampleThumbnailBgra(frame.bgra, frame.width, frame.height,
				width, height, result.bgra, [&sourceContext, &shouldContinue, &cpuLease] {
					return cpuLease && sourceContext.Continue() && shouldContinue();
				})) {
				result.error = "Cannot resize preview";
			} else {
				result.width = width;
				result.height = height;
			}
		} else if (success) {
			result.error = "Image has no preview frame";
		}
		if (result.sourceDescriptor.Valid() &&
			!ValidateAndRefreshPreviewSource(result.sourceDescriptor, sourceCapture,
				shouldContinue, result.observedSource)) {
			if (!shouldContinue()) return result;
			result.bgra.clear();
			result.width = 0;
			result.height = 0;
			result.error = "Image changed while preparing preview";
		}
	}
	return result;
}

} // namespace

struct FileDialogPreviewLoader::Impl {
	struct Task {
		std::filesystem::path path;
		bool directory = false;
		FileDialogSortMode mode = FileDialogSortMode::Name;
		int maximumWidth = 0;
		int maximumHeight = 0;
		std::uint64_t generation = 0;
		SourceDescriptor source;
	};

	explicit Impl(Processor previewProcessor, SourceCapture capture)
		: processor(std::move(previewProcessor)), sourceCapture(std::move(capture)) {
		useDefaultProcessor = !processor;
		if (!sourceCapture) {
			sourceCapture = [](const SourceDescriptor& requested) {
				return DescribeImageSource(requested.LogicalPath());
			};
		}
	}

	~Impl() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping.store(true);
			if (pending) {
				RecordPreviewCancellation(pending->generation,
					PreviewCancellationReason::PendingShutdown, PerfExecution::EventThread);
				pending.reset();
			}
			for (const FileDialogPreviewResult& result : ready) {
				RecordPreviewCancellation(result.generation,
					PreviewCancellationReason::ReadyShutdown, PerfExecution::EventThread);
			}
			ready.clear();
		}
		condition.notify_one();
		SourceWorkCoordinator::Global().NotifyWaiters();
		if (worker.joinable()) worker.join();
	}

	bool IsCurrent(std::uint64_t requestedGeneration) const {
		return !stopping.load() && generation.load() == requestedGeneration;
	}

	void Run() {
		while (!stopping.load()) {
			Task task;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] { return stopping.load() || pending.has_value(); });
				if (stopping.load()) return;
				task = std::move(*pending);
				pending.reset();
			}

			PerfContextScope workContext(PerfWorkClass::FocusedPreview,
				PerfExecution::WorkerThread);
			FileDialogPreviewResult result;
			for (;;) {
				bool interruptedForForeground = false;
				const auto shouldContinue = [this, generation = task.generation,
					&interruptedForForeground] {
					if (!IsCurrent(generation)) return false;
					if (SourceWorkCoordinator::Global().Snapshot().foregroundPending) {
						interruptedForForeground = true;
						return false;
					}
					return IsCurrent(generation);
				};
				WorkContext attemptContext = MakePathWorkContext(task.path,
					SourceWorkPriority::Metadata, shouldContinue);
				attemptContext.onForegroundYield = [&interruptedForForeground] {
					interruptedForForeground = true;
				};
				result = {};
				try {
					ScopedWorkContext activeContext(attemptContext);
					const bool hasInvalidRequestedImageSource = !task.directory &&
						!task.source.LogicalPath().empty() && !task.source.Valid() &&
						task.source.LogicalPath().lexically_normal() ==
							task.path.lexically_normal();
					SourceDescriptor recapturedRequestedSource;
					if (hasInvalidRequestedImageSource) {
						(void)CapturePreviewSource(task.source, task.path, sourceCapture,
							shouldContinue, recapturedRequestedSource);
						if (!recapturedRequestedSource.Valid() && shouldContinue()) {
							(void)CapturePreviewSource(task.source, task.path, sourceCapture,
								shouldContinue, recapturedRequestedSource);
						}
						if (!recapturedRequestedSource.Valid() && shouldContinue()) {
							result.source = task.path;
							result.sourceDescriptor = task.source;
							result.error = "Image changed while preparing preview";
						}
					}
					bool sourceWasStale = false;
					if (!hasInvalidRequestedImageSource && task.source.Valid() &&
						!ValidateAndRefreshPreviewSource(task.source, sourceCapture,
							shouldContinue, result.observedSource) && shouldContinue()) {
						sourceWasStale = true;
						result.source = task.source.LogicalPath();
						result.sourceDescriptor = task.source;
						result.error = "Image changed while preparing preview";
					}
					if (!sourceWasStale && (!hasInvalidRequestedImageSource ||
						recapturedRequestedSource.Valid())) {
						result = useDefaultProcessor ?
							LoadFileDialogPreview(task.path, task.directory, task.mode,
								task.maximumWidth, task.maximumHeight, shouldContinue,
								recapturedRequestedSource.Valid() ?
									recapturedRequestedSource : task.source,
								sourceCapture) :
							processor(task.path, task.directory, task.mode,
								task.maximumWidth, task.maximumHeight, shouldContinue);
					}
					if (hasInvalidRequestedImageSource && recapturedRequestedSource.Valid() &&
						shouldContinue()) {
						if (result.source.empty()) result.source = task.path;
						if (result.source.lexically_normal() == task.path.lexically_normal()) {
							result.sourceDescriptor = recapturedRequestedSource;
							result.observedSource = recapturedRequestedSource;
						}
					}
					if (result.sourceDescriptor.LogicalPath().empty() &&
						task.source.Valid() && !task.directory) {
						result.sourceDescriptor = task.source;
					}
					result.requestedSourceDescriptor = task.source;
					if (result.sourceDescriptor.Valid() && shouldContinue() &&
						!ValidateAndRefreshPreviewSource(result.sourceDescriptor,
							sourceCapture, shouldContinue, result.observedSource) &&
						shouldContinue()) {
						result.bgra.clear();
						result.width = 0;
						result.height = 0;
						result.error = "Image changed while preparing preview";
					}
				} catch (const std::exception& error) {
					result.source = task.path;
					result.error = error.what();
				} catch (...) {
					result.source = task.path;
					result.error = "unknown file preview worker failure";
				}
				if (interruptedForForeground && IsCurrent(task.generation)) {
					SourceWorkCoordinator& coordinator = SourceWorkCoordinator::Global();
					(void)coordinator.WaitForSnapshot(
						[this, generation = task.generation](
							const SourceWorkSnapshot& snapshot) {
							return !IsCurrent(generation) || !snapshot.foregroundPending;
						}, std::chrono::hours(24));
					if (!IsCurrent(task.generation)) break;
					continue;
				}
				break;
			}
			result.generation = task.generation;

			bool published = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (IsCurrent(task.generation)) {
					ready.clear();
					ready.push_back(std::move(result));
					published = true;
				}
			}
			if (!published) {
				RecordPreviewCancellation(task.generation,
					PreviewCancellationReason::StaleWorkerResult, PerfExecution::WorkerThread);
			}
			if (published) UiCompletionWakeup().Notify();
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	Processor processor;
	bool useDefaultProcessor = false;
	SourceCapture sourceCapture;
	std::mutex mutex;
	std::condition_variable condition;
	std::optional<Task> pending;
	std::vector<FileDialogPreviewResult> ready;
	std::atomic<std::uint64_t> generation{0};
	std::atomic<bool> stopping{false};
	std::thread worker;
};

FileDialogPreviewLoader::FileDialogPreviewLoader(Processor processor,
	SourceCapture sourceCapture)
	: impl_(std::make_unique<Impl>(std::move(processor), std::move(sourceCapture))) {}
FileDialogPreviewLoader::~FileDialogPreviewLoader() = default;

std::uint64_t FileDialogPreviewLoader::Request(const std::filesystem::path& path,
	bool directory, FileDialogSortMode mode, int maximumWidth, int maximumHeight,
	SourceDescriptor source) {
	const std::uint64_t requestedGeneration = impl_->generation.fetch_add(1) + 1;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->pending) {
			RecordPreviewCancellation(impl_->pending->generation,
				PreviewCancellationReason::PendingReplaced, PerfExecution::EventThread);
			impl_->pending.reset();
		}
		for (const FileDialogPreviewResult& result : impl_->ready) {
			RecordPreviewCancellation(result.generation,
				PreviewCancellationReason::ReadyReplaced, PerfExecution::EventThread);
		}
		impl_->ready.clear();
		if (!path.empty() && maximumWidth > 0 && maximumHeight > 0) {
			impl_->StartWorkerLocked();
			impl_->pending = Impl::Task{path, directory, mode,
				maximumWidth, maximumHeight, requestedGeneration, std::move(source)};
		}
	}
	impl_->condition.notify_one();
	SourceWorkCoordinator::Global().NotifyWaiters();
	return requestedGeneration;
}

void FileDialogPreviewLoader::Clear() {
	(void)Request({}, false, FileDialogSortMode::Name, 0, 0);
}

std::vector<FileDialogPreviewResult> FileDialogPreviewLoader::TakeReady() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<FileDialogPreviewResult> results;
	results.swap(impl_->ready);
	return results;
}

} // namespace jpegview_linux
