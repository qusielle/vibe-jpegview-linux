#pragma once

namespace jpegview_linux {

struct ZoomNavigatorRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct NormalizedImageRect {
	double left = 0.0;
	double top = 0.0;
	double right = 0.0;
	double bottom = 0.0;
	bool valid = false;
};

struct ZoomNavigatorPoint {
	double x = 0.0;
	double y = 0.0;
};

struct ZoomNavigatorPan {
	double x = 0.0;
	double y = 0.0;
};

struct ZoomNavigatorLayout {
	// Hot area includes the reserved corner even when the image's aspect ratio
	// leaves empty space around the actual overview thumbnail.
	ZoomNavigatorRect hotArea;
	ZoomNavigatorRect image;
};

ZoomNavigatorLayout CalculateZoomNavigatorLayout(int imageWidth, int imageHeight,
	int imageAreaX, int imageAreaY, int imageAreaWidth, int imageAreaHeight);

bool ImageNeedsZoomNavigator(int imageWidth, int imageHeight,
	int imageAreaWidth, int imageAreaHeight);

NormalizedImageRect CalculateVisibleImageRect(int destinationX, int destinationY,
	int destinationWidth, int destinationHeight, int imageAreaX, int imageAreaY,
	int imageAreaWidth, int imageAreaHeight);

ZoomNavigatorRect MapVisibleRectToNavigator(const NormalizedImageRect& visible,
	const ZoomNavigatorRect& image);

ZoomNavigatorPoint NavigatorPointToImage(int x, int y,
	const ZoomNavigatorRect& image);

ZoomNavigatorPan CalculateNavigatorCenterPan(double currentCenterX,
	double currentCenterY, double requestedCenterX, double requestedCenterY,
	double visibleWidthFraction, double visibleHeightFraction,
	int destinationWidth, int destinationHeight);

ZoomNavigatorPan CalculateNavigatorDragPan(int deltaX, int deltaY,
	const ZoomNavigatorRect& image, int destinationWidth, int destinationHeight);

} // namespace jpegview_linux
