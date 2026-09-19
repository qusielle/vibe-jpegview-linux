#pragma once

#include <cstddef>

namespace jpegview_linux {

struct OverlayLayout {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	int textWidth = 0;
	int visibleLines = 0;
};

OverlayLayout FilenameOverlayLayout(int contentWidth, int windowWidth,
	int inset = 4, int textPadding = 6, int height = 20);

OverlayLayout InformationOverlayLayout(int contentWidth, std::size_t lineCount,
	int windowWidth, int windowHeight, bool filenameVisible,
	int inset = 4, int textPadding = 6, int lineHeight = 18, int filenameHeight = 20);

} // namespace jpegview_linux
