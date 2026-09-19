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
