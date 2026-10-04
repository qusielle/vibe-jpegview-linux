#include "file_operation_service.h"

#include "clipboard.h"
#include "desktop_association.h"
#include "exif_reader.h"
#include "external_process.h"
#include "image_formats.h"
#include "event_loop_model.h"
#include "perf_diagnostics.h"
#include "source_work_coordinator.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>

namespace fs = std::filesystem;

namespace jpegview_linux {
namespace {

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
std::mutex losslessCropPublicationTestHookMutex;
detail::LosslessCropPublicationTestHook losslessCropPublicationTestHook = nullptr;
void* losslessCropPublicationTestHookContext = nullptr;
std::mutex imageSavePublicationTestHookMutex;
detail::ImageSavePublicationTestHook imageSavePublicationTestHook = nullptr;
void* imageSavePublicationTestHookContext = nullptr;

void InvokeLosslessCropPublicationTestHook() {
	detail::LosslessCropPublicationTestHook hook = nullptr;
	void* context = nullptr;
	{
		std::lock_guard<std::mutex> lock(losslessCropPublicationTestHookMutex);
		hook = losslessCropPublicationTestHook;
		context = losslessCropPublicationTestHookContext;
	}
	if (hook != nullptr) hook(context);
}

void InvokeImageSavePublicationTestHook() {
	detail::ImageSavePublicationTestHook hook = nullptr;
	void* context = nullptr;
	{
		std::lock_guard<std::mutex> lock(imageSavePublicationTestHookMutex);
		hook = imageSavePublicationTestHook;
		context = imageSavePublicationTestHookContext;
	}
	if (hook != nullptr) hook(context);
}
#endif

bool Continue(const std::function<bool()>& shouldContinue) {
	if (!shouldContinue) return true;
	try {
		return shouldContinue();
	} catch (...) {
		return false;
	}
}

FileOperationResult NewResult(FileOperationKind kind, std::uint64_t ownerGeneration) {
	FileOperationResult result;
	result.kind = kind;
	result.ownerGeneration = ownerGeneration;
	return result;
}

void Fail(FileOperationResult& result, const std::string& message,
	const std::function<bool()>& shouldContinue) {
	result.success = false;
	result.cancelled = !result.irreversibleCommandFinished &&
		!Continue(shouldContinue);
	result.message = message;
	result.failure = WorkerFailure{result.cancelled ? WorkerFailureKind::Cancelled :
		WorkerFailureKind::ProcessingFailed, message};
}

bool SetModificationTime(const fs::path& path, std::time_t timestamp) {
	const timespec times[2] = {{0, UTIME_OMIT}, {timestamp, 0}};
	return utimensat(AT_FDCWD, path.c_str(), times, 0) == 0;
}

bool AcquireSource(const fs::path& path, SourceWorkPriority priority,
	const std::function<bool()>& shouldContinue, SourceWorkLease& lease) {
	WorkContext context = MakePathWorkContext(path, priority, shouldContinue);
	lease = SourceWorkCoordinator::Global().Acquire(context, path);
	return static_cast<bool>(lease);
}

bool AcquireSourceAndCpu(const fs::path& path, SourceWorkPriority priority,
	const std::function<bool()>& shouldContinue, WorkContext& context,
	SourceCpuWorkLease& lease) {
	context = MakePathWorkContext(path, priority, shouldContinue);
	lease = SourceWorkCoordinator::Global().AcquireSourceAndCpu(context, path);
	if (!lease) return false;
	context.sourceAccessAlreadyAdmitted = true;
	context.cpuProcessingAlreadyAdmitted = true;
	return true;
}

bool IsJpegPath(const fs::path& path) {
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return extension == ".jpg" || extension == ".jpeg" || extension == ".jpe";
}

bool CreateTemporarySibling(const fs::path& target, const std::string& purpose,
	const std::string& extension, fs::path& temporary, std::string& errorMessage,
	mode_t* defaultPermissions = nullptr) {
	static std::atomic<std::uint64_t> sequence{0};
	const fs::path directory = target.parent_path().empty() ? fs::path(".") :
		target.parent_path();
	for (int attempt = 0; attempt < 128; ++attempt) {
		const std::uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
		fs::path candidate = directory /
			(".jpegview-" + purpose + "-" + std::to_string(::getpid()) + "-" +
				std::to_string(serial) + extension);
		const int descriptor = ::open(candidate.c_str(),
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
		if (descriptor >= 0) {
			int savedError = 0;
			struct stat status{};
			if (::fstat(descriptor, &status) != 0) {
				savedError = errno;
			} else {
				if (defaultPermissions != nullptr) {
					*defaultPermissions = status.st_mode & 0777;
				}
				if (::fchmod(descriptor, 0600) != 0) savedError = errno;
			}
			if (::close(descriptor) != 0 && savedError == 0) savedError = errno;
			if (savedError != 0) {
				std::error_code removeError;
				fs::remove(candidate, removeError);
				errorMessage = "cannot prepare temporary output: " +
					std::string(std::strerror(savedError));
				return false;
			}
			temporary.swap(candidate);
			return true;
		}
		if (errno != EEXIST) {
			errorMessage = "cannot create temporary output: " +
				std::string(std::strerror(errno));
			return false;
		}
	}
	errorMessage = "cannot allocate a unique temporary output name";
	return false;
}

class ScopedTemporaryDirectoryCreation {
public:
	explicit ScopedTemporaryDirectoryCreation(const char* path) : path_(path) {}
	~ScopedTemporaryDirectoryCreation() {
		if (path_ != nullptr) (void)::rmdir(path_);
	}
	ScopedTemporaryDirectoryCreation(const ScopedTemporaryDirectoryCreation&) = delete;
	ScopedTemporaryDirectoryCreation& operator=(
		const ScopedTemporaryDirectoryCreation&) = delete;
	void Release() { path_ = nullptr; }

private:
	const char* path_ = nullptr;
};

void RemoveFile(const fs::path& path) {
	if (path.empty()) return;
	std::error_code ignored;
	fs::remove(path, ignored);
}

void RemoveTree(const fs::path& path) {
	if (path.empty()) return;
	std::error_code ignored;
	fs::remove_all(path, ignored);
}

class ScopedTemporaryOutputs {
public:
	explicit ScopedTemporaryOutputs(FileOperationResult& result) : result_(result) {}
	~ScopedTemporaryOutputs() { Remove(); }
	ScopedTemporaryOutputs(const ScopedTemporaryOutputs&) = delete;
	ScopedTemporaryOutputs& operator=(const ScopedTemporaryOutputs&) = delete;

	void Release() { released_ = true; }
	void Remove() {
		if (released_) return;
		RemoveFile(result_.temporaryFile);
		RemoveTree(result_.temporaryDirectory);
		result_.temporaryFile.clear();
		result_.temporaryDirectory.clear();
		released_ = true;
	}

private:
	FileOperationResult& result_;
	bool released_ = false;
};

bool RemoveTemporaryPathsWithSourceAdmission(const fs::path& file,
	const fs::path& directory, std::string& errorMessage) {
	const fs::path admittedPath = !file.empty() ? file : directory;
	SourceWorkLease source;
	if (!admittedPath.empty() && !AcquireSource(admittedPath,
		SourceWorkPriority::Foreground, {}, source)) {
		errorMessage = "cannot admit temporary image cleanup";
		return false;
	}
	std::error_code error;
	if (!file.empty()) fs::remove(file, error);
	if (!error && !directory.empty()) fs::remove_all(directory, error);
	if (error) {
		errorMessage = "cannot remove temporary clipboard image: " + error.message();
		return false;
	}
	return true;
}

void RemoveResultTemporaryFiles(FileOperationResult& result) noexcept {
	try {
		std::string ignoredError;
		if (RemoveTemporaryPathsWithSourceAdmission(result.temporaryFile,
			result.temporaryDirectory, ignoredError)) {
			result.temporaryFile.clear();
			result.temporaryDirectory.clear();
		}
	} catch (...) {
		// Leaving a private temporary file is safer than deleting it while a
		// coordinated source reader still owns the path.
	}
}

bool ReadPermissions(const fs::path& source, mode_t& permissions,
	std::string& errorMessage) {
	struct stat status{};
	if (::stat(source.c_str(), &status) != 0) {
		errorMessage = "cannot read source permissions";
		return false;
	}
	permissions = status.st_mode & 07777;
	return true;
}

bool ApplyPermissions(const fs::path& output, mode_t permissions,
	std::string& errorMessage) {
	if (::chmod(output.c_str(), permissions) != 0) {
		errorMessage = "cannot set output permissions";
		return false;
	}
	return true;
}

bool PublishWithoutReplacing(const fs::path& temporary, const fs::path& output,
	std::string& errorMessage) {
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
	if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
		output.c_str(), RENAME_NOREPLACE) == 0) return true;
	const int renameError = errno;
	if (renameError == EEXIST) {
		errorMessage = "output appeared before publication";
		return false;
	}
	if (renameError != ENOSYS && renameError != EINVAL &&
		renameError != EOPNOTSUPP) {
		errorMessage = "cannot publish image without replacing output: " +
			std::string(std::strerror(renameError));
		return false;
	}
#endif
	if (::link(temporary.c_str(), output.c_str()) == 0) return true;
	const int linkError = errno;
	errorMessage = linkError == EEXIST ? "output appeared before publication" :
		"cannot publish image without replacing output: " +
		std::string(std::strerror(linkError));
	return false;
}

bool ResolveOutputPath(const fs::path& output, fs::path& resolved,
	std::string& errorMessage) {
	fs::path candidate = output;
	for (int depth = 0; depth < 40; ++depth) {
		std::error_code error;
		const fs::file_status status = fs::symlink_status(candidate, error);
		if (error && error != std::errc::no_such_file_or_directory) {
			errorMessage = "cannot inspect output path: " + error.message();
			return false;
		}
		if (!error && fs::is_symlink(status)) {
			const fs::path target = fs::read_symlink(candidate, error);
			if (error) {
				errorMessage = "cannot read output symlink: " + error.message();
				return false;
			}
			candidate = (target.is_absolute() ? target :
				candidate.parent_path() / target).lexically_normal();
			continue;
		}
		error.clear();
		resolved = fs::exists(candidate, error) ?
			fs::canonical(candidate, error) : fs::weakly_canonical(candidate, error);
		if (error) {
			errorMessage = "cannot resolve output file: " + error.message();
			return false;
		}
		return true;
	}
	errorMessage = "output symlink chain is too deep";
	return false;
}

bool WriteImageWithSourceCpuAdmission(const fs::path& output, const Image& image,
	const ImageWriteOptions& options, std::string& errorMessage,
	const std::function<bool()>& shouldContinue, bool overwriteConfirmed = true,
	const fs::path& selectedSourcePath = {}, bool* replacedSelectedSource = nullptr) {
	if (replacedSelectedSource != nullptr) *replacedSelectedSource = false;
	FileOperationResult stagedOutput;
	ScopedTemporaryOutputs temporaryCleanup(stagedOutput);
	WorkContext context;
	SourceCpuWorkLease admission;
	if (!AcquireSourceAndCpu(output, SourceWorkPriority::Foreground,
		shouldContinue, context, admission) || !Continue(shouldContinue)) {
		errorMessage = "image encoding was cancelled";
		return false;
	}
	ScopedWorkContext workContext(context);
	fs::path publicationPath;
	if (!ResolveOutputPath(output, publicationPath, errorMessage)) return false;
	bool targetsSelectedSource = false;
	if (!selectedSourcePath.empty()) {
		fs::path selectedPublicationPath;
		std::string ignoredError;
		if (ResolveOutputPath(selectedSourcePath, selectedPublicationPath,
			ignoredError)) {
			targetsSelectedSource = publicationPath == selectedPublicationPath;
		}
	}
	std::error_code statusError;
	const bool targetExists = fs::exists(publicationPath, statusError);
	if (statusError) {
		errorMessage = "cannot check output file: " + statusError.message();
		return false;
	}
	if (targetExists && !overwriteConfirmed) {
		errorMessage = "image overwrite was not confirmed";
		return false;
	}

	mode_t defaultPermissions = 0;
	if (!CreateTemporarySibling(publicationPath, "image-output", ".tmp",
		stagedOutput.temporaryFile, errorMessage, &defaultPermissions)) {
		return false;
	}
	mode_t finalPermissions = defaultPermissions;
	if (targetExists && !ReadPermissions(publicationPath, finalPermissions,
		errorMessage)) {
		return false;
	}
	if (!Continue(shouldContinue)) {
		errorMessage = "image encoding was cancelled";
		return false;
	}
	if (!WriteImageWithFormat(stagedOutput.temporaryFile,
		output.extension().string(), image.bgra.data(), image.width, image.height,
		options, errorMessage)) {
		return false;
	}
	if (!Continue(shouldContinue)) {
		errorMessage = "image encoding was cancelled before publication";
		return false;
	}
	if (!ApplyPermissions(stagedOutput.temporaryFile, finalPermissions,
		errorMessage)) {
		return false;
	}
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	InvokeImageSavePublicationTestHook();
#endif
	if (targetExists) {
		if (::rename(stagedOutput.temporaryFile.c_str(), publicationPath.c_str()) != 0) {
			errorMessage = "cannot publish image: " + std::string(std::strerror(errno));
			return false;
		}
	} else if (!PublishWithoutReplacing(stagedOutput.temporaryFile,
		publicationPath, errorMessage)) {
		return false;
	}
	if (replacedSelectedSource != nullptr) {
		*replacedSelectedSource = targetsSelectedSource;
	}
	return true;
}

bool CopyPermissions(const fs::path& source, const fs::path& output,
	std::string& errorMessage) {
	mode_t permissions = 0;
	return ReadPermissions(source, permissions, errorMessage) &&
		ApplyPermissions(output, permissions, errorMessage);
}

bool CopyModificationTime(const fs::path& source, const fs::path& output,
	std::string& errorMessage) {
	std::error_code error;
	const fs::file_time_type modificationTime = fs::last_write_time(source, error);
	if (error) {
		errorMessage = "cannot read source modification time: " + error.message();
		return false;
	}
	fs::last_write_time(output, modificationTime, error);
	if (error) {
		errorMessage = "cannot preserve source modification time: " + error.message();
		return false;
	}
	return true;
}

bool RunCommands(const std::vector<ExternalCommand>& commands,
	std::string& errorMessage, const std::function<bool()>& shouldContinue,
	bool detached, std::int64_t* detachedChild = nullptr) {
	for (const ExternalCommand& command : commands) {
		if (!Continue(shouldContinue)) {
			errorMessage = "external command was cancelled";
			return false;
		}
		if (detached) {
			pid_t child = -1;
			if (StartExternalCommandDetached(command, child, errorMessage)) {
				errorMessage.clear();
				if (detachedChild != nullptr) *detachedChild = child;
				return true;
			}
		} else if (RunExternalCommand(command, errorMessage, shouldContinue)) {
			errorMessage.clear();
			return true;
		}
	}
	if (errorMessage.empty()) errorMessage = "no external command could be started";
	return false;
}

bool RunIrreversibleCommand(const ExternalCommand& command,
	std::string& errorMessage, const std::function<bool()>& shouldContinue,
	bool& commandStarted) {
	commandStarted = false;
	if (!Continue(shouldContinue)) {
		errorMessage = "external command was cancelled before launch";
		return false;
	}
	// Once launched, this command may have committed an external side effect.
	// Let it finish and report its actual exit status rather than killing it on
	// application shutdown or a later cancellation request.
	return RunExternalCommand(command, errorMessage, shouldContinue,
		ExternalCommandWaitPolicy::FinishAfterLaunch, &commandStarted);
}

bool ApplyWallpaper(const fs::path& image,
	const std::function<bool()>& shouldContinue, std::string& errorMessage,
	bool& irreversibleCommandFinished) {
	if (image.empty()) {
		errorMessage = "wallpaper source path is empty";
		return false;
	}
	WorkContext context;
	SourceWorkLease admission;
	if (!AcquireSource(image, SourceWorkPriority::Foreground,
		shouldContinue, admission)) {
		errorMessage = "wallpaper update was cancelled before reading the image";
		return false;
	}
	context = MakePathWorkContext(image, SourceWorkPriority::Foreground,
		shouldContinue);
	context.sourceAccessAlreadyAdmitted = true;
	ScopedWorkContext workContext(context);
	for (const ExternalCommandSequence& sequence : WallpaperCommandSequences(image)) {
		if (!Continue(shouldContinue)) {
			errorMessage = "wallpaper update was cancelled before launch";
			return false;
		}
		bool commandStarted = false;
		if (!RunIrreversibleCommand(sequence.required, errorMessage, shouldContinue,
			commandStarted)) {
			irreversibleCommandFinished = irreversibleCommandFinished || commandStarted;
			if (!Continue(shouldContinue)) return false;
			continue;
		}
		irreversibleCommandFinished = irreversibleCommandFinished || commandStarted;
		for (const ExternalCommand& afterSuccess : sequence.afterSuccess) {
			std::string ignoredError;
			(void)RunExternalCommand(afterSuccess, ignoredError);
		}
		return true;
	}
	if (errorMessage.empty()) errorMessage = "install gsettings, feh, or nitrogen";
	return false;
}

bool CreateTemporaryDirectory(const std::string& prefix, fs::path& directory,
	std::string& errorMessage) {
	std::vector<char> name(prefix.begin(), prefix.end());
	name.push_back('\0');
	if (::mkdtemp(name.data()) == nullptr) {
		errorMessage = "cannot create temporary directory";
		return false;
	}
	ScopedTemporaryDirectoryCreation cleanup(name.data());
	directory = name.data();
	cleanup.Release();
	return true;
}

FileOperationResult Execute(const SaveImageOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::SaveImage, ownerGeneration);
	if (!operation.image || operation.image->width <= 0 || operation.image->height <= 0 ||
		operation.output.empty()) {
		Fail(result, "invalid image output request", shouldContinue);
		return result;
	}
	std::string errorMessage;
	if (!WriteImageWithSourceCpuAdmission(operation.output, *operation.image,
		operation.options, errorMessage, shouldContinue,
		operation.overwriteConfirmed, operation.selectedSourcePath,
		&result.replacedSelectedSource)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	result.path = operation.output;
	result.success = true;
	return result;
}

FileOperationResult Execute(const CopyImageOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::CopyImage, ownerGeneration);
	if (!operation.image) {
		Fail(result, "no image pixels were provided", shouldContinue);
		return result;
	}
	std::string errorMessage;
	pid_t helper = -1;
	if (!CopyImageToClipboard(operation.image->bgra.data(), operation.image->width,
		operation.image->height, errorMessage, helper, shouldContinue)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	result.detachedChild = helper;
	result.success = true;
	return result;
}

FileOperationResult Execute(const PasteImageOperation&,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::PasteImage, ownerGeneration);
	std::vector<std::uint8_t> encodedPng;
	std::string errorMessage;
	if (!PasteImageFromClipboard(encodedPng, errorMessage, shouldContinue)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	if (!Continue(shouldContinue)) {
		Fail(result, "clipboard image paste was cancelled", shouldContinue);
		return result;
	}
	if (!CreateTemporaryDirectory("/tmp/jpegview-paste-XXXXXX",
		result.temporaryDirectory, errorMessage)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	ScopedTemporaryOutputs temporaryOutputs(result);
	result.temporaryFile = result.temporaryDirectory / "clipboard.png";
	std::ofstream output(result.temporaryFile, std::ios::binary);
	if (!output) {
		Fail(result, "cannot write temporary clipboard image", shouldContinue);
		return result;
	}
	std::size_t offset = 0;
	constexpr std::size_t batchBytes = 65536;
	while (offset < encodedPng.size()) {
		if (!Continue(shouldContinue)) {
			output.close();
			Fail(result, "clipboard image paste was cancelled", shouldContinue);
			return result;
		}
		const std::size_t count = std::min(batchBytes, encodedPng.size() - offset);
		output.write(reinterpret_cast<const char*>(encodedPng.data() + offset),
			static_cast<std::streamsize>(count));
		if (!output) {
			output.close();
			Fail(result, "cannot write temporary clipboard image", shouldContinue);
			return result;
		}
		offset += count;
	}
	output.close();
	if (!output) {
		Fail(result, "cannot finish temporary clipboard image", shouldContinue);
		return result;
	}
	result.success = true;
	temporaryOutputs.Release();
	return result;
}

FileOperationResult Execute(const LosslessTransformOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::LosslessTransform, ownerGeneration);
	if (!IsJpegPath(operation.source)) {
		Fail(result, "lossless JPEG transformation requires a JPEG image", shouldContinue);
		return result;
	}
	std::string errorMessage;
	if (!CreateTemporarySibling(operation.source, "transform", ".tmp",
		result.temporaryFile, errorMessage)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	ScopedTemporaryOutputs temporaryOutputs(result);
	WorkContext context;
	SourceCpuWorkLease admission;
	if (!AcquireSourceAndCpu(operation.source, SourceWorkPriority::Foreground,
		shouldContinue, context, admission)) {
		Fail(result, "lossless transform was cancelled before reading the source", shouldContinue);
		return result;
	}
	ScopedWorkContext workContext(context);
	const ExternalCommand command = LosslessJpegCommand(operation.transform,
		operation.source, result.temporaryFile);
	if (!RunExternalCommand(command, errorMessage, shouldContinue)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	if (!Continue(shouldContinue)) {
		Fail(result, "lossless transform was cancelled before publication", shouldContinue);
		return result;
	}
	if (!CopyPermissions(operation.source, result.temporaryFile, errorMessage)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	if (::rename(result.temporaryFile.c_str(), operation.source.c_str()) != 0) {
		Fail(result, "cannot replace original file: " + std::string(std::strerror(errno)),
			shouldContinue);
		return result;
	}
	result.path = operation.source;
	result.temporaryFile.clear();
	result.success = true;
	return result;
}

FileOperationResult Execute(const LosslessCropOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::LosslessCrop, ownerGeneration);
	if (operation.source.empty() || operation.output.empty() || operation.x < 0 ||
		operation.y < 0 || operation.width <= 0 || operation.height <= 0 ||
		!IsJpegPath(operation.source) || !IsJpegPath(operation.output)) {
		Fail(result, "invalid lossless crop request", shouldContinue);
		return result;
	}
	std::error_code existsError;
	std::string errorMessage;
	SourceWorkLease outputAdmission;
	if (!AcquireSource(operation.output, SourceWorkPriority::Foreground,
		shouldContinue, outputAdmission)) {
		Fail(result, "lossless crop was cancelled before checking its output", shouldContinue);
		return result;
	}
	const bool outputExists = fs::exists(operation.output, existsError);
	outputAdmission.Reset();
	if (existsError) {
		Fail(result, "cannot check output file: " + existsError.message(), shouldContinue);
		return result;
	}
	if (outputExists && !operation.overwriteConfirmed) {
		Fail(result, "output overwrite was not confirmed", shouldContinue);
		return result;
	}
	mode_t defaultOutputPermissions = 0;
	if (!CreateTemporarySibling(operation.output, "crop", ".tmp",
		result.temporaryFile, errorMessage, &defaultOutputPermissions)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	ScopedTemporaryOutputs temporaryOutputs(result);
	{
		WorkContext context;
		SourceCpuWorkLease admission;
		if (!AcquireSourceAndCpu(operation.source, SourceWorkPriority::Foreground,
			shouldContinue, context, admission)) {
			Fail(result, "lossless crop was cancelled before reading the source", shouldContinue);
			return result;
		}
		ScopedWorkContext workContext(context);
		const ExternalCommand command = LosslessJpegCropCommand(operation.source,
			result.temporaryFile, operation.x, operation.y,
			operation.width, operation.height);
		if (!command.Valid() || !RunExternalCommand(command, errorMessage, shouldContinue)) {
			Fail(result, errorMessage.empty() ? "invalid lossless crop geometry" : errorMessage,
				shouldContinue);
			return result;
		}
		if (!Continue(shouldContinue)) {
			Fail(result, "lossless crop was cancelled before publication", shouldContinue);
			return result;
		}
	}
#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
	InvokeLosslessCropPublicationTestHook();
#endif
	SourceWorkLease publicationAdmission;
	if (!AcquireSource(operation.output, SourceWorkPriority::Foreground,
		shouldContinue, publicationAdmission)) {
		Fail(result, "lossless crop was cancelled before output admission", shouldContinue);
		return result;
	}
	std::error_code publishStatusError;
	const bool outputExistsAtPublish = fs::exists(operation.output, publishStatusError);
	if (publishStatusError) {
		Fail(result, "cannot check output file: " + publishStatusError.message(),
			shouldContinue);
		return result;
	}
	if (outputExistsAtPublish && !operation.overwriteConfirmed) {
		Fail(result, "output overwrite was not confirmed", shouldContinue);
		return result;
	}
	mode_t finalOutputPermissions = defaultOutputPermissions;
	if (outputExistsAtPublish && !ReadPermissions(operation.output,
		finalOutputPermissions, errorMessage)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	if (!Continue(shouldContinue)) {
		Fail(result, "lossless crop was cancelled before publication", shouldContinue);
		return result;
	}
	if (!ApplyPermissions(result.temporaryFile, finalOutputPermissions, errorMessage)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	if (outputExistsAtPublish) {
		if (::rename(result.temporaryFile.c_str(), operation.output.c_str()) != 0) {
			Fail(result, "cannot finalize output: " + std::string(std::strerror(errno)),
				shouldContinue);
			return result;
		}
	} else if (!PublishWithoutReplacing(result.temporaryFile,
		operation.output, errorMessage)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	result.path = operation.output;
	result.success = true;
	return result;
}

FileOperationResult Execute(const BatchCopyOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::BatchCopy, ownerGeneration);
	result.batch.preferredCurrentPath = operation.preferredCurrentPath;
	for (const BatchCopyItem& item : operation.items) {
		if (!item.selected) continue;
		if (!Continue(shouldContinue)) {
			result.cancelled = true;
			break;
		}
		if (item.destination.empty() || item.destination == item.source) {
			++result.batch.failed;
			if (result.batch.firstFailure.empty()) {
				result.batch.firstFailure = item.source.filename().string() + " has no distinct target";
			}
			continue;
		}
		SourceWorkLease destination;
		if (!AcquireSource(item.destination, SourceWorkPriority::Foreground,
			shouldContinue, destination)) {
			result.cancelled = !Continue(shouldContinue);
			if (!result.cancelled) {
				++result.batch.failed;
				if (result.batch.firstFailure.empty()) {
					result.batch.firstFailure = "cannot check target " +
						item.destination.filename().string();
				}
			}
			if (result.cancelled) break;
			continue;
		}
		std::error_code error;
		const bool destinationExists = fs::exists(item.destination, error);
		destination.Reset();
		if (destinationExists || error) {
			++result.batch.failed;
			if (result.batch.firstFailure.empty()) {
				result.batch.firstFailure = error ?
					"cannot check target " + item.destination.filename().string() :
					item.destination.filename().string() + " already exists";
			}
			continue;
		}
		SourceWorkLease source;
		if (!AcquireSource(item.source, SourceWorkPriority::Foreground,
			shouldContinue, source)) {
			result.cancelled = !Continue(shouldContinue);
			if (!result.cancelled) {
				++result.batch.failed;
				if (result.batch.firstFailure.empty()) {
					result.batch.firstFailure = "cannot admit source " +
						item.source.filename().string();
				}
			}
			if (result.cancelled) break;
			continue;
		}
		if (item.copy) {
			const fs::path parent = item.destination.parent_path();
			if (!parent.empty()) {
				const bool created = fs::create_directories(parent, error);
				if (error) {
					++result.batch.failed;
					if (result.batch.firstFailure.empty()) {
						result.batch.firstFailure = "cannot create " + parent.string();
					}
					continue;
				}
				if (created) ++result.batch.createdDirectories;
			}
			if (!Continue(shouldContinue)) {
				result.cancelled = true;
				break;
			}
			std::string errorMessage;
			FileOperationResult temporaryOutput;
			ScopedTemporaryOutputs temporaryCleanup(temporaryOutput);
			if (!CreateTemporarySibling(item.destination, "batch-copy", ".tmp",
				temporaryOutput.temporaryFile, errorMessage)) {
				++result.batch.failed;
				if (result.batch.firstFailure.empty()) {
					result.batch.firstFailure = errorMessage;
				}
				continue;
			}
			const bool copied = fs::copy_file(item.source, temporaryOutput.temporaryFile,
				fs::copy_options::overwrite_existing, error) && !error &&
				CopyPermissions(item.source, temporaryOutput.temporaryFile, errorMessage) &&
				CopyModificationTime(item.source, temporaryOutput.temporaryFile, errorMessage);
			source.Reset();
			if (!copied) {
				++result.batch.failed;
				if (result.batch.firstFailure.empty()) {
					result.batch.firstFailure = errorMessage.empty() ?
						"cannot copy " + item.source.filename().string() : errorMessage;
				}
				continue;
			}
			SourceWorkLease destination;
			if (!AcquireSource(item.destination, SourceWorkPriority::Foreground,
				{}, destination)) {
				++result.batch.failed;
				if (result.batch.firstFailure.empty()) {
					result.batch.firstFailure = "cannot admit target " +
						item.destination.filename().string();
				}
				continue;
			}
			const bool targetExists = fs::exists(item.destination, error);
			if (targetExists || error) {
				++result.batch.failed;
				if (result.batch.firstFailure.empty()) {
					result.batch.firstFailure = error ?
						"cannot check target " + item.destination.filename().string() :
						item.destination.filename().string() + " already exists";
				}
				continue;
			}
			if (!PublishWithoutReplacing(temporaryOutput.temporaryFile,
				item.destination, errorMessage)) {
				++result.batch.failed;
				if (result.batch.firstFailure.empty()) {
					result.batch.firstFailure = errorMessage.empty() ?
						"cannot publish copy " + item.destination.filename().string() :
						errorMessage;
				}
				continue;
			}
			++result.batch.copied;
			++result.batch.completed;
		} else {
			fs::rename(item.source, item.destination, error);
			if (error) {
				++result.batch.failed;
				if (result.batch.firstFailure.empty()) {
					result.batch.firstFailure = "cannot rename " + item.source.filename().string();
				}
				continue;
			}
			if (item.source == result.batch.preferredCurrentPath) {
				result.batch.preferredCurrentPath = item.destination;
			}
			++result.batch.renamed;
			++result.batch.completed;
		}
	}
	result.cancelled = result.cancelled || !Continue(shouldContinue);
	result.success = !result.cancelled;
	if (result.cancelled) {
		result.message = "batch operation cancelled after " +
			std::to_string(result.batch.completed) + " completed item(s)";
	} else {
		result.message = "batch operation completed";
	}
	if (result.batch.failed != 0 && !result.batch.firstFailure.empty()) {
		result.message += ": " + std::to_string(result.batch.failed) + " failed: " +
			result.batch.firstFailure;
	}
	return result;
}

FileOperationResult Execute(const SetModificationTimeOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::SetModificationTime,
		ownerGeneration);
	if (operation.path.empty() || !Continue(shouldContinue)) {
		Fail(result, "file timestamp operation was cancelled or invalid", shouldContinue);
		return result;
	}
	SourceWorkLease source;
	if (!AcquireSource(operation.path, SourceWorkPriority::Foreground,
		shouldContinue, source)) {
		Fail(result, "file timestamp update was cancelled before source admission",
			shouldContinue);
		return result;
	}
	if (!SetModificationTime(operation.path, operation.timestamp)) {
		Fail(result, "cannot set file modification date: " +
			std::string(std::strerror(errno)), shouldContinue);
		return result;
	}
	result.path = operation.path;
	result.updatedFiles = 1;
	result.success = true;
	return result;
}

FileOperationResult Execute(const TouchFolderExifDatesOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::TouchFolderExifDates,
		ownerGeneration);
	std::error_code iteratorError;
	std::vector<fs::path> imagePaths;
	SourceWorkLease directoryAdmission;
	if (!AcquireSource(operation.directory, SourceWorkPriority::Metadata,
		shouldContinue, directoryAdmission)) {
		Fail(result, "EXIF date update was cancelled before directory admission",
			shouldContinue);
		return result;
	}
	{
		fs::directory_iterator iterator(operation.directory, iteratorError);
		const fs::directory_iterator end;
		if (iteratorError) {
			Fail(result, "cannot read image directory: " + iteratorError.message(),
				shouldContinue);
			directoryAdmission.Reset();
			return result;
		}
		while (iterator != end) {
			if (!Continue(shouldContinue)) {
				result.cancelled = true;
				break;
			}
			const fs::path path = iterator->path();
			if (IsSupportedImagePath(path)) imagePaths.push_back(path);
			iterator.increment(iteratorError);
			if (iteratorError) break;
		}
	}
	directoryAdmission.Reset();
	for (const fs::path& path : imagePaths) {
		if (result.cancelled || !Continue(shouldContinue)) {
			result.cancelled = true;
			break;
		}
		{
			WorkContext context;
			SourceCpuWorkLease admission;
			if (!AcquireSourceAndCpu(path, SourceWorkPriority::Metadata,
				shouldContinue, context, admission)) {
				if (!Continue(shouldContinue)) {
					result.cancelled = true;
					break;
				}
			} else {
				ScopedWorkContext workContext(context);
				std::error_code statusError;
				if (fs::is_regular_file(path, statusError) && !statusError) {
					ExifInfo info;
					std::string comment;
					if (ReadJpegMetadata(path, info, comment, context)) {
						const std::string& date = !info.acquisitionDate.empty() ?
							info.acquisitionDate : info.dateTime;
						std::time_t timestamp = 0;
						if (!date.empty() && ParseLocalExifTimestamp(date, timestamp) &&
							SetModificationTime(path, timestamp)) {
							++result.updatedFiles;
						}
					}
				}
			}
		}
	}
	result.cancelled = result.cancelled || !Continue(shouldContinue);
	result.success = !result.cancelled && !iteratorError;
	result.path = operation.directory;
	if (iteratorError) {
		result.message = "image directory scan failed: " + iteratorError.message();
	}
	if (result.cancelled) {
		result.message = "EXIF date update cancelled after " +
			std::to_string(result.updatedFiles) + " completed file(s)";
	}
	return result;
}

FileOperationResult Execute(const PrintImageOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::PrintImage, ownerGeneration);
	if (!operation.image || !Continue(shouldContinue)) {
		Fail(result, "print preparation was cancelled or image pixels are unavailable",
			shouldContinue);
		return result;
	}
	std::string errorMessage;
	if (!CreateTemporaryDirectory("/tmp/jpegview-print-XXXXXX",
		result.temporaryDirectory, errorMessage)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	ScopedTemporaryOutputs temporaryOutputs(result);
	result.temporaryFile = result.temporaryDirectory / "image.png";
	if (!WriteImageWithSourceCpuAdmission(result.temporaryFile, *operation.image,
		operation.options, errorMessage, shouldContinue)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	SourceWorkLease admission;
	if (!AcquireSource(result.temporaryFile, SourceWorkPriority::Foreground,
		shouldContinue, admission)) {
		Fail(result, "printing was cancelled before reading the prepared image",
			shouldContinue);
		return result;
	}
	WorkContext context = MakePathWorkContext(result.temporaryFile,
		SourceWorkPriority::Foreground, shouldContinue);
	context.sourceAccessAlreadyAdmitted = true;
	ScopedWorkContext workContext(context);
	bool commandStarted = false;
	if (!RunIrreversibleCommand(PrintCommand(result.temporaryFile), errorMessage,
		shouldContinue, commandStarted)) {
		result.irreversibleCommandFinished = commandStarted;
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	result.irreversibleCommandFinished = commandStarted;
	temporaryOutputs.Remove();
	result.success = true;
	return result;
}

FileOperationResult Execute(const WallpaperImageOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::WallpaperImage,
		ownerGeneration);
	if (!operation.image || operation.cacheDirectory.empty() ||
		!Continue(shouldContinue)) {
		Fail(result, "wallpaper preparation was cancelled or image pixels are unavailable",
			shouldContinue);
		return result;
	}
	std::error_code error;
	fs::create_directories(operation.cacheDirectory, error);
	if (error) {
		Fail(result, "cannot create wallpaper cache directory", shouldContinue);
		return result;
	}
	result.path = operation.cacheDirectory / "wallpaper.png";
	std::string errorMessage;
	if (!WriteImageWithSourceCpuAdmission(result.path, *operation.image,
		operation.options, errorMessage, shouldContinue)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	if (!ApplyWallpaper(result.path, shouldContinue, errorMessage,
		result.irreversibleCommandFinished)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	result.success = true;
	return result;
}

FileOperationResult Execute(const WallpaperFileOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::WallpaperFile,
		ownerGeneration);
	std::string errorMessage;
	if (!ApplyWallpaper(operation.image, shouldContinue, errorMessage,
		result.irreversibleCommandFinished)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	result.path = operation.image;
	result.success = true;
	return result;
}

FileOperationResult Execute(const LaunchDesktopOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::LaunchDesktop,
		ownerGeneration);
	std::string errorMessage;
	if (!RunCommands(operation.fallbacks, errorMessage, shouldContinue, true,
		&result.detachedChild)) {
		Fail(result, errorMessage, shouldContinue);
		return result;
	}
	result.success = true;
	return result;
}

FileOperationResult Execute(const MoveToTrashOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::MoveToTrash,
		ownerGeneration);
	if (operation.path.empty()) {
		Fail(result, "move to trash has no source path", shouldContinue);
		return result;
	}
	SourceWorkLease source;
	if (!AcquireSource(operation.path, SourceWorkPriority::Foreground,
		shouldContinue, source)) {
		Fail(result, "move to trash was cancelled before source admission", shouldContinue);
		return result;
	}
	bool helperAvailable = false;
	std::string errorMessage;
	for (const ExternalCommand& command : operation.commands) {
		if (!ExternalCommandAvailable(command.executable)) continue;
		helperAvailable = true;
		bool commandStarted = false;
		if (!RunIrreversibleCommand(command, errorMessage, shouldContinue,
			commandStarted)) {
			result.irreversibleCommandFinished =
				result.irreversibleCommandFinished || commandStarted;
			if (!Continue(shouldContinue)) {
				Fail(result, errorMessage, shouldContinue);
				return result;
			}
			continue;
		}
		result.irreversibleCommandFinished =
			result.irreversibleCommandFinished || commandStarted;
		result.success = true;
		result.path = operation.path;
		return result;
	}
	if (!helperAvailable && operation.allowPermanentFallback &&
		Continue(shouldContinue)) {
		std::error_code removeError;
		if (fs::remove(operation.path, removeError) && !removeError) {
			result.success = true;
			result.path = operation.path;
			return result;
		}
		errorMessage = removeError ? removeError.message() : "cannot remove file";
	}
	Fail(result, errorMessage.empty() ? "no trash helper could move the image" :
		errorMessage, shouldContinue);
	return result;
}

FileOperationResult Execute(const RegisterDefaultViewerOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>& shouldContinue) {
	FileOperationResult result = NewResult(FileOperationKind::RegisterDefaultViewer,
		ownerGeneration);
	if (!Continue(shouldContinue)) {
		Fail(result, "default viewer registration was cancelled", shouldContinue);
		return result;
	}
	if (!RegisterDefaultViewer(operation.executable, operation.dataHome,
		operation.configHome, result.message)) {
		result.failure = WorkerFailure{WorkerFailureKind::ProcessingFailed, result.message};
		return result;
	}
	result.success = true;
	return result;
}

FileOperationResult Execute(const RemoveTemporaryFilesOperation& operation,
	std::uint64_t ownerGeneration, const std::function<bool()>&) {
	FileOperationResult result = NewResult(FileOperationKind::RemoveTemporaryFiles,
		ownerGeneration);
	if (!RemoveTemporaryPathsWithSourceAdmission(operation.file,
		operation.directory, result.message)) {
		result.failure = WorkerFailure{WorkerFailureKind::ProcessingFailed, result.message};
		return result;
	}
	result.success = true;
	return result;
}

} // namespace

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
void SetLosslessCropPublicationTestHookForTesting(
	detail::LosslessCropPublicationTestHook hook, void* context) {
	std::lock_guard<std::mutex> lock(losslessCropPublicationTestHookMutex);
	losslessCropPublicationTestHook = hook;
	losslessCropPublicationTestHookContext = context;
}

void SetImageSavePublicationTestHookForTesting(
	detail::ImageSavePublicationTestHook hook, void* context) {
	std::lock_guard<std::mutex> lock(imageSavePublicationTestHookMutex);
	imageSavePublicationTestHook = hook;
	imageSavePublicationTestHookContext = context;
}
#endif

FileOperationKind KindOf(const FileOperationPayload& payload) {
	return std::visit([](const auto& operation) -> FileOperationKind {
		using Type = std::decay_t<decltype(operation)>;
		if constexpr (std::is_same_v<Type, SaveImageOperation>) return FileOperationKind::SaveImage;
		else if constexpr (std::is_same_v<Type, CopyImageOperation>) return FileOperationKind::CopyImage;
		else if constexpr (std::is_same_v<Type, PasteImageOperation>) return FileOperationKind::PasteImage;
		else if constexpr (std::is_same_v<Type, LosslessTransformOperation>) return FileOperationKind::LosslessTransform;
		else if constexpr (std::is_same_v<Type, LosslessCropOperation>) return FileOperationKind::LosslessCrop;
		else if constexpr (std::is_same_v<Type, BatchCopyOperation>) return FileOperationKind::BatchCopy;
		else if constexpr (std::is_same_v<Type, SetModificationTimeOperation>) return FileOperationKind::SetModificationTime;
		else if constexpr (std::is_same_v<Type, TouchFolderExifDatesOperation>) return FileOperationKind::TouchFolderExifDates;
		else if constexpr (std::is_same_v<Type, PrintImageOperation>) return FileOperationKind::PrintImage;
		else if constexpr (std::is_same_v<Type, WallpaperImageOperation>) return FileOperationKind::WallpaperImage;
		else if constexpr (std::is_same_v<Type, WallpaperFileOperation>) return FileOperationKind::WallpaperFile;
		else if constexpr (std::is_same_v<Type, LaunchDesktopOperation>) return FileOperationKind::LaunchDesktop;
		else if constexpr (std::is_same_v<Type, MoveToTrashOperation>) return FileOperationKind::MoveToTrash;
		else if constexpr (std::is_same_v<Type, RegisterDefaultViewerOperation>) return FileOperationKind::RegisterDefaultViewer;
		else return FileOperationKind::RemoveTemporaryFiles;
	}, payload);
}

bool ParseLocalExifTimestamp(const std::string& value, std::time_t& result) {
	int year = 0;
	int month = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
	int second = 0;
	if (std::sscanf(value.c_str(), "%d:%d:%d %d:%d:%d", &year, &month, &day,
		&hour, &minute, &second) != 6) return false;
	std::tm localTime{};
	localTime.tm_year = year - 1900;
	localTime.tm_mon = month - 1;
	localTime.tm_mday = day;
	localTime.tm_hour = hour;
	localTime.tm_min = minute;
	localTime.tm_sec = second;
	localTime.tm_isdst = -1;
	const std::time_t converted = std::mktime(&localTime);
	if (converted == static_cast<std::time_t>(-1)) return false;
	result = converted;
	return true;
}

FileOperationResult ExecuteFileOperation(const FileOperationPayload& payload,
	std::uint64_t ownerGeneration,
	const std::function<bool()>& shouldContinue) {
	return std::visit([ownerGeneration, &shouldContinue](const auto& operation) {
		return Execute(operation, ownerGeneration, shouldContinue);
	}, payload);
}

FileOperationService::FileOperationService() {
	// Start only after every synchronization and state member has been initialized.
	worker_ = std::thread([this] { Run(); });
}

FileOperationService::~FileOperationService() {
	Stop();
}

std::uint64_t FileOperationService::Request(FileOperationPayload payload,
	std::uint64_t ownerGeneration) {
	std::shared_ptr<std::atomic<bool>> cancelled;
	try {
		cancelled = std::make_shared<std::atomic<bool>>(false);
	} catch (...) {
		return 0;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (stopping_ || active_ || pending_.has_value() || ready_.has_value()) return 0;
	++generation_;
	if (generation_ == 0) ++generation_;
	pending_ = Work{generation_, ownerGeneration, std::move(payload),
		std::move(cancelled)};
	available_.notify_all();
	return generation_;
}

void FileOperationService::Cancel(std::uint64_t id) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (pending_ && (id == 0 || pending_->id == id)) {
		pending_->cancelled->store(true, std::memory_order_relaxed);
	}
	if (active_ && activeCancellation_ && (id == 0 || id == activeId_)) {
		activeCancellation_->store(true, std::memory_order_relaxed);
	}
	available_.notify_all();
}

std::optional<FileOperationResult> FileOperationService::TakeReady() {
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<FileOperationResult> result = std::move(ready_);
	ready_.reset();
	return result;
}

bool FileOperationService::Busy() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return active_ || pending_.has_value() || ready_.has_value();
}

bool FileOperationService::WaitUntilIdle(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(mutex_);
	return idle_.wait_for(lock, timeout, [this] {
		return !active_ && !pending_.has_value();
	});
}

void FileOperationService::Stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!stopping_) {
			stopping_ = true;
			if (pending_) pending_->cancelled->store(true, std::memory_order_relaxed);
			if (activeCancellation_) activeCancellation_->store(true,
				std::memory_order_relaxed);
			available_.notify_all();
			idle_.notify_all();
		}
	}
	if (worker_.joinable()) worker_.join();
}

void FileOperationService::Run() {
	for (;;) {
		std::optional<Work> work;
		std::optional<Work> abandoned;
		std::optional<FileOperationResult> abandonedResult;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			available_.wait_for(lock, std::chrono::milliseconds(50), [this] {
				return stopping_ || pending_.has_value();
			});
			if (stopping_) {
				abandoned = std::move(pending_);
				pending_.reset();
				abandonedResult = std::move(ready_);
				ready_.reset();
				idle_.notify_all();
				available_.notify_all();
				lock.unlock();
				abandoned.reset();
				if (abandonedResult) RemoveResultTemporaryFiles(*abandonedResult);
				ReapDetachedChildren();
				return;
			}
			if (pending_) {
				work = std::move(pending_);
				pending_.reset();
				active_ = true;
				activeId_ = work->id;
				activeCancellation_ = work->cancelled;
			}
		}
		ReapDetachedChildren();
		if (!work) continue;

		FileOperationResult result;
		result.id = work->id;
		result.ownerGeneration = work->ownerGeneration;
		result.kind = KindOf(work->payload);
		const auto shouldContinue = [token = work->cancelled] {
			return token && !token->load(std::memory_order_relaxed);
		};
		try {
			PerfContextScope perfContext(PerfWorkClass::Unspecified,
				PerfExecution::WorkerThread);
			result = ExecuteFileOperation(work->payload, work->ownerGeneration,
				shouldContinue);
			result.id = work->id;
			result.ownerGeneration = work->ownerGeneration;
			result.kind = KindOf(work->payload);
			result.cancelled = result.cancelled ||
				(!result.success && !result.irreversibleCommandFinished &&
					!shouldContinue());
		} catch (const std::exception& error) {
			result.success = false;
			result.cancelled = !shouldContinue();
			result.failure = WorkerFailure{result.cancelled ? WorkerFailureKind::Cancelled :
				WorkerFailureKind::Exception, error.what()};
			result.message = error.what();
		} catch (...) {
			result.success = false;
			result.cancelled = !shouldContinue();
			result.failure = WorkerFailure{result.cancelled ? WorkerFailureKind::Cancelled :
				WorkerFailureKind::Exception, "unknown file operation failure"};
			result.message = "unknown file operation failure";
		}

		bool published = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			active_ = false;
			activeId_ = 0;
			activeCancellation_.reset();
			if (!stopping_) {
				if (result.detachedChild >= 0) {
					try {
						detachedChildren_.push_back(static_cast<pid_t>(result.detachedChild));
					} catch (...) {
						// If bookkeeping allocation fails, let process exit reparent the
						// detached child rather than allowing an exception to escape.
					}
				}
				ready_ = std::move(result);
				published = true;
			} else {
			}
			idle_.notify_all();
			available_.notify_all();
		}
		if (published) {
			UiCompletionWakeup().Notify();
		} else {
			RemoveResultTemporaryFiles(result);
		}
	}
}

void FileOperationService::ReapDetachedChildren() {
	std::lock_guard<std::mutex> lock(mutex_);
	auto child = detachedChildren_.begin();
	while (child != detachedChildren_.end()) {
		int status = 0;
		const pid_t waited = waitpid(*child, &status, WNOHANG);
		if (waited == *child || (waited < 0 && errno == ECHILD)) {
			child = detachedChildren_.erase(child);
		} else {
			++child;
		}
	}
}

} // namespace jpegview_linux
