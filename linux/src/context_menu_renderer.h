#pragma once

#include "context_menu_model.h"
#include "sdl_abi.h"
#include "viewer_chrome.h"

#include <vector>

namespace jpegview_linux {

class ChromeRendererAdapter;
class TextRenderer;

struct ContextMenuColumnLayout {
	std::size_t begin = 0;
	std::size_t end = 0;
	int x = 0;
	int width = 260;
	int height = 0;
};

// Paints a value snapshot from the context-menu controller. It does not read
// Viewer state or dispatch menu commands.
class ContextMenuRendererAdapter {
public:
	ContextMenuRendererAdapter(ChromeRendererAdapter& chromeRenderer,
		TextRenderer& textRenderer) noexcept
		: chromeRenderer_(chromeRenderer), textRenderer_(textRenderer) {}

	void Render(const std::vector<MenuItem>& items,
		const std::vector<ContextMenuColumnLayout>& columns,
		const SDL_Rect& menu, int selected, int rowHeight,
		int separatorHeight, int verticalPadding);

private:
	ChromeRendererAdapter& chromeRenderer_;
	TextRenderer& textRenderer_;
};

} // namespace jpegview_linux
