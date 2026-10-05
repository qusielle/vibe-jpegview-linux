#pragma once

#include "batch_copy.h"
#include "cache_budget.h"
#include "desktop_applications.h"
#include "external_commands.h"
#include "image.h"
#include "image_writer.h"
#include "work_context.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace jpegview_linux {

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
namespace detail {
using LosslessCropPublicationTestHook = void (*)(void*);
using ImageSavePublicationTestHook = void (*)(void*);
using BatchRenameBeforeMoveTestHook = void (*)(
	const std::filesystem::path&, const std::filesystem::path&, bool*, void*);
} // namespace detail
void SetLosslessCropPublicationTestHookForTesting(
	detail::LosslessCropPublicationTestHook hook, void* context);
void SetImageSavePublicationTestHookForTesting(
	detail::ImageSavePublicationTestHook hook, void* context);
void SetBatchRenameBeforeMoveTestHookForTesting(
	detail::BatchRenameBeforeMoveTestHook hook, void* context);
#endif

struct SaveImageOperation {
	std::filesystem::path output;
	// Set when the viewer captured pixels because this output resolves to the
	// selected source. The worker verifies the resolved destination again before
	// reporting that the selected source was replaced.
	std::filesystem::path selectedSourcePath;
	std::shared_ptr<const Image> image;
	CacheReservation imageReservation;
	ImageWriteOptions options;
	bool overwriteConfirmed = false;
};

struct CopyImageOperation {
	std::shared_ptr<const Image> image;
	CacheReservation imageReservation;
};

struct PasteImageOperation {};

struct LosslessTransformOperation {
	std::filesystem::path source;
	LosslessJpegOperation transform = LosslessJpegOperation::Rotate90;
};

struct LosslessCropOperation {
	std::filesystem::path source;
	std::filesystem::path output;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	bool overwriteConfirmed = false;
};

struct BatchCopyOperation {
	std::vector<BatchCopyItem> items;
	std::filesystem::path preferredCurrentPath;
};

struct SetModificationTimeOperation {
	std::filesystem::path path;
	std::time_t timestamp = 0;
};

struct TouchFolderExifDatesOperation {
	std::filesystem::path directory;
};

struct PrintImageOperation {
	std::shared_ptr<const Image> image;
	CacheReservation imageReservation;
	ImageWriteOptions options;
};

struct WallpaperImageOperation {
	std::shared_ptr<const Image> image;
	CacheReservation imageReservation;
	std::filesystem::path cacheDirectory;
	ImageWriteOptions options;
};

struct WallpaperFileOperation {
	std::filesystem::path image;
};

struct LaunchDesktopOperation {
	std::vector<ExternalCommand> fallbacks;
};

struct MoveToTrashOperation {
	std::filesystem::path path;
	std::vector<ExternalCommand> commands;
	bool allowPermanentFallback = true;
};

struct RegisterDefaultViewerOperation {
	std::filesystem::path executable;
	std::filesystem::path dataHome;
	std::filesystem::path configHome;
};

struct RemoveTemporaryFilesOperation {
	std::filesystem::path file;
	std::filesystem::path directory;
};

using FileOperationPayload = std::variant<SaveImageOperation, CopyImageOperation,
	PasteImageOperation, LosslessTransformOperation, LosslessCropOperation,
	BatchCopyOperation, SetModificationTimeOperation, TouchFolderExifDatesOperation,
	PrintImageOperation, WallpaperImageOperation, WallpaperFileOperation,
	LaunchDesktopOperation, MoveToTrashOperation, RegisterDefaultViewerOperation,
	RemoveTemporaryFilesOperation>;

enum class FileOperationKind {
	SaveImage,
	CopyImage,
	PasteImage,
	LosslessTransform,
	LosslessCrop,
	BatchCopy,
	SetModificationTime,
	TouchFolderExifDates,
	PrintImage,
	WallpaperImage,
	WallpaperFile,
	LaunchDesktop,
	MoveToTrash,
	RegisterDefaultViewer,
	RemoveTemporaryFiles,
};

struct BatchCopySummary {
	std::size_t renamed = 0;
	std::size_t copied = 0;
	std::size_t createdDirectories = 0;
	std::size_t failed = 0;
	std::size_t completed = 0;
	std::filesystem::path preferredCurrentPath;
	std::string firstFailure;
};

struct FileOperationResult {
	std::uint64_t id = 0;
	std::uint64_t ownerGeneration = 0;
	FileOperationKind kind = FileOperationKind::SaveImage;
	bool success = false;
	bool cancelled = false;
	bool replacedSelectedSource = false;
	bool irreversibleCommandFinished = false;
	std::string message;
	std::filesystem::path path;
	std::filesystem::path temporaryFile;
	std::filesystem::path temporaryDirectory;
	std::size_t updatedFiles = 0;
	BatchCopySummary batch;
	std::optional<WorkerFailure> failure;
	std::int64_t detachedChild = -1;
};

FileOperationKind KindOf(const FileOperationPayload& payload);
bool ParseLocalExifTimestamp(const std::string& value, std::time_t& result);
FileOperationResult ExecuteFileOperation(const FileOperationPayload& payload,
	std::uint64_t ownerGeneration,
	const std::function<bool()>& shouldContinue);

class FileOperationService {
public:
	FileOperationService();
	~FileOperationService();
	FileOperationService(const FileOperationService&) = delete;
	FileOperationService& operator=(const FileOperationService&) = delete;

	std::uint64_t Request(FileOperationPayload payload,
		std::uint64_t ownerGeneration = 0);
	void Cancel(std::uint64_t id = 0);
	std::optional<FileOperationResult> TakeReady();
	bool Busy() const;
	bool WaitUntilIdle(std::chrono::milliseconds timeout);
	void Stop();

private:
	struct Work {
		std::uint64_t id = 0;
		std::uint64_t ownerGeneration = 0;
		FileOperationPayload payload;
		std::shared_ptr<std::atomic<bool>> cancelled;
	};

	void Run();
	void ReapDetachedChildren();

	mutable std::mutex mutex_;
	std::condition_variable available_;
	std::condition_variable idle_;
	std::thread worker_;
	std::optional<Work> pending_;
	std::optional<FileOperationResult> ready_;
	std::shared_ptr<std::atomic<bool>> activeCancellation_;
	std::vector<pid_t> detachedChildren_;
	std::uint64_t generation_ = 0;
	std::uint64_t activeId_ = 0;
	bool active_ = false;
	bool stopping_ = false;
};

} // namespace jpegview_linux
