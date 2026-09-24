#pragma once

#include "file_list.h"
#include "overlay_layout.h"
#include "spectrum_model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jpegview_linux {

constexpr int kNavigationSortModeCommand = -2;

struct UiColor {
	std::uint8_t red = 0;
	std::uint8_t green = 0;
	std::uint8_t blue = 0;
	std::uint8_t alpha = 255;
};

struct UiRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct UiLine {
	int x1 = 0;
	int y1 = 0;
	int x2 = 0;
	int y2 = 0;
	UiColor color{235, 235, 235, 255};
};

struct UiText {
	std::string text;
	int x = 0;
	int y = 0;
	UiColor color{235, 235, 235, 255};
};

struct OverlayPaintPlan {
	UiRect panel;
	UiColor background{8, 8, 8, 205};
	UiColor border{105, 105, 105, 255};
	std::vector<UiText> text;
};

struct InformationOverlayPaintPlan {
	OverlayPaintPlan overlay;
	UiRect spectrumButton;
	std::vector<UiLine> spectrumLines;
};

OverlayPaintPlan FilenameOverlayPaint(const OverlayLayout& layout,
	std::string label, int textLineHeight, int textPadding = 6);
InformationOverlayPaintPlan InformationOverlayPaint(const OverlayLayout& layout,
	const std::vector<std::string>& lines, int lineHeight, int textPadding = 6,
	bool spectrumVisible = false, const GrayscaleSpectrum* spectrum = nullptr,
	bool buttonHovered = false);
UiRect InformationOverlaySpectrumButton(const OverlayLayout& layout, int textPadding = 6);

struct NavigationButtonPaint {
	UiRect rect;
	int command = 0;
	bool hovered = false;
	UiColor foreground{243, 242, 231, 255};
	std::vector<UiLine> lines;
	std::vector<UiRect> outlines;
	std::vector<UiText> text;
};

struct NavigationPanelPaint {
	UiRect panel;
	std::uint8_t opacity = 128;
	std::vector<NavigationButtonPaint> buttons;
};

NavigationPanelPaint BuildNavigationPanelPaint(int windowWidth, int windowHeight,
	int mouseX, int mouseY, bool fitToWindow, FileList::SortMode sortMode,
	int sortLabelWidth, int oneToOneLabelWidth, int textLineHeight,
	bool selectionModeEnabled = false);
std::string NavigationTooltip(int command, bool fitToWindow, bool fullscreen,
	FileList::SortMode sortMode, bool selectionModeEnabled = false);
OverlayPaintPlan NavigationTooltipPaint(const UiRect& anchor, std::string label,
	int labelWidth, int textLineHeight, int windowWidth, int windowHeight);
bool Contains(const UiRect& rect, int x, int y);

} // namespace jpegview_linux
