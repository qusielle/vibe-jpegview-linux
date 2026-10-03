#include "viewport.h"

#include "settings.h"

#include <algorithm>
#include <cmath>
#include <string>

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

bool Viewport::IsActualSize() const {
	const double actualSizeZoom = fitRelativeZoomMode_ ? fitRelativeZoomBase_ : 1.0;
	return !fitToWindow_ && std::abs(zoom_ - actualSizeZoom) < 1e-9;
}

double Viewport::ZoomTargetForPreset(double factor) const {
	if (!std::isfinite(factor) || factor <= 0.0) return zoom_;
	return ClampedZoom((fitRelativeZoomMode_ ? fitRelativeZoomBase_ : 1.0) * factor);
}

std::string Viewport::ZoomReadout() const {
	const auto percent = [](double zoom) {
		return std::to_string(std::max(0L, std::lround(zoom * 100.0))) + "%";
	};
	const std::string sourceScale = percent(zoom_);
	if (!fitRelativeZoomMode_ || std::abs(fitRelativeZoomBase_ - 1.0) < 0.0001) {
		return sourceScale;
	}
	return percent(zoom_ / fitRelativeZoomBase_) + " (" + sourceScale + ")";
}

void Viewport::UpdateFitRelativeZoomBase(int imageWidth, int imageHeight,
	int windowWidth, int windowHeight) {
	if (imageWidth <= 0 || imageHeight <= 0) return;
	const double widthScale = static_cast<double>(std::max(1, windowWidth)) / imageWidth;
	const double heightScale = static_cast<double>(std::max(1, windowHeight)) / imageHeight;
	fitRelativeZoomBase_ = ClampedZoom(std::min(widthScale, heightScale));
}

ViewportSnapshot Viewport::NavigationSnapshot() const {
	if (fitRelativeZoomMode_ && !fitToWindow_) return Snapshot();
	return navigationState_;
}

ViewportSnapshot Viewport::PrefetchSnapshot() const {
	ViewportSnapshot snapshot = prefetchFitState_;
	snapshot.fitToWindow = true;
	snapshot.zoom = 1.0;
	snapshot.relativeZoom = 1.0;
	return snapshot;
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
	prefetchFitState_ = navigationState_;
}

ViewportSnapshot Viewport::Snapshot() const {
	const double relativeZoom = fitRelativeZoomBase_ > 0.0 ?
		std::clamp(zoom_ / fitRelativeZoomBase_, kMinimumZoom / kMaximumZoom,
			kMaximumZoom / kMinimumZoom) : 1.0;
	return {fitToWindow_, fillWithCrop_, noEnlarge_, zoom_, relativeZoom};
}

void Viewport::Restore(const ViewportSnapshot& snapshot, int imageWidth, int imageHeight,
	int windowWidth, int windowHeight) {
	const ViewportSnapshot navigationState = navigationState_;
	const ViewportSnapshot prefetchFitState = prefetchFitState_;
	UpdateFitRelativeZoomBase(imageWidth, imageHeight, windowWidth, windowHeight);
	if (snapshot.fitToWindow) {
		Fit(imageWidth, imageHeight, windowWidth, windowHeight,
			snapshot.fillWithCrop, snapshot.noEnlarge);
	} else if (fitRelativeZoomMode_) {
		SetManualZoom(fitRelativeZoomBase_ * std::clamp(snapshot.relativeZoom,
			kMinimumZoom / kMaximumZoom, kMaximumZoom / kMinimumZoom));
	} else {
		SetManualZoom(snapshot.zoom);
	}
	navigationState_ = navigationState;
	prefetchFitState_ = prefetchFitState;
}

void Viewport::Fit(int imageWidth, int imageHeight, int windowWidth, int windowHeight,
	bool fillWithCrop, bool noEnlarge) {
	fitToWindow_ = true;
	fillWithCrop_ = fillWithCrop;
	noEnlarge_ = noEnlarge;
	offsetX_ = 0.0;
	offsetY_ = 0.0;
	if (imageWidth <= 0 || imageHeight <= 0) {
		navigationState_ = Snapshot();
		prefetchFitState_ = navigationState_;
		return;
	}
	UpdateFitRelativeZoomBase(imageWidth, imageHeight, windowWidth, windowHeight);
	// Fit against the complete client area. The old calculation reserved an
	// eight-pixel border on every side, leaving visible bands even when the
	// image and window had matching aspect ratios.
	const double widthScale = static_cast<double>(std::max(1, windowWidth)) / imageWidth;
	const double heightScale = static_cast<double>(std::max(1, windowHeight)) / imageHeight;
	const double windowScale = fillWithCrop ? std::max(widthScale, heightScale) :
		std::min(widthScale, heightScale);
	zoom_ = ClampedZoom(noEnlarge ? std::min(1.0, windowScale) : windowScale);
	navigationState_ = Snapshot();
	prefetchFitState_ = navigationState_;
}

void Viewport::ActualSize() {
	SetManualZoom(fitRelativeZoomMode_ ? fitRelativeZoomBase_ : 1.0);
	navigationState_ = Snapshot();
}

void Viewport::ZoomAt(double factor, int mouseX, int mouseY, int imageWidth, int imageHeight,
	int windowWidth, int windowHeight, bool pauseAtFitRelativeAnchor) {
	if (imageWidth <= 0 || imageHeight <= 0 || factor <= 0.0) return;
	UpdateFitRelativeZoomBase(imageWidth, imageHeight, windowWidth, windowHeight);
	const double oldZoom = zoom_;
	const double imageX = (mouseX - (windowWidth - imageWidth * oldZoom) / 2.0 - offsetX_) / oldZoom;
	const double imageY = (mouseY - (windowHeight - imageHeight * oldZoom) / 2.0 - offsetY_) / oldZoom;
	double newZoom = ClampedZoom(oldZoom * factor);
	if (fitRelativeZoomMode_) {
		const double anchor = fitRelativeZoomBase_;
		const bool nearFitRelativeAnchor = std::abs(newZoom - anchor) < anchor * 0.01;
		const bool crossedFitRelativeAnchor = pauseAtFitRelativeAnchor &&
			((oldZoom < anchor && newZoom > anchor) || (oldZoom > anchor && newZoom < anchor));
		if (nearFitRelativeAnchor || crossedFitRelativeAnchor) newZoom = anchor;
	}
	zoom_ = newZoom;
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

void Viewport::ClampToView(int imageWidth, int imageHeight, int windowWidth, int windowHeight) {
	if (imageWidth <= 0 || imageHeight <= 0 || windowWidth <= 0 || windowHeight <= 0) return;
	const int destinationWidth = std::max(1, static_cast<int>(std::round(imageWidth * zoom_)));
	const int destinationHeight = std::max(1, static_cast<int>(std::round(imageHeight * zoom_)));
	if (destinationWidth <= windowWidth) {
		offsetX_ = 0.0;
	} else {
		const double centeredOffset = (windowWidth - destinationWidth) / 2.0;
		offsetX_ = std::clamp(offsetX_, windowWidth - destinationWidth - centeredOffset,
			-centeredOffset);
	}
	if (destinationHeight <= windowHeight) {
		offsetY_ = 0.0;
	} else {
		const double centeredOffset = (windowHeight - destinationHeight) / 2.0;
		offsetY_ = std::clamp(offsetY_, windowHeight - destinationHeight - centeredOffset,
			-centeredOffset);
	}
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

void ApplyViewportIntent(Viewport& viewport, const ViewportIntent& intent,
	int imageWidth, int imageHeight, int windowWidth, int windowHeight) {
	switch (intent.type) {
	case ViewportIntentType::Fit:
		viewport.Fit(imageWidth, imageHeight, windowWidth, windowHeight,
			intent.fillWithCrop, intent.noEnlarge);
		break;
	case ViewportIntentType::ActualSize:
		viewport.ActualSize();
		break;
	case ViewportIntentType::ZoomByFactor:
		viewport.ZoomAt(intent.value, intent.mouseX, intent.mouseY,
			imageWidth, imageHeight, windowWidth, windowHeight,
			intent.pauseAtFitRelativeAnchor);
		break;
	case ViewportIntentType::ZoomPreset: {
		const double currentZoom = viewport.Zoom();
		if (currentZoom <= 0.0) break;
		const double targetZoom = viewport.ZoomTargetForPreset(intent.value);
		viewport.ZoomAt(targetZoom / currentZoom, intent.mouseX, intent.mouseY,
			imageWidth, imageHeight, windowWidth, windowHeight);
		break;
	}
	case ViewportIntentType::Pan:
		viewport.Pan(intent.deltaX, intent.deltaY);
		break;
	}
}

} // namespace jpegview_linux
