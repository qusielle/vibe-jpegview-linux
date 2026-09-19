#include "overlay_layout.h"

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
	return {inset, inset, width, height, std::max(1, width - 2 * textPadding), 1};
}

OverlayLayout InformationOverlayLayout(int contentWidth, std::size_t lineCount,
	int windowWidth, int windowHeight, bool filenameVisible,
	int inset, int textPadding, int lineHeight, int filenameHeight) {
	const int width = PanelWidth(contentWidth, windowWidth, inset, textPadding);
	const int maximumHeight = std::max(1, windowHeight - 2 * inset);
	const std::size_t safeLines = std::min(lineCount,
		static_cast<std::size_t>(std::numeric_limits<int>::max() / std::max(1, lineHeight)));
	const int requestedHeight = 2 * textPadding + static_cast<int>(safeLines) * lineHeight;
	const int height = std::min(maximumHeight, requestedHeight);
	const int y = filenameVisible ? inset + filenameHeight + inset : inset;
	const int visibleLines = std::max(0, (height - 2 * textPadding) / std::max(1, lineHeight));
	return {inset, y, width, height, std::max(1, width - 2 * textPadding), visibleLines};
}

} // namespace jpegview_linux
