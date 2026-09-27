#pragma once

namespace jpegview_linux {

struct MagnifyingGlassRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

enum class MagnifyingGlassWheelDirection {
	Up,
	Down
};

struct MagnifyingGlassWheelModifiers {
	bool control = false;
	bool alt = false;
	bool shift = false;
};

class MagnifyingGlassModel {
public:
	static constexpr int kDefaultWidth = 350;
	static constexpr int kDefaultHeight = 175;
	static constexpr int kMinimumWidth = 175;
	static constexpr int kMinimumHeight = 80;
	static constexpr int kMaximumDimension = 16384;
	static constexpr double kDefaultZoomLevel = 0.5;
	static constexpr double kMinimumZoomLevel = 0.2;
	static constexpr double kMaximumZoomLevel = 0.9;

	bool Enabled() const { return enabled_; }
	void SetEnabled(bool enabled) { enabled_ = enabled; }
	void Toggle() { enabled_ = !enabled_; }

	int Width() const { return width_; }
	int Height() const { return height_; }
	double ZoomLevel() const { return zoomLevel_; }
	void SetParameters(int width, int height, double zoomLevel,
		int parentWidth, int parentHeight);

	void HandleWheel(MagnifyingGlassWheelDirection direction,
		const MagnifyingGlassWheelModifiers& modifiers, int parentWidth, int parentHeight);

private:
	static int MaximumDimension(int parentDimension, int minimum);
	void ConstrainSize(int parentWidth, int parentHeight);

	bool enabled_ = false;
	int width_ = kDefaultWidth;
	int height_ = kDefaultHeight;
	double zoomLevel_ = kDefaultZoomLevel;
};

struct MagnifyingGlassGeometry {
	bool valid = false;
	MagnifyingGlassRect lensRect;
	MagnifyingGlassRect sourceRect;
	MagnifyingGlassRect contentDestinationRect;
};

// Maps a pointer inside the displayed image to a 1 / zoomLevel crop from the
// native texture. The lens stays centered on the pointer. The content rectangle
// may be smaller than the lens when the requested source crop crosses an image edge.
MagnifyingGlassGeometry CalculateMagnifyingGlassGeometry(double pointerX, double pointerY,
	const MagnifyingGlassRect& displayedImage, int textureWidth, int textureHeight,
	int lensWidth, int lensHeight, double zoomLevel);

} // namespace jpegview_linux
