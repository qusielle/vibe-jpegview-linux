#include "zoom_navigator_model.h"

#include <algorithm>
#include <cmath>

namespace jpegview_linux {
namespace {

constexpr int kMinimumThumbnailWidth = 133;
constexpr int kMaximumThumbnailWidth = 320;
constexpr int kCornerInset = 8;

int RoundedPositive(double value) {
	return std::max(1, static_cast<int>(std::lround(value)));
}

double ClampUnit(double value) {
	return std::clamp(value, 0.0, 1.0);
}

double ClampCenter(double requestedCenter, double visibleFraction) {
	const double visible = std::clamp(visibleFraction, 0.0, 1.0);
	if (visible >= 1.0) return 0.5;
	return std::clamp(requestedCenter, visible * 0.5, 1.0 - visible * 0.5);
}

} // namespace

ZoomNavigatorLayout CalculateZoomNavigatorLayout(int imageWidth, int imageHeight,
	int imageAreaX, int imageAreaY, int imageAreaWidth, int imageAreaHeight) {
	if (imageWidth <= 0 || imageHeight <= 0 || imageAreaWidth <= 0 || imageAreaHeight <= 0) return {};
	const int desiredWidth = std::clamp(static_cast<int>(std::lround(imageAreaWidth * 0.2 + 40.0)),
		kMinimumThumbnailWidth, kMaximumThumbnailWidth);
	const int hotWidth = std::min(desiredWidth, std::max(1, imageAreaWidth - 16));
	const int desiredHeight = RoundedPositive(hotWidth * 0.75);
	const int hotHeight = std::min(desiredHeight, std::max(1, imageAreaHeight - 16));
	const int insetX = std::min(kCornerInset, std::max(0, (imageAreaWidth - hotWidth) / 2));
	const int insetY = std::min(kCornerInset, std::max(0, (imageAreaHeight - hotHeight) / 2));
	const ZoomNavigatorRect hotArea{
		imageAreaX + imageAreaWidth - hotWidth - insetX,
		imageAreaY + insetY,
		hotWidth,
		hotHeight,
	};
	const double scale = std::min(static_cast<double>(hotWidth) / imageWidth,
		static_cast<double>(hotHeight) / imageHeight);
	const int width = std::min(hotWidth, RoundedPositive(imageWidth * scale));
	const int height = std::min(hotHeight, RoundedPositive(imageHeight * scale));
	const ZoomNavigatorRect image{
		hotArea.x + (hotWidth - width) / 2,
		hotArea.y + (hotHeight - height) / 2,
		width,
		height,
	};
	return {hotArea, image};
}

bool ImageNeedsZoomNavigator(int imageWidth, int imageHeight,
	int imageAreaWidth, int imageAreaHeight) {
	return imageWidth > 0 && imageHeight > 0 && imageAreaWidth > 0 && imageAreaHeight > 0 &&
		(imageWidth > imageAreaWidth || imageHeight > imageAreaHeight);
}

NormalizedImageRect CalculateVisibleImageRect(int destinationX, int destinationY,
	int destinationWidth, int destinationHeight, int imageAreaX, int imageAreaY,
	int imageAreaWidth, int imageAreaHeight) {
	if (destinationWidth <= 0 || destinationHeight <= 0 || imageAreaWidth <= 0 || imageAreaHeight <= 0) return {};
	const double left = ClampUnit(static_cast<double>(imageAreaX - destinationX) / destinationWidth);
	const double top = ClampUnit(static_cast<double>(imageAreaY - destinationY) / destinationHeight);
	const double right = ClampUnit(static_cast<double>(imageAreaX + imageAreaWidth - destinationX) / destinationWidth);
	const double bottom = ClampUnit(static_cast<double>(imageAreaY + imageAreaHeight - destinationY) / destinationHeight);
	if (right <= left || bottom <= top) return {};
	return {left, top, right, bottom, true};
}

ZoomNavigatorRect MapVisibleRectToNavigator(const NormalizedImageRect& visible,
	const ZoomNavigatorRect& image) {
	if (!visible.valid || image.width <= 0 || image.height <= 0) return {};
	const int left = image.x + static_cast<int>(std::lround(ClampUnit(visible.left) * image.width));
	const int top = image.y + static_cast<int>(std::lround(ClampUnit(visible.top) * image.height));
	const int right = image.x + static_cast<int>(std::lround(ClampUnit(visible.right) * image.width));
	const int bottom = image.y + static_cast<int>(std::lround(ClampUnit(visible.bottom) * image.height));
	return {left, top, std::max(1, right - left), std::max(1, bottom - top)};
}

ZoomNavigatorPoint NavigatorPointToImage(int x, int y,
	const ZoomNavigatorRect& image) {
	if (image.width <= 0 || image.height <= 0) return {};
	return {
		ClampUnit(static_cast<double>(x - image.x) / image.width),
		ClampUnit(static_cast<double>(y - image.y) / image.height),
	};
}

ZoomNavigatorPan CalculateNavigatorCenterPan(double currentCenterX,
	double currentCenterY, double requestedCenterX, double requestedCenterY,
	double visibleWidthFraction, double visibleHeightFraction,
	int destinationWidth, int destinationHeight) {
	if (destinationWidth <= 0 || destinationHeight <= 0) return {};
	const double targetX = ClampCenter(requestedCenterX, visibleWidthFraction);
	const double targetY = ClampCenter(requestedCenterY, visibleHeightFraction);
	return {
		(currentCenterX - targetX) * destinationWidth,
		(currentCenterY - targetY) * destinationHeight,
	};
}

ZoomNavigatorPan CalculateNavigatorDragPan(int deltaX, int deltaY,
	const ZoomNavigatorRect& image, int destinationWidth, int destinationHeight) {
	if (image.width <= 0 || image.height <= 0 || destinationWidth <= 0 || destinationHeight <= 0) return {};
	return {
		-static_cast<double>(deltaX) * destinationWidth / image.width,
		-static_cast<double>(deltaY) * destinationHeight / image.height,
	};
}

} // namespace jpegview_linux
