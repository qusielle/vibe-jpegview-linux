#include "chrome_renderer.h"

#include "text_renderer.h"

namespace jpegview_linux {

SDL_Rect ChromeRendererAdapter::SdlRect(const UiRect& rect) {
	return SDL_Rect{rect.x, rect.y, rect.width, rect.height};
}

void ChromeRendererAdapter::DrawLine(int x1, int y1, int x2, int y2,
	Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
	if (renderer_ == nullptr) return;
	SDL_SetRenderDrawColor(renderer_, red, green, blue, alpha);
	SDL_RenderDrawLine(renderer_, x1, y1, x2, y2);
}

void ChromeRendererAdapter::DrawRect(const SDL_Rect& rect,
	Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
	if (renderer_ == nullptr || rect.w <= 0 || rect.h <= 0) return;
	SDL_SetRenderDrawColor(renderer_, red, green, blue, alpha);
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

void ChromeRendererAdapter::FillRect(const SDL_Rect& rect, const UiColor& color) {
	if (renderer_ == nullptr || rect.w <= 0 || rect.h <= 0) return;
	SDL_SetRenderDrawColor(renderer_, color.red, color.green, color.blue, color.alpha);
	SDL_RenderFillRect(renderer_, &rect);
}

void ChromeRendererAdapter::Render(const OverlayPaintPlan& plan) {
	if (renderer_ == nullptr) return;
	const SDL_Rect panel = SdlRect(plan.panel);
	SDL_SetRenderDrawColor(renderer_, plan.background.red, plan.background.green,
		plan.background.blue, plan.background.alpha);
	SDL_RenderFillRect(renderer_, &panel);
	DrawRect(panel, plan.border.red, plan.border.green, plan.border.blue);
	for (const UiText& text : plan.text) {
		textRenderer_.Draw(text.text, text.x, text.y, 1,
			text.color.red, text.color.green, text.color.blue);
	}
}

void ChromeRendererAdapter::Render(const NavigationButtonPaint& button) {
	if (renderer_ == nullptr) return;
	const SDL_Rect rect = SdlRect(button.rect);
	auto drawLayer = [&](int offsetX, int offsetY, Uint8 red, Uint8 green, Uint8 blue,
		Uint8 alpha) {
		const SDL_Rect frame{rect.x + offsetX, rect.y + offsetY, rect.w, rect.h};
		DrawRect(frame, red, green, blue, alpha);
		for (const UiRect& outline : button.outlines) {
			const SDL_Rect shape = SdlRect(outline);
			DrawRect({shape.x + offsetX, shape.y + offsetY, shape.w, shape.h},
				red, green, blue, alpha);
		}
		for (const UiLine& line : button.lines) {
			DrawLine(line.x1 + offsetX, line.y1 + offsetY,
				line.x2 + offsetX, line.y2 + offsetY, red, green, blue, alpha);
		}
		for (const UiText& text : button.text) {
			textRenderer_.Draw(text.text, text.x + offsetX, text.y + offsetY, 1,
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

} // namespace jpegview_linux
