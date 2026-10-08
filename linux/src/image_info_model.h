#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace jpegview_linux {

class PlaybackScheduler;

inline constexpr char kDefaultWindowTitlePattern[] = "[%p] %f (%m) - %a";
inline constexpr std::size_t kMaximumWindowTitlePatternBytes = 1024;
inline constexpr char kWindowTitlePatternHelpLine1[] =
	"Title codes: %p visible position/total; %i one-based image index; %n total images; %f filename+extension; %F filename stem (no extension); %e extension (no dot)";
inline constexpr char kWindowTitlePatternHelpLine2[] =
	"%P full path; %D containing folder; %w/%h original pixel width/height; %s readable size; %b exact bytes";
inline constexpr char kWindowTitlePatternHelpLine3[] =
	"%m dimensions+readable size; %a app name; %v build version; %% literal percent";

struct WindowTitleContext {
	std::string position;
	std::size_t currentIndex = 0;
	std::size_t imageCount = 0;
	std::string filename;
	std::string filenameStem;
	std::string extension;
	std::string fullPath;
	std::string directory;
	int width = 0;
	int height = 0;
	std::optional<std::uintmax_t> fileSize;
	std::string applicationName = "JPEGView";
	std::string applicationVersion;
};

// Surrounding whitespace is discarded. An empty pattern resolves to the
// built-in format; unknown or incomplete %-codes are invalid.
std::string NormalizeWindowTitlePattern(std::string_view pattern);
bool ValidateWindowTitlePattern(std::string_view pattern, std::string* error = nullptr);
std::string FormatWindowTitle(std::string_view pattern, const WindowTitleContext& context);

std::string FormatFileSize(std::uintmax_t size);

// Formats a zero-based current image index and optional visible spread partner
// as a one-based position such as "1/123" or "1-2/123".
std::string FormatImagePosition(std::size_t currentIndex, std::size_t imageCount,
	std::optional<std::size_t> spreadPartnerIndex = std::nullopt);

// Produces the compact EXIF-popup summary: "W X H, Size". An unavailable file
// size is omitted without leaving punctuation behind.
std::string FormatImageDimensionsAndSize(int width, int height,
	std::string_view formattedFileSize);

std::string FormatModificationDateLine(std::string_view date);

// Describes playback intent, not whether the next frame is still being
// prepared. Frame readiness is transient during normal animation playback.
std::string FormatAnimationPlaybackStatus(bool animationPlaying,
	bool manuallyPaused, std::optional<double> framesPerSecond = std::nullopt);

// Source timing follows the frame actually presented while the scheduler may
// already own a later frame awaiting preparation. Movie timing is uniform.
std::string FormatAnimationPlaybackStatus(const PlaybackScheduler& playback,
	std::size_t displayedFrame, bool isGif);

// Avoids repeating title-template expansion while its source, document, and
// display-position revisions are unchanged.
class WindowTitleFormatCache {
public:
	const std::string& GetOrBuild(const std::string& key,
		const std::function<std::string()>& builder);
	void Clear();

private:
	std::string key_;
	std::string value_;
	bool valid_ = false;
};

// Avoids rebuilding formatted EXIF and image-information text on pan/paint
// events. Geometry clipping remains the renderer's responsibility.
class ImageInfoLineCache {
public:
	const std::vector<std::string>& GetOrBuild(const std::string& key,
		const std::function<std::vector<std::string>()>& builder);
	const std::string& Key() const { return key_; }
	void Clear();

private:
	std::string key_;
	std::vector<std::string> lines_;
	bool valid_ = false;
};

// Tracks the last string sent to SDL so repeated input events do not call
// SDL_SetWindowTitle with identical text.
class AppliedWindowTitle {
public:
	bool Update(std::string title);
	const std::string& Current() const { return title_; }
	void Clear();

private:
	std::string title_;
	bool valid_ = false;
};

} // namespace jpegview_linux
