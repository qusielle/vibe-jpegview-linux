#include "overlay_layout.h"

#include "spectrum_model.h"

#include <algorithm>
#include <limits>

namespace jpegview_linux {
namespace {

int PanelWidth(int contentWidth, int windowWidth, int inset, int textPadding) {
	const int maximumWidth = std::max(1, windowWidth - 2 * inset);
	return std::min(maximumWidth, std::max(0, contentWidth) + 2 * textPadding);
}

} // namespace

OverlayLayout FilenameOverlayLayout(int contentWidth, int windowWidth,
	int inset, int textPadding, int height) {
	const int width = PanelWidth(contentWidth, windowWidth, inset, textPadding);
	return {inset, inset, width, height, std::max(1, width - 2 * textPadding), 1, height};
}

OverlayLayout InformationOverlayLayout(int contentWidth, std::size_t lineCount,
	int windowWidth, int windowHeight, bool filenameVisible,
	int inset, int textPadding, int lineHeight, int filenameHeight, bool spectrumVisible) {
	const int maximumHeight = std::max(1, windowHeight - 2 * inset);
	const int buttonReserve = kSpectrumButtonSize + textPadding;
	const int requestedWidth = std::max(0, contentWidth) + 2 * textPadding + buttonReserve;
	const int spectrumWidth = spectrumVisible ? kSpectrumGraphWidth + 2 * textPadding : 0;
	const int maximumWidth = std::max(1, windowWidth - 2 * inset);
	const int width = std::min(maximumWidth, std::max(requestedWidth, spectrumWidth));
	const int textWidth = std::max(1, std::min(std::max(0, contentWidth),
		width - 2 * textPadding - buttonReserve));
	const std::size_t safeLines = std::min(lineCount,
		static_cast<std::size_t>(std::numeric_limits<int>::max() / std::max(1, lineHeight)));
	const int requestedHeight = 2 * textPadding + static_cast<int>(safeLines) * lineHeight;
	const int spectrumHeight = spectrumVisible ? std::min(maximumHeight,
		kSpectrumGraphHeight + kSpectrumBottomGap) : 0;
	const int contentHeight = std::min(std::max(0, maximumHeight - spectrumHeight), requestedHeight);
	const int height = contentHeight + spectrumHeight;
	const int y = filenameVisible ? inset + filenameHeight + inset : inset;
	const int visibleLines = std::max(0,
		(contentHeight - 2 * textPadding) / std::max(1, lineHeight));
	return {inset, y, width, height, textWidth, visibleLines, contentHeight};
}

} // namespace jpegview_linux
