#include "file_dialog_model.h"
#include "archive_source.h"
#include "image_formats.h"
#include "image_decoder.h"
#include "thumbnail_resampler.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace jpegview_linux {
namespace {

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

template<typename Continue>
DirectorySummary CountImmediateDirectoryContentsWhile(
	const std::filesystem::path& directory, Continue shouldContinue) {
	DirectorySummary summary;
	if (IsArchiveLocation(directory)) {
		std::vector<ArchiveEntryInfo> entries;
		std::string errorMessage;
		if (!ListArchiveDirectoryCancellable(directory, entries,
			[&shouldContinue] { return shouldContinue(); }, errorMessage)) return summary;
		for (const ArchiveEntryInfo& entry : entries) {
			if (!shouldContinue()) break;
			if (entry.directory) ++summary.subdirectoryCount;
			else if (IsSupportedImagePath(entry.path)) ++summary.imageCount;
		}
		return summary;
	}
	std::error_code iteratorError;
	std::filesystem::directory_iterator iterator(directory, iteratorError);
	const std::filesystem::directory_iterator end;
	while (!iteratorError && iterator != end && shouldContinue()) {
		std::error_code statusError;
		if (iterator->is_directory(statusError)) {
			if (!statusError) ++summary.subdirectoryCount;
		} else if (!statusError && iterator->is_regular_file(statusError) && !statusError) {
			if (IsArchiveContainerName(iterator->path())) {
				++summary.subdirectoryCount;
			} else if (IsSupportedImagePath(iterator->path())) {
				++summary.imageCount;
			}
		}
		iterator.increment(iteratorError);
	}
	return summary;
}

std::filesystem::path FirstImageInDirectoryWhile(
	const std::filesystem::path& directory, FileDialogSortMode mode,
	const std::function<bool()>& shouldContinue, std::string* errorMessage = nullptr,
	ArchiveErrorKind* errorKind = nullptr, bool* containsEncryptedEntries = nullptr) {
	std::vector<FileDialogEntry> images;
	if (IsArchiveLocation(directory)) {
		const std::string archiveFormat = ArchiveFormatName(directory);
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
				std::error_code modificationError;
				const std::filesystem::file_time_type modificationTime =
					ImageSourceModificationTime(entry.path, modificationError);
				images.push_back({entry.path, false, false,
					modificationError ? std::filesystem::file_time_type{} : modificationTime,
					false, true, entry.encrypted});
			}
		}
		SortFileDialogEntries(images, mode);
		if (errorMessage != nullptr) errorMessage->clear();
		if (errorKind != nullptr) *errorKind = ArchiveErrorKind::None;
		if (promptsForArchivePassword && !images.empty() && images.front().encrypted &&
			!HasSessionArchivePassword(directory)) {
			if (errorMessage != nullptr) {
				*errorMessage = "password required for encrypted archive image";
			}
			if (errorKind != nullptr) *errorKind = ArchiveErrorKind::PasswordRequired;
			return {};
		}
		return images.empty() ? std::filesystem::path{} : images.front().path;
	}
	std::error_code iteratorError;
	std::filesystem::directory_iterator iterator(directory, iteratorError);
	const std::filesystem::directory_iterator end;
	while (!iteratorError && iterator != end && shouldContinue()) {
		std::error_code statusError;
		if (iterator->is_regular_file(statusError) && !statusError &&
			IsSupportedImagePath(iterator->path())) {
			std::error_code modificationError;
			const std::filesystem::file_time_type modificationTime =
				iterator->last_write_time(modificationError);
			images.push_back({iterator->path(), false, false,
				modificationError ? std::filesystem::file_time_type{} : modificationTime});
		}
		iterator.increment(iteratorError);
	}
	if (!shouldContinue()) return {};
	SortFileDialogEntries(images, mode);
	return images.empty() ? std::filesystem::path{} : images.front().path;
}

} // namespace

void SortFileDialogEntries(std::vector<FileDialogEntry>& entries, FileDialogSortMode mode) {
	std::sort(entries.begin(), entries.end(), [mode](const FileDialogEntry& left, const FileDialogEntry& right) {
		if (left.parent != right.parent) return left.parent;
		if (left.directory != right.directory) return left.directory;
		if (mode == FileDialogSortMode::ModificationDate &&
			left.modificationTime != right.modificationTime) {
			return left.modificationTime > right.modificationTime;
		}
		const std::string leftName = Lower(left.path.filename().string());
		const std::string rightName = Lower(right.path.filename().string());
		return leftName == rightName ? left.path.string() < right.path.string() : leftName < rightName;
	});
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
		std::max(1, paneHeight - 82),
	};
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
	entries_.clear();
	selected_ = -1;
	scroll_ = 0;
	matchFullPath_ = false;
}

void FileDialogModel::Clear() {
	Begin(false);
}

void FileDialogModel::SetEntries(std::vector<FileDialogEntry> entries) {
	allEntries_ = std::move(entries);
	matchFullPath_ = false;
	SortFileDialogEntries(allEntries_, saveDialog_ ? FileDialogSortMode::Name : sortMode_);
	ApplyFilter();
}

void FileDialogModel::SetEntriesInOrder(std::vector<FileDialogEntry> entries, bool matchFullPath) {
	allEntries_ = std::move(entries);
	matchFullPath_ = matchFullPath;
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
	SortFileDialogEntries(allEntries_, sortMode_);
	ApplyFilter();
	if (!selectedPath.empty()) Focus(selectedPath, visibleRows);
}

void FileDialogModel::MoveSelection(int direction, int visibleRows) {
	if (entries_.empty()) return;
	selected_ = std::clamp(selected_ + direction, 0, static_cast<int>(entries_.size()) - 1);
	EnsureSelectionVisible(visibleRows);
}

void FileDialogModel::MoveSelectionByPage(int direction, int visibleRows) {
	MoveSelection(direction * std::max(1, visibleRows), visibleRows);
}

void FileDialogModel::ScrollBy(int rows, int visibleRows) {
	visibleRows = std::max(1, visibleRows);
	const int maximumScroll = std::max(0, static_cast<int>(entries_.size()) - visibleRows);
	const std::int64_t nextScroll = static_cast<std::int64_t>(scroll_) + rows;
	scroll_ = static_cast<int>(std::clamp<std::int64_t>(nextScroll, 0, maximumScroll));
}

void FileDialogModel::ScrollTo(int rows, int visibleRows) {
	visibleRows = std::max(1, visibleRows);
	const int maximumScroll = std::max(0, static_cast<int>(entries_.size()) - visibleRows);
	scroll_ = std::clamp(rows, 0, maximumScroll);
}

void FileDialogModel::SelectFirst(int visibleRows) {
	Select(0, visibleRows);
}

void FileDialogModel::SelectLast(int visibleRows) {
	Select(static_cast<int>(entries_.size()) - 1, visibleRows);
}

void FileDialogModel::Select(int index, int visibleRows) {
	if (index < 0 || index >= static_cast<int>(entries_.size())) return;
	selected_ = index;
	EnsureSelectionVisible(visibleRows);
}

bool FileDialogModel::Focus(const std::filesystem::path& path, int visibleRows) {
	const auto entry = std::find_if(entries_.begin(), entries_.end(), [&path](const FileDialogEntry& candidate) {
		return candidate.path == path;
	});
	if (entry == entries_.end()) return false;
	Select(static_cast<int>(std::distance(entries_.begin(), entry)), visibleRows);
	return true;
}

bool FileDialogModel::MarkEncrypted(const std::filesystem::path& path) {
	bool marked = false;
	const std::filesystem::path normalizedPath = path.lexically_normal();
	for (FileDialogEntry& entry : allEntries_) {
		if (entry.path.lexically_normal() == normalizedPath) {
			entry.encrypted = true;
			marked = true;
		}
	}
	for (FileDialogEntry& entry : entries_) {
		if (entry.path.lexically_normal() == normalizedPath) entry.encrypted = true;
	}
	return marked;
}

bool FileDialogModel::SetFileSize(const std::filesystem::path& path, std::uintmax_t size) {
	bool updated = false;
	const std::filesystem::path normalizedPath = path.lexically_normal();
	const auto update = [&normalizedPath, size, &updated](FileDialogEntry& entry) {
		if (entry.path.lexically_normal() == normalizedPath) {
			entry.fileSize = size;
			entry.fileSizeKnown = true;
			updated = true;
		}
	};
	for (FileDialogEntry& entry : allEntries_) update(entry);
	for (FileDialogEntry& entry : entries_) update(entry);
	return updated;
}

void FileDialogModel::ClearSelection() {
	selected_ = -1;
}

const FileDialogEntry* FileDialogModel::SelectedEntry() const {
	return selected_ >= 0 && selected_ < static_cast<int>(entries_.size()) ? &entries_[selected_] : nullptr;
}

void FileDialogModel::ApplyFilter() {
	entries_ = FilterFileDialogEntries(allEntries_,
		saveDialog_ ? std::string_view{} : std::string_view(filter_), matchFullPath_);
	scroll_ = 0;
	if (entries_.empty()) {
		selected_ = -1;
		return;
	}
	selected_ = 0;
	if (saveDialog_) return;
	const auto firstChild = std::find_if(entries_.begin(), entries_.end(), [](const FileDialogEntry& entry) {
		return !entry.parent;
	});
	if (firstChild != entries_.end()) {
		selected_ = static_cast<int>(std::distance(entries_.begin(), firstChild));
	} else if (!filter_.empty()) {
		selected_ = -1;
	}
}

void FileDialogModel::EnsureSelectionVisible(int visibleRows) {
	visibleRows = std::max(1, visibleRows);
	if (selected_ < scroll_) scroll_ = selected_;
	if (selected_ >= scroll_ + visibleRows) scroll_ = selected_ - visibleRows + 1;
	const int maximumScroll = std::max(0, static_cast<int>(entries_.size()) - visibleRows);
	scroll_ = std::clamp(scroll_, 0, maximumScroll);
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
			if (task.validatePassword) {
				(void)ValidateArchivePassword(task.directory, task.password, result.error,
					&result.errorKind);
				std::fill(task.password.begin(), task.password.end(), '\0');
			} else {
				const auto isCurrent = [this, generation = task.generation] {
					return !stopping.load() && currentGeneration.load() == generation;
				};
				(void)ListArchiveDirectoryCancellable(task.directory, result.entries,
					isCurrent, result.error, &result.errorKind,
					&result.containsEncryptedEntries);
			}
			const auto isCurrent = [this, generation = task.generation] {
				return !stopping.load() && currentGeneration.load() == generation;
			};
			if (!isCurrent()) continue;

			std::lock_guard<std::mutex> lock(mutex);
			if (isCurrent()) ready.push_back(std::move(result));
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
	impl_->condition.notify_one();
}

void ArchiveDirectoryLoader::Clear(std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->pending.clear();
	impl_->ready.clear();
}

std::vector<ArchiveDirectoryResult> ArchiveDirectoryLoader::TakeReady() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<ArchiveDirectoryResult> results;
	results.swap(impl_->ready);
	return results;
}

struct FileDialogFileSizeLoader::Impl {
	struct Task {
		std::vector<std::filesystem::path> paths;
		std::uint64_t generation = 0;
	};

	Impl() = default;

	~Impl() {
		stopping.store(true);
		condition.notify_one();
		if (worker.joinable()) worker.join();
	}

	bool IsCurrent(std::uint64_t requestedGeneration) const {
		return !stopping.load() && currentGeneration.load() == requestedGeneration;
	}

	void Publish(std::vector<FileDialogFileSizeResult>& batch, std::uint64_t requestedGeneration) {
		if (batch.empty() || !IsCurrent(requestedGeneration)) return;
		std::lock_guard<std::mutex> lock(mutex);
		if (IsCurrent(requestedGeneration)) {
			ready.insert(ready.end(), std::make_move_iterator(batch.begin()),
				std::make_move_iterator(batch.end()));
		}
		batch.clear();
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
			for (const std::filesystem::path& path : task.paths) {
				if (!IsCurrent(task.generation)) break;
				std::error_code error;
				const std::uintmax_t size = ImageSourceFileSize(path, error);
				if (!error) {
					batch.push_back(FileDialogFileSizeResult{task.generation, path, size});
				}
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
	std::optional<Task> pending;
	std::vector<FileDialogFileSizeResult> ready;
	std::atomic<std::uint64_t> currentGeneration{0};
	std::atomic<bool> stopping{false};
	std::thread worker;
};

FileDialogFileSizeLoader::FileDialogFileSizeLoader() : impl_(std::make_unique<Impl>()) {}
FileDialogFileSizeLoader::~FileDialogFileSizeLoader() = default;

void FileDialogFileSizeLoader::Request(
	const std::vector<std::filesystem::path>& paths, std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->pending.reset();
		impl_->ready.clear();
		if (!paths.empty()) {
			impl_->StartWorkerLocked();
			impl_->pending = Impl::Task{paths, generation};
		}
	}
	if (!paths.empty()) impl_->condition.notify_one();
}

void FileDialogFileSizeLoader::Clear(std::uint64_t generation) {
	impl_->currentGeneration.store(generation);
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->pending.reset();
	impl_->ready.clear();
}

std::vector<FileDialogFileSizeResult> FileDialogFileSizeLoader::TakeReady() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<FileDialogFileSizeResult> results;
	results.swap(impl_->ready);
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
		condition.notify_one();
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

			const DirectorySummary summary = CountImmediateDirectoryContentsWhile(task.directory,
				[this, generation = task.generation] {
					return !stopping.load() && currentGeneration.load() == generation;
				});
			if (stopping.load() || currentGeneration.load() != task.generation) continue;

			std::lock_guard<std::mutex> lock(mutex);
			if (currentGeneration.load() == task.generation) {
				ready.push_back(DirectorySummaryResult{task.directory, task.generation, summary});
			}
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::deque<Task> pending;
	std::vector<DirectorySummaryResult> ready;
	std::atomic<std::uint64_t> currentGeneration{0};
	std::atomic<bool> stopping{false};
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
	if (!directories.empty()) impl_->condition.notify_one();
}

std::vector<DirectorySummaryResult> DirectorySummaryLoader::TakeReady() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<DirectorySummaryResult> results;
	results.swap(impl_->ready);
	return results;
}

struct FileDialogPreviewLoader::Impl {
	struct Task {
		std::filesystem::path path;
		bool directory = false;
		FileDialogSortMode mode = FileDialogSortMode::Name;
		int maximumWidth = 0;
		int maximumHeight = 0;
		std::uint64_t generation = 0;
	};

	Impl() = default;

	~Impl() {
		stopping.store(true);
		condition.notify_one();
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

			FileDialogPreviewResult result;
			result.generation = task.generation;
			try {
				result.source = task.directory ? FirstImageInDirectoryWhile(task.path, task.mode,
					[this, &task] { return IsCurrent(task.generation); },
					&result.error, &result.errorKind, &result.encryptedArchive) : task.path;
				if (task.directory && result.source.empty() &&
					result.errorKind == ArchiveErrorKind::PasswordRequired) {
					result.source = task.path;
				}
				if (IsCurrent(task.generation)) {
					if (!result.error.empty()) {
						// Preserve archive errors for the UI; previews never request credentials.
					} else if (result.source.empty()) {
						result.error = "No images in this folder";
					} else {
						std::error_code sizeError;
						result.fileSize = ImageSourceFileSize(result.source, sizeError);
						result.fileSizeKnown = !sizeError;
						DecodedImage decoded;
						bool success = false;
						if (IsJpegPath(result.source)) {
							success = DecodeJpegForDisplay(result.source, task.maximumWidth,
								task.maximumHeight, decoded, result.sourceWidth,
								result.sourceHeight, result.error);
						} else {
							success = DecodeImage(result.source, decoded, result.error);
						}
						if (success && !decoded.frames.empty()) {
							DecodedFrame frame = std::move(decoded.frames.front());
							if (result.sourceWidth <= 0 || result.sourceHeight <= 0) {
								result.sourceWidth = frame.width;
								result.sourceHeight = frame.height;
							}
							result.hasTransparency = frame.hasTransparency;
							const double scale = std::min({1.0,
								static_cast<double>(task.maximumWidth) / frame.width,
								static_cast<double>(task.maximumHeight) / frame.height});
							const int width = std::max(1,
								static_cast<int>(std::floor(frame.width * scale + 0.5)));
							const int height = std::max(1,
								static_cast<int>(std::floor(frame.height * scale + 0.5)));
							if (!DownsampleThumbnailBgra(frame.bgra, frame.width, frame.height,
								width, height, result.bgra)) {
								result.error = "Cannot resize preview";
							} else {
								result.width = width;
								result.height = height;
							}
						} else if (success) {
							result.error = "Image has no preview frame";
						}
					}
				}
			} catch (const std::exception& error) {
				result.error = error.what();
			}
			if (!IsCurrent(task.generation)) continue;

			std::lock_guard<std::mutex> lock(mutex);
			if (IsCurrent(task.generation)) {
				ready.clear();
				ready.push_back(std::move(result));
			}
		}
	}

	void StartWorkerLocked() {
		if (!worker.joinable()) worker = std::thread([this] { Run(); });
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::optional<Task> pending;
	std::vector<FileDialogPreviewResult> ready;
	std::atomic<std::uint64_t> generation{0};
	std::atomic<bool> stopping{false};
	std::thread worker;
};

FileDialogPreviewLoader::FileDialogPreviewLoader() : impl_(std::make_unique<Impl>()) {}
FileDialogPreviewLoader::~FileDialogPreviewLoader() = default;

std::uint64_t FileDialogPreviewLoader::Request(const std::filesystem::path& path,
	bool directory, FileDialogSortMode mode, int maximumWidth, int maximumHeight) {
	const std::uint64_t requestedGeneration = impl_->generation.fetch_add(1) + 1;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->ready.clear();
		impl_->pending.reset();
		if (!path.empty() && maximumWidth > 0 && maximumHeight > 0) {
			impl_->StartWorkerLocked();
			impl_->pending = Impl::Task{path, directory, mode,
				maximumWidth, maximumHeight, requestedGeneration};
		}
	}
	impl_->condition.notify_one();
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
