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

// Builds a centered, positive-clockwise rotation. Auto-crop selects the largest
// centered rectangle contained by the rotated source quadrilateral; when
// preserveAspectRatio is true, that rectangle keeps the source aspect ratio.
bool BuildFreeRotationGeometry(int sourceWidth, int sourceHeight,
	double clockwiseDegrees, bool autoCrop, bool preserveAspectRatio,
	ImageTransformGeometry& geometry);

bool MapDestinationToSource(const ImageTransformGeometry& geometry,
	double destinationX, double destinationY,
	double& sourceX, double& sourceY);

} // namespace jpegview_linux
