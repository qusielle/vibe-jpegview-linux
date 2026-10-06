#include "sdl_abi.h"
#include "file_list.h"
#include "file_list_scan_worker.h"
#include "file_list_sort_worker.h"
#include "display_prefetch_planner.h"
#include "display_preparation_controller.h"
#include "display_upload_scheduler.h"
#include "exif_metadata_worker.h"
#include "pending_image_intents.h"
#include "double_page_model.h"
#include "presentation_controller.h"
#include "archive_source.h"
#include "exif_reader.h"
#include "clipboard.h"
#include "image_writer.h"
#include "image_decoder.h"
#include "image_cache.h"
#include "display_image_cache.h"
#include "display_texture_pins.h"
#include "cache_policy.h"
#include "image.h"
#include "image_processing.h"
#include "image_processing_store.h"
#include "image_session_controller.h"
#include "settings.h"
#include "advanced_configuration_model.h"
#include "sort_mode.h"
#include "desktop_applications.h"
#include "external_commands.h"
#include "gps_map_action.h"
#include "external_process.h"
#include "file_operation_service.h"
#include "batch_copy.h"
#include "image_formats.h"
#include "input_commands.h"
#include "viewport.h"
#include "resize_model.h"
#include "crop_selection_model.h"
#include "crop_size_dialog_model.h"
#include "go_to_image_number_model.h"
#include "pixel_color_sampler.h"
#include "zoom_navigator_model.h"
#include "magnifying_glass_model.h"
#include "context_menu_model.h"
#include "overlay_layout.h"
#include "viewer_chrome.h"
#include "playback_scheduler.h"
#include "thumbnail_panel_model.h"
#include "thumbnail_repository.h"
#include "thumbnail_resampler.h"
#include "interaction_work_policy.h"
#include "source_work_coordinator.h"
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
#include "event_loop_model.h"
#include "modal_event_router.h"
#include "renderer_thread_resource.h"
#include "text_renderer.h"
#include "chrome_renderer.h"
#include "context_menu_renderer.h"
#include "file_dialog_renderer.h"
#include "editing_dialog_renderer.h"
#include "renderer_texture_owner.h"
#include "image_spectrum_worker.h"
#include "image_document.h"
#include "image_operation_worker.h"

// Keep Linux command dispatch aligned with the original Windows application.
// resource.h is deliberately platform-neutral: it contains the command IDs
// shared by JPEGView.rc, CMainDlg::ExecuteCommand, and KeyMap.txt.default.
#include "../../src/JPEGView/resource.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <dlfcn.h>
#include <deque>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
constexpr std::size_t kMaximumPendingTextureUploads = 4;
constexpr std::size_t kMaximumSynchronousDisplayTextureUploadBytes = 8u * 1024u * 1024u;
constexpr std::size_t kDisplayTextureUploadBandBytes = 4u * 1024u * 1024u;
constexpr std::size_t kDisplayTextureRetirementsPerTick = 1;
constexpr std::size_t kDisplayTextureRetirementInteractionPressureThreshold =
	kMaximumPendingTextureUploads;
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

bool SameResolvedPath(const fs::path& left, const fs::path& right) {
	if (left.empty() || right.empty()) return false;
	std::error_code leftError;
	std::error_code rightError;
	const fs::path resolvedLeft = fs::weakly_canonical(left, leftError);
	const fs::path resolvedRight = fs::weakly_canonical(right, rightError);
	return !leftError && !rightError && resolvedLeft == resolvedRight;
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

bool HasExecutable(const std::string& executable) {
	return jpegview_linux::ExternalCommandAvailable(executable);
}

class Viewer : private jpegview_linux::RendererWindowResources<SDL_Window, SDL_Renderer,
	SDL_DestroyWindow, SDL_DestroyRenderer> {
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
		completionWakeEventType_ = SDL_RegisterEvents(1);
		if (completionWakeEventType_ != std::numeric_limits<Uint32>::max()) {
			const Uint32 eventType = completionWakeEventType_;
			jpegview_linux::UiCompletionWakeup().SetPostFunction([eventType] {
				SDL_Event event{};
				event.type = eventType;
				return SDL_PushEvent(&event) == 1;
			});
		}
		// Keep the window hidden while SDL and the window manager apply the
		// initial state.  Showing it first makes a restored maximized window
		// visibly appear in its normal size before it is maximized.
		Uint32 windowFlags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
		if (runtimeSettings_.Values().maximized && !startFullscreen_) windowFlags |= SDL_WINDOW_MAXIMIZED;
		window_ = SDL_CreateWindow("JPEGView — Loading", 0x2FFF0000, 0x2FFF0000,
			kDefaultWidth, kDefaultHeight, windowFlags);
		if (window_ == nullptr) {
			std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n';
			jpegview_linux::UiCompletionWakeup().SetPostFunction({});
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
				window_.Reset();
				jpegview_linux::UiCompletionWakeup().SetPostFunction({});
				SDL_Quit();
				return 1;
			}
		}
		textRenderer_.SetRenderer(renderer_);
		textRenderer_.SetFont(UiFont());
		chromeRenderer_.SetRenderer(renderer_);
		fileDialogRenderer_.SetRenderer(renderer_);
		editingDialogRenderer_.SetRenderer(renderer_);
		imageTextureOwner_.SetRenderer(renderer_);
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
		} else if (runtimeSettings_.Values().maximized) {
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
				ImageLoadStatePolicy::UseDefaults);
			startupInputs_.clear();
			if (!fileList_.Empty() && !LoadCurrent(0,
				ImageLoadStatePolicy::UseDefaults, false, true)) {
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
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::WindowExposure);
		while (running) {
			const int waitTimeoutMs = frameInvalidator_.NeedsRender() ? 0 :
				NextEventWaitTimeoutMs(SDL_GetTicks());
			HandleEvents(running, waitTimeoutMs);
			if (jpegview_linux::UiCompletionWakeup().Pending()) {
				jpegview_linux::UiCompletionWakeup().Consume();
			}
			if (quitRequested_) running = false;
			TickPlayback();
			UpdateInteractionWorkPolicy();
			TickFileListScan();
			TickFileListSort();
			TickFileDialogDirectoryListing();
			TickFileDialogArchiveDirectory();
			TickFileDialogDirectorySummaries();
			TickFileDialogFileSizes();
			UpdateFileDialogPreview();
			ApplyWorkerDetectedSourceChanges();
			TickDisplayPrefetchPlanner();
			TickDisplayPreparationRequests();
			TickExifMetadata();
			TickCurrentJpegDimensions();
			TickCurrentSelectedDecode();
			TickImageOperation();
			TickFileOperation();
			TickImageSpectrum();
			TickActiveSpreadDimensions();
			TickTimedPresentationEffects(SDL_GetTicks());
			if (frameInvalidator_.NeedsRender()) {
				UpdatePresentationState();
				UpdatePixelColorSampler();
				TouchVisibleThumbnailRows();
				const PresentedFrame presented = Render();
				frameInvalidator_.Consume();
				if (presentationController_.AcknowledgeFramePresented(
					presented.selectedIndex, presented.spreadTexturesReady)) {
					// Acknowledgement unlocks queued navigation after a complete pair
					// has actually reached SDL_RenderPresent.
				}
			}
			// Present the current image before doing renderer-thread cache uploads.
			// Held navigation then advances only after the closest ready neighbor
			// has had an opportunity to become a retained SDL texture.
			UpdateInteractionWorkPolicy();
			TickRendererTextureRetirement();
			TickDisplayTexturePreload();
			TickThumbnailPreload(heldNavigation_.Scancode() < 0 &&
				!displayImageCache_.HasPendingWork());
			const fs::path selectedBeforeHeldNavigation = fileList_.Current();
			TickHeldNavigation();
			if (selectedBeforeHeldNavigation != fileList_.Current()) {
				frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
			}
			RecordPeriodicPerformanceSnapshots();
		}

		Cleanup();
		return deferredExitCode_;
	}

private:
	enum class ImageLoadStatePolicy {
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

	enum class NavigationAttemptResult {
		Moved,
		WrappedToSameImage,
		PendingDirectoryScan,
		Blocked,
	};

	enum class ImageOperationPurpose {
		Materialize,
		Transform,
		Crop,
		Resize,
		Reprocess,
		Save,
		CopyImage,
		CopySelection,
		Print,
		Wallpaper,
	};

	struct PendingImageOperation {
		ImageOperationPurpose purpose = ImageOperationPurpose::Materialize;
		jpegview_linux::ImageOperationSpec operation;
		jpegview_linux::ImageDocumentSnapshot document;
		std::uint64_t workerGeneration = 0;
		int transformCommand = 0;
		bool fullSize = true;
		bool keepSpreadRotation = false;
		bool pausedAnimationPlayback = false;
		bool overwriteConfirmed = false;
		fs::path output;
	};

	struct PendingFileOperationUi {
		std::uint64_t id = 0;
		std::uint64_t ownerGeneration = 0;
		jpegview_linux::FileOperationKind kind =
			jpegview_linux::FileOperationKind::SaveImage;
		fs::path ownerPath;
		fs::path sourcePath;
		fs::path outputPath;
		fs::path directoryPath;
		std::string successTitle;
		std::string failurePrefix;
		bool selectionSensitive = true;
		bool cropDialogSave = false;
		bool batchDialog = false;
		bool inPlaceSave = false;
		bool flattenAnimationOnSuccess = false;
		bool resumeAnimationOnCompletion = false;
	};

	struct TemporaryCleanupRequest {
		fs::path file;
		fs::path directory;
	};

	using ContextMenuColumn = jpegview_linux::ContextMenuColumnLayout;

	struct PresentedFrame {
		std::size_t selectedIndex = 0;
		bool spreadTexturesReady = false;
	};

	struct ThumbnailTextureEntry {
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
		jpegview_linux::SourceKey source;
		jpegview_linux::DisplayImageCacheKey cacheKey;
		int width = 0;
		int height = 0;
		bool hasTransparency = false;
		std::shared_ptr<const jpegview_linux::GrayscaleSpectrum> spectrum;
		bool activeWorking = false;
		jpegview_linux::CacheProtectionTier protection =
			jpegview_linux::CacheProtectionTier::DistantSpeculation;
		jpegview_linux::CacheReservation reservation;
		std::list<std::string>::iterator lru;
	};

	struct RetiredDisplayTexture {
		SDL_Texture* texture = nullptr;
		std::size_t bytes = 0;
		jpegview_linux::PerfWorkClass workClass =
			jpegview_linux::PerfWorkClass::Unspecified;
		jpegview_linux::CacheReservation reservation;
		bool countsTowardSpeculativeLimit = false;
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
		std::uint64_t generation = 0;
		std::shared_ptr<jpegview_linux::DisplayPreparationRequestChannel> completionChannel;
		jpegview_linux::DisplayPrefetchBatchOwner owner =
			jpegview_linux::DisplayPrefetchBatchOwner::NeighborPlanner;
		jpegview_linux::WorkBatchGate gate;
		std::unordered_map<std::string, jpegview_linux::PageDimensions> decodedPageDimensions;
		bool pageDimensionsChanged = false;
		std::unordered_map<std::string, std::pair<jpegview_linux::SourceKey,
			jpegview_linux::DecodedImageCache::ImagePtr>> decodedImages;
		std::unordered_set<std::string> failedDecodeFilenames;
		std::vector<jpegview_linux::RetainedDisplayTexture> retainedTextures;
		std::vector<std::string> protectedTextureKeys;
		std::vector<jpegview_linux::DisplayImageCacheKey> protectedTextureCacheKeys;
		std::unordered_map<std::string, std::size_t> priorityByFilename;
		std::unordered_map<std::string, std::size_t> indexByFilename;
		std::unordered_map<std::string, jpegview_linux::SourceDescriptor> sourceByFilename;
		std::unordered_map<std::string, jpegview_linux::ImageProcessingParams> processingByFilename;
		std::unordered_map<std::string, bool> autoContrastByFilename;
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

	struct PendingTextureUpload {
		jpegview_linux::DisplayImageCache::ImagePtr image;
		jpegview_linux::DisplayUploadPriority priority;
		std::uint64_t lastAttemptRetainedCapacityRevision = 0;
		bool attempted = false;
		bool uploadStaged = false;
		std::uint64_t selectionGeneration = 0;
		SDL_Texture* incompleteTexture = nullptr;
		jpegview_linux::DisplayTextureUploadPlan uploadPlan;
		jpegview_linux::CacheReservation textureReservation;
		jpegview_linux::CacheProtectionTier textureProtection =
			jpegview_linux::CacheProtectionTier::DistantSpeculation;
		bool textureActiveWorking = false;
		bool countsTowardSpeculativeLimit = false;
		bool thumbnailQueued = false;
	};

	struct TransitionFrame {
		SDL_Texture* texture = nullptr;
		std::string displayKey;
		int width = 0;
		int height = 0;
		bool hasTransparency = false;
		bool ownsTexture = false;
	};

	enum class TextureCacheOutcome {
		Cached,
		Deferred,
		NotRetained,
		Failed,
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
		jpegview_linux::UiCompletionWakeup().SetPostFunction({});
		fileOperationService_.Stop();
		imageSpectrumWorker_.Stop();
		fileDialogPreviewLoader_.Shutdown();
		fileDialogDirectoryLoader_.Shutdown();
		fileDialogArchiveLoader_.Shutdown();
		fileDialogFileSizeLoader_.Shutdown();
		fileDialogSummaryLoader_.Shutdown();
		CancelPendingImageOperation();
		jpegview_linux::RetiredImageBuffers retiredDocument = imageDocument_.ClearPixels();
		retiredDocument.decoded = std::move(currentDecoded_);
		imageOperationWorker_.Retire(std::move(retiredDocument));
		imageOperationWorker_.Stop();
		DeactivateDisplayPrefetchBatch();
		displayPreparationController_.Shutdown();
		exifMetadataWorker_.Stop();
		thumbnailPreparation_.Clear();
		{
			std::lock_guard<std::mutex> lock(currentJpegDimensionsMailbox_->mutex);
			currentJpegDimensionsMailbox_->active = false;
			currentJpegDimensionsMailbox_->ready.clear();
		}
		selectedSourceDecodeChannel_->Shutdown();
		CancelPendingCurrentJpegDimensions();
		fileListScanWorker_.Stop();
		fileListSortWorker_.Clear();
		std::error_code temporaryCleanupError;
		if (!clipboardTempFile_.empty()) fs::remove(clipboardTempFile_, temporaryCleanupError);
		if (!clipboardTempDirectory_.empty()) {
			fs::remove_all(clipboardTempDirectory_, temporaryCleanupError);
		}
		for (const TemporaryCleanupRequest& cleanup : pendingTemporaryCleanups_) {
			if (!cleanup.file.empty()) fs::remove(cleanup.file, temporaryCleanupError);
			if (!cleanup.directory.empty()) fs::remove_all(cleanup.directory,
				temporaryCleanupError);
		}
		pendingTemporaryCleanups_.clear();
		SaveSettings();
		if (magnifyingGlassCursorActive_) {
			SDL_ShowCursor(magnifyingGlassPreviousCursorVisibility_);
			magnifyingGlassCursorActive_ = false;
		}
		if (recentFilesLoaded_) {
			if (!clipboardMode_ && !fileList_.Empty() &&
				imageSession_.OwnsLoadedPath(fileList_.Current())) {
				recentFiles_.RememberViewport(imageSession_.LoadedPath(),
					imageSession_.ClipboardReturnViewport().value_or(viewport_.Snapshot()));
				recentFiles_.RememberDoublePageMode(imageSession_.LoadedPath(),
					{doublePageModeEnabled_, mangaReadingOrderEnabled_});
			}
			if (!recentFilesPath_.empty() &&
				!jpegview_linux::SaveRecentFiles(recentFilesPath_, recentFiles_)) {
				std::cerr << "Could not save recent-file history to " << recentFilesPath_ << '\n';
			}
		}
		fileListSortWorker_.Retire(
			std::make_shared<jpegview_linux::FileList>(std::move(fileList_)));
		fileListSortWorker_.Stop();
		ClearFileDialogPreview();
		if (clipboardMode_) {
			std::error_code removeError;
			fs::remove(clipboardTempFile_, removeError);
			if (!clipboardTempDirectory_.empty()) fs::remove(clipboardTempDirectory_, removeError);
		}
		if (texture_ != nullptr) {
			DestroyTextureMeasured(texture_);
			texture_ = nullptr;
		}
		ClearDisplayTexture();
		ClearTransition();
		ClearDisplayTextureCache();
		displayImageCache_.Shutdown();
		imageCache_.Clear();
		imageCache_.Shutdown();
		ClearThumbnailCache();
		thumbnailPreparation_.Shutdown();
		ClearTextTextureCache();
		DrainRetiredDisplayTextures();
		const std::size_t orphanedImageTextures = imageTextureOwner_.DestroyAll();
		if (orphanedImageTextures != 0) {
			std::cerr << "RendererTextureOwner reclaimed " << orphanedImageTextures
				<< " unreferenced image textures during shutdown\n";
		}
		imageTextureOwner_.SetRenderer(nullptr);
		chromeRenderer_.SetRenderer(nullptr);
		fileDialogRenderer_.SetRenderer(nullptr);
		editingDialogRenderer_.SetRenderer(nullptr);
		textRenderer_.SetRenderer(nullptr);
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
		RendererWindowResources::Reset();
		SDL_Quit();
	}

	void LoadSettings() {
		jpegview_linux::LoadImageProcessingStore(
			jpegview_linux::ImageProcessingStorePath(), imageProcessingStore_);
		const fs::path settingsPath = jpegview_linux::ViewerSettingsPath();
		if (settingsPath.empty() || !runtimeSettings_.Load(settingsPath)) return;
		const jpegview_linux::ViewerSettings& settings = runtimeSettings_.Values();
		autoContrastEnabled_ = settings.autoContrast;
		jpegview_linux::FileList::SortMode sortMode;
		if (jpegview_linux::ParseSortMode(settings.sortMode, sortMode)) {
			fileList_.SetSorting(sortMode, settings.sortAscending);
		}
		fileList_.SetWrapAroundFolder(settings.folderWrapAround);

		viewport_.SetFitRelativeZoomMode(settings.fitRelativeZoomMode);
		doublePageModeEnabled_ = settings.doublePageModeEnabled;
		mangaReadingOrderEnabled_ = settings.mangaReadingOrderEnabled;
		const SDL_Rect magnifyingGlassArea = ImageAreaRect();
		magnifyingGlass_.SetParameters(settings.magnifyingGlassWidth,
			settings.magnifyingGlassHeight, settings.magnifyingGlassZoomLevel,
			magnifyingGlassArea.w, magnifyingGlassArea.h);
		cropSelection_.SetFixedSize(settings.fixedCropWidth, settings.fixedCropHeight,
			settings.fixedCropScreenPixels);
		cropSelection_.SetMode(jpegview_linux::CropSelectionMode::Free);
		cacheBudget_->SetCapacity(jpegview_linux::CacheBytesFromMiB(runtimeSettings_.Values().cacheSizeMiB));
		viewport_.LoadScaleMode(settings.scaleMode, settings.manualZoomSet, settings.manualZoom);
	}

	jpegview_linux::ViewerSettings CurrentViewerSettings() const {
		jpegview_linux::ViewerSettings settings = runtimeSettings_.Values();
		settings.scaleMode = viewport_.NavigationScaleMode();
		settings.sortMode = jpegview_linux::SortModeSettingName(fileList_.GetSorting());
		settings.sortAscending = fileList_.IsSortedAscending();
		settings.manualZoom = viewport_.NavigationZoom();
		settings.folderWrapAround = fileList_.WrapAroundFolder();
		settings.fitRelativeZoomMode = viewport_.FitRelativeZoomMode();
		settings.magnifyingGlassWidth = magnifyingGlass_.Width();
		settings.magnifyingGlassHeight = magnifyingGlass_.Height();
		settings.magnifyingGlassZoomLevel = magnifyingGlass_.ZoomLevel();
		settings.fixedCropWidth = cropSelection_.FixedWidth();
		settings.fixedCropHeight = cropSelection_.FixedHeight();
		settings.fixedCropScreenPixels = cropSelection_.FixedSizeUsesScreenPixels();
		return settings;
	}

	bool SaveSettings() {
		const fs::path settingsPath = jpegview_linux::ViewerSettingsPath();
		if (settingsPath.empty()) return false;
		return runtimeSettings_.SaveAndAdopt(settingsPath, CurrentViewerSettings());
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
		imageSession_.CancelPendingLoad();
		pendingImageIntents_.Cancel();
		ClearPendingTransitionFrame();
		if (pendingCurrentDecodedSource_.has_value()) {
			imageCache_.CancelActiveSpreadRequest(
				*pendingCurrentDecodedSource_, false);
			pendingCurrentDecodedSource_.reset();
		}
		if (pendingCurrentJpegDimensions_.has_value()) {
			imageCache_.CancelActiveSpreadRequest(
				pendingCurrentJpegDimensions_->source, true);
			pendingCurrentJpegDimensions_.reset();
		}
		currentJpegHeaderPending_ = false;
		currentSelectedLoadPending_ = false;
		currentSelectedStartupLoad_ = false;
		pendingMaterializationIntents_.clear();
		pendingSelectedDisplayKey_.clear();
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
				bool published = false;
				try {
					std::lock_guard<std::mutex> lock(mailbox->mutex);
					if (!mailbox->active) return;
					mailbox->ready.push_back(CurrentJpegDimensionsResult{
						loadGeneration, sourceKey, succeeded, width, height});
					published = true;
				} catch (...) {
					return;
				}
				if (published) jpegview_linux::UiCompletionWakeup().Notify();
			}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	}

	void RequestSelectedSourceDecode(
		const jpegview_linux::SourceDescriptor& source, std::uint64_t loadGeneration) {
		if (pendingCurrentDecodedSource_.has_value() &&
			pendingCurrentDecodedSource_->Key() == source.Key()) return;
		pendingCurrentDecodedSource_ = source;
		if (currentSelectedLoadPending_) {
			(void)imageSession_.SetStage(loadGeneration, source.Key(),
				jpegview_linux::ImageSessionStage::AwaitingDecodedSource);
		}
		const std::shared_ptr<jpegview_linux::SelectedSourceDecodeChannel> channel =
			selectedSourceDecodeChannel_;
		imageCache_.RequestSelectedSource(source,
			[channel, loadGeneration](const jpegview_linux::SourceDescriptor& completedSource,
				const jpegview_linux::DecodedImageCache::ImagePtr& image,
				const jpegview_linux::WorkerFailure& failure) {
			(void)channel->Publish({loadGeneration, completedSource, image, failure});
			}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	}

	bool IsCurrentSelectedDisplayRequest(std::uint64_t generation,
		const jpegview_linux::SourceKey& source, const std::string& key) const {
		if (!imageSession_.MatchesSelection(generation, source) ||
			imageSession_.Stage() == jpegview_linux::ImageSessionStage::Failed ||
			!currentDisplayRequest_.has_value() ||
			currentDisplayRequest_->source.Key() != source ||
			currentDisplayRequest_->key != key) return false;
		return !currentSelectedLoadPending_ || pendingSelectedDisplayKey_.empty() ||
			pendingSelectedDisplayKey_ == key;
	}

	void FailCurrentSelectedLoad(std::uint64_t generation,
		const jpegview_linux::SourceKey& source, const std::string& errorMessage,
		bool passwordFailure) {
		if (!imageSession_.MatchesSelection(generation, source) || fileList_.Empty()) return;
		const fs::path path = AbsoluteNormalized(fileList_.Current());
		const bool startupLoad = currentSelectedStartupLoad_;
		(void)imageSession_.FailSelectedLoad(generation, source, path);
		currentSelectedLoadPending_ = false;
		currentSelectedStartupLoad_ = false;
		currentJpegHeaderPending_ = false;
		pendingSelectedDisplayKey_.clear();
		pendingCurrentDecodedSource_.reset();
		pendingImageIntents_.Cancel();
		ClearPendingTransitionFrame();
		pendingMaterializationIntents_.clear();
		currentDisplayRequest_.reset();
		CancelPendingImageOperation();
		CancelImageSpectrumBeforeImageMutation();
		jpegview_linux::RetiredImageBuffers retiredDocument = imageDocument_.ClearPixels();
		retiredDocument.decoded = std::move(currentDecoded_);
		imageOperationWorker_.Retire(std::move(retiredDocument));
		currentSourcePageDimensions_.reset();
		playback_.SetImageReady(false, SDL_GetTicks());
		if (passwordFailure) {
			if (errorMessage.find("incorrect archive password") != std::string::npos) {
				jpegview_linux::ForgetSessionArchivePassword(fileList_.Current());
			}
			SetTitle("JPEGView — Open image");
			OpenFileDialog(fileList_.Current().parent_path());
			return;
		}
		SetTitle(fileList_.Current().filename().string() + " — decode failed: " + errorMessage);
		std::cerr << fileList_.Current() << ": " << errorMessage << '\n';
		if (startupLoad) {
			startupImageLoadFailed_ = true;
			deferredExitCode_ = 1;
			quitRequested_ = true;
		}
	}

	void MarkSelectedDisplayFrameReady(std::uint64_t generation,
		const jpegview_linux::SourceKey& source, const std::string& key) {
		const bool currentRequest = IsCurrentSelectedDisplayRequest(generation, source, key);
		if (!currentRequest) return;
		if (failedCurrentDisplayKey_ == key) failedCurrentDisplayKey_.clear();
		RecordPresentedDisplayTexture(key);
		if (!currentSelectedLoadPending_) {
			playback_.SetImageReady(true, SDL_GetTicks());
			return;
		}
		const std::optional<jpegview_linux::ImageSessionSelection> selection =
			imageSession_.Selection();
		if (!selection.has_value() || selection->filename !=
			AbsoluteNormalized(fileList_.Current()) ||
			!imageSession_.MarkDisplayFrameReady(generation, source)) return;
		if (!imageSession_.CommitSelectedLoad(generation, source,
			selection->filename, recentFiles_)) {
			FailCurrentSelectedLoad(generation, source,
				"selected image history could not be committed", false);
			return;
		}
		imageSession_.ClearClipboardReturnViewport();
		currentSelectedLoadPending_ = false;
		currentSelectedStartupLoad_ = false;
		currentJpegHeaderPending_ = false;
		pendingSelectedDisplayKey_.clear();
		const std::optional<jpegview_linux::PendingImageIntentBatch> laterIntents =
			pendingImageIntents_.Take(source, generation);
		if (laterIntents.has_value()) {
			pendingMaterializationIntents_.insert(pendingMaterializationIntents_.end(),
				laterIntents->actions.begin(), laterIntents->actions.end());
		}
		const jpegview_linux::ExifDateActionCompletion deferredAction =
			deferredExifDateAction_.MarkImageCommitted(selection->filename, source);
		if (deferredAction.runDeferredAction) TouchCurrentImage(true);
		playback_.SetImageReady(true, SDL_GetTicks());
		if (pendingTransitionFrame_.texture != nullptr) {
			TransitionFrame previous;
			MoveTransitionFrame(previous, pendingTransitionFrame_);
			StartTransition(previous);
		}
		RefreshDoublePageRenderState();
		SetTitle();
		if (pendingImageOperation_.has_value()) {
			(void)StartPendingImageOperation();
		}
		if (!pendingImageOperation_.has_value() && !pendingMaterializationIntents_.empty()) {
			ReplayPendingMaterializationIntents();
		}
	}

	void HandleCurrentDisplayFailure(std::uint64_t generation,
		const jpegview_linux::SourceKey& source, const std::string& key,
		const std::string& errorMessage) {
		if (!currentDisplayRequest_.has_value() ||
			currentDisplayRequest_->key != key ||
			currentDisplayRequest_->source.Key() != source ||
			!imageSession_.MatchesSelection(generation, source)) return;
		if (currentSelectedLoadPending_) {
			if (generation == 0 || !IsCurrentSelectedDisplayRequest(
				generation, source, key)) return;
			const bool passwordFailure =
				jpegview_linux::IsArchiveMemberLocation(fileList_.Current()) &&
				(errorMessage.find("password required") != std::string::npos ||
					errorMessage.find("incorrect archive password") != std::string::npos);
			FailCurrentSelectedLoad(generation, source, errorMessage, passwordFailure);
			return;
		}
		if (fileList_.Empty()) return;
		failedCurrentDisplayKey_ = key;
		if (currentDecoded_ && currentDecoded_->animation) {
			playback_.FrameDisplayFailed();
		} else {
			playback_.SetImageReady(true, SDL_GetTicks());
		}
		SetTitle(fileList_.Current().filename().string() +
			" — display update failed: " + errorMessage);
	}

	void RequestCurrentDisplayFrame() {
		if (fileList_.Empty() || CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
		const SDL_Rect area = ImageAreaRect();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			CurrentImage().width, CurrentImage().height, area.w, area.h);
		const jpegview_linux::DisplayImageRequest* request =
			CurrentDisplayRequest(destination.width, destination.height);
		if (request == nullptr) return;
		const std::uint64_t generation = imageSession_.Generation();
		const jpegview_linux::SourceKey source = request->source.Key();
		if (currentSelectedLoadPending_) {
			pendingSelectedDisplayKey_ = request->key;
			(void)imageSession_.SetStage(generation, source,
				jpegview_linux::ImageSessionStage::AwaitingDisplayFrame);
		}
		SetDisplayTextureProtection(request->key,
			jpegview_linux::CacheProtectionTier::Active);
		if (FindDisplayTexture(request->key) != nullptr) {
			CaptureCachedDisplaySpectrum(request->key);
			MarkSelectedDisplayFrameReady(generation, source, request->key);
			return;
		}
		const auto prepared = displayImageCache_.Find(*request);
		if (!prepared) {
			displayImageCache_.Request(*request);
			return;
		}
		CapturePreparedSourceSpectrum(*prepared);
		PendingTextureUpload pending;
		pending.image = prepared;
		pending.priority = {request->workClass, request->priority};
		pending.lastAttemptRetainedCapacityRevision =
			cacheBudget_->RetainedCapacityRevision();
		pending.selectionGeneration = currentSelectedLoadPending_ ? generation : 0;
		QueuePendingTextureUpload(std::move(pending));
	}

	void TickCurrentSelectedDecode() {
		if (fileList_.Empty()) return;
		const jpegview_linux::SourceDescriptor selectedSource =
			SourceDescriptorForPath(fileList_.Current());
		const std::uint64_t generation = imageSession_.Generation();
		auto result = selectedSourceDecodeChannel_->Take(generation,
			selectedSource.Key());
		if (!result.has_value() || !imageSession_.MatchesSelection(
			result->generation, result->source.Key())) return;
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		pendingCurrentDecodedSource_.reset();
		if (!result->image || result->image->frames.empty() || result->failure.Failed()) {
			const std::string errorMessage = result->failure.message.empty() ?
				"image decoding failed" : result->failure.message;
			const bool passwordFailure =
				jpegview_linux::IsArchiveMemberLocation(fileList_.Current()) &&
				(errorMessage.find("password required") != std::string::npos ||
					errorMessage.find("incorrect archive password") != std::string::npos);
			if (currentSelectedLoadPending_) {
				FailCurrentSelectedLoad(generation, result->source.Key(),
					errorMessage, passwordFailure);
			} else if (pendingImageOperation_.has_value() &&
				pendingImageOperation_->workerGeneration == 0) {
				const ImageOperationPurpose purpose = pendingImageOperation_->purpose;
				pendingImageOperation_.reset();
				ReportImageOperationFailure(purpose, errorMessage);
				if (!pendingMaterializationIntents_.empty()) {
					ReplayPendingMaterializationIntents();
				}
			} else {
				pendingMaterializationIntents_.clear();
				pendingImageIntents_.Cancel();
				SetTitle(fileList_.Current().filename().string() +
					" — source pixels unavailable: " + errorMessage);
			}
			return;
		}
		if (currentDecoded_ && currentDecoded_ != result->image) {
			jpegview_linux::RetiredImageBuffers retired;
			retired.decoded = std::move(currentDecoded_);
			imageOperationWorker_.Retire(std::move(retired));
		}
		currentDecoded_ = std::move(result->image);
		imageCache_.PromoteToActiveUse(selectedSource.Key());
		const jpegview_linux::DecodedFrame& firstFrame = currentDecoded_->frames.front();
		currentSourcePageDimensions_ =
			jpegview_linux::PageDimensions{firstFrame.width, firstFrame.height};
		if (imageDocument_.FrameIndex() != 0 ||
			imageDocument_.Animated() != currentDecoded_->animation) {
			jpegview_linux::RetiredImageBuffers retired = imageDocument_.SetFrame(
				0, currentDecoded_->animation, firstFrame.width, firstFrame.height,
				firstFrame.hasTransparency);
			imageOperationWorker_.Retire(std::move(retired));
		}
		imageDocument_.SetFrameIndex(0);
		imageDocument_.SetAnimation(currentDecoded_->animation);
		imageDocument_.SetDimensions(firstFrame.width, firstFrame.height,
			firstFrame.hasTransparency);
		if (pendingImageOperation_.has_value() &&
			pendingImageOperation_->workerGeneration == 0 &&
			pendingImageOperation_->document.source == imageDocument_.Source() &&
			pendingImageOperation_->document.ownerGeneration ==
				imageDocument_.OwnerGeneration()) {
			pendingImageOperation_->document = imageDocument_.Snapshot();
		}
		currentAnimationFrame_ = 0;
		std::vector<int> frameDelaysMs;
		frameDelaysMs.reserve(currentDecoded_->frames.size());
		for (const jpegview_linux::DecodedFrame& frame : currentDecoded_->frames) {
			frameDelaysMs.push_back(std::max(10, frame.delayMs));
		}
		playback_.ConfigureImage(std::move(frameDelaysMs), currentDecoded_->loopCount,
			currentDecoded_->animation, SDL_GetTicks());
		playback_.SetImageReady(false, SDL_GetTicks());
		if (currentSelectedLoadPending_) {
			const std::optional<jpegview_linux::ImageSessionSelection> selection =
				imageSession_.Selection();
			if (selection.has_value()) RestoreScaleMode(selection->viewport);
			cropSelection_.SetImageSize(CurrentImage().width, CurrentImage().height);
			const std::optional<jpegview_linux::PendingImageIntentBatch> intents =
				pendingImageIntents_.Drain(selectedSource.Key(), generation);
			if (intents.has_value()) {
				pendingMaterializationIntents_.insert(pendingMaterializationIntents_.end(),
					intents->actions.begin(), intents->actions.end());
			}
			RequestCurrentDisplayFrame();
			PrepareThumbnailPreload();
			PrepareImagePrefetch(pendingPrefetchDirection_);
			RefreshDoublePageRenderState();
			if (currentSelectedLoadPending_) SetPendingImageTitle();
		} else if (pendingImageOperation_.has_value() &&
			pendingImageOperation_->workerGeneration == 0) {
			(void)StartPendingImageOperation();
		} else if (!pendingMaterializationIntents_.empty()) {
			ReplayPendingMaterializationIntents();
		}
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
			if (!imageSession_.MatchesSelection(result.loadGeneration, result.source) ||
				!jpegview_linux::IsCurrentJpegDimensionsResult(result.loadGeneration,
				result.source, imageSession_.Generation(), source.Key()) ||
				!pendingCurrentJpegDimensions_.has_value() ||
				pendingCurrentJpegDimensions_->loadGeneration != result.loadGeneration ||
				pendingCurrentJpegDimensions_->source.Key() != result.source) continue;
			const std::optional<jpegview_linux::PendingRecentImageLoad> pendingLoad =
				imageSession_.TakePendingLoad(AbsoluteNormalized(fileList_.Current()));
			if (!pendingLoad.has_value()) continue;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
			const PendingCurrentJpegDimensions pending =
				std::move(*pendingCurrentJpegDimensions_);
			pendingCurrentJpegDimensions_.reset();
			currentJpegHeaderPending_ = false;
			std::optional<jpegview_linux::PendingImageIntentBatch> intents =
				pendingImageIntents_.Take(result.source, result.loadGeneration);
			TransitionFrame transitionFrame;
			if (intents.has_value() && intents->startTransition) {
				MoveTransitionFrame(transitionFrame, pendingTransitionFrame_);
				transitionCaptureDisplayKey_ = transitionFrame.displayKey;
			}
			if (result.succeeded && result.width > 0 && result.height > 0) {
				jpegDimensionCache_[result.source] = {result.width, result.height};
				failedJpegDimensionKeys_.erase(result.source);
			} else {
				failedJpegDimensionKeys_.insert(result.source);
			}
			const bool loaded = LoadCurrent(pending.prefetchDirection,
				ImageLoadStatePolicy::PreserveCurrent, true, pending.startupLoad,
				pendingLoad, intents.has_value() && !intents->actions.empty());
			if (!loaded && pending.startupLoad) {
				startupImageLoadFailed_ = true;
				deferredExitCode_ = 1;
				quitRequested_ = true;
				ReleaseTransitionFrame(transitionFrame);
				transitionCaptureDisplayKey_.clear();
				ClearActiveWorkingDisplayTextures();
				continue;
			}
			if (loaded && intents.has_value()) {
				pendingMaterializationIntents_.insert(pendingMaterializationIntents_.end(),
					intents->actions.begin(), intents->actions.end());
				if (intents->startTransition) {
					MoveTransitionFrame(pendingTransitionFrame_, transitionFrame);
				}
			}
			ReleaseTransitionFrame(transitionFrame);
			transitionCaptureDisplayKey_.clear();
			ClearActiveWorkingDisplayTextures();
			ReplayPendingMaterializationIntents();
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
			if (runtimeSettings_.Values().infoVisible) {
				frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
			}
			const jpegview_linux::ExifDateActionCompletion deferredAction =
				deferredExifDateAction_.Complete(result,
					AbsoluteNormalized(fileList_.Current()), current.Key());
			if (deferredAction.runDeferredAction) TouchCurrentImage(true);
		}
	}

	void RefreshCacheProtectionSnapshots(
		const std::vector<jpegview_linux::DisplayImageRequest>& requests,
		const std::vector<std::pair<jpegview_linux::SourceKey,
			jpegview_linux::CacheProtectionTier>>& decodedCandidates = {},
		const std::vector<std::pair<jpegview_linux::DisplayImageCacheKey,
			jpegview_linux::CacheProtectionTier>>& preparedCandidates = {},
			const std::vector<std::pair<std::string,
			jpegview_linux::CacheProtectionTier>>& textureCandidates = {}) {
		jpegview_linux::CacheProtectionSnapshot snapshot;
		const auto addTexture = [&snapshot](const std::string& key,
			jpegview_linux::CacheProtectionTier tier) {
			jpegview_linux::AddTextureProtection(snapshot, key, tier);
		};
		const auto add = [&snapshot](
			const jpegview_linux::DisplayImageRequest& request) {
			const jpegview_linux::CacheProtectionTier tier =
				jpegview_linux::CacheProtectionForWorkClass(request.workClass);
			jpegview_linux::AddDisplayRequestProtection(snapshot, request, tier);
		};
		const auto addWithTier = [&snapshot](
			const jpegview_linux::DisplayImageRequest& request,
			jpegview_linux::CacheProtectionTier tier) {
			jpegview_linux::AddDisplayRequestProtection(snapshot, request, tier);
		};
		if (currentDisplayRequest_.has_value()) addWithTier(*currentDisplayRequest_,
			jpegview_linux::CacheProtectionTier::Active);
		if (doublePagePartnerRequest_.has_value()) addWithTier(*doublePagePartnerRequest_,
			jpegview_linux::CacheProtectionTier::Active);
		addTexture(presentationController_.AnchorTextureKey(),
			jpegview_linux::CacheProtectionTier::Active);
		addTexture(presentationController_.PartnerTextureKey(),
			jpegview_linux::CacheProtectionTier::Active);
		jpegview_linux::AddMagnifyingGlassProtection(snapshot,
			CurrentMagnifyingGlassProtectionRequest());
		for (const auto& request : requests) add(request);
		for (const auto& candidate : decodedCandidates) {
			jpegview_linux::AddDecodedProtection(snapshot, candidate.first, candidate.second);
		}
		for (const auto& candidate : preparedCandidates) {
			jpegview_linux::AddPreparedProtection(snapshot, candidate.first, candidate.second);
		}
		for (const auto& candidate : textureCandidates) addTexture(candidate.first, candidate.second);
		imageCache_.SetProtectionSnapshot(snapshot.decoded);
		displayImageCache_.SetProtectionSnapshot(snapshot.prepared);
		ApplyDisplayTextureProtectionSnapshot(snapshot.textures);
	}

	void RefreshCacheProtectionSnapshotsForBatch() {
		std::vector<jpegview_linux::DisplayImageRequest> requests;
		std::vector<std::pair<jpegview_linux::SourceKey,
			jpegview_linux::CacheProtectionTier>> decodedCandidates;
		std::vector<std::pair<jpegview_linux::DisplayImageCacheKey,
			jpegview_linux::CacheProtectionTier>> preparedCandidates;
		std::vector<std::pair<std::string,
			jpegview_linux::CacheProtectionTier>> textureCandidates;
		const std::shared_ptr<DisplayPrefetchBatch> batch = displayPrefetchBatch_;
		if (batch) {
			std::lock_guard<std::mutex> lock(batch->mutex);
			for (const auto& source : batch->sourceByFilename) {
				const auto priority = batch->priorityByFilename.find(source.first);
				if (priority == batch->priorityByFilename.end()) continue;
				decodedCandidates.emplace_back(source.second.Key(), priority->second <= 2 ?
					jpegview_linux::CacheProtectionTier::Neighbor :
					jpegview_linux::CacheProtectionTier::DistantSpeculation);
			}
			for (const jpegview_linux::DisplayImageCacheKey& key :
				batch->protectedTextureCacheKeys) {
				preparedCandidates.emplace_back(key,
					jpegview_linux::CacheProtectionTier::Neighbor);
			}
			for (const std::string& key : batch->protectedTextureKeys) {
				textureCandidates.emplace_back(key,
					jpegview_linux::CacheProtectionTier::Neighbor);
			}
		}
		RefreshCacheProtectionSnapshots(requests, decodedCandidates, preparedCandidates,
			textureCandidates);
	}

	void TickDisplayPrefetchPlanner() {
		for (jpegview_linux::DisplayPrefetchPlannerResult& result :
			displayPreparationController_.TakePlanResults()) {
			if (result.failure.Failed() || fileList_.Empty() || clipboardMode_) continue;
			const SDL_Rect imageArea = ImageAreaRect();
			if (!jpegview_linux::MatchesDisplayPrefetchSnapshot(result,
				displayPreparationController_.ActivePlanGeneration(),
				fileList_.MutationRevision(),
				fileList_.DescriptorRevision(), displayPreparationController_.ViewportRevision(),
				fileList_.CurrentIndex(), pendingPrefetchDirection_,
				viewport_.PrefetchSnapshot(), imageArea.w, imageArea.h) ||
				CurrentInteractionWorkPlan().cancelQueuedSpeculation) continue;
			for (const jpegview_linux::DisplayPrefetchPlannedDimensions& dimensions :
				result.dimensions) {
				const jpegview_linux::SourceDescriptor* current =
					fileList_.DescriptorAt(dimensions.index);
				if (current == nullptr || current->Key() != dimensions.source ||
					dimensions.width <= 0 || dimensions.height <= 0) continue;
				jpegDimensionCache_[dimensions.source] = {
					dimensions.width, dimensions.height};
				if (doublePageModeEnabled_ &&
					dimensions.index == fileList_.CurrentIndex() + 1) {
					frameInvalidator_.Mark(
						jpegview_linux::FrameInvalidationReason::ImageResource);
				}
			}
			const std::shared_ptr<DisplayPrefetchBatch> batch = displayPrefetchBatch_;
			if (!batch) continue;
			bool published = batch->gate.Publish([&] {
				{
					std::lock_guard<std::mutex> lock(batch->mutex);
					batch->protectedTextureCacheKeys.insert(
						batch->protectedTextureCacheKeys.end(),
						result.protectedTextureCacheKeys.begin(),
						result.protectedTextureCacheKeys.end());
					batch->protectedTextureKeys.insert(batch->protectedTextureKeys.end(),
						result.protectedTextureKeys.begin(), result.protectedTextureKeys.end());
				}
				// PrepareImagePrefetch already replaced the previous neighbor set.
				// Append these JPEG requests while the batch is still publishable;
				// storing their descriptors in the batch would retain decoded aliases.
				displayImageCache_.RequestBackgroundBatch(result.requests);
			});
			if (!published) continue;
			RefreshCacheProtectionSnapshotsForBatch();
		}
	}

	void TickDisplayPreparationRequests() {
		if (!displayPrefetchBatch_) return;
		std::vector<jpegview_linux::DisplayImageRequest> requests =
			displayPreparationController_.TakeRequestBatch(
				displayPrefetchBatch_->generation);
		if (!requests.empty()) displayImageCache_.RequestBackgroundBatch(std::move(requests));
	}

	std::optional<jpegview_linux::PageDimensions> PageDimensionsAt(std::size_t index) {
		if (index >= fileList_.Files().size()) return std::nullopt;
		if (activeDoublePageRender_.has_value()) {
			if (index == activeDoublePageRender_->layout.firstIndex)
				return activeDoublePageRender_->currentPage;
			if (index == activeDoublePageRender_->layout.secondIndex)
				return activeDoublePageRender_->nextPage;
		}
		if (index == fileList_.CurrentIndex() && CurrentImage().width > 0 && CurrentImage().height > 0) {
			if (currentSpreadRotationValid_ && currentSourcePageDimensions_.has_value()) {
				return currentSourcePageDimensions_;
			}
			return jpegview_linux::PageDimensions{CurrentImage().width, CurrentImage().height};
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
		return {CurrentImage().width, CurrentImage().height};
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
				runtimeSettings_.Values().keepPictureLevels, runtimeSettings_.Values().autoContrast, runtimeSettings_.Values().defaultImageProcessing);
		jpegview_linux::ImageProcessingParams processing = filePreset.processing;
		processing.unsharpRadius = runtimeSettings_.Values().unsharpMaskRadius;
		processing.unsharpAmount = 0.0;
		processing.unsharpThreshold = runtimeSettings_.Values().unsharpMaskThreshold;
		return DoublePagePartnerSpec{filename, targetWidth, targetHeight,
			filePreset.autoContrast, processing, spread.clockwiseQuarterTurns};
	}

	std::optional<jpegview_linux::DisplayImageRequest> MakeDoublePagePartnerRequest(
		const DoublePagePartnerSpec& spec, const jpegview_linux::PageDimensions& nextPage) {
		const fs::path& filename = spec.filename;
		const jpegview_linux::SourceDescriptor source = SourceDescriptorForPath(filename);
		const jpegview_linux::DisplayImageTarget resolution =
			jpegview_linux::ClampDisplayImageTarget(nextPage.width, nextPage.height,
				spec.targetWidth, spec.targetHeight, spec.rotationQuarterTurns);
		if (jpegview_linux::IsJpegPath(filename)) {
			jpegview_linux::DisplayImageRequest request =
				jpegview_linux::MakeJpegDisplayImageRequest(source, nextPage.width,
				nextPage.height, resolution.width, resolution.height,
				spec.autoContrast, 1, spec.processing, spec.rotationQuarterTurns);
			request.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
			return request;
		}
		// Once the partner texture is ready, its decoded alias is deliberately
		// released. Reuse the stored display identity while that texture remains
		// cached instead of requiring the decoded source to be present again.
		if (doublePagePartnerRequest_.has_value()) {
			const jpegview_linux::DisplayImageRequest& cachedRequest =
				*doublePagePartnerRequest_;
			if (cachedRequest.source.Key() == source.Key() &&
				cachedRequest.frameIndex == 0 &&
				cachedRequest.sourceWidth == nextPage.width &&
				cachedRequest.sourceHeight == nextPage.height &&
				cachedRequest.targetWidth == resolution.width &&
				cachedRequest.targetHeight == resolution.height &&
				cachedRequest.autoContrast == spec.autoContrast &&
				jpegview_linux::EqualImageProcessing(cachedRequest.processing,
					spec.processing) &&
				cachedRequest.rotationQuarterTurns == spec.rotationQuarterTurns &&
				cachedRequest.key == presentationController_.PartnerTextureKey() &&
				presentationController_.SpreadReady(fileList_.CurrentIndex()) &&
				FindDisplayTexture(cachedRequest.key) != nullptr) {
				return cachedRequest;
			}
		}
		auto decoded = imageCache_.Find(source);
		if (!decoded && displayPrefetchBatch_) {
			std::lock_guard<std::mutex> lock(displayPrefetchBatch_->mutex);
			const auto working = displayPrefetchBatch_->decodedImages.find(filename.string());
			if (working != displayPrefetchBatch_->decodedImages.end() &&
				working->second.first == source.Key()) {
				decoded = working->second.second;
			}
		}
		if (!decoded || decoded->frames.empty()) return std::nullopt;
		jpegview_linux::DisplayImageRequest request =
			jpegview_linux::MakeDisplayImageRequest(source, decoded, 0,
			resolution.width, resolution.height, spec.autoContrast, 1, spec.processing,
			spec.rotationQuarterTurns);
		request.workClass = jpegview_linux::PerfWorkClass::ActiveImageSpread;
		return request;
	}

	std::string ModifiedSpreadAnchorKey() const {
		if (fileList_.Empty()) return {};
		return "modified-spread-anchor:" +
			AbsoluteNormalized(fileList_.Current()).string() + ':' +
			std::to_string(imageSession_.DocumentRevision()) + ':' +
			std::to_string(currentImageRotationQuarterTurns_);
	}

	SDL_Texture* ActiveDoublePageAnchorTexture() {
		if (!activeDoublePageRender_.has_value()) return nullptr;
		if (activeDoublePageRender_->transformedAnchorTexture) return texture_;
		return FindDisplayTexture(presentationController_.AnchorTextureKey());
	}

	void CancelPendingDoublePageRequests() {
		const auto cancel = [this](const std::string& key) {
			if (key.empty()) return;
			SetDisplayTextureProtection(key,
				jpegview_linux::CacheProtectionTier::DistantSpeculation);
			if (FindDisplayTexture(key) == nullptr) {
				displayImageCache_.CancelBackground(key);
				displayImageCache_.Release(key);
			}
		};
		cancel(presentationController_.AnchorTextureKey());
		cancel(presentationController_.PartnerTextureKey());
	}

	void RefreshDoublePageRenderState() {
		jpegview_linux::PerfContextScope workContext(
			jpegview_linux::PerfWorkClass::ActiveImageSpread,
			jpegview_linux::PerfExecution::EventThread);
		const SDL_Rect area = ImageAreaRect();
		const auto deactivate = [this, &area] {
			SetDisplayTextureProtection(doublePagePartnerDisplayKey_,
				jpegview_linux::CacheProtectionTier::DistantSpeculation);
			doublePagePartnerDisplayKey_.clear();
			if (!activeDoublePageRender_.has_value()) return;
			const jpegview_linux::ViewportSnapshot snapshot = viewport_.Snapshot();
			activeDoublePageRender_.reset();
			viewport_.Restore(snapshot, CurrentImage().width, CurrentImage().height, area.w, area.h);
			currentDisplayRequest_.reset();
			SetTitle();
		};
		const std::size_t currentIndex = fileList_.Empty() ? 0 : fileList_.CurrentIndex();
		const auto useSinglePage = [this, &deactivate, currentIndex](
			bool keepDeferredDisplay = false) {
			CancelActiveSpreadPartnerSourceRequest();
			CancelPendingDoublePageRequests();
			presentationController_.UseSinglePage(currentIndex);
			doublePagePartnerRequest_.reset();
			if (!keepDeferredDisplay) deferredCurrentDisplayPreparation_ = false;
			deactivate();
		};
		const auto awaitDimensions = [this, &deactivate, currentIndex] {
			ReconcileActiveSpreadPartnerSourceRequest();
			CancelPendingDoublePageRequests();
			presentationController_.AwaitDimensions(currentIndex, currentIndex + 1);
			doublePagePartnerRequest_.reset();
			deactivate();
		};
		const bool selectionMatchesLoadedImage = !fileList_.Empty() &&
			(clipboardMode_ || imageSession_.LoadedPath() ==
				AbsoluteNormalized(fileList_.Current()));
		jpegview_linux::SpreadPreparationSnapshot spreadSnapshot;
		spreadSnapshot.selectedIndex = currentIndex;
		spreadSnapshot.pageCount = fileList_.Size();
		spreadSnapshot.selectedImageCommitted = selectionMatchesLoadedImage;
		spreadSnapshot.cacheAvailable = cacheBudget_->Capacity() != 0;
		spreadSnapshot.anchorPixelsAvailable = CurrentImage().width > 0 && CurrentImage().height > 0;
		spreadSnapshot.anchorRotationQuarterTurns = currentImageRotationQuarterTurns_;
		spreadSnapshot.modes = {doublePageModeEnabled_, mangaReadingOrderEnabled_};
		jpegview_linux::SpreadPreparationDecision spreadDecision =
			presentationController_.PlanSpreadPreparation(spreadSnapshot);
		if (spreadDecision.action == jpegview_linux::SpreadPreparationAction::AwaitDimensions) {
			awaitDimensions();
			return;
		}
		if (spreadDecision.action == jpegview_linux::SpreadPreparationAction::UseSinglePage ||
			spreadDecision.action ==
				jpegview_linux::SpreadPreparationAction::UseSinglePageAfterPartnerFailure) {
			useSinglePage(spreadDecision.preserveDeferredDisplay);
			return;
		}
		if (spreadDecision.action !=
			jpegview_linux::SpreadPreparationAction::CapturePageDimensions) {
			useSinglePage();
			return;
		}
		spreadSnapshot.pageDimensionsCaptured = true;
		spreadSnapshot.anchorDimensions = PageDimensionsAt(currentIndex);
		spreadSnapshot.partnerDimensions = PageDimensionsAt(currentIndex + 1);
		if (!spreadSnapshot.partnerDimensions.has_value()) {
			spreadSnapshot.partnerDecodeFailed = NeighborDecodeFailed(currentIndex + 1);
		}
		spreadDecision = presentationController_.PlanSpreadPreparation(spreadSnapshot);
		if (spreadDecision.action == jpegview_linux::SpreadPreparationAction::AwaitDimensions) {
			awaitDimensions();
			return;
		}
		if (spreadDecision.action == jpegview_linux::SpreadPreparationAction::UseSinglePage ||
			spreadDecision.action ==
				jpegview_linux::SpreadPreparationAction::UseSinglePageAfterPartnerFailure) {
			useSinglePage(spreadDecision.preserveDeferredDisplay);
			return;
		}
		if (spreadDecision.action != jpegview_linux::SpreadPreparationAction::PrepareSpread ||
			!spreadDecision.layout.has_value() ||
			!spreadSnapshot.anchorDimensions.has_value() ||
			!spreadSnapshot.partnerDimensions.has_value()) {
			useSinglePage();
			return;
		}
		CancelActiveSpreadPartnerSourceRequest();
		const jpegview_linux::PageDimensions& current = *spreadSnapshot.anchorDimensions;
		const jpegview_linux::PageDimensions& next = *spreadSnapshot.partnerDimensions;
		const jpegview_linux::DoublePageSpread& layout = *spreadDecision.layout;
		const auto spec = DoublePagePartnerDisplaySpec(layout);
		if (!spec.has_value()) {
			useSinglePage();
			return;
		}
		auto partnerRequest = MakeDoublePagePartnerRequest(*spec, next);
		const bool cachedReadyPartner = partnerRequest.has_value() &&
			!partnerRequest->Valid() &&
			presentationController_.SpreadReady(currentIndex) &&
			partnerRequest->key == presentationController_.PartnerTextureKey() &&
			FindDisplayTexture(partnerRequest->key) != nullptr;
		const bool partnerRequestAvailable = partnerRequest.has_value() &&
			(partnerRequest->Valid() || cachedReadyPartner);
		if (!partnerRequestAvailable) {
			const jpegview_linux::SpreadRequestDecision unavailablePartnerDecision =
				presentationController_.PlanSpreadRequests({false, 0, 0,
					cacheBudget_->Capacity(), false});
			useSinglePage(unavailablePartnerDecision.preserveDeferredDisplay);
			return;
		}
		const std::string previousCurrentDisplayKey = currentDisplayRequest_.has_value() ?
			currentDisplayRequest_->key : std::string();

		const bool sameSpreadCanvas = activeDoublePageRender_.has_value() &&
			activeDoublePageRender_->layout.firstIndex == layout.firstIndex &&
			activeDoublePageRender_->layout.secondIndex == layout.secondIndex &&
			activeDoublePageRender_->layout.canvasWidth == layout.canvasWidth &&
			activeDoublePageRender_->layout.canvasHeight == layout.canvasHeight;
		const std::string previousPosition = CurrentImagePositionText();
		if (!sameSpreadCanvas) {
			const jpegview_linux::ViewportSnapshot snapshot = viewport_.Snapshot();
			viewport_.Restore(snapshot, layout.canvasWidth, layout.canvasHeight,
				area.w, area.h);
			currentDisplayRequest_.reset();
		}
		const bool transformedAnchor = (currentPixelsDetachedFromSource_ ||
			(currentSpreadRotationValid_ && imageModified_)) && texture_ != nullptr;
		activeDoublePageRender_ = ActiveDoublePageRender{
			layout, current, next, *spec, transformedAnchor};
		if (CurrentImagePositionText() != previousPosition) SetTitle();
		const auto anchorSize = DoublePagePageDisplaySize(layout, layout.currentPage, area);
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
		std::size_t anchorTextureBytes = std::numeric_limits<std::size_t>::max();
		std::size_t partnerTextureBytes = std::numeric_limits<std::size_t>::max();
		if (anchorRequest.has_value()) {
			if (!jpegview_linux::EstimateDisplayImageBytes(*anchorRequest,
				anchorTextureBytes)) {
				anchorTextureBytes = std::numeric_limits<std::size_t>::max();
			}
		} else if (CurrentImage().width > 0 && CurrentImage().height > 0) {
			const std::size_t width = static_cast<std::size_t>(CurrentImage().width);
			const std::size_t height = static_cast<std::size_t>(CurrentImage().height);
			const std::size_t maximum = std::numeric_limits<std::size_t>::max();
			if (width <= maximum / height && width * height <= maximum / 4) {
				anchorTextureBytes = width * height * 4;
			}
		}
		if (!jpegview_linux::EstimateDisplayImageBytes(*partnerRequest,
			partnerTextureBytes)) {
			partnerTextureBytes = std::numeric_limits<std::size_t>::max();
		}

		const std::string oldAnchorKey = presentationController_.AnchorTextureKey();
		const std::string oldPartnerKey = presentationController_.PartnerTextureKey();
		const jpegview_linux::SpreadRequestDecision initialRequestDecision =
			presentationController_.PlanSpreadRequests({true,
				anchorTextureBytes, partnerTextureBytes, cacheBudget_->Capacity(),
				presentationController_.SpreadFailed(currentIndex,
					anchorKey, partnerRequest->key)});
		jpegview_linux::SpreadRequestDecision admittedRequestDecision =
			initialRequestDecision;
		if (admittedRequestDecision.action ==
			jpegview_linux::SpreadRequestAction::ReturnToSinglePage &&
			anchorRequest.has_value()) {
			const jpegview_linux::DisplayImageTarget fittedResolution =
				jpegview_linux::ClampDisplayImageTarget(anchorRequest->sourceWidth,
					anchorRequest->sourceHeight, anchorSize.first, anchorSize.second,
					anchorRequest->rotationQuarterTurns);
			if (fittedResolution.width < anchorRequest->targetWidth ||
				fittedResolution.height < anchorRequest->targetHeight) {
				jpegview_linux::DisplayImageRequest fittedAnchorRequest;
				if (anchorRequest->decoded) {
					fittedAnchorRequest = jpegview_linux::MakeDisplayImageRequest(
						anchorRequest->source, anchorRequest->decoded,
						anchorRequest->frameIndex, fittedResolution.width,
						fittedResolution.height, anchorRequest->autoContrast,
						anchorRequest->priority, anchorRequest->processing,
						anchorRequest->rotationQuarterTurns,
						anchorRequest->includeSpectrum);
				} else {
					fittedAnchorRequest = jpegview_linux::MakeJpegDisplayImageRequest(
						anchorRequest->source, anchorRequest->sourceWidth,
						anchorRequest->sourceHeight, fittedResolution.width,
						fittedResolution.height, anchorRequest->autoContrast,
						anchorRequest->priority, anchorRequest->processing,
						anchorRequest->rotationQuarterTurns,
						anchorRequest->includeSpectrum);
				}
				fittedAnchorRequest.workClass = anchorRequest->workClass;
				fittedAnchorRequest.selectionGeneration =
					anchorRequest->selectionGeneration;
				std::size_t fittedAnchorTextureBytes =
					std::numeric_limits<std::size_t>::max();
				if (fittedAnchorRequest.Valid() &&
					fittedAnchorRequest.key != anchorRequest->key &&
					jpegview_linux::EstimateDisplayImageBytes(fittedAnchorRequest,
						fittedAnchorTextureBytes)) {
					const jpegview_linux::SpreadRequestDecision fittedDecision =
						presentationController_.PlanSpreadRequests({true,
							fittedAnchorTextureBytes, partnerTextureBytes,
							cacheBudget_->Capacity(),
							presentationController_.SpreadFailed(currentIndex,
								fittedAnchorRequest.key, partnerRequest->key)});
					if (fittedDecision.action !=
						jpegview_linux::SpreadRequestAction::ReturnToSinglePage) {
						anchorTextureBytes = fittedAnchorTextureBytes;
						anchorKey = fittedAnchorRequest.key;
						anchorRequest = std::move(fittedAnchorRequest);
						admittedRequestDecision = fittedDecision;
					}
				}
			}
		}
		if (admittedRequestDecision.action ==
			jpegview_linux::SpreadRequestAction::ReturnToSinglePage) {
			useSinglePage(admittedRequestDecision.preserveDeferredDisplay);
			return;
		}
		if (admittedRequestDecision.action ==
			jpegview_linux::SpreadRequestAction::KeepFailedSpreadDeferred) {
			CancelPendingDoublePageRequests();
			deferredCurrentDisplayPreparation_ = true;
			doublePagePartnerRequest_.reset();
			deactivate();
			return;
		}
		const bool newPairRequests = presentationController_.BeginSpread(currentIndex,
			currentIndex + 1, anchorKey, partnerRequest->key);
		if (newPairRequests) {
			for (const std::string* oldKey : {&oldAnchorKey, &oldPartnerKey}) {
				if (oldKey->empty() || *oldKey == anchorKey ||
					*oldKey == partnerRequest->key) continue;
				SetDisplayTextureProtection(*oldKey,
					jpegview_linux::CacheProtectionTier::DistantSpeculation);
				if (FindDisplayTexture(*oldKey) == nullptr) {
					displayImageCache_.CancelBackground(*oldKey);
					displayImageCache_.Release(*oldKey);
				}
			}
			doublePagePartnerRequest_.reset();
		}
		if (!previousCurrentDisplayKey.empty() &&
			previousCurrentDisplayKey != anchorKey) {
			SetDisplayTextureProtection(previousCurrentDisplayKey,
				jpegview_linux::CacheProtectionTier::DistantSpeculation);
		}
		if (anchorRequest.has_value()) currentDisplayRequest_ = *anchorRequest;
		doublePagePartnerRequest_ = std::move(partnerRequest);
		doublePagePartnerDisplayKey_ = doublePagePartnerRequest_->key;
		SetDisplayTextureProtection(anchorKey, jpegview_linux::CacheProtectionTier::Active);
		SetDisplayTextureProtection(doublePagePartnerDisplayKey_,
			jpegview_linux::CacheProtectionTier::Active);

		std::vector<jpegview_linux::DisplayImageRequest> requests;
		if (anchorRequest.has_value() && FindDisplayTexture(anchorKey) == nullptr) {
			requests.push_back(*anchorRequest);
		}
		if (FindDisplayTexture(doublePagePartnerDisplayKey_) == nullptr) {
			requests.push_back(*doublePagePartnerRequest_);
		}
		if (!requests.empty()) displayImageCache_.RequestBackgroundBatch(requests);
		if (transformedAnchor || FindDisplayTexture(anchorKey) != nullptr) {
			presentationController_.MarkTextureReady(anchorKey);
		}
		if (FindDisplayTexture(doublePagePartnerDisplayKey_) != nullptr) {
			presentationController_.MarkTextureReady(doublePagePartnerDisplayKey_);
			doublePagePartnerRequest_->decoded.reset();
		}
		const bool anchorUnavailable = !transformedAnchor &&
			FindDisplayTexture(anchorKey) == nullptr &&
			!displayImageCache_.HasPendingOrCached(anchorKey);
		const bool partnerUnavailable = FindDisplayTexture(doublePagePartnerDisplayKey_) == nullptr &&
			!displayImageCache_.HasPendingOrCached(doublePagePartnerDisplayKey_);
		if (!presentationController_.SpreadReady(currentIndex) &&
			(anchorUnavailable || partnerUnavailable)) {
			presentationController_.MarkTextureFailed(
				anchorUnavailable ? anchorKey : doublePagePartnerDisplayKey_);
			deferredCurrentDisplayPreparation_ = true;
		}
	}

	void SelectPictureLevelsForCurrentFile(bool continuingPendingLoad = false) {
		const std::string key = AbsoluteNormalized(fileList_.Current()).string();
		const auto saved = imageProcessingStore_.find(key);
		const jpegview_linux::ImageProcessingPreset current{imageProcessing_, autoContrastEnabled_};
		const jpegview_linux::ImageProcessingPreset selected =
			jpegview_linux::ResolveImageProcessingForLoad(current,
				saved == imageProcessingStore_.end() ? nullptr : &saved->second,
				runtimeSettings_.Values().keepPictureLevels, runtimeSettings_.Values().autoContrast, runtimeSettings_.Values().defaultImageProcessing,
				continuingPendingLoad);
		imageProcessing_ = selected.processing;
		autoContrastEnabled_ = selected.autoContrast;
		imageProcessing_.unsharpRadius = runtimeSettings_.Values().unsharpMaskRadius;
		imageProcessing_.unsharpAmount = 0.0;
		imageProcessing_.unsharpThreshold = runtimeSettings_.Values().unsharpMaskThreshold;
		imageSession_.UpdateProcessing({imageProcessing_, autoContrastEnabled_});
	}

	bool LoadCurrent(int prefetchDirection = 0,
		ImageLoadStatePolicy loadStatePolicy = ImageLoadStatePolicy::PreserveCurrent,
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
		CancelPendingImageOperation();
		CancelImageSpectrumBeforeImageMutation();
		const fs::path targetPath = AbsoluteNormalized(fileList_.Current());
		const bool pathChanged = imageSession_.LoadedPath().empty() ||
			targetPath != imageSession_.LoadedPath();
		if (!clipboardMode_) {
			imageSession_.SaveCurrentBeforeLoad(targetPath,
				imageSession_.ClipboardReturnViewport().value_or(viewport_.Snapshot()),
				{doublePageModeEnabled_, mangaReadingOrderEnabled_}, recentFiles_);
		}
		if (!clipboardMode_ && pathChanged &&
			loadStatePolicy != ImageLoadStatePolicy::PreserveCurrent) {
			const std::optional<jpegview_linux::DoublePageModeState> savedModes =
				loadStatePolicy == ImageLoadStatePolicy::RestoreRecent ?
					recentFiles_.FindDoublePageMode(targetPath) : std::nullopt;
			doublePageModeEnabled_ = savedModes.has_value() ? savedModes->enabled :
				runtimeSettings_.Values().doublePageModeEnabled;
			mangaReadingOrderEnabled_ = savedModes.has_value() ? savedModes->mangaReadingOrder :
				runtimeSettings_.Values().mangaReadingOrderEnabled;
		}
		const std::size_t currentIndex = fileList_.CurrentIndex();
		CancelPendingDoublePageRequests();
		if (IsPotentialDoublePageAnchor(currentIndex)) {
			presentationController_.AwaitDimensions(currentIndex, currentIndex + 1);
		} else {
			presentationController_.UseSinglePage(currentIndex);
		}
		jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.NavigationSnapshot();
		if (resumePendingLoad.has_value()) {
			viewportSnapshot = replayPendingIntents ?
				resumePendingLoad->intentBaseSnapshot : resumePendingLoad->viewportSnapshot;
		} else {
			viewportSnapshot = imageSession_.ResolveViewportForSelection(targetPath,
				clipboardMode_, viewport_.Snapshot(), viewport_.NavigationSnapshot(), recentFiles_,
				loadStatePolicy == ImageLoadStatePolicy::RestoreRecent);
		}
		// Resolve the snapshot while the previous selected identity is still known. A
		// cancellation here would otherwise make a reversal look like the committed
		// image still owned the live viewport.
		CancelPendingCurrentJpegDimensions();
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		ClearCropSelection();
		SelectPictureLevelsForCurrentFile(resumePendingLoad.has_value());
		const jpegview_linux::ImageSessionStart sessionStart = imageSession_.BeginSelection(
			source, targetPath, viewportSnapshot,
			{imageProcessing_, autoContrastEnabled_}, !clipboardMode_);
		const std::uint64_t loadGeneration = sessionStart.selection.generation;
		const bool clearDocument = true;
		const bool retainDecodedSource = currentDecoded_ &&
			imageDocument_.Source() == source.Key();
		jpegview_linux::RetiredImageBuffers retiredDocument = imageDocument_.BeginSelection(
			source.Key(), loadGeneration, clearDocument);
		if (!retainDecodedSource) retiredDocument.decoded = std::move(currentDecoded_);
		imageOperationWorker_.Retire(std::move(retiredDocument));
		imageDocument_.UpdateProcessing(imageProcessing_, autoContrastEnabled_);
		selectedSourceDecodeChannel_->Activate(loadGeneration, source.Key());
		currentSelectedLoadPending_ = true;
		currentSelectedStartupLoad_ = startupLoad;
		currentJpegHeaderPending_ = false;
		pendingSelectedDisplayKey_.clear();
		pendingImageIntents_.Begin(targetPath, source.Key(), loadGeneration);
		playback_.SetImageReady(false, SDL_GetTicks());
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
		failedCurrentDisplayKey_.clear();

		if (sessionStart.effects.clearPreviousPresentation) {
			if (texture_ != nullptr) DestroyTextureMeasured(texture_);
			texture_ = nullptr;
			ClearDisplayTexture();
			ClearDisplayTextureProtections();
			ClearPendingTextureUploads();
			ClearActiveWorkingDisplayTextures();
			currentAnimationFrame_ = 0;
			currentDisplayRequest_.reset();
			activeDoublePageRender_.reset();
			doublePagePartnerDisplayKey_.clear();
			doublePagePartnerRequest_.reset();
			deferredCurrentDisplayPreparation_ = false;
			currentPixelsDetachedFromSource_ = false;
			editedImageSpectrumValid_ = false;
			imageModified_ = false;
			currentImageRotationQuarterTurns_ = 0;
			currentSpreadRotationValid_ = false;
			currentSourcePageDimensions_.reset();
		} else {
			const bool discardEditedPresentation = imageModified_ ||
				currentPixelsDetachedFromSource_;
			if (discardEditedPresentation && texture_ != nullptr) {
				DestroyTextureMeasured(texture_);
				texture_ = nullptr;
			}
			currentAnimationFrame_ = 0;
			currentDisplayRequest_.reset();
			activeDoublePageRender_.reset();
			doublePagePartnerDisplayKey_.clear();
			doublePagePartnerRequest_.reset();
			currentPixelsDetachedFromSource_ = false;
			imageModified_ = false;
			currentImageRotationQuarterTurns_ = 0;
			currentSpreadRotationValid_ = false;
			editedImageSpectrumValid_ = false;
			if (currentDecoded_ && !currentDecoded_->frames.empty()) {
				const jpegview_linux::DecodedFrame& firstFrame =
					currentDecoded_->frames.front();
				imageDocument_.SetDimensions(firstFrame.width, firstFrame.height,
					firstFrame.hasTransparency);
				currentSourcePageDimensions_ =
					jpegview_linux::PageDimensions{firstFrame.width, firstFrame.height};
			} else if (discardEditedPresentation) {
				imageDocument_.SetDimensions(0, 0, false);
				currentSourcePageDimensions_.reset();
			}
		}
		if (currentDecoded_ && !currentDecoded_->frames.empty()) {
			const jpegview_linux::DecodedFrame& firstFrame =
				currentDecoded_->frames.front();
			imageDocument_.SetDimensions(firstFrame.width, firstFrame.height,
				firstFrame.hasTransparency);
			imageDocument_.SetFrameIndex(0);
			imageDocument_.SetAnimation(currentDecoded_->animation);
		}
		bool waitingForJpegDimensions = false;
		int sourceWidth = 0;
		int sourceHeight = 0;
		const bool displayCacheEnabled = cacheBudget_->Capacity() != 0;
		const bool jpegSource = jpegview_linux::IsJpegPath(fileList_.Current());
		const bool cachedJpegDimensions = sessionStart.effects.requestSelectedSourcePreparation &&
			displayCacheEnabled && jpegSource &&
			CachedJpegDimensions(source, sourceWidth, sourceHeight);
		const jpegview_linux::SelectedSourcePreparationAction sourcePreparationAction =
			imageSession_.PlanSelectedSourcePreparation({
				sessionStart.effects.requestSelectedSourcePreparation,
				jpegSource, displayCacheEnabled, cachedJpegDimensions, source.Valid(),
				source.Valid() && failedJpegDimensionKeys_.find(source.Key()) !=
					failedJpegDimensionKeys_.end()});
		if (sourcePreparationAction ==
			jpegview_linux::SelectedSourcePreparationAction::UseCachedJpegDimensions) {
			currentSourcePageDimensions_ =
				jpegview_linux::PageDimensions{sourceWidth, sourceHeight};
			imageDocument_.SetDimensions(sourceWidth, sourceHeight, false);
			if (sessionStart.effects.restoreViewport) RestoreScaleMode(viewportSnapshot);
			deferredCurrentDisplayPreparation_ = false;
			RequestCurrentDisplayFrame();
		} else if (sourcePreparationAction ==
			jpegview_linux::SelectedSourcePreparationAction::RequestJpegDimensions) {
			currentJpegHeaderPending_ = true;
			waitingForJpegDimensions = true;
			(void)imageSession_.SetStage(loadGeneration, source.Key(),
				jpegview_linux::ImageSessionStage::AwaitingJpegDimensions);
			if (sessionStart.effects.restoreViewport) RestoreScaleMode(viewportSnapshot);
			TransferActiveSpreadPartnerRequestToCurrentImage(source, true);
			RequestCurrentJpegDimensions(source, loadGeneration,
				prefetchDirection, startupLoad);
		}

		if (sourcePreparationAction ==
			jpegview_linux::SelectedSourcePreparationAction::DecodeSelectedSource) {
			TransferActiveSpreadPartnerRequestToCurrentImage(source, false);
			const jpegview_linux::DecodedImageCache::ImagePtr decoded = imageCache_.Find(source);
			if (decoded) {
				(void)selectedSourceDecodeChannel_->Publish({loadGeneration, source,
					decoded, {}});
			} else {
				RequestSelectedSourceDecode(source, loadGeneration);
			}
		}
		if (CurrentImage().width > 0 && CurrentImage().height > 0) {
			cropSelection_.SetImageSize(CurrentImage().width, CurrentImage().height);
		}
		playback_.ConfigureImage({}, 0, false, SDL_GetTicks());
		if (waitingForJpegDimensions) {
			SetPendingHeaderTitle();
		} else if (currentSelectedLoadPending_) {
			SetPendingImageTitle();
		} else {
			SetTitle();
		}
		PrepareThumbnailPreload();
		PrepareImagePrefetch(prefetchDirection);
		RefreshDoublePageRenderState();
		return true;
	}

	std::vector<std::size_t> VisibleThumbnailIndices() const {
		std::vector<std::size_t> indices;
		if (!runtimeSettings_.Values().thumbnailPanelVisible || fileList_.Empty()) return indices;
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
			presentationController_.Phase() ==
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

	void DiscardThumbnailPixelStoreRetry() {
		if (!thumbnailPixelStoreRetry_) return;
		thumbnailPreparation_.Retire(thumbnailPixelStoreRetry_->image);
		thumbnailPixelStoreRetry_.reset();
	}

	void PauseThumbnailPreparation() {
		if (thumbnailPixelStoreRetry_) {
			RetryThumbnail(*thumbnailPixelStoreRetry_);
			DiscardThumbnailPixelStoreRetry();
		}
		RetryCancelledThumbnails(thumbnailPreparation_.Cancel({
			jpegview_linux::PerfWorkClass::VisibleThumbnail,
			jpegview_linux::PerfWorkClass::DistantSpeculation}));
	}

	void DeactivateDisplayPrefetchBatch() {
		if (!displayPrefetchBatch_) return;
		displayPrefetchBatch_->gate.Deactivate();
		displayPreparationController_.CancelRequestBatch(
			displayPrefetchBatch_->generation);
	}

	void InvalidateViewportPrefetch() {
		displayPreparationController_.AdvanceViewportRevision();
		displayPreparationController_.CancelPlan();
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
				DeactivateDisplayPrefetchBatch();
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
				displayPreparationController_.CancelRequestBatch(batch->generation);
			}
		}
		imageCache_.CancelActiveSpreadRequest(
			activeSpreadSourceRequest_->source,
			activeSpreadSourceRequest_->dimensionsOnly);
		activeSpreadSourceRequest_.reset();
	}

	void TransferActiveSpreadPartnerRequestToCurrentImage(
		const jpegview_linux::SourceDescriptor& currentSource,
		bool dimensionsOnly) {
		if (!activeSpreadSourceRequest_.has_value()) return;
		const jpegview_linux::DecodedImageRequestIdentity partnerRequest{
			activeSpreadSourceRequest_->source.Key(),
			activeSpreadSourceRequest_->dimensionsOnly};
		const jpegview_linux::DecodedImageRequestIdentity currentRequest{
			currentSource.Key(), dimensionsOnly};
		if (!jpegview_linux::CanTransferActiveSpreadRequestToCurrentImage(
			partnerRequest, currentRequest)) return;
		if (const auto batch = activeSpreadSourceRequest_->batch.lock()) {
			batch->gate.Deactivate();
			displayPreparationController_.CancelRequestBatch(batch->generation);
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
		batch->generation = displayPreparationController_.BeginRequestBatch();
		batch->completionChannel = displayPreparationController_.RequestChannel();
		batch->owner = jpegview_linux::DisplayPrefetchBatchOwner::ActiveSpread;
		batch->sourceByFilename.emplace(sourceRequest->filename.string(), partnerSource);
		batch->priorityByFilename.emplace(sourceRequest->filename.string(), 1);
		batch->context.currentIndex = currentIndex;
		batch->context.pageCount = fileList_.Size();
		batch->context.currentPageDimensions = currentDimensions;
		batch->context.doublePageMode = {doublePageModeEnabled_, mangaReadingOrderEnabled_};
		batch->context.viewport = viewport_.NavigationSnapshot();
		const SDL_Rect imageArea = ImageAreaRect();
		batch->context.imageAreaWidth = imageArea.w;
		batch->context.imageAreaHeight = imageArea.h;
		displayPrefetchBatch_ = batch;
		RefreshCacheProtectionSnapshotsForBatch();
		ActiveSpreadSourceRequest trackedRequest = *sourceRequest;
		trackedRequest.batch = batch;
		activeSpreadSourceRequest_ = std::move(trackedRequest);
		const auto publishDimensions = [batch, partnerSource](const fs::path& filename,
			bool succeeded, int width, int height,
			jpegview_linux::DecodedImageCache::ImagePtr decoded = {}) {
			batch->gate.Publish([&] {
				std::lock_guard<std::mutex> lock(batch->mutex);
				if (!succeeded || width <= 0 || height <= 0) {
					batch->failedDecodeFilenames.insert(filename.string());
					batch->pageDimensionsChanged = true;
					return;
				}
				batch->decodedPageDimensions[filename.string()] = {width, height};
				batch->pageDimensionsChanged = true;
				const auto retained = jpegview_linux::RetainDisplayPrefetchDecodedImage(
					batch->owner, decoded);
				if (retained) batch->decodedImages[filename.string()] =
					{partnerSource.Key(), retained};
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
				publishDimensions(filename, succeeded, width, height, decoded);
			}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
		}
	}

	void TickActiveSpreadDimensions() {
		const std::shared_ptr<DisplayPrefetchBatch> batch = displayPrefetchBatch_;
		if (!batch || batch->owner != jpegview_linux::DisplayPrefetchBatchOwner::ActiveSpread) {
			return;
		}
		bool dimensionsChanged = false;
		{
			std::lock_guard<std::mutex> lock(batch->mutex);
			dimensionsChanged = batch->pageDimensionsChanged;
			batch->pageDimensionsChanged = false;
		}
		if (dimensionsChanged) {
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		}
	}

	void UpdateInteractionWorkPolicy() {
		interactionWorkPolicy_.SetCaptureActive(dragging_ || cropMouseDragging_ ||
			zoomNavigatorDragging_ || thumbnailPanelResizing_);
		const jpegview_linux::InteractionWorkPlan plan = CurrentInteractionWorkPlan();
		fileListScanWorker_.SetForegroundPending(plan.foregroundPending);
		jpegview_linux::SourceWorkCoordinator::Global().SetForegroundPending(
			plan.foregroundPending);
		const bool enteringSuspension = plan.cancelQueuedSpeculation &&
			(!speculativeWorkSuspended_ ||
				(plan.foregroundPending && !lastForegroundPending_));
		if (enteringSuspension) {
			DeactivateDisplayPrefetchBatch();
			displayPreparationController_.CancelPlan();
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
		displayPreparationController_.AdvanceViewportRevision();
		displayPreparationController_.CancelPlan();
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
		batch->generation = displayPreparationController_.BeginRequestBatch();
		batch->completionChannel = displayPreparationController_.RequestChannel();
		batch->context.viewport = viewport_.PrefetchSnapshot();
		batch->context.imageAreaWidth = imageArea.w;
		batch->context.imageAreaHeight = imageArea.h;
		batch->context.currentIndex = fileList_.CurrentIndex();
		batch->context.pageCount = fileList_.Size();
		batch->context.doublePageMode = {doublePageModeEnabled_, mangaReadingOrderEnabled_};
		batch->context.currentPageDimensions = PageDimensionsAt(fileList_.CurrentIndex());
		jpegview_linux::DisplayPrefetchPlannerRequest plannerRequest;
		plannerRequest.catalogRevision = fileList_.MutationRevision();
		plannerRequest.descriptorRevision = fileList_.DescriptorRevision();
		plannerRequest.viewportRevision = displayPreparationController_.ViewportRevision();
		plannerRequest.currentIndex = fileList_.CurrentIndex();
		plannerRequest.pageCount = fileList_.Size();
		plannerRequest.preferredDirection = preferredDirection;
		plannerRequest.viewport = batch->context.viewport;
		plannerRequest.imageAreaWidth = imageArea.w;
		plannerRequest.imageAreaHeight = imageArea.h;
		plannerRequest.doublePageMode = batch->context.doublePageMode;
		plannerRequest.currentPageDimensions = batch->context.currentPageDimensions;
		for (const auto& retained : displayTextureCache_) {
			batch->retainedTextures.push_back({retained.first,
				retained.second.cacheKey});
		}
		plannerRequest.retainedTextures = batch->retainedTextures;
		ClearDisplayTextureProtections();
		if (currentDisplayRequest_.has_value()) {
			SetDisplayTextureProtection(currentDisplayRequest_->key,
				jpegview_linux::CacheProtectionTier::Active);
		}
		if (!presentationController_.AnchorTextureKey().empty()) {
			SetDisplayTextureProtection(presentationController_.AnchorTextureKey(),
				jpegview_linux::CacheProtectionTier::Active);
		}
		if (!presentationController_.PartnerTextureKey().empty()) {
			SetDisplayTextureProtection(presentationController_.PartnerTextureKey(),
				jpegview_linux::CacheProtectionTier::Active);
		}
		const std::size_t cacheCapacity = cacheBudget_->Capacity();
		plannerRequest.maximumPreparedBytes = cacheCapacity;
		if (CurrentImage().width > 0 && CurrentImage().height > 0) {
			const std::size_t width = static_cast<std::size_t>(CurrentImage().width);
			const std::size_t height = static_cast<std::size_t>(CurrentImage().height);
			const std::size_t maximum = std::numeric_limits<std::size_t>::max();
			if (width <= maximum / height && width * height <= maximum / 4) {
				std::size_t currentSourceBytes = width * height * 4;
				if (runtimeSettings_.Values().showHistogram && runtimeSettings_.Values().infoVisible) {
					const std::size_t spectrumBytes =
						sizeof(jpegview_linux::GrayscaleSpectrum);
					currentSourceBytes = currentSourceBytes <= maximum - spectrumBytes ?
						currentSourceBytes + spectrumBytes : maximum;
				}
				plannerRequest.maximumPreparedBytes = currentSourceBytes >= cacheCapacity ?
					0 : cacheCapacity - currentSourceBytes;
			} else {
				plannerRequest.maximumPreparedBytes = 0;
			}
		}
		const std::size_t maximumNeighborCount =
			jpegview_linux::DisplayPrefetchCandidateLimit(
				cacheCapacity, fileList_.Files().size());
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
					runtimeSettings_.Values().keepPictureLevels, runtimeSettings_.Values().autoContrast, runtimeSettings_.Values().defaultImageProcessing);
			jpegview_linux::ImageProcessingParams fileProcessing = filePreset.processing;
			fileProcessing.unsharpRadius = runtimeSettings_.Values().unsharpMaskRadius;
			fileProcessing.unsharpAmount = 0.0;
			fileProcessing.unsharpThreshold = runtimeSettings_.Values().unsharpMaskThreshold;
			batch->processingByFilename.emplace(filename.string(), fileProcessing);
			batch->autoContrastByFilename.emplace(filename.string(), filePreset.autoContrast);
			plannerRequest.neighbors.push_back({filename, source, fileIndex, position + 1,
				fileProcessing, filePreset.autoContrast,
				jpegview_linux::IsJpegPath(filename)});
		}
		displayPreparationController_.RequestPlan(std::move(plannerRequest));
		displayPrefetchBatch_ = batch;
		RefreshCacheProtectionSnapshotsForBatch();
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
					const auto source = batch->sourceByFilename.find(filename.string());
					if (source != batch->sourceByFilename.end()) {
						const auto retained =
							jpegview_linux::RetainDisplayPrefetchDecodedImage(
								batch->owner, decoded);
						if (retained) batch->decodedImages[filename.string()] =
							{source->second.Key(), retained};
					}
				}
				jpegview_linux::Viewport viewport;
				viewport.Restore(batch->context.viewport, frame.width, frame.height,
					batch->context.imageAreaWidth, batch->context.imageAreaHeight);
				const jpegview_linux::ViewportRect target = viewport.Destination(
					frame.width, frame.height, batch->context.imageAreaWidth,
					batch->context.imageAreaHeight);
				const jpegview_linux::DisplayImageTarget resolution =
					jpegview_linux::ClampDisplayImageTarget(frame.width, frame.height,
						target.width, target.height);
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
						resolution.width, resolution.height, autoContrast->second,
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
				if (!request.Valid()) return;
				const jpegview_linux::RetainedDisplayTexture* retainedTexture =
					jpegview_linux::FindReusableRetainedDisplayTexture(
						request.cacheKey, batch->retainedTextures);
				batch->gate.Publish([&] {
					{
						std::lock_guard<std::mutex> lock(batch->mutex);
						if (priority->second <= 2) {
							if (retainedTexture != nullptr) {
								batch->protectedTextureCacheKeys.push_back(
									retainedTexture->cacheKey);
								batch->protectedTextureKeys.push_back(retainedTexture->key);
							} else {
								batch->protectedTextureCacheKeys.push_back(request.cacheKey);
								batch->protectedTextureKeys.push_back(request.key);
							}
						}
					}
					if (retainedTexture != nullptr) return;
					// A late neighbor decode must not replace/cancel the active spread's
					// foreground pair requests in the display cache.
					if (batch->completionChannel) {
						batch->completionChannel->Publish(batch->generation, std::move(request));
					}
				});
			}, [](const fs::path& filename) {
				return !jpegview_linux::IsJpegPath(filename);
			}, 2, [this](std::size_t index) {
				return fileList_.DescriptorAt(index);
			});
	}

	const Image& CurrentImage() const {
		return imageDocument_.Presentation();
	}

	void ReplaceCurrentTexture(SDL_Texture* replacement) {
		ClearDisplayTexture();
		if (texture_ != nullptr) DestroyTextureMeasured(texture_);
		texture_ = replacement;
		if (!lastPresentedDisplayKey_.empty() &&
			lastPresentedDisplayKey_ != transitionDisplayKey_ &&
			lastPresentedDisplayKey_ != pendingTransitionFrame_.displayKey) {
			SetDisplayTextureProtection(lastPresentedDisplayKey_,
				jpegview_linux::CacheProtectionTier::DistantSpeculation);
			lastPresentedDisplayKey_.clear();
		}
	}

	bool UpdateTexture() {
		jpegview_linux::PerfContextScope workContext(
			jpegview_linux::PerfWorkClass::ActiveImageSpread,
			jpegview_linux::PerfExecution::EventThread);
		SDL_Texture* newTexture = CreateTexture(CurrentImage());
		if (newTexture == nullptr) {
			std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << '\n';
			return false;
		}
		ReplaceCurrentTexture(newTexture);
		return true;
	}

	SDL_Texture* CreateTexture(const Image& source) {
		return CreateTexture(source.bgra, source.width, source.height, source.hasTransparency);
	}

	SDL_Texture* CreateTexture(const std::vector<std::uint8_t>& bgra, int width, int height,
		bool hasTransparency = false) {
		return imageTextureOwner_.CreateAndUpload(bgra, width, height, hasTransparency);
	}

	void DestroyTextureMeasured(SDL_Texture* texture, std::size_t bytes = 0) {
		if (texture == nullptr) return;
		if (!imageTextureOwner_.Destroy(texture, bytes)) {
			std::cerr << "Viewer attempted to destroy a texture outside its image owner\n";
		}
	}

	void CancelPendingImageOperation() {
		if (!pendingImageOperation_.has_value()) return;
		ResumeImageOperationPlayback(*pendingImageOperation_);
		pendingImageOperation_.reset();
		imageOperationWorker_.Cancel();
	}

	void ResumeImageOperationPlayback(PendingImageOperation& pending) {
		if (!pending.pausedAnimationPlayback) return;
		pending.pausedAnimationPlayback = false;
		playback_.SetImageReady(true, SDL_GetTicks());
	}

	void ShowImageOperationWaitingForSelectedCommit(ImageOperationPurpose purpose) {
		if (purpose == ImageOperationPurpose::Save) {
			fileDialogMessage_ = "Waiting for selected image";
		} else if (purpose == ImageOperationPurpose::Resize) {
			resizeDialog_.SetMessage("Waiting for selected image");
		} else {
			SetTitle("Waiting for selected image before image operation");
		}
	}

	bool RequestImageOperation(const jpegview_linux::ImageOperationSpec& operation,
		ImageOperationPurpose purpose, int transformCommand = 0, bool fullSize = true,
		const fs::path& output = {},
		bool replayingMaterializationIntent = false) {
		if (fileList_.Empty()) return false;
		const jpegview_linux::PendingImageOperationAdmission admission =
			jpegview_linux::PlanPendingImageOperationAdmission(
				pendingImageOperation_.has_value(), PendingImageIntentCount(),
				currentSelectedLoadPending_, replayingMaterializationIntent);
		if (admission == jpegview_linux::PendingImageOperationAdmission::Rejected) return false;
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		if (!source.Valid() || source.Key() != imageDocument_.Source()) return false;
		imageDocument_.UpdateProcessing(imageProcessing_, autoContrastEnabled_);
		PendingImageOperation pending;
		pending.purpose = purpose;
		pending.operation = operation;
		pending.document = imageDocument_.Snapshot();
		pending.transformCommand = transformCommand;
		pending.fullSize = fullSize;
		pending.keepSpreadRotation = currentSpreadRotationValid_ || !imageModified_;
		pending.output = output;
		pending.overwriteConfirmed = purpose == ImageOperationPurpose::Save &&
			fileDialogOverwriteConfirmed_;
		pendingImageOperation_ = std::move(pending);
		if (admission ==
			jpegview_linux::PendingImageOperationAdmission::WaitForSelectedCommit) {
			ShowImageOperationWaitingForSelectedCommit(purpose);
			return true;
		}
		return StartPendingImageOperation();
	}

	bool StartPendingImageOperation() {
		if (!pendingImageOperation_.has_value()) return false;
		PendingImageOperation& pending = *pendingImageOperation_;
		if (pending.workerGeneration != 0) return true;
		if (!imageDocument_.Matches(pending.document)) {
			pendingImageOperation_.reset();
			return false;
		}
		if (currentSelectedLoadPending_) {
			ShowImageOperationWaitingForSelectedCommit(pending.purpose);
			return true;
		}

		jpegview_linux::DecodedImageCache::ImagePtr decoded = currentDecoded_;
		if (!decoded) {
			const jpegview_linux::SourceDescriptor source =
				SourceDescriptorForPath(fileList_.Current());
			if (source.Key() == pending.document.source) {
				decoded = imageCache_.Find(source);
				if (decoded) currentDecoded_ = decoded;
			}
		}
		if (!pending.document.sourcePixels && !decoded) {
			const jpegview_linux::SourceDescriptor source =
				SourceDescriptorForPath(fileList_.Current());
			if (source.Key() != pending.document.source) {
				pendingImageOperation_.reset();
				return false;
			}
			RequestSelectedSourceDecode(source, pending.document.ownerGeneration);
			if (pending.purpose == ImageOperationPurpose::Save) {
				fileDialogMessage_ = "Preparing source pixels";
			} else if (pending.purpose == ImageOperationPurpose::Resize) {
				resizeDialog_.SetMessage("Preparing source pixels");
			} else {
				SetTitle("Preparing source pixels for image operation");
			}
			return true;
		}
		if (imageDocument_.Animated() && playback_.AnimationPlaying()) {
			pending.pausedAnimationPlayback = true;
			playback_.SetImageReady(false, SDL_GetTicks());
		}
		jpegview_linux::ImageOperationRequest request;
		request.document = pending.document;
		request.decoded = std::move(decoded);
		request.operation = pending.operation;
		pending.workerGeneration = imageOperationWorker_.Request(std::move(request));
		if (pending.workerGeneration == 0) {
			ResumeImageOperationPlayback(pending);
			pendingImageOperation_.reset();
			return false;
		}
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
		if (pending.purpose == ImageOperationPurpose::Save) {
			fileDialogMessage_ = "Preparing image pixels";
		} else if (pending.purpose == ImageOperationPurpose::Resize) {
			resizeDialog_.SetMessage("Preparing image pixels");
		} else {
			SetTitle("Preparing image operation");
		}
		return true;
	}

	void ReportImageOperationFailure(ImageOperationPurpose purpose,
		const std::string& reason) {
		const std::string detail = reason.empty() ? "operation failed" : reason;
		switch (purpose) {
		case ImageOperationPurpose::Save:
			fileDialogMessage_ = "Cannot prepare image pixels: " + detail;
			break;
		case ImageOperationPurpose::Resize:
			resizeDialog_.SetMessage("Resizing failed: " + detail);
			break;
		case ImageOperationPurpose::Transform:
			SetTitle("Image transform failed: " + detail);
			break;
		case ImageOperationPurpose::Crop:
			SetTitle("Crop failed: " + detail);
			break;
		case ImageOperationPurpose::Reprocess:
			SetTitle("Picture-level processing failed: " + detail);
			break;
		case ImageOperationPurpose::CopyImage:
		case ImageOperationPurpose::CopySelection:
			SetTitle("Copy failed: " + detail);
			break;
		case ImageOperationPurpose::Print:
			SetTitle("Print failed: " + detail);
			break;
		case ImageOperationPurpose::Wallpaper:
			SetTitle("Set wallpaper failed: " + detail);
			break;
		case ImageOperationPurpose::Materialize:
			SetTitle("Image pixels could not be prepared: " + detail);
			break;
		}
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
	}

	bool OperationNeedsTexture(const PendingImageOperation& pending,
		const jpegview_linux::ImageOperationResult& result) const {
		if (pending.purpose == ImageOperationPurpose::Transform ||
			pending.purpose == ImageOperationPurpose::Crop ||
			pending.purpose == ImageOperationPurpose::Resize) return true;
		return pending.purpose == ImageOperationPurpose::Reprocess &&
			(result.modified || result.detached);
	}

	void TickImageOperation() {
		std::optional<jpegview_linux::ImageOperationResult> result =
			imageOperationWorker_.TakeReady();
		if (!result.has_value()) return;
		const auto replayLaterIntents = [this]() {
			if (!pendingMaterializationIntents_.empty()) {
				ReplayPendingMaterializationIntents();
			}
		};
		if (!pendingImageOperation_.has_value() ||
			pendingImageOperation_->workerGeneration != result->requestGeneration) {
			imageOperationWorker_.Retire(std::move(*result));
			replayLaterIntents();
			return;
		}
		PendingImageOperation pending = std::move(*pendingImageOperation_);
		pendingImageOperation_.reset();
		if (!result->success || !imageDocument_.Matches(result->expected)) {
			if (result->failure.kind != jpegview_linux::WorkerFailureKind::Cancelled) {
				ReportImageOperationFailure(pending.purpose, result->failure.message);
			}
			ResumeImageOperationPlayback(pending);
			imageOperationWorker_.Retire(std::move(*result));
			replayLaterIntents();
			return;
		}

		if (result->updatesDocument) {
			SDL_Texture* replacementTexture = nullptr;
			const bool needsTexture = OperationNeedsTexture(pending, *result);
			if (needsTexture) {
				jpegview_linux::PerfContextScope workContext(
					jpegview_linux::PerfWorkClass::ActiveImageSpread,
					jpegview_linux::PerfExecution::EventThread);
				replacementTexture = CreateTexture(*result->presentationPixels);
				if (replacementTexture == nullptr) {
					ReportImageOperationFailure(pending.purpose,
						"could not upload the completed image");
					ResumeImageOperationPlayback(pending);
					imageOperationWorker_.Retire(std::move(*result));
					replayLaterIntents();
					return;
				}
			}
			jpegview_linux::RetiredImageBuffers retired;
			if (!imageDocument_.Apply(*result, retired)) {
				if (replacementTexture != nullptr) DestroyTextureMeasured(replacementTexture);
				ResumeImageOperationPlayback(pending);
				imageOperationWorker_.Retire(std::move(*result));
				replayLaterIntents();
				return;
			}
			if (replacementTexture != nullptr) ReplaceCurrentTexture(replacementTexture);
			imageModified_ = imageDocument_.Modified();
			currentPixelsDetachedFromSource_ = imageDocument_.Detached();
			currentImageRotationQuarterTurns_ = imageDocument_.RotationQuarterTurns();
			const bool preserveLazySavePresentation =
				pending.purpose == ImageOperationPurpose::Save &&
				pending.operation.preserveDocumentPixels;
			if (!preserveLazySavePresentation) currentDisplayRequest_.reset();
			if (result->flattenAnimation) {
				playback_.ConfigureImage({}, 0, false, SDL_GetTicks());
				retired.decoded = std::move(currentDecoded_);
			}
			if (pending.purpose == ImageOperationPurpose::Transform) {
				const bool rotated = pending.transformCommand == IDM_ROTATE_90 ||
					pending.transformCommand == IDM_ROTATE_270;
				currentSpreadRotationValid_ = rotated && pending.keepSpreadRotation;
				ClearCropSelection();
				CancelImageSpectrumBeforeImageMutation();
				imageSession_.MarkDocumentChanged();
				RestoreScaleMode(viewport_.Snapshot());
			} else if (pending.purpose == ImageOperationPurpose::Crop ||
				pending.purpose == ImageOperationPurpose::Resize) {
				currentSpreadRotationValid_ = false;
				CancelImageSpectrumBeforeImageMutation();
				imageSession_.MarkDocumentChanged();
				if (pending.purpose == ImageOperationPurpose::Crop) ClearCropSelection();
				cropSelection_.SetImageSize(CurrentImage().width, CurrentImage().height);
				RestoreScaleMode(viewport_.Snapshot());
				if (pending.purpose == ImageOperationPurpose::Resize) CloseResizeDialog();
			} else if (pending.purpose == ImageOperationPurpose::Reprocess &&
				(result->modified || result->detached)) {
				CancelImageSpectrumBeforeImageMutation();
			}
			imageOperationWorker_.Retire(std::move(retired));
			RefreshDoublePageRenderState();
			if (pending.purpose == ImageOperationPurpose::Transform ||
				pending.purpose == ImageOperationPurpose::Crop ||
				pending.purpose == ImageOperationPurpose::Resize) SetTitle();
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		}
		if (!result->updatesDocument ||
			(pending.purpose == ImageOperationPurpose::Save &&
				pending.operation.preserveDocumentPixels)) {
			if (!result->outputPixels) {
				ReportImageOperationFailure(pending.purpose, "no output pixels were produced");
			} else {
				switch (pending.purpose) {
				case ImageOperationPurpose::Save:
					{
						const bool deferPlaybackResume =
							pending.operation.preserveDocumentPixels && pending.document.animated &&
							pending.pausedAnimationPlayback;
						const bool queued = CompleteImageSave(pending.output, result->outputPixels,
							result->outputReservation.ShareAlias(),
							pending.overwriteConfirmed,
							pending.operation.preserveDocumentPixels,
							pending.operation.preserveDocumentPixels && pending.document.animated,
							deferPlaybackResume);
						if (queued && deferPlaybackResume) {
							pending.pausedAnimationPlayback = false;
						}
					}
					break;
				case ImageOperationPurpose::CopyImage:
					CopyPreparedImage(result->outputPixels,
						result->outputReservation.ShareAlias(), pending.fullSize, false);
					break;
				case ImageOperationPurpose::CopySelection:
					CopyPreparedImage(result->outputPixels,
						result->outputReservation.ShareAlias(), true, true);
					break;
				case ImageOperationPurpose::Print:
					PrintPreparedImage(result->outputPixels,
						result->outputReservation.ShareAlias());
					break;
				case ImageOperationPurpose::Wallpaper:
					SetWallpaperFromPreparedImage(result->outputPixels,
						result->outputReservation.ShareAlias());
					break;
				default:
					break;
				}
			}
		}
		ResumeImageOperationPlayback(pending);
		imageOperationWorker_.Retire(std::move(*result));
		replayLaterIntents();
	}

	SDL_Texture* CreateIncompleteDisplayTexture(
		const jpegview_linux::PreparedDisplayImage& prepared) {
		if (prepared.width <= 0 || prepared.height <= 0 ||
			prepared.bgra.size() != jpegview_linux::PreparedDisplayImageBytes(prepared)) {
			return nullptr;
		}
		return imageTextureOwner_.CreateEmpty(prepared.width, prepared.height);
	}

	void QueueRendererTextureRetirement(SDL_Texture* texture, std::size_t bytes,
		jpegview_linux::PerfWorkClass workClass,
		jpegview_linux::CacheReservation reservation,
		bool countsTowardSpeculativeLimit) {
		if (texture == nullptr) return;
		RetiredDisplayTexture retired;
		retired.texture = texture;
		retired.bytes = bytes;
		retired.workClass = workClass;
		retired.reservation = std::move(reservation);
		retired.countsTowardSpeculativeLimit = countsTowardSpeculativeLimit;
		try {
			retiredDisplayTextures_.push_back(std::move(retired));
			if (countsTowardSpeculativeLimit) retiredSpeculativeDisplayTextureBytes_ += bytes;
		} catch (...) {
			jpegview_linux::PerfContextScope workContext(workClass,
				jpegview_linux::PerfExecution::EventThread);
			DestroyTextureMeasured(retired.texture, bytes);
			if (countsTowardSpeculativeLimit) {
				speculativeDisplayTextureBytes_ -= std::min(
					bytes, speculativeDisplayTextureBytes_);
			}
			retired.reservation.Reset();
		}
	}

	void RetireIncompleteDisplayTexture(PendingTextureUpload& pending) {
		if (pending.incompleteTexture == nullptr) return;
		const std::size_t bytes = pending.textureReservation.Bytes();
		const bool wasSpeculative = pending.countsTowardSpeculativeLimit;
		QueueRendererTextureRetirement(pending.incompleteTexture, bytes,
			pending.priority.workClass, std::move(pending.textureReservation),
			wasSpeculative);
		pending.incompleteTexture = nullptr;
		pending.countsTowardSpeculativeLimit = false;
		pending.textureActiveWorking = false;
	}

	void DestroyRetiredDisplayTexture(RetiredDisplayTexture& retired) {
		if (retired.texture != nullptr) {
			jpegview_linux::PerfContextScope workContext(retired.workClass,
				jpegview_linux::PerfExecution::EventThread);
			DestroyTextureMeasured(retired.texture, retired.bytes);
			retired.texture = nullptr;
		}
		if (retired.countsTowardSpeculativeLimit) {
			speculativeDisplayTextureBytes_ -= std::min(
				retired.bytes, speculativeDisplayTextureBytes_);
			retiredSpeculativeDisplayTextureBytes_ -= std::min(
				retired.bytes, retiredSpeculativeDisplayTextureBytes_);
			retired.countsTowardSpeculativeLimit = false;
		}
		retired.reservation.Reset();
	}

	void TickRendererTextureRetirement() {
		const std::size_t retireCount = jpegview_linux::DisplayTextureRetirementsForTick(
			retiredDisplayTextures_.size(),
			CurrentInteractionWorkPlan().interactionActive,
			kDisplayTextureRetirementsPerTick,
			kDisplayTextureRetirementInteractionPressureThreshold);
		for (std::size_t retiredCount = 0; retiredCount < retireCount; ++retiredCount) {
			RetiredDisplayTexture retired = std::move(retiredDisplayTextures_.front());
			retiredDisplayTextures_.pop_front();
			DestroyRetiredDisplayTexture(retired);
		}
	}

	void DrainRetiredDisplayTextures() {
		while (!retiredDisplayTextures_.empty()) {
			RetiredDisplayTexture retired = std::move(retiredDisplayTextures_.front());
			retiredDisplayTextures_.pop_front();
			DestroyRetiredDisplayTexture(retired);
		}
	}

	void ClearDisplayTextureCache() {
		ClearPendingTextureUploads();
		while (!displayTextureCache_.empty()) EraseDisplayTexture(displayTextureCache_.begin());
		for (auto& list : displayTextureLru_) list.clear();
		displayTextureProtectedKeys_.clear();
		displayTextureActiveKeys_.clear();
		displayTextureCacheBytes_ = 0;
		lastPresentedDisplayKey_.clear();
		failedCurrentDisplayKey_.clear();
		displayImageCache_.Clear();
	}

	void EraseDisplayTexture(
		std::unordered_map<std::string, DisplayTextureCacheEntry>::iterator entry) {
		if (entry == displayTextureCache_.end()) return;
		if (entry->first == lastPresentedDisplayKey_) lastPresentedDisplayKey_.clear();
		const jpegview_linux::CacheProtectionTier protection = entry->second.protection;
		const std::size_t bytes = entry->second.bytes;
		QueueRendererTextureRetirement(entry->second.texture, bytes,
			protection == jpegview_linux::CacheProtectionTier::Active ?
				jpegview_linux::PerfWorkClass::ActiveImageSpread :
			(protection == jpegview_linux::CacheProtectionTier::Neighbor ?
				jpegview_linux::PerfWorkClass::NearestNavigationNeighbor :
				jpegview_linux::PerfWorkClass::DistantSpeculation),
			std::move(entry->second.reservation),
			protection != jpegview_linux::CacheProtectionTier::Active);
		if (!entry->second.activeWorking) {
			displayTextureCacheBytes_ -= bytes;
		}
		displayTextureLru_[static_cast<std::size_t>(entry->second.protection)].erase(
			entry->second.lru);
		displayTextureCache_.erase(entry);
	}

	void ClearActiveWorkingDisplayTextures() {
		for (auto entry = displayTextureCache_.begin();
			entry != displayTextureCache_.end();) {
			if (!entry->second.activeWorking || IsPinnedDisplayTexture(entry->first)) {
				++entry;
				continue;
			}
			auto retiring = entry++;
			EraseDisplayTexture(retiring);
		}
	}

	SDL_Texture* FindDisplayTexture(const std::string& key) {
		const auto found = displayTextureCache_.find(key);
		if (found == displayTextureCache_.end()) return nullptr;
		auto& list = displayTextureLru_[static_cast<std::size_t>(found->second.protection)];
		list.splice(list.end(), list, found->second.lru);
		return found->second.texture;
	}

	SDL_Texture* PeekDisplayTexture(const std::string& key) const {
		const auto found = displayTextureCache_.find(key);
		return found == displayTextureCache_.end() ? nullptr : found->second.texture;
	}

	void SetDisplayTextureProtection(const std::string& key,
		jpegview_linux::CacheProtectionTier protection) {
		if (key.empty()) return;
		displayTextureProtectedKeys_.erase(key);
		displayTextureActiveKeys_.erase(key);
		if (protection == jpegview_linux::CacheProtectionTier::Neighbor) {
			displayTextureProtectedKeys_.insert(key);
		} else if (protection == jpegview_linux::CacheProtectionTier::Active) {
			displayTextureActiveKeys_.insert(key);
		}
		MoveDisplayTextureToTier(key, protection);
	}

	void MoveDisplayTextureToTier(const std::string& key,
		jpegview_linux::CacheProtectionTier protection) {
		auto found = displayTextureCache_.find(key);
		if (found == displayTextureCache_.end() || found->second.protection == protection) return;
		const bool wasSpeculative = found->second.protection !=
			jpegview_linux::CacheProtectionTier::Active;
		bool becomesSpeculative = protection !=
			jpegview_linux::CacheProtectionTier::Active;
		if (!wasSpeculative && becomesSpeculative &&
			!jpegview_linux::CanRetainSpeculativeDisplayTexture(
				speculativeDisplayTextureBytes_, found->second.bytes,
				jpegview_linux::SpeculativeDisplayTextureBudgetBytes(
					cacheBudget_->Capacity()))) {
			if (IsPinnedDisplayTexture(key)) {
				protection = jpegview_linux::CacheProtectionTier::Active;
				becomesSpeculative = false;
				displayTextureProtectedKeys_.erase(key);
				displayTextureActiveKeys_.insert(key);
			} else {
				EraseDisplayTexture(found);
				return;
			}
		}
		if (wasSpeculative != becomesSpeculative) {
			if (becomesSpeculative) {
				speculativeDisplayTextureBytes_ += found->second.bytes;
			} else {
				speculativeDisplayTextureBytes_ -= std::min(
					found->second.bytes, speculativeDisplayTextureBytes_);
			}
		}
		displayTextureLru_[static_cast<std::size_t>(found->second.protection)].erase(
			found->second.lru);
		auto& list = displayTextureLru_[static_cast<std::size_t>(protection)];
		list.push_back(key);
		found->second.lru = std::prev(list.end());
		found->second.protection = protection;
	}

	void ApplyDisplayTextureProtectionSnapshot(
		const std::vector<std::pair<std::string,
			jpegview_linux::CacheProtectionTier>>& protections) {
		std::unordered_map<std::string, jpegview_linux::CacheProtectionTier> strongest;
		for (const auto& protection : protections) {
			if (protection.first.empty()) continue;
			auto inserted = strongest.emplace(protection.first, protection.second);
			if (!inserted.second && static_cast<unsigned>(protection.second) >
				static_cast<unsigned>(inserted.first->second)) {
				inserted.first->second = protection.second;
			}
		}
		const auto preserveActive = [&strongest](const std::string& key) {
			if (!key.empty()) strongest[key] = jpegview_linux::CacheProtectionTier::Active;
		};
		preserveActive(transitionDisplayKey_);
		preserveActive(pendingTransitionFrame_.displayKey);
		preserveActive(transitionCaptureDisplayKey_);
		if (LastPresentedTextureMatchesCurrentSource()) {
			preserveActive(lastPresentedDisplayKey_);
		}

		std::vector<std::pair<std::string,
			jpegview_linux::CacheProtectionTier>> changes;
		for (std::size_t tier = 0; tier < displayTextureLru_.size(); ++tier) {
			const std::vector<std::string> keys(displayTextureLru_[tier].begin(),
				displayTextureLru_[tier].end());
			for (const std::string& key : keys) {
				const auto cached = displayTextureCache_.find(key);
				if (cached == displayTextureCache_.end()) continue;
				const auto desired = strongest.find(key);
				const jpegview_linux::CacheProtectionTier protection = desired == strongest.end() ?
					jpegview_linux::CacheProtectionTier::DistantSpeculation : desired->second;
				if (cached->second.protection != protection) changes.emplace_back(key, protection);
			}
		}
		for (const auto& change : changes) MoveDisplayTextureToTier(change.first, change.second);

		displayTextureProtectedKeys_.clear();
		displayTextureActiveKeys_.clear();
		for (const auto& protection : strongest) {
			if (protection.second == jpegview_linux::CacheProtectionTier::Neighbor) {
				displayTextureProtectedKeys_.insert(protection.first);
			} else if (protection.second == jpegview_linux::CacheProtectionTier::Active) {
				displayTextureActiveKeys_.insert(protection.first);
			}
		}
	}

	void ClearDisplayTextureProtections() {
		ApplyDisplayTextureProtectionSnapshot({});
	}

	bool EvictOldestDisplayTexture(jpegview_linux::CacheProtectionTier maximumTier) {
		for (std::size_t tier = 0; tier <= static_cast<std::size_t>(maximumTier); ++tier) {
			auto& list = displayTextureLru_[tier];
			for (auto lru = list.begin(); lru != list.end(); ++lru) {
				const auto oldest = displayTextureCache_.find(*lru);
				if (oldest == displayTextureCache_.end() || oldest->second.activeWorking ||
					IsPinnedDisplayTexture(*lru)) continue;
				EraseDisplayTexture(oldest);
				return true;
			}
		}
		return false;
	}

	jpegview_linux::CacheReservation ReserveDisplayTextureBytes(std::size_t bytes,
		jpegview_linux::CacheProtectionTier incomingProtection, bool mayEvict) {
		return cacheAdmission_.Reserve(bytes,
			jpegview_linux::CacheMemoryCategory::RetainedImageTextures, {},
			incomingProtection,
			[this](jpegview_linux::CacheProtectionTier tier) {
				if (imageCache_.EvictLeastRecentlyUsed(tier) != 0) return true;
				if (displayImageCache_.EvictLeastRecentlyUsed(tier) != 0) return true;
				return EvictOldestDisplayTexture(tier);
			}, mayEvict);
	}

	jpegview_linux::CacheProtectionTier TextureProtectionFor(
		const jpegview_linux::DisplayImageCache::ImagePtr& prepared,
		jpegview_linux::PerfWorkClass workClass) const {
		if (displayTextureActiveKeys_.find(prepared->key) != displayTextureActiveKeys_.end() ||
			workClass == jpegview_linux::PerfWorkClass::ActiveImageSpread) {
			return jpegview_linux::CacheProtectionTier::Active;
		}
		if (displayTextureProtectedKeys_.find(prepared->key) !=
			displayTextureProtectedKeys_.end() || workClass ==
			jpegview_linux::PerfWorkClass::NearestNavigationNeighbor) {
			return jpegview_linux::CacheProtectionTier::Neighbor;
		}
		return jpegview_linux::CacheProtectionForWorkClass(workClass);
	}

	void ClearPendingTextureUploads() {
		for (PendingTextureUpload& pending : pendingTextureUploads_) {
			DiscardPendingTextureUpload(pending);
		}
		pendingTextureUploads_.clear();
	}

	void SetPendingTextureProtection(PendingTextureUpload& pending,
		jpegview_linux::CacheProtectionTier protection) {
		pending.textureProtection = protection;
		if (pending.incompleteTexture == nullptr) return;
		const bool speculative = protection !=
			jpegview_linux::CacheProtectionTier::Active;
		if (pending.countsTowardSpeculativeLimit == speculative) return;
		const std::size_t bytes = pending.textureReservation.Bytes();
		if (speculative) {
			speculativeDisplayTextureBytes_ += bytes;
		} else {
			speculativeDisplayTextureBytes_ -= std::min(
				bytes, speculativeDisplayTextureBytes_);
		}
		pending.countsTowardSpeculativeLimit = speculative;
	}

	void DiscardPendingTextureUpload(PendingTextureUpload& pending) {
		RetireIncompleteDisplayTexture(pending);
		if (pending.image) displayImageCache_.Retire(pending.image);
		pending.image.reset();
	}

	jpegview_linux::DisplayUploadPriority TextureUploadPriority(
		const PendingTextureUpload& upload) const {
		return upload.priority;
	}

	void StagePendingTextureUpload(PendingTextureUpload& pending) {
		if (!pending.image || pending.uploadStaged) return;
		displayImageCache_.ReleaseForUpload(pending.image);
		pending.uploadStaged = true;
	}

	void QueuePendingTextureUpload(PendingTextureUpload pending) {
		if (!pending.image) {
			RetireIncompleteDisplayTexture(pending);
			return;
		}
		const auto duplicate = std::find_if(pendingTextureUploads_.begin(),
			pendingTextureUploads_.end(), [&pending](const PendingTextureUpload& queued) {
			return queued.image && queued.image->key == pending.image->key;
		});
		if (duplicate != pendingTextureUploads_.end()) {
			if (pending.selectionGeneration != 0) {
				duplicate->selectionGeneration = pending.selectionGeneration;
			}
			const bool promoted = jpegview_linux::MergeDisplayUploadPriority(
				duplicate->priority, pending.priority);
			if (duplicate->incompleteTexture == nullptr &&
				pending.incompleteTexture != nullptr) {
				duplicate->incompleteTexture = pending.incompleteTexture;
				pending.incompleteTexture = nullptr;
				duplicate->uploadPlan = std::move(pending.uploadPlan);
				duplicate->textureReservation = std::move(pending.textureReservation);
				duplicate->textureProtection = pending.textureProtection;
				duplicate->textureActiveWorking = pending.textureActiveWorking;
				duplicate->countsTowardSpeculativeLimit =
					pending.countsTowardSpeculativeLimit;
				pending.countsTowardSpeculativeLimit = false;
			}
			if (promoted) {
				duplicate->attempted = false;
				duplicate->lastAttemptRetainedCapacityRevision = 0;
			}
			if (duplicate->incompleteTexture != nullptr) {
				SetPendingTextureProtection(*duplicate,
					TextureProtectionFor(duplicate->image, duplicate->priority.workClass));
			}
			DiscardPendingTextureUpload(pending);
			return;
		}
		if (pendingTextureUploads_.size() >= kMaximumPendingTextureUploads) {
			auto worst = pendingTextureUploads_.begin();
			for (auto candidate = std::next(worst);
				candidate != pendingTextureUploads_.end(); ++candidate) {
				if (candidate->image && worst->image &&
					jpegview_linux::DisplayUploadHasHigherPriority(
						TextureUploadPriority(*worst),
						TextureUploadPriority(*candidate))) worst = candidate;
			}
			if (worst == pendingTextureUploads_.end() || !worst->image ||
				!jpegview_linux::DisplayUploadHasHigherPriority(
					TextureUploadPriority(pending), TextureUploadPriority(*worst))) {
				DiscardPendingTextureUpload(pending);
				return;
			}
			DiscardPendingTextureUpload(*worst);
			pendingTextureUploads_.erase(worst);
		}
		auto insertion = std::find_if(pendingTextureUploads_.begin(),
			pendingTextureUploads_.end(), [this, &pending](const PendingTextureUpload& queued) {
				return queued.image && jpegview_linux::DisplayUploadHasHigherPriority(
					TextureUploadPriority(pending), TextureUploadPriority(queued));
		});
		StagePendingTextureUpload(pending);
		try {
			pendingTextureUploads_.insert(insertion, std::move(pending));
		} catch (...) {
			DiscardPendingTextureUpload(pending);
		}
	}

	TextureCacheOutcome CacheDisplayTexture(
		PendingTextureUpload& pending,
		bool mayEvict = true,
		std::uint64_t* retainedRevisionBeforeAdmission = nullptr) {
		const jpegview_linux::DisplayImageCache::ImagePtr prepared = pending.image;
		if (!prepared || prepared->key.empty()) return TextureCacheOutcome::Failed;
		jpegview_linux::PerfContextScope workContext(pending.priority.workClass,
			jpegview_linux::PerfExecution::EventThread);
		if (!pending.thumbnailQueued && prepared->rotationQuarterTurns == 0) {
			QueuePreparedThumbnail(prepared);
			pending.thumbnailQueued = true;
		}
		if (FindDisplayTexture(prepared->key) != nullptr) {
			RetireIncompleteDisplayTexture(pending);
			displayImageCache_.ReleaseForActiveUse(prepared);
			return TextureCacheOutcome::Cached;
		}
		const std::size_t bytes = jpegview_linux::PreparedDisplayImageBytes(*prepared);
		if (bytes == 0) {
			displayImageCache_.ReleaseForActiveUse(prepared);
			return TextureCacheOutcome::Failed;
		}
		if (retainedRevisionBeforeAdmission != nullptr) {
			*retainedRevisionBeforeAdmission = cacheBudget_->RetainedCapacityRevision();
		}
		if (pending.incompleteTexture == nullptr) {
			// Keep the prepared allocation charged as staging while retained-cache
			// admission may evict other owners and while SDL copies the pixels.
			StagePendingTextureUpload(pending);
			const jpegview_linux::CacheProtectionTier protection =
				TextureProtectionFor(prepared, pending.priority.workClass);
			if (protection != jpegview_linux::CacheProtectionTier::Active) {
				const std::size_t speculativeLimit =
					jpegview_linux::SpeculativeDisplayTextureBudgetBytes(
						cacheBudget_->Capacity());
				if (!jpegview_linux::CanRetainSpeculativeDisplayTexture(
						speculativeDisplayTextureBytes_, bytes, speculativeLimit)) {
					if (bytes > speculativeLimit) return TextureCacheOutcome::NotRetained;
					if (protection == jpegview_linux::CacheProtectionTier::Neighbor &&
						(EvictOldestDisplayTexture(protection) ||
							retiredSpeculativeDisplayTextureBytes_ != 0)) {
						return TextureCacheOutcome::Deferred;
					}
					return TextureCacheOutcome::NotRetained;
				}
			}
			if (bytes <= cacheBudget_->Capacity()) {
				pending.textureReservation = ReserveDisplayTextureBytes(
					bytes, protection, mayEvict);
			}
			if (!pending.textureReservation && pending.priority.workClass ==
				jpegview_linux::PerfWorkClass::ActiveImageSpread) {
				pending.textureReservation = cacheBudget_->TrackTemporary(bytes,
					jpegview_linux::CacheMemoryCategory::ActiveWorkingData);
				pending.textureActiveWorking =
					static_cast<bool>(pending.textureReservation);
			}
			if (!pending.textureReservation) {
				const jpegview_linux::CacheBudgetSnapshot snapshot = cacheBudget_->Snapshot();
				jpegview_linux::PerfDiagnostics::Instance().RecordText(
					jpegview_linux::PerfMetric::CacheSnapshot, bytes, cacheBudget_->Available(),
					snapshot.retainedBytes, static_cast<std::uint64_t>(protection),
					snapshot.releaseRevision, 0, "texture_admission_deferred");
				return TextureCacheOutcome::Deferred;
			}
			jpegview_linux::DisplayTextureUploadPlan uploadPlan(prepared->width,
				prepared->height, bytes, kMaximumSynchronousDisplayTextureUploadBytes,
				kDisplayTextureUploadBandBytes);
			if (!uploadPlan.Valid()) return TextureCacheOutcome::Failed;
			SDL_Texture* incomplete = CreateIncompleteDisplayTexture(*prepared);
			if (incomplete == nullptr) return TextureCacheOutcome::Failed;
			pending.incompleteTexture = incomplete;
			pending.uploadPlan = std::move(uploadPlan);
			SetPendingTextureProtection(pending, protection);
		}

		const std::optional<jpegview_linux::DisplayTextureUploadBand> band =
			pending.uploadPlan.CurrentBand();
		if (!band.has_value()) return TextureCacheOutcome::Failed;
		SDL_Rect destination{0, band->y, prepared->width, band->height};
		const std::size_t rowBytes = static_cast<std::size_t>(prepared->width) * 4;
		const std::size_t offset = static_cast<std::size_t>(band->y) * rowBytes;
		const std::size_t bandBytes = static_cast<std::size_t>(band->height) * rowBytes;
		int updateResult = 0;
		{
			jpegview_linux::PerfScopedTimer uploadTimer(
				jpegview_linux::PerfDiagnostics::Instance(),
				jpegview_linux::PerfMetric::TextureUpload, bandBytes,
				static_cast<std::uint64_t>(prepared->width),
				static_cast<std::uint64_t>(band->height));
			updateResult = SDL_UpdateTexture(pending.incompleteTexture, &destination,
				prepared->bgra.data() + offset, prepared->width * 4);
		}
		if (updateResult != 0) {
			std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << '\n';
			pending.uploadPlan.Fail();
			RetireIncompleteDisplayTexture(pending);
			return TextureCacheOutcome::Failed;
		}
		if (!pending.uploadPlan.MarkCurrentBandUploaded()) {
			pending.uploadPlan.Fail();
			RetireIncompleteDisplayTexture(pending);
			return TextureCacheOutcome::Failed;
		}
		if (!pending.uploadPlan.Complete()) return TextureCacheOutcome::Deferred;
		if (SDL_SetTextureBlendMode(pending.incompleteTexture,
			prepared->hasTransparency ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE) != 0) {
			std::cerr << "SDL_SetTextureBlendMode failed: " << SDL_GetError() << '\n';
			RetireIncompleteDisplayTexture(pending);
			return TextureCacheOutcome::Failed;
		}

		const jpegview_linux::CacheProtectionTier protection = pending.textureProtection;
		auto& list = displayTextureLru_[static_cast<std::size_t>(protection)];
		std::list<std::string>::iterator lruPosition;
		bool lruInserted = false;
		try {
			list.push_back(prepared->key);
			lruPosition = std::prev(list.end());
			lruInserted = true;
			DisplayTextureCacheEntry entry{pending.incompleteTexture, bytes,
				prepared->source.Key(), prepared->cacheKey, prepared->width,
				prepared->height, prepared->hasTransparency, prepared->spectrum,
				pending.textureActiveWorking, protection,
				pending.textureReservation.ShareAlias(), lruPosition};
			const auto inserted = displayTextureCache_.emplace(prepared->key,
				std::move(entry));
			if (!inserted.second) {
				list.erase(lruPosition);
				lruInserted = false;
				RetireIncompleteDisplayTexture(pending);
				displayImageCache_.ReleaseForActiveUse(prepared);
				return TextureCacheOutcome::Cached;
			}
		} catch (...) {
			if (lruInserted) list.erase(lruPosition);
			RetireIncompleteDisplayTexture(pending);
			return TextureCacheOutcome::Failed;
		}
		pending.textureReservation.Reset();
		pending.incompleteTexture = nullptr;
		pending.countsTowardSpeculativeLimit = false;
		if (!pending.textureActiveWorking) displayTextureCacheBytes_ += bytes;
		if (pending.textureActiveWorking) {
			for (auto entry = displayTextureCache_.begin();
				entry != displayTextureCache_.end();) {
				if (entry->first == prepared->key || !entry->second.activeWorking ||
					entry->second.source != prepared->source.Key() ||
					IsPinnedDisplayTexture(entry->first)) {
					++entry;
					continue;
				}
				auto retiring = entry++;
				EraseDisplayTexture(retiring);
			}
		}
		return TextureCacheOutcome::Cached;
	}

	void TickDisplayTexturePreload() {
		RefreshCacheProtectionSnapshotsForBatch();
		for (const jpegview_linux::DisplayImageFailureInfo& failure :
			displayImageCache_.TakeFailedCompletions()) {
			if (failure.cacheKey.includeSpectrum &&
				currentSourceSpectrumRequestKey_.has_value()) {
				const std::optional<jpegview_linux::ImageSpectrumKey> currentKey =
					CurrentImageSpectrumKey();
				if (currentKey.has_value() && !UsesEditedImageSpectrum() &&
					*currentSourceSpectrumRequestKey_ == *currentKey &&
					failure.cacheKey.source == currentKey->source &&
					failure.cacheKey.frameIndex == currentKey->frameIndex &&
					failure.cacheKey.rotationQuarterTurns == currentKey->rotationQuarterTurns &&
					failure.cacheKey.autoContrast == currentKey->autoContrast &&
					jpegview_linux::EqualEffectiveImageProcessingParams(failure.cacheKey.processing,
						currentKey->processing)) {
					currentSourceSpectrumRequestKey_.reset();
					currentSourceSpectrumDisplayCacheKey_.clear();
					if (failure.failure.kind != jpegview_linux::WorkerFailureKind::Cancelled) {
						currentSourceSpectrumUnavailableKey_ = *currentKey;
					}
					++imageSpectrumPresentationRevision_;
					frameInvalidator_.Mark(
						jpegview_linux::FrameInvalidationReason::Overlay);
				}
			}
			if (currentSelectedLoadPending_ &&
				failure.key != pendingSelectedDisplayKey_) continue;
			if (!currentDisplayRequest_.has_value() ||
				failure.key != currentDisplayRequest_->key) continue;
			HandleCurrentDisplayFailure(failure.selectionGeneration,
				currentDisplayRequest_->source.Key(), failure.key,
				failure.failure.message);
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		}
		const jpegview_linux::InteractionWorkPlan workPlan = CurrentInteractionWorkPlan();
		const std::size_t uploadLimit =
			presentationController_.Phase() ==
				jpegview_linux::DoublePagePresentationPhase::PreparingSpread ? 2 :
				kDisplayTextureUploadsPerTick;
		struct UploadCandidate {
			PendingTextureUpload pending;
		};
		std::vector<UploadCandidate> candidates;
		std::deque<PendingTextureUpload> notPermitted;
		while (!pendingTextureUploads_.empty()) {
			PendingTextureUpload pending = std::move(pendingTextureUploads_.front());
			pendingTextureUploads_.pop_front();
			if (!pending.image || !workPlan.Allows(pending.priority.workClass)) {
				notPermitted.push_back(std::move(pending));
				continue;
			}
			candidates.push_back({std::move(pending)});
		}
		pendingTextureUploads_ = std::move(notPermitted);

		jpegview_linux::DisplayImageCompletionBatch readyBatch(displayImageCache_,
			displayImageCache_.TakeCompletedWithMetadata(
				uploadLimit, workPlan.permittedWorkClasses));
		for (std::size_t index = 0; index < readyBatch.Size(); ++index) {
			jpegview_linux::DisplayImageCompletionInfo completion =
				readyBatch.TakeCompletion(index);
			if (completion.image) {
				CapturePreparedSourceSpectrum(*completion.image);
				PendingTextureUpload pending;
				pending.image = std::move(completion.image);
				pending.priority = {completion.workClass, completion.priority};
				pending.lastAttemptRetainedCapacityRevision =
					cacheBudget_->RetainedCapacityRevision();
				pending.selectionGeneration = completion.selectionGeneration;
				candidates.push_back({std::move(pending)});
			}
		}
		std::vector<jpegview_linux::DisplayUploadPriority> priorities;
		priorities.reserve(candidates.size());
		for (const UploadCandidate& candidate : candidates) {
			priorities.push_back(TextureUploadPriority(candidate.pending));
		}
		const std::vector<std::size_t> order = jpegview_linux::PlanDisplayTextureUploads(
			priorities, workPlan.permittedWorkClasses, uploadLimit, 1);
		std::vector<bool> selected(candidates.size(), false);
		for (const std::size_t index : order) selected[index] = true;
		std::vector<jpegview_linux::DisplayImageCache::ImagePtr> newlyDeferredImages;
		for (std::size_t index = 0; index < candidates.size(); ++index) {
			if (!selected[index] && candidates[index].pending.image &&
				!candidates[index].pending.uploadStaged) {
				newlyDeferredImages.push_back(candidates[index].pending.image);
			}
		}
		// Release every unselected completion's retained-frame charge before a
		// selected foreground upload can try to evict that still-aliased frame.
		displayImageCache_.ReleaseForUpload(newlyDeferredImages);
		for (std::size_t index = 0; index < candidates.size(); ++index) {
			if (!selected[index] && candidates[index].pending.image) {
				candidates[index].pending.uploadStaged = true;
			}
		}
		for (const std::size_t index : order) {
			UploadCandidate& candidate = candidates[index];
			PendingTextureUpload pending = std::move(candidate.pending);
			const auto prepared = pending.image;
			if (!prepared) continue;
			if (pending.selectionGeneration != 0 &&
				!IsCurrentSelectedDisplayRequest(pending.selectionGeneration,
					prepared->source.Key(), prepared->key)) {
				DiscardPendingTextureUpload(pending);
				continue;
			}
			std::uint64_t retainedRevision = cacheBudget_->RetainedCapacityRevision();
			const bool mayEvict = !pending.attempted ||
				retainedRevision > pending.lastAttemptRetainedCapacityRevision ||
				cacheBudget_->Available() >=
					jpegview_linux::PreparedDisplayImageBytes(*prepared);
			const TextureCacheOutcome outcome = CacheDisplayTexture(
				pending, mayEvict,
				&retainedRevision);
			if (outcome == TextureCacheOutcome::Deferred) {
				pending.lastAttemptRetainedCapacityRevision = retainedRevision;
				pending.attempted = true;
				pending.uploadStaged = true;
				QueuePendingTextureUpload(std::move(pending));
			} else if (outcome == TextureCacheOutcome::Cached) {
				presentationController_.MarkTextureReady(prepared->key);
				frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
				if (pending.selectionGeneration != 0) {
					MarkSelectedDisplayFrameReady(pending.selectionGeneration,
						prepared->source.Key(), prepared->key);
				}
			} else if (outcome == TextureCacheOutcome::NotRetained) {
				DiscardPendingTextureUpload(pending);
			} else {
				DiscardPendingTextureUpload(pending);
				HandleCurrentDisplayFailure(pending.selectionGeneration,
					prepared->source.Key(), prepared->key,
					"display texture upload failed");
				if (presentationController_.MarkTextureFailed(prepared->key)) {
					CancelPendingDoublePageRequests();
					doublePagePartnerRequest_.reset();
					deferredCurrentDisplayPreparation_ = true;
				}
				frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
			}
		}
		for (std::size_t index = 0; index < candidates.size(); ++index) {
			if (selected[index]) continue;
			UploadCandidate& candidate = candidates[index];
			QueuePendingTextureUpload(std::move(candidate.pending));
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
		if (!changed.empty()) {
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		}
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
			jpegview_linux::RefreshFileListSource(fileList_, previous, observed, true);
		const bool ownedInPlaceSave = pendingFileOperation_.has_value() &&
			pendingFileOperation_->kind == jpegview_linux::FileOperationKind::SaveImage &&
			pendingFileOperation_->inPlaceSave &&
			FileOperationOwnerStillCurrent(*pendingFileOperation_) &&
			!fileList_.Empty() && AbsoluteNormalized(fileList_.Current()) == changedPath &&
			observed.Valid();
		if (!refresh.applied) return false;
		const jpegview_linux::SourceRefreshDisplayAction displayAction =
			jpegview_linux::ResolveSourceRefreshDisplayAction(refresh,
				preserveCurrentPixels || ownedInPlaceSave);
		const bool orderChanged = refresh.orderChanged;
		bool spreadAffected = orderChanged || refresh.sortKeyChanged;
		if (refresh.previousIndex.has_value()) {
			const std::size_t changedIndex = *refresh.previousIndex;
			if (activeDoublePageRender_.has_value() &&
				jpegview_linux::DoublePageSpreadAffectedBySourceRefresh(
					activeDoublePageRender_->layout, changedIndex, orderChanged)) {
				spreadAffected = true;
			}
			if (presentationController_.Phase() !=
					jpegview_linux::DoublePagePresentationPhase::SinglePage &&
				(presentationController_.AnchorIndex() == changedIndex ||
					presentationController_.PartnerIndex() == changedIndex)) {
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
		if (orderChanged || refresh.sortKeyChanged) prefetchAffected = true;
		if (refresh.sortKeyChanged) {
			BeginFileListSort(fileList_.GetSorting(), fileList_.IsSortedAscending(), false);
		}
		if (prefetchAffected || spreadAffected) {
			DeactivateDisplayPrefetchBatch();
			displayPrefetchBatch_.reset();
		}
		PrepareThumbnailPreload();
		if (displayAction == jpegview_linux::SourceRefreshDisplayAction::PreserveCurrentPixels &&
			!fileList_.Empty()) {
			currentPixelsDetachedFromSource_ = true;
			imageDocument_.MarkDetached(observed.Key());
			currentDisplayRequest_.reset();
			deferredCurrentDisplayPreparation_ = false;
			imageSession_.MarkDocumentChanged();
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
			presentationController_.UseSinglePage(currentIndex);
			doublePagePartnerRequest_.reset();
			SetDisplayTextureProtection(doublePagePartnerDisplayKey_,
				jpegview_linux::CacheProtectionTier::DistantSpeculation);
			doublePagePartnerDisplayKey_.clear();
			if (activeDoublePageRender_.has_value()) {
				const SDL_Rect area = ImageAreaRect();
				const jpegview_linux::ViewportSnapshot snapshot = viewport_.Snapshot();
				activeDoublePageRender_.reset();
				viewport_.Restore(snapshot, CurrentImage().width, CurrentImage().height, area.w, area.h);
				currentDisplayRequest_.reset();
			}
		}
		if ((prefetchAffected || spreadAffected) && !refresh.sortKeyChanged) {
			PrepareImagePrefetch(pendingPrefetchDirection_);
		}
		if (spreadAffected) RefreshDoublePageRenderState();
		if (orderChanged) SetTitle();
		return true;
	}

	void ClearDisplayTexture() {
		if (displayTexture_ != nullptr) {
			DestroyTextureMeasured(displayTexture_);
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
		const int sourceWidth = CurrentImage().originalWidth > 0 ?
			CurrentImage().originalWidth : CurrentImage().width;
		const int sourceHeight = CurrentImage().originalHeight > 0 ?
			CurrentImage().originalHeight : CurrentImage().height;
		const jpegview_linux::DisplayImageTarget resolution =
			jpegview_linux::ClampDisplayImageTarget(sourceWidth, sourceHeight,
				width, height);
		const bool histogramRequested = runtimeSettings_.Values().showHistogram && runtimeSettings_.Values().infoVisible;
		const std::optional<jpegview_linux::ImageSpectrumKey> histogramKey =
			histogramRequested ? CurrentImageSpectrumKey() : std::nullopt;
		if (histogramKey.has_value() && currentSourceSpectrumRequestKey_.has_value() &&
			*currentSourceSpectrumRequestKey_ != *histogramKey) {
			currentSourceSpectrumRequestKey_.reset();
			currentSourceSpectrumDisplayCacheKey_.clear();
			++imageSpectrumPresentationRevision_;
		}
		const bool spectrumAvailable = histogramKey.has_value() &&
			currentSourceSpectrumValid_ && currentSourceSpectrumKey_.has_value() &&
			*currentSourceSpectrumKey_ == *histogramKey;
		const bool spectrumPending = histogramKey.has_value() &&
			currentSourceSpectrumRequestKey_.has_value() &&
			*currentSourceSpectrumRequestKey_ == *histogramKey;
		const bool spectrumUnavailable = histogramKey.has_value() &&
			currentSourceSpectrumUnavailableKey_.has_value() &&
			*currentSourceSpectrumUnavailableKey_ == *histogramKey;
		const bool failedRequestNeedsNewResolution = currentDisplayRequest_.has_value() &&
			jpegview_linux::FailedDisplayRequestNeedsNewResolution(
				*currentDisplayRequest_, failedCurrentDisplayKey_, resolution);
		const bool pendingSpectrumRepresentationFits = spectrumPending &&
			currentDisplayRequest_.has_value() && !failedRequestNeedsNewResolution &&
			currentDisplayRequest_->includeSpectrum &&
			currentDisplayRequest_->source.Key() == source.Key() &&
			currentDisplayRequest_->frameIndex == currentAnimationFrame_ &&
			currentDisplayRequest_->targetWidth >= resolution.width &&
			currentDisplayRequest_->targetHeight >= resolution.height &&
			currentDisplayRequest_->autoContrast == autoContrastEnabled_ &&
			jpegview_linux::EqualEffectiveImageProcessingParams(
				currentDisplayRequest_->cacheKey.processing,
				jpegview_linux::EffectiveImageProcessingParams(imageProcessing_,
					autoContrastEnabled_));
		const bool needsSpectrumWork = histogramRequested && histogramKey.has_value() &&
			!spectrumAvailable && !spectrumUnavailable;
		const bool includeSpectrum = needsSpectrumWork &&
			(!spectrumPending || pendingSpectrumRepresentationFits);
		const bool currentRepresentationSufficient = currentDisplayRequest_.has_value() &&
			!failedRequestNeedsNewResolution &&
			currentDisplayRequest_->source.Key() == source.Key() &&
			currentDisplayRequest_->decoded == currentDecoded_ &&
			currentDisplayRequest_->frameIndex == currentAnimationFrame_ &&
			currentDisplayRequest_->targetWidth >= resolution.width &&
			currentDisplayRequest_->targetHeight >= resolution.height &&
			currentDisplayRequest_->rotationQuarterTurns == 0 &&
			currentDisplayRequest_->autoContrast == autoContrastEnabled_ &&
			(!includeSpectrum || currentDisplayRequest_->includeSpectrum) &&
			jpegview_linux::EqualEffectiveImageProcessingParams(
				currentDisplayRequest_->cacheKey.processing,
				jpegview_linux::EffectiveImageProcessingParams(imageProcessing_,
					autoContrastEnabled_));
		if (!currentRepresentationSufficient) {
			failedCurrentDisplayKey_.clear();
			jpegview_linux::DisplayImageRequest requested;
			if (currentDecoded_) {
				requested = jpegview_linux::MakeDisplayImageRequest(
					source, currentDecoded_, currentAnimationFrame_,
					resolution.width, resolution.height,
					autoContrastEnabled_, 0, imageProcessing_, 0,
					includeSpectrum);
			} else {
				requested = jpegview_linux::MakeJpegDisplayImageRequest(
					source, CurrentImage().originalWidth, CurrentImage().originalHeight,
					resolution.width, resolution.height,
					autoContrastEnabled_, 0, imageProcessing_, 0,
					includeSpectrum);
			}
			currentDisplayRequest_ = std::move(requested);
			std::size_t bestBytes = std::numeric_limits<std::size_t>::max();
			const DisplayTextureCacheEntry* bestRepresentation = nullptr;
			for (const auto& cached : displayTextureCache_) {
				if (!jpegview_linux::CanReuseDisplayImageRepresentation(
					currentDisplayRequest_->cacheKey, cached.second.cacheKey) ||
					cached.second.bytes >= bestBytes) continue;
				bestBytes = cached.second.bytes;
				bestRepresentation = &cached.second;
			}
			if (bestRepresentation != nullptr &&
				bestRepresentation->cacheKey != currentDisplayRequest_->cacheKey) {
				if (currentDecoded_) {
					currentDisplayRequest_ = jpegview_linux::MakeDisplayImageRequest(
						source, currentDecoded_, currentAnimationFrame_,
						bestRepresentation->cacheKey.targetWidth,
						bestRepresentation->cacheKey.targetHeight,
						autoContrastEnabled_, 0, imageProcessing_,
						bestRepresentation->cacheKey.rotationQuarterTurns,
						includeSpectrum);
				} else {
					currentDisplayRequest_ = jpegview_linux::MakeJpegDisplayImageRequest(
						source, CurrentImage().originalWidth, CurrentImage().originalHeight,
						bestRepresentation->cacheKey.targetWidth,
						bestRepresentation->cacheKey.targetHeight,
						autoContrastEnabled_, 0, imageProcessing_,
						bestRepresentation->cacheKey.rotationQuarterTurns,
						includeSpectrum);
				}
			}
			currentDisplayRequest_->workClass =
				jpegview_linux::PerfWorkClass::ActiveImageSpread;
		}
		if (includeSpectrum && histogramKey.has_value() &&
			currentDisplayRequest_->Valid() &&
			(!currentSourceSpectrumRequestKey_.has_value() ||
			*currentSourceSpectrumRequestKey_ != *histogramKey)) {
			currentSourceSpectrumRequestKey_ = *histogramKey;
			currentSourceSpectrumDisplayCacheKey_ = currentDisplayRequest_->key;
			currentSourceSpectrumUnavailableKey_.reset();
			++imageSpectrumPresentationRevision_;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
		}
		currentDisplayRequest_->selectionGeneration =
			imageSession_.Selection().has_value() &&
			imageSession_.Selection()->source.Key() == source.Key() ?
			imageSession_.Generation() : 0;
		if (currentSelectedLoadPending_ && imageSession_.Selection().has_value() &&
			imageSession_.Selection()->source.Key() == source.Key()) {
			pendingSelectedDisplayKey_ = currentDisplayRequest_->key;
		}
		return currentDisplayRequest_->Valid() ? &*currentDisplayRequest_ : nullptr;
	}

	SDL_Texture* DisplayTextureFor(int width, int height) {
		const jpegview_linux::DisplayImageRequest* request = CurrentDisplayRequest(width, height);
		if (request != nullptr) {
			SetDisplayTextureProtection(request->key,
				jpegview_linux::CacheProtectionTier::Active);
			if (request->key == failedCurrentDisplayKey_) {
				if (texture_ != nullptr) return texture_;
				if (LastPresentedTextureMatches(request->source.Key())) {
					return FindDisplayTexture(lastPresentedDisplayKey_);
				}
				return nullptr;
			}
			if (SDL_Texture* cached = FindDisplayTexture(request->key)) {
				CaptureCachedDisplaySpectrum(request->key);
				MarkSelectedDisplayFrameReady(request->selectionGeneration,
					request->source.Key(), request->key);
				return cached;
			}
			displayImageCache_.Request(*request);
			if (texture_ == nullptr && lastPresentedDisplayKey_ != request->key &&
				LastPresentedTextureMatches(request->source.Key())) {
				if (SDL_Texture* previous = FindDisplayTexture(lastPresentedDisplayKey_)) {
					return previous;
				}
			}
			// A same-source prepared texture may remain visible while a replacement
			// resolution is built. A new source clears it before this path is reached.
			return texture_;
		}
		return texture_;
	}

	SDL_Texture* DisplayTextureForRender() const {
		const jpegview_linux::DisplayImageRequest* request =
			currentDisplayRequest_.has_value() && currentDisplayRequest_->Valid() ?
				&*currentDisplayRequest_ : nullptr;
		if (request == nullptr) return texture_;
		if (request->key == failedCurrentDisplayKey_) {
			if (texture_ != nullptr) return texture_;
			return LastPresentedTextureMatches(request->source.Key()) ?
				PeekDisplayTexture(lastPresentedDisplayKey_) : nullptr;
		}
		if (SDL_Texture* cached = PeekDisplayTexture(request->key)) return cached;
		if (texture_ == nullptr && lastPresentedDisplayKey_ != request->key &&
			LastPresentedTextureMatches(request->source.Key())) {
			if (SDL_Texture* previous = PeekDisplayTexture(lastPresentedDisplayKey_)) {
				return previous;
			}
		}
		return texture_;
	}

	void UpdateCurrentDisplayTextureRequest() {
		if (fileList_.Empty() || (!clipboardMode_ && imageSession_.LoadedPath() !=
			AbsoluteNormalized(fileList_.Current())) ||
			presentationController_.SuppressSinglePage(fileList_.CurrentIndex())) return;
		const std::string previousKey = currentDisplayRequest_.has_value() ?
			currentDisplayRequest_->key : std::string();
		const SDL_Rect destination = CurrentPageScreenRect(ImageAreaRect());
		(void)DisplayTextureFor(destination.w, destination.h);
		if (currentDisplayRequest_.has_value() &&
			currentDisplayRequest_->key != previousKey) {
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		}
	}

	void UpdatePresentationState() {
		const SDL_Rect area = ImageAreaRect();
		imageCenterX_ = area.x + area.w / 2;
		imageCenterY_ = area.y + area.h / 2;
		renderContextMenuNeedsCleanFrame_ = contextMenuNeedsCleanFrame_;
		contextMenuNeedsCleanFrame_ = false;
		RefreshDoublePageRenderState();
		UpdateCurrentDisplayTextureRequest();
		UpdateMagnifyingGlassRequest();
		UpdateMagnifyingGlassCursor(
			fileList_.Empty() || presentationController_.SuppressSinglePage(
				fileList_.CurrentIndex()) ? -1 : lastMouseX_,
			fileList_.Empty() || presentationController_.SuppressSinglePage(
				fileList_.CurrentIndex()) ? -1 : lastMouseY_);
	}

	void RenderPendingPresentationWithoutConsumingInvalidation() {
		if (!frameInvalidator_.NeedsRender()) return;
		UpdatePresentationState();
		TouchVisibleThumbnailRows();
		(void)Render();
	}

	void ClearTransition() {
		if (transitionTexture_ != nullptr && transitionTextureOwned_) {
			DestroyTextureMeasured(transitionTexture_);
		} else if (!transitionDisplayKey_.empty()) {
			if (transitionTexture_ != nullptr) {
				SDL_SetTextureBlendMode(transitionTexture_, transitionOriginalBlendMode_);
				SDL_SetTextureAlphaMod(transitionTexture_, 255);
			}
			SetDisplayTextureProtection(transitionDisplayKey_,
				jpegview_linux::CacheProtectionTier::DistantSpeculation);
		}
		transitionTexture_ = nullptr;
		transitionTextureOwned_ = false;
		transitionDisplayKey_.clear();
		transitionOriginalBlendMode_ = SDL_BLENDMODE_NONE;
		transitionWidth_ = 0;
		transitionHeight_ = 0;
		transitionHasTransparency_ = false;
		transitionStartTick_ = 0;
		transitionFrameTick_ = 0;
		ClearActiveWorkingDisplayTextures();
	}

	void ReleaseTransitionFrame(TransitionFrame& frame) {
		if (frame.ownsTexture && frame.texture != nullptr) DestroyTextureMeasured(frame.texture);
		if (!frame.displayKey.empty()) SetDisplayTextureProtection(frame.displayKey,
			jpegview_linux::CacheProtectionTier::DistantSpeculation);
		frame = {};
		ClearActiveWorkingDisplayTextures();
	}

	void MoveTransitionFrame(TransitionFrame& destination, TransitionFrame& source) {
		ReleaseTransitionFrame(destination);
		destination.texture = source.texture;
		destination.displayKey = std::move(source.displayKey);
		destination.width = source.width;
		destination.height = source.height;
		destination.hasTransparency = source.hasTransparency;
		destination.ownsTexture = source.ownsTexture;
		source = {};
	}

	void ClearPendingTransitionFrame() {
		ReleaseTransitionFrame(pendingTransitionFrame_);
	}

	bool LastPresentedTextureMatches(const jpegview_linux::SourceKey& source) const {
		if (lastPresentedDisplayKey_.empty()) return false;
		const auto found = displayTextureCache_.find(lastPresentedDisplayKey_);
		return found != displayTextureCache_.end() && found->second.source == source;
	}

	bool LastPresentedTextureMatchesCurrentSource() const {
		const std::optional<jpegview_linux::ImageSessionSelection>& selection =
			imageSession_.Selection();
		return selection.has_value() &&
			LastPresentedTextureMatches(selection->source.Key());
	}

	void RecordPresentedDisplayTexture(const std::string& key) {
		if (key.empty()) return;
		if (!lastPresentedDisplayKey_.empty() && lastPresentedDisplayKey_ != key &&
			lastPresentedDisplayKey_ != transitionDisplayKey_ &&
			lastPresentedDisplayKey_ != pendingTransitionFrame_.displayKey &&
			lastPresentedDisplayKey_ != transitionCaptureDisplayKey_) {
			SetDisplayTextureProtection(lastPresentedDisplayKey_,
				jpegview_linux::CacheProtectionTier::DistantSpeculation);
		}
		lastPresentedDisplayKey_ = key;
		SetDisplayTextureProtection(key, jpegview_linux::CacheProtectionTier::Active);
		ClearActiveWorkingDisplayTextures();
	}

	bool IsPinnedDisplayTexture(const std::string& key) const {
		return jpegview_linux::IsDisplayTexturePinned(key, transitionDisplayKey_,
			pendingTransitionFrame_.displayKey, transitionCaptureDisplayKey_,
			lastPresentedDisplayKey_, LastPresentedTextureMatchesCurrentSource(),
			presentationController_.AnchorTextureKey(),
			presentationController_.PartnerTextureKey());
	}

	TransitionFrame CaptureTransitionFrame() {
		TransitionFrame frame;
		if (transitionEffect_ == IDM_EFFECT_NONE || CurrentImage().width <= 0 || CurrentImage().height <= 0) {
			return frame;
		}
		frame.width = CurrentImage().width;
		frame.height = CurrentImage().height;
		frame.hasTransparency = CurrentImage().hasTransparency;
		if (texture_ != nullptr) {
			frame.texture = texture_;
			frame.ownsTexture = true;
			texture_ = nullptr;
			return frame;
		}
		const std::optional<jpegview_linux::ImageSessionSelection>& selection =
			imageSession_.Selection();
		if (!selection.has_value()) return {};
		const jpegview_linux::SourceKey source = selection->source.Key();
		if (!LastPresentedTextureMatches(source)) return {};
		const auto found = displayTextureCache_.find(lastPresentedDisplayKey_);
		if (found == displayTextureCache_.end()) return {};
		frame.texture = found->second.texture;
		frame.displayKey = lastPresentedDisplayKey_;
		frame.hasTransparency = found->second.hasTransparency;
		transitionCaptureDisplayKey_ = frame.displayKey;
		SetDisplayTextureProtection(frame.displayKey,
			jpegview_linux::CacheProtectionTier::Active);
		return frame;
	}

	void DestroyThumbnailTextures() {
		for (auto& cached : thumbnailTextureCache_) {
			if (cached.second.texture != nullptr) DestroyTextureMeasured(cached.second.texture);
		}
		thumbnailTextureCache_.clear();
	}

	void ClearThumbnailCache() {
		thumbnailPreparation_.Clear();
		DiscardThumbnailPixelStoreRetry();
		thumbnailUploadRetryKey_.reset();
		DestroyThumbnailTextures();
		thumbnailTextureWindowKeys_.clear();
		thumbnailTextureWindowIndices_.clear();
		thumbnailRepository_->Clear([this](const auto& image) {
			thumbnailPreparation_.Retire(image);
		});
		thumbnailScheduler_.Clear();
		thumbnailCatalogRevisionTracker_.Reset();
	}

	void EvictThumbnails(const std::vector<jpegview_linux::SourceKey>& keys) {
		for (const jpegview_linux::SourceKey& key : keys) {
			const auto texture = thumbnailTextureCache_.find(key);
			if (texture != thumbnailTextureCache_.end()) {
				if (texture->second.texture != nullptr) DestroyTextureMeasured(texture->second.texture);
				thumbnailTextureCache_.erase(texture);
			}
			const jpegview_linux::ThumbnailRepository::ImagePtr pixels =
				thumbnailRepository_->Erase(key);
			thumbnailPreparation_.Retire(pixels);
		}
	}

	void ReconcileThumbnailTextureWindow() {
		std::vector<std::size_t> indices;
		const std::optional<std::size_t> confirmationPin = confirmationOpen_ &&
			!fileList_.Empty() ? std::optional<std::size_t>(fileList_.CurrentIndex()) :
			std::nullopt;
		if (!fileList_.Empty()) {
			const SDL_Rect panel = ThumbnailPanelRect();
			const int rowHeight = runtimeSettings_.Values().thumbnailPanelVisible ?
				jpegview_linux::ThumbnailRowHeight(panel.w, kThumbnailVerticalMargin) :
				jpegview_linux::ThumbnailRowHeight(thumbnailTargetWidth_, kThumbnailVerticalMargin);
			indices = jpegview_linux::ThumbnailTextureWindowIndices(fileList_.Size(),
				fileList_.CurrentIndex(), panel.h, rowHeight, runtimeSettings_.Values().thumbnailPanelVisible,
				confirmationPin, 1);
		}
		std::unordered_set<jpegview_linux::SourceKey, jpegview_linux::SourceKeyHash> desiredKeys;
		desiredKeys.reserve(indices.size());
		for (const std::size_t index : indices) {
			const jpegview_linux::SourceDescriptor* source = fileList_.DescriptorAt(index);
			if (source != nullptr && !source->Key().Empty()) desiredKeys.insert(source->Key());
		}
		for (auto texture = thumbnailTextureCache_.begin();
			texture != thumbnailTextureCache_.end();) {
			if (desiredKeys.find(texture->first) != desiredKeys.end()) {
				++texture;
				continue;
			}
			if (texture->second.texture != nullptr) DestroyTextureMeasured(texture->second.texture);
			texture = thumbnailTextureCache_.erase(texture);
		}
		thumbnailTextureWindowKeys_.swap(desiredKeys);
		thumbnailTextureWindowIndices_.swap(indices);
		if (thumbnailUploadRetryKey_ &&
			thumbnailTextureWindowKeys_.find(*thumbnailUploadRetryKey_) ==
				thumbnailTextureWindowKeys_.end()) {
			thumbnailUploadRetryKey_.reset();
		}
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
				DiscardThumbnailPixelStoreRetry();
				thumbnailUploadRetryKey_.reset();
			}
		}
		EvictThumbnails(thumbnailScheduler_.SetSourceCurrent(fileList_.CurrentIndex()));
		if (!runtimeSettings_.Values().thumbnailPanelVisible || fileList_.Empty()) {
			PauseThumbnailPreparation();
			ReconcileThumbnailTextureWindow();
			return;
		}
		const SDL_Rect panel = ThumbnailPanelRect();
		const int rowHeight = jpegview_linux::ThumbnailRowHeight(
			panel.w, kThumbnailVerticalMargin);
		const int targetWidth = panel.w;
		const int targetHeight = rowHeight - kThumbnailVerticalMargin * 2 - 1;
		if (targetWidth != thumbnailTargetWidth_ || targetHeight != thumbnailTargetHeight_) {
			thumbnailPreparation_.Clear();
			DiscardThumbnailPixelStoreRetry();
			thumbnailUploadRetryKey_.reset();
			(void)thumbnailScheduler_.SetGeometry(targetWidth, targetHeight);
			thumbnailRepository_->SetGeometry(targetWidth, targetHeight,
				[this](const auto& image) { thumbnailPreparation_.Retire(image); });
			DestroyThumbnailTextures();
			thumbnailTargetWidth_ = targetWidth;
			thumbnailTargetHeight_ = targetHeight;
		}
		ReconcileThumbnailTextureWindow();
	}

	void QueuePreparedThumbnail(
		const jpegview_linux::DisplayImageCache::ImagePtr& prepared) {
		if (!runtimeSettings_.Values().thumbnailPanelVisible || autoContrastEnabled_ || !prepared ||
			prepared->filename.empty() || prepared->bgra.empty()) return;
		if (!jpegview_linux::CanReuseDisplayPixelsForThumbnail(
			prepared->width, prepared->height, kMaximumThumbnailSourcePixels)) return;
		const jpegview_linux::SourceKey key = prepared->source.Key();
		if (thumbnailRepository_->Find(key) != nullptr) return;
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
		if (runtimeSettings_.Values().thumbnailPanelVisible) {
			const SDL_Rect panel = ThumbnailPanelRect();
			const int rowHeight = jpegview_linux::ThumbnailRowHeight(
				panel.w, kThumbnailVerticalMargin);
			const int targetWidth = panel.w;
			const int targetHeight = rowHeight - kThumbnailVerticalMargin * 2 - 1;
			if (targetWidth != thumbnailTargetWidth_ || targetHeight != thumbnailTargetHeight_) {
				PrepareThumbnailPreload();
				return;
			}
		}
		ReconcileThumbnailTextureWindow();
		if (!runtimeSettings_.Values().thumbnailPanelVisible && !confirmationOpen_) return;

		const jpegview_linux::InteractionWorkPlan workPlan = CurrentInteractionWorkPlan();
		const Uint32 now = SDL_GetTicks();
		const SDL_Rect panel = ThumbnailPanelRect();
		const int rowHeight = runtimeSettings_.Values().thumbnailPanelVisible ?
			jpegview_linux::ThumbnailRowHeight(panel.w, kThumbnailVerticalMargin) :
			jpegview_linux::ThumbnailRowHeight(thumbnailTargetWidth_, kThumbnailVerticalMargin);
		const int targetWidth = runtimeSettings_.Values().thumbnailPanelVisible ? panel.w : thumbnailTargetWidth_;
		const int targetHeight = thumbnailTargetHeight_;
		const auto resultIsCurrent = [&](const jpegview_linux::ThumbnailPreparationResult& result,
			std::size_t* activeIndex = nullptr) {
			if (result.fileIndex >= fileList_.Files().size()) return false;
			const jpegview_linux::SourceDescriptor* source =
				fileList_.DescriptorAt(result.fileIndex);
			if (result.key.Empty() || !jpegview_linux::ThumbnailPreparationResultMatches(result,
				thumbnailScheduler_.CatalogRevision(), thumbnailScheduler_.GeometryRevision(),
				result.fileIndex, result.key, targetWidth, targetHeight) || source == nullptr ||
				source->Key() != result.key) return false;
			if (activeIndex != nullptr) *activeIndex = result.fileIndex;
			return true;
		};
		const auto requestForResult = [](const jpegview_linux::ThumbnailPreparationResult& result,
			std::size_t fileIndex) {
			return jpegview_linux::ThumbnailLoadRequest{fileIndex, result.key,
				result.catalogRevision, result.geometryRevision,
				result.maximumWidth, result.maximumHeight};
		};
		const auto retainResultPixels = [&](jpegview_linux::ThumbnailPreparationResult& result,
			std::size_t activeIndex) {
			if (!result.image || result.image->key != result.key) {
				thumbnailScheduler_.Fail(requestForResult(result, activeIndex), now);
				thumbnailPreparation_.Retire(result.image);
				return true;
			}
			const jpegview_linux::ThumbnailPixelStoreOutcome outcome =
				thumbnailRepository_->Store(result.image);
			if (outcome == jpegview_linux::ThumbnailPixelStoreOutcome::Stored ||
				outcome == jpegview_linux::ThumbnailPixelStoreOutcome::AlreadyPresent) {
				EvictThumbnails(thumbnailScheduler_.Store(result.key));
				return true;
			}
			if (outcome == jpegview_linux::ThumbnailPixelStoreOutcome::AllocationFailure) {
				jpegview_linux::PreserveThumbnailPreparationRetry(
					thumbnailPixelStoreRetry_, result);
				thumbnailPixelStoreRetryTick_ = now + 50;
				return false;
			}
			thumbnailScheduler_.Fail(requestForResult(result, activeIndex), now);
			return true;
		};
		if (thumbnailPixelStoreRetry_) {
			std::size_t activeIndex = 0;
			if (!resultIsCurrent(*thumbnailPixelStoreRetry_, &activeIndex)) {
				DiscardThumbnailPixelStoreRetry();
			} else if (static_cast<std::int32_t>(now - thumbnailPixelStoreRetryTick_) >= 0) {
				if (retainResultPixels(*thumbnailPixelStoreRetry_, activeIndex)) {
					thumbnailPreparation_.Retire(thumbnailPixelStoreRetry_->image);
					thumbnailPixelStoreRetry_.reset();
				}
			}
		}

		if (runtimeSettings_.Values().thumbnailPanelVisible && !thumbnailPixelStoreRetry_) {
			auto completed = thumbnailPreparation_.TakeCompleted(1,
				workPlan.permittedWorkClasses);
			if (!completed.empty()) {
				auto result = std::move(completed.front());
				if (!result.observedSource.LogicalPath().empty()) {
					(void)ApplySourceChange(result.key, result.observedSource);
				}
				std::size_t activeIndex = 0;
				if (resultIsCurrent(result, &activeIndex)) {
					if (result.cancelled) {
						thumbnailScheduler_.Retry(requestForResult(result, activeIndex));
					} else if (!result.image) {
						thumbnailScheduler_.Fail(requestForResult(result, activeIndex), now);
					} else {
						(void)retainResultPixels(result, activeIndex);
					}
				}
				thumbnailPreparation_.Retire(result.image);
			}
		}

		std::vector<std::size_t> uploadOrder = thumbnailTextureWindowIndices_;
		const std::size_t current = fileList_.Empty() ? 0 : fileList_.CurrentIndex();
		std::sort(uploadOrder.begin(), uploadOrder.end(), [current](std::size_t left,
			std::size_t right) {
			const std::size_t leftDistance = left > current ? left - current : current - left;
			const std::size_t rightDistance = right > current ? right - current : current - right;
			if (leftDistance != rightDistance) return leftDistance < rightDistance;
			return left < current && right >= current;
		});
		for (const std::size_t index : uploadOrder) {
			if (index >= fileList_.Files().size()) continue;
			const jpegview_linux::SourceDescriptor* source = fileList_.DescriptorAt(index);
			if (source == nullptr) continue;
			const jpegview_linux::SourceKey key = source->Key();
			if (thumbnailTextureCache_.find(key) != thumbnailTextureCache_.end()) continue;
			const auto prepared = thumbnailRepository_->Find(key);
			if (!prepared) continue;
			const bool visible = runtimeSettings_.Values().thumbnailPanelVisible &&
				jpegview_linux::ThumbnailIndexVisible(fileList_.Size(), current,
					index, panel.h, rowHeight);
			const bool confirmationPin = confirmationOpen_ && index == current;
			const jpegview_linux::PerfWorkClass uploadClass = visible || confirmationPin ?
				jpegview_linux::PerfWorkClass::VisibleThumbnail :
				jpegview_linux::PerfWorkClass::DistantSpeculation;
			const bool uploadPermitted = confirmationPin ||
				(uploadClass == jpegview_linux::PerfWorkClass::VisibleThumbnail ?
					workPlan.AllowsThumbnail(index) :
					workPlan.Allows(jpegview_linux::PerfWorkClass::DistantSpeculation));
			if (!uploadPermitted) continue;
			if (thumbnailUploadRetryKey_ && *thumbnailUploadRetryKey_ == key &&
				static_cast<std::int32_t>(now - thumbnailUploadRetryTick_) < 0) continue;
			jpegview_linux::PerfContextScope workContext(uploadClass,
				jpegview_linux::PerfExecution::EventThread);
			SDL_Texture* texture = CreateTexture(prepared->bgra, prepared->width,
				prepared->height, prepared->hasTransparency);
			if (texture == nullptr) {
				thumbnailUploadRetryKey_ = key;
				thumbnailUploadRetryTick_ = now + 50;
				break;
			}
			ThumbnailTextureEntry entry;
			entry.texture = texture;
			entry.width = prepared->width;
			entry.height = prepared->height;
			entry.hasTransparency = prepared->hasTransparency;
			try {
				const auto inserted = thumbnailTextureCache_.emplace(key, std::move(entry));
				if (!inserted.second) DestroyTextureMeasured(texture);
			} catch (...) {
				DestroyTextureMeasured(texture);
				thumbnailUploadRetryKey_ = key;
				thumbnailUploadRetryTick_ = now + 50;
				break;
			}
			if (thumbnailUploadRetryKey_ && *thumbnailUploadRetryKey_ == key) {
				thumbnailUploadRetryKey_.reset();
			}
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
			break;
		}

		if (!runtimeSettings_.Values().thumbnailPanelVisible || thumbnailPixelStoreRetry_ || !allowIndependentDecode ||
			thumbnailPreparation_.HasPendingWork()) return;
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
		if (thumbnailRepository_->Find(request.key) != nullptr) {
			EvictThumbnails(thumbnailScheduler_.Store(request.key));
			return;
		}
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
		if (!admission) thumbnailScheduler_.Retry(request);
	}

	void StartTransition(TransitionFrame& previousFrame) {
		jpegview_linux::DisplayTexturePinHandoff borrowedTexturePin(
			transitionCaptureDisplayKey_, previousFrame.displayKey);
		ClearTransition();
		if (transitionEffect_ == IDM_EFFECT_NONE || previousFrame.texture == nullptr ||
			previousFrame.width <= 0 || previousFrame.height <= 0) {
			ReleaseTransitionFrame(previousFrame);
			borrowedTexturePin.Restore();
			ClearActiveWorkingDisplayTextures();
			return;
		}
		transitionTexture_ = previousFrame.texture;
		transitionDisplayKey_ = std::move(previousFrame.displayKey);
		transitionWidth_ = previousFrame.width;
		transitionHeight_ = previousFrame.height;
		transitionHasTransparency_ = previousFrame.hasTransparency;
		transitionTextureOwned_ = previousFrame.ownsTexture;
		previousFrame = {};
		borrowedTexturePin.Restore();
		if (!transitionTextureOwned_) {
			(void)SDL_GetTextureBlendMode(transitionTexture_, &transitionOriginalBlendMode_);
		}
		if (!transitionDisplayKey_.empty()) SetDisplayTextureProtection(
			transitionDisplayKey_, jpegview_linux::CacheProtectionTier::Active);
		SDL_SetTextureBlendMode(transitionTexture_, SDL_BLENDMODE_BLEND);
		const SDL_Rect imageArea = ImageAreaRect();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			CurrentImage().width, CurrentImage().height, imageArea.w, imageArea.h);
		if (SDL_Texture* current = DisplayTextureFor(destination.width, destination.height)) {
			SDL_SetTextureBlendMode(current, SDL_BLENDMODE_BLEND);
		}
		transitionStartTick_ = SDL_GetTicks();
		transitionFrameTick_ = transitionStartTick_;
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Transition);
	}

	void ApplyTransform(int command, bool replayPendingIntent = false) {
		if (!replayPendingIntent &&
			(currentSelectedLoadPending_ || !pendingMaterializationIntents_.empty())) {
			jpegview_linux::PendingImageIntent intent;
			intent.type = jpegview_linux::PendingImageIntentType::Transform;
			intent.transform = command;
			(void)QueuePendingMaterializationIntent(intent);
			return;
		}
		jpegview_linux::ImageOperationSpec operation;
		operation.kind = jpegview_linux::ImageOperationKind::Transform;
		switch (command) {
		case IDM_ROTATE_90:
			operation.transform = jpegview_linux::ImageTransformKind::RotateClockwise;
			break;
		case IDM_ROTATE_270:
			operation.transform = jpegview_linux::ImageTransformKind::RotateCounterClockwise;
			break;
		case IDM_MIRROR_H:
			operation.transform = jpegview_linux::ImageTransformKind::MirrorHorizontal;
			break;
		case IDM_MIRROR_V:
			operation.transform = jpegview_linux::ImageTransformKind::MirrorVertical;
			break;
		default:
			return;
		}
		if (!RequestImageOperation(operation, ImageOperationPurpose::Transform, command)) {
			ReportImageOperationFailure(ImageOperationPurpose::Transform,
				"another operation is pending or the selected source changed");
		}
	}

	void ReplayPendingMaterializationIntents() {
		if (currentSelectedLoadPending_ || fileList_.Empty() ||
			pendingImageOperation_.has_value()) return;
		bool refreshTitleAtEnd = false;
		while (!pendingMaterializationIntents_.empty()) {
			const jpegview_linux::PendingImageIntent intent =
				pendingMaterializationIntents_.front();
			if (intent.type == jpegview_linux::PendingImageIntentType::Viewport) {
				pendingMaterializationIntents_.erase(pendingMaterializationIntents_.begin());
				ApplyPendingViewportIntent(intent.viewport);
				refreshTitleAtEnd = true;
				continue;
			}
			jpegview_linux::ImageOperationSpec operation;
			ImageOperationPurpose purpose = ImageOperationPurpose::Materialize;
			int transformCommand = 0;
			if (intent.type == jpegview_linux::PendingImageIntentType::Transform) {
				purpose = ImageOperationPurpose::Transform;
				transformCommand = intent.transform;
				operation.kind = jpegview_linux::ImageOperationKind::Transform;
				switch (intent.transform) {
				case IDM_ROTATE_90:
					operation.transform = jpegview_linux::ImageTransformKind::RotateClockwise;
					break;
				case IDM_ROTATE_270:
					operation.transform = jpegview_linux::ImageTransformKind::RotateCounterClockwise;
					break;
				case IDM_MIRROR_H:
					operation.transform = jpegview_linux::ImageTransformKind::MirrorHorizontal;
					break;
				case IDM_MIRROR_V:
					operation.transform = jpegview_linux::ImageTransformKind::MirrorVertical;
					break;
				default:
					pendingMaterializationIntents_.erase(pendingMaterializationIntents_.begin());
					continue;
				}
			} else if (intent.type == jpegview_linux::PendingImageIntentType::CopySelection) {
				purpose = ImageOperationPurpose::CopySelection;
				operation.kind = jpegview_linux::ImageOperationKind::CopySelection;
				operation.left = intent.selectionLeft;
				operation.top = intent.selectionTop;
				operation.right = intent.selectionRight;
				operation.bottom = intent.selectionBottom;
			} else if (intent.type == jpegview_linux::PendingImageIntentType::CopyImage) {
				purpose = ImageOperationPurpose::CopyImage;
				operation.kind = jpegview_linux::ImageOperationKind::PrepareOutput;
				if (!intent.fullSize) {
					operation.width = std::max(1, static_cast<int>(std::round(
						CurrentImage().width * viewport_.Zoom())));
					operation.height = std::max(1, static_cast<int>(std::round(
						CurrentImage().height * viewport_.Zoom())));
				}
			} else if (intent.type == jpegview_linux::PendingImageIntentType::CropSelection) {
				purpose = ImageOperationPurpose::Crop;
				operation.kind = jpegview_linux::ImageOperationKind::Crop;
				operation.left = intent.selectionLeft;
				operation.top = intent.selectionTop;
				operation.right = intent.selectionRight;
				operation.bottom = intent.selectionBottom;
			} else {
				pendingMaterializationIntents_.erase(pendingMaterializationIntents_.begin());
				continue;
			}
			if (!RequestImageOperation(operation, purpose, transformCommand,
				intent.fullSize, {}, true)) {
				pendingMaterializationIntents_.erase(pendingMaterializationIntents_.begin());
				continue;
			} else if (intent.type == jpegview_linux::PendingImageIntentType::CropSelection) {
				refreshTitleAtEnd = false;
			}
			pendingMaterializationIntents_.erase(pendingMaterializationIntents_.begin());
			return;
		}
		if (refreshTitleAtEnd) SetTitle();
	}

	void ToggleAutoContrast() {
		autoContrastEnabled_ = !autoContrastEnabled_;
		runtimeSettings_.Values().autoContrast = autoContrastEnabled_;
		RefreshPictureLevels();
		SaveSettings();
	}

	void RefreshPictureLevels(bool refreshNeighbors = true) {
		CancelPendingImageOperation();
		imageSession_.UpdateProcessing({imageProcessing_, autoContrastEnabled_});
		imageDocument_.UpdateProcessing(imageProcessing_, autoContrastEnabled_);
		currentDisplayRequest_.reset();
		if (imageDocument_.SourcePixels() || imageModified_ ||
			currentPixelsDetachedFromSource_) {
			jpegview_linux::ImageOperationSpec operation;
			operation.kind = jpegview_linux::ImageOperationKind::Reprocess;
			if (!RequestImageOperation(operation, ImageOperationPurpose::Reprocess)) {
				SetTitle("Picture-level processing could not be queued");
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
		jpegview_linux::LosslessTransformOperation fileOperation;
		fileOperation.source = fileList_.Current();
		fileOperation.transform = *operation;
		PendingFileOperationUi pending;
		pending.sourcePath = fileOperation.source;
		pending.successTitle = "Applied lossless JPEG transformation";
		pending.failurePrefix = "Lossless JPEG transformation failed: ";
		if (!SubmitFileOperation(std::move(fileOperation), std::move(pending))) {
			SetTitle("Lossless JPEG transformation failed: another file operation is still in progress");
			return;
		}
		SetTitle("Applying lossless JPEG transformation…");
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

	void SetPendingImageTitle() {
		if (fileList_.Empty()) return;
		std::string title = fileList_.Current().filename().string() + " [" +
			CurrentImagePositionText() + "] — Loading image";
		if (pendingImageIntentLimitReached_) {
			title += " (pending input limit reached)";
		}
		SetTitle(title);
	}

	void SetTitle() {
		if (currentJpegHeaderPending_ && !fileList_.Empty()) {
			SetPendingHeaderTitle();
			return;
		}
		if (currentSelectedLoadPending_ && !fileList_.Empty()) {
			SetPendingImageTitle();
			return;
		}
		if (fileList_.Empty() || CurrentImage().originalWidth <= 0 || CurrentImage().originalHeight <= 0) {
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
		context.width = CurrentImage().originalWidth;
		context.height = CurrentImage().originalHeight;
		if (source.Metadata().hasFileSize) context.fileSize = source.Metadata().fileSize;
		context.applicationName = "JPEGView";
		context.applicationVersion = JPEGVIEW_APP_VERSION;
		std::ostringstream cacheKey;
		cacheKey << runtimeSettings_.Values().windowTitlePattern.size() << ':' << runtimeSettings_.Values().windowTitlePattern << '|'
			<< fileList_.MutationRevision() << ':' << fileList_.DescriptorRevision() << '|'
			<< context.position.size() << ':' << context.position << '|'
			<< context.currentIndex << ':' << context.imageCount << '|'
			<< context.fullPath.size() << ':' << context.fullPath << '|'
			<< context.width << ':' << context.height << '|'
			<< (context.fileSize.has_value() ? std::to_string(*context.fileSize) : "-") << '|'
			<< context.applicationVersion;
		const std::string& title = windowTitleFormatCache_.GetOrBuild(
			cacheKey.str(), [this, &context] {
				return jpegview_linux::FormatWindowTitle(runtimeSettings_.Values().windowTitlePattern, context);
			});
		SetTitle(title);
	}

	void OpenDroppedFiles(const std::vector<std::string>& droppedFiles,
		ImageLoadStatePolicy loadStatePolicy = ImageLoadStatePolicy::UseDefaults) {
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
			true, false, 0, {}, loadStatePolicy);
		if (fileDialogOpen_) SetTitle("JPEGView — Opening selection");
	}

	void CloseFileDialog(bool cancelPendingDrop = true) {
		if (pendingImageOperation_.has_value() &&
			pendingImageOperation_->purpose == ImageOperationPurpose::Save) {
			CancelPendingImageOperation();
		}
		if (pendingFileOperation_.has_value() &&
			(pendingFileOperation_->kind == jpegview_linux::FileOperationKind::SaveImage ||
			 pendingFileOperation_->kind == jpegview_linux::FileOperationKind::LosslessCrop)) {
			fileOperationService_.Cancel(pendingFileOperation_->id);
		}
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
		fileDialogDirectoryLoader_.Clear(++fileDialogDirectoryGeneration_);
		fileDialogListingPending_ = false;
		fileDialogFocusAfterListing_.clear();
		fileDialogActivateAfterListing_ = false;
		fileDialogOpenDirectoryAfterListing_ = false;
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
		const jpegview_linux::SourceDescriptor* cropSource =
			fileList_.DescriptorAt(fileList_.CurrentIndex());
		const jpegview_linux::WorkContext cropMetadataWork = cropSource != nullptr &&
			cropSource->Valid() ? jpegview_linux::MakeWorkContext(*cropSource,
				jpegview_linux::SourceWorkPriority::Foreground) :
			jpegview_linux::MakePathWorkContext(fileList_.Current(),
				jpegview_linux::SourceWorkPriority::Foreground);
		if (!jpegview_linux::ReadJpegMcuSize(fileList_.Current(), mcuWidth, mcuHeight,
			errorMessage, cropMetadataWork)) {
			SetTitle("Cannot read JPEG crop block size: " + errorMessage);
			return;
		}
		const jpegview_linux::SelectionRect aligned = jpegview_linux::CropSelectionModel::AlignToMcu(
			cropSelection_.Rect(), CurrentImage().width, CurrentImage().height, mcuWidth, mcuHeight);
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
		const jpegview_linux::SelectionRect bounds = fileDialogLosslessCropRect_;
		jpegview_linux::LosslessCropOperation operation;
		operation.source = AbsoluteNormalized(fileList_.Current());
		operation.output = output;
		operation.x = bounds.left;
		operation.y = bounds.top;
		operation.width = bounds.Width();
		operation.height = bounds.Height();
		operation.overwriteConfirmed = fileDialogOverwriteConfirmed_;
		PendingFileOperationUi pending;
		pending.sourcePath = operation.source;
		pending.outputPath = output;
		pending.cropDialogSave = true;
		pending.successTitle = "Saved lossless crop: " + output.filename().string();
		pending.failurePrefix = "Lossless crop failed: ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			fileDialogMessage_ = "Lossless crop failed: another file operation is still in progress";
			return;
		}
		fileDialogMessage_ = "Writing lossless crop…";
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
		fs::path filename(fileDialogFilename_);
		if (filename.extension().empty()) filename += ".jpg";
		const fs::path output = AbsoluteNormalized(fileDialogDirectory_ / filename);
		std::error_code existsError;
		if (fs::exists(output, existsError) && !existsError && !fileDialogOverwriteConfirmed_) {
			fileDialogOverwriteConfirmed_ = true;
			fileDialogMessage_ = "File exists; press ENTER to overwrite or ESC to cancel";
			return;
		}

		jpegview_linux::ImageOperationSpec operation;
		operation.kind = jpegview_linux::ImageOperationKind::PrepareOutput;
		operation.preserveDocumentPixels = !fileList_.Empty() &&
			SameResolvedPath(output, fileList_.Current());
		if (!fileDialogSaveFullSize_) {
			operation.width = std::max(1, static_cast<int>(std::round(
				CurrentImage().width * viewport_.Zoom())));
			operation.height = std::max(1, static_cast<int>(std::round(
				CurrentImage().height * viewport_.Zoom())));
		}
		if (!RequestImageOperation(operation, ImageOperationPurpose::Save,
			0, fileDialogSaveFullSize_, output)) {
			fileDialogMessage_ = "Cannot prepare image pixels";
		}
	}

	bool SubmitFileOperation(jpegview_linux::FileOperationPayload payload,
		PendingFileOperationUi pending) {
		if (pendingFileOperation_.has_value() || fileOperationService_.Busy()) return false;
		pending.kind = jpegview_linux::KindOf(payload);
		pending.ownerGeneration = imageSession_.Generation();
		pending.ownerPath = fileList_.Empty() ? fs::path{} : fileList_.Current();
		const std::uint64_t id = fileOperationService_.Request(std::move(payload),
			pending.ownerGeneration);
		if (id == 0) return false;
		pending.id = id;
		pendingFileOperation_ = std::move(pending);
		return true;
	}

	bool FileOperationOwnerStillCurrent(const PendingFileOperationUi& pending) const {
		if (!pending.selectionSensitive) return true;
		if (pending.ownerGeneration != imageSession_.Generation()) return false;
		if (pending.ownerPath.empty()) return fileList_.Empty();
		return !fileList_.Empty() && fileList_.Current() == pending.ownerPath;
	}

	void QueueTemporaryCleanup(fs::path file, fs::path directory) {
		if (file.empty() && directory.empty()) return;
		pendingTemporaryCleanups_.push_back(
			TemporaryCleanupRequest{std::move(file), std::move(directory)});
	}

	void ScheduleTemporaryCleanup() {
		if (pendingTemporaryCleanups_.empty() || pendingFileOperation_.has_value() ||
			fileOperationService_.Busy()) return;
		const TemporaryCleanupRequest& cleanup = pendingTemporaryCleanups_.front();
		PendingFileOperationUi pending;
		pending.selectionSensitive = false;
		if (!SubmitFileOperation(jpegview_linux::RemoveTemporaryFilesOperation{
			cleanup.file, cleanup.directory}, std::move(pending))) return;
		pendingTemporaryCleanups_.pop_front();
	}

	void ReloadCurrentDirectoryAfterFileOperation(const fs::path& directory,
		bool forceImageReload, const fs::path& preferredPath,
		const std::string& completionTitle) {
		if (fileList_.Empty() || AbsoluteNormalized(fileList_.Current().parent_path()) !=
			AbsoluteNormalized(directory)) return;
		RequestFileListScan(jpegview_linux::FileList::ScanOperation::Reload,
			0, FileListScanHandling::Reload, true, forceImageReload, preferredPath,
			ImageLoadStatePolicy::PreserveCurrent, {}, completionTitle);
	}

	void TickFileOperation() {
		std::optional<jpegview_linux::FileOperationResult> result =
			fileOperationService_.TakeReady();
		if (!result.has_value()) {
			ScheduleTemporaryCleanup();
			return;
		}
		if (!pendingFileOperation_.has_value() ||
			pendingFileOperation_->id != result->id) {
			QueueTemporaryCleanup(std::move(result->temporaryFile),
				std::move(result->temporaryDirectory));
			ScheduleTemporaryCleanup();
			return;
		}
		PendingFileOperationUi pending = std::move(*pendingFileOperation_);
		pendingFileOperation_.reset();
		const bool ownerCurrent = FileOperationOwnerStillCurrent(pending);
		const auto reportFailure = [this, &pending, ownerCurrent,
			&result](const std::string& fallback) {
			if (!ownerCurrent) return;
			const std::string detail = result->message.empty() ? fallback : result->message;
			if (pending.cropDialogSave && fileDialogOpen_) {
				fileDialogMessage_ = pending.failurePrefix + detail;
			} else if (pending.kind == jpegview_linux::FileOperationKind::SaveImage &&
				fileDialogOpen_) {
				fileDialogMessage_ = pending.failurePrefix + detail;
			} else if (!pending.failurePrefix.empty()) {
				SetTitle(pending.failurePrefix + detail);
			}
		};

		switch (result->kind) {
		case jpegview_linux::FileOperationKind::SaveImage:
			if (result->success && ownerCurrent) {
				const bool flattenAnimation = result->replacedSelectedSource &&
					pending.flattenAnimationOnSuccess;
				CompleteImageSaveOnUi(pending.outputPath,
					result->replacedSelectedSource, flattenAnimation);
			} else if (!result->success) {
				reportFailure("image output could not be written");
			}
			if (pending.resumeAnimationOnCompletion && ownerCurrent &&
				!(result->success && result->replacedSelectedSource &&
					pending.flattenAnimationOnSuccess)) {
				playback_.SetImageReady(true, SDL_GetTicks());
			}
			break;
		case jpegview_linux::FileOperationKind::CopyImage:
			if (result->success) {
				if (ownerCurrent && !pending.successTitle.empty()) SetTitle(pending.successTitle);
			} else {
				reportFailure("clipboard image copy failed");
			}
			break;
		case jpegview_linux::FileOperationKind::PasteImage:
			if (result->success && ownerCurrent && !result->temporaryFile.empty()) {
				ActivateClipboardImage(result->temporaryFile, result->temporaryDirectory);
			} else {
				if (result->success) {
					QueueTemporaryCleanup(std::move(result->temporaryFile),
						std::move(result->temporaryDirectory));
				} else {
					reportFailure("clipboard does not contain a usable image");
				}
			}
			break;
		case jpegview_linux::FileOperationKind::LosslessTransform:
			if (result->success) {
				ReloadCurrentDirectoryAfterFileOperation(pending.sourcePath.parent_path(),
					ownerCurrent, ownerCurrent ? pending.sourcePath :
						(fileList_.Empty() ? fs::path{} : fileList_.Current()),
					ownerCurrent ? pending.successTitle : std::string());
			} else {
				reportFailure("external JPEG transform failed");
			}
			break;
		case jpegview_linux::FileOperationKind::LosslessCrop:
			if (result->success) {
				if (pending.cropDialogSave && fileDialogOpen_ && ownerCurrent) CloseFileDialog();
				ReloadCurrentDirectoryAfterFileOperation(pending.sourcePath.parent_path(),
					pending.outputPath == pending.sourcePath,
					fileList_.Empty() ? fs::path{} : fileList_.Current(),
					ownerCurrent ? pending.successTitle : std::string());
				if (ownerCurrent && fileList_.Empty()) SetTitle(pending.successTitle);
			} else {
				reportFailure("external JPEG crop failed");
			}
			break;
		case jpegview_linux::FileOperationKind::BatchCopy: {
			const jpegview_linux::BatchCopySummary& batch = result->batch;
			const bool changed = batch.renamed != 0 || batch.copied != 0;
			if (changed) {
				ReloadCurrentDirectoryAfterFileOperation(pending.directoryPath, true,
					ownerCurrent ? batch.preferredCurrentPath :
						(fileList_.Empty() ? fs::path{} : fileList_.Current()), {});
			}
			if (pending.batchDialog && batchCopyDialog_.IsOpen()) {
				batchCopyDialog_.ReplaceItems(CollectBatchCopyEntries(),
					fileList_.CurrentIndex(), BatchCopyVisibleRows());
				batchCopyDialog_.Preview();
				std::string message = result->cancelled ?
					"Cancelled after " + std::to_string(batch.completed) + " completed item(s): " :
					"Completed: ";
				message += std::to_string(batch.renamed) + " renamed, " +
					std::to_string(batch.copied) + " copied, " +
					std::to_string(batch.createdDirectories) + " folder(s) created";
				if (batch.failed != 0) {
					message += "; " + std::to_string(batch.failed) + " failed";
					if (!batch.firstFailure.empty()) message += ": " + batch.firstFailure;
				}
				batchCopyDialog_.SetMessage(std::move(message));
			}
			break;
		}
		case jpegview_linux::FileOperationKind::SetModificationTime:
			if (result->success) {
				ReloadCurrentDirectoryAfterFileOperation(pending.sourcePath.parent_path(),
					false, fileList_.Empty() ? fs::path{} : fileList_.Current(),
					ownerCurrent ? pending.successTitle : std::string());
			} else {
				reportFailure("file timestamp update failed");
			}
			break;
		case jpegview_linux::FileOperationKind::TouchFolderExifDates:
			if (result->updatedFiles != 0) {
				ReloadCurrentDirectoryAfterFileOperation(pending.directoryPath, false,
					fileList_.Empty() ? fs::path{} : fileList_.Current(), {});
			}
			if (ownerCurrent) {
				std::string message = result->cancelled ? "Cancelled after updating " :
					"Set EXIF dates for ";
				message += std::to_string(result->updatedFiles) + " image(s)";
				if (!result->success && !result->message.empty()) message += ": " + result->message;
				SetTitle(std::move(message));
			}
			break;
		case jpegview_linux::FileOperationKind::PrintImage:
		case jpegview_linux::FileOperationKind::WallpaperImage:
		case jpegview_linux::FileOperationKind::WallpaperFile:
		case jpegview_linux::FileOperationKind::LaunchDesktop:
		case jpegview_linux::FileOperationKind::RegisterDefaultViewer:
			if (result->success) {
				if (ownerCurrent && !pending.successTitle.empty()) SetTitle(pending.successTitle);
			} else {
				reportFailure("file operation failed");
			}
			break;
		case jpegview_linux::FileOperationKind::MoveToTrash:
			if (result->success) {
				if (!fileList_.Empty() && AbsoluteNormalized(
					fileList_.Current().parent_path()) ==
					AbsoluteNormalized(pending.directoryPath)) {
					const bool removedCurrent = AbsoluteNormalized(fileList_.Current()) ==
						AbsoluteNormalized(pending.sourcePath);
					quitAfterEmptyScan_ = removedCurrent;
					const fs::path preferredPath = removedCurrent ? fs::path{} :
						fileList_.Current();
					RequestFileListScan(jpegview_linux::FileList::ScanOperation::Reload,
						0, FileListScanHandling::Reload, true, true, preferredPath,
						ImageLoadStatePolicy::PreserveCurrent, {},
						ownerCurrent ? "Moved image to trash" : std::string());
				}
			} else {
				reportFailure("trash operation failed");
			}
			break;
		case jpegview_linux::FileOperationKind::RemoveTemporaryFiles:
			if (!result->success && !result->message.empty()) {
				std::cerr << result->message << '\n';
			}
			break;
		}
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
		ScheduleTemporaryCleanup();
	}

	bool CompleteImageSave(const fs::path& output,
		std::shared_ptr<const Image> outputImage,
		jpegview_linux::CacheReservation imageReservation,
		bool overwriteConfirmed,
		bool inPlaceSave, bool flattenAnimationOnSuccess,
		bool resumeAnimationOnCompletion) {
		jpegview_linux::ImageWriteOptions options;
		jpegview_linux::SaveImageOperation operation;
		operation.output = output;
		if (inPlaceSave && !fileList_.Empty()) {
			operation.selectedSourcePath = fileList_.Current();
		}
		operation.image = std::move(outputImage);
		operation.imageReservation = std::move(imageReservation);
		operation.options = options;
		operation.overwriteConfirmed = overwriteConfirmed;
		PendingFileOperationUi pending;
		pending.outputPath = output;
		pending.successTitle = "Saved processed image: " + output.filename().string();
		pending.failurePrefix = "Save failed: ";
		pending.inPlaceSave = inPlaceSave;
		pending.flattenAnimationOnSuccess = flattenAnimationOnSuccess;
		pending.resumeAnimationOnCompletion = resumeAnimationOnCompletion;
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			fileDialogMessage_ = "Save failed: another file operation is still in progress";
			return false;
		}
		fileDialogMessage_ = "Saving processed image…";
		return true;
	}

	void StopAnimationAfterSavedSourceReplacement() {
		if (!imageDocument_.Animated()) return;
		imageDocument_.SetAnimation(false);
		imageDocument_.SetFrameIndex(0);
		currentAnimationFrame_ = 0;
		playback_.ConfigureStillImage(SDL_GetTicks());
		jpegview_linux::RetiredImageBuffers retired;
		retired.decoded = std::move(currentDecoded_);
		imageOperationWorker_.Retire(std::move(retired));
	}

	void CompleteImageSaveOnUi(const fs::path& output,
		bool replacedSelectedSource, bool flattenAnimation) {
		const std::string savedName = output.filename().string();
		const std::optional<std::size_t> outputIndex = fileList_.IndexOf(output);
		CloseFileDialog();
		if (replacedSelectedSource && !fileList_.Empty()) {
			const std::size_t selectedIndex = fileList_.CurrentIndex();
			const jpegview_linux::SourceDescriptor* selectedDescriptor =
				fileList_.DescriptorAt(selectedIndex);
			const jpegview_linux::SourceKey previousSourceKey = selectedDescriptor != nullptr ?
				selectedDescriptor->Key() : imageDocument_.Source();
			const jpegview_linux::SourceDescriptor observed =
				jpegview_linux::DescribeImageSource(fileList_.Current());
			const bool alreadyReconciled = observed.Valid() && selectedDescriptor != nullptr &&
				selectedDescriptor->Key() == observed.Key() &&
				imageDocument_.Source() == observed.Key() && imageDocument_.Detached();
			if (!alreadyReconciled) {
				(void)ApplySourceChange(previousSourceKey, observed, true);
			}
			if (flattenAnimation) StopAnimationAfterSavedSourceReplacement();
			SetTitle("Saved processed image: " + savedName);
		} else if (outputIndex.has_value() &&
			fileList_.DescriptorAt(*outputIndex) != nullptr) {
			const jpegview_linux::SourceKey previousOutputKey =
				fileList_.DescriptorAt(*outputIndex)->Key();
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
		if (!fileList_.Empty() && !runtimeSettings_.Values().keepPictureLevels) SelectPictureLevelsForCurrentFile();
		RefreshPictureLevels();
		SetTitle("Restored picture-level database");
	}

	void CopyCurrentImage(bool fullSize) {
		if (currentSelectedLoadPending_ || !pendingMaterializationIntents_.empty()) {
			jpegview_linux::PendingImageIntent intent;
			intent.type = jpegview_linux::PendingImageIntentType::CopyImage;
			intent.fullSize = fullSize;
			(void)QueuePendingMaterializationIntent(intent);
			return;
		}
		if (CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
		CopyImagePixels(fullSize);
	}

	void CopyImagePixels(bool fullSize) {
		if (CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
		jpegview_linux::ImageOperationSpec operation;
		operation.kind = jpegview_linux::ImageOperationKind::PrepareOutput;
		if (!fullSize) {
			operation.width = std::max(1, static_cast<int>(std::round(
				CurrentImage().width * viewport_.Zoom())));
			operation.height = std::max(1, static_cast<int>(std::round(
				CurrentImage().height * viewport_.Zoom())));
		}
		if (!RequestImageOperation(operation, ImageOperationPurpose::CopyImage,
			0, fullSize)) {
			SetTitle("Copy failed: another operation is pending or the image is unavailable");
		}
	}

	void CopyPreparedImage(std::shared_ptr<const Image> copied,
		jpegview_linux::CacheReservation imageReservation, bool fullSize, bool selection) {
		if (!copied) return;
		jpegview_linux::CopyImageOperation operation;
		operation.image = std::move(copied);
		operation.imageReservation = std::move(imageReservation);
		PendingFileOperationUi pending;
		pending.successTitle = selection ? "Copied selection to clipboard" : (fullSize ?
			"Copied original-size image to clipboard" : "Copied displayed image to clipboard");
		pending.failurePrefix = selection ? "Copy selection failed: " : "Copy image failed: ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle(selection ? "Copy selection failed: another file operation is still in progress" :
				"Copy image failed: another file operation is still in progress");
			return;
		}
		SetTitle(selection ? "Copying selection to clipboard…" : "Copying image to clipboard…");
	}

	bool CropCurrentSelection() {
		if (!cropSelection_.HasSelection() || CurrentImage().width <= 0 || CurrentImage().height <= 0) return false;
		const jpegview_linux::SelectionRect bounds = cropSelection_.Rect();
		if (currentSelectedLoadPending_ || !pendingMaterializationIntents_.empty()) {
			jpegview_linux::PendingImageIntent intent;
			intent.type = jpegview_linux::PendingImageIntentType::CropSelection;
			intent.selectionLeft = bounds.left;
			intent.selectionTop = bounds.top;
			intent.selectionRight = bounds.right;
			intent.selectionBottom = bounds.bottom;
			return QueuePendingMaterializationIntent(intent);
		}
		return CropSelectionPixels(bounds);
	}

	bool CropSelectionPixels(const jpegview_linux::SelectionRect& bounds) {
		if (!bounds.Valid() || CurrentImage().width <= 0 || CurrentImage().height <= 0) return false;
		jpegview_linux::ImageOperationSpec operation;
		operation.kind = jpegview_linux::ImageOperationKind::Crop;
		operation.left = bounds.left;
		operation.top = bounds.top;
		operation.right = bounds.right;
		operation.bottom = bounds.bottom;
		return RequestImageOperation(operation, ImageOperationPurpose::Crop);
	}

	void CopyCurrentSelection() {
		if (!cropSelection_.HasSelection() || CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
		const jpegview_linux::SelectionRect bounds = cropSelection_.Rect();
		if (currentSelectedLoadPending_ || !pendingMaterializationIntents_.empty()) {
			jpegview_linux::PendingImageIntent intent;
			intent.type = jpegview_linux::PendingImageIntentType::CopySelection;
			intent.selectionLeft = bounds.left;
			intent.selectionTop = bounds.top;
			intent.selectionRight = bounds.right;
			intent.selectionBottom = bounds.bottom;
			(void)QueuePendingMaterializationIntent(intent);
			return;
		}
		CopySelectionPixels(bounds);
	}

	void CopySelectionPixels(const jpegview_linux::SelectionRect& bounds) {
		if (!bounds.Valid() || CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
		jpegview_linux::ImageOperationSpec operation;
		operation.kind = jpegview_linux::ImageOperationKind::CopySelection;
		operation.left = bounds.left;
		operation.top = bounds.top;
		operation.right = bounds.right;
		operation.bottom = bounds.bottom;
		if (!RequestImageOperation(operation, ImageOperationPurpose::CopySelection)) {
			SetTitle("Copy selection failed: another operation is pending");
		}
	}

	void ZoomToSelection() {
		if (!cropSelection_.HasSelection() || CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
		const jpegview_linux::SelectionRect bounds = cropSelection_.Rect();
		const SDL_Rect imageArea = ImageAreaRect();
		const SDL_Rect pageDestination = CurrentPageScreenRect(imageArea);
		if (pageDestination.w <= 0 || pageDestination.h <= 0) return;
		const double imageScaleX = static_cast<double>(pageDestination.w) / CurrentImage().width;
		const double imageScaleY = static_cast<double>(pageDestination.h) / CurrentImage().height;
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
		if (runtimeSettings_.Values().selectionModeEnabled == enabled) return;
		runtimeSettings_.Values().selectionModeEnabled = enabled;
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
		jpegview_linux::LaunchDesktopOperation operation;
		operation.fallbacks = jpegview_linux::OpenContainingFolderCommands(directory);
		PendingFileOperationUi pending;
		pending.successTitle = "Opened containing folder";
		pending.failurePrefix = "Cannot open containing folder: ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle("Cannot open containing folder: another file operation is still in progress");
		}
	}

	bool CurrentGpsCoordinatesAvailable() const {
		if (fileList_.Empty() || clipboardMode_ || !metadata_.hasGps) return false;
		const jpegview_linux::SourceDescriptor* source =
			fileList_.DescriptorAt(fileList_.CurrentIndex());
		return source != nullptr && source->Valid() && source->Key() == exifMetadataSource_;
	}

	bool CurrentGpsMapActionAvailable() const {
		return CurrentGpsCoordinatesAvailable() &&
			jpegview_linux::IsValidGpsMapProviderUrlTemplate(
				runtimeSettings_.Values().gpsMapProviderUrl) &&
			jpegview_linux::AreGpsCoordinatesValid(
				metadata_.gpsLatitude, metadata_.gpsLongitude);
	}

	std::string CurrentGpsMapUrl() const {
		if (!CurrentGpsCoordinatesAvailable()) return {};
		return jpegview_linux::BuildGpsMapUrl(
			runtimeSettings_.Values().gpsMapProviderUrl,
			metadata_.gpsLatitude, metadata_.gpsLongitude);
	}

	void OpenGpsMap() {
		const std::string url = CurrentGpsMapUrl();
		if (url.empty()) {
			SetTitle("Cannot open map: check gps_map_provider_url in settings.conf");
			return;
		}
		jpegview_linux::LaunchDesktopOperation operation;
		operation.fallbacks = jpegview_linux::OpenUrlCommands(url);
		PendingFileOperationUi pending;
		pending.successTitle = "Opened GPS location in map";
		pending.failurePrefix = "Cannot open GPS location in map: ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle("Cannot open GPS location in map: another file operation is still in progress");
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

		jpegview_linux::LaunchDesktopOperation operation;
		operation.fallbacks.push_back(*command);
		PendingFileOperationUi pending;
		pending.successTitle = "Opened image with " + application.name;
		pending.failurePrefix = "Cannot open image with " + application.name + ": ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle("Cannot open image with " + application.name +
				": another file operation is still in progress");
		}
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
		jpegview_linux::RegisterDefaultViewerOperation operation;
		operation.executable = executable;
		operation.dataHome = dataHome;
		operation.configHome = configHome;
		PendingFileOperationUi pending;
		pending.successTitle = "JPEGView is now the default viewer for common image formats";
		pending.failurePrefix = "Default viewer registration failed: ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle("Default viewer registration failed: another file operation is still in progress");
		}
	}

	void PrintCurrentImage() {
		if (fileList_.Empty() || CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Printing images inside archives is unavailable");
			return;
		}
		jpegview_linux::ImageOperationSpec operation;
		operation.kind = jpegview_linux::ImageOperationKind::PrepareOutput;
		if (!RequestImageOperation(operation, ImageOperationPurpose::Print)) {
			SetTitle("Print failed: image pixels could not be prepared");
		}
	}

	void PrintPreparedImage(std::shared_ptr<const Image> image,
		jpegview_linux::CacheReservation imageReservation) {
		if (!image) return;
		jpegview_linux::PrintImageOperation operation;
		operation.image = std::move(image);
		operation.imageReservation = std::move(imageReservation);
		PendingFileOperationUi pending;
		pending.successTitle = "Sent image to the default printer";
		pending.failurePrefix = "Print failed: ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle("Print failed: another file operation is still in progress");
			return;
		}
		SetTitle("Preparing image for printing…");
	}

	void SubmitFileListScan(jpegview_linux::FileList::ScanRequest request,
		FileListScanHandling handling, bool force, bool forceImageReload,
		int direction = 0, const fs::path& preferredPath = {},
		ImageLoadStatePolicy loadStatePolicy = ImageLoadStatePolicy::PreserveCurrent,
		const fs::path& markedToggleReturnPath = {},
		std::string completionTitle = {}) {
		if (!force && pendingFileListScanOperation_.has_value() &&
			*pendingFileListScanOperation_ == request.operation &&
			pendingFileListScanHandling_ == handling &&
				pendingFileListScanDirection_ == direction) return;
		fileListSortWorker_.Clear();
		SettlePendingPlaybackBoundaryScan();
		if (!preferredPath.empty()) request.selectedPath = preferredPath;
		pendingDroppedScanRequest_.reset();
		if (handling == FileListScanHandling::DroppedInputs) {
			pendingDroppedScanRequest_ = request;
		}
		pendingFileListScanOperation_ = request.operation;
		pendingFileListScanSourcePath_ = request.selectedPath;
		pendingFileListScanHandling_ = handling;
		pendingFileListScanDirection_ = direction;
		pendingFileListScanForceImageReload_ = forceImageReload;
		pendingFileListScanPreferredPath_ = preferredPath;
		pendingFileListScanLoadStatePolicy_ = loadStatePolicy;
		pendingMarkedToggleReturnPath_ = markedToggleReturnPath;
		pendingFileListScanCompletionTitle_ = std::move(completionTitle);
		fileListScanGeneration_ = fileListScanWorker_.Request(std::move(request));
	}

	void BeginFileListSort(jpegview_linux::FileList::SortMode sortMode,
		bool sortAscending, bool saveSettings = true) {
		const std::uint64_t generation = fileListSortWorker_.Request(
			fileList_.MakeSortRequest(sortMode, sortAscending));
		if (generation == 0) return;
		fileListSortGeneration_ = generation;
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		if (saveSettings) SaveSettings();
	}

	void RequestFileListScan(jpegview_linux::FileList::ScanOperation operation,
		int direction = 0, FileListScanHandling handling = FileListScanHandling::Navigation,
		bool force = false, bool forceImageReload = false,
		const fs::path& preferredPath = {},
		ImageLoadStatePolicy loadStatePolicy = ImageLoadStatePolicy::PreserveCurrent,
		const fs::path& markedToggleReturnPath = {},
		std::string completionTitle = {}) {
		SubmitFileListScan(fileList_.MakeScanRequest(operation, direction), handling,
			force, forceImageReload, direction, preferredPath, loadStatePolicy,
			markedToggleReturnPath, std::move(completionTitle));
	}

	void ClearPendingFileListScan() {
		SettlePendingPlaybackBoundaryScan();
		pendingFileListScanOperation_.reset();
		pendingFileListScanSourcePath_.clear();
		pendingDroppedScanRequest_.reset();
		pendingFileListScanHandling_ = FileListScanHandling::None;
		pendingFileListScanDirection_ = 0;
		pendingFileListScanForceImageReload_ = false;
		pendingFileListScanPreferredPath_.clear();
		pendingFileListScanLoadStatePolicy_ = ImageLoadStatePolicy::PreserveCurrent;
		pendingMarkedToggleReturnPath_.clear();
		pendingFileListScanCompletionTitle_.clear();
	}

	bool IsPendingPlaybackBoundaryScan(std::uint64_t generation) const {
		return generation != 0 && playbackBoundaryScanGeneration_ == generation &&
			pendingFileListScanSourcePath_ == fileList_.Current();
	}

	void SettlePendingPlaybackBoundaryScan() {
		if (playbackBoundaryScanGeneration_ == 0 ||
			playbackBoundaryScanGeneration_ != fileListScanGeneration_) return;
		playbackBoundaryScanGeneration_ = 0;
		if (!currentSelectedLoadPending_) {
			playback_.SetImageReady(true, SDL_GetTicks());
		}
	}

	void StopPlaybackAfterFailedBoundaryScan() {
		const std::uint32_t now = SDL_GetTicks();
		playback_.Stop(now);
		if (!currentSelectedLoadPending_) playback_.SetImageReady(true, now);
	}

	void RetireFileListBoundPresentation() {
		DeactivateDisplayPrefetchBatch();
		displayPrefetchBatch_.reset();
		CancelActiveSpreadPartnerSourceRequest();
		CancelPendingDoublePageRequests();
		const std::size_t currentIndex = fileList_.Empty() ? 0 : fileList_.CurrentIndex();
		presentationController_.InvalidateForFileListReplacement(currentIndex);
		doublePagePartnerRequest_.reset();
		SetDisplayTextureProtection(doublePagePartnerDisplayKey_,
			jpegview_linux::CacheProtectionTier::DistantSpeculation);
		doublePagePartnerDisplayKey_.clear();
		if (activeDoublePageRender_.has_value()) {
			const SDL_Rect area = ImageAreaRect();
			const jpegview_linux::ViewportSnapshot snapshot = viewport_.Snapshot();
			activeDoublePageRender_.reset();
			viewport_.Restore(snapshot, CurrentImage().width, CurrentImage().height, area.w, area.h);
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
			auto retirePreparedScan = [this](jpegview_linux::FileListPreparedScan* prepared) noexcept {
				try {
					fileListSortWorker_.Retire(
						std::make_shared<jpegview_linux::FileListPreparedScan>(std::move(*prepared)));
				} catch (...) {
				}
			};
			std::unique_ptr<jpegview_linux::FileListPreparedScan,
				decltype(retirePreparedScan)> retireOnExit(&result.prepared, retirePreparedScan);
			if (result.generation != fileListScanGeneration_ ||
				!pendingFileListScanOperation_.has_value()) continue;
			const bool playbackBoundaryScan =
				IsPendingPlaybackBoundaryScan(result.generation);
			const FileListScanHandling handling = pendingFileListScanHandling_;
			if (!result.error.empty()) {
				ClearPendingFileListScan();
				if (playbackBoundaryScan) StopPlaybackAfterFailedBoundaryScan();
				SetTitle("Directory scan failed: " + result.error);
				frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
				if (handling == FileListScanHandling::Startup) {
					deferredExitCode_ = 2;
					quitRequested_ = true;
				}
				continue;
			}
			const bool forceImageReload = pendingFileListScanForceImageReload_;
			const ImageLoadStatePolicy loadStatePolicy = pendingFileListScanLoadStatePolicy_;
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
					frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
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
					SubmitFileListScan(std::move(retry), handling, true, false, 0, {}, loadStatePolicy);
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
					preferredPath, loadStatePolicy, markedToggleReturnPath, completionTitle);
				if (playbackBoundaryScan) {
					playbackBoundaryScanGeneration_ = fileListScanGeneration_;
					playback_.SetImageReady(false, SDL_GetTicks());
				}
				continue;
			}
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
			if (fileList_.Empty()) CancelPendingCurrentJpegDimensions();
			if (handling == FileListScanHandling::DroppedInputs) {
				thumbnailCatalogRevisionTracker_.NoteReplacement();
				ClearPendingFileListScan();
				dragging_ = false;
				CloseFileDialog();
				LoadCurrent(0, loadStatePolicy);
				continue;
			}
			ClearPendingFileListScan();
			if (!targetFound) {
				if (playbackBoundaryScan) StopPlaybackAfterFailedBoundaryScan();
				continue;
			}
			if (handling == FileListScanHandling::MarkedToggle) {
				if (fileList_.CompleteMarkedToggle(markedToggleReturnPath)) LoadCurrent(direction);
				continue;
			}

			const fs::path currentPath = fileList_.Current();
			const jpegview_linux::SourceKey currentSourceKey =
				SourceDescriptorForPath(currentPath).Key();
			const bool startupLoadRequest = currentSelectedStartupLoad_ &&
				currentSelectedLoadPending_;
			const bool samePendingStartupLoad = pendingImageIntents_.MatchesStartupLoad(
				handling == FileListScanHandling::Startup, currentSelectedLoadPending_,
				startupLoadRequest,
				AbsoluteNormalized(currentPath), currentSourceKey, imageSession_.Generation());
			const bool currentPathChanged = currentPath != previousPath ||
				(!samePendingStartupLoad && !currentPath.empty() &&
					imageSession_.LoadedPath() != AbsoluteNormalized(currentPath));
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
					if (!LoadCurrent(0, loadStatePolicy, false, true)) {
						deferredExitCode_ = 1;
						quitRequested_ = true;
						continue;
					}
				} else {
					if (startupImageLoadFailed_ && imageSession_.LoadedPath().empty()) {
						deferredExitCode_ = 1;
						quitRequested_ = true;
						continue;
					}
					RefreshFileListConsumers();
					if (samePendingStartupLoad) {
						SetTitle();
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

	void TickFileListSort() {
		for (jpegview_linux::FileListSortResult& result : fileListSortWorker_.TakeReady()) {
			if (result.generation != fileListSortGeneration_) {
				fileListSortWorker_.Retire(std::move(result.prepared));
				continue;
			}
			if (!result.error.empty()) {
				fileListSortWorker_.Retire(std::move(result.prepared));
				SetTitle("File-list sorting failed: " + result.error);
				frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
				continue;
			}
			if (!fileList_.ApplyPreparedSort(result.prepared)) {
				fileListSortWorker_.Retire(std::move(result.prepared));
				if (!pendingFileListScanOperation_) {
					BeginFileListSort(fileList_.GetSorting(), fileList_.IsSortedAscending(), false);
				}
				SetTitle();
				continue;
			}
			fileListSortWorker_.Retire(std::move(result.prepared.retiredEntries));
			fileListSortWorker_.Retire(std::move(result.prepared.retiredPaths));
			fileListSortWorker_.Retire(std::move(result.prepared.retiredPathIndices));
			SetTitle();
			PrepareThumbnailPreload();
			PrepareImagePrefetch();
			RefreshDoublePageRenderState();
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		}
	}

	void ReloadAfterFileChange(const std::string& completionTitle = {}) {
		if (!fileList_.Empty()) RequestFileListScan(
			jpegview_linux::FileList::ScanOperation::Reload,
			0, FileListScanHandling::Reload, true, true, {},
			ImageLoadStatePolicy::PreserveCurrent, {}, completionTitle);
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
			if (exifDate.empty() ||
				!jpegview_linux::ParseLocalExifTimestamp(exifDate, timestamp)) {
				SetTitle("Cannot set date: image has no usable EXIF date");
				return;
			}
		}
		const fs::path path = fileList_.Current();
		PendingFileOperationUi pending;
		pending.sourcePath = path;
		pending.successTitle = useExifDate ? "Set modification date to EXIF date" :
			"Set modification date to current date";
		pending.failurePrefix = "Cannot set image modification date: ";
		jpegview_linux::SetModificationTimeOperation operation{path, timestamp};
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle("Cannot set image modification date: another file operation is still in progress");
			return;
		}
		SetTitle("Updating image modification date…");
	}

	void TouchFolderImagesToExifDate() {
		if (fileList_.Empty() || clipboardMode_) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Cannot change file dates inside an archive");
			return;
		}
		const fs::path directory = fileList_.Current().parent_path();
		PendingFileOperationUi pending;
		pending.sourcePath = fileList_.Current();
		pending.directoryPath = directory;
		pending.successTitle = "Set EXIF dates for folder";
		pending.failurePrefix = "Cannot set EXIF dates: ";
		if (!SubmitFileOperation(jpegview_linux::TouchFolderExifDatesOperation{directory},
			std::move(pending))) {
			SetTitle("Cannot set EXIF dates: another file operation is still in progress");
			return;
		}
		SetTitle("Reading EXIF dates in folder…");
	}

	void SetWallpaper(bool processed) {
		if (fileList_.Empty()) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current()) && !processed) {
			SetTitle("Extract or save the image before setting it as wallpaper");
			return;
		}
		if (processed) {
			jpegview_linux::ImageOperationSpec operation;
			operation.kind = jpegview_linux::ImageOperationKind::PrepareOutput;
			if (!RequestImageOperation(operation, ImageOperationPurpose::Wallpaper,
				0, true)) {
				SetTitle("Set wallpaper failed: image pixels could not be prepared");
			}
			return;
		}
		ApplyWallpaperFile(fileList_.Current());
	}

	void SetWallpaperFromPreparedImage(std::shared_ptr<const Image> image,
		jpegview_linux::CacheReservation imageReservation) {
		if (!image) return;
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
		jpegview_linux::WallpaperImageOperation operation;
		operation.image = std::move(image);
		operation.imageReservation = std::move(imageReservation);
		operation.cacheDirectory = std::move(cacheDirectory);
		jpegview_linux::ImageWriteOptions options;
		operation.options = options;
		PendingFileOperationUi pending;
		pending.successTitle = "Set desktop wallpaper";
		pending.failurePrefix = "Set wallpaper failed: ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle("Set wallpaper failed: another file operation is still in progress");
			return;
		}
		SetTitle("Preparing desktop wallpaper…");
	}

	void ApplyWallpaperFile(const fs::path& wallpaperFile) {
		PendingFileOperationUi pending;
		pending.successTitle = "Set desktop wallpaper";
		pending.failurePrefix = "Set wallpaper failed: ";
		if (!SubmitFileOperation(jpegview_linux::WallpaperFileOperation{wallpaperFile},
			std::move(pending))) {
			SetTitle("Set wallpaper failed: another file operation is still in progress");
			return;
		}
		SetTitle("Applying desktop wallpaper…");
	}

	void RequestConfirmation(int command, const std::string& message) {
		confirmationCommand_ = command;
		confirmationMessage_ = message;
		confirmationOpen_ = true;
		contextMenuOpen_ = false;
		fileDialogOpen_ = false;
		ReconcileThumbnailTextureWindow();
	}

	void MoveCurrentToTrash() {
		if (fileList_.Empty() || clipboardMode_) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Cannot delete an image directly from an archive");
			return;
		}
		const fs::path filename = fileList_.Current();
		jpegview_linux::MoveToTrashOperation operation;
		operation.path = filename;
		operation.commands = jpegview_linux::TrashCommands(filename);
		operation.allowPermanentFallback = true;
		PendingFileOperationUi pending;
		pending.sourcePath = filename;
		pending.directoryPath = filename.parent_path();
		pending.failurePrefix = "Delete failed: ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle("Delete failed: another file operation is still in progress");
			return;
		}
		SetTitle("Moving image to trash…");
	}

	void HandleConfirmationEvents(const SDL_Event& event) {
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				if (confirmationCommand_ == kConfirmRestoreParameterDb) {
					pendingParameterDbRestore_.clear();
					pendingParameterDbRestoreSource_.clear();
				}
				confirmationOpen_ = false;
				ReconcileThumbnailTextureWindow();
			} else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
				const int command = confirmationCommand_;
				confirmationOpen_ = false;
				ReconcileThumbnailTextureWindow();
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
		const jpegview_linux::ViewerSettings& draft = advancedConfiguration_.Draft();
		const int previousThumbnailWidth = runtimeSettings_.Values().thumbnailPanelWidth;
		const bool magnifierChanged = magnifyingGlass_.Width() != draft.magnifyingGlassWidth ||
			magnifyingGlass_.Height() != draft.magnifyingGlassHeight ||
			std::abs(magnifyingGlass_.ZoomLevel() - draft.magnifyingGlassZoomLevel) > 1e-9;
		if (settingsPath.empty() || !runtimeSettings_.SaveAndAdopt(settingsPath, draft)) {
			advancedConfiguration_.SetMessage("Could not save settings.conf; changes were not applied");
			return false;
		}

		const jpegview_linux::ViewerSettings& settings = runtimeSettings_.Values();
		viewport_.SetFitRelativeZoomMode(settings.fitRelativeZoomMode);
		RefreshFitRelativeZoomBase();
		fileList_.SetWrapAroundFolder(settings.folderWrapAround);
		if (fileListBeforeClipboard_) {
			fileListBeforeClipboard_->SetWrapAroundFolder(settings.folderWrapAround);
		}

		if (magnifierChanged) {
			ClearMagnifyingGlassRequest();
			const SDL_Rect imageArea = ImageAreaRect();
			magnifyingGlass_.SetParameters(settings.magnifyingGlassWidth,
				settings.magnifyingGlassHeight, settings.magnifyingGlassZoomLevel,
				imageArea.w, imageArea.h);
		}
		if (runtimeSettings_.Values().thumbnailPanelWidth != previousThumbnailWidth) {
			if (runtimeSettings_.Values().thumbnailPanelVisible) {
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
		jpegview_linux::LaunchDesktopOperation operation;
		operation.fallbacks = jpegview_linux::OpenUrlCommands(kRepositoryUrl);
		PendingFileOperationUi pending;
		pending.successTitle = "About JPEGView Linux";
		pending.failurePrefix = "Cannot open project page: ";
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			SetTitle("Cannot open project page: another file operation is still in progress");
		}
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
		QueueTemporaryCleanup(temporaryFile, temporaryDirectory);
	}

	void PasteCurrentImage() {
		PendingFileOperationUi pending;
		pending.successTitle = "Clipboard image — press next/previous to return to the file list";
		pending.failurePrefix = "Paste image failed: ";
		if (!SubmitFileOperation(jpegview_linux::PasteImageOperation{},
			std::move(pending))) {
			SetTitle("Paste image failed: another file operation is still in progress");
			return;
		}
		SetTitle("Reading image from clipboard…");
	}

	void ActivateClipboardImage(const fs::path& temporaryFile,
		const fs::path& temporaryDirectory) {
		if (!clipboardMode_) {
			imageSession_.SetClipboardReturnViewport(viewport_.Snapshot());
			if (!fileList_.Empty() &&
				imageSession_.OwnsLoadedPath(fileList_.Current())) {
				recentFiles_.RememberViewport(imageSession_.LoadedPath(),
					*imageSession_.ClipboardReturnViewport());
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
		const bool deferred = ShouldDeferViewportIntent();
		if (deferred) {
			if (!QueueDeferredViewportIntent(intent)) return;
		} else {
			jpegview_linux::ApplyViewportIntent(viewport_, intent,
				dimensions.first, dimensions.second, imageArea.w, imageArea.h);
			currentDisplayRequest_.reset();
			PrepareImagePrefetch();
		}
		ShowZoomReadoutTemporarily();
		SetTitle();
	}

	void ActualSize() {
		if (!CanQueuePendingViewportIntent()) return;
		interactionWorkPolicy_.NotifyActivity(
			jpegview_linux::InteractionActivity::Zoom);
		jpegview_linux::ViewportIntent intent;
		intent.type = jpegview_linux::ViewportIntentType::ActualSize;
		const bool deferred = ShouldDeferViewportIntent();
		if (deferred) {
			if (!QueueDeferredViewportIntent(intent)) return;
		} else {
			jpegview_linux::ApplyViewportIntent(viewport_, intent, 0, 0, 0, 0);
			currentDisplayRequest_.reset();
			PrepareImagePrefetch();
		}
		ShowZoomReadoutTemporarily();
		SetTitle();
	}

	NavigationAttemptResult NavigateByPageStep(int direction) {
		if (fileList_.Empty() || (direction != -1 && direction != 1)) {
			return NavigationAttemptResult::Blocked;
		}
		const std::size_t currentIndex = fileList_.CurrentIndex();
		const fs::path currentPath = fileList_.Current();
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
				return NavigationAttemptResult::PendingDirectoryScan;
			}
			if (moved == jpegview_linux::FileList::LoadedNavigationResult::NoMove) {
				if (fileList_.CurrentIndex() != currentIndex) fileList_.Select(currentIndex);
				return NavigationAttemptResult::Blocked;
			}
		}
		return fileList_.CurrentIndex() == currentIndex && fileList_.Current() == currentPath ?
			NavigationAttemptResult::WrappedToSameImage : NavigationAttemptResult::Moved;
	}

	void PanActualSize(int command) {
		if (!(ShouldDeferViewportIntent() ? PendingViewportWillBeActualSize() :
			viewport_.IsActualSize())) return;
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
		const bool deferred = ShouldDeferViewportIntent();
		if (deferred) {
			if (!QueueDeferredViewportIntent(intent)) return;
		} else {
			const auto dimensions = ViewportContentDimensions();
			const SDL_Rect imageArea = ImageAreaRect();
			jpegview_linux::ApplyViewportIntent(viewport_, intent,
				dimensions.first, dimensions.second, imageArea.w, imageArea.h);
			viewport_.ClampToView(dimensions.first, dimensions.second,
				imageArea.w, imageArea.h);
		}
		ShowZoomNavigatorTemporarily();
	}

	void ShowZoomNavigatorTemporarily() {
		zoomNavigatorVisibleUntil_ = SDL_GetTicks() + 1200;
		zoomNavigatorTimedVisible_ = true;
	}

	void ShowZoomReadoutTemporarily() {
		if (!viewport_.FitRelativeZoomMode()) return;
		zoomReadoutVisibleUntil_ = SDL_GetTicks() + 1200;
		zoomReadoutTimedVisible_ = true;
	}

	std::size_t PendingImageIntentCount() const {
		return pendingMaterializationIntents_.size() + pendingImageIntents_.ActionCount();
	}

	bool QueuePendingMaterializationIntent(
		const jpegview_linux::PendingImageIntent& intent) {
		if (PendingImageIntentCount() >= jpegview_linux::kMaximumPendingImageIntents ||
			fileList_.Empty()) {
			SetPendingImageIntentLimitTitle();
			return false;
		}
		if (!currentSelectedLoadPending_) {
			pendingMaterializationIntents_.push_back(intent);
			return true;
		}
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		const fs::path currentPath = AbsoluteNormalized(fileList_.Current());
		bool queued = false;
		switch (intent.type) {
		case jpegview_linux::PendingImageIntentType::Viewport:
			queued = pendingImageIntents_.QueueViewport(currentPath, source.Key(),
				imageSession_.Generation(), intent.viewport);
			break;
		case jpegview_linux::PendingImageIntentType::Transform:
			queued = pendingImageIntents_.QueueTransform(currentPath, source.Key(),
				imageSession_.Generation(), intent.transform);
			break;
		case jpegview_linux::PendingImageIntentType::CopySelection:
			queued = pendingImageIntents_.QueueCopySelection(currentPath, source.Key(),
				imageSession_.Generation(), intent.selectionLeft, intent.selectionTop,
				intent.selectionRight, intent.selectionBottom);
			break;
		case jpegview_linux::PendingImageIntentType::CopyImage:
			queued = pendingImageIntents_.QueueCopyImage(currentPath, source.Key(),
				imageSession_.Generation(), intent.fullSize);
			break;
		case jpegview_linux::PendingImageIntentType::CropSelection:
			queued = pendingImageIntents_.QueueCropSelection(currentPath, source.Key(),
				imageSession_.Generation(), intent.selectionLeft, intent.selectionTop,
				intent.selectionRight, intent.selectionBottom);
			break;
		}
		if (!queued) SetPendingImageIntentLimitTitle();
		return queued;
	}

	bool UpdatePendingViewportSnapshot(
		const std::optional<jpegview_linux::ViewportIntent>& intent = std::nullopt) {
		if (fileList_.Empty()) return false;
		const fs::path currentPath = AbsoluteNormalized(fileList_.Current());
		if (intent.has_value()) {
			const jpegview_linux::SourceDescriptor source =
				SourceDescriptorForPath(fileList_.Current());
			if (!pendingImageIntents_.QueueViewport(currentPath, source.Key(),
				imageSession_.Generation(), *intent)) return false;
		}
		imageSession_.UpdatePendingViewport(currentPath, viewport_.Snapshot());
		return true;
	}

	bool ShouldDeferViewportIntent() const {
		return currentSelectedLoadPending_ || !pendingMaterializationIntents_.empty();
	}

	bool QueueDeferredViewportIntent(const jpegview_linux::ViewportIntent& intent) {
		if (currentSelectedLoadPending_) {
			return UpdatePendingViewportSnapshot(intent);
		}
		if (pendingMaterializationIntents_.empty() ||
			PendingImageIntentCount() >= jpegview_linux::kMaximumPendingImageIntents) {
			return false;
		}
		pendingMaterializationIntents_.push_back({
			jpegview_linux::PendingImageIntentType::Viewport, intent, 0});
		return true;
	}

	bool PendingViewportWillBeActualSize() const {
		jpegview_linux::Viewport projected = viewport_;
		const auto apply = [&projected](
			const jpegview_linux::PendingImageIntent& action) {
			if (action.type != jpegview_linux::PendingImageIntentType::Viewport) return;
			jpegview_linux::ApplyViewportIntent(projected, action.viewport,
				1, 1, 1, 1);
		};
		for (const jpegview_linux::PendingImageIntent& action : pendingMaterializationIntents_) {
			apply(action);
		}
		const std::vector<jpegview_linux::PendingImageIntent>* pendingActions =
			pendingImageIntents_.Actions();
		if (pendingActions != nullptr) {
			for (const jpegview_linux::PendingImageIntent& action : *pendingActions) {
				apply(action);
			}
		}
		return projected.IsActualSize();
	}

	bool CanQueuePendingViewportIntent() {
		if (fileList_.Empty()) return false;
		if (PendingImageIntentCount() < jpegview_linux::kMaximumPendingImageIntents) {
			if (!currentSelectedLoadPending_) return true;
			const jpegview_linux::SourceDescriptor source =
				SourceDescriptorForPath(fileList_.Current());
			const fs::path currentPath = AbsoluteNormalized(fileList_.Current());
			if (pendingImageIntents_.CanQueue(currentPath, source.Key(),
				imageSession_.Generation())) return true;
		}
		SetPendingImageIntentLimitTitle();
		return false;
	}

	void SetPendingImageIntentLimitTitle() {
		pendingImageIntentLimitReached_ = true;
		if (currentJpegHeaderPending_) SetPendingHeaderTitle(true);
		else if (currentSelectedLoadPending_) SetPendingImageTitle();
		else if (!fileList_.Empty()) SetTitle(
			fileList_.Current().filename().string() + " — pending input limit reached");
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
		const bool deferred = ShouldDeferViewportIntent();
		if (deferred) {
			if (!QueueDeferredViewportIntent(intent)) return;
		} else {
			jpegview_linux::ApplyViewportIntent(viewport_, intent,
				dimensions.first, dimensions.second, imageArea.w, imageArea.h);
			if (dimensions.first > 0 && dimensions.second > 0) {
				viewport_.ClampToView(dimensions.first, dimensions.second,
					imageArea.w, imageArea.h);
			}
		}
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
		const bool deferred = ShouldDeferViewportIntent();
		if (deferred) {
			if (!QueueDeferredViewportIntent(intent)) return;
		} else {
			jpegview_linux::ApplyViewportIntent(viewport_, intent,
				dimensions.first, dimensions.second, imageArea.w, imageArea.h);
			if (dimensions.first > 0 && dimensions.second > 0) {
				viewport_.ClampToView(dimensions.first, dimensions.second,
					imageArea.w, imageArea.h);
			}
		}
		ShowZoomNavigatorTemporarily();
		ShowZoomReadoutTemporarily();
		playback_.NotifyInteraction(SDL_GetTicks());
		SetTitle();
	}

	NavigationAttemptResult NextImage(bool showPendingNavigation = false) {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = !doublePageModeEnabled_ && playback_.SlideshowSeconds() > 0.0 &&
			transitionEffect_ != IDM_EFFECT_NONE;
		TransitionFrame previousFrame;
		if (animate && !currentSelectedLoadPending_) {
			previousFrame = CaptureTransitionFrame();
		}
		const NavigationAttemptResult navigation = NavigateByPageStep(1);
		if (navigation != NavigationAttemptResult::Moved) {
			if (previousFrame.ownsTexture && previousFrame.texture != nullptr) {
				texture_ = previousFrame.texture;
				previousFrame.texture = nullptr;
				previousFrame.ownsTexture = false;
			}
			ReleaseTransitionFrame(previousFrame);
			transitionCaptureDisplayKey_.clear();
			ClearActiveWorkingDisplayTextures();
			if (navigation == NavigationAttemptResult::PendingDirectoryScan) RequestFileListScan(
				jpegview_linux::FileList::ScanOperation::ForwardBoundary,
				1, FileListScanHandling::Navigation);
			return navigation;
		}
		transitionCaptureDisplayKey_ = previousFrame.displayKey;
		SetTitle();
		if (showPendingNavigation) {
			RenderPendingPresentationWithoutConsumingInvalidation();
		}
		const bool loaded = LoadCurrent(1);
		if (loaded && animate) {
			if (currentSelectedLoadPending_) {
				const jpegview_linux::SourceDescriptor source =
					SourceDescriptorForPath(fileList_.Current());
				if (pendingImageIntents_.RequestTransition(source.Key(),
					imageSession_.Generation())) {
				MoveTransitionFrame(pendingTransitionFrame_, previousFrame);
				}
			} else {
				StartTransition(previousFrame);
			}
		}
		ReleaseTransitionFrame(previousFrame);
		transitionCaptureDisplayKey_.clear();
		ClearActiveWorkingDisplayTextures();
		return NavigationAttemptResult::Moved;
	}

	void PreviousImage(bool showPendingNavigation = false) {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = !doublePageModeEnabled_ && playback_.SlideshowSeconds() > 0.0 &&
			transitionEffect_ != IDM_EFFECT_NONE;
		TransitionFrame previousFrame;
		if (animate && !currentSelectedLoadPending_) {
			previousFrame = CaptureTransitionFrame();
		}
		if (NavigateByPageStep(-1) != NavigationAttemptResult::Moved) {
			if (previousFrame.ownsTexture && previousFrame.texture != nullptr) {
				texture_ = previousFrame.texture;
				previousFrame.texture = nullptr;
				previousFrame.ownsTexture = false;
			}
			ReleaseTransitionFrame(previousFrame);
			transitionCaptureDisplayKey_.clear();
			ClearActiveWorkingDisplayTextures();
			return;
		}
		transitionCaptureDisplayKey_ = previousFrame.displayKey;
		SetTitle();
		if (showPendingNavigation) {
			RenderPendingPresentationWithoutConsumingInvalidation();
		}
		const bool loaded = LoadCurrent(-1);
		if (loaded && animate) {
			if (currentSelectedLoadPending_) {
				const jpegview_linux::SourceDescriptor source =
					SourceDescriptorForPath(fileList_.Current());
				if (pendingImageIntents_.RequestTransition(source.Key(),
					imageSession_.Generation())) {
					MoveTransitionFrame(pendingTransitionFrame_, previousFrame);
				}
			} else {
				StartTransition(previousFrame);
			}
		}
		ReleaseTransitionFrame(previousFrame);
		transitionCaptureDisplayKey_.clear();
		ClearActiveWorkingDisplayTextures();
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
		const int panelWidth = runtimeSettings_.Values().thumbnailPanelVisible ? runtimeSettings_.Values().thumbnailPanelWidth : 0;
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
		SettlePendingPlaybackBoundaryScan();
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

	int NextEventWaitTimeoutMs(Uint32 now) const {
		std::vector<Uint32> deadlines;
		if (const auto playbackDeadline = playback_.NextDeadline();
			playbackDeadline.has_value()) {
			deadlines.push_back(*playbackDeadline);
		}
		if (transitionTexture_ != nullptr) {
			if (now - transitionStartTick_ >= transitionDurationMs_) {
				deadlines.push_back(now);
			} else {
				deadlines.push_back(transitionFrameTick_ + 16u);
			}
		}
		if (heldNavigation_.Scancode() >= 0) deadlines.push_back(now + 16u);
		if (zoomNavigatorTimedVisible_) deadlines.push_back(zoomNavigatorVisibleUntil_);
		if (zoomReadoutTimedVisible_) deadlines.push_back(zoomReadoutVisibleUntil_);
		if (thumbnailPixelStoreRetry_) deadlines.push_back(thumbnailPixelStoreRetryTick_);
		if (thumbnailUploadRetryKey_.has_value()) deadlines.push_back(thumbnailUploadRetryTick_);
		if (!pendingTextureUploads_.empty()) {
			const jpegview_linux::InteractionWorkPlan uploadWorkPlan =
				CurrentInteractionWorkPlan();
			const bool eligibleBandedUpload = std::any_of(pendingTextureUploads_.begin(),
				pendingTextureUploads_.end(), [&uploadWorkPlan](
					const PendingTextureUpload& pending) {
					return pending.incompleteTexture != nullptr &&
						pending.uploadPlan.CurrentBand().has_value() &&
						uploadWorkPlan.Allows(pending.priority.workClass);
				});
			if (const auto uploadDeadline =
				jpegview_linux::DisplayUploadContinuationDeadline(
					now, eligibleBandedUpload); uploadDeadline.has_value()) {
				deadlines.push_back(*uploadDeadline);
			}
		}
		int timeout = jpegview_linux::EventWaitTimeoutMs(now, deadlines, 100);
		if (const auto idleDeadline = interactionWorkPolicy_.NextIdleDeadline();
			idleDeadline.has_value()) {
			const auto remaining = *idleDeadline - std::chrono::steady_clock::now();
			const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
			const int idleTimeout = rounded <= 0 ? 0 : static_cast<int>(
				std::min<std::int64_t>(rounded, std::numeric_limits<int>::max()));
			timeout = std::min(timeout, idleTimeout);
		}
		return timeout;
	}

	void TickTimedPresentationEffects(Uint32 now) {
		bool navigatorExpired = false;
		if (zoomNavigatorTimedVisible_ &&
			static_cast<Sint32>(now - zoomNavigatorVisibleUntil_) >= 0) {
			zoomNavigatorVisibleUntil_ = 0;
			zoomNavigatorTimedVisible_ = false;
			navigatorExpired = true;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
		}
		if (zoomReadoutTimedVisible_ &&
			static_cast<Sint32>(now - zoomReadoutVisibleUntil_) >= 0) {
			zoomReadoutVisibleUntil_ = 0;
			zoomReadoutTimedVisible_ = false;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
		}
		if (navigatorExpired) {
			UpdateZoomNavigatorCursor(lastMouseX_, lastMouseY_);
			UpdateMagnifyingGlassCursor(lastMouseX_, lastMouseY_);
		}
		if (thumbnailPixelStoreRetry_ &&
			static_cast<Sint32>(now - thumbnailPixelStoreRetryTick_) >= 0) {
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		}
		if (thumbnailUploadRetryKey_.has_value() &&
			static_cast<Sint32>(now - thumbnailUploadRetryTick_) >= 0) {
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::ImageResource);
		}
		if (transitionTexture_ == nullptr) return;
		if (now - transitionStartTick_ >= transitionDurationMs_) {
			ClearTransition();
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Transition);
			return;
		}
		if (now - transitionFrameTick_ >= 16u) {
			transitionFrameTick_ = now;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Transition);
		}
	}

	void TickPlayback() {
		const jpegview_linux::PlaybackAction action = playback_.Tick(SDL_GetTicks());
		if (action.type == jpegview_linux::PlaybackActionType::ShowFrame) {
			if (!SetAnimationFrame(action.frameIndex)) playback_.FrameDisplayFailed();
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Animation);
		} else if (action.type == jpegview_linux::PlaybackActionType::NextImage) {
			const NavigationAttemptResult navigation = NextImage();
			if (navigation == NavigationAttemptResult::Blocked) {
				StopPlaybackAfterFailedBoundaryScan();
			} else if (navigation == NavigationAttemptResult::PendingDirectoryScan) {
				playback_.SetImageReady(false, SDL_GetTicks());
				playbackBoundaryScanGeneration_ = fileListScanGeneration_;
			} else if (navigation == NavigationAttemptResult::WrappedToSameImage) {
				playback_.NotifyInteraction(SDL_GetTicks());
			} else {
				frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Animation);
			}
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
		const std::size_t selectedIndex = fileList_.Empty() ? 0 : fileList_.CurrentIndex();
		const bool currentImageReady = presentationController_.CanRepeatNavigation({
			selectedIndex,
			!fileList_.Empty() && (clipboardMode_ || imageSession_.LoadedPath() ==
				AbsoluteNormalized(fileList_.Current())),
			currentSelectedLoadPending_,
			activeDoublePageRender_.has_value(),
			!activeDoublePageRender_.has_value() || ActiveDoublePageAnchorTexture() != nullptr,
			!activeDoublePageRender_.has_value() ||
				FindDisplayTexture(presentationController_.PartnerTextureKey()) != nullptr});
		const int direction = heldNavigation_.AfterImageShown(keyIsHeld, currentImageReady);
		if (direction == 0 || fileList_.Empty()) return;

		const std::size_t previousIndex = fileList_.CurrentIndex();
		if (direction > 0) NextImage(true);
		else PreviousImage(true);
		if (fileList_.CurrentIndex() == previousIndex) heldNavigation_.Reset();
	}

	bool SetAnimationFrame(std::size_t index) {
		if (!currentDecoded_ || index >= currentDecoded_->frames.size()) return false;
		CancelPendingImageOperation();
		CancelImageSpectrumBeforeImageMutation();
		const jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.Snapshot();
		const jpegview_linux::DecodedFrame& frame = currentDecoded_->frames[index];
		jpegview_linux::RetiredImageBuffers retired = imageDocument_.SetFrame(
			index, currentDecoded_->animation, frame.width, frame.height,
			frame.hasTransparency);
		imageOperationWorker_.Retire(std::move(retired));
		imageModified_ = false;
		currentPixelsDetachedFromSource_ = false;
		currentImageRotationQuarterTurns_ = 0;
		currentSpreadRotationValid_ = false;
		editedImageSpectrumValid_ = false;
		currentSourcePageDimensions_ =
			jpegview_linux::PageDimensions{frame.width, frame.height};
		currentAnimationFrame_ = index;
		currentDisplayRequest_.reset();
		failedCurrentDisplayKey_.clear();
		RestoreScaleMode(viewportSnapshot);
		playback_.SetImageReady(false, SDL_GetTicks());
		RequestCurrentDisplayFrame();
		return true;
	}

	void UpdateNavigationPanelVisibility(int, int mouseY) {
		if (!runtimeSettings_.Values().navigationPanelEnabled) {
			controlsVisible_ = false;
			return;
		}
		if (!runtimeSettings_.Values().navigationPanelAutoReveal) {
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
		if (fileList_.Empty() || currentSelectedLoadPending_) return lines;

		std::ostringstream title;
		title << '[' << CurrentImagePositionText() << "] "
			<< InfoText(fileList_.Current().filename().string());
		lines.push_back(title.str());

		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		const jpegview_linux::SourceMetadata& sourceMetadata = source.Metadata();
		lines.push_back(jpegview_linux::FormatImageDimensionsAndSize(
			CurrentImage().originalWidth, CurrentImage().originalHeight,
			sourceMetadata.hasFileSize ? jpegview_linux::FormatFileSize(sourceMetadata.fileSize) :
				std::string()));
		if (currentDecoded_ && currentDecoded_->frames.size() > 1) {
			lines.push_back("Frame: " + std::to_string(playback_.FrameIndex() + 1) + "/" +
				std::to_string(currentDecoded_->frames.size()));
			lines.push_back(std::string("Playback: ") + (playback_.AnimationPlaying() ? "playing" : "paused"));
		}
		if (CurrentImage().width != CurrentImage().originalWidth || CurrentImage().height != CurrentImage().originalHeight) {
			lines.push_back("Displayed size: " + std::to_string(CurrentImage().width) + " x " + std::to_string(CurrentImage().height));
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
		if (fileList_.Empty() || currentSelectedLoadPending_) {
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
			<< CurrentImage().originalWidth << ':' << CurrentImage().originalHeight << '|'
			<< CurrentImage().width << ':' << CurrentImage().height << '|'
			<< playback_.FrameIndex() << ':' << playback_.AnimationPlaying() << '|'
			<< (currentDecoded_ ? currentDecoded_->frames.size() : 0) << ':'
			<< imageInfoMetadataRevision_;
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

	std::optional<jpegview_linux::ImageSpectrumKey> CurrentImageSpectrumKey() const {
		if (fileList_.Empty()) return std::nullopt;
		const jpegview_linux::SourceDescriptor source =
			SourceDescriptorForPath(fileList_.Current());
		if (!source.Key().Valid()) return std::nullopt;
		return jpegview_linux::ImageSpectrumKey{source.Key(),
			imageDocument_.Revision(), currentAnimationFrame_,
			jpegview_linux::EffectiveImageProcessingParams(imageProcessing_,
				autoContrastEnabled_),
			autoContrastEnabled_, currentImageRotationQuarterTurns_};
	}

	void CaptureSourceSpectrum(
		const jpegview_linux::DisplayImageCacheKey& cacheKey,
		const jpegview_linux::GrayscaleSpectrum* spectrum) {
		if (spectrum == nullptr || !cacheKey.includeSpectrum ||
			UsesEditedImageSpectrum() ||
			!currentSourceSpectrumRequestKey_.has_value()) return;
		const std::optional<jpegview_linux::ImageSpectrumKey> currentKey =
			CurrentImageSpectrumKey();
		if (!currentKey.has_value() ||
			*currentSourceSpectrumRequestKey_ != *currentKey ||
			cacheKey.source != currentKey->source ||
			cacheKey.frameIndex != currentKey->frameIndex ||
			cacheKey.rotationQuarterTurns != currentKey->rotationQuarterTurns ||
			cacheKey.autoContrast != currentKey->autoContrast ||
			!jpegview_linux::EqualEffectiveImageProcessingParams(cacheKey.processing,
				currentKey->processing)) return;
		currentSourceSpectrum_ = *spectrum;
		currentSourceSpectrumKey_ = *currentKey;
		currentSourceSpectrumValid_ = true;
		currentSourceSpectrumRequestKey_.reset();
		currentSourceSpectrumDisplayCacheKey_.clear();
		currentSourceSpectrumUnavailableKey_.reset();
		++imageSpectrumPresentationRevision_;
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
	}

	void CapturePreparedSourceSpectrum(
		const jpegview_linux::PreparedDisplayImage& prepared) {
		CaptureSourceSpectrum(prepared.cacheKey,
			prepared.spectrum ? prepared.spectrum.get() : nullptr);
	}

	void CaptureCachedDisplaySpectrum(const std::string& key) {
		const auto cached = displayTextureCache_.find(key);
		if (cached == displayTextureCache_.end()) return;
		CaptureSourceSpectrum(cached->second.cacheKey,
			cached->second.spectrum ? cached->second.spectrum.get() : nullptr);
	}

	bool UsesEditedImageSpectrum() const {
		return imageModified_ || currentPixelsDetachedFromSource_;
	}

	void CancelImageSpectrumBeforeImageMutation() {
		imageSpectrumWorker_.Cancel();
		editedImageSpectrumValid_ = false;
		editedImageSpectrumKey_.reset();
		editedImageSpectrumUnavailableKey_.reset();
		editedImageSpectrumRequestKey_.reset();
		editedImageSpectrumRequestGeneration_ = 0;
		currentSourceSpectrumRequestKey_.reset();
		currentSourceSpectrumDisplayCacheKey_.clear();
		++imageSpectrumPresentationRevision_;
	}

	void TickImageSpectrum() {
		const bool requested = runtimeSettings_.Values().showHistogram && runtimeSettings_.Values().infoVisible;
		const bool editedPixels = UsesEditedImageSpectrum();
		if (currentSourceSpectrumRequestKey_.has_value()) {
			const std::optional<jpegview_linux::ImageSpectrumKey> selectedKey =
				CurrentImageSpectrumKey();
			if (editedPixels || !selectedKey.has_value() ||
				*selectedKey != *currentSourceSpectrumRequestKey_) {
				currentSourceSpectrumRequestKey_.reset();
				currentSourceSpectrumDisplayCacheKey_.clear();
				++imageSpectrumPresentationRevision_;
			} else if (!currentSourceSpectrumDisplayCacheKey_.empty() &&
				displayTextureCache_.find(currentSourceSpectrumDisplayCacheKey_) ==
					displayTextureCache_.end() &&
				!displayImageCache_.HasPendingOrCached(
					currentSourceSpectrumDisplayCacheKey_)) {
				currentSourceSpectrumRequestKey_.reset();
				currentSourceSpectrumDisplayCacheKey_.clear();
				++imageSpectrumPresentationRevision_;
			}
		}
		const std::optional<jpegview_linux::ImageSpectrumKey> currentKey =
			editedPixels ? CurrentImageSpectrumKey() : std::nullopt;
		const std::optional<jpegview_linux::ImageSpectrumResult> result =
			imageSpectrumWorker_.TakeReady();
		if (result.has_value() && editedImageSpectrumRequestKey_.has_value() &&
			currentKey.has_value() && jpegview_linux::IsCurrentImageSpectrumResult(
				*result, editedImageSpectrumRequestGeneration_, *currentKey)) {
			editedImageSpectrumRequestKey_.reset();
			editedImageSpectrumRequestGeneration_ = 0;
			if (result->spectrum.has_value()) {
				editedImageSpectrum_ = *result->spectrum;
				editedImageSpectrumKey_ = result->key;
				editedImageSpectrumValid_ = true;
				editedImageSpectrumUnavailableKey_.reset();
			} else {
				editedImageSpectrumValid_ = false;
				editedImageSpectrumKey_.reset();
				editedImageSpectrumUnavailableKey_ = result->key;
			}
			++imageSpectrumPresentationRevision_;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
		} else if (result.has_value() && editedImageSpectrumRequestKey_.has_value() &&
			result->generation == editedImageSpectrumRequestGeneration_) {
			editedImageSpectrumRequestKey_.reset();
			editedImageSpectrumRequestGeneration_ = 0;
		}

		if (!requested) {
			if (editedImageSpectrumRequestKey_.has_value()) {
				imageSpectrumWorker_.Cancel();
				editedImageSpectrumRequestKey_.reset();
				editedImageSpectrumRequestGeneration_ = 0;
				++imageSpectrumPresentationRevision_;
			}
			return;
		}
		if (currentSelectedLoadPending_) return;
		if (!editedPixels) {
			if (editedImageSpectrumRequestKey_.has_value()) {
				imageSpectrumWorker_.Cancel();
				editedImageSpectrumRequestKey_.reset();
				editedImageSpectrumRequestGeneration_ = 0;
				++imageSpectrumPresentationRevision_;
			}
			const std::optional<jpegview_linux::ImageSpectrumKey> sourceKey =
				CurrentImageSpectrumKey();
			if (!sourceKey.has_value()) return;
			if (currentSourceSpectrumValid_ && currentSourceSpectrumKey_.has_value() &&
				*currentSourceSpectrumKey_ == *sourceKey) return;
			if (currentSourceSpectrumUnavailableKey_.has_value() &&
				*currentSourceSpectrumUnavailableKey_ == *sourceKey) return;
			if (currentSourceSpectrumRequestKey_.has_value() &&
				*currentSourceSpectrumRequestKey_ == *sourceKey) return;
			if (CurrentImage().width > 0) RequestCurrentDisplayFrame();
			return;
		}
		if (!currentKey.has_value()) return;
		if (editedImageSpectrumValid_ && editedImageSpectrumKey_.has_value() &&
			*editedImageSpectrumKey_ == *currentKey) return;
		if (editedImageSpectrumUnavailableKey_.has_value() &&
			*editedImageSpectrumUnavailableKey_ == *currentKey) return;
		if (editedImageSpectrumRequestKey_.has_value() &&
			*editedImageSpectrumRequestKey_ == *currentKey) {
			if (imageSpectrumWorker_.IsPendingFor(*currentKey)) return;
			imageSpectrumWorker_.Cancel();
			editedImageSpectrumRequestKey_.reset();
			editedImageSpectrumRequestGeneration_ = 0;
		}
		const std::uint64_t generation = imageSpectrumWorker_.Request(
			imageDocument_.PresentationPixels(), *currentKey);
		if (generation == 0) {
			editedImageSpectrumUnavailableKey_ = *currentKey;
		} else {
			editedImageSpectrumRequestKey_ = *currentKey;
			editedImageSpectrumRequestGeneration_ = generation;
			editedImageSpectrumUnavailableKey_.reset();
		}
		++imageSpectrumPresentationRevision_;
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
	}

	const jpegview_linux::InformationOverlayPaintPlan& BuildImageInfoPaintPlan() {
		const std::vector<std::string>& sourceLines = CachedImageInfoLines();
		const bool gpsLinkAvailable = CurrentGpsMapActionAvailable();
		const bool editedPixels = UsesEditedImageSpectrum();
		const std::optional<jpegview_linux::ImageSpectrumKey> spectrumKey =
			runtimeSettings_.Values().showHistogram ? CurrentImageSpectrumKey() : std::nullopt;
		const jpegview_linux::GrayscaleSpectrum* spectrumPointer = nullptr;
		int spectrumState = 0;
		std::string spectrumStatus;
		if (runtimeSettings_.Values().showHistogram) {
			spectrumState = 1;
			if (editedPixels) {
				if (spectrumKey.has_value() && editedImageSpectrumValid_ &&
					editedImageSpectrumKey_.has_value() &&
					*editedImageSpectrumKey_ == *spectrumKey) {
					spectrumPointer = &editedImageSpectrum_;
					spectrumState = 0;
				} else if (spectrumKey.has_value() &&
					editedImageSpectrumUnavailableKey_.has_value() &&
					*editedImageSpectrumUnavailableKey_ == *spectrumKey) {
					spectrumState = 2;
				}
			} else if (!spectrumKey.has_value()) {
				spectrumState = 2;
			} else if (currentSourceSpectrumValid_ && currentSourceSpectrumKey_.has_value() &&
				*currentSourceSpectrumKey_ == *spectrumKey) {
				spectrumPointer = &currentSourceSpectrum_;
				spectrumState = 0;
			} else if (currentSourceSpectrumUnavailableKey_.has_value() &&
				*currentSourceSpectrumUnavailableKey_ == *spectrumKey) {
				spectrumState = 2;
			}
			if (spectrumState == 1) spectrumStatus = "Histogram loading...";
			else if (spectrumState == 2) spectrumStatus = "Histogram unavailable";
		}

		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		std::ostringstream cacheKey;
		const std::string& linesKey = imageInfoLineCache_.Key();
		cacheKey << linesKey.size() << ':' << linesKey << '|'
			<< gpsLinkAvailable << '|'
			<< windowWidth << ':' << windowHeight << '|'
			<< runtimeSettings_.Values().showFilename << ':' << runtimeSettings_.Values().showHistogram << '|'
			<< OverlayLineHeight() << ':' << FilenameOverlayHeight() << '|'
			<< imageSession_.DocumentRevision() << ':' << currentAnimationFrame_ << ':'
			<< currentImageRotationQuarterTurns_ << ':' << imageSpectrumPresentationRevision_ << '|'
			<< spectrumState;
		const std::string key = cacheKey.str();
		return imageInfoPaintPlanCache_.GetOrBuild(key, lastMouseX_, lastMouseY_,
			[this, &sourceLines, windowWidth, windowHeight, spectrumPointer,
				spectrumStatus, gpsLinkAvailable] {
				if (sourceLines.empty()) return jpegview_linux::InformationOverlayPaintPlan{};
				std::vector<std::string> lines = sourceLines;
				if (!spectrumStatus.empty()) lines.insert(lines.begin(), spectrumStatus);
				int gpsLocationLineIndex = -1;
				if (gpsLinkAvailable) {
					const auto locationLine = std::find_if(lines.begin(), lines.end(),
						[](const std::string& line) {
							return line.compare(0, 10, "Location: ") == 0;
						});
					if (locationLine != lines.end()) {
						gpsLocationLineIndex = static_cast<int>(
							std::distance(lines.begin(), locationLine));
					}
				}
				int contentWidth = 0;
				for (std::string& line : lines) {
					line = InfoText(line);
					contentWidth = std::max(contentWidth, TextWidth(line, kUiTextScale));
				}
				const jpegview_linux::OverlayLayout layout =
					jpegview_linux::InformationOverlayLayout(contentWidth, lines.size(),
						windowWidth, windowHeight, runtimeSettings_.Values().showFilename, kOverlayInset,
						kOverlayTextPadding, OverlayLineHeight(), FilenameOverlayHeight(),
						runtimeSettings_.Values().showHistogram);
				for (std::string& line : lines) {
					if (TextWidth(line, kUiTextScale) <= layout.textWidth) continue;
					line = ClipText(line, layout.textWidth);
				}
				return jpegview_linux::InformationOverlayPaint(layout, lines,
					OverlayLineHeight(), kOverlayTextPadding, runtimeSettings_.Values().showHistogram,
					spectrumPointer, false, gpsLocationLineIndex);
			});
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
			TextLineHeight(kUiTextScale), runtimeSettings_.Values().selectionModeEnabled,
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
		if (index == 0) runtimeSettings_.Values().unsharpMaskRadius = value;
		else if (index == 1) runtimeSettings_.Values().unsharpMaskAmount = value;
		else runtimeSettings_.Values().unsharpMaskThreshold = value;
		imageProcessing_.unsharpRadius = runtimeSettings_.Values().unsharpMaskRadius;
		imageProcessing_.unsharpAmount = runtimeSettings_.Values().unsharpMaskAmount;
		imageProcessing_.unsharpThreshold = runtimeSettings_.Values().unsharpMaskThreshold;
		RefreshPictureLevels(false);
	}

	void OpenUnsharpMaskDialog() {
		if (fileList_.Empty() || CurrentImage().width <= 0) return;
		unsharpOriginalProcessing_ = imageProcessing_;
		unsharpOriginalRadius_ = runtimeSettings_.Values().unsharpMaskRadius;
		unsharpOriginalAmount_ = runtimeSettings_.Values().unsharpMaskAmount;
		unsharpOriginalThreshold_ = runtimeSettings_.Values().unsharpMaskThreshold;
		if (imageProcessing_.unsharpAmount > 0.0) {
			runtimeSettings_.Values().unsharpMaskRadius = imageProcessing_.unsharpRadius;
			runtimeSettings_.Values().unsharpMaskAmount = imageProcessing_.unsharpAmount;
			runtimeSettings_.Values().unsharpMaskThreshold = imageProcessing_.unsharpThreshold;
		}
		unsharpDialogOpen_ = true;
		unsharpDraggingControl_ = -1;
		imageProcessing_.unsharpRadius = runtimeSettings_.Values().unsharpMaskRadius;
		imageProcessing_.unsharpAmount = runtimeSettings_.Values().unsharpMaskAmount;
		imageProcessing_.unsharpThreshold = runtimeSettings_.Values().unsharpMaskThreshold;
		RefreshPictureLevels(false);
	}

	void CancelUnsharpMaskDialog() {
		imageProcessing_.unsharpRadius = unsharpOriginalProcessing_.unsharpRadius;
		imageProcessing_.unsharpAmount = unsharpOriginalProcessing_.unsharpAmount;
		imageProcessing_.unsharpThreshold = unsharpOriginalProcessing_.unsharpThreshold;
		runtimeSettings_.Values().unsharpMaskRadius = unsharpOriginalRadius_;
		runtimeSettings_.Values().unsharpMaskAmount = unsharpOriginalAmount_;
		runtimeSettings_.Values().unsharpMaskThreshold = unsharpOriginalThreshold_;
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
		jpegview_linux::UnsharpMaskDialogPaint paint;
		paint.visible = unsharpDialogOpen_;
		if (!paint.visible) return;
		paint.dialog = UnsharpMaskDialogRect();
		const char* labels[] = {"Radius", "Amount", "Threshold"};
		const double values[] = {runtimeSettings_.Values().unsharpMaskRadius,
			runtimeSettings_.Values().unsharpMaskAmount,
			runtimeSettings_.Values().unsharpMaskThreshold};
		const double maxima[] = {5.0, 10.0, 20.0};
		for (int index = 0; index < 3; ++index) {
			jpegview_linux::UnsharpSliderPaint& slider =
				paint.sliders[static_cast<std::size_t>(index)];
			slider.rect = UnsharpMaskSliderRect(index);
			slider.label = labels[index];
			std::ostringstream value;
			value << std::fixed << std::setprecision(2) << values[index];
			slider.value = value.str();
			const int left = slider.rect.x + 156;
			const int right = slider.rect.x + slider.rect.w - 12;
			slider.knobX = left + static_cast<int>(std::lround(
				values[index] / maxima[index] * (right - left)));
		}
		const int cancelIndex = 0;
		paint.buttons[cancelIndex] = {UnsharpMaskActionRect(false), "Cancel",
			PointInRect(lastMouseX_, lastMouseY_, UnsharpMaskActionRect(false))};
		paint.buttons[1] = {UnsharpMaskActionRect(true), "Apply",
			PointInRect(lastMouseX_, lastMouseY_, UnsharpMaskActionRect(true))};
		editingDialogRenderer_.Render(paint);
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
					if (runtimeSettings_.Values().keepPictureLevels && (action == 2 || action == 3)) return;
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
		if (CurrentImage().width <= 0 || fileList_.Empty()) return;
		pictureLevelsPanelOpen_ = true;
		levelsDraggingControl_ = -1;
	}

	void SaveCurrentPictureLevels() {
		if (fileList_.Empty() || runtimeSettings_.Values().keepPictureLevels) return;
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
		const jpegview_linux::ImageProcessingParams previousProcessing = runtimeSettings_.Values().defaultImageProcessing;
		const bool previousAutoContrast = runtimeSettings_.Values().autoContrast;
		runtimeSettings_.Values().defaultImageProcessing = imageProcessing_;
		runtimeSettings_.Values().autoContrast = autoContrastEnabled_;
		runtimeSettings_.Values().defaultImageProcessing.unsharpRadius = 1.0;
		runtimeSettings_.Values().defaultImageProcessing.unsharpAmount = 0.0;
		runtimeSettings_.Values().defaultImageProcessing.unsharpThreshold = 4.0;
		if (!SaveSettings()) {
			runtimeSettings_.Values().defaultImageProcessing = previousProcessing;
			runtimeSettings_.Values().autoContrast = previousAutoContrast;
			SetTitle("Could not save default picture levels");
			return;
		}
		PrepareImagePrefetch();
		SetTitle(jpegview_linux::IsDefaultImageProcessing(runtimeSettings_.Values().defaultImageProcessing) ?
			"Default picture levels restored to neutral" :
			"Current picture levels saved as defaults for other images");
	}

	void ClearCurrentPictureLevels() {
		if (fileList_.Empty() || runtimeSettings_.Values().keepPictureLevels) return;
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
		imageProcessing_ = runtimeSettings_.Values().defaultImageProcessing;
		autoContrastEnabled_ = runtimeSettings_.Values().autoContrast;
		imageProcessing_.unsharpRadius = runtimeSettings_.Values().unsharpMaskRadius;
		imageProcessing_.unsharpAmount = 0.0;
		imageProcessing_.unsharpThreshold = runtimeSettings_.Values().unsharpMaskThreshold;
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
			runtimeSettings_.Values().keepPictureLevels = !runtimeSettings_.Values().keepPictureLevels;
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
			runtimeSettings_.Values().keepPictureLevels ? "Keep levels: on" : "Keep levels: off",
			"Save to DB", "Remove from DB", "Reset", "Unsharp mask...", "Close",
		};
		for (int action = 0; action < 7; ++action) {
			const SDL_Rect button = PictureLevelsActionRect(action);
			const bool disabled = runtimeSettings_.Values().keepPictureLevels && (action == 2 || action == 3);
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
		if (!runtimeSettings_.Values().navigationPanelEnabled || !controlsVisible_) return false;
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
		case jpegview_linux::kCommandOpenGpsLocation:
			OpenGpsMap();
			break;
		case jpegview_linux::kCommandGoToImageNumber:
			OpenGoToImageNumberDialog();
			break;
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
			SetCropAspect(runtimeSettings_.Values().userCropAspectWidth, runtimeSettings_.Values().userCropAspectHeight);
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
						markedTarget, ImageLoadStatePolicy::PreserveCurrent, previousPath);
				} else if (fileList_.ToggleBetweenMarkedAndCurrent()) {
					LoadCurrent();
				}
			}
			break;
		case IDM_MARK_FOR_TOGGLE:
			if (!clipboardMode_ && CurrentImage().width > 0 && CurrentImage().height > 0) {
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
			runtimeSettings_.Values().keepPictureLevels = !runtimeSettings_.Values().keepPictureLevels;
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
			runtimeSettings_.Values().infoVisible = !runtimeSettings_.Values().infoVisible;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
			SaveSettings();
			break;
		case IDM_SHOW_FILENAME:
			runtimeSettings_.Values().showFilename = !runtimeSettings_.Values().showFilename;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
			SaveSettings();
			break;
		case IDM_SHOW_NAVPANEL:
			runtimeSettings_.Values().navigationPanelEnabled = !runtimeSettings_.Values().navigationPanelEnabled;
			UpdateNavigationPanelVisibility(lastMouseX_, lastMouseY_);
			SaveSettings();
			break;
		case jpegview_linux::kCommandToggleThumbnailPanel:
			runtimeSettings_.Values().thumbnailPanelVisible = !runtimeSettings_.Values().thumbnailPanelVisible;
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
			runtimeSettings_.Values().showZoomNavigator = !runtimeSettings_.Values().showZoomNavigator;
			SaveSettings();
			UpdateCropCursor(lastMouseX_, lastMouseY_);
			UpdateZoomNavigatorCursor(lastMouseX_, lastMouseY_);
			break;
		case jpegview_linux::kCommandToggleMagnifyingGlass:
			if (CurrentImage().width > 0 && CurrentImage().height > 0 &&
				(!fileList_.Empty() || clipboardMode_)) {
				SetMagnifyingGlassEnabled(!magnifyingGlass_.Enabled());
			}
			break;
		case jpegview_linux::kCommandToggleDoublePageMode:
			doublePageModeEnabled_ = !doublePageModeEnabled_;
			runtimeSettings_.Values().doublePageModeEnabled = doublePageModeEnabled_;
			ClearTransition();
			if (!clipboardMode_ && !fileList_.Empty() &&
				imageSession_.OwnsLoadedPath(fileList_.Current())) {
				recentFiles_.RememberDoublePageMode(imageSession_.LoadedPath(),
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
			runtimeSettings_.Values().mangaReadingOrderEnabled = mangaReadingOrderEnabled_;
			ClearTransition();
			if (!clipboardMode_ && !fileList_.Empty() &&
				imageSession_.OwnsLoadedPath(fileList_.Current())) {
				recentFiles_.RememberDoublePageMode(imageSession_.LoadedPath(),
					{doublePageModeEnabled_, mangaReadingOrderEnabled_});
			}
			currentDisplayRequest_.reset();
			RefreshDoublePageRenderState();
			PrepareImagePrefetch();
			SaveSettings();
			SetTitle();
			break;
		case jpegview_linux::kCommandToggleSelectionMode:
			SetSelectionModeEnabled(!runtimeSettings_.Values().selectionModeEnabled);
			break;
		case jpegview_linux::kToggleNavigationPanelAutoReveal:
			runtimeSettings_.Values().navigationPanelAutoReveal = !runtimeSettings_.Values().navigationPanelAutoReveal;
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
			BeginFileListSort(jpegview_linux::FileList::SortMode::LastModificationTime,
				fileList_.IsSortedAscending());
			break;
		case jpegview_linux::kNavigationSortModeCommand:
			BeginFileListSort(
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::FileName ?
					jpegview_linux::FileList::SortMode::LastModificationTime :
					jpegview_linux::FileList::SortMode::FileName,
				fileList_.IsSortedAscending());
			break;
		case IDM_SORT_CREATION_DATE:
			BeginFileListSort(jpegview_linux::FileList::SortMode::CreationTime,
				fileList_.IsSortedAscending());
			break;
		case IDM_SORT_NAME:
			BeginFileListSort(jpegview_linux::FileList::SortMode::FileName,
				fileList_.IsSortedAscending());
			break;
		case IDM_SORT_RANDOM:
			BeginFileListSort(jpegview_linux::FileList::SortMode::Random,
				fileList_.IsSortedAscending());
			break;
		case IDM_SORT_SIZE:
			BeginFileListSort(jpegview_linux::FileList::SortMode::FileSize,
				fileList_.IsSortedAscending());
			break;
		case IDM_SORT_ASCENDING:
			BeginFileListSort(fileList_.GetSorting(), true);
			break;
		case IDM_SORT_DESCENDING:
			BeginFileListSort(fileList_.GetSorting(), false);
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
		state.infoVisible = runtimeSettings_.Values().infoVisible;
		state.gpsLocationAvailable = CurrentGpsCoordinatesAvailable();
		state.gpsMapProviderValid = CurrentGpsMapActionAvailable();
		state.filenameVisible = runtimeSettings_.Values().showFilename;
		state.navigationPanelEnabled = runtimeSettings_.Values().navigationPanelEnabled;
		state.navigationPanelAutoReveal = runtimeSettings_.Values().navigationPanelAutoReveal;
		state.thumbnailPanelVisible = runtimeSettings_.Values().thumbnailPanelVisible;
		state.showZoomNavigator = runtimeSettings_.Values().showZoomNavigator;
		state.magnifyingGlassEnabled = magnifyingGlass_.Enabled();
		state.doublePageModeEnabled = doublePageModeEnabled_;
		state.mangaReadingOrderEnabled = mangaReadingOrderEnabled_;
		state.selectionModeEnabled = runtimeSettings_.Values().selectionModeEnabled;
		state.spacebarNavigatesImages = runtimeSettings_.Values().spacebarNavigatesImages;
		state.navigationMode = fileList_.GetNavigationMode();
		state.sortMode = fileList_.GetSorting();
		state.sortAscending = fileList_.IsSortedAscending();
		state.imageAvailable = CurrentImage().width > 0;
		state.fileListAvailable = !fileList_.Empty() && !clipboardMode_;
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
		state.userCropAspectWidth = runtimeSettings_.Values().userCropAspectWidth;
		state.userCropAspectHeight = runtimeSettings_.Values().userCropAspectHeight;
		state.autoCorrectionEnabled = autoContrastEnabled_;
		state.pictureLevelsAvailable = !clipboardMode_ && !fileList_.Empty() && CurrentImage().width > 0;
		state.localDensityEnabled = imageProcessing_.localDensityEnabled;
		state.keepPictureLevels = runtimeSettings_.Values().keepPictureLevels;
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
		entries.reserve(fileList_.Size());
		for (std::size_t index = 0; index < fileList_.Size(); ++index) {
			BatchCopyItem item;
			item.source = fileList_.Files()[index];
			const jpegview_linux::SourceDescriptor* descriptor = fileList_.DescriptorAt(index);
			if (descriptor != nullptr && descriptor->Metadata().hasModificationTime) {
				item.modificationTime = static_cast<std::time_t>(
					descriptor->Metadata().modificationTimeNanoseconds / 1000000000LL);
			}
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
		runtimeSettings_.Values().copyRenamePattern = batchCopyDialog_.Pattern();
		SaveSettings();
		batchCopyDialog_.SetMessage("Saved batch pattern");
	}

	void PerformBatchCopy() {
		if (batchCopyDialog_.Pattern().empty()) {
			batchCopyDialog_.Preview();
			return;
		}
		batchCopyDialog_.Preview();
		jpegview_linux::BatchCopyOperation operation;
		operation.preferredCurrentPath = fileList_.Current();
		for (const BatchCopyItem& item : batchCopyDialog_.Items()) {
			if (item.selected) operation.items.push_back(item);
		}
		if (operation.items.empty()) return;
		PendingFileOperationUi pending;
		pending.sourcePath = fileList_.Current();
		pending.directoryPath = fileList_.Current().parent_path();
		pending.batchDialog = true;
		if (!SubmitFileOperation(std::move(operation), std::move(pending))) {
			batchCopyDialog_.SetMessage("Another file operation is still in progress");
			return;
		}
		batchCopyDialog_.SetMessage("Applying selected file operations… Press ESC to cancel");
	}

	void OpenBatchCopyDialog() {
		if (fileList_.Empty() || clipboardMode_) return;
		if (jpegview_linux::IsArchiveMemberLocation(fileList_.Current())) {
			SetTitle("Batch rename/copy is unavailable for images inside archives");
			return;
		}
		batchCopyDialog_.Open(CollectBatchCopyEntries(), fileList_.CurrentIndex(),
			runtimeSettings_.Values().copyRenamePattern, BatchCopyVisibleRows());
		contextMenuOpen_ = false;
		fileDialogOpen_ = false;
		SDL_StartTextInput();
	}

	void CloseBatchCopyDialog() {
		if (pendingFileOperation_.has_value() &&
			pendingFileOperation_->kind == jpegview_linux::FileOperationKind::BatchCopy) {
			fileOperationService_.Cancel(pendingFileOperation_->id);
			batchCopyDialog_.SetMessage("Cancelling after the current file operation…");
			return;
		}
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
		if (pendingFileOperation_.has_value() &&
			pendingFileOperation_->kind == jpegview_linux::FileOperationKind::BatchCopy) {
			if (event.type == SDL_QUIT) {
				running = false;
			} else if ((event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
				event.key.keysym.sym == SDLK_ESCAPE) ||
				(event.type == SDL_MOUSEBUTTONDOWN &&
					PointInRect(event.button.x, event.button.y,
						BatchCopyButtonRect(kBatchClose)))) {
				CloseBatchCopyDialog();
			}
			return;
		}
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

	void RenderBatchCopy() {
		jpegview_linux::BatchCopyDialogPaint paint;
		paint.visible = batchCopyDialog_.IsOpen();
		if (!paint.visible) return;
		paint.dialog = BatchCopyRect();
		paint.list = BatchCopyListRect();
		paint.rightX = BatchCopyRightX();
		paint.rightWidth = BatchCopyRightWidth();
		paint.pattern = BatchCopyPatternRect();
		paint.patternFocused = batchCopyDialog_.PatternFocused();
		const std::string directory = fileList_.Empty() ? std::string() :
			fileList_.Current().parent_path().string();
		paint.directory = ClipText("IMAGE FILES IN " + directory, paint.dialog.w - 36);
		paint.patternText = ClipText(batchCopyDialog_.Pattern(), paint.pattern.w - 16);
		paint.message = ClipText(batchCopyDialog_.Message(), paint.rightWidth);
		const int visibleRows = BatchCopyVisibleRows();
		for (int rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
			const int itemIndex = static_cast<int>(batchCopyDialog_.Scroll()) + rowIndex;
			if (itemIndex >= static_cast<int>(batchCopyDialog_.Items().size())) break;
			const BatchCopyItem& item = batchCopyDialog_.Items()[
				static_cast<std::size_t>(itemIndex)];
			const std::string destination = item.destinationText.empty() ? "-" :
				std::string(item.copy ? ">> " : "") + InfoText(item.destinationText);
			paint.rows.push_back({item.selected, itemIndex == batchCopyDialog_.Cursor(),
				ClipText(InfoText(item.source.filename().string()), 190),
				ClipText(jpegview_linux::FormatBatchDate(item.modificationTime), 125),
				ClipText(destination, paint.list.w - 390), item.copy});
		}
		const int buttonIds[] = {kBatchSelectAll, kBatchSelectNone, kBatchPreview,
			kBatchSavePattern, kBatchRename, kBatchClose};
		const char* buttonLabels[] = {"SELECT ALL", "SELECT NONE", "PREVIEW",
			"SAVE TEMPLATE", "RENAME/COPY", "CLOSE"};
		for (std::size_t index = 0; index < paint.buttons.size(); ++index) {
			const SDL_Rect rect = BatchCopyButtonRect(buttonIds[index]);
			paint.buttons[index] = {rect, ClipText(buttonLabels[index], rect.w - 12),
				PointInRect(lastMouseX_, lastMouseY_, rect)};
		}
		editingDialogRenderer_.Render(paint);
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
		if (CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
		resizeDialog_.Open(CurrentImage().width, CurrentImage().height);
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
		if (width == CurrentImage().width && height == CurrentImage().height) {
			CloseResizeDialog();
			return;
		}
		jpegview_linux::ImageOperationSpec operation;
		operation.kind = jpegview_linux::ImageOperationKind::Resize;
		operation.width = width;
		operation.height = height;
		operation.resizeFilter = resizeDialog_.Model().Filter();
		if (!RequestImageOperation(operation, ImageOperationPurpose::Resize)) {
			resizeDialog_.SetMessage("Resizing could not be queued");
		}
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

	void RenderResizeDialog() {
		jpegview_linux::ResizeDialogPaint paint;
		paint.visible = resizeDialog_.IsOpen();
		if (!paint.visible) return;
		paint.dialog = ResizeDialogRect();
		paint.originalSize = std::to_string(resizeDialog_.Model().OriginalWidth()) + "X" +
			std::to_string(resizeDialog_.Model().OriginalHeight());
		paint.message = ClipText(resizeDialog_.Message(), paint.dialog.w - 40);
		const char* labels[] = {"NEW SIZE", "NEW WIDTH", "NEW HEIGHT", "FILTER"};
		for (int fieldIndex = kResizePercent; fieldIndex <= kResizeFilter; ++fieldIndex) {
			const std::size_t index = static_cast<std::size_t>(fieldIndex);
			jpegview_linux::ResizeFieldPaint& field = paint.fields[index];
			field.rect = ResizeFieldRect(fieldIndex);
			field.label = labels[fieldIndex];
			const std::string value = fieldIndex == kResizeFilter ? ResizeFilterName() :
				resizeDialog_.Model().FieldText(fieldIndex);
			field.value = ClipText(value, field.rect.w - 16);
			field.focused = resizeDialog_.FocusedField() == fieldIndex;
			if (fieldIndex == kResizePercent) field.suffix = "%";
			if (fieldIndex == kResizeWidth || fieldIndex == kResizeHeight) {
				field.suffix = "PIXELS";
			}
		}
		const int buttonIds[] = {kResizeApply, kResizeCancel};
		const char* buttonLabels[] = {"APPLY", "CANCEL"};
		for (std::size_t index = 0; index < paint.buttons.size(); ++index) {
			const SDL_Rect rect = ResizeButtonRect(buttonIds[index]);
			paint.buttons[index] = {rect, buttonLabels[index],
				PointInRect(lastMouseX_, lastMouseY_, rect)};
		}
		editingDialogRenderer_.Render(paint);
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

	void RenderFixedCropSizeDialog() {
		jpegview_linux::FixedCropSizeDialogPaint paint;
		paint.visible = cropSizeDialog_.IsOpen();
		if (!paint.visible) return;
		paint.dialog = CropSizeDialogRect();
		paint.message = ClipText(cropSizeDialog_.Message(), paint.dialog.w - 40);
		const char* labels[] = {"WIDTH", "HEIGHT"};
		const std::string values[] = {cropSizeDialog_.WidthText(),
			cropSizeDialog_.HeightText()};
		for (int fieldIndex = 0; fieldIndex < 2; ++fieldIndex) {
			jpegview_linux::CropSizeFieldPaint& field =
				paint.fields[static_cast<std::size_t>(fieldIndex)];
			field.rect = CropSizeFieldRect(fieldIndex);
			field.label = labels[fieldIndex];
			field.value = ClipText(values[fieldIndex], field.rect.w - 16);
			field.focused = cropSizeDialog_.FocusedField() == fieldIndex;
		}
		paint.units[0] = {CropSizeUnitRect(true), "Screen pixels",
			cropSizeDialog_.UsesScreenPixels()};
		paint.units[1] = {CropSizeUnitRect(false), "Image pixels",
			!cropSizeDialog_.UsesScreenPixels()};
		const int buttonIds[] = {kCropSizeApply, kCropSizeCancel};
		const char* buttonLabels[] = {"APPLY", "CANCEL"};
		for (std::size_t index = 0; index < paint.buttons.size(); ++index) {
			const SDL_Rect rect = CropSizeButtonRect(buttonIds[index]);
			paint.buttons[index] = {rect, buttonLabels[index],
				PointInRect(lastMouseX_, lastMouseY_, rect)};
		}
		editingDialogRenderer_.Render(paint);
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

	SDL_Rect GoToImageNumberDialogRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(460, std::max(320, windowWidth - 32));
		const int height = 190;
		return {(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
	}

	SDL_Rect GoToImageNumberButtonRect(bool go) const {
		const SDL_Rect dialog = GoToImageNumberDialogRect();
		return {dialog.x + dialog.w - (go ? 196 : 104), dialog.y + dialog.h - 48,
			84, 30};
	}

	void OpenGoToImageNumberDialog() {
		if (fileList_.Empty() || clipboardMode_) {
			SetTitle("Go to image number is unavailable without an active file list");
			return;
		}
		goToImageNumberDialog_.Open(fileList_.Size(), fileList_.CurrentIndex());
		if (!goToImageNumberDialog_.IsOpen()) return;
		contextMenuOpen_ = false;
		contextMenuCropOnly_ = false;
		SDL_StartTextInput();
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
		SetTitle("Go to image number");
	}

	void CloseGoToImageNumberDialog() {
		if (!goToImageNumberDialog_.IsOpen()) return;
		SDL_StopTextInput();
		goToImageNumberDialog_.Close();
		frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
		SetTitle();
	}

	void ApplyGoToImageNumberDialog() {
		const std::optional<std::size_t> target = goToImageNumberDialog_.Submit();
		if (!target.has_value()) {
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
			return;
		}
		const std::size_t previous = fileList_.CurrentIndex();
		CloseGoToImageNumberDialog();
		if (*target == previous) return;
		if (fileList_.Select(*target)) LoadCurrent(*target > previous ? 1 : -1);
	}

	void HandleGoToImageNumberDialogEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN:
			if (event.key.repeat != 0) break;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseGoToImageNumberDialog();
			} else if (event.key.keysym.sym == SDLK_RETURN) {
				ApplyGoToImageNumberDialog();
			} else if ((event.key.keysym.mod & 0x00c0u) != 0 &&
				event.key.keysym.sym == 'a') {
				goToImageNumberDialog_.SelectAll();
			} else if (event.key.keysym.sym == SDLK_BACKSPACE) {
				goToImageNumberDialog_.Backspace();
			}
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
			break;
		case SDL_TEXTINPUT:
			(void)goToImageNumberDialog_.AppendText(event.text.text);
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
			break;
		case SDL_MOUSEMOTION:
			lastMouseX_ = event.motion.x;
			lastMouseY_ = event.motion.y;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
			break;
		case SDL_MOUSEBUTTONDOWN:
			if (event.button.button != SDL_BUTTON_LEFT) break;
			if (PointInRect(event.button.x, event.button.y, GoToImageNumberButtonRect(true))) {
				ApplyGoToImageNumberDialog();
			} else if (PointInRect(event.button.x, event.button.y,
				GoToImageNumberButtonRect(false))) {
				CloseGoToImageNumberDialog();
			} else {
				goToImageNumberDialog_.SelectAll();
			}
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
			break;
		default:
			break;
		}
	}

	void RenderGoToImageNumberDialog() {
		if (!goToImageNumberDialog_.IsOpen()) return;
		const SDL_Rect dialog = GoToImageNumberDialogRect();
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 238);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 160, 190, 225);
		DrawText("GO TO IMAGE NUMBER", dialog.x + 18, dialog.y + 14,
			kUiTextScale, 255, 255, 255);
		DrawText("Enter a position from 1 to " +
			std::to_string(goToImageNumberDialog_.ImageCount()), dialog.x + 18,
			dialog.y + 48, kUiTextScale, 210, 220, 240);
		const SDL_Rect input{dialog.x + 18, dialog.y + 78, dialog.w - 36, 34};
		SDL_SetRenderDrawColor(renderer_, 24, 27, 32, 255);
		SDL_RenderFillRect(renderer_, &input);
		DrawRect(input, 115, 155, 205);
		DrawText(ClipText(goToImageNumberDialog_.Text(), input.w - 16),
			input.x + 8, input.y + (input.h - TextLineHeight()) / 2,
			kUiTextScale, 245, 245, 245);
		if (!goToImageNumberDialog_.Message().empty()) {
			DrawText(ClipText(goToImageNumberDialog_.Message(), dialog.w - 36),
				dialog.x + 18, dialog.y + 120, kUiTextScale, 255, 145, 135);
		}
		const SDL_Rect go = GoToImageNumberButtonRect(true);
		SDL_SetRenderDrawColor(renderer_, 38, 75, 105, 255);
		SDL_RenderFillRect(renderer_, &go);
		DrawRect(go, 135, 175, 205);
		DrawText("Go", go.x + (go.w - TextWidth("Go", kUiTextScale)) / 2,
			go.y + (go.h - TextLineHeight()) / 2, kUiTextScale, 245, 245, 250);
		const SDL_Rect cancel = GoToImageNumberButtonRect(false);
		SDL_SetRenderDrawColor(renderer_, 40, 40, 42, 255);
		SDL_RenderFillRect(renderer_, &cancel);
		DrawRect(cancel, 115, 115, 120);
		DrawText("Cancel", cancel.x + (cancel.w - TextWidth("Cancel", kUiTextScale)) / 2,
			cancel.y + (cancel.h - TextLineHeight()) / 2, kUiTextScale, 225, 225, 230);
	}

	SDL_Rect FileDialogRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int maximumWidth = std::max(kFileDialogMinimumWidth, windowWidth - 40);
		const int maximumHeight = std::max(kFileDialogMinimumHeight, windowHeight - 40);
		const int width = std::clamp(runtimeSettings_.Values().fileDialogWidth, kFileDialogMinimumWidth, maximumWidth);
		const int height = std::clamp(runtimeSettings_.Values().fileDialogHeight, kFileDialogMinimumHeight, maximumHeight);
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
		const int requestedWidth = runtimeSettings_.Values().fileDialogPreviewRatio > 0.0 ?
			static_cast<int>(std::lround(runtimeSettings_.Values().fileDialogPreviewRatio * availableWidth)) : defaultWidth;
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
			runtimeSettings_.Values().fileDialogWidth = std::clamp(fileDialogDragStartRect_.w + x - fileDialogDragStartX_,
				std::min(kFileDialogMinimumWidth, availableWidth), availableWidth);
			runtimeSettings_.Values().fileDialogHeight = std::clamp(fileDialogDragStartRect_.h + y - fileDialogDragStartY_,
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
			runtimeSettings_.Values().fileDialogPreviewRatio = static_cast<double>(previewWidth) / availableWidth;
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
		if (fileDialogPreviewTexture_ != nullptr) DestroyTextureMeasured(fileDialogPreviewTexture_);
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
		if (fileDialogTab_ == FileDialogTab::Browse && fileDialogListingPending_) {
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
				if (fileDialogPreviewTexture_ != nullptr) DestroyTextureMeasured(fileDialogPreviewTexture_);
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
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
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
						DestroyTextureMeasured(fileDialogPreviewTexture_);
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
						DestroyTextureMeasured(fileDialogPreviewTexture_);
					}
					fileDialogPreviewTexture_ = previewTexture;
					fileDialogPreviewWidth_ = result.width;
					fileDialogPreviewHeight_ = result.height;
					fileDialogPreviewHasTransparency_ = result.hasTransparency;
				}
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
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
		}
	}

	void RequestFileDialogFileSizes() {
		++fileDialogFileSizeGeneration_;
		std::vector<jpegview_linux::SourceDescriptor> sources;
		std::unordered_set<std::string> seen;
		const auto appendUnknownEntry = [&sources, &seen](const FileDialogEntry& entry) {
			if (entry.directory || (entry.sourceDescriptor.Valid() && entry.fileSizeKnown)) return;
			const std::string identity = entry.path.lexically_normal().string();
			if (!seen.insert(identity).second) return;
			sources.push_back(entry.sourceDescriptor.LogicalPath().empty() ?
				jpegview_linux::SourceDescriptor(entry.path, {}, {}) : entry.sourceDescriptor);
		};
		const auto appendUnknownFiles = [this, &appendUnknownEntry](
			const jpegview_linux::FileDialogModel& model) {
			if (const FileDialogEntry* selected = model.SelectedEntry()) {
				appendUnknownEntry(*selected);
			}
			const auto rows = model.Entries();
			const int begin = std::clamp(model.Scroll(), 0, static_cast<int>(rows.size()));
			const int end = std::min(static_cast<int>(rows.size()),
				begin + FileDialogVisibleRows());
			for (int index = begin; index < end; ++index) {
				appendUnknownEntry(rows[static_cast<std::size_t>(index)]);
			}
			for (const FileDialogEntry& entry : model.AllEntries()) appendUnknownEntry(entry);
		};
		if (!fileDialogSave_ && !fileDialogParameterBackup_ && !fileDialogParameterRestore_) {
			appendUnknownFiles(ActiveFileDialogModel());
			appendUnknownFiles(fileDialogTab_ == FileDialogTab::Browse ?
				recentFileDialogModel_ : fileDialogModel_);
		}
		fileDialogFileSizeLoader_.RequestSources(sources, fileDialogFileSizeGeneration_);
	}

	void TickFileDialogFileSizes() {
		for (const jpegview_linux::FileDialogFileSizeResult& result :
			fileDialogFileSizeLoader_.TakeReady()) {
			if (!fileDialogOpen_ || result.generation != fileDialogFileSizeGeneration_) continue;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
			if (!result.observedSource.LogicalPath().empty()) {
				const jpegview_linux::SourceKey previous = result.requestedSource.Key();
				fileDialogModel_.RefreshSourceDescriptor(previous, result.observedSource,
					result.parentDisplayName);
				recentFileDialogModel_.RefreshSourceDescriptor(previous,
					result.observedSource, result.parentDisplayName);
			}
			if (result.observedSource.Metadata().hasFileSize) {
				fileDialogModel_.SetFileSize(result.path, result.size);
				recentFileDialogModel_.SetFileSize(result.path, result.size);
			}
		}
	}

	void TickFileDialogDirectoryListing() {
		for (jpegview_linux::FileDialogDirectoryResult& result :
			fileDialogDirectoryLoader_.TakeReady()) {
			const bool includeNonImageFiles = fileDialogParameterBackup_ ||
				fileDialogParameterRestore_;
			const bool includeArchives = !fileDialogSave_ && !includeNonImageFiles;
			if (!fileDialogOpen_ || result.generation != fileDialogDirectoryGeneration_ ||
				result.directory != fileDialogDirectory_ || result.directory.empty() ||
				result.policy.saveDialog != fileDialogSave_ ||
				result.policy.includeNonImageFiles != includeNonImageFiles ||
				result.policy.includeArchives != includeArchives) continue;
			fileDialogListingPending_ = false;
			fileDialogLocationDisplayName_ = result.locationDisplayName.empty() ?
				result.directory.string() : std::move(result.locationDisplayName);
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
			if (result.containsEncryptedEntries) {
				const fs::path backing = jpegview_linux::ArchiveBackingFile(result.directory);
				encryptedArchivePaths_.insert(AbsoluteNormalized(backing).string());
			}
			std::string listingMessage;
			if (!result.error.empty()) {
				if (result.errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired ||
					result.errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword) {
					if (result.errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword) {
						jpegview_linux::ForgetSessionArchivePassword(result.directory);
					}
					const fs::path backing = jpegview_linux::ArchiveBackingFile(result.directory);
					encryptedArchivePaths_.insert(AbsoluteNormalized(backing).string());
					FileDialogEntry archiveEntry{result.directory, true, false, {},
						true, false, true};
					archiveEntry.archiveFormatName = result.archiveFormatName;
					result.entries.push_back(std::move(archiveEntry));
					listingMessage = result.errorKind ==
						jpegview_linux::ArchiveErrorKind::PasswordRequired ?
						"Archive contents are encrypted" : "Saved archive password is incorrect";
				} else {
					listingMessage = result.archiveLocation ?
						"Cannot read archive: " + result.error :
						"Cannot read folder: " + result.error;
				}
			}
			jpegview_linux::UpdateFileDialogListingMessage(fileDialogMessage_, listingMessage);
			if (!result.sortOrders.completed ||
				result.entries.size() != result.sortOrders.name.size() ||
				result.entries.size() != result.sortOrders.modificationDate.size()) {
				result.sortOrders = jpegview_linux::BuildFileDialogEntrySortOrders(result.entries);
			}
			fileDialogModel_.SetEntriesWithPreparedOrder(std::move(result.entries),
				std::move(result.sortOrders));
			if (jpegview_linux::FileDialogShouldClearSelectionAfterListing(
				result.policy.saveDialog, result.policy.includeNonImageFiles,
				fileDialogActivateAfterListing_)) {
				fileDialogModel_.ClearSelection();
			} else if (!result.policy.includeNonImageFiles &&
				!fileDialogFocusAfterListing_.empty()) {
				fileDialogModel_.Focus(fileDialogFocusAfterListing_, FileDialogVisibleRows());
			} else if (!result.policy.includeNonImageFiles && !fileList_.Empty() &&
				fileList_.Current().parent_path() == fileDialogDirectory_) {
				fileDialogModel_.Focus(AbsoluteNormalized(fileList_.Current()),
					FileDialogVisibleRows());
			}
			fileDialogFocusAfterListing_.clear();
			RequestFileDialogFileSizes();
			RequestFileDialogDirectorySummaries();
			if (result.errorKind == jpegview_linux::ArchiveErrorKind::PasswordRequired &&
				!jpegview_linux::HasSessionArchivePassword(result.directory)) {
				BeginArchivePasswordDialog(result.directory);
			} else if (result.errorKind == jpegview_linux::ArchiveErrorKind::InvalidPassword) {
				BeginArchivePasswordDialog(result.directory,
					"Saved password is incorrect; try again");
			} else if (result.error.empty() && result.containsEncryptedEntries &&
				!jpegview_linux::HasSessionArchivePassword(result.directory)) {
				BeginArchivePasswordDialog(result.directory);
			}
			if (fileDialogActivateAfterListing_) {
				const bool openDirectoryImmediately = fileDialogOpenDirectoryAfterListing_;
				fileDialogActivateAfterListing_ = false;
				fileDialogOpenDirectoryAfterListing_ = false;
				if (fileDialogOpen_ && !archivePasswordDialog_.IsOpen()) {
					ActivateFileDialogSelection(openDirectoryImmediately);
				}
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
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
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
		fileDialogDirectoryLoader_.Clear(++fileDialogDirectoryGeneration_);
		fileDialogArchiveLoader_.Clear(++fileDialogArchiveGeneration_);
		fileDialogLocationDisplayName_ = fileDialogDirectory_.string();
		std::vector<FileDialogEntry> entries;
		const fs::path parent = fileDialogDirectory_.parent_path();
		if (!parent.empty() && parent != fileDialogDirectory_) {
			entries.push_back(FileDialogEntry{parent, true, true});
		}
		fileDialogMessage_ = jpegview_linux::FileDialogListingLoadingMessage();
		fileDialogListingPending_ = true;
		fileDialogModel_.SetEntriesInOrder(std::move(entries));
		fileDialogModel_.ClearSelection();
		++fileDialogSummaryGeneration_;
		fileDialogDirectorySummaries_.clear();
		fileDialogSummaryLoader_.Request({}, fileDialogSummaryGeneration_);
		fileDialogFileSizeLoader_.Clear(++fileDialogFileSizeGeneration_);
		jpegview_linux::FileDialogListingPolicy policy;
		policy.saveDialog = fileDialogSave_;
		policy.includeNonImageFiles = fileDialogParameterBackup_ ||
			fileDialogParameterRestore_;
		policy.includeArchives = !policy.saveDialog && !policy.includeNonImageFiles;
		policy.encryptedArchivePaths = encryptedArchivePaths_;
		fileDialogDirectoryLoader_.Request(fileDialogDirectory_,
			fileDialogDirectoryGeneration_, policy);
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
		fileDialogActivateAfterListing_ = false;
		fileDialogOpenDirectoryAfterListing_ = false;
		SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
		UpdateFileDialogCursor(lastMouseX_, lastMouseY_);
		contextMenuOpen_ = false;
		fileDialogFocusAfterListing_.clear();
		if (!fileList_.Empty() && fileList_.Current().parent_path() == fileDialogDirectory_) {
			fileDialogFocusAfterListing_ = AbsoluteNormalized(fileList_.Current());
		}
		RefreshFileDialog();
		SDL_StartTextInput();
	}

	void SwitchFileDialogTab(FileDialogTab tab) {
		if (!FileDialogHasTabs() || fileDialogTab_ == tab) return;
		fileDialogTab_ = tab;
		fileDialogActivateAfterListing_ = false;
		fileDialogOpenDirectoryAfterListing_ = false;
		InvalidateFileDialogPreview();
		RequestFileDialogFileSizes();
	}

	void RebuildRecentFileDialogEntries() {
		std::vector<FileDialogEntry> entries;
		entries.reserve(recentFiles_.Files().size());
		for (const fs::path& path : recentFiles_.Files()) {
			FileDialogEntry entry{path, false, false, {}, false,
				false};
			entry.sourceDescriptor = jpegview_linux::SourceDescriptor(path, {}, {});
			entry.parentDisplayName = path.parent_path().string();
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
		if (fileList_.Empty() || CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
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
		fileDialogActivateAfterListing_ = false;
		fileDialogOpenDirectoryAfterListing_ = false;
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
		fileDialogActivateAfterListing_ = false;
		fileDialogOpenDirectoryAfterListing_ = false;
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
		fileDialogActivateAfterListing_ = false;
		fileDialogOpenDirectoryAfterListing_ = false;
		SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
		UpdateFileDialogCursor(lastMouseX_, lastMouseY_);
		contextMenuOpen_ = false;
		RefreshFileDialog();
		fileDialogModel_.ClearSelection();
		SDL_StartTextInput();
	}

	std::string FileDialogEntryLabel(const FileDialogEntry& entry) const {
		if (entry.parent) return "[..]";
		if (entry.archiveContainer) return "[" + entry.archiveFormatName + "] " +
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
		fileDialogActivateAfterListing_ = false;
		fileDialogOpenDirectoryAfterListing_ = false;
		fileDialogDirectory_ = directory;
		if (!fileDialogSave_) fileDialogModel_.ClearFilter();
		fileDialogFocusAfterListing_ = returningToParent ? previousDirectory : fs::path{};
		RefreshFileDialog();
		if (fileDialogSave_) fileDialogModel_.ClearSelection();
		fileDialogOverwriteConfirmed_ = false;
	}

	void ActivateFileDialogSelection(bool openDirectoryImmediately = false) {
		if (fileDialogListingPending_ && fileDialogTab_ == FileDialogTab::Browse &&
			!fileDialogSave_) {
			fileDialogActivateAfterListing_ = true;
			fileDialogOpenDirectoryAfterListing_ = openDirectoryImmediately;
			return;
		}
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
			OpenDroppedFiles({entry.path.string()}, ImageLoadStatePolicy::RestoreRecent);
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

	jpegview_linux::FileDialogRenderSnapshot BuildFileDialogRenderSnapshot() const {
		jpegview_linux::FileDialogRenderSnapshot snapshot;
		snapshot.visible = fileDialogOpen_;
		if (!snapshot.visible) return snapshot;
		snapshot.dialog = FileDialogRect();
		snapshot.removeRecentButton = FileDialogRemoveRecentButtonRect();
		snapshot.browseTab = FileDialogTabRect(FileDialogTab::Browse);
		snapshot.recentsTab = FileDialogTabRect(FileDialogTab::Recents);
		snapshot.sortButton = FileDialogSortRect();
		snapshot.input = FileDialogInputRect();
		snapshot.list = FileDialogListRect();
		snapshot.listContent = FileDialogListContentRect();
		snapshot.scrollbarTrack = FileDialogScrollbarRect();
		snapshot.scrollbarThumb = FileDialogScrollbarThumbRect();
		snapshot.divider = FileDialogDividerRect();
		snapshot.resizeHandle = FileDialogResizeHandleRect();
		snapshot.showTabs = FileDialogHasTabs();
		snapshot.recentTab = fileDialogTab_ == FileDialogTab::Recents;
		snapshot.showRemoveRecent = FileDialogCanRemoveRecent();
		snapshot.recentSelectionAvailable = recentFileDialogModel_.SelectedEntry() != nullptr;
		snapshot.removeRecentHovered = snapshot.recentSelectionAvailable &&
			PointInRect(lastMouseX_, lastMouseY_, snapshot.removeRecentButton);
		snapshot.showSort = FileDialogCanSort();
		snapshot.sortByName = ActiveFileDialogModel().SortMode() ==
			jpegview_linux::FileDialogSortMode::Name;
		snapshot.showPreview = FileDialogHasPreviewColumn();
		snapshot.message = fileDialogMessage_;
		snapshot.location = snapshot.recentTab ? "One recent image per folder" :
			fileDialogLocationDisplayName_;
		snapshot.title = fileDialogLosslessCrop_ ? "Save lossless JPEG crop" :
			(fileDialogParameterBackup_ ? "Back up picture-level database" :
			(fileDialogParameterRestore_ ? "Restore picture-level database" :
			(fileDialogSave_ ? "Save processed image" : "Open image")));
		snapshot.fieldTitle = fileDialogSave_ ? "File name" :
			(fileDialogParameterRestore_ ? "Backup filter" : "Filter");
		const jpegview_linux::FileDialogModel& model = ActiveFileDialogModel();
		const std::string& inputText = fileDialogSave_ ? fileDialogFilename_ : model.Filter();
		snapshot.inputText = ClipInputText(inputText, snapshot.input.w - 20);
		snapshot.message = fileDialogMessage_;
		if (fileDialogSave_) {
			snapshot.shortcutHint = "Enter: Save   Backspace: Edit/parent   Esc: Cancel";
		} else if (fileDialogParameterRestore_) {
			snapshot.shortcutHint =
				"Type: Filter   Enter: Restore backup   Backspace: Parent   Esc: Cancel";
		} else if (snapshot.recentTab) {
			snapshot.shortcutHint =
				"Type: Filter   Ctrl+Tab: Tabs   Up/Down: Move   PgUp/Dn: Page   Del: Remove   Ctrl+Z: Undo   Enter: Open   Backspace: Filter   Esc: Cancel";
		} else {
			snapshot.shortcutHint =
				"Type: Filter   Ctrl+Tab: Tabs   Up/Down: Move   PgUp/Dn: Page   Enter: Open   Ctrl+Return: Folder   Backspace: Filter/parent   Esc: Cancel";
		}
		const int visibleRows = FileDialogVisibleRows();
		const auto entries = model.Entries();
		for (int rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
			const int item = model.Scroll() + rowIndex;
			if (item >= static_cast<int>(entries.size())) break;
			const FileDialogEntry& entry = entries[static_cast<std::size_t>(item)];
			jpegview_linux::FileDialogRenderRow row;
			row.selected = item == model.SelectedIndex();
			row.recent = snapshot.recentTab;
			row.archive = snapshot.recentTab ? entry.archiveMember :
				(entry.archiveContainer || entry.archiveMember);
			row.directory = entry.directory;
			if (snapshot.recentTab) {
				row.parent = entry.parentDisplayName.empty() ?
					entry.path.parent_path().string() : entry.parentDisplayName;
				row.filename = entry.path.filename().string();
				if (entry.fileSizeKnown) row.sizeText = jpegview_linux::FormatFileSize(entry.fileSize);
			} else {
				if (snapshot.showSort && entry.directory && !entry.parent && !entry.encrypted) {
					const auto summary = fileDialogDirectorySummaries_.find(entry.path.string());
					row.rightText = summary == fileDialogDirectorySummaries_.end() ? "Scanning..." :
						jpegview_linux::FormatDirectorySummary(summary->second);
				} else if (!entry.directory && entry.fileSizeKnown) {
					row.rightText = jpegview_linux::FormatFileSize(entry.fileSize);
				}
				row.rightText = ClipText(row.rightText,
					std::max(1, snapshot.listContent.w / 2 - 20));
				row.label = FileDialogEntryLabel(entry);
			}
			snapshot.rows.push_back(std::move(row));
		}
		snapshot.recentListEmpty = entries.empty() && snapshot.recentTab;
		if (snapshot.recentListEmpty) {
			snapshot.emptyRecentMessage = model.Filter().empty() ?
				"No recent files" : "No recent files match this filter";
		}
		const jpegview_linux::FileDialogScrollbarGeometry scrollbar = FileDialogScrollGeometry();
		snapshot.scrollbarScrollable = scrollbar.scrollable;
		snapshot.scrollbarDragging = fileDialogDragMode_ == FileDialogDragMode::Scrollbar;
		snapshot.scrollbarHovered = PointInRect(lastMouseX_, lastMouseY_, snapshot.scrollbarTrack);
		if (snapshot.showPreview) {
			snapshot.preview.pane = FileDialogPreviewRect();
			snapshot.preview.imageArea = FileDialogPreviewImageRect(snapshot.preview.pane);
			snapshot.preview.texture = fileDialogPreviewTexture_;
			snapshot.preview.width = fileDialogPreviewWidth_;
			snapshot.preview.height = fileDialogPreviewHeight_;
			snapshot.preview.hasTransparency = fileDialogPreviewHasTransparency_;
			snapshot.preview.transparencyPattern = runtimeSettings_.Values().transparencyPattern;
			snapshot.preview.message = fileDialogPreviewMessage_;
			if (!fileDialogPreviewSource_.empty()) {
				const std::string filename = fileDialogPreviewSource_.filename().string();
				std::string dimensions;
				if (fileDialogPreviewSourceWidth_ > 0 && fileDialogPreviewSourceHeight_ > 0) {
					const std::string formattedSize = fileDialogPreviewFileSizeKnown_ ?
						jpegview_linux::FormatFileSize(fileDialogPreviewFileSize_) : std::string();
					dimensions = jpegview_linux::FormatImageDimensionsAndSize(
						fileDialogPreviewSourceWidth_, fileDialogPreviewSourceHeight_, formattedSize);
				}
				const int footerX = snapshot.preview.pane.x + 8;
				const int footerWidth = std::max(0, snapshot.preview.pane.w - 16);
				const jpegview_linux::FileDialogPreviewFooterLayout footerLayout =
					jpegview_linux::CalculateFileDialogPreviewFooterLayout(footerWidth,
						TextWidth(filename, kUiTextScale), TextWidth(dimensions, kUiTextScale));
				snapshot.preview.filename = ClipText(filename, footerLayout.filenameWidth);
				snapshot.preview.dimensions = ClipText(dimensions, footerLayout.detailsWidth);
				snapshot.preview.filenameX = footerX;
				snapshot.preview.dimensionsX = footerX + footerLayout.detailsOffsetX +
					footerLayout.detailsWidth - TextWidth(snapshot.preview.dimensions, kUiTextScale);
				snapshot.preview.footerY = snapshot.preview.pane.y + snapshot.preview.pane.h - 19;
			}
		}
		return snapshot;
	}

	void RenderFileDialog() {
		fileDialogRenderer_.Render(BuildFileDialogRenderSnapshot());
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

	void ClearTextTextureCache() { textRenderer_.Clear(); }

	void DrawText(const std::string& text, int x, int y, int scale,
		Uint8 r = 235, Uint8 g = 235, Uint8 b = 235, Uint8 alpha = 255) {
		textRenderer_.Draw(text, x, y, scale, r, g, b, alpha);
	}

	void DrawLine(int x1, int y1, int x2, int y2, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235,
		Uint8 alpha = 255) {
		chromeRenderer_.DrawLine(x1, y1, x2, y2, r, g, b, alpha);
	}

	void DrawRect(const SDL_Rect& rect, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235,
		Uint8 alpha = 255) {
		chromeRenderer_.DrawRect(rect, r, g, b, alpha);
	}

	static SDL_Rect SdlRect(const jpegview_linux::UiRect& rect) {
		return SDL_Rect{rect.x, rect.y, rect.width, rect.height};
	}

	void RenderOverlayPaint(const jpegview_linux::OverlayPaintPlan& plan) {
		chromeRenderer_.Render(plan);
	}

	void RenderNavigationButton(const jpegview_linux::NavigationButtonPaint& button) {
		chromeRenderer_.Render(button);
	}

	void RenderFileName() {
		if (!runtimeSettings_.Values().showFilename || fileList_.Empty() || contextMenuOpen_ || fileDialogOpen_ ||
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
		if (!viewport_.FitRelativeZoomMode() || fileList_.Empty() || CurrentImage().width <= 0 ||
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

	void UpdatePixelColorSampler() {
		jpegview_linux::PixelColorSamplerInput input;
		if (fileList_.Empty() || contextMenuOpen_ || fileDialogOpen_ ||
			advancedConfiguration_.IsOpen() || batchCopyDialog_.IsOpen() ||
			resizeDialog_.IsOpen() || cropSizeDialog_.IsOpen() ||
			goToImageNumberDialog_.IsOpen() || unsharpDialogOpen_ ||
			pictureLevelsPanelOpen_ || aboutOpen_ || helpOpen_ || confirmationOpen_ ||
			archivePasswordDialog_.IsOpen() || clipboardMode_ || currentSelectedLoadPending_ ||
			imageSession_.LoadedPath() != AbsoluteNormalized(fileList_.Current()) ||
			CurrentImage().width <= 0 || CurrentImage().height <= 0) {
			pixelColorSampler_.Clear();
			return;
		}

		input.enabled = true;
		input.ownerGeneration = imageSession_.Generation();
		input.documentRevision = imageDocument_.Revision();
		input.frameIndex = currentAnimationFrame_;
		input.imageWidth = CurrentImage().width;
		input.imageHeight = CurrentImage().height;
		input.pointerX = lastMouseX_;
		input.pointerY = lastMouseY_;
		input.labelWidth = TextWidth("DOC #FFFFFFFF", kUiTextScale);
		input.lineHeight = TextLineHeight();
		SDL_GetWindowSize(window_, &input.windowWidth, &input.windowHeight);
		const SDL_Rect area = ImageAreaRect();
		const SDL_Rect destination = CurrentPageScreenRect(area);
		input.imageArea = {area.x, area.y, area.w, area.h};
		input.destination = {destination.x, destination.y,
			destination.w, destination.h};

		const std::vector<std::uint8_t>* pixels = nullptr;
		int pixelWidth = 0;
		int pixelHeight = 0;
		if (const std::shared_ptr<const Image>& source = imageDocument_.SourcePixels()) {
			input.pixelOwner = source.get();
			pixels = &source->bgra;
			pixelWidth = source->width;
			pixelHeight = source->height;
		} else if (currentDecoded_ && currentAnimationFrame_ < currentDecoded_->frames.size()) {
			const jpegview_linux::DecodedFrame& frame =
				currentDecoded_->frames[currentAnimationFrame_];
			input.pixelOwner = currentDecoded_.get();
			pixels = &frame.bgra;
			pixelWidth = frame.width;
			pixelHeight = frame.height;
		}
		if (pixels != nullptr && pixelWidth == input.imageWidth &&
			pixelHeight == input.imageHeight &&
			PointInRect(input.pointerX, input.pointerY, area) &&
			PointInRect(input.pointerX, input.pointerY, destination)) {
			const jpegview_linux::SelectionPoint point =
				jpegview_linux::CropSelectionModel::ScreenToImage(input.pointerX,
				input.pointerY, {input.destination.x, input.destination.y,
					input.destination.width, input.destination.height},
				pixelWidth, pixelHeight);
			input.bgra = pixels;
			input.pixelX = point.x;
			input.pixelY = point.y;
		}
		pixelColorSampler_.Update(input);
	}

	void RenderPixelColorSampler() {
		const jpegview_linux::PixelColorSamplerPaintPlan& plan =
			pixelColorSampler_.PaintPlan();
		if (!plan.color.has_value() || plan.panel.width <= 0 || plan.panel.height <= 0) return;
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 228);
		const SDL_Rect panel{plan.panel.x, plan.panel.y,
			plan.panel.width, plan.panel.height};
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 150, 165, 185);
		const SDL_Rect swatch{plan.swatch.x, plan.swatch.y,
			plan.swatch.width, plan.swatch.height};
		SDL_SetRenderDrawColor(renderer_, plan.color->red, plan.color->green,
			plan.color->blue, plan.color->alpha);
		SDL_RenderFillRect(renderer_, &swatch);
		DrawRect(swatch, 235, 235, 235);
		DrawText(plan.label, panel.x + 6 + 14 + 8,
			panel.y + (panel.h - TextLineHeight()) / 2,
			kUiTextScale, 245, 245, 245);
	}

	void RenderImageInfo() {
		if (!runtimeSettings_.Values().infoVisible || contextMenuOpen_ || fileDialogOpen_ || advancedConfiguration_.IsOpen() ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return;
		const jpegview_linux::InformationOverlayPaintPlan& paint = BuildImageInfoPaintPlan();
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
			const std::string label = runtimeSettings_.Values().showHistogram ? "Hide histogram" : "Show histogram";
			int windowWidth = 0;
			int windowHeight = 0;
			SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
			RenderOverlayPaint(jpegview_linux::NavigationTooltipPaint(paint.spectrumButton,
				label, TextWidth(label, kUiTextScale), TextLineHeight(), windowWidth, windowHeight));
		} else if (jpegview_linux::Contains(paint.gpsLocationLink, lastMouseX_, lastMouseY_)) {
			int windowWidth = 0;
			int windowHeight = 0;
			SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
			const std::string label = "Open GPS location in map";
			RenderOverlayPaint(jpegview_linux::NavigationTooltipPaint(paint.gpsLocationLink,
				label, TextWidth(label, kUiTextScale), TextLineHeight(), windowWidth, windowHeight));
		}
	}

	jpegview_linux::ThumbnailPanelLayout CurrentThumbnailPanelLayout() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		return jpegview_linux::CalculateThumbnailPanelLayout(windowWidth, windowHeight,
			runtimeSettings_.Values().thumbnailPanelVisible, runtimeSettings_.Values().thumbnailPanelWidth);
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

		if (runtimeSettings_.Values().transparencyPattern != jpegview_linux::TransparencyPattern::Checkerboard) {
			const jpegview_linux::TransparencyPatternColor color =
				jpegview_linux::TransparencyPatternTileColor(runtimeSettings_.Values().transparencyPattern, 0, 0);
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
					jpegview_linux::TransparencyPatternTileColor(runtimeSettings_.Values().transparencyPattern, tileX, tileY);
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
		if (!runtimeSettings_.Values().showZoomNavigator || dimensions.first <= 0 || dimensions.second <= 0 ||
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
		if (!magnifyingGlass_.Enabled() || CurrentImage().width <= 0 || CurrentImage().height <= 0 ||
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
		SetDisplayTextureProtection(key,
			jpegview_linux::CacheProtectionTier::DistantSpeculation);
		displayImageCache_.CancelBackground(key);
		displayImageCache_.Release(key);
		const auto texture = displayTextureCache_.find(key);
		if (texture == displayTextureCache_.end()) return;
		EraseDisplayTexture(texture);
	}

	void ClearMagnifyingGlassRequest() {
		DiscardMagnifyingGlassRequest(magnifyingGlassRequestKey_);
		magnifyingGlassRequestKey_.clear();
		magnifyingGlassRequestBaseKey_.clear();
		magnifyingGlassBackgroundRequestedKey_.clear();
		magnifyingGlassRequest_.reset();
	}

	void UpdateMagnifyingGlassRequest() {
		if (!magnifyingGlass_.Enabled() || activeDoublePageRender_.has_value() ||
			!IsMagnifyingGlassVisibleAt(lastMouseX_, lastMouseY_)) {
			ClearMagnifyingGlassRequest();
			return;
		}
		const auto request = MagnifyingGlassDisplayRequest();
		if (!request.has_value() || (currentDisplayRequest_.has_value() &&
			request->key == currentDisplayRequest_->key)) {
			ClearMagnifyingGlassRequest();
			return;
		}
		if (magnifyingGlassRequestKey_ != request->key) {
			DiscardMagnifyingGlassRequest(magnifyingGlassRequestKey_);
			magnifyingGlassRequestKey_ = request->key;
			magnifyingGlassBackgroundRequestedKey_.clear();
		}
		SetDisplayTextureProtection(request->key,
			jpegview_linux::CacheProtectionTier::Neighbor);
		if (PeekDisplayTexture(request->key) == nullptr &&
			magnifyingGlassBackgroundRequestedKey_ != request->key &&
			request->targetHeight > 0 &&
			cacheBudget_->Capacity() / 4 /
				static_cast<std::size_t>(request->targetHeight) >=
				static_cast<std::size_t>(request->targetWidth)) {
			displayImageCache_.RequestBackground(*request);
			magnifyingGlassBackgroundRequestedKey_ = request->key;
		}
	}

	void SetMagnifyingGlassEnabled(bool enabled) {
		magnifyingGlass_.SetEnabled(enabled);
		if (!enabled) ClearMagnifyingGlassRequest();
		UpdateMagnifyingGlassCursor(lastMouseX_, lastMouseY_);
		playback_.NotifyInteraction(SDL_GetTicks());
	}

	std::optional<jpegview_linux::DisplayImageRequest>
	CurrentMagnifyingGlassProtectionRequest() const {
		if (!magnifyingGlass_.Enabled() || activeDoublePageRender_.has_value() ||
			imageModified_ || currentPixelsDetachedFromSource_ ||
			!currentDisplayRequest_.has_value() || !magnifyingGlassRequest_.has_value() ||
		(!fileList_.Empty() &&
			presentationController_.SuppressSinglePage(fileList_.CurrentIndex())) ||
			magnifyingGlassRequestBaseKey_ != currentDisplayRequest_->key ||
			magnifyingGlassRequest_->key == currentDisplayRequest_->key ||
			magnifyingGlassRequest_->source.Key() != currentDisplayRequest_->source.Key() ||
			!IsMagnifyingGlassVisibleAt(lastMouseX_, lastMouseY_)) return std::nullopt;
		return magnifyingGlassRequest_;
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
		if (!magnifyingGlass_.Enabled() ||
			!IsMagnifyingGlassVisibleAt(lastMouseX_, lastMouseY_)) return;
		const SDL_Rect area = ImageAreaRect();
		SDL_Rect displayed = CurrentPageScreenRect(area);
		SDL_Texture* lensTexture = fallbackTexture;
		int textureWidth = CurrentImage().width;
		int textureHeight = CurrentImage().height;
		bool hasTransparency = CurrentImage().hasTransparency;
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
			lensTexture = PeekDisplayTexture(doublePagePartnerDisplayKey_);
			displayed = NextPageScreenRect(area);
			const auto partner = displayTextureCache_.find(doublePagePartnerDisplayKey_);
			if (partner != displayTextureCache_.end()) {
				textureWidth = partner->second.width;
				textureHeight = partner->second.height;
				hasTransparency = partner->second.hasTransparency;
			}
		}

		if (!activeDoublePageRender_.has_value() && magnifyingGlassRequest_.has_value() &&
			magnifyingGlassRequest_->key != (currentDisplayRequest_.has_value() ?
				currentDisplayRequest_->key : std::string())) {
			const jpegview_linux::DisplayImageRequest& request = *magnifyingGlassRequest_;
			if (SDL_Texture* cached = PeekDisplayTexture(request.key)) {
				lensTexture = cached;
				const auto dimensions = displayTextureCache_.find(request.key);
				if (dimensions != displayTextureCache_.end()) {
					textureWidth = dimensions->second.width;
					textureHeight = dimensions->second.height;
				}
			}
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
		if (CurrentImage().width <= 0 || CurrentImage().height <= 0 || pictureLevelsPanelOpen_ ||
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
			screenX, screenY, destination, CurrentImage().width, CurrentImage().height);
		if (cropSelection_.HasSelection()) {
			const jpegview_linux::SelectionScreenRect selected =
				jpegview_linux::CropSelectionModel::ToScreen(cropSelection_.Rect(), destination,
					CurrentImage().width, CurrentImage().height);
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
		if (!jpegview_linux::ShouldStartNewCropSelection(runtimeSettings_.Values().selectionModeEnabled,
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
					CurrentImage().width, CurrentImage().height);
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
			runtimeSettings_.Values().selectionModeEnabled, forcedByModifier, imageNeedsPanning);
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
			screenX, screenY, ImageDestinationScreenRect(), CurrentImage().width, CurrentImage().height);
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
			switch (jpegview_linux::ResolveNewCropSelectionReleaseAction(
				cropDragMoved_, cropZoomOnRelease_,
				runtimeSettings_.Values().copySelectionOnRelease)) {
			case jpegview_linux::NewCropSelectionReleaseAction::Clear:
				cropSelection_.Clear();
				break;
			case jpegview_linux::NewCropSelectionReleaseAction::ZoomToSelection:
				ZoomToSelection();
				cropSelection_.Clear();
				break;
			case jpegview_linux::NewCropSelectionReleaseAction::CopySelection:
				CopyCurrentSelection();
				ClearCropSelection();
				break;
			case jpegview_linux::NewCropSelectionReleaseAction::OpenContextMenu:
				OpenCropContextMenu();
				break;
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
		if (!runtimeSettings_.Values().thumbnailPanelVisible) return false;
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
		if (width == runtimeSettings_.Values().thumbnailPanelWidth) return;
		runtimeSettings_.Values().thumbnailPanelWidth = width;
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

	void TouchVisibleThumbnailRows() {
		if (!runtimeSettings_.Values().thumbnailPanelVisible || fileList_.Empty()) return;
		const SDL_Rect panel = ThumbnailPanelRect();
		if (panel.w <= 0 || panel.h <= 0) return;
		const int rowHeight = jpegview_linux::ThumbnailRowHeight(panel.w,
			kThumbnailVerticalMargin);
		const std::optional<std::size_t> doublePagePartnerIndex =
			activeDoublePageRender_.has_value() ?
				std::optional<std::size_t>(activeDoublePageRender_->layout.secondIndex) :
				std::nullopt;
		const std::vector<jpegview_linux::ThumbnailSlot> slots =
			jpegview_linux::ThumbnailPanelSlots(fileList_.Size(), fileList_.CurrentIndex(),
				panel.h, rowHeight, fileList_.MarkedIndex(), doublePagePartnerIndex);
		for (const jpegview_linux::ThumbnailSlot& slot : slots) {
			const jpegview_linux::SourceDescriptor* source = fileList_.DescriptorAt(slot.fileIndex);
			if (source == nullptr) continue;
			const jpegview_linux::SourceKey key = source->Key();
			const auto cached = thumbnailTextureCache_.find(key);
			if (cached != thumbnailTextureCache_.end() && cached->second.texture != nullptr) {
				thumbnailScheduler_.Touch(key);
			}
		}
	}

	void RenderThumbnailPanel() {
		if (!runtimeSettings_.Values().thumbnailPanelVisible || fileList_.Empty()) return;
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
			auto cached = thumbnailTextureCache_.find(key);
			if (cached != thumbnailTextureCache_.end() && cached->second.texture != nullptr) {
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
		if (!runtimeSettings_.Values().thumbnailPanelVisible || fileList_.Empty()) return false;
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
		if (!runtimeSettings_.Values().infoVisible || fileList_.Empty() || contextMenuOpen_ || fileDialogOpen_ ||
			advancedConfiguration_.IsOpen() ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return false;
		const jpegview_linux::InformationOverlayPaintPlan& paint = BuildImageInfoPaintPlan();
		if (jpegview_linux::Contains(paint.spectrumButton, x, y)) {
			runtimeSettings_.Values().showHistogram = !runtimeSettings_.Values().showHistogram;
			frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Overlay);
			SaveSettings();
			return true;
		}
		if (jpegview_linux::Contains(paint.gpsLocationLink, x, y)) {
			OpenGpsMap();
			return true;
		}
		return false;
	}

	void RenderControls() {
		if (!runtimeSettings_.Values().navigationPanelEnabled || !controlsVisible_ || contextMenuOpen_ || fileDialogOpen_ ||
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
			viewport_.IsFitToWindow(), fullscreen_, fileList_.GetSorting(), runtimeSettings_.Values().selectionModeEnabled,
			doublePageModeEnabled_, mangaReadingOrderEnabled_, runtimeSettings_.Values().spacebarNavigatesImages,
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
		const auto thumbnail = thumbnailTextureCache_.find(source.Key());
		if (thumbnail != thumbnailTextureCache_.end() && thumbnail->second.texture != nullptr) {
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
		if (imageSession_.LoadedPath().lexically_normal() != normalizedCurrent) return {};
		if (displayTexture_ != nullptr) {
			return {displayTexture_, displayTextureWidth_, displayTextureHeight_,
				displayImage_.hasTransparency};
		}
		if (texture_ != nullptr && CurrentImage().width > 0 && CurrentImage().height > 0) {
			return {texture_, CurrentImage().width, CurrentImage().height, CurrentImage().hasTransparency};
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
		const std::string spaceHelp = runtimeSettings_.Values().spacebarNavigatesImages ?
			"Navigate: Space next; Shift+Space previous; Return fit; Ctrl+Return fill with crop; +/- zoom" :
			"Scale: Space fit/actual; Return fit; Ctrl+Return fill with crop; +/- zoom";
		const std::array<std::string, 10> lines = {
			"Navigate: Ctrl+G number; arrows/wheel; Home/End; Alt+arrows sibling folders",
			"Zoom/pan: Ctrl+wheel or Ctrl+Up/Down; drag; Shift+Arrow; Z lens; wheel resizes it",
			"Navigator: hover upper-right when magnified; click or drag its map to reposition",
			spaceHelp,
			"Panels: F2 info; Shift+N filename; Ctrl+N nav; Ctrl+T thumbs; Ctrl+E crop mode",
			"Files: Ctrl+O open; Ctrl+S save processed; Ctrl+Shift+S save displayed size",
			"Clipboard: Ctrl+C copy image; Ctrl+Shift+C copy path; Ctrl+V paste PNG",
			"Adjustments: Up/Down rotate; F5 auto correction; F6 local density; Ctrl+Shift+R resize",
			"View/window: D double page; J manga; Ctrl+M mark; F11 full; Shift+F11 title; Shift+F12 top",
			"Dialogs: Ctrl+Tab switches Browse/Recents; type to filter; arrows/pages select; wheel scrolls; drag to resize"};
		for (std::size_t index = 0; index < lines.size(); ++index) {
			DrawText(ClipText(lines[index], width - 36), panel.x + 18,
				panel.y + 48 + static_cast<int>(index) * 27, kUiTextScale, 220, 225, 235);
		}
		DrawText("Right-click: compact; Shift+right: full; Menu: compact; Esc/F1 closes",
			panel.x + 18, panel.y + height - 32, kUiTextScale, 180, 190, 205);
	}

	void RenderImageTransition(const SDL_Rect& destination, const SDL_Rect& imageArea, SDL_Texture* currentTexture) {
		if (transitionTexture_ == nullptr) {
			SDL_RenderCopy(renderer_, currentTexture, nullptr, &destination);
			return;
		}
		const Uint32 elapsed = SDL_GetTicks() - transitionStartTick_;
		const double progress = std::min(1.0, static_cast<double>(elapsed) /
			static_cast<double>(transitionDurationMs_));
		if (progress >= 1.0) {
			SDL_RenderCopy(renderer_, currentTexture, nullptr, &destination);
			return;
		}

		const jpegview_linux::ViewportRect oldRect = viewport_.Destination(
			transitionWidth_, transitionHeight_, imageArea.w, imageArea.h);
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
		if (!cropSelection_.HasSelection() || CurrentImage().width <= 0 || CurrentImage().height <= 0) return;
		const jpegview_linux::SelectionScreenRect destination = ImageDestinationScreenRect();
		const jpegview_linux::SelectionScreenRect selected =
			jpegview_linux::CropSelectionModel::ToScreen(cropSelection_.Rect(), destination,
				CurrentImage().width, CurrentImage().height);
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
		contextMenuRenderer_.Render(contextMenuItems_, columns, menu, menuSelected_,
			ContextMenuRowHeight(), kContextMenuSeparatorHeight, kContextMenuVerticalPadding);
	}

	void HandleEvents(bool& running, int waitTimeoutMs) {
		SDL_Event event{};
		SDL_Event deferredEvent{};
		bool deferredReady = false;
		bool firstRead = true;
		const auto readNextEvent = [&] {
			if (deferredReady) {
				event = deferredEvent;
				deferredReady = false;
				return true;
			}
			if (firstRead) {
				firstRead = false;
				if (waitTimeoutMs > 0) return SDL_WaitEventTimeout(&event, waitTimeoutMs) == 1;
			}
			return SDL_PollEvent(&event) != 0;
		};
		while (readNextEvent()) {
			if (event.type == SDL_MOUSEMOTION) {
				jpegview_linux::MouseMotionSample motion{event.motion.x, event.motion.y,
					event.motion.xrel, event.motion.yrel, event.motion.state};
				SDL_Event next{};
				while (SDL_PollEvent(&next) != 0) {
					if (next.type != SDL_MOUSEMOTION ||
						next.motion.windowID != event.motion.windowID ||
						next.motion.which != event.motion.which ||
						!jpegview_linux::CoalesceMouseMotion(motion,
							{next.motion.x, next.motion.y, next.motion.xrel,
								next.motion.yrel, next.motion.state})) {
						deferredEvent = next;
						deferredReady = true;
						break;
					}
				}
				event.motion.x = motion.x;
				event.motion.y = motion.y;
				event.motion.xrel = motion.xrel;
				event.motion.yrel = motion.yrel;
			}
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
			const bool completionWake = completionWakeEventType_ !=
				std::numeric_limits<Uint32>::max() && event.type == completionWakeEventType_;
			if (completionWake) {
				jpegview_linux::UiCompletionWakeup().Consume();
			} else {
				if (event.type == SDL_WINDOWEVENT &&
					event.window.event == SDL_WINDOWEVENT_EXPOSED) {
					frameInvalidator_.Mark(
						jpegview_linux::FrameInvalidationReason::WindowExposure);
				} else if (event.type == SDL_WINDOWEVENT &&
					(event.window.event == SDL_WINDOWEVENT_RESIZED ||
						event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
					frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Viewport);
				} else if (fileDialogOpen_) {
					frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Dialog);
				} else {
					frameInvalidator_.Mark(jpegview_linux::FrameInvalidationReason::Input);
				}
				playback_.NotifyInteraction(SDL_GetTicks());
			}
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
			jpegview_linux::ModalEventState modalState;
			modalState.archivePassword = archivePasswordDialog_.IsOpen() ||
				archivePasswordValidationPending_;
			modalState.confirmation = confirmationOpen_;
			modalState.help = helpOpen_;
			modalState.about = aboutOpen_;
			modalState.advancedConfiguration = advancedConfiguration_.IsOpen();
			modalState.fileDialog = fileDialogOpen_;
			modalState.batchCopy = batchCopyDialog_.IsOpen();
			modalState.resize = resizeDialog_.IsOpen();
			modalState.fixedCropSize = cropSizeDialog_.IsOpen();
			modalState.goToImageNumber = goToImageNumberDialog_.IsOpen();
			modalState.unsharpMask = unsharpDialogOpen_;
			modalState.pictureLevels = pictureLevelsPanelOpen_;
			modalState.contextMenu = contextMenuOpen_;
			const jpegview_linux::ModalEventRoute modalRoute =
				jpegview_linux::ResolveModalEventRoute(modalState);
			if (modalRoute == jpegview_linux::ModalEventRoute::ArchivePassword) {
				HandleArchivePasswordEvents(event, running);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::Confirmation) {
				if (event.type == SDL_QUIT) running = false;
				else HandleConfirmationEvents(event);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::Help) {
				if (event.type == SDL_QUIT) running = false;
				else HandleHelpEvents(event);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::About) {
				if (event.type == SDL_QUIT) running = false;
				else HandleAboutEvents(event);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::AdvancedConfiguration) {
				HandleAdvancedConfigurationEvents(event, running);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::FileDialog) {
				HandleFileDialogEvents(event, running);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::BatchCopy) {
				HandleBatchCopyEvents(event, running);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::Resize) {
				HandleResizeDialogEvents(event, running);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::FixedCropSize) {
				HandleFixedCropSizeDialogEvents(event, running);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::GoToImageNumber) {
				HandleGoToImageNumberDialogEvents(event, running);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::UnsharpMask) {
				HandleUnsharpMaskDialogEvents(event, running);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::PictureLevels) {
				HandlePictureLevelsEvents(event, running);
				continue;
			}
			if (modalRoute == jpegview_linux::ModalEventRoute::ContextMenu) {
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
					runtimeSettings_.Values().maximized = true;
				} else if (event.window.event == SDL_WINDOWEVENT_RESTORED) {
					runtimeSettings_.Values().maximized = false;
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
					} else if (CurrentImage().width > 0) {
						RefreshFitRelativeZoomBase();
					}
				}
				break;
			case SDL_KEYDOWN:
			{
				const Uint16 modifiers = event.key.keysym.mod;
				const int spaceNavigationDirection =
					jpegview_linux::SpacebarNavigationDirection(event.key, runtimeSettings_.Values().spacebarNavigatesImages);
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
						physicalDirection, mangaReadingOrderEnabled_, runtimeSettings_.Values().mangaModeInvertsLeftRight);
					heldNavigation_.KeyDown(direction, event.key.keysym.scancode, true);
					break;
				}
				if (plainNavigationKey) {
					const int physicalDirection = event.key.keysym.sym == SDLK_RIGHT ? 1 : -1;
					const int direction = jpegview_linux::LogicalDirectionForPhysicalKey(
						physicalDirection, mangaReadingOrderEnabled_, runtimeSettings_.Values().mangaModeInvertsLeftRight);
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
				if (plainNavigationKey && mangaReadingOrderEnabled_ && runtimeSettings_.Values().mangaModeInvertsLeftRight) {
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
					if (const std::optional<std::string> colorText =
						pixelColorSampler_.CopyTextAt(event.button.x, event.button.y)) {
						if (SDL_SetClipboardText(colorText->c_str()) != 0) {
							SetTitle("Could not copy pixel color");
						}
						dragging_ = false;
						break;
					}
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
				pixelColorSampler_.PointerMoved(event.motion.x, event.motion.y);
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

	PresentedFrame RenderFrame() {
		auto& diagnostics = jpegview_linux::PerfDiagnostics::Instance();
		const std::uint64_t frameStart = diagnostics.Begin();
		const SDL_Rect imageArea = ImageAreaRect();
		const std::size_t currentIndex = fileList_.Empty() ? 0 : fileList_.CurrentIndex();
		const bool selectedImageLoaded = !fileList_.Empty() && (clipboardMode_ ||
			imageSession_.LoadedPath() == AbsoluteNormalized(fileList_.Current()));
		const bool spreadModelReady = !fileList_.Empty() &&
			presentationController_.SpreadReady(currentIndex);
		const SDL_Rect destination = CurrentPageScreenRect(imageArea);
		SDL_Texture* renderTexture = nullptr;
		SDL_Texture* nextPageTexture = nullptr;
		if (activeDoublePageRender_.has_value() && spreadModelReady) {
			renderTexture = activeDoublePageRender_->transformedAnchorTexture ? texture_ :
				PeekDisplayTexture(presentationController_.AnchorTextureKey());
			nextPageTexture = PeekDisplayTexture(presentationController_.PartnerTextureKey());
		} else if (!activeDoublePageRender_.has_value() && selectedImageLoaded &&
			!presentationController_.SuppressSinglePage(currentIndex)) {
			renderTexture = DisplayTextureForRender();
		}
		const bool spreadTexturesReady = activeDoublePageRender_.has_value() &&
			spreadModelReady && renderTexture != nullptr && nextPageTexture != nullptr;
		const bool suppressSingleImage = !selectedImageLoaded ||
			presentationController_.SuppressSinglePage(currentIndex) ||
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
			if (CurrentImage().hasTransparency) RenderTransparencyBackground(destination, imageArea);
			const auto partner = displayTextureCache_.find(doublePagePartnerDisplayKey_);
			if (partner != displayTextureCache_.end() && partner->second.hasTransparency) {
				RenderTransparencyBackground(nextPageDestination, imageArea);
			}
			if (renderTexture != nullptr) SDL_RenderCopy(renderer_, renderTexture, nullptr, &destination);
			SDL_RenderCopy(renderer_, nextPageTexture, nullptr, &nextPageDestination);
		} else if (!suppressSingleImage) {
			if (CurrentImage().hasTransparency ||
				(transitionTexture_ != nullptr && transitionHasTransparency_)) {
				RenderTransparencyBackground(destination, imageArea);
			}
			RenderImageTransition(destination, imageArea, renderTexture);
		}
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		if (!suppressSingleImage) RenderCropSelection();
		RenderZoomNavigator(renderTexture, nextPageTexture);
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
		RenderGoToImageNumberDialog();
		RenderAdvancedConfigurationDialog();
		RenderConfirmation();
		RenderAbout();
		RenderHelp();
		RenderArchivePasswordDialog();
		RenderPixelColorSampler();
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		diagnostics.End(jpegview_linux::PerfMetric::FrameBuild, frameStart);
		{
			jpegview_linux::PerfScopedTimer presentTimer(diagnostics,
				jpegview_linux::PerfMetric::Present);
			SDL_RenderPresent(renderer_);
		}
		diagnostics.RecordPresentation();
		return {currentIndex, spreadTexturesReady};
	}

	void RecordPeriodicPerformanceSnapshots() {
		auto& diagnostics = jpegview_linux::PerfDiagnostics::Instance();
		if (!diagnostics.Enabled()) return;
		const std::uint64_t sampleStart = diagnostics.Begin();
		if (sampleStart - lastPerfSnapshotUs_ < 1000000u) return;
		lastPerfSnapshotUs_ = sampleStart;
		std::uint64_t thumbnailTextureBytes = 0;
		for (const auto& cached : thumbnailTextureCache_) {
			if (cached.second.texture != nullptr) {
				thumbnailTextureBytes += static_cast<std::uint64_t>(cached.second.width) *
					static_cast<std::uint64_t>(cached.second.height) * 4;
			}
		}
		const jpegview_linux::ThumbnailRepositoryDiagnostics thumbnailPixels =
			thumbnailRepository_->Diagnostics();
		const jpegview_linux::DecodedImageCacheDiagnostics decodedStats =
			imageCache_.GetDiagnostics();
		const jpegview_linux::DisplayImageCacheDiagnostics displayStats =
			displayImageCache_.GetDiagnostics();
		const jpegview_linux::ThumbnailPreparationDiagnostics thumbnailStats =
			thumbnailPreparation_.GetDiagnostics();
		const jpegview_linux::CacheBudgetSnapshot budgetStats = cacheBudget_->Snapshot();
		diagnostics.End(jpegview_linux::PerfMetric::CacheSnapshot, sampleStart,
			budgetStats.capacityBytes, budgetStats.retainedBytes,
			budgetStats.decodedPixelBytes, budgetStats.preparedFrameBytes,
			budgetStats.imageTextureBytes, budgetStats.uploadStagingBytes);
		diagnostics.RecordText(jpegview_linux::PerfMetric::CacheSnapshot,
			budgetStats.capacityBytes, budgetStats.retainedBytes,
			budgetStats.decodedPixelBytes, budgetStats.preparedFrameBytes,
			budgetStats.imageTextureBytes, budgetStats.uploadStagingBytes, "budget");
		diagnostics.RecordText(jpegview_linux::PerfMetric::CacheSnapshot,
			budgetStats.activeWorkingBytes, budgetStats.releaseRevision,
			0, 0, 0, 0, "budget_working");
		diagnostics.RecordText(jpegview_linux::PerfMetric::CacheSnapshot,
			speculativeDisplayTextureBytes_,
			jpegview_linux::SpeculativeDisplayTextureBudgetBytes(cacheBudget_->Capacity()),
			retiredSpeculativeDisplayTextureBytes_, retiredDisplayTextures_.size(),
			displayTextureCacheBytes_, displayTextureCache_.size(),
			"display_texture_residency");
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
			thumbnailPixels.pixelBytes, thumbnailPixels.imageCount,
			thumbnailStats.completedBytes, thumbnailStats.completedImages,
			thumbnailStats.retiredSourceBytes, thumbnailStats.retiredSources,
			"thumbnail_pixels");
		diagnostics.RecordText(jpegview_linux::PerfMetric::CacheSnapshot,
			thumbnailTextureBytes, thumbnailTextureCache_.size(),
			thumbnailTextureWindowKeys_.size(), 0, 0, 0, "thumbnail_textures");
		diagnostics.RecordText(jpegview_linux::PerfMetric::QueueSnapshot,
			thumbnailStats.queued, thumbnailStats.active,
			thumbnailStats.retainedSourceBytes, thumbnailStats.retiredSourceBytes,
			thumbnailStats.completedImages, thumbnailStats.retiredSources, "thumbnail");
	}

	PresentedFrame Render() {
		PresentedFrame presented = RenderFrame();
		if (renderContextMenuNeedsCleanFrame_) presented = RenderFrame();
		return presented;
	}

	void PresentStartupFrame() {
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		SDL_SetRenderDrawColor(renderer_, 18, 18, 18, 255);
		SDL_RenderClear(renderer_);
		SDL_RenderPresent(renderer_);
	}

	jpegview_linux::RuntimeSettingsOwner runtimeSettings_;
	jpegview_linux::FileList fileList_;
	jpegview_linux::FileListScanWorker fileListScanWorker_;
	jpegview_linux::FileListSortWorker fileListSortWorker_;
	jpegview_linux::DisplayPreparationController displayPreparationController_;
	jpegview_linux::ExifMetadataWorker exifMetadataWorker_;
	jpegview_linux::InteractionWorkPolicy interactionWorkPolicy_;
	bool speculativeWorkSuspended_ = false;
	bool lastForegroundPending_ = false;
	bool prefetchRefreshNeeded_ = false;
	int pendingPrefetchDirection_ = 0;
	std::uint64_t fileListScanGeneration_ = 0;
	std::uint64_t fileListSortGeneration_ = 0;
	std::uint64_t playbackBoundaryScanGeneration_ = 0;
	fs::path pendingFileListScanSourcePath_;
	std::optional<jpegview_linux::FileList::ScanOperation> pendingFileListScanOperation_;
	std::optional<jpegview_linux::FileList::ScanRequest> pendingDroppedScanRequest_;
	FileListScanHandling pendingFileListScanHandling_ = FileListScanHandling::None;
	int pendingFileListScanDirection_ = 0;
	bool pendingFileListScanForceImageReload_ = false;
	fs::path pendingFileListScanPreferredPath_;
	fs::path pendingMarkedToggleReturnPath_;
	ImageLoadStatePolicy pendingFileListScanLoadStatePolicy_ = ImageLoadStatePolicy::PreserveCurrent;
	std::string pendingFileListScanCompletionTitle_;
	bool startupImageLoadFailed_ = false;
	bool quitAfterEmptyScan_ = false;
	int deferredExitCode_ = 0;
	std::vector<std::string> startupInputs_;
	jpegview_linux::RecentFiles recentFiles_;
	jpegview_linux::ImageSessionController imageSession_;
	fs::path recentFilesPath_;
	bool recentFilesLoaded_ = false;
	std::shared_ptr<jpegview_linux::SharedCacheBudget> cacheBudget_ =
		std::make_shared<jpegview_linux::SharedCacheBudget>(
			jpegview_linux::CacheBytesFromMiB(jpegview_linux::kDefaultCacheSizeMiB));
	jpegview_linux::CacheAdmissionPolicy cacheAdmission_{*cacheBudget_};
	// Declaration order is intentional: the decoder (destroyed first) may
	// schedule final display work while joining its worker during teardown.
	jpegview_linux::DisplayImageCache displayImageCache_{
		std::numeric_limits<std::size_t>::max(), 0, {}, cacheBudget_};
	jpegview_linux::DecodedImageCache imageCache_{
		std::numeric_limits<std::size_t>::max(), {}, cacheBudget_, 2};
	jpegview_linux::ImageDocument imageDocument_;
	jpegview_linux::ImageOperationWorker imageOperationWorker_{cacheBudget_};
	jpegview_linux::FileOperationService fileOperationService_;
	std::optional<PendingFileOperationUi> pendingFileOperation_;
	std::deque<TemporaryCleanupRequest> pendingTemporaryCleanups_;
	std::shared_ptr<DisplayPrefetchBatch> displayPrefetchBatch_;
	std::shared_ptr<CurrentJpegDimensionsMailbox> currentJpegDimensionsMailbox_ =
		std::make_shared<CurrentJpegDimensionsMailbox>();
	std::shared_ptr<jpegview_linux::SelectedSourceDecodeChannel>
		selectedSourceDecodeChannel_ =
			std::make_shared<jpegview_linux::SelectedSourceDecodeChannel>();
	std::optional<PendingCurrentJpegDimensions> pendingCurrentJpegDimensions_;
	std::optional<jpegview_linux::SourceDescriptor> pendingCurrentDecodedSource_;
	jpegview_linux::PendingImageIntents pendingImageIntents_;
	TransitionFrame pendingTransitionFrame_;
	std::unordered_set<jpegview_linux::SourceKey,
		jpegview_linux::SourceKeyHash> failedJpegDimensionKeys_;
	bool currentJpegHeaderPending_ = false;
	bool currentSelectedLoadPending_ = false;
	bool currentSelectedStartupLoad_ = false;
	std::string pendingSelectedDisplayKey_;
	std::vector<jpegview_linux::PendingImageIntent> pendingMaterializationIntents_;
	std::optional<PendingImageOperation> pendingImageOperation_;
	bool pendingImageIntentLimitReached_ = false;
	std::uint64_t exifMetadataRequestGeneration_ = 0;
	jpegview_linux::SourceKey exifMetadataSource_;
	jpegview_linux::DeferredExifDateAction deferredExifDateAction_;
	std::uint64_t imageInfoMetadataRevision_ = 0;
	std::optional<ActiveSpreadSourceRequest> activeSpreadSourceRequest_;
	jpegview_linux::PresentationController presentationController_;
	bool deferredCurrentDisplayPreparation_ = false;
	double initialSlideshowSeconds_ = 0.0;
	jpegview_linux::PlaybackScheduler playback_;
	jpegview_linux::FrameInvalidator frameInvalidator_;
	jpegview_linux::WindowTitleFormatCache windowTitleFormatCache_;
	jpegview_linux::AppliedWindowTitle appliedWindowTitle_;
	jpegview_linux::ImageInfoLineCache imageInfoLineCache_;
	jpegview_linux::InformationOverlayPaintPlanCache imageInfoPaintPlanCache_;
	int transitionEffect_ = IDM_EFFECT_NONE;
	Uint32 transitionDurationMs_ = 500;
	Uint32 transitionStartTick_ = 0;
	Uint32 transitionFrameTick_ = 0;
	bool startFullscreen_ = false;
	jpegview_linux::ImageSpectrumWorker imageSpectrumWorker_;
	std::optional<jpegview_linux::PageDimensions> currentSourcePageDimensions_;
	int currentImageRotationQuarterTurns_ = 0;
	bool currentSpreadRotationValid_ = false;
	jpegview_linux::ImageProcessingParams imageProcessing_;
	jpegview_linux::ImageProcessingStore imageProcessingStore_;
	jpegview_linux::ImageProcessingStore pendingParameterDbRestore_;
	std::string pendingParameterDbRestoreSource_;
	jpegview_linux::TextRenderer textRenderer_;
	jpegview_linux::ChromeRendererAdapter chromeRenderer_{textRenderer_};
	jpegview_linux::ContextMenuRendererAdapter contextMenuRenderer_{chromeRenderer_, textRenderer_};
	jpegview_linux::FileDialogRendererAdapter fileDialogRenderer_{textRenderer_, chromeRenderer_};
	jpegview_linux::EditingDialogRendererAdapter editingDialogRenderer_{textRenderer_, chromeRenderer_};
	jpegview_linux::RendererTextureOwner imageTextureOwner_;
	Uint32 completionWakeEventType_ = std::numeric_limits<Uint32>::max();
	SDL_Texture* texture_ = nullptr;
	Image displayImage_;
	SDL_Texture* displayTexture_ = nullptr;
	int displayTextureWidth_ = 0;
	int displayTextureHeight_ = 0;
	std::unordered_map<std::string, DisplayTextureCacheEntry> displayTextureCache_;
	std::deque<PendingTextureUpload> pendingTextureUploads_;
	std::deque<RetiredDisplayTexture> retiredDisplayTextures_;
	std::unordered_set<std::string> displayTextureProtectedKeys_;
	std::unordered_set<std::string> displayTextureActiveKeys_;
	std::array<std::list<std::string>, 3> displayTextureLru_;
	std::unordered_map<jpegview_linux::SourceKey, JpegDimensionCacheEntry,
		jpegview_linux::SourceKeyHash> jpegDimensionCache_;
	std::size_t displayTextureCacheBytes_ = 0;
	std::size_t speculativeDisplayTextureBytes_ = 0;
	std::size_t retiredSpeculativeDisplayTextureBytes_ = 0;
	std::string lastPresentedDisplayKey_;
	std::string failedCurrentDisplayKey_;
	std::string transitionDisplayKey_;
	std::string transitionCaptureDisplayKey_;
		std::uint64_t lastPerfSnapshotUs_ = 0;
	jpegview_linux::DecodedImageCache::ImagePtr currentDecoded_;
	std::size_t currentAnimationFrame_ = 0;
	std::optional<jpegview_linux::DisplayImageRequest> currentDisplayRequest_;
	SDL_Texture* transitionTexture_ = nullptr;
	int transitionOriginalBlendMode_ = SDL_BLENDMODE_NONE;
	int transitionWidth_ = 0;
	int transitionHeight_ = 0;
	bool transitionTextureOwned_ = false;
	bool transitionHasTransparency_ = false;
	jpegview_linux::Viewport viewport_;
	jpegview_linux::CropSelectionModel cropSelection_;
	jpegview_linux::MagnifyingGlassModel magnifyingGlass_;
	std::string magnifyingGlassRequestKey_;
	std::string magnifyingGlassRequestBaseKey_;
	std::string magnifyingGlassBackgroundRequestedKey_;
	std::optional<jpegview_linux::DisplayImageRequest> magnifyingGlassRequest_;
	bool doublePageModeEnabled_ = false;
	bool mangaReadingOrderEnabled_ = false;
	std::optional<ActiveDoublePageRender> activeDoublePageRender_;
	std::string doublePagePartnerDisplayKey_;
	std::optional<jpegview_linux::DisplayImageRequest> doublePagePartnerRequest_;
	bool cropMouseDragging_ = false;
	bool zoomNavigatorDragging_ = false;
	Uint32 zoomNavigatorVisibleUntil_ = 0;
	Uint32 zoomReadoutVisibleUntil_ = 0;
	bool zoomNavigatorTimedVisible_ = false;
	bool zoomReadoutTimedVisible_ = false;
	bool cropDragWasNew_ = false;
	bool cropZoomOnRelease_ = false;
	bool cropDragMoved_ = false;
	int cropDragStartX_ = 0;
	int cropDragStartY_ = 0;
	int cropAspectWidth_ = 1;
	int cropAspectHeight_ = 1;
	bool fullscreen_ = false;
	bool borderless_ = false;
	bool alwaysOnTop_ = false;
	bool dragging_ = false;
	bool controlsVisible_ = true;
	bool pictureLevelsPanelOpen_ = false;
	int levelsDraggingControl_ = -1;
	bool unsharpDialogOpen_ = false;
	int unsharpDraggingControl_ = -1;
	jpegview_linux::ImageProcessingParams unsharpOriginalProcessing_;
	double unsharpOriginalRadius_ = 1.0;
	double unsharpOriginalAmount_ = 0.0;
	double unsharpOriginalThreshold_ = 4.0;
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
	bool editedImageSpectrumValid_ = false;
	jpegview_linux::GrayscaleSpectrum editedImageSpectrum_{};
	std::optional<jpegview_linux::ImageSpectrumKey> editedImageSpectrumKey_;
	std::optional<jpegview_linux::ImageSpectrumKey> editedImageSpectrumUnavailableKey_;
	std::optional<jpegview_linux::ImageSpectrumKey> editedImageSpectrumRequestKey_;
	std::uint64_t editedImageSpectrumRequestGeneration_ = 0;
	jpegview_linux::GrayscaleSpectrum currentSourceSpectrum_{};
	bool currentSourceSpectrumValid_ = false;
	std::optional<jpegview_linux::ImageSpectrumKey> currentSourceSpectrumKey_;
	std::optional<jpegview_linux::ImageSpectrumKey> currentSourceSpectrumUnavailableKey_;
	std::optional<jpegview_linux::ImageSpectrumKey> currentSourceSpectrumRequestKey_;
	std::string currentSourceSpectrumDisplayCacheKey_;
	std::uint64_t imageSpectrumPresentationRevision_ = 0;
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
	bool renderContextMenuNeedsCleanFrame_ = false;
	bool contextMenuRightKeyDown_ = false;
	char pendingMenuMnemonicTextInput_ = '\0';
	int contextMenuX_ = 0;
	int contextMenuY_ = 0;
	int menuSelected_ = -1;
	std::vector<MenuItem> contextMenuItems_;
	std::unordered_map<jpegview_linux::SourceKey, ThumbnailTextureEntry,
		jpegview_linux::SourceKeyHash> thumbnailTextureCache_;
	std::unordered_set<jpegview_linux::SourceKey,
		jpegview_linux::SourceKeyHash> thumbnailTextureWindowKeys_;
	std::vector<std::size_t> thumbnailTextureWindowIndices_;
	std::unique_ptr<jpegview_linux::ThumbnailRepository> thumbnailRepository_ =
		jpegview_linux::MakeInMemoryThumbnailRepository();
	jpegview_linux::ThumbnailCacheScheduler thumbnailScheduler_;
	jpegview_linux::ThumbnailPreparationWorker thumbnailPreparation_;
	std::optional<jpegview_linux::ThumbnailPreparationResult> thumbnailPixelStoreRetry_;
	jpegview_linux::ThumbnailCatalogRevisionTracker thumbnailCatalogRevisionTracker_;
	int thumbnailTargetWidth_ = 0;
	int thumbnailTargetHeight_ = 0;
	std::optional<jpegview_linux::SourceKey> thumbnailUploadRetryKey_;
	Uint32 thumbnailPixelStoreRetryTick_ = 0;
	Uint32 thumbnailUploadRetryTick_ = 0;
	std::vector<jpegview_linux::OpenWithApplication> openWithApplications_;
	bool imageModified_ = false;
	bool currentPixelsDetachedFromSource_ = false;
	bool autoContrastEnabled_ = false;
	bool quitRequested_ = false;
	bool fileDialogOpen_ = false;
	bool fileDialogSave_ = false;
	bool fileDialogParameterBackup_ = false;
	bool fileDialogParameterRestore_ = false;
	int fileDialogX_ = 0;
	int fileDialogY_ = 0;
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
	std::string fileDialogLocationDisplayName_;
	std::string fileDialogFilename_;
	std::string fileDialogMessage_;
	jpegview_linux::FileDialogModel fileDialogModel_;
	jpegview_linux::FileDialogModel recentFileDialogModel_;
	std::vector<jpegview_linux::RecentFileRemoval> recentFileRemovalUndo_;
	jpegview_linux::ArchivePasswordDialogModel archivePasswordDialog_;
	fs::path archivePasswordDialogTarget_;
	std::string archivePasswordPendingValue_;
	bool archivePasswordValidationPending_ = false;
	bool fileDialogListingPending_ = false;
	bool fileDialogActivateAfterListing_ = false;
	bool fileDialogOpenDirectoryAfterListing_ = false;
	fs::path fileDialogFocusAfterListing_;
	jpegview_linux::DirectorySummaryLoader fileDialogSummaryLoader_;
	jpegview_linux::FileDialogDirectoryLoader fileDialogDirectoryLoader_;
	jpegview_linux::FileDialogFileSizeLoader fileDialogFileSizeLoader_;
	jpegview_linux::ArchiveDirectoryLoader fileDialogArchiveLoader_;
	jpegview_linux::FileDialogPreviewLoader fileDialogPreviewLoader_;
	std::unordered_map<std::string, jpegview_linux::DirectorySummary> fileDialogDirectorySummaries_;
	std::unordered_set<std::string> encryptedArchivePaths_;
	std::uint64_t fileDialogSummaryGeneration_ = 0;
	std::uint64_t fileDialogDirectoryGeneration_ = 0;
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
	jpegview_linux::BatchCopyDialogController batchCopyDialog_;
	jpegview_linux::ResizeDialogController resizeDialog_;
	jpegview_linux::CropSizeDialogController cropSizeDialog_;
	jpegview_linux::GoToImageNumberModel goToImageNumberDialog_;
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
	jpegview_linux::PixelColorSamplerModel pixelColorSampler_;
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
		<< "          Ctrl+G jumps to an image number, F2 toggles picture information, Shift+N toggles the filename overlay, Ctrl+O opens, Ctrl+S saves full size, Ctrl+Shift+S saves screen size, Ctrl+R reloads, Ctrl+N toggles navigation, Ctrl+T toggles thumbnails, Ctrl+E toggles crop selection mode,\n"
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
