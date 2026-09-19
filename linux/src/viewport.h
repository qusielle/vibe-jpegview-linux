#pragma once

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
};

// Owns image scale and pan state independently of SDL. Viewer supplies the
// current image and window dimensions whenever geometry must be recomputed.
class Viewport {
public:
	double Zoom() const { return zoom_; }
	double OffsetX() const { return offsetX_; }
	double OffsetY() const { return offsetY_; }
	bool IsFitToWindow() const { return fitToWindow_; }
	bool FillWithCrop() const { return fillWithCrop_; }
	bool NoEnlarge() const { return noEnlarge_; }

	const char* ScaleMode() const;
	void LoadScaleMode(std::string_view mode, bool manualZoomSet, double manualZoom);
	ViewportSnapshot Snapshot() const;

	void Restore(const ViewportSnapshot& snapshot, int imageWidth, int imageHeight,
		int windowWidth, int windowHeight);
	void Fit(int imageWidth, int imageHeight, int windowWidth, int windowHeight,
		bool fillWithCrop = false, bool noEnlarge = true);
	void ActualSize();
	void ZoomAt(double factor, int mouseX, int mouseY, int imageWidth, int imageHeight,
		int windowWidth, int windowHeight);
	void Pan(double deltaX, double deltaY);

	ViewportRect Destination(int imageWidth, int imageHeight,
		int windowWidth, int windowHeight) const;

private:
	void SetManualZoom(double zoom);

	double zoom_ = 1.0;
	double offsetX_ = 0.0;
	double offsetY_ = 0.0;
	bool fitToWindow_ = true;
	bool fillWithCrop_ = false;
	bool noEnlarge_ = true;
};

} // namespace jpegview_linux
