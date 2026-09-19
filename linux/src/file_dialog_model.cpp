#include "file_dialog_model.h"
#include "image_formats.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <mutex>
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
	std::error_code iteratorError;
	std::filesystem::directory_iterator iterator(directory, iteratorError);
	const std::filesystem::directory_iterator end;
	while (!iteratorError && iterator != end && shouldContinue()) {
		std::error_code statusError;
		if (iterator->is_directory(statusError)) {
			if (!statusError) ++summary.subdirectoryCount;
		} else if (!statusError && iterator->is_regular_file(statusError) && !statusError &&
			IsSupportedImagePath(iterator->path())) {
			++summary.imageCount;
		}
		iterator.increment(iteratorError);
	}
	return summary;
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
	const std::vector<FileDialogEntry>& entries, std::string_view filter) {
	if (filter.empty()) return entries;
	const std::string loweredFilter = Lower(std::string(filter));
	std::vector<FileDialogEntry> filtered;
	filtered.reserve(entries.size());
	for (const FileDialogEntry& entry : entries) {
		if (entry.parent || Lower(entry.path.filename().string()).find(loweredFilter) != std::string::npos) {
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

void FileDialogModel::Begin(bool saveDialog) {
	saveDialog_ = saveDialog;
	filter_.clear();
	allEntries_.clear();
	entries_.clear();
	selected_ = -1;
	scroll_ = 0;
}

void FileDialogModel::Clear() {
	Begin(false);
}

void FileDialogModel::SetEntries(std::vector<FileDialogEntry> entries) {
	allEntries_ = std::move(entries);
	SortFileDialogEntries(allEntries_, saveDialog_ ? FileDialogSortMode::Name : sortMode_);
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

void FileDialogModel::ClearSelection() {
	selected_ = -1;
}

const FileDialogEntry* FileDialogModel::SelectedEntry() const {
	return selected_ >= 0 && selected_ < static_cast<int>(entries_.size()) ? &entries_[selected_] : nullptr;
}

void FileDialogModel::ApplyFilter() {
	entries_ = FilterFileDialogEntries(allEntries_, saveDialog_ ? std::string_view{} : std::string_view(filter_));
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

struct DirectorySummaryLoader::Impl {
	struct Task {
		std::filesystem::path directory;
		std::uint64_t generation = 0;
	};

	Impl() : worker([this] { Run(); }) {}

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
		for (const std::filesystem::path& directory : directories) {
			impl_->pending.push_back(Impl::Task{directory, generation});
		}
	}
	impl_->condition.notify_one();
}

std::vector<DirectorySummaryResult> DirectorySummaryLoader::TakeReady() {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	std::vector<DirectorySummaryResult> results;
	results.swap(impl_->ready);
	return results;
}

} // namespace jpegview_linux
