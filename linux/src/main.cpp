#include "sdl_abi.h"
#include "file_list.h"
#include "exif_reader.h"
#include "clipboard.h"
#include "image_writer.h"

// Keep Linux command dispatch aligned with the original Windows application.
// resource.h is deliberately platform-neutral: it contains the command IDs
// shared by JPEGView.rc, CMainDlg::ExecuteCommand, and KeyMap.txt.default.
#include "../../src/JPEGView/resource.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../third_party/stb_image.h"

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
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr double kMinZoom = 0.01;
constexpr double kMaxZoom = 32.0;

struct ControlButton {
	SDL_Rect rect{};
	int command = IDM_NEXT;
};

struct MenuItem {
	const char* label = nullptr;
	int command = IDM_NEXT;
	bool separator = false;
	bool checked = false;
	bool enabled = true;
};

struct FileDialogEntry {
	fs::path path;
	bool directory = false;
	bool parent = false;
};

struct FontGlyph {
	char character;
	std::array<Uint8, 7> rows;
};

// A small built-in 5x7 font keeps the native SDL frontend independent of a
// host font or an additional text-rendering shared library.  The Windows
// popup menu is text based too; this is its portable drawing equivalent.
const std::array<Uint8, 7>& GlyphRows(char character) {
	static const std::array<Uint8, 7> empty{};
	static const std::array<FontGlyph, 49> glyphs = {{
		{'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
		{'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
		{'C', {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}},
		{'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
		{'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
		{'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
		{'G', {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F}},
		{'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
		{'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
		{'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}},
		{'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
		{'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
		{'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
		{'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
		{'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
		{'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
		{'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
		{'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
		{'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
		{'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
		{'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
		{'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
		{'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
		{'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
		{'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
		{'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
		{'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
		{'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
		{'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
		{'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
		{'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
		{'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
		{'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
		{'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
		{'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
		{'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
		{'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}},
		{':', {0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00}},
		{'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
		{'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
		{'%', {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13}},
		{'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
		{'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
		{'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},
		{']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
		{'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
		{')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
		{'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
		{'=', {0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00}},
	}};
	const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	for (const FontGlyph& glyph : glyphs) {
		if (glyph.character == upper) return glyph.rows;
	}
	return empty;
}

int TextWidth(const std::string& text, int scale) {
	return text.empty() ? 0 : static_cast<int>(text.size()) * (5 * scale + scale) - scale;
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

bool IsImagePath(const fs::path& path) {
	static const std::set<std::string> extensions = {
		".jpg", ".jpeg", ".jpe", ".png", ".gif", ".bmp", ".tga",
		".psd", ".pnm", ".ppm", ".pgm", ".pic", ".webp"
	};
	return extensions.find(Lower(path.extension().string())) != extensions.end();
}

fs::path AbsoluteNormalized(const fs::path& path) {
	std::error_code error;
	const fs::path absolute = fs::absolute(path, error);
	return (error ? path : absolute).lexically_normal();
}

struct Image {
	int width = 0;
	int height = 0;
	int originalWidth = 0;
	int originalHeight = 0;
	std::vector<std::uint8_t> bgra;

	bool StoreRGBA(const unsigned char* rgbaPixels, int imageWidth, int imageHeight) {
		if (rgbaPixels == nullptr || imageWidth <= 0 || imageHeight <= 0 ||
			imageWidth > std::numeric_limits<int>::max() / 4) {
			return false;
		}
		const std::size_t widthValue = static_cast<std::size_t>(imageWidth);
		const std::size_t heightValue = static_cast<std::size_t>(imageHeight);
		if (widthValue > std::numeric_limits<std::size_t>::max() / heightValue) {
			return false;
		}
		const std::size_t pixelCount = widthValue * heightValue;
		if (pixelCount > std::numeric_limits<std::size_t>::max() / 4) {
			return false;
		}
		try {
			bgra.resize(pixelCount * 4);
		} catch (const std::exception&) {
			return false;
		}
		width = imageWidth;
		height = imageHeight;
		originalWidth = imageWidth;
		originalHeight = imageHeight;
		for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
			const unsigned char* source = rgbaPixels + pixel * 4;
			std::uint8_t* target = bgra.data() + pixel * 4;
			target[0] = source[2];
			target[1] = source[1];
			target[2] = source[0];
			target[3] = source[3];
		}
		return true;
	}

	bool LoadWebP(const fs::path& filename, std::string& errorMessage) {
		std::ifstream input(filename, std::ios::binary);
		if (!input) {
			errorMessage = "cannot open file";
			return false;
		}
		const std::vector<std::uint8_t> encoded((std::istreambuf_iterator<char>(input)), {});
		if (encoded.empty()) {
			errorMessage = "empty file";
			return false;
		}

		using DecodeRGBA = unsigned char* (*)(const std::uint8_t*, std::size_t, int*, int*);
		using FreePixels = void (*)(void*);
		const char* libraryNames[] = {"libwebp.so.7", "libwebp.so.6", "libwebp.so"};
		void* library = nullptr;
		for (const char* libraryName : libraryNames) {
			library = dlopen(libraryName, RTLD_NOW | RTLD_LOCAL);
			if (library != nullptr) break;
		}
		if (library == nullptr) {
			errorMessage = "WebP decoder library not available";
			return false;
		}

		auto decodeRGBA = reinterpret_cast<DecodeRGBA>(dlsym(library, "WebPDecodeRGBA"));
		auto freePixels = reinterpret_cast<FreePixels>(dlsym(library, "WebPFree"));
		if (decodeRGBA == nullptr || freePixels == nullptr) {
			dlclose(library);
			errorMessage = "incompatible WebP decoder library";
			return false;
		}

		int decodedWidth = 0;
		int decodedHeight = 0;
		unsigned char* rgba = decodeRGBA(encoded.data(), encoded.size(), &decodedWidth, &decodedHeight);
		if (rgba == nullptr) {
			dlclose(library);
			errorMessage = "invalid WebP image";
			return false;
		}
		const bool stored = StoreRGBA(rgba, decodedWidth, decodedHeight);
		freePixels(rgba);
		dlclose(library);
		if (!stored) {
			errorMessage = "image is too large";
		}
		return stored;
	}

	bool Load(const fs::path& filename, std::string& errorMessage) {
		if (Lower(filename.extension().string()) == ".webp") {
			return LoadWebP(filename, errorMessage);
		}
		int channels = 0;
		int decodedWidth = 0;
		int decodedHeight = 0;
		unsigned char* rgba = stbi_load(filename.string().c_str(), &decodedWidth, &decodedHeight, &channels, 4);
		if (rgba == nullptr) {
			errorMessage = stbi_failure_reason() == nullptr ? "unknown decoder error" : stbi_failure_reason();
			return false;
		}

		const bool stored = StoreRGBA(rgba, decodedWidth, decodedHeight);
		stbi_image_free(rgba);
		if (!stored) {
			errorMessage = "image is too large";
		}
		return stored;
	}

	bool Rotate(bool clockwise) {
		if (width <= 0 || height <= 0) return false;
		const int newWidth = height;
		const int newHeight = width;
		std::vector<std::uint8_t> transformed;
		try {
			transformed.resize(bgra.size());
		} catch (const std::exception&) {
			return false;
		}
		for (int sourceY = 0; sourceY < height; ++sourceY) {
			for (int sourceX = 0; sourceX < width; ++sourceX) {
				const int targetX = clockwise ? height - sourceY - 1 : sourceY;
				const int targetY = clockwise ? sourceX : width - sourceX - 1;
				const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
				const std::size_t targetOffset = (static_cast<std::size_t>(targetY) * newWidth + targetX) * 4;
				std::copy_n(bgra.data() + sourceOffset, 4, transformed.data() + targetOffset);
			}
		}
		width = newWidth;
		height = newHeight;
		bgra.swap(transformed);
		return true;
	}

	bool Mirror(bool horizontal) {
		if (width <= 0 || height <= 0) return false;
		std::vector<std::uint8_t> transformed;
		try {
			transformed.resize(bgra.size());
		} catch (const std::exception&) {
			return false;
		}
		for (int sourceY = 0; sourceY < height; ++sourceY) {
			for (int sourceX = 0; sourceX < width; ++sourceX) {
				const int targetX = horizontal ? width - sourceX - 1 : sourceX;
				const int targetY = horizontal ? sourceY : height - sourceY - 1;
				const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
				const std::size_t targetOffset = (static_cast<std::size_t>(targetY) * width + targetX) * 4;
				std::copy_n(bgra.data() + sourceOffset, 4, transformed.data() + targetOffset);
			}
		}
		bgra.swap(transformed);
		return true;
	}

	bool Resize(int newWidth, int newHeight) {
		if (width <= 0 || height <= 0 || newWidth <= 0 || newHeight <= 0) return false;
		if (newWidth == width && newHeight == height) return true;
		const std::size_t pixelCount = static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight);
		if (pixelCount > std::numeric_limits<std::size_t>::max() / 4) return false;
		std::vector<std::uint8_t> resized;
		try {
			resized.resize(pixelCount * 4);
		} catch (const std::exception&) {
			return false;
		}
		for (int y = 0; y < newHeight; ++y) {
			const int sourceY = std::min(height - 1, static_cast<int>((static_cast<std::int64_t>(y) * height) / newHeight));
			for (int x = 0; x < newWidth; ++x) {
				const int sourceX = std::min(width - 1, static_cast<int>((static_cast<std::int64_t>(x) * width) / newWidth));
				const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
				const std::size_t targetOffset = (static_cast<std::size_t>(y) * newWidth + x) * 4;
				std::copy_n(bgra.data() + sourceOffset, 4, resized.data() + targetOffset);
			}
		}
		width = newWidth;
		height = newHeight;
		bgra.swap(resized);
		return true;
	}
};

std::string FormatPercent(double zoom) {
	std::ostringstream stream;
	if (zoom >= 10.0) {
		stream.precision(0);
	} else {
		stream.precision(1);
	}
	stream << std::fixed << zoom * 100.0 << "%";
	return stream.str();
}

std::string InfoText(const std::string& value) {
	std::string result;
	result.reserve(value.size());
	for (const unsigned char character : value) {
		result.push_back(character >= 32 && character < 127 ? static_cast<char>(character) : '?');
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

std::string FormatFileSize(std::uintmax_t size) {
	static constexpr const char* suffixes[] = {"B", "KB", "MB", "GB"};
	double value = static_cast<double>(size);
	std::size_t suffix = 0;
	while (value >= 1024.0 && suffix + 1 < std::size(suffixes)) {
		value /= 1024.0;
		++suffix;
	}
	std::ostringstream stream;
	if (suffix == 0) {
		stream << size << ' ' << suffixes[suffix];
	} else {
		stream << std::fixed << std::setprecision(value >= 10.0 ? 0 : 1) << value << ' ' << suffixes[suffix];
	}
	return stream.str();
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

[[noreturn]] void ExecProcess(const std::string& executable, const std::vector<std::string>& arguments) {
	std::vector<char*> argv;
	argv.reserve(arguments.size() + 2);
	argv.push_back(const_cast<char*>(executable.c_str()));
	for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
	argv.push_back(nullptr);
	execvp(executable.c_str(), argv.data());
	_exit(127);
}

bool RunProcess(const std::string& executable, const std::vector<std::string>& arguments,
	std::string& errorMessage) {
	if (!HasExecutable(executable)) {
		errorMessage = executable + " is not installed";
		return false;
	}
	const pid_t child = fork();
	if (child < 0) {
		errorMessage = "cannot start " + executable;
		return false;
	}
	if (child == 0) ExecProcess(executable, arguments);
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errorMessage = executable + " failed";
		return false;
	}
	return true;
}

bool StartDetachedProcess(const std::string& executable, const std::vector<std::string>& arguments,
	std::string& errorMessage) {
	if (!HasExecutable(executable)) {
		errorMessage = executable + " is not installed";
		return false;
	}
	const pid_t child = fork();
	if (child < 0) {
		errorMessage = "cannot start " + executable;
		return false;
	}
	if (child == 0) {
		const pid_t detached = fork();
		if (detached < 0) _exit(127);
		if (detached > 0) _exit(0);
		setsid();
		ExecProcess(executable, arguments);
	}
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errorMessage = executable + " failed to start";
		return false;
	}
	return true;
}

std::string FileUri(const fs::path& filename) {
	const std::string path = AbsoluteNormalized(filename).string();
	std::ostringstream uri;
	uri << "file://";
	for (const unsigned char character : path) {
		if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == '-' || character == '_' ||
			character == '.' || character == '/' || character == '~') {
			uri << static_cast<char>(character);
		} else {
			static constexpr char hex[] = "0123456789ABCDEF";
			uri << '%' << hex[character >> 4] << hex[character & 15];
		}
	}
	return uri.str();
}

class Viewer {
public:
	Viewer(jpegview_linux::FileList fileList, double slideshowSeconds, bool startFullscreen)
		: fileList_(std::move(fileList)), slideshowSeconds_(slideshowSeconds),
		  lastSlideshowSeconds_(slideshowSeconds > 0.0 ? slideshowSeconds : 3.0), startFullscreen_(startFullscreen) {}

	int Run() {
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
			return 1;
		}

		window_ = SDL_CreateWindow("JPEGView Linux", 0x2FFF0000, 0x2FFF0000,
			kDefaultWidth, kDefaultHeight, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
		if (window_ == nullptr) {
			std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n';
			SDL_Quit();
			return 1;
		}

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

		if (startFullscreen_) {
			fullscreen_ = true;
			SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
		}

		if (!LoadCurrent()) {
			Cleanup();
			return 1;
		}
		ShowControls();

		bool running = true;
		while (running) {
			HandleEvents(running);
			if (quitRequested_) running = false;
			if (slideshowSeconds_ > 0.0 &&
				static_cast<double>(SDL_GetTicks() - lastInteractionTick_) >= slideshowSeconds_ * 1000.0) {
				NextImage();
			}
			Render();
			SDL_Delay(4);
		}

		Cleanup();
		return 0;
	}

private:
	void Cleanup() {
		if (clipboardMode_) {
			std::error_code removeError;
			fs::remove(clipboardTempFile_, removeError);
			if (!clipboardTempDirectory_.empty()) fs::remove(clipboardTempDirectory_, removeError);
		}
		if (texture_ != nullptr) {
			SDL_DestroyTexture(texture_);
			texture_ = nullptr;
		}
		ClearTransition();
		if (renderer_ != nullptr) {
			SDL_DestroyRenderer(renderer_);
			renderer_ = nullptr;
		}
		if (window_ != nullptr) {
			SDL_DestroyWindow(window_);
			window_ = nullptr;
		}
		SDL_Quit();
	}

	bool LoadCurrent() {
		if (fileList_.Empty()) {
			return false;
		}
		metadata_ = {};
		jpegComment_.clear();
		ClearTransition();
		std::string errorMessage;
		if (!image_.Load(fileList_.Current(), errorMessage)) {
			SetTitle(fileList_.Current().filename().string() + " — decode failed: " + errorMessage);
			std::cerr << fileList_.Current() << ": " << errorMessage << '\n';
			return false;
		}
		jpegview_linux::ReadJpegMetadata(fileList_.Current(), metadata_, jpegComment_);

		if (texture_ != nullptr) {
			SDL_DestroyTexture(texture_);
		}
		texture_ = nullptr;
		if (!UpdateTexture()) return false;

		FitToWindow(fillWithCrop_, autoZoomNoEnlarge_);
		lastInteractionTick_ = SDL_GetTicks();
		imageModified_ = false;
		SetTitle();
		return true;
	}

	bool UpdateTexture() {
		SDL_Texture* newTexture = CreateTexture(image_);
		if (newTexture == nullptr) {
			std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << '\n';
			return false;
		}
		if (texture_ != nullptr) SDL_DestroyTexture(texture_);
		texture_ = newTexture;
		return true;
	}

	SDL_Texture* CreateTexture(const Image& source) {
		SDL_Texture* result = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
			source.width, source.height);
		if (result == nullptr) return nullptr;
		if (SDL_UpdateTexture(result, nullptr, source.bgra.data(), source.width * 4) != 0) {
			std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << '\n';
			SDL_DestroyTexture(result);
			return nullptr;
		}
		return result;
	}

	void ClearTransition() {
		if (transitionTexture_ != nullptr) {
			SDL_DestroyTexture(transitionTexture_);
			transitionTexture_ = nullptr;
		}
		transitionImage_ = {};
		transitionStartTick_ = 0;
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
		SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
		transitionStartTick_ = SDL_GetTicks();
	}

	void ApplyTransform(int command) {
		bool transformed = false;
		switch (command) {
		case IDM_ROTATE_90:
			transformed = image_.Rotate(true);
			break;
		case IDM_ROTATE_270:
			transformed = image_.Rotate(false);
			break;
		case IDM_MIRROR_H:
			transformed = image_.Mirror(true);
			break;
		case IDM_MIRROR_V:
			transformed = image_.Mirror(false);
			break;
		default:
			return;
		}
		if (!transformed || !UpdateTexture()) {
			SetTitle("Image transform failed");
			return;
		}
		imageModified_ = true;
		FitToWindow(fillWithCrop_, autoZoomNoEnlarge_);
	}

	void ApplyLosslessJpegTransform(int command) {
		if (fileList_.Empty() || clipboardMode_) return;
		const std::string extension = Lower(fileList_.Current().extension().string());
		if (extension != ".jpg" && extension != ".jpeg" && extension != ".jpe") {
			SetTitle("Lossless JPEG transformation requires a JPEG image");
			return;
		}
		std::string operation;
		switch (command) {
		case IDM_ROTATE_90_LOSSLESS:
		case IDM_ROTATE_90_LOSSLESS_CONFIRM:
			operation = "90";
			break;
		case IDM_ROTATE_270_LOSSLESS:
		case IDM_ROTATE_270_LOSSLESS_CONFIRM:
			operation = "270";
			break;
		case IDM_ROTATE_180_LOSSLESS:
			operation = "180";
			break;
		default:
			break;
		}
		const bool horizontalFlip = command == IDM_MIRROR_H_LOSSLESS;
		const bool verticalFlip = command == IDM_MIRROR_V_LOSSLESS;
		if (operation.empty() && !horizontalFlip && !verticalFlip) return;
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
		std::vector<std::string> arguments{"-copy", "all"};
		if (!operation.empty()) {
			arguments.push_back("-rotate");
			arguments.push_back(operation);
		} else {
			arguments.push_back("-flip");
			arguments.push_back(horizontalFlip ? "horizontal" : "vertical");
		}
		arguments.push_back("-outfile");
		arguments.push_back(temporaryFile.string());
		arguments.push_back(fileList_.Current().string());
		std::string errorMessage;
		const bool transformed = RunProcess("jpegtran", arguments, errorMessage);
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
		std::ostringstream title;
		title << "JPEGView Linux — " << fileList_.Current().filename().string()
			<< (imageModified_ ? " *" : "")
			<< " [" << fileList_.CurrentIndex() + 1 << '/' << fileList_.Size() << "] "
			<< image_.width << 'x' << image_.height << " @ " << FormatPercent(zoom_)
			<< " — arrows: navigate/rotate, wheel: zoom, drag: pan, Space: fit/actual, F11: fullscreen, Esc: quit";
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
		if (fileDialogSave_) SDL_StopTextInput();
		fileDialogOpen_ = false;
		fileDialogSave_ = false;
		fileDialogFilename_.clear();
	}

	void SaveImageFromDialog() {
		if (!fileDialogSave_ || fileDialogFilename_.empty()) return;
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
			const int outputWidth = std::max(1, static_cast<int>(std::round(image_.width * zoom_)));
			const int outputHeight = std::max(1, static_cast<int>(std::round(image_.height * zoom_)));
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
		Image copied = image_;
		if (!fullSize) {
			const int outputWidth = std::max(1, static_cast<int>(std::round(image_.width * zoom_)));
			const int outputHeight = std::max(1, static_cast<int>(std::round(image_.height * zoom_)));
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
		if (StartDetachedProcess("xdg-open", {directory.string()}, errorMessage) ||
			StartDetachedProcess("gio", {"open", directory.string()}, errorMessage)) {
			SetTitle("Opened containing folder");
		} else {
			SetTitle("Cannot open containing folder: " + errorMessage);
		}
	}

	void PrintCurrentImage() {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
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
		const bool printed = RunProcess("lp", {temporaryFile.string()}, errorMessage);
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
			if (!entry.is_regular_file(iteratorError) || iteratorError || !IsImagePath(entry.path())) continue;
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
		if (HasExecutable("gsettings")) {
			applied = RunProcess("gsettings", {"set", "org.gnome.desktop.background", "picture-uri", FileUri(wallpaperFile)}, errorMessage);
			if (applied) {
				// GNOME 42+ also consults the dark-mode URI.  Older versions simply
				// report an unknown key; the light-mode setting above is sufficient.
				std::string ignoredError;
				RunProcess("gsettings", {"set", "org.gnome.desktop.background", "picture-uri-dark", FileUri(wallpaperFile)}, ignoredError);
			}
		}
		if (!applied && HasExecutable("feh")) {
			applied = RunProcess("feh", {"--bg-fill", wallpaperFile.string()}, errorMessage);
		}
		if (!applied && HasExecutable("nitrogen")) {
			applied = RunProcess("nitrogen", {"--set-zoom-fill", wallpaperFile.string()}, errorMessage);
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
		if (HasExecutable("gio")) {
			moved = RunProcess("gio", {"trash", filename.string()}, errorMessage);
		} else if (HasExecutable("trash-put")) {
			moved = RunProcess("trash-put", {filename.string()}, errorMessage);
		}
		if (!moved && !HasExecutable("gio") && !HasExecutable("trash-put")) {
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

	void FitToWindow(bool fillCrop = false, bool noEnlarge = false) {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const double widthScale = static_cast<double>(std::max(1, windowWidth - 16)) / image_.width;
		const double heightScale = static_cast<double>(std::max(1, windowHeight - 16)) / image_.height;
		const double windowScale = fillCrop ? std::max(widthScale, heightScale) : std::min(widthScale, heightScale);
		zoom_ = std::clamp(noEnlarge ? std::min(1.0, windowScale) : windowScale, kMinZoom, kMaxZoom);
		fitToWindow_ = true;
		fillWithCrop_ = fillCrop;
		autoZoomNoEnlarge_ = noEnlarge;
		offsetX_ = 0.0;
		offsetY_ = 0.0;
		SetTitle();
	}

	void ActualSize() {
		zoom_ = 1.0;
		fitToWindow_ = false;
		fillWithCrop_ = false;
		autoZoomNoEnlarge_ = false;
		offsetX_ = 0.0;
		offsetY_ = 0.0;
		SetTitle();
	}

	void ZoomAt(double factor, int mouseX, int mouseY) {
		if (image_.width == 0 || image_.height == 0) {
			return;
		}
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const double oldZoom = zoom_;
		const double imageX = (mouseX - (windowWidth - image_.width * oldZoom) / 2.0 - offsetX_) / oldZoom;
		const double imageY = (mouseY - (windowHeight - image_.height * oldZoom) / 2.0 - offsetY_) / oldZoom;
		zoom_ = std::clamp(oldZoom * factor, kMinZoom, kMaxZoom);
		offsetX_ = mouseX - (windowWidth - image_.width * zoom_) / 2.0 - imageX * zoom_;
		offsetY_ = mouseY - (windowHeight - image_.height * zoom_) / 2.0 - imageY * zoom_;
		fitToWindow_ = false;
		fillWithCrop_ = false;
		autoZoomNoEnlarge_ = false;
		lastInteractionTick_ = SDL_GetTicks();
		SetTitle();
	}

	void NextImage() {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = slideshowSeconds_ > 0.0 && transitionEffect_ != IDM_EFFECT_NONE;
		Image previousImage = animate ? image_ : Image{};
		if (fileList_.Next() && LoadCurrent() && animate) StartTransition(previousImage);
	}

	void PreviousImage() {
		if (clipboardMode_) RestoreClipboardImage();
		const bool animate = slideshowSeconds_ > 0.0 && transitionEffect_ != IDM_EFFECT_NONE;
		Image previousImage = animate ? image_ : Image{};
		if (fileList_.Previous() && LoadCurrent() && animate) StartTransition(previousImage);
	}

	void FirstImage() {
		if (clipboardMode_) RestoreClipboardImage();
		if (fileList_.Empty() || fileList_.CurrentIndex() == 0) return;
		fileList_.First();
		LoadCurrent();
	}

	void LastImage() {
		if (clipboardMode_) RestoreClipboardImage();
		if (fileList_.Empty() || fileList_.CurrentIndex() + 1 == fileList_.Size()) return;
		fileList_.Last();
		LoadCurrent();
	}

	void ToggleFullscreen() {
		fullscreen_ = !fullscreen_;
		SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : static_cast<Uint32>(0));
		if (fitToWindow_) {
			FitToWindow(fillWithCrop_, autoZoomNoEnlarge_);
		} else {
			SetTitle();
		}
	}

	void FitWindowToImage() {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
		const int width = std::clamp(image_.width + 16, 160, 4096);
		const int height = std::clamp(image_.height + 16, 120, 4096);
		SDL_SetWindowSize(window_, width, height);
		FitToWindow(fillWithCrop_, autoZoomNoEnlarge_);
	}

	void ToggleTitleBar() {
		borderless_ = !borderless_;
		SDL_SetWindowBordered(window_, borderless_ ? 0 : 1);
		SetTitle();
	}

	void ToggleAlwaysOnTop() {
		alwaysOnTop_ = !alwaysOnTop_;
		SDL_SetWindowAlwaysOnTop(window_, alwaysOnTop_ ? 1 : 0);
		SetTitle();
	}

	void StartSlideshow(double seconds) {
		slideshowSeconds_ = std::max(0.1, seconds);
		lastSlideshowSeconds_ = slideshowSeconds_;
		lastInteractionTick_ = SDL_GetTicks();
		SetTitle();
	}

	void ShowControls() {
		if (!navigationPanelEnabled_) return;
		controlsVisible_ = true;
	}

	std::vector<std::string> ImageInfoLines() const {
		std::vector<std::string> lines;
		if (fileList_.Empty()) return lines;

		std::ostringstream title;
		title << '[' << fileList_.CurrentIndex() + 1 << '/' << fileList_.Size() << "] "
			<< InfoText(fileList_.Current().filename().string());
		lines.push_back(title.str());

		lines.push_back("Image width: " + std::to_string(image_.originalWidth));
		lines.push_back("Image height: " + std::to_string(image_.originalHeight));
		if (image_.width != image_.originalWidth || image_.height != image_.originalHeight) {
			lines.push_back("Displayed size: " + std::to_string(image_.width) + " x " + std::to_string(image_.height));
		}
		std::error_code fileError;
		const std::uintmax_t fileSize = fs::file_size(fileList_.Current(), fileError);
		if (!fileError) lines.push_back("File size: " + FormatFileSize(fileSize));

		const std::string modificationDate = FormatFileTime(fileList_.Current());
		if (!metadata_.acquisitionDate.empty()) {
			lines.push_back("Acquisition date: " + InfoText(metadata_.acquisitionDate));
		} else if (!metadata_.dateTime.empty()) {
			lines.push_back("Exif Date Time: " + InfoText(metadata_.dateTime));
		} else if (!modificationDate.empty()) {
			lines.push_back("Modification date: " + modificationDate);
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

	SDL_Rect ControlPanelRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int buttonSize = 40;
		const int gap = 5;
		const int margin = 8;
		const int separator = 12;
		const int buttonCount = 8;
		const int panelWidth = margin * 2 + buttonSize * buttonCount + gap * (buttonCount - 2) + separator * 2;
		return SDL_Rect{
			(windowWidth - panelWidth) / 2,
			windowHeight - buttonSize - margin * 2,
			panelWidth,
			buttonSize + margin * 2
		};
	}

	void LayoutControls(std::vector<ControlButton>& buttons) const {
		const SDL_Rect panel = ControlPanelRect();
		const int buttonSize = 40;
		const int gap = 5;
		const int margin = 8;
		const int separator = 12;
		const int commands[] = {
			IDM_FIRST, IDM_PREV, IDM_NEXT,
			IDM_LAST, IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, IDM_FULL_SCREEN_MODE,
			IDM_ROTATE_90, IDM_ROTATE_270
		};
		buttons.clear();
		int x = panel.x + margin;
		for (std::size_t i = 0; i < std::size(commands); ++i) {
			buttons.push_back(ControlButton{SDL_Rect{x, panel.y + margin, buttonSize, buttonSize}, commands[i]});
			x += buttonSize + gap;
			if (i == 3 || i == 5) x += separator;
		}
	}

	static bool PointInRect(int x, int y, const SDL_Rect& rect) {
		return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
	}

	bool HandleControlClick(int x, int y) {
		if (!navigationPanelEnabled_ || !controlsVisible_) return false;
		std::vector<ControlButton> buttons;
		LayoutControls(buttons);
		for (const ControlButton& button : buttons) {
			if (!PointInRect(x, y, button.rect)) continue;
			ExecuteCommand(button.command);
			ShowControls();
			return true;
		}
		return PointInRect(x, y, ControlPanelRect());
	}

	void ExecuteCommand(int command) {
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
			if (fitToWindow_) ActualSize(); else FitToWindow();
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
		case IDM_COPY:
			CopyCurrentImage(false);
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
			break;
		case IDM_SHOW_FILENAME:
			showFileName_ = !showFileName_;
			break;
		case IDM_SHOW_NAVPANEL:
			navigationPanelEnabled_ = !navigationPanelEnabled_;
			if (navigationPanelEnabled_) ShowControls(); else controlsVisible_ = false;
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
			break;
		case IDM_SORT_CREATION_DATE:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::CreationTime,
				fileList_.IsSortedAscending());
			SetTitle();
			break;
		case IDM_SORT_NAME:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::FileName,
				fileList_.IsSortedAscending());
			SetTitle();
			break;
		case IDM_SORT_RANDOM:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::Random,
				fileList_.IsSortedAscending());
			SetTitle();
			break;
		case IDM_SORT_SIZE:
			fileList_.SetSorting(jpegview_linux::FileList::SortMode::FileSize,
				fileList_.IsSortedAscending());
			SetTitle();
			break;
		case IDM_SORT_ASCENDING:
			fileList_.SetSorting(fileList_.GetSorting(), true);
			SetTitle();
			break;
		case IDM_SORT_DESCENDING:
			fileList_.SetSorting(fileList_.GetSorting(), false);
			SetTitle();
			break;
		case IDM_STOP_MOVIE:
			slideshowSeconds_ = 0.0;
			lastInteractionTick_ = SDL_GetTicks();
			SetTitle();
			break;
		case IDM_SLIDESHOW_RESUME:
			StartSlideshow(lastSlideshowSeconds_);
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
			ZoomAt(4.0 / zoom_, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_200:
			ZoomAt(2.0 / zoom_, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_50:
			ZoomAt(0.5 / zoom_, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_25:
			ZoomAt(0.25 / zoom_, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_INC:
			ZoomAt(1.2, imageCenterX_, imageCenterY_);
			break;
		case IDM_ZOOM_DEC:
			ZoomAt(1.0 / 1.2, imageCenterX_, imageCenterY_);
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
			FitToWindow(false, false);
			break;
		case IDM_AUTO_ZOOM_FILL:
			FitToWindow(true, false);
			break;
		case IDM_ABOUT:
			OpenAbout();
			break;
		case IDM_MOVIE_START_FPS:
			StartSlideshow(1.0 / 25.0);
			break;
		case IDM_MOVIE_5_FPS:
		case IDM_MOVIE_10_FPS:
		case IDM_MOVIE_25_FPS:
		case IDM_MOVIE_30_FPS:
		case IDM_MOVIE_50_FPS:
		case IDM_MOVIE_100_FPS:
			StartSlideshow(1.0 / static_cast<double>(command - IDM_MOVIE_START_FPS));
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
			if (slideshowSeconds_ > 0.0) {
				slideshowSeconds_ = 0.0;
				lastInteractionTick_ = SDL_GetTicks();
				SetTitle();
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

	// This is the supported portion of Config/KeyMap.txt.default expressed in
	// SDL key symbols.  It deliberately returns the original Windows command
	// IDs, just like CKeyMap::GetCommandIdForKey does on Windows.
	int CommandForKey(const SDL_KeyboardEvent& event) const {
		const Sint32 key = event.keysym.sym;
		const Uint16 modifiers = event.keysym.mod;
		const bool ctrl = (modifiers & 0x00C0u) != 0;
		const bool shift = (modifiers & 0x0003u) != 0;
		const bool alt = (modifiers & 0x0300u) != 0;
		if (alt) return 0;

		if (key == SDLK_ESCAPE) return slideshowSeconds_ > 0.0 ? IDM_DEFAULT_ESC : IDM_EXIT;
		if (!ctrl && !shift && key == SDLK_q) return IDM_EXIT; // Linux viewer convenience alias.
		if (ctrl && !shift && key == SDLK_o) return IDM_OPEN;
		if (ctrl && !shift && key == SDLK_F2) return IDM_SHOW_FILENAME;
		if (ctrl && !shift && key == 'c') return IDM_COPY;
		if (ctrl && shift && key == 'c') return IDM_COPY_PATH;
		if (ctrl && !shift && key == 'x') return IDM_COPY_FULL;
		if (ctrl && !shift && key == 'v') return IDM_PASTE;
		if (ctrl && !shift && key == 'p') return IDM_PRINT;
		if (ctrl && !shift && key == 's') return IDM_SAVE_ALLOW_NO_PROMPT;
		if (ctrl && shift && key == 's') return IDM_SAVE_SCREEN;
		if (ctrl && !shift && key == SDLK_r) return IDM_RELOAD;
		if (ctrl && shift && key == 'm') return IDM_TOUCH_IMAGE;
		if (ctrl && shift && key == 'e') return IDM_TOUCH_IMAGE_EXIF;
		if (ctrl && !shift && key == 'n') return IDM_SHOW_NAVPANEL;
		if (!ctrl && !shift && key == SDLK_F2) return IDM_SHOW_FILEINFO;
		if (!ctrl && !shift && key == SDLK_F3) return IDM_TOGGLE_RESAMPLING_QUALITY;
		if (!ctrl && !shift && key == SDLK_F4) return IDM_KEEP_PARAMETERS;
		if (!ctrl && !shift && key == SDLK_F5) return IDM_AUTO_CORRECTION;
		if (!ctrl && !shift && key == SDLK_F6) return IDM_LDC;
		if (!ctrl && !shift && key == 'c') return IDM_SORT_CREATION_DATE;
		if (!ctrl && !shift && key == 'n') return IDM_SORT_NAME;
		if (!ctrl && !shift && key == 'm') return IDM_SORT_MOD_DATE;
		if (!ctrl && !shift && key == 'z') return IDM_SORT_RANDOM;
		if (!ctrl && !shift && key == SDLK_F7) return IDM_LOOP_FOLDER;
		if (!ctrl && !shift && key == SDLK_F8) return IDM_LOOP_RECURSIVELY;
		if (!ctrl && !shift && key == SDLK_F9) return IDM_LOOP_SIBLINGS;
		if (!ctrl && !shift && key == SDLK_DELETE) return IDM_MOVE_TO_RECYCLE_BIN_CONFIRM;
		if (!ctrl && !shift && key == 'w') return IDM_EXPLORE;

		if (!ctrl && !shift && (key == SDLK_RIGHT || key == SDLK_PAGEDOWN)) return IDM_NEXT;
		if (!ctrl && !shift && (key == SDLK_LEFT || key == SDLK_PAGEUP)) return IDM_PREV;
		if (!ctrl && !shift && key == SDLK_HOME) return IDM_FIRST;
		if (!ctrl && !shift && key == SDLK_END) return IDM_LAST;
		if (!ctrl && !shift && key == SDLK_SPACE) return IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS;
		if (!ctrl && !shift && key == SDLK_RETURN) return IDM_FIT_TO_SCREEN;
		if (!ctrl && !shift && key == SDLK_DOWN) return IDM_ROTATE_90;
		if (!ctrl && !shift && key == SDLK_UP) return IDM_ROTATE_270;
		if (ctrl && key == SDLK_DOWN) return IDM_ZOOM_DEC;
		if (ctrl && key == SDLK_UP) return IDM_ZOOM_INC;
		if (!ctrl && !shift && key == SDLK_F11) return IDM_FULL_SCREEN_MODE;
		if (shift && !ctrl && key == SDLK_F11) return IDM_HIDE_TITLE_BAR;
		if (ctrl && !shift && key == SDLK_F11) return IDM_FIT_WINDOW_TO_IMAGE;
		if (!ctrl && !shift && key == SDLK_F12) return IDM_SPAN_SCREENS;
		if (shift && !ctrl && key == SDLK_F12) return IDM_ALWAYS_ON_TOP;
		if (ctrl && !shift && key == SDLK_RETURN) return IDM_FILL_WITH_CROP;
		if (!ctrl && !shift && key == 'r') return IDM_ROTATE_90_LOSSLESS_CONFIRM;
		if (!ctrl && !shift && key == 't') return IDM_ROTATE_270_LOSSLESS_CONFIRM;

		if (!ctrl && !shift && key == SDLK_0) return IDM_FIT_TO_SCREEN; // retained compatibility alias.
		if (!ctrl && !shift && key == SDLK_f) return IDM_FULL_SCREEN_MODE; // retained compatibility alias.

		const bool plus = key == SDLK_EQUALS || key == SDLK_KP_PLUS;
		if (plus && !ctrl && !alt) return IDM_ZOOM_INC;
		if ((key == SDLK_MINUS || key == SDLK_KP_MINUS) && !ctrl && !alt) return IDM_ZOOM_DEC;
		return 0;
	}

	std::vector<MenuItem> ContextMenuItems() const {
		const std::string extension = fileList_.Empty() ? std::string() : Lower(fileList_.Current().extension().string());
		const bool losslessJpegAvailable = !clipboardMode_ && HasExecutable("jpegtran") &&
			(extension == ".jpg" || extension == ".jpeg" || extension == ".jpe");
		return {
			// This is a flattened rendering of the complete Windows PopupMenu
			// resource.  Indented entries are the portable equivalent of its
			// submenus. Unsupported Windows-only commands remain visible but
			// disabled instead of silently doing nothing.
			{"Stop slide show/movie", IDM_STOP_MOVIE, false, false, slideshowSeconds_ > 0.0},
			{nullptr, 0, true},
			{"Open image...", IDM_OPEN},
			{"Open image with", 0},
			{"  (no configured applications)", 0, false, false, false},
			{"Save processed image...", IDM_SAVE},
			{"Save displayed image...", IDM_SAVE_SCREEN},
			{"Reload image", IDM_RELOAD},
			{"Open containing folder", IDM_EXPLORE},
			{"Print image...", IDM_PRINT},
			{"Batch rename/copy...", IDM_BATCH_COPY, false, false, false},
			{"Set modification date", 0},
			{"  To current date", IDM_TOUCH_IMAGE},
			{"  To EXIF date", IDM_TOUCH_IMAGE_EXIF},
			{"  To EXIF date all files in folder", IDM_TOUCH_IMAGE_EXIF_FOLDER},
			{"Set as desktop wallpaper", 0},
			{"  Use original image", IDM_SET_WALLPAPER_ORIG},
			{"  Use processed image as displayed", IDM_SET_WALLPAPER_DISPLAY},
			{nullptr, 0, true},
			{"Copy to clipboard", IDM_COPY},
			{"Copy original size image", IDM_COPY_FULL},
			{"Copy file path", IDM_COPY_PATH},
			{"Paste from clipboard", IDM_PASTE},
			{nullptr, 0, true},
			{"Show picture info (EXIF)", IDM_SHOW_FILEINFO, false, infoVisible_},
			{"Show filename", IDM_SHOW_FILENAME, false, showFileName_},
			{"Show navigation panel", IDM_SHOW_NAVPANEL, false, navigationPanelEnabled_},
			{nullptr, 0, true},
			{"Next image", IDM_NEXT},
			{"Previous image", IDM_PREV},
			{"First image", IDM_FIRST},
			{"Last image", IDM_LAST},
			{nullptr, 0, true},
			{"Navigation", 0},
			{"  Loop folder", IDM_LOOP_FOLDER, false,
				fileList_.GetNavigationMode() == jpegview_linux::FileList::NavigationMode::LoopDirectory},
			{"  Loop recursively", IDM_LOOP_RECURSIVELY, false,
				fileList_.GetNavigationMode() == jpegview_linux::FileList::NavigationMode::LoopSubDirectories},
			{"  Loop siblings", IDM_LOOP_SIBLINGS, false,
				fileList_.GetNavigationMode() == jpegview_linux::FileList::NavigationMode::LoopSameDirectoryLevel},
			{"Display order", 0},
			{"  Modification date", IDM_SORT_MOD_DATE, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::LastModificationTime},
			{"  Creation date", IDM_SORT_CREATION_DATE, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::CreationTime},
			{"  File name", IDM_SORT_NAME, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::FileName},
			{"  File size", IDM_SORT_SIZE, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::FileSize},
			{"  Random", IDM_SORT_RANDOM, false,
				fileList_.GetSorting() == jpegview_linux::FileList::SortMode::Random},
			{"  Ascending", IDM_SORT_ASCENDING, false, fileList_.IsSortedAscending()},
			{"  Descending", IDM_SORT_DESCENDING, false, !fileList_.IsSortedAscending()},
			{nullptr, 0, true},
			{"Transform image", 0},
			{"  Rotate +90", IDM_ROTATE_90},
			{"  Rotate -90", IDM_ROTATE_270},
			{"  Rotate...", IDM_ROTATE, false, false, false},
			{"  Change size...", IDM_CHANGESIZE, false, false, false},
			{"  Perspective correction...", IDM_PERSPECTIVE, false, false, false},
			{"  Mirror horizontally", IDM_MIRROR_H},
			{"  Mirror vertically", IDM_MIRROR_V},
			{"Lossless JPEG transformations", 0},
			{"  Rotate +90", IDM_ROTATE_90_LOSSLESS, false, false, losslessJpegAvailable},
			{"  Rotate -90", IDM_ROTATE_270_LOSSLESS, false, false, losslessJpegAvailable},
			{"  Rotate 180", IDM_ROTATE_180_LOSSLESS, false, false, losslessJpegAvailable},
			{"  Mirror horizontally", IDM_MIRROR_H_LOSSLESS, false, false, losslessJpegAvailable},
			{"  Mirror vertically", IDM_MIRROR_V_LOSSLESS, false, false, losslessJpegAvailable},
			{"Auto correction", IDM_AUTO_CORRECTION, false, false, false},
			{"Local density correction", IDM_LDC, false, false, false},
			{"Keep parameters", IDM_KEEP_PARAMETERS, false, false, false},
			{"Save parameters to DB", IDM_SAVE_PARAM_DB, false, false, false},
			{"Clear parameters from DB", IDM_CLEAR_PARAM_DB, false, false, false},
			{nullptr, 0, true},
			{"Zoom", 0},
			{"  Fit to screen", IDM_FIT_TO_SCREEN, false, fitToWindow_ && !fillWithCrop_},
			{"  Fill with crop", IDM_FILL_WITH_CROP, false, fitToWindow_ && fillWithCrop_},
			{"  Span all screens", IDM_SPAN_SCREENS, false, fullscreen_},
			{"  400 %", IDM_ZOOM_400},
			{"  200 %", IDM_ZOOM_200},
			{"  100 %", IDM_ZOOM_100, false, !fitToWindow_ && std::abs(zoom_ - 1.0) < 0.01},
			{"  50 %", IDM_ZOOM_50},
			{"  25 %", IDM_ZOOM_25},
			{"  Full screen mode", IDM_FULL_SCREEN_MODE, false, fullscreen_},
			{"  Fit window to image", IDM_FIT_WINDOW_TO_IMAGE},
			{"  Hide window title bar", IDM_HIDE_TITLE_BAR, false, borderless_},
			{"  Set window always on top", IDM_ALWAYS_ON_TOP, false, alwaysOnTop_},
			{"Auto zoom mode", 0},
			{"  Fit to screen no zoom", IDM_AUTO_ZOOM_FIT_NO_ZOOM, false, fitToWindow_ && !fillWithCrop_ && autoZoomNoEnlarge_},
			{"  Fill with crop no zoom", IDM_AUTO_ZOOM_FILL_NO_ZOOM, false, fitToWindow_ && fillWithCrop_ && autoZoomNoEnlarge_},
			{"  Fit to screen", IDM_AUTO_ZOOM_FIT, false, fitToWindow_ && !fillWithCrop_ && !autoZoomNoEnlarge_},
			{"  Fill with crop", IDM_AUTO_ZOOM_FILL, false, fitToWindow_ && fillWithCrop_ && !autoZoomNoEnlarge_},
			{nullptr, 0, true},
			{"Play folder as slideshow/movie", 0},
			{slideshowSeconds_ > 0.0 ? "  Stop slide show/movie" : "  Slideshow", slideshowSeconds_ > 0.0 ? IDM_STOP_MOVIE : IDM_SLIDESHOW_START},
			{"  Waiting time 1 sec", IDM_SLIDESHOW_1, false, false, true},
			{"  Waiting time 2 sec", IDM_SLIDESHOW_2, false, false, true},
			{"  Waiting time 3 sec", IDM_SLIDESHOW_3, false, false, true},
			{"  Waiting time 4 sec", IDM_SLIDESHOW_4, false, false, true},
			{"  Waiting time 5 sec", IDM_SLIDESHOW_5, false, false, true},
			{"  Waiting time 7 sec", IDM_SLIDESHOW_7, false, false, true},
			{"  Waiting time 10 sec", IDM_SLIDESHOW_10, false, false, true},
			{"  Waiting time 20 sec", IDM_SLIDESHOW_20, false, false, true},
			{"  Transition effect", 0},
			{"    None", IDM_EFFECT_NONE, false, transitionEffect_ == IDM_EFFECT_NONE, true},
			{"    Blend", IDM_EFFECT_BLEND, false, transitionEffect_ == IDM_EFFECT_BLEND, true},
			{"    Slide from right", IDM_EFFECT_SLIDE_RL, false, transitionEffect_ == IDM_EFFECT_SLIDE_RL, true},
			{"    Slide from left", IDM_EFFECT_SLIDE_LR, false, transitionEffect_ == IDM_EFFECT_SLIDE_LR, true},
			{"    Slide from top", IDM_EFFECT_SLIDE_TB, false, transitionEffect_ == IDM_EFFECT_SLIDE_TB, true},
			{"    Slide from bottom", IDM_EFFECT_SLIDE_BT, false, transitionEffect_ == IDM_EFFECT_SLIDE_BT, true},
			{"    Roll from right", IDM_EFFECT_ROLL_RL, false, transitionEffect_ == IDM_EFFECT_ROLL_RL, true},
			{"    Roll from left", IDM_EFFECT_ROLL_LR, false, transitionEffect_ == IDM_EFFECT_ROLL_LR, true},
			{"    Roll from top", IDM_EFFECT_ROLL_TB, false, transitionEffect_ == IDM_EFFECT_ROLL_TB, true},
			{"    Roll from bottom", IDM_EFFECT_ROLL_BT, false, transitionEffect_ == IDM_EFFECT_ROLL_BT, true},
			{"    Scroll from right", IDM_EFFECT_SCROLL_RL, false, transitionEffect_ == IDM_EFFECT_SCROLL_RL, true},
			{"    Scroll from left", IDM_EFFECT_SCROLL_LR, false, transitionEffect_ == IDM_EFFECT_SCROLL_LR, true},
			{"    Scroll from top", IDM_EFFECT_SCROLL_TB, false, transitionEffect_ == IDM_EFFECT_SCROLL_TB, true},
			{"    Scroll from bottom", IDM_EFFECT_SCROLL_BT, false, transitionEffect_ == IDM_EFFECT_SCROLL_BT, true},
			{"  Transition speed", 0},
			{"    Very fast", IDM_EFFECTTIME_VERY_FAST, false, transitionDurationMs_ == 100, true},
			{"    Fast", IDM_EFFECTTIME_FAST, false, transitionDurationMs_ == 250, true},
			{"    Normal", IDM_EFFECTTIME_NORMAL, false, transitionDurationMs_ == 500, true},
			{"    Slow", IDM_EFFECTTIME_SLOW, false, transitionDurationMs_ == 1000, true},
			{"    Very slow", IDM_EFFECTTIME_VERY_SLOW, false, transitionDurationMs_ == 2000, true},
			{"  Movie", IDM_MOVIE_START_FPS},
			{"  Playback speed 5 fps", IDM_MOVIE_5_FPS, false, false, false},
			{"  Playback speed 10 fps", IDM_MOVIE_10_FPS, false, false, false},
			{"  Playback speed 25 fps", IDM_MOVIE_25_FPS, false, false, false},
			{"  Playback speed 30 fps", IDM_MOVIE_30_FPS, false, false, false},
			{"  Playback speed 50 fps", IDM_MOVIE_50_FPS, false, false, false},
			{"  Playback speed 100 fps", IDM_MOVIE_100_FPS, false, false, false},
			{nullptr, 0, true},
			{"Settings Admin", 0},
			{"  Edit global settings...", IDM_EDIT_GLOBAL_CONFIG, false, false, false},
			{"  Edit user settings...", IDM_EDIT_USER_CONFIG, false, false, false},
			{"  Update user settings...", IDM_UPDATE_USER_CONFIG, false, false, false},
			{"  Manage Open image with menu...", IDM_MANAGE_OPEN_WITH_MENU, false, false, false},
			{"  Set current parameters as default...", IDM_SAVE_PARAMETERS, false, false, false},
			{"  Set as default viewer...", IDM_SET_AS_DEFAULT_VIEWER, false, false, false},
			{"  Backup parameter DB...", IDM_BACKUP_PARAMDB, false, false, false},
			{"  Restore parameter DB...", IDM_RESTORE_PARAMDB, false, false, false},
			{"User commands", 0},
			{"  (none configured)", 0, false, false, false},
			{nullptr, 0, true},
			{"About JPEGView...", IDM_ABOUT},
			{nullptr, 0, true},
			{"Exit", IDM_EXIT},
		};
	}

	std::string MenuLabel(const MenuItem& item) const {
		if (item.checked) return std::string("[X] ") + item.label;
		return item.label == nullptr ? std::string() : item.label;
	}

	int ContextMenuVisibleCount() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		return std::max(1, std::min(22, (windowHeight - 32) / 28));
	}

	SDL_Rect ContextMenuRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		int width = 260;
		int height = 16;
		const int visibleCount = ContextMenuVisibleCount();
		const std::size_t visibleEnd = std::min(contextMenuItems_.size(),
			contextMenuScroll_ + static_cast<std::size_t>(visibleCount));
		for (std::size_t index = contextMenuScroll_; index < visibleEnd; ++index) {
			const MenuItem& item = contextMenuItems_[index];
			if (item.separator) {
				height += 9;
				continue;
			}
			width = std::max(width, TextWidth(MenuLabel(item), 2) + 28);
			height += 28;
		}
		int x = contextMenuX_;
		int y = contextMenuY_;
		if (x + width > windowWidth) x = windowWidth - width - 4;
		if (y + height > windowHeight) y = windowHeight - height - 4;
		x = std::max(4, x);
		y = std::max(4, y);
		return SDL_Rect{x, y, width, height};
	}

	int ContextMenuItemAt(int x, int y) const {
		const SDL_Rect menu = ContextMenuRect();
		if (!PointInRect(x, y, menu)) return -1;
		int itemTop = menu.y + 8;
		const int visibleCount = ContextMenuVisibleCount();
		const std::size_t visibleEnd = std::min(contextMenuItems_.size(),
			contextMenuScroll_ + static_cast<std::size_t>(visibleCount));
		for (std::size_t i = contextMenuScroll_; i < visibleEnd; ++i) {
			const MenuItem& item = contextMenuItems_[i];
			const int itemHeight = item.separator ? 9 : 28;
			if (y >= itemTop && y < itemTop + itemHeight) {
				return item.separator || item.command == 0 || !item.enabled ? -1 : static_cast<int>(i);
			}
			itemTop += itemHeight;
		}
		return -1;
	}

	void UpdateContextMenuSelection(int x, int y) {
		menuSelected_ = ContextMenuItemAt(x, y);
	}

	void OpenContextMenu(int x, int y) {
		contextMenuItems_ = ContextMenuItems();
		contextMenuX_ = x;
		contextMenuY_ = y;
		contextMenuScroll_ = 0;
		contextMenuOpen_ = true;
		menuSelected_ = -1;
	}

	void EnsureContextMenuSelectionVisible() {
		const std::size_t visibleCount = static_cast<std::size_t>(ContextMenuVisibleCount());
		if (menuSelected_ >= 0) {
			const std::size_t selected = static_cast<std::size_t>(menuSelected_);
			if (selected < contextMenuScroll_) contextMenuScroll_ = selected;
			if (selected >= contextMenuScroll_ + visibleCount) contextMenuScroll_ = selected - visibleCount + 1;
		}
		const std::size_t maximumScroll = contextMenuItems_.size() > visibleCount ?
			contextMenuItems_.size() - visibleCount : 0;
		contextMenuScroll_ = std::min(contextMenuScroll_, maximumScroll);
	}

	void MoveContextMenuSelection(int direction) {
		if (contextMenuItems_.empty()) return;
		int candidate = menuSelected_;
		for (std::size_t tries = 0; tries < contextMenuItems_.size(); ++tries) {
			candidate += direction;
			if (candidate < 0) candidate = static_cast<int>(contextMenuItems_.size()) - 1;
			if (candidate >= static_cast<int>(contextMenuItems_.size())) candidate = 0;
			if (!contextMenuItems_[candidate].separator && contextMenuItems_[candidate].command != 0 &&
				contextMenuItems_[candidate].enabled) {
				menuSelected_ = candidate;
				EnsureContextMenuSelectionVisible();
				return;
			}
		}
	}

	void ActivateContextMenuSelection(bool& running) {
		if (menuSelected_ < 0 || menuSelected_ >= static_cast<int>(contextMenuItems_.size()) ||
			contextMenuItems_[menuSelected_].separator || contextMenuItems_[menuSelected_].command == 0 ||
			!contextMenuItems_[menuSelected_].enabled) {
			return;
		}
		const int command = contextMenuItems_[menuSelected_].command;
		contextMenuOpen_ = false;
		ExecuteCommand(command);
		if (command == IDM_EXIT) running = false;
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
		return FileDialogRect().y + (fileDialogSave_ ? 112 : 72);
	}

	int FileDialogVisibleRows() const {
		const int reservedHeight = fileDialogSave_ ? 168 : 128;
		return std::max(1, (FileDialogRect().h - reservedHeight) / 26);
	}

	void RefreshFileDialog() {
		fileDialogEntries_.clear();
		std::error_code error;
		const fs::path parent = fileDialogDirectory_.parent_path();
		if (!parent.empty() && parent != fileDialogDirectory_) {
			fileDialogEntries_.push_back(FileDialogEntry{parent, true, true});
		}

		for (const fs::directory_entry& entry : fs::directory_iterator(fileDialogDirectory_, error)) {
			if (error) break;
			std::error_code statusError;
			const bool directory = entry.is_directory(statusError);
			if (statusError || (!directory && (!entry.is_regular_file(statusError) || !IsImagePath(entry.path())))) {
				continue;
			}
			fileDialogEntries_.push_back(FileDialogEntry{AbsoluteNormalized(entry.path()), directory, false});
		}

		std::sort(fileDialogEntries_.begin(), fileDialogEntries_.end(), [](const FileDialogEntry& left, const FileDialogEntry& right) {
			if (left.parent != right.parent) return left.parent;
			if (left.directory != right.directory) return left.directory;
			const std::string leftName = Lower(left.path.filename().string());
			const std::string rightName = Lower(right.path.filename().string());
			return leftName == rightName ? left.path.string() < right.path.string() : leftName < rightName;
		});
		fileDialogSelected_ = 0;
		fileDialogScroll_ = 0;
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
		fileDialogMessage_.clear();
		fileDialogOpen_ = true;
		contextMenuOpen_ = false;
		RefreshFileDialog();
	}

	void OpenSaveFileDialog(bool fullSize) {
		if (fileList_.Empty() || image_.width <= 0 || image_.height <= 0) return;
		fs::path directory = fileList_.Current().parent_path();
		if (directory.empty()) directory = fs::current_path();
		fileDialogDirectory_ = AbsoluteNormalized(directory);
		fileDialogSave_ = true;
		fileDialogSaveFullSize_ = fullSize;
		fileDialogFilename_ = fileList_.Current().stem().string() + "_proc.jpg";
		fileDialogMessage_.clear();
		fileDialogOverwriteConfirmed_ = false;
		fileDialogOpen_ = true;
		contextMenuOpen_ = false;
		RefreshFileDialog();
		fileDialogSelected_ = -1;
		SDL_StartTextInput();
	}

	std::string FileDialogEntryLabel(const FileDialogEntry& entry) const {
		if (entry.parent) return "[..]";
		return entry.directory ? std::string("[DIR] ") + entry.path.filename().string() : entry.path.filename().string();
	}

	int FileDialogItemAt(int x, int y) const {
		const SDL_Rect dialog = FileDialogRect();
		const int listTop = FileDialogListTop();
		const int rows = FileDialogVisibleRows();
		if (!PointInRect(x, y, SDL_Rect{dialog.x + 12, listTop, dialog.w - 24, rows * 26})) return -1;
		const int row = (y - listTop) / 26;
		const int item = fileDialogScroll_ + row;
		return item >= 0 && item < static_cast<int>(fileDialogEntries_.size()) ? item : -1;
	}

	void EnsureFileDialogSelectionVisible() {
		const int rows = FileDialogVisibleRows();
		if (fileDialogSelected_ < fileDialogScroll_) fileDialogScroll_ = fileDialogSelected_;
		if (fileDialogSelected_ >= fileDialogScroll_ + rows) fileDialogScroll_ = fileDialogSelected_ - rows + 1;
		const int maximumScroll = std::max(0, static_cast<int>(fileDialogEntries_.size()) - rows);
		fileDialogScroll_ = std::clamp(fileDialogScroll_, 0, maximumScroll);
	}

	void MoveFileDialogSelection(int direction) {
		if (fileDialogEntries_.empty()) return;
		fileDialogSelected_ = std::clamp(fileDialogSelected_ + direction, 0,
			static_cast<int>(fileDialogEntries_.size()) - 1);
		EnsureFileDialogSelectionVisible();
	}

	void ActivateFileDialogSelection() {
		if (fileDialogSave_ && fileDialogSelected_ < 0) {
			SaveImageFromDialog();
			return;
		}
		if (fileDialogSelected_ < 0 || fileDialogSelected_ >= static_cast<int>(fileDialogEntries_.size())) return;
		const FileDialogEntry entry = fileDialogEntries_[fileDialogSelected_];
		if (entry.directory) {
			fileDialogDirectory_ = entry.path;
			RefreshFileDialog();
			if (fileDialogSave_) fileDialogSelected_ = -1;
			fileDialogOverwriteConfirmed_ = false;
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
		case SDL_KEYDOWN:
			if (event.key.repeat != 0) break;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				CloseFileDialog();
			} else if (event.key.keysym.sym == SDLK_UP) {
				MoveFileDialogSelection(-1);
			} else if (event.key.keysym.sym == SDLK_DOWN) {
				MoveFileDialogSelection(1);
			} else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
				ActivateFileDialogSelection();
			} else if (event.key.keysym.sym == SDLK_BACKSPACE) {
				if (fileDialogSave_ && fileDialogSelected_ < 0 && !fileDialogFilename_.empty()) {
					fileDialogFilename_.pop_back();
					fileDialogMessage_.clear();
					fileDialogOverwriteConfirmed_ = false;
					break;
				}
				const fs::path parent = fileDialogDirectory_.parent_path();
				if (!parent.empty() && parent != fileDialogDirectory_) {
					fileDialogDirectory_ = parent;
					RefreshFileDialog();
					if (fileDialogSave_) fileDialogSelected_ = -1;
					if (fileDialogSave_) fileDialogOverwriteConfirmed_ = false;
				}
			}
			break;
		case SDL_TEXTINPUT:
			if (fileDialogSave_) {
				fileDialogFilename_ += event.text.text;
				fileDialogSelected_ = -1;
				fileDialogMessage_.clear();
				fileDialogOverwriteConfirmed_ = false;
			}
			break;
		case SDL_MOUSEMOTION: {
			const int item = FileDialogItemAt(event.motion.x, event.motion.y);
			if (item >= 0) {
				fileDialogSelected_ = item;
				EnsureFileDialogSelectionVisible();
			}
			break;
		}
		case SDL_MOUSEBUTTONDOWN:
			if (event.button.button == SDL_BUTTON_RIGHT ||
			(event.button.button == SDL_BUTTON_LEFT && FileDialogItemAt(event.button.x, event.button.y) < 0)) {
				CloseFileDialog();
			} else if (event.button.button == SDL_BUTTON_LEFT) {
				fileDialogSelected_ = FileDialogItemAt(event.button.x, event.button.y);
				if (fileDialogSave_ && fileDialogSelected_ >= 0 &&
					fileDialogSelected_ < static_cast<int>(fileDialogEntries_.size()) &&
					!fileDialogEntries_[fileDialogSelected_].directory) {
					fileDialogFilename_ = fileDialogEntries_[fileDialogSelected_].path.filename().string();
				}
				if (event.button.clicks >= 2) ActivateFileDialogSelection();
			}
			break;
		default:
			break;
		}
	}

	void RenderFileDialog() {
		if (!fileDialogOpen_) return;
		const SDL_Rect dialog = FileDialogRect();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 255);
		SDL_RenderFillRect(renderer_, &dialog);
		DrawRect(dialog, 190, 190, 190);
		DrawText(fileDialogSave_ ? "SAVE PROCESSED IMAGE" : "OPEN IMAGE", dialog.x + 18, dialog.y + 14, 2);
		DrawText(fileDialogDirectory_.string(), dialog.x + 18, dialog.y + 42, 2, 170, 170, 170);
		if (fileDialogSave_) {
			DrawText("FILE NAME", dialog.x + 18, dialog.y + 68, 2, 190, 190, 190);
			SDL_Rect inputRect{dialog.x + 12, dialog.y + 86, dialog.w - 24, 28};
			SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 255);
			SDL_RenderFillRect(renderer_, &inputRect);
			DrawRect(inputRect, 100, 130, 165);
			DrawText(fileDialogFilename_, inputRect.x + 10, inputRect.y + 6, 2);
		}

		const int listTop = FileDialogListTop();
		const int rows = FileDialogVisibleRows();
		SDL_Rect listRect{dialog.x + 12, listTop, dialog.w - 24, rows * 26};
		SDL_SetRenderDrawColor(renderer_, 25, 25, 25, 255);
		SDL_RenderFillRect(renderer_, &listRect);
		DrawRect(listRect, 75, 75, 75);
		for (int row = 0; row < rows; ++row) {
			const int item = fileDialogScroll_ + row;
			if (item >= static_cast<int>(fileDialogEntries_.size())) break;
			const FileDialogEntry& entry = fileDialogEntries_[item];
			const int rowTop = listTop + row * 26;
			if (item == fileDialogSelected_) {
				SDL_SetRenderDrawColor(renderer_, 45, 82, 120, 255);
				SDL_Rect selection{listRect.x + 2, rowTop + 1, listRect.w - 4, 24};
				SDL_RenderFillRect(renderer_, &selection);
			}
			DrawText(FileDialogEntryLabel(entry), listRect.x + 10, rowTop + 5, 2,
				entry.directory ? 185 : 235, entry.directory ? 205 : 235, entry.directory ? 235 : 235);
		}
		if (!fileDialogMessage_.empty()) {
			DrawText(fileDialogMessage_, dialog.x + 18, dialog.y + dialog.h - 60, 2, 235, 150, 120);
		}
		DrawText(fileDialogSave_ ? "ENTER SAVE   BACKSPACE EDIT/PARENT   ESC CANCEL" :
			"ENTER OPEN   BACKSPACE PARENT   ESC CANCEL", dialog.x + 18, dialog.y + dialog.h - 34, 2, 170, 170, 170);
	}

	void DrawText(const std::string& text, int x, int y, int scale, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235) {
		SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
		int cursorX = x;
		for (const char character : text) {
			const std::array<Uint8, 7>& rows = GlyphRows(character);
			for (int row = 0; row < 7; ++row) {
				for (int column = 0; column < 5; ++column) {
					if ((rows[row] & (1u << (4 - column))) == 0) continue;
					SDL_Rect pixel{cursorX + column * scale, y + row * scale, scale, scale};
					SDL_RenderFillRect(renderer_, &pixel);
				}
			}
			cursorX += 6 * scale;
		}
	}

	void DrawLine(int x1, int y1, int x2, int y2, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235) {
		SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
		SDL_RenderDrawLine(renderer_, x1, y1, x2, y2);
	}

	void DrawRect(const SDL_Rect& rect, Uint8 r = 235, Uint8 g = 235, Uint8 b = 235) {
		SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
		SDL_RenderDrawRect(renderer_, &rect);
	}

	void DrawNavigationIcon(const ControlButton& button, bool hovered) {
		const SDL_Rect r = button.rect;
		if (hovered) {
			SDL_SetRenderDrawColor(renderer_, 65, 65, 65, 255);
			SDL_RenderFillRect(renderer_, &r);
		}
		DrawRect(r, 150, 150, 150);
		const int left = r.x + 8;
		const int right = r.x + r.w - 8;
		const int top = r.y + 8;
		const int bottom = r.y + r.h - 8;
		const int middle = r.y + r.h / 2;
		switch (button.command) {
		case IDM_FIRST:
			DrawLine(left, top, left, bottom);
			DrawLine(left + 8, top, left + 8, bottom);
			DrawLine(right, top, right - 10, middle);
			DrawLine(right - 10, middle, right, bottom);
			break;
		case IDM_PREV:
			DrawLine(left + 5, top, left + 5, bottom);
			DrawLine(right - 1, top, right - 12, middle);
			DrawLine(right - 12, middle, right - 1, bottom);
			break;
		case IDM_NEXT:
			DrawLine(left + 1, top, left + 12, middle);
			DrawLine(left + 12, middle, left + 1, bottom);
			DrawLine(right - 5, top, right - 5, bottom);
			break;
		case IDM_LAST:
			DrawLine(left, top, left + 10, middle);
			DrawLine(left + 10, middle, left, bottom);
			DrawLine(right - 8, top, right - 8, bottom);
			DrawLine(right, top, right, bottom);
			break;
		case IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS:
			if (fitToWindow_) {
				DrawLine(left + 5, top + 4, left + 14, top + 4);
				DrawLine(left + 5, top + 4, left + 5, top + 13);
				DrawLine(right - 5, top + 4, right - 14, top + 4);
				DrawLine(right - 5, top + 4, right - 5, top + 13);
				DrawLine(left + 5, bottom - 4, left + 14, bottom - 4);
				DrawLine(left + 5, bottom - 4, left + 5, bottom - 13);
				DrawLine(right - 5, bottom - 4, right - 14, bottom - 4);
				DrawLine(right - 5, bottom - 4, right - 5, bottom - 13);
			} else {
				DrawLine(left + 7, top + 3, left + 7, bottom - 3);
				DrawLine(left + 7, top + 3, left + 16, top + 3);
				DrawLine(left + 7, bottom - 3, left + 16, bottom - 3);
				DrawLine(right - 7, top + 3, right - 7, bottom - 3);
				DrawLine(right - 7, top + 3, right - 16, top + 3);
				DrawLine(right - 7, bottom - 3, right - 16, bottom - 3);
			}
			break;
		case IDM_FULL_SCREEN_MODE:
			DrawRect(SDL_Rect{left, top, right - left, bottom - top});
			DrawLine(left, top + 7, right, top + 7);
			break;
		case IDM_ROTATE_90:
			DrawLine(left + 4, bottom - 2, right - 2, bottom - 2);
			DrawLine(right - 2, bottom - 2, right - 2, top + 7);
			DrawLine(right - 2, top + 7, right - 9, top + 7);
			DrawLine(right - 9, top + 7, right - 5, top + 3);
			DrawLine(right - 9, top + 7, right - 5, top + 11);
			break;
		case IDM_ROTATE_270:
			DrawLine(left + 2, top + 7, left + 2, bottom - 2);
			DrawLine(left + 2, bottom - 2, right - 4, bottom - 2);
			DrawLine(left + 2, top + 7, left + 9, top + 7);
			DrawLine(left + 9, top + 7, left + 5, top + 3);
			DrawLine(left + 9, top + 7, left + 5, top + 11);
			break;
		default:
			break;
		}
	}

	void RenderFileName() {
		if (!showFileName_ || fileList_.Empty() || contextMenuOpen_ || fileDialogOpen_) return;
		int windowWidth = 0;
		SDL_GetWindowSize(window_, &windowWidth, nullptr);
		std::ostringstream text;
		text << '[' << fileList_.CurrentIndex() + 1 << '/' << fileList_.Size() << "] "
			<< InfoText(fileList_.Current().filename().string());
		std::string label = text.str();
		const int panelWidth = std::min(std::max(260, windowWidth - 16), 900);
		const int textWidth = panelWidth - 20;
		if (TextWidth(label, 2) > textWidth) {
			const std::size_t maximumCharacters = static_cast<std::size_t>(std::max(3, textWidth / 12));
			label.resize(maximumCharacters - 3);
			label += "...";
		}
		const SDL_Rect panel{8, 8, panelWidth, 28};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 235);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 105, 105, 105);
		DrawText(label, panel.x + 10, panel.y + 6, 2, 255, 255, 255);
	}

	void RenderImageInfo() {
		if (!infoVisible_ || contextMenuOpen_ || fileDialogOpen_) return;
		std::vector<std::string> lines = ImageInfoLines();
		if (lines.empty()) return;

		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int panelWidth = std::min(std::max(260, windowWidth - 16), 620);
		const int textWidth = panelWidth - 20;
		for (std::string& line : lines) {
			line = InfoText(line);
			if (TextWidth(line, 2) <= textWidth) continue;
			const std::size_t maximumCharacters = static_cast<std::size_t>(std::max(3, textWidth / 12));
			line.resize(maximumCharacters - 3);
			line += "...";
		}

		const int lineHeight = 18;
		const int panelHeight = std::min(windowHeight - 16, 20 + static_cast<int>(lines.size()) * lineHeight);
		const int panelTop = showFileName_ ? 42 : 8;
		SDL_Rect panel{8, panelTop, panelWidth, panelHeight};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 235);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 105, 105, 105);
		const int visibleLines = std::max(0, (panelHeight - 12) / lineHeight);
		for (int index = 0; index < visibleLines && index < static_cast<int>(lines.size()); ++index) {
			DrawText(lines[static_cast<std::size_t>(index)], panel.x + 10, panel.y + 10 + index * lineHeight, 2,
				index == 0 ? 255 : 243, index == 0 ? 255 : 242, index == 0 ? 255 : 231);
		}
	}

	void RenderControls() {
		if (!navigationPanelEnabled_ || !controlsVisible_ || contextMenuOpen_ || fileDialogOpen_) return;
		std::vector<ControlButton> buttons;
		LayoutControls(buttons);
		const SDL_Rect panel = ControlPanelRect();

		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 235);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 105, 105, 105);
		for (const ControlButton& button : buttons) {
			DrawNavigationIcon(button, PointInRect(lastMouseX_, lastMouseY_, button.rect));
		}
	}

	void RenderConfirmation() {
		if (!confirmationOpen_) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(760, std::max(360, windowWidth - 40));
		const int height = 136;
		const SDL_Rect panel{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 245);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 220, 170, 110);
		DrawText("CONFIRM ACTION", panel.x + 18, panel.y + 14, 2, 255, 220, 150);
		DrawText(confirmationMessage_, panel.x + 18, panel.y + 42, 2);
		std::string filename = fileList_.Empty() ? std::string() : InfoText(fileList_.Current().filename().string());
		const int maximumCharacters = std::max(3, (width - 36) / 12);
		if (static_cast<int>(filename.size()) > maximumCharacters) {
			filename.resize(static_cast<std::size_t>(maximumCharacters - 3));
			filename += "...";
		}
		DrawText(filename, panel.x + 18, panel.y + 68, 2, 220, 220, 220);
		DrawText("ENTER or SPACE: YES     ESC: CANCEL", panel.x + 18, panel.y + 104, 2, 180, 180, 180);
	}

	void RenderAbout() {
		if (!aboutOpen_) return;
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int width = std::min(620, std::max(360, windowWidth - 40));
		const int height = 196;
		const SDL_Rect panel{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 245);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 160, 190, 225);
		DrawText("JPEGVIEW LINUX", panel.x + 18, panel.y + 16, 2, 255, 255, 255);
		DrawText("NATIVE SDL2 VIEWER", panel.x + 18, panel.y + 48, 2, 210, 225, 250);
		DrawText("PORT OF JPEGVIEW 1.3.46", panel.x + 18, panel.y + 80, 2, 210, 225, 250);
		DrawText("FOLDER NAVIGATION AND IMAGE VIEWING", panel.x + 18, panel.y + 112, 2, 185, 205, 220);
		DrawText("PRESS ESC TO CLOSE", panel.x + 18, panel.y + 156, 2, 180, 180, 180);
	}

	void RenderImageTransition(const SDL_Rect& destination, int windowWidth, int windowHeight) {
		if (transitionTexture_ == nullptr || transitionStartTick_ == 0) {
			SDL_RenderCopy(renderer_, texture_, nullptr, &destination);
			return;
		}
		const Uint32 elapsed = SDL_GetTicks() - transitionStartTick_;
		const double progress = std::min(1.0, static_cast<double>(elapsed) /
			static_cast<double>(transitionDurationMs_));
		if (progress >= 1.0) {
			ClearTransition();
			SDL_RenderCopy(renderer_, texture_, nullptr, &destination);
			return;
		}

		SDL_Rect oldDestination{
			static_cast<int>(std::round((windowWidth - transitionImage_.width * zoom_) / 2.0 + offsetX_)),
			static_cast<int>(std::round((windowHeight - transitionImage_.height * zoom_) / 2.0 + offsetY_)),
			std::max(1, static_cast<int>(std::round(transitionImage_.width * zoom_))),
			std::max(1, static_cast<int>(std::round(transitionImage_.height * zoom_))),
		};
		SDL_Rect enteringDestination = destination;
		const int horizontalDistance = std::max(windowWidth, destination.w);
		const int verticalDistance = std::max(windowHeight, destination.h);
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
		SDL_SetTextureAlphaMod(texture_, 255);
		if (effect == IDM_EFFECT_BLEND) {
			SDL_SetTextureAlphaMod(transitionTexture_, static_cast<Uint8>(std::round(255.0 * (1.0 - progress))));
		}
		SDL_RenderCopy(renderer_, transitionTexture_, nullptr, &oldDestination);
		SDL_RenderCopy(renderer_, texture_, nullptr, &enteringDestination);
	}

	void RenderContextMenu() {
		if (!contextMenuOpen_) return;
		const SDL_Rect menu = ContextMenuRect();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 255);
		SDL_RenderFillRect(renderer_, &menu);
		DrawRect(menu, 185, 185, 185);

		int itemTop = menu.y + 8;
		const int visibleCount = ContextMenuVisibleCount();
		const std::size_t visibleEnd = std::min(contextMenuItems_.size(),
			contextMenuScroll_ + static_cast<std::size_t>(visibleCount));
		for (std::size_t i = contextMenuScroll_; i < visibleEnd; ++i) {
			const MenuItem& item = contextMenuItems_[i];
			if (item.separator) {
				DrawLine(menu.x + 10, itemTop + 4, menu.x + menu.w - 10, itemTop + 4, 75, 75, 75);
				itemTop += 9;
				continue;
			}
			if (static_cast<int>(i) == menuSelected_) {
				SDL_SetRenderDrawColor(renderer_, 45, 82, 120, 255);
				SDL_Rect selection{menu.x + 3, itemTop, menu.w - 6, 28};
				SDL_RenderFillRect(renderer_, &selection);
			}
			const Uint8 textColor = item.command == 0 ? 135 : (item.enabled ? 235 : 100);
			DrawText(MenuLabel(item), menu.x + 14, itemTop + 6, 2, textColor, textColor, textColor);
			itemTop += 28;
		}
	}

	void HandleEvents(bool& running) {
		SDL_Event event{};
		while (SDL_PollEvent(&event) != 0) {
			lastInteractionTick_ = SDL_GetTicks();
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
			if (contextMenuOpen_) {
				switch (event.type) {
				case SDL_QUIT:
					running = false;
					break;
				case SDL_KEYDOWN:
					if (event.key.repeat != 0) break;
					if (event.key.keysym.sym == SDLK_ESCAPE) {
						contextMenuOpen_ = false;
					} else if (event.key.keysym.sym == SDLK_UP) {
						MoveContextMenuSelection(-1);
					} else if (event.key.keysym.sym == SDLK_DOWN) {
						MoveContextMenuSelection(1);
					} else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
						ActivateContextMenuSelection(running);
					}
					break;
				case SDL_MOUSEMOTION:
					UpdateContextMenuSelection(event.motion.x, event.motion.y);
					break;
				case SDL_MOUSEWHEEL: {
					const int maximumScroll = std::max(0, static_cast<int>(contextMenuItems_.size()) - ContextMenuVisibleCount());
					contextMenuScroll_ = static_cast<std::size_t>(std::clamp(
						static_cast<int>(contextMenuScroll_) - event.wheel.y, 0, maximumScroll));
					menuSelected_ = -1;
					break;
				}
				case SDL_MOUSEBUTTONDOWN:
					if (event.button.button == SDL_BUTTON_LEFT) {
						const int item = ContextMenuItemAt(event.button.x, event.button.y);
						if (item >= 0) {
							menuSelected_ = item;
							ActivateContextMenuSelection(running);
						} else {
							contextMenuOpen_ = false;
						}
					} else {
						contextMenuOpen_ = false;
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
				if (event.window.event == SDL_WINDOWEVENT_RESIZED || event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					if (fitToWindow_) FitToWindow(fillWithCrop_, autoZoomNoEnlarge_);
				}
				break;
			case SDL_KEYDOWN:
			{
				if (event.key.repeat != 0) break;
				const Uint16 modifiers = event.key.keysym.mod;
				const bool plainKey = (modifiers & 0x03C3u) == 0;
				if (plainKey && event.key.keysym.sym >= '1' && event.key.keysym.sym <= '9') {
					// CMainDlg::OnKeyDown reserves the number row for quick
					// slideshow intervals before consulting KeyMap.txt.default.
					StartSlideshow(static_cast<double>(event.key.keysym.sym - '0'));
					break;
				}
				const int command = CommandForKey(event.key);
				if (command != 0) ExecuteCommand(command);
				if (quitRequested_) running = false;
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
				if (event.button.button == SDL_BUTTON_LEFT) {
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
					OpenContextMenu(event.button.x, event.button.y);
				}
				break;
			case SDL_MOUSEBUTTONUP:
				if (event.button.button == SDL_BUTTON_LEFT) dragging_ = false;
				break;
			case SDL_MOUSEMOTION:
				imageCenterX_ = event.motion.x;
				imageCenterY_ = event.motion.y;
				ShowControls();
				if (dragging_) {
					offsetX_ += event.motion.xrel;
					offsetY_ += event.motion.yrel;
					fitToWindow_ = false;
					SetTitle();
				}
				lastMouseX_ = event.motion.x;
				lastMouseY_ = event.motion.y;
				break;
			case SDL_MOUSEWHEEL:
				if (event.wheel.y > 0) {
					ZoomAt(1.2, lastMouseX_, lastMouseY_);
				} else if (event.wheel.y < 0) {
					ZoomAt(1.0 / 1.2, lastMouseX_, lastMouseY_);
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

	void Render() {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		imageCenterX_ = windowWidth / 2;
		imageCenterY_ = windowHeight / 2;
		const int renderWidth = std::max(1, static_cast<int>(std::round(image_.width * zoom_)));
		const int renderHeight = std::max(1, static_cast<int>(std::round(image_.height * zoom_)));
		SDL_Rect destination{
			static_cast<int>(std::round((windowWidth - renderWidth) / 2.0 + offsetX_)),
			static_cast<int>(std::round((windowHeight - renderHeight) / 2.0 + offsetY_)),
			renderWidth,
			renderHeight
		};
		SDL_SetRenderDrawColor(renderer_, 18, 18, 18, 255);
		SDL_RenderClear(renderer_);
		RenderImageTransition(destination, windowWidth, windowHeight);
		RenderFileName();
		RenderImageInfo();
		RenderControls();
		RenderContextMenu();
		RenderFileDialog();
		RenderConfirmation();
		RenderAbout();
		SDL_RenderPresent(renderer_);
	}

	jpegview_linux::FileList fileList_;
	double slideshowSeconds_ = 0.0;
	double lastSlideshowSeconds_ = 3.0;
	int transitionEffect_ = IDM_EFFECT_NONE;
	Uint32 transitionDurationMs_ = 500;
	Uint32 transitionStartTick_ = 0;
	bool startFullscreen_ = false;
	Uint32 lastInteractionTick_ = 0;
	Image image_;
	SDL_Window* window_ = nullptr;
	SDL_Renderer* renderer_ = nullptr;
	SDL_Texture* texture_ = nullptr;
	Image transitionImage_;
	SDL_Texture* transitionTexture_ = nullptr;
	double zoom_ = 1.0;
	double offsetX_ = 0.0;
	double offsetY_ = 0.0;
	bool fitToWindow_ = true;
	bool fillWithCrop_ = false;
	bool autoZoomNoEnlarge_ = false;
	bool fullscreen_ = false;
	bool borderless_ = false;
	bool alwaysOnTop_ = false;
	bool dragging_ = false;
	bool controlsVisible_ = true;
	bool navigationPanelEnabled_ = true;
	bool infoVisible_ = false;
	bool showFileName_ = false;
	bool confirmationOpen_ = false;
	int confirmationCommand_ = 0;
	std::string confirmationMessage_;
	bool aboutOpen_ = false;
	jpegview_linux::ExifInfo metadata_;
	std::string jpegComment_;
	bool contextMenuOpen_ = false;
	int contextMenuX_ = 0;
	int contextMenuY_ = 0;
	std::size_t contextMenuScroll_ = 0;
	int menuSelected_ = -1;
	std::vector<MenuItem> contextMenuItems_;
	bool imageModified_ = false;
	bool quitRequested_ = false;
	bool fileDialogOpen_ = false;
	bool fileDialogSave_ = false;
	bool fileDialogSaveFullSize_ = true;
	bool fileDialogOverwriteConfirmed_ = false;
	fs::path fileDialogDirectory_;
	std::string fileDialogFilename_;
	std::string fileDialogMessage_;
	std::vector<FileDialogEntry> fileDialogEntries_;
	int fileDialogSelected_ = 0;
	int fileDialogScroll_ = 0;
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
		<< "  --help             Show this help\n\n"
		<< "Controls: Right/Left navigate, Up/Down rotate, mouse wheel zooms, left-drag pans, drop files to open,\n"
		<< "          Space toggles fit/actual, Enter fits, 0 fits, 1-9 start a slideshow, F11/F fullscreen,\n"
		<< "          F7/F8/F9 select folder/recursive/sibling navigation, N/M/C/Z select display order,\n"
		<< "          F2 toggles picture information, Ctrl+O opens, Ctrl+S saves full size, Ctrl+Shift+S saves screen size, Ctrl+R reloads, Ctrl+N toggles the navigation panel,\n"
		<< "          right-click opens the context menu, Esc or Q quits.\n";
}

} // namespace

int main(int argc, char** argv) {
	bool startFullscreen = false;
	double slideshowSeconds = 0.0;
	bool decodeCheck = false;
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
		if (!value.empty() && value[0] == '-') {
			std::cerr << "Unknown option: " << value << '\n';
			PrintUsage(argv[0]);
			return 2;
		}
		inputs.push_back(value);
	}

	jpegview_linux::FileList fileList(inputs);
	if (fileList.Empty()) {
		std::cerr << "No supported images found.\n";
		return 2;
	}

	if (decodeCheck) {
		for (const fs::path& file : fileList.Files()) {
			Image image;
			std::string errorMessage;
			if (!image.Load(file, errorMessage)) {
				std::cerr << file << ": " << errorMessage << '\n';
				return 1;
			}
			std::cout << file << ": " << image.width << 'x' << image.height << '\n';
		}
		return 0;
	}

	Viewer viewer(std::move(fileList), slideshowSeconds, startFullscreen);
	const int result = viewer.Run();
	return result;
}
