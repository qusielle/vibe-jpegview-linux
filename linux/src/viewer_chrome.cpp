#include "viewer_chrome.h"

#include "input_commands.h"
#include "sort_mode.h"

#include "../../src/JPEGView/resource.h"

#include <algorithm>
#include <array>
#include <utility>

namespace jpegview_linux {
namespace {

constexpr UiColor kGuiColor{243, 242, 231, 255};
constexpr UiColor kHighlightColor{255, 205, 0, 255};

UiRect AsRect(const OverlayLayout& layout) {
	return {layout.x, layout.y, layout.width, layout.height};
}

void AddLine(NavigationButtonPaint& button, int x1, int y1, int x2, int y2) {
	button.lines.push_back({x1, y1, x2, y2, button.foreground});
}

UiRect WindowsInflatedRect(const UiRect& rect, float amount) {
	const int inset = static_cast<int>(amount * rect.width);
	UiRect result{rect.x + inset, rect.y + inset,
		rect.width - inset * 2, rect.height - inset * 2};
	if ((result.height & 1) != 0) --result.height;
	return result;
}

void AddNavigationIcon(NavigationButtonPaint& button, bool fitToWindow,
	const std::string& sortLabel, int sortLabelWidth, int oneToOneLabelWidth,
	int textLineHeight) {
	const UiRect& rect = button.rect;
	switch (button.command) {
	case IDM_FIRST: {
		const UiRect icon = WindowsInflatedRect(rect, 0.3f);
		const int right = icon.x + icon.width;
		const int bottom = icon.y + icon.height;
		const int secondBar = icon.x + static_cast<int>(icon.width * 0.4f);
		const int halfHeight = icon.height / 2;
		AddLine(button, icon.x, icon.y, icon.x, bottom);
		AddLine(button, secondBar, icon.y, secondBar, bottom);
		AddLine(button, right, icon.y + 1, right - halfHeight + 1, icon.y + halfHeight);
		AddLine(button, right - halfHeight + 1, icon.y + halfHeight, right + 1, bottom);
		break;
	}
	case IDM_PREV: {
		const UiRect icon = WindowsInflatedRect(rect, 0.3f);
		const int right = icon.x + icon.width;
		const int bottom = icon.y + icon.height;
		const int horizontalGap = static_cast<int>(icon.width * 0.2f);
		const int bar = icon.x + horizontalGap;
		const int arrow = right - horizontalGap;
		const int halfHeight = icon.height / 2;
		AddLine(button, bar, icon.y, bar, bottom);
		AddLine(button, arrow, icon.y + 1, arrow - halfHeight + 1, icon.y + halfHeight);
		AddLine(button, arrow - halfHeight + 1, icon.y + halfHeight, arrow + 1, bottom);
		break;
	}
	case IDM_NEXT: {
		const UiRect icon = WindowsInflatedRect(rect, 0.3f);
		const int right = icon.x + icon.width;
		const int bottom = icon.y + icon.height;
		const int horizontalGap = static_cast<int>(icon.width * 0.2f);
		const int arrow = icon.x + horizontalGap - 1;
		const int bar = right - horizontalGap;
		const int halfHeight = icon.height / 2;
		AddLine(button, arrow + 1, icon.y + 1, arrow + halfHeight, icon.y + halfHeight);
		AddLine(button, arrow + halfHeight, icon.y + halfHeight, arrow, bottom);
		AddLine(button, bar, icon.y, bar, bottom);
		break;
	}
	case IDM_LAST: {
		const UiRect icon = WindowsInflatedRect(rect, 0.3f);
		const int right = icon.x + icon.width;
		const int bottom = icon.y + icon.height;
		const int halfHeight = icon.height / 2;
		const int firstBar = right - static_cast<int>(icon.width * 0.4f);
		AddLine(button, icon.x, icon.y + 1, icon.x + halfHeight - 1, icon.y + halfHeight);
		AddLine(button, icon.x + halfHeight - 1, icon.y + halfHeight, icon.x - 1, bottom);
		AddLine(button, firstBar, icon.y, firstBar, bottom);
		AddLine(button, right, icon.y, right, bottom);
		break;
	}
	case IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS:
		if (fitToWindow) {
			button.text.push_back({"1:1",
				rect.x + (rect.width - oneToOneLabelWidth) / 2,
				rect.y + (rect.height - textLineHeight) / 2,
				button.foreground});
		} else {
			const UiRect icon = WindowsInflatedRect(rect, 0.25f);
			const int right = icon.x + icon.width;
			const int bottom = icon.y + icon.height;
			const int corner = icon.width / 3;
			const int diagonal = icon.width / 2 - 1;
			for (int index = 0; index < 4; ++index) {
				const bool onLeft = index < 2;
				const bool onBottom = (index & 1) != 0;
				const int x = onLeft ? icon.x : right;
				const int y = onBottom ? bottom : icon.y;
				const int cornerX = onLeft ? corner : -corner;
				const int diagonalX = onLeft ? diagonal : -diagonal;
				const int diagonalY = onBottom ? -diagonal : diagonal;
				const int cornerY = onBottom ? -corner : corner;
				const int adjacentX = onLeft ? 1 : -1;
				AddLine(button, x, y + cornerY, x, y);
				AddLine(button, x, y, x + cornerX + adjacentX, y);
				AddLine(button, x, y, x + diagonalX, y + diagonalY);
			}
		}
		break;
	case IDM_FULL_SCREEN_MODE: {
		const UiRect icon = WindowsInflatedRect(rect, 0.25f);
		button.outlines.push_back(icon);
		AddLine(button, icon.x + 1, icon.y + icon.height / 4,
			icon.x + icon.width, icon.y + icon.height / 4);
		break;
	}
	case IDM_ROTATE_90: {
		const UiRect icon = WindowsInflatedRect(rect, 0.3f);
		const int right = icon.x + icon.width;
		const int bottom = icon.y + icon.height;
		const int x = icon.x + static_cast<int>(icon.width * 0.65f);
		AddLine(button, icon.x - 2, bottom, x, bottom);
		AddLine(button, x, bottom, x, bottom - static_cast<int>(icon.height * 0.4f));
		AddLine(button, x, bottom - static_cast<int>(icon.height * 0.4f), icon.x - 2, bottom);
		AddLine(button, x + 2, bottom, right + 2, bottom);
		AddLine(button, right + 2, bottom, x + 2, icon.y);
		AddLine(button, x + 2, icon.y, x + 2, bottom);
		break;
	}
	case IDM_ROTATE_270: {
		const UiRect icon = WindowsInflatedRect(rect, 0.3f);
		const int right = icon.x + icon.width;
		const int bottom = icon.y + icon.height;
		const int x = icon.x + static_cast<int>(icon.width * 0.33f);
		AddLine(button, icon.x - 2, bottom, x, bottom);
		AddLine(button, x, bottom, x, icon.y);
		AddLine(button, x, icon.y, icon.x - 2, bottom);
		AddLine(button, x + 2, bottom, right + 2, bottom);
		AddLine(button, right + 2, bottom, x + 2,
			bottom - static_cast<int>(icon.height * 0.4f));
		AddLine(button, x + 2, bottom - static_cast<int>(icon.height * 0.4f), x + 2, bottom);
		break;
	}
	case kNavigationSortModeCommand:
		button.text.push_back({sortLabel,
			rect.x + (rect.width - sortLabelWidth) / 2,
			rect.y + (rect.height - textLineHeight) / 2,
			button.foreground});
		break;
	case kCommandToggleSelectionMode: {
		const int left = rect.x + 7;
		const int top = rect.y + 7;
		const int right = rect.x + rect.width - 8;
		const int bottom = rect.y + rect.height - 8;
		const int corner = 4;
		AddLine(button, left, top, left + corner, top);
		AddLine(button, left, top, left, top + corner);
		AddLine(button, right - corner, top, right, top);
		AddLine(button, right, top, right, top + corner);
		AddLine(button, left, bottom - corner, left, bottom);
		AddLine(button, left, bottom, left + corner, bottom);
		AddLine(button, right, bottom - corner, right, bottom);
		AddLine(button, right - corner, bottom, right, bottom);
		break;
	}
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

UiRect InformationOverlaySpectrumButton(const OverlayLayout& layout, int textPadding) {
	return {layout.x + layout.width - textPadding - kSpectrumButtonSize,
		layout.y + std::max(0, layout.contentHeight - textPadding - kSpectrumButtonSize),
		kSpectrumButtonSize, kSpectrumButtonSize};
}

InformationOverlayPaintPlan InformationOverlayPaint(const OverlayLayout& layout,
	const std::vector<std::string>& lines, int lineHeight, int textPadding,
	bool spectrumVisible, const GrayscaleSpectrum* spectrum, bool buttonHovered) {
	InformationOverlayPaintPlan result;
	OverlayPaintPlan& plan = result.overlay;
	plan.panel = AsRect(layout);
	const int count = std::min(layout.visibleLines, static_cast<int>(lines.size()));
	for (int index = 0; index < count; ++index) {
		plan.text.push_back({lines[static_cast<std::size_t>(index)], layout.x + textPadding,
			layout.y + textPadding + index * lineHeight,
			index == 0 ? UiColor{255, 255, 255, 255} : UiColor{243, 242, 231, 255}});
	}

	const int contentHeight = layout.contentHeight;
	result.spectrumButton = InformationOverlaySpectrumButton(layout, textPadding);
	const UiColor buttonColor = buttonHovered ? UiColor{255, 255, 255, 255} : kGuiColor;
	const int centerX = result.spectrumButton.x + result.spectrumButton.width / 2;
	const int centerY = result.spectrumButton.y + result.spectrumButton.height / 2;
	if (spectrumVisible) {
		result.spectrumLines.push_back({centerX - 5, centerY + 3, centerX, centerY - 2, buttonColor});
		result.spectrumLines.push_back({centerX, centerY - 2, centerX + 5, centerY + 3, buttonColor});
	} else {
		result.spectrumLines.push_back({centerX - 5, centerY - 3, centerX, centerY + 2, buttonColor});
		result.spectrumLines.push_back({centerX, centerY + 2, centerX + 5, centerY - 3, buttonColor});
	}

	if (spectrumVisible) {
		const int graphX = layout.x + (layout.width - kSpectrumGraphWidth) / 2;
		const int graphY = layout.y + contentHeight;
		const int baselineY = graphY + kSpectrumGraphHeight;
		const auto heights = spectrum == nullptr ? std::array<int, kSpectrumBinCount>{} :
			GrayscaleSpectrumBarHeights(*spectrum, kSpectrumGraphHeight);
		result.spectrumLines.push_back({graphX, baselineY, graphX + kSpectrumGraphWidth,
			baselineY, {255, 255, 255, 255}});
		for (int index = 0; index < kSpectrumBinCount; ++index) {
			const int height = heights[static_cast<std::size_t>(index)];
			if (height == 0) continue;
			result.spectrumLines.push_back({graphX + index, baselineY - height,
				graphX + index, baselineY, {190, 190, 170, 255}});
		}
	}
	return result;
}

NavigationPanelPaint BuildNavigationPanelPaint(int windowWidth, int windowHeight,
	int mouseX, int mouseY, bool fitToWindow, FileList::SortMode sortMode,
	int sortLabelWidth, int oneToOneLabelWidth, int textLineHeight,
	bool selectionModeEnabled) {
	constexpr int buttonSize = 26;
	constexpr int panelHeight = 32;
	constexpr int gap = 5;
	constexpr int margin = 6;
	constexpr int separator = 8;
	constexpr std::array<int, 10> commands = {
		IDM_FIRST, IDM_PREV, IDM_NEXT, IDM_LAST, kNavigationSortModeCommand,
		IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, IDM_FULL_SCREEN_MODE,
		IDM_ROTATE_90, IDM_ROTATE_270, kCommandToggleSelectionMode
	};
	const int panelWidth = margin * 2 + buttonSize * static_cast<int>(commands.size()) +
		gap * (static_cast<int>(commands.size()) - 1) + separator * 2;
	NavigationPanelPaint plan;
	plan.panel = {(windowWidth - panelWidth) / 2, windowHeight - panelHeight,
		panelWidth, panelHeight};
	plan.opacity = Contains(plan.panel, mouseX, mouseY) ? 255 : 128;
	const std::string sortLabel = SortModeShortLabel(sortMode);
	int x = plan.panel.x + margin;
	for (std::size_t index = 0; index < commands.size(); ++index) {
		NavigationButtonPaint button;
		button.rect = {x, plan.panel.y + (panelHeight - buttonSize) / 2, buttonSize, buttonSize};
		button.command = commands[index];
		button.hovered = Contains(button.rect, mouseX, mouseY);
		button.foreground = button.hovered ||
			(button.command == kCommandToggleSelectionMode && selectionModeEnabled) ?
			kHighlightColor : kGuiColor;
		button.foreground.alpha = plan.opacity;
		AddNavigationIcon(button, fitToWindow, sortLabel, sortLabelWidth,
			oneToOneLabelWidth, textLineHeight);
		plan.buttons.push_back(std::move(button));
		x += buttonSize + gap;
		if (index == 4 || index == 6) x += separator;
	}
	return plan;
}

std::string NavigationTooltip(int command, bool fitToWindow, bool fullscreen,
	FileList::SortMode sortMode, bool selectionModeEnabled) {
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
	case kCommandToggleSelectionMode:
		return selectionModeEnabled ? "Disable crop selection mode (Ctrl+E)" :
			"Enable crop selection mode (Ctrl+E)";
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
