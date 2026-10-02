#include "sdl_abi.h"
#include "file_list.h"
#include "file_list_scan_worker.h"
#include "display_prefetch_planner.h"
#include "exif_metadata_worker.h"
#include "pending_image_intents.h"
#include "double_page_model.h"
#include "archive_source.h"
#include "exif_reader.h"
#include "clipboard.h"
#include "image_writer.h"
#include "image_decoder.h"
#include "image_cache.h"
#include "display_image_cache.h"
#include "image.h"
#include "image_processing.h"
#include "image_processing_store.h"
#include "settings.h"
#include "advanced_configuration_model.h"
#include "sort_mode.h"
#include "desktop_applications.h"
#include "external_commands.h"
#include "batch_copy.h"
#include "image_formats.h"
#include "input_commands.h"
#include "viewport.h"
#include "resize_model.h"
#include "crop_selection_model.h"
#include "crop_size_dialog_model.h"
#include "zoom_navigator_model.h"
#include "magnifying_glass_model.h"
#include "context_menu_model.h"
#include "overlay_layout.h"
#include "viewer_chrome.h"
#include "playback_scheduler.h"
#include "thumbnail_panel_model.h"
#include "thumbnail_resampler.h"
#include "interaction_work_policy.h"
#include "work_batch_gate.h"
#include "app_icon.h"
#include "image_info_model.h"
#include "file_dialog_model.h"
#include "archive_password_dialog_model.h"
#include "recent_files.h"
#include "system_font.h"
#include "bitmap_font.h"
#include "spectrum_model.h"
#include "desktop_association.h"
#include "perf_diagnostics.h"

// Keep Linux command dispatch aligned with the original Windows application.
// resource.h is deliberately platform-neutral: it contains the command IDs
// shared by JPEGView.rc, CMainDlg::ExecuteCommand, and KeyMap.txt.default.
#include "../../src/JPEGView/resource.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using jpegview_linux::BatchCopyItem;
using jpegview_linux::FileDialogEntry;
using jpegview_linux::Image;
using jpegview_linux::MenuItem;
using jpegview_linux::PlaybackMode;

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr int kUiTextScale = 1;
constexpr const char* kRenderScaleQualityHint = "SDL_RENDER_SCALE_QUALITY";
constexpr const char* kImageTextureScaleQuality = "2";
constexpr const char* kBitmapTextScaleQuality = "0";
constexpr int kContextMenuSeparatorHeight = 7;
constexpr int kContextMenuVerticalPadding = 3;
constexpr int kContextMenuWindowInset = 4;
constexpr int kNavigationPanelHoverHeight = 64;
constexpr int kOverlayInset = 4;
constexpr int kOverlayTextPadding = 6;
constexpr int kThumbnailVerticalMargin = 1;
constexpr int kThumbnailResizeHandleHalfWidth = 3;
constexpr int kFileDialogMinimumWidth = jpegview_linux::kMinimumFileDialogWidth;
constexpr int kFileDialogMinimumHeight = jpegview_linux::kMinimumFileDialogHeight;
constexpr int kFileDialogDividerWidth = 12;
constexpr int kFileDialogScrollbarWidth = 16;
constexpr int kFileDialogScrollbarMinimumThumbHeight = 26;
constexpr int kFileDialogResizeHandleSize = 18;
constexpr int kFileDialogMinimumListWidth = 180;
constexpr int kFileDialogMinimumPreviewWidth = 120;
constexpr int kSdlCursorQuery = -1;
constexpr int kSdlCursorDisabled = 0;
constexpr int kSdlCursorEnabled = 1;
constexpr std::size_t kMagnifyingGlassDisplayPriority = 1000000;
constexpr std::size_t kDecodedImagePrefetchCount = 32;
constexpr std::size_t kDisplayTextureUploadsPerTick = 1;
constexpr std::size_t kMaximumThumbnailSourcePixels = 4u * 1024u * 1024u;
constexpr double kKeyboardPanStep = 48.0;
constexpr int kBatchSelectAll = 0;
constexpr int kBatchSelectNone = 1;
constexpr int kBatchPreview = 2;
constexpr int kBatchSavePattern = 3;
constexpr int kBatchRename = 4;
constexpr int kBatchClose = 5;
constexpr int kResizePercent = 0;
constexpr int kResizeWidth = 1;
constexpr int kResizeHeight = 2;
constexpr int kResizeFilter = 3;
constexpr int kResizeApply = 0;
constexpr int kResizeCancel = 1;
constexpr int kCropSizeApply = 0;
constexpr int kCropSizeCancel = 1;
constexpr int kAdvancedConfigurationMaximumWidth = 1040;
constexpr int kAdvancedConfigurationMaximumHeight = 760;
constexpr int kAdvancedConfigurationRowHeight = 27;
constexpr int kAdvancedConfigurationCategoryHeight = 28;
constexpr int kConfirmRestoreParameterDb = -8;
constexpr char kRepositoryUrl[] = "https://github.com/qusielle/vibe-jpegview-linux";

struct SdlClipboardTextWiper {
	void operator()(char* text) const {
		if (text == nullptr) return;
		std::size_t length = 0;
		while (text[length] != '\0') ++length;
		volatile char* bytes = text;
		for (std::size_t index = 0; index < length; ++index) bytes[index] = '\0';
		SDL_free(text);
	}
};

jpegview_linux::SystemFont& UiFont() {
	static jpegview_linux::SystemFont font;
	return font;
}

int TextWidth(const std::string& text, int scale) {
	return UiFont().TextWidth(text, scale);
}

int TextLineHeight(int scale = kUiTextScale) {
	return UiFont().LineHeight(scale);
}

int ContextMenuRowHeight() {
	return std::max(16, TextLineHeight() + 4);
}

int OverlayLineHeight() {
	return std::max(18, TextLineHeight() + 4);
}

int FilenameOverlayHeight() {
	return std::max(20, TextLineHeight() + 6);
}

std::size_t NextUtf8Boundary(const std::string& value, std::size_t position) {
	if (position >= value.size()) return value.size();
	++position;
	while (position < value.size() &&
		(static_cast<unsigned char>(value[position]) & 0xc0u) == 0x80u) ++position;
	return position;
}

std::size_t PreviousUtf8Boundary(const std::string& value, std::size_t position) {
	if (position == 0) return 0;
	--position;
	while (position > 0 && (static_cast<unsigned char>(value[position]) & 0xc0u) == 0x80u) --position;
	return position;
}

std::string ClipText(const std::string& value, int maximumWidth, int scale = kUiTextScale) {
	if (maximumWidth <= 0) return {};
	if (TextWidth(value, scale) <= maximumWidth) return value;
	const std::string ellipsis = "...";
	if (TextWidth(ellipsis, scale) > maximumWidth) return {};
	std::size_t end = 0;
	for (std::size_t next = NextUtf8Boundary(value, end); next > end;
		next = NextUtf8Boundary(value, end)) {
		if (TextWidth(value.substr(0, next) + ellipsis, scale) > maximumWidth) break;
		end = next;
		if (end == value.size()) break;
	}
	return value.substr(0, end) + ellipsis;
}

std::string ClipInputText(const std::string& value, int maximumWidth, int scale = kUiTextScale) {
	if (maximumWidth <= 0) return {};
	if (TextWidth(value, scale) <= maximumWidth) return value;
	const std::string ellipsis = "...";
	if (TextWidth(ellipsis, scale) > maximumWidth) return {};
	std::size_t start = value.size();
	while (start > 0) {
		const std::size_t previous = PreviousUtf8Boundary(value, start);
		if (TextWidth(ellipsis + value.substr(previous), scale) > maximumWidth) break;
		start = previous;
	}
	return ellipsis + value.substr(start);
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

fs::path AbsoluteNormalized(const fs::path& path) {
	std::error_code error;
	const fs::path absolute = fs::absolute(path, error);
	return (error ? path : absolute).lexically_normal();
}

fs::path CurrentViewerExecutable() {
	if (const char* appImage = std::getenv("APPIMAGE"); appImage != nullptr && *appImage != '\0') {
		return AbsoluteNormalized(fs::path(appImage));
	}
	std::error_code error;
	const fs::path executable = fs::read_symlink("/proc/self/exe", error);
	return error ? fs::path() : AbsoluteNormalized(executable);
}


std::string InfoText(const std::string& value) {
	std::string result;
	result.reserve(value.size());
	for (const unsigned char character : value) {
		result.push_back(character >= 32 && character != 127 ? static_cast<char>(character) : '?');
	}
	return result;
}

std::string FormatUnixTime(std::int64_t seconds) {
	const std::time_t timestamp = static_cast<std::time_t>(seconds);
	std::tm localTime{};
	if (localtime_r(&timestamp, &localTime) == nullptr) return {};
	char formatted[32]{};
	if (std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &localTime) == 0) return {};
	return formatted;
}

std::time_t FileModificationTime(const fs::path& filename) {
	struct stat status{};
	if (stat(filename.c_str(), &status) != 0) return 0;
	return status.st_mtime;
}

bool HasExecutable(const std::string& executable) {
	const char* path = std::getenv("PATH");
	if (path == nullptr) return false;
	const std::string searchPath(path);
	std::size_t begin = 0;
	while (begin <= searchPath.size()) {
		const std::size_t end = searchPath.find(':', begin);
		const fs::path directory = searchPath.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		const fs::path candidate = (directory.empty() ? fs::path(".") : directory) / executable;
		if (access(candidate.c_str(), X_OK) == 0) return true;
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return false;
}

[[noreturn]] void ExecProcess(const jpegview_linux::ExternalCommand& command) {
	std::vector<char*> argv;
	argv.reserve(command.arguments.size() + 2);
	argv.push_back(const_cast<char*>(command.executable.c_str()));
	for (const std::string& argument : command.arguments) argv.push_back(const_cast<char*>(argument.c_str()));
	argv.push_back(nullptr);
	execvp(command.executable.c_str(), argv.data());
	_exit(127);
}

bool RunProcess(const jpegview_linux::ExternalCommand& command, std::string& errorMessage) {
	if (!command.Valid() || !HasExecutable(command.executable)) {
		errorMessage = command.executable.empty() ? "invalid external command" :
			command.executable + " is not installed";
		return false;
	}
	const pid_t child = fork();
	if (child < 0) {
		errorMessage = "cannot start " + command.executable;
		return false;
	}
	if (child == 0) ExecProcess(command);
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errorMessage = command.executable + " failed";
		return false;
	}
	return true;
}

bool StartDetachedProcess(const jpegview_linux::ExternalCommand& command,
	std::string& errorMessage) {
	if (!command.Valid() || !HasExecutable(command.executable)) {
		errorMessage = command.executable.empty() ? "invalid external command" :
			command.executable + " is not installed";
		return false;
	}
	const pid_t child = fork();
	if (child < 0) {
		errorMessage = "cannot start " + command.executable;
		return false;
	}
	if (child == 0) {
		const pid_t detached = fork();
		if (detached < 0) _exit(127);
		if (detached > 0) _exit(0);
		setsid();
		ExecProcess(command);
	}
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errorMessage = command.executable + " failed to start";
		return false;
	}
	return true;
}

class Viewer {
public:
	Viewer(std::vector<std::string> inputs, double slideshowSeconds, bool startFullscreen)
		: startupInputs_(std::move(inputs)), initialSlideshowSeconds_(slideshowSeconds),
		  startFullscreen_(startFullscreen) {}

	int Run() {
		jpegview_linux::PerfContextScope mainThreadContext(
			jpegview_linux::PerfWorkClass::Unspecified,
			jpegview_linux::PerfExecution::EventThread);
		jpegview_linux::PerfDiagnostics::Instance();
		LoadSettings();
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
			return 1;
		}
		// Keep the window hidden while SDL and the window manager apply the
		// initial state.  Showing it first makes a restored maximized window
		// visibly appear in its normal size before it is maximized.
		Uint32 windowFlags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
		if (maximized_ && !startFullscreen_) windowFlags |= SDL_WINDOW_MAXIMIZED;
		window_ = SDL_CreateWindow("JPEGView — Loading", 0x2FFF0000, 0x2FFF0000,
			kDefaultWidth, kDefaultHeight, windowFlags);
		if (window_ == nullptr) {
			std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n';
			SDL_Quit();
			return 1;
		}
		jpegview_linux::ApplicationIcon applicationIcon;
		std::string iconError;
		if (jpegview_linux::DecodeApplicationIcon(applicationIcon, iconError)) {
			SDL_Surface* iconSurface = SDL_CreateRGBSurfaceFrom(applicationIcon.bgra.data(),
				applicationIcon.width, applicationIcon.height, 32, applicationIcon.width * 4,
				0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u);
			if (iconSurface != nullptr) {
				SDL_SetWindowIcon(window_, iconSurface);
				SDL_FreeSurface(iconSurface);
			}
		}
		SDL_SetHint(kRenderScaleQualityHint, kImageTextureScaleQuality);
		renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (renderer_ == nullptr) {
			// This is useful for software-only systems and also makes the viewer
			// testable with SDL_VIDEODRIVER=dummy in CI.
			renderer_ = SDL_CreateRenderer(window_, -1, 0);
			if (renderer_ == nullptr) {
				std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << '\n';
				SDL_DestroyWindow(window_);
				SDL_Quit();
				return 1;
			}
		}
		jpegview_linux::PerfDiagnostics& diagnostics =
			jpegview_linux::PerfDiagnostics::Instance();
		if (diagnostics.Enabled()) {
			SDL_RendererInfo rendererInfo{};
			SDL_version sdlVersion{};
			SDL_GetVersion(&sdlVersion);
			const bool hasRendererInfo = SDL_GetRendererInfo(renderer_, &rendererInfo) == 0;
			diagnostics.RecordText(jpegview_linux::PerfMetric::Renderer,
				hasRendererInfo ? rendererInfo.flags : 0,
				hasRendererInfo ? static_cast<std::uint64_t>(rendererInfo.max_texture_width) : 0,
				hasRendererInfo ? static_cast<std::uint64_t>(rendererInfo.max_texture_height) : 0,
				sdlVersion.major, sdlVersion.minor, sdlVersion.patch,
				hasRendererInfo && rendererInfo.name != nullptr ? rendererInfo.name : "unknown");
		}
		thumbnailResizeCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
		fileDialogResizeCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENWSE);
		fileDialogScrollbarCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
		cropCrosshairCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
		cropMoveCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);
		cropHorizontalCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
		cropVerticalCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
		cropDiagonalDownCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENWSE);
		cropDiagonalUpCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENESW);
		if (startFullscreen_) {
			fullscreen_ = true;
			SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
		} else if (maximized_) {
			// The creation flag handles backends that can apply the state before
			// mapping; this call covers backends that require an explicit request.
			SDL_MaximizeWindow(window_);
		}

		if (initialSlideshowSeconds_ > 0.0) {
			playback_.StartSlideshow(initialSlideshowSeconds_, SDL_GetTicks());
		}
		SetTitle("JPEGView — Loading");
		PresentStartupFrame();
		SDL_ShowWindow(window_);
		// Some X11 window managers publish the creation title when mapping a
		// previously hidden window, so repeat the loading title after the map.
		appliedWindowTitle_.Clear();
		SetTitle("JPEGView — Loading");
		PresentStartupFrame();
		SDL_PumpEvents();

		recentFilesPath_ = jpegview_linux::RecentFilesDatabasePath();
		recentFilesLoaded_ = recentFilesPath_.empty() ||
			jpegview_linux::LoadRecentFiles(recentFilesPath_, recentFiles_);

		const bool startedWithoutInputs = startupInputs_.empty();
		if (startedWithoutInputs) {
			SetTitle();
			OpenFileDialog();
		} else {
			fileList_.SetProvisionalInputs(startupInputs_);
			RequestFileListScan(jpegview_linux::FileList::ScanOperation::Initialize,
				0, FileListScanHandling::Startup, true, false, {},
				DisplayModeLoadPolicy::UseDefaults);
			startupInputs_.clear();
			if (!fileList_.Empty() && !LoadCurrent(0,
				DisplayModeLoadPolicy::UseDefaults, false, true)) {
				startupImageLoadFailed_ = true;
			} else if (fileList_.Empty()) {
				SetTitle("JPEGView — Scanning folder");
			}
		}
		int mouseX = 0;
		int mouseY = 0;
		SDL_GetMouseState(&mouseX, &mouseY);
		UpdateNavigationPanelVisibility(mouseX, mouseY);

		bool running = true;
		while (running) {
			HandleEvents(running);
			if (quitRequested_) running = false;
			TickPlayback();
			UpdateInteractionWorkPolicy();
			TickFileListScan();
			TickFileDialogArchiveDirectory();
			TickFileDialogDirectorySummaries();
			TickFileDialogFileSizes();
			ApplyWorkerDetectedSourceChanges();
			TickDisplayPrefetchPlanner();
			TickExifMetadata();
			TickCurrentJpegDimensions();
			Render();
			// Present the current image before doing renderer-thread cache uploads.
			// Held navigation then advances only after the closest ready neighbor
			// has had an opportunity to become a retained SDL texture.
			UpdateInteractionWorkPolicy();
			TickDisplayTexturePreload();
			TickThumbnailPreload(heldNavigation_.Scancode() < 0 &&
				!displayImageCache_.HasPendingWork());
			TickHeldNavigation();
			SDL_Delay(4);
		}

		Cleanup();
		return deferredExitCode_;
	}

private:
	enum class DisplayModeLoadPolicy {
		PreserveCurrent,
		UseDefaults,
		RestoreRecent,
	};

	enum class FileListScanHandling {
		None,
		Startup,
		DroppedInputs,
		Reload,
		Navigation,
		MarkedToggle,
	};

	struct ContextMenuColumn {
		std::size_t begin = 0;
		std::size_t end = 0;
		int x = 0;
		int width = 260;
		int height = 0;
	};

	struct ThumbnailCacheEntry {
		SDL_Texture* texture = nullptr;
		int width = 0;
		int height = 0;
		bool hasTransparency = false;
	};

	struct ConfirmationPreview {
		SDL_Texture* texture = nullptr;
		int width = 0;
		int height = 0;
		bool hasTransparency = false;
	};

	struct DisplayTextureCacheEntry {
		SDL_Texture* texture = nullptr;
		std::size_t bytes = 0;
		int width = 0;
		int height = 0;
		bool hasTransparency = false;
		std::uint64_t lastUsed = 0;
	};

	struct JpegDimensionCacheEntry {
		int width = 0;
		int height = 0;
	};

	struct CurrentJpegDimensionsResult {
		std::uint64_t loadGeneration = 0;
		jpegview_linux::SourceKey source;
		bool succeeded = false;
		int width = 0;
		int height = 0;
	};

	struct PendingCurrentJpegDimensions {
		jpegview_linux::SourceDescriptor source;
		std::uint64_t loadGeneration = 0;
		int prefetchDirection = 0;
		bool startupLoad = false;
	};

	struct CurrentJpegDimensionsMailbox {
		std::mutex mutex;
		std::vector<CurrentJpegDimensionsResult> ready;
		bool active = true;
	};

	struct DisplayPrefetchContext {
		jpegview_linux::ViewportSnapshot viewport;
		int imageAreaWidth = 0;
		int imageAreaHeight = 0;
		std::size_t currentIndex = 0;
		std::size_t pageCount = 0;
		jpegview_linux::DoublePageModeState doublePageMode;
		std::optional<jpegview_linux::PageDimensions> currentPageDimensions;
	};

	struct DisplayPrefetchBatch {
		std::mutex mutex;
		jpegview_linux::DisplayPrefetchBatchOwner owner =
			jpegview_linux::DisplayPrefetchBatchOwner::NeighborPlanner;
		jpegview_linux::WorkBatchGate gate;
		std::vector<jpegview_linux::DisplayImageRequest> requests;
		std::unordered_map<std::string, jpegview_linux::PageDimensions> decodedPageDimensions;
		std::unordered_set<std::string> failedDecodeFilenames;
		std::unordered_set<std::string> retainedTextureKeys;
		std::unordered_map<std::string, std::size_t> priorityByFilename;
		std::unordered_map<std::string, std::size_t> indexByFilename;
		std::unordered_map<std::string, jpegview_linux::SourceDescriptor> sourceByFilename;
		std::unordered_map<std::string, jpegview_linux::ImageProcessingParams> processingByFilename;
		std::unordered_map<std::string, bool> autoContrastByFilename;
		jpegview_linux::DisplayImageCache* cache = nullptr;
		DisplayPrefetchContext context;
	};

	struct ActiveSpreadSourceRequest {
		fs::path filename;
		bool dimensionsOnly = false;
		jpegview_linux::SourceDescriptor source;
		std::weak_ptr<DisplayPrefetchBatch> batch;
	};

	struct DoublePagePartnerSpec {
		fs::path filename;
		int targetWidth = 0;
		int targetHeight = 0;
		bool autoContrast = false;
		jpegview_linux::ImageProcessingParams processing;
		int rotationQuarterTurns = 0;
	};

	struct ActiveDoublePageRender {
		jpegview_linux::DoublePageSpread layout;
		jpegview_linux::PageDimensions currentPage;
		jpegview_linux::PageDimensions nextPage;
		DoublePagePartnerSpec partnerSpec;
		bool transformedAnchorTexture = false;
	};

	struct TextTextureCacheEntry {
		SDL_Texture* texture = nullptr;
		int width = 0;
		int height = 0;
		int offsetX = 0;
		int offsetY = 0;
		std::uint64_t lastUsed = 0;
	};

	enum class FileDialogDragMode {
		None,
		Resize,
		PreviewDivider,
		Scrollbar,
	};

	enum class FileDialogTab {
		Browse,
		Recents,
	};

	jpegview_linux::FileDialogModel& ActiveFileDialogModel() {
		return fileDialogTab_ == FileDialogTab::Recents ? recentFileDialogModel_ : fileDialogModel_;
	}

	const jpegview_linux::FileDialogModel& ActiveFileDialogModel() const {
		return fileDialogTab_ == FileDialogTab::Recents ? recentFileDialogModel_ : fileDialogModel_;
	}

	bool FileDialogHasTabs() const {
		return fileDialogOpen_ && !fileDialogSave_ && !fileDialogParameterRestore_;
	}

	void Cleanup() {
		displayPrefetchPlannerWorker_.Stop();
		exifMetadataWorker_.Stop();
		{
			std::lock_guard<std::mutex> lock(currentJpegDimensionsMailbox_->mutex);
			currentJpegDimensionsMailbox_->active = false;
			currentJpegDimensionsMailbox_->ready.clear();
		}
		CancelPendingCurrentJpegDimensions();
		fileListScanWorker_.Clear();
		SaveSettings();
		if (magnifyingGlassCursorActive_) {
			SDL_ShowCursor(magnifyingGlassPreviousCursorVisibility_);
			magnifyingGlassCursorActive_ = false;
		}
		if (recentFilesLoaded_) {
			if (!clipboardMode_ && !fileList_.Empty() &&
				recentImageLoadState_.OwnsLoadedPath(fileList_.Current())) {
				recentFiles_.RememberViewport(recentImageLoadState_.LoadedPath(),
					clipboardReturnViewport_.value_or(viewport_.Snapshot()));
				recentFiles_.RememberDoublePageMode(recentImageLoadState_.LoadedPath(),
					{doublePageModeEnabled_, mangaReadingOrderEnabled_});
			}
			if (!recentFilesPath_.empty() &&
				!jpegview_linux::SaveRecentFiles(recentFilesPath_, recentFiles_)) {
				std::cerr << "Could not save recent-file history to " << recentFilesPath_ << '\n';
			}
		}
		ClearFileDialogPreview();
		if (clipboardMode_) {
			std::error_code removeError;
			fs::remove(clipboardTempFile_, removeError);
			if (!clipboardTempDirectory_.empty()) fs::remove(clipboardTempDirectory_, removeError);
		}
		if (texture_ != nullptr) {
			SDL_DestroyTexture(texture_);
			texture_ = nullptr;
		}
		ClearDisplayTexture();
		ClearDisplayTextureCache();
		ClearTransition();
		ClearThumbnailCache();
		ClearTextTextureCache();
		if (renderer_ != nullptr) {
			SDL_DestroyRenderer(renderer_);
			renderer_ = nullptr;
		}
		if (window_ != nullptr) {
			SDL_DestroyWindow(window_);
			window_ = nullptr;
		}
		if (thumbnailResizeCursor_ != nullptr || fileDialogResizeCursor_ != nullptr ||
			fileDialogScrollbarCursor_ != nullptr ||
			cropCrosshairCursor_ != nullptr || cropMoveCursor_ != nullptr ||
			cropHorizontalCursor_ != nullptr || cropVerticalCursor_ != nullptr ||
			cropDiagonalDownCursor_ != nullptr || cropDiagonalUpCursor_ != nullptr) {
			SDL_SetCursor(SDL_GetDefaultCursor());
		}
		if (thumbnailResizeCursor_ != nullptr) {
			SDL_FreeCursor(thumbnailResizeCursor_);
			thumbnailResizeCursor_ = nullptr;
		}
		if (fileDialogResizeCursor_ != nullptr) {
			SDL_FreeCursor(fileDialogResizeCursor_);
			fileDialogResizeCursor_ = nullptr;
		}
		if (fileDialogScrollbarCursor_ != nullptr) {
			SDL_FreeCursor(fileDialogScrollbarCursor_);
			fileDialogScrollbarCursor_ = nullptr;
		}
		const auto freeCursor = [](SDL_Cursor*& cursor) {
			if (cursor != nullptr) {
				SDL_FreeCursor(cursor);
				cursor = nullptr;
			}
		};
		freeCursor(cropCrosshairCursor_);
		freeCursor(cropMoveCursor_);
		freeCursor(cropHorizontalCursor_);
		freeCursor(cropVerticalCursor_);
		freeCursor(cropDiagonalDownCursor_);
		freeCursor(cropDiagonalUpCursor_);
		SDL_Quit();
	}

	void LoadSettings() {
		jpegview_linux::LoadImageProcessingStore(
			jpegview_linux::ImageProcessingStorePath(), imageProcessingStore_);
		const fs::path settingsPath = jpegview_linux::ViewerSettingsPath();
		if (settingsPath.empty()) return;

		jpegview_linux::ViewerSettings settings;
		if (!jpegview_linux::LoadViewerSettings(settingsPath, settings)) return;
		copyRenamePattern_ = settings.copyRenamePattern;
		windowTitlePattern_ = settings.windowTitlePattern;
		transparencyPattern_ = settings.transparencyPattern;
		defaultAutoContrastEnabled_ = settings.autoContrast;
		defaultImageProcessing_ = settings.defaultImageProcessing;
		autoContrastEnabled_ = settings.autoContrast;
		keepPictureLevels_ = settings.keepPictureLevels;
		unsharpMaskRadius_ = settings.unsharpMaskRadius;
		unsharpMaskAmount_ = settings.unsharpMaskAmount;
		unsharpMaskThreshold_ = settings.unsharpMaskThreshold;
		jpegview_linux::FileList::SortMode sortMode;
		if (jpegview_linux::ParseSortMode(settings.sortMode, sortMode)) {
			fileList_.SetSorting(sortMode, settings.sortAscending);
		}
		fileList_.SetWrapAroundFolder(settings.folderWrapAround);

		maximized_ = settings.maximized;
		navigationPanelEnabled_ = settings.navigationPanelEnabled;
		navigationPanelAutoReveal_ = settings.navigationPanelAutoReveal;
		thumbnailPanelVisible_ = settings.thumbnailPanelVisible;
		showZoomNavigator_ = settings.showZoomNavigator;
		doublePageModeDefault_ = settings.doublePageModeEnabled;
		mangaReadingOrderDefault_ = settings.mangaReadingOrderEnabled;
		mangaModeInvertsLeftRight_ = settings.mangaModeInvertsLeftRight;
		spacebarNavigatesImages_ = settings.spacebarNavigatesImages;
		viewport_.SetFitRelativeZoomMode(settings.fitRelativeZoomMode);
		doublePageModeEnabled_ = doublePageModeDefault_;
		mangaReadingOrderEnabled_ = mangaReadingOrderDefault_;
		thumbnailPanelWidth_ = settings.thumbnailPanelWidth;
		fileDialogWidth_ = settings.fileDialogWidth;
		fileDialogHeight_ = settings.fileDialogHeight;
		fileDialogPreviewRatio_ = settings.fileDialogPreviewRatio;
		const SDL_Rect magnifyingGlassArea = ImageAreaRect();
		magnifyingGlass_.SetParameters(settings.magnifyingGlassWidth,
			settings.magnifyingGlassHeight, settings.magnifyingGlassZoomLevel,
			magnifyingGlassArea.w, magnifyingGlassArea.h);
		cropSelection_.SetFixedSize(settings.fixedCropWidth, settings.fixedCropHeight,
			settings.fixedCropScreenPixels);
		cropSelection_.SetMode(jpegview_linux::CropSelectionMode::Free);
		cropUserAspectWidth_ = settings.userCropAspectWidth;
		cropUserAspectHeight_ = settings.userCropAspectHeight;
		selectionModeEnabled_ = settings.selectionModeEnabled;
		infoVisible_ = settings.infoVisible;
		showHistogram_ = settings.showHistogram;
		showFileName_ = settings.showFilename;
		autoContrastEnabled_ = settings.autoContrast;
		cacheSizeMiB_ = settings.cacheSizeMiB;
		cacheBudget_->SetCapacity(jpegview_linux::CacheBytesFromMiB(cacheSizeMiB_));
		viewport_.LoadScaleMode(settings.scaleMode, settings.manualZoomSet, settings.manualZoom);
	}

	jpegview_linux::ViewerSettings CurrentViewerSettings() const {
		jpegview_linux::ViewerSettings settings;
		settings.scaleMode = viewport_.NavigationScaleMode();
		settings.sortMode = jpegview_linux::SortModeSettingName(fileList_.GetSorting());
		settings.sortAscending = fileList_.IsSortedAscending();
		settings.manualZoom = viewport_.NavigationZoom();
		settings.maximized = maximized_;
		settings.navigationPanelEnabled = navigationPanelEnabled_;
		settings.navigationPanelAutoReveal = navigationPanelAutoReveal_;
		settings.thumbnailPanelVisible = thumbnailPanelVisible_;
		settings.showZoomNavigator = showZoomNavigator_;
		settings.doublePageModeEnabled = doublePageModeDefault_;
		settings.mangaReadingOrderEnabled = mangaReadingOrderDefault_;
		settings.mangaModeInvertsLeftRight = mangaModeInvertsLeftRight_;
		settings.spacebarNavigatesImages = spacebarNavigatesImages_;
		settings.folderWrapAround = fileList_.WrapAroundFolder();
		settings.fitRelativeZoomMode = viewport_.FitRelativeZoomMode();
		settings.transparencyPattern = transparencyPattern_;
		settings.thumbnailPanelWidth = thumbnailPanelWidth_;
		settings.fileDialogWidth = fileDialogWidth_;
		settings.fileDialogHeight = fileDialogHeight_;
		settings.fileDialogPreviewRatio = fileDialogPreviewRatio_;
		settings.magnifyingGlassWidth = magnifyingGlass_.Width();
		settings.magnifyingGlassHeight = magnifyingGlass_.Height();
		settings.magnifyingGlassZoomLevel = magnifyingGlass_.ZoomLevel();
		settings.fixedCropWidth = cropSelection_.FixedWidth();
		settings.fixedCropHeight = cropSelection_.FixedHeight();
		settings.fixedCropScreenPixels = cropSelection_.FixedSizeUsesScreenPixels();
		settings.userCropAspectWidth = cropUserAspectWidth_;
		settings.userCropAspectHeight = cropUserAspectHeight_;
		settings.selectionModeEnabled = selectionModeEnabled_;
		settings.infoVisible = infoVisible_;
		settings.showHistogram = showHistogram_;
		settings.showFilename = showFileName_;
		settings.autoContrast = defaultAutoContrastEnabled_;
		settings.defaultImageProcessing = defaultImageProcessing_;
		settings.keepPictureLevels = keepPictureLevels_;
		settings.unsharpMaskRadius = unsharpMaskRadius_;
		settings.unsharpMaskAmount = unsharpMaskAmount_;
		settings.unsharpMaskThreshold = unsharpMaskThreshold_;
		settings.cacheSizeMiB = cacheSizeMiB_;
		settings.copyRenamePattern = copyRenamePattern_;
		settings.windowTitlePattern = windowTitlePattern_;
		return settings;
	}

	bool SaveSettings() const {
		const fs::path settingsPath = jpegview_linux::ViewerSettingsPath();
		if (settingsPath.empty()) return false;
		return jpegview_linux::SaveViewerSettings(settingsPath, CurrentViewerSettings());
	}

	jpegview_linux::SourceDescriptor SourceDescriptorForPath(const fs::path& filename) const {
		const std::optional<std::size_t> index = fileList_.IndexOf(filename);
		if (!index.has_value()) return {};
		const jpegview_linux::SourceDescriptor* source = fileList_.DescriptorAt(*index);
		return source == nullptr ? jpegview_linux::SourceDescriptor() : *source;
	}

	bool CachedJpegDimensions(const jpegview_linux::SourceDescriptor& source,
		int& width, int& height) const {
		const jpegview_linux::SourceKey key = source.Key();
		if (source.Metadata().hasDimensions) {
			width = source.Metadata().width;
			height = source.Metadata().height;
			return width > 0 && height > 0;
		}
		const auto cached = jpegDimensionCache_.find(key);
		if (key.Valid() && cached != jpegDimensionCache_.end()) {
			width = cached->second.width;
			height = cached->second.height;
			return true;
		}
		return false;
	}

	void CancelPendingCurrentJpegDimensions() {
		recentImageLoadState_.CancelPendingLoad();
		pendingImageIntents_.Cancel();
		pendingTransitionImage_ = {};
		if (pendingCurrentJpegDimensions_.has_value()) {
			imageCache_.CancelActiveSpreadRequest(
				pendingCurrentJpegDimensions_->source, true);
			pendingCurrentJpegDimensions_.reset();
		}
		currentJpegHeaderPending_ = false;
		pendingImageIntentLimitReached_ = false;
	}

	void RequestCurrentJpegDimensions(const jpegview_linux::SourceDescriptor& source,
		std::uint64_t loadGeneration, int prefetchDirection, bool startupLoad) {
		const std::shared_ptr<CurrentJpegDimensionsMailbox> mailbox =
			currentJpegDimensionsMailbox_;
		pendingCurrentJpegDimensions_ = PendingCurrentJpegDimensions{
			source, loadGeneration, prefetchDirection, startupLoad};
		imageCache_.RequestJpegDimensions(source,
			[mailbox, loadGeneration, sourceKey = source.Key()](
				const fs::path&, bool succeeded, int width, int height) {
				std::lock_guard<std::mutex> lock(mailbox->mutex);
				if (!mailbox->active) return;
				mailbox->ready.push_back(CurrentJpegDimensionsResult{
					loadGeneration, sourceKey,
					succeeded, width, height});
			}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	}

	void TickCurrentJpegDimensions() {
		std::vector<CurrentJpegDimensionsResult> results;
		{
			std::lock_guard<std::mutex> lock(currentJpegDimensionsMailbox_->mutex);
			results.swap(currentJpegDimensionsMailbox_->ready);
		}
		for (const CurrentJpegDimensionsResult& result : results) {
			if (fileList_.Empty()) continue;
			const jpegview_linux::SourceDescriptor source =
				SourceDescriptorForPath(fileList_.Current());
			if (!jpegview_linux::IsCurrentJpegDimensionsResult(result.loadGeneration,
				result.source, currentJpegLoadGeneration_, source.Key()) ||
				!pendingCurrentJpegDimensions_.has_value() ||
				pendingCurrentJpegDimensions_->loadGeneration != result.loadGeneration ||
				pendingCurrentJpegDimensions_->source.Key() != result.source) continue;
			const std::optional<jpegview_linux::PendingRecentImageLoad> pendingLoad =
				recentImageLoadState_.TakePendingLoad(AbsoluteNormalized(fileList_.Current()));
			if (!pendingLoad.has_value()) continue;
			const PendingCurrentJpegDimensions pending =
				std::move(*pendingCurrentJpegDimensions_);
			pendingCurrentJpegDimensions_.reset();
			currentJpegHeaderPending_ = false;
			std::optional<jpegview_linux::PendingImageIntentBatch> intents =
				pendingImageIntents_.Take(result.source, result.loadGeneration);
			Image transitionImage;
			if (intents.has_value() && intents->startTransition) {
				transitionImage = std::move(pendingTransitionImage_);
			}
			pendingTransitionImage_ = {};
			if (result.succeeded && result.width > 0 && result.height > 0) {
				jpegDimensionCache_[result.source] = {result.width, result.height};
				failedJpegDimensionKeys_.erase(result.source);
			} else {
				failedJpegDimensionKeys_.insert(result.source);
			}
			const bool loaded = LoadCurrent(pending.prefetchDirection,
				DisplayModeLoadPolicy::PreserveCurrent, true, pending.startupLoad,
				pendingLoad, intents.has_value() && !intents->actions.empty());
			if (!loaded && pending.startupLoad) {
				startupImageLoadFailed_ = true;
				deferredExitCode_ = 1;
				quitRequested_ = true;
				continue;
			}
			if (loaded && intents.has_value()) {
				bool replayedViewportIntent = false;
				for (const jpegview_linux::PendingImageIntent& intent : intents->actions) {
					if (intent.type == jpegview_linux::PendingImageIntentType::Viewport) {
						ApplyPendingViewportIntent(intent.viewport);
						replayedViewportIntent = true;
					} else {
						ApplyTransform(intent.transform);
					}
				}
				if (replayedViewportIntent) {
					currentDisplayRequest_.reset();
					PrepareImagePrefetch(pending.prefetchDirection);
					RefreshDoublePageRenderState();
				}
				if (intents->startTransition) StartTransition(transitionImage);
			}
		}
	}

	void TickExifMetadata() {
		for (jpegview_linux::ExifMetadataResult& result : exifMetadataWorker_.TakeReady()) {
			if (clipboardMode_ || fileList_.Empty() ||
				!jpegview_linux::IsCurrentExifMetadataResult(result,
					exifMetadataRequestGeneration_, exifMetadataSource_)) continue;
			const jpegview_linux::SourceDescriptor current =
				SourceDescriptorForPath(fileList_.Current());
			if (current.Key() != result.source) continue;
			metadata_ = std::move(result.metadata);
			jpegComment_ = std::move(result.jpegComment);
			++imageInfoMetadataRevision_;
			const jpegview_linux::ExifDateActionCompletion deferredAction =
				deferredExifDateAction_.Complete(result,
					AbsoluteNormalized(fileList_.Current()), current.Key());
			if (deferredAction.runDeferredAction) TouchCurrentImage(true);
		}
	}

	void TickDisplayPrefetchPlanner() {
		for (jpegview_linux::DisplayPrefetchPlannerResult& result :
			displayPrefetchPlannerWorker_.TakeReady()) {
			if (fileList_.Empty() || clipboardMode_) continue;
			const SDL_Rect imageArea = ImageAreaRect();
			if (!jpegview_linux::MatchesDisplayPrefetchSnapshot(result,
				activeDisplayPrefetchGeneration_, fileList_.MutationRevision(),
				fileList_.DescriptorRevision(), prefetchViewportRevision_,
				fileList_.CurrentIndex(), pendingPrefetchDirection_,
				viewport_.NavigationSnapshot(), imageArea.w, imageArea.h) ||
				CurrentInteractionWorkPlan().cancelQueuedSpeculation) continue;
			for (const jpegview_linux::DisplayPrefetchPlannedDimensions& dimensions :
				result.dimensions) {
				const jpegview_linux::SourceDescriptor* current =
					fileList_.DescriptorAt(dimensions.index);
				if (current == nullptr || current->Key() != dimensions.source ||
					dimensions.width <= 0 || dimensions.height <= 0) continue;
				jpegDimensionCache_[dimensions.source] = {
					dimensions.width, dimensions.height};
			}
			const std::shared_ptr<DisplayPrefetchBatch> batch = displayPrefetchBatch_;
			if (!batch) continue;
			bool published = batch->gate.Publish([&] {
				std::vector<jpegview_linux::DisplayImageRequest> requests;
				{
					std::lock_guard<std::mutex> lock(batch->mutex);
					jpegview_linux::AppendDisplayPrefetchRequests(batch->requests,
						result.requests);
					requests = batch->requests;
				}
				displayImageCache_.Prefetch(std::move(requests));
			});
			if (!published) continue;
			for (const std::string& key : result.protectedTextureKeys) {
				displayTextureProtectedKeys_.insert(key);
			}
		}
	}

	std::optional<jpegview_linux::PageDimensions> PageDimensionsAt(std::size_t index) {
		if (index >= fileList_.Files().size()) return std::nullopt;
		if (activeDoublePageRender_.has_value()) {
			if (index == activeDoublePageRender_->layout.firstIndex)
				return activeDoublePageRender_->currentPage;
			if (index == activeDoublePageRender_->layout.secondIndex)
				return activeDoublePageRender_->nextPage;
		}
		if (index == fileList_.CurrentIndex() && image_.width > 0 && image_.height > 0) {
			if (currentSpreadRotationValid_ && currentSourcePageDimensions_.has_value()) {
				return currentSourcePageDimensions_;
			}
			return jpegview_linux::PageDimensions{image_.width, image_.height};
		}
		const fs::path& filename = fileList_.Files()[index];
		const jpegview_linux::SourceDescriptor source = SourceDescriptorForPath(filename);
		if (const auto decoded = imageCache_.Find(source); decoded && !decoded->frames.empty()) {
			const jpegview_linux::DecodedFrame& frame = decoded->frames.front();
			return jpegview_linux::PageDimensions{frame.width, frame.height};
		}
		if (displayPrefetchBatch_) {
			std::lock_guard<std::mutex> lock(displayPrefetchBatch_->mutex);
			const auto prefetched = displayPrefetchBatch_->decodedPageDimensions.find(
				filename.string());
			if (prefetched != displayPrefetchBatch_->decodedPageDimensions.end()) {
				return prefetched->second;
			}
		}
		const auto cached = jpegDimensionCache_.find(source.Key());
		if (cached != jpegDimensionCache_.end()) {
			return jpegview_linux::PageDimensions{cached->second.width, cached->second.height};
		}
		return std::nullopt;
	}

	bool IsPotentialDoublePageAnchor(std::size_t index) const {
		return doublePageModeEnabled_ && cacheBudget_->Capacity() != 0 && index > 0 &&
			index + 1 < fileList_.Size();
	}

	bool NeighborDecodeFailed(std::size_t index) const {
		if (index >= fileList_.Files().size() || !displayPrefetchBatch_) return false;
		std::lock_guard<std::mutex> lock(displayPrefetchBatch_->mutex);
		return displayPrefetchBatch_->failedDecodeFilenames.find(
			fileList_.Files()[index].string()) !=
			displayPrefetchBatch_->failedDecodeFilenames.end();
	}

	std::pair<int, int> ViewportContentDimensions() const {
		if (activeDoublePageRender_.has_value()) {
			return {activeDoublePageRender_->layout.canvasWidth,
				activeDoublePageRender_->layout.canvasHeight};
		}
		return {image_.width, image_.height};
	}

	std::pair<int, int> DoublePagePageDisplaySize(
		const jpegview_linux::DoublePageSpread& spread,
		const jpegview_linux::SpreadPagePlacement& page, const SDL_Rect& area) const {
		jpegview_linux::Viewport spreadViewport = viewport_;
		spreadViewport.Restore(viewport_.Snapshot(), spread.canvasWidth, spread.canvasHeight,
			area.w, area.h);
		const jpegview_linux::ViewportRect canvas = spreadViewport.Destination(
			spread.canvasWidth, spread.canvasHeight, area.w, area.h);
		const int left = static_cast<int>(std::lround(
			static_cast<double>(canvas.width) * page.x / spread.canvasWidth));
		const int top = static_cast<int>(std::lround(
			static_cast<double>(canvas.height) * page.y / spread.canvasHeight));
		const int right = static_cast<int>(std::lround(
			static_cast<double>(canvas.width) * (page.x + page.width) / spread.canvasWidth));
		const int bottom = static_cast<int>(std::lround(
			static_cast<double>(canvas.height) * (page.y + page.height) / spread.canvasHeight));
		return {std::max(1, right - left), std::max(1, bottom - top)};
	}

	std::optional<DoublePagePartnerSpec> DoublePagePartnerDisplaySpec(
		const jpegview_linux::DoublePageSpread& spread) {
		if (spread.secondIndex >= fileList_.Files().size()) return std::nullopt;
		const fs::path& filename = fileList_.Files()[spread.secondIndex];
		const SDL_Rect area = ImageAreaRect();
		const auto targetSize = DoublePagePageDisplaySize(spread, spread.nextPage, area);
		const int targetWidth = targetSize.first;
		const int targetHeight = targetSize.second;
		const auto savedProcessing = imageProcessingStore_.find(
			AbsoluteNormalized(filename).string());
		const jpegview_linux::ImageProcessingPreset currentPreset{
			imageProcessing_, autoContrastEnabled_};
		const jpegview_linux::ImageProcessingPreset filePreset =
			jpegview_linux::ResolveImageProcessingForFile(currentPreset,
				savedProcessing == imageProcessingStore_.end() ? nullptr : &savedProcessing->second,
				keepPictureLevels_, defaultAutoContrastEnabled_, defaultImageProcessing_);
		jpegview_linux::ImageProcessingParams processing = filePreset.processing;
		processing.unsharpRadius = unsharpMaskRadius_;
		processing.unsharpAmount = 0.0;
		processing.unsharpThreshold = unsharpMaskThreshold_;
		return DoublePagePartnerSpec{filename, targetWidth, targetHeight,
			filePreset.autoContrast, processing, spread.clockwiseQuarterTurns};
	}

	std::optional<jpegview_linux::DisplayImageRequest> MakeDoublePagePartnerRequest(
		const DoublePagePartnerSpec& spec, const jpegview_linux::PageDimensions& nextPage) {
		const fs::path& filename = spec.filename;
		const jpegview_linux::SourceDescriptor source = SourceDescriptorForPath(filename);
		if (jpegview_linux::IsJpegPath(filename)) {
			jpegview_linux::DisplayImageRequest request =
				jpegview_linux::MakeJpegDisplayImageRequest(source, nextPage.width,
				nextPage.height, spec.targetWidth, spec.targetHeight,
				spec.autoContrast, 1, spec.processing, spec.rotationQuarterTurns);
			request.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
			return request;
		}
		const auto decoded = imageCache_.Find(source);
		if (!decoded || decoded->frames.empty()) return std::nullopt;
		jpegview_linux::DisplayImageRequest request =
			jpegview_linux::MakeDisplayImageRequest(source, decoded, 0,
			spec.targetWidth, spec.targetHeight, spec.autoContrast, 1, spec.processing,
			spec.rotationQuarterTurns);
		request.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
		return request;
	}

	std::string ModifiedSpreadAnchorKey() const {
		if (fileList_.Empty()) return {};
		return "modified-spread-anchor:" +
			AbsoluteNormalized(fileList_.Current()).string() + ':' +
			std::to_string(modifiedImageRevision_) + ':' +
			std::to_string(currentImageRotationQuarterTurns_);
	}

	SDL_Texture* ActiveDoublePageAnchorTexture() {
		if (!activeDoublePageRender_.has_value()) return nullptr;
		if (activeDoublePageRender_->transformedAnchorTexture) return texture_;
		return FindDisplayTexture(doublePagePresentation_.AnchorTextureKey());
	}

	void CancelPendingDoublePageRequests() {
		const auto cancel = [this](const std::string& key) {
			if (key.empty()) return;
			displayTextureProtectedKeys_.erase(key);
			if (FindDisplayTexture(key) == nullptr) {
				displayImageCache_.CancelBackground(key);
				displayImageCache_.Release(key);
			}
		};
		cancel(doublePagePresentation_.AnchorTextureKey());
		cancel(doublePagePresentation_.PartnerTextureKey());
	}

	void RefreshDoublePageRenderState() {
		jpegview_linux::PerfContextScope workContext(
			jpegview_linux::PerfWorkClass::ActiveImageSpread,
			jpegview_linux::PerfExecution::EventThread);
		const SDL_Rect area = ImageAreaRect();
		const auto deactivate = [this, &area] {
			displayTextureProtectedKeys_.erase(doublePagePartnerDisplayKey_);
			doublePagePartnerDisplayKey_.clear();
			if (!activeDoublePageRender_.has_value()) return;
			const jpegview_linux::ViewportSnapshot snapshot = viewport_.Snapshot();
			activeDoublePageRender_.reset();
			viewport_.Restore(snapshot, image_.width, image_.height, area.w, area.h);
			currentDisplayRequest_.reset();
			SetTitle();
		};
		const std::size_t currentIndex = fileList_.Empty() ? 0 : fileList_.CurrentIndex();
		const auto useSinglePage = [this, &deactivate, currentIndex](
			bool keepDeferredDisplay = false) {
			CancelActiveSpreadPartnerSourceRequest();
			CancelPendingDoublePageRequests();
			doublePagePresentation_.UseSinglePage(currentIndex);
			doublePagePartnerRequest_.reset();
			if (!keepDeferredDisplay) deferredCurrentDisplayPreparation_ = false;
			deactivate();
		};
		const auto awaitDimensions = [this, &deactivate, currentIndex] {
			ReconcileActiveSpreadPartnerSourceRequest();
			CancelPendingDoublePageRequests();
			doublePagePresentation_.AwaitDimensions(currentIndex, currentIndex + 1);
			doublePagePartnerRequest_.reset();
			deactivate();
		};
		if (fileList_.Empty()) {
			useSinglePage();
			return;
		}
		const bool selectionMatchesLoadedImage = clipboardMode_ ||
			recentImageLoadState_.LoadedPath() == AbsoluteNormalized(fileList_.Current());
		if (!selectionMatchesLoadedImage) {
			if (IsPotentialDoublePageAnchor(currentIndex)) awaitDimensions();
			else useSinglePage();
			return;
		}
		if (!doublePageModeEnabled_ || cacheBudget_->Capacity() == 0 || currentIndex == 0 ||
			currentIndex + 1 >= fileList_.Size() || image_.width <= 0 || image_.height <= 0) {
			useSinglePage();
			return;
		}
		auto current = PageDimensionsAt(currentIndex);
		if (!current.has_value()) {
			useSinglePage();
			return;
		}
		if (!jpegview_linux::CanAnchorDoublePageSpread(currentIndex,
				fileList_.Size(), *current, true)) {
			useSinglePage();
			return;
		}
		auto next = PageDimensionsAt(currentIndex + 1);
		if (!next.has_value()) {
			if (NeighborDecodeFailed(currentIndex + 1)) useSinglePage(true);
			else awaitDimensions();
			return;
		}
		CancelActiveSpreadPartnerSourceRequest();
		const jpegview_linux::DoublePageModeState modes{
			doublePageModeEnabled_, mangaReadingOrderEnabled_};
		const auto layout = jpegview_linux::BuildDoublePageSpread(currentIndex,
			fileList_.Size(), *current, next, modes, true,
			currentImageRotationQuarterTurns_);
		if (!layout.has_value()) {
			useSinglePage(true);
			return;
		}
		const auto spec = DoublePagePartnerDisplaySpec(*layout);
		if (!spec.has_value()) {
			useSinglePage();
			return;
		}
		auto partnerRequest = MakeDoublePagePartnerRequest(*spec, *next);
		if (!partnerRequest.has_value() || !partnerRequest->Valid()) {
			useSinglePage(true);
			return;
		}
		const std::string previousCurrentDisplayKey = currentDisplayRequest_.has_value() ?
			currentDisplayRequest_->key : std::string();

		const bool sameSpreadCanvas = activeDoublePageRender_.has_value() &&
			activeDoublePageRender_->layout.firstIndex == layout->firstIndex &&
			activeDoublePageRender_->layout.secondIndex == layout->secondIndex &&
			activeDoublePageRender_->layout.canvasWidth == layout->canvasWidth &&
			activeDoublePageRender_->layout.canvasHeight == layout->canvasHeight;
		const std::string previousPosition = CurrentImagePositionText();
		if (!sameSpreadCanvas) {
			const jpegview_linux::ViewportSnapshot snapshot = viewport_.Snapshot();
			viewport_.Restore(snapshot, layout->canvasWidth, layout->canvasHeight,
				area.w, area.h);
			currentDisplayRequest_.reset();
		}
		const bool transformedAnchor = (currentPixelsDetachedFromSource_ ||
			(currentSpreadRotationValid_ && imageModified_)) && texture_ != nullptr;
		activeDoublePageRender_ = ActiveDoublePageRender{
			*layout, *current, *next, *spec, transformedAnchor};
		if (CurrentImagePositionText() != previousPosition) SetTitle();
		const auto anchorSize = DoublePagePageDisplaySize(*layout, layout->currentPage, area);
		std::optional<jpegview_linux::DisplayImageRequest> anchorRequest;
		std::string anchorKey;
		if (transformedAnchor) {
			currentDisplayRequest_.reset();
			deferredCurrentDisplayPreparation_ = false;
			anchorKey = ModifiedSpreadAnchorKey();
		} else {
			const jpegview_linux::DisplayImageRequest* anchorRequestPointer =
				CurrentDisplayRequest(anchorSize.first, anchorSize.second);
			if (anchorRequestPointer == nullptr || !anchorRequestPointer->Valid()) {
				useSinglePage();
				return;
			}
			anchorRequest = *anchorRequestPointer;
			anchorKey = anchorRequest->key;
		}
		const std::size_t anchorPixels = static_cast<std::size_t>(anchorSize.first) *
			static_cast<std::size_t>(anchorSize.second);
		const std::size_t partnerPixels =
			static_cast<std::size_t>(spec->targetWidth) *
			static_cast<std::size_t>(spec->targetHeight);
		if (anchorPixels > std::numeric_limits<std::size_t>::max() / 4 ||
			partnerPixels > std::numeric_limits<std::size_t>::max() / 4 ||
			anchorPixels * 4 > cacheBudget_->Capacity() ||
			partnerPixels * 4 > cacheBudget_->Capacity() - anchorPixels * 4) {
			useSinglePage();
			return;
		}

		const std::string oldAnchorKey = doublePagePresentation_.AnchorTextureKey();
		const std::string oldPartnerKey = doublePagePresentation_.PartnerTextureKey();
		if (doublePagePresentation_.SpreadFailed(currentIndex, anchorKey,
			partnerRequest->key)) {
			CancelPendingDoublePageRequests();
			deferredCurrentDisplayPreparation_ = true;
			doublePagePartnerRequest_.reset();
			deactivate();
			return;
		}
		const bool newPairRequests = doublePagePresentation_.BeginSpread(currentIndex,
			currentIndex + 1, anchorKey, partnerRequest->key);
		if (newPairRequests) {
			for (const std::string* oldKey : {&oldAnchorKey, &oldPartnerKey}) {
				if (oldKey->empty() || *oldKey == anchorKey ||
					*oldKey == partnerRequest->key) continue;
				displayTextureProtectedKeys_.erase(*oldKey);
				if (FindDisplayTexture(*oldKey) == nullptr) {
					displayImageCache_.CancelBackground(*oldKey);
					displayImageCache_.Release(*oldKey);
				}
			}
			doublePagePartnerRequest_.reset();
		}
		if (!previousCurrentDisplayKey.empty() &&
			previousCurrentDisplayKey != anchorKey) {
			displayTextureProtectedKeys_.erase(previousCurrentDisplayKey);
		}
		if (anchorRequest.has_value()) currentDisplayRequest_ = *anchorRequest;
		doublePagePartnerRequest_ = std::move(partnerRequest);
		doublePagePartnerDisplayKey_ = doublePagePartnerRequest_->key;
		displayTextureProtectedKeys_.insert(anchorKey);
		displayTextureProtectedKeys_.insert(doublePagePartnerDisplayKey_);

		std::vector<jpegview_linux::DisplayImageRequest> requests;
		if (anchorRequest.has_value() && FindDisplayTexture(anchorKey) == nullptr) {
			requests.push_back(*anchorRequest);
		}
		if (FindDisplayTexture(doublePagePartnerDisplayKey_) == nullptr) {
			requests.push_back(*doublePagePartnerRequest_);
		}
		if (!requests.empty()) displayImageCache_.RequestBackgroundBatch(requests);
		if (transformedAnchor || FindDisplayTexture(anchorKey) != nullptr) {
			doublePagePresentation_.MarkTextureReady(anchorKey);
		}
		if (FindDisplayTexture(doublePagePartnerDisplayKey_) != nullptr) {
			doublePagePresentation_.MarkTextureReady(doublePagePartnerDisplayKey_);
			doublePagePartnerRequest_->decoded.reset();
		}
		const bool anchorUnavailable = !transformedAnchor &&
			FindDisplayTexture(anchorKey) == nullptr &&
			!displayImageCache_.HasPendingOrCached(anchorKey);
		const bool partnerUnavailable = FindDisplayTexture(doublePagePartnerDisplayKey_) == nullptr &&
			!displayImageCache_.HasPendingOrCached(doublePagePartnerDisplayKey_);
		if (!doublePagePresentation_.SpreadReady(currentIndex) &&
			(anchorUnavailable || partnerUnavailable)) {
			doublePagePresentation_.MarkTextureFailed(
				anchorUnavailable ? anchorKey : doublePagePartnerDisplayKey_);
			deferredCurrentDisplayPreparation_ = true;
		}
	}

	bool MaterializeCurrentPixels() {
		jpegview_linux::PerfContextScope workContext(
			jpegview_linux::PerfWorkClass::ActiveImageSpread,
			jpegview_linux::PerfExecution::EventThread);
		if (currentPixelsMaterialized_ &&
			jpegview_linux::EqualImageProcessing(materializedProcessing_, imageProcessing_) &&
			materializedAutoContrast_ == autoContrastEnabled_) return true;
		if (currentPixelsMaterialized_ && correctionBaseValid_) {
			image_ = correctionBase_;
			jpegview_linux::PerfScopedTimer processingTimer(
				jpegview_linux::PerfDiagnostics::Instance(), jpegview_linux::PerfMetric::Processing);
			if (!image_.ApplyProcessing(imageProcessing_, autoContrastEnabled_)) return false;
			materializedProcessing_ = imageProcessing_;
			materializedAutoContrast_ = autoContrastEnabled_;
			return true;
		}
		if (!currentDecoded_) {
			const jpegview_linux::SourceDescriptor source =
				SourceDescriptorForPath(fileList_.Current());
			currentDecoded_ = imageCache_.Find(source);
			if (!currentDecoded_) currentDecoded_ = imageCache_.FindOrWait(source);
			if (!currentDecoded_) {
				auto decoded = std::make_shared<jpegview_linux::DecodedImage>();
				std::string errorMessage;
				if (!jpegview_linux::DecodeImage(fileList_.Current(), *decoded, errorMessage) ||
					decoded->frames.empty()) {
					SetTitle(fileList_.Current().filename().string() +
						" — decode failed: " + errorMessage);
					return false;
				}
				imageCache_.Store(source, decoded);
				currentDecoded_ = std::move(decoded);
			}
		}
		if (currentDecoded_->frames.empty()) return false;
		std::vector<Image> decodedFrames;
		if (currentDecoded_->frames.size() > 1) decodedFrames.reserve(currentDecoded_->frames.size());
		for (const jpegview_linux::DecodedFrame& decodedFrame : currentDecoded_->frames) {
			Image frame;
			if (!frame.StoreBGRA(decodedFrame.bgra.data(), decodedFrame.width, decodedFrame.height,
				decodedFrame.hasTransparency)) {
				SetTitle(fileList_.Current().filename().string() + " — image is too large");
				return false;
			}
			if (currentDecoded_->frames.size() > 1) decodedFrames.push_back(std::move(frame));
			else image_ = std::move(frame);
		}
		if (!decodedFrames.empty()) {
			animationFrames_ = std::move(decodedFrames);
			image_ = animationFrames_[currentAnimationFrame_];
		} else {
			animationFrames_.clear();
		}
		correctionBase_ = image_;
		correctionBaseValid_ = true;
		jpegview_linux::PerfScopedTimer processingTimer(
			jpegview_linux::PerfDiagnostics::Instance(), jpegview_linux::PerfMetric::Processing);
		if (!image_.ApplyProcessing(imageProcessing_, autoContrastEnabled_)) {
			SetTitle(fileList_.Current().filename().string() + " — picture-level processing failed");
			return false;
		}
		materializedProcessing_ = imageProcessing_;
		materializedAutoContrast_ = autoContrastEnabled_;
		currentPixelsMaterialized_ = true;
		return true;
	}

	void SelectPictureLevelsForCurrentFile(bool continuingPendingLoad = false) {
		const std::string key = AbsoluteNormalized(fileList_.Current()).string();
		const auto saved = imageProcessingStore_.find(key);
		const jpegview_linux::ImageProcessingPreset current{imageProcessing_, autoContrastEnabled_};
		const jpegview_linux::ImageProcessingPreset selected =
			jpegview_linux::ResolveImageProcessingForLoad(current,
				saved == imageProcessingStore_.end() ? nullptr : &saved->second,
				keepPictureLevels_, defaultAutoContrastEnabled_, defaultImageProcessing_,
				continuingPendingLoad);
		imageProcessing_ = selected.processing;
		autoContrastEnabled_ = selected.autoContrast;
		imageProcessing_.unsharpRadius = unsharpMaskRadius_;
		imageProcessing_.unsharpAmount = 0.0;
		imageProcessing_.unsharpThreshold = unsharpMaskThreshold_;
	}

	bool LoadCurrent(int prefetchDirection = 0,
		DisplayModeLoadPolicy modeLoadPolicy = DisplayModeLoadPolicy::PreserveCurrent,
		bool preserveExifMetadataRequest = false, bool startupLoad = false,
		std::optional<jpegview_linux::PendingRecentImageLoad> resumePendingLoad = std::nullopt,
		bool replayPendingIntents = false) {
		jpegview_linux::PerfContextScope workContext(
			jpegview_linux::PerfWorkClass::ActiveImageSpread,
			jpegview_linux::PerfExecution::EventThread);
		if (fileList_.Empty()) {
			CancelPendingCurrentJpegDimensions();
			return false;
		}
		const fs::path targetPath = AbsoluteNormalized(fileList_.Current());
		const bool pathChanged = recentImageLoadState_.LoadedPath().empty() ||
			targetPath != recentImageLoadState_.LoadedPath();
		if (!clipboardMode_) {
			recentImageLoadState_.SaveCurrentBeforeLoad(targetPath,
				clipboardReturnViewport_.value_or(viewport_.Snapshot()),
				{doublePageModeEnabled_, mangaReadingOrderEnabled_}, recentFiles_);
		}
		if (!clipboardMode_ && pathChanged &&
			modeLoadPolicy != DisplayModeLoadPolicy::PreserveCurrent) {
			const std::optional<jpegview_linux::DoublePageModeState> savedModes =
				modeLoadPolicy == DisplayModeLoadPolicy::RestoreRecent ?
					recentFiles_.FindDoublePageMode(targetPath) : std::nullopt;
			doublePageModeEnabled_ = savedModes.has_value() ? savedModes->enabled :
				doublePageModeDefault_;
			mangaReadingOrderEnabled_ = savedModes.has_value() ? savedModes->mangaReadingOrder :
				mangaReadingOrderDefault_;
		}
		const std::size_t currentIndex = fileList_.CurrentIndex();
		CancelPendingDoublePageRequests();
		if (IsPotentialDoublePageAnchor(currentIndex)) {
			doublePagePresentation_.AwaitDimensions(currentIndex, currentIndex + 1);
		} else {
			doublePagePresentation_.UseSinglePage(currentIndex);
		}
		jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.NavigationSnapshot();
		if (resumePendingLoad.has_value()) {
			viewportSnapshot = replayPendingIntents ?
				resumePendingLoad->intentBaseSnapshot : resumePendingLoad->viewportSnapshot;
		} else if (!clipboardMode_) {
			if (clipboardReturnViewport_.has_value() &&
				targetPath == recentImageLoadState_.LoadedPath()) {
				viewportSnapshot = *clipboardReturnViewport_;
			} else {
				viewportSnapshot = recentImageLoadState_.ViewportForSelection(targetPath,
					viewport_.Snapshot(), viewport_.NavigationSnapshot(), recentFiles_);
			}
		}
		// Resolve the snapshot while the previous selected identity is still known. A
		// cancellation here would otherwise make a reversal look like the committed
		// image still owned the live viewport.
		CancelPendingCurrentJpegDimensions();
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		const std::uint64_t loadGeneration = ++currentJpegLoadGeneration_;
		currentJpegHeaderPending_ = false;
		const auto failedLoad = [this] {
			deferredExifDateAction_.Cancel();
			if (!clipboardMode_) {
				recentImageLoadState_.FailLoad(AbsoluteNormalized(fileList_.Current()));
			}
			return false;
		};
		if (!clipboardMode_) recentImageLoadState_.BeginLoad(targetPath, viewportSnapshot);
		ClearCropSelection();
		SelectPictureLevelsForCurrentFile(resumePendingLoad.has_value());
		if (!preserveExifMetadataRequest) {
			metadata_ = {};
			jpegComment_.clear();
			++imageInfoMetadataRevision_;
			if (jpegview_linux::IsJpegPath(source.LogicalPath()) && source.Valid()) {
				exifMetadataRequestGeneration_ = exifMetadataWorker_.Request(source);
				exifMetadataSource_ = source.Key();
				deferredExifDateAction_.Begin(targetPath, source.Key(),
					exifMetadataRequestGeneration_);
			} else {
				exifMetadataWorker_.Cancel();
				exifMetadataRequestGeneration_ = 0;
				exifMetadataSource_ = {};
				deferredExifDateAction_.Cancel();
			}
		}
		ClearTransition();
		std::string errorMessage;

		if (texture_ != nullptr) SDL_DestroyTexture(texture_);
		texture_ = nullptr;
		ClearDisplayTexture();
		displayTextureProtectedKeys_.clear();
		currentDecoded_.reset();
		currentAnimationFrame_ = 0;
		currentDisplayRequest_.reset();
		activeDoublePageRender_.reset();
		doublePagePartnerDisplayKey_.clear();
		doublePagePartnerRequest_.reset();
		deferredCurrentDisplayPreparation_ = false;
		currentPixelsMaterialized_ = false;
		currentPixelsDetachedFromSource_ = false;
		materializedProcessing_ = {};
		materializedAutoContrast_ = false;
		animationFrames_.clear();
		correctionBase_ = {};
		correctionBaseValid_ = false;
		image_ = {};
		imageModified_ = false;
		currentImageRotationQuarterTurns_ = 0;
		currentSpreadRotationValid_ = false;
		modifiedImageRevision_ = 0;
		currentSourcePageDimensions_.reset();
		const Uint32 now = SDL_GetTicks();
		const SDL_Rect imageArea = ImageAreaRect();
		const bool deferForPossibleSpread = IsPotentialDoublePageAnchor(currentIndex);

		bool cachedDisplay = false;
		bool waitingForJpegDimensions = false;
		if (cacheBudget_->Capacity() != 0 && jpegview_linux::IsJpegPath(fileList_.Current())) {
			int sourceWidth = 0;
			int sourceHeight = 0;
			if (CachedJpegDimensions(source, sourceWidth, sourceHeight)) {
				currentSourcePageDimensions_ =
					jpegview_linux::PageDimensions{sourceWidth, sourceHeight};
				image_.width = image_.originalWidth = sourceWidth;
				image_.height = image_.originalHeight = sourceHeight;
				RestoreScaleMode(viewportSnapshot);
				const jpegview_linux::ViewportRect destination = viewport_.Destination(
					sourceWidth, sourceHeight, imageArea.w, imageArea.h);
				currentDisplayRequest_ = jpegview_linux::MakeJpegDisplayImageRequest(
					source, sourceWidth, sourceHeight, destination.width,
					destination.height, autoContrastEnabled_, 0, imageProcessing_);
				currentDisplayRequest_->workClass =
					jpegview_linux::PerfWorkClass::ActiveImageSpread;
				if (currentDisplayRequest_->Valid()) {
					displayTextureProtectedKeys_.insert(currentDisplayRequest_->key);
					cachedDisplay = FindDisplayTexture(currentDisplayRequest_->key) != nullptr;
					if (!cachedDisplay && !deferForPossibleSpread) {
						auto prepared = displayImageCache_.Find(*currentDisplayRequest_);
						if (!prepared) prepared =
							displayImageCache_.RequestAndWait(*currentDisplayRequest_);
						if (prepared) cachedDisplay = CacheDisplayTexture(prepared);
					}
					deferredCurrentDisplayPreparation_ =
						deferForPossibleSpread && !cachedDisplay;
				}
			} else if (source.Valid() &&
				failedJpegDimensionKeys_.find(source.Key()) == failedJpegDimensionKeys_.end()) {
				currentJpegHeaderPending_ = true;
				waitingForJpegDimensions = true;
				RestoreScaleMode(viewportSnapshot);
				pendingImageIntents_.Begin(targetPath, source.Key(), loadGeneration);
				playback_.SetImageReady(false, now);
				TransferActiveSpreadPartnerRequestToCurrentImage(source);
				RequestCurrentJpegDimensions(source, loadGeneration,
					prefetchDirection, startupLoad);
			}
		}

		std::vector<int> animationFrameDelaysMs;
		if (!cachedDisplay && !deferredCurrentDisplayPreparation_ &&
			!waitingForJpegDimensions) {
			jpegview_linux::DecodedImageCache::ImagePtr decoded = imageCache_.Find(source);
			if (!decoded) decoded = imageCache_.FindOrWait(source);
			if (!decoded) {
				auto loaded = std::make_shared<jpegview_linux::DecodedImage>();
				if (!jpegview_linux::DecodeImage(fileList_.Current(), *loaded, errorMessage) ||
					loaded->frames.empty()) {
					const bool passwordFailure =
						jpegview_linux::IsArchiveMemberLocation(fileList_.Current()) &&
						(errorMessage.find("password required") != std::string::npos ||
							errorMessage.find("incorrect archive password") != std::string::npos);
					if (passwordFailure) {
						if (errorMessage.find("incorrect archive password") != std::string::npos) {
							jpegview_linux::ForgetSessionArchivePassword(fileList_.Current());
						}
						failedLoad();
						SetTitle("JPEGView — Open image");
						OpenFileDialog(fileList_.Current().parent_path());
						return true;
					}
					SetTitle(fileList_.Current().filename().string() +
						" — decode failed: " + errorMessage);
					std::cerr << fileList_.Current() << ": " << errorMessage << '\n';
					return failedLoad();
				}
				imageCache_.Store(source, loaded);
				decoded = std::move(loaded);
			}
			currentDecoded_ = decoded;
			animationFrameDelaysMs.reserve(decoded->frames.size());
			for (const jpegview_linux::DecodedFrame& decodedFrame : decoded->frames) {
				animationFrameDelaysMs.push_back(std::max(10, decodedFrame.delayMs));
			}
			const jpegview_linux::DecodedFrame& firstFrame = decoded->frames.front();
			currentSourcePageDimensions_ =
				jpegview_linux::PageDimensions{firstFrame.width, firstFrame.height};
			image_.width = image_.originalWidth = firstFrame.width;
			image_.height = image_.originalHeight = firstFrame.height;
			image_.hasTransparency = firstFrame.hasTransparency;
			RestoreScaleMode(viewportSnapshot);
			const jpegview_linux::ViewportRect destination = viewport_.Destination(
				image_.width, image_.height, imageArea.w, imageArea.h);
			const jpegview_linux::DisplayImageRequest* displayRequest =
				CurrentDisplayRequest(destination.width, destination.height);
			if (displayRequest != nullptr) displayTextureProtectedKeys_.insert(displayRequest->key);
			cachedDisplay = displayRequest != nullptr &&
				FindDisplayTexture(displayRequest->key) != nullptr;
			deferredCurrentDisplayPreparation_ = !cachedDisplay &&
				deferForPossibleSpread && cacheBudget_->Capacity() != 0;
			if (!cachedDisplay && displayRequest != nullptr) {
				if (const auto prepared = displayImageCache_.Find(*displayRequest)) {
					cachedDisplay = CacheDisplayTexture(prepared);
					deferredCurrentDisplayPreparation_ = false;
				}
			}
			const bool needsCurrentPixels = !cachedDisplay &&
				(!deferredCurrentDisplayPreparation_ || decoded->frames.size() > 1);
			if (needsCurrentPixels) {
				if (!MaterializeCurrentPixels()) return failedLoad();
			}
			if (needsCurrentPixels && !UpdateTexture()) return failedLoad();
			playback_.ConfigureImage(std::move(animationFrameDelaysMs), decoded->loopCount,
				decoded->animation, now);
		} else {
			playback_.ConfigureImage({}, 0, false, now);
		}
		cropSelection_.SetImageSize(image_.width, image_.height);
		if (waitingForJpegDimensions) {
			SetPendingHeaderTitle();
		} else {
			SetTitle();
		}
		PrepareThumbnailPreload();
		PrepareImagePrefetch(prefetchDirection);
		if (!clipboardMode_ && !waitingForJpegDimensions) {
			if (!recentImageLoadState_.CommitLoad(targetPath, recentFiles_)) {
				return failedLoad();
			}
			clipboardReturnViewport_.reset();
		}
		RefreshDoublePageRenderState();
		if (!waitingForJpegDimensions) {
			const jpegview_linux::ExifDateActionCompletion deferredAction =
				deferredExifDateAction_.MarkImageCommitted(targetPath, source.Key());
			if (deferredAction.runDeferredAction) TouchCurrentImage(true);
			playback_.SetImageReady(true, SDL_GetTicks());
		}
		return true;
	}

	std::vector<std::size_t> VisibleThumbnailIndices() const {
		std::vector<std::size_t> indices;
		if (!thumbnailPanelVisible_ || fileList_.Empty()) return indices;
		const SDL_Rect panel = ThumbnailPanelRect();
		const int rowHeight = std::max(1, jpegview_linux::ThumbnailRowHeight(
			panel.w, kThumbnailVerticalMargin));
		const std::size_t visibleRows = static_cast<std::size_t>(
			std::max(1, panel.h / rowHeight + 2));
		const std::size_t current = fileList_.CurrentIndex();
		const std::size_t first = current > visibleRows ? current - visibleRows : 0;
		const std::size_t last = std::min(fileList_.Size(), current + visibleRows + 1);
		indices.reserve(last - first);
		for (std::size_t index = first; index < last; ++index) {
			if (jpegview_linux::ThumbnailIndexVisible(fileList_.Size(), current, index,
				panel.h, rowHeight)) indices.push_back(index);
		}
		return indices;
	}

	bool ForegroundSourceWorkPending() const {
		const jpegview_linux::DisplayImageCacheDiagnostics display =
			displayImageCache_.GetDiagnostics();
		const jpegview_linux::DecodedImageCacheDiagnostics decoded = imageCache_.GetDiagnostics();
		return jpegview_linux::ForegroundSourceWorkPending({
			display.foregroundQueued != 0,
			display.foregroundActive != 0,
			decoded.foregroundQueued != 0,
			decoded.foregroundActive != 0,
			decoded.activeSpreadQueued != 0,
			decoded.activeSpreadActive != 0,
			doublePagePresentation_.Phase() ==
				jpegview_linux::DoublePagePresentationPhase::PreparingSpread});
	}

	jpegview_linux::InteractionWorkPlan CurrentInteractionWorkPlan() const {
		return interactionWorkPolicy_.Plan(ForegroundSourceWorkPending(),
			VisibleThumbnailIndices());
	}

	void RetryThumbnail(const jpegview_linux::ThumbnailPreparationResult& result) {
		const jpegview_linux::ThumbnailLoadRequest request{result.fileIndex, result.key,
			result.catalogRevision, result.geometryRevision,
			result.maximumWidth, result.maximumHeight};
		const jpegview_linux::SourceDescriptor* source =
			fileList_.DescriptorAt(result.fileIndex);
		if (result.key.empty() || !thumbnailScheduler_.IsCurrent(request) ||
			result.fileIndex >= fileList_.Files().size() ||
			source == nullptr || source->Key() != result.key) return;
		thumbnailScheduler_.Retry(request);
	}

	void RetryDisplacedThumbnail(
		const jpegview_linux::ThumbnailPreparationAdmission& admission) {
		if (admission.displaced) RetryThumbnail(*admission.displaced);
	}

	void RetryCancelledThumbnails(
		const std::vector<jpegview_linux::ThumbnailPreparationResult>& cancelled) {
		for (const auto& result : cancelled) {
			if (result.cancelled) RetryThumbnail(result);
		}
	}

	void PauseThumbnailPreparation() {
		if (thumbnailUploadRetry_) {
			RetryThumbnail(*thumbnailUploadRetry_);
			thumbnailUploadRetry_.reset();
		}
		RetryCancelledThumbnails(thumbnailPreparation_.Cancel({
			jpegview_linux::PerfWorkClass::VisibleThumbnail,
			jpegview_linux::PerfWorkClass::DistantSpeculation}));
	}

	void DeactivateDisplayPrefetchBatch() {
		if (displayPrefetchBatch_) displayPrefetchBatch_->gate.Deactivate();
	}

	void InvalidateViewportPrefetch() {
		++prefetchViewportRevision_;
		displayPrefetchPlannerWorker_.Cancel();
		activeDisplayPrefetchGeneration_ = 0;
		ReconcileActiveSpreadPartnerSourceRequest();
		if (displayPrefetchBatch_) {
			const std::shared_ptr<DisplayPrefetchBatch> spreadBatch =
				activeSpreadSourceRequest_.has_value() ?
					activeSpreadSourceRequest_->batch.lock() : nullptr;
			const bool activeSpreadRequestStillCurrent =
				spreadBatch == displayPrefetchBatch_;
			if (jpegview_linux::ShouldDeactivateDisplayPrefetchBatch(
				displayPrefetchBatch_->owner,
				jpegview_linux::DisplayPrefetchBatchInvalidation::ViewportChanged,
				activeSpreadRequestStillCurrent)) {
				displayPrefetchBatch_->gate.Deactivate();
			}
		}
	}

	void CancelActiveSpreadPartnerSourceRequest() {
		if (!activeSpreadSourceRequest_.has_value()) return;
		if (const auto batch = activeSpreadSourceRequest_->batch.lock()) {
			if (jpegview_linux::ShouldDeactivateDisplayPrefetchBatch(
				batch->owner,
				jpegview_linux::DisplayPrefetchBatchInvalidation::OwnerChanged, false)) {
				batch->gate.Deactivate();
			}
		}
		imageCache_.CancelActiveSpreadRequest(
			activeSpreadSourceRequest_->source,
			activeSpreadSourceRequest_->dimensionsOnly);
		activeSpreadSourceRequest_.reset();
	}

	void TransferActiveSpreadPartnerRequestToCurrentImage(
		const jpegview_linux::SourceDescriptor& currentSource) {
		if (!activeSpreadSourceRequest_.has_value()) return;
		const jpegview_linux::DecodedImageRequestIdentity partnerRequest{
			activeSpreadSourceRequest_->source.Key(),
			activeSpreadSourceRequest_->dimensionsOnly};
		const jpegview_linux::DecodedImageRequestIdentity currentRequest{
			currentSource.Key(), true};
		if (!jpegview_linux::CanTransferActiveSpreadRequestToCurrentImage(
			partnerRequest, currentRequest)) return;
		if (const auto batch = activeSpreadSourceRequest_->batch.lock()) {
			batch->gate.Deactivate();
		}
		// RequestJpegDimensions below rebinds this source key's completion to
		// the current-load mailbox. Retire only the old batch owner here; it
		// must not cancel the worker request after that rebinding takes effect.
		activeSpreadSourceRequest_.reset();
	}

	std::optional<ActiveSpreadSourceRequest> PendingActiveSpreadPartnerSource() {
		if (clipboardMode_ || fileList_.Empty()) return std::nullopt;
		const std::size_t currentIndex = fileList_.CurrentIndex();
		if (!IsPotentialDoublePageAnchor(currentIndex)) return std::nullopt;
		const auto currentDimensions = PageDimensionsAt(currentIndex);
		if (!currentDimensions.has_value() ||
			currentDimensions->width >= currentDimensions->height ||
			PageDimensionsAt(currentIndex + 1).has_value()) {
			return std::nullopt;
		}
		const fs::path& partner = fileList_.Files()[currentIndex + 1];
		return ActiveSpreadSourceRequest{partner, jpegview_linux::IsJpegPath(partner),
			SourceDescriptorForPath(partner), {}};
	}

	void ReconcileActiveSpreadPartnerSourceRequest() {
		if (!activeSpreadSourceRequest_.has_value()) return;
		const auto pending = PendingActiveSpreadPartnerSource();
		if (!pending.has_value() ||
			pending->filename != activeSpreadSourceRequest_->filename ||
			!jpegview_linux::SameDecodedImageRequest(
				jpegview_linux::DecodedImageRequestIdentity{
					pending->source.Key(), pending->dimensionsOnly},
				jpegview_linux::DecodedImageRequestIdentity{
					activeSpreadSourceRequest_->source.Key(),
					activeSpreadSourceRequest_->dimensionsOnly})) {
			CancelActiveSpreadPartnerSourceRequest();
		}
	}

	void PrepareActiveSpreadPartnerDecode() {
		const auto sourceRequest = PendingActiveSpreadPartnerSource();
		if (!sourceRequest.has_value()) {
			CancelActiveSpreadPartnerSourceRequest();
			return;
		}
		if (activeSpreadSourceRequest_.has_value() &&
			(activeSpreadSourceRequest_->filename != sourceRequest->filename ||
			 !jpegview_linux::SameDecodedImageRequest(
				jpegview_linux::DecodedImageRequestIdentity{
					sourceRequest->source.Key(), sourceRequest->dimensionsOnly},
				jpegview_linux::DecodedImageRequestIdentity{
				activeSpreadSourceRequest_->source.Key(),
				activeSpreadSourceRequest_->dimensionsOnly}))) {
			CancelActiveSpreadPartnerSourceRequest();
		}
		const std::size_t currentIndex = fileList_.CurrentIndex();
		const auto currentDimensions = PageDimensionsAt(currentIndex);
		if (!currentDimensions.has_value()) return;
		const jpegview_linux::SourceDescriptor& partnerSource = sourceRequest->source;

		auto batch = std::make_shared<DisplayPrefetchBatch>();
		batch->owner = jpegview_linux::DisplayPrefetchBatchOwner::ActiveSpread;
		batch->context.currentIndex = currentIndex;
		batch->context.pageCount = fileList_.Size();
		batch->context.currentPageDimensions = currentDimensions;
		batch->context.doublePageMode = {doublePageModeEnabled_, mangaReadingOrderEnabled_};
		batch->context.viewport = viewport_.NavigationSnapshot();
		const SDL_Rect imageArea = ImageAreaRect();
		batch->context.imageAreaWidth = imageArea.w;
		batch->context.imageAreaHeight = imageArea.h;
		displayPrefetchBatch_ = batch;
		ActiveSpreadSourceRequest trackedRequest = *sourceRequest;
		trackedRequest.batch = batch;
		activeSpreadSourceRequest_ = std::move(trackedRequest);
		const auto publishDimensions = [batch](const fs::path& filename,
			bool succeeded, int width, int height) {
			batch->gate.Publish([&] {
				std::lock_guard<std::mutex> lock(batch->mutex);
				if (!succeeded || width <= 0 || height <= 0) {
					batch->failedDecodeFilenames.insert(filename.string());
					return;
				}
				batch->decodedPageDimensions[filename.string()] = {width, height};
			});
		};
		if (sourceRequest->dimensionsOnly) {
			imageCache_.RequestJpegDimensions(partnerSource, publishDimensions,
				jpegview_linux::PerfWorkClass::ActiveImageSpread);
		} else {
			imageCache_.RequestBackground(partnerSource,
				[publishDimensions](const fs::path& filename,
					const jpegview_linux::DecodedImageCache::ImagePtr& decoded) {
				const bool succeeded = decoded && !decoded->frames.empty();
				const int width = succeeded ? decoded->frames.front().width : 0;
				const int height = succeeded ? decoded->frames.front().height : 0;
				publishDimensions(filename, succeeded, width, height);
			}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
		}
	}

	void UpdateInteractionWorkPolicy() {
		interactionWorkPolicy_.SetCaptureActive(dragging_ || cropMouseDragging_ ||
			zoomNavigatorDragging_ || thumbnailPanelResizing_);
		const jpegview_linux::InteractionWorkPlan plan = CurrentInteractionWorkPlan();
		fileListScanWorker_.SetForegroundPending(plan.foregroundPending);
		const bool enteringSuspension = plan.cancelQueuedSpeculation &&
			(!speculativeWorkSuspended_ ||
				(plan.foregroundPending && !lastForegroundPending_));
		if (enteringSuspension) {
			DeactivateDisplayPrefetchBatch();
			displayPrefetchPlannerWorker_.Cancel();
			activeDisplayPrefetchGeneration_ = 0;
			displayImageCache_.Prefetch({});
			imageCache_.Prefetch({}, 0, 0, 0);
			std::set<jpegview_linux::PerfWorkClass> canceledClasses{
				jpegview_linux::PerfWorkClass::NearestNavigationNeighbor,
				jpegview_linux::PerfWorkClass::DistantSpeculation};
			if (plan.foregroundPending) {
				canceledClasses.insert(jpegview_linux::PerfWorkClass::VisibleThumbnail);
			}
			RetryCancelledThumbnails(thumbnailPreparation_.Cancel(canceledClasses));
			prefetchRefreshNeeded_ = true;
			PrepareActiveSpreadPartnerDecode();
		}
		speculativeWorkSuspended_ = plan.cancelQueuedSpeculation;
		lastForegroundPending_ = plan.foregroundPending;
		if (!plan.cancelQueuedSpeculation && prefetchRefreshNeeded_) {
			prefetchRefreshNeeded_ = false;
			PrepareImagePrefetch(pendingPrefetchDirection_);
		}
	}

	void PrepareImagePrefetch(int preferredDirection = 0) {
		pendingPrefetchDirection_ = preferredDirection;
		++prefetchViewportRevision_;
		displayPrefetchPlannerWorker_.Cancel();
		activeDisplayPrefetchGeneration_ = 0;
		// Prefetch replaces its neighbor set, so let an active lens reassert its
		// optional request on the next renderer tick if this call cancels it.
		magnifyingGlassBackgroundRequestedKey_.clear();
		DeactivateDisplayPrefetchBatch();
		const jpegview_linux::InteractionWorkPlan plan = CurrentInteractionWorkPlan();
		if (plan.cancelQueuedSpeculation) {
			displayImageCache_.Prefetch({});
			imageCache_.Prefetch({}, 0, 0, 0);
			prefetchRefreshNeeded_ = true;
			PrepareActiveSpreadPartnerDecode();
			return;
		}
		// Once distant admission resumes, ordinary neighbor planning owns this
		// source again. Retire the paused-only request before replacing its batch.
		CancelActiveSpreadPartnerSourceRequest();
		prefetchRefreshNeeded_ = false;
		if (fileList_.Empty() || clipboardMode_) {
			imageCache_.Prefetch({}, 0, 0, 0);
			return;
		}
		displayImageCache_.Prefetch({});
		const SDL_Rect imageArea = ImageAreaRect();
		auto batch = std::make_shared<DisplayPrefetchBatch>();
		batch->cache = &displayImageCache_;
		batch->context.viewport = viewport_.NavigationSnapshot();
		batch->context.imageAreaWidth = imageArea.w;
		batch->context.imageAreaHeight = imageArea.h;
		batch->context.currentIndex = fileList_.CurrentIndex();
		batch->context.pageCount = fileList_.Size();
		batch->context.doublePageMode = {doublePageModeEnabled_, mangaReadingOrderEnabled_};
		batch->context.currentPageDimensions = PageDimensionsAt(fileList_.CurrentIndex());
		jpegview_linux::DisplayPrefetchPlannerRequest plannerRequest;
		plannerRequest.catalogRevision = fileList_.MutationRevision();
		plannerRequest.descriptorRevision = fileList_.DescriptorRevision();
		plannerRequest.viewportRevision = prefetchViewportRevision_;
		plannerRequest.currentIndex = fileList_.CurrentIndex();
		plannerRequest.pageCount = fileList_.Size();
		plannerRequest.preferredDirection = preferredDirection;
		plannerRequest.viewport = batch->context.viewport;
		plannerRequest.imageAreaWidth = imageArea.w;
		plannerRequest.imageAreaHeight = imageArea.h;
		plannerRequest.doublePageMode = batch->context.doublePageMode;
		plannerRequest.currentPageDimensions = batch->context.currentPageDimensions;
		for (const auto& retained : displayTextureCache_) {
			batch->retainedTextureKeys.insert(retained.first);
		}
		plannerRequest.retainedTextureKeys = batch->retainedTextureKeys;
		displayTextureProtectedKeys_.clear();
		if (currentDisplayRequest_.has_value()) {
			displayTextureProtectedKeys_.insert(currentDisplayRequest_->key);
		}
		if (!doublePagePresentation_.AnchorTextureKey().empty()) {
			displayTextureProtectedKeys_.insert(doublePagePresentation_.AnchorTextureKey());
		}
		if (!doublePagePresentation_.PartnerTextureKey().empty()) {
			displayTextureProtectedKeys_.insert(doublePagePresentation_.PartnerTextureKey());
		}
		const std::size_t maximumNeighborCount = jpegview_linux::DisplayPrefetchCount(
			cacheBudget_->Capacity(), imageArea.w, imageArea.h, fileList_.Files().size());
		plannerRequest.maximumCount = maximumNeighborCount;
		const std::vector<std::size_t> prefetchOrder = jpegview_linux::ImagePrefetchOrder(
			fileList_.Files().size(), fileList_.CurrentIndex(), preferredDirection,
			maximumNeighborCount);
		plannerRequest.neighbors.reserve(prefetchOrder.size());
		for (std::size_t position = 0; position < prefetchOrder.size(); ++position) {
			jpegview_linux::PerfContextScope workContext(
				position < 2 ? jpegview_linux::PerfWorkClass::NearestNavigationNeighbor :
					jpegview_linux::PerfWorkClass::DistantSpeculation,
				jpegview_linux::PerfExecution::EventThread);
			const std::size_t fileIndex = prefetchOrder[position];
			const fs::path& filename = fileList_.Files()[fileIndex];
			const jpegview_linux::SourceDescriptor* sourceEntry = fileList_.DescriptorAt(fileIndex);
			if (sourceEntry == nullptr) continue;
			jpegview_linux::SourceDescriptor source = *sourceEntry;
			if (jpegview_linux::IsJpegPath(filename)) {
				int cachedWidth = 0;
				int cachedHeight = 0;
				if (CachedJpegDimensions(source, cachedWidth, cachedHeight)) {
					source = source.WithImageProperties(cachedWidth, cachedHeight, false);
				}
			}
			batch->priorityByFilename.emplace(filename.string(), position + 1);
			batch->indexByFilename.emplace(filename.string(), fileIndex);
			batch->sourceByFilename.emplace(filename.string(), source);
			const auto savedProcessing = imageProcessingStore_.find(AbsoluteNormalized(filename).string());
			const jpegview_linux::ImageProcessingPreset current{imageProcessing_, autoContrastEnabled_};
			const jpegview_linux::ImageProcessingPreset filePreset =
				jpegview_linux::ResolveImageProcessingForFile(current,
					savedProcessing == imageProcessingStore_.end() ? nullptr : &savedProcessing->second,
					keepPictureLevels_, defaultAutoContrastEnabled_, defaultImageProcessing_);
			jpegview_linux::ImageProcessingParams fileProcessing = filePreset.processing;
			fileProcessing.unsharpRadius = unsharpMaskRadius_;
			fileProcessing.unsharpAmount = 0.0;
			fileProcessing.unsharpThreshold = unsharpMaskThreshold_;
			batch->processingByFilename.emplace(filename.string(), fileProcessing);
			batch->autoContrastByFilename.emplace(filename.string(), filePreset.autoContrast);
			plannerRequest.neighbors.push_back({filename, source, fileIndex, position + 1,
				fileProcessing, filePreset.autoContrast,
				jpegview_linux::IsJpegPath(filename)});
		}
		activeDisplayPrefetchGeneration_ = displayPrefetchPlannerWorker_.Request(
			std::move(plannerRequest));
		displayPrefetchBatch_ = batch;
		imageCache_.Prefetch(fileList_.Files(), fileList_.CurrentIndex(),
			preferredDirection, kDecodedImagePrefetchCount,
			[batch](const fs::path& filename,
				const jpegview_linux::DecodedImageCache::ImagePtr& decoded) {
				if (!decoded || decoded->frames.empty()) {
					std::lock_guard<std::mutex> lock(batch->mutex);
					batch->failedDecodeFilenames.insert(filename.string());
					return;
				}
				const jpegview_linux::DecodedFrame& frame = decoded->frames.front();
				{
					std::lock_guard<std::mutex> lock(batch->mutex);
					batch->decodedPageDimensions[filename.string()] = {frame.width, frame.height};
				}
				jpegview_linux::Viewport viewport;
				viewport.Restore(batch->context.viewport, frame.width, frame.height,
					batch->context.imageAreaWidth, batch->context.imageAreaHeight);
				const jpegview_linux::ViewportRect target = viewport.Destination(
					frame.width, frame.height, batch->context.imageAreaWidth,
					batch->context.imageAreaHeight);
				const auto priority = batch->priorityByFilename.find(filename.string());
				const auto processing = batch->processingByFilename.find(filename.string());
				const auto autoContrast = batch->autoContrastByFilename.find(filename.string());
				const auto source = batch->sourceByFilename.find(filename.string());
				if (priority == batch->priorityByFilename.end() ||
					processing == batch->processingByFilename.end() ||
				autoContrast == batch->autoContrastByFilename.end() ||
					source == batch->sourceByFilename.end()) return;
				jpegview_linux::DisplayImageRequest request =
					jpegview_linux::MakeDisplayImageRequest(source->second, decoded, 0,
						target.width, target.height, autoContrast->second,
						priority->second, processing->second);
				request.workClass = priority->second <= 2 ?
					jpegview_linux::PerfWorkClass::NearestNavigationNeighbor :
					jpegview_linux::PerfWorkClass::DistantSpeculation;
				if (batch->context.currentPageDimensions.has_value()) {
					const auto index = batch->indexByFilename.find(filename.string());
					if (index != batch->indexByFilename.end() &&
						index->second == batch->context.currentIndex + 1 &&
						batch->context.doublePageMode.enabled &&
						jpegview_linux::BuildDoublePageSpread(batch->context.currentIndex,
							batch->context.pageCount, *batch->context.currentPageDimensions,
							jpegview_linux::PageDimensions{frame.width, frame.height},
							batch->context.doublePageMode).has_value()) {
						return;
					}
				}
				if (!request.Valid() ||
					batch->retainedTextureKeys.find(request.key) !=
						batch->retainedTextureKeys.end()) return;
				batch->gate.Publish([&] {
					std::lock_guard<std::mutex> lock(batch->mutex);
					batch->requests.push_back(std::move(request));
					// A late neighbor decode must not replace/cancel the active spread's
					// foreground pair requests in the display cache.
					batch->cache->RequestBackgroundBatch(batch->requests);
				});
			}, [](const fs::path& filename) {
				return !jpegview_linux::IsJpegPath(filename);
			}, 2, [this](std::size_t index) {
				return fileList_.DescriptorAt(index);
			});
	}

	bool UpdateTexture() {
		jpegview_linux::PerfContextScope workContext(
			jpegview_linux::PerfWorkClass::ActiveImageSpread,
			jpegview_linux::PerfExecution::EventThread);
		SDL_Texture* newTexture = CreateTexture(image_);
		if (newTexture == nullptr) {
			std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << '\n';
			return false;
		}
		ClearDisplayTexture();
		if (texture_ != nullptr) SDL_DestroyTexture(texture_);
		texture_ = newTexture;
		return true;
	}

	SDL_Texture* CreateTexture(const Image& source) {
		return CreateTexture(source.bgra, source.width, source.height, source.hasTransparency);
	}

	SDL_Texture* CreateTexture(const std::vector<std::uint8_t>& bgra, int width, int height,
		bool hasTransparency = false) {
		if (width <= 0 || height <= 0 || bgra.size() !=
			static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) return nullptr;
		// Viewer textures are uploaded once and then sampled repeatedly. Static
		// access lets accelerated backends place them for rendering instead of
		// maintaining the lockable staging behavior intended for frequent writes.
		SDL_Texture* result = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
			width, height);
		if (result == nullptr) return nullptr;
		int updateResult = 0;
		{
			jpegview_linux::PerfScopedTimer uploadTimer(
				jpegview_linux::PerfDiagnostics::Instance(),
				jpegview_linux::PerfMetric::TextureUpload, bgra.size(),
				static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height));
			updateResult = SDL_UpdateTexture(result, nullptr, bgra.data(), width * 4);
		}
		if (updateResult != 0) {
			std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << '\n';
			SDL_DestroyTexture(result);
			return nullptr;
		}
		if (SDL_SetTextureBlendMode(result,
			hasTransparency ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE) != 0) {
			std::cerr << "SDL_SetTextureBlendMode failed: " << SDL_GetError() << '\n';
			SDL_DestroyTexture(result);
			return nullptr;
		}
		return result;
	}

	void ClearDisplayTextureCache() {
		for (auto& cached : displayTextureCache_) {
			if (cached.second.texture != nullptr) SDL_DestroyTexture(cached.second.texture);
		}
		displayTextureCache_.clear();
		displayTextureProtectedKeys_.clear();
		cacheBudget_->Release(displayTextureCacheBytes_);
		displayTextureCacheBytes_ = 0;
		displayImageCache_.Clear();
	}

	SDL_Texture* FindDisplayTexture(const std::string& key) {
		const auto found = displayTextureCache_.find(key);
		if (found == displayTextureCache_.end()) return nullptr;
		found->second.lastUsed = ++displayTextureUseCounter_;
		return found->second.texture;
	}

	bool EvictOldestDisplayTexture(const std::string& protectedKey) {
			auto oldest = displayTextureCache_.end();
			for (auto candidate = displayTextureCache_.begin(); candidate != displayTextureCache_.end();
				++candidate) {
				if (candidate->first == protectedKey ||
					displayTextureProtectedKeys_.find(candidate->first) !=
						displayTextureProtectedKeys_.end()) continue;
				if (oldest == displayTextureCache_.end() ||
					candidate->second.lastUsed < oldest->second.lastUsed) oldest = candidate;
			}
			if (oldest == displayTextureCache_.end()) return false;
			if (oldest->second.texture != nullptr) SDL_DestroyTexture(oldest->second.texture);
			displayTextureCacheBytes_ -= oldest->second.bytes;
			cacheBudget_->Release(oldest->second.bytes);
			displayTextureCache_.erase(oldest);
			return true;
	}

	bool ReserveDisplayTextureBytes(std::size_t bytes, const std::string& protectedKey) {
		while (!cacheBudget_->TryReserve(bytes)) {
			// Display-ready nearest neighbors have precedence: decoded pixels can
			// be recreated in the background, while a ready texture removes work
			// from the latency-sensitive navigation path.
			if (imageCache_.EvictLeastRecentlyUsed() != 0) continue;
			if (EvictOldestDisplayTexture(protectedKey)) continue;
			return false;
		}
		return true;
	}

	bool CacheDisplayTexture(const jpegview_linux::DisplayImageCache::ImagePtr& prepared) {
		if (!prepared || prepared->key.empty()) return false;
		jpegview_linux::PerfContextScope workContext(prepared->workClass,
			jpegview_linux::PerfExecution::EventThread);
		if (prepared->rotationQuarterTurns == 0) QueuePreparedThumbnail(prepared);
		if (FindDisplayTexture(prepared->key) != nullptr) {
			displayImageCache_.Release(prepared->key);
			displayImageCache_.Retire(prepared);
			return true;
		}
		const std::size_t bytes = jpegview_linux::PreparedDisplayImageBytes(*prepared);
		if (bytes == 0 || bytes > cacheBudget_->Capacity()) {
			displayImageCache_.Release(prepared->key);
			displayImageCache_.Retire(prepared);
			return false;
		}
		const std::string protectedKey = currentDisplayRequest_.has_value() ?
			currentDisplayRequest_->key : std::string();
		// Convert the staging reservation into a renderer-texture reservation.
		// The shared pointer keeps pixels alive until SDL_UpdateTexture returns.
		displayImageCache_.Release(prepared->key);
		if (!ReserveDisplayTextureBytes(bytes, protectedKey)) {
			displayImageCache_.Retire(prepared);
			return false;
		}
		SDL_Texture* texture = CreateTexture(prepared->bgra, prepared->width,
			prepared->height, prepared->hasTransparency);
		if (texture == nullptr) {
			cacheBudget_->Release(bytes);
			displayImageCache_.Retire(prepared);
			return false;
		}
		displayTextureCache_.emplace(prepared->key,
			DisplayTextureCacheEntry{texture, bytes, prepared->width, prepared->height,
				prepared->hasTransparency,
				++displayTextureUseCounter_});
		displayTextureCacheBytes_ += bytes;
		displayImageCache_.Retire(prepared);
		return true;
	}

	void TickDisplayTexturePreload() {
		const jpegview_linux::InteractionWorkPlan workPlan = CurrentInteractionWorkPlan();
		const std::size_t uploadLimit =
			doublePagePresentation_.Phase() ==
				jpegview_linux::DoublePagePresentationPhase::PreparingSpread ? 2 :
				kDisplayTextureUploadsPerTick;
		for (const jpegview_linux::DisplayImageCache::ImagePtr& prepared :
			displayImageCache_.TakeCompleted(uploadLimit,
				workPlan.permittedWorkClasses)) {
			if (!prepared) continue;
			const bool cached = CacheDisplayTexture(prepared);
			if (cached) {
				doublePagePresentation_.MarkTextureReady(prepared->key);
			} else if (doublePagePresentation_.MarkTextureFailed(prepared->key)) {
				CancelPendingDoublePageRequests();
				doublePagePartnerRequest_.reset();
				deferredCurrentDisplayPreparation_ = true;
			}
		}
	}

	void ApplyWorkerDetectedSourceChanges() {
		std::vector<jpegview_linux::SourceChangeNotice> changed =
			displayImageCache_.TakeChangedSources();
		std::vector<jpegview_linux::SourceChangeNotice> decodedChanges =
			imageCache_.TakeChangedSources();
		changed.insert(changed.end(),
			std::make_move_iterator(decodedChanges.begin()),
			std::make_move_iterator(decodedChanges.end()));
		for (const jpegview_linux::SourceChangeNotice& notice : changed) {
			ApplySourceChange(notice.previous, notice.observed);
		}
	}

	bool ApplySourceChange(const jpegview_linux::SourceKey& previous,
		const jpegview_linux::SourceDescriptor& observed,
		bool preserveCurrentPixels = false) {
		if (observed.LogicalPath().empty()) return false;
		const fs::path changedPath = AbsoluteNormalized(observed.LogicalPath());
		bool prefetchAffected = false;
		if (displayPrefetchBatch_) {
			for (const auto& source : displayPrefetchBatch_->sourceByFilename) {
				if (source.second.Key() == previous) {
					prefetchAffected = true;
					break;
				}
			}
		}
		const jpegview_linux::SourceRefreshOutcome refresh =
			jpegview_linux::RefreshFileListSource(fileList_, previous, observed);
		if (!refresh.applied) return false;
		const jpegview_linux::SourceRefreshDisplayAction displayAction =
			jpegview_linux::ResolveSourceRefreshDisplayAction(refresh, preserveCurrentPixels);
		const bool orderChanged = refresh.orderChanged;
		bool spreadAffected = orderChanged;
		if (refresh.previousIndex.has_value()) {
			const std::size_t changedIndex = *refresh.previousIndex;
			if (activeDoublePageRender_.has_value() &&
				jpegview_linux::DoublePageSpreadAffectedBySourceRefresh(
					activeDoublePageRender_->layout, changedIndex, orderChanged)) {
				spreadAffected = true;
			}
			if (doublePagePresentation_.Phase() !=
					jpegview_linux::DoublePagePresentationPhase::SinglePage &&
				(doublePagePresentation_.AnchorIndex() == changedIndex ||
					doublePagePresentation_.PartnerIndex() == changedIndex)) {
				spreadAffected = true;
			}
		}
		if (activeSpreadSourceRequest_.has_value() &&
			AbsoluteNormalized(activeSpreadSourceRequest_->filename) == changedPath) {
			spreadAffected = true;
		}
		if (activeDoublePageRender_.has_value() &&
			AbsoluteNormalized(activeDoublePageRender_->partnerSpec.filename) == changedPath) {
			spreadAffected = true;
		}
		if (orderChanged) prefetchAffected = true;
		if (prefetchAffected || spreadAffected) {
			DeactivateDisplayPrefetchBatch();
			displayPrefetchBatch_.reset();
		}
		PrepareThumbnailPreload();
		if (displayAction == jpegview_linux::SourceRefreshDisplayAction::PreserveCurrentPixels &&
			!fileList_.Empty()) {
			currentPixelsDetachedFromSource_ = true;
			currentDisplayRequest_.reset();
			deferredCurrentDisplayPreparation_ = false;
			++modifiedImageRevision_;
			(void)UpdateTexture();
		}
		if (displayAction == jpegview_linux::SourceRefreshDisplayAction::ReloadCurrent &&
			!fileList_.Empty()) {
			LoadCurrent();
			return true;
		}
		if (spreadAffected) {
			CancelActiveSpreadPartnerSourceRequest();
			CancelPendingDoublePageRequests();
			const std::size_t currentIndex = fileList_.Empty() ? 0 : fileList_.CurrentIndex();
			doublePagePresentation_.UseSinglePage(currentIndex);
			doublePagePartnerRequest_.reset();
			displayTextureProtectedKeys_.erase(doublePagePartnerDisplayKey_);
			doublePagePartnerDisplayKey_.clear();
			if (activeDoublePageRender_.has_value()) {
				const SDL_Rect area = ImageAreaRect();
				const jpegview_linux::ViewportSnapshot snapshot = viewport_.Snapshot();
				activeDoublePageRender_.reset();
				viewport_.Restore(snapshot, image_.width, image_.height, area.w, area.h);
				currentDisplayRequest_.reset();
			}
		}
		if (prefetchAffected || spreadAffected) {
			PrepareImagePrefetch(pendingPrefetchDirection_);
		}
		if (spreadAffected) RefreshDoublePageRenderState();
		if (orderChanged) SetTitle();
		return true;
	}

	void ClearDisplayTexture() {
		if (displayTexture_ != nullptr) {
			SDL_DestroyTexture(displayTexture_);
			displayTexture_ = nullptr;
		}
		displayTextureWidth_ = 0;
		displayTextureHeight_ = 0;
		displayImage_ = {};
	}

	const jpegview_linux::DisplayImageRequest* CurrentDisplayRequest(int width, int height) {
		if (imageModified_ || currentPixelsDetachedFromSource_ || fileList_.Empty() ||
			width <= 0 || height <= 0 ||
			(currentDecoded_ && currentAnimationFrame_ >= currentDecoded_->frames.size())) {
			currentDisplayRequest_.reset();
			return nullptr;
		}
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		if (!currentDisplayRequest_.has_value() ||
			currentDisplayRequest_->source.Key() != source.Key() ||
			currentDisplayRequest_->decoded != currentDecoded_ ||
			currentDisplayRequest_->frameIndex != currentAnimationFrame_ ||
			currentDisplayRequest_->targetWidth != width ||
			currentDisplayRequest_->targetHeight != height ||
			currentDisplayRequest_->autoContrast != autoContrastEnabled_ ||
			!jpegview_linux::EqualImageProcessing(currentDisplayRequest_->processing,
				imageProcessing_)) {
			if (currentDecoded_) {
				currentDisplayRequest_ = jpegview_linux::MakeDisplayImageRequest(
					source, currentDecoded_, currentAnimationFrame_, width, height,
					autoContrastEnabled_, 0, imageProcessing_);
			} else {
				currentDisplayRequest_ = jpegview_linux::MakeJpegDisplayImageRequest(
					source, image_.originalWidth, image_.originalHeight, width, height,
					autoContrastEnabled_, 0, imageProcessing_);
			}
			currentDisplayRequest_->workClass =
				jpegview_linux::PerfWorkClass::ActiveImageSpread;
		}
		return currentDisplayRequest_->Valid() ? &*currentDisplayRequest_ : nullptr;
	}

	SDL_Texture* DisplayTextureFor(int width, int height) {
		std::string previousDisplayKey;
		if (currentDisplayRequest_.has_value() &&
			currentDisplayRequest_->decoded == currentDecoded_) {
			previousDisplayKey = currentDisplayRequest_->key;
		}
		const jpegview_linux::DisplayImageRequest* request = CurrentDisplayRequest(width, height);
		if (request != nullptr) {
			if (SDL_Texture* cached = FindDisplayTexture(request->key)) return cached;
			if (cacheBudget_->Capacity() != 0) displayImageCache_.Request(*request);
			if (texture_ == nullptr && !previousDisplayKey.empty() &&
				previousDisplayKey != request->key) {
				if (SDL_Texture* previous = FindDisplayTexture(previousDisplayKey)) return previous;
			}
			if (texture_ == nullptr && !deferredCurrentDisplayPreparation_) {
				jpegview_linux::PerfContextScope workContext(
					jpegview_linux::PerfWorkClass::ActiveImageSpread,
					jpegview_linux::PerfExecution::EventThread);
				if (MaterializeCurrentPixels()) texture_ = CreateTexture(image_);
			}
			// The fallback lets SDL scale the source texture while workers prepare
			// the high-quality frame. No CPU resize runs on the renderer thread.
			return texture_;
		}
		if (texture_ == nullptr || image_.width <= 0 || image_.height <= 0) return texture_;
		if (width >= image_.width && height >= image_.height) return texture_;
		if (displayTexture_ != nullptr && displayTextureWidth_ == width && displayTextureHeight_ == height) {
			return displayTexture_;
		}

		ClearDisplayTexture();
		jpegview_linux::PerfContextScope workContext(
			jpegview_linux::PerfWorkClass::ActiveImageSpread,
			jpegview_linux::PerfExecution::EventThread);
		displayImage_ = image_;
		{
			jpegview_linux::PerfScopedTimer resamplingTimer(
				jpegview_linux::PerfDiagnostics::Instance(), jpegview_linux::PerfMetric::Resampling);
			if (!displayImage_.Resize(width, height)) {
				displayImage_ = {};
				return texture_;
			}
		}
		displayTexture_ = CreateTexture(displayImage_);
		if (displayTexture_ == nullptr) {
			displayImage_ = {};
			return texture_;
		}
		displayTextureWidth_ = width;
		displayTextureHeight_ = height;
		return displayTexture_;
	}

	void ClearTransition() {
		if (transitionTexture_ != nullptr) {
			SDL_DestroyTexture(transitionTexture_);
			transitionTexture_ = nullptr;
		}
		transitionImage_ = {};
		transitionStartTick_ = 0;
	}

	void DestroyThumbnailTextures() {
		for (auto& cached : thumbnailCache_) {
			if (cached.second.texture != nullptr) SDL_DestroyTexture(cached.second.texture);
		}
		thumbnailCache_.clear();
	}

	void ClearThumbnailCache() {
		thumbnailPreparation_.Clear();
		thumbnailUploadRetry_.reset();
		DestroyThumbnailTextures();
		thumbnailScheduler_.Clear();
		thumbnailCatalogRevisionTracker_.Reset();
	}

	void EvictThumbnails(const std::vector<jpegview_linux::SourceKey>& keys) {
		(void)jpegview_linux::EraseThumbnailCacheEntries(thumbnailCache_, keys,
			[](ThumbnailCacheEntry& cached) {
				if (cached.texture != nullptr) SDL_DestroyTexture(cached.texture);
			});
	}

	void PrepareThumbnailPreload() {
		const std::uint64_t listRevision = fileList_.MutationRevision();
		const std::uint64_t descriptorRevision = fileList_.DescriptorRevision();
		if (thumbnailCatalogRevisionTracker_.NeedsUpdate(listRevision, descriptorRevision)) {
			std::vector<jpegview_linux::SourceKey> keys;
			keys.reserve(fileList_.Files().size());
			for (std::size_t index = 0; index < fileList_.Files().size(); ++index) {
				const jpegview_linux::SourceDescriptor* source = fileList_.DescriptorAt(index);
				keys.push_back(source == nullptr ? jpegview_linux::SourceKey() : source->Key());
			}
			const std::uint64_t previousCatalogRevision = thumbnailScheduler_.CatalogRevision();
			EvictThumbnails(thumbnailScheduler_.SetSourceCatalog(std::move(keys)));
			thumbnailCatalogRevisionTracker_.MarkUpdated(listRevision, descriptorRevision);
			if (thumbnailScheduler_.CatalogRevision() != previousCatalogRevision) {
				thumbnailPreparation_.Clear();
				thumbnailUploadRetry_.reset();
			}
		}
		EvictThumbnails(thumbnailScheduler_.SetSourceCurrent(fileList_.CurrentIndex()));
		if (!thumbnailPanelVisible_ || fileList_.Empty()) {
			PauseThumbnailPreparation();
			return;
		}
		const SDL_Rect panel = ThumbnailPanelRect();
		const int rowHeight = jpegview_linux::ThumbnailRowHeight(
			panel.w, kThumbnailVerticalMargin);
		const int targetWidth = panel.w;
		const int targetHeight = rowHeight - kThumbnailVerticalMargin * 2 - 1;
		if (targetWidth != thumbnailTargetWidth_ || targetHeight != thumbnailTargetHeight_) {
			thumbnailPreparation_.Clear();
			thumbnailUploadRetry_.reset();
			DestroyThumbnailTextures();
			(void)thumbnailScheduler_.SetGeometry(targetWidth, targetHeight);
			thumbnailTargetWidth_ = targetWidth;
			thumbnailTargetHeight_ = targetHeight;
		}
	}

	void QueuePreparedThumbnail(
		const jpegview_linux::DisplayImageCache::ImagePtr& prepared) {
		if (!thumbnailPanelVisible_ || autoContrastEnabled_ || !prepared ||
			prepared->filename.empty() || prepared->bgra.empty()) return;
		if (!jpegview_linux::CanReuseDisplayPixelsForThumbnail(
			prepared->width, prepared->height, kMaximumThumbnailSourcePixels)) return;
		const jpegview_linux::SourceKey key = prepared->source.Key();
		if (thumbnailCache_.find(key) != thumbnailCache_.end()) return;
		const std::optional<std::size_t> found = fileList_.IndexOf(prepared->filename);
		if (!found.has_value()) return;
		const std::size_t index = *found;
		const jpegview_linux::SourceDescriptor* listedSource = fileList_.DescriptorAt(index);
		if (listedSource == nullptr || listedSource->Key() != key) return;
		const std::size_t current = fileList_.CurrentIndex();
		const std::size_t distance = index > current ? index - current : current - index;
		const std::size_t priority = distance * 2 + (index > current ? 1 : 0);
		const SDL_Rect panel = ThumbnailPanelRect();
		const int rowHeight = jpegview_linux::ThumbnailRowHeight(
			panel.w, kThumbnailVerticalMargin);
		const jpegview_linux::PerfWorkClass workClass =
			jpegview_linux::ThumbnailIndexVisible(fileList_.Size(), current, index,
				panel.h, rowHeight) ? jpegview_linux::PerfWorkClass::VisibleThumbnail :
				jpegview_linux::PerfWorkClass::DistantSpeculation;
		const jpegview_linux::InteractionWorkPlan workPlan = CurrentInteractionWorkPlan();
		if (workClass == jpegview_linux::PerfWorkClass::VisibleThumbnail ?
			!workPlan.AllowsThumbnail(index) : !workPlan.Allows(workClass)) return;
		jpegview_linux::ThumbnailPreparationRequest request;
		request.key = key;
		request.sourceDescriptor = *listedSource;
		request.source = prepared;
		request.maximumWidth = panel.w;
		request.maximumHeight = rowHeight - kThumbnailVerticalMargin * 2 - 1;
		request.priority = priority;
		request.workClass = workClass;
		request.fileIndex = index;
		request.catalogRevision = thumbnailScheduler_.CatalogRevision();
		request.geometryRevision = thumbnailScheduler_.GeometryRevision();
		RetryDisplacedThumbnail(thumbnailPreparation_.Request(request));
	}

	void TickThumbnailPreload(bool allowIndependentDecode) {
		if (!thumbnailPanelVisible_) return;
		const jpegview_linux::InteractionWorkPlan workPlan = CurrentInteractionWorkPlan();
		const Uint32 now = SDL_GetTicks();
		const SDL_Rect panel = ThumbnailPanelRect();
		const int rowHeight = jpegview_linux::ThumbnailRowHeight(panel.w, kThumbnailVerticalMargin);
		const int targetWidth = panel.w;
		const int targetHeight = rowHeight - kThumbnailVerticalMargin * 2 - 1;
		if (targetWidth != thumbnailTargetWidth_ || targetHeight != thumbnailTargetHeight_) {
			PrepareThumbnailPreload();
			return;
		}
		const auto resultIsCurrent = [&](const jpegview_linux::ThumbnailPreparationResult& result,
			std::size_t* activeIndex = nullptr) {
			const jpegview_linux::SourceDescriptor* source =
				fileList_.DescriptorAt(result.fileIndex);
			if (result.key.empty() || !jpegview_linux::ThumbnailPreparationResultMatches(result,
				thumbnailScheduler_.CatalogRevision(), thumbnailScheduler_.GeometryRevision(),
				result.fileIndex, result.key, targetWidth, targetHeight) ||
				result.fileIndex >= fileList_.Files().size() || source == nullptr ||
				source->Key() != result.key) return false;
			if (activeIndex != nullptr) *activeIndex = result.fileIndex;
			return true;
		};
		const auto tryUploadPending = [&]() {
			if (!thumbnailUploadRetry_) return false;
			std::size_t activeIndex = 0;
			if (!resultIsCurrent(*thumbnailUploadRetry_, &activeIndex)) {
				thumbnailUploadRetry_.reset();
				return false;
			}
			const bool uploadPermitted = thumbnailUploadRetry_->workClass ==
				jpegview_linux::PerfWorkClass::VisibleThumbnail ?
				workPlan.AllowsThumbnail(activeIndex) :
				workPlan.Allows(thumbnailUploadRetry_->workClass);
			if (!uploadPermitted) {
				RetryThumbnail(*thumbnailUploadRetry_);
				thumbnailUploadRetry_.reset();
				return false;
			}
			if (thumbnailUploadRetry_->cancelled) {
				thumbnailScheduler_.Retry({activeIndex, thumbnailUploadRetry_->key,
					thumbnailUploadRetry_->catalogRevision,
					thumbnailUploadRetry_->geometryRevision,
					thumbnailUploadRetry_->maximumWidth,
					thumbnailUploadRetry_->maximumHeight});
				thumbnailUploadRetry_.reset();
				return true;
			}
			if (!thumbnailUploadRetry_->image ||
				thumbnailUploadRetry_->image->key != thumbnailUploadRetry_->key) {
				thumbnailUploadRetry_.reset();
				return false;
			}
			if (static_cast<std::int32_t>(now - thumbnailUploadRetryTick_) < 0) return true;
			const jpegview_linux::PreparedThumbnailImage& prepared =
				*thumbnailUploadRetry_->image;
			jpegview_linux::PerfContextScope workContext(prepared.workClass,
				jpegview_linux::PerfExecution::EventThread);
			ThumbnailCacheEntry cached;
			cached.texture = CreateTexture(prepared.bgra, prepared.width,
				prepared.height, prepared.hasTransparency);
			if (cached.texture == nullptr) {
				thumbnailUploadRetryTick_ = now + 50;
				return true;
			}
			cached.width = prepared.width;
			cached.height = prepared.height;
			cached.hasTransparency = prepared.hasTransparency;
			thumbnailCache_.insert_or_assign(thumbnailUploadRetry_->key, std::move(cached));
			EvictThumbnails(thumbnailScheduler_.Store(thumbnailUploadRetry_->key));
			thumbnailUploadRetry_.reset();
			return true;
		};
		if (thumbnailUploadRetry_ && tryUploadPending()) return;
		const auto completed = thumbnailPreparation_.TakeCompleted(1,
			workPlan.permittedWorkClasses);
		if (!completed.empty()) {
			const jpegview_linux::ThumbnailPreparationResult& result = completed.front();
			if (!result.observedSource.LogicalPath().empty()) {
				(void)ApplySourceChange(result.key, result.observedSource);
			}
			std::size_t activeIndex = 0;
			if (resultIsCurrent(result, &activeIndex)) {
				if (result.cancelled) {
					thumbnailScheduler_.Retry({activeIndex, result.key,
						result.catalogRevision, result.geometryRevision,
						result.maximumWidth, result.maximumHeight});
				} else if (!result.image) {
					thumbnailScheduler_.Fail({activeIndex, result.key,
						result.catalogRevision, result.geometryRevision,
						result.maximumWidth, result.maximumHeight}, now);
				} else if (thumbnailCache_.find(result.key) != thumbnailCache_.end()) {
					EvictThumbnails(thumbnailScheduler_.Store(result.key));
				} else {
					thumbnailUploadRetry_ = result;
					thumbnailUploadRetryTick_ = now;
					(void)tryUploadPending();
					return;
				}
			}
		}
		if (!allowIndependentDecode || thumbnailPreparation_.HasPendingWork()) return;
		const std::vector<jpegview_linux::ThumbnailLoadRequest> requests =
			thumbnailScheduler_.TakeNext(now, 1, [&workPlan](
				const jpegview_linux::ThumbnailLoadRequest& pending) {
				return workPlan.AllowsThumbnail(pending.fileIndex) ||
					workPlan.Allows(jpegview_linux::PerfWorkClass::DistantSpeculation);
			});
		if (requests.empty()) return;
		const jpegview_linux::ThumbnailLoadRequest& request = requests.front();
		const jpegview_linux::SourceDescriptor* source =
			fileList_.DescriptorAt(request.fileIndex);
		if (request.fileIndex >= fileList_.Files().size() || source == nullptr ||
			source->Key() != request.key) return;
		const auto cached = thumbnailCache_.find(request.key);
		if (cached != thumbnailCache_.end() && cached->second.texture != nullptr) {
			EvictThumbnails(thumbnailScheduler_.Store(request.key));
			return;
		}
		const std::size_t current = fileList_.CurrentIndex();
		const std::size_t distance = request.fileIndex > current ?
			request.fileIndex - current : current - request.fileIndex;
		const std::size_t priority = distance * 2 + (request.fileIndex > current ? 1 : 0);
		const jpegview_linux::PerfWorkClass workClass =
			jpegview_linux::ThumbnailIndexVisible(fileList_.Size(), current,
				request.fileIndex, panel.h, rowHeight) ?
			jpegview_linux::PerfWorkClass::VisibleThumbnail :
			jpegview_linux::PerfWorkClass::DistantSpeculation;
		jpegview_linux::ThumbnailPreparationRequest fileRequest;
		fileRequest.key = request.key;
		fileRequest.sourceDescriptor = *source;
		fileRequest.maximumWidth = targetWidth;
		fileRequest.maximumHeight = targetHeight;
		fileRequest.priority = priority;
		fileRequest.workClass = workClass;
		fileRequest.logicalSource = source->LogicalPath();
		fileRequest.catalogRevision = request.catalogRevision;
		fileRequest.geometryRevision = request.geometryRevision;
		fileRequest.fileIndex = request.fileIndex;
		const jpegview_linux::ThumbnailPreparationAdmission admission =
			thumbnailPreparation_.Request(fileRequest);
		RetryDisplacedThumbnail(admission);
		if (!admission) {
			thumbnailScheduler_.Retry(request);
		}
	}

	void StartTransition(const Image& previousImage) {
		ClearTransition();
		if (transitionEffect_ == IDM_EFFECT_NONE || previousImage.width <= 0 || previousImage.height <= 0) return;
		transitionImage_ = previousImage;
		transitionTexture_ = CreateTexture(transitionImage_);
		if (transitionTexture_ == nullptr) {
			transitionImage_ = {};
			return;
		}
		SDL_SetTextureBlendMode(transitionTexture_, SDL_BLENDMODE_BLEND);
		const SDL_Rect imageArea = ImageAreaRect();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			image_.width, image_.height, imageArea.w, imageArea.h);
		if (SDL_Texture* current = DisplayTextureFor(destination.width, destination.height)) {
			SDL_SetTextureBlendMode(current, SDL_BLENDMODE_BLEND);
		}
		transitionStartTick_ = SDL_GetTicks();
	}

	bool TransformImage(Image& image, int command) {
		switch (command) {
		case IDM_ROTATE_90: return image.Rotate(true);
		case IDM_ROTATE_270: return image.Rotate(false);
		case IDM_MIRROR_H: return image.Mirror(true);
		case IDM_MIRROR_V: return image.Mirror(false);
		default: return false;
		}
	}

	void ApplyTransform(int command) {
		if (currentJpegHeaderPending_) {
			const jpegview_linux::SourceDescriptor source =
				SourceDescriptorForPath(fileList_.Current());
			const fs::path currentPath = AbsoluteNormalized(fileList_.Current());
			if (!pendingImageIntents_.QueueTransform(currentPath, source.Key(),
				currentJpegLoadGeneration_, command)) {
				SetPendingImageIntentLimitTitle();
			}
			return;
		}
		if (!MaterializeCurrentPixels()) return;
		ClearCropSelection();
		const bool canKeepSpreadRotation = currentSpreadRotationValid_ || !imageModified_;
		const jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.Snapshot();
		if (!TransformImage(image_, command) ||
			(correctionBaseValid_ && !TransformImage(correctionBase_, command)) || !UpdateTexture()) {
			SetTitle("Image transform failed");
			return;
		}
		imageModified_ = true;
		if (command == IDM_ROTATE_90 || command == IDM_ROTATE_270) {
			const int turns = command == IDM_ROTATE_90 ? 1 : 3;
			currentImageRotationQuarterTurns_ =
				(currentImageRotationQuarterTurns_ + turns) % 4;
			currentSpreadRotationValid_ = canKeepSpreadRotation;
		} else {
			currentSpreadRotationValid_ = false;
		}
		++modifiedImageRevision_;
		RestoreScaleMode(viewportSnapshot);
	}

	void ToggleAutoContrast() {
		autoContrastEnabled_ = !autoContrastEnabled_;
		defaultAutoContrastEnabled_ = autoContrastEnabled_;
		RefreshPictureLevels();
		SaveSettings();
	}

	void RefreshPictureLevels(bool refreshNeighbors = true) {
		currentDisplayRequest_.reset();
		if (imageModified_ || currentPixelsDetachedFromSource_ ||
			cacheBudget_->Capacity() == 0) {
			if (!MaterializeCurrentPixels() || !UpdateTexture()) {
				SetTitle("Picture-level processing failed: could not update the image");
				return;
			}
		}
		if (refreshNeighbors) PrepareImagePrefetch();
	}

	void ApplyLosslessJpegTransform(int command) {
		if (fileList_.Empty() || clipboardMode_) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Lossless transforms are unavailable for archive images");
			return;
		}
		const std::string extension = Lower(fileList_.Current().extension().string());
		if (extension != ".jpg" && extension != ".jpeg" && extension != ".jpe") {
			SetTitle("Lossless JPEG transformation requires a JPEG image");
			return;
		}
		std::optional<jpegview_linux::LosslessJpegOperation> operation;
		switch (command) {
		case IDM_ROTATE_90_LOSSLESS:
		case IDM_ROTATE_90_LOSSLESS_CONFIRM:
			operation = jpegview_linux::LosslessJpegOperation::Rotate90;
			break;
		case IDM_ROTATE_270_LOSSLESS:
		case IDM_ROTATE_270_LOSSLESS_CONFIRM:
			operation = jpegview_linux::LosslessJpegOperation::Rotate270;
			break;
		case IDM_ROTATE_180_LOSSLESS:
			operation = jpegview_linux::LosslessJpegOperation::Rotate180;
			break;
		case IDM_MIRROR_H_LOSSLESS:
			operation = jpegview_linux::LosslessJpegOperation::FlipHorizontal;
			break;
		case IDM_MIRROR_V_LOSSLESS:
			operation = jpegview_linux::LosslessJpegOperation::FlipVertical;
			break;
		default:
			break;
		}
		if (!operation.has_value()) return;
		if (!HasExecutable("jpegtran")) {
			SetTitle("Lossless JPEG transformation requires jpegtran");
			return;
		}

		char temporaryDirectoryName[] = "/tmp/jpegview-jpegtran-XXXXXX";
		if (mkdtemp(temporaryDirectoryName) == nullptr) {
			SetTitle("Lossless JPEG transformation failed: cannot create temporary file");
			return;
		}
		const fs::path temporaryDirectory(temporaryDirectoryName);
		const fs::path temporaryFile = temporaryDirectory / "transformed.jpg";
		const jpegview_linux::ExternalCommand transform = jpegview_linux::LosslessJpegCommand(
			*operation, fileList_.Current(), temporaryFile);
		std::string errorMessage;
		const bool transformed = RunProcess(transform, errorMessage);
		if (!transformed) {
			std::error_code removeError;
			fs::remove_all(temporaryDirectory, removeError);
			SetTitle("Lossless JPEG transformation failed: " + errorMessage);
			return;
		}
		if (::rename(temporaryFile.c_str(), fileList_.Current().c_str()) != 0) {
			std::error_code removeError;
			fs::remove_all(temporaryDirectory, removeError);
			SetTitle("Lossless JPEG transformation failed: cannot replace original file");
			return;
		}
		std::error_code removeError;
		fs::remove_all(temporaryDirectory, removeError);
		ReloadAfterFileChange("Applied lossless JPEG transformation");
	}

	void SetTitle(const std::string& title) {
		if (appliedWindowTitle_.Update(title)) {
			SDL_SetWindowTitle(window_, appliedWindowTitle_.Current().c_str());
		}
	}

	void SetPendingHeaderTitle(bool inputLimitReached = false) {
		if (fileList_.Empty()) return;
		std::string title = fileList_.Current().filename().string() + " [" +
			CurrentImagePositionText() + "] — Loading image header";
		if (inputLimitReached || pendingImageIntentLimitReached_) {
			title += " (pending input limit reached)";
		}
		SetTitle(title);
	}

	void SetTitle() {
		if (currentJpegHeaderPending_ && !fileList_.Empty()) {
			SetPendingHeaderTitle();
			return;
		}
		if (fileList_.Empty() || image_.originalWidth <= 0 || image_.originalHeight <= 0) {
			SetTitle("JPEGView");
			return;
		}
		const fs::path& currentPath = fileList_.Current();
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(currentPath);
		jpegview_linux::WindowTitleContext context;
		context.position = CurrentImagePositionText();
		context.currentIndex = fileList_.CurrentIndex();
		context.imageCount = fileList_.Size();
		context.filename = currentPath.filename().string();
		context.filenameStem = currentPath.stem().string();
		context.extension = currentPath.extension().string();
		if (!context.extension.empty() && context.extension.front() == '.') {
			context.extension.erase(context.extension.begin());
		}
		context.fullPath = currentPath.string();
		context.directory = currentPath.parent_path().string();
		context.width = image_.originalWidth;
		context.height = image_.originalHeight;
		if (source.Metadata().hasFileSize) context.fileSize = source.Metadata().fileSize;
		context.applicationName = "JPEGView";
		context.applicationVersion = JPEGVIEW_APP_VERSION;
		std::ostringstream cacheKey;
		cacheKey << windowTitlePattern_.size() << ':' << windowTitlePattern_ << '|'
			<< fileList_.MutationRevision() << ':' << fileList_.DescriptorRevision() << '|'
			<< context.position.size() << ':' << context.position << '|'
			<< context.currentIndex << ':' << context.imageCount << '|'
			<< context.fullPath.size() << ':' << context.fullPath << '|'
			<< context.width << ':' << context.height << '|'
			<< (context.fileSize.has_value() ? std::to_string(*context.fileSize) : "-") << '|'
			<< context.applicationVersion;
		const std::string& title = windowTitleFormatCache_.GetOrBuild(
			cacheKey.str(), [this, &context] {
				return jpegview_linux::FormatWindowTitle(windowTitlePattern_, context);
			});
		SetTitle(title);
	}

	void OpenDroppedFiles(const std::vector<std::string>& droppedFiles,
		DisplayModeLoadPolicy modeLoadPolicy = DisplayModeLoadPolicy::UseDefaults) {
		if (droppedFiles.empty()) {
			return;
		}
		RestoreClipboardImage();
		jpegview_linux::FileList::ScanRequest request =
			jpegview_linux::FileList::InitialScanRequest(droppedFiles,
				fileList_.GetSorting(), fileList_.IsSortedAscending(),
				fileList_.WrapAroundFolder(), fileList_.GetNavigationMode());
		request.expectedRevision = fileList_.MutationRevision();
		request.expectedDescriptorRevision = fileList_.DescriptorRevision();
		SubmitFileListScan(std::move(request), FileListScanHandling::DroppedInputs,
			true, false, 0, {}, modeLoadPolicy);
		if (fileDialogOpen_) SetTitle("JPEGView — Opening selection");
	}

	void CloseFileDialog(bool cancelPendingDrop = true) {
		if (cancelPendingDrop &&
			pendingFileListScanHandling_ == FileListScanHandling::DroppedInputs) {
			fileListScanWorker_.Clear();
			ClearPendingFileListScan();
		}
		if (fileDialogOpen_) SDL_StopTextInput();
		archivePasswordDialog_.Cancel();
		archivePasswordValidationPending_ = false;
		std::fill(archivePasswordPendingValue_.begin(),
			archivePasswordPendingValue_.end(), '\0');
		archivePasswordPendingValue_.clear();
		if (fileDialogDragMode_ != FileDialogDragMode::None) SDL_CaptureMouse(SDL_FALSE);
		fileDialogDragMode_ = FileDialogDragMode::None;
		SDL_SetCursor(SDL_GetDefaultCursor());
		ClearFileDialogPreview();
		fileDialogArchiveLoader_.Clear(++fileDialogArchiveGeneration_);
		++fileDialogSummaryGeneration_;
		fileDialogSummaryLoader_.Request({}, fileDialogSummaryGeneration_);
		fileDialogFileSizeLoader_.Clear(++fileDialogFileSizeGeneration_);
		fileDialogOpen_ = false;
		fileDialogSave_ = false;
		fileDialogParameterBackup_ = false;
		fileDialogParameterRestore_ = false;
		fileDialogLosslessCrop_ = false;
		fileDialogLosslessCropRect_ = {};
		fileDialogFilename_.clear();
		fileDialogModel_.Clear();
		recentFileDialogModel_.Clear();
		recentFileRemovalUndo_.clear();
		fileDialogTab_ = FileDialogTab::Browse;
		fileDialogDirectorySummaries_.clear();
	}

	void OpenLosslessCropDialog() {
		if (fileList_.Empty() || clipboardMode_ || imageModified_ ||
			currentPixelsDetachedFromSource_ ||
			!jpegview_linux::IsJpegPath(fileList_.Current()) || !cropSelection_.HasSelection()) {
			SetTitle("Lossless crop requires an untransformed JPEG selection");
			return;
		}
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Lossless crop is unavailable for archive images");
			return;
		}
		int mcuWidth = 0;
		int mcuHeight = 0;
		std::string errorMessage;
		if (!jpegview_linux::ReadJpegMcuSize(fileList_.Current(), mcuWidth, mcuHeight,
			errorMessage)) {
			SetTitle("Cannot read JPEG crop block size: " + errorMessage);
			return;
		}
		const jpegview_linux::SelectionRect aligned = jpegview_linux::CropSelectionModel::AlignToMcu(
			cropSelection_.Rect(), image_.width, image_.height, mcuWidth, mcuHeight);
		if (!aligned.Valid()) {
			SetTitle("Lossless crop is too small after JPEG block alignment");
			return;
		}
		OpenSaveFileDialog(true);
		if (!fileDialogOpen_) return;
		fileDialogLosslessCrop_ = true;
		fileDialogLosslessCropRect_ = aligned;
		fileDialogFilename_ = fileList_.Current().stem().string() + "_crop.jpg";
		fileDialogMessage_ = "Lossless JPEG crop: " + std::to_string(aligned.Width()) + " x " +
			std::to_string(aligned.Height()) + " pixels (MCU-aligned)";
	}

	void SaveLosslessCropFromDialog() {
		if (!fileDialogLosslessCrop_ || !fileDialogLosslessCropRect_.Valid() ||
			fileDialogFilename_.empty() || fileList_.Empty()) return;
		fs::path filename(fileDialogFilename_);
		if (filename.extension().empty()) filename += ".jpg";
		const std::string extension = Lower(filename.extension().string());
		if (extension != ".jpg" && extension != ".jpeg" && extension != ".jpe") {
			fileDialogMessage_ = "Lossless crop output must use a JPEG extension";
			return;
		}
		const fs::path output = AbsoluteNormalized(fileDialogDirectory_ / filename);
		std::error_code existsError;
		if (fs::exists(output, existsError) && !existsError && !fileDialogOverwriteConfirmed_) {
			fileDialogOverwriteConfirmed_ = true;
			fileDialogMessage_ = "File exists; press ENTER to overwrite or ESC to cancel";
			return;
		}
		if (existsError) {
			fileDialogMessage_ = "Cannot check output file: " + existsError.message();
			return;
		}
		std::string pattern = (output.parent_path() / ".jpegview-crop-XXXXXX").string();
		std::vector<char> temporaryName(pattern.begin(), pattern.end());
		temporaryName.push_back('\0');
		const int descriptor = mkstemp(temporaryName.data());
		if (descriptor < 0) {
			fileDialogMessage_ = "Lossless crop failed: cannot create a temporary output";
			return;
		}
		struct stat existingOutputStatus{};
		mode_t outputMode = 0;
		if (::stat(output.c_str(), &existingOutputStatus) == 0) {
			outputMode = existingOutputStatus.st_mode & 0777;
		} else {
			const mode_t processMask = ::umask(0);
			::umask(processMask);
			outputMode = static_cast<mode_t>(0666 & ~processMask);
		}
		::close(descriptor);
		const fs::path temporary(temporaryName.data());
		const jpegview_linux::SelectionRect bounds = fileDialogLosslessCropRect_;
		const jpegview_linux::ExternalCommand command = jpegview_linux::LosslessJpegCropCommand(
			fileList_.Current(), temporary, bounds.left, bounds.top,
			bounds.Width(), bounds.Height());
		std::string errorMessage;
		if (!RunProcess(command, errorMessage)) {
			std::error_code removeError;
			fs::remove(temporary, removeError);
			fileDialogMessage_ = "Lossless crop failed: " + errorMessage;
			return;
		}
		if (::chmod(temporary.c_str(), outputMode) != 0) {
			std::error_code removeError;
			fs::remove(temporary, removeError);
			fileDialogMessage_ = "Lossless crop failed: cannot set output permissions";
			return;
		}
		if (::rename(temporary.c_str(), output.c_str()) != 0) {
			std::error_code removeError;
			fs::remove(temporary, removeError);
			fileDialogMessage_ = "Lossless crop failed: cannot finalize output";
			return;
		}
		const fs::path source = AbsoluteNormalized(fileList_.Current());
		const std::string savedName = output.filename().string();
		CloseFileDialog();
		if (output == source) {
			ReloadAfterFileChange("Saved lossless crop: " + savedName);
		} else {
			RequestFileListScan(jpegview_linux::FileList::ScanOperation::Reload,
				0, FileListScanHandling::Reload, true, false, {},
				DisplayModeLoadPolicy::PreserveCurrent, {}, "Saved lossless crop: " + savedName);
		}
	}

	void SaveImageFromDialog() {
		if (!fileDialogSave_ || fileDialogFilename_.empty()) return;
		if (fileDialogLosslessCrop_) {
			SaveLosslessCropFromDialog();
			return;
		}
		if (fileDialogParameterBackup_) {
			fs::path filename(fileDialogFilename_);
			const fs::path output = AbsoluteNormalized(fileDialogDirectory_ / filename);
			if (output == AbsoluteNormalized(jpegview_linux::ImageProcessingStorePath())) {
				fileDialogMessage_ = "Choose another name; this is the live parameter database";
				return;
			}
			const fs::path liveDatabase = jpegview_linux::ImageProcessingStorePath();
			std::error_code databaseStatusError;
			const bool liveDatabaseExists = fs::exists(liveDatabase, databaseStatusError);
			jpegview_linux::ImageProcessingStore backupContents;
			if (databaseStatusError || (liveDatabaseExists &&
				!jpegview_linux::LoadImageProcessingStore(liveDatabase, backupContents))) {
				fileDialogMessage_ = "Backup failed: the live parameter database is unreadable";
				return;
			}
			std::error_code existsError;
			if (fs::exists(output, existsError) && !existsError && !fileDialogOverwriteConfirmed_) {
				fileDialogOverwriteConfirmed_ = true;
				fileDialogMessage_ = "File exists; press ENTER to overwrite or ESC to cancel";
				return;
			}
			if (!jpegview_linux::SaveImageProcessingStore(output, backupContents)) {
				fileDialogMessage_ = "Backup failed: could not write the parameter database";
				return;
			}
			const std::string savedName = output.filename().string();
			CloseFileDialog();
			SetTitle("Backed up picture-level parameters: " + savedName);
			return;
		}
		if (!MaterializeCurrentPixels()) {
			fileDialogMessage_ = "Cannot prepare image pixels";
			return;
		}
		fs::path filename(fileDialogFilename_);
		if (filename.extension().empty()) filename += ".jpg";
		const fs::path output = AbsoluteNormalized(fileDialogDirectory_ / filename);
		std::error_code existsError;
		if (fs::exists(output, existsError) && !existsError && !fileDialogOverwriteConfirmed_) {
			fileDialogOverwriteConfirmed_ = true;
			fileDialogMessage_ = "File exists; press ENTER to overwrite or ESC to cancel";
			return;
		}

		Image outputImage = image_;
		if (!fileDialogSaveFullSize_) {
			const int outputWidth = std::max(1, static_cast<int>(std::round(image_.width * viewport_.Zoom())));
			const int outputHeight = std::max(1, static_cast<int>(std::round(image_.height * viewport_.Zoom())));
			if (!outputImage.Resize(outputWidth, outputHeight)) {
				fileDialogMessage_ = "Cannot resize image for screen-size output";
				return;
			}
		}

		jpegview_linux::ImageWriteOptions options;
		std::string errorMessage;
		if (!jpegview_linux::WriteImage(output, outputImage.bgra.data(), outputImage.width,
			outputImage.height, options, errorMessage)) {
			fileDialogMessage_ = "Save failed: " + errorMessage;
			return;
		}

		const std::string savedName = output.filename().string();
		const bool replacedCurrentSource = !fileList_.Empty() &&
			output == AbsoluteNormalized(fileList_.Current());
		const std::optional<std::size_t> outputIndex = fileList_.IndexOf(output);
		const jpegview_linux::SourceKey previousOutputKey = outputIndex.has_value() &&
			fileList_.DescriptorAt(*outputIndex) != nullptr ?
			fileList_.DescriptorAt(*outputIndex)->Key() : jpegview_linux::SourceKey{};
		CloseFileDialog();
		if (replacedCurrentSource) {
			const jpegview_linux::SourceDescriptor observed =
				jpegview_linux::DescribeImageSource(output);
			(void)ApplySourceChange(previousOutputKey, observed, true);
			SetTitle("Saved processed image: " + savedName);
		} else if (outputIndex.has_value()) {
			const jpegview_linux::SourceDescriptor observed =
				jpegview_linux::DescribeImageSource(output);
			(void)ApplySourceChange(previousOutputKey, observed);
			SetTitle("Saved processed image: " + savedName);
		} else {
			SetTitle("Saved processed image: " + savedName);
		}
	}

	void BeginParameterDbRestore(const fs::path& source) {
		const fs::path liveDatabase = AbsoluteNormalized(
			jpegview_linux::ImageProcessingStorePath());
		if (source.empty() || AbsoluteNormalized(source) == liveDatabase) {
			fileDialogMessage_ = "Choose a backup file, not the active parameter database";
			return;
		}
		jpegview_linux::ImageProcessingStore restored;
		if (!jpegview_linux::LoadImageProcessingStore(source, restored)) {
			fileDialogMessage_ = "Restore failed: this is not a valid picture-level database";
			return;
		}
		pendingParameterDbRestore_ = std::move(restored);
		pendingParameterDbRestoreSource_ = source.filename().string();
		const std::size_t entryCount = pendingParameterDbRestore_.size();
		CloseFileDialog();
		RequestConfirmation(kConfirmRestoreParameterDb,
			"Replace the active database with " + std::to_string(entryCount) +
			" saved picture-level entr" + (entryCount == 1 ? "y?" : "ies?"));
	}

	void RestorePendingParameterDb() {
		if (pendingParameterDbRestoreSource_.empty()) return;
		if (!jpegview_linux::SaveImageProcessingStore(
			jpegview_linux::ImageProcessingStorePath(), pendingParameterDbRestore_)) {
			pendingParameterDbRestore_.clear();
			pendingParameterDbRestoreSource_.clear();
			SetTitle("Could not restore the picture-level database");
			return;
		}
		imageProcessingStore_ = std::move(pendingParameterDbRestore_);
		pendingParameterDbRestore_.clear();
		pendingParameterDbRestoreSource_.clear();
		if (!fileList_.Empty() && !keepPictureLevels_) SelectPictureLevelsForCurrentFile();
		RefreshPictureLevels();
		SetTitle("Restored picture-level database");
	}

	void CopyCurrentImage(bool fullSize) {
		if (image_.width <= 0 || image_.height <= 0) return;
		if (!MaterializeCurrentPixels()) return;
		Image copied = image_;
		if (!fullSize) {
			const int outputWidth = std::max(1, static_cast<int>(std::round(image_.width * viewport_.Zoom())));
			const int outputHeight = std::max(1, static_cast<int>(std::round(image_.height * viewport_.Zoom())));
			if (!copied.Resize(outputWidth, outputHeight)) {
				SetTitle("Copy failed: image is too large");
				return;
			}
		}
		std::string errorMessage;
		if (jpegview_linux::CopyImageToClipboard(copied.bgra.data(), copied.width, copied.height, errorMessage)) {
			SetTitle(fullSize ? "Copied original-size image to clipboard" : "Copied displayed image to clipboard");
		} else {
			SetTitle("Copy image failed: " + errorMessage);
		}
	}

	bool CropCurrentSelection() {
		if (!cropSelection_.HasSelection() || image_.width <= 0 || image_.height <= 0) return false;
		if (!MaterializeCurrentPixels()) return false;
		const jpegview_linux::SelectionRect bounds = cropSelection_.Rect();
		const jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.Snapshot();
		Image croppedBase;
		const Image& sourceBase = correctionBaseValid_ ? correctionBase_ : image_;
		if (!sourceBase.CopyCrop(bounds.left, bounds.top, bounds.right, bounds.bottom, croppedBase)) {
			SetTitle("Crop failed: invalid selection or insufficient memory");
			return false;
		}
		croppedBase.originalWidth = croppedBase.width;
		croppedBase.originalHeight = croppedBase.height;
		Image croppedImage;
		if (!croppedBase.CopyCrop(0, 0, croppedBase.width, croppedBase.height, croppedImage)) {
			SetTitle("Crop failed: insufficient memory for the processed image");
			return false;
		}
		if (!croppedImage.ApplyProcessing(imageProcessing_, autoContrastEnabled_)) {
			SetTitle("Crop failed: picture-level processing could not be reapplied");
			return false;
		}
		correctionBase_ = std::move(croppedBase);
		correctionBaseValid_ = true;
		image_ = std::move(croppedImage);
		currentPixelsMaterialized_ = true;
		materializedProcessing_ = imageProcessing_;
		materializedAutoContrast_ = autoContrastEnabled_;
		if (animationFrames_.size() > 1) {
			animationFrames_.clear();
			playback_.ConfigureImage({}, 0, false, SDL_GetTicks());
		}
		imageModified_ = true;
		currentSpreadRotationValid_ = false;
		++modifiedImageRevision_;
		currentDisplayRequest_.reset();
		ClearCropSelection();
		cropSelection_.SetImageSize(image_.width, image_.height);
		if (!UpdateTexture()) {
			SetTitle("Crop applied, but the display texture could not be refreshed");
			return false;
		}
		RestoreScaleMode(viewportSnapshot);
		SetTitle();
		return true;
	}

	void CopyCurrentSelection() {
		if (!cropSelection_.HasSelection() || image_.width <= 0 || image_.height <= 0) return;
		if (!MaterializeCurrentPixels()) return;
		const jpegview_linux::SelectionRect bounds = cropSelection_.Rect();
		Image copied;
		if (!image_.CopyCrop(bounds.left, bounds.top, bounds.right, bounds.bottom, copied)) {
			SetTitle("Copy selection failed: invalid selection or insufficient memory");
			return;
		}
		std::string errorMessage;
		if (jpegview_linux::CopyImageToClipboard(copied.bgra.data(), copied.width,
			copied.height, errorMessage)) {
			SetTitle("Copied selection to clipboard");
		} else {
			SetTitle("Copy selection failed: " + errorMessage);
		}
	}

	void ZoomToSelection() {
		if (!cropSelection_.HasSelection() || image_.width <= 0 || image_.height <= 0) return;
		const jpegview_linux::SelectionRect bounds = cropSelection_.Rect();
		const SDL_Rect imageArea = ImageAreaRect();
		const SDL_Rect pageDestination = CurrentPageScreenRect(imageArea);
		if (pageDestination.w <= 0 || pageDestination.h <= 0) return;
		const double imageScaleX = static_cast<double>(pageDestination.w) / image_.width;
		const double imageScaleY = static_cast<double>(pageDestination.h) / image_.height;
		const double targetZoom = std::min(
			static_cast<double>(std::max(1, imageArea.w)) /
				(std::max(1.0, bounds.Width() * imageScaleX)),
			static_cast<double>(std::max(1, imageArea.h)) /
				(std::max(1.0, bounds.Height() * imageScaleY)));
		const int selectionCenterX = pageDestination.x - imageArea.x + static_cast<int>(std::lround(
			(bounds.left + bounds.right) * 0.5 * imageScaleX));
		const int selectionCenterY = pageDestination.y - imageArea.y + static_cast<int>(std::lround(
			(bounds.top + bounds.bottom) * 0.5 * imageScaleY));
		const auto dimensions = ViewportContentDimensions();
		viewport_.ZoomAt(targetZoom, selectionCenterX, selectionCenterY,
			dimensions.first, dimensions.second, imageArea.w, imageArea.h);
		viewport_.ClampToView(dimensions.first, dimensions.second, imageArea.w, imageArea.h);
		PanViewport(imageArea.w / 2.0 - selectionCenterX,
			imageArea.h / 2.0 - selectionCenterY);
		ShowZoomReadoutTemporarily();
		currentDisplayRequest_.reset();
		PrepareImagePrefetch();
		playback_.NotifyInteraction(SDL_GetTicks());
		SetTitle();
	}

	void SetCropAspect(int width, int height) {
		if (width <= 0 || height <= 0) return;
		cropAspectWidth_ = width;
		cropAspectHeight_ = height;
		cropSelection_.SetAspectRatio(width, height);
		cropSelection_.ReapplyMode(viewport_.Zoom());
		SetSelectionModeEnabled(true);
	}

	void SetSelectionModeEnabled(bool enabled) {
		if (selectionModeEnabled_ == enabled) return;
		selectionModeEnabled_ = enabled;
		SaveSettings();
		UpdateCropCursor(lastMouseX_, lastMouseY_);
	}

	void CopyCurrentPath() {
		if (fileList_.Empty() || clipboardMode_) return;
		std::string errorMessage;
		if (jpegview_linux::CopyTextToClipboard(fileList_.Current().string(), errorMessage)) {
			SetTitle("Copied image path to clipboard");
		} else {
			SetTitle("Copy path failed: " + errorMessage);
		}
	}

	void OpenContainingFolder() {
		if (fileList_.Empty() || clipboardMode_) return;
		const fs::path current = fileList_.Current();
		const fs::path directory = jpegview_linux::IsArchiveMemberLocation(current) ?
			jpegview_linux::ArchiveBackingFile(current).parent_path() : current.parent_path();
		std::string errorMessage;
		bool started = false;
		for (const jpegview_linux::ExternalCommand& command :
			jpegview_linux::OpenContainingFolderCommands(directory)) {
			if (StartDetachedProcess(command, errorMessage)) {
				started = true;
				break;
			}
		}
		if (started) {
			SetTitle("Opened containing folder");
		} else {
			SetTitle("Cannot open containing folder: " + errorMessage);
		}
	}

	void OpenCurrentWith(std::size_t applicationIndex) {
		if (fileList_.Empty() || clipboardMode_ || applicationIndex >= openWithApplications_.size()) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Open With is unavailable for archive images");
			return;
		}
		const jpegview_linux::OpenWithApplication& application = openWithApplications_[applicationIndex];
		const std::optional<jpegview_linux::ExternalCommand> command =
			jpegview_linux::OpenWithCommand(application, fileList_.Current(),
				HasExecutable("x-terminal-emulator"));
		if (!command.has_value()) {
			SetTitle("Cannot open image with " + application.name + ": invalid desktop command");
			return;
		}

		std::string errorMessage;
		const bool started = StartDetachedProcess(*command, errorMessage);
		SetTitle(started ? "Opened image with " + application.name :
			"Cannot open image with " + application.name + ": " + errorMessage);
	}

	void SetAsDefaultViewer() {
		const char* homeValue = std::getenv("HOME");
		if (homeValue == nullptr || *homeValue == '\0') {
			SetTitle("Cannot register default viewer: HOME is not set");
			return;
		}
		const fs::path home(homeValue);
		const auto xdgHome = [&home](const char* variable, const fs::path& fallback) {
			if (const char* value = std::getenv(variable); value != nullptr && *value != '\0') {
				const fs::path configured(value);
				if (configured.is_absolute()) return configured;
			}
			return fallback;
		};
		const fs::path dataHome = xdgHome("XDG_DATA_HOME", home / ".local" / "share");
		const fs::path configHome = xdgHome("XDG_CONFIG_HOME", home / ".config");
		const fs::path executable = CurrentViewerExecutable();
		if (executable.empty()) {
			SetTitle("Cannot determine the running JPEGView executable path");
			return;
		}
		std::string errorMessage;
		if (!jpegview_linux::RegisterDefaultViewer(executable, dataHome, configHome, errorMessage)) {
			SetTitle("Default viewer registration failed: " + errorMessage);
			return;
		}
		SetTitle("JPEGView is now the default viewer for common image formats");
	}

	void PrintCurrentImage() {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Printing images inside archives is unavailable");
			return;
		}
		if (!MaterializeCurrentPixels()) return;
		char temporaryDirectoryName[] = "/tmp/jpegview-print-XXXXXX";
		if (mkdtemp(temporaryDirectoryName) == nullptr) {
			SetTitle("Print failed: cannot create temporary file");
			return;
		}
		const fs::path temporaryDirectory(temporaryDirectoryName);
		const fs::path temporaryFile = temporaryDirectory / "image.png";
		jpegview_linux::ImageWriteOptions options;
		std::string errorMessage;
		const bool written = jpegview_linux::WriteImage(temporaryFile, image_.bgra.data(), image_.width,
			image_.height, options, errorMessage);
		if (!written) {
			std::error_code removeError;
			fs::remove_all(temporaryDirectory, removeError);
			SetTitle("Print failed: " + errorMessage);
			return;
		}
		const bool printed = RunProcess(jpegview_linux::PrintCommand(temporaryFile), errorMessage);
		std::error_code removeError;
		fs::remove_all(temporaryDirectory, removeError);
		SetTitle(printed ? "Sent image to the default printer" : "Print failed: " + errorMessage);
	}

	static bool ParseExifTimestamp(const std::string& value, std::time_t& result) {
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

	static bool SetFileModificationTime(const fs::path& filename, std::time_t timestamp) {
		const timespec times[2] = {
			{0, UTIME_OMIT},
			{timestamp, 0},
		};
		return utimensat(AT_FDCWD, filename.c_str(), times, 0) == 0;
	}

	void SubmitFileListScan(jpegview_linux::FileList::ScanRequest request,
		FileListScanHandling handling, bool force, bool forceImageReload,
		int direction = 0, const fs::path& preferredPath = {},
		DisplayModeLoadPolicy modeLoadPolicy = DisplayModeLoadPolicy::PreserveCurrent,
		const fs::path& markedToggleReturnPath = {},
		std::string completionTitle = {}) {
		if (!force && pendingFileListScanOperation_.has_value() &&
			*pendingFileListScanOperation_ == request.operation &&
			pendingFileListScanHandling_ == handling &&
				pendingFileListScanDirection_ == direction) return;
		if (!preferredPath.empty()) request.selectedPath = preferredPath;
		pendingDroppedScanRequest_.reset();
		if (handling == FileListScanHandling::DroppedInputs) {
			pendingDroppedScanRequest_ = request;
		}
		pendingFileListScanOperation_ = request.operation;
		pendingFileListScanHandling_ = handling;
		pendingFileListScanDirection_ = direction;
		pendingFileListScanForceImageReload_ = forceImageReload;
		pendingFileListScanPreferredPath_ = preferredPath;
		pendingFileListScanModeLoadPolicy_ = modeLoadPolicy;
		pendingMarkedToggleReturnPath_ = markedToggleReturnPath;
		pendingFileListScanCompletionTitle_ = std::move(completionTitle);
		fileListScanGeneration_ = fileListScanWorker_.Request(std::move(request));
	}

	void RequestFileListScan(jpegview_linux::FileList::ScanOperation operation,
		int direction = 0, FileListScanHandling handling = FileListScanHandling::Navigation,
		bool force = false, bool forceImageReload = false,
		const fs::path& preferredPath = {},
		DisplayModeLoadPolicy modeLoadPolicy = DisplayModeLoadPolicy::PreserveCurrent,
		const fs::path& markedToggleReturnPath = {},
		std::string completionTitle = {}) {
		SubmitFileListScan(fileList_.MakeScanRequest(operation, direction), handling,
			force, forceImageReload, direction, preferredPath, modeLoadPolicy,
			markedToggleReturnPath, std::move(completionTitle));
	}

	void ClearPendingFileListScan() {
		pendingFileListScanOperation_.reset();
		pendingDroppedScanRequest_.reset();
		pendingFileListScanHandling_ = FileListScanHandling::None;
		pendingFileListScanDirection_ = 0;
		pendingFileListScanForceImageReload_ = false;
		pendingFileListScanPreferredPath_.clear();
		pendingFileListScanModeLoadPolicy_ = DisplayModeLoadPolicy::PreserveCurrent;
		pendingMarkedToggleReturnPath_.clear();
		pendingFileListScanCompletionTitle_.clear();
	}

	void RetireFileListBoundPresentation() {
		DeactivateDisplayPrefetchBatch();
		displayPrefetchBatch_.reset();
		CancelActiveSpreadPartnerSourceRequest();
		CancelPendingDoublePageRequests();
		const std::size_t currentIndex = fileList_.Empty() ? 0 : fileList_.CurrentIndex();
		doublePagePresentation_.InvalidateForFileListReplacement(currentIndex);
		doublePagePartnerRequest_.reset();
		displayTextureProtectedKeys_.erase(doublePagePartnerDisplayKey_);
		doublePagePartnerDisplayKey_.clear();
		if (activeDoublePageRender_.has_value()) {
			const SDL_Rect area = ImageAreaRect();
			const jpegview_linux::ViewportSnapshot snapshot = viewport_.Snapshot();
			activeDoublePageRender_.reset();
			viewport_.Restore(snapshot, image_.width, image_.height, area.w, area.h);
			currentDisplayRequest_.reset();
		}
	}

	void RefreshFileListConsumers(int preferredDirection = 0) {
		RetireFileListBoundPresentation();
		SetTitle();
		PrepareThumbnailPreload();
		PrepareImagePrefetch(preferredDirection);
		RefreshDoublePageRenderState();
	}

	void TickFileListScan() {
		for (jpegview_linux::FileListScanResult& result : fileListScanWorker_.TakeReady()) {
			if (result.generation != fileListScanGeneration_ ||
				!pendingFileListScanOperation_.has_value()) continue;
			const FileListScanHandling handling = pendingFileListScanHandling_;
			if (!result.error.empty()) {
				ClearPendingFileListScan();
				SetTitle("Directory scan failed: " + result.error);
				if (handling == FileListScanHandling::Startup) {
					deferredExitCode_ = 2;
					quitRequested_ = true;
				}
				continue;
			}
			const bool forceImageReload = pendingFileListScanForceImageReload_;
			const DisplayModeLoadPolicy modeLoadPolicy = pendingFileListScanModeLoadPolicy_;
			const int direction = pendingFileListScanDirection_;
			const std::string completionTitle = pendingFileListScanCompletionTitle_;
			const fs::path previousPath = fileList_.Current();
			const jpegview_linux::SourceKey previousSourceKey =
				SourceDescriptorForPath(previousPath).Key();
			const fs::path markedToggleReturnPath = pendingMarkedToggleReturnPath_;
			const bool targetFound = result.prepared.targetFound;
			if ((result.prepared.operation == jpegview_linux::FileList::ScanOperation::ForwardBoundary &&
				previousPath != result.prepared.sourceSelectedPath) ||
				((result.prepared.operation == jpegview_linux::FileList::ScanOperation::PreviousSibling ||
					result.prepared.operation == jpegview_linux::FileList::ScanOperation::NextSibling) &&
					!previousPath.empty() &&
					previousPath.parent_path() != result.prepared.sourceSelectedPath.parent_path()) ||
				(handling == FileListScanHandling::MarkedToggle &&
					(previousPath != markedToggleReturnPath ||
					fileList_.MarkedToggleTarget() != pendingFileListScanPreferredPath_))) {
				ClearPendingFileListScan();
				continue;
			}

			if (handling == FileListScanHandling::DroppedInputs) {
				if (!result.prepared.completed || !targetFound || result.prepared.replacement.Empty()) {
					ClearPendingFileListScan();
					SetTitle("No supported images in dropped input");
					continue;
				}
			}

			const fs::path preferredPathAtApply = handling == FileListScanHandling::DroppedInputs ||
				result.prepared.operation == jpegview_linux::FileList::ScanOperation::MarkedToggleTarget ?
				fs::path{} : previousPath;
			const bool accepted = fileList_.ApplyPreparedScan(std::move(result.prepared),
				preferredPathAtApply);
			if (!accepted) {
				if (handling == FileListScanHandling::DroppedInputs &&
					pendingDroppedScanRequest_.has_value()) {
					jpegview_linux::FileList::ScanRequest retry = *pendingDroppedScanRequest_;
					retry.expectedRevision = fileList_.MutationRevision();
					retry.expectedDescriptorRevision = fileList_.DescriptorRevision();
					SubmitFileListScan(std::move(retry), handling, true, false, 0, {}, modeLoadPolicy);
					continue;
				}
				if (handling == FileListScanHandling::DroppedInputs) {
					ClearPendingFileListScan();
					SetTitle("Dropped input changed during scan");
					continue;
				}
				const jpegview_linux::FileList::ScanOperation operation = *pendingFileListScanOperation_;
				const fs::path preferredPath = pendingFileListScanPreferredPath_;
				RequestFileListScan(operation, direction, handling, true, forceImageReload,
					preferredPath, modeLoadPolicy, markedToggleReturnPath, completionTitle);
				continue;
			}
			if (fileList_.Empty()) CancelPendingCurrentJpegDimensions();
			if (handling == FileListScanHandling::DroppedInputs) {
				thumbnailCatalogRevisionTracker_.NoteReplacement();
				ClearPendingFileListScan();
				dragging_ = false;
				CloseFileDialog();
				LoadCurrent(0, modeLoadPolicy);
				continue;
			}
			ClearPendingFileListScan();
			if (!targetFound) continue;
			if (handling == FileListScanHandling::MarkedToggle) {
				if (fileList_.CompleteMarkedToggle(markedToggleReturnPath)) LoadCurrent(direction);
				continue;
			}

			const fs::path currentPath = fileList_.Current();
			const jpegview_linux::SourceKey currentSourceKey =
				SourceDescriptorForPath(currentPath).Key();
			const bool dimensionsRequestPending =
				pendingCurrentJpegDimensions_.has_value();
			const bool startupLoadRequest = dimensionsRequestPending &&
				pendingCurrentJpegDimensions_->startupLoad &&
				pendingCurrentJpegDimensions_->loadGeneration == currentJpegLoadGeneration_;
			const bool samePendingStartupLoad = pendingImageIntents_.MatchesStartupLoad(
				handling == FileListScanHandling::Startup, currentJpegHeaderPending_,
				dimensionsRequestPending, startupLoadRequest,
				AbsoluteNormalized(currentPath), currentSourceKey, currentJpegLoadGeneration_);
			const bool currentPathChanged = currentPath != previousPath ||
				(!samePendingStartupLoad && !currentPath.empty() &&
					recentImageLoadState_.LoadedPath() != AbsoluteNormalized(currentPath));
			const bool currentSourceChanged = !currentPath.empty() &&
				currentSourceKey != previousSourceKey;
			if (handling == FileListScanHandling::Startup) {
				if (fileList_.Empty()) {
					const fs::path browseLocation = fileList_.BrowseLocationOnEmpty();
					if (!browseLocation.empty()) {
						SetTitle();
						OpenFileDialog(browseLocation);
					} else {
						std::cerr << "No supported images found.\n";
						deferredExitCode_ = 2;
						quitRequested_ = true;
					}
					continue;
				}
				if (currentPathChanged || currentSourceChanged) {
					if (!LoadCurrent(0, modeLoadPolicy, false, true)) {
						deferredExitCode_ = 1;
						quitRequested_ = true;
						continue;
					}
				} else {
					if (startupImageLoadFailed_ && recentImageLoadState_.LoadedPath().empty()) {
						deferredExitCode_ = 1;
						quitRequested_ = true;
						continue;
					}
					RefreshFileListConsumers();
					if (samePendingStartupLoad) {
						SetPendingHeaderTitle();
					}
				}
				continue;
			}

			if (handling == FileListScanHandling::Reload && fileList_.Empty()) {
				if (quitAfterEmptyScan_) quitRequested_ = true;
				else if (!completionTitle.empty()) SetTitle(completionTitle);
				else SetTitle("No supported images in this folder");
				quitAfterEmptyScan_ = false;
				continue;
			}
			if (handling == FileListScanHandling::Reload) quitAfterEmptyScan_ = false;
			bool refreshed = true;
			if (currentPathChanged || currentSourceChanged || forceImageReload) {
				if (!fileList_.Empty()) refreshed = LoadCurrent(direction);
			} else {
				RefreshFileListConsumers(direction);
			}
			if (refreshed && !completionTitle.empty()) SetTitle(completionTitle);
		}
	}

	void ReloadAfterFileChange(const std::string& completionTitle = {}) {
		if (!fileList_.Empty()) RequestFileListScan(
			jpegview_linux::FileList::ScanOperation::Reload,
			0, FileListScanHandling::Reload, true, true, {},
			DisplayModeLoadPolicy::PreserveCurrent, {}, completionTitle);
	}

	void TouchCurrentImage(bool useExifDate) {
		if (fileList_.Empty() || clipboardMode_) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Cannot change file dates inside an archive");
			return;
		}
		std::time_t timestamp = std::time(nullptr);
		if (useExifDate) {
			const jpegview_linux::SourceDescriptor source =
				SourceDescriptorForPath(fileList_.Current());
			const fs::path targetPath = AbsoluteNormalized(fileList_.Current());
			if (deferredExifDateAction_.MustDeferFor(targetPath, source.Key())) {
				(void)deferredExifDateAction_.Defer(targetPath, source.Key());
				return;
			}
			const std::string& exifDate = !metadata_.acquisitionDate.empty() ? metadata_.acquisitionDate : metadata_.dateTime;
			if (exifDate.empty() || !ParseExifTimestamp(exifDate, timestamp)) {
				SetTitle("Cannot set date: image has no usable EXIF date");
				return;
			}
		}
		if (!SetFileModificationTime(fileList_.Current(), timestamp)) {
			SetTitle("Cannot set image modification date");
			return;
		}
		ReloadAfterFileChange(useExifDate ? "Set modification date to EXIF date" :
			"Set modification date to current date");
	}

	void TouchFolderImagesToExifDate() {
		if (fileList_.Empty() || clipboardMode_) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Cannot change file dates inside an archive");
			return;
		}
		const fs::path directory = fileList_.Current().parent_path();
		int updated = 0;
		std::error_code iteratorError;
		for (const fs::directory_entry& entry : fs::directory_iterator(directory, iteratorError)) {
			if (iteratorError) break;
			if (!entry.is_regular_file(iteratorError) || iteratorError ||
				!jpegview_linux::IsSupportedImagePath(entry.path())) continue;
			jpegview_linux::ExifInfo info;
			std::string comment;
			jpegview_linux::ReadJpegMetadata(entry.path(), info, comment);
			const std::string& exifDate = !info.acquisitionDate.empty() ? info.acquisitionDate : info.dateTime;
			std::time_t timestamp = 0;
			if (!exifDate.empty() && ParseExifTimestamp(exifDate, timestamp) &&
				SetFileModificationTime(entry.path(), timestamp)) {
				++updated;
			}
		}
		ReloadAfterFileChange("Set EXIF dates for " + std::to_string(updated) + " image(s)");
	}

	void SetWallpaper(bool processed) {
		if (fileList_.Empty()) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current()) && !processed) {
			SetTitle("Extract or save the image before setting it as wallpaper");
			return;
		}
		fs::path wallpaperFile = fileList_.Current();
		std::error_code error;
		if (processed) {
			if (!MaterializeCurrentPixels()) return;
			const char* cacheHome = std::getenv("XDG_CACHE_HOME");
			fs::path cacheDirectory;
			if (cacheHome != nullptr && *cacheHome != '\0') {
				cacheDirectory = fs::path(cacheHome) / "jpegview-linux";
			} else {
				const char* home = std::getenv("HOME");
				if (home == nullptr || *home == '\0') {
					SetTitle("Set wallpaper failed: home directory is unknown");
					return;
				}
				cacheDirectory = fs::path(home) / ".cache" / "jpegview-linux";
			}
			fs::create_directories(cacheDirectory, error);
			if (error) {
				SetTitle("Set wallpaper failed: cannot create cache directory");
				return;
			}
			wallpaperFile = cacheDirectory / "wallpaper.png";
			jpegview_linux::ImageWriteOptions options;
			std::string writeError;
			if (!jpegview_linux::WriteImage(wallpaperFile, image_.bgra.data(), image_.width, image_.height,
				options, writeError)) {
				SetTitle("Set wallpaper failed: " + writeError);
				return;
			}
		}

		std::string errorMessage;
		bool applied = false;
		for (const jpegview_linux::ExternalCommandSequence& sequence :
			jpegview_linux::WallpaperCommandSequences(wallpaperFile)) {
			if (!RunProcess(sequence.required, errorMessage)) continue;
			applied = true;
			for (const jpegview_linux::ExternalCommand& command : sequence.afterSuccess) {
				std::string ignoredError;
				RunProcess(command, ignoredError);
			}
			break;
		}
		SetTitle(applied ? "Set desktop wallpaper" : "Set wallpaper failed: install gsettings, feh, or nitrogen");
	}

	void RequestConfirmation(int command, const std::string& message) {
		confirmationCommand_ = command;
		confirmationMessage_ = message;
		confirmationOpen_ = true;
		contextMenuOpen_ = false;
		fileDialogOpen_ = false;
	}

	void MoveCurrentToTrash() {
		if (fileList_.Empty() || clipboardMode_) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Cannot delete an image directly from an archive");
			return;
		}
		const fs::path filename = fileList_.Current();
		std::string errorMessage;
		bool moved = false;
		bool helperAvailable = false;
		for (const jpegview_linux::ExternalCommand& command : jpegview_linux::TrashCommands(filename)) {
			if (!HasExecutable(command.executable)) continue;
			helperAvailable = true;
			if (RunProcess(command, errorMessage)) {
				moved = true;
				break;
			}
		}
		if (!moved && !helperAvailable) {
			// The confirmation dialog has already made this an explicit user
			// action.  This fallback keeps the key binding useful on minimal
			// systems that do not ship a freedesktop trash helper.
			std::error_code removeError;
			moved = fs::remove(filename, removeError);
			if (!moved) errorMessage = "cannot remove file";
		}
		if (!moved) {
			SetTitle("Delete failed: " + errorMessage);
			return;
		}
		quitAfterEmptyScan_ = true;
		RequestFileListScan(jpegview_linux::FileList::ScanOperation::Reload,
			0, FileListScanHandling::Reload, true, true, {},
			DisplayModeLoadPolicy::PreserveCurrent, {}, "Moved image to trash");
	}

	void HandleConfirmationEvents(const SDL_Event& event) {
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				if (confirmationCommand_ == kConfirmRestoreParameterDb) {
					pendingParameterDbRestore_.clear();
					pendingParameterDbRestoreSource_.clear();
				}
				confirmationOpen_ = false;
			} else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
				const int command = confirmationCommand_;
				confirmationOpen_ = false;
				if (command == kConfirmRestoreParameterDb) {
					RestorePendingParameterDb();
				} else if (command == IDM_MOVE_TO_RECYCLE_BIN || command == IDM_MOVE_TO_RECYCLE_BIN_CONFIRM ||
					command == IDM_MOVE_TO_RECYCLE_BIN_CONFIRM_PERMANENT_DELETE) {
					MoveCurrentToTrash();
				}
			}
		}
	}

	void OpenAdvancedConfigurationDialog() {
		advancedConfiguration_.Open(CurrentViewerSettings());
		advancedConfiguration_.SetMessage({});
		const fs::path settingsPath = jpegview_linux::ViewerSettingsPath();
		advancedConfigurationLocation_ = settingsPath.empty() ?
			"Config file path unavailable" : "Config file: " + settingsPath.string();
		SDL_StartTextInput();
	}

	void CloseAdvancedConfigurationDialog() {
		if (!advancedConfiguration_.IsOpen()) return;
		if (advancedConfiguration_.IsEditing()) advancedConfiguration_.CancelEdit();
		advancedConfiguration_.Close();
		SDL_StopTextInput();
		SetTitle();
	}

	bool ApplyAdvancedConfigurationDialog() {
		if (!advancedConfiguration_.IsOpen()) return false;
		if (advancedConfiguration_.IsEditing() && !advancedConfiguration_.CommitEdit()) return false;
		const fs::path settingsPath = jpegview_linux::ViewerSettingsPath();
		if (settingsPath.empty() || !jpegview_linux::SaveViewerSettings(
			settingsPath, advancedConfiguration_.Draft())) {
			advancedConfiguration_.SetMessage("Could not save settings.conf; changes were not applied");
			return false;
		}

		const jpegview_linux::ViewerSettings& settings = advancedConfiguration_.Draft();
		const int previousThumbnailWidth = thumbnailPanelWidth_;
		const bool magnifierChanged = magnifyingGlass_.Width() != settings.magnifyingGlassWidth ||
			magnifyingGlass_.Height() != settings.magnifyingGlassHeight ||
			std::abs(magnifyingGlass_.ZoomLevel() - settings.magnifyingGlassZoomLevel) > 1e-9;
		transparencyPattern_ = settings.transparencyPattern;
		windowTitlePattern_ = settings.windowTitlePattern;
		mangaModeInvertsLeftRight_ = settings.mangaModeInvertsLeftRight;
		spacebarNavigatesImages_ = settings.spacebarNavigatesImages;
		viewport_.SetFitRelativeZoomMode(settings.fitRelativeZoomMode);
		RefreshFitRelativeZoomBase();
		fileList_.SetWrapAroundFolder(settings.folderWrapAround);
		if (fileListBeforeClipboard_) {
			fileListBeforeClipboard_->SetWrapAroundFolder(settings.folderWrapAround);
		}
		showHistogram_ = settings.showHistogram;
		thumbnailPanelWidth_ = settings.thumbnailPanelWidth;
		fileDialogWidth_ = settings.fileDialogWidth;
		fileDialogHeight_ = settings.fileDialogHeight;
		fileDialogPreviewRatio_ = settings.fileDialogPreviewRatio;
		cropUserAspectWidth_ = settings.userCropAspectWidth;
		cropUserAspectHeight_ = settings.userCropAspectHeight;
		defaultImageProcessing_ = settings.defaultImageProcessing;
		unsharpMaskRadius_ = settings.unsharpMaskRadius;
		unsharpMaskAmount_ = settings.unsharpMaskAmount;
		unsharpMaskThreshold_ = settings.unsharpMaskThreshold;
		cacheSizeMiB_ = settings.cacheSizeMiB;
		copyRenamePattern_ = settings.copyRenamePattern;

		if (magnifierChanged) {
			ClearMagnifyingGlassRequest();
			const SDL_Rect imageArea = ImageAreaRect();
			magnifyingGlass_.SetParameters(settings.magnifyingGlassWidth,
				settings.magnifyingGlassHeight, settings.magnifyingGlassZoomLevel,
				imageArea.w, imageArea.h);
		}
		if (thumbnailPanelWidth_ != previousThumbnailWidth) {
			if (thumbnailPanelVisible_) {
				PrepareThumbnailPreload();
				if (viewport_.IsFitToWindow()) {
					FitToWindow(viewport_.FillWithCrop(), viewport_.NoEnlarge());
				} else {
					RefreshFitRelativeZoomBase();
				}
			}
		}
		advancedConfiguration_.Close();
		SDL_StopTextInput();
		SetTitle();
		return true;
	}

	SDL_Rect AdvancedConfigurationDialogRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::max(1, std::min(kAdvancedConfigurationMaximumWidth, windowWidth - 32));
		const int height = std::max(1, std::min(kAdvancedConfigurationMaximumHeight, windowHeight - 32));
		return SDL_Rect{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	SDL_Rect AdvancedConfigurationCategoryRect(int category) const {
		const SDL_Rect dialog = AdvancedConfigurationDialogRect();
		const int count = std::max(1, advancedConfiguration_.CategoryCount());
		const int usableWidth = dialog.w - 32;
		const int tabWidth = usableWidth / count;
		const int remainder = usableWidth % count;
		const int xOffset = category * tabWidth + std::min(category, remainder);
		return SDL_Rect{dialog.x + 16 + xOffset, dialog.y + 92,
			tabWidth + (category < remainder ? 1 : 0), kAdvancedConfigurationCategoryHeight};
	}

	SDL_Rect AdvancedConfigurationRowsRect() const {
		const SDL_Rect dialog = AdvancedConfigurationDialogRect();
		return SDL_Rect{dialog.x + 16, dialog.y + 132, dialog.w - 32, dialog.h - 202};
	}

	SDL_Rect AdvancedConfigurationRowRect(int visibleRow) const {
		const SDL_Rect rows = AdvancedConfigurationRowsRect();
		return SDL_Rect{rows.x, rows.y + visibleRow * kAdvancedConfigurationRowHeight,
			rows.w, kAdvancedConfigurationRowHeight - 1};
	}

	int AdvancedConfigurationVisibleRows() const {
		return std::max(1, AdvancedConfigurationRowsRect().h / kAdvancedConfigurationRowHeight);
	}

	SDL_Rect AdvancedConfigurationButtonRect(bool apply) const {
		const SDL_Rect dialog = AdvancedConfigurationDialogRect();
		const int y = dialog.y + dialog.h - 46;
		return apply ? SDL_Rect{dialog.x + dialog.w - 208, y, 94, 30} :
			SDL_Rect{dialog.x + dialog.w - 106, y, 90, 30};
	}

	int AdvancedConfigurationRowAt(int x, int y) const {
		const SDL_Rect rows = AdvancedConfigurationRowsRect();
		if (!PointInRect(x, y, rows)) return -1;
		const int visibleRow = (y - rows.y) / kAdvancedConfigurationRowHeight;
		if (visibleRow < 0 || visibleRow >= advancedConfiguration_.VisibleRows()) return -1;
		const int row = static_cast<int>(advancedConfiguration_.Scroll()) + visibleRow;
		return row < advancedConfiguration_.RowCount() ? row : -1;
	}

	void HandleAdvancedConfigurationEvents(const SDL_Event& event, bool& running) {
		if (!advancedConfiguration_.IsOpen()) return;
		advancedConfiguration_.SetVisibleRows(AdvancedConfigurationVisibleRows());
		if (event.type == SDL_QUIT) {
			running = false;
			return;
		}
		if (event.type == SDL_TEXTINPUT) {
			advancedConfiguration_.AppendText(event.text.text);
			return;
		}
		if (event.type == SDL_MOUSEMOTION) {
			lastMouseX_ = event.motion.x;
			lastMouseY_ = event.motion.y;
			return;
		}
		if (event.type == SDL_MOUSEWHEEL) {
			if (!advancedConfiguration_.IsEditing()) {
				advancedConfiguration_.ScrollBy(-std::clamp(event.wheel.y, -10, 10));
			}
			return;
		}
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
			const int key = event.key.keysym.sym;
			const bool control = (event.key.keysym.mod & 0x00C0u) != 0;
			const bool shift = (event.key.keysym.mod & 0x0003u) != 0;
			const auto commitPendingEdit = [this]() {
				return !advancedConfiguration_.IsEditing() || advancedConfiguration_.CommitEdit();
			};
			if (key == SDLK_ESCAPE) {
				if (advancedConfiguration_.IsEditing()) advancedConfiguration_.CancelEdit();
				else CloseAdvancedConfigurationDialog();
			} else if (control && key == SDLK_RETURN) {
				ApplyAdvancedConfigurationDialog();
			} else if (advancedConfiguration_.IsEditing() && control &&
				(key == 'a' || key == 'A')) {
				advancedConfiguration_.SelectAll();
			} else if (advancedConfiguration_.IsEditing() && control &&
				(key == 'v' || key == 'V')) {
				std::unique_ptr<char, SdlClipboardTextWiper> clipboardText(SDL_GetClipboardText());
				if (clipboardText) advancedConfiguration_.AppendText(clipboardText.get());
			} else if (advancedConfiguration_.IsEditing() && key == SDLK_BACKSPACE) {
				advancedConfiguration_.Backspace();
			} else if (advancedConfiguration_.IsEditing() &&
				key == SDLK_RETURN) {
				advancedConfiguration_.CommitEdit();
			} else if (key == SDLK_TAB) {
				if (commitPendingEdit()) advancedConfiguration_.MoveCategory(shift ? -1 : 1);
			} else if (key == SDLK_UP) {
				if (commitPendingEdit()) advancedConfiguration_.MoveSelection(-1);
			} else if (key == SDLK_DOWN) {
				if (commitPendingEdit()) advancedConfiguration_.MoveSelection(1);
			} else if (key == SDLK_PAGEUP) {
				if (commitPendingEdit()) {
					advancedConfiguration_.ScrollBy(-advancedConfiguration_.VisibleRows());
				}
			} else if (key == SDLK_PAGEDOWN) {
				if (commitPendingEdit()) {
					advancedConfiguration_.ScrollBy(advancedConfiguration_.VisibleRows());
				}
			} else if (key == SDLK_HOME) {
				if (commitPendingEdit()) advancedConfiguration_.SelectRow(0);
			} else if (key == SDLK_END) {
				if (commitPendingEdit()) {
					advancedConfiguration_.SelectRow(advancedConfiguration_.RowCount() - 1);
				}
			} else if (!advancedConfiguration_.IsEditing() &&
				(key == SDLK_RETURN || key == SDLK_SPACE)) {
				advancedConfiguration_.ActivateSelected();
			} else if (!advancedConfiguration_.IsEditing() && key == SDLK_LEFT) {
				advancedConfiguration_.AdjustSelected(-1);
			} else if (!advancedConfiguration_.IsEditing() && key == SDLK_RIGHT) {
				advancedConfiguration_.AdjustSelected(1);
			}
			return;
		}
		if (event.type != SDL_MOUSEBUTTONDOWN || event.button.button != SDL_BUTTON_LEFT) return;
		lastMouseX_ = event.button.x;
		lastMouseY_ = event.button.y;
		if (PointInRect(event.button.x, event.button.y, AdvancedConfigurationButtonRect(true))) {
			ApplyAdvancedConfigurationDialog();
			return;
		}
		if (PointInRect(event.button.x, event.button.y, AdvancedConfigurationButtonRect(false))) {
			CloseAdvancedConfigurationDialog();
			return;
		}
		for (int category = 0; category < advancedConfiguration_.CategoryCount(); ++category) {
			if (PointInRect(event.button.x, event.button.y,
				AdvancedConfigurationCategoryRect(category))) {
				if (!advancedConfiguration_.IsEditing() || advancedConfiguration_.CommitEdit()) {
					advancedConfiguration_.SelectCategory(category);
				}
				return;
			}
		}
		const int row = AdvancedConfigurationRowAt(event.button.x, event.button.y);
		if (row >= 0 && (!advancedConfiguration_.IsEditing() || advancedConfiguration_.CommitEdit())) {
			advancedConfiguration_.SelectRow(row);
			advancedConfiguration_.ActivateSelected();
		}
	}

	void RenderAdvancedConfigurationDialog() {
		if (!advancedConfiguration_.IsOpen()) return;
		advancedConfiguration_.SetVisibleRows(AdvancedConfigurationVisibleRows());
		const SDL_Rect dialog = AdvancedConfigurationDialogRect();
		SDL_SetRenderDrawColor(renderer_, 8, 12, 18, 246);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 195, 205, 220);
		DrawText("ADVANCED CONFIGURATION", dialog.x + 16, dialog.y + 12,
			kUiTextScale, 245, 245, 250);
		DrawText(ClipText(advancedConfigurationLocation_, dialog.w - 32),
			dialog.x + 16, dialog.y + 32, kUiTextScale, 175, 185, 200);
		DrawText(ClipText(jpegview_linux::kWindowTitlePatternHelpLine1, dialog.w - 32),
			dialog.x + 16, dialog.y + 46, kUiTextScale, 145, 160, 180);
		DrawText(ClipText(jpegview_linux::kWindowTitlePatternHelpLine2, dialog.w - 32),
			dialog.x + 16, dialog.y + 59, kUiTextScale, 145, 160, 180);
		DrawText(ClipText(jpegview_linux::kWindowTitlePatternHelpLine3, dialog.w - 32),
			dialog.x + 16, dialog.y + 72, kUiTextScale, 145, 160, 180);
		for (int category = 0; category < advancedConfiguration_.CategoryCount(); ++category) {
			const SDL_Rect tab = AdvancedConfigurationCategoryRect(category);
			const bool selected = category == advancedConfiguration_.ActiveCategory();
			SDL_SetRenderDrawColor(renderer_, selected ? 55 : 25, selected ? 82 : 32,
				selected ? 112 : 42, 255);
			SDL_RenderFillRect(renderer_, &tab);
			DrawRect(tab, selected ? 150 : 75, selected ? 185 : 85, selected ? 220 : 95);
			const std::string label = ClipText(advancedConfiguration_.CategoryName(category), tab.w - 8);
			const int labelX = tab.x + std::max(4, (tab.w - TextWidth(label, kUiTextScale)) / 2);
			DrawText(label, labelX, tab.y + (tab.h - TextLineHeight()) / 2,
				kUiTextScale, 225, 230, 240);
		}

		const SDL_Rect rows = AdvancedConfigurationRowsRect();
		const int firstRow = static_cast<int>(advancedConfiguration_.Scroll());
		for (int visibleRow = 0; visibleRow < advancedConfiguration_.VisibleRows(); ++visibleRow) {
			const int row = firstRow + visibleRow;
			if (row >= advancedConfiguration_.RowCount()) break;
			const jpegview_linux::AdvancedConfigurationField* field =
				advancedConfiguration_.FieldAt(row);
			if (field == nullptr) continue;
			const SDL_Rect rowRect = AdvancedConfigurationRowRect(visibleRow);
			const bool selected = row == advancedConfiguration_.SelectedRow();
			SDL_SetRenderDrawColor(renderer_, selected ? 38 : 18, selected ? 54 : 22,
				selected ? 72 : 28, 255);
			SDL_RenderFillRect(renderer_, &rowRect);
			if (selected) DrawRect(rowRect, 92, 135, 175);
			DrawText(advancedConfiguration_.FieldLabel(row), rowRect.x + 8, rowRect.y + 2,
				kUiTextScale, 225, 230, 235);
			DrawText(advancedConfiguration_.FieldKey(row), rowRect.x + 8, rowRect.y + 14,
				kUiTextScale, 135, 145, 160);
			SDL_Rect valueRect{rowRect.x + rowRect.w * 2 / 3, rowRect.y + 3,
				rowRect.w / 3 - 8, rowRect.h - 6};
			const bool editing = selected && advancedConfiguration_.IsEditing();
			SDL_SetRenderDrawColor(renderer_, editing ? 48 : 10, editing ? 58 : 14,
				editing ? 72 : 18, 255);
			SDL_RenderFillRect(renderer_, &valueRect);
			DrawRect(valueRect, editing ? 175 : 78, editing ? 190 : 88, editing ? 220 : 98);
			std::string value = editing ? advancedConfiguration_.EditingText() :
				advancedConfiguration_.FieldValue(row);
			if (editing) value += "_";
			value = ClipText(value, valueRect.w - 10);
			DrawText(value, valueRect.x + 5,
				valueRect.y + (valueRect.h - TextLineHeight()) / 2, kUiTextScale,
				255, 225, 150);
		}
		if (advancedConfiguration_.RowCount() > advancedConfiguration_.VisibleRows()) {
			const std::string page = std::to_string(firstRow + 1) + "–" +
				std::to_string(std::min(advancedConfiguration_.RowCount(),
					firstRow + advancedConfiguration_.VisibleRows())) + "/" +
				std::to_string(advancedConfiguration_.RowCount());
			DrawText(page, rows.x + rows.w - TextWidth(page, kUiTextScale),
				rows.y + rows.h + 3, kUiTextScale, 160, 170, 185);
		}
		const std::string footer = advancedConfiguration_.Message().empty() ?
			"Up/Down select  Left/Right adjust  Enter edit/toggle  Tab group  Ctrl+Enter apply  Esc cancel" :
			advancedConfiguration_.Message();
		DrawText(ClipText(footer, dialog.w - 232), dialog.x + 16, dialog.y + dialog.h - 38,
			kUiTextScale, 205, 205, advancedConfiguration_.Message().empty() ? 215 : 140);
		const SDL_Rect apply = AdvancedConfigurationButtonRect(true);
		SDL_SetRenderDrawColor(renderer_, 38, 75, 105, 255);
		SDL_RenderFillRect(renderer_, &apply);
		DrawRect(apply, 135, 175, 205);
		DrawText("Apply", apply.x + (apply.w - TextWidth("Apply", kUiTextScale)) / 2,
			apply.y + (apply.h - TextLineHeight()) / 2, kUiTextScale, 245, 245, 250);
		const SDL_Rect cancel = AdvancedConfigurationButtonRect(false);
		SDL_SetRenderDrawColor(renderer_, 40, 40, 42, 255);
		SDL_RenderFillRect(renderer_, &cancel);
		DrawRect(cancel, 115, 115, 120);
		DrawText("Cancel", cancel.x + (cancel.w - TextWidth("Cancel", kUiTextScale)) / 2,
			cancel.y + (cancel.h - TextLineHeight()) / 2, kUiTextScale, 225, 225, 230);
	}

	void OpenAbout() {
		aboutOpen_ = true;
		contextMenuOpen_ = false;
		SetTitle("About JPEGView Linux");
	}

	SDL_Rect AboutPanelRect(int windowWidth, int windowHeight) const {
		const int width = std::min(620, std::max(360, windowWidth - 40));
		const int height = 196;
		return SDL_Rect{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	SDL_Rect AboutRepositoryLinkRect(int windowWidth, int windowHeight) const {
		const SDL_Rect panel = AboutPanelRect(windowWidth, windowHeight);
		const std::string label = ClipText(kRepositoryUrl, panel.w - 36);
		return SDL_Rect{panel.x + 18, panel.y + 112,
			std::min(panel.w - 36, TextWidth(label, kUiTextScale)), TextLineHeight()};
	}

	void OpenRepositoryPage() {
		std::string errorMessage;
		for (const jpegview_linux::ExternalCommand& command :
			jpegview_linux::OpenUrlCommands(kRepositoryUrl)) {
			if (StartDetachedProcess(command, errorMessage)) {
				SetTitle("About JPEGView Linux");
				return;
			}
		}
		SetTitle("Cannot open project page: " + errorMessage);
	}

	void HandleAboutEvents(const SDL_Event& event) {
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
			(event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_RETURN ||
			 event.key.keysym.sym == SDLK_SPACE)) {
			aboutOpen_ = false;
			SetTitle();
		} else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
			int windowWidth = 0;
			int windowHeight = 0;
			SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
			if (PointInRect(event.button.x, event.button.y,
				AboutRepositoryLinkRect(windowWidth, windowHeight))) {
				OpenRepositoryPage();
				return;
			}
			aboutOpen_ = false;
			SetTitle();
		}
	}

	void OpenHelp() {
		helpOpen_ = true;
		aboutOpen_ = false;
		contextMenuOpen_ = false;
		SetTitle("JPEGView Linux - Help");
	}

	void HandleHelpEvents(const SDL_Event& event) {
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
			(event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_F1 ||
			 event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE)) {
			helpOpen_ = false;
			SetTitle();
		} else if (event.type == SDL_MOUSEBUTTONDOWN) {
			helpOpen_ = false;
			SetTitle();
		}
	}

	void RestoreClipboardImage() {
		if (!clipboardMode_) return;
		const fs::path temporaryFile = clipboardTempFile_;
		const fs::path temporaryDirectory = clipboardTempDirectory_;
		if (fileListBeforeClipboard_) {
			fileList_ = std::move(*fileListBeforeClipboard_);
			thumbnailCatalogRevisionTracker_.NoteReplacement();
			fileListBeforeClipboard_.reset();
		}
		clipboardMode_ = false;
		clipboardTempFile_.clear();
		clipboardTempDirectory_.clear();
		std::error_code removeError;
		if (!temporaryFile.empty()) fs::remove(temporaryFile, removeError);
		if (!temporaryDirectory.empty()) fs::remove(temporaryDirectory, removeError);
	}

	void PasteCurrentImage() {
		std::vector<std::uint8_t> encodedPng;
		std::string errorMessage;
		if (!jpegview_linux::PasteImageFromClipboard(encodedPng, errorMessage)) {
			SetTitle("Paste image failed: " + errorMessage);
			return;
		}
		char temporaryDirectoryName[] = "/tmp/jpegview-paste-XXXXXX";
		if (mkdtemp(temporaryDirectoryName) == nullptr) {
			SetTitle("Paste image failed: cannot create temporary file");
			return;
		}
		const fs::path temporaryDirectory(temporaryDirectoryName);
		const fs::path temporaryFile = temporaryDirectory / "clipboard.png";
		std::ofstream output(temporaryFile, std::ios::binary);
		output.write(reinterpret_cast<const char*>(encodedPng.data()), static_cast<std::streamsize>(encodedPng.size()));
		const bool written = static_cast<bool>(output);
		output.close();
		std::error_code removeError;
		if (!written) {
			fs::remove(temporaryFile, removeError);
			fs::remove(temporaryDirectory, removeError);
			SetTitle("Paste image failed: cannot write temporary file");
			return;
		}

		if (!clipboardMode_) {
			clipboardReturnViewport_ = viewport_.Snapshot();
			if (!fileList_.Empty() &&
				recentImageLoadState_.OwnsLoadedPath(fileList_.Current())) {
				recentFiles_.RememberViewport(recentImageLoadState_.LoadedPath(),
					*clipboardReturnViewport_);
			}
		}
		RestoreClipboardImage();
		fileListBeforeClipboard_ = std::make_unique<jpegview_linux::FileList>(std::move(fileList_));
		fileList_ = jpegview_linux::FileList({temporaryFile.string()});
		thumbnailCatalogRevisionTracker_.NoteReplacement();
		fileList_.SetWrapAroundFolder(fileListBeforeClipboard_->WrapAroundFolder());
		clipboardTempFile_ = temporaryFile;
		clipboardTempDirectory_ = temporaryDirectory;
		clipboardMode_ = true;
		LoadCurrent();
		SetTitle("Clipboard image — press next/previous to return to the file list");
	}

	void RestoreScaleMode(const jpegview_linux::ViewportSnapshot& snapshot) {
		const SDL_Rect imageArea = ImageAreaRect();
		const auto dimensions = ViewportContentDimensions();
		viewport_.Restore(snapshot, dimensions.first, dimensions.second,
			imageArea.w, imageArea.h);
		SetTitle();
	}

	void ApplyPendingViewportIntent(const jpegview_linux::ViewportIntent& intent) {
		const SDL_Rect imageArea = ImageAreaRect();
		const auto dimensions = ViewportContentDimensions();
		jpegview_linux::ApplyViewportIntent(viewport_, intent,
			dimensions.first, dimensions.second, imageArea.w, imageArea.h);
		if (intent.type == jpegview_linux::ViewportIntentType::ZoomByFactor ||
			intent.type == jpegview_linux::ViewportIntentType::ZoomPreset ||
			intent.type == jpegview_linux::ViewportIntentType::Pan) {
			viewport_.ClampToView(dimensions.first, dimensions.second,
				imageArea.w, imageArea.h);
		}
	}

	void RefreshFitRelativeZoomBase() {
		const auto dimensions = ViewportContentDimensions();
		if (dimensions.first <= 0 || dimensions.second <= 0) return;
		const SDL_Rect imageArea = ImageAreaRect();
		viewport_.UpdateFitRelativeZoomBase(dimensions.first, dimensions.second,
			imageArea.w, imageArea.h);
	}

	void FitToWindow(bool fillCrop = false, bool noEnlarge = true) {
		if (!CanQueuePendingViewportIntent()) return;
		interactionWorkPolicy_.NotifyActivity(
			jpegview_linux::InteractionActivity::Zoom);
		const SDL_Rect imageArea = ImageAreaRect();
		const auto dimensions = ViewportContentDimensions();
		jpegview_linux::ViewportIntent intent;
		intent.type = jpegview_linux::ViewportIntentType::Fit;
		intent.fillWithCrop = fillCrop;
		intent.noEnlarge = noEnlarge;
		jpegview_linux::ApplyViewportIntent(viewport_, intent,
			dimensions.first, dimensions.second, imageArea.w, imageArea.h);
		UpdatePendingViewportSnapshot(intent);
		currentDisplayRequest_.reset();
		PrepareImagePrefetch();
		ShowZoomReadoutTemporarily();
		SetTitle();
	}

	void ActualSize() {
		if (!CanQueuePendingViewportIntent()) return;
		interactionWorkPolicy_.NotifyActivity(
			jpegview_linux::InteractionActivity::Zoom);
		jpegview_linux::ViewportIntent intent;
		intent.type = jpegview_linux::ViewportIntentType::ActualSize;
		jpegview_linux::ApplyViewportIntent(viewport_, intent, 0, 0, 0, 0);
		UpdatePendingViewportSnapshot(intent);
		currentDisplayRequest_.reset();
		PrepareImagePrefetch();
		ShowZoomReadoutTemporarily();
		SetTitle();
	}

	bool NavigateByPageStep(int direction, bool& needsDirectoryScan) {
		needsDirectoryScan = false;
		if (fileList_.Empty() || (direction != -1 && direction != 1)) return false;
		const std::size_t currentIndex = fileList_.CurrentIndex();
		const std::optional<jpegview_linux::PageDimensions> current =
			PageDimensionsAt(currentIndex);
		const std::optional<jpegview_linux::PageDimensions> next = direction > 0 &&
			currentIndex + 1 < fileList_.Size() ? PageDimensionsAt(currentIndex + 1) : std::nullopt;
		const std::optional<jpegview_linux::PageDimensions> previousFirst = direction < 0 &&
			currentIndex >= 2 ? PageDimensionsAt(currentIndex - 2) : std::nullopt;
		const std::optional<jpegview_linux::PageDimensions> previousSecond = direction < 0 &&
			currentIndex >= 1 ? PageDimensionsAt(currentIndex - 1) : std::nullopt;
		const int step = jpegview_linux::DoublePageNavigationStep(direction, currentIndex,
			fileList_.Size(), doublePageModeEnabled_, current, next,
			previousFirst, previousSecond);
		for (int index = 0; index < step; ++index) {
			const jpegview_linux::FileList::LoadedNavigationResult moved = direction > 0 ?
				fileList_.NextLoaded() : fileList_.PreviousLoaded();
			if (moved == jpegview_linux::FileList::LoadedNavigationResult::NeedsDirectoryScan) {
				if (fileList_.CurrentIndex() != currentIndex) fileList_.Select(currentIndex);
				needsDirectoryScan = true;
				return false;
			}
			if (moved == jpegview_linux::FileList::LoadedNavigationResult::NoMove) {
				if (fileList_.CurrentIndex() != currentIndex) fileList_.Select(currentIndex);
				return false;
			}
		}
		return fileList_.CurrentIndex() != currentIndex;
	}

	void PanActualSize(int command) {
		if (!viewport_.IsActualSize()) return;
		const double deltaX = command == IDM_PAN_LEFT ? kKeyboardPanStep :
			command == IDM_PAN_RIGHT ? -kKeyboardPanStep : 0.0;
		const double deltaY = command == IDM_PAN_UP ? kKeyboardPanStep :
			command == IDM_PAN_DOWN ? -kKeyboardPanStep : 0.0;
		PanViewport(deltaX, deltaY);
		playback_.NotifyInteraction(SDL_GetTicks());
	}

	void PanViewport(double deltaX, double deltaY) {
		if (!CanQueuePendingViewportIntent()) return;
		InvalidateViewportPrefetch();
		interactionWorkPolicy_.NotifyActivity(jpegview_linux::InteractionActivity::Pan);
		jpegview_linux::ViewportIntent intent;
		intent.type = jpegview_linux::ViewportIntentType::Pan;
		intent.deltaX = deltaX;
		intent.deltaY = deltaY;
		const auto dimensions = ViewportContentDimensions();
		const SDL_Rect imageArea = ImageAreaRect();
		jpegview_linux::ApplyViewportIntent(viewport_, intent,
			dimensions.first, dimensions.second, imageArea.w, imageArea.h);
		viewport_.ClampToView(dimensions.first, dimensions.second, imageArea.w, imageArea.h);
		UpdatePendingViewportSnapshot(intent);
		ShowZoomNavigatorTemporarily();
	}

	void ShowZoomNavigatorTemporarily() {
		zoomNavigatorVisibleUntil_ = SDL_GetTicks() + 1200;
	}

	void ShowZoomReadoutTemporarily() {
		if (!viewport_.FitRelativeZoomMode()) return;
		zoomReadoutVisibleUntil_ = SDL_GetTicks() + 1200;
	}

	void UpdatePendingViewportSnapshot(
		const std::optional<jpegview_linux::ViewportIntent>& intent = std::nullopt) {
		if (clipboardMode_ || fileList_.Empty()) return;
		const fs::path currentPath = AbsoluteNormalized(fileList_.Current());
		if (intent.has_value()) {
			const jpegview_linux::SourceDescriptor source =
				SourceDescriptorForPath(fileList_.Current());
			(void)pendingImageIntents_.QueueViewport(currentPath, source.Key(),
				currentJpegLoadGeneration_, *intent);
		}
		recentImageLoadState_.UpdatePendingViewport(currentPath, viewport_.Snapshot());
	}

	bool CanQueuePendingViewportIntent() {
		if (!currentJpegHeaderPending_) return true;
		if (fileList_.Empty()) return false;
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		const fs::path currentPath = AbsoluteNormalized(fileList_.Current());
		if (pendingImageIntents_.CanQueue(currentPath, source.Key(),
			currentJpegLoadGeneration_)) return true;
		SetPendingImageIntentLimitTitle();
		return false;
	}

	void SetPendingImageIntentLimitTitle() {
		pendingImageIntentLimitReached_ = true;
		SetPendingHeaderTitle(true);
	}

	void ZoomByStep(int direction, int mouseX, int mouseY) {
		if (direction == 0) return;
		const double step = viewport_.ZoomStepMultiplier();
		ZoomAt(direction > 0 ? step : 1.0 / step, mouseX, mouseY,
			viewport_.FitRelativeZoomMode());
	}

	void ZoomToPreset(double relativeFactor) {
		if (!CanQueuePendingViewportIntent()) return;
		interactionWorkPolicy_.NotifyActivity(
			jpegview_linux::InteractionActivity::Zoom);
		const SDL_Rect imageArea = ImageAreaRect();
		const auto dimensions = ViewportContentDimensions();
		jpegview_linux::ViewportIntent intent;
		intent.type = jpegview_linux::ViewportIntentType::ZoomPreset;
		intent.value = relativeFactor;
		intent.mouseX = std::clamp(imageCenterX_ - imageArea.x, 0, imageArea.w);
		intent.mouseY = std::clamp(imageCenterY_ - imageArea.y, 0, imageArea.h);
		jpegview_linux::ApplyViewportIntent(viewport_, intent,
			dimensions.first, dimensions.second, imageArea.w, imageArea.h);
		if (dimensions.first > 0 && dimensions.second > 0) {
			viewport_.ClampToView(dimensions.first, dimensions.second,
				imageArea.w, imageArea.h);
		}
		UpdatePendingViewportSnapshot(intent);
		ShowZoomNavigatorTemporarily();
		ShowZoomReadoutTemporarily();
		playback_.NotifyInteraction(SDL_GetTicks());
		SetTitle();
	}

	void ZoomAt(double factor, int mouseX, int mouseY, bool pauseAtFitRelativeAnchor = false) {
		if (!CanQueuePendingViewportIntent()) return;
		interactionWorkPolicy_.NotifyActivity(jpegview_linux::InteractionActivity::Zoom);
		const auto dimensions = ViewportContentDimensions();
		const SDL_Rect imageArea = ImageAreaRect();
		const int localMouseX = std::clamp(mouseX - imageArea.x, 0, imageArea.w);
		const int localMouseY = std::clamp(mouseY - imageArea.y, 0, imageArea.h);
		jpegview_linux::ViewportIntent intent;
		intent.type = jpegview_linux::ViewportIntentType::ZoomByFactor;
		intent.value = factor;
		intent.mouseX = localMouseX;
		intent.mouseY = localMouseY;
		intent.pauseAtFitRelativeAnchor = pauseAtFitRelativeAnchor;
		jpegview_linux::ApplyViewportIntent(viewport_, intent,
			dimensions.first, dimensions.second, imageArea.w, imageArea.h);
		if (dimensions.first > 0 && dimensions.second > 0) {
			viewport_.ClampToView(dimensions.first, dimensions.second,
				imageArea.w, imageArea.h);
		}
		UpdatePendingViewportSnapshot(intent);
		ShowZoomNavigatorTemporarily();
		ShowZoomReadoutTemporarily();
		playback_.NotifyInteraction(SDL_GetTicks());
		SetTitle();
	}

	void NextImage(bool showPendingNavigation = false) {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = !doublePageModeEnabled_ && playback_.SlideshowSeconds() > 0.0 &&
			transitionEffect_ != IDM_EFFECT_NONE;
		Image previousImage;
		if (animate && !currentJpegHeaderPending_ && MaterializeCurrentPixels()) previousImage = image_;
		bool needsDirectoryScan = false;
		if (!NavigateByPageStep(1, needsDirectoryScan)) {
			if (needsDirectoryScan) RequestFileListScan(
				jpegview_linux::FileList::ScanOperation::ForwardBoundary,
				1, FileListScanHandling::Navigation);
			return;
		}
		SetTitle();
		if (showPendingNavigation) {
			Render();
		}
		const bool loaded = LoadCurrent(1);
		if (loaded && animate) {
			if (currentJpegHeaderPending_) {
				const jpegview_linux::SourceDescriptor source =
					SourceDescriptorForPath(fileList_.Current());
				if (pendingImageIntents_.RequestTransition(source.Key(),
					currentJpegLoadGeneration_)) {
					pendingTransitionImage_ = std::move(previousImage);
				}
			} else {
				StartTransition(previousImage);
			}
		}
	}

	void PreviousImage(bool showPendingNavigation = false) {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = !doublePageModeEnabled_ && playback_.SlideshowSeconds() > 0.0 &&
			transitionEffect_ != IDM_EFFECT_NONE;
		Image previousImage;
		if (animate && !currentJpegHeaderPending_ && MaterializeCurrentPixels()) previousImage = image_;
		bool needsDirectoryScan = false;
		if (!NavigateByPageStep(-1, needsDirectoryScan)) return;
		SetTitle();
		if (showPendingNavigation) {
			Render();
		}
		const bool loaded = LoadCurrent(-1);
		if (loaded && animate) {
			if (currentJpegHeaderPending_) {
				const jpegview_linux::SourceDescriptor source =
					SourceDescriptorForPath(fileList_.Current());
				if (pendingImageIntents_.RequestTransition(source.Key(),
					currentJpegLoadGeneration_)) {
					pendingTransitionImage_ = std::move(previousImage);
				}
			} else {
				StartTransition(previousImage);
			}
		}
	}

	void NavigateToSiblingFolder(int direction) {
		if (clipboardMode_) RestoreClipboardImage();
		if (fileList_.Empty() || (direction != -1 && direction != 1)) return;
		RequestFileListScan(direction < 0 ?
			jpegview_linux::FileList::ScanOperation::PreviousSibling :
			jpegview_linux::FileList::ScanOperation::NextSibling,
			direction, FileListScanHandling::Navigation);
	}

	void FirstImage() {
		if (clipboardMode_) RestoreClipboardImage();
		if (fileList_.Empty() || fileList_.CurrentIndex() == 0) return;
		fileList_.First();
		LoadCurrent(1);
	}

	void LastImage() {
		if (clipboardMode_) RestoreClipboardImage();
		if (fileList_.Empty() || fileList_.CurrentIndex() + 1 == fileList_.Size()) return;
		fileList_.Last();
		LoadCurrent(-1);
	}

	void ToggleFullscreen() {
		fullscreen_ = !fullscreen_;
		SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : static_cast<Uint32>(0));
		if (viewport_.IsFitToWindow()) {
			FitToWindow(viewport_.FillWithCrop(), viewport_.NoEnlarge());
		} else {
			RefreshFitRelativeZoomBase();
			SetTitle();
		}
	}

	void FitWindowToImage() {
		const auto dimensions = ViewportContentDimensions();
		if (fileList_.Empty() || dimensions.first <= 0 || dimensions.second <= 0) return;
		const int panelWidth = thumbnailPanelVisible_ ? thumbnailPanelWidth_ : 0;
		const int width = std::clamp(dimensions.first + panelWidth + 16, 160, 4096);
		const int height = std::clamp(dimensions.second + 16, 120, 4096);
		SDL_SetWindowSize(window_, width, height);
		FitToWindow(viewport_.FillWithCrop(), viewport_.NoEnlarge());
	}

	void ToggleTitleBar() {
		borderless_ = !borderless_;
		SDL_SetWindowBordered(window_, borderless_ ? 0 : 1);
		SetTitle();
	}

	void ToggleAlwaysOnTop() {
		// SDL_SetWindowAlwaysOnTop is not exported by every SDL2 runtime that
		// can be found on the supported Ubuntu releases. Resolve it lazily so
		// the viewer still links and runs when that optional window-manager
		// feature is unavailable.
		using SetWindowAlwaysOnTop = void (*)(SDL_Window*, int);
		static const SetWindowAlwaysOnTop setWindowAlwaysOnTop =
			reinterpret_cast<SetWindowAlwaysOnTop>(dlsym(RTLD_DEFAULT, "SDL_SetWindowAlwaysOnTop"));
		if (setWindowAlwaysOnTop == nullptr) {
			SetTitle("Always-on-top is not supported by this SDL2 runtime");
			return;
		}
		alwaysOnTop_ = !alwaysOnTop_;
		setWindowAlwaysOnTop(window_, alwaysOnTop_ ? 1 : 0);
		SetTitle();
	}

	void StartSlideshow(double seconds) {
		playback_.StartSlideshow(seconds, SDL_GetTicks());
		SetTitle();
	}

	void StartMovie(double framesPerSecond) {
		playback_.StartMovie(framesPerSecond, SDL_GetTicks());
		SetTitle();
	}

	void StopPlayback() {
		playback_.Stop(SDL_GetTicks());
		SetTitle();
	}

	void ResumePlayback() {
		const jpegview_linux::PlaybackAction action = playback_.Resume(SDL_GetTicks());
		if (action.type == jpegview_linux::PlaybackActionType::ShowFrame &&
			!SetAnimationFrame(action.frameIndex)) {
			playback_.FrameDisplayFailed();
		}
		SetTitle();
	}

	void TickPlayback() {
		const jpegview_linux::PlaybackAction action = playback_.Tick(SDL_GetTicks());
		if (action.type == jpegview_linux::PlaybackActionType::ShowFrame) {
			if (!SetAnimationFrame(action.frameIndex)) playback_.FrameDisplayFailed();
		} else if (action.type == jpegview_linux::PlaybackActionType::NextImage) {
			NextImage();
		}
	}

	void TickHeldNavigation() {
		if (heldNavigation_.Scancode() < 0) return;
		const Uint16 blockedModifierMask = heldNavigation_.ShiftModifierAllowed() ?
			0x03C0u : 0x03C3u;
		if (contextMenuOpen_ || fileDialogOpen_ || confirmationOpen_ || aboutOpen_ || helpOpen_ ||
			advancedConfiguration_.IsOpen() ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen() || cropSizeDialog_.IsOpen() ||
			(SDL_GetModState() & blockedModifierMask) != 0) {
			heldNavigation_.Reset();
			return;
		}

		SDL_PumpEvents();
		int keyCount = 0;
		const Uint8* keyStates = SDL_GetKeyboardState(&keyCount);
		const int scancode = heldNavigation_.Scancode();
		const bool keyIsHeld = keyStates != nullptr && scancode < keyCount && keyStates[scancode] != 0;
		if (keyIsHeld) interactionWorkPolicy_.NotifyActivity(
			jpegview_linux::InteractionActivity::HeldNavigation);
		bool currentImageReady = !fileList_.Empty() &&
			(clipboardMode_ || recentImageLoadState_.LoadedPath() ==
				AbsoluteNormalized(fileList_.Current())) &&
			!currentJpegHeaderPending_ &&
			!doublePagePresentation_.SuppressSinglePage(fileList_.CurrentIndex());
		if (currentImageReady && activeDoublePageRender_.has_value()) {
			currentImageReady = doublePagePresentation_.SpreadReady(fileList_.CurrentIndex()) &&
				doublePagePresentation_.SpreadPresented(fileList_.CurrentIndex()) &&
				ActiveDoublePageAnchorTexture() != nullptr &&
				FindDisplayTexture(doublePagePresentation_.PartnerTextureKey()) != nullptr;
		}
		const int direction = heldNavigation_.AfterImageShown(keyIsHeld, currentImageReady);
		if (direction == 0 || fileList_.Empty()) return;

		const std::size_t previousIndex = fileList_.CurrentIndex();
		if (direction > 0) NextImage(true);
		else PreviousImage(true);
		if (fileList_.CurrentIndex() == previousIndex) heldNavigation_.Reset();
	}

	bool SetAnimationFrame(std::size_t index) {
		if (index >= animationFrames_.size()) return false;
		const jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.Snapshot();
		Image base = animationFrames_[index];
		Image displayed = base;
		if (!displayed.ApplyProcessing(imageProcessing_, autoContrastEnabled_)) return false;
		image_ = std::move(displayed);
		correctionBase_ = std::move(base);
		correctionBaseValid_ = true;
		imageModified_ = false;
		currentImageRotationQuarterTurns_ = 0;
		currentSpreadRotationValid_ = false;
		currentAnimationFrame_ = index;
		currentPixelsMaterialized_ = true;
		materializedProcessing_ = imageProcessing_;
		materializedAutoContrast_ = autoContrastEnabled_;
		currentDisplayRequest_.reset();
		if (!UpdateTexture()) return false;
		RestoreScaleMode(viewportSnapshot);
		return true;
	}

	void UpdateNavigationPanelVisibility(int, int mouseY) {
		if (!navigationPanelEnabled_) {
			controlsVisible_ = false;
			return;
		}
		if (!navigationPanelAutoReveal_) {
			controlsVisible_ = true;
			return;
		}
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		(void)windowWidth;
		controlsVisible_ = mouseY >= std::max(0, windowHeight - kNavigationPanelHoverHeight);
	}

	std::vector<std::string> ImageInfoLines() const {
		std::vector<std::string> lines;
		if (fileList_.Empty() || currentJpegHeaderPending_) return lines;

		std::ostringstream title;
		title << '[' << CurrentImagePositionText() << "] "
			<< InfoText(fileList_.Current().filename().string());
		lines.push_back(title.str());

		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		const jpegview_linux::SourceMetadata& sourceMetadata = source.Metadata();
		lines.push_back(jpegview_linux::FormatImageDimensionsAndSize(
			image_.originalWidth, image_.originalHeight,
			sourceMetadata.hasFileSize ? jpegview_linux::FormatFileSize(sourceMetadata.fileSize) :
				std::string()));
		if (animationFrames_.size() > 1) {
			lines.push_back("Frame: " + std::to_string(playback_.FrameIndex() + 1) + "/" +
				std::to_string(animationFrames_.size()));
			lines.push_back(std::string("Playback: ") + (playback_.AnimationPlaying() ? "playing" : "paused"));
		}
		if (image_.width != image_.originalWidth || image_.height != image_.originalHeight) {
			lines.push_back("Displayed size: " + std::to_string(image_.width) + " x " + std::to_string(image_.height));
		}
		const std::string modificationDate = sourceMetadata.hasModificationTime ?
			FormatUnixTime(sourceMetadata.modificationTimeNanoseconds / 1000000000ll) :
			std::string();
		if (!metadata_.acquisitionDate.empty()) {
			lines.push_back("Acquisition date: " + InfoText(metadata_.acquisitionDate));
		} else if (!metadata_.dateTime.empty()) {
			lines.push_back("Exif Date Time: " + InfoText(metadata_.dateTime));
		} else if (!modificationDate.empty()) {
			lines.push_back(jpegview_linux::FormatModificationDateLine(modificationDate));
		}
		if (!metadata_.cameraModel.empty()) lines.push_back("Camera model: " + InfoText(metadata_.cameraModel));
		if (!metadata_.exposureTime.empty()) lines.push_back("Exposure time (s): " + metadata_.exposureTime);
		if (metadata_.hasExposureBias) {
			std::ostringstream value;
			value << std::fixed << std::setprecision(2) << metadata_.exposureBias;
			lines.push_back("Exposure bias (EV): " + value.str());
		}
		if (metadata_.hasFlash) lines.push_back(std::string("Flash fired: ") + (metadata_.flashFired ? "yes" : "no"));
		if (metadata_.hasFocalLength) {
			std::ostringstream value;
			value << std::fixed << std::setprecision(1) << metadata_.focalLength;
			lines.push_back("Focal length (mm): " + value.str());
		}
		if (metadata_.hasFNumber) {
			std::ostringstream value;
			value << std::fixed << std::setprecision(1) << metadata_.fNumber;
			lines.push_back("F-Number: " + value.str());
		}
		if (metadata_.isoSpeed > 0) lines.push_back("ISO Speed: " + std::to_string(metadata_.isoSpeed));
		if (!metadata_.software.empty()) lines.push_back("Software: " + InfoText(metadata_.software));
		if (!metadata_.imageDescription.empty()) lines.push_back("Description: " + InfoText(metadata_.imageDescription));
		if (!metadata_.userComment.empty()) lines.push_back("Comment: " + InfoText(metadata_.userComment));
		if (!jpegComment_.empty() && metadata_.userComment.empty()) lines.push_back("Comment: " + InfoText(jpegComment_));
		if (metadata_.hasGps) lines.push_back("Location: " + metadata_.gpsLocation);
		if (metadata_.hasAltitude) {
			std::ostringstream value;
			value << std::fixed << std::setprecision(0) << metadata_.altitude;
			lines.push_back("Altitude (m): " + value.str());
		}
		return lines;
	}

	const std::vector<std::string>& CachedImageInfoLines() {
		if (fileList_.Empty() || currentJpegHeaderPending_) {
			return imageInfoLineCache_.GetOrBuild("empty", [] {
				return std::vector<std::string>();
			});
		}
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		std::ostringstream key;
		key << fileList_.MutationRevision() << ':' << fileList_.DescriptorRevision() << '|'
			<< source.Key().logicalPath.size() << ':' << source.Key().logicalPath << '|'
			<< fileList_.CurrentIndex() << ':' << fileList_.Size() << '|'
			<< CurrentImagePositionText().size() << ':' << CurrentImagePositionText() << '|'
			<< image_.originalWidth << ':' << image_.originalHeight << '|'
			<< image_.width << ':' << image_.height << '|'
			<< playback_.FrameIndex() << ':' << playback_.AnimationPlaying() << '|'
			<< animationFrames_.size() << ':' << imageInfoMetadataRevision_;
		return imageInfoLineCache_.GetOrBuild(key.str(), [this] {
			return ImageInfoLines();
		});
	}

	std::string CurrentImagePositionText() const {
		std::optional<std::size_t> spreadPartnerIndex;
		if (activeDoublePageRender_.has_value()) {
			const jpegview_linux::DoublePageSpread& spread = activeDoublePageRender_->layout;
			if (spread.firstIndex == fileList_.CurrentIndex()) {
				spreadPartnerIndex = spread.secondIndex;
			} else if (spread.secondIndex == fileList_.CurrentIndex()) {
				spreadPartnerIndex = spread.firstIndex;
			}
		}
		return jpegview_linux::FormatImagePosition(fileList_.CurrentIndex(),
			fileList_.Size(), spreadPartnerIndex);
	}

	jpegview_linux::InformationOverlayPaintPlan BuildImageInfoPaintPlan() {
		std::vector<std::string> lines = CachedImageInfoLines();
		if (lines.empty()) return {};
		int contentWidth = 0;
		for (std::string& line : lines) {
			line = InfoText(line);
			contentWidth = std::max(contentWidth, TextWidth(line, kUiTextScale));
		}

		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const jpegview_linux::OverlayLayout layout = jpegview_linux::InformationOverlayLayout(
			contentWidth, lines.size(), windowWidth, windowHeight, showFileName_,
			kOverlayInset, kOverlayTextPadding, OverlayLineHeight(), FilenameOverlayHeight(),
			showHistogram_);
		for (std::string& line : lines) {
			if (TextWidth(line, kUiTextScale) <= layout.textWidth) continue;
			line = ClipText(line, layout.textWidth);
		}

		jpegview_linux::GrayscaleSpectrum spectrum{};
		const jpegview_linux::GrayscaleSpectrum* spectrumPointer = nullptr;
		if (showHistogram_) {
			if (!MaterializeCurrentPixels()) return {};
			spectrum = jpegview_linux::BuildGrayscaleSpectrum(image_.bgra, image_.width, image_.height);
			spectrumPointer = &spectrum;
		}
		const jpegview_linux::UiRect button = jpegview_linux::InformationOverlaySpectrumButton(
			layout, kOverlayTextPadding);
		const bool buttonHovered = jpegview_linux::Contains(button, lastMouseX_, lastMouseY_);
		return jpegview_linux::InformationOverlayPaint(layout, lines, OverlayLineHeight(),
			kOverlayTextPadding, showHistogram_, spectrumPointer, buttonHovered);
	}

	jpegview_linux::NavigationPanelPaint CurrentNavigationPanelPaint() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const std::string sortLabel = jpegview_linux::SortModeShortLabel(fileList_.GetSorting());
		const std::string scaleLabel = viewport_.FitRelativeZoomMode() ? "100%" : "1:1";
		return jpegview_linux::BuildNavigationPanelPaint(windowWidth, windowHeight,
			lastMouseX_, lastMouseY_, viewport_.IsFitToWindow(), fileList_.GetSorting(),
			TextWidth(sortLabel, kUiTextScale), TextWidth(scaleLabel, kUiTextScale),
			TextLineHeight(kUiTextScale), selectionModeEnabled_,
			doublePageModeEnabled_, mangaReadingOrderEnabled_, viewport_.FitRelativeZoomMode());
	}

	SDL_Rect PictureLevelsPanelRect() const {
		int windowWidth = 0, windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::max(320, windowWidth - 16);
		const int height = 144;
		return {std::max(8, (windowWidth - width) / 2), std::max(8, windowHeight - height - 8),
			width, std::min(height, std::max(1, windowHeight - 16))};
	}

	SDL_Rect PictureLevelsActionRect(int action) const {
		const SDL_Rect panel = PictureLevelsPanelRect();
		const int gap = 4;
		const int buttonWidth = (panel.w - 16 - 6 * gap) / 7;
		return {panel.x + 8 + action * (buttonWidth + gap), panel.y + 5,
			buttonWidth, 22};
	}

	SDL_Rect UnsharpMaskDialogRect() const {
		int windowWidth = 0, windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(560, std::max(420, windowWidth - 32));
		const int height = 236;
		return {(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	SDL_Rect UnsharpMaskSliderRect(int index) const {
		const SDL_Rect dialog = UnsharpMaskDialogRect();
		return {dialog.x + 20, dialog.y + 58 + index * 40, dialog.w - 40, 34};
	}

	SDL_Rect UnsharpMaskActionRect(bool apply) const {
		const SDL_Rect dialog = UnsharpMaskDialogRect();
		return {dialog.x + dialog.w - (apply ? 124 : 244), dialog.y + dialog.h - 42, 108, 28};
	}

	int UnsharpMaskSliderAt(int x, int y) const {
		for (int index = 0; index < 3; ++index) {
			if (PointInRect(x, y, UnsharpMaskSliderRect(index))) return index;
		}
		return -1;
	}

	void SetUnsharpMaskValueFromX(int index, int x) {
		if (index < 0 || index >= 3) return;
		const SDL_Rect slider = UnsharpMaskSliderRect(index);
		const int left = slider.x + 156;
		const int right = slider.x + slider.w - 12;
		const double maximum = index == 0 ? 5.0 : index == 1 ? 10.0 : 20.0;
		const double value = std::clamp(static_cast<double>(x - left) / std::max(1, right - left), 0.0, 1.0) * maximum;
		if (index == 0) unsharpMaskRadius_ = value;
		else if (index == 1) unsharpMaskAmount_ = value;
		else unsharpMaskThreshold_ = value;
		imageProcessing_.unsharpRadius = unsharpMaskRadius_;
		imageProcessing_.unsharpAmount = unsharpMaskAmount_;
		imageProcessing_.unsharpThreshold = unsharpMaskThreshold_;
		RefreshPictureLevels(false);
	}

	void OpenUnsharpMaskDialog() {
		if (fileList_.Empty() || image_.width <= 0) return;
		unsharpOriginalProcessing_ = imageProcessing_;
		unsharpOriginalRadius_ = unsharpMaskRadius_;
		unsharpOriginalAmount_ = unsharpMaskAmount_;
		unsharpOriginalThreshold_ = unsharpMaskThreshold_;
		if (imageProcessing_.unsharpAmount > 0.0) {
			unsharpMaskRadius_ = imageProcessing_.unsharpRadius;
			unsharpMaskAmount_ = imageProcessing_.unsharpAmount;
			unsharpMaskThreshold_ = imageProcessing_.unsharpThreshold;
		}
		unsharpDialogOpen_ = true;
		unsharpDraggingControl_ = -1;
		imageProcessing_.unsharpRadius = unsharpMaskRadius_;
		imageProcessing_.unsharpAmount = unsharpMaskAmount_;
		imageProcessing_.unsharpThreshold = unsharpMaskThreshold_;
		RefreshPictureLevels(false);
	}

	void CancelUnsharpMaskDialog() {
		imageProcessing_.unsharpRadius = unsharpOriginalProcessing_.unsharpRadius;
		imageProcessing_.unsharpAmount = unsharpOriginalProcessing_.unsharpAmount;
		imageProcessing_.unsharpThreshold = unsharpOriginalProcessing_.unsharpThreshold;
		unsharpMaskRadius_ = unsharpOriginalRadius_;
		unsharpMaskAmount_ = unsharpOriginalAmount_;
		unsharpMaskThreshold_ = unsharpOriginalThreshold_;
		unsharpDialogOpen_ = false;
		unsharpDraggingControl_ = -1;
		RefreshPictureLevels();
	}

	void ApplyUnsharpMaskDialog() {
		unsharpDialogOpen_ = false;
		unsharpDraggingControl_ = -1;
		SaveSettings();
		RefreshPictureLevels();
	}

	void HandleUnsharpMaskDialogEvents(const SDL_Event& event, bool& running) {
		if (event.type == SDL_QUIT) {
			running = false;
			return;
		}
		if (event.type == SDL_KEYDOWN) {
			if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_ESCAPE) CancelUnsharpMaskDialog();
			else if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_RETURN) ApplyUnsharpMaskDialog();
			return;
		}
		if (event.type == SDL_MOUSEMOTION) {
			lastMouseX_ = event.motion.x;
			lastMouseY_ = event.motion.y;
			if (unsharpDraggingControl_ >= 0) SetUnsharpMaskValueFromX(unsharpDraggingControl_, event.motion.x);
			return;
		}
		if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
			lastMouseX_ = event.button.x;
			lastMouseY_ = event.button.y;
			if (PointInRect(event.button.x, event.button.y, UnsharpMaskActionRect(true))) {
				ApplyUnsharpMaskDialog();
			} else if (PointInRect(event.button.x, event.button.y, UnsharpMaskActionRect(false))) {
				CancelUnsharpMaskDialog();
			} else {
				unsharpDraggingControl_ = UnsharpMaskSliderAt(event.button.x, event.button.y);
				SetUnsharpMaskValueFromX(unsharpDraggingControl_, event.button.x);
			}
			return;
		}
		if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
			if (unsharpDraggingControl_ >= 0) PrepareImagePrefetch();
			unsharpDraggingControl_ = -1;
		}
	}

	void RenderUnsharpMaskDialog() {
		if (!unsharpDialogOpen_) return;
		const SDL_Rect dialog = UnsharpMaskDialogRect();
		SDL_SetRenderDrawColor(renderer_, 8, 12, 18, 248);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 205, 210, 220);
		DrawText("APPLY UNSHARP MASK", dialog.x + 20, dialog.y + 18, kUiTextScale, 245, 245, 250);
		const char* labels[] = {"Radius", "Amount", "Threshold"};
		const double values[] = {unsharpMaskRadius_, unsharpMaskAmount_, unsharpMaskThreshold_};
		const double maxima[] = {5.0, 10.0, 20.0};
		for (int index = 0; index < 3; ++index) {
			const SDL_Rect slider = UnsharpMaskSliderRect(index);
			std::ostringstream value;
			value << std::fixed << std::setprecision(2) << values[index];
			DrawText(labels[index], slider.x, slider.y + 2, kUiTextScale, 220, 225, 232);
			DrawText(value.str(), slider.x + 72, slider.y + 2, kUiTextScale, 190, 200, 212);
			const int left = slider.x + 156, right = slider.x + slider.w - 12, trackY = slider.y + 25;
			DrawLine(left, trackY, right, trackY, 95, 108, 124);
			const int knobX = left + static_cast<int>(std::lround(values[index] / maxima[index] * (right - left)));
			SDL_Rect knob{knobX - 4, trackY - 5, 9, 11};
			SDL_SetRenderDrawColor(renderer_, 120, 190, 235, 255);
			SDL_RenderFillRect(renderer_, &knob);
		}
		const auto drawButton = [this](const SDL_Rect& button, const char* label) {
			const bool hovered = PointInRect(lastMouseX_, lastMouseY_, button);
			SDL_SetRenderDrawColor(renderer_, hovered ? 66 : 36, hovered ? 82 : 48,
				hovered ? 104 : 62, 245);
			SDL_RenderFillRect(renderer_, &button);
			DrawRect(button, 130, 145, 165);
			DrawText(label, button.x + (button.w - TextWidth(label, kUiTextScale)) / 2,
				button.y + 8, kUiTextScale, 235, 240, 245);
		};
		drawButton(UnsharpMaskActionRect(false), "Cancel");
		drawButton(UnsharpMaskActionRect(true), "Apply");
	}

	SDL_Rect PictureLevelSliderRect(std::size_t index) const {
		const SDL_Rect panel = PictureLevelsPanelRect();
		constexpr int columns = 4;
		const int gap = 4;
		const int cellWidth = (panel.w - 16 - (columns - 1) * gap) / columns;
		const int row = static_cast<int>(index / columns);
		const int column = static_cast<int>(index % columns);
		return {panel.x + 8 + column * (cellWidth + gap), panel.y + 31 + row * 34,
			cellWidth, 32};
	}

	int PictureLevelSliderAt(int x, int y) const {
		for (std::size_t index = 0; index < static_cast<std::size_t>(jpegview_linux::LevelControl::Count); ++index) {
			if (PointInRect(x, y, PictureLevelSliderRect(index))) return static_cast<int>(index);
		}
		return -1;
	}

	double PictureLevelSliderPosition(jpegview_linux::LevelControl control) const {
		return jpegview_linux::LevelControlPosition(imageProcessing_, control);
	}

	void SetPictureLevelFromX(int index, int x) {
		if (index < 0 || index >= static_cast<int>(jpegview_linux::LevelControl::Count)) return;
		const auto control = static_cast<jpegview_linux::LevelControl>(index);
		const jpegview_linux::LevelControlInfo& info = jpegview_linux::GetLevelControlInfo(control);
		if ((info.enabledByLocalDensity && !imageProcessing_.localDensityEnabled) ||
			(info.enabledByAutoContrast && !autoContrastEnabled_)) return;
		const SDL_Rect cell = PictureLevelSliderRect(static_cast<std::size_t>(index));
		const int left = cell.x + 7;
		const int right = std::max(left + 1, cell.x + cell.w - 7);
		const double fraction = std::clamp(static_cast<double>(x - left) / (right - left), 0.0, 1.0);
		const double value = jpegview_linux::LevelControlValueAtPosition(control, fraction);
		jpegview_linux::ImageProcessingParams updated = imageProcessing_;
		jpegview_linux::SetLevelControlValue(updated, control, value);
		if (jpegview_linux::EqualImageProcessing(updated, imageProcessing_)) return;
		imageProcessing_ = updated;
		RefreshPictureLevels(false);
	}

	void HandlePictureLevelsEvents(const SDL_Event& event, bool& running) {
		if (event.type == SDL_QUIT) {
			running = false;
			return;
		}
		if (event.type == SDL_KEYDOWN) {
			if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_ESCAPE) {
				pictureLevelsPanelOpen_ = false;
				levelsDraggingControl_ = -1;
			} else if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_r) {
				imageProcessing_ = {};
				RefreshPictureLevels();
			}
			return;
		}
		if (event.type == SDL_MOUSEMOTION) {
			lastMouseX_ = event.motion.x;
			lastMouseY_ = event.motion.y;
			if (levelsDraggingControl_ >= 0) SetPictureLevelFromX(levelsDraggingControl_, event.motion.x);
			return;
		}
		if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
			lastMouseX_ = event.button.x;
			lastMouseY_ = event.button.y;
			if (!PointInRect(event.button.x, event.button.y, PictureLevelsPanelRect())) {
				pictureLevelsPanelOpen_ = false;
				levelsDraggingControl_ = -1;
				return;
			}
			for (int action = 0; action < 7; ++action) {
				if (PointInRect(event.button.x, event.button.y, PictureLevelsActionRect(action))) {
					if (keepPictureLevels_ && (action == 2 || action == 3)) return;
					HandlePictureLevelsAction(action);
					return;
				}
			}
			levelsDraggingControl_ = PictureLevelSliderAt(event.button.x, event.button.y);
			SetPictureLevelFromX(levelsDraggingControl_, event.button.x);
			return;
		}
		if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
			if (levelsDraggingControl_ >= 0) PrepareImagePrefetch();
			levelsDraggingControl_ = -1;
		}
	}

	void OpenPictureLevelsPanel() {
		if (image_.width <= 0 || fileList_.Empty()) return;
		pictureLevelsPanelOpen_ = true;
		levelsDraggingControl_ = -1;
	}

	void SaveCurrentPictureLevels() {
		if (fileList_.Empty() || keepPictureLevels_) return;
		auto updatedStore = imageProcessingStore_;
		updatedStore[AbsoluteNormalized(fileList_.Current()).string()] =
			jpegview_linux::ImageProcessingPreset{imageProcessing_, autoContrastEnabled_};
		if (!jpegview_linux::SaveImageProcessingStore(
			jpegview_linux::ImageProcessingStorePath(), updatedStore)) {
			SetTitle("Could not save picture-level parameters");
			return;
		}
		imageProcessingStore_ = std::move(updatedStore);
		SetTitle("Picture-level parameters saved for this image");
	}

	void SaveCurrentPictureLevelsAsDefault() {
		if (fileList_.Empty() || clipboardMode_) return;
		const jpegview_linux::ImageProcessingParams previousProcessing = defaultImageProcessing_;
		const bool previousAutoContrast = defaultAutoContrastEnabled_;
		defaultImageProcessing_ = imageProcessing_;
		defaultAutoContrastEnabled_ = autoContrastEnabled_;
		defaultImageProcessing_.unsharpRadius = 1.0;
		defaultImageProcessing_.unsharpAmount = 0.0;
		defaultImageProcessing_.unsharpThreshold = 4.0;
		if (!SaveSettings()) {
			defaultImageProcessing_ = previousProcessing;
			defaultAutoContrastEnabled_ = previousAutoContrast;
			SetTitle("Could not save default picture levels");
			return;
		}
		PrepareImagePrefetch();
		SetTitle(jpegview_linux::IsDefaultImageProcessing(defaultImageProcessing_) ?
			"Default picture levels restored to neutral" :
			"Current picture levels saved as defaults for other images");
	}

	void ClearCurrentPictureLevels() {
		if (fileList_.Empty() || keepPictureLevels_) return;
		auto updatedStore = imageProcessingStore_;
		const bool hadSavedValues = updatedStore.erase(
			AbsoluteNormalized(fileList_.Current()).string()) != 0;
		if (!hadSavedValues) return;
		if (!jpegview_linux::SaveImageProcessingStore(
			jpegview_linux::ImageProcessingStorePath(), updatedStore)) {
			SetTitle("Could not update picture-level parameter database");
			return;
		}
		imageProcessingStore_ = std::move(updatedStore);
		imageProcessing_ = defaultImageProcessing_;
		autoContrastEnabled_ = defaultAutoContrastEnabled_;
		imageProcessing_.unsharpRadius = unsharpMaskRadius_;
		imageProcessing_.unsharpAmount = 0.0;
		imageProcessing_.unsharpThreshold = unsharpMaskThreshold_;
		SetTitle("Saved picture-level parameters removed");
		RefreshPictureLevels();
	}

	void HandlePictureLevelsAction(int action) {
		switch (action) {
		case 0:
			imageProcessing_.localDensityEnabled = !imageProcessing_.localDensityEnabled;
			RefreshPictureLevels();
			break;
		case 1:
			keepPictureLevels_ = !keepPictureLevels_;
			SaveSettings();
			break;
		case 2:
			SaveCurrentPictureLevels();
			break;
		case 3:
			ClearCurrentPictureLevels();
			break;
		case 4:
			imageProcessing_ = {};
			RefreshPictureLevels();
			break;
		case 5:
			OpenUnsharpMaskDialog();
			break;
		case 6:
			pictureLevelsPanelOpen_ = false;
			levelsDraggingControl_ = -1;
			break;
		}
	}

	void RenderPictureLevels() {
		if (!pictureLevelsPanelOpen_ || fileList_.Empty() || contextMenuOpen_ || fileDialogOpen_ ||
			advancedConfiguration_.IsOpen() ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return;
		const SDL_Rect panel = PictureLevelsPanelRect();
		SDL_SetRenderDrawColor(renderer_, 10, 15, 22, 238);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 170, 190, 215);
		DrawText("PICTURE LEVELS", panel.x + 9, panel.y + 8, kUiTextScale, 235, 240, 248);
		const char* labels[] = {
			imageProcessing_.localDensityEnabled ? "Local density: on" : "Local density: off",
			keepPictureLevels_ ? "Keep levels: on" : "Keep levels: off",
			"Save to DB", "Remove from DB", "Reset", "Unsharp mask...", "Close",
		};
		for (int action = 0; action < 7; ++action) {
			const SDL_Rect button = PictureLevelsActionRect(action);
			const bool disabled = keepPictureLevels_ && (action == 2 || action == 3);
			const bool hovered = PointInRect(lastMouseX_, lastMouseY_, button);
			const Uint8 base = disabled ? 20 : hovered ? 64 : 34;
			SDL_SetRenderDrawColor(renderer_, base, base + 8, base + 16, 240);
			SDL_RenderFillRect(renderer_, &button);
			DrawRect(button, 105, 125, 145);
			DrawText(ClipText(labels[action], button.w - 8), button.x + 4, button.y + 6,
				kUiTextScale, disabled ? 105 : 225, disabled ? 110 : 230, disabled ? 115 : 235);
		}
		for (std::size_t index = 0; index < static_cast<std::size_t>(jpegview_linux::LevelControl::Count); ++index) {
			const auto control = static_cast<jpegview_linux::LevelControl>(index);
			const jpegview_linux::LevelControlInfo& info = jpegview_linux::GetLevelControlInfo(control);
			const SDL_Rect cell = PictureLevelSliderRect(index);
			const bool disabled = (info.enabledByLocalDensity && !imageProcessing_.localDensityEnabled) ||
				(info.enabledByAutoContrast && !autoContrastEnabled_);
			const bool hovered = PictureLevelSliderAt(lastMouseX_, lastMouseY_) == static_cast<int>(index);
			SDL_SetRenderDrawColor(renderer_, hovered ? 34 : 22, hovered ? 40 : 27, hovered ? 48 : 35, 230);
			SDL_RenderFillRect(renderer_, &cell);
			const double value = jpegview_linux::GetLevelControlValue(imageProcessing_, control);
			std::ostringstream formatted;
			formatted << std::fixed << std::setprecision(2) << value;
			const std::string valueText = formatted.str();
			const int valueWidth = TextWidth(valueText, kUiTextScale);
			DrawText(ClipText(info.label, std::max(12, cell.w - valueWidth - 16)),
				cell.x + 4, cell.y + 2, kUiTextScale, disabled ? 100 : 220, disabled ? 105 : 225, disabled ? 110 : 232);
			DrawText(valueText, cell.x + cell.w - valueWidth - 4, cell.y + 2, kUiTextScale,
				disabled ? 100 : 195, disabled ? 105 : 205, disabled ? 110 : 215);
			const int trackLeft = cell.x + 7;
			const int trackRight = std::max(trackLeft + 1, cell.x + cell.w - 7);
			const int trackY = cell.y + 24;
			DrawLine(trackLeft, trackY, trackRight, trackY, disabled ? 65 : 90, disabled ? 70 : 100, disabled ? 75 : 110);
			const int knobX = trackLeft + static_cast<int>(std::lround(PictureLevelSliderPosition(control) * (trackRight - trackLeft)));
			SDL_Rect knob{knobX - 3, trackY - 4, 7, 9};
			SDL_SetRenderDrawColor(renderer_, disabled ? 100 : 110, disabled ? 105 : 190, disabled ? 110 : 235, 255);
			SDL_RenderFillRect(renderer_, &knob);
		}
	}

	static bool PointInRect(int x, int y, const SDL_Rect& rect) {
		return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
	}

	bool HandleControlClick(int x, int y) {
		if (!navigationPanelEnabled_ || !controlsVisible_) return false;
		const jpegview_linux::NavigationPanelPaint panel = CurrentNavigationPanelPaint();
		for (const jpegview_linux::NavigationButtonPaint& button : panel.buttons) {
			if (!jpegview_linux::Contains(button.rect, x, y)) continue;
			ExecuteCommand(button.command);
			UpdateNavigationPanelVisibility(x, y);
			return true;
		}
		return jpegview_linux::Contains(panel.panel, x, y);
	}

	void ExecuteCommand(int command) {
		if (command >= IDM_FIRST_OPENWITH_CMD && command <= IDM_LAST_OPENWITH_CMD) {
			OpenCurrentWith(static_cast<std::size_t>(command - IDM_FIRST_OPENWITH_CMD));
			return;
		}
		if (command == jpegview_linux::kCommandAdvancedConfiguration) {
			OpenAdvancedConfigurationDialog();
			return;
		}
		switch (command) {
		case IDM_CROP_SEL:
			CropCurrentSelection();
			break;
		case IDM_LOSSLESS_CROP_SEL:
			OpenLosslessCropDialog();
			break;
		case IDM_COPY_SEL:
			CopyCurrentSelection();
			break;
		case IDM_ZOOM_SEL:
			ZoomToSelection();
			ClearCropSelection();
			break;
		case IDM_CROPMODE_FREE:
			cropSelection_.SetMode(jpegview_linux::CropSelectionMode::Free);
			SetSelectionModeEnabled(true);
			break;
		case IDM_CROPMODE_FIXED_SIZE:
			OpenFixedCropSizeDialog();
			break;
		case IDM_CROPMODE_1_1:
			SetCropAspect(1, 1);
			break;
		case IDM_CROPMODE_5_4:
			SetCropAspect(5, 4);
			break;
		case IDM_CROPMODE_4_3:
			SetCropAspect(4, 3);
			break;
		case IDM_CROPMODE_7_5:
			SetCropAspect(7, 5);
			break;
		case IDM_CROPMODE_3_2:
			SetCropAspect(3, 2);
			break;
		case IDM_CROPMODE_16_10:
			SetCropAspect(16, 10);
			break;
		case IDM_CROPMODE_16_9:
			SetCropAspect(16, 9);
			break;
		case IDM_CROPMODE_USER:
			SetCropAspect(cropUserAspectWidth_, cropUserAspectHeight_);
			break;
		case IDM_CROPMODE_IMAGE:
			cropSelection_.SetMode(jpegview_linux::CropSelectionMode::ImageAspect);
			cropSelection_.ReapplyMode(viewport_.Zoom());
			SetSelectionModeEnabled(true);
			break;
		case jpegview_linux::kCommandPreviousSiblingFolder:
			NavigateToSiblingFolder(-1);
			break;
		case jpegview_linux::kCommandNextSiblingFolder:
			NavigateToSiblingFolder(1);
			break;
		case jpegview_linux::kCommandEditPictureLevels:
			OpenPictureLevelsPanel();
			break;
		case IDM_FIRST:
			FirstImage();
			break;
		case IDM_PREV:
			PreviousImage();
			break;
		case IDM_TOGGLE:
			if (fileList_.HasMarkedFile()) {
				if (clipboardMode_) RestoreClipboardImage();
				const fs::path markedTarget = fileList_.MarkedToggleTarget();
				const fs::path previousPath = fileList_.Current();
				if (!markedTarget.empty() && !fileList_.ContainsPath(markedTarget)) {
					RequestFileListScan(
						jpegview_linux::FileList::ScanOperation::MarkedToggleTarget,
						0, FileListScanHandling::MarkedToggle, true, false,
						markedTarget, DisplayModeLoadPolicy::PreserveCurrent, previousPath);
				} else if (fileList_.ToggleBetweenMarkedAndCurrent()) {
					LoadCurrent();
				}
			}
			break;
		case IDM_MARK_FOR_TOGGLE:
			if (!clipboardMode_ && image_.width > 0 && image_.height > 0) {
				fileList_.MarkCurrentForToggle();
			}
			break;
		case IDM_NEXT:
			NextImage();
			break;
		case IDM_LAST:
			LastImage();
			break;
		case IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS:
			if (viewport_.IsFitToWindow()) ActualSize(); else FitToWindow();
			break;
		case IDM_FIT_TO_SCREEN:
			FitToWindow();
			break;
		case IDM_ZOOM_100:
			ActualSize();
			break;
		case IDM_FULL_SCREEN_MODE:
			ToggleFullscreen();
			break;
		case IDM_ROTATE_90:
		case IDM_ROTATE_270:
		case IDM_MIRROR_H:
		case IDM_MIRROR_V:
			ApplyTransform(command);
			break;
		case IDM_CHANGESIZE:
			OpenResizeDialog();
			break;
		case IDM_AUTO_CORRECTION:
			ToggleAutoContrast();
			break;
		case IDM_LDC:
			imageProcessing_.localDensityEnabled = !imageProcessing_.localDensityEnabled;
			RefreshPictureLevels();
			break;
		case IDM_KEEP_PARAMETERS:
			keepPictureLevels_ = !keepPictureLevels_;
			SaveSettings();
			break;
		case IDM_SAVE_PARAM_DB:
			SaveCurrentPictureLevels();
			break;
		case IDM_CLEAR_PARAM_DB:
			ClearCurrentPictureLevels();
			break;
		case IDM_BACKUP_PARAMDB:
			OpenParameterDbBackupDialog();
			break;
		case IDM_RESTORE_PARAMDB:
			OpenParameterDbRestoreDialog();
			break;
		case IDM_SET_AS_DEFAULT_VIEWER:
			SetAsDefaultViewer();
			break;
		case IDM_SAVE_PARAMETERS:
			SaveCurrentPictureLevelsAsDefault();
			break;
		case IDM_ROTATE_90_LOSSLESS:
		case IDM_ROTATE_90_LOSSLESS_CONFIRM:
		case IDM_ROTATE_270_LOSSLESS:
		case IDM_ROTATE_270_LOSSLESS_CONFIRM:
		case IDM_ROTATE_180_LOSSLESS:
		case IDM_MIRROR_H_LOSSLESS:
		case IDM_MIRROR_V_LOSSLESS:
			ApplyLosslessJpegTransform(command);
			break;
		case IDM_OPEN:
			OpenFileDialog();
			break;
		case IDM_EXPLORE:
			OpenContainingFolder();
			break;
		case IDM_PRINT:
			PrintCurrentImage();
			break;
		case IDM_BATCH_COPY:
			OpenBatchCopyDialog();
			break;
		case IDM_COPY_FULL:
			CopyCurrentImage(true);
			break;
		case IDM_COPY_PATH:
			CopyCurrentPath();
			break;
		case IDM_PASTE:
			PasteCurrentImage();
			break;
		case IDM_TOUCH_IMAGE:
			TouchCurrentImage(false);
			break;
		case IDM_TOUCH_IMAGE_EXIF:
			TouchCurrentImage(true);
			break;
		case IDM_TOUCH_IMAGE_EXIF_FOLDER:
			TouchFolderImagesToExifDate();
			break;
		case IDM_SET_WALLPAPER_ORIG:
			SetWallpaper(false);
			break;
		case IDM_SET_WALLPAPER_DISPLAY:
			SetWallpaper(true);
			break;
		case IDM_MOVE_TO_RECYCLE_BIN:
			MoveCurrentToTrash();
			break;
		case IDM_MOVE_TO_RECYCLE_BIN_CONFIRM:
		case IDM_MOVE_TO_RECYCLE_BIN_CONFIRM_PERMANENT_DELETE:
			if (!fileList_.Empty() && !clipboardMode_) {
				RequestConfirmation(command, "Move current image to the desktop trash?");
			}
			break;
		case IDM_SAVE:
		case IDM_SAVE_ALLOW_NO_PROMPT:
			OpenSaveFileDialog(true);
			break;
		case IDM_SAVE_SCREEN:
			OpenSaveFileDialog(false);
			break;
		case IDM_RELOAD:
			if (!fileList_.Empty()) RequestFileListScan(
				jpegview_linux::FileList::ScanOperation::Reload,
				0, FileListScanHandling::Reload, true, true);
			break;
		case IDM_SHOW_FILEINFO:
			infoVisible_ = !infoVisible_;
			SaveSettings();
			break;
		case IDM_SHOW_FILENAME:
			showFileName_ = !showFileName_;
			SaveSettings();
			break;
		case IDM_SHOW_NAVPANEL:
			navigationPanelEnabled_ = !navigationPanelEnabled_;
			UpdateNavigationPanelVisibility(lastMouseX_, lastMouseY_);
			SaveSettings();
			break;
		case jpegview_linux::kCommandToggleThumbnailPanel:
			thumbnailPanelVisible_ = !thumbnailPanelVisible_;
			PrepareThumbnailPreload();
			UpdateThumbnailPanelCursor(lastMouseX_, lastMouseY_);
			if (viewport_.IsFitToWindow()) {
				FitToWindow(viewport_.FillWithCrop(), viewport_.NoEnlarge());
			} else {
				RefreshFitRelativeZoomBase();
			}
			SaveSettings();
			break;
		case jpegview_linux::kCommandToggleZoomNavigator:
			showZoomNavigator_ = !showZoomNavigator_;
			SaveSettings();
			UpdateCropCursor(lastMouseX_, lastMouseY_);
			UpdateZoomNavigatorCursor(lastMouseX_, lastMouseY_);
			break;
		case jpegview_linux::kCommandToggleMagnifyingGlass:
			if (image_.width > 0 && image_.height > 0 &&
				(!fileList_.Empty() || clipboardMode_)) {
				SetMagnifyingGlassEnabled(!magnifyingGlass_.Enabled());
			}
			break;
		case jpegview_linux::kCommandToggleDoublePageMode:
			doublePageModeEnabled_ = !doublePageModeEnabled_;
			doublePageModeDefault_ = doublePageModeEnabled_;
			ClearTransition();
			if (!clipboardMode_ && !fileList_.Empty() &&
				recentImageLoadState_.OwnsLoadedPath(fileList_.Current())) {
				recentFiles_.RememberDoublePageMode(recentImageLoadState_.LoadedPath(),
					{doublePageModeEnabled_, mangaReadingOrderEnabled_});
			}
			currentDisplayRequest_.reset();
			RefreshDoublePageRenderState();
			PrepareImagePrefetch();
			SaveSettings();
			SetTitle();
			break;
		case jpegview_linux::kCommandToggleMangaReadingOrder:
			mangaReadingOrderEnabled_ = !mangaReadingOrderEnabled_;
			mangaReadingOrderDefault_ = mangaReadingOrderEnabled_;
			ClearTransition();
			if (!clipboardMode_ && !fileList_.Empty() &&
				recentImageLoadState_.OwnsLoadedPath(fileList_.Current())) {
				recentFiles_.RememberDoublePageMode(recentImageLoadState_.LoadedPath(),
					{doublePageModeEnabled_, mangaReadingOrderEnabled_});
			}
			currentDisplayRequest_.reset();
			RefreshDoublePageRenderState();
			PrepareImagePrefetch();
			SaveSettings();
			SetTitle();
			break;
		case jpegview_linux::kCommandToggleSelectionMode:
			SetSelectionModeEnabled(!selectionModeEnabled_);
			break;
		case jpegview_linux::kToggleNavigationPanelAutoReveal:
			navigationPanelAutoReveal_ = !navigationPanelAutoReveal_;
			UpdateNavigationPanelVisibility(lastMouseX_, lastMouseY_);
			SaveSettings();
			break;
		case IDM_LOOP_FOLDER:
			(void)fileList_.SetNavigationMode(jpegview_linux::FileList::NavigationMode::LoopDirectory);
			SetTitle();
			break;
		case IDM_LOOP_RECURSIVELY:
			if (fileList_.SetNavigationMode(
				jpegview_linux::FileList::NavigationMode::LoopSubDirectories)) {
				RequestFileListScan(jpegview_linux::FileList::ScanOperation::PrepareDirectoryScope,
					0, FileListScanHandling::Navigation, true);
			}
			SetTitle();
			break;
		case IDM_LOOP_SIBLINGS:
			if (fileList_.SetNavigationMode(
				jpegview_linux::FileList::NavigationMode::LoopSameDirectoryLevel)) {
				RequestFileListScan(jpegview_linux::FileList::ScanOperation::PrepareDirectoryScope,
					0, FileListScanHandling::Navigation, true);
			}
			SetTitle();
			break;
		case IDM_SORT_MOD_DATE:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::LastModificationTime,
				fileList_.IsSortedAscending());
			SetTitle();
			PrepareThumbnailPreload();
			PrepareImagePrefetch();
			SaveSettings();
			break;
		case jpegview_linux::kNavigationSortModeCommand:
			fileList_.SetSorting(
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::FileName ?
					jpegview_linux::FileList::SortMode::LastModificationTime :
					jpegview_linux::FileList::SortMode::FileName,
				fileList_.IsSortedAscending());
			SetTitle();
			PrepareThumbnailPreload();
			PrepareImagePrefetch();
			SaveSettings();
			break;
		case IDM_SORT_CREATION_DATE:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::CreationTime,
				fileList_.IsSortedAscending());
			SetTitle();
			PrepareThumbnailPreload();
			PrepareImagePrefetch();
			SaveSettings();
			break;
		case IDM_SORT_NAME:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::FileName,
				fileList_.IsSortedAscending());
			SetTitle();
			PrepareThumbnailPreload();
			PrepareImagePrefetch();
			SaveSettings();
			break;
		case IDM_SORT_RANDOM:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::Random,
				fileList_.IsSortedAscending());
			SetTitle();
			PrepareThumbnailPreload();
			PrepareImagePrefetch();
			SaveSettings();
			break;
		case IDM_SORT_SIZE:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::FileSize,
				fileList_.IsSortedAscending());
			SetTitle();
			PrepareThumbnailPreload();
			PrepareImagePrefetch();
			SaveSettings();
			break;
		case IDM_SORT_ASCENDING:
			fileList_.SetSorting(fileList_.GetSorting(), true);
			SetTitle();
			PrepareThumbnailPreload();
			PrepareImagePrefetch();
			SaveSettings();
			break;
		case IDM_SORT_DESCENDING:
			fileList_.SetSorting(fileList_.GetSorting(), false);
			SetTitle();
			PrepareThumbnailPreload();
			PrepareImagePrefetch();
			SaveSettings();
			break;
		case IDM_STOP_MOVIE:
			StopPlayback();
			break;
		case IDM_SLIDESHOW_RESUME:
			ResumePlayback();
			break;
		case IDM_SLIDESHOW_START:
			StartSlideshow(3.0);
			break;
		case IDM_SLIDESHOW_1:
		case IDM_SLIDESHOW_2:
		case IDM_SLIDESHOW_3:
		case IDM_SLIDESHOW_4:
		case IDM_SLIDESHOW_5:
		case IDM_SLIDESHOW_7:
		case IDM_SLIDESHOW_10:
		case IDM_SLIDESHOW_20:
			StartSlideshow(static_cast<double>(command - IDM_SLIDESHOW_START));
			break;
		case IDM_ZOOM_400:
			ZoomToPreset(4.0);
			break;
		case IDM_ZOOM_200:
			ZoomToPreset(2.0);
			break;
		case IDM_ZOOM_50:
			ZoomToPreset(0.5);
			break;
		case IDM_ZOOM_25:
			ZoomToPreset(0.25);
			break;
		case IDM_ZOOM_INC:
			ZoomByStep(1, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_DEC:
			ZoomByStep(-1, imageCenterX_, imageCenterY_);
			break;
		case IDM_PAN_UP:
		case IDM_PAN_DOWN:
		case IDM_PAN_RIGHT:
		case IDM_PAN_LEFT:
			PanActualSize(command);
			break;
		case IDM_FILL_WITH_CROP:
			FitToWindow(true, false);
			break;
		case IDM_FIT_TO_SCREEN_NO_ENLARGE:
			FitToWindow(false, true);
			break;
		case IDM_SPAN_SCREENS:
			ToggleFullscreen();
			break;
		case IDM_FIT_WINDOW_TO_IMAGE:
			FitWindowToImage();
			break;
		case IDM_HIDE_TITLE_BAR:
			ToggleTitleBar();
			break;
		case IDM_ALWAYS_ON_TOP:
			ToggleAlwaysOnTop();
			break;
		case IDM_AUTO_ZOOM_FIT_NO_ZOOM:
			FitToWindow(false, true);
			break;
		case IDM_AUTO_ZOOM_FILL_NO_ZOOM:
			FitToWindow(true, true);
			break;
		case IDM_AUTO_ZOOM_FIT:
			FitToWindow(false, true);
			break;
		case IDM_AUTO_ZOOM_FILL:
			FitToWindow(true, false);
			break;
		case IDM_ABOUT:
			OpenAbout();
			break;
		case IDM_HELP:
			OpenHelp();
			break;
		case IDM_MOVIE_START_FPS:
			StartMovie(25.0);
			break;
		case IDM_MOVIE_5_FPS:
		case IDM_MOVIE_10_FPS:
		case IDM_MOVIE_25_FPS:
		case IDM_MOVIE_30_FPS:
		case IDM_MOVIE_50_FPS:
		case IDM_MOVIE_100_FPS:
			StartMovie(static_cast<double>(command - IDM_MOVIE_START_FPS));
			break;
		case IDM_EFFECT_NONE:
		case IDM_EFFECT_BLEND:
		case IDM_EFFECT_SLIDE_RL:
		case IDM_EFFECT_SLIDE_LR:
		case IDM_EFFECT_SLIDE_TB:
		case IDM_EFFECT_SLIDE_BT:
		case IDM_EFFECT_ROLL_RL:
		case IDM_EFFECT_ROLL_LR:
		case IDM_EFFECT_ROLL_TB:
		case IDM_EFFECT_ROLL_BT:
		case IDM_EFFECT_SCROLL_RL:
		case IDM_EFFECT_SCROLL_LR:
		case IDM_EFFECT_SCROLL_TB:
		case IDM_EFFECT_SCROLL_BT:
			transitionEffect_ = command;
			SetTitle();
			break;
		case IDM_EFFECTTIME_VERY_FAST:
			transitionDurationMs_ = 100;
			break;
		case IDM_EFFECTTIME_FAST:
			transitionDurationMs_ = 250;
			break;
		case IDM_EFFECTTIME_NORMAL:
			transitionDurationMs_ = 500;
			break;
		case IDM_EFFECTTIME_SLOW:
			transitionDurationMs_ = 1000;
			break;
		case IDM_EFFECTTIME_VERY_SLOW:
			transitionDurationMs_ = 2000;
			break;
		case IDM_EXIT:
			quitRequested_ = true;
			break;
		case IDM_DEFAULT_ESC:
			if (playback_.Mode() != PlaybackMode::None || playback_.AnimationPlaying()) {
				StopPlayback();
			} else {
				quitRequested_ = true;
			}
			break;
		default:
			// The command is known to the Windows resource/key-map files but is
			// not implemented by the focused Linux viewer yet.
			break;
		}
	}

	std::vector<MenuItem> ContextMenuItems(bool advancedOptions) {
		const std::string extension = fileList_.Empty() ? std::string() :
			Lower(fileList_.Current().extension().string());
		const bool archiveMember = !fileList_.Empty() &&
			jpegview_linux::IsArchiveMemberLocation(fileList_.Current());
		if (contextMenuCropOnly_) openWithApplications_.clear();
		else if (archiveMember) openWithApplications_.clear();
		else openWithApplications_ = jpegview_linux::DiscoverOpenWithApplications(extension);

		jpegview_linux::ContextMenuState state;
		state.playbackMode = playback_.Mode();
		state.animationPlaying = playback_.AnimationPlaying();
		state.animationAvailable = playback_.HasAnimation();
		state.movieFramesPerSecond = playback_.MovieFramesPerSecond();
		state.infoVisible = infoVisible_;
		state.filenameVisible = showFileName_;
		state.navigationPanelEnabled = navigationPanelEnabled_;
		state.navigationPanelAutoReveal = navigationPanelAutoReveal_;
		state.thumbnailPanelVisible = thumbnailPanelVisible_;
		state.showZoomNavigator = showZoomNavigator_;
		state.magnifyingGlassEnabled = magnifyingGlass_.Enabled();
		state.doublePageModeEnabled = doublePageModeEnabled_;
		state.mangaReadingOrderEnabled = mangaReadingOrderEnabled_;
		state.selectionModeEnabled = selectionModeEnabled_;
		state.spacebarNavigatesImages = spacebarNavigatesImages_;
		state.navigationMode = fileList_.GetNavigationMode();
		state.sortMode = fileList_.GetSorting();
		state.sortAscending = fileList_.IsSortedAscending();
		state.imageAvailable = image_.width > 0;
		state.archiveMember = archiveMember;
		state.losslessJpegAvailable = !clipboardMode_ && !archiveMember && HasExecutable("jpegtran") &&
			(extension == ".jpg" || extension == ".jpeg" || extension == ".jpe");
		state.cropContextMenu = contextMenuCropOnly_;
		state.cropSelectionAvailable = cropSelection_.HasSelection();
		state.losslessJpegCropAvailable = jpegview_linux::CanOfferLosslessJpegCrop(
			state.losslessJpegAvailable, imageModified_, currentPixelsDetachedFromSource_);
		state.cropMode = cropSelection_.Mode();
		state.cropAspectWidth = cropAspectWidth_;
		state.cropAspectHeight = cropAspectHeight_;
		state.userCropAspectWidth = cropUserAspectWidth_;
		state.userCropAspectHeight = cropUserAspectHeight_;
		state.autoCorrectionEnabled = autoContrastEnabled_;
		state.pictureLevelsAvailable = !clipboardMode_ && !fileList_.Empty() && image_.width > 0;
		state.localDensityEnabled = imageProcessing_.localDensityEnabled;
		state.keepPictureLevels = keepPictureLevels_;
		state.pictureLevelsSaved = !fileList_.Empty() && imageProcessingStore_.find(
			AbsoluteNormalized(fileList_.Current()).string()) != imageProcessingStore_.end();
		state.parameterDatabaseAvailable =
			!jpegview_linux::ImageProcessingStorePath().empty();
		state.fitToWindow = viewport_.IsFitToWindow();
		state.fillWithCrop = viewport_.FillWithCrop();
		state.noEnlarge = viewport_.NoEnlarge();
		state.fitRelativeZoomMode = viewport_.FitRelativeZoomMode();
		state.fitRelativeZoomBase = viewport_.FitRelativeZoomBase();
		state.zoom = viewport_.Zoom();
		state.fullscreen = fullscreen_;
		state.borderless = borderless_;
		state.alwaysOnTop = alwaysOnTop_;
		state.transitionEffect = transitionEffect_;
		state.transitionDurationMs = transitionDurationMs_;
		for (const auto& application : openWithApplications_) {
			state.openWithApplicationNames.push_back(application.name);
		}
		return jpegview_linux::BuildContextMenu(state, advancedOptions);
	}

	std::string MenuLabel(const MenuItem& item) const {
		if (item.checked) return std::string("[X] ") + item.label;
		return item.label;
	}

	std::string MenuShortcut(const MenuItem& item) const {
		return item.shortcut;
	}

	int ContextMenuItemHeight(std::size_t index) const {
		return contextMenuItems_[index].separator ? kContextMenuSeparatorHeight : ContextMenuRowHeight();
	}

	bool IsContextMenuItemSelectable(std::size_t index) const {
		const MenuItem& item = contextMenuItems_[index];
		return !item.separator && item.command != 0 && item.enabled;
	}

	std::vector<ContextMenuColumn> ContextMenuColumns() const {
		int windowHeight = 0;
		SDL_GetWindowSize(window_, nullptr, &windowHeight);
		const std::vector<jpegview_linux::MenuColumn> layout = jpegview_linux::LayoutMenuColumns(
			contextMenuItems_, windowHeight - 2 *
				(kContextMenuVerticalPadding + kContextMenuWindowInset),
			ContextMenuRowHeight(), kContextMenuSeparatorHeight);
		std::vector<ContextMenuColumn> columns;
		columns.reserve(layout.size());
		for (const jpegview_linux::MenuColumn& source : layout) {
			columns.push_back({source.begin, source.end, 0, 260, source.height});
		}

		int columnX = 0;
		for (ContextMenuColumn& column : columns) {
			for (std::size_t index = column.begin; index < column.end; ++index) {
				const MenuItem& item = contextMenuItems_[index];
				if (item.separator) continue;
				const int labelWidth = TextWidth(MenuLabel(item), kUiTextScale);
				const int shortcutWidth = TextWidth(MenuShortcut(item), kUiTextScale);
				column.width = std::max(column.width,
					labelWidth + shortcutWidth + (shortcutWidth > 0 ? 40 : 24));
			}
			column.x = columnX;
			columnX += column.width + 1;
		}
		return columns;
	}

	SDL_Rect ContextMenuRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const std::vector<ContextMenuColumn> columns = ContextMenuColumns();
		int width = 0;
		int contentHeight = 0;
		for (const ContextMenuColumn& column : columns) {
			width = std::max(width, column.x + column.width);
			contentHeight = std::max(contentHeight, column.height);
		}
		const int height = contentHeight + 2 * kContextMenuVerticalPadding;
		int x = contextMenuX_;
		int y = contextMenuY_;
		if (!contextMenuPositionLocked_) {
			if (x + width > windowWidth) x = windowWidth - width - kContextMenuWindowInset;
			if (y + height > windowHeight) y = windowHeight - height - kContextMenuWindowInset;
			x = std::max(kContextMenuWindowInset, x);
			y = std::max(kContextMenuWindowInset, y);
		}
		return SDL_Rect{x, y, width, height};
	}

	void RepositionContextMenuToFit() {
		contextMenuPositionLocked_ = false;
		const SDL_Rect fittedMenu = ContextMenuRect();
		contextMenuX_ = fittedMenu.x;
		contextMenuY_ = fittedMenu.y;
		contextMenuPositionLocked_ = true;
	}

	int ContextMenuItemAt(int x, int y) const {
		const SDL_Rect menu = ContextMenuRect();
		if (!PointInRect(x, y, menu)) return -1;
		const std::vector<ContextMenuColumn> columns = ContextMenuColumns();
		for (const ContextMenuColumn& column : columns) {
			const int columnLeft = menu.x + column.x;
			if (x < columnLeft || x >= columnLeft + column.width) continue;
			int itemTop = menu.y + kContextMenuVerticalPadding;
			for (std::size_t index = column.begin; index < column.end; ++index) {
				const int itemHeight = ContextMenuItemHeight(index);
				if (y >= itemTop && y < itemTop + itemHeight) {
					return IsContextMenuItemSelectable(index) ? static_cast<int>(index) : -1;
				}
				itemTop += itemHeight;
			}
			return -1;
		}
		return -1;
	}

	void UpdateContextMenuSelection(int x, int y) {
		menuSelected_ = ContextMenuItemAt(x, y);
	}

	void OpenContextMenu(bool advancedOptions = false) {
		// A first right-click can arrive before SDL has delivered any motion
		// event. Read the current pointer state so both mouse and keyboard
		// invocation place the menu at the actual pointer position.
		int x = 0;
		int y = 0;
		SDL_GetMouseState(&x, &y);
		contextMenuAdvancedOptions_ = advancedOptions;
		contextMenuCropOnly_ = false;
		contextMenuItems_ = ContextMenuItems(contextMenuAdvancedOptions_);
		contextMenuX_ = x;
		contextMenuY_ = y;
		RepositionContextMenuToFit();
		contextMenuOpen_ = true;
		menuSelected_ = -1;
		contextMenuRightKeyDown_ = false;
	}

	void OpenCropContextMenu() {
		if (!cropSelection_.HasSelection()) return;
		contextMenuAdvancedOptions_ = false;
		contextMenuCropOnly_ = true;
		contextMenuItems_ = ContextMenuItems(false);
		contextMenuX_ = lastMouseX_;
		contextMenuY_ = lastMouseY_;
		RepositionContextMenuToFit();
		contextMenuOpen_ = true;
		menuSelected_ = -1;
		contextMenuRightKeyDown_ = false;
	}

	void CloseContextMenu() {
		if (!contextMenuOpen_) return;
		contextMenuOpen_ = false;
		contextMenuAdvancedOptions_ = false;
		contextMenuCropOnly_ = false;
		contextMenuPositionLocked_ = false;
		menuSelected_ = -1;
		contextMenuRightKeyDown_ = false;
		contextMenuItems_.clear();
		SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
		UpdateNavigationPanelVisibility(lastMouseX_, lastMouseY_);
		// Some SDL2 backends can leave a single stale pixel from the last
		// presented menu frame. Render one additional clean frame after closing
		// so the image underneath is fully restored before continuing.
		contextMenuNeedsCleanFrame_ = true;
	}

	void MoveContextMenuSelection(int direction) {
		if (menuSelected_ < 0) {
			menuSelected_ = jpegview_linux::NextMenuSelection(contextMenuItems_, menuSelected_, direction);
			return;
		}
		const std::vector<ContextMenuColumn> columns = ContextMenuColumns();
		for (const ContextMenuColumn& column : columns) {
			if (static_cast<std::size_t>(menuSelected_) < column.begin ||
				static_cast<std::size_t>(menuSelected_) >= column.end) continue;
			menuSelected_ = jpegview_linux::NextMenuSelectionInColumn(contextMenuItems_,
				{column.begin, column.end, column.height}, menuSelected_, direction);
			return;
		}
	}

	void MoveContextMenuSelectionAcrossColumns(int direction) {
		const std::vector<ContextMenuColumn> columns = ContextMenuColumns();
		std::vector<jpegview_linux::MenuColumn> layout;
		layout.reserve(columns.size());
		for (const ContextMenuColumn& column : columns) {
			layout.push_back({column.begin, column.end, column.height});
		}
		const int selection = jpegview_linux::AdjacentMenuSelection(contextMenuItems_, layout,
			menuSelected_, direction, ContextMenuRowHeight(), kContextMenuSeparatorHeight);
		if (selection >= 0) menuSelected_ = selection;
	}

	void ActivateContextMenuSelection(bool& running) {
		if (menuSelected_ < 0 || menuSelected_ >= static_cast<int>(contextMenuItems_.size()) ||
			contextMenuItems_[menuSelected_].separator || contextMenuItems_[menuSelected_].command == 0 ||
			!contextMenuItems_[menuSelected_].enabled) {
			return;
		}
		const int command = contextMenuItems_[menuSelected_].command;
		if (command == jpegview_linux::kContextMenuShowAdvanced) {
			contextMenuAdvancedOptions_ = true;
			contextMenuItems_ = ContextMenuItems(contextMenuAdvancedOptions_);
			RepositionContextMenuToFit();
			menuSelected_ = -1;
			return;
		}
		CloseContextMenu();
		ExecuteCommand(command);
		if (command == IDM_EXIT) running = false;
	}

	void ReleaseContextMenuRightKey(bool& running) {
		if (!contextMenuRightKeyDown_) return;
		contextMenuRightKeyDown_ = false;

		SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
		const int item = ContextMenuItemAt(lastMouseX_, lastMouseY_);
		if (item >= 0) {
			menuSelected_ = item;
			ActivateContextMenuSelection(running);
		} else if (!PointInRect(lastMouseX_, lastMouseY_, ContextMenuRect())) {
			MoveContextMenuSelectionAcrossColumns(1);
		}
	}

	SDL_Rect BatchCopyRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(1120, std::max(760, windowWidth - 40));
		const int height = std::min(760, std::max(520, windowHeight - 40));
		return SDL_Rect{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	SDL_Rect BatchCopyListRect() const {
		const SDL_Rect dialog = BatchCopyRect();
		const int listWidth = std::max(400, dialog.w * 3 / 5);
		return SDL_Rect{dialog.x + 14, dialog.y + 64, listWidth, dialog.h - 136};
	}

	int BatchCopyRightX() const {
		const SDL_Rect list = BatchCopyListRect();
		return list.x + list.w + 20;
	}

	int BatchCopyRightWidth() const {
		const SDL_Rect dialog = BatchCopyRect();
		return std::max(200, dialog.x + dialog.w - BatchCopyRightX() - 14);
	}

	SDL_Rect BatchCopyPatternRect() const {
		const SDL_Rect dialog = BatchCopyRect();
		return SDL_Rect{BatchCopyRightX(), dialog.y + 236, BatchCopyRightWidth(), 30};
	}

	SDL_Rect BatchCopyButtonRect(int button) const {
		const SDL_Rect dialog = BatchCopyRect();
		const SDL_Rect list = BatchCopyListRect();
		const int y = dialog.y + dialog.h - 54;
		const int height = 30;
		switch (button) {
		case kBatchSelectAll: return SDL_Rect{list.x, y, 100, height};
		case kBatchSelectNone: return SDL_Rect{list.x + 108, y, 108, height};
		case kBatchPreview: return SDL_Rect{list.x + 224, y, 90, height};
		case kBatchSavePattern: return SDL_Rect{list.x + 322, y, 118, height};
		case kBatchRename: return SDL_Rect{list.x + 448, y, 118, height};
		case kBatchClose: return SDL_Rect{list.x + 574, y, 80, height};
		default: return SDL_Rect{};
		}
	}

	int BatchCopyEntryAt(int x, int y) const {
		const SDL_Rect list = BatchCopyListRect();
		if (!PointInRect(x, y, list)) return -1;
		const int row = (y - list.y - 22) / 24;
		if (row < 0) return -1;
		const int item = static_cast<int>(batchCopyDialog_.Scroll()) + row;
		return item >= 0 && item < static_cast<int>(batchCopyDialog_.Items().size()) ? item : -1;
	}

	int BatchCopyButtonAt(int x, int y) const {
		for (int button = kBatchSelectAll; button <= kBatchClose; ++button) {
			if (PointInRect(x, y, BatchCopyButtonRect(button))) return button;
		}
		return -1;
	}

	int BatchCopyVisibleRows() const {
		const SDL_Rect list = BatchCopyListRect();
		return std::max(1, (list.h - 22) / 24);
	}

	std::vector<BatchCopyItem> CollectBatchCopyEntries() const {
		std::vector<BatchCopyItem> entries;
		for (const fs::path& filename : fileList_.Files()) {
			BatchCopyItem item;
			item.source = filename;
			item.modificationTime = FileModificationTime(filename);
			entries.push_back(std::move(item));
		}
		return entries;
	}

	void PreviewBatchCopy() {
		batchCopyDialog_.Preview();
	}

	void SaveBatchCopyPattern() {
		if (batchCopyDialog_.Pattern().empty()) {
			batchCopyDialog_.Preview();
			return;
		}
		copyRenamePattern_ = batchCopyDialog_.Pattern();
		SaveSettings();
		batchCopyDialog_.SetMessage("Saved batch pattern");
	}

	void PerformBatchCopy() {
		if (batchCopyDialog_.Pattern().empty()) {
			batchCopyDialog_.Preview();
			return;
		}
		batchCopyDialog_.Preview();
		fs::path preferredCurrentPath = fileList_.Current();
		int renamed = 0;
		int copied = 0;
		int createdDirectories = 0;
		int failed = 0;
		std::string firstFailure;
		for (BatchCopyItem& item : batchCopyDialog_.Items()) {
			if (!item.selected) continue;
			if (item.destination.empty() || item.destination == item.source) {
				++failed;
				if (firstFailure.empty()) firstFailure = item.source.filename().string() + " has no distinct target";
				continue;
			}
			std::error_code error;
			if (fs::exists(item.destination, error) || error) {
				++failed;
				if (firstFailure.empty()) firstFailure = item.destination.filename().string() + " already exists";
				continue;
			}
			if (item.copy) {
				const fs::path parent = item.destination.parent_path();
				if (!parent.empty()) {
					const bool created = fs::create_directories(parent, error);
					if (error) {
						++failed;
						if (firstFailure.empty()) firstFailure = "cannot create " + parent.string();
						continue;
					}
					if (created) ++createdDirectories;
				}
				if (!fs::copy_file(item.source, item.destination, fs::copy_options::none, error) || error) {
					++failed;
					if (firstFailure.empty()) firstFailure = "cannot copy " + item.source.filename().string();
					continue;
				}
				++copied;
			} else {
				fs::rename(item.source, item.destination, error);
				if (error) {
					++failed;
					if (firstFailure.empty()) firstFailure = "cannot rename " + item.source.filename().string();
					continue;
				}
				if (item.source == preferredCurrentPath) preferredCurrentPath = item.destination;
				++renamed;
			}
		}

		if (renamed > 0 || copied > 0) {
			RequestFileListScan(jpegview_linux::FileList::ScanOperation::Reload,
				0, FileListScanHandling::Reload, true, true, preferredCurrentPath);
		}
		batchCopyDialog_.ReplaceItems(CollectBatchCopyEntries(), fileList_.CurrentIndex(), BatchCopyVisibleRows());
		batchCopyDialog_.Preview();
		std::string message = "Completed: " + std::to_string(renamed) + " renamed, " +
			std::to_string(copied) + " copied, " + std::to_string(createdDirectories) + " folder(s) created";
		if (failed > 0) message += "; " + std::to_string(failed) + " failed" +
			(firstFailure.empty() ? std::string() : ": " + firstFailure);
		batchCopyDialog_.SetMessage(std::move(message));
	}

	void OpenBatchCopyDialog() {
		if (fileList_.Empty() || clipboardMode_) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Batch rename/copy is unavailable for images inside archives");
			return;
		}
		batchCopyDialog_.Open(CollectBatchCopyEntries(), fileList_.CurrentIndex(),
			copyRenamePattern_, BatchCopyVisibleRows());
		contextMenuOpen_ = false;
		fileDialogOpen_ = false;
		SDL_StartTextInput();
	}

	void CloseBatchCopyDialog() {
		SDL_StopTextInput();
		batchCopyDialog_.Close();
	}

	void HandleBatchCopyButton(int button) {
		switch (button) {
		case kBatchSelectAll:
			batchCopyDialog_.SelectAll(true);
			break;
		case kBatchSelectNone:
			batchCopyDialog_.SelectAll(false);
			break;
		case kBatchPreview:
			PreviewBatchCopy();
			break;
		case kBatchSavePattern:
			SaveBatchCopyPattern();
			break;
		case kBatchRename:
			PerformBatchCopy();
			break;
		case kBatchClose:
			CloseBatchCopyDialog();
			break;
		default:
			break;
		}
	}

	void HandleBatchCopyEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN: {
			if (event.key.repeat != 0) break;
			const Uint16 modifiers = event.key.keysym.mod;
			const bool ctrl = (modifiers & 0x00C0u) != 0;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseBatchCopyDialog();
			} else if (ctrl && event.key.keysym.sym == 'a') {
				batchCopyDialog_.SelectAll(true);
			} else if (event.key.keysym.sym == SDLK_TAB) {
				batchCopyDialog_.TogglePatternFocus();
			} else if (batchCopyDialog_.PatternFocused() && event.key.keysym.sym == SDLK_BACKSPACE) {
				batchCopyDialog_.BackspacePattern();
			} else if (!batchCopyDialog_.PatternFocused() && event.key.keysym.sym == SDLK_UP) {
				batchCopyDialog_.MoveCursor(-1, BatchCopyVisibleRows());
			} else if (!batchCopyDialog_.PatternFocused() && event.key.keysym.sym == SDLK_DOWN) {
				batchCopyDialog_.MoveCursor(1, BatchCopyVisibleRows());
			} else if (!batchCopyDialog_.PatternFocused() && event.key.keysym.sym == SDLK_SPACE) {
				batchCopyDialog_.ToggleItem(batchCopyDialog_.Cursor());
			} else if (event.key.keysym.sym == SDLK_RETURN) {
				PreviewBatchCopy();
			}
			break;
		}
		case SDL_TEXTINPUT:
			batchCopyDialog_.AppendPattern(event.text.text);
			break;
		case SDL_MOUSEWHEEL: {
			batchCopyDialog_.ScrollBy(-event.wheel.y, BatchCopyVisibleRows());
			break;
		}
		case SDL_MOUSEMOTION: {
			const int item = BatchCopyEntryAt(event.motion.x, event.motion.y);
			batchCopyDialog_.FocusItem(item);
			break;
		}
		case SDL_MOUSEBUTTONDOWN:
			if (event.button.button != SDL_BUTTON_LEFT) break;
			if (const int button = BatchCopyButtonAt(event.button.x, event.button.y); button >= 0) {
				HandleBatchCopyButton(button);
				break;
			}
			if (PointInRect(event.button.x, event.button.y, BatchCopyPatternRect())) {
				batchCopyDialog_.SetPatternFocused(true);
				break;
			}
			if (const int item = BatchCopyEntryAt(event.button.x, event.button.y); item >= 0) {
				batchCopyDialog_.ToggleItem(item);
			}
			break;
		default:
			break;
		}
	}

	void RenderBatchButton(int button, const std::string& label) {
		const SDL_Rect rect = BatchCopyButtonRect(button);
		const bool hovered = PointInRect(lastMouseX_, lastMouseY_, rect);
		SDL_SetRenderDrawColor(renderer_, hovered ? 52 : 28, hovered ? 78 : 28, hovered ? 108 : 28, 220);
		SDL_RenderFillRect(renderer_, &rect);
		DrawRect(rect, 125, 145, 165);
		DrawText(ClipText(label, rect.w - 12), rect.x + 6, rect.y + 10, kUiTextScale,
			255, 255, 255);
	}

	void RenderBatchCopy() {
		if (!batchCopyDialog_.IsOpen()) return;
		const SDL_Rect dialog = BatchCopyRect();
		const SDL_Rect list = BatchCopyListRect();
		const int rightX = BatchCopyRightX();
		const int rightWidth = BatchCopyRightWidth();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 224);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 190, 190, 190);
		DrawText("BATCH RENAME/COPY OF FILES", dialog.x + 18, dialog.y + 14, kUiTextScale);
		const std::string directory = fileList_.Empty() ? std::string() : fileList_.Current().parent_path().string();
		DrawText(ClipText("IMAGE FILES IN " + directory, dialog.w - 36), dialog.x + 18, dialog.y + 38,
			kUiTextScale, 170, 170, 170);

		SDL_SetRenderDrawColor(renderer_, 25, 25, 25, 215);
		SDL_RenderFillRect(renderer_, &list);
		DrawRect(list, 75, 75, 75);
		DrawText("SEL", list.x + 8, list.y + 7, kUiTextScale, 170, 170, 170);
		DrawText("OLD NAME", list.x + 42, list.y + 7, kUiTextScale, 170, 170, 170);
		DrawText("DATE", list.x + 245, list.y + 7, kUiTextScale, 170, 170, 170);
		DrawText("NEW NAME (>> COPY)", list.x + 380, list.y + 7, kUiTextScale, 170, 170, 170);

		const int rows = BatchCopyVisibleRows();
		for (int row = 0; row < rows; ++row) {
			const int itemIndex = static_cast<int>(batchCopyDialog_.Scroll()) + row;
			if (itemIndex >= static_cast<int>(batchCopyDialog_.Items().size())) break;
			const BatchCopyItem& item = batchCopyDialog_.Items()[static_cast<std::size_t>(itemIndex)];
			const int rowTop = list.y + 22 + row * 24;
			if (itemIndex == batchCopyDialog_.Cursor()) {
				SDL_SetRenderDrawColor(renderer_, 45, 82, 120, 205);
				SDL_Rect selection{list.x + 2, rowTop, list.w - 4, 22};
				SDL_RenderFillRect(renderer_, &selection);
			}
			DrawText(item.selected ? "[X]" : "[ ]", list.x + 8, rowTop + 6, kUiTextScale,
				item.selected ? 255 : 150, item.selected ? 255 : 150, item.selected ? 255 : 150);
			DrawText(ClipText(InfoText(item.source.filename().string()), 190), list.x + 42, rowTop + 6,
				kUiTextScale, 235, 235, 235);
		DrawText(ClipText(jpegview_linux::FormatBatchDate(item.modificationTime), 125), list.x + 245, rowTop + 6,
				kUiTextScale, 210, 210, 210);
			const std::string destination = item.destinationText.empty() ? "-" :
				std::string(item.copy ? ">> " : "") + InfoText(item.destinationText);
			DrawText(ClipText(destination, list.w - 390), list.x + 380, rowTop + 6, kUiTextScale,
				item.copy ? 255 : 220, item.copy ? 220 : 220, item.copy ? 150 : 220);
		}

		DrawText("PLACEHOLDERS", rightX, dialog.y + 68, kUiTextScale, 190, 210, 235);
		DrawText("%x  consecutive number   %Nx  padded number", rightX, dialog.y + 92, kUiTextScale, 205, 205, 205);
		DrawText("%n  number from filename  %f  original filename", rightX, dialog.y + 110, kUiTextScale, 205, 205, 205);
		DrawText("%F  filename without ext  %e  extension", rightX, dialog.y + 128, kUiTextScale, 205, 205, 205);
		DrawText("%d %m %y  day/month/year   %2y  short year", rightX, dialog.y + 146, kUiTextScale, 205, 205, 205);
		DrawText("%h %min  hour/minute       %M %3M  month text", rightX, dialog.y + 164, kUiTextScale, 205, 205, 205);
		DrawText("%pictures%  HOME/Pictures or XDG_PICTURES_DIR", rightX, dialog.y + 182, kUiTextScale, 205, 205, 205);
		DrawText("TARGET PATTERN (use / for folders)", rightX, dialog.y + 216, kUiTextScale, 190, 210, 235);
		const SDL_Rect patternRect = BatchCopyPatternRect();
		SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 220);
		SDL_RenderFillRect(renderer_, &patternRect);
		DrawRect(patternRect, batchCopyDialog_.PatternFocused() ? 100 : 75,
			batchCopyDialog_.PatternFocused() ? 130 : 75, batchCopyDialog_.PatternFocused() ? 165 : 75);
		DrawText(ClipText(batchCopyDialog_.Pattern(), patternRect.w - 16), patternRect.x + 8, patternRect.y + 10,
			kUiTextScale);
		if (!batchCopyDialog_.Message().empty()) {
			DrawText(ClipText(batchCopyDialog_.Message(), rightWidth), rightX, dialog.y + dialog.h - 88, kUiTextScale,
				235, 180, 130);
		}
		RenderBatchButton(kBatchSelectAll, "SELECT ALL");
		RenderBatchButton(kBatchSelectNone, "SELECT NONE");
		RenderBatchButton(kBatchPreview, "PREVIEW");
		RenderBatchButton(kBatchSavePattern, "SAVE TEMPLATE");
		RenderBatchButton(kBatchRename, "RENAME/COPY");
		RenderBatchButton(kBatchClose, "CLOSE");
	}

	SDL_Rect ResizeDialogRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(620, std::max(470, windowWidth - 40));
		const int height = std::min(360, std::max(320, windowHeight - 40));
		return SDL_Rect{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	SDL_Rect ResizeFieldRect(int field) const {
		const SDL_Rect dialog = ResizeDialogRect();
		return SDL_Rect{dialog.x + 190, dialog.y + 70 + field * 42, 220, 28};
	}

	SDL_Rect ResizeButtonRect(int button) const {
		const SDL_Rect dialog = ResizeDialogRect();
		const int y = dialog.y + dialog.h - 48;
		if (button == kResizeApply) return SDL_Rect{dialog.x + dialog.w - 198, y, 86, 30};
		if (button == kResizeCancel) return SDL_Rect{dialog.x + dialog.w - 102, y, 86, 30};
		return SDL_Rect{};
	}

	const char* ResizeFilterName() const {
		return resizeDialog_.Model().FilterName();
	}

	bool ResizeTarget(int& width, int& height) const {
		return resizeDialog_.Target(width, height);
	}

	void OpenResizeDialog() {
		if (image_.width <= 0 || image_.height <= 0) return;
		resizeDialog_.Open(image_.width, image_.height);
		contextMenuOpen_ = false;
		fileDialogOpen_ = false;
		batchCopyDialog_.Close();
		SDL_StartTextInput();
	}

	void CloseResizeDialog() {
		SDL_StopTextInput();
		resizeDialog_.Close();
	}

	void ApplyResizeDialog() {
		int width = 0;
		int height = 0;
		if (!ResizeTarget(width, height)) {
			resizeDialog_.SetMessage("Enter a valid size (maximum 65535 x 65535 / 100 MP)");
			return;
		}
		if (width == image_.width && height == image_.height) {
			CloseResizeDialog();
			return;
		}
		if (!MaterializeCurrentPixels()) {
			resizeDialog_.SetMessage("Cannot prepare image pixels");
			return;
		}
		const jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.Snapshot();
		Image resizedImage = correctionBaseValid_ ? correctionBase_ : image_;
		if (!resizedImage.Resize(width, height, resizeDialog_.Model().Filter())) {
			resizeDialog_.SetMessage("Resizing failed: not enough memory or the image is too large");
			return;
		}
		resizedImage.originalWidth = width;
		resizedImage.originalHeight = height;
		correctionBase_ = std::move(resizedImage);
		correctionBaseValid_ = true;
		image_ = correctionBase_;
		if (!image_.ApplyProcessing(imageProcessing_, autoContrastEnabled_)) {
			resizeDialog_.SetMessage("Image processing failed");
			return;
		}
		materializedProcessing_ = imageProcessing_;
		materializedAutoContrast_ = autoContrastEnabled_;
		if (!UpdateTexture()) {
			resizeDialog_.SetMessage("Resizing failed: could not update the display texture");
			return;
		}
		imageModified_ = true;
		currentSpreadRotationValid_ = false;
		++modifiedImageRevision_;
		RestoreScaleMode(viewportSnapshot);
		SetTitle();
		CloseResizeDialog();
	}

	void HandleResizeDialogEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN: {
			if (event.key.repeat != 0) break;
			const Uint16 modifiers = event.key.keysym.mod;
			const bool shift = (modifiers & 0x0003u) != 0;
			const bool ctrl = (modifiers & 0x00C0u) != 0;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseResizeDialog();
			} else if (event.key.keysym.sym == SDLK_RETURN) {
				ApplyResizeDialog();
			} else if (event.key.keysym.sym == SDLK_TAB) {
				resizeDialog_.MoveFocus(shift ? -1 : 1);
			} else if (ctrl && event.key.keysym.sym == 'a') {
				resizeDialog_.SelectAll();
			} else if (resizeDialog_.FocusedField() <= kResizeHeight && event.key.keysym.sym == SDLK_BACKSPACE) {
				resizeDialog_.Backspace();
			} else if (resizeDialog_.FocusedField() == kResizeFilter && event.key.keysym.sym == SDLK_LEFT) {
				resizeDialog_.CycleFilter(-1);
			} else if (resizeDialog_.FocusedField() == kResizeFilter && event.key.keysym.sym == SDLK_RIGHT) {
				resizeDialog_.CycleFilter(1);
			} else if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_LEFT) {
				resizeDialog_.MoveFocus(-1);
			} else if (event.key.keysym.sym == SDLK_DOWN || event.key.keysym.sym == SDLK_RIGHT) {
				resizeDialog_.MoveFocus(1);
			}
			break;
		}
		case SDL_TEXTINPUT:
			resizeDialog_.AppendText(event.text.text);
			break;
		case SDL_MOUSEMOTION:
			lastMouseX_ = event.motion.x;
			lastMouseY_ = event.motion.y;
			break;
		case SDL_MOUSEBUTTONDOWN:
			if (event.button.button != SDL_BUTTON_LEFT) break;
			if (PointInRect(event.button.x, event.button.y, ResizeButtonRect(kResizeApply))) {
				ApplyResizeDialog();
			} else if (PointInRect(event.button.x, event.button.y, ResizeButtonRect(kResizeCancel))) {
				CloseResizeDialog();
			} else {
				for (int field = kResizePercent; field <= kResizeFilter; ++field) {
					if (!PointInRect(event.button.x, event.button.y, ResizeFieldRect(field))) continue;
					if (field == kResizeFilter) resizeDialog_.CycleFilter(1);
					else resizeDialog_.SelectField(field);
					break;
				}
			}
			break;
		default:
			break;
		}
	}

	void RenderResizeButton(int button, const char* label) {
		const SDL_Rect rect = ResizeButtonRect(button);
		const bool hovered = PointInRect(lastMouseX_, lastMouseY_, rect);
		SDL_SetRenderDrawColor(renderer_, hovered ? 52 : 28, hovered ? 78 : 28, hovered ? 108 : 28, 220);
		SDL_RenderFillRect(renderer_, &rect);
		DrawRect(rect, 125, 145, 165);
		DrawText(label, rect.x + 12, rect.y + 10, kUiTextScale, 255, 255, 255);
	}

	void RenderResizeDialog() {
		if (!resizeDialog_.IsOpen()) return;
		const SDL_Rect dialog = ResizeDialogRect();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 232);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 190, 190, 190);
		DrawText("RESIZE IMAGE", dialog.x + 20, dialog.y + 16, kUiTextScale, 255, 255, 255);
		DrawText("ORIGINAL SIZE", dialog.x + 20, dialog.y + 43, kUiTextScale, 180, 195, 215);
		DrawText(std::to_string(resizeDialog_.Model().OriginalWidth()) + "X" +
			std::to_string(resizeDialog_.Model().OriginalHeight()),
			dialog.x + 190, dialog.y + 43, kUiTextScale, 220, 220, 220);

		const char* labels[] = {"NEW SIZE", "NEW WIDTH", "NEW HEIGHT", "FILTER"};
		for (int field = kResizePercent; field <= kResizeFilter; ++field) {
			const SDL_Rect rect = ResizeFieldRect(field);
			const bool focused = resizeDialog_.FocusedField() == field;
			SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 225);
			SDL_RenderFillRect(renderer_, &rect);
			DrawRect(rect, focused ? 100 : 75, focused ? 130 : 75, focused ? 165 : 75);
			DrawText(labels[field], dialog.x + 20, rect.y + 9, kUiTextScale, 205, 215, 230);
			const std::string value = field == kResizeFilter ? ResizeFilterName() : resizeDialog_.Model().FieldText(field);
			DrawText(ClipText(value, rect.w - 16), rect.x + 8, rect.y + 9, kUiTextScale, 255, 255, 255);
			if (field == kResizePercent) DrawText("%", rect.x + rect.w + 10, rect.y + 9, kUiTextScale, 185, 185, 185);
			if (field == kResizeWidth || field == kResizeHeight) {
				DrawText("PIXELS", rect.x + rect.w + 10, rect.y + 9, kUiTextScale, 185, 185, 185);
			}
		}
		if (!resizeDialog_.Message().empty()) {
			DrawText(ClipText(resizeDialog_.Message(), dialog.w - 40), dialog.x + 20, dialog.y + dialog.h - 82,
				kUiTextScale, 235, 180, 130);
		}
		DrawText("TAB: NEXT FIELD   ARROWS: CHANGE FILTER/FIELD   ENTER: APPLY   ESC: CANCEL",
			dialog.x + 20, dialog.y + dialog.h - 62, kUiTextScale, 160, 160, 160);
		RenderResizeButton(kResizeApply, "APPLY");
		RenderResizeButton(kResizeCancel, "CANCEL");
	}

	SDL_Rect CropSizeDialogRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(460, std::max(360, windowWidth - 40));
		const int height = 290;
		return SDL_Rect{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	SDL_Rect CropSizeFieldRect(int field) const {
		const SDL_Rect dialog = CropSizeDialogRect();
		return SDL_Rect{dialog.x + 190, dialog.y + 68 + field * 42, 190, 30};
	}

	SDL_Rect CropSizeUnitRect(bool screenPixels) const {
		const SDL_Rect dialog = CropSizeDialogRect();
		return screenPixels ? SDL_Rect{dialog.x + 20, dialog.y + 164, 185, 30} :
			SDL_Rect{dialog.x + 218, dialog.y + 164, 185, 30};
	}

	SDL_Rect CropSizeButtonRect(int button) const {
		const SDL_Rect dialog = CropSizeDialogRect();
		const int y = dialog.y + dialog.h - 48;
		if (button == kCropSizeApply) return SDL_Rect{dialog.x + dialog.w - 198, y, 86, 30};
		if (button == kCropSizeCancel) return SDL_Rect{dialog.x + dialog.w - 102, y, 86, 30};
		return {};
	}

	void OpenFixedCropSizeDialog() {
		cropSizeDialog_.Open(cropSelection_.FixedWidth(), cropSelection_.FixedHeight(),
			cropSelection_.FixedSizeUsesScreenPixels());
		contextMenuOpen_ = false;
		fileDialogOpen_ = false;
		batchCopyDialog_.Close();
		resizeDialog_.Close();
		SDL_StartTextInput();
		SetTitle("Set fixed crop size");
	}

	void CloseFixedCropSizeDialog() {
		if (!cropSizeDialog_.IsOpen()) return;
		SDL_StopTextInput();
		cropSizeDialog_.Close();
		SetTitle();
	}

	void ApplyFixedCropSizeDialog() {
		int width = 0;
		int height = 0;
		bool screenPixels = true;
		if (!cropSizeDialog_.Apply(width, height, screenPixels)) return;
		cropSelection_.SetFixedSize(width, height, screenPixels);
		cropSelection_.ReapplyMode(viewport_.Zoom());
		SetSelectionModeEnabled(true);
		SaveSettings();
		CloseFixedCropSizeDialog();
	}

	void RenderCropSizeButton(int button, const char* label) {
		const SDL_Rect rect = CropSizeButtonRect(button);
		const bool hovered = PointInRect(lastMouseX_, lastMouseY_, rect);
		SDL_SetRenderDrawColor(renderer_, hovered ? 52 : 28, hovered ? 78 : 28,
			hovered ? 108 : 28, 220);
		SDL_RenderFillRect(renderer_, &rect);
		DrawRect(rect, 125, 145, 165);
		DrawText(label, rect.x + 12, rect.y + 10, kUiTextScale, 255, 255, 255);
	}

	void RenderFixedCropSizeDialog() {
		if (!cropSizeDialog_.IsOpen()) return;
		const SDL_Rect dialog = CropSizeDialogRect();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 232);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 190, 190, 190);
		DrawText("SET FIXED CROP SIZE", dialog.x + 20, dialog.y + 16,
			kUiTextScale, 255, 255, 255);
		const char* labels[] = {"WIDTH", "HEIGHT"};
		const std::string values[] = {cropSizeDialog_.WidthText(), cropSizeDialog_.HeightText()};
		for (int field = 0; field < 2; ++field) {
			const SDL_Rect rect = CropSizeFieldRect(field);
			const bool focused = cropSizeDialog_.FocusedField() == field;
			SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 225);
			SDL_RenderFillRect(renderer_, &rect);
			DrawRect(rect, focused ? 100 : 75, focused ? 130 : 75, focused ? 165 : 75);
			DrawText(labels[field], dialog.x + 20, rect.y + 10, kUiTextScale, 205, 215, 230);
			DrawText(ClipText(values[field], rect.w - 16), rect.x + 8, rect.y + 10,
				kUiTextScale, 255, 255, 255);
		}
		for (const bool screenPixels : {true, false}) {
			const SDL_Rect rect = CropSizeUnitRect(screenPixels);
			const bool selected = cropSizeDialog_.UsesScreenPixels() == screenPixels;
			SDL_SetRenderDrawColor(renderer_, selected ? 45 : 28, selected ? 68 : 28,
				selected ? 92 : 28, 220);
			SDL_RenderFillRect(renderer_, &rect);
			DrawRect(rect, selected ? 120 : 75, selected ? 150 : 75, selected ? 190 : 75);
			DrawText(screenPixels ? "Screen pixels" : "Image pixels", rect.x + 10,
				rect.y + 9, kUiTextScale, 235, 235, 235);
		}
		if (!cropSizeDialog_.Message().empty()) {
			DrawText(ClipText(cropSizeDialog_.Message(), dialog.w - 40), dialog.x + 20,
				dialog.y + 222, kUiTextScale, 235, 180, 130);
		}
		DrawText("Screen-pixel sizes follow zoom; image-pixel sizes use source pixels.",
			dialog.x + 20, dialog.y + 201, kUiTextScale, 160, 160, 160);
		RenderCropSizeButton(kCropSizeApply, "APPLY");
		RenderCropSizeButton(kCropSizeCancel, "CANCEL");
	}

	void HandleFixedCropSizeDialogEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN: {
			if (event.key.repeat != 0) break;
			const bool control = (event.key.keysym.mod & 0x00c0u) != 0;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseFixedCropSizeDialog();
			} else if (event.key.keysym.sym == SDLK_RETURN) {
				ApplyFixedCropSizeDialog();
			} else if (event.key.keysym.sym == SDLK_TAB || event.key.keysym.sym == SDLK_UP ||
				event.key.keysym.sym == SDLK_DOWN) {
				cropSizeDialog_.MoveFocus(1);
			} else if (control && event.key.keysym.sym == 'a') {
				cropSizeDialog_.SelectAll();
			} else if (event.key.keysym.sym == SDLK_BACKSPACE) {
				cropSizeDialog_.Backspace();
			} else if (event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_RIGHT) {
				cropSizeDialog_.ToggleUnits();
			}
			break;
		}
		case SDL_TEXTINPUT:
			cropSizeDialog_.AppendText(event.text.text);
			break;
		case SDL_MOUSEMOTION:
			lastMouseX_ = event.motion.x;
			lastMouseY_ = event.motion.y;
			break;
		case SDL_MOUSEBUTTONDOWN:
			if (event.button.button != SDL_BUTTON_LEFT) break;
			if (PointInRect(event.button.x, event.button.y, CropSizeButtonRect(kCropSizeApply))) {
				ApplyFixedCropSizeDialog();
			} else if (PointInRect(event.button.x, event.button.y,
				CropSizeButtonRect(kCropSizeCancel))) {
				CloseFixedCropSizeDialog();
			} else if (PointInRect(event.button.x, event.button.y, CropSizeFieldRect(0))) {
				cropSizeDialog_.SelectField(jpegview_linux::CropSizeDialogController::kWidthField);
			} else if (PointInRect(event.button.x, event.button.y, CropSizeFieldRect(1))) {
				cropSizeDialog_.SelectField(jpegview_linux::CropSizeDialogController::kHeightField);
			} else if (PointInRect(event.button.x, event.button.y, CropSizeUnitRect(true))) {
				cropSizeDialog_.SetScreenPixels(true);
			} else if (PointInRect(event.button.x, event.button.y, CropSizeUnitRect(false))) {
				cropSizeDialog_.SetScreenPixels(false);
			}
			break;
		default:
			break;
		}
	}

	SDL_Rect FileDialogRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int maximumWidth = std::max(kFileDialogMinimumWidth, windowWidth - 40);
		const int maximumHeight = std::max(kFileDialogMinimumHeight, windowHeight - 40);
		const int width = std::clamp(fileDialogWidth_, kFileDialogMinimumWidth, maximumWidth);
		const int height = std::clamp(fileDialogHeight_, kFileDialogMinimumHeight, maximumHeight);
		const int x = std::clamp(fileDialogX_, 0, std::max(0, windowWidth - width));
		const int y = std::clamp(fileDialogY_, 0, std::max(0, windowHeight - height));
		return SDL_Rect{x, y, width, height};
	}

	void PositionFileDialogGeometry() {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const SDL_Rect dialog = FileDialogRect();
		fileDialogX_ = std::max(0, (windowWidth - dialog.w) / 2);
		fileDialogY_ = std::max(0, (windowHeight - dialog.h) / 2);
		fileDialogDragMode_ = FileDialogDragMode::None;
	}

	int FileDialogListTop() const {
		return FileDialogRect().y + 112;
	}

	bool FileDialogCanRemoveRecent() const {
		return FileDialogHasTabs() && fileDialogTab_ == FileDialogTab::Recents;
	}

	SDL_Rect FileDialogRemoveRecentButtonRect() const {
		const SDL_Rect dialog = FileDialogRect();
		return SDL_Rect{dialog.x + dialog.w - 90, dialog.y + 40, 72, 24};
	}

	SDL_Rect FileDialogTabRect(FileDialogTab tab) const {
		const SDL_Rect dialog = FileDialogRect();
		const int width = tab == FileDialogTab::Browse ? 62 : 70;
		const int x = dialog.x + dialog.w - (tab == FileDialogTab::Browse ? 150 : 82);
		return SDL_Rect{x, dialog.y + 8, width, 28};
	}

	int FileDialogVisibleRows() const {
		return std::max(1, (FileDialogRect().h - 168) / 26);
	}

	SDL_Rect FileDialogInputRect() const {
		const SDL_Rect dialog = FileDialogRect();
		return SDL_Rect{dialog.x + 12, dialog.y + 86, dialog.w - 24, 28};
	}

	SDL_Rect FileDialogSortRect() const {
		const SDL_Rect dialog = FileDialogRect();
		return SDL_Rect{dialog.x + dialog.w - 158, dialog.y + 61, 140, 23};
	}

	bool FileDialogHasPreviewColumn() const {
		return !fileDialogSave_ && !fileDialogParameterRestore_ && FileDialogRect().w >= 560;
	}

	bool FileDialogCanSort() const {
		return !fileDialogSave_ && !fileDialogParameterRestore_ &&
			fileDialogTab_ == FileDialogTab::Browse;
	}

	SDL_Rect FileDialogListRect() const {
		const SDL_Rect dialog = FileDialogRect();
		const int previewWidth = FileDialogHasPreviewColumn() ?
			FileDialogPreviewWidth() + kFileDialogDividerWidth : 0;
		const int width = dialog.w - 24 - previewWidth;
		return SDL_Rect{dialog.x + 12, FileDialogListTop(), width,
			FileDialogVisibleRows() * 26};
	}

	SDL_Rect FileDialogListContentRect() const {
		SDL_Rect content = FileDialogListRect();
		content.w = std::max(1, content.w - kFileDialogScrollbarWidth);
		return content;
	}

	SDL_Rect FileDialogScrollbarRect() const {
		const SDL_Rect list = FileDialogListRect();
		return SDL_Rect{list.x + list.w - kFileDialogScrollbarWidth + 1, list.y + 2,
			kFileDialogScrollbarWidth - 2, std::max(1, list.h - 4)};
	}

	jpegview_linux::FileDialogScrollbarGeometry FileDialogScrollGeometry() const {
		const SDL_Rect track = FileDialogScrollbarRect();
		const jpegview_linux::FileDialogModel& model = ActiveFileDialogModel();
		return jpegview_linux::CalculateFileDialogScrollbarGeometry(
			static_cast<int>(model.Entries().size()), FileDialogVisibleRows(), model.Scroll(),
			track.y, track.h, kFileDialogScrollbarMinimumThumbHeight);
	}

	SDL_Rect FileDialogScrollbarThumbRect() const {
		const SDL_Rect track = FileDialogScrollbarRect();
		const jpegview_linux::FileDialogScrollbarGeometry geometry = FileDialogScrollGeometry();
		return SDL_Rect{track.x + 2, geometry.thumbY, std::max(1, track.w - 4),
			geometry.thumbHeight};
	}

	int FileDialogPreviewWidth() const {
		const SDL_Rect dialog = FileDialogRect();
		const int availableWidth = dialog.w - 24 - kFileDialogDividerWidth;
		const int defaultWidth = std::min(260, std::max(200, dialog.w / 3));
		const int requestedWidth = fileDialogPreviewRatio_ > 0.0 ?
			static_cast<int>(std::lround(fileDialogPreviewRatio_ * availableWidth)) : defaultWidth;
		const int maximumWidth = std::max(kFileDialogMinimumPreviewWidth,
			availableWidth - kFileDialogMinimumListWidth);
		return std::clamp(requestedWidth, kFileDialogMinimumPreviewWidth, maximumWidth);
	}

	SDL_Rect FileDialogDividerRect() const {
		const SDL_Rect list = FileDialogListRect();
		return SDL_Rect{list.x + list.w, list.y, kFileDialogDividerWidth, list.h};
	}

	SDL_Rect FileDialogPreviewRect() const {
		const SDL_Rect dialog = FileDialogRect();
		const int width = FileDialogPreviewWidth();
		return SDL_Rect{dialog.x + dialog.w - 12 - width, FileDialogListTop(), width,
			FileDialogVisibleRows() * 26};
	}

	SDL_Rect FileDialogPreviewImageRect(const SDL_Rect& previewRect) const {
		const jpegview_linux::FileDialogPreviewSize imageSize =
			jpegview_linux::FileDialogPreviewImageSize(previewRect.w, previewRect.h);
		return SDL_Rect{previewRect.x + 8, previewRect.y + 28,
			imageSize.width, imageSize.height};
	}

	SDL_Rect FileDialogResizeHandleRect() const {
		const SDL_Rect dialog = FileDialogRect();
		return SDL_Rect{dialog.x + dialog.w - kFileDialogResizeHandleSize,
			dialog.y + dialog.h - kFileDialogResizeHandleSize,
			kFileDialogResizeHandleSize, kFileDialogResizeHandleSize};
	}

	void KeepFileDialogSelectionVisible() {
		jpegview_linux::FileDialogModel& model = ActiveFileDialogModel();
		const int selected = model.SelectedIndex();
		if (selected >= 0) model.Select(selected, FileDialogVisibleRows());
	}

	void ResizeFileDialog(int x, int y) {
		if (fileDialogDragMode_ == FileDialogDragMode::Resize) {
			int windowWidth = 0;
			int windowHeight = 0;
			SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
			const int availableWidth = std::max(1,
				windowWidth - fileDialogDragStartRect_.x - 20);
			const int availableHeight = std::max(1,
				windowHeight - fileDialogDragStartRect_.y - 20);
			fileDialogWidth_ = std::clamp(fileDialogDragStartRect_.w + x - fileDialogDragStartX_,
				std::min(kFileDialogMinimumWidth, availableWidth), availableWidth);
			fileDialogHeight_ = std::clamp(fileDialogDragStartRect_.h + y - fileDialogDragStartY_,
				std::min(kFileDialogMinimumHeight, availableHeight), availableHeight);
			fileDialogX_ = fileDialogDragStartRect_.x;
			fileDialogY_ = fileDialogDragStartRect_.y;
			KeepFileDialogSelectionVisible();
		} else if (fileDialogDragMode_ == FileDialogDragMode::PreviewDivider) {
			const SDL_Rect dialog = FileDialogRect();
			const int availableWidth = dialog.w - 24 - kFileDialogDividerWidth;
			const int maximumWidth = std::max(kFileDialogMinimumPreviewWidth,
				availableWidth - kFileDialogMinimumListWidth);
			const int previewWidth = std::clamp(fileDialogDragStartPreviewWidth_ +
				fileDialogDragStartX_ - x, kFileDialogMinimumPreviewWidth, maximumWidth);
			fileDialogPreviewRatio_ = static_cast<double>(previewWidth) / availableWidth;
		} else if (fileDialogDragMode_ == FileDialogDragMode::Scrollbar) {
			const jpegview_linux::FileDialogScrollbarGeometry geometry = FileDialogScrollGeometry();
			const int thumbY = y - fileDialogScrollbarGrabOffset_;
			ActiveFileDialogModel().ScrollTo(
				jpegview_linux::FileDialogScrollForThumbPosition(geometry, thumbY),
				FileDialogVisibleRows());
		}
	}

	bool BeginFileDialogResize(int x, int y) {
		if (!PointInRect(x, y, FileDialogResizeHandleRect())) return false;
		fileDialogDragMode_ = FileDialogDragMode::Resize;
		fileDialogDragStartX_ = x;
		fileDialogDragStartY_ = y;
		fileDialogDragStartRect_ = FileDialogRect();
		SDL_CaptureMouse(SDL_TRUE);
		UpdateFileDialogCursor(x, y);
		return true;
	}

	bool BeginFileDialogPreviewResize(int x, int y) {
		if (!FileDialogHasPreviewColumn() || !PointInRect(x, y, FileDialogDividerRect())) return false;
		fileDialogDragMode_ = FileDialogDragMode::PreviewDivider;
		fileDialogDragStartX_ = x;
		fileDialogDragStartPreviewWidth_ = FileDialogPreviewWidth();
		SDL_CaptureMouse(SDL_TRUE);
		UpdateFileDialogCursor(x, y);
		return true;
	}

	bool BeginFileDialogScrollbarInteraction(int x, int y) {
		if (!PointInRect(x, y, FileDialogScrollbarRect())) return false;
		const jpegview_linux::FileDialogScrollbarGeometry geometry = FileDialogScrollGeometry();
		if (!geometry.scrollable) return true;
		const SDL_Rect thumb = FileDialogScrollbarThumbRect();
		if (PointInRect(x, y, thumb)) {
			fileDialogDragMode_ = FileDialogDragMode::Scrollbar;
			fileDialogScrollbarGrabOffset_ = y - geometry.thumbY;
			SDL_CaptureMouse(SDL_TRUE);
		} else {
			const int direction = y < geometry.thumbY + geometry.thumbHeight / 2 ? -1 : 1;
			ActiveFileDialogModel().ScrollBy(direction * FileDialogVisibleRows(),
				FileDialogVisibleRows());
		}
		return true;
	}

	void EndFileDialogResize(int x, int y) {
		if (fileDialogDragMode_ == FileDialogDragMode::None) return;
		const bool savePersistentSettings = fileDialogDragMode_ != FileDialogDragMode::Scrollbar;
		fileDialogDragMode_ = FileDialogDragMode::None;
		SDL_CaptureMouse(SDL_FALSE);
		UpdateFileDialogCursor(x, y);
		if (savePersistentSettings) SaveSettings();
	}

	void UpdateFileDialogCursor(int x, int y) const {
		if (fileDialogDragMode_ == FileDialogDragMode::Resize ||
			PointInRect(x, y, FileDialogResizeHandleRect())) {
			if (fileDialogResizeCursor_ != nullptr) SDL_SetCursor(fileDialogResizeCursor_);
		} else if (fileDialogDragMode_ == FileDialogDragMode::Scrollbar ||
			(FileDialogScrollGeometry().scrollable &&
				PointInRect(x, y, FileDialogScrollbarRect()))) {
			if (fileDialogScrollbarCursor_ != nullptr) SDL_SetCursor(fileDialogScrollbarCursor_);
		} else if (fileDialogDragMode_ == FileDialogDragMode::PreviewDivider ||
			(FileDialogHasPreviewColumn() && PointInRect(x, y, FileDialogDividerRect()))) {
			if (thumbnailResizeCursor_ != nullptr) SDL_SetCursor(thumbnailResizeCursor_);
		} else {
			SDL_SetCursor(SDL_GetDefaultCursor());
		}
	}

	void ClearFileDialogPreview() {
		fileDialogPreviewLoader_.Clear();
		if (fileDialogPreviewTexture_ != nullptr) SDL_DestroyTexture(fileDialogPreviewTexture_);
		fileDialogPreviewTexture_ = nullptr;
		fileDialogPreviewWidth_ = 0;
		fileDialogPreviewHeight_ = 0;
		fileDialogPreviewSourceWidth_ = 0;
		fileDialogPreviewSourceHeight_ = 0;
		fileDialogPreviewFileSize_ = 0;
		fileDialogPreviewFileSizeKnown_ = false;
		fileDialogPreviewHasTransparency_ = false;
		fileDialogPreviewRequestKey_.clear();
		fileDialogPreviewContentKey_.clear();
		fileDialogPreviewSource_.clear();
		fileDialogPreviewMessage_.clear();
		fileDialogPreviewGeneration_ = 0;
	}

	void InvalidateFileDialogPreview() {
		ClearFileDialogPreview();
	}

	void UpdateFileDialogPreview() {
		if (!fileDialogOpen_ || fileDialogSave_ || !FileDialogHasPreviewColumn()) {
			if (!fileDialogPreviewRequestKey_.empty()) ClearFileDialogPreview();
			return;
		}

		const SDL_Rect previewRect = FileDialogPreviewRect();
		const jpegview_linux::FileDialogPreviewSize previewSize =
			jpegview_linux::FileDialogPreviewImageSize(previewRect.w, previewRect.h);
		jpegview_linux::FileDialogModel& model = ActiveFileDialogModel();
		const FileDialogEntry* selected = model.SelectedEntry();
		std::string contentKey;
		std::string requestKey;
		if (selected != nullptr) {
			contentKey = selected->path.string() + (selected->directory ? "\nD\n" : "\nF\n") +
				(model.SortMode() == jpegview_linux::FileDialogSortMode::Name ? "N" : "M") +
				"\n";
			const jpegview_linux::SourceIdentity& identity =
				selected->sourceDescriptor.BackingIdentity();
			contentKey += std::to_string(identity.valid) + ":" +
				std::to_string(identity.device) + ":" + std::to_string(identity.inode) + ":" +
				std::to_string(identity.size) + ":" +
				std::to_string(identity.modifiedSeconds) + ":" +
				std::to_string(identity.modifiedNanoseconds) + "\n";
			requestKey = contentKey + std::to_string(previewSize.width) + "x" +
				std::to_string(previewSize.height);
		}
		if (requestKey != fileDialogPreviewRequestKey_ &&
			fileDialogDragMode_ == FileDialogDragMode::None) {
			const bool sameContent = contentKey == fileDialogPreviewContentKey_;
			if (!sameContent) {
				if (fileDialogPreviewTexture_ != nullptr) SDL_DestroyTexture(fileDialogPreviewTexture_);
				fileDialogPreviewTexture_ = nullptr;
				fileDialogPreviewWidth_ = 0;
				fileDialogPreviewHeight_ = 0;
				fileDialogPreviewSourceWidth_ = 0;
				fileDialogPreviewSourceHeight_ = 0;
				fileDialogPreviewFileSize_ = 0;
				fileDialogPreviewFileSizeKnown_ = false;
				fileDialogPreviewHasTransparency_ = false;
				fileDialogPreviewSource_.clear();
				fileDialogPreviewMessage_.clear();
				fileDialogPreviewContentKey_ = contentKey;
			}
			fileDialogPreviewRequestKey_ = requestKey;
			if (selected == nullptr) {
				fileDialogPreviewLoader_.Clear();
				fileDialogPreviewGeneration_ = 0;
			} else if (selected->encrypted &&
				!jpegview_linux::HasSessionArchivePassword(selected->path)) {
				fileDialogPreviewLoader_.Clear();
				fileDialogPreviewGeneration_ = 0;
				fileDialogPreviewMessage_ = "Encrypted image — open to enter password";
			} else {
				fileDialogPreviewGeneration_ = fileDialogPreviewLoader_.Request(selected->path,
					selected->directory, model.SortMode(), previewSize.width,
					previewSize.height, selected->sourceDescriptor);
				if (!sameContent || fileDialogPreviewTexture_ == nullptr) {
					fileDialogPreviewMessage_ = "Loading preview...";
				}
			}
		}

		for (jpegview_linux::FileDialogPreviewResult& result : fileDialogPreviewLoader_.TakeReady()) {
			if (result.generation != fileDialogPreviewGeneration_) continue;
			if (!result.observedSource.LogicalPath().empty()) {
				const bool requestedSourceMatches =
					!result.requestedSourceDescriptor.LogicalPath().empty() &&
					result.requestedSourceDescriptor.LogicalPath().lexically_normal() ==
						result.observedSource.LogicalPath().lexically_normal();
				const jpegview_linux::SourceDescriptor& requestedSource =
					requestedSourceMatches || result.sourceDescriptor.LogicalPath().empty() ?
						result.requestedSourceDescriptor : result.sourceDescriptor;
				if (jpegview_linux::ShouldRefreshFileDialogPreviewSource(
					requestedSource.Key(), result.observedSource)) {
					if (!result.requestedSourceDescriptor.LogicalPath().empty()) {
						model.RefreshSourceDescriptor(
							result.requestedSourceDescriptor.Key(), result.observedSource);
					}
					if (!result.sourceDescriptor.LogicalPath().empty()) {
						model.RefreshSourceDescriptor(result.sourceDescriptor.Key(),
							result.observedSource);
						ApplySourceChange(requestedSourceMatches ?
							result.requestedSourceDescriptor.Key() :
							result.sourceDescriptor.Key(), result.observedSource);
					}
					if (fileDialogPreviewTexture_ != nullptr) {
						SDL_DestroyTexture(fileDialogPreviewTexture_);
						fileDialogPreviewTexture_ = nullptr;
					}
					fileDialogPreviewWidth_ = 0;
					fileDialogPreviewHeight_ = 0;
					fileDialogPreviewRequestKey_.clear();
				}
			}
			fileDialogPreviewSource_ = result.source;
			fileDialogPreviewSourceWidth_ = result.sourceWidth;
			fileDialogPreviewSourceHeight_ = result.sourceHeight;
			fileDialogPreviewFileSize_ = result.fileSize;
			fileDialogPreviewFileSizeKnown_ = result.fileSizeKnown;
			fileDialogPreviewMessage_ = result.error;
			if (result.encryptedArchive ||
				result.errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired) {
				const fs::path backing = jpegview_linux::ArchiveBackingFile(result.source);
				encryptedArchivePaths_.insert(AbsoluteNormalized(backing).string());
				model.MarkEncrypted(AbsoluteNormalized(backing));
			} else if (result.errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword) {
				jpegview_linux::ForgetSessionArchivePassword(result.source);
			}
			if (result.error.find("incorrect archive password") != std::string::npos) {
				jpegview_linux::ForgetSessionArchivePassword(result.source);
				fileDialogPreviewMessage_ = "Saved password is incorrect — reopen archive to retry";
			}
			if (result.error.find("password required") != std::string::npos) {
				fileDialogPreviewMessage_ = "Encrypted image — open to enter password";
			}
			if (!result.bgra.empty()) {
				jpegview_linux::PerfContextScope previewUploadContext(
					jpegview_linux::PerfWorkClass::FocusedPreview,
					jpegview_linux::PerfExecution::EventThread);
				SDL_Texture* previewTexture = CreateTexture(result.bgra, result.width,
					result.height, result.hasTransparency);
				if (previewTexture == nullptr) {
					fileDialogPreviewMessage_ = "Cannot create preview";
				} else {
					if (fileDialogPreviewTexture_ != nullptr) {
						SDL_DestroyTexture(fileDialogPreviewTexture_);
					}
					fileDialogPreviewTexture_ = previewTexture;
					fileDialogPreviewWidth_ = result.width;
					fileDialogPreviewHeight_ = result.height;
					fileDialogPreviewHasTransparency_ = result.hasTransparency;
				}
			}
		}
	}

	void RenderFileDialogPreview(const SDL_Rect& previewRect) {
		SDL_SetRenderDrawColor(renderer_, 25, 25, 25, 210);
		SDL_RenderFillRect(renderer_, &previewRect);
		DrawRect(previewRect, 75, 75, 75);
		DrawText("Preview", previewRect.x + 8, previewRect.y + 6, kUiTextScale,
			190, 205, 220);

		const SDL_Rect imageRect = FileDialogPreviewImageRect(previewRect);
		if (fileDialogPreviewTexture_ != nullptr && fileDialogPreviewWidth_ > 0 &&
			fileDialogPreviewHeight_ > 0) {
			const double scale = std::min({1.0,
				static_cast<double>(imageRect.w) / fileDialogPreviewWidth_,
				static_cast<double>(imageRect.h) / fileDialogPreviewHeight_});
			const int width = std::max(1, static_cast<int>(fileDialogPreviewWidth_ * scale));
			const int height = std::max(1, static_cast<int>(fileDialogPreviewHeight_ * scale));
			const SDL_Rect destination{imageRect.x + (imageRect.w - width) / 2,
				imageRect.y + (imageRect.h - height) / 2, width, height};
			if (fileDialogPreviewHasTransparency_) {
				RenderTransparencyBackground(destination, imageRect);
			}
			SDL_RenderCopy(renderer_, fileDialogPreviewTexture_, nullptr, &destination);
		} else if (!fileDialogPreviewMessage_.empty()) {
			const int textWidth = TextWidth(fileDialogPreviewMessage_, kUiTextScale);
			DrawText(ClipText(fileDialogPreviewMessage_, imageRect.w - 12),
				imageRect.x + std::max(6, (imageRect.w - textWidth) / 2),
				imageRect.y + std::max(0, (imageRect.h - 12) / 2), kUiTextScale,
				165, 165, 165);
		}
		if (!fileDialogPreviewSource_.empty()) {
			const std::string filename = fileDialogPreviewSource_.filename().string();
			std::string dimensions;
			if (fileDialogPreviewSourceWidth_ > 0 && fileDialogPreviewSourceHeight_ > 0) {
				const std::string formattedSize = fileDialogPreviewFileSizeKnown_ ?
					jpegview_linux::FormatFileSize(fileDialogPreviewFileSize_) : std::string();
				dimensions = jpegview_linux::FormatImageDimensionsAndSize(
					fileDialogPreviewSourceWidth_, fileDialogPreviewSourceHeight_, formattedSize);
			}
			const int footerX = previewRect.x + 8;
			const int footerWidth = std::max(0, previewRect.w - 16);
			const jpegview_linux::FileDialogPreviewFooterLayout footerLayout =
				jpegview_linux::CalculateFileDialogPreviewFooterLayout(footerWidth,
					TextWidth(filename, kUiTextScale), TextWidth(dimensions, kUiTextScale));
			const std::string visibleFilename = ClipText(filename, footerLayout.filenameWidth);
			const int footerY = previewRect.y + previewRect.h - 19;
			DrawText(visibleFilename, footerX, footerY, kUiTextScale, 165, 175, 185);
			if (!dimensions.empty() && footerLayout.detailsWidth > 0) {
				const std::string visibleDimensions = ClipText(dimensions, footerLayout.detailsWidth);
				const int detailsX = footerX + footerLayout.detailsOffsetX +
					footerLayout.detailsWidth - TextWidth(visibleDimensions, kUiTextScale);
				DrawText(visibleDimensions, detailsX, footerY, kUiTextScale, 165, 175, 185);
			}
		}
	}

	void RequestFileDialogDirectorySummaries() {
		++fileDialogSummaryGeneration_;
		fileDialogDirectorySummaries_.clear();
		std::vector<fs::path> directories;
		if (!fileDialogSave_ && !fileDialogParameterRestore_) {
			for (const FileDialogEntry& entry : fileDialogModel_.AllEntries()) {
				if (entry.directory && !entry.parent && !entry.encrypted) {
					directories.push_back(entry.path);
				}
			}
		}
		fileDialogSummaryLoader_.Request(directories, fileDialogSummaryGeneration_);
	}

	void TickFileDialogDirectorySummaries() {
		for (jpegview_linux::DirectorySummaryResult& result : fileDialogSummaryLoader_.TakeReady()) {
			if (!fileDialogOpen_ || fileDialogSave_ || fileDialogParameterRestore_ ||
				result.generation != fileDialogSummaryGeneration_) continue;
			fileDialogDirectorySummaries_[result.directory.string()] = result.summary;
		}
	}

	void RequestFileDialogFileSizes() {
		++fileDialogFileSizeGeneration_;
		std::vector<jpegview_linux::SourceDescriptor> sources;
		std::unordered_set<std::string> seen;
		const auto appendUnknownFiles = [&sources, &seen](
			const std::vector<FileDialogEntry>& entries) {
			for (const FileDialogEntry& entry : entries) {
				if (entry.directory || entry.fileSizeKnown) continue;
				const std::string identity = entry.path.lexically_normal().string();
				if (seen.insert(identity).second) {
					sources.push_back(entry.sourceDescriptor.LogicalPath().empty() ?
						jpegview_linux::SourceDescriptor(entry.path, {}, {}) :
						entry.sourceDescriptor);
				}
			}
		};
		if (!fileDialogSave_ && !fileDialogParameterBackup_ && !fileDialogParameterRestore_) {
			appendUnknownFiles(fileDialogModel_.AllEntries());
			appendUnknownFiles(recentFileDialogModel_.AllEntries());
		}
		fileDialogFileSizeLoader_.RequestSources(sources, fileDialogFileSizeGeneration_);
	}

	void TickFileDialogFileSizes() {
		for (const jpegview_linux::FileDialogFileSizeResult& result :
			fileDialogFileSizeLoader_.TakeReady()) {
			if (!fileDialogOpen_ || result.generation != fileDialogFileSizeGeneration_) continue;
			if (!result.observedSource.LogicalPath().empty()) {
				const jpegview_linux::SourceKey previous = result.requestedSource.Key();
				fileDialogModel_.RefreshSourceDescriptor(previous, result.observedSource);
				recentFileDialogModel_.RefreshSourceDescriptor(previous,
					result.observedSource);
			}
		}
	}

	void TickFileDialogArchiveDirectory() {
		for (jpegview_linux::ArchiveDirectoryResult& result :
			fileDialogArchiveLoader_.TakeReady()) {
			if (!fileDialogOpen_ || fileDialogSave_ || fileDialogParameterRestore_ ||
				result.generation != fileDialogArchiveGeneration_ ||
				(result.passwordValidation ?
					result.directory != archivePasswordDialogTarget_ :
					result.directory != fileDialogDirectory_)) continue;
			if (result.passwordValidation) {
				archivePasswordValidationPending_ = false;
				if (result.error.empty()) {
					if (!jpegview_linux::SetSessionArchivePassword(result.directory,
						archivePasswordPendingValue_)) {
						archivePasswordDialog_.Begin(result.directory.string(),
							"Could not cache the password for this archive");
					} else {
						archivePasswordDialog_.Cancel();
						fileDialogMessage_.clear();
						RefreshFileDialog();
					}
				} else {
					archivePasswordDialog_.Begin(result.directory.string(),
						result.errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword ?
						"Incorrect password; please try again" : result.error);
				}
				std::fill(archivePasswordPendingValue_.begin(),
					archivePasswordPendingValue_.end(), '\0');
				archivePasswordPendingValue_.clear();
				continue;
			}
			std::vector<FileDialogEntry> entries;
			const fs::path parent = fileDialogDirectory_.parent_path();
			if (!parent.empty() && parent != fileDialogDirectory_) {
				entries.push_back(FileDialogEntry{parent, true, true});
			}
			if (!result.error.empty()) {
				fileDialogMessage_ = "Cannot read archive: " + result.error;
				if (result.errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired) {
					const fs::path backing = jpegview_linux::ArchiveBackingFile(result.directory);
					encryptedArchivePaths_.insert(AbsoluteNormalized(backing).string());
					entries.push_back(FileDialogEntry{result.directory, true, false, {}, true,
						false, true});
					fileDialogMessage_ = "Archive contents are encrypted";
					if (!jpegview_linux::HasSessionArchivePassword(result.directory)) {
						BeginArchivePasswordDialog(result.directory);
					}
				} else if (result.errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword) {
					jpegview_linux::ForgetSessionArchivePassword(result.directory);
					const fs::path backing = jpegview_linux::ArchiveBackingFile(result.directory);
					encryptedArchivePaths_.insert(AbsoluteNormalized(backing).string());
					entries.push_back(FileDialogEntry{result.directory, true, false, {}, true,
						false, true});
					fileDialogMessage_ = "Saved archive password is incorrect";
					BeginArchivePasswordDialog(result.directory, "Saved password is incorrect; try again");
				}
			} else {
				fileDialogMessage_.clear();
				if (result.containsEncryptedEntries) {
					const fs::path backing = jpegview_linux::ArchiveBackingFile(result.directory);
					encryptedArchivePaths_.insert(AbsoluteNormalized(backing).string());
				}
				for (const jpegview_linux::ArchiveEntryInfo& archiveEntry : result.entries) {
					if (!archiveEntry.directory && (fileDialogSave_ ||
						!jpegview_linux::IsSupportedImagePath(archiveEntry.path))) continue;
					const fs::file_time_type modificationTime =
						jpegview_linux::ArchiveFileModificationTime(archiveEntry.modificationTime);
					FileDialogEntry dialogEntry{archiveEntry.path, archiveEntry.directory,
						false, modificationTime, false, true, archiveEntry.encrypted,
						archiveEntry.directory ? 0 : archiveEntry.size, !archiveEntry.directory};
					if (!archiveEntry.directory) {
						dialogEntry.sourceDescriptor = jpegview_linux::DescribeArchiveMember(
							archiveEntry.path, archiveEntry.backingIdentity, archiveEntry.size,
							archiveEntry.modificationTime, archiveEntry.encrypted);
					}
					entries.push_back(std::move(dialogEntry));
				}
			}
			fileDialogModel_.SetEntries(std::move(entries));
			if (!fileList_.Empty() &&
				fileList_.Current().parent_path() == fileDialogDirectory_) {
				fileDialogModel_.Focus(AbsoluteNormalized(fileList_.Current()),
					FileDialogVisibleRows());
			}
			RequestFileDialogDirectorySummaries();
			RequestFileDialogFileSizes();
			if (result.error.empty() && result.containsEncryptedEntries &&
				!jpegview_linux::HasSessionArchivePassword(result.directory)) {
				BeginArchivePasswordDialog(result.directory);
			}
		}
	}

	void BeginArchivePasswordDialog(const fs::path& directory, std::string error = {}) {
		archivePasswordDialogTarget_ = directory;
		archivePasswordDialog_.Begin(directory.string(), std::move(error));
		archivePasswordValidationPending_ = false;
		archivePasswordPendingValue_.clear();
		fileDialogMessage_.clear();
		SDL_StartTextInput();
	}

	void PasteArchivePasswordFromClipboard() {
		std::unique_ptr<char, SdlClipboardTextWiper> clipboardText(SDL_GetClipboardText());
		if (clipboardText) archivePasswordDialog_.AppendText(clipboardText.get());
	}

	void HandleArchivePasswordEvents(const SDL_Event& event, bool& running) {
		if (event.type == SDL_QUIT) {
			running = false;
			return;
		}
		if (archivePasswordValidationPending_) {
			if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
				fileDialogArchiveLoader_.Clear(++fileDialogArchiveGeneration_);
				archivePasswordValidationPending_ = false;
				std::fill(archivePasswordPendingValue_.begin(),
					archivePasswordPendingValue_.end(), '\0');
				archivePasswordPendingValue_.clear();
				archivePasswordDialog_.Cancel();
			}
			return;
		}
		if (event.type == SDL_KEYDOWN) {
			const Uint16 modifiers = event.key.keysym.mod;
			const bool control = (modifiers & 0x00c0u) != 0;
			const bool shift = (modifiers & 0x0003u) != 0;
			const bool paste = (control && event.key.keysym.sym == 'v') ||
				(shift && event.key.keysym.sym == SDLK_INSERT);
			if (paste) {
				if (event.key.repeat == 0) PasteArchivePasswordFromClipboard();
			} else if (event.key.keysym.sym == SDLK_ESCAPE) {
				archivePasswordDialog_.Cancel();
			} else if (event.key.keysym.sym == SDLK_BACKSPACE) {
				archivePasswordDialog_.Backspace();
			} else if (event.key.keysym.sym == SDLK_RETURN) {
				std::optional<std::string> password = archivePasswordDialog_.Submit();
				if (!password) return;
				archivePasswordPendingValue_ = *password;
				std::fill(password->begin(), password->end(), '\0');
				fileDialogArchiveLoader_.RequestPasswordValidation(
					archivePasswordDialogTarget_, archivePasswordPendingValue_,
					++fileDialogArchiveGeneration_);
				archivePasswordValidationPending_ = true;
			}
		} else if (event.type == SDL_TEXTINPUT) {
			archivePasswordDialog_.AppendText(event.text.text);
		}
	}

	void RefreshFileDialog() {
		InvalidateFileDialogPreview();
		fileDialogArchiveLoader_.Clear(++fileDialogArchiveGeneration_);
		std::vector<FileDialogEntry> entries;
		const fs::path parent = fileDialogDirectory_.parent_path();
		if (!parent.empty() && parent != fileDialogDirectory_) {
			entries.push_back(FileDialogEntry{parent, true, true});
		}
		if (jpegview_linux::IsArchiveLocation(fileDialogDirectory_)) {
			fileDialogMessage_ = "Reading archive contents…";
			fileDialogModel_.SetEntries(std::move(entries));
			RequestFileDialogDirectorySummaries();
			RequestFileDialogFileSizes();
			fileDialogArchiveLoader_.Request(fileDialogDirectory_, fileDialogArchiveGeneration_);
			return;
		}
		fileDialogMessage_.clear();
		std::error_code error;

		for (const fs::directory_entry& entry : fs::directory_iterator(fileDialogDirectory_, error)) {
			if (error) break;
			std::error_code statusError;
			const bool directory = entry.is_directory(statusError);
			const bool archive = !statusError && !directory &&
				entry.is_regular_file(statusError) && !statusError &&
				jpegview_linux::IsArchiveContainerName(entry.path());
			if (statusError || (!directory && !archive && (!entry.is_regular_file(statusError) ||
				(!(fileDialogParameterBackup_ || fileDialogParameterRestore_) &&
					!jpegview_linux::IsSupportedImagePath(entry.path()))))) {
				continue;
			}
			if (archive && (fileDialogSave_ || fileDialogParameterBackup_ ||
				fileDialogParameterRestore_)) continue;
			std::error_code modificationError;
			const fs::file_time_type modificationTime = entry.last_write_time(modificationError);
			const fs::path normalizedPath = AbsoluteNormalized(entry.path());
			const bool encrypted = archive && encryptedArchivePaths_.count(normalizedPath.string()) != 0;
			FileDialogEntry dialogEntry{normalizedPath, directory || archive,
				false, modificationError ? fs::file_time_type{} : modificationTime,
				archive, false, encrypted};
			if (!directory && !archive) {
				dialogEntry.sourceDescriptor = jpegview_linux::SourceDescriptor(
					normalizedPath, {}, {});
			}
			entries.push_back(std::move(dialogEntry));
		}

		fileDialogModel_.SetEntries(std::move(entries));
		RequestFileDialogDirectorySummaries();
		RequestFileDialogFileSizes();
	}

	void OpenFileDialog(const fs::path& preferredDirectory = fs::path()) {
		if (pendingFileListScanHandling_ == FileListScanHandling::DroppedInputs) {
			fileListScanWorker_.Clear();
			ClearPendingFileListScan();
			SetTitle();
		}
		std::error_code error;
		fs::path directory = preferredDirectory;
		if (directory.empty()) {
			directory = fs::current_path(error);
			if (!fileList_.Empty()) {
				const fs::path currentDirectory = fileList_.Current().parent_path();
				if (!currentDirectory.empty()) directory = currentDirectory;
			}
		}
		if (error || directory.empty()) directory = fs::path(".");
		fileDialogDirectory_ = AbsoluteNormalized(directory);
		fileDialogSave_ = false;
		fileDialogParameterBackup_ = false;
		fileDialogParameterRestore_ = false;
		fileDialogTab_ = FileDialogTab::Browse;
		fileDialogSaveFullSize_ = true;
		PositionFileDialogGeometry();
		fileDialogFilename_.clear();
		fileDialogModel_.Begin(false);
		recentFileDialogModel_.Begin(false);
		recentFileRemovalUndo_.clear();
		RebuildRecentFileDialogEntries();
		recentFileDialogModel_.SelectFirst(FileDialogVisibleRows());
		fileDialogMessage_.clear();
		fileDialogOpen_ = true;
		SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
		UpdateFileDialogCursor(lastMouseX_, lastMouseY_);
		contextMenuOpen_ = false;
		RefreshFileDialog();
		if (!fileList_.Empty()) {
			fileDialogModel_.Focus(AbsoluteNormalized(fileList_.Current()), FileDialogVisibleRows());
		}
		SDL_StartTextInput();
	}

	void SwitchFileDialogTab(FileDialogTab tab) {
		if (!FileDialogHasTabs() || fileDialogTab_ == tab) return;
		fileDialogTab_ = tab;
		InvalidateFileDialogPreview();
	}

	void RebuildRecentFileDialogEntries() {
		std::vector<FileDialogEntry> entries;
		entries.reserve(recentFiles_.Files().size());
		for (const fs::path& path : recentFiles_.Files()) {
			FileDialogEntry entry{path, false, false, {}, false,
				jpegview_linux::IsArchiveMemberLocation(path)};
			entry.sourceDescriptor = jpegview_linux::SourceDescriptor(path, {}, {});
			entries.push_back(std::move(entry));
		}
		recentFileDialogModel_.SetEntriesInOrder(std::move(entries), true);
	}

	bool RemoveSelectedRecent() {
		if (!FileDialogCanRemoveRecent()) return false;
		const FileDialogEntry* selected = recentFileDialogModel_.SelectedEntry();
		if (selected == nullptr) return false;
		const int previousIndex = recentFileDialogModel_.SelectedIndex();
		const auto removal = recentFiles_.Remove(selected->path);
		if (!removal.has_value()) return false;
		recentFileRemovalUndo_.push_back(*removal);
		RebuildRecentFileDialogEntries();
		RequestFileDialogFileSizes();
		if (!recentFileDialogModel_.Entries().empty()) {
			recentFileDialogModel_.Select(std::min(previousIndex,
				static_cast<int>(recentFileDialogModel_.Entries().size()) - 1),
				FileDialogVisibleRows());
		}
		InvalidateFileDialogPreview();
		return true;
	}

	bool UndoRecentRemoval() {
		if (recentFileRemovalUndo_.empty()) return false;
		const jpegview_linux::RecentFileRemoval removal = recentFileRemovalUndo_.back();
		if (!recentFiles_.Restore(removal)) return false;
		recentFileRemovalUndo_.pop_back();
		RebuildRecentFileDialogEntries();
		recentFileDialogModel_.Focus(removal.path, FileDialogVisibleRows());
		InvalidateFileDialogPreview();
		return true;
	}

	void OpenSaveFileDialog(bool fullSize) {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
		const fs::path currentSource = fileList_.Current();
		fs::path directory = jpegview_linux::IsArchiveMemberLocation(currentSource) ?
			jpegview_linux::ArchiveBackingFile(currentSource).parent_path() : currentSource.parent_path();
		if (directory.empty()) directory = fs::current_path();
		fileDialogDirectory_ = AbsoluteNormalized(directory);
		fileDialogSave_ = true;
		fileDialogTab_ = FileDialogTab::Browse;
		fileDialogParameterBackup_ = false;
		fileDialogParameterRestore_ = false;
		fileDialogLosslessCrop_ = false;
		fileDialogLosslessCropRect_ = {};
		fileDialogSaveFullSize_ = fullSize;
		PositionFileDialogGeometry();
		fileDialogFilename_ = fileList_.Current().stem().string() + "_proc.jpg";
		fileDialogModel_.Begin(true);
		fileDialogMessage_.clear();
		fileDialogOverwriteConfirmed_ = false;
		fileDialogOpen_ = true;
		SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
		UpdateFileDialogCursor(lastMouseX_, lastMouseY_);
		contextMenuOpen_ = false;
		RefreshFileDialog();
		fileDialogModel_.ClearSelection();
		SDL_StartTextInput();
	}

	void OpenParameterDbBackupDialog() {
		const fs::path database = jpegview_linux::ImageProcessingStorePath();
		if (database.empty()) {
			SetTitle("Cannot determine the picture-level database path");
			return;
		}
		fileDialogDirectory_ = database.parent_path();
		fileDialogSave_ = true;
		fileDialogTab_ = FileDialogTab::Browse;
		fileDialogParameterBackup_ = true;
		fileDialogParameterRestore_ = false;
		fileDialogSaveFullSize_ = true;
		PositionFileDialogGeometry();
		fileDialogFilename_ = database.stem().string() + "-backup" + database.extension().string();
		fileDialogModel_.Begin(true);
		fileDialogMessage_.clear();
		fileDialogOverwriteConfirmed_ = false;
		fileDialogOpen_ = true;
		SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
		UpdateFileDialogCursor(lastMouseX_, lastMouseY_);
		contextMenuOpen_ = false;
		RefreshFileDialog();
		fileDialogModel_.ClearSelection();
		SDL_StartTextInput();
	}

	void OpenParameterDbRestoreDialog() {
		const fs::path database = jpegview_linux::ImageProcessingStorePath();
		if (database.empty()) {
			SetTitle("Cannot determine the picture-level database path");
			return;
		}
		fileDialogDirectory_ = database.parent_path();
		fileDialogSave_ = false;
		fileDialogTab_ = FileDialogTab::Browse;
		fileDialogParameterBackup_ = false;
		fileDialogParameterRestore_ = true;
		fileDialogSaveFullSize_ = true;
		PositionFileDialogGeometry();
		fileDialogFilename_.clear();
		fileDialogModel_.Begin(false);
		fileDialogMessage_.clear();
		fileDialogOverwriteConfirmed_ = false;
		fileDialogOpen_ = true;
		SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
		UpdateFileDialogCursor(lastMouseX_, lastMouseY_);
		contextMenuOpen_ = false;
		RefreshFileDialog();
		fileDialogModel_.ClearSelection();
		SDL_StartTextInput();
	}

	std::string FileDialogEntryLabel(const FileDialogEntry& entry) const {
		if (entry.parent) return "[..]";
		if (entry.archiveContainer) return "[" +
			jpegview_linux::ArchiveFormatName(entry.path) + "] " +
			(entry.encrypted ? "[Encrypted] " : "") + entry.path.filename().string();
		if (entry.encrypted) return "[Encrypted] " + entry.path.filename().string();
		return entry.directory ? std::string("[Dir] ") + entry.path.filename().string() : entry.path.filename().string();
	}

	int FileDialogItemAt(int x, int y) const {
		const SDL_Rect contentRect = FileDialogListContentRect();
		if (!PointInRect(x, y, contentRect)) return -1;
		const int row = (y - contentRect.y) / 26;
		const jpegview_linux::FileDialogModel& model = ActiveFileDialogModel();
		const int item = model.Scroll() + row;
		return item >= 0 && item < static_cast<int>(model.Entries().size()) ? item : -1;
	}

	void NavigateFileDialogDirectory(const fs::path& directory, bool returningToParent) {
		const fs::path previousDirectory = fileDialogDirectory_;
		fileDialogDirectory_ = directory;
		if (!fileDialogSave_) fileDialogModel_.ClearFilter();
		RefreshFileDialog();
		if (fileDialogSave_) {
			fileDialogModel_.ClearSelection();
		} else if (returningToParent) {
			fileDialogModel_.Focus(previousDirectory, FileDialogVisibleRows());
		}
		fileDialogOverwriteConfirmed_ = false;
	}

	void ActivateFileDialogSelection(bool openDirectoryImmediately = false) {
		jpegview_linux::FileDialogModel& model = ActiveFileDialogModel();
		if (fileDialogSave_ && fileDialogOverwriteConfirmed_ && !fileDialogFilename_.empty()) {
			SaveImageFromDialog();
			return;
		}
		if (fileDialogSave_ && model.SelectedIndex() < 0) {
			SaveImageFromDialog();
			return;
		}
		const FileDialogEntry* selected = model.SelectedEntry();
		if (selected == nullptr) return;
		const FileDialogEntry entry = *selected;
		if (fileDialogTab_ == FileDialogTab::Recents) {
			OpenDroppedFiles({entry.path.string()}, DisplayModeLoadPolicy::RestoreRecent);
			CloseFileDialog(false);
			return;
		}
		if (!fileDialogSave_ && !fileDialogParameterRestore_ && entry.encrypted &&
			!jpegview_linux::HasSessionArchivePassword(entry.path)) {
			BeginArchivePasswordDialog(entry.path);
			return;
		}
		if (entry.directory) {
			if (openDirectoryImmediately && !fileDialogSave_ && !fileDialogParameterRestore_) {
				OpenDroppedFiles({entry.path.string()});
			} else {
				NavigateFileDialogDirectory(entry.path, entry.parent);
			}
			return;
		}
		if (fileDialogSave_) {
			fileDialogFilename_ = entry.path.filename().string();
			fileDialogOverwriteConfirmed_ = false;
			SaveImageFromDialog();
		} else if (fileDialogParameterRestore_) {
			BeginParameterDbRestore(entry.path);
		} else {
			OpenDroppedFiles({entry.path.string()});
			CloseFileDialog(false);
		}
	}

	void HandleFileDialogEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN: {
			jpegview_linux::FileDialogModel& model = ActiveFileDialogModel();
			const bool repeatableSelectionKey = event.key.keysym.sym == SDLK_UP ||
				event.key.keysym.sym == SDLK_DOWN || event.key.keysym.sym == SDLK_PAGEUP ||
				event.key.keysym.sym == SDLK_PAGEDOWN || event.key.keysym.sym == SDLK_HOME ||
				event.key.keysym.sym == SDLK_END;
			if (event.key.repeat != 0 && !repeatableSelectionKey) break;
			const bool control = (event.key.keysym.mod & 0x00c0u) != 0;
			if (control && event.key.keysym.sym == SDLK_z) {
				UndoRecentRemoval();
			} else if (event.key.keysym.sym == SDLK_DELETE && FileDialogCanRemoveRecent()) {
				RemoveSelectedRecent();
			} else if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseFileDialog();
			} else if (event.key.keysym.sym == SDLK_TAB &&
				(event.key.keysym.mod & 0x00C0u) != 0 && FileDialogHasTabs()) {
				SwitchFileDialogTab(fileDialogTab_ == FileDialogTab::Browse ?
					FileDialogTab::Recents : FileDialogTab::Browse);
			} else if (event.key.keysym.sym == SDLK_UP) {
				model.MoveSelection(-1, FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_DOWN) {
				model.MoveSelection(1, FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_PAGEUP) {
				model.MoveSelectionByPage(-1, FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_PAGEDOWN) {
				model.MoveSelectionByPage(1, FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_HOME) {
				model.SelectFirst(FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_END) {
				model.SelectLast(FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_RETURN) {
				const bool ctrl = (event.key.keysym.mod & 0x00C0u) != 0;
				ActivateFileDialogSelection(ctrl);
			} else if (event.key.keysym.sym == SDLK_BACKSPACE) {
				if (fileDialogSave_ && model.SelectedIndex() < 0 &&
					jpegview_linux::EraseLastUtf8CodePoint(fileDialogFilename_)) {
					fileDialogMessage_.clear();
					fileDialogOverwriteConfirmed_ = false;
					break;
				}
				if (!fileDialogSave_ && model.BackspaceFilter()) {
					break;
				}
				if (fileDialogTab_ == FileDialogTab::Recents) break;
				const fs::path parent = fileDialogDirectory_.parent_path();
				if (!parent.empty() && parent != fileDialogDirectory_) {
					NavigateFileDialogDirectory(parent, true);
				}
			}
			break;
		}
		case SDL_TEXTINPUT:
			if (fileDialogSave_) {
				fileDialogFilename_ += event.text.text;
				ActiveFileDialogModel().ClearSelection();
				fileDialogMessage_.clear();
				fileDialogOverwriteConfirmed_ = false;
			} else {
				ActiveFileDialogModel().AppendFilter(event.text.text);
			}
			break;
		case SDL_MOUSEWHEEL: {
			SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
			const SDL_Rect listRect = FileDialogListRect();
			if (!PointInRect(lastMouseX_, lastMouseY_, listRect)) break;
			int wheelTicks = std::clamp(event.wheel.y, -100, 100);
			if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) wheelTicks = -wheelTicks;
			if (wheelTicks == 0) break;
			ActiveFileDialogModel().ScrollBy(-wheelTicks * 3, FileDialogVisibleRows());
			const int item = FileDialogItemAt(lastMouseX_, lastMouseY_);
			if (item >= 0) ActiveFileDialogModel().Select(item, FileDialogVisibleRows());
			break;
		}
		case SDL_MOUSEMOTION: {
			lastMouseX_ = event.motion.x;
			lastMouseY_ = event.motion.y;
			if (fileDialogDragMode_ != FileDialogDragMode::None) {
				ResizeFileDialog(lastMouseX_, lastMouseY_);
				UpdateFileDialogCursor(lastMouseX_, lastMouseY_);
				break;
			}
			const int item = FileDialogItemAt(event.motion.x, event.motion.y);
			if (item >= 0) {
				ActiveFileDialogModel().Select(item, FileDialogVisibleRows());
			}
			UpdateFileDialogCursor(lastMouseX_, lastMouseY_);
			break;
		}
		case SDL_MOUSEBUTTONDOWN: {
			lastMouseX_ = event.button.x;
			lastMouseY_ = event.button.y;
			if (event.button.button == SDL_BUTTON_LEFT && BeginFileDialogResize(lastMouseX_, lastMouseY_)) break;
			if (event.button.button == SDL_BUTTON_LEFT &&
				BeginFileDialogPreviewResize(lastMouseX_, lastMouseY_)) break;
			if (event.button.button == SDL_BUTTON_LEFT &&
				BeginFileDialogScrollbarInteraction(lastMouseX_, lastMouseY_)) break;
			const bool dialogClicked = PointInRect(event.button.x, event.button.y, FileDialogRect());
			const int item = FileDialogItemAt(event.button.x, event.button.y);
			const bool browseTabClicked = FileDialogHasTabs() && PointInRect(event.button.x,
				event.button.y, FileDialogTabRect(FileDialogTab::Browse));
			const bool recentsTabClicked = FileDialogHasTabs() && PointInRect(event.button.x,
				event.button.y, FileDialogTabRect(FileDialogTab::Recents));
			const bool removeRecentClicked = FileDialogCanRemoveRecent() && PointInRect(
				event.button.x, event.button.y, FileDialogRemoveRecentButtonRect());
			const bool inputClicked = PointInRect(event.button.x, event.button.y, FileDialogInputRect());
			const bool sortClicked = FileDialogCanSort() &&
				PointInRect(event.button.x, event.button.y, FileDialogSortRect());
			const bool previewClicked = FileDialogHasPreviewColumn() &&
				PointInRect(event.button.x, event.button.y, FileDialogPreviewRect());
			if ((event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT) &&
				!dialogClicked) {
				CloseFileDialog();
			} else if (event.button.button == SDL_BUTTON_LEFT && browseTabClicked) {
				SwitchFileDialogTab(FileDialogTab::Browse);
			} else if (event.button.button == SDL_BUTTON_LEFT && recentsTabClicked) {
				SwitchFileDialogTab(FileDialogTab::Recents);
			} else if (event.button.button == SDL_BUTTON_LEFT && removeRecentClicked) {
				RemoveSelectedRecent();
			} else if (event.button.button == SDL_BUTTON_LEFT && sortClicked) {
				fileDialogModel_.ToggleSortMode(FileDialogVisibleRows());
			} else if (event.button.button == SDL_BUTTON_LEFT && inputClicked) {
				if (fileDialogSave_) ActiveFileDialogModel().ClearSelection();
			} else if (event.button.button == SDL_BUTTON_LEFT && previewClicked) {
				// The preview is informational; clicking it leaves the selection alone.
			} else if (event.button.button == SDL_BUTTON_LEFT && item >= 0) {
				jpegview_linux::FileDialogModel& model = ActiveFileDialogModel();
				model.Select(item, FileDialogVisibleRows());
				const FileDialogEntry* selected = model.SelectedEntry();
				if (fileDialogSave_ && selected != nullptr && !selected->directory) {
					fileDialogFilename_ = selected->path.filename().string();
				}
				if (event.button.clicks >= 2) ActivateFileDialogSelection();
			}
			break;
		}
		case SDL_MOUSEBUTTONUP:
			lastMouseX_ = event.button.x;
			lastMouseY_ = event.button.y;
			if (event.button.button == SDL_BUTTON_LEFT) EndFileDialogResize(lastMouseX_, lastMouseY_);
			break;
		default:
			break;
		}
	}

	void RenderFileDialog() {
		if (!fileDialogOpen_) return;
		UpdateFileDialogPreview();
		const SDL_Rect dialog = FileDialogRect();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 220);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 190, 190, 190);
		DrawText(fileDialogLosslessCrop_ ? "Save lossless JPEG crop" :
			(fileDialogParameterBackup_ ? "Back up picture-level database" :
			(fileDialogParameterRestore_ ? "Restore picture-level database" :
			(fileDialogSave_ ? "Save processed image" : "Open image"))),
			dialog.x + 18, dialog.y + 14, kUiTextScale);
		if (FileDialogHasTabs()) {
			for (const FileDialogTab tab : {FileDialogTab::Browse, FileDialogTab::Recents}) {
				const SDL_Rect tabRect = FileDialogTabRect(tab);
				const bool active = fileDialogTab_ == tab;
				SDL_SetRenderDrawColor(renderer_, active ? 45 : 30, active ? 72 : 30,
					active ? 104 : 30, 230);
				SDL_RenderFillRect(renderer_, &tabRect);
				DrawRect(tabRect, active ? 100 : 75, active ? 130 : 75, active ? 165 : 75);
				const std::string label = tab == FileDialogTab::Browse ? "Browse" : "Recents";
				const int labelX = tabRect.x + std::max(4,
					(tabRect.w - TextWidth(label, kUiTextScale)) / 2);
				DrawText(label, labelX, tabRect.y + 7, kUiTextScale,
					active ? 235 : 175, active ? 240 : 185, active ? 250 : 195);
			}
		}
		DrawText(fileDialogTab_ == FileDialogTab::Recents ?
			"One recent image per folder" :
			jpegview_linux::ArchiveLocationDisplayName(fileDialogDirectory_),
			dialog.x + 18, dialog.y + 42, kUiTextScale, 170, 170, 170);
		if (FileDialogCanRemoveRecent()) {
			const SDL_Rect button = FileDialogRemoveRecentButtonRect();
			const bool enabled = recentFileDialogModel_.SelectedEntry() != nullptr;
			const bool hovered = enabled && PointInRect(lastMouseX_, lastMouseY_, button);
			SDL_SetRenderDrawColor(renderer_, enabled ? (hovered ? 52 : 32) : 24,
				enabled ? (hovered ? 78 : 38) : 24, enabled ? (hovered ? 108 : 52) : 24, 230);
			SDL_RenderFillRect(renderer_, &button);
			DrawRect(button, enabled ? 115 : 65, enabled ? 135 : 65, enabled ? 155 : 65);
			const int labelX = button.x + (button.w - TextWidth("Remove", kUiTextScale)) / 2;
			DrawText("Remove", labelX, button.y + 7, kUiTextScale,
				enabled ? 225 : 115, enabled ? 230 : 115, enabled ? 238 : 115);
		}
		DrawText(fileDialogSave_ ? "File name" :
			(fileDialogParameterRestore_ ? "Backup filter" : "Filter"),
			dialog.x + 18, dialog.y + 68,
			kUiTextScale, 190, 190, 190);
		if (FileDialogCanSort()) {
			const SDL_Rect sortRect = FileDialogSortRect();
			SDL_SetRenderDrawColor(renderer_, 36, 36, 36, 230);
			SDL_RenderFillRect(renderer_, &sortRect);
			DrawRect(sortRect, 100, 130, 165);
			const std::string sortLabel = ActiveFileDialogModel().SortMode() == jpegview_linux::FileDialogSortMode::Name ?
				"Sort: Name" : "Sort: Mod.date";
			const int labelX = sortRect.x + std::max(6, (sortRect.w - TextWidth(sortLabel, kUiTextScale)) / 2);
			DrawText(sortLabel, labelX, sortRect.y + 3, kUiTextScale, 210, 220, 230);
		}
		SDL_Rect inputRect = FileDialogInputRect();
		SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 220);
		SDL_RenderFillRect(renderer_, &inputRect);
		DrawRect(inputRect, 100, 130, 165);
		const std::string& inputText = fileDialogSave_ ? fileDialogFilename_ : ActiveFileDialogModel().Filter();
		DrawText(ClipInputText(inputText, inputRect.w - 20), inputRect.x + 10, inputRect.y + 6, kUiTextScale);

		const int listTop = FileDialogListTop();
		const int rows = FileDialogVisibleRows();
		const SDL_Rect listRect = FileDialogListRect();
		const SDL_Rect listContentRect = FileDialogListContentRect();
		const jpegview_linux::FileDialogModel& model = ActiveFileDialogModel();
		SDL_SetRenderDrawColor(renderer_, 25, 25, 25, 210);
		SDL_RenderFillRect(renderer_, &listRect);
		DrawRect(listRect, 75, 75, 75);
		for (int row = 0; row < rows; ++row) {
			const int item = model.Scroll() + row;
			if (item >= static_cast<int>(model.Entries().size())) break;
			const FileDialogEntry& entry = model.Entries()[item];
			const int rowTop = listTop + row * 26;
			if (item == model.SelectedIndex()) {
				SDL_SetRenderDrawColor(renderer_, 45, 82, 120, 205);
				SDL_Rect selection{listContentRect.x + 2, rowTop + 1,
					listContentRect.w - 4, 24};
				SDL_RenderFillRect(renderer_, &selection);
			}
			if (fileDialogTab_ == FileDialogTab::Recents) {
				const int leftX = listContentRect.x + 10;
				const int pathWidth = std::max(1, (listContentRect.w - 30) / 2);
				const std::string sizeText = entry.fileSizeKnown ?
					jpegview_linux::FormatFileSize(entry.fileSize) : std::string();
				const int sizeWidth = TextWidth(sizeText, kUiTextScale);
				const int rightEdge = listContentRect.x + listContentRect.w - 10;
				const int sizeX = rightEdge - sizeWidth;
				const int filenameRight = sizeText.empty() ? rightEdge : sizeX - 8;
				const int filenameWidth = std::max(1,
					filenameRight - (leftX + pathWidth + 8));
				const std::string parent = ClipText(
					jpegview_linux::ArchiveLocationDisplayName(entry.path.parent_path()), pathWidth);
				const std::string filename = ClipText(entry.path.filename().string(), filenameWidth);
				const int filenameX = filenameRight - TextWidth(filename, kUiTextScale);
				const bool fromArchive = entry.archiveMember;
				DrawText(parent, leftX, rowTop + 5, kUiTextScale,
					fromArchive ? 210 : 165, fromArchive ? 170 : 175, fromArchive ? 105 : 190);
				DrawText(filename, filenameX, rowTop + 5, kUiTextScale,
					fromArchive ? 255 : 235, fromArchive ? 205 : 235, fromArchive ? 125 : 235);
				if (!sizeText.empty()) {
					DrawText(sizeText, sizeX, rowTop + 5, kUiTextScale, 165, 180, 200);
				}
			} else {
				std::string rightText;
				if (FileDialogCanSort() && entry.directory && !entry.parent && !entry.encrypted) {
					const auto summary = fileDialogDirectorySummaries_.find(entry.path.string());
					rightText = summary == fileDialogDirectorySummaries_.end() ? "Scanning..." :
						jpegview_linux::FormatDirectorySummary(summary->second);
				} else if (!entry.directory && entry.fileSizeKnown) {
					rightText = jpegview_linux::FormatFileSize(entry.fileSize);
				}
				rightText = ClipText(rightText, std::max(1, listContentRect.w / 2 - 20));
				const int rightTextWidth = TextWidth(rightText, kUiTextScale);
				const int rightTextX = listContentRect.x + listContentRect.w - 10 - rightTextWidth;
				const int labelWidth = rightText.empty() ? listContentRect.w - 20 :
					std::max(1, rightTextX - (listContentRect.x + 10) - 12);
				const bool archiveEntry = entry.archiveContainer || entry.archiveMember;
				const Uint8 red = archiveEntry ? 255 : entry.directory ? 185 : 235;
				const Uint8 green = archiveEntry ? 205 : entry.directory ? 205 : 235;
				const Uint8 blue = archiveEntry ? 125 : 235;
				DrawText(ClipText(FileDialogEntryLabel(entry), labelWidth), listContentRect.x + 10,
					rowTop + 5, kUiTextScale, red, green, blue);
				if (!rightText.empty()) {
					DrawText(rightText, rightTextX, rowTop + 5, kUiTextScale, 155, 175, 195);
				}
			}
		}
		if (model.Entries().empty() && fileDialogTab_ == FileDialogTab::Recents) {
			const std::string emptyMessage = model.Filter().empty() ?
				"No recent files" : "No recent files match this filter";
			DrawText(emptyMessage, listContentRect.x + 12, listRect.y + 10, kUiTextScale,
				165, 175, 190);
		}
		const SDL_Rect scrollbarTrack = FileDialogScrollbarRect();
		const jpegview_linux::FileDialogScrollbarGeometry scrollbar = FileDialogScrollGeometry();
		SDL_SetRenderDrawColor(renderer_, 34, 34, 34, 230);
		SDL_RenderFillRect(renderer_, &scrollbarTrack);
		DrawRect(scrollbarTrack, 63, 63, 63);
		const SDL_Rect scrollbarThumb = FileDialogScrollbarThumbRect();
		const bool scrollbarHovered = PointInRect(lastMouseX_, lastMouseY_, scrollbarTrack);
		const Uint8 thumbShade = !scrollbar.scrollable ? 82 :
			(fileDialogDragMode_ == FileDialogDragMode::Scrollbar ? 185 :
				(scrollbarHovered ? 158 : 128));
		SDL_SetRenderDrawColor(renderer_, thumbShade, thumbShade, thumbShade, 255);
		SDL_RenderFillRect(renderer_, &scrollbarThumb);
		const Uint8 thumbBorder = static_cast<Uint8>(std::min(220,
			static_cast<int>(thumbShade) + 35));
		DrawRect(scrollbarThumb, thumbBorder, thumbBorder, thumbBorder);
		if (FileDialogHasPreviewColumn()) {
			RenderFileDialogPreview(FileDialogPreviewRect());
			const SDL_Rect divider = FileDialogDividerRect();
			const int centerX = divider.x + divider.w / 2;
			const int centerY = divider.y + divider.h / 2;
			DrawLine(centerX, divider.y + 8, centerX, divider.y + divider.h - 9, 74, 84, 96);
			for (int offset = -6; offset <= 6; offset += 6) {
				DrawLine(centerX - 2, centerY + offset, centerX + 2, centerY + offset,
					145, 160, 178);
			}
		}
		if (!fileDialogMessage_.empty()) {
			DrawText(fileDialogMessage_, dialog.x + 18, dialog.y + dialog.h - 60, kUiTextScale, 235, 150, 120);
		}
		DrawText(fileDialogSave_ ? "Enter: Save   Backspace: Edit/parent   Esc: Cancel" :
			(fileDialogParameterRestore_ ?
				"Type: Filter   Enter: Restore backup   Backspace: Parent   Esc: Cancel" :
				(fileDialogTab_ == FileDialogTab::Recents ?
					"Type: Filter   Ctrl+Tab: Tabs   Up/Down: Move   PgUp/Dn: Page   Del: Remove   Ctrl+Z: Undo   Enter: Open   Backspace: Filter   Esc: Cancel" :
					"Type: Filter   Ctrl+Tab: Tabs   Up/Down: Move   PgUp/Dn: Page   Enter: Open   Ctrl+Return: Folder   Backspace: Filter/parent   Esc: Cancel")),
			dialog.x + 18, dialog.y + dialog.h - 34, kUiTextScale, 170, 170, 170);
		const SDL_Rect resizeHandle = FileDialogResizeHandleRect();
		for (int offset = 5; offset <= 13; offset += 4) {
			DrawLine(resizeHandle.x + offset, resizeHandle.y + resizeHandle.h - 2,
				resizeHandle.x + resizeHandle.w - 2, resizeHandle.y + offset,
				135, 145, 155);
		}
	}

	void RenderArchivePasswordDialog() {
		if (!archivePasswordDialog_.IsOpen() && !archivePasswordValidationPending_) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 145);
		const SDL_Rect shade{0, 0, windowWidth, windowHeight};
		SDL_RenderFillRect(renderer_, &shade);
		const int width = std::min(480, std::max(320, windowWidth - 32));
		const int height = 178;
		const SDL_Rect panel{(windowWidth - width) / 2, (windowHeight - height) / 2,
			width, height};
		SDL_SetRenderDrawColor(renderer_, 18, 20, 24, 250);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 190, 170, 120);
		DrawText("ARCHIVE PASSWORD", panel.x + 16, panel.y + 12, kUiTextScale,
			255, 225, 160);
		const fs::path archive(archivePasswordDialogTarget_);
		DrawText(ClipText(archive.filename().string(), width - 32), panel.x + 16,
			panel.y + 38, kUiTextScale, 210, 215, 220);
		const SDL_Rect input{panel.x + 16, panel.y + 64, panel.w - 32, 30};
		SDL_SetRenderDrawColor(renderer_, 30, 32, 36, 255);
		SDL_RenderFillRect(renderer_, &input);
		DrawRect(input, 100, 120, 145);
		const std::string masked = archivePasswordDialog_.DisplayText();
		DrawText(ClipText(masked, input.w - 16), input.x + 8, input.y + 8,
			kUiTextScale, 240, 240, 240);
		const std::string status = archivePasswordValidationPending_ ?
			"Checking password...  (Esc cancels)" :
			(archivePasswordDialog_.Error().empty() ?
				"Enter: Unlock    Esc: Cancel" : archivePasswordDialog_.Error());
		DrawText(ClipText(status, width - 32), panel.x + 16, panel.y + 111,
			kUiTextScale, archivePasswordDialog_.Error().empty() ? 175 : 245,
			archivePasswordDialog_.Error().empty() ? 185 : 150,
			archivePasswordDialog_.Error().empty() ? 200 : 130);
		DrawText("Password is kept only until the app closes", panel.x + 16,
			panel.y + 145, kUiTextScale, 145, 150, 160);
	}

	void ClearTextTextureCache() {
		for (auto& cached : textTextureCache_) {
			if (cached.second.texture != nullptr) SDL_DestroyTexture(cached.second.texture);
		}
		textTextureCache_.clear();
	}

	TextTextureCacheEntry* TextTexture(const std::string& text, int scale) {
		if (renderer_ == nullptr || text.empty() || scale <= 0) return nullptr;
		const std::string key = std::to_string(scale) + '\n' + text;
		auto found = textTextureCache_.find(key);
		if (found != textTextureCache_.end()) {
			found->second.lastUsed = ++textTextureUseCounter_;
			return &found->second;
		}

		const jpegview_linux::RasterizedText raster = UiFont().Rasterize(text, scale);
		if (raster.width <= 0 || raster.height <= 0 || raster.argb.empty()) return nullptr;
		// The viewer keeps best-quality sampling for image textures. Bitmap text
		// uses one-bit ink and transparent white padding, so linear sampling at a
		// scaled renderer edge can leak a faint pixel beyond the final glyph.
		const bool bitmapText = jpegview_linux::Terminus9CanRender(text);
		if (bitmapText) SDL_SetHint(kRenderScaleQualityHint, kBitmapTextScaleQuality);
		SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STATIC, raster.width, raster.height);
		if (bitmapText) SDL_SetHint(kRenderScaleQualityHint, kImageTextureScaleQuality);
		if (texture == nullptr) return nullptr;
		if (SDL_UpdateTexture(texture, nullptr, raster.argb.data(), raster.width * 4) != 0) {
			SDL_DestroyTexture(texture);
			return nullptr;
		}
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
		TextTextureCacheEntry cached{texture, raster.width, raster.height,
			raster.offsetX, raster.offsetY, ++textTextureUseCounter_};
		auto inserted = textTextureCache_.emplace(key, cached).first;
		if (textTextureCache_.size() > 512) {
			auto oldest = textTextureCache_.begin();
			for (auto candidate = textTextureCache_.begin(); candidate != textTextureCache_.end(); ++candidate) {
				if (candidate->second.lastUsed < oldest->second.lastUsed) oldest = candidate;
			}
			if (oldest != inserted) {
				SDL_DestroyTexture(oldest->second.texture);
				textTextureCache_.erase(oldest);
			}
		}
		return &inserted->second;
	}

	void DrawText(const std::string& text, int x, int y, int scale,
		Uint8 r = 235, Uint8 g = 235, Uint8 b = 235, Uint8 alpha = 255) {
		TextTextureCacheEntry* cached = TextTexture(text, scale);
		if (cached == nullptr) return;
		SDL_SetTextureColorMod(cached->texture, r, g, b);
		SDL_SetTextureAlphaMod(cached->texture, alpha);
		const SDL_Rect destination{x + cached->offsetX, y + cached->offsetY,
			cached->width, cached->height};
		SDL_RenderCopy(renderer_, cached->texture, nullptr, &destination);
		SDL_SetTextureAlphaMod(cached->texture, 255);
	}

	void DrawLine(int x1, int y1, int x2, int y2, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235,
		Uint8 alpha = 255) {
		SDL_SetRenderDrawColor(renderer_, r, g, b, alpha);
		SDL_RenderDrawLine(renderer_, x1, y1, x2, y2);
	}

	void DrawRect(const SDL_Rect& rect, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235,
		Uint8 alpha = 255) {
		if (rect.w <= 0 || rect.h <= 0) return;
		SDL_SetRenderDrawColor(renderer_, r, g, b, alpha);
		const SDL_Rect top{rect.x, rect.y, rect.w, 1};
		SDL_RenderFillRect(renderer_, &top);
		if (rect.h > 1) {
			const SDL_Rect bottom{rect.x, rect.y + rect.h - 1, rect.w, 1};
			SDL_RenderFillRect(renderer_, &bottom);
		}
		if (rect.h > 2 && rect.w > 1) {
			const SDL_Rect left{rect.x, rect.y + 1, 1, rect.h - 2};
			const SDL_Rect right{rect.x + rect.w - 1, rect.y + 1, 1, rect.h - 2};
			SDL_RenderFillRect(renderer_, &left);
			SDL_RenderFillRect(renderer_, &right);
		}
	}

	static SDL_Rect SdlRect(const jpegview_linux::UiRect& rect) {
		return SDL_Rect{rect.x, rect.y, rect.width, rect.height};
	}

	void RenderOverlayPaint(const jpegview_linux::OverlayPaintPlan& plan) {
		const SDL_Rect panel = SdlRect(plan.panel);
		SDL_SetRenderDrawColor(renderer_, plan.background.red, plan.background.green,
			plan.background.blue, plan.background.alpha);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, plan.border.red, plan.border.green, plan.border.blue);
		for (const jpegview_linux::UiText& text : plan.text) {
			DrawText(text.text, text.x, text.y, kUiTextScale,
				text.color.red, text.color.green, text.color.blue);
		}
	}

	void RenderNavigationButton(const jpegview_linux::NavigationButtonPaint& button) {
		const SDL_Rect rect = SdlRect(button.rect);
		auto drawLayer = [&](int offsetX, int offsetY, Uint8 red, Uint8 green, Uint8 blue,
			Uint8 alpha) {
			const SDL_Rect frame{rect.x + offsetX, rect.y + offsetY, rect.w, rect.h};
			DrawRect(frame, red, green, blue, alpha);
			for (const jpegview_linux::UiRect& outline : button.outlines) {
				const SDL_Rect shape = SdlRect(outline);
				DrawRect({shape.x + offsetX, shape.y + offsetY, shape.w, shape.h},
					red, green, blue, alpha);
			}
			for (const jpegview_linux::UiLine& line : button.lines) {
				DrawLine(line.x1 + offsetX, line.y1 + offsetY,
					line.x2 + offsetX, line.y2 + offsetY, red, green, blue, alpha);
			}
			for (const jpegview_linux::UiText& text : button.text) {
				DrawText(text.text, text.x + offsetX, text.y + offsetY, kUiTextScale,
					red, green, blue, alpha);
			}
		};
		const Uint8 alpha = button.foreground.alpha;
		drawLayer(-1, 0, 0, 0, 0, alpha);
		drawLayer(1, 0, 0, 0, 0, alpha);
		drawLayer(0, -1, 0, 0, 0, alpha);
		drawLayer(0, 1, 0, 0, 0, alpha);
		drawLayer(0, 0, button.foreground.red, button.foreground.green,
			button.foreground.blue, alpha);
	}

	void RenderFileName() {
		if (!showFileName_ || fileList_.Empty() || contextMenuOpen_ || fileDialogOpen_ ||
			advancedConfiguration_.IsOpen() || batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return;
		int windowWidth = 0;
		SDL_GetWindowSize(window_, &windowWidth, nullptr);
		std::ostringstream text;
		text << '[' << CurrentImagePositionText() << "] "
			<< InfoText(fileList_.Current().filename().string());
		std::string label = text.str();
		const jpegview_linux::OverlayLayout layout = jpegview_linux::FilenameOverlayLayout(
			TextWidth(label, kUiTextScale), windowWidth, kOverlayInset,
			kOverlayTextPadding, FilenameOverlayHeight());
		label = ClipText(label, layout.textWidth);
		RenderOverlayPaint(jpegview_linux::FilenameOverlayPaint(layout, std::move(label),
			TextLineHeight(), kOverlayTextPadding));
	}

	void RenderZoomReadout() {
		if (!viewport_.FitRelativeZoomMode() || fileList_.Empty() || image_.width <= 0 ||
			static_cast<Sint32>(zoomReadoutVisibleUntil_ - SDL_GetTicks()) <= 0 ||
			contextMenuOpen_ || fileDialogOpen_ || advancedConfiguration_.IsOpen() ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return;
		const std::string label = viewport_.ZoomReadout();
		const int textWidth = TextWidth(label, kUiTextScale);
		const int lineHeight = TextLineHeight();
		const SDL_Rect imageArea = ImageAreaRect();
		const SDL_Rect panel{imageArea.x + imageArea.w - textWidth - 22,
			imageArea.y + imageArea.h - lineHeight - 16, textWidth + 16, lineHeight + 8};
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 210);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 120, 120, 120);
		DrawText(label, panel.x + 8, panel.y + (panel.h - lineHeight) / 2,
			kUiTextScale, 245, 245, 245);
	}

	void RenderImageInfo() {
		if (!infoVisible_ || contextMenuOpen_ || fileDialogOpen_ || advancedConfiguration_.IsOpen() ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return;
		const jpegview_linux::InformationOverlayPaintPlan paint = BuildImageInfoPaintPlan();
		if (paint.overlay.panel.width <= 0 || paint.overlay.panel.height <= 0) return;
		RenderOverlayPaint(paint.overlay);
		const SDL_Rect panel = SdlRect(paint.overlay.panel);
		SDL_RenderSetClipRect(renderer_, &panel);
		for (const jpegview_linux::UiLine& line : paint.spectrumLines) {
			DrawLine(line.x1, line.y1, line.x2, line.y2,
				line.color.red, line.color.green, line.color.blue, line.color.alpha);
		}
		SDL_RenderSetClipRect(renderer_, nullptr);

		if (jpegview_linux::Contains(paint.spectrumButton, lastMouseX_, lastMouseY_)) {
			const std::string label = showHistogram_ ? "Hide histogram" : "Show histogram";
			int windowWidth = 0;
			int windowHeight = 0;
			SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
			RenderOverlayPaint(jpegview_linux::NavigationTooltipPaint(paint.spectrumButton,
				label, TextWidth(label, kUiTextScale), TextLineHeight(), windowWidth, windowHeight));
		}
	}

	jpegview_linux::ThumbnailPanelLayout CurrentThumbnailPanelLayout() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		return jpegview_linux::CalculateThumbnailPanelLayout(windowWidth, windowHeight,
			thumbnailPanelVisible_, thumbnailPanelWidth_);
	}

	SDL_Rect ThumbnailPanelRect() const {
		const jpegview_linux::ThumbnailPanelLayout layout = CurrentThumbnailPanelLayout();
		return SDL_Rect{0, 0, layout.panelWidth, layout.imageHeight};
	}

	SDL_Rect ImageAreaRect() const {
		const jpegview_linux::ThumbnailPanelLayout layout = CurrentThumbnailPanelLayout();
		return SDL_Rect{layout.imageX, 0, layout.imageWidth, layout.imageHeight};
	}

	SDL_Rect SpreadPageScreenRect(const jpegview_linux::SpreadPagePlacement& page,
		const jpegview_linux::ViewportRect& destination, const SDL_Rect& area) const {
		if (!activeDoublePageRender_.has_value() ||
			activeDoublePageRender_->layout.canvasWidth <= 0 ||
			activeDoublePageRender_->layout.canvasHeight <= 0) return {};
		const jpegview_linux::DoublePageSpread& spread = activeDoublePageRender_->layout;
		const int left = static_cast<int>(std::lround(
			static_cast<double>(destination.width) * page.x / spread.canvasWidth));
		const int top = static_cast<int>(std::lround(
			static_cast<double>(destination.height) * page.y / spread.canvasHeight));
		const int right = static_cast<int>(std::lround(
			static_cast<double>(destination.width) * (page.x + page.width) / spread.canvasWidth));
		const int bottom = static_cast<int>(std::lround(
			static_cast<double>(destination.height) * (page.y + page.height) / spread.canvasHeight));
		return {destination.x + area.x + left, destination.y + area.y + top,
			std::max(1, right - left), std::max(1, bottom - top)};
	}

	SDL_Rect CurrentPageScreenRect(const SDL_Rect& area) const {
		const auto dimensions = ViewportContentDimensions();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			dimensions.first, dimensions.second, area.w, area.h);
		if (activeDoublePageRender_.has_value()) {
			return SpreadPageScreenRect(activeDoublePageRender_->layout.currentPage,
				destination, area);
		}
		return {destination.x + area.x, destination.y + area.y,
			destination.width, destination.height};
	}

	SDL_Rect NextPageScreenRect(const SDL_Rect& area) const {
		if (!activeDoublePageRender_.has_value()) return {};
		const auto dimensions = ViewportContentDimensions();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			dimensions.first, dimensions.second, area.w, area.h);
		return SpreadPageScreenRect(activeDoublePageRender_->layout.nextPage,
			destination, area);
	}

	void RenderTransparencyBackground(const SDL_Rect& imageRect, const SDL_Rect& clipRect) {
		const int visibleLeft = std::max(imageRect.x, clipRect.x);
		const int visibleTop = std::max(imageRect.y, clipRect.y);
		const int visibleRight = std::min(imageRect.x + imageRect.w, clipRect.x + clipRect.w);
		const int visibleBottom = std::min(imageRect.y + imageRect.h, clipRect.y + clipRect.h);
		if (visibleRight <= visibleLeft || visibleBottom <= visibleTop) return;

		if (transparencyPattern_ != jpegview_linux::TransparencyPattern::Checkerboard) {
			const jpegview_linux::TransparencyPatternColor color =
				jpegview_linux::TransparencyPatternTileColor(transparencyPattern_, 0, 0);
			SDL_SetRenderDrawColor(renderer_, color.red, color.green, color.blue, 255);
			const SDL_Rect visible{visibleLeft, visibleTop,
				visibleRight - visibleLeft, visibleBottom - visibleTop};
			SDL_RenderFillRect(renderer_, &visible);
			return;
		}

		const int cellSize = jpegview_linux::kTransparencyCheckerCellSize;
		const int firstTileX = std::max(0, (visibleLeft - imageRect.x) / cellSize);
		const int firstTileY = std::max(0, (visibleTop - imageRect.y) / cellSize);
		for (int tileY = firstTileY; imageRect.y + tileY * cellSize < visibleBottom; ++tileY) {
			const int tileTop = imageRect.y + tileY * cellSize;
			const int top = std::max(visibleTop, tileTop);
			const int bottom = std::min(visibleBottom, tileTop + cellSize);
			for (int tileX = firstTileX; imageRect.x + tileX * cellSize < visibleRight; ++tileX) {
				const int tileLeft = imageRect.x + tileX * cellSize;
				const int left = std::max(visibleLeft, tileLeft);
				const int right = std::min(visibleRight, tileLeft + cellSize);
				const jpegview_linux::TransparencyPatternColor color =
					jpegview_linux::TransparencyPatternTileColor(transparencyPattern_, tileX, tileY);
				SDL_SetRenderDrawColor(renderer_, color.red, color.green, color.blue, 255);
				const SDL_Rect tile{left, top, right - left, bottom - top};
				SDL_RenderFillRect(renderer_, &tile);
			}
		}
	}

	jpegview_linux::ZoomNavigatorLayout CurrentZoomNavigatorLayout() const {
		const SDL_Rect area = ImageAreaRect();
		const auto dimensions = ViewportContentDimensions();
		return jpegview_linux::CalculateZoomNavigatorLayout(dimensions.first, dimensions.second,
			area.x, area.y, area.w, area.h);
	}

	bool IsZoomNavigatorVisibleAt(int mouseX, int mouseY) const {
		const auto dimensions = ViewportContentDimensions();
		if (!showZoomNavigator_ || dimensions.first <= 0 || dimensions.second <= 0 ||
			cropSelection_.HasSelection() || cropMouseDragging_ || pictureLevelsPanelOpen_ ||
			unsharpDialogOpen_ || contextMenuOpen_ || fileDialogOpen_ || confirmationOpen_ ||
			aboutOpen_ || helpOpen_ || resizeDialog_.IsOpen() || cropSizeDialog_.IsOpen() ||
			batchCopyDialog_.IsOpen()) return false;
		const SDL_Rect area = ImageAreaRect();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			dimensions.first, dimensions.second, area.w, area.h);
		if (!jpegview_linux::ImageNeedsZoomNavigator(destination.width, destination.height,
			area.w, area.h)) return false;
		const jpegview_linux::ZoomNavigatorLayout layout = CurrentZoomNavigatorLayout();
		const bool visibleAfterZoom = static_cast<std::int32_t>(
			zoomNavigatorVisibleUntil_ - SDL_GetTicks()) > 0;
		return zoomNavigatorDragging_ || dragging_ || visibleAfterZoom ||
			PointInRect(mouseX, mouseY, SDL_Rect{layout.hotArea.x, layout.hotArea.y,
				layout.hotArea.width, layout.hotArea.height});
	}

	bool BeginZoomNavigatorDrag(int screenX, int screenY) {
		if (!IsZoomNavigatorVisibleAt(screenX, screenY)) return false;
		const jpegview_linux::ZoomNavigatorLayout layout = CurrentZoomNavigatorLayout();
		if (!PointInRect(screenX, screenY, SDL_Rect{layout.image.x, layout.image.y,
			layout.image.width, layout.image.height})) return false;
		const SDL_Rect area = ImageAreaRect();
		const auto dimensions = ViewportContentDimensions();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			dimensions.first, dimensions.second, area.w, area.h);
		const jpegview_linux::ZoomNavigatorPoint requested =
			jpegview_linux::NavigatorPointToImage(screenX, screenY, layout.image);
		const double currentCenterX = (area.w * 0.5 - destination.x) / destination.width;
		const double currentCenterY = (area.h * 0.5 - destination.y) / destination.height;
		const jpegview_linux::ZoomNavigatorPan pan =
			jpegview_linux::CalculateNavigatorCenterPan(currentCenterX, currentCenterY,
				requested.x, requested.y,
				static_cast<double>(area.w) / destination.width,
				static_cast<double>(area.h) / destination.height,
				destination.width, destination.height);
		PanViewport(pan.x, pan.y);
		zoomNavigatorDragging_ = true;
		interactionWorkPolicy_.NotifyActivity(
			jpegview_linux::InteractionActivity::NavigatorDrag);
		lastMouseX_ = screenX;
		lastMouseY_ = screenY;
		SDL_CaptureMouse(SDL_TRUE);
		SDL_Cursor* cursor = cropMoveCursor_;
		SDL_SetCursor(cursor != nullptr ? cursor : SDL_GetDefaultCursor());
		playback_.NotifyInteraction(SDL_GetTicks());
		return true;
	}

	void UpdateZoomNavigatorDrag(int deltaX, int deltaY) {
		if (!zoomNavigatorDragging_) return;
		interactionWorkPolicy_.NotifyActivity(
			jpegview_linux::InteractionActivity::NavigatorDrag);
		const SDL_Rect area = ImageAreaRect();
		const auto dimensions = ViewportContentDimensions();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			dimensions.first, dimensions.second, area.w, area.h);
		const jpegview_linux::ZoomNavigatorLayout layout = CurrentZoomNavigatorLayout();
		const jpegview_linux::ZoomNavigatorPan pan = jpegview_linux::CalculateNavigatorDragPan(
			deltaX, deltaY, layout.image, destination.width, destination.height);
		PanViewport(pan.x, pan.y);
		playback_.NotifyInteraction(SDL_GetTicks());
	}

	void EndZoomNavigatorDrag(int screenX, int screenY) {
		if (!zoomNavigatorDragging_) return;
		zoomNavigatorDragging_ = false;
		lastMouseX_ = screenX;
		lastMouseY_ = screenY;
		SDL_CaptureMouse(SDL_FALSE);
		UpdateCropCursor(screenX, screenY);
		UpdateZoomNavigatorCursor(screenX, screenY);
	}

	void UpdateZoomNavigatorCursor(int screenX, int screenY) const {
		if (thumbnailPanelResizing_ || IsThumbnailPanelResizeHandle(screenX, screenY)) return;
		const jpegview_linux::ZoomNavigatorLayout layout = CurrentZoomNavigatorLayout();
		if ((zoomNavigatorDragging_ || IsZoomNavigatorVisibleAt(screenX, screenY)) &&
			PointInRect(screenX, screenY, SDL_Rect{layout.image.x, layout.image.y,
				layout.image.width, layout.image.height})) {
			SDL_Cursor* cursor = cropMoveCursor_;
			SDL_SetCursor(cursor != nullptr ? cursor : SDL_GetDefaultCursor());
		}
	}

	bool IsMagnifyingGlassVisibleAt(int screenX, int screenY) const {
		if (!magnifyingGlass_.Enabled() || image_.width <= 0 || image_.height <= 0 ||
			dragging_ || cropMouseDragging_ ||
			zoomNavigatorDragging_ || thumbnailPanelResizing_ || transitionTexture_ != nullptr ||
			pictureLevelsPanelOpen_ || unsharpDialogOpen_ || contextMenuOpen_ || fileDialogOpen_ ||
			confirmationOpen_ || aboutOpen_ || helpOpen_ || resizeDialog_.IsOpen() ||
			cropSizeDialog_.IsOpen() || batchCopyDialog_.IsOpen()) return false;
		const SDL_Rect area = ImageAreaRect();
		const SDL_Rect currentPage = CurrentPageScreenRect(area);
		const SDL_Rect nextPage = NextPageScreenRect(area);
		const bool overCurrent = PointInRect(screenX, screenY, currentPage);
		const bool overNext = activeDoublePageRender_.has_value() &&
			PointInRect(screenX, screenY, nextPage);
		if (!overCurrent && !overNext) return false;
		// Let the transient zoom navigator retain its own hot area and cursor.
		return !IsZoomNavigatorVisibleAt(screenX, screenY);
	}

	void RestoreCursorForPoint(int screenX, int screenY) const {
		UpdateThumbnailPanelCursor(screenX, screenY);
		UpdateCropCursor(screenX, screenY);
		UpdateZoomNavigatorCursor(screenX, screenY);
	}

	void UpdateMagnifyingGlassCursor(int screenX, int screenY) {
		if (IsMagnifyingGlassVisibleAt(screenX, screenY)) {
			if (!magnifyingGlassCursorActive_) {
				const int previousVisibility = SDL_ShowCursor(kSdlCursorQuery);
				magnifyingGlassPreviousCursorVisibility_ = previousVisibility < 0 ?
					kSdlCursorEnabled : previousVisibility;
				SDL_ShowCursor(kSdlCursorDisabled);
				magnifyingGlassCursorActive_ = true;
			}
			return;
		}
		if (!magnifyingGlassCursorActive_) return;
		magnifyingGlassCursorActive_ = false;
		RestoreCursorForPoint(screenX, screenY);
		SDL_ShowCursor(magnifyingGlassPreviousCursorVisibility_);
	}

	void DiscardMagnifyingGlassRequest(const std::string& key) {
		if (key.empty() || (currentDisplayRequest_.has_value() &&
			currentDisplayRequest_->key == key)) return;
		displayTextureProtectedKeys_.erase(key);
		displayImageCache_.CancelBackground(key);
		displayImageCache_.Release(key);
		const auto texture = displayTextureCache_.find(key);
		if (texture == displayTextureCache_.end()) return;
		if (texture->second.texture != nullptr) SDL_DestroyTexture(texture->second.texture);
		displayTextureCacheBytes_ -= texture->second.bytes;
		cacheBudget_->Release(texture->second.bytes);
		displayTextureCache_.erase(texture);
	}

	void ClearMagnifyingGlassRequest() {
		DiscardMagnifyingGlassRequest(magnifyingGlassRequestKey_);
		magnifyingGlassRequestKey_.clear();
		magnifyingGlassRequestBaseKey_.clear();
		magnifyingGlassBackgroundRequestedKey_.clear();
		magnifyingGlassRequest_.reset();
	}

	void SetMagnifyingGlassEnabled(bool enabled) {
		magnifyingGlass_.SetEnabled(enabled);
		if (!enabled) ClearMagnifyingGlassRequest();
		UpdateMagnifyingGlassCursor(lastMouseX_, lastMouseY_);
		playback_.NotifyInteraction(SDL_GetTicks());
	}

	std::optional<jpegview_linux::DisplayImageRequest> MagnifyingGlassDisplayRequest() {
		if (!magnifyingGlass_.Enabled() || imageModified_ ||
			currentPixelsDetachedFromSource_ || !currentDisplayRequest_.has_value() ||
			currentDisplayRequest_->sourceWidth <= 0 ||
			currentDisplayRequest_->sourceHeight <= 0) return std::nullopt;
		const jpegview_linux::DisplayImageRequest& base = *currentDisplayRequest_;
		if (base.sourceWidth <= 0 || base.sourceHeight <= 0 ||
			base.targetWidth <= 0 || base.targetHeight <= 0) return std::nullopt;
		const double sourceScale = std::min(
			static_cast<double>(base.sourceWidth) / base.targetWidth,
			static_cast<double>(base.sourceHeight) / base.targetHeight);
		const double scale = std::min(1.0 / magnifyingGlass_.ZoomLevel(), sourceScale);
		const int targetWidth = std::min(base.sourceWidth,
			static_cast<int>(std::ceil(base.targetWidth * scale)));
		const int targetHeight = std::min(base.sourceHeight,
			static_cast<int>(std::ceil(base.targetHeight * scale)));
		if (targetWidth <= 0 || targetHeight <= 0) return std::nullopt;
		if (targetWidth == base.targetWidth && targetHeight == base.targetHeight) return base;
		if (magnifyingGlassRequest_.has_value() &&
			magnifyingGlassRequestBaseKey_ == base.key &&
			magnifyingGlassRequest_->decoded == base.decoded &&
			magnifyingGlassRequest_->frameIndex == base.frameIndex &&
			magnifyingGlassRequest_->targetWidth == targetWidth &&
			magnifyingGlassRequest_->targetHeight == targetHeight &&
			magnifyingGlassRequest_->autoContrast == base.autoContrast &&
			jpegview_linux::EqualImageProcessing(magnifyingGlassRequest_->processing,
				base.processing)) return magnifyingGlassRequest_;

		if (magnifyingGlassRequestBaseKey_ != base.key) {
			DiscardMagnifyingGlassRequest(magnifyingGlassRequestKey_);
			magnifyingGlassRequestKey_.clear();
			magnifyingGlassBackgroundRequestedKey_.clear();
			magnifyingGlassRequestBaseKey_.clear();
		}
		if (base.decoded) {
			magnifyingGlassRequest_ = jpegview_linux::MakeDisplayImageRequest(base.source,
				base.decoded, base.frameIndex, targetWidth, targetHeight, base.autoContrast,
				kMagnifyingGlassDisplayPriority, base.processing);
		} else {
			magnifyingGlassRequest_ = jpegview_linux::MakeJpegDisplayImageRequest(base.source,
				base.sourceWidth, base.sourceHeight, targetWidth, targetHeight,
				base.autoContrast, kMagnifyingGlassDisplayPriority, base.processing);
		}
		magnifyingGlassRequest_->workClass = jpegview_linux::PerfWorkClass::FocusedPreview;
		magnifyingGlassRequestBaseKey_ = base.key;
		return magnifyingGlassRequest_;
	}

	void RenderMagnifyingGlass(SDL_Texture* fallbackTexture) {
		if (!magnifyingGlass_.Enabled()) {
			if (!magnifyingGlassRequestKey_.empty() || magnifyingGlassRequest_.has_value() ||
				!magnifyingGlassBackgroundRequestedKey_.empty()) ClearMagnifyingGlassRequest();
			return;
		}
		if (!IsMagnifyingGlassVisibleAt(lastMouseX_, lastMouseY_)) {
			ClearMagnifyingGlassRequest();
			return;
		}
		const SDL_Rect area = ImageAreaRect();
		SDL_Rect displayed = CurrentPageScreenRect(area);
		SDL_Texture* lensTexture = fallbackTexture;
		int textureWidth = image_.width;
		int textureHeight = image_.height;
		bool hasTransparency = image_.hasTransparency;
		if (fallbackTexture == displayTexture_) {
			textureWidth = displayTextureWidth_;
			textureHeight = displayTextureHeight_;
		} else if (fallbackTexture != texture_) {
			for (auto& cached : displayTextureCache_) {
				if (cached.second.texture == fallbackTexture) {
					textureWidth = cached.second.width;
					textureHeight = cached.second.height;
					break;
				}
			}
		}
		if (activeDoublePageRender_.has_value() &&
			PointInRect(lastMouseX_, lastMouseY_, NextPageScreenRect(area))) {
			lensTexture = FindDisplayTexture(doublePagePartnerDisplayKey_);
			displayed = NextPageScreenRect(area);
			const auto partner = displayTextureCache_.find(doublePagePartnerDisplayKey_);
			if (partner != displayTextureCache_.end()) {
				textureWidth = partner->second.width;
				textureHeight = partner->second.height;
				hasTransparency = partner->second.hasTransparency;
			}
		}

		if (!activeDoublePageRender_.has_value()) {
			if (const auto request = MagnifyingGlassDisplayRequest(); request.has_value() &&
				request->key != (currentDisplayRequest_.has_value() ?
					currentDisplayRequest_->key : std::string())) {
				if (magnifyingGlassRequestKey_ != request->key) {
					DiscardMagnifyingGlassRequest(magnifyingGlassRequestKey_);
					magnifyingGlassRequestKey_ = request->key;
					magnifyingGlassBackgroundRequestedKey_.clear();
				}
				displayTextureProtectedKeys_.insert(request->key);
				if (SDL_Texture* cached = FindDisplayTexture(request->key)) {
					lensTexture = cached;
					const auto dimensions = displayTextureCache_.find(request->key);
					if (dimensions != displayTextureCache_.end()) {
						textureWidth = dimensions->second.width;
						textureHeight = dimensions->second.height;
					}
				} else {
					if (magnifyingGlassBackgroundRequestedKey_ != request->key &&
						request->targetHeight > 0 &&
						cacheBudget_->Capacity() / 4 /
							static_cast<std::size_t>(request->targetHeight) >=
							static_cast<std::size_t>(request->targetWidth)) {
						displayImageCache_.RequestBackground(*request);
						magnifyingGlassBackgroundRequestedKey_ = request->key;
					}
				}
			} else {
				ClearMagnifyingGlassRequest();
			}
		} else {
			ClearMagnifyingGlassRequest();
		}

		if (lensTexture == nullptr || textureWidth <= 0 || textureHeight <= 0) return;
		const jpegview_linux::MagnifyingGlassGeometry geometry =
			jpegview_linux::CalculateMagnifyingGlassGeometry(lastMouseX_, lastMouseY_,
				{displayed.x, displayed.y, displayed.w, displayed.h}, textureWidth, textureHeight,
				magnifyingGlass_.Width(), magnifyingGlass_.Height(), magnifyingGlass_.ZoomLevel());
		if (!geometry.valid) return;
		const SDL_Rect lens{geometry.lensRect.x, geometry.lensRect.y,
			geometry.lensRect.width, geometry.lensRect.height};
		const SDL_Rect content{geometry.contentDestinationRect.x,
			geometry.contentDestinationRect.y, geometry.contentDestinationRect.width,
			geometry.contentDestinationRect.height};
		const SDL_Rect source{geometry.sourceRect.x, geometry.sourceRect.y,
			geometry.sourceRect.width, geometry.sourceRect.height};
		SDL_SetRenderDrawColor(renderer_, 6, 6, 6, 255);
		SDL_RenderFillRect(renderer_, &lens);
		if (hasTransparency) RenderTransparencyBackground(lens, lens);
		SDL_RenderCopy(renderer_, lensTexture, &source, &content);
		DrawRect({lens.x - 2, lens.y - 2, lens.w + 4, lens.h + 4}, 8, 8, 8, 255);
		DrawRect(lens, 245, 245, 245, 255);
	}

	void RenderZoomNavigator(SDL_Texture* imageTexture, SDL_Texture* nextImageTexture) {
		if (imageTexture == nullptr || !IsZoomNavigatorVisibleAt(lastMouseX_, lastMouseY_)) return;
		const SDL_Rect area = ImageAreaRect();
		const jpegview_linux::ZoomNavigatorLayout layout = CurrentZoomNavigatorLayout();
		const SDL_Rect imageRect{layout.image.x, layout.image.y,
			layout.image.width, layout.image.height};
		const SDL_Rect frame{imageRect.x - 2, imageRect.y - 2,
			imageRect.w + 4, imageRect.h + 4};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 230);
		SDL_RenderFillRect(renderer_, &frame);
		if (activeDoublePageRender_.has_value() && nextImageTexture != nullptr) {
			const auto miniPage = [&imageRect, this](const jpegview_linux::SpreadPagePlacement& page) {
				const auto& spread = activeDoublePageRender_->layout;
				const int left = static_cast<int>(std::lround(
					static_cast<double>(imageRect.w) * page.x / spread.canvasWidth));
				const int top = static_cast<int>(std::lround(
					static_cast<double>(imageRect.h) * page.y / spread.canvasHeight));
				const int right = static_cast<int>(std::lround(
					static_cast<double>(imageRect.w) * (page.x + page.width) / spread.canvasWidth));
				const int bottom = static_cast<int>(std::lround(
					static_cast<double>(imageRect.h) * (page.y + page.height) / spread.canvasHeight));
				return SDL_Rect{imageRect.x + left, imageRect.y + top,
					std::max(1, right - left), std::max(1, bottom - top)};
			};
			const SDL_Rect currentMini = miniPage(activeDoublePageRender_->layout.currentPage);
			const SDL_Rect nextMini = miniPage(activeDoublePageRender_->layout.nextPage);
			SDL_RenderCopy(renderer_, imageTexture, nullptr, &currentMini);
			SDL_RenderCopy(renderer_, nextImageTexture, nullptr, &nextMini);
		} else {
			SDL_RenderCopy(renderer_, imageTexture, nullptr, &imageRect);
		}
		DrawRect({frame.x, frame.y, frame.w, frame.h}, 245, 245, 245, 255);

		const auto dimensions = ViewportContentDimensions();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			dimensions.first, dimensions.second, area.w, area.h);
		const jpegview_linux::NormalizedImageRect visible =
			jpegview_linux::CalculateVisibleImageRect(destination.x + area.x,
				destination.y + area.y, destination.width, destination.height,
				area.x, area.y, area.w, area.h);
		const jpegview_linux::ZoomNavigatorRect mapped =
			jpegview_linux::MapVisibleRectToNavigator(visible, layout.image);
		if (mapped.width <= 0 || mapped.height <= 0) return;
		DrawRect({mapped.x - 1, mapped.y - 1, mapped.width + 2, mapped.height + 2},
			0, 0, 0, 255);
		DrawRect({mapped.x, mapped.y, mapped.width, mapped.height}, 255, 255, 255, 255);
	}

	jpegview_linux::SelectionScreenRect ImageDestinationScreenRect() const {
		const SDL_Rect imageArea = ImageAreaRect();
		const SDL_Rect destination = CurrentPageScreenRect(imageArea);
		return {destination.x, destination.y, destination.w, destination.h};
	}

	bool BeginCropDrag(int screenX, int screenY) {
		if (image_.width <= 0 || image_.height <= 0 || pictureLevelsPanelOpen_ ||
			unsharpDialogOpen_ || resizeDialog_.IsOpen()) return false;
		const SDL_Rect imageArea = ImageAreaRect();
		const jpegview_linux::SelectionScreenRect destination = ImageDestinationScreenRect();
		if (!PointInRect(screenX, screenY, imageArea) || destination.width <= 0 ||
			destination.height <= 0 || screenX < destination.x || screenY < destination.y ||
			screenX >= destination.x + destination.width ||
			screenY >= destination.y + destination.height) return false;

		const Uint16 modifiers = static_cast<Uint16>(SDL_GetModState());
		const bool control = (modifiers & 0x00c0u) != 0;
		const bool shift = (modifiers & 0x0003u) != 0;
		const jpegview_linux::SelectionPoint point = jpegview_linux::CropSelectionModel::ScreenToImage(
			screenX, screenY, destination, image_.width, image_.height);
		if (cropSelection_.HasSelection()) {
			const jpegview_linux::SelectionScreenRect selected =
				jpegview_linux::CropSelectionModel::ToScreen(cropSelection_.Rect(), destination,
					image_.width, image_.height);
			const auto handle = jpegview_linux::CropSelectionModel::HitTest(screenX, screenY,
				selected, cropSelection_.Mode() == jpegview_linux::CropSelectionMode::FixedSize);
			if (handle != jpegview_linux::CropSelectionHandle::None &&
				cropSelection_.StartManipulation(point.x, point.y, handle)) {
				cropMouseDragging_ = true;
				interactionWorkPolicy_.NotifyActivity(
					jpegview_linux::InteractionActivity::CropDrag);
				cropDragHandle_ = handle;
				cropDragWasNew_ = false;
				cropZoomOnRelease_ = shift;
				cropDragMoved_ = false;
				cropDragStartX_ = screenX;
				cropDragStartY_ = screenY;
				SDL_CaptureMouse(SDL_TRUE);
				UpdateCropCursor(screenX, screenY);
				dragging_ = false;
				return true;
			}
		}

		const bool requiresPanning = destination.width > imageArea.w ||
			destination.height > imageArea.h;
		if (!jpegview_linux::ShouldStartNewCropSelection(selectionModeEnabled_,
			control || shift, requiresPanning)) return false;
		if (!cropSelection_.StartNew(point.x, point.y)) return false;
		cropMouseDragging_ = true;
		interactionWorkPolicy_.NotifyActivity(
			jpegview_linux::InteractionActivity::CropDrag);
		cropDragHandle_ = jpegview_linux::CropSelectionHandle::NewSelection;
		cropDragWasNew_ = true;
		cropZoomOnRelease_ = shift;
		cropDragMoved_ = false;
		cropDragStartX_ = screenX;
		cropDragStartY_ = screenY;
		SDL_CaptureMouse(SDL_TRUE);
		UpdateCropCursor(screenX, screenY);
		dragging_ = false;
		return true;
	}

	SDL_Cursor* CropCursorForHandle(jpegview_linux::CropSelectionHandle handle) const {
		using jpegview_linux::CropSelectionHandle;
		switch (handle) {
		case CropSelectionHandle::Move: return cropMoveCursor_;
		case CropSelectionHandle::Left:
		case CropSelectionHandle::Right: return cropHorizontalCursor_;
		case CropSelectionHandle::Top:
		case CropSelectionHandle::Bottom: return cropVerticalCursor_;
		case CropSelectionHandle::TopLeft:
		case CropSelectionHandle::BottomRight: return cropDiagonalDownCursor_;
		case CropSelectionHandle::TopRight:
		case CropSelectionHandle::BottomLeft: return cropDiagonalUpCursor_;
		case CropSelectionHandle::NewSelection: return cropCrosshairCursor_;
		default: return nullptr;
		}
	}

	void UpdateCropCursor(int screenX, int screenY) const {
		if (thumbnailPanelResizing_ || IsThumbnailPanelResizeHandle(screenX, screenY)) return;
		if (dragging_ && !cropMouseDragging_) {
			SDL_SetCursor(SDL_GetDefaultCursor());
			return;
		}
		if (cropMouseDragging_) {
			SDL_Cursor* cursor = CropCursorForHandle(cropDragHandle_);
			SDL_SetCursor(cursor != nullptr ? cursor : SDL_GetDefaultCursor());
			return;
		}
		const SDL_Rect imageArea = ImageAreaRect();
		const jpegview_linux::SelectionScreenRect destination = ImageDestinationScreenRect();
		if (cropSelection_.HasSelection() && PointInRect(screenX, screenY, imageArea) &&
			screenX >= destination.x && screenY >= destination.y &&
			screenX < destination.x + destination.width &&
			screenY < destination.y + destination.height) {
			const jpegview_linux::SelectionScreenRect selected =
				jpegview_linux::CropSelectionModel::ToScreen(cropSelection_.Rect(), destination,
					image_.width, image_.height);
			const auto handle = jpegview_linux::CropSelectionModel::HitTest(screenX, screenY,
				selected, cropSelection_.Mode() == jpegview_linux::CropSelectionMode::FixedSize);
			SDL_Cursor* cursor = CropCursorForHandle(handle);
			SDL_SetCursor(cursor != nullptr ? cursor : SDL_GetDefaultCursor());
			return;
		}
		const Uint16 modifiers = static_cast<Uint16>(SDL_GetModState());
		const bool forcedByModifier = (modifiers & (0x00c0u | 0x0003u)) != 0;
		const bool imageNeedsPanning = destination.width > imageArea.w ||
			destination.height > imageArea.h;
		const bool canStartSelection = jpegview_linux::ShouldStartNewCropSelection(
			selectionModeEnabled_, forcedByModifier, imageNeedsPanning);
		if (canStartSelection && PointInRect(screenX, screenY, imageArea) &&
			screenX >= destination.x && screenY >= destination.y &&
			screenX < destination.x + destination.width &&
			screenY < destination.y + destination.height && cropCrosshairCursor_ != nullptr) {
			SDL_SetCursor(cropCrosshairCursor_);
		} else {
			SDL_SetCursor(SDL_GetDefaultCursor());
		}
	}

	void UpdateCropDrag(int screenX, int screenY) {
		if (!cropMouseDragging_) return;
		interactionWorkPolicy_.NotifyActivity(
			jpegview_linux::InteractionActivity::CropDrag);
		const jpegview_linux::SelectionPoint point = jpegview_linux::CropSelectionModel::ScreenToImage(
			screenX, screenY, ImageDestinationScreenRect(), image_.width, image_.height);
		const bool updated = cropSelection_.Update(point.x, point.y, viewport_.Zoom());
		if (updated && (cropSelection_.Mode() == jpegview_linux::CropSelectionMode::FixedSize ||
			std::abs(screenX - cropDragStartX_) >= 2 ||
			std::abs(screenY - cropDragStartY_) >= 2)) cropDragMoved_ = true;
	}

	void EndCropDrag(int screenX, int screenY) {
		if (!cropMouseDragging_) return;
		lastMouseX_ = screenX;
		lastMouseY_ = screenY;
		UpdateCropDrag(screenX, screenY);
		cropSelection_.End();
		cropMouseDragging_ = false;
		SDL_CaptureMouse(SDL_FALSE);
		if (cropDragWasNew_) {
			if (!cropDragMoved_) {
				cropSelection_.Clear();
			} else if (cropZoomOnRelease_) {
				ZoomToSelection();
				cropSelection_.Clear();
			} else {
				OpenCropContextMenu();
			}
		} else if (cropDragMoved_ && cropZoomOnRelease_) {
			ZoomToSelection();
			cropSelection_.Clear();
		}
		cropDragWasNew_ = false;
		cropZoomOnRelease_ = false;
		cropDragMoved_ = false;
		cropDragHandle_ = jpegview_linux::CropSelectionHandle::None;
		UpdateCropCursor(screenX, screenY);
	}

	void ClearCropSelection() {
		if (cropMouseDragging_) SDL_CaptureMouse(SDL_FALSE);
		cropSelection_.Clear();
		cropMouseDragging_ = false;
		cropDragWasNew_ = false;
		cropZoomOnRelease_ = false;
		cropDragMoved_ = false;
		cropDragHandle_ = jpegview_linux::CropSelectionHandle::None;
		UpdateCropCursor(lastMouseX_, lastMouseY_);
	}

	bool IsThumbnailPanelResizeHandle(int x, int y) const {
		if (!thumbnailPanelVisible_) return false;
		const SDL_Rect panel = ThumbnailPanelRect();
		return panel.w > 0 && y >= panel.y && y < panel.y + panel.h &&
			x >= panel.x + panel.w - kThumbnailResizeHandleHalfWidth &&
			x <= panel.x + panel.w + kThumbnailResizeHandleHalfWidth;
	}

	void UpdateThumbnailPanelCursor(int x, int y) const {
		if (thumbnailResizeCursor_ == nullptr) return;
		SDL_SetCursor(thumbnailPanelResizing_ || IsThumbnailPanelResizeHandle(x, y) ?
			thumbnailResizeCursor_ : SDL_GetDefaultCursor());
	}

	bool BeginThumbnailPanelResize(int x, int y) {
		if (!IsThumbnailPanelResizeHandle(x, y)) return false;
		thumbnailPanelResizing_ = true;
		interactionWorkPolicy_.NotifyActivity(jpegview_linux::InteractionActivity::Resize);
		thumbnailPanelResizeChanged_ = false;
		thumbnailResizeOffset_ = ThumbnailPanelRect().w - x;
		SDL_CaptureMouse(SDL_TRUE);
		UpdateThumbnailPanelCursor(x, y);
		return true;
	}

	void ResizeThumbnailPanel(int mouseX) {
		interactionWorkPolicy_.NotifyActivity(jpegview_linux::InteractionActivity::Resize);
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		(void)windowHeight;
		const int maximumWidth = std::max(1, std::min(
			jpegview_linux::kMaximumThumbnailPanelWidth, windowWidth - 1));
		const int minimumWidth = std::min(jpegview_linux::kMinimumThumbnailPanelWidth, maximumWidth);
		const int width = std::clamp(mouseX + thumbnailResizeOffset_, minimumWidth, maximumWidth);
		if (width == thumbnailPanelWidth_) return;
		thumbnailPanelWidth_ = width;
		thumbnailPanelResizeChanged_ = true;
		if (viewport_.IsFitToWindow()) {
			FitToWindow(viewport_.FillWithCrop(), viewport_.NoEnlarge());
		} else {
			RefreshFitRelativeZoomBase();
		}
	}

	void EndThumbnailPanelResize(int mouseX, int mouseY) {
		if (!thumbnailPanelResizing_) return;
		thumbnailPanelResizing_ = false;
		SDL_CaptureMouse(SDL_FALSE);
		if (thumbnailPanelResizeChanged_) {
			PrepareThumbnailPreload();
			thumbnailPanelResizeChanged_ = false;
		}
		UpdateThumbnailPanelCursor(mouseX, mouseY);
		SaveSettings();
	}

	void RenderThumbnailPanel() {
		if (!thumbnailPanelVisible_ || fileList_.Empty()) return;
		const SDL_Rect panel = ThumbnailPanelRect();
		if (panel.w <= 0 || panel.h <= 0) return;
		SDL_SetRenderDrawColor(renderer_, 7, 7, 7, 238);
		SDL_RenderFillRect(renderer_, &panel);
		const int rowHeight = jpegview_linux::ThumbnailRowHeight(panel.w, kThumbnailVerticalMargin);
		const std::optional<std::size_t> doublePagePartnerIndex = activeDoublePageRender_.has_value() ?
			std::optional<std::size_t>(activeDoublePageRender_->layout.secondIndex) : std::nullopt;
		const std::vector<jpegview_linux::ThumbnailSlot> slots = jpegview_linux::ThumbnailPanelSlots(
			fileList_.Size(), fileList_.CurrentIndex(), panel.h, rowHeight,
			fileList_.MarkedIndex(), doublePagePartnerIndex);
		for (const jpegview_linux::ThumbnailSlot& slot : slots) {
			const SDL_Rect row{panel.x, slot.y, panel.w, rowHeight};
			const bool activeSpreadPage = slot.current || slot.doublePagePartner;
			if (activeSpreadPage) {
				SDL_SetRenderDrawColor(renderer_, 32, 58, 82, 255);
				SDL_RenderFillRect(renderer_, &row);
			}
			const jpegview_linux::SourceDescriptor* source =
				fileList_.DescriptorAt(slot.fileIndex);
			if (source == nullptr) continue;
			const jpegview_linux::SourceKey key = source->Key();
			auto cached = thumbnailCache_.find(key);
			if (cached != thumbnailCache_.end() && cached->second.texture != nullptr) {
				thumbnailScheduler_.Touch(key);
				const jpegview_linux::ThumbnailRect thumbnail = jpegview_linux::ThumbnailImageRect(
					cached->second.width, cached->second.height,
					panel.w, row.y, row.h, kThumbnailVerticalMargin);
				SDL_Rect imageRect{
					panel.x + thumbnail.x,
					thumbnail.y,
					thumbnail.width,
					thumbnail.height
				};
				if (cached->second.hasTransparency) {
					RenderTransparencyBackground(imageRect, panel);
				}
				SDL_RenderCopy(renderer_, cached->second.texture, nullptr, &imageRect);
				if (!activeSpreadPage) {
					SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 125);
					SDL_RenderFillRect(renderer_, &imageRect);
				} else {
					DrawRect(imageRect, 225, 225, 225);
				}
			} else {
				const std::string position = std::to_string(slot.fileIndex + 1);
				DrawText(position, panel.x + (panel.w - TextWidth(position, kUiTextScale)) / 2,
					row.y + (row.h - TextLineHeight()) / 2, kUiTextScale,
					activeSpreadPage ? 215 : 95, activeSpreadPage ? 215 : 95,
					activeSpreadPage ? 215 : 95);
			}
			DrawLine(panel.x, row.y + row.h - 1, std::max(panel.x, panel.x + panel.w - 2),
				row.y + row.h - 1, 48, 48, 48);
			if (fileList_.IsArchiveMember(slot.fileIndex)) {
				DrawLine(panel.x + 1, row.y + 2, panel.x + 1,
					std::max(row.y + 2, row.y + row.h - 3), 255, 195, 95);
			}
			if (slot.marked) {
				const SDL_Rect markedFrame{row.x + 1, row.y + 1, row.w - 2, row.h - 2};
				DrawRect(markedFrame, 255, 195, 65);
			}
		}
		DrawLine(panel.x + panel.w - 1, panel.y, panel.x + panel.w - 1,
			panel.y + panel.h - 1, 100, 100, 100);
	}

	bool HandleThumbnailPanelClick(int x, int y) {
		if (!thumbnailPanelVisible_ || fileList_.Empty()) return false;
		const SDL_Rect panel = ThumbnailPanelRect();
		if (!PointInRect(x, y, panel)) return false;
		const int rowHeight = jpegview_linux::ThumbnailRowHeight(panel.w, kThumbnailVerticalMargin);
		const std::vector<jpegview_linux::ThumbnailSlot> slots = jpegview_linux::ThumbnailPanelSlots(
			fileList_.Size(), fileList_.CurrentIndex(), panel.h, rowHeight);
		for (const jpegview_linux::ThumbnailSlot& slot : slots) {
			const SDL_Rect row{panel.x, slot.y, panel.w, rowHeight};
			if (!PointInRect(x, y, row) || slot.current) continue;
			const int direction = slot.fileIndex > fileList_.CurrentIndex() ? 1 : -1;
			if (fileList_.Select(slot.fileIndex)) LoadCurrent(direction);
			break;
		}
		return true;
	}

	bool HandleImageInfoClick(int x, int y) {
		if (!infoVisible_ || fileList_.Empty() || contextMenuOpen_ || fileDialogOpen_ ||
			advancedConfiguration_.IsOpen() ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return false;
		const jpegview_linux::InformationOverlayPaintPlan paint = BuildImageInfoPaintPlan();
		if (!jpegview_linux::Contains(paint.spectrumButton, x, y)) return false;
		showHistogram_ = !showHistogram_;
		SaveSettings();
		return true;
	}

	void RenderControls() {
		if (!navigationPanelEnabled_ || !controlsVisible_ || contextMenuOpen_ || fileDialogOpen_ ||
			advancedConfiguration_.IsOpen() ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen() || cropSizeDialog_.IsOpen()) return;
		const jpegview_linux::NavigationPanelPaint paint = CurrentNavigationPanelPaint();
		const SDL_Rect panel = SdlRect(paint.panel);
		const jpegview_linux::NavigationButtonPaint* hoveredButton = nullptr;

		// Some accelerated SDL/X11 renderers rasterize a line endpoint one pixel
		// beyond its logical bounds. Keep every navigation-panel primitive inside
		// the panel so reopening it after a context menu cannot damage the image.
		SDL_RenderSetClipRect(renderer_, &panel);
		for (const jpegview_linux::NavigationButtonPaint& button : paint.buttons) {
			if (button.hovered) hoveredButton = &button;
			RenderNavigationButton(button);
		}
		SDL_RenderSetClipRect(renderer_, nullptr);
		if (hoveredButton == nullptr) return;
		const std::string text = jpegview_linux::NavigationTooltip(hoveredButton->command,
			viewport_.IsFitToWindow(), fullscreen_, fileList_.GetSorting(), selectionModeEnabled_,
			doublePageModeEnabled_, mangaReadingOrderEnabled_, spacebarNavigatesImages_,
			viewport_.FitRelativeZoomMode());
		if (text.empty()) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const std::string label = ClipText(text, std::max(1, windowWidth - 24));
		RenderOverlayPaint(jpegview_linux::NavigationTooltipPaint(hoveredButton->rect,
			label, TextWidth(label, kUiTextScale), TextLineHeight(), windowWidth, windowHeight));
	}

	void RenderConfirmation() {
		if (!confirmationOpen_) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const bool moveToTrash = confirmationCommand_ == IDM_MOVE_TO_RECYCLE_BIN ||
			confirmationCommand_ == IDM_MOVE_TO_RECYCLE_BIN_CONFIRM ||
			confirmationCommand_ == IDM_MOVE_TO_RECYCLE_BIN_CONFIRM_PERMANENT_DELETE;
		const int width = std::min(760, std::max(360, windowWidth - 40));
		const int height = moveToTrash ? 180 : 136;
		const SDL_Rect panel{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 220);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 220, 170, 110);
		DrawText("CONFIRM ACTION", panel.x + 18, panel.y + 14, kUiTextScale, 255, 220, 150);
		DrawText(confirmationMessage_, panel.x + 18, panel.y + 42, kUiTextScale);
		const std::string filename = confirmationCommand_ == kConfirmRestoreParameterDb ?
			ClipText(InfoText(pendingParameterDbRestoreSource_), width - 36) :
			(fileList_.Empty() ? std::string() :
				ClipText(InfoText(fileList_.Current().filename().string()), width - 36));
		if (moveToTrash) {
			const ConfirmationPreview preview = AvailableConfirmationPreview();
			const SDL_Rect previewBox{panel.x + 18, panel.y + 64, 112, 72};
			SDL_SetRenderDrawColor(renderer_, 15, 15, 15, 255);
			SDL_RenderFillRect(renderer_, &previewBox);
			if (preview.texture != nullptr && preview.width > 0 && preview.height > 0) {
				const jpegview_linux::ThumbnailSize size = jpegview_linux::FitThumbnailSize(
					preview.width, preview.height, previewBox.w - 8, previewBox.h - 8);
				const SDL_Rect imageRect{previewBox.x + (previewBox.w - size.width) / 2,
					previewBox.y + (previewBox.h - size.height) / 2, size.width, size.height};
				if (preview.hasTransparency) RenderTransparencyBackground(imageRect, previewBox);
				SDL_RenderCopy(renderer_, preview.texture, nullptr, &imageRect);
			} else {
				DrawText("NO PREVIEW", previewBox.x + 10,
					previewBox.y + (previewBox.h - TextLineHeight()) / 2, kUiTextScale,
					150, 150, 150);
			}
			DrawRect(previewBox, 90, 90, 90);
			DrawText(ClipText(filename, width - 166), panel.x + 142, panel.y + 70,
				kUiTextScale, 235, 235, 235);
			DrawText("Selected image", panel.x + 142, panel.y + 94,
				kUiTextScale, 165, 165, 165);
			DrawText("ENTER or SPACE: YES     ESC: CANCEL", panel.x + 18,
				panel.y + height - 26, kUiTextScale, 180, 180, 180);
		} else {
			DrawText(filename, panel.x + 18, panel.y + 68, kUiTextScale, 220, 220, 220);
			DrawText("ENTER or SPACE: YES     ESC: CANCEL", panel.x + 18,
				panel.y + 104, kUiTextScale, 180, 180, 180);
		}
	}

	ConfirmationPreview AvailableConfirmationPreview() {
		if (fileList_.Empty()) return {};
		const fs::path& current = fileList_.Current();
		const fs::path normalizedCurrent = current.lexically_normal();
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(current);
		const auto thumbnail = thumbnailCache_.find(source.Key());
		if (thumbnail != thumbnailCache_.end() && thumbnail->second.texture != nullptr) {
			return {thumbnail->second.texture, thumbnail->second.width,
				thumbnail->second.height, thumbnail->second.hasTransparency};
		}
		if (currentDisplayRequest_.has_value() &&
			currentDisplayRequest_->filename.lexically_normal() == normalizedCurrent) {
			const auto display = displayTextureCache_.find(currentDisplayRequest_->key);
			if (display != displayTextureCache_.end() && display->second.texture != nullptr) {
				return {display->second.texture, display->second.width,
					display->second.height, display->second.hasTransparency};
			}
		}
		if (recentImageLoadState_.LoadedPath().lexically_normal() != normalizedCurrent) return {};
		if (displayTexture_ != nullptr) {
			return {displayTexture_, displayTextureWidth_, displayTextureHeight_,
				displayImage_.hasTransparency};
		}
		if (texture_ != nullptr && image_.width > 0 && image_.height > 0) {
			return {texture_, image_.width, image_.height, image_.hasTransparency};
		}
		return {};
	}

	void RenderAbout() {
		if (!aboutOpen_) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const SDL_Rect panel = AboutPanelRect(windowWidth, windowHeight);
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 220);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 160, 190, 225);
		DrawText("JPEGVIEW LINUX", panel.x + 18, panel.y + 16, kUiTextScale, 255, 255, 255);
		DrawText("A PORT OF JPEGVIEW FOR LINUX", panel.x + 18, panel.y + 48, kUiTextScale, 210, 225, 250);
		DrawText(std::string("VERSION ") + JPEGVIEW_APP_VERSION,
			panel.x + 18, panel.y + 80, kUiTextScale, 210, 225, 250);
		const SDL_Rect link = AboutRepositoryLinkRect(windowWidth, windowHeight);
		const std::string linkLabel = ClipText(kRepositoryUrl, panel.w - 36);
		DrawText(linkLabel, link.x, link.y, kUiTextScale, 125, 185, 255);
		if (link.w > 0) DrawLine(link.x, link.y + link.h - 2, link.x + link.w - 1,
			link.y + link.h - 2, 125, 185, 255);
		DrawText("PRESS ESC TO CLOSE", panel.x + 18, panel.y + 156, kUiTextScale, 180, 180, 180);
	}

	void RenderHelp() {
		if (!helpOpen_) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(940, std::max(480, windowWidth - 40));
		const int height = std::min(360, std::max(300, windowHeight - 40));
		const SDL_Rect panel{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 235);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 160, 190, 225);
		DrawText("QUICK HELP — JPEGVIEW LINUX", panel.x + 18, panel.y + 14,
			kUiTextScale, 255, 255, 255);
		const std::string spaceHelp = spacebarNavigatesImages_ ?
			"Navigate: Space next; Shift+Space previous; Return fit; Ctrl+Return fill with crop; +/- zoom" :
			"Scale: Space fit/actual; Return fit; Ctrl+Return fill with crop; +/- zoom";
		const std::array<std::string, 10> lines = {
			"Navigate: arrows/wheel; Home/End; D double page; J manga order; Ctrl+M mark; Alt+arrows siblings",
			"Zoom/pan: Ctrl+wheel or Ctrl+Up/Down; drag; Shift+Arrow; Z lens; wheel resizes it",
			"Navigator: hover upper-right when magnified; click or drag its map to reposition",
			spaceHelp,
			"Panels: F2 info; Shift+N filename; Ctrl+N nav; Ctrl+T thumbs; Ctrl+E crop mode",
			"Files: Ctrl+O open; Ctrl+S save processed; Ctrl+Shift+S save displayed size",
			"Clipboard: Ctrl+C copy image; Ctrl+Shift+C copy path; Ctrl+V paste PNG",
			"Adjustments: Up/Down rotate; F5 auto correction; F6 local density; Ctrl+Shift+R resize",
			"Window: F11 fullscreen; Shift+F11 title bar; Shift+F12 always on top",
			"Dialogs: Ctrl+Tab switches Browse/Recents; type to filter; arrows/pages select; wheel scrolls; drag to resize"};
		for (std::size_t index = 0; index < lines.size(); ++index) {
			DrawText(ClipText(lines[index], width - 36), panel.x + 18,
				panel.y + 48 + static_cast<int>(index) * 27, kUiTextScale, 220, 225, 235);
		}
		DrawText("Right-click: compact; Shift+right: full; Menu: compact; Esc/F1 closes",
			panel.x + 18, panel.y + height - 32, kUiTextScale, 180, 190, 205);
	}

	void RenderImageTransition(const SDL_Rect& destination, const SDL_Rect& imageArea, SDL_Texture* currentTexture) {
		if (transitionTexture_ == nullptr || transitionStartTick_ == 0) {
			SDL_RenderCopy(renderer_, currentTexture, nullptr, &destination);
			return;
		}
		const Uint32 elapsed = SDL_GetTicks() - transitionStartTick_;
		const double progress = std::min(1.0, static_cast<double>(elapsed) /
			static_cast<double>(transitionDurationMs_));
		if (progress >= 1.0) {
			ClearTransition();
			SDL_RenderCopy(renderer_, currentTexture, nullptr, &destination);
			return;
		}

		const jpegview_linux::ViewportRect oldRect = viewport_.Destination(
			transitionImage_.width, transitionImage_.height, imageArea.w, imageArea.h);
		SDL_Rect oldDestination{oldRect.x + imageArea.x, oldRect.y + imageArea.y,
			oldRect.width, oldRect.height};
		SDL_Rect enteringDestination = destination;
		const int horizontalDistance = std::max(imageArea.w, destination.w);
		const int verticalDistance = std::max(imageArea.h, destination.h);
		const int effect = transitionEffect_;
		const bool fromRight = effect == IDM_EFFECT_SLIDE_RL || effect == IDM_EFFECT_ROLL_RL || effect == IDM_EFFECT_SCROLL_RL;
		const bool fromLeft = effect == IDM_EFFECT_SLIDE_LR || effect == IDM_EFFECT_ROLL_LR || effect == IDM_EFFECT_SCROLL_LR;
		const bool fromTop = effect == IDM_EFFECT_SLIDE_TB || effect == IDM_EFFECT_ROLL_TB || effect == IDM_EFFECT_SCROLL_TB;
		const bool fromBottom = effect == IDM_EFFECT_SLIDE_BT || effect == IDM_EFFECT_ROLL_BT || effect == IDM_EFFECT_SCROLL_BT;
		if (fromRight || fromLeft) {
			const int distance = static_cast<int>(std::round(horizontalDistance * (1.0 - progress)));
			enteringDestination.x += fromRight ? distance : -distance;
			oldDestination.x += fromRight ? -static_cast<int>(std::round(horizontalDistance * progress)) :
				static_cast<int>(std::round(horizontalDistance * progress));
		} else if (fromTop || fromBottom) {
			const int distance = static_cast<int>(std::round(verticalDistance * (1.0 - progress)));
			enteringDestination.y += fromBottom ? distance : -distance;
			oldDestination.y += fromBottom ? -static_cast<int>(std::round(verticalDistance * progress)) :
				static_cast<int>(std::round(verticalDistance * progress));
		}

		SDL_SetTextureAlphaMod(transitionTexture_, 255);
		SDL_SetTextureBlendMode(currentTexture, SDL_BLENDMODE_BLEND);
		SDL_SetTextureAlphaMod(currentTexture, 255);
		if (effect == IDM_EFFECT_BLEND) {
			SDL_SetTextureAlphaMod(transitionTexture_, static_cast<Uint8>(std::round(255.0 * (1.0 - progress))));
		}
		SDL_RenderCopy(renderer_, transitionTexture_, nullptr, &oldDestination);
		SDL_RenderCopy(renderer_, currentTexture, nullptr, &enteringDestination);
	}

	void RenderCropSelection() {
		if (!cropSelection_.HasSelection() || image_.width <= 0 || image_.height <= 0) return;
		const jpegview_linux::SelectionScreenRect destination = ImageDestinationScreenRect();
		const jpegview_linux::SelectionScreenRect selected =
			jpegview_linux::CropSelectionModel::ToScreen(cropSelection_.Rect(), destination,
				image_.width, image_.height);
		if (selected.width <= 0 || selected.height <= 0) return;
		const int left = selected.x;
		const int top = selected.y;
		const int right = selected.x + selected.width - 1;
		const int bottom = selected.y + selected.height - 1;
		// A black under-stroke keeps the selection legible on both dark and bright
		// image content; short yellow dashes mirror the Windows dotted white frame
		// while matching the Linux frontend's existing focus accent.
		DrawLine(left - 1, top - 1, right + 1, top - 1, 0, 0, 0, 255);
		DrawLine(left - 1, bottom + 1, right + 1, bottom + 1, 0, 0, 0, 255);
		DrawLine(left - 1, top - 1, left - 1, bottom + 1, 0, 0, 0, 255);
		DrawLine(right + 1, top - 1, right + 1, bottom + 1, 0, 0, 0, 255);
		for (int x = left; x <= right; x += 4) {
			DrawLine(x, top, std::min(right, x + 1), top, 255, 205, 0, 255);
			DrawLine(x, bottom, std::min(right, x + 1), bottom, 255, 205, 0, 255);
		}
		for (int y = top; y <= bottom; y += 4) {
			DrawLine(left, y, left, std::min(bottom, y + 1), 255, 205, 0, 255);
			DrawLine(right, y, right, std::min(bottom, y + 1), 255, 205, 0, 255);
		}
		if (cropSelection_.Mode() == jpegview_linux::CropSelectionMode::FixedSize ||
			selected.width < 18 || selected.height < 18) return;
		const int middleX = (left + right) / 2;
		const int middleY = (top + bottom) / 2;
		std::array<std::pair<int, int>, 8> points = {{
			{left, top}, {middleX, top}, {right, top}, {left, middleY},
			{right, middleY}, {left, bottom}, {middleX, bottom}, {right, bottom}}};
		const std::size_t pointCount = selected.width > 70 && selected.height > 70 ? points.size() : 4;
		const std::array<std::size_t, 4> corners = {{0, 2, 5, 7}};
		for (std::size_t index = 0; index < pointCount; ++index) {
			const std::size_t pointIndex = pointCount == 4 ? corners[index] : index;
			const int x = points[pointIndex].first;
			const int y = points[pointIndex].second;
			SDL_Rect outer{x - 4, y - 4, 9, 9};
			SDL_SetRenderDrawColor(renderer_, 245, 245, 245, 255);
			SDL_RenderFillRect(renderer_, &outer);
			SDL_Rect inner{x - 2, y - 2, 5, 5};
			SDL_SetRenderDrawColor(renderer_, 20, 20, 20, 255);
			SDL_RenderFillRect(renderer_, &inner);
		}
	}

	void RenderContextMenu() {
		if (!contextMenuOpen_) return;
		const SDL_Rect menu = ContextMenuRect();
		const std::vector<ContextMenuColumn> columns = ContextMenuColumns();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 220);
		SDL_RenderFillRect(renderer_, &menu);
		DrawRect(menu, 185, 185, 185);

		for (const ContextMenuColumn& column : columns) {
			const int columnX = menu.x + column.x;
			int itemTop = menu.y + kContextMenuVerticalPadding;
			for (std::size_t i = column.begin; i < column.end; ++i) {
				const MenuItem& item = contextMenuItems_[i];
				if (item.separator) {
					DrawLine(columnX + 10, itemTop + 4, columnX + column.width - 10, itemTop + 4,
						75, 75, 75);
					itemTop += kContextMenuSeparatorHeight;
					continue;
				}
				if (static_cast<int>(i) == menuSelected_) {
					SDL_SetRenderDrawColor(renderer_, 45, 82, 120, 205);
					SDL_Rect selection{columnX + 3, itemTop, column.width - 6, ContextMenuRowHeight()};
					SDL_RenderFillRect(renderer_, &selection);
				}
				const Uint8 textColor = item.command == 0 ? 135 : (item.enabled ? 235 : 100);
				const std::string label = MenuLabel(item);
				const std::string shortcut = MenuShortcut(item);
				const int textY = itemTop + (ContextMenuRowHeight() - TextLineHeight()) / 2;
				DrawText(label, columnX + 12, textY, kUiTextScale,
					textColor, textColor, textColor);
				if (item.mnemonicOffset >= 0) {
					const std::size_t visibleOffset = static_cast<std::size_t>(item.mnemonicOffset) +
						(item.checked ? 4u : 0u);
					if (visibleOffset < label.size()) {
						int underlineX = columnX + 12 + TextWidth(
							label.substr(0, visibleOffset), kUiTextScale);
						int underlineWidth = std::max(1, TextWidth(
							label.substr(visibleOffset, 1), kUiTextScale));
						if (jpegview_linux::Terminus9CanRender(label)) {
							const jpegview_linux::BitmapGlyphInkBounds ink =
								jpegview_linux::Terminus9GlyphInkBounds(
									static_cast<unsigned char>(label[visibleOffset]));
							if (ink.width > 0) {
								underlineX += ink.left * kUiTextScale;
								underlineWidth = ink.width * kUiTextScale;
							}
						}
						const int underlineY = textY + std::max(0, TextLineHeight() - 2);
						SDL_SetRenderDrawColor(renderer_, textColor, textColor, textColor, 255);
						const SDL_Rect underline{underlineX, underlineY, underlineWidth, 1};
						SDL_RenderFillRect(renderer_, &underline);
					}
				}
				if (!shortcut.empty()) {
					const int shortcutWidth = TextWidth(shortcut, kUiTextScale);
					const Uint8 shortcutColor = item.enabled ? 175 : 90;
					DrawText(shortcut, columnX + column.width - 12 - shortcutWidth, textY, kUiTextScale,
						shortcutColor, shortcutColor, shortcutColor);
				}
				itemTop += ContextMenuRowHeight();
			}
			if (&column != &columns.back()) {
				DrawLine(columnX + column.width, menu.y + kContextMenuVerticalPadding,
					columnX + column.width, menu.y + menu.h - kContextMenuVerticalPadding,
					75, 75, 75);
			}
		}
	}

	void HandleEvents(bool& running) {
		SDL_Event event{};
		while (SDL_PollEvent(&event) != 0) {
			auto& diagnostics = jpegview_linux::PerfDiagnostics::Instance();
			jpegview_linux::PerfScopedTimer eventTimer(diagnostics,
				jpegview_linux::PerfMetric::EventHandling, event.type);
			if (diagnostics.Enabled()) {
				switch (event.type) {
				case SDL_KEYDOWN:
				case SDL_KEYUP:
				case SDL_TEXTINPUT:
				case SDL_MOUSEMOTION:
				case SDL_MOUSEBUTTONDOWN:
				case SDL_MOUSEBUTTONUP:
				case SDL_MOUSEWHEEL:
				case SDL_DROPBEGIN:
				case SDL_DROPFILE:
				case SDL_DROPCOMPLETE:
					diagnostics.MarkInput();
					break;
				default:
					break;
				}
			}
			playback_.NotifyInteraction(SDL_GetTicks());
			if (pendingMenuMnemonicTextInput_ != '\0') {
				if (event.type == SDL_KEYDOWN) {
					pendingMenuMnemonicTextInput_ = '\0';
				} else if (event.type == SDL_TEXTINPUT) {
					const char textCharacter = event.text.text[0];
					const char uppercaseMnemonic = pendingMenuMnemonicTextInput_ >= 'a' &&
						pendingMenuMnemonicTextInput_ <= 'z' ?
						pendingMenuMnemonicTextInput_ - 'a' + 'A' : pendingMenuMnemonicTextInput_;
					const bool matchingTextInput = event.text.text[1] == '\0' &&
						(textCharacter == pendingMenuMnemonicTextInput_ ||
							textCharacter == uppercaseMnemonic);
					pendingMenuMnemonicTextInput_ = '\0';
					if (matchingTextInput) continue;
				}
			}
			if (archivePasswordDialog_.IsOpen() || archivePasswordValidationPending_) {
				HandleArchivePasswordEvents(event, running);
				continue;
			}
			if (confirmationOpen_) {
				if (event.type == SDL_QUIT) running = false;
				else HandleConfirmationEvents(event);
				continue;
			}
			if (helpOpen_) {
				if (event.type == SDL_QUIT) running = false;
				else HandleHelpEvents(event);
				continue;
			}
			if (aboutOpen_) {
				if (event.type == SDL_QUIT) running = false;
				else HandleAboutEvents(event);
				continue;
			}
			if (advancedConfiguration_.IsOpen()) {
				HandleAdvancedConfigurationEvents(event, running);
				continue;
			}
			if (fileDialogOpen_) {
				HandleFileDialogEvents(event, running);
				continue;
			}
			if (batchCopyDialog_.IsOpen()) {
				HandleBatchCopyEvents(event, running);
				continue;
			}
			if (resizeDialog_.IsOpen()) {
				HandleResizeDialogEvents(event, running);
				continue;
			}
			if (cropSizeDialog_.IsOpen()) {
				HandleFixedCropSizeDialogEvents(event, running);
				continue;
			}
			if (unsharpDialogOpen_) {
				HandleUnsharpMaskDialogEvents(event, running);
				continue;
			}
			if (pictureLevelsPanelOpen_) {
				HandlePictureLevelsEvents(event, running);
				continue;
			}
			if (contextMenuOpen_) {
				switch (event.type) {
				case SDL_QUIT:
					running = false;
					break;
				case SDL_WINDOWEVENT:
					if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
						contextMenuRightKeyDown_ = false;
					}
					break;
				case SDL_KEYUP:
					if (event.key.keysym.sym == SDLK_RIGHT) {
						ReleaseContextMenuRightKey(running);
					}
					break;
				case SDL_KEYDOWN:
					if (event.key.keysym.sym == SDLK_UP) {
						MoveContextMenuSelection(-1);
					} else if (event.key.keysym.sym == SDLK_DOWN) {
						MoveContextMenuSelection(1);
					} else if (event.key.keysym.sym == SDLK_HOME) {
						menuSelected_ = jpegview_linux::NextMenuSelection(contextMenuItems_, -1, 1);
					} else if (event.key.keysym.sym == SDLK_END) {
						menuSelected_ = jpegview_linux::NextMenuSelection(contextMenuItems_,
							static_cast<int>(contextMenuItems_.size()), -1);
					} else if (event.key.keysym.sym == SDLK_LEFT) {
						MoveContextMenuSelectionAcrossColumns(-1);
					} else if (event.key.keysym.sym == SDLK_RIGHT) {
						if (event.key.repeat == 0) contextMenuRightKeyDown_ = true;
					} else if (event.key.repeat == 0 &&
						(event.key.keysym.mod & 0x0FC0u) == 0 &&
						((event.key.keysym.sym >= 'a' && event.key.keysym.sym <= 'z') ||
							(event.key.keysym.sym >= 'A' && event.key.keysym.sym <= 'Z'))) {
						const char letter = static_cast<char>(event.key.keysym.sym);
						const std::vector<int> matches =
							jpegview_linux::MenuMnemonicMatches(contextMenuItems_, letter);
						if (matches.size() == 1) {
							menuSelected_ = matches.front();
							pendingMenuMnemonicTextInput_ = letter >= 'A' && letter <= 'Z' ?
								letter - 'A' + 'a' : letter;
							ActivateContextMenuSelection(running);
						} else if (!matches.empty()) {
							menuSelected_ = jpegview_linux::NextMenuMnemonicSelection(
								contextMenuItems_, letter, menuSelected_);
						}
					} else if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_ESCAPE) {
						CloseContextMenu();
					} else if (event.key.repeat == 0 &&
						(event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE)) {
						ActivateContextMenuSelection(running);
					}
					break;
				case SDL_MOUSEMOTION:
					UpdateContextMenuSelection(event.motion.x, event.motion.y);
					break;
				case SDL_MOUSEWHEEL: {
					const int wheelSteps = std::clamp(event.wheel.y, -10, 10);
					for (int step = 0; step < std::abs(wheelSteps); ++step) {
						MoveContextMenuSelection(wheelSteps > 0 ? -1 : 1);
					}
					break;
				}
				case SDL_MOUSEBUTTONDOWN:
					if (event.button.button == SDL_BUTTON_LEFT) {
						const int item = ContextMenuItemAt(event.button.x, event.button.y);
						if (item >= 0) {
							menuSelected_ = item;
							ActivateContextMenuSelection(running);
						} else {
							CloseContextMenu();
						}
					} else {
						CloseContextMenu();
					}
					break;
				default:
					break;
				}
				continue;
			}
			switch (event.type) {
			case SDL_QUIT:
				running = false;
				break;
			case SDL_WINDOWEVENT:
				if (event.window.event == SDL_WINDOWEVENT_MAXIMIZED) {
					maximized_ = true;
				} else if (event.window.event == SDL_WINDOWEVENT_RESTORED) {
					maximized_ = false;
				} else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
					EndThumbnailPanelResize(lastMouseX_, lastMouseY_);
					EndZoomNavigatorDrag(lastMouseX_, lastMouseY_);
					if (cropMouseDragging_) {
						ClearCropSelection();
					}
					dragging_ = false;
				} else if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
					event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					interactionWorkPolicy_.NotifyActivity(
						jpegview_linux::InteractionActivity::Resize);
					if (viewport_.IsFitToWindow()) {
						FitToWindow(viewport_.FillWithCrop(), viewport_.NoEnlarge());
					} else if (image_.width > 0) {
						RefreshFitRelativeZoomBase();
					}
				}
				break;
			case SDL_KEYDOWN:
			{
				const Uint16 modifiers = event.key.keysym.mod;
				const int spaceNavigationDirection =
					jpegview_linux::SpacebarNavigationDirection(event.key, spacebarNavigatesImages_);
				const bool spaceNavigationKey = spaceNavigationDirection != 0;
				const bool shiftSpaceNavigation = spaceNavigationKey &&
					(modifiers & 0x0003u) != 0;
				const bool plainNavigationKey =
					(modifiers & 0x03C3u) == 0 &&
					(event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_RIGHT);
				const bool shiftPanKey = (modifiers & 0x03C0u) == 0 &&
					(modifiers & 0x0003u) != 0 &&
					(event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_RIGHT ||
						event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_DOWN);
				if (plainNavigationKey || spaceNavigationKey) {
					interactionWorkPolicy_.NotifyActivity(
						jpegview_linux::InteractionActivity::HeldNavigation);
				}
				// SDL marks OS key-repeat events instead of generating a fresh
				// physical press. Apply shift-pan repeats directly, but let image
				// navigation poll key state after each frame to avoid a backlog.
				if (event.key.repeat != 0 && !plainNavigationKey && !shiftPanKey &&
					!spaceNavigationKey) break;
				if (spaceNavigationKey && event.key.repeat != 0) {
					heldNavigation_.KeyDown(spaceNavigationDirection,
						event.key.keysym.scancode, true, shiftSpaceNavigation);
					break;
				}
				if (plainNavigationKey && event.key.repeat != 0) {
					const int physicalDirection = event.key.keysym.sym == SDLK_RIGHT ? 1 : -1;
					const int direction = jpegview_linux::LogicalDirectionForPhysicalKey(
						physicalDirection, mangaReadingOrderEnabled_, mangaModeInvertsLeftRight_);
					heldNavigation_.KeyDown(direction, event.key.keysym.scancode, true);
					break;
				}
				if (plainNavigationKey) {
					const int physicalDirection = event.key.keysym.sym == SDLK_RIGHT ? 1 : -1;
					const int direction = jpegview_linux::LogicalDirectionForPhysicalKey(
						physicalDirection, mangaReadingOrderEnabled_, mangaModeInvertsLeftRight_);
					heldNavigation_.KeyDown(direction, event.key.keysym.scancode, false);
				}
				if (spaceNavigationKey) {
					heldNavigation_.KeyDown(spaceNavigationDirection,
						event.key.keysym.scancode, false, shiftSpaceNavigation);
				}
				if (event.key.keysym.sym == SDLK_MENU) {
					if (cropSelection_.HasSelection()) OpenCropContextMenu();
					else OpenContextMenu();
					break;
				}
				if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_ESCAPE &&
					cropSelection_.HasSelection()) {
					ClearCropSelection();
					break;
				}
				const bool plainKey = (modifiers & 0x03C3u) == 0;
				if (plainKey && event.key.keysym.sym >= '1' && event.key.keysym.sym <= '9') {
					// CMainDlg::OnKeyDown reserves the number row for quick
					// slideshow intervals before consulting KeyMap.txt.default.
					StartSlideshow(static_cast<double>(event.key.keysym.sym - '0'));
					break;
				}
				int command = jpegview_linux::CommandForKey(event.key,
					playback_.Mode() != PlaybackMode::None || playback_.AnimationPlaying());
				if (spaceNavigationKey) {
					command = spaceNavigationDirection > 0 ? IDM_NEXT : IDM_PREV;
				}
				if (plainNavigationKey && mangaReadingOrderEnabled_ && mangaModeInvertsLeftRight_) {
					if (command == IDM_NEXT) command = IDM_PREV;
					else if (command == IDM_PREV) command = IDM_NEXT;
				}
				if (command != 0) {
					ExecuteCommand(command);
				}
				if (quitRequested_) running = false;
				if (plainNavigationKey || shiftPanKey || spaceNavigationKey) return;
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
				if (event.button.button == SDL_BUTTON_LEFT) {
					if (HandleImageInfoClick(event.button.x, event.button.y)) {
						dragging_ = false;
						break;
					}
					if (BeginThumbnailPanelResize(event.button.x, event.button.y)) {
						dragging_ = false;
						break;
					}
					if (HandleThumbnailPanelClick(event.button.x, event.button.y)) {
						dragging_ = false;
						break;
					}
					if (HandleControlClick(event.button.x, event.button.y)) {
						dragging_ = false;
						break;
					}
					if (BeginZoomNavigatorDrag(event.button.x, event.button.y)) {
						dragging_ = false;
						break;
					}
					if (BeginCropDrag(event.button.x, event.button.y)) break;
					const SDL_Rect imageArea = ImageAreaRect();
					const auto dimensions = ViewportContentDimensions();
					const jpegview_linux::ViewportRect destination = viewport_.Destination(
						dimensions.first, dimensions.second, imageArea.w, imageArea.h);
					if (destination.width <= imageArea.w && destination.height <= imageArea.h) break;
					dragging_ = true;
					lastMouseX_ = event.button.x;
					lastMouseY_ = event.button.y;
					imageCenterX_ = event.button.x;
					imageCenterY_ = event.button.y;
				} else if (event.button.button == SDL_BUTTON_RIGHT) {
					const bool shiftHeld =
						(static_cast<Uint16>(SDL_GetModState()) & 0x0003u) != 0;
					if (shiftHeld) OpenContextMenu(true);
					else if (cropSelection_.HasSelection()) OpenCropContextMenu();
					else OpenContextMenu();
				}
				break;
			case SDL_MOUSEBUTTONUP:
				if (event.button.button == SDL_BUTTON_LEFT) {
					if (zoomNavigatorDragging_) EndZoomNavigatorDrag(event.button.x, event.button.y);
					else if (cropMouseDragging_) EndCropDrag(event.button.x, event.button.y);
					else {
						EndThumbnailPanelResize(event.button.x, event.button.y);
						dragging_ = false;
					}
				}
				break;
			case SDL_MOUSEMOTION:
				UpdateThumbnailPanelCursor(event.motion.x, event.motion.y);
				UpdateCropCursor(event.motion.x, event.motion.y);
				UpdateZoomNavigatorCursor(event.motion.x, event.motion.y);
				if (thumbnailPanelResizing_) {
					ResizeThumbnailPanel(event.motion.x);
					lastMouseX_ = event.motion.x;
					lastMouseY_ = event.motion.y;
					break;
				}
				if (zoomNavigatorDragging_) {
					UpdateZoomNavigatorDrag(event.motion.xrel, event.motion.yrel);
					lastMouseX_ = event.motion.x;
					lastMouseY_ = event.motion.y;
					break;
				}
				if (cropMouseDragging_) {
					UpdateCropDrag(event.motion.x, event.motion.y);
					lastMouseX_ = event.motion.x;
					lastMouseY_ = event.motion.y;
					break;
				}
				imageCenterX_ = event.motion.x;
				imageCenterY_ = event.motion.y;
				UpdateNavigationPanelVisibility(event.motion.x, event.motion.y);
				if (dragging_) {
					PanViewport(event.motion.xrel, event.motion.yrel);
				}
				lastMouseX_ = event.motion.x;
				lastMouseY_ = event.motion.y;
				break;
			case SDL_MOUSEWHEEL:
				interactionWorkPolicy_.NotifyActivity(jpegview_linux::InteractionActivity::Wheel);
				SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
				if (event.wheel.y != 0 &&
					IsMagnifyingGlassVisibleAt(lastMouseX_, lastMouseY_)) {
					int wheelTicks = std::clamp(event.wheel.y, -10, 10);
					if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) wheelTicks = -wheelTicks;
					const Uint16 modifiers = static_cast<Uint16>(SDL_GetModState());
					const jpegview_linux::MagnifyingGlassWheelModifiers wheelModifiers{
						(modifiers & 0x00C0u) != 0, (modifiers & 0x0300u) != 0,
						(modifiers & 0x0003u) != 0};
					const SDL_Rect area = ImageAreaRect();
					const int oldWidth = magnifyingGlass_.Width();
					const int oldHeight = magnifyingGlass_.Height();
					const double oldZoomLevel = magnifyingGlass_.ZoomLevel();
					for (int tick = 0; tick < std::abs(wheelTicks); ++tick) {
						magnifyingGlass_.HandleWheel(wheelTicks > 0 ?
							jpegview_linux::MagnifyingGlassWheelDirection::Up :
							jpegview_linux::MagnifyingGlassWheelDirection::Down,
							wheelModifiers, area.w, area.h);
					}
					if (oldWidth != magnifyingGlass_.Width() ||
						oldHeight != magnifyingGlass_.Height() ||
						oldZoomLevel != magnifyingGlass_.ZoomLevel()) {
						SaveSettings();
					}
					playback_.NotifyInteraction(SDL_GetTicks());
					UpdateMagnifyingGlassCursor(lastMouseX_, lastMouseY_);
					break;
				}
				if ((SDL_GetModState() & 0x00C0u) != 0) {
					ZoomByStep(event.wheel.y > 0 ? 1 : event.wheel.y < 0 ? -1 : 0,
						lastMouseX_, lastMouseY_);
				} else if (event.wheel.y > 0) {
					PreviousImage();
				} else if (event.wheel.y < 0) {
					NextImage();
				}
				break;
			case SDL_DROPBEGIN:
				pendingDroppedFiles_.clear();
				break;
			case SDL_DROPFILE:
				if (event.drop.file != nullptr) {
					pendingDroppedFiles_.emplace_back(event.drop.file);
					SDL_free(event.drop.file);
				}
				break;
			case SDL_DROPCOMPLETE:
				OpenDroppedFiles(pendingDroppedFiles_);
				pendingDroppedFiles_.clear();
				break;
			default:
				break;
			}
		}
	}

	void RenderFrame() {
		auto& diagnostics = jpegview_linux::PerfDiagnostics::Instance();
		const std::uint64_t frameStart = diagnostics.Begin();
		RefreshDoublePageRenderState();
		const SDL_Rect imageArea = ImageAreaRect();
		imageCenterX_ = imageArea.x + imageArea.w / 2;
		imageCenterY_ = imageArea.y + imageArea.h / 2;
		const std::size_t currentIndex = fileList_.Empty() ? 0 : fileList_.CurrentIndex();
		const bool selectedImageLoaded = !fileList_.Empty() && (clipboardMode_ ||
			recentImageLoadState_.LoadedPath() == AbsoluteNormalized(fileList_.Current()));
		const bool spreadModelReady = !fileList_.Empty() &&
			doublePagePresentation_.SpreadReady(currentIndex);
		const SDL_Rect destination = CurrentPageScreenRect(imageArea);
		const int renderWidth = destination.w;
		const int renderHeight = destination.h;
		SDL_Texture* renderTexture = nullptr;
		SDL_Texture* nextPageTexture = nullptr;
		if (activeDoublePageRender_.has_value() && spreadModelReady) {
			renderTexture = ActiveDoublePageAnchorTexture();
			nextPageTexture = FindDisplayTexture(doublePagePresentation_.PartnerTextureKey());
		} else if (!activeDoublePageRender_.has_value() && selectedImageLoaded &&
			!doublePagePresentation_.SuppressSinglePage(currentIndex)) {
			renderTexture = DisplayTextureFor(renderWidth, renderHeight);
		}
		const bool spreadTexturesReady = activeDoublePageRender_.has_value() &&
			spreadModelReady && renderTexture != nullptr && nextPageTexture != nullptr;
		const bool suppressSingleImage = !selectedImageLoaded ||
			doublePagePresentation_.SuppressSinglePage(currentIndex) ||
			(activeDoublePageRender_.has_value() && !spreadTexturesReady);
		const SDL_Rect nextPageDestination = activeDoublePageRender_.has_value() ?
			NextPageScreenRect(imageArea) : SDL_Rect{};
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		SDL_SetRenderDrawColor(renderer_, 18, 18, 18, 255);
		SDL_RenderClear(renderer_);
		// Explicitly repaint the image viewport so newly exposed pillarbox and
		// letterbox margins are overwritten when navigation changes image size.
		SDL_RenderFillRect(renderer_, &imageArea);
		SDL_RenderSetClipRect(renderer_, &imageArea);
		if (spreadTexturesReady) {
			if (image_.hasTransparency) RenderTransparencyBackground(destination, imageArea);
			const auto partner = displayTextureCache_.find(doublePagePartnerDisplayKey_);
			if (partner != displayTextureCache_.end() && partner->second.hasTransparency) {
				RenderTransparencyBackground(nextPageDestination, imageArea);
			}
			if (renderTexture != nullptr) SDL_RenderCopy(renderer_, renderTexture, nullptr, &destination);
			SDL_RenderCopy(renderer_, nextPageTexture, nullptr, &nextPageDestination);
		} else if (!suppressSingleImage) {
			if (image_.hasTransparency ||
				(transitionTexture_ != nullptr && transitionImage_.hasTransparency)) {
				RenderTransparencyBackground(destination, imageArea);
			}
			RenderImageTransition(destination, imageArea, renderTexture);
		}
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		if (!suppressSingleImage) RenderCropSelection();
		RenderZoomNavigator(renderTexture, nextPageTexture);
		if (suppressSingleImage) UpdateMagnifyingGlassCursor(-1, -1);
		else UpdateMagnifyingGlassCursor(lastMouseX_, lastMouseY_);
		RenderMagnifyingGlass(renderTexture);
		SDL_RenderSetClipRect(renderer_, nullptr);
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		RenderThumbnailPanel();
		RenderFileName();
		RenderImageInfo();
		RenderZoomReadout();
		RenderControls();
		RenderPictureLevels();
		RenderUnsharpMaskDialog();
		RenderContextMenu();
		RenderFileDialog();
		RenderBatchCopy();
		RenderResizeDialog();
		RenderFixedCropSizeDialog();
		RenderAdvancedConfigurationDialog();
		RenderConfirmation();
		RenderAbout();
		RenderHelp();
		RenderArchivePasswordDialog();
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		diagnostics.End(jpegview_linux::PerfMetric::FrameBuild, frameStart);
		{
			jpegview_linux::PerfScopedTimer presentTimer(diagnostics,
				jpegview_linux::PerfMetric::Present);
			SDL_RenderPresent(renderer_);
		}
		diagnostics.RecordPresentation();
		if (spreadTexturesReady) doublePagePresentation_.MarkSpreadPresented(currentIndex);
		if (diagnostics.Enabled()) {
			const std::uint64_t sampleStart = diagnostics.Begin();
			if (sampleStart - lastPerfSnapshotUs_ >= 1000000u) {
				lastPerfSnapshotUs_ = sampleStart;
				std::uint64_t thumbnailBytes = 0;
				for (const auto& cached : thumbnailCache_) {
					if (cached.second.texture != nullptr) {
						thumbnailBytes += static_cast<std::uint64_t>(cached.second.width) *
							static_cast<std::uint64_t>(cached.second.height) * 4;
					}
				}
				const jpegview_linux::DecodedImageCacheDiagnostics decodedStats =
					imageCache_.GetDiagnostics();
				const jpegview_linux::DisplayImageCacheDiagnostics displayStats =
					displayImageCache_.GetDiagnostics();
				const jpegview_linux::ThumbnailPreparationDiagnostics thumbnailStats =
					thumbnailPreparation_.GetDiagnostics();
				diagnostics.End(jpegview_linux::PerfMetric::CacheSnapshot, sampleStart,
					cacheBudget_->Used(), cacheBudget_->Capacity(), imageCache_.CachedBytes(),
					displayImageCache_.CachedBytes(), displayTextureCacheBytes_, thumbnailBytes);
				diagnostics.RecordText(jpegview_linux::PerfMetric::CacheSnapshot,
					decodedStats.cachedBytes, decodedStats.cachedImages, decodedStats.retiredBytes,
					decodedStats.retiredImages, decodedStats.foregroundQueued,
					decodedStats.backgroundQueued, "decoded");
				diagnostics.RecordText(jpegview_linux::PerfMetric::QueueSnapshot,
					decodedStats.foregroundActive, decodedStats.backgroundActive, 0, 0, 0, 0,
					"decoded_active");
				diagnostics.RecordText(jpegview_linux::PerfMetric::CacheSnapshot,
					displayStats.cachedBytes, displayStats.preparedBytes,
					displayStats.borrowedBytes, displayStats.retiredBytes,
					displayStats.cachedImages, displayStats.preparedImages, "display");
				diagnostics.RecordText(jpegview_linux::PerfMetric::QueueSnapshot,
					displayStats.foregroundQueued, displayStats.backgroundQueued,
					displayStats.foregroundActive, displayStats.backgroundActive,
					displayStats.retiredImages, displayStats.borrowedImages, "display");
				diagnostics.RecordText(jpegview_linux::PerfMetric::CacheSnapshot,
					displayStats.borrowedBytes, displayStats.borrowedImages,
					displayStats.retiredBytes, displayStats.retiredImages,
					displayStats.activeRetiredBytes, 0, "display_lifetime");
				diagnostics.RecordText(jpegview_linux::PerfMetric::CacheSnapshot,
					thumbnailBytes, thumbnailCache_.size(), thumbnailStats.completedBytes,
					thumbnailStats.completedImages, thumbnailStats.retiredSourceBytes,
					thumbnailStats.retiredSources, "thumbnail");
				diagnostics.RecordText(jpegview_linux::PerfMetric::QueueSnapshot,
					thumbnailStats.queued, thumbnailStats.active,
					thumbnailStats.retainedSourceBytes, thumbnailStats.retiredSourceBytes,
					thumbnailStats.completedImages, thumbnailStats.retiredSources,
					"thumbnail");
				diagnostics.RecordText(jpegview_linux::PerfMetric::CacheSnapshot,
					cacheBudget_->Used(), cacheBudget_->Capacity(), displayTextureCacheBytes_,
					thumbnailBytes, displayTextureCache_.size(), thumbnailCache_.size(), "viewer");
			}
		}
	}

	void Render() {
		const bool needsCleanContextMenuFrame = contextMenuNeedsCleanFrame_;
		contextMenuNeedsCleanFrame_ = false;
		RenderFrame();
		if (needsCleanContextMenuFrame) RenderFrame();
	}

	void PresentStartupFrame() {
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		SDL_SetRenderDrawColor(renderer_, 18, 18, 18, 255);
		SDL_RenderClear(renderer_);
		SDL_RenderPresent(renderer_);
	}

	jpegview_linux::FileList fileList_;
	jpegview_linux::FileListScanWorker fileListScanWorker_;
	jpegview_linux::DisplayPrefetchPlannerWorker displayPrefetchPlannerWorker_;
	jpegview_linux::ExifMetadataWorker exifMetadataWorker_;
	jpegview_linux::InteractionWorkPolicy interactionWorkPolicy_;
	bool speculativeWorkSuspended_ = false;
	bool lastForegroundPending_ = false;
	bool prefetchRefreshNeeded_ = false;
	int pendingPrefetchDirection_ = 0;
	std::uint64_t prefetchViewportRevision_ = 0;
	std::uint64_t activeDisplayPrefetchGeneration_ = 0;
	std::uint64_t fileListScanGeneration_ = 0;
	std::optional<jpegview_linux::FileList::ScanOperation> pendingFileListScanOperation_;
	std::optional<jpegview_linux::FileList::ScanRequest> pendingDroppedScanRequest_;
	FileListScanHandling pendingFileListScanHandling_ = FileListScanHandling::None;
	int pendingFileListScanDirection_ = 0;
	bool pendingFileListScanForceImageReload_ = false;
	fs::path pendingFileListScanPreferredPath_;
	fs::path pendingMarkedToggleReturnPath_;
	DisplayModeLoadPolicy pendingFileListScanModeLoadPolicy_ = DisplayModeLoadPolicy::PreserveCurrent;
	std::string pendingFileListScanCompletionTitle_;
	bool startupImageLoadFailed_ = false;
	bool quitAfterEmptyScan_ = false;
	int deferredExitCode_ = 0;
	std::vector<std::string> startupInputs_;
	jpegview_linux::RecentFiles recentFiles_;
	jpegview_linux::RecentImageLoadState recentImageLoadState_;
	fs::path recentFilesPath_;
	std::optional<jpegview_linux::ViewportSnapshot> clipboardReturnViewport_;
	bool recentFilesLoaded_ = false;
	std::shared_ptr<jpegview_linux::SharedCacheBudget> cacheBudget_ =
		std::make_shared<jpegview_linux::SharedCacheBudget>(
			jpegview_linux::CacheBytesFromMiB(jpegview_linux::kDefaultCacheSizeMiB));
	// Declaration order is intentional: the decoder (destroyed first) may
	// schedule final display work while joining its worker during teardown.
	jpegview_linux::DisplayImageCache displayImageCache_{
		std::numeric_limits<std::size_t>::max(), 0, {}, cacheBudget_};
	jpegview_linux::DecodedImageCache imageCache_{
		std::numeric_limits<std::size_t>::max(), {}, cacheBudget_, 2};
	std::shared_ptr<DisplayPrefetchBatch> displayPrefetchBatch_;
	std::shared_ptr<CurrentJpegDimensionsMailbox> currentJpegDimensionsMailbox_ =
		std::make_shared<CurrentJpegDimensionsMailbox>();
	std::optional<PendingCurrentJpegDimensions> pendingCurrentJpegDimensions_;
	jpegview_linux::PendingImageIntents pendingImageIntents_;
	Image pendingTransitionImage_;
	std::unordered_set<jpegview_linux::SourceKey,
		jpegview_linux::SourceKeyHash> failedJpegDimensionKeys_;
	std::uint64_t currentJpegLoadGeneration_ = 0;
	bool currentJpegHeaderPending_ = false;
	bool pendingImageIntentLimitReached_ = false;
	std::uint64_t exifMetadataRequestGeneration_ = 0;
	jpegview_linux::SourceKey exifMetadataSource_;
	jpegview_linux::DeferredExifDateAction deferredExifDateAction_;
	std::uint64_t imageInfoMetadataRevision_ = 0;
	std::optional<ActiveSpreadSourceRequest> activeSpreadSourceRequest_;
	jpegview_linux::DoublePagePresentationModel doublePagePresentation_;
	bool deferredCurrentDisplayPreparation_ = false;
	std::size_t cacheSizeMiB_ = jpegview_linux::kDefaultCacheSizeMiB;
	double initialSlideshowSeconds_ = 0.0;
	jpegview_linux::PlaybackScheduler playback_;
	std::vector<Image> animationFrames_;
	jpegview_linux::WindowTitleFormatCache windowTitleFormatCache_;
	jpegview_linux::AppliedWindowTitle appliedWindowTitle_;
	jpegview_linux::ImageInfoLineCache imageInfoLineCache_;
	int transitionEffect_ = IDM_EFFECT_NONE;
	Uint32 transitionDurationMs_ = 500;
	Uint32 transitionStartTick_ = 0;
	bool startFullscreen_ = false;
	Image image_;
	Image correctionBase_;
	std::optional<jpegview_linux::PageDimensions> currentSourcePageDimensions_;
	int currentImageRotationQuarterTurns_ = 0;
	bool currentSpreadRotationValid_ = false;
	std::uint64_t modifiedImageRevision_ = 0;
	jpegview_linux::ImageProcessingParams imageProcessing_;
	jpegview_linux::ImageProcessingParams defaultImageProcessing_;
	jpegview_linux::ImageProcessingParams materializedProcessing_;
	jpegview_linux::ImageProcessingStore imageProcessingStore_;
	jpegview_linux::ImageProcessingStore pendingParameterDbRestore_;
	std::string pendingParameterDbRestoreSource_;
	bool correctionBaseValid_ = false;
	SDL_Window* window_ = nullptr;
	SDL_Renderer* renderer_ = nullptr;
	SDL_Texture* texture_ = nullptr;
	Image displayImage_;
	SDL_Texture* displayTexture_ = nullptr;
	int displayTextureWidth_ = 0;
	int displayTextureHeight_ = 0;
	std::unordered_map<std::string, DisplayTextureCacheEntry> displayTextureCache_;
	std::unordered_set<std::string> displayTextureProtectedKeys_;
	std::unordered_map<jpegview_linux::SourceKey, JpegDimensionCacheEntry,
		jpegview_linux::SourceKeyHash> jpegDimensionCache_;
	std::size_t displayTextureCacheBytes_ = 0;
	std::uint64_t displayTextureUseCounter_ = 0;
	std::uint64_t lastPerfSnapshotUs_ = 0;
	jpegview_linux::DecodedImageCache::ImagePtr currentDecoded_;
	std::size_t currentAnimationFrame_ = 0;
	std::optional<jpegview_linux::DisplayImageRequest> currentDisplayRequest_;
	bool currentPixelsMaterialized_ = false;
	bool materializedAutoContrast_ = false;
	Image transitionImage_;
	SDL_Texture* transitionTexture_ = nullptr;
	jpegview_linux::Viewport viewport_;
	jpegview_linux::CropSelectionModel cropSelection_;
	jpegview_linux::MagnifyingGlassModel magnifyingGlass_;
	std::string magnifyingGlassRequestKey_;
	std::string magnifyingGlassRequestBaseKey_;
	std::string magnifyingGlassBackgroundRequestedKey_;
	std::optional<jpegview_linux::DisplayImageRequest> magnifyingGlassRequest_;
	bool selectionModeEnabled_ = false;
	bool doublePageModeEnabled_ = false;
	bool mangaReadingOrderEnabled_ = false;
	bool mangaModeInvertsLeftRight_ = true;
	bool spacebarNavigatesImages_ = false;
	bool doublePageModeDefault_ = false;
	bool mangaReadingOrderDefault_ = false;
	std::optional<ActiveDoublePageRender> activeDoublePageRender_;
	std::string doublePagePartnerDisplayKey_;
	std::optional<jpegview_linux::DisplayImageRequest> doublePagePartnerRequest_;
	bool cropMouseDragging_ = false;
	bool zoomNavigatorDragging_ = false;
	Uint32 zoomNavigatorVisibleUntil_ = 0;
	Uint32 zoomReadoutVisibleUntil_ = 0;
	bool cropDragWasNew_ = false;
	bool cropZoomOnRelease_ = false;
	bool cropDragMoved_ = false;
	int cropDragStartX_ = 0;
	int cropDragStartY_ = 0;
	int cropAspectWidth_ = 1;
	int cropAspectHeight_ = 1;
	int cropUserAspectWidth_ = jpegview_linux::kDefaultUserCropAspectWidth;
	int cropUserAspectHeight_ = jpegview_linux::kDefaultUserCropAspectHeight;
	bool fullscreen_ = false;
	bool maximized_ = false;
	bool borderless_ = false;
	bool alwaysOnTop_ = false;
	bool dragging_ = false;
	bool controlsVisible_ = true;
	bool pictureLevelsPanelOpen_ = false;
	int levelsDraggingControl_ = -1;
	bool unsharpDialogOpen_ = false;
	int unsharpDraggingControl_ = -1;
	jpegview_linux::ImageProcessingParams unsharpOriginalProcessing_;
	double unsharpMaskRadius_ = 1.0;
	double unsharpMaskAmount_ = 0.0;
	double unsharpMaskThreshold_ = 4.0;
	double unsharpOriginalRadius_ = 1.0;
	double unsharpOriginalAmount_ = 0.0;
	double unsharpOriginalThreshold_ = 4.0;
	bool keepPictureLevels_ = false;
	bool navigationPanelEnabled_ = true;
	bool navigationPanelAutoReveal_ = true;
	bool thumbnailPanelVisible_ = false;
	bool showZoomNavigator_ = true;
	jpegview_linux::TransparencyPattern transparencyPattern_ =
		jpegview_linux::TransparencyPattern::Black;
	int thumbnailPanelWidth_ = jpegview_linux::kDefaultThumbnailPanelWidth;
	bool thumbnailPanelResizing_ = false;
	bool thumbnailPanelResizeChanged_ = false;
	int thumbnailResizeOffset_ = 0;
	SDL_Cursor* thumbnailResizeCursor_ = nullptr;
	SDL_Cursor* fileDialogResizeCursor_ = nullptr;
	SDL_Cursor* fileDialogScrollbarCursor_ = nullptr;
	SDL_Cursor* cropCrosshairCursor_ = nullptr;
	SDL_Cursor* cropMoveCursor_ = nullptr;
	SDL_Cursor* cropHorizontalCursor_ = nullptr;
	SDL_Cursor* cropVerticalCursor_ = nullptr;
	SDL_Cursor* cropDiagonalDownCursor_ = nullptr;
	SDL_Cursor* cropDiagonalUpCursor_ = nullptr;
	bool magnifyingGlassCursorActive_ = false;
	int magnifyingGlassPreviousCursorVisibility_ = kSdlCursorEnabled;
	jpegview_linux::CropSelectionHandle cropDragHandle_ = jpegview_linux::CropSelectionHandle::None;
	bool infoVisible_ = false;
	bool showHistogram_ = false;
	bool showFileName_ = false;
	jpegview_linux::HeldNavigationController heldNavigation_;
	bool confirmationOpen_ = false;
	int confirmationCommand_ = 0;
	std::string confirmationMessage_;
	bool aboutOpen_ = false;
	bool helpOpen_ = false;
	jpegview_linux::ExifInfo metadata_;
	std::string jpegComment_;
	bool contextMenuOpen_ = false;
	bool contextMenuAdvancedOptions_ = false;
	bool contextMenuCropOnly_ = false;
	bool contextMenuPositionLocked_ = false;
	bool contextMenuNeedsCleanFrame_ = false;
	bool contextMenuRightKeyDown_ = false;
	char pendingMenuMnemonicTextInput_ = '\0';
	int contextMenuX_ = 0;
	int contextMenuY_ = 0;
	int menuSelected_ = -1;
	std::vector<MenuItem> contextMenuItems_;
	std::unordered_map<jpegview_linux::SourceKey, ThumbnailCacheEntry,
		jpegview_linux::SourceKeyHash> thumbnailCache_;
	jpegview_linux::ThumbnailCacheScheduler thumbnailScheduler_;
	jpegview_linux::ThumbnailPreparationWorker thumbnailPreparation_;
	std::optional<jpegview_linux::ThumbnailPreparationResult> thumbnailUploadRetry_;
	jpegview_linux::ThumbnailCatalogRevisionTracker thumbnailCatalogRevisionTracker_;
	int thumbnailTargetWidth_ = 0;
	int thumbnailTargetHeight_ = 0;
	Uint32 thumbnailUploadRetryTick_ = 0;
	std::unordered_map<std::string, TextTextureCacheEntry> textTextureCache_;
	std::uint64_t textTextureUseCounter_ = 0;
	std::vector<jpegview_linux::OpenWithApplication> openWithApplications_;
	bool imageModified_ = false;
	bool currentPixelsDetachedFromSource_ = false;
	bool autoContrastEnabled_ = false;
	bool defaultAutoContrastEnabled_ = false;
	bool quitRequested_ = false;
	bool fileDialogOpen_ = false;
	bool fileDialogSave_ = false;
	bool fileDialogParameterBackup_ = false;
	bool fileDialogParameterRestore_ = false;
	int fileDialogX_ = 0;
	int fileDialogY_ = 0;
	int fileDialogWidth_ = jpegview_linux::kDefaultFileDialogWidth;
	int fileDialogHeight_ = jpegview_linux::kDefaultFileDialogHeight;
	double fileDialogPreviewRatio_ = 0.0;
	FileDialogDragMode fileDialogDragMode_ = FileDialogDragMode::None;
	FileDialogTab fileDialogTab_ = FileDialogTab::Browse;
	int fileDialogDragStartX_ = 0;
	int fileDialogDragStartY_ = 0;
	int fileDialogDragStartPreviewWidth_ = 0;
	SDL_Rect fileDialogDragStartRect_{};
	int fileDialogScrollbarGrabOffset_ = 0;
	bool fileDialogSaveFullSize_ = true;
	bool fileDialogOverwriteConfirmed_ = false;
	bool fileDialogLosslessCrop_ = false;
	jpegview_linux::SelectionRect fileDialogLosslessCropRect_;
	fs::path fileDialogDirectory_;
	std::string fileDialogFilename_;
	std::string fileDialogMessage_;
	jpegview_linux::FileDialogModel fileDialogModel_;
	jpegview_linux::FileDialogModel recentFileDialogModel_;
	std::vector<jpegview_linux::RecentFileRemoval> recentFileRemovalUndo_;
	jpegview_linux::ArchivePasswordDialogModel archivePasswordDialog_;
	fs::path archivePasswordDialogTarget_;
	std::string archivePasswordPendingValue_;
	bool archivePasswordValidationPending_ = false;
	jpegview_linux::DirectorySummaryLoader fileDialogSummaryLoader_;
	jpegview_linux::FileDialogFileSizeLoader fileDialogFileSizeLoader_;
	jpegview_linux::ArchiveDirectoryLoader fileDialogArchiveLoader_;
	jpegview_linux::FileDialogPreviewLoader fileDialogPreviewLoader_;
	std::unordered_map<std::string, jpegview_linux::DirectorySummary> fileDialogDirectorySummaries_;
	std::unordered_set<std::string> encryptedArchivePaths_;
	std::uint64_t fileDialogSummaryGeneration_ = 0;
	std::uint64_t fileDialogFileSizeGeneration_ = 0;
	std::uint64_t fileDialogArchiveGeneration_ = 0;
	SDL_Texture* fileDialogPreviewTexture_ = nullptr;
	int fileDialogPreviewWidth_ = 0;
	int fileDialogPreviewHeight_ = 0;
	int fileDialogPreviewSourceWidth_ = 0;
	int fileDialogPreviewSourceHeight_ = 0;
	std::uintmax_t fileDialogPreviewFileSize_ = 0;
	bool fileDialogPreviewFileSizeKnown_ = false;
	bool fileDialogPreviewHasTransparency_ = false;
	std::uint64_t fileDialogPreviewGeneration_ = 0;
	std::string fileDialogPreviewRequestKey_;
	std::string fileDialogPreviewContentKey_;
	fs::path fileDialogPreviewSource_;
	std::string fileDialogPreviewMessage_;
	std::string copyRenamePattern_;
	std::string windowTitlePattern_ = jpegview_linux::kDefaultWindowTitlePattern;
	jpegview_linux::BatchCopyDialogController batchCopyDialog_;
	jpegview_linux::ResizeDialogController resizeDialog_;
	jpegview_linux::CropSizeDialogController cropSizeDialog_;
	jpegview_linux::AdvancedConfigurationModel advancedConfiguration_;
	std::string advancedConfigurationLocation_;
	std::vector<std::string> pendingDroppedFiles_;
	std::unique_ptr<jpegview_linux::FileList> fileListBeforeClipboard_;
	fs::path clipboardTempFile_;
	fs::path clipboardTempDirectory_;
	bool clipboardMode_ = false;
	int lastMouseX_ = kDefaultWidth / 2;
	int lastMouseY_ = kDefaultHeight / 2;
	int imageCenterX_ = kDefaultWidth / 2;
	int imageCenterY_ = kDefaultHeight / 2;
};

void PrintUsage(const char* program) {
	std::cout << "Usage: " << program << " [options] [image-or-directory ...]\n\n"
		<< "Options:\n"
		<< "  --fullscreen       Start fullscreen\n"
		<< "  --slideshow N      Advance every N seconds\n"
		<< "  --decode-check     Decode inputs and exit (useful for CI)\n"
		<< "  --export-app-icon FILE  Export the embedded JPEGView icon as PNG\n"
		<< "  --version          Show the build version\n"
		<< "  --help             Show this help\n\n"
		<< "Controls: Right/Left navigate, Up/Down rotate, mouse wheel up/down navigates previous/next, Ctrl+mouse wheel zooms, left-drag pans, drop files to open,\n"
		<< "          Space toggles fit/actual by default; spacebar_navigates_images=1 maps Space/Shift+Space to next/previous, Enter fits, 0 fits,\n"
		<< "          1-9 start a slideshow, F11/F fullscreen,\n"
		<< "          F7/F8/F9 select folder/recursive/sibling navigation; folder_wrap_around=0 disables F7/list wrapping, Alt+Left/Right open the first image in adjacent sibling folders,\n"
		<< "          Ctrl+M marks an image; Ctrl+Left/Right toggles between it and the paired image,\n"
		<< "          N/M/C select display order, Z toggles the magnifying glass,\n"
		<< "          D toggles double-page mode, J reverses manga reading order,\n"
		<< "          F2 toggles picture information, Shift+N toggles the filename overlay, Ctrl+O opens, Ctrl+S saves full size, Ctrl+Shift+S saves screen size, Ctrl+R reloads, Ctrl+N toggles navigation, Ctrl+T toggles thumbnails, Ctrl+E toggles crop selection mode,\n"
		<< "          right-click or the Context Menu key opens the context menu, Esc or Q quits.\n";
}

} // namespace

int main(int argc, char** argv) {
	bool startFullscreen = false;
	double slideshowSeconds = 0.0;
	bool decodeCheck = false;
	fs::path exportAppIcon;
	std::vector<std::string> inputs;
	for (int argument = 1; argument < argc; ++argument) {
		const std::string value = argv[argument];
		if (value == "--version") {
			std::cout << "JPEGView Linux " << JPEGVIEW_APP_VERSION << '\n';
			return 0;
		}
		if (value == "--help" || value == "-h") {
			PrintUsage(argv[0]);
			return 0;
		}
		if (value == "--fullscreen" || value == "-f") {
			startFullscreen = true;
			continue;
		}
		if (value == "--slideshow") {
			if (argument + 1 >= argc) {
				std::cerr << "--slideshow requires a positive duration in seconds.\n";
				return 2;
			}
			try {
				const std::string duration = argv[++argument];
				std::size_t parsedCharacters = 0;
				const double parsedDuration = std::stod(duration, &parsedCharacters);
				if (parsedCharacters != duration.size() || !std::isfinite(parsedDuration) || parsedDuration <= 0.0) {
					throw std::invalid_argument("invalid slideshow duration");
				}
				slideshowSeconds = std::max(0.1, parsedDuration);
			} catch (const std::exception&) {
				std::cerr << "--slideshow requires a positive duration in seconds.\n";
				return 2;
			}
			continue;
		}
		if (value == "--decode-check") {
			decodeCheck = true;
			continue;
		}
		if (value == "--export-app-icon") {
			if (argument + 1 >= argc) {
				std::cerr << "--export-app-icon requires an output filename.\n";
				return 2;
			}
			exportAppIcon = argv[++argument];
			continue;
		}
		if (!value.empty() && value[0] == '-') {
			std::cerr << "Unknown option: " << value << '\n';
			PrintUsage(argv[0]);
			return 2;
		}
		inputs.push_back(value);
	}

	if (!exportAppIcon.empty()) {
		jpegview_linux::ApplicationIcon icon;
		std::string errorMessage;
		if (!jpegview_linux::DecodeApplicationIcon(icon, errorMessage)) {
			std::cerr << "Cannot decode embedded application icon: " << errorMessage << '\n';
			return 1;
		}
		jpegview_linux::ImageWriteOptions options;
		if (!jpegview_linux::WriteImage(exportAppIcon, icon.bgra.data(), icon.width,
			icon.height, options, errorMessage)) {
			std::cerr << "Cannot export application icon: " << errorMessage << '\n';
			return 1;
		}
		return 0;
	}

	if (decodeCheck) {
		jpegview_linux::FileList fileList(inputs);
		if (fileList.Empty()) {
			std::cerr << "No supported images found.\n";
			return 2;
		}
		for (const fs::path& file : fileList.Files()) {
			jpegview_linux::DecodedImage decoded;
			std::string errorMessage;
			if (!jpegview_linux::DecodeImage(file, decoded, errorMessage) || decoded.frames.empty()) {
				std::cerr << file << ": " << errorMessage << '\n';
				return 1;
			}
			const jpegview_linux::DecodedFrame& firstFrame = decoded.frames.front();
			std::cout << file << ": " << firstFrame.width << 'x' << firstFrame.height;
			if (decoded.frames.size() > 1) {
				std::cout << " (" << decoded.frames.size() << " frames";
				if (decoded.animation) std::cout << ", animation";
				std::cout << ')';
			}
			std::cout << '\n';
		}
		return 0;
	}

	Viewer viewer(std::move(inputs), slideshowSeconds, startFullscreen);
	const int result = viewer.Run();
	return result;
}
