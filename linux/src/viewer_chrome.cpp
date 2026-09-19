#include "viewer_chrome.h"

#include "sort_mode.h"

#include "../../src/JPEGView/resource.h"

#include <algorithm>
#include <array>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr UiColor kIconColor{235, 235, 235, 255};

UiRect AsRect(const OverlayLayout& layout) {
	return {layout.x, layout.y, layout.width, layout.height};
}

void AddLine(NavigationButtonPaint& button, int x1, int y1, int x2, int y2) {
	button.lines.push_back({x1, y1, x2, y2, kIconColor});
}

void AddNavigationIcon(NavigationButtonPaint& button, bool fitToWindow,
	const std::string& sortLabel, int sortLabelWidth, int textLineHeight) {
	const UiRect& rect = button.rect;
	const int left = rect.x + 8;
	const int right = rect.x + rect.width - 8;
	const int top = rect.y + 8;
	const int bottom = rect.y + rect.height - 8;
	const int middle = rect.y + rect.height / 2;
	switch (button.command) {
	case IDM_FIRST:
		AddLine(button, left, top, left, bottom);
		AddLine(button, left + 8, top, left + 8, bottom);
		AddLine(button, right, top, right - 10, middle);
		AddLine(button, right - 10, middle, right, bottom);
		break;
	case IDM_PREV:
		AddLine(button, left + 5, top, left + 5, bottom);
		AddLine(button, right - 1, top, right - 12, middle);
		AddLine(button, right - 12, middle, right - 1, bottom);
		break;
	case IDM_NEXT:
		AddLine(button, left + 1, top, left + 12, middle);
		AddLine(button, left + 12, middle, left + 1, bottom);
		AddLine(button, right - 5, top, right - 5, bottom);
		break;
	case IDM_LAST:
		AddLine(button, left, top, left + 10, middle);
		AddLine(button, left + 10, middle, left, bottom);
		AddLine(button, right - 8, top, right - 8, bottom);
		AddLine(button, right, top, right, bottom);
		break;
	case IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS:
		if (fitToWindow) {
			AddLine(button, left + 5, top + 4, left + 14, top + 4);
			AddLine(button, left + 5, top + 4, left + 5, top + 13);
			AddLine(button, right - 5, top + 4, right - 14, top + 4);
			AddLine(button, right - 5, top + 4, right - 5, top + 13);
			AddLine(button, left + 5, bottom - 4, left + 14, bottom - 4);
			AddLine(button, left + 5, bottom - 4, left + 5, bottom - 13);
			AddLine(button, right - 5, bottom - 4, right - 14, bottom - 4);
			AddLine(button, right - 5, bottom - 4, right - 5, bottom - 13);
		} else {
			AddLine(button, left + 7, top + 3, left + 7, bottom - 3);
			AddLine(button, left + 7, top + 3, left + 16, top + 3);
			AddLine(button, left + 7, bottom - 3, left + 16, bottom - 3);
			AddLine(button, right - 7, top + 3, right - 7, bottom - 3);
			AddLine(button, right - 7, top + 3, right - 16, top + 3);
			AddLine(button, right - 7, bottom - 3, right - 16, bottom - 3);
		}
		break;
	case IDM_FULL_SCREEN_MODE:
		button.outlines.push_back({left, top, right - left, bottom - top});
		AddLine(button, left, top + 7, right, top + 7);
		break;
	case IDM_ROTATE_90:
		AddLine(button, left + 4, bottom - 2, right - 2, bottom - 2);
		AddLine(button, right - 2, bottom - 2, right - 2, top + 7);
		AddLine(button, right - 2, top + 7, right - 9, top + 7);
		AddLine(button, right - 9, top + 7, right - 5, top + 3);
		AddLine(button, right - 9, top + 7, right - 5, top + 11);
		break;
	case IDM_ROTATE_270:
		AddLine(button, left + 2, top + 7, left + 2, bottom - 2);
		AddLine(button, left + 2, bottom - 2, right - 4, bottom - 2);
		AddLine(button, left + 2, top + 7, left + 9, top + 7);
		AddLine(button, left + 9, top + 7, left + 5, top + 3);
		AddLine(button, left + 9, top + 7, left + 5, top + 11);
		break;
	case kNavigationSortModeCommand:
		button.text.push_back({sortLabel,
			rect.x + (rect.width - sortLabelWidth) / 2,
			rect.y + (rect.height - textLineHeight) / 2,
			kIconColor});
		break;
	default:
		break;
	}
}

} // namespace

OverlayPaintPlan FilenameOverlayPaint(const OverlayLayout& layout,
	std::string label, int textLineHeight, int textPadding) {
	OverlayPaintPlan plan;
	plan.panel = AsRect(layout);
	plan.text.push_back({std::move(label), layout.x + textPadding,
		layout.y + (layout.height - textLineHeight) / 2, {255, 255, 255, 255}});
	return plan;
}

OverlayPaintPlan InformationOverlayPaint(const OverlayLayout& layout,
	const std::vector<std::string>& lines, int lineHeight, int textPadding) {
	OverlayPaintPlan plan;
	plan.panel = AsRect(layout);
	const int count = std::min(layout.visibleLines, static_cast<int>(lines.size()));
	for (int index = 0; index < count; ++index) {
		plan.text.push_back({lines[static_cast<std::size_t>(index)], layout.x + textPadding,
			layout.y + textPadding + index * lineHeight,
			index == 0 ? UiColor{255, 255, 255, 255} : UiColor{243, 242, 231, 255}});
	}
	return plan;
}

NavigationPanelPaint BuildNavigationPanelPaint(int windowWidth, int windowHeight,
	int mouseX, int mouseY, bool fitToWindow, FileList::SortMode sortMode,
	int sortLabelWidth, int textLineHeight) {
	constexpr int buttonSize = 40;
	constexpr int gap = 5;
	constexpr int margin = 8;
	constexpr int separator = 12;
	constexpr std::array<int, 9> commands = {
		IDM_FIRST, IDM_PREV, IDM_NEXT, IDM_LAST, kNavigationSortModeCommand,
		IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, IDM_FULL_SCREEN_MODE,
		IDM_ROTATE_90, IDM_ROTATE_270
	};
	const int panelWidth = margin * 2 + buttonSize * static_cast<int>(commands.size()) +
		gap * (static_cast<int>(commands.size()) - 1) + separator * 2;
	NavigationPanelPaint plan;
	plan.panel = {(windowWidth - panelWidth) / 2, windowHeight - buttonSize - margin * 2,
		panelWidth, buttonSize + margin * 2};
	const std::string sortLabel = SortModeShortLabel(sortMode);
	int x = plan.panel.x + margin;
	for (std::size_t index = 0; index < commands.size(); ++index) {
		NavigationButtonPaint button;
		button.rect = {x, plan.panel.y + margin, buttonSize, buttonSize};
		button.command = commands[index];
		button.hovered = Contains(button.rect, mouseX, mouseY);
		AddNavigationIcon(button, fitToWindow, sortLabel, sortLabelWidth, textLineHeight);
		plan.buttons.push_back(std::move(button));
		x += buttonSize + gap;
		if (index == 3 || index == 6) x += separator;
	}
	return plan;
}

std::string NavigationTooltip(int command, bool fitToWindow, bool fullscreen,
	FileList::SortMode sortMode) {
	switch (command) {
	case IDM_FIRST: return "Show first image in folder (Home)";
	case IDM_PREV: return "Show previous image (Left)";
	case IDM_NEXT: return "Show next image (Right)";
	case IDM_LAST: return "Show last image in folder (End)";
	case IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS:
		return fitToWindow ? "Actual size of image (Space)" : "Fit image to screen (Space)";
	case IDM_FULL_SCREEN_MODE:
		return fullscreen ? "Window mode (F11)" : "Full screen mode (F11)";
	case IDM_ROTATE_90: return "Rotate image 90 deg clockwise (Down)";
	case IDM_ROTATE_270: return "Rotate image 90 deg counter-clockwise (Up)";
	case kNavigationSortModeCommand: {
		const std::string nextMode = sortMode == FileList::SortMode::FileName ?
			"modification date" : "file name";
		return "Current order: " + std::string(SortModeDescription(sortMode)) +
			"; click for " + nextMode + " order";
	}
	default: return {};
	}
}

OverlayPaintPlan NavigationTooltipPaint(const UiRect& anchor, std::string label,
	int labelWidth, int textLineHeight, int windowWidth, int windowHeight) {
	const int maximumWidth = std::max(1, windowWidth - 8);
	const int width = std::min(maximumWidth, labelWidth + 16);
	const int height = std::max(22, textLineHeight + 8);
	int x = anchor.x + (anchor.width - width) / 2;
	x = std::clamp(x, 4, std::max(4, windowWidth - width - 4));
	int y = anchor.y - height - 6;
	if (y < 4) y = anchor.y + anchor.height + 6;
	if (y + height > windowHeight) y = std::max(4, windowHeight - height - 4);
	OverlayPaintPlan plan;
	plan.panel = {x, y, width, height};
	plan.background = {8, 8, 8, 215};
	plan.border = {190, 190, 190, 255};
	plan.text.push_back({std::move(label), x + 8, y + (height - textLineHeight) / 2,
		{255, 255, 255, 255}});
	return plan;
}

bool Contains(const UiRect& rect, int x, int y) {
	return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

} // namespace jpegview_linux
