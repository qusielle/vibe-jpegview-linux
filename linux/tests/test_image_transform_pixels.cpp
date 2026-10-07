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
using jpegview_linux::Image;
using jpegview_linux::ImageTransformGeometry;
using jpegview_linux::ImageTransformSampling;
using jpegview_linux::ResampleFreeRotation;

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
		false, false, geometry) && ResampleFreeRotation(source, geometry,
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
		false, false, geometry) && ResampleFreeRotation(source, geometry,
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
		ResampleFreeRotation(source, geometry,
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
		ResampleFreeRotation(source, geometry,
			ImageTransformSampling::FinalBicubic, output),
		"bicubic free-rotation resampling failed");
	const std::size_t sampleOffset = (static_cast<std::size_t>(4) * output.width + 5) * 4;
	Expect(output.bgra[sampleOffset] == 76 && output.bgra[sampleOffset + 1] == 38 &&
		output.bgra[sampleOffset + 2] == 33,
		"bicubic sampling did not preserve an interior linear color gradient");

	Image unchanged = MakeImage(1, 1, {9, 8, 7, 6});
	const std::vector<std::uint8_t> original = unchanged.bgra;
	int checks = 0;
	const bool completed = ResampleFreeRotation(source, geometry,
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
		!ResampleFreeRotation(source, geometry,
			ImageTransformSampling::FinalBicubic, output) &&
		output.width == 1 && output.height == 1 && output.bgra == original,
		"resampling accepted geometry for another source or modified output on failure");

	geometry.sourceWidth = source.width;
	geometry.sourceHeight = source.height;
	geometry.outputWidth = 0;
	Expect(!ResampleFreeRotation(source, geometry,
		ImageTransformSampling::PreviewBilinear, output) && output.bgra == original,
		"resampling accepted an invalid output extent");
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
};

} // namespace

const TestSuite& GetImageTransformPixelsSuite() {
	static const TestSuite suite{"image_transform_pixels", kTests,
		sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
