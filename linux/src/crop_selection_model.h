#pragma once

#include <cstdint>

namespace jpegview_linux {

// Half-open image-space bounds: left/top are included, right/bottom excluded.
struct SelectionRect {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;

	int Width() const { return right - left; }
	int Height() const { return bottom - top; }
	bool Valid() const { return left >= 0 && top >= 0 && right > left && bottom > top; }
};

struct SelectionScreenRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct SelectionPoint {
	int x = 0;
	int y = 0;
};

enum class CropSelectionMode {
	Free,
	FixedAspect,
	ImageAspect,
	FixedSize,
};

bool ShouldStartNewCropSelection(bool selectionModeEnabled,
	bool forcedByModifier, bool imageNeedsPanning);

enum class CropSelectionHandle {
	None,
	NewSelection,
	Move,
	Left,
	Right,
	Top,
	Bottom,
	TopLeft,
	TopRight,
	BottomLeft,
	BottomRight,
};

// Stores crop bounds in source-image pixels. SDL coordinates are converted at
// the adapter boundary so zooming and panning never change the selected pixels.
class CropSelectionModel {
public:
	void SetImageSize(int width, int height);
	void SetMode(CropSelectionMode mode) { mode_ = mode; }
	CropSelectionMode Mode() const { return mode_; }
	void SetAspectRatio(int width, int height);
	void SetFixedSize(int width, int height, bool screenPixels);

	bool StartNew(int imageX, int imageY);
	bool StartManipulation(int imageX, int imageY, CropSelectionHandle handle);
	bool Update(int imageX, int imageY, double zoom = 1.0);
	// Applies a newly selected aspect or fixed-size mode to existing bounds,
	// retaining their top-left corner and using the current view zoom if needed.
	bool ReapplyMode(double zoom = 1.0);
	void End() { dragging_ = false; handle_ = CropSelectionHandle::None; }
	void Clear();
	void Cancel() { End(); }

	bool IsDragging() const { return dragging_; }
	bool HasSelection() const { return selection_.Valid(); }
	const SelectionRect& Rect() const { return selection_; }
	int ImageWidth() const { return imageWidth_; }
	int ImageHeight() const { return imageHeight_; }
	double AspectRatio() const;
	int FixedWidth() const { return fixedWidth_; }
	int FixedHeight() const { return fixedHeight_; }
	bool FixedSizeUsesScreenPixels() const { return fixedSizeScreenPixels_; }

	static SelectionScreenRect ToScreen(const SelectionRect& selection,
		const SelectionScreenRect& imageDestination, int imageWidth, int imageHeight);
	static SelectionPoint ScreenToImage(int screenX, int screenY,
		const SelectionScreenRect& imageDestination, int imageWidth, int imageHeight);
	static CropSelectionHandle HitTest(int screenX, int screenY,
		const SelectionScreenRect& selectionOnScreen, bool fixedSizeMode,
		int tolerance = 12);
	static SelectionRect AlignToMcu(const SelectionRect& selection, int imageWidth,
		int imageHeight, int mcuWidth, int mcuHeight);

private:
	SelectionRect BuildNewSelection(int imageX, int imageY, double zoom) const;
	SelectionRect BuildResizedSelection(int imageX, int imageY) const;
	SelectionRect ClampRect(SelectionRect rect) const;
	double EffectiveAspectRatio() const;
	void ClampPoint(int& x, int& y) const;

	int imageWidth_ = 0;
	int imageHeight_ = 0;
	CropSelectionMode mode_ = CropSelectionMode::Free;
	int aspectWidth_ = 1;
	int aspectHeight_ = 1;
	int fixedWidth_ = 320;
	int fixedHeight_ = 200;
	bool fixedSizeScreenPixels_ = true;
	SelectionRect selection_;
	SelectionRect manipulationStart_;
	SelectionPoint pointerStart_;
	SelectionPoint creationAnchor_;
	CropSelectionHandle handle_ = CropSelectionHandle::None;
	bool dragging_ = false;
};

} // namespace jpegview_linux
