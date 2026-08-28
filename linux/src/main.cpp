#include "sdl_abi.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../third_party/stb_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr double kMinZoom = 0.01;
constexpr double kMaxZoom = 32.0;
constexpr Uint32 kControlPanelTimeoutMs = 2400;

enum class ViewerAction {
	First,
	Previous,
	Next,
	Last,
	ToggleFit,
	ToggleFullscreen,
	Open,
	Reload,
	ToggleControls,
	ToggleSlideshow,
	Quit,
};

struct ControlButton {
	SDL_Rect rect{};
	ViewerAction action = ViewerAction::Next;
};

struct MenuItem {
	const char* label = nullptr;
	ViewerAction action = ViewerAction::Next;
	bool separator = false;
	bool checked = false;
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

std::vector<fs::path> ImagesInDirectory(const fs::path& directory) {
	std::vector<fs::path> result;
	std::error_code error;
	for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
		if (error) {
			break;
		}
		std::error_code statusError;
		if (entry.is_regular_file(statusError) && IsImagePath(entry.path())) {
			result.push_back(AbsoluteNormalized(entry.path()));
		}
	}
	std::sort(result.begin(), result.end(), [](const fs::path& left, const fs::path& right) {
		const std::string leftName = Lower(left.filename().string());
		const std::string rightName = Lower(right.filename().string());
		return leftName == rightName ? left.string() < right.string() : leftName < rightName;
	});
	return result;
}

std::vector<fs::path> BuildFileList(const std::vector<std::string>& inputs, std::size_t& initialIndex) {
	std::vector<fs::path> files;
	initialIndex = 0;

	if (inputs.empty()) {
		return ImagesInDirectory(fs::current_path());
	}

	if (inputs.size() == 1) {
		const fs::path input = AbsoluteNormalized(inputs.front());
		std::error_code error;
		if (fs::is_directory(input, error)) {
			return ImagesInDirectory(input);
		}
		if (fs::is_regular_file(input, error) && IsImagePath(input)) {
			std::vector<fs::path> result = ImagesInDirectory(input.parent_path());
			const auto it = std::find(result.begin(), result.end(), input);
			if (it != result.end()) {
				initialIndex = static_cast<std::size_t>(std::distance(result.begin(), it));
			}
			return result;
		}
	}

	std::vector<fs::path> result;
	for (const std::string& inputString : inputs) {
		const fs::path input = AbsoluteNormalized(inputString);
		std::error_code error;
		if (fs::is_directory(input, error)) {
			std::vector<fs::path> directoryFiles = ImagesInDirectory(input);
			result.insert(result.end(), directoryFiles.begin(), directoryFiles.end());
		} else if (fs::is_regular_file(input, error) && IsImagePath(input)) {
			result.push_back(input);
		}
	}
	std::sort(result.begin(), result.end(), [](const fs::path& left, const fs::path& right) {
		return Lower(left.string()) < Lower(right.string());
	});
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

struct Image {
	int width = 0;
	int height = 0;
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

class Viewer {
public:
	Viewer(std::vector<fs::path> files, std::size_t initialIndex, double slideshowSeconds, bool startFullscreen)
		: files_(std::move(files)), index_(initialIndex), slideshowSeconds_(slideshowSeconds), startFullscreen_(startFullscreen) {}

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
		if (texture_ != nullptr) {
			SDL_DestroyTexture(texture_);
			texture_ = nullptr;
		}
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
		if (files_.empty() || index_ >= files_.size()) {
			return false;
		}
		std::string errorMessage;
		if (!image_.Load(files_[index_], errorMessage)) {
			SetTitle(files_[index_].filename().string() + " — decode failed: " + errorMessage);
			std::cerr << files_[index_] << ": " << errorMessage << '\n';
			return false;
		}

		if (texture_ != nullptr) {
			SDL_DestroyTexture(texture_);
		}
		texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
			image_.width, image_.height);
		if (texture_ == nullptr) {
			std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << '\n';
			return false;
		}
		if (SDL_UpdateTexture(texture_, nullptr, image_.bgra.data(), image_.width * 4) != 0) {
			std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << '\n';
			return false;
		}

		FitToWindow();
		lastInteractionTick_ = SDL_GetTicks();
		SetTitle();
		return true;
	}

	void SetTitle(const std::string& title) {
		SDL_SetWindowTitle(window_, title.c_str());
	}

	void SetTitle() {
		std::ostringstream title;
		title << "JPEGView Linux — " << files_[index_].filename().string()
			<< " [" << index_ + 1 << '/' << files_.size() << "] "
			<< image_.width << 'x' << image_.height << " @ " << FormatPercent(zoom_)
			<< " — arrows: navigate, wheel: zoom, drag: pan, 0: fit, 1: actual, F: fullscreen, Esc: quit";
		SetTitle(title.str());
	}

	void OpenDroppedFiles(const std::vector<std::string>& droppedFiles) {
		if (droppedFiles.empty()) {
			return;
		}

		std::size_t droppedIndex = 0;
		std::vector<fs::path> droppedImageFiles;
		try {
			droppedImageFiles = BuildFileList(droppedFiles, droppedIndex);
		} catch (const fs::filesystem_error& error) {
			SetTitle(std::string("Cannot open dropped input: ") + error.what());
			return;
		}
		if (droppedImageFiles.empty()) {
			SetTitle("No supported images in dropped input");
			return;
		}

		files_ = std::move(droppedImageFiles);
		index_ = droppedIndex;
		dragging_ = false;
		fileDialogOpen_ = false;
		LoadCurrent();
	}

	void FitToWindow() {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const double widthScale = static_cast<double>(std::max(1, windowWidth - 16)) / image_.width;
		const double heightScale = static_cast<double>(std::max(1, windowHeight - 16)) / image_.height;
		zoom_ = std::clamp(std::min(widthScale, heightScale), kMinZoom, kMaxZoom);
		fitToWindow_ = true;
		offsetX_ = 0.0;
		offsetY_ = 0.0;
		SetTitle();
	}

	void ActualSize() {
		zoom_ = 1.0;
		fitToWindow_ = false;
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
		lastInteractionTick_ = SDL_GetTicks();
		SetTitle();
	}

	void NextImage() {
		if (files_.empty()) return;
		index_ = (index_ + 1) % files_.size();
		LoadCurrent();
	}

	void PreviousImage() {
		if (files_.empty()) return;
		index_ = index_ == 0 ? files_.size() - 1 : index_ - 1;
		LoadCurrent();
	}

	void FirstImage() {
		if (files_.empty() || index_ == 0) return;
		index_ = 0;
		LoadCurrent();
	}

	void LastImage() {
		if (files_.empty() || index_ + 1 == files_.size()) return;
		index_ = files_.size() - 1;
		LoadCurrent();
	}

	void ToggleFullscreen() {
		fullscreen_ = !fullscreen_;
		SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : static_cast<Uint32>(0));
		if (fitToWindow_) {
			FitToWindow();
		} else {
			SetTitle();
		}
	}

	void ShowControls() {
		if (!navigationPanelEnabled_) return;
		controlsVisible_ = true;
		controlsDeadline_ = SDL_GetTicks() + kControlPanelTimeoutMs;
	}

	SDL_Rect ControlPanelRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		const int buttonSize = 40;
		const int gap = 5;
		const int margin = 8;
		const int separator = 12;
		const int panelWidth = margin * 2 + buttonSize * 6 + gap * 4 + separator * 2;
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
		const ViewerAction actions[] = {
			ViewerAction::First, ViewerAction::Previous, ViewerAction::Next,
			ViewerAction::Last, ViewerAction::ToggleFit, ViewerAction::ToggleFullscreen
		};
		buttons.clear();
		int x = panel.x + margin;
		for (std::size_t i = 0; i < std::size(actions); ++i) {
			buttons.push_back(ControlButton{SDL_Rect{x, panel.y + margin, buttonSize, buttonSize}, actions[i]});
			x += buttonSize + gap;
			if (i == 3 || i == 4) x += separator;
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
			switch (button.action) {
			case ViewerAction::First:
				FirstImage();
				break;
			case ViewerAction::Previous:
				PreviousImage();
				break;
			case ViewerAction::Next:
				NextImage();
				break;
			case ViewerAction::Last:
				LastImage();
				break;
			case ViewerAction::ToggleFit:
				if (fitToWindow_) ActualSize(); else FitToWindow();
				break;
			case ViewerAction::ToggleFullscreen:
				ToggleFullscreen();
				break;
			default:
				break;
			}
			ShowControls();
			return true;
		}
		return PointInRect(x, y, ControlPanelRect());
	}

	void ExecuteAction(ViewerAction action) {
		switch (action) {
		case ViewerAction::First:
			FirstImage();
			break;
		case ViewerAction::Previous:
			PreviousImage();
			break;
		case ViewerAction::Next:
			NextImage();
			break;
		case ViewerAction::Last:
			LastImage();
			break;
		case ViewerAction::ToggleFit:
			if (fitToWindow_) ActualSize(); else FitToWindow();
			break;
		case ViewerAction::ToggleFullscreen:
			ToggleFullscreen();
			break;
		case ViewerAction::Open:
			OpenFileDialog();
			break;
		case ViewerAction::Reload:
			LoadCurrent();
			break;
		case ViewerAction::ToggleControls:
			navigationPanelEnabled_ = !navigationPanelEnabled_;
			if (navigationPanelEnabled_) ShowControls(); else controlsVisible_ = false;
			break;
		case ViewerAction::ToggleSlideshow:
			slideshowSeconds_ = slideshowSeconds_ > 0.0 ? 0.0 : 3.0;
			lastInteractionTick_ = SDL_GetTicks();
			SetTitle();
			break;
		case ViewerAction::Quit:
			quitRequested_ = true;
			break;
		}
	}

	std::vector<MenuItem> ContextMenuItems() const {
		return {
			{"Open image...", ViewerAction::Open},
			{"Reload image", ViewerAction::Reload},
			{nullptr, ViewerAction::Next, true},
			{"First image", ViewerAction::First},
			{"Previous image", ViewerAction::Previous},
			{"Next image", ViewerAction::Next},
			{"Last image", ViewerAction::Last},
			{nullptr, ViewerAction::Next, true},
			{"Fit to screen / Actual size", ViewerAction::ToggleFit, false, fitToWindow_},
			{"Full screen mode", ViewerAction::ToggleFullscreen, false, fullscreen_},
			{"Show navigation panel", ViewerAction::ToggleControls, false, navigationPanelEnabled_},
			{nullptr, ViewerAction::Next, true},
			{slideshowSeconds_ > 0.0 ? "Stop slide show/movie" : "Start slideshow (3 sec)", ViewerAction::ToggleSlideshow},
			{nullptr, ViewerAction::Next, true},
			{"Exit", ViewerAction::Quit},
		};
	}

	std::string MenuLabel(const MenuItem& item) const {
		if (item.checked) return std::string("[X] ") + item.label;
		return item.label == nullptr ? std::string() : item.label;
	}

	SDL_Rect ContextMenuRect() const {
		int windowWidth = 0;
		int windowHeight = 0;
		SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
		int width = 260;
		int height = 16;
		for (const MenuItem& item : contextMenuItems_) {
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
		return SDL_Rect{x, y, width, height};
	}

	int ContextMenuItemAt(int x, int y) const {
		const SDL_Rect menu = ContextMenuRect();
		if (!PointInRect(x, y, menu)) return -1;
		int itemTop = menu.y + 8;
		for (std::size_t i = 0; i < contextMenuItems_.size(); ++i) {
			const MenuItem& item = contextMenuItems_[i];
			const int itemHeight = item.separator ? 9 : 28;
			if (y >= itemTop && y < itemTop + itemHeight) return item.separator ? -1 : static_cast<int>(i);
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
		contextMenuOpen_ = true;
		menuSelected_ = -1;
	}

	void MoveContextMenuSelection(int direction) {
		if (contextMenuItems_.empty()) return;
		int candidate = menuSelected_;
		for (std::size_t tries = 0; tries < contextMenuItems_.size(); ++tries) {
			candidate += direction;
			if (candidate < 0) candidate = static_cast<int>(contextMenuItems_.size()) - 1;
			if (candidate >= static_cast<int>(contextMenuItems_.size())) candidate = 0;
			if (!contextMenuItems_[candidate].separator) {
				menuSelected_ = candidate;
				return;
			}
		}
	}

	void ActivateContextMenuSelection(bool& running) {
		if (menuSelected_ < 0 || menuSelected_ >= static_cast<int>(contextMenuItems_.size()) ||
			contextMenuItems_[menuSelected_].separator) {
			return;
		}
		const ViewerAction action = contextMenuItems_[menuSelected_].action;
		contextMenuOpen_ = false;
		ExecuteAction(action);
		if (action == ViewerAction::Quit) running = false;
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
		return FileDialogRect().y + 72;
	}

	int FileDialogVisibleRows() const {
		return std::max(1, (FileDialogRect().h - 128) / 26);
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
		if (!files_.empty() && index_ < files_.size()) {
			const fs::path currentDirectory = files_[index_].parent_path();
			if (!currentDirectory.empty()) directory = currentDirectory;
		}
		if (error || directory.empty()) directory = fs::path(".");
		fileDialogDirectory_ = AbsoluteNormalized(directory);
		fileDialogOpen_ = true;
		contextMenuOpen_ = false;
		RefreshFileDialog();
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
		if (fileDialogSelected_ < 0 || fileDialogSelected_ >= static_cast<int>(fileDialogEntries_.size())) return;
		const FileDialogEntry entry = fileDialogEntries_[fileDialogSelected_];
		if (entry.directory) {
			fileDialogDirectory_ = entry.path;
			RefreshFileDialog();
			return;
		}
		OpenDroppedFiles({entry.path.string()});
	}

	void HandleFileDialogEvents(const SDL_Event& event, bool& running) {
		switch (event.type) {
		case SDL_QUIT:
			running = false;
			break;
		case SDL_KEYDOWN:
			if (event.key.repeat != 0) break;
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				fileDialogOpen_ = false;
			} else if (event.key.keysym.sym == SDLK_UP) {
				MoveFileDialogSelection(-1);
			} else if (event.key.keysym.sym == SDLK_DOWN) {
				MoveFileDialogSelection(1);
			} else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
				ActivateFileDialogSelection();
			} else if (event.key.keysym.sym == SDLK_BACKSPACE) {
				const fs::path parent = fileDialogDirectory_.parent_path();
				if (!parent.empty() && parent != fileDialogDirectory_) {
					fileDialogDirectory_ = parent;
					RefreshFileDialog();
				}
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
				fileDialogOpen_ = false;
			} else if (event.button.button == SDL_BUTTON_LEFT) {
				fileDialogSelected_ = FileDialogItemAt(event.button.x, event.button.y);
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
		DrawText("OPEN IMAGE", dialog.x + 18, dialog.y + 14, 2);
		DrawText(fileDialogDirectory_.string(), dialog.x + 18, dialog.y + 42, 2, 170, 170, 170);

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
		DrawText("ENTER OPEN   BACKSPACE PARENT   ESC CANCEL", dialog.x + 18, dialog.y + dialog.h - 34, 2, 170, 170, 170);
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
		switch (button.action) {
		case ViewerAction::First:
			DrawLine(left, top, left, bottom);
			DrawLine(left + 8, top, left + 8, bottom);
			DrawLine(right, top, right - 10, middle);
			DrawLine(right - 10, middle, right, bottom);
			break;
		case ViewerAction::Previous:
			DrawLine(left + 5, top, left + 5, bottom);
			DrawLine(right - 1, top, right - 12, middle);
			DrawLine(right - 12, middle, right - 1, bottom);
			break;
		case ViewerAction::Next:
			DrawLine(left + 1, top, left + 12, middle);
			DrawLine(left + 12, middle, left + 1, bottom);
			DrawLine(right - 5, top, right - 5, bottom);
			break;
		case ViewerAction::Last:
			DrawLine(left, top, left + 10, middle);
			DrawLine(left + 10, middle, left, bottom);
			DrawLine(right - 8, top, right - 8, bottom);
			DrawLine(right, top, right, bottom);
			break;
		case ViewerAction::ToggleFit:
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
		case ViewerAction::ToggleFullscreen:
			DrawRect(SDL_Rect{left, top, right - left, bottom - top});
			DrawLine(left, top + 7, right, top + 7);
			break;
		default:
			break;
		}
	}

	void RenderControls() {
		if (!navigationPanelEnabled_ || !controlsVisible_ || contextMenuOpen_ || fileDialogOpen_) return;
		const Uint32 now = SDL_GetTicks();
		std::vector<ControlButton> buttons;
		LayoutControls(buttons);
		const SDL_Rect panel = ControlPanelRect();
		if (now >= controlsDeadline_ && !PointInRect(lastMouseX_, lastMouseY_, panel)) {
			controlsVisible_ = false;
			return;
		}

		SDL_SetRenderDrawColor(renderer_, 8, 8, 8, 235);
		SDL_RenderFillRect(renderer_, &panel);
		DrawRect(panel, 105, 105, 105);
		for (const ControlButton& button : buttons) {
			DrawNavigationIcon(button, PointInRect(lastMouseX_, lastMouseY_, button.rect));
		}
	}

	void RenderContextMenu() {
		if (!contextMenuOpen_) return;
		const SDL_Rect menu = ContextMenuRect();
		SDL_SetRenderDrawColor(renderer_, 12, 12, 12, 255);
		SDL_RenderFillRect(renderer_, &menu);
		DrawRect(menu, 185, 185, 185);

		int itemTop = menu.y + 8;
		for (std::size_t i = 0; i < contextMenuItems_.size(); ++i) {
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
			DrawText(MenuLabel(item), menu.x + 14, itemTop + 6, 2);
			itemTop += 28;
		}
	}

	void HandleEvents(bool& running) {
		SDL_Event event{};
		while (SDL_PollEvent(&event) != 0) {
			lastInteractionTick_ = SDL_GetTicks();
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
					if (fitToWindow_) FitToWindow();
				}
				break;
			case SDL_KEYDOWN:
				if (event.key.repeat != 0) break;
				if ((event.key.keysym.mod & 0x00C0u) != 0 && event.key.keysym.sym == SDLK_o) {
					OpenFileDialog();
					break;
				}
				switch (event.key.keysym.sym) {
				case SDLK_ESCAPE:
				case SDLK_q:
					running = false;
					break;
				case SDLK_RIGHT:
				case SDLK_DOWN:
				case SDLK_SPACE:
					NextImage();
					break;
				case SDLK_LEFT:
				case SDLK_UP:
					PreviousImage();
					break;
				case SDLK_EQUALS:
				case SDLK_KP_PLUS:
					ZoomAt(1.2, imageCenterX_, imageCenterY_);
					break;
				case SDLK_MINUS:
				case SDLK_KP_MINUS:
					ZoomAt(1.0 / 1.2, imageCenterX_, imageCenterY_);
					break;
				case SDLK_0:
					FitToWindow();
					break;
				case SDLK_1:
					ActualSize();
					break;
				case SDLK_f:
					ToggleFullscreen();
					break;
				case SDLK_r:
					LoadCurrent();
					break;
				default:
					break;
				}
				break;
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
		SDL_RenderCopy(renderer_, texture_, nullptr, &destination);
		RenderControls();
		RenderContextMenu();
		RenderFileDialog();
		SDL_RenderPresent(renderer_);
	}

	std::vector<fs::path> files_;
	std::size_t index_ = 0;
	double slideshowSeconds_ = 0.0;
	bool startFullscreen_ = false;
	Uint32 lastInteractionTick_ = 0;
	Image image_;
	SDL_Window* window_ = nullptr;
	SDL_Renderer* renderer_ = nullptr;
	SDL_Texture* texture_ = nullptr;
	double zoom_ = 1.0;
	double offsetX_ = 0.0;
	double offsetY_ = 0.0;
	bool fitToWindow_ = true;
	bool fullscreen_ = false;
	bool dragging_ = false;
	bool controlsVisible_ = true;
	Uint32 controlsDeadline_ = 0;
	bool navigationPanelEnabled_ = true;
	bool contextMenuOpen_ = false;
	int contextMenuX_ = 0;
	int contextMenuY_ = 0;
	int menuSelected_ = -1;
	std::vector<MenuItem> contextMenuItems_;
		bool quitRequested_ = false;
		bool fileDialogOpen_ = false;
		fs::path fileDialogDirectory_;
		std::vector<FileDialogEntry> fileDialogEntries_;
		int fileDialogSelected_ = 0;
		int fileDialogScroll_ = 0;
		std::vector<std::string> pendingDroppedFiles_;
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
		<< "Controls: arrows/space navigate, mouse wheel zooms, left-drag pans, drop files to open,\n"
		<< "          0 fits, 1 shows actual size, F toggles fullscreen, R reloads, Esc quits.\n";
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

	std::size_t initialIndex = 0;
	std::vector<fs::path> files;
	try {
		files = BuildFileList(inputs, initialIndex);
	} catch (const fs::filesystem_error& error) {
		std::cerr << "Cannot enumerate input: " << error.what() << '\n';
		return 2;
	}
	if (files.empty()) {
		std::cerr << "No supported images found.\n";
		return 2;
	}
	if (decodeCheck) {
		for (const fs::path& file : files) {
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

	Viewer viewer(std::move(files), initialIndex, slideshowSeconds, startFullscreen);
	const int result = viewer.Run();
	return result;
}
