#include "sdl_abi.h"
#include "file_list.h"
#include "exif_reader.h"
#include "clipboard.h"
#include "image_writer.h"
#include "image_decoder.h"
#include "image_cache.h"
#include "display_image_cache.h"
#include "image.h"
#include "settings.h"
#include "sort_mode.h"
#include "desktop_applications.h"
#include "external_commands.h"
#include "batch_copy.h"
#include "image_formats.h"
#include "input_commands.h"
#include "viewport.h"
#include "resize_model.h"
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

// Keep Linux command dispatch aligned with the original Windows application.
// resource.h is deliberately platform-neutral: it contains the command IDs
// shared by JPEGView.rc, CMainDlg::ExecuteCommand, and KeyMap.txt.default.
#include "../../src/JPEGView/resource.h"

#include <algorithm>
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
constexpr int kNavigationPanelHoverHeight = 64;
constexpr int kOverlayInset = 4;
constexpr int kOverlayTextPadding = 6;
constexpr int kThumbnailVerticalMargin = 1;
constexpr int kThumbnailResizeHandleHalfWidth = 3;
constexpr int kFileDialogPreviewMaximumWidth = 256;
constexpr int kFileDialogPreviewMaximumHeight = 256;
constexpr std::size_t kDecodedImagePrefetchCount = 32;
constexpr std::size_t kDisplayTextureUploadsPerTick = 1;
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
	return std::max(18, TextLineHeight() + 8);
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
	Viewer(jpegview_linux::FileList fileList, double slideshowSeconds, bool startFullscreen)
		: fileList_(std::move(fileList)), initialSlideshowSeconds_(slideshowSeconds),
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
		window_ = SDL_CreateWindow("JPEGView Linux", 0x2FFF0000, 0x2FFF0000,
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
		if (!LoadCurrent()) {
			Cleanup();
			return 1;
		}
		SDL_ShowWindow(window_);
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
			if (heldNavigation_.Scancode() < 0 && !displayImageCache_.HasPendingWork()) {
				TickThumbnailPreload();
			}
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
		bool autoContrast = false;
	};

	struct DisplayPrefetchBatch {
		std::mutex mutex;
		std::vector<jpegview_linux::DisplayImageRequest> requests;
		std::unordered_set<std::string> retainedTextureKeys;
		std::unordered_map<std::string, std::size_t> priorityByFilename;
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
		if (thumbnailResizeCursor_ != nullptr) {
			SDL_SetCursor(SDL_GetDefaultCursor());
			SDL_FreeCursor(thumbnailResizeCursor_);
			thumbnailResizeCursor_ = nullptr;
		}
		SDL_Quit();
	}

	void LoadSettings() {
		const fs::path settingsPath = jpegview_linux::ViewerSettingsPath();
		if (settingsPath.empty()) return;

		jpegview_linux::ViewerSettings settings;
		if (!jpegview_linux::LoadViewerSettings(settingsPath, settings)) return;
		copyRenamePattern_ = settings.copyRenamePattern;
		jpegview_linux::FileList::SortMode sortMode;
		if (jpegview_linux::ParseSortMode(settings.sortMode, sortMode)) {
			fileList_.SetSorting(sortMode, settings.sortAscending);
		}

		maximized_ = settings.maximized;
		navigationPanelEnabled_ = settings.navigationPanelEnabled;
		navigationPanelAutoReveal_ = settings.navigationPanelAutoReveal;
		thumbnailPanelVisible_ = settings.thumbnailPanelVisible;
		thumbnailPanelWidth_ = settings.thumbnailPanelWidth;
		infoVisible_ = settings.infoVisible;
		showHistogram_ = settings.showHistogram;
		showFileName_ = settings.showFilename;
		autoContrastEnabled_ = settings.autoContrast;
		cacheSizeMiB_ = settings.cacheSizeMiB;
		cacheBudget_->SetCapacity(jpegview_linux::CacheBytesFromMiB(cacheSizeMiB_));
		viewport_.LoadScaleMode(settings.scaleMode, settings.manualZoomSet, settings.manualZoom);
	}

	void SaveSettings() const {
		const fs::path settingsPath = jpegview_linux::ViewerSettingsPath();
		if (settingsPath.empty()) return;

		jpegview_linux::ViewerSettings settings;
		settings.scaleMode = viewport_.NavigationScaleMode();
		settings.sortMode = jpegview_linux::SortModeSettingName(fileList_.GetSorting());
		settings.sortAscending = fileList_.IsSortedAscending();
		settings.manualZoom = viewport_.NavigationZoom();
		settings.maximized = maximized_;
		settings.navigationPanelEnabled = navigationPanelEnabled_;
		settings.navigationPanelAutoReveal = navigationPanelAutoReveal_;
		settings.thumbnailPanelVisible = thumbnailPanelVisible_;
		settings.thumbnailPanelWidth = thumbnailPanelWidth_;
		settings.infoVisible = infoVisible_;
		settings.showHistogram = showHistogram_;
		settings.showFilename = showFileName_;
		settings.autoContrast = autoContrastEnabled_;
		settings.cacheSizeMiB = cacheSizeMiB_;
		settings.copyRenamePattern = copyRenamePattern_;
		jpegview_linux::SaveViewerSettings(settingsPath, settings);
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
		if (currentPixelsMaterialized_) return true;
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
		if (autoContrastEnabled_ && !image_.AutoContrast()) {
			SetTitle(fileList_.Current().filename().string() + " — automatic correction failed");
			return false;
		}
		currentPixelsMaterialized_ = true;
		return true;
	}

	bool LoadCurrent(int prefetchDirection = 0) {
		if (fileList_.Empty()) {
			return false;
		}
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
					destination.height, autoContrastEnabled_);
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
		batch->context = {viewport_.NavigationSnapshot(), imageArea.w, imageArea.h,
			autoContrastEnabled_};
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
					target.width, target.height, batch->context.autoContrast, position + 1);
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
				if (priority == batch->priorityByFilename.end()) return;
				jpegview_linux::DisplayImageRequest request =
					jpegview_linux::MakeDisplayImageRequest(filename, decoded, 0,
						target.width, target.height, batch->context.autoContrast,
						priority->second);
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
			currentDisplayRequest_->autoContrast != autoContrastEnabled_) {
			if (currentDecoded_) {
				currentDisplayRequest_ = jpegview_linux::MakeDisplayImageRequest(
					fileList_.Current(), currentDecoded_, currentAnimationFrame_, width, height,
					autoContrastEnabled_);
			} else {
				currentDisplayRequest_ = jpegview_linux::MakeJpegDisplayImageRequest(
					fileList_.Current(), image_.originalWidth, image_.originalHeight, width, height,
					autoContrastEnabled_);
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

	void TickThumbnailPreload() {
		if (!thumbnailPanelVisible_) return;
		const Uint32 now = SDL_GetTicks();
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
		const jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.Snapshot();
		if (!TransformImage(image_, command) ||
			(correctionBaseValid_ && !TransformImage(correctionBase_, command)) || !UpdateTexture()) {
			SetTitle("Image transform failed");
			return;
		}
		imageModified_ = true;
		RestoreScaleMode(viewportSnapshot);
	}

	void RebuildAutoContrastImage(const jpegview_linux::ViewportSnapshot& viewportSnapshot) {
		if (!correctionBaseValid_) return;
		image_ = correctionBase_;
		if (autoContrastEnabled_) image_.AutoContrast();
		if (!UpdateTexture()) {
			SetTitle("Automatic correction failed: could not update the display texture");
			return;
		}
		RestoreScaleMode(viewportSnapshot);
		SetTitle();
	}

	void ToggleAutoContrast() {
		if (!MaterializeCurrentPixels() || !correctionBaseValid_) return;
		const jpegview_linux::ViewportSnapshot viewportSnapshot = viewport_.Snapshot();
		autoContrastEnabled_ = !autoContrastEnabled_;
		RebuildAutoContrastImage(viewportSnapshot);
		currentDisplayRequest_.reset();
		PrepareImagePrefetch();
		SaveSettings();
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
		ClearFileDialogPreview();
		++fileDialogSummaryGeneration_;
		fileDialogSummaryLoader_.Request({}, fileDialogSummaryGeneration_);
		fileDialogOpen_ = false;
		fileDialogSave_ = false;
		fileDialogFilename_.clear();
		fileDialogModel_.Clear();
		fileDialogDirectorySummaries_.clear();
	}

	void SaveImageFromDialog() {
		if (!fileDialogSave_ || fileDialogFilename_.empty()) return;
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
				confirmationOpen_ = false;
			} else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
				const int command = confirmationCommand_;
				confirmationOpen_ = false;
				if (command == IDM_MOVE_TO_RECYCLE_BIN || command == IDM_MOVE_TO_RECYCLE_BIN_CONFIRM ||
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
		viewport_.Pan(deltaX, deltaY);
		playback_.NotifyInteraction(SDL_GetTicks());
		SetTitle();
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
		if (contextMenuOpen_ || fileDialogOpen_ || confirmationOpen_ || aboutOpen_ ||
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen() ||
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
		if (autoContrastEnabled_ && !displayed.AutoContrast()) return false;
		image_ = std::move(displayed);
		correctionBase_ = std::move(base);
		correctionBaseValid_ = true;
		imageModified_ = false;
		currentAnimationFrame_ = index;
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
			TextLineHeight(kUiTextScale));
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
		case IDM_FIRST:
			FirstImage();
			break;
		case IDM_PREV:
			PreviousImage();
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
		openWithApplications_ = jpegview_linux::DiscoverOpenWithApplications(extension);

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
		state.navigationMode = fileList_.GetNavigationMode();
		state.sortMode = fileList_.GetSorting();
		state.sortAscending = fileList_.IsSortedAscending();
		state.imageAvailable = image_.width > 0;
		state.losslessJpegAvailable = !clipboardMode_ && HasExecutable("jpegtran") &&
			(extension == ".jpg" || extension == ".jpeg" || extension == ".jpe");
		state.autoCorrectionEnabled = autoContrastEnabled_;
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
			contextMenuItems_, windowHeight - 24, ContextMenuRowHeight(), kContextMenuSeparatorHeight);
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
		const int height = contentHeight + 12;
		int x = contextMenuX_;
		int y = contextMenuY_;
		if (!contextMenuPositionLocked_) {
			if (x + width > windowWidth) x = windowWidth - width - 4;
			if (y + height > windowHeight) y = windowHeight - height - 4;
			x = std::max(4, x);
			y = std::max(4, y);
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
			int itemTop = menu.y + 6;
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
		contextMenuItems_ = ContextMenuItems(contextMenuAdvancedOptions_);
		contextMenuX_ = x;
		contextMenuY_ = y;
		RepositionContextMenuToFit();
		contextMenuOpen_ = true;
		menuSelected_ = -1;
	}

	void CloseContextMenu() {
		if (!contextMenuOpen_) return;
		contextMenuOpen_ = false;
		contextMenuAdvancedOptions_ = false;
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
		if (autoContrastEnabled_) image_.AutoContrast();
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

	SDL_Rect FileDialogRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(900, std::max(320, windowWidth - 40));
		const int height = std::min(650, std::max(260, windowHeight - 40));
		return SDL_Rect{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
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
		return !fileDialogSave_ && FileDialogRect().w >= 560;
	}

	SDL_Rect FileDialogListRect() const {
		const SDL_Rect dialog = FileDialogRect();
		const int previewWidth = FileDialogHasPreviewColumn() ? FileDialogPreviewWidth() + 12 : 0;
		const int width = dialog.w - 24 - previewWidth;
		return SDL_Rect{dialog.x + 12, FileDialogListTop(), width,
			FileDialogVisibleRows() * 26};
	}

	int FileDialogPreviewWidth() const {
		return std::min(260, std::max(200, FileDialogRect().w / 3));
	}

	SDL_Rect FileDialogPreviewRect() const {
		const SDL_Rect dialog = FileDialogRect();
		const int width = FileDialogPreviewWidth();
		return SDL_Rect{dialog.x + dialog.w - 12 - width, FileDialogListTop(), width,
			FileDialogVisibleRows() * 26};
	}

	void ClearFileDialogPreview() {
		fileDialogPreviewLoader_.Clear();
		if (fileDialogPreviewTexture_ != nullptr) SDL_DestroyTexture(fileDialogPreviewTexture_);
		fileDialogPreviewTexture_ = nullptr;
		fileDialogPreviewRequestKey_.clear();
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

		const FileDialogEntry* selected = fileDialogModel_.SelectedEntry();
		std::string requestKey;
		if (selected != nullptr) {
			requestKey = selected->path.string() + (selected->directory ? "\nD\n" : "\nF\n") +
				(fileDialogModel_.SortMode() == jpegview_linux::FileDialogSortMode::Name ? "N" : "M");
		}
		if (requestKey != fileDialogPreviewRequestKey_) {
			if (fileDialogPreviewTexture_ != nullptr) SDL_DestroyTexture(fileDialogPreviewTexture_);
			fileDialogPreviewTexture_ = nullptr;
			fileDialogPreviewSource_.clear();
			fileDialogPreviewMessage_.clear();
			fileDialogPreviewRequestKey_ = requestKey;
			if (selected == nullptr) {
				fileDialogPreviewLoader_.Clear();
				fileDialogPreviewGeneration_ = 0;
			} else {
				fileDialogPreviewGeneration_ = fileDialogPreviewLoader_.Request(selected->path,
					selected->directory, fileDialogModel_.SortMode(), kFileDialogPreviewMaximumWidth,
					kFileDialogPreviewMaximumHeight);
				fileDialogPreviewMessage_ = "Loading preview...";
			}
		}

		for (jpegview_linux::FileDialogPreviewResult& result : fileDialogPreviewLoader_.TakeReady()) {
			if (result.generation != fileDialogPreviewGeneration_) continue;
			fileDialogPreviewSource_ = result.source;
			fileDialogPreviewMessage_ = result.error;
			if (!result.bgra.empty()) {
				fileDialogPreviewWidth_ = result.width;
				fileDialogPreviewHeight_ = result.height;
				fileDialogPreviewTexture_ = CreateTexture(result.bgra, result.width, result.height);
				if (fileDialogPreviewTexture_ == nullptr) {
					fileDialogPreviewMessage_ = "Cannot create preview";
				} else {
					SDL_SetTextureBlendMode(fileDialogPreviewTexture_, SDL_BLENDMODE_BLEND);
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

		const int footerHeight = fileDialogPreviewSource_.empty() ? 0 : 24;
		const SDL_Rect imageRect{previewRect.x + 8, previewRect.y + 28,
			previewRect.w - 16, std::max(1, previewRect.h - 58 - footerHeight)};
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
		if (!fileDialogSave_) {
			for (const FileDialogEntry& entry : fileDialogModel_.AllEntries()) {
				if (entry.directory && !entry.parent) directories.push_back(entry.path);
			}
		}
		fileDialogSummaryLoader_.Request(directories, fileDialogSummaryGeneration_);
	}

	void TickFileDialogDirectorySummaries() {
		for (jpegview_linux::DirectorySummaryResult& result : fileDialogSummaryLoader_.TakeReady()) {
			if (!fileDialogOpen_ || fileDialogSave_ ||
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
				!jpegview_linux::IsSupportedImagePath(entry.path())))) {
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
		fileDialogSaveFullSize_ = true;
		fileDialogFilename_.clear();
		fileDialogModel_.Begin(false);
		fileDialogMessage_.clear();
		fileDialogOpen_ = true;
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
		fileDialogSaveFullSize_ = fullSize;
		fileDialogFilename_ = fileList_.Current().stem().string() + "_proc.jpg";
		fileDialogModel_.Begin(true);
		fileDialogMessage_.clear();
		fileDialogOverwriteConfirmed_ = false;
		fileDialogOpen_ = true;
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
		if (fileDialogSave_ && fileDialogModel_.SelectedIndex() < 0) {
			SaveImageFromDialog();
			return;
		}
		const FileDialogEntry* selected = fileDialogModel_.SelectedEntry();
		if (selected == nullptr) return;
		const FileDialogEntry entry = *selected;
		if (entry.directory) {
			if (openDirectoryImmediately && !fileDialogSave_) {
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
		case SDL_MOUSEMOTION: {
			const int item = FileDialogItemAt(event.motion.x, event.motion.y);
			if (item >= 0) {
				fileDialogModel_.Select(item, FileDialogVisibleRows());
			}
			break;
		}
		case SDL_MOUSEBUTTONDOWN: {
			const int item = FileDialogItemAt(event.button.x, event.button.y);
			const bool inputClicked = PointInRect(event.button.x, event.button.y, FileDialogInputRect());
			const bool sortClicked = !fileDialogSave_ &&
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
		DrawText(fileDialogSave_ ? "Save processed image" : "Open image",
			dialog.x + 18, dialog.y + 14, kUiTextScale);
		DrawText(fileDialogDirectory_.string(), dialog.x + 18, dialog.y + 42, kUiTextScale, 170, 170, 170);
		DrawText(fileDialogSave_ ? "File name" : "Filter", dialog.x + 18, dialog.y + 68,
			kUiTextScale, 190, 190, 190);
		if (!fileDialogSave_) {
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
			if (!fileDialogSave_ && entry.directory && !entry.parent) {
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
		if (FileDialogHasPreviewColumn()) RenderFileDialogPreview(FileDialogPreviewRect());
		if (!fileDialogMessage_.empty()) {
			DrawText(fileDialogMessage_, dialog.x + 18, dialog.y + dialog.h - 60, kUiTextScale, 235, 150, 120);
		}
		DrawText(fileDialogSave_ ? "Enter: Save   Backspace: Edit/parent   Esc: Cancel" :
			"Type: Filter   Home/End: First/last   PgUp/PgDn: Page   Enter: Open   Ctrl+Return: Open folder   Backspace: Edit/parent   Esc: Cancel",
			dialog.x + 18, dialog.y + dialog.h - 34, kUiTextScale, 170, 170, 170);
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
			fileList_.Size(), fileList_.CurrentIndex(), panel.h, rowHeight);
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
			batchCopyDialog_.IsOpen() || resizeDialog_.IsOpen()) return;
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
			viewport_.IsFitToWindow(), fullscreen_, fileList_.GetSorting());
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
		const std::string filename = fileList_.Empty() ? std::string() :
			ClipText(InfoText(fileList_.Current().filename().string()), width - 36);
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

	void RenderContextMenu() {
		if (!contextMenuOpen_) return;
		const SDL_Rect menu = ContextMenuRect();
		const std::vector<ContextMenuColumn> columns = ContextMenuColumns();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 220);
		SDL_RenderFillRect(renderer_, &menu);
		DrawRect(menu, 185, 185, 185);

		for (const ContextMenuColumn& column : columns) {
			const int columnX = menu.x + column.x;
			int itemTop = menu.y + 6;
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
				DrawLine(columnX + column.width, menu.y + 6, columnX + column.width,
					menu.y + menu.h - 6, 75, 75, 75);
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
					OpenContextMenu();
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
					dragging_ = true;
					lastMouseX_ = event.button.x;
					lastMouseY_ = event.button.y;
					imageCenterX_ = event.button.x;
					imageCenterY_ = event.button.y;
				} else if (event.button.button == SDL_BUTTON_RIGHT) {
					OpenContextMenu();
				}
				break;
			case SDL_MOUSEBUTTONUP:
				if (event.button.button == SDL_BUTTON_LEFT) {
					EndThumbnailPanelResize(event.button.x, event.button.y);
					dragging_ = false;
				}
				break;
			case SDL_MOUSEMOTION:
				UpdateThumbnailPanelCursor(event.motion.x, event.motion.y);
				if (thumbnailPanelResizing_) {
					ResizeThumbnailPanel(event.motion.x);
					lastMouseX_ = event.motion.x;
					lastMouseY_ = event.motion.y;
					break;
				}
				imageCenterX_ = event.motion.x;
				imageCenterY_ = event.motion.y;
				UpdateNavigationPanelVisibility(event.motion.x, event.motion.y);
				if (dragging_) {
					viewport_.Pan(event.motion.xrel, event.motion.yrel);
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
		SDL_RenderSetClipRect(renderer_, nullptr);
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		RenderThumbnailPanel();
		RenderFileName();
		RenderImageInfo();
		RenderControls();
		RenderContextMenu();
		RenderFileDialog();
		RenderBatchCopy();
		RenderResizeDialog();
		RenderConfirmation();
		RenderAbout();
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
		SDL_RenderPresent(renderer_);
	}

	void Render() {
		const bool needsCleanContextMenuFrame = contextMenuNeedsCleanFrame_;
		contextMenuNeedsCleanFrame_ = false;
		RenderFrame();
		if (needsCleanContextMenuFrame) RenderFrame();
	}

	jpegview_linux::FileList fileList_;
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
	Image transitionImage_;
	SDL_Texture* transitionTexture_ = nullptr;
	jpegview_linux::Viewport viewport_;
	bool fullscreen_ = false;
	bool maximized_ = false;
	bool borderless_ = false;
	bool alwaysOnTop_ = false;
	bool dragging_ = false;
	bool controlsVisible_ = true;
	bool navigationPanelEnabled_ = true;
	bool navigationPanelAutoReveal_ = true;
	bool thumbnailPanelVisible_ = false;
	int thumbnailPanelWidth_ = jpegview_linux::kDefaultThumbnailPanelWidth;
	bool thumbnailPanelResizing_ = false;
	bool thumbnailPanelResizeChanged_ = false;
	int thumbnailResizeOffset_ = 0;
	SDL_Cursor* thumbnailResizeCursor_ = nullptr;
	bool infoVisible_ = false;
	bool showHistogram_ = false;
	bool showFileName_ = false;
	jpegview_linux::HeldNavigationController heldNavigation_;
	bool confirmationOpen_ = false;
	int confirmationCommand_ = 0;
	std::string confirmationMessage_;
	bool aboutOpen_ = false;
	jpegview_linux::ExifInfo metadata_;
	std::string jpegComment_;
	bool contextMenuOpen_ = false;
	bool contextMenuAdvancedOptions_ = false;
	bool contextMenuPositionLocked_ = false;
	bool contextMenuNeedsCleanFrame_ = false;
	int contextMenuX_ = 0;
	int contextMenuY_ = 0;
	int menuSelected_ = -1;
	std::vector<MenuItem> contextMenuItems_;
	std::unordered_map<std::string, ThumbnailCacheEntry> thumbnailCache_;
	jpegview_linux::ThumbnailCacheScheduler thumbnailScheduler_;
	std::unordered_map<std::string, TextTextureCacheEntry> textTextureCache_;
	std::uint64_t textTextureUseCounter_ = 0;
	std::vector<jpegview_linux::OpenWithApplication> openWithApplications_;
	bool imageModified_ = false;
	bool autoContrastEnabled_ = false;
	bool quitRequested_ = false;
	bool fileDialogOpen_ = false;
	bool fileDialogSave_ = false;
	bool fileDialogSaveFullSize_ = true;
	bool fileDialogOverwriteConfirmed_ = false;
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
	fs::path fileDialogPreviewSource_;
	std::string fileDialogPreviewMessage_;
	std::string copyRenamePattern_;
	jpegview_linux::BatchCopyDialogController batchCopyDialog_;
	jpegview_linux::ResizeDialogController resizeDialog_;
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
		<< "          F7/F8/F9 select folder/recursive/sibling navigation, N/M/C/Z select display order,\n"
		<< "          F2 toggles picture information, Shift+N toggles the filename overlay, Ctrl+O opens, Ctrl+S saves full size, Ctrl+Shift+S saves screen size, Ctrl+R reloads, Ctrl+N toggles navigation, Ctrl+T toggles thumbnails,\n"
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

	jpegview_linux::FileList fileList(inputs);
	if (fileList.Empty()) {
		std::cerr << "No supported images found.\n";
		return 2;
	}

	if (decodeCheck) {
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

	Viewer viewer(std::move(fileList), slideshowSeconds, startFullscreen);
	const int result = viewer.Run();
	return result;
}
