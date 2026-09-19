#include "viewport.h"

#include "settings.h"

#include <algorithm>
#include <cmath>

namespace jpegview_linux {
namespace {

double ClampedZoom(double zoom) {
	return std::clamp(zoom, kMinimumZoom, kMaximumZoom);
}

} // namespace

const char* Viewport::ScaleMode() const {
	if (!fitToWindow_) return "manual";
	if (fillWithCrop_ && noEnlarge_) return "fill_no_enlarge";
	if (fillWithCrop_) return "fill";
	if (noEnlarge_) return "fit_no_enlarge";
	return "fit";
}

void Viewport::LoadScaleMode(std::string_view mode, bool manualZoomSet, double manualZoom) {
	if (mode == "manual") {
		SetManualZoom(manualZoomSet ? manualZoom : zoom_);
		return;
	}

	fitToWindow_ = true;
	fillWithCrop_ = mode == "fill" || mode == "fill_no_enlarge";
	noEnlarge_ = mode != "fit" && mode != "fill";
	offsetX_ = 0.0;
	offsetY_ = 0.0;
}

ViewportSnapshot Viewport::Snapshot() const {
	return {fitToWindow_, fillWithCrop_, noEnlarge_, zoom_};
}

void Viewport::Restore(const ViewportSnapshot& snapshot, int imageWidth, int imageHeight,
	int windowWidth, int windowHeight) {
	if (snapshot.fitToWindow) {
		Fit(imageWidth, imageHeight, windowWidth, windowHeight,
			snapshot.fillWithCrop, snapshot.noEnlarge);
	} else {
		SetManualZoom(snapshot.zoom);
	}
}

void Viewport::Fit(int imageWidth, int imageHeight, int windowWidth, int windowHeight,
	bool fillWithCrop, bool noEnlarge) {
	if (imageWidth <= 0 || imageHeight <= 0) return;
	// Fit against the complete client area. The old calculation reserved an
	// eight-pixel border on every side, leaving visible bands even when the
	// image and window had matching aspect ratios.
	const double widthScale = static_cast<double>(std::max(1, windowWidth)) / imageWidth;
	const double heightScale = static_cast<double>(std::max(1, windowHeight)) / imageHeight;
	const double windowScale = fillWithCrop ? std::max(widthScale, heightScale) :
		std::min(widthScale, heightScale);
	zoom_ = ClampedZoom(noEnlarge ? std::min(1.0, windowScale) : windowScale);
	fitToWindow_ = true;
	fillWithCrop_ = fillWithCrop;
	noEnlarge_ = noEnlarge;
	offsetX_ = 0.0;
	offsetY_ = 0.0;
}

void Viewport::ActualSize() {
	SetManualZoom(1.0);
}

void Viewport::ZoomAt(double factor, int mouseX, int mouseY, int imageWidth, int imageHeight,
	int windowWidth, int windowHeight) {
	if (imageWidth <= 0 || imageHeight <= 0 || factor <= 0.0) return;
	const double oldZoom = zoom_;
	const double imageX = (mouseX - (windowWidth - imageWidth * oldZoom) / 2.0 - offsetX_) / oldZoom;
	const double imageY = (mouseY - (windowHeight - imageHeight * oldZoom) / 2.0 - offsetY_) / oldZoom;
	zoom_ = ClampedZoom(oldZoom * factor);
	offsetX_ = mouseX - (windowWidth - imageWidth * zoom_) / 2.0 - imageX * zoom_;
	offsetY_ = mouseY - (windowHeight - imageHeight * zoom_) / 2.0 - imageY * zoom_;
	fitToWindow_ = false;
	fillWithCrop_ = false;
	noEnlarge_ = false;
}

void Viewport::Pan(double deltaX, double deltaY) {
	offsetX_ += deltaX;
	offsetY_ += deltaY;
	fitToWindow_ = false;
	fillWithCrop_ = false;
	noEnlarge_ = false;
}

ViewportRect Viewport::Destination(int imageWidth, int imageHeight,
	int windowWidth, int windowHeight) const {
	const int renderWidth = std::max(1, static_cast<int>(std::round(imageWidth * zoom_)));
	const int renderHeight = std::max(1, static_cast<int>(std::round(imageHeight * zoom_)));
	return {
		static_cast<int>(std::round((windowWidth - renderWidth) / 2.0 + offsetX_)),
		static_cast<int>(std::round((windowHeight - renderHeight) / 2.0 + offsetY_)),
		renderWidth,
		renderHeight,
	};
}

void Viewport::SetManualZoom(double zoom) {
	zoom_ = ClampedZoom(zoom);
	fitToWindow_ = false;
	fillWithCrop_ = false;
	noEnlarge_ = false;
	offsetX_ = 0.0;
	offsetY_ = 0.0;
}

} // namespace jpegview_linux
