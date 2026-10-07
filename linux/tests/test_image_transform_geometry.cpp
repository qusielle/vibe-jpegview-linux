#include "test_harness.h"

#include "image_transform_geometry.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using jpegview_linux::BuildFreeRotationGeometry;
using jpegview_linux::ImageTransformGeometry;
using jpegview_linux::MapDestinationToSource;

void Expect(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

void ExpectMappedPoint(const ImageTransformGeometry& geometry,
	double destinationX, double destinationY, double expectedX, double expectedY) {
	double sourceX = 0.0;
	double sourceY = 0.0;
	Expect(MapDestinationToSource(geometry, destinationX, destinationY,
		sourceX, sourceY), "rotation geometry could not map an output point");
	Expect(std::abs(sourceX - expectedX) < 1e-9 &&
		std::abs(sourceY - expectedY) < 1e-9,
		"rotation geometry mapped an output point to the wrong source coordinate");
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

const TestCase kTests[] = {
	{"free-rotation-exact-angles-and-coordinate-mapping",
		&TestFreeRotationExactAnglesAndCoordinateMapping},
	{"free-rotation-bounds-cropping-and-aspect-policy",
		&TestFreeRotationBoundsCroppingAndAspectPolicy},
	{"free-rotation-rejects-unsafe-input-and-handles-single-axis-images",
		&TestFreeRotationRejectsUnsafeInputAndHandlesSingleAxisImages},
};

} // namespace

const TestSuite& GetImageTransformGeometrySuite() {
	static const TestSuite suite{"image_transform_geometry", kTests,
		sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
