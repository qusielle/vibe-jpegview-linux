#include "perspective_dialog_layout.h"

#include <algorithm>

namespace jpegview_linux {
namespace {

SDL_Rect MakeRect(int x, int y, int width, int height) {
	return {x, y, std::max(0, width), std::max(0, height)};
}

} // namespace

PerspectiveCorrectionDialogLayout BuildPerspectiveCorrectionDialogLayout(
	int windowWidth, int windowHeight) {
	PerspectiveCorrectionDialogLayout layout;
	windowWidth = std::max(0, windowWidth);
	windowHeight = std::max(0, windowHeight);
	const int horizontalMargin = windowWidth < 220 ? 0 : 16;
	const int verticalMargin = windowHeight < 208 ? 0 : 8;
	const int width = std::min(620,
		std::max(0, windowWidth - horizontalMargin * 2));
	const int height = std::min(390,
		std::max(0, windowHeight - verticalMargin * 2));
	layout.dialog = MakeRect((windowWidth - width) / 2,
		(windowHeight - height) / 2, width, height);
	layout.compact = height < 354;
	layout.veryCompact = width < 220 || height < 158;

	const SDL_Rect& dialog = layout.dialog;
	const int sliderWidth = std::max(0,
		dialog.w - (layout.veryCompact ? 16 : 48));
	if (layout.veryCompact) {
		for (int edge = 0; edge < 2; ++edge) {
			layout.sliders[static_cast<std::size_t>(edge)] = MakeRect(
				dialog.x + 8, dialog.y + 42 + edge * 22, sliderWidth, 12);
		}
		const int toggleWidth = std::max(0, (dialog.w - 12 - 4) / 3);
		for (int index = 0; index < 3; ++index) {
			layout.toggles[static_cast<std::size_t>(index)] = MakeRect(
				dialog.x + 4 + index * (toggleWidth + 2), dialog.y + 80,
				toggleWidth, 16);
		}
	} else if (!layout.compact) {
		for (int edge = 0; edge < 2; ++edge) {
			layout.sliders[static_cast<std::size_t>(edge)] = MakeRect(
				dialog.x + 24, dialog.y + 86 + edge * 60, sliderWidth, 22);
		}
		for (int index = 0; index < 3; ++index) {
			layout.toggles[static_cast<std::size_t>(index)] = MakeRect(
				dialog.x + 18, dialog.y + 184 + index * 28,
				std::max(0, dialog.w - 36), 26);
		}
		layout.message = MakeRect(dialog.x + 18, dialog.y + dialog.h - 87,
			std::max(0, dialog.w - 36), 14);
		layout.helpY = dialog.w >= 500 ? dialog.y + dialog.h - 69 : -1;
	} else if (height >= 208) {
		for (int edge = 0; edge < 2; ++edge) {
			layout.sliders[static_cast<std::size_t>(edge)] = MakeRect(
				dialog.x + 24, dialog.y + 50 + edge * 40, sliderWidth, 20);
		}
		const int toggleWidth = std::max(0, (dialog.w - 36 - 16) / 3);
		for (int index = 0; index < 3; ++index) {
			layout.toggles[static_cast<std::size_t>(index)] = MakeRect(
				dialog.x + 18 + index * (toggleWidth + 8), dialog.y + 122,
				toggleWidth, 22);
		}
		layout.message = MakeRect(dialog.x + 18, dialog.y + 150,
			std::max(0, dialog.w - 36), 14);
		layout.helpY = dialog.w >= 500 ? dialog.y + dialog.h - 24 : -1;
	} else {
		for (int edge = 0; edge < 2; ++edge) {
			layout.sliders[static_cast<std::size_t>(edge)] = MakeRect(
				dialog.x + 24, dialog.y + 43 + edge * 31, sliderWidth, 16);
		}
		const int toggleWidth = std::max(0, (dialog.w - 36 - 16) / 3);
		for (int index = 0; index < 3; ++index) {
			layout.toggles[static_cast<std::size_t>(index)] = MakeRect(
				dialog.x + 18 + index * (toggleWidth + 8), dialog.y + 98,
				toggleWidth, 22);
		}
	}

	const int buttonMargin = layout.veryCompact ? 4 : 16;
	const int buttonGap = layout.veryCompact ? 4 : 10;
	const int buttonWidth = std::min(86, std::max(0,
		(dialog.w - buttonMargin * 2 - buttonGap) / 2));
	const int buttonHeight = layout.veryCompact ? 20 : 30;
	const int buttonBottomMargin = layout.veryCompact ? 4 :
		height < 208 ? 8 : 13;
	const int buttonY = dialog.y + dialog.h - buttonBottomMargin - buttonHeight;
	const int applyX = dialog.x + dialog.w - buttonMargin -
		buttonWidth * 2 - buttonGap;
	const int cancelX = dialog.x + dialog.w - buttonMargin - buttonWidth;
	layout.buttons[0] = MakeRect(applyX, buttonY, buttonWidth, buttonHeight);
	layout.buttons[1] = MakeRect(cancelX, buttonY, buttonWidth, buttonHeight);
	return layout;
}

} // namespace jpegview_linux
