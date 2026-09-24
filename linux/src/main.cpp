#include "sdl_abi.h"
#include "file_list.h"
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
#include "context_menu_model.h"
#include "overlay_layout.h"
#include "viewer_chrome.h"
#include "playback_scheduler.h"
#include "thumbnail_panel_model.h"
#include "thumbnail_resampler.h"
#include "app_icon.h"
#include "image_info_model.h"
#include "file_dialog_model.h"
#include "system_font.h"
#include "spectrum_model.h"
#include "desktop_association.h"

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
constexpr int kFileDialogResizeHandleSize = 18;
constexpr int kFileDialogMinimumListWidth = 180;
constexpr int kFileDialogMinimumPreviewWidth = 120;
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
constexpr int kConfirmRestoreParameterDb = -8;

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

std::string FormatFileTime(const fs::path& filename) {
	struct stat status{};
	if (stat(filename.c_str(), &status) != 0) return {};
	std::tm localTime{};
	if (localtime_r(&status.st_mtime, &localTime) == nullptr) return {};
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
		SDL_SetHint("SDL_RENDER_SCALE_QUALITY", "2");
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
		thumbnailResizeCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
		fileDialogResizeCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENWSE);
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
		SetTitle("JPEGView — Loading");
		PresentStartupFrame();
		SDL_PumpEvents();

		const jpegview_linux::FileList::SortMode initialSortMode = fileList_.GetSorting();
		const bool initialSortAscending = fileList_.IsSortedAscending();
		fileList_ = jpegview_linux::FileList(startupInputs_, initialSortMode,
			initialSortAscending);
		startupInputs_.clear();
		if (fileList_.Empty()) {
			std::cerr << "No supported images found.\n";
			Cleanup();
			return 2;
		}
		if (!LoadCurrent()) {
			Cleanup();
			return 1;
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
			TickFileDialogDirectorySummaries();
			Render();
			// Present the current image before doing renderer-thread cache uploads.
			// Held navigation then advances only after the closest ready neighbor
			// has had an opportunity to become a retained SDL texture.
			TickDisplayTexturePreload();
			TickThumbnailPreload(heldNavigation_.Scancode() < 0 &&
				!displayImageCache_.HasPendingWork());
			TickHeldNavigation();
			SDL_Delay(4);
		}

		Cleanup();
		return 0;
	}

private:
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
	};

	struct DisplayTextureCacheEntry {
		SDL_Texture* texture = nullptr;
		std::size_t bytes = 0;
		std::uint64_t lastUsed = 0;
	};

	struct JpegDimensionCacheEntry {
		std::uintmax_t fileSize = 0;
		fs::file_time_type modified{};
		int width = 0;
		int height = 0;
	};

	struct DisplayPrefetchContext {
		jpegview_linux::ViewportSnapshot viewport;
		int imageAreaWidth = 0;
		int imageAreaHeight = 0;
	};

	struct DisplayPrefetchBatch {
		std::mutex mutex;
		std::vector<jpegview_linux::DisplayImageRequest> requests;
		std::unordered_set<std::string> retainedTextureKeys;
		std::unordered_map<std::string, std::size_t> priorityByFilename;
		std::unordered_map<std::string, jpegview_linux::ImageProcessingParams> processingByFilename;
		std::unordered_map<std::string, bool> autoContrastByFilename;
		jpegview_linux::DisplayImageCache* cache = nullptr;
		DisplayPrefetchContext context;
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
	};

	void Cleanup() {
		SaveSettings();
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

		maximized_ = settings.maximized;
		navigationPanelEnabled_ = settings.navigationPanelEnabled;
		navigationPanelAutoReveal_ = settings.navigationPanelAutoReveal;
		thumbnailPanelVisible_ = settings.thumbnailPanelVisible;
		showZoomNavigator_ = settings.showZoomNavigator;
		thumbnailPanelWidth_ = settings.thumbnailPanelWidth;
		fileDialogWidth_ = settings.fileDialogWidth;
		fileDialogHeight_ = settings.fileDialogHeight;
		fileDialogPreviewRatio_ = settings.fileDialogPreviewRatio;
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

	bool SaveSettings() const {
		const fs::path settingsPath = jpegview_linux::ViewerSettingsPath();
		if (settingsPath.empty()) return false;

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
		settings.thumbnailPanelWidth = thumbnailPanelWidth_;
		settings.fileDialogWidth = fileDialogWidth_;
		settings.fileDialogHeight = fileDialogHeight_;
		settings.fileDialogPreviewRatio = fileDialogPreviewRatio_;
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
		return jpegview_linux::SaveViewerSettings(settingsPath, settings);
	}

	bool JpegDimensions(const fs::path& filename, int& width, int& height,
		std::string& errorMessage) {
		std::error_code error;
		const std::uintmax_t fileSize = fs::file_size(filename, error);
		if (error) return jpegview_linux::ReadJpegDimensions(
			filename, width, height, errorMessage);
		const fs::file_time_type modified = fs::last_write_time(filename, error);
		if (error) return jpegview_linux::ReadJpegDimensions(
			filename, width, height, errorMessage);
		const std::string key = filename.string();
		const auto cached = jpegDimensionCache_.find(key);
		if (cached != jpegDimensionCache_.end() && cached->second.fileSize == fileSize &&
			cached->second.modified == modified) {
			width = cached->second.width;
			height = cached->second.height;
			return true;
		}
		if (!jpegview_linux::ReadJpegDimensions(filename, width, height, errorMessage)) return false;
		jpegDimensionCache_[key] = {fileSize, modified, width, height};
		return true;
	}

	bool MaterializeCurrentPixels() {
		if (currentPixelsMaterialized_ &&
			jpegview_linux::EqualImageProcessing(materializedProcessing_, imageProcessing_) &&
			materializedAutoContrast_ == autoContrastEnabled_) return true;
		if (currentPixelsMaterialized_ && correctionBaseValid_) {
			image_ = correctionBase_;
			if (!image_.ApplyProcessing(imageProcessing_, autoContrastEnabled_)) return false;
			materializedProcessing_ = imageProcessing_;
			materializedAutoContrast_ = autoContrastEnabled_;
			return true;
		}
		if (!currentDecoded_) {
			currentDecoded_ = imageCache_.Find(fileList_.Current());
			if (!currentDecoded_) currentDecoded_ = imageCache_.FindOrWait(fileList_.Current());
			if (!currentDecoded_) {
				auto decoded = std::make_shared<jpegview_linux::DecodedImage>();
				std::string errorMessage;
				if (!jpegview_linux::DecodeImage(fileList_.Current(), *decoded, errorMessage) ||
					decoded->frames.empty()) {
					SetTitle(fileList_.Current().filename().string() +
						" — decode failed: " + errorMessage);
					return false;
				}
				imageCache_.Store(fileList_.Current(), decoded);
				currentDecoded_ = std::move(decoded);
			}
		}
		if (currentDecoded_->frames.empty()) return false;
		std::vector<Image> decodedFrames;
		if (currentDecoded_->frames.size() > 1) decodedFrames.reserve(currentDecoded_->frames.size());
		for (const jpegview_linux::DecodedFrame& decodedFrame : currentDecoded_->frames) {
			Image frame;
			if (!frame.StoreBGRA(decodedFrame.bgra.data(), decodedFrame.width, decodedFrame.height)) {
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
		if (!image_.ApplyProcessing(imageProcessing_, autoContrastEnabled_)) {
			SetTitle(fileList_.Current().filename().string() + " — picture-level processing failed");
			return false;
		}
		materializedProcessing_ = imageProcessing_;
		materializedAutoContrast_ = autoContrastEnabled_;
		currentPixelsMaterialized_ = true;
		return true;
	}

	void SelectPictureLevelsForCurrentFile() {
		const std::string key = AbsoluteNormalized(fileList_.Current()).string();
		const auto saved = imageProcessingStore_.find(key);
		const jpegview_linux::ImageProcessingPreset current{imageProcessing_, autoContrastEnabled_};
		const jpegview_linux::ImageProcessingPreset selected =
			jpegview_linux::ResolveImageProcessingForFile(current,
				saved == imageProcessingStore_.end() ? nullptr : &saved->second,
				keepPictureLevels_, defaultAutoContrastEnabled_, defaultImageProcessing_);
		imageProcessing_ = selected.processing;
		autoContrastEnabled_ = selected.autoContrast;
		imageProcessing_.unsharpRadius = unsharpMaskRadius_;
		imageProcessing_.unsharpAmount = 0.0;
		imageProcessing_.unsharpThreshold = unsharpMaskThreshold_;
	}

	bool LoadCurrent(int prefetchDirection = 0) {
		if (fileList_.Empty()) {
			return false;
		}
		ClearCropSelection();
		SelectPictureLevelsForCurrentFile();
		const jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.NavigationSnapshot();
		metadata_ = {};
		jpegComment_.clear();
		ClearTransition();
		std::string errorMessage;
		jpegview_linux::ReadJpegMetadata(fileList_.Current(), metadata_, jpegComment_);

		if (texture_ != nullptr) SDL_DestroyTexture(texture_);
		texture_ = nullptr;
		ClearDisplayTexture();
		displayTextureProtectedKeys_.clear();
		currentDecoded_.reset();
		currentAnimationFrame_ = 0;
		currentDisplayRequest_.reset();
		currentPixelsMaterialized_ = false;
		materializedProcessing_ = {};
		materializedAutoContrast_ = false;
		animationFrames_.clear();
		correctionBase_ = {};
		correctionBaseValid_ = false;
		image_ = {};
		imageModified_ = false;
		const Uint32 now = SDL_GetTicks();
		const SDL_Rect imageArea = ImageAreaRect();

		bool cachedDisplay = false;
		if (cacheBudget_->Capacity() != 0 && jpegview_linux::IsJpegPath(fileList_.Current())) {
			int sourceWidth = 0;
			int sourceHeight = 0;
			if (JpegDimensions(fileList_.Current(), sourceWidth,
				sourceHeight, errorMessage)) {
				image_.width = image_.originalWidth = sourceWidth;
				image_.height = image_.originalHeight = sourceHeight;
				RestoreScaleMode(viewportSnapshot);
				const jpegview_linux::ViewportRect destination = viewport_.Destination(
					sourceWidth, sourceHeight, imageArea.w, imageArea.h);
				currentDisplayRequest_ = jpegview_linux::MakeJpegDisplayImageRequest(
					fileList_.Current(), sourceWidth, sourceHeight, destination.width,
					destination.height, autoContrastEnabled_, 0, imageProcessing_);
				if (currentDisplayRequest_->Valid()) {
					displayTextureProtectedKeys_.insert(currentDisplayRequest_->key);
					cachedDisplay = FindDisplayTexture(currentDisplayRequest_->key) != nullptr;
					if (!cachedDisplay) {
						auto prepared = displayImageCache_.Find(*currentDisplayRequest_);
						if (!prepared) prepared =
							displayImageCache_.RequestAndWait(*currentDisplayRequest_);
						if (prepared) cachedDisplay = CacheDisplayTexture(prepared);
					}
				}
			}
		}

		std::vector<int> animationFrameDelaysMs;
		if (!cachedDisplay) {
			jpegview_linux::DecodedImageCache::ImagePtr decoded = imageCache_.Find(fileList_.Current());
			if (!decoded) decoded = imageCache_.FindOrWait(fileList_.Current());
			if (!decoded) {
				auto loaded = std::make_shared<jpegview_linux::DecodedImage>();
				if (!jpegview_linux::DecodeImage(fileList_.Current(), *loaded, errorMessage) ||
					loaded->frames.empty()) {
					SetTitle(fileList_.Current().filename().string() +
						" — decode failed: " + errorMessage);
					std::cerr << fileList_.Current() << ": " << errorMessage << '\n';
					return false;
				}
				imageCache_.Store(fileList_.Current(), loaded);
				decoded = std::move(loaded);
			}
			currentDecoded_ = decoded;
			animationFrameDelaysMs.reserve(decoded->frames.size());
			for (const jpegview_linux::DecodedFrame& decodedFrame : decoded->frames) {
				animationFrameDelaysMs.push_back(std::max(10, decodedFrame.delayMs));
			}
			const jpegview_linux::DecodedFrame& firstFrame = decoded->frames.front();
			image_.width = image_.originalWidth = firstFrame.width;
			image_.height = image_.originalHeight = firstFrame.height;
			RestoreScaleMode(viewportSnapshot);
			const jpegview_linux::ViewportRect destination = viewport_.Destination(
				image_.width, image_.height, imageArea.w, imageArea.h);
			const jpegview_linux::DisplayImageRequest* displayRequest =
				CurrentDisplayRequest(destination.width, destination.height);
			if (displayRequest != nullptr) displayTextureProtectedKeys_.insert(displayRequest->key);
			cachedDisplay = displayRequest != nullptr &&
				FindDisplayTexture(displayRequest->key) != nullptr;
			if (!cachedDisplay && displayRequest != nullptr) {
				if (const auto prepared = displayImageCache_.Find(*displayRequest)) {
					cachedDisplay = CacheDisplayTexture(prepared);
				}
			}
			if (!cachedDisplay || decoded->frames.size() > 1) {
				if (!MaterializeCurrentPixels()) return false;
			}
			if (!cachedDisplay && !UpdateTexture()) return false;
			playback_.ConfigureImage(std::move(animationFrameDelaysMs), decoded->loopCount,
				decoded->animation, now);
		} else {
			playback_.ConfigureImage({}, 0, false, now);
		}
		cropSelection_.SetImageSize(image_.width, image_.height);
		SetTitle();
		PrepareThumbnailPreload();
		PrepareImagePrefetch(prefetchDirection);
		return true;
	}

	void PrepareImagePrefetch(int preferredDirection = 0) {
		if (fileList_.Empty() || clipboardMode_) return;
		displayImageCache_.Prefetch({});
		const SDL_Rect imageArea = ImageAreaRect();
		auto batch = std::make_shared<DisplayPrefetchBatch>();
		batch->cache = &displayImageCache_;
		batch->context = {viewport_.NavigationSnapshot(), imageArea.w, imageArea.h};
		for (const auto& retained : displayTextureCache_) {
			batch->retainedTextureKeys.insert(retained.first);
		}
		displayTextureProtectedKeys_.clear();
		if (currentDisplayRequest_.has_value()) {
			displayTextureProtectedKeys_.insert(currentDisplayRequest_->key);
		}
		const std::vector<std::size_t> prefetchOrder = jpegview_linux::ImagePrefetchOrder(
			fileList_.Files().size(), fileList_.CurrentIndex(), preferredDirection,
			jpegview_linux::DisplayPrefetchCount(cacheBudget_->Capacity(), imageArea.w,
				imageArea.h, fileList_.Files().size()));
		for (std::size_t position = 0; position < prefetchOrder.size(); ++position) {
			const fs::path& filename = fileList_.Files()[prefetchOrder[position]];
			batch->priorityByFilename.emplace(filename.string(), position + 1);
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
			if (!jpegview_linux::IsJpegPath(filename)) continue;
			int sourceWidth = 0;
			int sourceHeight = 0;
			std::string errorMessage;
			if (!JpegDimensions(filename, sourceWidth, sourceHeight,
				errorMessage)) continue;
			jpegview_linux::Viewport viewport;
			viewport.Restore(batch->context.viewport, sourceWidth, sourceHeight,
				batch->context.imageAreaWidth, batch->context.imageAreaHeight);
			const jpegview_linux::ViewportRect target = viewport.Destination(
				sourceWidth, sourceHeight, batch->context.imageAreaWidth,
				batch->context.imageAreaHeight);
			jpegview_linux::DisplayImageRequest request =
				jpegview_linux::MakeJpegDisplayImageRequest(filename, sourceWidth, sourceHeight,
					target.width, target.height, filePreset.autoContrast, position + 1,
					fileProcessing);
			if (!request.Valid()) continue;
			displayTextureProtectedKeys_.insert(request.key);
			if (batch->retainedTextureKeys.find(request.key) ==
				batch->retainedTextureKeys.end()) batch->requests.push_back(std::move(request));
		}
		displayImageCache_.Prefetch(batch->requests);
		imageCache_.Prefetch(fileList_.Files(), fileList_.CurrentIndex(),
			preferredDirection, kDecodedImagePrefetchCount,
			[batch](const fs::path& filename,
				const jpegview_linux::DecodedImageCache::ImagePtr& decoded) {
				if (!decoded || decoded->frames.empty()) return;
				const jpegview_linux::DecodedFrame& frame = decoded->frames.front();
				jpegview_linux::Viewport viewport;
				viewport.Restore(batch->context.viewport, frame.width, frame.height,
					batch->context.imageAreaWidth, batch->context.imageAreaHeight);
				const jpegview_linux::ViewportRect target = viewport.Destination(
					frame.width, frame.height, batch->context.imageAreaWidth,
					batch->context.imageAreaHeight);
				const auto priority = batch->priorityByFilename.find(filename.string());
				const auto processing = batch->processingByFilename.find(filename.string());
				const auto autoContrast = batch->autoContrastByFilename.find(filename.string());
				if (priority == batch->priorityByFilename.end() ||
					processing == batch->processingByFilename.end() ||
					autoContrast == batch->autoContrastByFilename.end()) return;
				jpegview_linux::DisplayImageRequest request =
					jpegview_linux::MakeDisplayImageRequest(filename, decoded, 0,
						target.width, target.height, autoContrast->second,
						priority->second, processing->second);
				if (!request.Valid() ||
					batch->retainedTextureKeys.find(request.key) !=
						batch->retainedTextureKeys.end()) return;
				std::lock_guard<std::mutex> lock(batch->mutex);
				batch->requests.push_back(std::move(request));
				batch->cache->Prefetch(batch->requests);
			}, [](const fs::path& filename) {
				return !jpegview_linux::IsJpegPath(filename);
			});
	}

	bool UpdateTexture() {
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
		return CreateTexture(source.bgra, source.width, source.height);
	}

	SDL_Texture* CreateTexture(const std::vector<std::uint8_t>& bgra, int width, int height) {
		if (width <= 0 || height <= 0 || bgra.size() !=
			static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) return nullptr;
		// Viewer textures are uploaded once and then sampled repeatedly. Static
		// access lets accelerated backends place them for rendering instead of
		// maintaining the lockable staging behavior intended for frequent writes.
		SDL_Texture* result = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
			width, height);
		if (result == nullptr) return nullptr;
		if (SDL_UpdateTexture(result, nullptr, bgra.data(), width * 4) != 0) {
			std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << '\n';
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
		QueuePreparedThumbnail(prepared);
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
		SDL_Texture* texture = CreateTexture(prepared->bgra, prepared->width, prepared->height);
		if (texture == nullptr) {
			cacheBudget_->Release(bytes);
			displayImageCache_.Retire(prepared);
			return false;
		}
		displayTextureCache_.emplace(prepared->key,
			DisplayTextureCacheEntry{texture, bytes, ++displayTextureUseCounter_});
		displayTextureCacheBytes_ += bytes;
		displayImageCache_.Retire(prepared);
		return true;
	}

	void TickDisplayTexturePreload() {
		for (const jpegview_linux::DisplayImageCache::ImagePtr& prepared :
			displayImageCache_.TakeCompleted(kDisplayTextureUploadsPerTick)) {
			CacheDisplayTexture(prepared);
		}
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
		if (imageModified_ || fileList_.Empty() || width <= 0 || height <= 0 ||
			(currentDecoded_ && currentAnimationFrame_ >= currentDecoded_->frames.size())) {
			currentDisplayRequest_.reset();
			return nullptr;
		}
		if (!currentDisplayRequest_.has_value() ||
			currentDisplayRequest_->decoded != currentDecoded_ ||
			currentDisplayRequest_->frameIndex != currentAnimationFrame_ ||
			currentDisplayRequest_->targetWidth != width ||
			currentDisplayRequest_->targetHeight != height ||
			currentDisplayRequest_->autoContrast != autoContrastEnabled_ ||
			!jpegview_linux::EqualImageProcessing(currentDisplayRequest_->processing,
				imageProcessing_)) {
			if (currentDecoded_) {
				currentDisplayRequest_ = jpegview_linux::MakeDisplayImageRequest(
					fileList_.Current(), currentDecoded_, currentAnimationFrame_, width, height,
					autoContrastEnabled_, 0, imageProcessing_);
			} else {
				currentDisplayRequest_ = jpegview_linux::MakeJpegDisplayImageRequest(
					fileList_.Current(), image_.originalWidth, image_.originalHeight, width, height,
					autoContrastEnabled_, 0, imageProcessing_);
			}
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
			if (texture_ == nullptr && MaterializeCurrentPixels()) texture_ = CreateTexture(image_);
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
		displayImage_ = image_;
		if (!displayImage_.Resize(width, height)) {
			displayImage_ = {};
			return texture_;
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

	void ClearThumbnailCache() {
		thumbnailPreparation_.Clear();
		for (auto& cached : thumbnailCache_) {
			if (cached.second.texture != nullptr) SDL_DestroyTexture(cached.second.texture);
		}
		thumbnailCache_.clear();
		thumbnailScheduler_.Clear();
	}

	void EvictThumbnails(const std::vector<std::string>& keys) {
		for (const std::string& key : keys) {
			const auto cached = thumbnailCache_.find(key);
			if (cached == thumbnailCache_.end()) continue;
			if (cached->second.texture != nullptr) SDL_DestroyTexture(cached->second.texture);
			thumbnailCache_.erase(cached);
		}
	}

	void PrepareThumbnailPreload() {
		if (!thumbnailPanelVisible_ || fileList_.Empty()) {
			thumbnailPreparation_.Clear();
			EvictThumbnails(thumbnailScheduler_.Prepare({}, 0, thumbnailCache_.size()));
			return;
		}
		std::vector<std::string> keys;
		keys.reserve(fileList_.Files().size());
		for (const fs::path& path : fileList_.Files()) {
			keys.push_back(path.string());
		}
		// Thumbnails are intentionally outside the large-image cache budget and
		// are retained for every file in the active list once generated.
		EvictThumbnails(thumbnailScheduler_.Prepare(
			keys, fileList_.CurrentIndex(), keys.size()));
	}

	void QueuePreparedThumbnail(
		const jpegview_linux::DisplayImageCache::ImagePtr& prepared) {
		if (!thumbnailPanelVisible_ || autoContrastEnabled_ || !prepared ||
			prepared->filename.empty() || prepared->bgra.empty()) return;
		if (!jpegview_linux::CanReuseDisplayPixelsForThumbnail(
			prepared->width, prepared->height, kMaximumThumbnailSourcePixels)) return;
		const std::string key = prepared->filename.string();
		if (thumbnailCache_.find(key) != thumbnailCache_.end()) return;
		const auto found = std::find(fileList_.Files().begin(), fileList_.Files().end(),
			prepared->filename);
		if (found == fileList_.Files().end()) return;
		const std::size_t index = static_cast<std::size_t>(
			std::distance(fileList_.Files().begin(), found));
		const std::size_t current = fileList_.CurrentIndex();
		const std::size_t distance = index > current ? index - current : current - index;
		const std::size_t priority = distance * 2 + (index > current ? 1 : 0);
		const SDL_Rect panel = ThumbnailPanelRect();
		const int rowHeight = jpegview_linux::ThumbnailRowHeight(
			panel.w, kThumbnailVerticalMargin);
		(void)thumbnailPreparation_.Request({key, prepared, panel.w,
			rowHeight - kThumbnailVerticalMargin * 2 - 1, priority});
	}

	void TickThumbnailPreload(bool allowIndependentDecode) {
		if (!thumbnailPanelVisible_) return;
		const Uint32 now = SDL_GetTicks();
		for (const auto& prepared : thumbnailPreparation_.TakeCompleted(1)) {
			if (!prepared || prepared->key.empty() ||
				thumbnailCache_.find(prepared->key) != thumbnailCache_.end()) continue;
			const auto active = std::find_if(fileList_.Files().begin(), fileList_.Files().end(),
				[&prepared](const fs::path& path) { return path.string() == prepared->key; });
			if (active == fileList_.Files().end()) continue;
			ThumbnailCacheEntry cached;
			cached.texture = CreateTexture(prepared->bgra, prepared->width, prepared->height);
			cached.width = prepared->width;
			cached.height = prepared->height;
			thumbnailCache_.emplace(prepared->key, std::move(cached));
			EvictThumbnails(thumbnailScheduler_.Store(prepared->key));
		}
		if (!allowIndependentDecode || thumbnailPreparation_.HasPendingWork()) return;
		const std::optional<jpegview_linux::ThumbnailLoadRequest> request = thumbnailScheduler_.Next(now);
		if (!request.has_value() || request->fileIndex >= fileList_.Files().size() ||
			fileList_.Files()[request->fileIndex].string() != request->key) return;
		const fs::path& path = fileList_.Files()[request->fileIndex];
		ThumbnailCacheEntry cached;
		const SDL_Rect panel = ThumbnailPanelRect();
		const int rowHeight = jpegview_linux::ThumbnailRowHeight(panel.w, kThumbnailVerticalMargin);
		jpegview_linux::DecodedImageCache::ImagePtr decoded;
		jpegview_linux::ThumbnailSize size;
		if (jpegview_linux::IsJpegPath(path)) {
			int sourceWidth = 0;
			int sourceHeight = 0;
			std::string errorMessage;
			if (JpegDimensions(path, sourceWidth, sourceHeight, errorMessage)) {
				size = jpegview_linux::FitThumbnailSize(sourceWidth, sourceHeight, panel.w,
					rowHeight - kThumbnailVerticalMargin * 2 - 1);
				auto loaded = std::make_shared<jpegview_linux::DecodedImage>();
				int decodedSourceWidth = 0;
				int decodedSourceHeight = 0;
				if (size.width > 0 && size.height > 0 &&
					jpegview_linux::DecodeJpegForDisplay(path, size.width, size.height, *loaded,
						decodedSourceWidth, decodedSourceHeight, errorMessage) &&
					!loaded->frames.empty()) decoded = std::move(loaded);
			}
		} else {
			decoded = imageCache_.Find(path);
			if (!decoded) {
				auto loaded = std::make_shared<jpegview_linux::DecodedImage>();
				std::string errorMessage;
				if (jpegview_linux::DecodeImage(path, *loaded, errorMessage) &&
					!loaded->frames.empty()) {
					imageCache_.Store(path, loaded);
					decoded = std::move(loaded);
				}
			}
		}
		if (decoded && !decoded->frames.empty()) {
			const jpegview_linux::DecodedFrame& frame = decoded->frames.front();
			if (size.width <= 0 || size.height <= 0) {
				size = jpegview_linux::FitThumbnailSize(frame.width, frame.height, panel.w,
					rowHeight - kThumbnailVerticalMargin * 2 - 1);
			}
			Image thumbnail;
			std::vector<std::uint8_t> thumbnailPixels;
			if (size.width > 0 && size.height > 0 &&
				jpegview_linux::DownsampleThumbnailBgra(frame.bgra, frame.width, frame.height,
					size.width, size.height, thumbnailPixels) &&
				thumbnail.StoreBGRA(thumbnailPixels.data(), size.width, size.height)) {
				cached.texture = CreateTexture(thumbnail);
				cached.width = size.width;
				cached.height = size.height;
			}
		}
		thumbnailCache_.emplace(request->key, std::move(cached));
		EvictThumbnails(thumbnailScheduler_.Complete(*request, now));
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
		if (!MaterializeCurrentPixels()) return;
		ClearCropSelection();
		const jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.Snapshot();
		if (!TransformImage(image_, command) ||
			(correctionBaseValid_ && !TransformImage(correctionBase_, command)) || !UpdateTexture()) {
			SetTitle("Image transform failed");
			return;
		}
		imageModified_ = true;
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
		if (imageModified_ || cacheBudget_->Capacity() == 0) {
			if (!MaterializeCurrentPixels() || !UpdateTexture()) {
				SetTitle("Picture-level processing failed: could not update the image");
				return;
			}
		}
		if (refreshNeighbors) PrepareImagePrefetch();
	}

	void ApplyLosslessJpegTransform(int command) {
		if (fileList_.Empty() || clipboardMode_) return;
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
		ReloadAfterFileChange();
		SetTitle("Applied lossless JPEG transformation");
	}

	void SetTitle(const std::string& title) {
		SDL_SetWindowTitle(window_, title.c_str());
	}

	void SetTitle() {
		if (fileList_.Empty() || image_.originalWidth <= 0 || image_.originalHeight <= 0) {
			SetTitle("JPEGView");
			return;
		}
		std::error_code fileError;
		const std::uintmax_t fileSize = fs::file_size(fileList_.Current(), fileError);
		std::ostringstream title;
		title << fileList_.Current().filename().string()
			<< " (" << image_.originalWidth << 'x' << image_.originalHeight;
		if (!fileError) title << ", " << jpegview_linux::FormatFileSize(fileSize);
		title << ") - JPEGView";
		SetTitle(title.str());
	}

	void OpenDroppedFiles(const std::vector<std::string>& droppedFiles) {
		if (droppedFiles.empty()) {
			return;
		}
		RestoreClipboardImage();

		try {
			jpegview_linux::FileList droppedFileList(
				droppedFiles, fileList_.GetSorting(), fileList_.IsSortedAscending(), fileList_.WrapAroundFolder());
			// Construction performs the same initial-file/folder discovery as
			// the Windows CFileList constructor.
			if (droppedFileList.Empty()) {
				SetTitle("No supported images in dropped input");
				return;
			}

			const jpegview_linux::FileList::NavigationMode navigationMode = fileList_.GetNavigationMode();
			fileList_ = std::move(droppedFileList);
			fileList_.SetNavigationMode(navigationMode);
		} catch (const fs::filesystem_error& error) {
			SetTitle(std::string("Cannot open dropped input: ") + error.what());
			return;
		}
		dragging_ = false;
		CloseFileDialog();
		LoadCurrent();
	}

	void CloseFileDialog() {
		if (fileDialogOpen_) SDL_StopTextInput();
		if (fileDialogDragMode_ != FileDialogDragMode::None) SDL_CaptureMouse(SDL_FALSE);
		fileDialogDragMode_ = FileDialogDragMode::None;
		SDL_SetCursor(SDL_GetDefaultCursor());
		ClearFileDialogPreview();
		++fileDialogSummaryGeneration_;
		fileDialogSummaryLoader_.Request({}, fileDialogSummaryGeneration_);
		fileDialogOpen_ = false;
		fileDialogSave_ = false;
		fileDialogParameterBackup_ = false;
		fileDialogParameterRestore_ = false;
		fileDialogLosslessCrop_ = false;
		fileDialogLosslessCropRect_ = {};
		fileDialogFilename_.clear();
		fileDialogModel_.Clear();
		fileDialogDirectorySummaries_.clear();
	}

	void OpenLosslessCropDialog() {
		if (fileList_.Empty() || clipboardMode_ || imageModified_ ||
			!jpegview_linux::IsJpegPath(fileList_.Current()) || !cropSelection_.HasSelection()) {
			SetTitle("Lossless crop requires an untransformed JPEG selection");
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
			ReloadAfterFileChange();
		} else {
			fileList_.Reload();
			PrepareThumbnailPreload();
		}
		SetTitle("Saved lossless crop: " + savedName);
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
		CloseFileDialog();
		SetTitle("Saved processed image: " + savedName);
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
		const double targetZoom = std::min(
			static_cast<double>(std::max(1, imageArea.w)) / bounds.Width(),
			static_cast<double>(std::max(1, imageArea.h)) / bounds.Height());
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			image_.width, image_.height, imageArea.w, imageArea.h);
		const int selectionCenterX = destination.x + static_cast<int>(std::lround(
			(bounds.left + bounds.right) * 0.5 * viewport_.Zoom()));
		const int selectionCenterY = destination.y + static_cast<int>(std::lround(
			(bounds.top + bounds.bottom) * 0.5 * viewport_.Zoom()));
		viewport_.ZoomAt(targetZoom / viewport_.Zoom(), selectionCenterX, selectionCenterY,
			image_.width, image_.height, imageArea.w, imageArea.h);
		viewport_.ClampToView(image_.width, image_.height, imageArea.w, imageArea.h);
		PanViewport(imageArea.w / 2.0 - selectionCenterX,
			imageArea.h / 2.0 - selectionCenterY);
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
		const fs::path directory = fileList_.Current().parent_path();
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

	void ReloadAfterFileChange() {
		if (fileList_.Reload()) LoadCurrent();
	}

	void TouchCurrentImage(bool useExifDate) {
		if (fileList_.Empty() || clipboardMode_) return;
		std::time_t timestamp = std::time(nullptr);
		if (useExifDate) {
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
		ReloadAfterFileChange();
		SetTitle(useExifDate ? "Set modification date to EXIF date" : "Set modification date to current date");
	}

	void TouchFolderImagesToExifDate() {
		if (fileList_.Empty() || clipboardMode_) return;
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
		ReloadAfterFileChange();
		SetTitle("Set EXIF dates for " + std::to_string(updated) + " image(s)");
	}

	void SetWallpaper(bool processed) {
		if (fileList_.Empty()) return;
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
		if (fileList_.Reload()) {
			LoadCurrent();
		} else {
			quitRequested_ = true;
		}
		SetTitle("Moved image to trash");
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

	void OpenAbout() {
		aboutOpen_ = true;
		contextMenuOpen_ = false;
		SetTitle("About JPEGView Linux");
	}

	void HandleAboutEvents(const SDL_Event& event) {
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
			(event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_RETURN ||
			 event.key.keysym.sym == SDLK_SPACE)) {
			aboutOpen_ = false;
			SetTitle();
		} else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
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

		RestoreClipboardImage();
		fileListBeforeClipboard_ = std::make_unique<jpegview_linux::FileList>(std::move(fileList_));
		fileList_ = jpegview_linux::FileList({temporaryFile.string()});
		clipboardTempFile_ = temporaryFile;
		clipboardTempDirectory_ = temporaryDirectory;
		clipboardMode_ = true;
		LoadCurrent();
		SetTitle("Clipboard image — press next/previous to return to the file list");
	}

	void RestoreScaleMode(const jpegview_linux::ViewportSnapshot& snapshot) {
		const SDL_Rect imageArea = ImageAreaRect();
		viewport_.Restore(snapshot, image_.width, image_.height, imageArea.w, imageArea.h);
		SetTitle();
	}

	void FitToWindow(bool fillCrop = false, bool noEnlarge = true) {
		const SDL_Rect imageArea = ImageAreaRect();
		viewport_.Fit(image_.width, image_.height, imageArea.w, imageArea.h, fillCrop, noEnlarge);
		currentDisplayRequest_.reset();
		PrepareImagePrefetch();
		SetTitle();
	}

	void ActualSize() {
		viewport_.ActualSize();
		currentDisplayRequest_.reset();
		PrepareImagePrefetch();
		SetTitle();
	}

	void PanActualSize(int command) {
		if (!viewport_.IsActualSize()) return;
		const double deltaX = command == IDM_PAN_LEFT ? kKeyboardPanStep :
			command == IDM_PAN_RIGHT ? -kKeyboardPanStep : 0.0;
		const double deltaY = command == IDM_PAN_UP ? kKeyboardPanStep :
			command == IDM_PAN_DOWN ? -kKeyboardPanStep : 0.0;
		PanViewport(deltaX, deltaY);
		playback_.NotifyInteraction(SDL_GetTicks());
		SetTitle();
	}

	void PanViewport(double deltaX, double deltaY) {
		viewport_.Pan(deltaX, deltaY);
		const SDL_Rect imageArea = ImageAreaRect();
		viewport_.ClampToView(image_.width, image_.height, imageArea.w, imageArea.h);
		ShowZoomNavigatorTemporarily();
	}

	void ShowZoomNavigatorTemporarily() {
		zoomNavigatorVisibleUntil_ = SDL_GetTicks() + 1200;
	}

	void ZoomAt(double factor, int mouseX, int mouseY) {
		if (image_.width == 0 || image_.height == 0) {
			return;
		}
		const SDL_Rect imageArea = ImageAreaRect();
		const int localMouseX = std::clamp(mouseX - imageArea.x, 0, imageArea.w);
		const int localMouseY = std::clamp(mouseY - imageArea.y, 0, imageArea.h);
		viewport_.ZoomAt(factor, localMouseX, localMouseY, image_.width, image_.height,
			imageArea.w, imageArea.h);
		viewport_.ClampToView(image_.width, image_.height, imageArea.w, imageArea.h);
		ShowZoomNavigatorTemporarily();
		playback_.NotifyInteraction(SDL_GetTicks());
		SetTitle();
	}

	void NextImage(bool showPendingNavigation = false) {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = playback_.SlideshowSeconds() > 0.0 && transitionEffect_ != IDM_EFFECT_NONE;
		Image previousImage;
		if (animate && MaterializeCurrentPixels()) previousImage = image_;
		if (!fileList_.Next()) return;
		SetTitle();
		if (showPendingNavigation) {
			Render();
		}
		const bool loaded = LoadCurrent(1);
		if (loaded && animate) StartTransition(previousImage);
	}

	void PreviousImage(bool showPendingNavigation = false) {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = playback_.SlideshowSeconds() > 0.0 && transitionEffect_ != IDM_EFFECT_NONE;
		Image previousImage;
		if (animate && MaterializeCurrentPixels()) previousImage = image_;
		if (!fileList_.Previous()) return;
		SetTitle();
		if (showPendingNavigation) {
			Render();
		}
		const bool loaded = LoadCurrent(-1);
		if (loaded && animate) StartTransition(previousImage);
	}

	void NavigateToSiblingFolder(int direction) {
		if (clipboardMode_) RestoreClipboardImage();
		const bool navigated = direction < 0 ? fileList_.PreviousSiblingDirectory() :
			fileList_.NextSiblingDirectory();
		if (navigated) LoadCurrent(direction);
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
			SetTitle();
		}
	}

	void FitWindowToImage() {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
		const int panelWidth = thumbnailPanelVisible_ ? thumbnailPanelWidth_ : 0;
		const int width = std::clamp(image_.width + panelWidth + 16, 160, 4096);
		const int height = std::clamp(image_.height + 16, 120, 4096);
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
		if (contextMenuOpen_ || fileDialogOpen_ || confirmationOpen_ || aboutOpen_ || helpOpen_ ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen() || cropSizeDialog_.IsOpen() ||
			(SDL_GetModState() & 0x03C3u) != 0) {
			heldNavigation_.Reset();
			return;
		}

		SDL_PumpEvents();
		int keyCount = 0;
		const Uint8* keyStates = SDL_GetKeyboardState(&keyCount);
		const int scancode = heldNavigation_.Scancode();
		const bool keyIsHeld = keyStates != nullptr && scancode < keyCount && keyStates[scancode] != 0;
		const int direction = heldNavigation_.AfterImageShown(keyIsHeld);
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
		if (fileList_.Empty()) return lines;

		std::ostringstream title;
		title << '[' << fileList_.CurrentIndex() + 1 << '/' << fileList_.Size() << "] "
			<< InfoText(fileList_.Current().filename().string());
		lines.push_back(title.str());

		std::error_code fileError;
		const std::uintmax_t fileSize = fs::file_size(fileList_.Current(), fileError);
		lines.push_back(jpegview_linux::FormatImageDimensionsAndSize(
			image_.originalWidth, image_.originalHeight,
			fileError ? std::string() : jpegview_linux::FormatFileSize(fileSize)));
		if (animationFrames_.size() > 1) {
			lines.push_back("Frame: " + std::to_string(playback_.FrameIndex() + 1) + "/" +
				std::to_string(animationFrames_.size()));
			lines.push_back(std::string("Playback: ") + (playback_.AnimationPlaying() ? "playing" : "paused"));
		}
		if (image_.width != image_.originalWidth || image_.height != image_.originalHeight) {
			lines.push_back("Displayed size: " + std::to_string(image_.width) + " x " + std::to_string(image_.height));
		}
		const std::string modificationDate = FormatFileTime(fileList_.Current());
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

	jpegview_linux::InformationOverlayPaintPlan BuildImageInfoPaintPlan() {
		std::vector<std::string> lines = ImageInfoLines();
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
		return jpegview_linux::BuildNavigationPanelPaint(windowWidth, windowHeight,
			lastMouseX_, lastMouseY_, viewport_.IsFitToWindow(), fileList_.GetSorting(),
			TextWidth(sortLabel, kUiTextScale), TextWidth("1:1", kUiTextScale),
			TextLineHeight(kUiTextScale), selectionModeEnabled_);
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
				if (fileList_.ToggleBetweenMarkedAndCurrent()) {
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
			if (fileList_.Reload()) LoadCurrent();
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
			}
			SaveSettings();
			break;
		case jpegview_linux::kCommandToggleZoomNavigator:
			showZoomNavigator_ = !showZoomNavigator_;
			SaveSettings();
			UpdateCropCursor(lastMouseX_, lastMouseY_);
			UpdateZoomNavigatorCursor(lastMouseX_, lastMouseY_);
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
			fileList_.SetNavigationMode(jpegview_linux::FileList::NavigationMode::LoopDirectory);
			SetTitle();
			break;
		case IDM_LOOP_RECURSIVELY:
			fileList_.SetNavigationMode(jpegview_linux::FileList::NavigationMode::LoopSubDirectories);
			SetTitle();
			break;
		case IDM_LOOP_SIBLINGS:
			fileList_.SetNavigationMode(jpegview_linux::FileList::NavigationMode::LoopSameDirectoryLevel);
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
			ZoomAt(4.0 / viewport_.Zoom(), imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_200:
			ZoomAt(2.0 / viewport_.Zoom(), imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_50:
			ZoomAt(0.5 / viewport_.Zoom(), imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_25:
			ZoomAt(0.25 / viewport_.Zoom(), imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_INC:
			ZoomAt(1.2, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_DEC:
			ZoomAt(1.0 / 1.2, imageCenterX_, imageCenterY_);
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
		if (contextMenuCropOnly_) openWithApplications_.clear();
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
		state.selectionModeEnabled = selectionModeEnabled_;
		state.navigationMode = fileList_.GetNavigationMode();
		state.sortMode = fileList_.GetSorting();
		state.sortAscending = fileList_.IsSortedAscending();
		state.imageAvailable = image_.width > 0;
		state.losslessJpegAvailable = !clipboardMode_ && HasExecutable("jpegtran") &&
			(extension == ".jpg" || extension == ".jpeg" || extension == ".jpe");
		state.cropContextMenu = contextMenuCropOnly_;
		state.cropSelectionAvailable = cropSelection_.HasSelection();
		state.losslessJpegCropAvailable = state.losslessJpegAvailable && !imageModified_;
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

	void OpenContextMenu() {
		// A first right-click can arrive before SDL has delivered any motion
		// event. Read the current pointer state so both mouse and keyboard
		// invocation place the menu at the actual pointer position.
		int x = 0;
		int y = 0;
		SDL_GetMouseState(&x, &y);
		contextMenuAdvancedOptions_ = false;
		contextMenuCropOnly_ = false;
		contextMenuItems_ = ContextMenuItems(contextMenuAdvancedOptions_);
		contextMenuX_ = x;
		contextMenuY_ = y;
		RepositionContextMenuToFit();
		contextMenuOpen_ = true;
		menuSelected_ = -1;
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
	}

	void CloseContextMenu() {
		if (!contextMenuOpen_) return;
		contextMenuOpen_ = false;
		contextMenuAdvancedOptions_ = false;
		contextMenuCropOnly_ = false;
		contextMenuPositionLocked_ = false;
		menuSelected_ = -1;
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
			if (fileList_.Reload(preferredCurrentPath)) LoadCurrent();
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
		return !fileDialogSave_ && !fileDialogParameterRestore_;
	}

	SDL_Rect FileDialogListRect() const {
		const SDL_Rect dialog = FileDialogRect();
		const int previewWidth = FileDialogHasPreviewColumn() ?
			FileDialogPreviewWidth() + kFileDialogDividerWidth : 0;
		const int width = dialog.w - 24 - previewWidth;
		return SDL_Rect{dialog.x + 12, FileDialogListTop(), width,
			FileDialogVisibleRows() * 26};
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
		const int selected = fileDialogModel_.SelectedIndex();
		if (selected >= 0) fileDialogModel_.Select(selected, FileDialogVisibleRows());
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

	void EndFileDialogResize(int x, int y) {
		if (fileDialogDragMode_ == FileDialogDragMode::None) return;
		fileDialogDragMode_ = FileDialogDragMode::None;
		SDL_CaptureMouse(SDL_FALSE);
		UpdateFileDialogCursor(x, y);
		SaveSettings();
	}

	void UpdateFileDialogCursor(int x, int y) const {
		if (fileDialogDragMode_ == FileDialogDragMode::Resize ||
			PointInRect(x, y, FileDialogResizeHandleRect())) {
			if (fileDialogResizeCursor_ != nullptr) SDL_SetCursor(fileDialogResizeCursor_);
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
		const FileDialogEntry* selected = fileDialogModel_.SelectedEntry();
		std::string contentKey;
		std::string requestKey;
		if (selected != nullptr) {
			contentKey = selected->path.string() + (selected->directory ? "\nD\n" : "\nF\n") +
				(fileDialogModel_.SortMode() == jpegview_linux::FileDialogSortMode::Name ? "N" : "M") +
				"\n";
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
				fileDialogPreviewSource_.clear();
				fileDialogPreviewMessage_.clear();
				fileDialogPreviewContentKey_ = contentKey;
			}
			fileDialogPreviewRequestKey_ = requestKey;
			if (selected == nullptr) {
				fileDialogPreviewLoader_.Clear();
				fileDialogPreviewGeneration_ = 0;
			} else {
				fileDialogPreviewGeneration_ = fileDialogPreviewLoader_.Request(selected->path,
					selected->directory, fileDialogModel_.SortMode(), previewSize.width,
					previewSize.height);
				if (!sameContent || fileDialogPreviewTexture_ == nullptr) {
					fileDialogPreviewMessage_ = "Loading preview...";
				}
			}
		}

		for (jpegview_linux::FileDialogPreviewResult& result : fileDialogPreviewLoader_.TakeReady()) {
			if (result.generation != fileDialogPreviewGeneration_) continue;
			fileDialogPreviewSource_ = result.source;
			fileDialogPreviewMessage_ = result.error;
			if (!result.bgra.empty()) {
				SDL_Texture* previewTexture = CreateTexture(result.bgra, result.width, result.height);
				if (previewTexture == nullptr) {
					fileDialogPreviewMessage_ = "Cannot create preview";
				} else {
					SDL_SetTextureBlendMode(previewTexture, SDL_BLENDMODE_BLEND);
					if (fileDialogPreviewTexture_ != nullptr) {
						SDL_DestroyTexture(fileDialogPreviewTexture_);
					}
					fileDialogPreviewTexture_ = previewTexture;
					fileDialogPreviewWidth_ = result.width;
					fileDialogPreviewHeight_ = result.height;
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
			DrawText(ClipText(filename, previewRect.w - 16), previewRect.x + 8,
				previewRect.y + previewRect.h - 19, kUiTextScale, 165, 175, 185);
		}
	}

	void RequestFileDialogDirectorySummaries() {
		++fileDialogSummaryGeneration_;
		fileDialogDirectorySummaries_.clear();
		std::vector<fs::path> directories;
		if (!fileDialogSave_ && !fileDialogParameterRestore_) {
			for (const FileDialogEntry& entry : fileDialogModel_.AllEntries()) {
				if (entry.directory && !entry.parent) directories.push_back(entry.path);
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

	void RefreshFileDialog() {
		InvalidateFileDialogPreview();
		std::vector<FileDialogEntry> entries;
		std::error_code error;
		const fs::path parent = fileDialogDirectory_.parent_path();
		if (!parent.empty() && parent != fileDialogDirectory_) {
			entries.push_back(FileDialogEntry{parent, true, true});
		}

		for (const fs::directory_entry& entry : fs::directory_iterator(fileDialogDirectory_, error)) {
			if (error) break;
			std::error_code statusError;
			const bool directory = entry.is_directory(statusError);
			if (statusError || (!directory && (!entry.is_regular_file(statusError) ||
				(!(fileDialogParameterBackup_ || fileDialogParameterRestore_) &&
					!jpegview_linux::IsSupportedImagePath(entry.path()))))) {
				continue;
			}
			std::error_code modificationError;
			const fs::file_time_type modificationTime = entry.last_write_time(modificationError);
			entries.push_back(FileDialogEntry{AbsoluteNormalized(entry.path()), directory, false,
				modificationError ? fs::file_time_type{} : modificationTime});
		}

		fileDialogModel_.SetEntries(std::move(entries));
		RequestFileDialogDirectorySummaries();
	}

	void OpenFileDialog() {
		std::error_code error;
		fs::path directory = fs::current_path(error);
		if (!fileList_.Empty()) {
			const fs::path currentDirectory = fileList_.Current().parent_path();
			if (!currentDirectory.empty()) directory = currentDirectory;
		}
		if (error || directory.empty()) directory = fs::path(".");
		fileDialogDirectory_ = AbsoluteNormalized(directory);
		fileDialogSave_ = false;
		fileDialogParameterBackup_ = false;
		fileDialogParameterRestore_ = false;
		fileDialogSaveFullSize_ = true;
		PositionFileDialogGeometry();
		fileDialogFilename_.clear();
		fileDialogModel_.Begin(false);
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

	void OpenSaveFileDialog(bool fullSize) {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
		fs::path directory = fileList_.Current().parent_path();
		if (directory.empty()) directory = fs::current_path();
		fileDialogDirectory_ = AbsoluteNormalized(directory);
		fileDialogSave_ = true;
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
		return entry.directory ? std::string("[Dir] ") + entry.path.filename().string() : entry.path.filename().string();
	}

	int FileDialogItemAt(int x, int y) const {
		const SDL_Rect listRect = FileDialogListRect();
		if (!PointInRect(x, y, listRect)) return -1;
		const int row = (y - listRect.y) / 26;
		const int item = fileDialogModel_.Scroll() + row;
		return item >= 0 && item < static_cast<int>(fileDialogModel_.Entries().size()) ? item : -1;
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
		if (fileDialogSave_ && fileDialogOverwriteConfirmed_ && !fileDialogFilename_.empty()) {
			SaveImageFromDialog();
			return;
		}
		if (fileDialogSave_ && fileDialogModel_.SelectedIndex() < 0) {
			SaveImageFromDialog();
			return;
		}
		const FileDialogEntry* selected = fileDialogModel_.SelectedEntry();
		if (selected == nullptr) return;
		const FileDialogEntry entry = *selected;
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
		}
	}

	void HandleFileDialogEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN: {
			const bool repeatableSelectionKey = event.key.keysym.sym == SDLK_UP ||
				event.key.keysym.sym == SDLK_DOWN || event.key.keysym.sym == SDLK_PAGEUP ||
				event.key.keysym.sym == SDLK_PAGEDOWN || event.key.keysym.sym == SDLK_HOME ||
				event.key.keysym.sym == SDLK_END;
			if (event.key.repeat != 0 && !repeatableSelectionKey) break;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseFileDialog();
			} else if (event.key.keysym.sym == SDLK_UP) {
				fileDialogModel_.MoveSelection(-1, FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_DOWN) {
				fileDialogModel_.MoveSelection(1, FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_PAGEUP) {
				fileDialogModel_.MoveSelectionByPage(-1, FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_PAGEDOWN) {
				fileDialogModel_.MoveSelectionByPage(1, FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_HOME) {
				fileDialogModel_.SelectFirst(FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_END) {
				fileDialogModel_.SelectLast(FileDialogVisibleRows());
			} else if (event.key.keysym.sym == SDLK_RETURN) {
				const bool ctrl = (event.key.keysym.mod & 0x00C0u) != 0;
				ActivateFileDialogSelection(ctrl);
			} else if (event.key.keysym.sym == SDLK_BACKSPACE) {
				if (fileDialogSave_ && fileDialogModel_.SelectedIndex() < 0 &&
					jpegview_linux::EraseLastUtf8CodePoint(fileDialogFilename_)) {
					fileDialogMessage_.clear();
					fileDialogOverwriteConfirmed_ = false;
					break;
				}
				if (!fileDialogSave_ && fileDialogModel_.BackspaceFilter()) {
					break;
				}
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
				fileDialogModel_.ClearSelection();
				fileDialogMessage_.clear();
				fileDialogOverwriteConfirmed_ = false;
			} else {
				fileDialogModel_.AppendFilter(event.text.text);
			}
			break;
		case SDL_MOUSEWHEEL: {
			SDL_GetMouseState(&lastMouseX_, &lastMouseY_);
			const SDL_Rect listRect = FileDialogListRect();
			if (!PointInRect(lastMouseX_, lastMouseY_, listRect)) break;
			int wheelTicks = std::clamp(event.wheel.y, -100, 100);
			if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) wheelTicks = -wheelTicks;
			if (wheelTicks == 0) break;
			fileDialogModel_.ScrollBy(-wheelTicks * 3, FileDialogVisibleRows());
			const int item = FileDialogItemAt(lastMouseX_, lastMouseY_);
			if (item >= 0) fileDialogModel_.Select(item, FileDialogVisibleRows());
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
				fileDialogModel_.Select(item, FileDialogVisibleRows());
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
			const int item = FileDialogItemAt(event.button.x, event.button.y);
			const bool inputClicked = PointInRect(event.button.x, event.button.y, FileDialogInputRect());
			const bool sortClicked = FileDialogCanSort() &&
				PointInRect(event.button.x, event.button.y, FileDialogSortRect());
			const bool previewClicked = FileDialogHasPreviewColumn() &&
				PointInRect(event.button.x, event.button.y, FileDialogPreviewRect());
			if (event.button.button == SDL_BUTTON_RIGHT ||
				(event.button.button == SDL_BUTTON_LEFT && item < 0 && !inputClicked && !sortClicked &&
					!previewClicked)) {
				CloseFileDialog();
			} else if (event.button.button == SDL_BUTTON_LEFT && sortClicked) {
				fileDialogModel_.ToggleSortMode(FileDialogVisibleRows());
			} else if (event.button.button == SDL_BUTTON_LEFT && inputClicked) {
				if (fileDialogSave_) fileDialogModel_.ClearSelection();
			} else if (event.button.button == SDL_BUTTON_LEFT && previewClicked) {
				// The preview is informational; clicking it leaves the selection alone.
			} else if (event.button.button == SDL_BUTTON_LEFT) {
				fileDialogModel_.Select(item, FileDialogVisibleRows());
				const FileDialogEntry* selected = fileDialogModel_.SelectedEntry();
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
		DrawText(fileDialogDirectory_.string(), dialog.x + 18, dialog.y + 42, kUiTextScale, 170, 170, 170);
		DrawText(fileDialogSave_ ? "File name" :
			(fileDialogParameterRestore_ ? "Backup filter" : "Filter"),
			dialog.x + 18, dialog.y + 68,
			kUiTextScale, 190, 190, 190);
		if (FileDialogCanSort()) {
			const SDL_Rect sortRect = FileDialogSortRect();
			SDL_SetRenderDrawColor(renderer_, 36, 36, 36, 230);
			SDL_RenderFillRect(renderer_, &sortRect);
			DrawRect(sortRect, 100, 130, 165);
			const std::string sortLabel = fileDialogModel_.SortMode() == jpegview_linux::FileDialogSortMode::Name ?
				"Sort: Name" : "Sort: Mod.date";
			const int labelX = sortRect.x + std::max(6, (sortRect.w - TextWidth(sortLabel, kUiTextScale)) / 2);
			DrawText(sortLabel, labelX, sortRect.y + 3, kUiTextScale, 210, 220, 230);
		}
		SDL_Rect inputRect = FileDialogInputRect();
		SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 220);
		SDL_RenderFillRect(renderer_, &inputRect);
		DrawRect(inputRect, 100, 130, 165);
		const std::string& inputText = fileDialogSave_ ? fileDialogFilename_ : fileDialogModel_.Filter();
		DrawText(ClipInputText(inputText, inputRect.w - 20), inputRect.x + 10, inputRect.y + 6, kUiTextScale);

		const int listTop = FileDialogListTop();
		const int rows = FileDialogVisibleRows();
		const SDL_Rect listRect = FileDialogListRect();
		SDL_SetRenderDrawColor(renderer_, 25, 25, 25, 210);
		SDL_RenderFillRect(renderer_, &listRect);
		DrawRect(listRect, 75, 75, 75);
		for (int row = 0; row < rows; ++row) {
			const int item = fileDialogModel_.Scroll() + row;
			if (item >= static_cast<int>(fileDialogModel_.Entries().size())) break;
			const FileDialogEntry& entry = fileDialogModel_.Entries()[item];
			const int rowTop = listTop + row * 26;
			if (item == fileDialogModel_.SelectedIndex()) {
				SDL_SetRenderDrawColor(renderer_, 45, 82, 120, 205);
				SDL_Rect selection{listRect.x + 2, rowTop + 1, listRect.w - 4, 24};
				SDL_RenderFillRect(renderer_, &selection);
			}
			std::string summaryText;
			if (FileDialogCanSort() && entry.directory && !entry.parent) {
				const auto summary = fileDialogDirectorySummaries_.find(entry.path.string());
				summaryText = summary == fileDialogDirectorySummaries_.end() ? "Scanning..." :
					jpegview_linux::FormatDirectorySummary(summary->second);
				summaryText = ClipText(summaryText, std::max(1, listRect.w / 2 - 20));
			}
			const int summaryWidth = TextWidth(summaryText, kUiTextScale);
			const int summaryX = listRect.x + listRect.w - 10 - summaryWidth;
			const int labelWidth = summaryText.empty() ? listRect.w - 20 :
				std::max(1, summaryX - (listRect.x + 10) - 12);
			DrawText(ClipText(FileDialogEntryLabel(entry), labelWidth), listRect.x + 10, rowTop + 5, kUiTextScale,
				entry.directory ? 185 : 235, entry.directory ? 205 : 235, entry.directory ? 235 : 235);
			if (!summaryText.empty()) {
				DrawText(summaryText, summaryX, rowTop + 5, kUiTextScale, 155, 175, 195);
			}
		}
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
				"Type: Filter   Home/End: First/last   PgUp/PgDn: Page   Enter: Open   Ctrl+Return: Open folder   Backspace: Edit/parent   Esc: Cancel"),
			dialog.x + 18, dialog.y + dialog.h - 34, kUiTextScale, 170, 170, 170);
		const SDL_Rect resizeHandle = FileDialogResizeHandleRect();
		for (int offset = 5; offset <= 13; offset += 4) {
			DrawLine(resizeHandle.x + offset, resizeHandle.y + resizeHandle.h - 2,
				resizeHandle.x + resizeHandle.w - 2, resizeHandle.y + offset,
				135, 145, 155);
		}
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
		SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STATIC, raster.width, raster.height);
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
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return;
		int windowWidth = 0;
		SDL_GetWindowSize(window_, &windowWidth, nullptr);
		std::ostringstream text;
		text << '[' << fileList_.CurrentIndex() + 1 << '/' << fileList_.Size() << "] "
			<< InfoText(fileList_.Current().filename().string());
		std::string label = text.str();
		const jpegview_linux::OverlayLayout layout = jpegview_linux::FilenameOverlayLayout(
			TextWidth(label, kUiTextScale), windowWidth, kOverlayInset,
			kOverlayTextPadding, FilenameOverlayHeight());
		label = ClipText(label, layout.textWidth);
		RenderOverlayPaint(jpegview_linux::FilenameOverlayPaint(layout, std::move(label),
			TextLineHeight(), kOverlayTextPadding));
	}

	void RenderImageInfo() {
		if (!infoVisible_ || contextMenuOpen_ || fileDialogOpen_ ||
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

	jpegview_linux::ZoomNavigatorLayout CurrentZoomNavigatorLayout() const {
		const SDL_Rect area = ImageAreaRect();
		return jpegview_linux::CalculateZoomNavigatorLayout(image_.width, image_.height,
			area.x, area.y, area.w, area.h);
	}

	bool IsZoomNavigatorVisibleAt(int mouseX, int mouseY) const {
		if (!showZoomNavigator_ || image_.width <= 0 || image_.height <= 0 ||
			cropSelection_.HasSelection() || cropMouseDragging_ || pictureLevelsPanelOpen_ ||
			unsharpDialogOpen_ || contextMenuOpen_ || fileDialogOpen_ || confirmationOpen_ ||
			aboutOpen_ || helpOpen_ || resizeDialog_.IsOpen() || cropSizeDialog_.IsOpen() ||
			batchCopyDialog_.IsOpen()) return false;
		const SDL_Rect area = ImageAreaRect();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			image_.width, image_.height, area.w, area.h);
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
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			image_.width, image_.height, area.w, area.h);
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
		const SDL_Rect area = ImageAreaRect();
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			image_.width, image_.height, area.w, area.h);
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

	void RenderZoomNavigator(SDL_Texture* imageTexture) {
		if (imageTexture == nullptr || !IsZoomNavigatorVisibleAt(lastMouseX_, lastMouseY_)) return;
		const SDL_Rect area = ImageAreaRect();
		const jpegview_linux::ZoomNavigatorLayout layout = CurrentZoomNavigatorLayout();
		const SDL_Rect imageRect{layout.image.x, layout.image.y,
			layout.image.width, layout.image.height};
		const SDL_Rect frame{imageRect.x - 2, imageRect.y - 2,
			imageRect.w + 4, imageRect.h + 4};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 230);
		SDL_RenderFillRect(renderer_, &frame);
		SDL_RenderCopy(renderer_, imageTexture, nullptr, &imageRect);
		DrawRect({frame.x, frame.y, frame.w, frame.h}, 245, 245, 245, 255);

		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			image_.width, image_.height, area.w, area.h);
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
		const jpegview_linux::ViewportRect destination = viewport_.Destination(
			image_.width, image_.height, imageArea.w, imageArea.h);
		return {imageArea.x + destination.x, imageArea.y + destination.y,
			destination.width, destination.height};
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
		thumbnailPanelResizeChanged_ = false;
		thumbnailResizeOffset_ = ThumbnailPanelRect().w - x;
		SDL_CaptureMouse(SDL_TRUE);
		UpdateThumbnailPanelCursor(x, y);
		return true;
	}

	void ResizeThumbnailPanel(int mouseX) {
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
		}
	}

	void EndThumbnailPanelResize(int mouseX, int mouseY) {
		if (!thumbnailPanelResizing_) return;
		thumbnailPanelResizing_ = false;
		SDL_CaptureMouse(SDL_FALSE);
		if (thumbnailPanelResizeChanged_) {
			ClearThumbnailCache();
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
		const std::vector<jpegview_linux::ThumbnailSlot> slots = jpegview_linux::ThumbnailPanelSlots(
			fileList_.Size(), fileList_.CurrentIndex(), panel.h, rowHeight, fileList_.MarkedIndex());
		for (const jpegview_linux::ThumbnailSlot& slot : slots) {
			const SDL_Rect row{panel.x, slot.y, panel.w, rowHeight};
			if (slot.current) {
				SDL_SetRenderDrawColor(renderer_, 32, 58, 82, 255);
				SDL_RenderFillRect(renderer_, &row);
			}
			const std::string key = fileList_.Files()[slot.fileIndex].string();
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
				SDL_RenderCopy(renderer_, cached->second.texture, nullptr, &imageRect);
				if (!slot.current) {
					SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 125);
					SDL_RenderFillRect(renderer_, &imageRect);
				} else {
					DrawRect(imageRect, 225, 225, 225);
				}
			} else {
				const std::string position = std::to_string(slot.fileIndex + 1);
				DrawText(position, panel.x + (panel.w - TextWidth(position, kUiTextScale)) / 2,
					row.y + (row.h - TextLineHeight()) / 2, kUiTextScale,
					slot.current ? 215 : 95, slot.current ? 215 : 95, slot.current ? 215 : 95);
			}
			DrawLine(panel.x, row.y + row.h - 1, std::max(panel.x, panel.x + panel.w - 2),
				row.y + row.h - 1, 48, 48, 48);
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
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return false;
		const jpegview_linux::InformationOverlayPaintPlan paint = BuildImageInfoPaintPlan();
		if (!jpegview_linux::Contains(paint.spectrumButton, x, y)) return false;
		showHistogram_ = !showHistogram_;
		SaveSettings();
		return true;
	}

	void RenderControls() {
		if (!navigationPanelEnabled_ || !controlsVisible_ || contextMenuOpen_ || fileDialogOpen_ ||
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
			viewport_.IsFitToWindow(), fullscreen_, fileList_.GetSorting(), selectionModeEnabled_);
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
		const int width = std::min(760, std::max(360, windowWidth - 40));
		const int height = 136;
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
		DrawText(filename, panel.x + 18, panel.y + 68, kUiTextScale, 220, 220, 220);
		DrawText("ENTER or SPACE: YES     ESC: CANCEL", panel.x + 18, panel.y + 104, kUiTextScale, 180, 180, 180);
	}

	void RenderAbout() {
		if (!aboutOpen_) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(620, std::max(360, windowWidth - 40));
		const int height = 196;
		const SDL_Rect panel{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 220);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 160, 190, 225);
		DrawText("JPEGVIEW LINUX", panel.x + 18, panel.y + 16, kUiTextScale, 255, 255, 255);
		DrawText("NATIVE SDL2 VIEWER", panel.x + 18, panel.y + 48, kUiTextScale, 210, 225, 250);
		DrawText("PORT OF JPEGVIEW 1.3.46", panel.x + 18, panel.y + 80, kUiTextScale, 210, 225, 250);
		DrawText("FOLDER NAVIGATION AND IMAGE VIEWING", panel.x + 18, panel.y + 112, kUiTextScale, 185, 205, 220);
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
		static const std::array<const char*, 10> lines = {
			"Navigate: arrows/wheel; Home/End; Ctrl+M mark; Ctrl+Left/Right toggle; Alt+arrows siblings",
			"Zoom and pan: Ctrl+wheel or Ctrl+Up/Down; drag to pan; Shift+Arrow pans at actual size",
			"Navigator: hover upper-right when magnified; click or drag its map to reposition",
			"Scale: Space fit/actual; Return fit; Ctrl+Return fill with crop; +/- zoom",
			"Panels: F2 info; Shift+N filename; Ctrl+N nav; Ctrl+T thumbs; Ctrl+E crop mode",
			"Files: Ctrl+O open; Ctrl+S save processed; Ctrl+Shift+S save displayed size",
			"Clipboard: Ctrl+C copy image; Ctrl+Shift+C copy path; Ctrl+V paste PNG",
			"Adjustments: Up/Down rotate; F5 auto correction; F6 local density; Ctrl+Shift+R resize",
			"Window: F11 fullscreen; Shift+F11 title bar; Shift+F12 always on top",
			"Dialogs: type to filter; arrows/pages select; wheel scrolls; drag corner or preview divider to resize"};
		for (std::size_t index = 0; index < lines.size(); ++index) {
			DrawText(ClipText(lines[index], width - 36), panel.x + 18,
				panel.y + 48 + static_cast<int>(index) * 27, kUiTextScale, 220, 225, 235);
		}
		DrawText("Right-click or Menu key: context menu     F1 / Esc / click: close help",
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
			playback_.NotifyInteraction(SDL_GetTicks());
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
				case SDL_KEYDOWN:
					if (event.key.keysym.sym == SDLK_UP) {
						MoveContextMenuSelection(-1);
					} else if (event.key.keysym.sym == SDLK_DOWN) {
						MoveContextMenuSelection(1);
					} else if (event.key.keysym.sym == SDLK_LEFT) {
						MoveContextMenuSelectionAcrossColumns(-1);
					} else if (event.key.keysym.sym == SDLK_RIGHT) {
						MoveContextMenuSelectionAcrossColumns(1);
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
				} else if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
					event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					if (viewport_.IsFitToWindow()) {
						FitToWindow(viewport_.FillWithCrop(), viewport_.NoEnlarge());
					}
				}
				break;
			case SDL_KEYDOWN:
			{
				const Uint16 modifiers = event.key.keysym.mod;
				const bool plainNavigationKey =
					(modifiers & 0x03C3u) == 0 &&
					(event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_RIGHT);
				const bool shiftPanKey = (modifiers & 0x03C0u) == 0 &&
					(modifiers & 0x0003u) != 0 &&
					(event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_RIGHT ||
						event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_DOWN);
				// SDL marks OS key-repeat events instead of generating a fresh
				// physical key press. Pan on repeats, but let held-navigation poll
				// the actual key state after each rendered image to avoid a backlog.
				if (event.key.repeat != 0 && !plainNavigationKey && !shiftPanKey) break;
				if (plainNavigationKey && event.key.repeat != 0) {
					const int direction = event.key.keysym.sym == SDLK_RIGHT ? 1 : -1;
					heldNavigation_.KeyDown(direction, event.key.keysym.scancode, true);
					break;
				}
				if (plainNavigationKey) {
					const int direction = event.key.keysym.sym == SDLK_RIGHT ? 1 : -1;
					heldNavigation_.KeyDown(direction, event.key.keysym.scancode, false);
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
				const int command = jpegview_linux::CommandForKey(event.key,
					playback_.Mode() != PlaybackMode::None || playback_.AnimationPlaying());
				if (command != 0) {
					ExecuteCommand(command);
				}
				if (quitRequested_) running = false;
				if (plainNavigationKey || shiftPanKey) return;
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
					const jpegview_linux::ViewportRect destination = viewport_.Destination(
						image_.width, image_.height, imageArea.w, imageArea.h);
					if (destination.width <= imageArea.w && destination.height <= imageArea.h) break;
					dragging_ = true;
					lastMouseX_ = event.button.x;
					lastMouseY_ = event.button.y;
					imageCenterX_ = event.button.x;
					imageCenterY_ = event.button.y;
				} else if (event.button.button == SDL_BUTTON_RIGHT) {
					if (cropSelection_.HasSelection()) OpenCropContextMenu();
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
					SetTitle();
				}
				lastMouseX_ = event.motion.x;
				lastMouseY_ = event.motion.y;
				break;
			case SDL_MOUSEWHEEL:
				if ((SDL_GetModState() & 0x00C0u) != 0) {
					if (event.wheel.y > 0) {
						ZoomAt(1.2, lastMouseX_, lastMouseY_);
					} else if (event.wheel.y < 0) {
						ZoomAt(1.0 / 1.2, lastMouseX_, lastMouseY_);
					}
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
		const SDL_Rect imageArea = ImageAreaRect();
		imageCenterX_ = imageArea.x + imageArea.w / 2;
		imageCenterY_ = imageArea.y + imageArea.h / 2;
		const jpegview_linux::ViewportRect viewportRect = viewport_.Destination(
			image_.width, image_.height, imageArea.w, imageArea.h);
		const int renderWidth = viewportRect.width;
		const int renderHeight = viewportRect.height;
		SDL_Rect destination{viewportRect.x + imageArea.x, viewportRect.y + imageArea.y,
			viewportRect.width, viewportRect.height};
		SDL_Texture* renderTexture = DisplayTextureFor(renderWidth, renderHeight);
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		SDL_SetRenderDrawColor(renderer_, 18, 18, 18, 255);
		SDL_RenderClear(renderer_);
		// Explicitly repaint the image viewport so newly exposed pillarbox and
		// letterbox margins are overwritten when navigation changes image size.
		SDL_RenderFillRect(renderer_, &imageArea);
		SDL_RenderSetClipRect(renderer_, &imageArea);
		RenderImageTransition(destination, imageArea, renderTexture);
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		RenderCropSelection();
		RenderZoomNavigator(renderTexture);
		SDL_RenderSetClipRect(renderer_, nullptr);
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		RenderThumbnailPanel();
		RenderFileName();
		RenderImageInfo();
		RenderControls();
		RenderPictureLevels();
		RenderUnsharpMaskDialog();
		RenderContextMenu();
		RenderFileDialog();
		RenderBatchCopy();
		RenderResizeDialog();
		RenderFixedCropSizeDialog();
		RenderConfirmation();
		RenderAbout();
		RenderHelp();
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		SDL_RenderPresent(renderer_);
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
	std::vector<std::string> startupInputs_;
	std::shared_ptr<jpegview_linux::SharedCacheBudget> cacheBudget_ =
		std::make_shared<jpegview_linux::SharedCacheBudget>(
			jpegview_linux::CacheBytesFromMiB(jpegview_linux::kDefaultCacheSizeMiB));
	// Declaration order is intentional: the decoder (destroyed first) may
	// schedule final display work while joining its worker during teardown.
	jpegview_linux::DisplayImageCache displayImageCache_{
		std::numeric_limits<std::size_t>::max(), 0, {}, cacheBudget_};
	jpegview_linux::DecodedImageCache imageCache_{
		std::numeric_limits<std::size_t>::max(), {}, cacheBudget_, 2};
	std::size_t cacheSizeMiB_ = jpegview_linux::kDefaultCacheSizeMiB;
	double initialSlideshowSeconds_ = 0.0;
	jpegview_linux::PlaybackScheduler playback_;
	std::vector<Image> animationFrames_;
	int transitionEffect_ = IDM_EFFECT_NONE;
	Uint32 transitionDurationMs_ = 500;
	Uint32 transitionStartTick_ = 0;
	bool startFullscreen_ = false;
	Image image_;
	Image correctionBase_;
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
	std::unordered_map<std::string, JpegDimensionCacheEntry> jpegDimensionCache_;
	std::size_t displayTextureCacheBytes_ = 0;
	std::uint64_t displayTextureUseCounter_ = 0;
	jpegview_linux::DecodedImageCache::ImagePtr currentDecoded_;
	std::size_t currentAnimationFrame_ = 0;
	std::optional<jpegview_linux::DisplayImageRequest> currentDisplayRequest_;
	bool currentPixelsMaterialized_ = false;
	bool materializedAutoContrast_ = false;
	Image transitionImage_;
	SDL_Texture* transitionTexture_ = nullptr;
	jpegview_linux::Viewport viewport_;
	jpegview_linux::CropSelectionModel cropSelection_;
	bool selectionModeEnabled_ = false;
	bool cropMouseDragging_ = false;
	bool zoomNavigatorDragging_ = false;
	Uint32 zoomNavigatorVisibleUntil_ = 0;
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
	int thumbnailPanelWidth_ = jpegview_linux::kDefaultThumbnailPanelWidth;
	bool thumbnailPanelResizing_ = false;
	bool thumbnailPanelResizeChanged_ = false;
	int thumbnailResizeOffset_ = 0;
	SDL_Cursor* thumbnailResizeCursor_ = nullptr;
	SDL_Cursor* fileDialogResizeCursor_ = nullptr;
	SDL_Cursor* cropCrosshairCursor_ = nullptr;
	SDL_Cursor* cropMoveCursor_ = nullptr;
	SDL_Cursor* cropHorizontalCursor_ = nullptr;
	SDL_Cursor* cropVerticalCursor_ = nullptr;
	SDL_Cursor* cropDiagonalDownCursor_ = nullptr;
	SDL_Cursor* cropDiagonalUpCursor_ = nullptr;
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
	int contextMenuX_ = 0;
	int contextMenuY_ = 0;
	int menuSelected_ = -1;
	std::vector<MenuItem> contextMenuItems_;
	std::unordered_map<std::string, ThumbnailCacheEntry> thumbnailCache_;
	jpegview_linux::ThumbnailCacheScheduler thumbnailScheduler_;
	jpegview_linux::ThumbnailPreparationWorker thumbnailPreparation_;
	std::unordered_map<std::string, TextTextureCacheEntry> textTextureCache_;
	std::uint64_t textTextureUseCounter_ = 0;
	std::vector<jpegview_linux::OpenWithApplication> openWithApplications_;
	bool imageModified_ = false;
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
	int fileDialogDragStartX_ = 0;
	int fileDialogDragStartY_ = 0;
	int fileDialogDragStartPreviewWidth_ = 0;
	SDL_Rect fileDialogDragStartRect_{};
	bool fileDialogSaveFullSize_ = true;
	bool fileDialogOverwriteConfirmed_ = false;
	bool fileDialogLosslessCrop_ = false;
	jpegview_linux::SelectionRect fileDialogLosslessCropRect_;
	fs::path fileDialogDirectory_;
	std::string fileDialogFilename_;
	std::string fileDialogMessage_;
	jpegview_linux::FileDialogModel fileDialogModel_;
	jpegview_linux::DirectorySummaryLoader fileDialogSummaryLoader_;
	jpegview_linux::FileDialogPreviewLoader fileDialogPreviewLoader_;
	std::unordered_map<std::string, jpegview_linux::DirectorySummary> fileDialogDirectorySummaries_;
	std::uint64_t fileDialogSummaryGeneration_ = 0;
	SDL_Texture* fileDialogPreviewTexture_ = nullptr;
	int fileDialogPreviewWidth_ = 0;
	int fileDialogPreviewHeight_ = 0;
	std::uint64_t fileDialogPreviewGeneration_ = 0;
	std::string fileDialogPreviewRequestKey_;
	std::string fileDialogPreviewContentKey_;
	fs::path fileDialogPreviewSource_;
	std::string fileDialogPreviewMessage_;
	std::string copyRenamePattern_;
	jpegview_linux::BatchCopyDialogController batchCopyDialog_;
	jpegview_linux::ResizeDialogController resizeDialog_;
	jpegview_linux::CropSizeDialogController cropSizeDialog_;
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
		<< "  --help             Show this help\n\n"
		<< "Controls: Right/Left navigate, Up/Down rotate, mouse wheel up/down navigates previous/next, Ctrl+mouse wheel zooms, left-drag pans, drop files to open,\n"
		<< "          Space toggles fit/actual, Enter fits, 0 fits, 1-9 start a slideshow, F11/F fullscreen,\n"
		<< "          F7/F8/F9 select folder/recursive/sibling navigation, Alt+Left/Right open the first image in adjacent sibling folders,\n"
		<< "          Ctrl+M marks an image; Ctrl+Left/Right toggles between it and the paired image,\n"
		<< "          N/M/C/Z select display order,\n"
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
