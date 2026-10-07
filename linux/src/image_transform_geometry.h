#pragma once

#include <array>

namespace jpegview_linux {

struct ImageTransformGeometry {
	int sourceWidth = 0;
	int sourceHeight = 0;
	int outputWidth = 0;
	int outputHeight = 0;
	// Maps output pixel-center coordinates to source pixel-center coordinates.
	std::array<double, 9> destinationToSource{};
	// A nonnegative value identifies an exact clockwise quarter-turn mapping.
	// General rotations use -1 and are sampled through the matrix above.
	int exactClockwiseQuarterTurns = -1;
};

struct PerspectiveCorrectionParameters {
	// Horizontal edge movement as a fraction of the source pixel-center width.
	// Positive values move the top corners right and the matching bottom corners left.
	double leftDeltaFraction = 0.0;
	double rightDeltaFraction = 0.0;
	bool autoCrop = true;
	bool preserveAspectRatio = false;
};

// Builds a centered, positive-clockwise rotation. Auto-crop selects the largest
// centered rectangle contained by the rotated source quadrilateral; when
// preserveAspectRatio is true, that rectangle keeps the source aspect ratio.
bool BuildFreeRotationGeometry(int sourceWidth, int sourceHeight,
	double clockwiseDegrees, bool autoCrop, bool preserveAspectRatio,
	ImageTransformGeometry& geometry);

// Builds a projective correction from a source rectangle to a symmetric
// horizontal trapezoid. Auto-crop selects a centered rectangle contained by
// the trapezoid; aspect preservation constrains that rectangle to the source's
// pixel-center aspect ratio. Without auto-crop, the expanded canvas leaves
// uncovered areas transparent during resampling.
bool BuildPerspectiveGeometry(int sourceWidth, int sourceHeight,
	const PerspectiveCorrectionParameters& parameters,
	ImageTransformGeometry& geometry);

bool MapDestinationToSource(const ImageTransformGeometry& geometry,
	double destinationX, double destinationY,
	double& sourceX, double& sourceY);

} // namespace jpegview_linux
