#pragma once

#include <string>
#include <string_view>

namespace jpegview_linux {

struct ViewportRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct ViewportSnapshot {
	bool fitToWindow = true;
	bool fillWithCrop = false;
	bool noEnlarge = true;
	double zoom = 1.0;
	double relativeZoom = 1.0;
};

// Owns image scale and pan state independently of SDL. Viewer supplies the
// current image and window dimensions whenever geometry must be recomputed.
class Viewport {
public:
	double Zoom() const { return zoom_; }
	double OffsetX() const { return offsetX_; }
	double OffsetY() const { return offsetY_; }
	bool IsFitToWindow() const { return fitToWindow_; }
	bool IsActualSize() const;
	bool FillWithCrop() const { return fillWithCrop_; }
	bool NoEnlarge() const { return noEnlarge_; }
	bool FitRelativeZoomMode() const { return fitRelativeZoomMode_; }
	double FitRelativeZoomBase() const { return fitRelativeZoomBase_; }
	double ZoomTargetForPreset(double factor) const;
	double ZoomStepMultiplier() const { return fitRelativeZoomMode_ ? 1.1 : 1.2; }
	std::string ZoomReadout() const;
	void SetFitRelativeZoomMode(bool enabled) { fitRelativeZoomMode_ = enabled; }
	void UpdateFitRelativeZoomBase(int imageWidth, int imageHeight,
		int windowWidth, int windowHeight);

	const char* ScaleMode() const;
	const char* NavigationScaleMode() const;
	double NavigationZoom() const { return navigationState_.zoom; }
	void LoadScaleMode(std::string_view mode, bool manualZoomSet, double manualZoom);
	ViewportSnapshot Snapshot() const;
	ViewportSnapshot NavigationSnapshot() const;

	void Restore(const ViewportSnapshot& snapshot, int imageWidth, int imageHeight,
		int windowWidth, int windowHeight);
	void Fit(int imageWidth, int imageHeight, int windowWidth, int windowHeight,
		bool fillWithCrop = false, bool noEnlarge = true);
	void ActualSize();
	void ZoomAt(double factor, int mouseX, int mouseY, int imageWidth, int imageHeight,
		int windowWidth, int windowHeight, bool pauseAtFitRelativeAnchor = false);
	void Pan(double deltaX, double deltaY);
	void ClampToView(int imageWidth, int imageHeight, int windowWidth, int windowHeight);

	ViewportRect Destination(int imageWidth, int imageHeight,
		int windowWidth, int windowHeight) const;

private:
	void SetManualZoom(double zoom);

	double zoom_ = 1.0;
	double fitRelativeZoomBase_ = 1.0;
	double offsetX_ = 0.0;
	double offsetY_ = 0.0;
	bool fitToWindow_ = true;
	bool fillWithCrop_ = false;
	bool noEnlarge_ = true;
	bool fitRelativeZoomMode_ = false;
	ViewportSnapshot navigationState_;
};

} // namespace jpegview_linux
