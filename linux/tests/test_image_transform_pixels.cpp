#include "test_harness.h"

#include "image_transform_pixels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

using jpegview_linux::BuildFreeRotationGeometry;
using jpegview_linux::BuildPerspectiveGeometry;
using jpegview_linux::Image;
using jpegview_linux::ImageTransformGeometry;
using jpegview_linux::ImageTransformSampling;
using jpegview_linux::PerspectiveCorrectionParameters;
using jpegview_linux::ResampleImageTransform;

void Expect(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

Image MakeImage(int width, int height, const std::vector<std::uint8_t>& pixels) {
	Image image;
	Expect(image.StoreBGRA(pixels.data(), width, height, true),
		"test image pixels could not be stored");
	return image;
}

void TestFreeRotationExactTurnsPreservePixels() {
	const Image source = MakeImage(2, 3, {
		1, 2, 3, 255, 4, 5, 6, 255,
		7, 8, 9, 255, 10, 11, 12, 255,
		13, 14, 15, 255, 16, 17, 18, 255});
	ImageTransformGeometry geometry;
	Image output;
	Expect(BuildFreeRotationGeometry(source.width, source.height, 90.0,
		false, false, geometry) && ResampleImageTransform(source, geometry,
		ImageTransformSampling::FinalBicubic, output),
		"exact clockwise rotation could not be resampled");
	Expect(output.width == 3 && output.height == 2 &&
		output.originalWidth == source.originalWidth &&
		output.originalHeight == source.originalHeight,
		"exact clockwise rotation changed dimensions or source metadata");
	const std::vector<std::uint8_t> expected = {
		13, 14, 15, 255, 7, 8, 9, 255, 1, 2, 3, 255,
		16, 17, 18, 255, 10, 11, 12, 255, 4, 5, 6, 255};
	Expect(output.bgra == expected,
		"exact quarter-turn resampling changed source pixel values");

	Expect(BuildFreeRotationGeometry(source.width, source.height, 360.0,
		false, false, geometry) && ResampleImageTransform(source, geometry,
		ImageTransformSampling::PreviewBilinear, output) &&
		output.width == source.width && output.height == source.height &&
		output.bgra == source.bgra,
		"full-turn identity did not preserve all pixels exactly");
}

void TestFreeRotationBilinearAlphaAndTransparentCanvas() {
	const Image source = MakeImage(2, 2, {
		200, 0, 0, 255, 0, 0, 255, 0,
		200, 0, 0, 255, 200, 0, 0, 255});
	ImageTransformGeometry geometry;
	Image output;
	Expect(BuildFreeRotationGeometry(2, 2, 45.0, false, false, geometry) &&
		ResampleImageTransform(source, geometry,
			ImageTransformSampling::PreviewBilinear, output),
		"bilinear free-rotation resampling failed");
	Expect(output.width == 3 && output.height == 3,
		"expanded rotation did not allocate its rounded output canvas");
	const std::size_t center = (static_cast<std::size_t>(1) * output.width + 1) * 4;
	Expect(output.bgra[center] == 200 && output.bgra[center + 2] == 0 &&
		output.bgra[center + 3] == 191,
		"bilinear sampling mixed hidden transparent RGB into visible pixels");
	Expect(output.hasTransparency && output.bgra[3] < 255,
		"expanded rotation did not retain transparent pixels outside the source polygon");
}

void TestFreeRotationBicubicSamplingAndCancellation() {
	std::vector<std::uint8_t> pixels(7 * 7 * 4);
	for (int y = 0; y < 7; ++y) {
		for (int x = 0; x < 7; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * 7 + x) * 4;
			pixels[offset] = static_cast<std::uint8_t>(x * 10 + y * 20);
			pixels[offset + 1] = static_cast<std::uint8_t>(x * 5 + y * 10);
			pixels[offset + 2] = 33;
			pixels[offset + 3] = 255;
		}
	}
	const Image source = MakeImage(7, 7, pixels);
	ImageTransformGeometry geometry;
	Image output;
	Expect(BuildFreeRotationGeometry(7, 7, 45.0, false, false, geometry) &&
		ResampleImageTransform(source, geometry,
			ImageTransformSampling::FinalBicubic, output),
		"bicubic free-rotation resampling failed");
	const std::size_t sampleOffset = (static_cast<std::size_t>(4) * output.width + 5) * 4;
	Expect(output.bgra[sampleOffset] == 76 && output.bgra[sampleOffset + 1] == 38 &&
		output.bgra[sampleOffset + 2] == 33,
		"bicubic sampling did not preserve an interior linear color gradient");

	Image unchanged = MakeImage(1, 1, {9, 8, 7, 6});
	const std::vector<std::uint8_t> original = unchanged.bgra;
	int checks = 0;
	const bool completed = ResampleImageTransform(source, geometry,
		ImageTransformSampling::FinalBicubic, unchanged, [&checks] {
			return ++checks < 3;
		});
	Expect(!completed && unchanged.width == 1 && unchanged.height == 1 &&
		unchanged.bgra == original,
		"cancelled resampling published a partial output image");
}

void TestFreeRotationRejectsMismatchedGeometry() {
	const Image source = MakeImage(2, 2, {
		1, 2, 3, 255, 4, 5, 6, 255,
		7, 8, 9, 255, 10, 11, 12, 255});
	ImageTransformGeometry geometry;
	Image output = MakeImage(1, 1, {90, 80, 70, 60});
	const std::vector<std::uint8_t> original = output.bgra;
	Expect(BuildFreeRotationGeometry(3, 2, 30.0, false, false, geometry) &&
		!ResampleImageTransform(source, geometry,
			ImageTransformSampling::FinalBicubic, output) &&
		output.width == 1 && output.height == 1 && output.bgra == original,
		"resampling accepted geometry for another source or modified output on failure");

	geometry.sourceWidth = source.width;
	geometry.sourceHeight = source.height;
	geometry.outputWidth = 0;
	Expect(!ResampleImageTransform(source, geometry,
		ImageTransformSampling::PreviewBilinear, output) && output.bgra == original,
		"resampling accepted an invalid output extent");
}

void TestFreeRotationSingleRowCropKeepsColumnMapping() {
	std::vector<std::uint8_t> pixels(4 * 2 * 4, 0);
	for (int y = 0; y < 2; ++y) {
		for (int x = 0; x < 4; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * 4 + x) * 4;
			pixels[offset] = static_cast<std::uint8_t>(y * 200);
			pixels[offset + 3] = 255;
		}
	}
	const Image source = MakeImage(4, 2, pixels);
	ImageTransformGeometry geometry;
	Expect(BuildFreeRotationGeometry(source.width, source.height, 90.0,
		true, true, geometry) && geometry.outputWidth == 2 &&
		geometry.outputHeight == 1 && geometry.exactClockwiseQuarterTurns == -1,
		"aspect-preserving rotation did not produce the expected single-row crop");
	for (ImageTransformSampling sampling : {ImageTransformSampling::PreviewBilinear,
		ImageTransformSampling::FinalBicubic}) {
		Image output;
		Expect(ResampleImageTransform(source, geometry, sampling, output) &&
			output.bgra[0] == 200 && output.bgra[4] == 0 &&
			output.bgra[3] == 255 && output.bgra[7] == 255,
			"single-row rotation repeated its first source row across output columns");
	}
}

Image MakePerspectiveRampImage() {
	std::vector<std::uint8_t> pixels(5 * 5 * 4);
	for (int y = 0; y < 5; ++y) {
		for (int x = 0; x < 5; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * 5 + x) * 4;
			pixels[offset] = static_cast<std::uint8_t>(y * 20);
			pixels[offset + 1] = static_cast<std::uint8_t>(x * 20);
			pixels[offset + 2] = 37;
			pixels[offset + 3] = 255;
		}
	}
	return MakeImage(5, 5, pixels);
}

void TestPerspectiveResamplingUsesProjectiveCoordinates() {
	const Image source = MakePerspectiveRampImage();
	PerspectiveCorrectionParameters parameters;
	parameters.leftDeltaFraction = 0.2;
	parameters.rightDeltaFraction = -0.1;
	parameters.autoCrop = false;
	ImageTransformGeometry geometry;
	Expect(BuildPerspectiveGeometry(source.width, source.height, parameters, geometry) &&
		geometry.outputWidth == 7 && geometry.outputHeight == source.height,
		"perspective test geometry could not create its expanded canvas");

	const double topWidth = 4.0 - 0.4 - 0.8;
	const double bottomWidth = 4.0 + 0.4 + 0.8;
	const double targetY = 2.0;
	const double expectedSourceY = 4.0 * bottomWidth * (targetY / 4.0) /
		(topWidth - (topWidth - bottomWidth) * (targetY / 4.0));
	const std::uint8_t expectedRamp = static_cast<std::uint8_t>(
		std::lround(expectedSourceY * 20.0));
	Expect(expectedRamp == 52,
		"test's independent projective coordinate oracle changed unexpectedly");

	for (ImageTransformSampling sampling : {ImageTransformSampling::PreviewBilinear,
		ImageTransformSampling::FinalBicubic}) {
		Image output;
		Expect(ResampleImageTransform(source, geometry, sampling, output),
			"projective image transform could not be resampled");
		const std::size_t sample = (static_cast<std::size_t>(2) * output.width + 3) * 4;
		Expect(output.bgra[sample] == expectedRamp && output.bgra[sample + 2] == 37 &&
			output.bgra[sample + 3] == 255,
			"projective resampling did not use the nonlinear homography coordinates");
		Expect(output.bgra[sample] != 40,
			"projective resampling regressed to affine vertical interpolation");
	}
}

void TestProjectiveResamplingSupportsColumnDenominatorVariation() {
	std::vector<std::uint8_t> pixels(3 * 3 * 4);
	for (int y = 0; y < 3; ++y) {
		for (int x = 0; x < 3; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * 3 + x) * 4;
			pixels[offset] = static_cast<std::uint8_t>(x * 100);
			pixels[offset + 1] = static_cast<std::uint8_t>(y * 100);
			pixels[offset + 2] = 33;
			pixels[offset + 3] = 255;
		}
	}
	const Image source = MakeImage(3, 3, pixels);
	ImageTransformGeometry geometry;
	geometry.sourceWidth = source.width;
	geometry.sourceHeight = source.height;
	geometry.outputWidth = 3;
	geometry.outputHeight = 3;
	geometry.destinationToSource = {1.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 0.1, 0.0, 1.0};
	Image output;
	Expect(ResampleImageTransform(source, geometry,
		ImageTransformSampling::PreviewBilinear, output),
		"valid column-varying projective geometry could not be sampled");
	const std::size_t first = (static_cast<std::size_t>(1) * output.width + 1) * 4;
	const std::size_t second = (static_cast<std::size_t>(1) * output.width + 2) * 4;
	Expect(output.bgra[first] == 91 && output.bgra[first + 1] == 91 &&
		output.bgra[second] == 167 && output.bgra[second + 1] == 83,
		"projective sampling ignored denominator variation across columns");
}

void TestProjectiveResamplingPreservesAlphaAndExactIdentity() {
	const Image source = MakeImage(2, 2, {
		0, 0, 200, 255, 255, 0, 0, 0,
		0, 0, 200, 255, 0, 0, 200, 255});
	ImageTransformGeometry projective;
	projective.sourceWidth = source.width;
	projective.sourceHeight = source.height;
	projective.outputWidth = 1;
	projective.outputHeight = 1;
	projective.destinationToSource = {1.0, 0.0, 0.5,
		0.0, 1.0, 0.5, 0.0, 0.25, 1.0};
	projective.exactClockwiseQuarterTurns = -1;
	Image sampled;
	Expect(ResampleImageTransform(source, projective,
		ImageTransformSampling::PreviewBilinear, sampled) &&
		sampled.bgra[0] == 0 && sampled.bgra[1] == 0 &&
		sampled.bgra[2] == 200 && sampled.bgra[3] == 191 &&
		sampled.hasTransparency,
		"projective interpolation mixed hidden RGB into a partially transparent sample");

	ImageTransformGeometry scaledProjective = projective;
	for (double& value : scaledProjective.destinationToSource) value *= 1e-200;
	Image scaledSampled;
	Expect(ResampleImageTransform(source, scaledProjective,
		ImageTransformSampling::PreviewBilinear, scaledSampled) &&
		scaledSampled.bgra == sampled.bgra,
		"projective sampling changed under an equivalent small matrix scale");

	const Image affineSource = MakeImage(2, 2, {
		1, 2, 3, 255, 4, 5, 6, 255,
		7, 8, 9, 255, 10, 11, 12, 255});
	ImageTransformGeometry scaledAffine;
	Expect(BuildFreeRotationGeometry(affineSource.width, affineSource.height, 0.0,
		false, false, scaledAffine), "affine identity geometry could not be built");
	scaledAffine.exactClockwiseQuarterTurns = -1;
	for (double& value : scaledAffine.destinationToSource) value *= 1e-200;
	Image scaledAffineOutput;
	Expect(ResampleImageTransform(affineSource, scaledAffine,
		ImageTransformSampling::PreviewBilinear, scaledAffineOutput) &&
		scaledAffineOutput.bgra == affineSource.bgra,
		"affine sampling changed under an equivalent small matrix scale");

	PerspectiveCorrectionParameters identityParameters;
	ImageTransformGeometry identityGeometry;
	const Image identitySource = MakeImage(3, 2, {
		1, 2, 3, 255, 4, 5, 6, 0, 7, 8, 9, 255,
		10, 11, 12, 255, 13, 14, 15, 255, 16, 17, 18, 255});
	Image identityOutput;
	Expect(BuildPerspectiveGeometry(identitySource.width, identitySource.height,
		identityParameters, identityGeometry) &&
		ResampleImageTransform(identitySource, identityGeometry,
			ImageTransformSampling::FinalBicubic, identityOutput) &&
		identityOutput.bgra == identitySource.bgra,
		"identity perspective resampling did not preserve source pixels exactly");
}

void TestProjectiveResamplingCentersSinglePixelOutputAxis() {
	const Image source = MakeImage(2, 2, {
		0, 0, 0, 255, 100, 0, 0, 255,
		0, 0, 0, 255, 100, 0, 0, 255});
	PerspectiveCorrectionParameters parameters;
	parameters.leftDeltaFraction = 0.25;
	parameters.rightDeltaFraction = 0.25;
	ImageTransformGeometry geometry;
	Image output;
	Expect(BuildPerspectiveGeometry(source.width, source.height, parameters, geometry) &&
		geometry.outputWidth == 1 && geometry.outputHeight == 2 &&
		ResampleImageTransform(source, geometry,
			ImageTransformSampling::PreviewBilinear, output) &&
		output.bgra[0] == 25 && output.bgra[4] == 75,
		"single-pixel crop axis did not sample through the centered crop region");
}

void TestProjectiveResamplingRejectsPolesAndPreservesOutputOnCancellation() {
	const Image source = MakeImage(3, 3, std::vector<std::uint8_t>(3 * 3 * 4, 255));
	Image output = MakeImage(1, 1, {9, 8, 7, 6});
	const std::vector<std::uint8_t> original = output.bgra;
	ImageTransformGeometry geometry;
	geometry.sourceWidth = source.width;
	geometry.sourceHeight = source.height;
	geometry.outputWidth = 3;
	geometry.outputHeight = 3;
	geometry.destinationToSource = {1.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 1.0, 0.0, -0.5};
	Expect(!ResampleImageTransform(source, geometry,
		ImageTransformSampling::PreviewBilinear, output) &&
		output.width == 1 && output.height == 1 && output.bgra == original,
		"projective resampling accepted a denominator pole across its output canvas");

	geometry.destinationToSource = {1.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 0.0, 0.0, 1e-15};
	Expect(!ResampleImageTransform(source, geometry,
		ImageTransformSampling::PreviewBilinear, output) && output.bgra == original,
		"projective resampling accepted a nearly singular source mapping");

	geometry.destinationToSource = {1.0, 0.0, 0.0,
		0.0, 0.0, 0.0, 0.0, 0.0, 1.0};
	Expect(!ResampleImageTransform(source, geometry,
		ImageTransformSampling::PreviewBilinear, output) && output.bgra == original,
		"projective resampling accepted a singular two-dimensional mapping");

	geometry.destinationToSource = {1.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 1.0, 0.0, 1e-13};
	Expect(!ResampleImageTransform(source, geometry,
		ImageTransformSampling::PreviewBilinear, output) && output.bgra == original,
		"projective resampling accepted a denominator too close to a pole");

	PerspectiveCorrectionParameters parameters;
	parameters.leftDeltaFraction = 0.2;
	parameters.rightDeltaFraction = -0.1;
	parameters.autoCrop = false;
	Expect(BuildPerspectiveGeometry(source.width, source.height, parameters, geometry),
		"valid projective geometry could not be rebuilt after rejection");
	int checks = 0;
	const bool completed = ResampleImageTransform(source, geometry,
		ImageTransformSampling::FinalBicubic, output, [&checks] {
			return ++checks < 3;
		});
	Expect(!completed && output.width == 1 && output.height == 1 &&
		output.bgra == original,
		"cancelled projective resampling published a partial output image");
}

const TestCase kTests[] = {
	{"free-rotation-exact-turns-preserve-pixels",
		&TestFreeRotationExactTurnsPreservePixels},
	{"free-rotation-bilinear-alpha-and-transparent-canvas",
		&TestFreeRotationBilinearAlphaAndTransparentCanvas},
	{"free-rotation-bicubic-sampling-and-cancellation",
		&TestFreeRotationBicubicSamplingAndCancellation},
	{"free-rotation-rejects-mismatched-geometry",
		&TestFreeRotationRejectsMismatchedGeometry},
	{"free-rotation-single-row-crop-keeps-column-mapping",
		&TestFreeRotationSingleRowCropKeepsColumnMapping},
	{"perspective-resampling-uses-projective-coordinates",
		&TestPerspectiveResamplingUsesProjectiveCoordinates},
	{"projective-resampling-supports-column-denominator-variation",
		&TestProjectiveResamplingSupportsColumnDenominatorVariation},
	{"projective-resampling-preserves-alpha-and-exact-identity",
		&TestProjectiveResamplingPreservesAlphaAndExactIdentity},
	{"projective-resampling-centers-single-pixel-output-axis",
		&TestProjectiveResamplingCentersSinglePixelOutputAxis},
	{"projective-resampling-rejects-poles-and-preserves-output-on-cancellation",
		&TestProjectiveResamplingRejectsPolesAndPreservesOutputOnCancellation},
};

} // namespace

const TestSuite& GetImageTransformPixelsSuite() {
	static const TestSuite suite{"image_transform_pixels", kTests,
		sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
