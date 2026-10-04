#include "context_menu_renderer.h"

#include "bitmap_font.h"
#include "chrome_renderer.h"
#include "text_renderer.h"

#include <algorithm>

namespace jpegview_linux {

void ContextMenuRendererAdapter::Render(const std::vector<MenuItem>& items,
	const std::vector<ContextMenuColumnLayout>& columns,
	const SDL_Rect& menu, int selected, int rowHeight,
	int separatorHeight, int verticalPadding) {
	chromeRenderer_.FillRect(menu, UiColor{12, 12, 12, 220});
	chromeRenderer_.DrawRect(menu, 185, 185, 185);
	for (const ContextMenuColumnLayout& column : columns) {
		const int columnX = menu.x + column.x;
		int itemTop = menu.y + verticalPadding;
		for (std::size_t index = column.begin; index < column.end; ++index) {
			const MenuItem& item = items[index];
			if (item.separator) {
				chromeRenderer_.DrawLine(columnX + 10, itemTop + 4,
					columnX + column.width - 10, itemTop + 4, 75, 75, 75);
				itemTop += separatorHeight;
				continue;
			}
			if (static_cast<int>(index) == selected) {
				chromeRenderer_.FillRect(
					SDL_Rect{columnX + 3, itemTop, column.width - 6, rowHeight},
					UiColor{45, 82, 120, 205});
			}
			const Uint8 textColor = item.command == 0 ? 135 : (item.enabled ? 235 : 100);
			const std::string label = item.checked ? std::string("[X] ") + item.label : item.label;
			const int textY = itemTop + (rowHeight - textRenderer_.LineHeight(1)) / 2;
			textRenderer_.Draw(label, columnX + 12, textY, 1,
				textColor, textColor, textColor);
			if (item.mnemonicOffset >= 0) {
				const std::size_t visibleOffset = static_cast<std::size_t>(item.mnemonicOffset) +
					(item.checked ? 4u : 0u);
				if (visibleOffset < label.size()) {
					int underlineX = columnX + 12 +
						textRenderer_.TextWidth(label.substr(0, visibleOffset), 1);
					int underlineWidth = std::max(1,
						textRenderer_.TextWidth(label.substr(visibleOffset, 1), 1));
					if (Terminus9CanRender(label)) {
						const BitmapGlyphInkBounds ink = Terminus9GlyphInkBounds(
							static_cast<unsigned char>(label[visibleOffset]));
						if (ink.width > 0) {
							underlineX += ink.left;
							underlineWidth = ink.width;
						}
					}
					const int underlineY = textY +
						std::max(0, textRenderer_.LineHeight(1) - 2);
					chromeRenderer_.FillRect(
						SDL_Rect{underlineX, underlineY, underlineWidth, 1},
						UiColor{textColor, textColor, textColor, 255});
				}
			}
			if (!item.shortcut.empty()) {
				const int shortcutWidth = textRenderer_.TextWidth(item.shortcut, 1);
				const Uint8 shortcutColor = item.enabled ? 175 : 90;
				textRenderer_.Draw(item.shortcut,
					columnX + column.width - 12 - shortcutWidth, textY, 1,
					shortcutColor, shortcutColor, shortcutColor);
			}
			itemTop += rowHeight;
		}
		if (&column != &columns.back()) {
			chromeRenderer_.DrawLine(columnX + column.width,
				menu.y + verticalPadding, columnX + column.width,
				menu.y + menu.h - verticalPadding, 75, 75, 75);
		}
	}
}

} // namespace jpegview_linux
