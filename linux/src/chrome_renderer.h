#pragma once

#include "sdl_abi.h"
#include "viewer_chrome.h"

namespace jpegview_linux {

class TextRenderer;

// SDL adapter for viewer overlays and navigation chrome. It receives paint
// plans and a text renderer; it does not own or inspect Viewer state.
class ChromeRendererAdapter {
public:
	explicit ChromeRendererAdapter(TextRenderer& textRenderer) noexcept
		: textRenderer_(textRenderer) {}

	void SetRenderer(SDL_Renderer* renderer) noexcept { renderer_ = renderer; }
	void DrawLine(int x1, int y1, int x2, int y2,
		Uint8 red = 235, Uint8 green = 235, Uint8 blue = 235, Uint8 alpha = 255);
	void DrawRect(const SDL_Rect& rect,
		Uint8 red = 235, Uint8 green = 235, Uint8 blue = 235, Uint8 alpha = 255);
	void FillRect(const SDL_Rect& rect, const UiColor& color);
	void Render(const OverlayPaintPlan& plan);
	void Render(const NavigationButtonPaint& button);

private:
	static SDL_Rect SdlRect(const UiRect& rect);

	SDL_Renderer* renderer_ = nullptr;
	TextRenderer& textRenderer_;
};

} // namespace jpegview_linux
