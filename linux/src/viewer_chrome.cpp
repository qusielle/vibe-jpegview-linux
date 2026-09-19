#include "viewer_chrome.h"

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
			const int inset = rect.width / 4;
			const int leftEdge = rect.x + inset;
			const int rightEdge = rect.x + rect.width - inset;
			const int topEdge = rect.y + inset;
			const int bottomEdge = rect.y + rect.height - inset;
			const int corner = (rightEdge - leftEdge) / 3;
			const int diagonal = (rightEdge - leftEdge) / 2 - 1;
			for (int index = 0; index < 4; ++index) {
				const bool onLeft = index < 2;
				const bool onTop = (index & 1) == 0;
				const int x = onLeft ? leftEdge : rightEdge;
				const int y = onTop ? topEdge : bottomEdge;
				const int cornerX = onLeft ? corner : -corner;
				const int diagonalX = onLeft ? diagonal : -diagonal;
				const int diagonalY = onTop ? diagonal : -diagonal;
				const int cornerY = onTop ? corner : -corner;
				const int adjacentX = onLeft ? 1 : -1;
				AddLine(button, x, y + cornerY, x, y);
				AddLine(button, x, y, x + cornerX + adjacentX, y);
				AddLine(button, x, y, x + diagonalX, y + diagonalY);
			}
		} else {
			const int inset = rect.width / 3;
			const int leftEdge = rect.x + inset;
			const int rightEdge = rect.x + rect.width - inset;
			const int topEdge = rect.y + inset;
			const int bottomEdge = rect.y + rect.height - inset;
			const int middle = (leftEdge + rightEdge) / 2;
			AddLine(button, leftEdge, topEdge, leftEdge, bottomEdge);
			AddLine(button, leftEdge, topEdge, middle, topEdge);
			AddLine(button, leftEdge, bottomEdge, middle, bottomEdge);
			AddLine(button, rightEdge, topEdge, rightEdge, bottomEdge);
			AddLine(button, rightEdge, topEdge, middle, topEdge);
			AddLine(button, rightEdge, bottomEdge, middle, bottomEdge);
		}
		break;
	case IDM_FULL_SCREEN_MODE:
		{
			const int inset = rect.width / 4;
			const int leftEdge = rect.x + inset;
			const int rightEdge = rect.x + rect.width - inset;
			const int topEdge = rect.y + inset;
			const int bottomEdge = rect.y + rect.height - inset;
			button.outlines.push_back({leftEdge, topEdge,
				rightEdge - leftEdge, bottomEdge - topEdge});
			AddLine(button, leftEdge, topEdge + (bottomEdge - topEdge) / 4, rightEdge,
				topEdge + (bottomEdge - topEdge) / 4);
		}
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
			button.foreground});
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
	int sortLabelWidth, int textLineHeight) {
	constexpr int buttonSize = 26;
	constexpr int panelHeight = 32;
	constexpr int gap = 5;
	constexpr int margin = 6;
	constexpr int separator = 8;
	constexpr std::array<int, 9> commands = {
		IDM_FIRST, IDM_PREV, IDM_NEXT, IDM_LAST, kNavigationSortModeCommand,
		IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, IDM_FULL_SCREEN_MODE,
		IDM_ROTATE_90, IDM_ROTATE_270
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
		button.foreground = button.hovered ? kHighlightColor : kGuiColor;
		button.foreground.alpha = plan.opacity;
		AddNavigationIcon(button, fitToWindow, sortLabel, sortLabelWidth, textLineHeight);
		plan.buttons.push_back(std::move(button));
		x += buttonSize + gap;
		if (index == 4 || index == 6) x += separator;
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
