#pragma once

#include "sdl_abi.h"

#include <array>
#include <string>
#include <vector>

namespace jpegview_linux {

class ChromeRendererAdapter;
class TextRenderer;

struct DialogButtonPaint {
	SDL_Rect rect{};
	std::string label;
	bool hovered = false;
};

struct UnsharpSliderPaint {
	SDL_Rect rect{};
	std::string label;
	std::string value;
	int knobX = 0;
};

struct UnsharpMaskDialogPaint {
	bool visible = false;
	SDL_Rect dialog{};
	std::array<UnsharpSliderPaint, 3> sliders;
	std::array<DialogButtonPaint, 2> buttons;
};

struct ResizeFieldPaint {
	SDL_Rect rect{};
	std::string label;
	std::string value;
	std::string suffix;
	bool focused = false;
};

struct ResizeDialogPaint {
	bool visible = false;
	SDL_Rect dialog{};
	std::string originalSize;
	std::string message;
	std::array<ResizeFieldPaint, 4> fields;
	std::array<DialogButtonPaint, 2> buttons;
};

struct DialogTogglePaint {
	SDL_Rect rect{};
	std::string label;
	bool checked = false;
	bool enabled = true;
};

struct FreeRotationDialogPaint {
	bool visible = false;
	bool applying = false;
	SDL_Rect dialog{};
	SDL_Rect angleSlider{};
	int angleKnobX = 0;
	std::string angleValue;
	std::string message;
	std::array<DialogTogglePaint, 3> toggles;
	std::array<DialogButtonPaint, 2> buttons;
};

struct CropSizeFieldPaint {
	SDL_Rect rect{};
	std::string label;
	std::string value;
	bool focused = false;
};

struct CropSizeUnitPaint {
	SDL_Rect rect{};
	std::string label;
	bool selected = false;
};

struct FixedCropSizeDialogPaint {
	bool visible = false;
	SDL_Rect dialog{};
	std::string message;
	std::array<CropSizeFieldPaint, 2> fields;
	std::array<CropSizeUnitPaint, 2> units;
	std::array<DialogButtonPaint, 2> buttons;
};

struct BatchCopyRowPaint {
	bool selected = false;
	bool current = false;
	std::string source;
	std::string date;
	std::string destination;
	bool copy = false;
};

struct BatchCopyDialogPaint {
	bool visible = false;
	SDL_Rect dialog{};
	SDL_Rect list{};
	SDL_Rect pattern{};
	int rightX = 0;
	int rightWidth = 0;
	bool patternFocused = false;
	std::string directory;
	std::string patternText;
	std::string message;
	std::vector<BatchCopyRowPaint> rows;
	std::array<DialogButtonPaint, 6> buttons;
};

// Renderer-thread paint adapter for image editing and file-operation dialogs.
// Dialog models and geometry remain owned by their controllers and Viewer.
class EditingDialogRendererAdapter {
public:
	EditingDialogRendererAdapter(TextRenderer& textRenderer,
		ChromeRendererAdapter& chromeRenderer) noexcept
		: textRenderer_(textRenderer), chromeRenderer_(chromeRenderer) {}

	void SetRenderer(SDL_Renderer* renderer) noexcept { renderer_ = renderer; }
	void Render(const UnsharpMaskDialogPaint& paint);
	void Render(const ResizeDialogPaint& paint);
	void Render(const FreeRotationDialogPaint& paint);
	void Render(const FixedCropSizeDialogPaint& paint);
	void Render(const BatchCopyDialogPaint& paint);

private:
	void Fill(const SDL_Rect& rect, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha);
	void DrawButton(const DialogButtonPaint& button, bool compact);

	SDL_Renderer* renderer_ = nullptr;
	TextRenderer& textRenderer_;
	ChromeRendererAdapter& chromeRenderer_;
};

} // namespace jpegview_linux
