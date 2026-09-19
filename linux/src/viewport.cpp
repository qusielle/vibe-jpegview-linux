#include "viewport.h"

#include "settings.h"

#include <algorithm>
#include <cmath>

namespace jpegview_linux {
namespace {

double ClampedZoom(double zoom) {
	return std::clamp(zoom, kMinimumZoom, kMaximumZoom);
}

const char* ScaleModeName(const ViewportSnapshot& state) {
	if (!state.fitToWindow) return "manual";
	if (state.fillWithCrop && state.noEnlarge) return "fill_no_enlarge";
	if (state.fillWithCrop) return "fill";
	if (state.noEnlarge) return "fit_no_enlarge";
	return "fit";
}

} // namespace

const char* Viewport::ScaleMode() const {
	return ScaleModeName(Snapshot());
}

const char* Viewport::NavigationScaleMode() const {
	return ScaleModeName(navigationState_);
}

void Viewport::LoadScaleMode(std::string_view mode, bool manualZoomSet, double manualZoom) {
	(void)manualZoomSet;
	(void)manualZoom;
	if (mode == "manual") {
		// Manual zoom is transient. The persisted non-fit mode represents Actual
		// Size so an old ad-hoc zoom cannot leak into a newly opened file.
		SetManualZoom(1.0);
		navigationState_ = Snapshot();
		return;
	}

	fitToWindow_ = true;
	fillWithCrop_ = mode == "fill" || mode == "fill_no_enlarge";
	noEnlarge_ = mode != "fit" && mode != "fill";
	offsetX_ = 0.0;
	offsetY_ = 0.0;
	navigationState_ = Snapshot();
}

ViewportSnapshot Viewport::Snapshot() const {
	return {fitToWindow_, fillWithCrop_, noEnlarge_, zoom_};
}

void Viewport::Restore(const ViewportSnapshot& snapshot, int imageWidth, int imageHeight,
	int windowWidth, int windowHeight) {
	const ViewportSnapshot navigationState = navigationState_;
	if (snapshot.fitToWindow) {
		Fit(imageWidth, imageHeight, windowWidth, windowHeight,
			snapshot.fillWithCrop, snapshot.noEnlarge);
	} else {
		SetManualZoom(snapshot.zoom);
	}
	navigationState_ = navigationState;
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
	navigationState_ = Snapshot();
}

void Viewport::ActualSize() {
	SetManualZoom(1.0);
	navigationState_ = Snapshot();
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
