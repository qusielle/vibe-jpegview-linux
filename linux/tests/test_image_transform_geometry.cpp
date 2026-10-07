#include "test_harness.h"

#include "image_transform_geometry.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using jpegview_linux::BuildFreeRotationGeometry;
using jpegview_linux::BuildPerspectiveGeometry;
using jpegview_linux::ImageTransformGeometry;
using jpegview_linux::MapDestinationToSource;
using jpegview_linux::PerspectiveCorrectionParameters;

void Expect(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

void ExpectMappedPoint(const ImageTransformGeometry& geometry,
	double destinationX, double destinationY, double expectedX, double expectedY) {
	double sourceX = 0.0;
	double sourceY = 0.0;
	Expect(MapDestinationToSource(geometry, destinationX, destinationY,
		sourceX, sourceY), "transform geometry could not map an output point");
	Expect(std::abs(sourceX - expectedX) < 1e-9 &&
		std::abs(sourceY - expectedY) < 1e-9,
		"transform geometry mapped an output point to the wrong source coordinate");
}

void ExpectContainedCorners(const ImageTransformGeometry& geometry) {
	const std::pair<double, double> corners[] = {
		{0.0, 0.0},
		{static_cast<double>(geometry.outputWidth - 1), 0.0},
		{static_cast<double>(geometry.outputWidth - 1),
			static_cast<double>(geometry.outputHeight - 1)},
		{0.0, static_cast<double>(geometry.outputHeight - 1)}};
	for (const auto& point : corners) {
		double sourceX = 0.0;
		double sourceY = 0.0;
		Expect(MapDestinationToSource(geometry, point.first, point.second,
			sourceX, sourceY) && sourceX >= -1e-8 && sourceY >= -1e-8 &&
			sourceX <= geometry.sourceWidth - 1 + 1e-8 &&
			sourceY <= geometry.sourceHeight - 1 + 1e-8,
			"perspective auto-crop mapped a corner outside its source image");
	}
}

void TestFreeRotationExactAnglesAndCoordinateMapping() {
	ImageTransformGeometry geometry;
	Expect(BuildFreeRotationGeometry(5, 3, 0.0, true, true, geometry) &&
		geometry.outputWidth == 5 && geometry.outputHeight == 3 &&
		geometry.exactClockwiseQuarterTurns == 0,
		"identity rotation did not preserve exact source geometry");
	ExpectMappedPoint(geometry, 0.0, 0.0, 0.0, 0.0);
	ExpectMappedPoint(geometry, 4.0, 2.0, 4.0, 2.0);

	Expect(BuildFreeRotationGeometry(5, 3, 90.0, false, false, geometry) &&
		geometry.outputWidth == 3 && geometry.outputHeight == 5 &&
		geometry.exactClockwiseQuarterTurns == 1,
		"clockwise quarter-turn geometry changed its exact dimensions");
	ExpectMappedPoint(geometry, 0.0, 0.0, 0.0, 2.0);
	ExpectMappedPoint(geometry, 2.0, 4.0, 4.0, 0.0);

	Expect(BuildFreeRotationGeometry(5, 3, -90.0, false, false, geometry) &&
		geometry.outputWidth == 3 && geometry.outputHeight == 5 &&
		geometry.exactClockwiseQuarterTurns == 3,
		"negative quarter-turn did not normalize to an exact counter-clockwise turn");
	ExpectMappedPoint(geometry, 0.0, 0.0, 4.0, 0.0);
	ExpectMappedPoint(geometry, 2.0, 4.0, 0.0, 2.0);

	Expect(BuildFreeRotationGeometry(5, 3, 180.0, false, false, geometry) &&
		geometry.outputWidth == 5 && geometry.outputHeight == 3 &&
		geometry.exactClockwiseQuarterTurns == 2,
		"half-turn geometry did not preserve exact dimensions");
	ExpectMappedPoint(geometry, 0.0, 0.0, 4.0, 2.0);
	ExpectMappedPoint(geometry, 4.0, 2.0, 0.0, 0.0);
}

void TestFreeRotationBoundsCroppingAndAspectPolicy() {
	ImageTransformGeometry expanded;
	ImageTransformGeometry cropped;
	ImageTransformGeometry aspectCropped;
	Expect(BuildFreeRotationGeometry(100, 50, 30.0, false, false, expanded) &&
		BuildFreeRotationGeometry(100, 50, 30.0, true, false, cropped) &&
		BuildFreeRotationGeometry(100, 50, 30.0, true, true, aspectCropped),
		"valid free-rotation bounds could not be calculated");
	Expect(expanded.outputWidth > 100 && expanded.outputHeight > 50 &&
		cropped.outputWidth < expanded.outputWidth &&
		cropped.outputHeight < expanded.outputHeight,
		"expanded bounds or contained auto-crop dimensions were not calculated");
	const double aspectRatio = static_cast<double>(aspectCropped.outputWidth) /
		aspectCropped.outputHeight;
	Expect(std::abs(aspectRatio - 2.0) < 0.08,
		"aspect-preserving auto-crop changed the source aspect ratio excessively");

	for (const ImageTransformGeometry* contained : {&cropped, &aspectCropped}) {
		const std::pair<double, double> corners[] = {
			{0.0, 0.0},
			{static_cast<double>(contained->outputWidth - 1), 0.0},
			{static_cast<double>(contained->outputWidth - 1),
				static_cast<double>(contained->outputHeight - 1)},
			{0.0, static_cast<double>(contained->outputHeight - 1)}};
		for (const auto& point : corners) {
			double sourceX = 0.0;
			double sourceY = 0.0;
			Expect(MapDestinationToSource(*contained, point.first, point.second,
				sourceX, sourceY) && sourceX >= -1e-8 && sourceY >= -1e-8 &&
				sourceX <= contained->sourceWidth - 1 + 1e-8 &&
				sourceY <= contained->sourceHeight - 1 + 1e-8,
				"auto-crop boundary included pixels outside the rotated source rectangle");
		}
	}

	Expect(BuildFreeRotationGeometry(5, 3, 45.0, false, false, expanded) &&
		expanded.outputWidth == 6 && expanded.outputHeight == 6,
		"non-orthogonal rotation did not round its expanded bounds outward");
}

void TestFreeRotationRejectsUnsafeInputAndHandlesSingleAxisImages() {
	ImageTransformGeometry geometry;
	geometry.outputWidth = 71;
	Expect(!BuildFreeRotationGeometry(0, 2, 15.0, false, false, geometry) &&
		geometry.outputWidth == 71,
		"invalid input modified the caller's existing geometry");
	Expect(!BuildFreeRotationGeometry(65536, 2, 15.0, false, false, geometry) &&
		!BuildFreeRotationGeometry(40000, 40000, 15.0, false, false, geometry) &&
		!BuildFreeRotationGeometry(2, 2, std::numeric_limits<double>::infinity(),
			false, false, geometry) &&
		!BuildFreeRotationGeometry(2, 2, std::numeric_limits<double>::quiet_NaN(),
			false, false, geometry),
		"rotation geometry accepted unsafe dimensions or a nonfinite angle");

	Expect(BuildFreeRotationGeometry(1, 9, 25.0, false, false, geometry) &&
		geometry.outputWidth > 1 && geometry.outputHeight > 1,
		"expanded rotation divided by a zero source-axis span");
	Expect(BuildFreeRotationGeometry(1, 9, 25.0, true, true, geometry) &&
		geometry.outputWidth == 1 && geometry.outputHeight == 1,
		"auto-cropped single-axis source did not use its documented one-pixel fallback");
}

void TestFreeRotationQuarterTurnsHonorAspectCrop() {
	for (double angle : {-90.0, 90.0}) {
		ImageTransformGeometry cropped;
		Expect(BuildFreeRotationGeometry(4000, 2500, angle, true, true, cropped) &&
			cropped.outputWidth == 2500 && cropped.outputHeight == 1562 &&
			cropped.exactClockwiseQuarterTurns == -1,
			"exact quarter-turn bypassed the source-aspect auto-crop policy");
		for (double adjacentAngle : {angle - 0.1, angle + 0.1}) {
			ImageTransformGeometry adjacent;
			Expect(BuildFreeRotationGeometry(4000, 2500, adjacentAngle, true, true,
				adjacent) && std::abs(adjacent.outputWidth - cropped.outputWidth) <= 4 &&
				std::abs(adjacent.outputHeight - cropped.outputHeight) <= 4,
				"aspect-preserving rotation dimensions jumped at a quarter-turn");
		}
		for (bool preserveAspectRatio : {false, true}) {
			ImageTransformGeometry fullFrame;
			Expect(BuildFreeRotationGeometry(4000, 2500, angle, false,
				preserveAspectRatio, fullFrame) && fullFrame.outputWidth == 2500 &&
				fullFrame.outputHeight == 4000 && fullFrame.exactClockwiseQuarterTurns >= 0,
				"uncropped quarter-turn stopped preserving the exact full-frame mapping");
		}
		ImageTransformGeometry unconstrained;
		Expect(BuildFreeRotationGeometry(4000, 2500, angle, true, false,
			unconstrained) && unconstrained.outputWidth == 2500 &&
			unconstrained.outputHeight == 4000 &&
			unconstrained.exactClockwiseQuarterTurns >= 0,
			"unconstrained auto-crop discarded part of an exact quarter-turn");
	}
}

void TestPerspectiveGeometryIdentityAndProjectiveMapping() {
	PerspectiveCorrectionParameters parameters;
	ImageTransformGeometry identity;
	Expect(BuildPerspectiveGeometry(100, 60, parameters, identity) &&
		identity.outputWidth == 100 && identity.outputHeight == 60 &&
		identity.exactClockwiseQuarterTurns == 0,
		"zero perspective parameters did not preserve the exact source geometry");
	ExpectMappedPoint(identity, 0.0, 0.0, 0.0, 0.0);
	ExpectMappedPoint(identity, 99.0, 59.0, 99.0, 59.0);

	parameters.leftDeltaFraction = 0.1;
	parameters.rightDeltaFraction = -0.1;
	parameters.autoCrop = false;
	ImageTransformGeometry perspective;
	Expect(BuildPerspectiveGeometry(100, 60, parameters, perspective) &&
		perspective.outputWidth > 100 && perspective.outputHeight == 60 &&
		perspective.exactClockwiseQuarterTurns == -1,
		"uncropped perspective geometry did not expand its trapezoid bounds");
	const double left = 0.1 * 99.0;
	const double right = -0.1 * 99.0;
	const double outputOriginX = std::ceil(left);
	ExpectMappedPoint(perspective, left + outputOriginX, 0.0, 0.0, 0.0);
	ExpectMappedPoint(perspective, 99.0 + right + outputOriginX, 0.0, 99.0, 0.0);
	ExpectMappedPoint(perspective, -left + outputOriginX, 59.0, 0.0, 59.0);
	ExpectMappedPoint(perspective, 99.0 - right + outputOriginX, 59.0, 99.0, 59.0);

	const double topWidth = 99.0 + right - left;
	const double bottomWidth = 99.0 - right + left;
	double sourceX = 0.0;
	double sourceY = 0.0;
	Expect(MapDestinationToSource(perspective, 49.5 + outputOriginX, 29.5,
		sourceX, sourceY), "perspective geometry failed to map its center scanline");
	const double expectedSourceY = 59.0 * bottomWidth / (topWidth + bottomWidth);
	Expect(std::abs(sourceY - expectedSourceY) < 1e-9 &&
		std::abs(sourceY - 29.5) > 1.0,
		"perspective geometry used affine rather than projective vertical mapping");
}

void TestPerspectiveGeometryCroppingAndAspectPolicy() {
	PerspectiveCorrectionParameters parameters;
	parameters.leftDeltaFraction = 0.1;
	parameters.rightDeltaFraction = -0.1;
	ImageTransformGeometry cropped;
	Expect(BuildPerspectiveGeometry(100, 60, parameters, cropped) &&
		cropped.outputWidth < 100 && cropped.outputHeight == 60,
		"perspective auto-crop did not select the contained intersection rectangle");
	ExpectContainedCorners(cropped);

	parameters.preserveAspectRatio = true;
	ImageTransformGeometry aspectCropped;
	Expect(BuildPerspectiveGeometry(100, 60, parameters, aspectCropped) &&
		aspectCropped.outputWidth < 100 && aspectCropped.outputHeight < 60 &&
		aspectCropped.outputHeight < cropped.outputHeight,
		"aspect-preserving perspective crop did not reduce the contained rectangle");
	ExpectContainedCorners(aspectCropped);
	const double sourceAspect = 99.0 / 59.0;
	const double outputAspect = static_cast<double>(aspectCropped.outputWidth - 1) /
		(aspectCropped.outputHeight - 1);
	Expect(std::abs(outputAspect - sourceAspect) < 0.04,
		"aspect-preserving perspective crop changed the source pixel-center aspect ratio");
}

void TestPerspectiveGeometryRejectsUnsafeParametersAndDimensions() {
	PerspectiveCorrectionParameters parameters;
	ImageTransformGeometry geometry;
	geometry.outputWidth = 71;
	parameters.leftDeltaFraction = 0.250001;
	Expect(!BuildPerspectiveGeometry(100, 60, parameters, geometry) &&
		geometry.outputWidth == 71,
		"perspective geometry accepted an out-of-range parameter or modified output");
	parameters.leftDeltaFraction = std::numeric_limits<double>::quiet_NaN();
	Expect(!BuildPerspectiveGeometry(100, 60, parameters, geometry),
		"perspective geometry accepted a nonfinite parameter");
	parameters.leftDeltaFraction = 0.0;
	Expect(!BuildPerspectiveGeometry(1, 60, parameters, geometry) &&
		!BuildPerspectiveGeometry(60, 1, parameters, geometry),
		"perspective geometry accepted a source with no two-dimensional pixel span");

	parameters.autoCrop = false;
	parameters.leftDeltaFraction = 0.25;
	parameters.rightDeltaFraction = -0.25;
	Expect(BuildPerspectiveGeometry(100, 60, parameters, geometry) &&
		geometry.outputWidth <= 65535 && geometry.outputHeight == 60,
		"maximum supported perspective adjustments were rejected");
	Expect(!BuildPerspectiveGeometry(65535, 1500, parameters, geometry),
		"perspective geometry accepted expanded bounds beyond image limits");
}

const TestCase kTests[] = {
	{"free-rotation-exact-angles-and-coordinate-mapping",
		&TestFreeRotationExactAnglesAndCoordinateMapping},
	{"free-rotation-bounds-cropping-and-aspect-policy",
		&TestFreeRotationBoundsCroppingAndAspectPolicy},
	{"free-rotation-rejects-unsafe-input-and-handles-single-axis-images",
		&TestFreeRotationRejectsUnsafeInputAndHandlesSingleAxisImages},
	{"free-rotation-quarter-turns-honor-aspect-crop",
		&TestFreeRotationQuarterTurnsHonorAspectCrop},
	{"perspective-geometry-identity-and-projective-mapping",
		&TestPerspectiveGeometryIdentityAndProjectiveMapping},
	{"perspective-geometry-cropping-and-aspect-policy",
		&TestPerspectiveGeometryCroppingAndAspectPolicy},
	{"perspective-geometry-rejects-unsafe-parameters-and-dimensions",
		&TestPerspectiveGeometryRejectsUnsafeParametersAndDimensions},
};

} // namespace

const TestSuite& GetImageTransformGeometrySuite() {
	static const TestSuite suite{"image_transform_geometry", kTests,
		sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
