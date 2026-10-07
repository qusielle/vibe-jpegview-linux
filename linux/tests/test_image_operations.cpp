#include "test_harness.h"
#include "test_support.h"
#include "gps_map_action.h"
#include "image_metadata_reader.h"
#include "image_transform_geometry.h"
#include "image_transform_pixels.h"
#include "tiff_metadata_reader.h"

#include <cstring>
#include <limits>

namespace {

void TestImageStorageTransformsAndValidation() {
	jpegview_linux::Image empty;
	Expect(!empty.StoreBGRA(nullptr, 1, 1) && !empty.StoreBGRA(nullptr, 0, 0),
		"image storage accepted null or empty pixel input");
	const std::uint8_t onePixel[4] = {1, 2, 3, 4};
	Expect(!empty.StoreBGRA(onePixel, 65535, 65535),
		"image storage accepted dimensions above the pixel safety limit");
	jpegview_linux::Image transparent;
	Expect(transparent.StoreBGRA(onePixel, 1, 1, true) && transparent.hasTransparency,
		"image storage discarded its transparency metadata");
	jpegview_linux::Image transparentCrop;
	Expect(transparent.CopyCrop(0, 0, 1, 1, transparentCrop) && transparentCrop.hasTransparency,
		"cropping discarded its transparency metadata");

	const jpegview_linux::Image source = MakeIndexedImage(2, 3);
	jpegview_linux::Image transformed = source;
	Expect(transformed.Rotate(true) && transformed.width == 3 && transformed.height == 2 &&
		ImageBlueChannel(transformed) == std::vector<std::uint8_t>({5, 3, 1, 6, 4, 2}),
		"clockwise image rotation changed pixel orientation");
	Expect(transformed.originalWidth == 2 && transformed.originalHeight == 3,
		"image rotation changed source dimensions");

	transformed = source;
	Expect(transformed.Rotate(false) &&
		ImageBlueChannel(transformed) == std::vector<std::uint8_t>({2, 4, 6, 1, 3, 5}),
		"counter-clockwise image rotation changed pixel orientation");
	transformed = source;
	Expect(transformed.Mirror(true) &&
		ImageBlueChannel(transformed) == std::vector<std::uint8_t>({2, 1, 4, 3, 6, 5}),
		"horizontal image mirror changed pixel orientation");
	transformed = source;
	Expect(transformed.Mirror(false) &&
		ImageBlueChannel(transformed) == std::vector<std::uint8_t>({5, 6, 3, 4, 1, 2}),
		"vertical image mirror changed pixel orientation");
	std::vector<std::uint8_t> cancellationPixels(32u * 32u * 4u, 63);
	jpegview_linux::Image cancellableMirror;
	Expect(cancellableMirror.StoreBGRA(cancellationPixels.data(), 32, 32),
		"could not create cancellable mirror fixture");
	const std::vector<std::uint8_t> unchangedMirror = cancellableMirror.bgra;
	int mirrorChecks = 0;
	Expect(!cancellableMirror.Mirror(true, [&mirrorChecks] {
		return ++mirrorChecks < 3;
	}) && mirrorChecks == 3 && cancellableMirror.bgra == unchangedMirror,
		"canceled mirror changed source pixels or ignored its bounded row checks");

	jpegview_linux::Image malformed;
	malformed.width = 2;
	malformed.height = 2;
	malformed.bgra = {1, 2, 3, 4};
	Expect(!malformed.Rotate(true) && !malformed.Mirror(true) &&
		!malformed.Resize(1, 1) && !malformed.Crop(0, 0, 1, 1) && !malformed.AutoContrast(),
		"image operations accepted a truncated pixel buffer");
}

void TestImageCropCopiesHalfOpenRectangle() {
	jpegview_linux::Image image = MakeIndexedImage(3, 2);
	const std::vector<std::uint8_t> originalPixels = image.bgra;
	jpegview_linux::Image copied;
	Expect(image.CopyCrop(1, 0, 3, 2, copied) && copied.width == 2 && copied.height == 2 &&
		copied.originalWidth == 3 && copied.originalHeight == 2 &&
		ImageBlueChannel(copied) == std::vector<std::uint8_t>({2, 3, 5, 6}) &&
		image.bgra == originalPixels,
		"copy-crop did not extract the half-open rectangle while retaining its source");
	jpegview_linux::Image preserved = MakeIndexedImage(1, 1);
	const std::vector<std::uint8_t> preservedPixels = preserved.bgra;
	Expect(!image.CopyCrop(1, 0, 4, 2, preserved) && preserved.width == 1 &&
		preserved.height == 1 && preserved.bgra == preservedPixels,
		"invalid copy-crop changed its output image");
	std::vector<std::uint8_t> cancellablePixels(32u * 32u * 4u, 41);
	jpegview_linux::Image cancellableSource;
	Expect(cancellableSource.StoreBGRA(cancellablePixels.data(), 32, 32),
		"could not create cancellable copy-crop fixture");
	jpegview_linux::Image unchangedOutput = MakeIndexedImage(1, 1);
	const std::vector<std::uint8_t> unchangedOutputPixels = unchangedOutput.bgra;
	int cropChecks = 0;
	Expect(!cancellableSource.CopyCrop(0, 0, 24, 24, unchangedOutput,
		[&cropChecks] { return ++cropChecks < 3; }) && cropChecks == 3 &&
		unchangedOutput.width == 1 && unchangedOutput.height == 1 &&
		unchangedOutput.bgra == unchangedOutputPixels,
		"canceled copy-crop changed its output or ignored bounded row checks");
	Expect(image.CopyCrop(1, 0, 3, 2, image) && image.width == 2 && image.height == 2 &&
		image.originalWidth == 3 && image.originalHeight == 2 &&
		ImageBlueChannel(image) == std::vector<std::uint8_t>({2, 3, 5, 6}),
		"copy-crop could not safely replace its source image");
	image = MakeIndexedImage(3, 2);
	Expect(image.Crop(1, 0, 3, 2), "valid image crop was rejected");
	Expect(image.width == 2 && image.height == 2 &&
		ImageBlueChannel(image) == std::vector<std::uint8_t>({2, 3, 5, 6}),
		"crop did not copy the requested half-open pixel rectangle");
	Expect(image.originalWidth == 3 && image.originalHeight == 2,
		"crop unexpectedly replaced the dimensions retained from the source image");
	const std::vector<std::uint8_t> validPixels = image.bgra;
	for (const auto& bounds : std::vector<std::array<int, 4>>{
		{{-1, 0, 1, 1}}, {{0, -1, 1, 1}}, {{0, 0, 4, 1}},
		{{0, 0, 1, 3}}, {{2, 0, 1, 1}}, {{0, 1, 1, 1}}}) {
		Expect(!image.Crop(bounds[0], bounds[1], bounds[2], bounds[3]) && image.bgra == validPixels,
			"invalid image crop was accepted or partially changed pixels");
	}
	Expect(image.Crop(0, 0, 2, 2) && image.width == 2 && image.height == 2 &&
		ImageBlueChannel(image) == std::vector<std::uint8_t>({2, 3, 5, 6}),
		"crop of the full current image changed its pixel content");
}

void TestCropSelectionModelGeometryAndManipulation() {
	using jpegview_linux::CropSelectionHandle;
	using jpegview_linux::CropSelectionMode;
	using jpegview_linux::CropSelectionModel;
	using jpegview_linux::SelectionRect;
	Expect(!jpegview_linux::ShouldStartNewCropSelection(false, false, false) &&
		!jpegview_linux::ShouldStartNewCropSelection(false, false, true) &&
		jpegview_linux::ShouldStartNewCropSelection(true, false, false) &&
		!jpegview_linux::ShouldStartNewCropSelection(true, false, true) &&
		jpegview_linux::ShouldStartNewCropSelection(false, true, true),
		"crop-selection drag gating did not keep selection off by default or preserve modifier override");
	using ReleaseAction = jpegview_linux::NewCropSelectionReleaseAction;
	Expect(jpegview_linux::ResolveNewCropSelectionReleaseAction(false, false, false) ==
		ReleaseAction::Clear &&
		jpegview_linux::ResolveNewCropSelectionReleaseAction(false, true, true) ==
		ReleaseAction::Clear &&
		jpegview_linux::ResolveNewCropSelectionReleaseAction(true, true, true) ==
		ReleaseAction::ZoomToSelection &&
		jpegview_linux::ResolveNewCropSelectionReleaseAction(true, false, true) ==
		ReleaseAction::CopySelection &&
		jpegview_linux::ResolveNewCropSelectionReleaseAction(true, false, false) ==
		ReleaseAction::OpenContextMenu,
		"new selection release policy changed empty, Shift-zoom, automatic-copy, or menu behavior");
	CropSelectionModel selection;
	selection.SetImageSize(100, 80);
	Expect(selection.StartNew(2, 1), "selection could not start on a valid image");
	Expect(selection.Update(5, 3), "drag did not update its selection rectangle");
	selection.End();
	Expect(selection.HasSelection() && selection.Rect().left == 2 && selection.Rect().top == 1 &&
		selection.Rect().right == 6 && selection.Rect().bottom == 4,
		"forward selection drag did not produce half-open, inclusive-pixel bounds");

	selection.StartNew(5, 3);
	selection.Update(2, 1);
	selection.End();
	Expect(selection.Rect().left == 2 && selection.Rect().top == 1 &&
		selection.Rect().right == 6 && selection.Rect().bottom == 4,
		"reverse-direction selection drag changed normalized bounds");

	selection.StartNew(99, 79);
	selection.Update(1000, 1000);
	selection.End();
	Expect(selection.Rect().left == 99 && selection.Rect().top == 79 &&
		selection.Rect().right == 100 && selection.Rect().bottom == 80,
		"selection at the bottom-right image boundary exceeded the source dimensions");

	selection.SetImageSize(200, 120);
	selection.SetAspectRatio(16, 9);
	selection.StartNew(10, 10);
	selection.Update(90, 60);
	selection.End();
	const SelectionRect widescreen = selection.Rect();
	ExpectNear(static_cast<double>(widescreen.Width()) / widescreen.Height(), 16.0 / 9.0,
		0.03, "fixed-aspect creation did not preserve the requested ratio");
	Expect(widescreen.left >= 0 && widescreen.top >= 0 && widescreen.right <= 200 &&
		widescreen.bottom <= 120, "fixed-aspect selection escaped image bounds");

	selection.SetImageSize(400, 200);
	selection.SetMode(CropSelectionMode::ImageAspect);
	selection.StartNew(10, 10);
	selection.Update(70, 45);
	selection.End();
	ExpectNear(static_cast<double>(selection.Rect().Width()) / selection.Rect().Height(), 2.0,
		0.04, "same-as-image mode did not use the current image aspect ratio");

	selection.SetFixedSize(320, 200, true);
	selection.StartNew(4, 7);
	selection.Update(20, 20, 2.0);
	selection.End();
	Expect(selection.Rect().left == 20 && selection.Rect().top == 20 &&
		selection.Rect().Width() == 160 && selection.Rect().Height() == 100,
		"screen-pixel fixed size did not scale back to source-image pixels");
	selection.SetFixedSize(80, 45, false);
	selection.StartNew(4, 7);
	selection.Update(20, 20, 3.0);
	selection.End();
	Expect(selection.Rect().left == 20 && selection.Rect().top == 20 &&
		selection.Rect().Width() == 80 && selection.Rect().Height() == 45,
		"image-pixel fixed size incorrectly depended on viewport zoom");
	selection.SetFixedSize(60, 30, true);
	selection.StartNew(15, 15);
	selection.Update(25, 30, 2.0);
	selection.End();
	Expect(selection.Rect().left == 25 && selection.Rect().top == 30 &&
		selection.Rect().Width() == 30 && selection.Rect().Height() == 15,
		"fixed screen-pixel selection did not follow the pointer at the current zoom");

	selection.SetImageSize(50, 40);
	selection.SetMode(CropSelectionMode::Free);
	selection.StartNew(10, 10);
	selection.Update(29, 29);
	selection.End();
	Expect(selection.Rect().Width() == 20 && selection.Rect().Height() == 20,
		"selection setup for manipulation failed");
	Expect(selection.StartManipulation(15, 15, CropSelectionHandle::Move),
		"selection move could not start");
	selection.Update(20, 22);
	selection.End();
	Expect(selection.Rect().left == 15 && selection.Rect().top == 17 &&
		selection.Rect().right == 35 && selection.Rect().bottom == 37,
		"moving an existing selection did not preserve its size and pointer offset");
	selection.StartManipulation(20, 27, CropSelectionHandle::Move);
	selection.Update(1000, 1000);
	selection.End();
	Expect(selection.Rect().right == 50 && selection.Rect().bottom == 40,
		"moving a selection past the image edge did not clamp it inside the image");

	selection.SetImageSize(50, 50);
	selection.SetMode(CropSelectionMode::Free);
	selection.Clear();
	selection.StartNew(10, 10);
	selection.Update(29, 29);
	selection.End();
	selection.StartManipulation(10, 20, CropSelectionHandle::Left);
	selection.Update(5, 20);
	selection.End();
	Expect(selection.Rect().left == 5 && selection.Rect().right == 30,
		"left-edge resize did not preserve the opposite edge");
	selection.SetAspectRatio(1, 1);
	selection.StartManipulation(selection.Rect().right - 1, selection.Rect().bottom - 1,
		CropSelectionHandle::BottomRight);
	selection.Update(selection.Rect().right + 5, selection.Rect().bottom + 2);
	selection.End();
	ExpectNear(static_cast<double>(selection.Rect().Width()) / selection.Rect().Height(), 1.0,
		0.03, "fixed-aspect corner resize did not retain its ratio");
	selection.SetImageSize(200, 200);
	selection.SetMode(CropSelectionMode::Free);
	selection.StartNew(20, 30);
	selection.Update(49, 49);
	selection.End();
	const SelectionRect beforeAspectChange = selection.Rect();
	selection.SetAspectRatio(16, 9);
	Expect(selection.ReapplyMode() && selection.Rect().left == beforeAspectChange.left &&
		selection.Rect().top == beforeAspectChange.top,
		"changing aspect mode did not retain the selection's top-left anchor");
	ExpectNear(static_cast<double>(selection.Rect().Width()) / selection.Rect().Height(), 16.0 / 9.0,
		0.03, "changing aspect mode did not immediately resize the selection");
	selection.SetFixedSize(12, 8, false);
	Expect(selection.ReapplyMode() && selection.Rect().left == beforeAspectChange.left &&
		selection.Rect().top == beforeAspectChange.top && selection.Rect().Width() == 12 &&
		selection.Rect().Height() == 8,
		"applying fixed-size mode did not immediately resize the selection");
	selection.SetMode(CropSelectionMode::Free);
	const SelectionRect beforeFreeMode = selection.Rect();
	Expect(!selection.ReapplyMode() && selection.Rect().left == beforeFreeMode.left &&
		selection.Rect().top == beforeFreeMode.top && selection.Rect().right == beforeFreeMode.right &&
		selection.Rect().bottom == beforeFreeMode.bottom,
		"free mode unexpectedly changed the selection bounds");
	selection.SetImageSize(65535, 65535);
	selection.SetAspectRatio(65535, 1);
	selection.StartNew(0, 0);
	selection.Update(65534, 65534);
	selection.End();
	Expect(selection.Rect().Width() == 65535 && selection.Rect().Height() == 1 &&
		selection.Rect().right <= selection.ImageWidth() &&
		selection.Rect().bottom <= selection.ImageHeight(),
		"extreme aspect ratio overflowed or failed to fit within image boundaries");

	selection.SetImageSize(0, 0);
	Expect(!selection.StartNew(0, 0) && !selection.HasSelection(),
		"selection accepted an empty source image");
}

void TestLosslessJpegCropAvailabilityPolicy() {
	using jpegview_linux::CanOfferLosslessJpegCrop;
	Expect(CanOfferLosslessJpegCrop(true, false, false),
		"unmodified source-backed JPEG did not offer lossless crop");
	Expect(!CanOfferLosslessJpegCrop(true, true, false),
		"modified pixels offered lossless crop");
	Expect(!CanOfferLosslessJpegCrop(true, false, true),
		"detached source pixels offered lossless crop");
	Expect(!CanOfferLosslessJpegCrop(false, false, false),
		"unavailable JPEG source offered lossless crop");
}

void TestCropSelectionViewMappingAndHitTesting() {
	using jpegview_linux::CropSelectionHandle;
	using jpegview_linux::CropSelectionModel;
	using jpegview_linux::SelectionRect;
	using jpegview_linux::SelectionScreenRect;
	const SelectionScreenRect imageDestination{100, 50, 200, 100};
	const SelectionScreenRect mapped = CropSelectionModel::ToScreen(
		SelectionRect{20, 10, 100, 60}, imageDestination, 400, 200);
	Expect(mapped.x == 110 && mapped.y == 55 && mapped.width == 40 && mapped.height == 25,
		"selection screen rectangle did not follow the image scale and offset");
	const auto point = CropSelectionModel::ScreenToImage(150, 75, imageDestination, 400, 200);
	Expect(point.x == 100 && point.y == 50,
		"screen-to-image mapping did not invert the destination scale");
	const auto clipped = CropSelectionModel::ScreenToImage(-500, 900,
		imageDestination, 400, 200);
	Expect(clipped.x == 0 && clipped.y == 199,
		"screen-to-image mapping did not clamp points outside the rendered image");

	const SelectionScreenRect selected{10, 20, 100, 60};
	Expect(CropSelectionModel::HitTest(10, 20, selected, false) == CropSelectionHandle::TopLeft &&
		CropSelectionModel::HitTest(60, 20, selected, false) == CropSelectionHandle::Top &&
		CropSelectionModel::HitTest(50, 45, selected, false) == CropSelectionHandle::Move &&
		CropSelectionModel::HitTest(150, 100, selected, false) == CropSelectionHandle::None,
		"selection handle and interior hit-testing returned incorrect actions");
	Expect(CropSelectionModel::HitTest(10, 20, selected, true) == CropSelectionHandle::Move,
		"fixed-size mode exposed a resize handle instead of move-only behavior");
	const SelectionRect aligned = CropSelectionModel::AlignToMcu(
		SelectionRect{10, 10, 61, 51}, 100, 80, 16, 16);
	Expect(aligned.left == 0 && aligned.top == 0 && aligned.right == 64 && aligned.bottom == 64,
		"lossless crop did not expand bounds to MCU edges");
	const SelectionRect edgeAligned = CropSelectionModel::AlignToMcu(
		SelectionRect{80, 64, 100, 80}, 100, 80, 16, 16);
	Expect(edgeAligned.left == 80 && edgeAligned.top == 64 &&
		edgeAligned.right == 96 && edgeAligned.bottom == 80,
		"lossless crop did not trim a partial right-edge MCU like the Windows implementation");
	Expect(!CropSelectionModel::AlignToMcu(SelectionRect{96, 0, 100, 8},
		100, 80, 16, 8).Valid(), "lossless crop accepted a rectangle trimmed to zero width");
}

void TestImageResizeFiltersAndLimits() {
	jpegview_linux::Image source = MakeIndexedImage(4, 1);
	jpegview_linux::Image point = source;
	Expect(point.Resize(2, 1, 0) && ImageBlueChannel(point) == std::vector<std::uint8_t>({1, 3}),
		"point downsampling did not map destination pixels to expected source pixels");
	jpegview_linux::Image enlarged = MakeIndexedImage(2, 1);
	Expect(enlarged.Resize(3, 1, 0) &&
		ImageBlueChannel(enlarged) == std::vector<std::uint8_t>({1, 1, 2}),
		"point enlargement did not preserve source endpoints");
	jpegview_linux::Image clampedFilter = source;
	Expect(clampedFilter.Resize(2, 1, -50) && clampedFilter.bgra == point.bgra,
		"resize did not clamp a low filter index to point sampling");
	jpegview_linux::Image filteredEnlargement = MakeIndexedImage(2, 2);
	const jpegview_linux::Image enlargementSource = filteredEnlargement;
	Expect(filteredEnlargement.Resize(5, 4, 1) &&
		std::equal(filteredEnlargement.bgra.begin(), filteredEnlargement.bgra.begin() + 4,
			enlargementSource.bgra.begin()) &&
		std::equal(filteredEnlargement.bgra.end() - 4, filteredEnlargement.bgra.end(),
			enlargementSource.bgra.end() - 4),
		"bicubic enlargement did not preserve the first and last source pixels");
	jpegview_linux::Image singlePixel = MakeIndexedImage(1, 1);
	const std::vector<std::uint8_t> singleColor = singlePixel.bgra;
	Expect(singlePixel.Resize(4, 3, 3), "filtered enlargement rejected a one-pixel source");
	for (std::size_t offset = 0; offset < singlePixel.bgra.size(); offset += 4) {
		Expect(std::equal(singlePixel.bgra.begin() + static_cast<std::ptrdiff_t>(offset),
			singlePixel.bgra.begin() + static_cast<std::ptrdiff_t>(offset + 4), singleColor.begin()),
			"one-pixel enlargement did not retain its constant BGRA value");
	}
	jpegview_linux::Image unchanged = source;
	Expect(unchanged.Resize(4, 1, 3) && unchanged.bgra == source.bgra,
		"same-size resize modified source pixels");
	const jpegview_linux::Image sharedKernelSource = MakeIndexedImage(257, 193);
	jpegview_linux::Image concurrentResizeA = sharedKernelSource;
	jpegview_linux::Image concurrentResizeB = sharedKernelSource;
	bool concurrentResizeAOk = false;
	bool concurrentResizeBOk = false;
	std::thread concurrentResizeThreadA([&] {
		concurrentResizeAOk = concurrentResizeA.Resize(91, 71, 1);
	});
	std::thread concurrentResizeThreadB([&] {
		concurrentResizeBOk = concurrentResizeB.Resize(91, 71, 1);
	});
	concurrentResizeThreadA.join();
	concurrentResizeThreadB.join();
	Expect(concurrentResizeAOk && concurrentResizeBOk &&
		concurrentResizeA.bgra == concurrentResizeB.bgra,
		"concurrent resizes with a shared kernel key produced different pixels");
	jpegview_linux::Image highFilter = source;
	jpegview_linux::Image defaultFilter = source;
	Expect(highFilter.Resize(3, 1, 999) && defaultFilter.Resize(3, 1, 3) &&
		highFilter.bgra == defaultFilter.bgra,
		"resize did not clamp a high filter index to sharpen-medium");

	std::vector<std::uint8_t> constantPixels(8u * 8u * 4u);
	for (std::size_t offset = 0; offset < constantPixels.size(); offset += 4) {
		constantPixels[offset] = 25;
		constantPixels[offset + 1] = 75;
		constantPixels[offset + 2] = 125;
		constantPixels[offset + 3] = 175;
	}
	jpegview_linux::Image constant;
	Expect(constant.StoreBGRA(constantPixels.data(), 8, 8), "could not create constant resize fixture");
	for (int filter = 1; filter < 4; ++filter) {
		jpegview_linux::Image resized = constant;
		Expect(resized.Resize(3, 3, filter) && resized.width == 3 && resized.height == 3,
			"filtered resize rejected valid dimensions");
		for (std::size_t offset = 0; offset < resized.bgra.size(); offset += 4) {
			Expect(resized.bgra[offset] == 25 && resized.bgra[offset + 1] == 75 &&
				resized.bgra[offset + 2] == 125 && resized.bgra[offset + 3] == 175,
				"normalized resize kernel changed a constant color or alpha value");
		}
	}

	jpegview_linux::Image multipass = MakeIndexedImage(30, 2);
	Expect(multipass.Resize(3, 1, 3) && multipass.width == 3 && multipass.height == 1 &&
		multipass.originalWidth == 30 && multipass.originalHeight == 2,
		"large reduction did not complete through the multi-pass resize path");

	const auto resizeOracleInput = [](int width, int height) {
		std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
		for (int index = 0; index < width * height; ++index) {
			const std::size_t offset = static_cast<std::size_t>(index) * 4;
			pixels[offset] = static_cast<std::uint8_t>((index * 31 + 3) % 256);
			pixels[offset + 1] = static_cast<std::uint8_t>((index * 17 + 19) % 256);
			pixels[offset + 2] = static_cast<std::uint8_t>((index * 13 + 79) % 256);
			pixels[offset + 3] = static_cast<std::uint8_t>((index * 23 + 7) % 256);
		}
		jpegview_linux::Image image;
		if (!image.StoreBGRA(pixels.data(), width, height, true)) {
			throw TestFailure("could not create resize-output oracle fixture");
		}
		return image;
	};
	const auto pixelHash = [](const std::vector<std::uint8_t>& pixels) {
		std::uint64_t hash = 14695981039346656037ull;
		for (const std::uint8_t value : pixels) {
			hash ^= value;
			hash *= 1099511628211ull;
		}
		return hash;
	};
	struct ResizeOracleCase {
		int sourceWidth;
		int sourceHeight;
		int targetWidth;
		int targetHeight;
		int filter;
		std::uint64_t expectedHash;
	};
	const std::vector<ResizeOracleCase> resizeOracleCases = {
		{5, 4, 3, 2, 1, 0xf702f7abe1db8b38ull},
		{5, 4, 3, 2, 2, 0xc5249a1d78cc1cddull},
		{5, 4, 3, 2, 3, 0xf3f213dac576c858ull},
		{5, 4, 5, 2, 3, 0x4f38d395416a2441ull},
		{5, 4, 3, 4, 2, 0x66397571be4b076cull},
		{40, 13, 4, 2, 3, 0xf1bb4fbc2593c573ull},
	};
	for (const ResizeOracleCase& oracle : resizeOracleCases) {
		jpegview_linux::Image first = resizeOracleInput(oracle.sourceWidth,
			oracle.sourceHeight);
		jpegview_linux::Image repeated = first;
		Expect(first.Resize(oracle.targetWidth, oracle.targetHeight, oracle.filter) &&
			pixelHash(first.bgra) == oracle.expectedHash,
			"resize changed its characterized scalar output for a filter or pass geometry");
		Expect(repeated.Resize(oracle.targetWidth, oracle.targetHeight, oracle.filter) &&
			pixelHash(repeated.bgra) == oracle.expectedHash,
			"repeated resize changed its characterized scalar output");
	}

	const jpegview_linux::Image beforeFailure = multipass;
	Expect(!multipass.Resize(0, 1) && !multipass.Resize(65535, 65535) &&
		multipass.width == beforeFailure.width && multipass.height == beforeFailure.height &&
		multipass.bgra == beforeFailure.bgra,
		"invalid resize dimensions modified the image");

	std::vector<std::uint8_t> batchPixels(32u * 32u * 4u, 127);
	jpegview_linux::Image cancellableResize;
	Expect(cancellableResize.StoreBGRA(batchPixels.data(), 32, 32),
		"could not create cancellable resize fixture");
	int resizeChecks = 0;
	const std::vector<std::uint8_t> originalPixels = cancellableResize.bgra;
	Expect(!cancellableResize.Resize(24, 24, 0, [&resizeChecks] {
		return ++resizeChecks < 3;
	}) && resizeChecks == 3 && cancellableResize.width == 32 &&
		cancellableResize.height == 32 && cancellableResize.bgra == originalPixels,
		"canceled resize did not stop at a bounded row batch before publishing its output");

	jpegview_linux::Image cancellableProcessing;
	Expect(cancellableProcessing.StoreBGRA(batchPixels.data(), 32, 32),
		"could not create cancellable color-processing fixture");
	jpegview_linux::ImageProcessingParams processing;
	processing.contrast = 0.2;
	int processingChecks = 0;
	Expect(!cancellableProcessing.ApplyProcessing(processing, false, [&processingChecks] {
		return ++processingChecks < 4;
	}) && processingChecks == 4,
		"canceled color processing did not stop at a bounded row batch");
}

void TestImageAutoContrastInvariants() {
	const std::vector<std::uint8_t> pixels = {
		20, 40, 60, 17, 80, 100, 120, 18,
		140, 160, 180, 19, 200, 220, 240, 20,
	};
	jpegview_linux::Image image;
	Expect(image.StoreBGRA(pixels.data(), 2, 2), "could not create auto-contrast fixture");
	Expect(image.AutoContrast(), "auto contrast rejected a valid image");
	Expect(image.width == 2 && image.height == 2 && image.originalWidth == 2 && image.originalHeight == 2,
		"auto contrast changed image dimensions");
	bool colorChanged = false;
	for (std::size_t offset = 0; offset < image.bgra.size(); offset += 4) {
		colorChanged = colorChanged || !std::equal(image.bgra.begin() + static_cast<std::ptrdiff_t>(offset),
			image.bgra.begin() + static_cast<std::ptrdiff_t>(offset + 3),
			pixels.begin() + static_cast<std::ptrdiff_t>(offset));
		Expect(image.bgra[offset + 3] == pixels[offset + 3],
			"auto contrast modified straight-alpha values");
	}
	Expect(colorChanged, "auto contrast left a non-uniform low-range fixture unchanged");
}

void TestPictureLevelsModelAndProcessing() {
	using jpegview_linux::ImageProcessingParams;
	using jpegview_linux::LevelControl;
	ImageProcessingParams params;
	Expect(jpegview_linux::IsDefaultImageProcessing(params), "picture-level defaults are not identity values");
	Expect(static_cast<std::size_t>(LevelControl::Count) == 12,
		"not all Windows picture-level sliders are represented");
	Expect(jpegview_linux::GetLevelControlInfo(LevelControl::ColorCorrection).enabledByAutoContrast &&
		jpegview_linux::GetLevelControlInfo(LevelControl::ContrastCorrection).enabledByAutoContrast,
		"automatic-correction refinement sliders were not tied to auto-contrast state");
	for (std::size_t index = 0; index < static_cast<std::size_t>(LevelControl::Count); ++index) {
		const LevelControl control = static_cast<LevelControl>(index);
		const auto& info = jpegview_linux::GetLevelControlInfo(control);
		jpegview_linux::SetLevelControlValue(params, control, info.minimum - 10.0);
		ExpectNear(jpegview_linux::GetLevelControlValue(params, control), info.minimum, 1e-12,
			"picture-level minimum was not clamped");
		jpegview_linux::SetLevelControlValue(params, control, info.maximum + 10.0);
		ExpectNear(jpegview_linux::GetLevelControlValue(params, control), info.maximum, 1e-12,
			"picture-level maximum was not clamped");
		jpegview_linux::SetLevelControlValue(params, control, info.defaultValue);
	}
	Expect(jpegview_linux::IsDefaultImageProcessing(params),
		"range-boundary testing failed to restore slider identity values");
	ExpectNear(jpegview_linux::LevelControlValueAtPosition(LevelControl::Brightness, 0.0),
		2.0, 1e-12, "brightness slider left endpoint is not logarithmic maximum");
	ExpectNear(jpegview_linux::LevelControlValueAtPosition(LevelControl::Brightness, 1.0),
		0.5, 1e-12, "brightness slider right endpoint is not logarithmic minimum");
	jpegview_linux::SetLevelControlValue(params, LevelControl::Brightness, 1.0);
	ExpectNear(jpegview_linux::LevelControlPosition(params, LevelControl::Brightness),
		0.5, 1e-12, "brightness slider logarithmic position did not round-trip");
	jpegview_linux::SetLevelControlValue(params, LevelControl::Contrast, 1.0);
	ExpectNear(params.contrast, 0.5, 1e-12, "contrast slider was not clamped to its Windows range");
	jpegview_linux::SetLevelControlValue(params, LevelControl::Brightness,
		std::numeric_limits<double>::quiet_NaN());
	ExpectNear(params.gamma, 1.0, 1e-12, "malformed slider input did not restore its default");
	jpegview_linux::SetLevelControlValue(params, LevelControl::Contrast, 0.0);
	Expect(jpegview_linux::IsDefaultImageProcessing(params), "reset slider values did not restore defaults");
	jpegview_linux::ImageProcessingPreset current;
	jpegview_linux::ImageProcessingPreset saved;
	jpegview_linux::SetLevelControlValue(current.processing, LevelControl::Contrast, 0.25);
	current.autoContrast = true;
	jpegview_linux::SetLevelControlValue(saved.processing, LevelControl::Contrast, -0.25);
	ImageProcessingParams defaultProcessing;
	jpegview_linux::SetLevelControlValue(defaultProcessing, LevelControl::Saturation, 1.4);
	const auto kept = jpegview_linux::ResolveImageProcessingForFile(current, &saved, true, false,
		defaultProcessing);
	const auto restored = jpegview_linux::ResolveImageProcessingForFile(current, &saved, false, false,
		defaultProcessing);
	const auto defaults = jpegview_linux::ResolveImageProcessingForFile(current, nullptr, false, true,
		defaultProcessing);
	current.processing.localDensityEnabled = true;
	const auto pendingContinuation = jpegview_linux::ResolveImageProcessingForLoad(
		current, &saved, false, false, defaultProcessing, true);
	const auto freshLoad = jpegview_linux::ResolveImageProcessingForLoad(
		current, &saved, false, false, defaultProcessing, false);
	ExpectNear(kept.processing.contrast, 0.25, 1e-12,
		"keep-between-images did not override the saved per-file levels");
	Expect(kept.autoContrast, "keep-between-images did not preserve auto correction state");
	ExpectNear(restored.processing.contrast, -0.25, 1e-12,
		"per-file levels were not restored when keep was disabled");
	Expect(!restored.autoContrast, "per-file auto-correction state was not restored");
	ExpectNear(defaults.processing.saturation, 1.4, 1e-12,
		"image without saved levels did not receive the configured default preset");
	Expect(defaults.autoContrast, "image without saved levels did not receive default auto correction");
	Expect(pendingContinuation.autoContrast == current.autoContrast &&
		jpegview_linux::EqualImageProcessing(pendingContinuation.processing, current.processing),
		"cold-header continuation replaced effective picture-level edits with the saved preset");
	Expect(pendingContinuation.processing.localDensityEnabled &&
		!freshLoad.processing.localDensityEnabled &&
		std::abs(freshLoad.processing.contrast + 0.25) < 1e-12,
		"cold-header regression fixture did not distinguish continuation from a fresh load");

	const std::vector<std::uint8_t> pixels = {
		32, 64, 96, 17, 64, 96, 128, 18,
		96, 128, 160, 19, 128, 160, 192, 20,
	};
	jpegview_linux::Image original;
	Expect(original.StoreBGRA(pixels.data(), 2, 2), "could not create levels fixture");
	ImageProcessingParams color;
	jpegview_linux::SetLevelControlValue(color, LevelControl::Saturation, 0.0);
	jpegview_linux::Image grayscale = original;
	Expect(grayscale.ApplyProcessing(color, false), "saturation processing rejected a valid image");
	for (std::size_t offset = 0; offset < grayscale.bgra.size(); offset += 4) {
		Expect(std::abs(static_cast<int>(grayscale.bgra[offset]) - grayscale.bgra[offset + 1]) <= 1 &&
			std::abs(static_cast<int>(grayscale.bgra[offset + 1]) - grayscale.bgra[offset + 2]) <= 1,
			"zero saturation did not produce grayscale");
		Expect(grayscale.bgra[offset + 3] == pixels[offset + 3], "levels processing changed alpha");
	}
	color = {};
	jpegview_linux::SetLevelControlValue(color, LevelControl::CyanRed, 1.0);
	jpegview_linux::Image redTint = original;
	Expect(redTint.ApplyProcessing(color, false) && redTint.bgra[2] > original.bgra[2],
		"cyan-red level did not tint toward red");
	color = {};
	jpegview_linux::SetLevelControlValue(color, LevelControl::Brightness, 0.5);
	jpegview_linux::Image brighter = original;
	Expect(brighter.ApplyProcessing(color, false) && brighter.bgra[0] > original.bgra[0],
		"brightness/gamma level did not brighten the image");
	color = {};
	color.localDensityEnabled = true;
	color.lightenShadows = 1.0;
	color.deepShadows = 0.8;
	jpegview_linux::Image locallyCorrected = original;
	Expect(locallyCorrected.ApplyProcessing(color, false) &&
		locallyCorrected.bgra.size() == original.bgra.size(),
		"local density correction failed on a small image");
	color = {};
	color.unsharpRadius = 1.0;
	color.unsharpAmount = 2.0;
	color.unsharpThreshold = 0.0;
	jpegview_linux::Image unsharp = original;
	Expect(unsharp.ApplyProcessing(color, false) && unsharp.bgra != original.bgra,
		"unsharp mask did not alter detail when enabled");
	color = {};
	color.colorCorrection = 0.3;
	color.contrastCorrection = 0.6;
	color.deepShadows = 0.8;
	color.unsharpRadius = 4.0;
	color.unsharpThreshold = 10.0;
	jpegview_linux::Image inactive = original;
	Expect(inactive.ApplyProcessing(color, false) && inactive.bgra == original.bgra,
		"inactive correction controls needlessly changed image pixels");
}

void TestColorCastMatchesScalarPixelOracle() {
	const std::vector<std::array<double, 3>> controls = {
		{0.43, -0.28, 0.35}, {-0.8, 0.6, -0.4}, {1.0, 1.0, 1.0},
	};
	std::vector<std::uint8_t> source(256u * 4u);
	for (int value = 0; value < 256; ++value) {
		const std::size_t offset = static_cast<std::size_t>(value) * 4;
		source[offset] = static_cast<std::uint8_t>(value);
		source[offset + 1] = static_cast<std::uint8_t>(255 - value);
		source[offset + 2] = static_cast<std::uint8_t>((value * 73 + 29) % 256);
		source[offset + 3] = static_cast<std::uint8_t>((value * 11 + 3) % 256);
	}
	for (const auto& values : controls) {
		jpegview_linux::ImageProcessingParams params;
		params.cyanRed = values[0];
		params.magentaGreen = values[1];
		params.yellowBlue = values[2];
		jpegview_linux::Image image;
		Expect(image.StoreBGRA(source.data(), 256, 1, true),
			"could not create scalar color-cast oracle fixture");
		std::vector<std::uint8_t> expected = source;
		const double cyanRed = std::clamp(params.cyanRed, -1.0, 1.0);
		const double magentaGreen = std::clamp(params.magentaGreen, -1.0, 1.0);
		const double yellowBlue = std::clamp(params.yellowBlue, -1.0, 1.0);
		const auto scalarColorCast = [cyanRed, magentaGreen, yellowBlue](int channel,
			int value) {
			const double midtone = 1.0 - std::pow((value - 127.5) / 127.5, 2.0);
			double amount = 0.0;
			if (channel == 2) amount = 0.18 * cyanRed + 0.12 * yellowBlue -
				0.06 * magentaGreen;
			else if (channel == 1) amount = 0.18 * magentaGreen +
				0.12 * yellowBlue - 0.06 * cyanRed;
			else amount = -0.18 * yellowBlue - 0.06 * cyanRed - 0.06 * magentaGreen;
			return static_cast<int>(std::lround(amount * midtone * 255.0));
		};
		for (std::size_t offset = 0; offset < expected.size(); offset += 4) {
			const int blue = source[offset];
			const int green = source[offset + 1];
			const int red = source[offset + 2];
			expected[offset] = static_cast<std::uint8_t>(std::clamp(
				blue + scalarColorCast(0, blue), 0, 255));
			expected[offset + 1] = static_cast<std::uint8_t>(std::clamp(
				green + scalarColorCast(1, green), 0, 255));
			expected[offset + 2] = static_cast<std::uint8_t>(std::clamp(
				red + scalarColorCast(2, red), 0, 255));
		}
		Expect(image.ApplyProcessing(params, false) && image.bgra == expected,
			"color-cast table output differs from the previous scalar per-pixel calculation");
	}
}

void TestImageDocumentRejectsStaleOperationsAndOwnsCurrentPixels() {
	TemporaryDirectory temporary;
	const fs::path sourcePath = temporary.path() / "document.ppm";
	WriteTinyImage(sourcePath);
	const jpegview_linux::SourceKey source =
		jpegview_linux::DescribeImageSource(sourcePath).Key();
	jpegview_linux::ImageDocument document;
	jpegview_linux::RetiredImageBuffers retired = document.BeginSelection(source, 41, true);
	Expect(retired.sourcePixels == nullptr && document.OwnerGeneration() == 41 &&
		document.Source() == source,
		"new image document did not capture the selected source owner");
	document.UpdateProcessing({}, false);
	const jpegview_linux::ImageDocumentSnapshot materializeSnapshot = document.Snapshot();
	auto sourcePixels = std::make_shared<jpegview_linux::Image>(MakeIndexedImage(3, 2));
	auto presentationPixels = std::make_shared<jpegview_linux::Image>(*sourcePixels);
	jpegview_linux::ImageOperationResult materialized;
	materialized.expected = materializeSnapshot;
	materialized.kind = jpegview_linux::ImageOperationKind::Materialize;
	materialized.updatesDocument = true;
	materialized.success = true;
	materialized.sourcePixels = sourcePixels;
	materialized.presentationPixels = presentationPixels;
	Expect(document.Apply(materialized, retired) && document.HasMaterializedPixels() &&
		document.Presentation().width == 3 && document.Revision() > materializeSnapshot.revision,
		"current materialization did not atomically publish source and presentation pixels");

	const auto currentSource = document.SourcePixels();
	const auto currentPresentation = document.PresentationPixels();
	jpegview_linux::ImageOperationResult failed;
	failed.expected = document.Snapshot();
	failed.kind = jpegview_linux::ImageOperationKind::Crop;
	failed.updatesDocument = true;
	failed.failure = {jpegview_linux::WorkerFailureKind::ProcessingFailed,
		"synthetic crop failure"};
	Expect(!document.Apply(failed, retired) && document.SourcePixels() == currentSource &&
		document.PresentationPixels() == currentPresentation &&
		document.Presentation().width == 3,
		"failed pixel work replaced the last successful document or presentation");

	jpegview_linux::ImageOperationResult stale;
	stale.expected = document.Snapshot();
	stale.kind = jpegview_linux::ImageOperationKind::Resize;
	stale.updatesDocument = true;
	stale.success = true;
	stale.sourcePixels = currentSource;
	stale.presentationPixels = currentPresentation;
	jpegview_linux::ImageProcessingParams changed;
	changed.contrast = 0.2;
	document.UpdateProcessing(changed, false);
	Expect(!document.CanApply(stale) && document.SourcePixels() == currentSource &&
		document.PresentationPixels() == currentPresentation,
		"processing revision change did not reject a stale operation result");
	retired = document.ClearPixels();
	Expect(!document.HasMaterializedPixels() && document.Presentation().bgra.empty(),
		"clearing a source did not leave the document in lazy-pixel state");
}

void TestImageOperationWorkerRunsCropAndFlattensCapturedFrame() {
	TemporaryDirectory temporary;
	const fs::path sourcePath = temporary.path() / "animated-source.ppm";
	WriteTinyImage(sourcePath);
	const jpegview_linux::SourceKey source =
		jpegview_linux::DescribeImageSource(sourcePath).Key();
	const auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(1024);
	jpegview_linux::ImageDocument document;
	jpegview_linux::RetiredImageBuffers retired = document.BeginSelection(source, 57, true);
	retired = document.SetFrame(3, true, 3, 2, false);
	const jpegview_linux::ImageDocumentSnapshot initial = document.Snapshot();
	auto sourcePixels = std::make_shared<jpegview_linux::Image>(MakeIndexedImage(3, 2));
	auto presentationPixels = std::make_shared<jpegview_linux::Image>(*sourcePixels);
	jpegview_linux::ImageOperationResult materialized;
	materialized.expected = initial;
	materialized.kind = jpegview_linux::ImageOperationKind::Materialize;
	materialized.updatesDocument = true;
	materialized.success = true;
	materialized.sourcePixels = sourcePixels;
	materialized.presentationPixels = presentationPixels;
	Expect(document.Apply(materialized, retired),
		"animation-frame document could not install its immutable pixel fixture");
	const std::vector<std::uint8_t> originalPixels = sourcePixels->bgra;
	jpegview_linux::ImageDocumentSnapshot cropSnapshot = document.Snapshot();
	jpegview_linux::ImageOperationWorker worker(budget);
	jpegview_linux::ImageOperationRequest cropRequest;
	cropRequest.document = cropSnapshot;
	jpegview_linux::ImageOperationRequest rotationRequest;
	rotationRequest.document = cropSnapshot;
	rotationRequest.operation.kind = jpegview_linux::ImageOperationKind::FreeRotate;
	rotationRequest.operation.clockwiseDegrees = 27.0;
	const std::uint64_t rotationGeneration = worker.Request(std::move(rotationRequest));
	Expect(rotationGeneration != 0 && worker.WaitUntilIdle(std::chrono::seconds(3)),
		"asynchronous free rotation did not complete within its bounded deadline");
	auto rotationResult = worker.TakeReady();
	Expect(rotationResult.has_value() && rotationResult->success &&
		rotationResult->requestGeneration == rotationGeneration &&
		rotationResult->updatesDocument && rotationResult->flattenAnimation &&
		rotationResult->sourcePixels && rotationResult->presentationPixels &&
		rotationResult->sourcePixels->width != sourcePixels->width &&
		document.CanApply(*rotationResult) && document.Animated(),
		"worker rotation failed to produce a provisional animated-frame result");
	worker.Retire(std::move(*rotationResult));

	cropRequest.operation.kind = jpegview_linux::ImageOperationKind::Crop;
	cropRequest.operation.left = 1;
	cropRequest.operation.top = 0;
	cropRequest.operation.right = 3;
	cropRequest.operation.bottom = 2;
	const std::uint64_t cropGeneration = worker.Request(std::move(cropRequest));
	Expect(cropGeneration != 0 && worker.WaitUntilIdle(std::chrono::seconds(3)),
		"asynchronous crop did not complete within its bounded deadline");
	auto cropResult = worker.TakeReady();
	Expect(cropResult.has_value() && cropResult->success &&
		cropResult->requestGeneration == cropGeneration && cropResult->flattenAnimation &&
		cropResult->expected.frameIndex == 3 && cropResult->sourcePixels->width == 2 &&
		cropResult->sourcePixels->originalWidth == 2 &&
		ImageBlueChannel(*cropResult->presentationPixels) ==
			std::vector<std::uint8_t>({2, 3, 5, 6}) && sourcePixels->bgra == originalPixels,
		"worker crop changed half-open pixels, source ownership, or captured animation identity");
	Expect(document.CanApply(*cropResult),
		"matching animation-frame crop was rejected before document publication");
	Expect(document.Apply(*cropResult, retired) && !document.Animated() &&
		document.FrameIndex() == 3 && document.Modified() &&
		document.Presentation().width == 2 && document.Presentation().height == 2,
		"successful crop did not publish as a modified still from the captured animation frame");
	worker.Retire(std::move(retired));
	worker.Retire(std::move(*cropResult));

	jpegview_linux::ImageDocumentSnapshot resizeSnapshot = document.Snapshot();
	jpegview_linux::ImageOperationRequest resizeRequest;
	resizeRequest.document = resizeSnapshot;
	resizeRequest.operation.kind = jpegview_linux::ImageOperationKind::Resize;
	resizeRequest.operation.width = 1;
	resizeRequest.operation.height = 1;
	resizeRequest.operation.resizeFilter = 0;
	const std::uint64_t resizeGeneration = worker.Request(std::move(resizeRequest));
	document.UpdateProcessing(document.Processing(), true);
	Expect(worker.WaitUntilIdle(std::chrono::seconds(3)),
		"superseded resize did not reach a bounded completion point");
	auto resizeResult = worker.TakeReady();
	Expect(resizeResult.has_value() && resizeResult->success &&
		resizeResult->requestGeneration == resizeGeneration &&
		!document.CanApply(*resizeResult) && document.Presentation().width == 2,
		"worker result crossed a changed processing revision and replaced the visible document");
	worker.Retire(std::move(*resizeResult));
	retired = document.ClearPixels();
	worker.Retire(std::move(retired));
	worker.Stop();
	Expect(budget->Snapshot().activeWorkingBytes != 0,
		"retained operation snapshot did not keep its shared pixel charge alive");
	resizeSnapshot = {};
	cropSnapshot = {};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (budget->Snapshot().activeWorkingBytes != 0 &&
		std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	Expect(budget->Snapshot().activeWorkingBytes == 0,
		"retired operation buffers remained charged after their sole owners were released");
}

void TestImageOperationWorkerMatchesTransformsAndPixelPipelines() {
	TemporaryDirectory temporary;
	const fs::path sourcePath = temporary.path() / "operation-parity.ppm";
	WriteTinyImage(sourcePath);
	const jpegview_linux::SourceKey sourceKey =
		jpegview_linux::DescribeImageSource(sourcePath).Key();
	auto source = std::make_shared<jpegview_linux::Image>(MakeIndexedImage(4, 3));
	source->hasTransparency = true;
	jpegview_linux::ImageProcessingParams processing;
	processing.contrast = 0.12;
	processing.saturation = 0.7;
	processing.unsharpAmount = 0.0;
	jpegview_linux::ImageDocumentSnapshot document;
	document.source = sourceKey;
	document.ownerGeneration = 91;
	document.revision = 5;
	document.frameIndex = 2;
	document.animated = true;
	document.sourcePixels = source;
	document.processing = processing;
	const auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(0);
	const auto run = [&document, &budget](const jpegview_linux::ImageOperationSpec& operation) {
		jpegview_linux::ImageOperationRequest request;
		request.document = document;
		request.operation = operation;
		return jpegview_linux::ProcessImageOperation(request, [] { return true; }, *budget);
	};
	jpegview_linux::Image processedSource = *source;
	Expect(processedSource.ApplyProcessing(processing, false),
		"could not build the processing parity reference");
	for (const auto& transform : std::vector<std::pair<jpegview_linux::ImageTransformKind,
		int>>{
		{jpegview_linux::ImageTransformKind::RotateClockwise, 1},
		{jpegview_linux::ImageTransformKind::RotateCounterClockwise, 3},
		{jpegview_linux::ImageTransformKind::MirrorHorizontal, 0},
		{jpegview_linux::ImageTransformKind::MirrorVertical, 0}}) {
		jpegview_linux::ImageOperationSpec operation;
		operation.kind = jpegview_linux::ImageOperationKind::Transform;
		operation.transform = transform.first;
		auto result = run(operation);
		jpegview_linux::Image expectedSource = *source;
		jpegview_linux::Image expectedPresentation = processedSource;
		if (transform.first == jpegview_linux::ImageTransformKind::RotateClockwise) {
			Expect(expectedSource.Rotate(true) && expectedPresentation.Rotate(true),
				"could not build the clockwise transform reference");
		} else if (transform.first == jpegview_linux::ImageTransformKind::RotateCounterClockwise) {
			Expect(expectedSource.Rotate(false) && expectedPresentation.Rotate(false),
				"could not build the counter-clockwise transform reference");
		} else if (transform.first == jpegview_linux::ImageTransformKind::MirrorHorizontal) {
			Expect(expectedSource.Mirror(true) && expectedPresentation.Mirror(true),
				"could not build the horizontal mirror reference");
		} else {
			Expect(expectedSource.Mirror(false) && expectedPresentation.Mirror(false),
				"could not build the vertical mirror reference");
		}
		Expect(result.success && result.sourcePixels && result.presentationPixels &&
			result.sourcePixels->bgra == expectedSource.bgra &&
			result.presentationPixels->bgra == expectedPresentation.bgra &&
			result.sourcePixels->width == expectedSource.width &&
			result.sourcePixels->height == expectedSource.height &&
			result.sourcePixels->hasTransparency && result.flattenAnimation &&
			result.modified && result.rotationQuarterTurns == transform.second,
			"worker transform diverged from mutable image transforms or changed alpha, dimensions, or animation state");
	}

	document.rotationQuarterTurns = 2;
	for (const auto& cropPolicy : std::vector<std::pair<bool, bool>>{
		{false, false}, {true, false}, {true, true}}) {
		jpegview_linux::ImageOperationSpec operation;
		operation.kind = jpegview_linux::ImageOperationKind::FreeRotate;
		operation.clockwiseDegrees = 27.0;
		operation.autoCrop = cropPolicy.first;
		operation.preserveAspectRatio = cropPolicy.second;
		const jpegview_linux::ImageOperationResult result = run(operation);
		jpegview_linux::ImageTransformGeometry geometry;
		Expect(jpegview_linux::BuildFreeRotationGeometry(source->width, source->height,
			operation.clockwiseDegrees, operation.autoCrop, operation.preserveAspectRatio,
			geometry), "could not build free-rotation worker reference geometry");
		jpegview_linux::Image expectedSource;
		jpegview_linux::Image expectedPresentation;
		Expect(jpegview_linux::ResampleFreeRotation(*source, geometry,
			jpegview_linux::ImageTransformSampling::FinalBicubic, expectedSource) &&
			jpegview_linux::ResampleFreeRotation(processedSource, geometry,
				jpegview_linux::ImageTransformSampling::FinalBicubic, expectedPresentation),
			"could not build free-rotation worker parity references");
		Expect(result.success && result.updatesDocument && result.sourcePixels &&
			result.presentationPixels && result.flattenAnimation && result.modified &&
			result.rotationQuarterTurns == 0 &&
			result.sourcePixels->bgra == expectedSource.bgra &&
			result.presentationPixels->bgra == expectedPresentation.bgra &&
			result.presentationPixels->width == geometry.outputWidth &&
			result.presentationPixels->height == geometry.outputHeight,
			"free-rotation worker changed processing order, crop policy, or animation metadata");
	}
	jpegview_linux::ImageOperationSpec invalidRotation;
	invalidRotation.kind = jpegview_linux::ImageOperationKind::FreeRotate;
	invalidRotation.clockwiseDegrees = std::numeric_limits<double>::infinity();
	const auto invalidRotationResult = run(invalidRotation);
	Expect(!invalidRotationResult.success && invalidRotationResult.failure.Failed() &&
		!invalidRotationResult.sourcePixels && !invalidRotationResult.presentationPixels,
		"free-rotation worker accepted invalid geometry or published partial pixels");
	jpegview_linux::ImageOperationSpec previewRotation;
	previewRotation.kind = jpegview_linux::ImageOperationKind::FreeRotatePreview;
	previewRotation.clockwiseDegrees = 27.0;
	const jpegview_linux::ImageOperationResult preview = run(previewRotation);
	jpegview_linux::ImageTransformGeometry previewGeometry;
	Expect(jpegview_linux::BuildFreeRotationGeometry(source->width, source->height,
		previewRotation.clockwiseDegrees, false, false, previewGeometry),
		"could not build free-rotation preview reference geometry");
	jpegview_linux::Image expectedPreview;
	Expect(jpegview_linux::ResampleFreeRotation(processedSource, previewGeometry,
		jpegview_linux::ImageTransformSampling::PreviewBilinear, expectedPreview) &&
		preview.success && !preview.updatesDocument && !preview.sourcePixels &&
		!preview.presentationPixels && preview.outputPixels &&
		preview.outputPixels->bgra == expectedPreview.bgra,
		"free-rotation preview did not produce an isolated provisional result");

	jpegview_linux::ImageOperationSpec crop;
	crop.kind = jpegview_linux::ImageOperationKind::Crop;
	crop.left = 1;
	crop.top = 1;
	crop.right = 4;
	crop.bottom = 3;
	auto cropped = run(crop);
	jpegview_linux::Image expectedCropSource;
	Expect(source->CopyCrop(1, 1, 4, 3, expectedCropSource),
		"could not build crop parity reference");
	expectedCropSource.originalWidth = expectedCropSource.width;
	expectedCropSource.originalHeight = expectedCropSource.height;
	jpegview_linux::Image expectedCropPresentation = expectedCropSource;
	Expect(expectedCropPresentation.ApplyProcessing(processing, false) && cropped.success &&
		cropped.sourcePixels->bgra == expectedCropSource.bgra &&
		cropped.presentationPixels->bgra == expectedCropPresentation.bgra &&
		cropped.sourcePixels->originalWidth == 3 && cropped.sourcePixels->originalHeight == 2 &&
		cropped.presentationPixels->hasTransparency && cropped.flattenAnimation,
		"worker crop changed half-open geometry, processing order, alpha, or animation flattening");

	jpegview_linux::ImageOperationSpec resize;
	resize.kind = jpegview_linux::ImageOperationKind::Resize;
	resize.width = 2;
	resize.height = 2;
	resize.resizeFilter = 0;
	auto resized = run(resize);
	jpegview_linux::Image expectedResizeSource = *source;
	Expect(expectedResizeSource.Resize(2, 2, 0), "could not build resize parity reference");
	expectedResizeSource.originalWidth = 2;
	expectedResizeSource.originalHeight = 2;
	jpegview_linux::Image expectedResizePresentation = expectedResizeSource;
	Expect(expectedResizePresentation.ApplyProcessing(processing, false) && resized.success &&
		resized.sourcePixels->bgra == expectedResizeSource.bgra &&
		resized.presentationPixels->bgra == expectedResizePresentation.bgra &&
		resized.presentationPixels->width == 2 && resized.presentationPixels->height == 2 &&
		resized.flattenAnimation,
		"worker resize changed filter output, processing order, or animation flattening");

	jpegview_linux::ImageDocumentSnapshot materialized = document;
	materialized.animated = false;
	materialized.presentationPixels = std::make_shared<jpegview_linux::Image>(processedSource);
	materialized.materializedProcessing = processing;
	jpegview_linux::ImageOperationRequest outputRequest;
	outputRequest.document = materialized;
	outputRequest.operation.kind = jpegview_linux::ImageOperationKind::PrepareOutput;
	outputRequest.operation.width = 2;
	outputRequest.operation.height = 2;
	outputRequest.operation.resizeFilter = 0;
	auto output = jpegview_linux::ProcessImageOperation(outputRequest,
		[] { return true; }, *budget);
	jpegview_linux::Image expectedOutput = processedSource;
	Expect(expectedOutput.Resize(2, 2, 0) && output.success && output.outputPixels &&
		output.outputPixels->bgra == expectedOutput.bgra &&
		output.outputPixels->width == 2 && output.outputPixels->height == 2,
		"worker output preparation changed processing-before-resize pixels or dimensions");

	outputRequest.operation.kind = jpegview_linux::ImageOperationKind::CopySelection;
	outputRequest.operation.left = 1;
	outputRequest.operation.top = 0;
	outputRequest.operation.right = 4;
	outputRequest.operation.bottom = 2;
	auto selection = jpegview_linux::ProcessImageOperation(outputRequest,
		[] { return true; }, *budget);
	jpegview_linux::Image expectedSelection;
	Expect(processedSource.CopyCrop(1, 0, 4, 2, expectedSelection) && selection.success &&
		selection.outputPixels && selection.outputPixels->bgra == expectedSelection.bgra &&
		selection.outputPixels->width == 3 && selection.outputPixels->height == 2 &&
		selection.outputPixels->hasTransparency,
		"worker selection copy changed processed pixels, half-open bounds, or alpha metadata");

	jpegview_linux::Image identitySource = MakeIndexedImage(4, 3);
	auto identityPixels = std::make_shared<const jpegview_linux::Image>(identitySource);
	const auto identityBudget = std::make_shared<jpegview_linux::SharedCacheBudget>(0);
	jpegview_linux::ImageDocumentSnapshot identityDocument = document;
	identityDocument.animated = false;
	identityDocument.processing = {};
	identityDocument.sourcePixels = identityPixels;
	identityDocument.presentationPixels.reset();
	identityDocument.materializedProcessing = {};
	identityDocument.autoContrast = false;
	identityDocument.materializedAutoContrast = false;
	identityDocument.modified = false;
	jpegview_linux::ImageOperationRequest materializeRequest;
	materializeRequest.document = identityDocument;
	materializeRequest.operation.kind = jpegview_linux::ImageOperationKind::Materialize;
	auto noOpMaterialization = jpegview_linux::ProcessImageOperation(materializeRequest,
		[] { return true; }, *identityBudget);
	Expect(noOpMaterialization.success &&
		noOpMaterialization.sourcePixels == noOpMaterialization.presentationPixels &&
		identityBudget->Snapshot().activeWorkingBytes == identityPixels->bgra.size(),
		"identity materialization copied or double-charged the immutable source pixels");

	identityDocument.revision++;
	materializeRequest.document = identityDocument;
	materializeRequest.operation.kind = jpegview_linux::ImageOperationKind::Transform;
	materializeRequest.operation.transform = jpegview_linux::ImageTransformKind::RotateClockwise;
	auto identityTransform = jpegview_linux::ProcessImageOperation(materializeRequest,
		[] { return true; }, *identityBudget);
	Expect(identityTransform.success &&
		identityTransform.sourcePixels == identityTransform.presentationPixels &&
		identityTransform.sourcePixels->width == 3 && identityTransform.sourcePixels->height == 4 &&
		identityBudget->Snapshot().activeWorkingBytes == identityPixels->bgra.size() * 2,
		"identity transform built or charged two full-size outputs instead of sharing one");
}

void TestFreeRotationPreviewBoundsLargeSourcePixels() {
	constexpr int width = 3000;
	constexpr int height = 2000;
	std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 91);
	for (std::size_t offset = 3; offset < pixels.size(); offset += 4) pixels[offset] = 255;
	auto source = std::make_shared<jpegview_linux::Image>();
	Expect(source->StoreBGRA(pixels.data(), width, height),
		"could not create a large free-rotation preview source");
	pixels.clear();
	jpegview_linux::ImageDocumentSnapshot document;
	document.ownerGeneration = 3;
	document.revision = 8;
	document.sourcePixels = source;
	jpegview_linux::ImageOperationRequest request;
	request.document = document;
	request.operation.kind = jpegview_linux::ImageOperationKind::FreeRotatePreview;
	request.operation.clockwiseDegrees = 45.0;
	const auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(0);
	const jpegview_linux::ImageOperationResult result =
		jpegview_linux::ProcessImageOperation(request, [] { return true; }, *budget);
	Expect(result.success && result.outputPixels &&
		static_cast<std::uint64_t>(result.outputPixels->width) *
			result.outputPixels->height <= 4ull * 1024ull * 1024ull &&
		std::max(result.outputPixels->width, result.outputPixels->height) <= 2048,
		"large free-rotation preview exceeded its output-pixel or dimension bound");
}

void TestInPlaceSaveMaterializesLazyDocumentAndRebindsSource() {
	TemporaryDirectory temporary;
	const fs::path sourcePath = temporary.path() / "in-place-save.png";
	WriteBytes(sourcePath, {1});
	const jpegview_linux::SourceDescriptor originalSource =
		jpegview_linux::DescribeImageSource(sourcePath);
	Expect(originalSource.Valid(), "in-place save fixture did not capture its original source");

	const jpegview_linux::Image sourceImage = MakeIndexedImage(4, 3);
	auto decoded = std::make_shared<jpegview_linux::DecodedImage>();
	jpegview_linux::DecodedFrame decodedFrame;
	decodedFrame.width = sourceImage.width;
	decodedFrame.height = sourceImage.height;
	decodedFrame.bgra = sourceImage.bgra;
	decodedFrame.hasTransparency = sourceImage.hasTransparency;
	decoded->frames.push_back(std::move(decodedFrame));

	jpegview_linux::ImageProcessingParams processing;
	processing.contrast = 0.12;
	const auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(0);
	jpegview_linux::ImageDocument document;
	(void)document.BeginSelection(originalSource.Key(), 17, true);
	document.SetDimensions(sourceImage.width, sourceImage.height,
		sourceImage.hasTransparency);
	document.UpdateProcessing(processing, false);
	Expect(!document.HasMaterializedPixels(),
		"in-place save fixture unexpectedly began with materialized document pixels");

	jpegview_linux::ImageOperationRequest request;
	request.document = document.Snapshot();
	request.decoded = decoded;
	request.operation.kind = jpegview_linux::ImageOperationKind::PrepareOutput;
	request.operation.preserveDocumentPixels = true;
	const jpegview_linux::Image expectedPresentation = [&sourceImage, &processing] {
		jpegview_linux::Image expected = sourceImage;
		if (!expected.ApplyProcessing(processing, false)) {
			throw TestFailure("could not prepare in-place save processing reference");
		}
		return expected;
	}();
	jpegview_linux::ImageOperationResult prepared =
		jpegview_linux::ProcessImageOperation(request, [] { return true; }, *budget);
	Expect(prepared.success && prepared.updatesDocument && prepared.sourcePixels &&
		prepared.presentationPixels && prepared.outputPixels &&
		prepared.presentationPixels->bgra == expectedPresentation.bgra &&
		prepared.outputPixels->bgra == expectedPresentation.bgra,
		"in-place save preparation did not retain its processed lazy document pixels");
	jpegview_linux::RetiredImageBuffers retired;
	Expect(document.Apply(prepared, retired) && document.HasMaterializedPixels(),
		"in-place save preparation could not commit document pixels");

	WriteBytes(sourcePath, {1, 2});
	const jpegview_linux::SourceDescriptor savedSource =
		jpegview_linux::DescribeImageSource(sourcePath);
	Expect(savedSource.Valid() && savedSource.Key() != originalSource.Key(),
		"in-place save fixture did not produce a new source identity");
	document.MarkDetached(savedSource.Key());
	Expect(document.Source() == savedSource.Key() && document.Detached(),
		"in-place save did not rebind the materialized document to the saved source identity");

	request = {};
	request.document = document.Snapshot();
	request.operation.kind = jpegview_linux::ImageOperationKind::Transform;
	request.operation.transform = jpegview_linux::ImageTransformKind::RotateClockwise;
	const jpegview_linux::ImageOperationResult nextOperation =
		jpegview_linux::ProcessImageOperation(request, [] { return true; }, *budget);
	Expect(nextOperation.success && nextOperation.sourcePixels &&
		nextOperation.sourcePixels->width == sourceImage.height &&
		nextOperation.sourcePixels->height == sourceImage.width,
		"a transform after in-place save could not use the rebound source identity and retained pixels");
}

void TestImageOperationWorkerSupersedesAndSurvivesFailures() {
	std::mutex mutex;
	std::condition_variable condition;
	bool firstEntered = false;
	bool releaseFirst = false;
	int calls = 0;
	const auto processor = [&mutex, &condition, &firstEntered, &releaseFirst, &calls](
		const jpegview_linux::ImageOperationRequest& request,
		const std::function<bool()>& shouldContinue,
		jpegview_linux::SharedCacheBudget&) {
		jpegview_linux::ImageOperationResult result;
		result.expected = request.document;
		result.kind = request.operation.kind;
		int call = 0;
		{
			std::unique_lock<std::mutex> lock(mutex);
			call = ++calls;
			if (call == 1) {
				firstEntered = true;
				condition.notify_all();
				condition.wait(lock, [&releaseFirst] { return releaseFirst; });
			}
		}
		if (call == 1 && !shouldContinue()) {
			result.failure = {jpegview_linux::WorkerFailureKind::Cancelled,
				"superseded test operation"};
			return result;
		}
		if (request.operation.kind == jpegview_linux::ImageOperationKind::Transform) {
			throw std::runtime_error("synthetic operation exception");
		}
		result.success = true;
		return result;
	};
	const auto budget = std::make_shared<jpegview_linux::SharedCacheBudget>(0);
	jpegview_linux::ImageOperationWorker worker(budget, processor);
	jpegview_linux::ImageOperationRequest first;
	first.operation.kind = jpegview_linux::ImageOperationKind::Crop;
	const std::uint64_t firstGeneration = worker.Request(std::move(first));
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(condition.wait_for(lock, std::chrono::seconds(2),
			[&firstEntered] { return firstEntered; }),
			"test processor did not reach its deterministic cancellation barrier");
	}
	jpegview_linux::ImageOperationRequest second;
	second.operation.kind = jpegview_linux::ImageOperationKind::Resize;
	const std::uint64_t secondGeneration = worker.Request(std::move(second));
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseFirst = true;
	}
	condition.notify_all();
	Expect(secondGeneration > firstGeneration && worker.WaitUntilIdle(std::chrono::seconds(3)),
		"latest operation did not replace and complete after canceling its predecessor");
	auto latest = worker.TakeReady();
	Expect(latest.has_value() && latest->success &&
		latest->requestGeneration == secondGeneration &&
		latest->kind == jpegview_linux::ImageOperationKind::Resize,
		"superseded operation published after its replacement");
	worker.Retire(std::move(*latest));

	jpegview_linux::ImageOperationRequest throwing;
	throwing.operation.kind = jpegview_linux::ImageOperationKind::Transform;
	worker.Request(std::move(throwing));
	Expect(worker.WaitUntilIdle(std::chrono::seconds(3)),
		"throwing operation escaped its bounded worker deadline");
	auto failure = worker.TakeReady();
	Expect(failure.has_value() && !failure->success && failure->failure.kind ==
		jpegview_linux::WorkerFailureKind::Exception,
		"worker exception did not publish a structured failure");
	worker.Retire(std::move(*failure));

	jpegview_linux::ImageOperationRequest validAfterFailure;
	validAfterFailure.operation.kind = jpegview_linux::ImageOperationKind::Reprocess;
	const std::uint64_t recoveredGeneration = worker.Request(std::move(validAfterFailure));
	Expect(worker.WaitUntilIdle(std::chrono::seconds(3)),
		"worker did not continue after a structured failure");
	auto recovered = worker.TakeReady();
	Expect(recovered.has_value() && recovered->success &&
		recovered->requestGeneration == recoveredGeneration,
		"valid later image operation was not serviced after a worker exception");
	worker.Retire(std::move(*recovered));
	worker.Stop();
}

void TestPictureLevelsStoreRoundTrip() {
	TemporaryDirectory temporary;
	const fs::path database = temporary.path() / "picture-levels.db";
	jpegview_linux::ImageProcessingStore expected;
	jpegview_linux::ImageProcessingPreset preset;
	preset.processing.contrast = 0.23;
	preset.processing.gamma = 1.2;
	preset.processing.cyanRed = -0.4;
	preset.processing.localDensityEnabled = true;
	preset.autoContrast = true;
	expected["/images/a \"quoted\" photo.jpg"] = preset;
	Expect(jpegview_linux::SaveImageProcessingStore(database, expected),
		"picture-level store could not be written");
	jpegview_linux::ImageProcessingStore loaded;
	Expect(jpegview_linux::LoadImageProcessingStore(database, loaded),
		"picture-level store could not be read");
	Expect(loaded.size() == 1 && jpegview_linux::EqualImageProcessing(
		loaded.begin()->second.processing, preset.processing) && loaded.begin()->second.autoContrast,
		"picture-level store did not round-trip parameters and correction state");
	Expect(loaded.begin()->first == expected.begin()->first,
		"picture-level store did not preserve quoted path characters");
	preset.processing.contrast = 0.36;
	preset.autoContrast = false;
	expected.begin()->second = preset;
	Expect(jpegview_linux::SaveImageProcessingStore(database, expected),
		"picture-level store could not replace an existing database atomically");
	loaded.clear();
	Expect(jpegview_linux::LoadImageProcessingStore(database, loaded) && loaded.size() == 1 &&
		std::abs(loaded.begin()->second.processing.contrast - 0.36) < 1e-12 &&
		!loaded.begin()->second.autoContrast,
		"replaced picture-level store did not contain the new backup contents");
	std::ofstream malformed(database, std::ios::trunc);
	malformed << "\"/broken\" 1 no-number\n";
	malformed.close();
	Expect(!jpegview_linux::LoadImageProcessingStore(database, loaded) && loaded.size() == 1 &&
		std::abs(loaded.begin()->second.processing.contrast - 0.36) < 1e-12,
		"malformed picture-level database was accepted or replaced the current store");
	std::ofstream outOfRange(database, std::ios::trunc);
	outOfRange << "\"/invalid\" 0 0 0 -1 0 0 0 0 0 0 0 0 0 0 0\n";
	outOfRange.close();
	Expect(!jpegview_linux::LoadImageProcessingStore(database, loaded) && loaded.size() == 1 &&
		std::abs(loaded.begin()->second.processing.contrast - 0.36) < 1e-12,
		"out-of-range picture-level database was accepted or replaced the current store");
}

void TestSettingsRoundTripAndMalformedValues() {
	TemporaryDirectory temporary;
	const fs::path settingsPath = temporary.path() / "config" / "settings.conf";
	Expect(!jpegview_linux::ViewerSettings{}.copySelectionOnRelease,
		"copy-selection-on-release was enabled by default");
	Expect(!jpegview_linux::ViewerSettings{}.fitRelativeZoomMode,
		"fit-relative zoom was enabled in the built-in settings defaults");
	Expect(!jpegview_linux::ViewerSettings{}.pixelColorSamplerEnabled,
		"pixel color sampler was enabled in the built-in settings defaults");
	Expect(jpegview_linux::ViewerSettings{}.gpsMapProviderUrl ==
		jpegview_linux::kDefaultGpsMapProviderUrl,
		"GPS map provider did not retain the built-in URL template");
	jpegview_linux::ViewerSettings expected;
	expected.scaleMode = "manual";
	expected.sortMode = "file_name";
	expected.sortAscending = false;
	expected.manualZoom = 2.375;
	expected.maximized = true;
	expected.navigationPanelEnabled = false;
	expected.navigationPanelAutoReveal = false;
	expected.thumbnailPanelVisible = true;
	expected.showZoomNavigator = false;
	expected.pixelColorSamplerEnabled = true;
	expected.doublePageModeEnabled = true;
	expected.mangaReadingOrderEnabled = true;
	expected.mangaModeInvertsLeftRight = false;
	expected.spacebarNavigatesImages = true;
	expected.folderWrapAround = false;
	expected.fitRelativeZoomMode = true;
	expected.transparencyPattern = jpegview_linux::TransparencyPattern::Checkerboard;
	expected.thumbnailPanelWidth = 287;
	expected.fileDialogWidth = 1040;
	expected.fileDialogHeight = 735;
	expected.fileDialogPreviewRatio = 0.375;
	expected.magnifyingGlassWidth = 425;
	expected.magnifyingGlassHeight = 215;
	expected.magnifyingGlassZoomLevel = 0.725;
	expected.fixedCropWidth = 512;
	expected.fixedCropHeight = 288;
	expected.fixedCropScreenPixels = false;
	expected.userCropAspectWidth = 13;
	expected.userCropAspectHeight = 7;
	expected.selectionModeEnabled = true;
	expected.copySelectionOnRelease = true;
	expected.infoVisible = true;
	expected.showHistogram = true;
	expected.showFilename = true;
	expected.autoContrast = true;
	expected.keepPictureLevels = true;
	expected.defaultImageProcessing.contrast = 0.18;
	expected.defaultImageProcessing.gamma = 1.15;
	expected.defaultImageProcessing.saturation = 1.25;
	expected.defaultImageProcessing.cyanRed = 0.2;
	expected.defaultImageProcessing.magentaGreen = -0.2;
	expected.defaultImageProcessing.yellowBlue = 0.3;
	expected.defaultImageProcessing.lightenShadows = 0.4;
	expected.defaultImageProcessing.darkenHighlights = 0.2;
	expected.defaultImageProcessing.deepShadows = 0.6;
	expected.defaultImageProcessing.colorCorrection = 0.1;
	expected.defaultImageProcessing.contrastCorrection = 0.55;
	expected.defaultImageProcessing.sharpen = 0.15;
	expected.defaultImageProcessing.localDensityEnabled = true;
	expected.unsharpMaskRadius = 2.25;
	expected.unsharpMaskAmount = 3.5;
	expected.unsharpMaskThreshold = 7.0;
	expected.cacheSizeMiB = 1536;
	expected.copyRenamePattern = "%F=%n";
	expected.gpsMapProviderUrl = "https://maps.example.test/?lat={lat}&lon={lng}";
	expected.windowTitlePattern = "%f | [%p] | %m | %%";
	Expect(jpegview_linux::SaveViewerSettings(settingsPath, expected), "settings could not be saved");
	Expect(fs::exists(settingsPath), "settings file was not created");
	Expect(!fs::exists(settingsPath.string() + ".tmp"), "temporary settings file was left behind");
	std::ifstream settingsInput(settingsPath);
	std::string settingsContents;
	std::string settingsLine;
	while (std::getline(settingsInput, settingsLine)) settingsContents += settingsLine + '\n';
	Expect(settingsContents.find("# Window title codes:") != std::string::npos &&
		settingsContents.find("# %a application name, %v build version, %% literal percent.") !=
			std::string::npos &&
		settingsContents.find("%i one-based image index") != std::string::npos &&
		settingsContents.find("%F filename without extension (stem)") != std::string::npos &&
		settingsContents.find("%w/%h original width/height in pixels") != std::string::npos &&
		settingsContents.find("window_title_pattern=%f | [%p] | %m | %%\n") !=
			std::string::npos,
		"saved config did not explain title codes beside the custom pattern");
	Expect(settingsContents.find("copy_selection_on_release=1\n") != std::string::npos,
		"saved config omitted the enabled copy-selection-on-release setting");
	Expect(settingsContents.find("pixel_color_sampler_enabled=1\n") != std::string::npos,
		"saved config omitted the enabled pixel color sampler preference");
	const std::size_t titleSettingPosition = settingsContents.find("window_title_pattern=");
	const std::size_t titleCommentPosition = settingsContents.rfind("# Window title codes:", titleSettingPosition);
	const std::string titleCommentText = titleSettingPosition == std::string::npos ||
		titleCommentPosition == std::string::npos ? std::string() :
		settingsContents.substr(titleCommentPosition, titleSettingPosition - titleCommentPosition);
	for (const char* token : {"%p", "%i", "%n", "%f", "%F", "%e", "%P", "%D",
		"%w", "%h", "%s", "%b", "%m", "%a", "%v", "%%"}) {
		Expect(titleCommentText.find(token) != std::string::npos,
			std::string("settings.conf title comment omits ") + token);
	}

	jpegview_linux::ViewerSettings loaded;
	Expect(jpegview_linux::LoadViewerSettings(settingsPath, loaded), "settings could not be loaded");
	Expect(loaded.scaleMode == expected.scaleMode && loaded.sortMode == expected.sortMode,
		"settings string values did not round-trip");
	Expect(loaded.sortAscending == expected.sortAscending && loaded.maximized == expected.maximized,
		"settings boolean values did not round-trip");
	Expect(loaded.navigationPanelEnabled == expected.navigationPanelEnabled &&
		loaded.navigationPanelAutoReveal == expected.navigationPanelAutoReveal,
		"navigation panel settings did not round-trip");
	Expect(loaded.thumbnailPanelVisible == expected.thumbnailPanelVisible,
		"thumbnail panel visibility did not round-trip");
	Expect(loaded.showZoomNavigator == expected.showZoomNavigator,
		"zoom navigator visibility did not round-trip");
	Expect(loaded.pixelColorSamplerEnabled == expected.pixelColorSamplerEnabled,
		"pixel color sampler preference did not round-trip");
	Expect(loaded.doublePageModeEnabled == expected.doublePageModeEnabled &&
		loaded.mangaReadingOrderEnabled == expected.mangaReadingOrderEnabled &&
		loaded.mangaModeInvertsLeftRight == expected.mangaModeInvertsLeftRight &&
		loaded.spacebarNavigatesImages == expected.spacebarNavigatesImages &&
		loaded.folderWrapAround == expected.folderWrapAround &&
		loaded.fitRelativeZoomMode == expected.fitRelativeZoomMode,
		"navigation and display-mode settings did not round-trip");
	Expect(settingsContents.find("# Define fit-to-window as 100% zoom instead of source-pixel scale.\n"
		"fit_relative_zoom_mode=1\n") != std::string::npos,
		"saved config omitted the fit-relative zoom setting or its explanation");
	Expect(loaded.transparencyPattern == expected.transparencyPattern,
		"transparent-image pattern did not round-trip");
	Expect(loaded.thumbnailPanelWidth == expected.thumbnailPanelWidth,
		"thumbnail panel width did not round-trip");
	Expect(loaded.fileDialogWidth == expected.fileDialogWidth &&
		loaded.fileDialogHeight == expected.fileDialogHeight,
		"file-dialog dimensions did not round-trip");
	ExpectNear(loaded.fileDialogPreviewRatio, expected.fileDialogPreviewRatio, 0.0000001,
		"file-dialog preview proportion did not round-trip");
	Expect(loaded.magnifyingGlassWidth == expected.magnifyingGlassWidth &&
		loaded.magnifyingGlassHeight == expected.magnifyingGlassHeight,
		"magnifying-glass size did not round-trip");
	ExpectNear(loaded.magnifyingGlassZoomLevel, expected.magnifyingGlassZoomLevel, 0.0000001,
		"magnifying-glass zoom level did not round-trip");
	Expect(loaded.fixedCropWidth == expected.fixedCropWidth &&
		loaded.fixedCropHeight == expected.fixedCropHeight &&
		loaded.fixedCropScreenPixels == expected.fixedCropScreenPixels,
		"fixed crop size and pixel units did not round-trip");
	Expect(loaded.userCropAspectWidth == expected.userCropAspectWidth &&
		loaded.userCropAspectHeight == expected.userCropAspectHeight &&
		loaded.selectionModeEnabled == expected.selectionModeEnabled &&
		loaded.copySelectionOnRelease == expected.copySelectionOnRelease,
		"user crop ratio, selection mode, or automatic-copy setting did not round-trip");
	Expect(loaded.infoVisible == expected.infoVisible && loaded.showHistogram == expected.showHistogram &&
		loaded.showFilename == expected.showFilename &&
		loaded.autoContrast == expected.autoContrast,
		"overlay/correction settings did not round-trip");
	Expect(loaded.keepPictureLevels == expected.keepPictureLevels,
		"keep picture levels setting did not round-trip");
	Expect(jpegview_linux::EqualImageProcessing(loaded.defaultImageProcessing,
		expected.defaultImageProcessing), "default picture levels did not round-trip");
	ExpectNear(loaded.unsharpMaskRadius, expected.unsharpMaskRadius, 0.0000001,
		"unsharp radius setting did not round-trip");
	ExpectNear(loaded.unsharpMaskAmount, expected.unsharpMaskAmount, 0.0000001,
		"unsharp amount setting did not round-trip");
	ExpectNear(loaded.unsharpMaskThreshold, expected.unsharpMaskThreshold, 0.0000001,
		"unsharp threshold setting did not round-trip");
	Expect(loaded.copyRenamePattern == expected.copyRenamePattern, "batch pattern did not round-trip");
	Expect(loaded.gpsMapProviderUrl == expected.gpsMapProviderUrl,
		"GPS map provider URL did not round-trip");
	Expect(loaded.windowTitlePattern == expected.windowTitlePattern,
		"window-title pattern did not round-trip");
	Expect(loaded.cacheSizeMiB == expected.cacheSizeMiB, "cache size did not round-trip");
	Expect(loaded.manualZoomSet, "saved manual zoom was not marked present");
	ExpectNear(loaded.manualZoom, expected.manualZoom, 0.0000001, "manual zoom did not round-trip");

	const fs::path malformed = temporary.path() / "malformed.conf";
	std::ofstream malformedOutput(malformed);
	malformedOutput << "  scale_mode = manual\nmanual_zoom=not-a-number\ndefault_gamma=not-a-number\n"
		"window_title_pattern=%q is invalid\n"
		"thumbnail_panel_width=not-a-number\nfile_dialog_width=not-a-number\n"
		"file_dialog_height=not-a-number\nfile_dialog_preview_ratio=nan\n"
		"magnifying_glass_width=not-a-number\nmagnifying_glass_height=not-a-number\n"
		"magnifying_glass_zoom_level=nan\n"
		"fixed_crop_width=not-a-number\nfixed_crop_height=0\n"
		"user_crop_aspect_width=0\nuser_crop_aspect_height=nan\n"
		"fixed_crop_screen_pixels=maybe\ndefault_selection_mode=1\n"
		"selection_mode_enabled=maybe\n"
		"copy_selection_on_release=maybe\n"
		"show_zoom_navigator=maybe\npixel_color_sampler_enabled=maybe\n"
		"manga_mode_inverts_left_right=maybe\n"
		"spacebar_navigates_images=maybe\nfolder_wrap_around=maybe\n"
		"fit_relative_zoom_mode=maybe\n"
		"transparency_pattern=diagonal\n"
		"cache_size_mb=not-a-number\nunknown_key=value\n";
	malformedOutput.close();
	loaded = {};
	Expect(jpegview_linux::LoadViewerSettings(malformed, loaded), "malformed settings file was rejected entirely");
	Expect(loaded.scaleMode == "manual", "whitespace around a setting was not trimmed");
	Expect(loaded.windowTitlePattern == jpegview_linux::kDefaultWindowTitlePattern,
		"unknown window-title code did not retain the built-in default");
	Expect(!loaded.manualZoomSet && loaded.manualZoom == 1.0,
		"malformed manual zoom did not retain its default");
	Expect(!loaded.thumbnailPanelVisible,
		"settings without thumbnail visibility did not retain the hidden default");
	Expect(!loaded.showHistogram,
		"settings without a histogram choice did not retain the hidden default");
	Expect(!loaded.copySelectionOnRelease,
		"malformed copy-selection-on-release setting did not retain its default");
	Expect(loaded.showZoomNavigator,
		"malformed zoom navigator visibility did not retain its enabled default");
	Expect(!loaded.pixelColorSamplerEnabled,
		"missing or malformed pixel sampler setting did not retain its disabled default");
	Expect(!loaded.doublePageModeEnabled && !loaded.mangaReadingOrderEnabled,
		"missing double-page settings did not retain their disabled defaults");
	Expect(loaded.mangaModeInvertsLeftRight,
		"missing or malformed manga inversion setting did not preserve its enabled default");
	Expect(!loaded.spacebarNavigatesImages,
		"missing or malformed Space navigation setting did not preserve its disabled default");
	Expect(loaded.folderWrapAround,
		"missing or malformed folder-wrap setting did not preserve the wrapping default");
	Expect(!loaded.fitRelativeZoomMode,
		"missing or malformed fit-relative zoom setting did not preserve its disabled default");
	Expect(loaded.transparencyPattern == jpegview_linux::TransparencyPattern::Black,
		"malformed transparency pattern did not retain the default black background");
	Expect(loaded.thumbnailPanelWidth == jpegview_linux::kDefaultThumbnailPanelWidth,
		"malformed thumbnail width did not retain its default");
	Expect(loaded.fileDialogWidth == jpegview_linux::kDefaultFileDialogWidth &&
		loaded.fileDialogHeight == jpegview_linux::kDefaultFileDialogHeight &&
		loaded.fileDialogPreviewRatio == 0.0,
		"malformed file-dialog geometry did not retain its defaults");
	Expect(loaded.magnifyingGlassWidth == jpegview_linux::MagnifyingGlassModel::kDefaultWidth &&
		loaded.magnifyingGlassHeight == jpegview_linux::MagnifyingGlassModel::kDefaultHeight &&
		loaded.magnifyingGlassZoomLevel ==
			jpegview_linux::MagnifyingGlassModel::kDefaultZoomLevel,
		"malformed magnifying-glass settings did not retain their defaults");
	Expect(loaded.fixedCropWidth == jpegview_linux::kDefaultFixedCropWidth &&
		loaded.fixedCropHeight == jpegview_linux::kMinimumFixedCropDimension &&
		loaded.fixedCropScreenPixels &&
		loaded.userCropAspectWidth == jpegview_linux::kDefaultUserCropAspectWidth &&
		loaded.userCropAspectHeight == jpegview_linux::kDefaultUserCropAspectHeight &&
		!loaded.selectionModeEnabled,
		"malformed or legacy selection-mode settings did not retain the disabled default");
	Expect(loaded.cacheSizeMiB == jpegview_linux::kDefaultCacheSizeMiB,
		"malformed cache size did not retain its default");

	const fs::path clamped = temporary.path() / "clamped.conf";
	WriteText(clamped, "manual_zoom=1000\nthumbnail_panel_width=2\n"
		"file_dialog_width=1\nfile_dialog_height=999999\nfile_dialog_preview_ratio=4\n"
		"magnifying_glass_width=1\nmagnifying_glass_height=999999\n"
		"magnifying_glass_zoom_level=4\n"
		"transparency_pattern=white\n"
		"fixed_crop_width=999999\nfixed_crop_height=-10\n"
		"user_crop_aspect_width=999999\nuser_crop_aspect_height=5\n"
		"cache_size_mb=999999999\n");
	loaded = {};
	Expect(jpegview_linux::LoadViewerSettings(clamped, loaded) && loaded.manualZoomSet &&
		loaded.manualZoom == jpegview_linux::kMaximumZoom &&
		loaded.thumbnailPanelWidth == jpegview_linux::kMinimumThumbnailPanelWidth &&
		loaded.fileDialogWidth == jpegview_linux::kMinimumFileDialogWidth &&
		loaded.fileDialogHeight == jpegview_linux::kMaximumFileDialogDimension &&
		loaded.fileDialogPreviewRatio == 0.8 &&
		loaded.magnifyingGlassWidth == jpegview_linux::MagnifyingGlassModel::kMinimumWidth &&
		loaded.magnifyingGlassHeight == jpegview_linux::MagnifyingGlassModel::kMaximumDimension &&
		loaded.magnifyingGlassZoomLevel ==
			jpegview_linux::MagnifyingGlassModel::kMaximumZoomLevel &&
		loaded.transparencyPattern == jpegview_linux::TransparencyPattern::White &&
		loaded.fixedCropWidth == jpegview_linux::kMaximumFixedCropDimension &&
		loaded.fixedCropHeight == jpegview_linux::kMinimumFixedCropDimension &&
		loaded.userCropAspectWidth == jpegview_linux::kMaximumFixedCropDimension &&
		loaded.userCropAspectHeight == 5 &&
		loaded.cacheSizeMiB == jpegview_linux::kMaximumCacheSizeMiB,
		"out-of-range settings were not clamped to their public limits");

	jpegview_linux::ViewerSettings unchanged;
	unchanged.scaleMode = "sentinel";
	Expect(!jpegview_linux::LoadViewerSettings(temporary.path() / "missing.conf", unchanged) &&
		unchanged.scaleMode == "sentinel",
		"missing settings file modified the caller's existing settings");
	unchanged.windowTitlePattern = "invalid %q";
	Expect(!jpegview_linux::SaveViewerSettings(settingsPath, unchanged),
		"invalid window-title pattern was written to settings");
	loaded = {};
	Expect(jpegview_linux::LoadViewerSettings(settingsPath, loaded) &&
		loaded.windowTitlePattern == expected.windowTitlePattern,
		"invalid settings save replaced the last valid title pattern");
}

void TestGpsMapProviderUrlValidationAndCoordinateFormatting() {
	const std::string defaultUrl = jpegview_linux::BuildGpsMapUrl(
		jpegview_linux::kDefaultGpsMapProviderUrl, -12.582222, -98.118333);
	Expect(defaultUrl == "https://opentopomap.org/#marker=15/-12.58222/-98.11833",
		"default GPS map URL did not substitute signed decimal coordinates");
	const std::string repeated = jpegview_linux::BuildGpsMapUrl(
		"HTTPS://maps.example.test/{lat}/{lng}?center={lat},{lng}", 51.5, -0.12);
	Expect(repeated == "HTTPS://maps.example.test/51.50000/-0.12000?center=51.50000,-0.12000",
		"custom GPS map URL did not replace every placeholder with locale-stable values");
	Expect(jpegview_linux::BuildGpsMapUrl("https://map.example/?lat={lat}", 0.0, 0.0).empty() &&
		jpegview_linux::BuildGpsMapUrl("javascript:open({lat},{lng})", 0.0, 0.0).empty() &&
		jpegview_linux::BuildGpsMapUrl("https:///map?lat={lat}&lon={lng}", 0.0, 0.0).empty() &&
		jpegview_linux::BuildGpsMapUrl("https://:443/?lat={lat}&lon={lng}", 0.0, 0.0).empty() &&
		jpegview_linux::BuildGpsMapUrl("https://maps.example:bad/?lat={lat}&lon={lng}", 0.0, 0.0).empty() &&
		jpegview_linux::BuildGpsMapUrl("https://user@maps.example/?lat={lat}&lon={lng}", 0.0, 0.0).empty() &&
		jpegview_linux::BuildGpsMapUrl("https://map.example/?lat={lat}&lon={lng} extra", 0.0, 0.0).empty(),
		"GPS map URL accepted an unsafe scheme, malformed authority, missing coordinate, or whitespace");
	Expect(jpegview_linux::BuildGpsMapUrl(
		"https://maps.example:8443/?lat={lat}&lon={lng}", 1.0, 2.0) ==
		"https://maps.example:8443/?lat=1.00000&lon=2.00000" &&
		jpegview_linux::BuildGpsMapUrl(
			"https://[2001:db8::1]:8443/?lat={lat}&lon={lng}", 1.0, 2.0) ==
			"https://[2001:db8::1]:8443/?lat=1.00000&lon=2.00000",
		"GPS map URL rejected a valid hostname port or bracketed IPv6 authority");
	Expect(jpegview_linux::BuildGpsMapUrl(
		"https://map.example/?lat={lat}&lon={lng}", 90.00001, 0.0).empty() &&
		jpegview_linux::BuildGpsMapUrl(
			"https://map.example/?lat={lat}&lon={lng}", 0.0, -180.00001).empty() &&
		jpegview_linux::BuildGpsMapUrl(
			"https://map.example/?lat={lat}&lon={lng}",
			std::numeric_limits<double>::quiet_NaN(), 0.0).empty(),
		"GPS map URL accepted non-finite or out-of-range coordinates");
	Expect(!jpegview_linux::AreGpsCoordinatesValid(90.00001, 0.0) &&
		!jpegview_linux::AreGpsCoordinatesValid(0.0, -180.00001) &&
		jpegview_linux::AreGpsCoordinatesValid(-90.0, 180.0),
		"GPS coordinate range validation accepted an invalid pair or rejected its inclusive limits");
}

void TestAdvancedConfigurationModelCategoriesAndRoundTrips() {
	using Category = jpegview_linux::AdvancedConfigurationCategory;
	using FieldKind = jpegview_linux::AdvancedConfigurationFieldKind;
	jpegview_linux::ViewerSettings source;
	source.scaleMode = "manual";
	source.maximized = true;
	source.selectionModeEnabled = true;
	source.copySelectionOnRelease = true;
	source.autoContrast = true;
	source.mangaModeInvertsLeftRight = false;
	source.spacebarNavigatesImages = true;
	source.folderWrapAround = false;
	source.fitRelativeZoomMode = true;
	source.transparencyPattern = jpegview_linux::TransparencyPattern::White;
	source.showHistogram = true;
	source.thumbnailPanelWidth = 287;
	source.fileDialogWidth = 1040;
	source.fileDialogHeight = 735;
	source.fileDialogPreviewRatio = 0.375;
	source.magnifyingGlassWidth = 425;
	source.magnifyingGlassHeight = 215;
	source.magnifyingGlassZoomLevel = 0.725;
	source.userCropAspectWidth = 13;
	source.userCropAspectHeight = 7;
	source.defaultImageProcessing.contrast = 0.18;
	source.defaultImageProcessing.gamma = 1.15;
	source.defaultImageProcessing.saturation = 1.25;
	source.defaultImageProcessing.cyanRed = 0.2;
	source.defaultImageProcessing.magentaGreen = -0.2;
	source.defaultImageProcessing.yellowBlue = 0.3;
	source.defaultImageProcessing.lightenShadows = 0.4;
	source.defaultImageProcessing.darkenHighlights = 0.2;
	source.defaultImageProcessing.deepShadows = 0.6;
	source.defaultImageProcessing.colorCorrection = 0.1;
	source.defaultImageProcessing.contrastCorrection = 0.55;
	source.defaultImageProcessing.sharpen = 0.15;
	source.defaultImageProcessing.localDensityEnabled = true;
	source.unsharpMaskRadius = 2.25;
	source.unsharpMaskAmount = 3.5;
	source.unsharpMaskThreshold = 7.0;
	source.cacheSizeMiB = 1536;
	source.copyRenamePattern = u8"%F=旅行-%n";
	source.windowTitlePattern = "%f (%m) [%p]";

	const std::array<std::vector<std::string>, 7> expectedKeys = {{
		{"manga_mode_inverts_left_right", "spacebar_navigates_images", "folder_wrap_around",
			"fit_relative_zoom_mode"},
		{"transparency_pattern", "show_histogram", "window_title_pattern"},
		{"thumbnail_panel_width", "file_dialog_width", "file_dialog_height",
			"file_dialog_preview_ratio"},
		{"magnifying_glass_width", "magnifying_glass_height", "magnifying_glass_zoom_level"},
		{"user_crop_aspect_width", "user_crop_aspect_height", "copy_selection_on_release"},
		{"default_local_density", "default_contrast", "default_gamma", "default_saturation",
			"default_cyan_red", "default_magenta_green", "default_yellow_blue",
			"default_lighten_shadows", "default_darken_highlights", "default_deep_shadows",
			"default_color_correction", "default_contrast_correction", "default_sharpen",
			"unsharp_mask_radius", "unsharp_mask_amount", "unsharp_mask_threshold"},
		{"cache_size_mb", "copy_rename_pattern"},
	}};
	const char* const categoryNames[] = {
		"Behavior", "Appearance", "Panels & dialogs", "Magnifying glass", "Crop",
		"Image defaults", "Performance & batch",
	};

	jpegview_linux::AdvancedConfigurationModel model;
	model.Open(source);
	Expect(model.IsOpen() && model.CategoryCount() == 7 && model.ActiveCategory() == 0,
		"advanced configuration did not open on its first category");
	std::size_t fieldTotal = 0;
	for (int category = 0; category < model.CategoryCount(); ++category) {
		Expect(std::string(model.CategoryName(category)) == categoryNames[category],
			"advanced configuration category label changed");
		Expect(model.SelectCategory(category) && model.ActiveCategory() == category,
			"advanced configuration could not select a category");
		Expect(model.RowCount() == static_cast<int>(expectedKeys[category].size()),
			"advanced configuration category has an unexpected row count");
		for (int row = 0; row < model.RowCount(); ++row) {
			const jpegview_linux::AdvancedConfigurationField* field = model.FieldAt(row);
			Expect(field != nullptr && model.FieldKey(row) == expectedKeys[category][row] &&
				model.FieldLabel(row)[0] != '\0',
				"advanced configuration field metadata is missing or out of order");
			if (field->kind == FieldKind::Integer || field->kind == FieldKind::Decimal) {
				Expect(field->minimum < field->maximum && field->step > 0.0,
					"numeric advanced setting has invalid bounds or adjustment step");
			} else if (field->kind == FieldKind::Choice) {
				Expect(field->choices != nullptr && field->choiceCount >= 2,
					"choice advanced setting has no choice labels");
			}
			++fieldTotal;
		}
	}
	Expect(fieldTotal == 35, "advanced configuration did not expose every requested setting");
	Expect(model.SelectCategory(static_cast<int>(Category::Behavior)) &&
		model.FieldValue(2) == "Off" && model.FieldValue(3) == "On",
		"behavior settings did not display their draft values");
	Expect(model.SelectCategory(static_cast<int>(Category::Appearance)) &&
		model.FieldValue(0) == "White" && model.FieldValue(1) == "On" &&
		model.FieldValue(2) == source.windowTitlePattern,
		"choice or boolean setting values were not formatted for display");
	Expect(model.SelectCategory(static_cast<int>(Category::PanelsAndDialogs)) &&
		model.FieldValue(3) == "37.5%",
		"file-dialog preview ratio did not use its percentage display format");
	Expect(model.SelectCategory(static_cast<int>(Category::MagnifyingGlass)) &&
		model.FieldValue(2) == "0.725",
		"magnifying-glass zoom level did not retain its source-scale meaning");
	Expect(model.SelectCategory(static_cast<int>(Category::Crop)) &&
		model.FieldValue(2) == "On",
		"copy-selection-on-release setting did not display its draft value");
	Expect(jpegview_linux::EqualImageProcessing(model.Draft().defaultImageProcessing,
		source.defaultImageProcessing) && model.Draft().copyRenamePattern == source.copyRenamePattern &&
		model.Draft().magnifyingGlassWidth == source.magnifyingGlassWidth &&
		model.Draft().fileDialogPreviewRatio == source.fileDialogPreviewRatio &&
		model.Draft().windowTitlePattern == source.windowTitlePattern &&
		model.Draft().fitRelativeZoomMode == source.fitRelativeZoomMode,
		"opening and visiting categories changed the persisted settings draft");
	Expect(model.Draft().scaleMode == source.scaleMode && model.Draft().maximized == source.maximized &&
		model.Draft().selectionModeEnabled == source.selectionModeEnabled &&
		model.Draft().copySelectionOnRelease == source.copySelectionOnRelease &&
		model.Draft().autoContrast == source.autoContrast,
		"advanced configuration altered excluded automatic or session settings");

	auto edit = [&model](int row, const std::string& text) {
		model.SelectRow(row);
		Expect(model.ActivateSelected() && model.IsEditing(),
			"activating a typed advanced setting did not begin editing");
		Expect(model.AppendText(text), "advanced configuration rejected valid edit text");
		Expect(model.CommitEdit() && !model.IsEditing(),
			"valid advanced setting edit did not commit");
	};

	model.SelectCategory(static_cast<int>(Category::Behavior));
	model.SelectRow(0);
	Expect(model.ActivateSelected() && model.Draft().mangaModeInvertsLeftRight,
		"behavior boolean did not toggle on activation");
	Expect(model.AdjustSelected(1) && !model.Draft().mangaModeInvertsLeftRight,
		"directional adjustment did not toggle a boolean setting");
	model.SelectRow(1);
	Expect(model.AdjustSelected(-1) && !model.Draft().spacebarNavigatesImages,
		"behavior boolean did not toggle through directional adjustment");
	model.SelectRow(2);
	Expect(model.ActivateSelected() && model.Draft().folderWrapAround &&
		model.AdjustSelected(-1) && !model.Draft().folderWrapAround,
		"folder-wrap setting did not toggle in both advanced-configuration controls");
	model.SelectRow(3);
	Expect(model.ActivateSelected() && !model.Draft().fitRelativeZoomMode &&
		model.AdjustSelected(1) && model.Draft().fitRelativeZoomMode,
		"fit-relative zoom setting did not toggle in both advanced-configuration controls");

	model.SelectCategory(static_cast<int>(Category::Appearance));
	model.SelectRow(0);
	Expect(model.AdjustSelected(1) &&
		model.Draft().transparencyPattern == jpegview_linux::TransparencyPattern::Checkerboard,
		"appearance choice did not advance to the next transparency pattern");
	Expect(model.AdjustSelected(1) &&
		model.Draft().transparencyPattern == jpegview_linux::TransparencyPattern::Black,
		"appearance choice did not wrap after its final value");
	model.SelectRow(1);
	Expect(model.ActivateSelected() && !model.Draft().showHistogram,
		"appearance boolean did not toggle on activation");
	model.SelectRow(2);
	model.BeginEdit();
	model.SelectAll();
	Expect(model.AppendText("%f - [%i/%n] - %a") && model.CommitEdit() &&
		model.Draft().windowTitlePattern == "%f - [%i/%n] - %a" &&
		model.Draft().copyRenamePattern == source.copyRenamePattern,
		"window-title text editor did not update its independent setting");

	model.SelectCategory(static_cast<int>(Category::PanelsAndDialogs));
	model.SelectRow(0);
	Expect(model.AdjustSelected(1) && model.Draft().thumbnailPanelWidth == 295,
		"thumbnail panel width did not use its configured adjustment step");
	edit(1, "1400");
	edit(2, "850");
	edit(3, "0.5");
	Expect(model.Draft().fileDialogWidth == 1400 && model.Draft().fileDialogHeight == 850 &&
		std::abs(model.Draft().fileDialogPreviewRatio - 0.5) < 1e-12,
		"panels and dialogs fields did not round-trip edits to the ViewerSettings draft");

	model.SelectCategory(static_cast<int>(Category::MagnifyingGlass));
	edit(0, "560");
	edit(1, "280");
	model.SelectRow(2);
	Expect(model.AdjustSelected(1) &&
		std::abs(model.Draft().magnifyingGlassZoomLevel - 0.75) < 1e-12,
		"magnifying-glass zoom level did not adjust in its source-scale units");
	Expect(model.FieldValue(2) == "0.750", "magnifying-glass zoom formatting lost precision");
	model.SelectCategory(static_cast<int>(Category::Crop));
	edit(0, "21");
	edit(1, "9");
	model.SelectRow(2);
	Expect(model.ActivateSelected() && !model.Draft().copySelectionOnRelease &&
		model.AdjustSelected(1) && model.Draft().copySelectionOnRelease &&
		model.Draft().userCropAspectWidth == 21 && model.Draft().userCropAspectHeight == 9,
		"crop aspect fields or automatic selection-copy toggle did not update the settings draft");

	model.SelectCategory(static_cast<int>(Category::ImageDefaults));
	model.SelectRow(0);
	Expect(model.ActivateSelected() && !model.Draft().defaultImageProcessing.localDensityEnabled,
		"default local-density boolean did not toggle");
	for (std::size_t index = 0; index < static_cast<std::size_t>(jpegview_linux::LevelControl::Count); ++index) {
		const int row = static_cast<int>(index) + 1;
		const auto* field = model.FieldAt(row);
		const auto control = static_cast<jpegview_linux::LevelControl>(index);
		const double target = (field->minimum + field->maximum) / 2.0;
		edit(row, std::to_string(target));
		ExpectNear(jpegview_linux::GetLevelControlValue(model.Draft().defaultImageProcessing,
			control), target, 0.00001,
			"one of the twelve default picture-level controls did not round-trip");
	}
	edit(13, "3.25");
	edit(14, "8.75");
	edit(15, "19.5");
	Expect(model.Draft().unsharpMaskRadius == 3.25 && model.Draft().unsharpMaskAmount == 8.75 &&
		model.Draft().unsharpMaskThreshold == 19.5,
		"unsharp-mask defaults did not round-trip through typed edits");

	model.SelectCategory(static_cast<int>(Category::PerformanceAndBatch));
	model.SelectRow(0);
	Expect(model.AdjustSelected(1) && model.Draft().cacheSizeMiB == 1600,
		"cache budget did not adjust by the declared MB step");
	edit(1, u8"copy-旅行-%F_%n");
	Expect(model.Draft().copyRenamePattern == u8"copy-旅行-%F_%n",
		"copy/rename pattern did not preserve Unicode and template text");

	model.SelectCategory(static_cast<int>(Category::ImageDefaults));
	model.SetVisibleRows(3);
	Expect(model.VisibleRows() == 3 && model.Scroll() == 0,
		"advanced configuration did not retain the visible row count");
	model.MoveSelection(8);
	Expect(model.SelectedRow() == 8 && model.Scroll() == 6,
		"row selection did not scroll to keep the selected field visible");
	model.ScrollBy(100);
	Expect(model.Scroll() == static_cast<std::size_t>(model.RowCount() - model.VisibleRows()) &&
		model.SelectedRow() == 8,
		"independent field scrolling did not clamp at the end or changed selection");
	model.ScrollBy(-100);
	Expect(model.Scroll() == 0, "field scrolling did not clamp at the beginning");
	model.MoveSelection(-100);
	Expect(model.SelectedRow() == 0 && model.Scroll() == 0,
		"row selection did not clamp to the first field");
	model.SelectCategory(static_cast<int>(Category::Behavior));
	model.MoveCategory(-1);
	Expect(model.ActiveCategory() == static_cast<int>(Category::PerformanceAndBatch),
		"category cycling did not wrap backward");
	model.MoveCategory(1);
	Expect(model.ActiveCategory() == static_cast<int>(Category::Behavior),
		"category cycling did not wrap forward");

	model.Close();
	Expect(!model.IsOpen() && !model.IsEditing(), "advanced configuration did not close cleanly");
}

void TestAdvancedConfigurationModelValidationAndCancellation() {
	using Category = jpegview_linux::AdvancedConfigurationCategory;
	jpegview_linux::ViewerSettings original;
	original.fileDialogWidth = 900;
	original.fileDialogPreviewRatio = 0.375;
	original.unsharpMaskRadius = 1.0;
	original.copyRenamePattern = u8"old-写像-%n";
	original.windowTitlePattern = "%f - [%p]";
	jpegview_linux::AdvancedConfigurationModel model;
	model.Open(original);

	model.SelectCategory(static_cast<int>(Category::PanelsAndDialogs));
	model.SelectRow(1);
	model.BeginEdit();
	Expect(model.IsEditing() && model.EditingText() == "900",
		"numeric edit did not start with the current setting");
	Expect(model.AppendText("12px"), "numeric editor rejected input before strict validation");
	Expect(!model.CommitEdit() && model.IsEditing() && !model.Message().empty() &&
		model.Draft().fileDialogWidth == 900,
		"malformed integer input changed the draft or failed without a validation message");
	model.CancelEdit();
	Expect(!model.IsEditing() && model.Draft().fileDialogWidth == 900,
		"cancelling an invalid numeric edit changed the draft");

	model.BeginEdit();
	model.AppendText("999999");
	Expect(model.CommitEdit() && model.Draft().fileDialogWidth == jpegview_linux::kMaximumFileDialogDimension &&
		!model.Message().empty(),
		"integer setting did not clamp to its documented maximum");
	model.SelectRow(1);
	model.BeginEdit();
	model.AppendText("-5");
	Expect(model.CommitEdit() && model.Draft().fileDialogWidth == jpegview_linux::kMinimumFileDialogWidth,
		"integer setting did not clamp to its documented minimum");

	model.SelectRow(3);
	model.BeginEdit();
	model.AppendText("nan");
	Expect(!model.CommitEdit() && model.Draft().fileDialogPreviewRatio == 0.375,
		"non-finite decimal input was accepted or changed the draft");
	model.SelectAll();
	model.AppendText("0x1p-1");
	Expect(!model.CommitEdit() && model.Draft().fileDialogPreviewRatio == 0.375,
		"non-decimal hexadecimal notation passed strict numeric validation");
	model.SelectAll();
	model.AppendText("1");
	Expect(model.CommitEdit() && model.Draft().fileDialogPreviewRatio == 0.8,
		"decimal setting did not clamp to its documented maximum");

	model.SelectCategory(static_cast<int>(Category::MagnifyingGlass));
	model.SelectRow(2);
	model.BeginEdit();
	model.AppendText("-1");
	Expect(model.CommitEdit() &&
		model.Draft().magnifyingGlassZoomLevel == jpegview_linux::MagnifyingGlassModel::kMinimumZoomLevel,
		"magnifying-glass zoom did not clamp to its documented minimum");
	model.SelectCategory(static_cast<int>(Category::ImageDefaults));
	model.SelectRow(13);
	model.BeginEdit();
	model.AppendText("100");
	Expect(model.CommitEdit() && model.Draft().unsharpMaskRadius == 5.0,
		"unsharp radius did not clamp to its documented maximum");
	model.BeginEdit();
	model.AppendText("-1");
	Expect(model.CommitEdit() && model.Draft().unsharpMaskRadius == 0.0,
		"unsharp radius did not clamp to its documented minimum");

	model.SelectCategory(static_cast<int>(Category::Crop));
	model.SelectRow(0);
	model.BeginEdit();
	model.AppendText("0");
	Expect(model.CommitEdit() && model.Draft().userCropAspectWidth == 1,
		"crop aspect integer did not clamp to its documented minimum");
	model.SelectRow(1);
	model.BeginEdit();
	model.AppendText("999999");
	Expect(model.CommitEdit() && model.Draft().userCropAspectHeight ==
		jpegview_linux::kMaximumFixedCropDimension,
		"crop aspect integer did not clamp to its documented maximum");

	model.SelectCategory(static_cast<int>(Category::PerformanceAndBatch));
	model.SelectRow(1);
	model.BeginEdit();
	model.SelectAll();
	model.AppendText(u8"copy-写像");
	model.Backspace();
	Expect(model.EditingText() == u8"copy-写",
		"copy/rename pattern backspace split a UTF-8 code point");
	model.AppendText(u8"像");
	Expect(model.CommitEdit() && model.Draft().copyRenamePattern == u8"copy-写像",
		"copy/rename text edit did not commit the edited string");

	model.BeginEdit();
	model.SelectAll();
	model.AppendText("temporary");
	model.CancelEdit();
	Expect(model.Draft().copyRenamePattern == u8"copy-写像",
		"cancelling a text edit changed the active draft");
	model.BeginEdit();
	model.SelectAll();
	Expect(!model.AppendText("bad\npattern") && model.EditingText().empty(),
		"copy/rename editor accepted a line break that would corrupt settings storage");
	model.CancelEdit();

	model.SelectCategory(static_cast<int>(Category::Appearance));
	model.SelectRow(2);
	model.BeginEdit();
	model.SelectAll();
	model.AppendText("%f %q");
	Expect(!model.CommitEdit() && model.IsEditing() &&
		model.Message().find("%q") != std::string::npos &&
		model.Draft().windowTitlePattern == original.windowTitlePattern,
		"unknown title-format code changed the draft or omitted its validation message");
	model.CancelEdit();
	model.BeginEdit();
	model.SelectAll();
	model.AppendText("%f [%p] %%");
	Expect(model.CommitEdit() && model.Draft().windowTitlePattern == "%f [%p] %%",
		"valid title-format edit was not committed");
	model.BeginEdit();
	model.SelectAll();
	model.AppendText("temporary %v");
	model.CancelEdit();
	Expect(model.Draft().windowTitlePattern == "%f [%p] %%",
		"cancelling a title-format edit changed the active draft");
	model.BeginEdit();
	model.SelectAll();
	Expect(model.AppendText("   ") && model.CommitEdit() &&
		model.Draft().windowTitlePattern == jpegview_linux::kDefaultWindowTitlePattern,
		"empty title-format edit did not restore the built-in pattern");

	model.Open(original);
	model.SelectCategory(static_cast<int>(Category::Behavior));
	model.SelectRow(0);
	Expect(model.ActivateSelected() && model.Draft().mangaModeInvertsLeftRight !=
		original.mangaModeInvertsLeftRight,
		"draft mutation could not be staged before cancelling the dialog");
	Expect(original.mangaModeInvertsLeftRight && original.fileDialogWidth == 900 &&
		original.copyRenamePattern == u8"old-写像-%n" &&
		original.windowTitlePattern == "%f - [%p]",
		"editing an advanced configuration draft mutated its source settings");
	model.Close();
	model.Open(original);
	Expect(model.Draft().mangaModeInvertsLeftRight == original.mangaModeInvertsLeftRight &&
		model.Draft().fileDialogWidth == original.fileDialogWidth &&
		model.Draft().copyRenamePattern == original.copyRenamePattern &&
		model.Draft().windowTitlePattern == original.windowTitlePattern,
		"reopening after cancellation did not restore the caller-provided settings");
	model.SetMessage("adapter message");
	Expect(model.Message() == "adapter message", "advanced configuration message was not retained");
}

void TestTransparencyPatternValuesAndTileColors() {
	using jpegview_linux::TransparencyPattern;
	TransparencyPattern pattern = TransparencyPattern::Black;
	Expect(jpegview_linux::ParseTransparencyPattern("black", pattern) &&
		pattern == TransparencyPattern::Black,
		"black transparency pattern could not be parsed");
	Expect(jpegview_linux::ParseTransparencyPattern("white", pattern) &&
		pattern == TransparencyPattern::White,
		"white transparency pattern could not be parsed");
	Expect(jpegview_linux::ParseTransparencyPattern("checkerboard", pattern) &&
		pattern == TransparencyPattern::Checkerboard,
		"checkerboard transparency pattern could not be parsed");
	Expect(!jpegview_linux::ParseTransparencyPattern("stripe", pattern) &&
		pattern == TransparencyPattern::Checkerboard,
		"invalid transparency pattern changed the active value");
	const auto black = jpegview_linux::TransparencyPatternTileColor(TransparencyPattern::Black, 1, 0);
	const auto white = jpegview_linux::TransparencyPatternTileColor(TransparencyPattern::White, 1, 0);
	const auto light = jpegview_linux::TransparencyPatternTileColor(TransparencyPattern::Checkerboard, 0, 0);
	const auto dark = jpegview_linux::TransparencyPatternTileColor(TransparencyPattern::Checkerboard, 1, 0);
	Expect(black.red == 0 && black.green == 0 && black.blue == 0 &&
		white.red == 255 && white.green == 255 && white.blue == 255,
		"solid transparency patterns returned incorrect colors");
	Expect(light.red == 208 && light.green == 208 && light.blue == 208 &&
		dark.red == 144 && dark.green == 144 && dark.blue == 144 &&
		jpegview_linux::kTransparencyCheckerCellSize > 0,
		"checkerboard pattern colors or tile geometry were invalid");
}

void TestRuntimeSettingsOwnerCommitSemantics() {
	TemporaryDirectory temporary;
	const fs::path settingsPath = temporary.path() / "config" / "settings.conf";
	jpegview_linux::RuntimeSettingsOwner owner;
	owner.Values().maximized = true;
	owner.Values().thumbnailPanelVisible = true;
	Expect(!owner.Load(temporary.path() / "missing.conf") &&
		owner.Values().maximized && owner.Values().thumbnailPanelVisible,
		"a missing settings file changed the live runtime defaults");

	jpegview_linux::ViewerSettings candidate = owner.Values();
	candidate.maximized = false;
	candidate.doublePageModeEnabled = true;
	candidate.cacheSizeMiB = 768;
	Expect(owner.SaveAndAdopt(settingsPath, candidate) &&
		!owner.Values().maximized && owner.Values().doublePageModeEnabled &&
		owner.Values().cacheSizeMiB == 768,
		"successful settings persistence did not commit the new runtime defaults");
	jpegview_linux::ViewerSettings persisted;
	Expect(jpegview_linux::LoadViewerSettings(settingsPath, persisted) &&
		persisted.doublePageModeEnabled && persisted.cacheSizeMiB == 768,
		"runtime settings owner committed values that were not persisted");

	const fs::path blockedParent = temporary.path() / "not-a-directory";
	{
		std::ofstream blocker(blockedParent);
		blocker << "file blocks settings directory creation";
	}
	candidate.maximized = true;
	Expect(!owner.SaveAndAdopt(blockedParent / "settings.conf", candidate) &&
		!owner.Values().maximized && owner.Values().doublePageModeEnabled &&
		owner.Values().cacheSizeMiB == 768,
		"failed settings persistence applied a draft to the live runtime owner");
}

void TestSettingsPathSelection() {
	TemporaryDirectory temporary;
	const fs::path xdgHome = temporary.path() / "xdg";
	const fs::path home = temporary.path() / "home";
	ScopedEnvironment xdg("XDG_CONFIG_HOME", xdgHome.string());
	ScopedEnvironment homeEnvironment("HOME", home.string());
	Expect(jpegview_linux::ViewerSettingsPath() == xdgHome / "jpegview-linux" / "settings.conf",
		"XDG_CONFIG_HOME settings path is incorrect");
	xdg.Clear();
	Expect(jpegview_linux::ViewerSettingsPath() == home / ".config" / "jpegview-linux" / "settings.conf",
		"HOME settings path fallback is incorrect");
	homeEnvironment.Clear();
	Expect(jpegview_linux::ViewerSettingsPath().empty(),
		"settings path was invented when neither XDG_CONFIG_HOME nor HOME was available");
}

void TestSortModeMappings() {
	struct SortCase {
		FileList::SortMode mode;
		const char* setting;
		const char* label;
		const char* description;
	};
	const std::vector<SortCase> cases = {
		{FileList::SortMode::LastModificationTime, "modification_date", "D", "modification date"},
		{FileList::SortMode::CreationTime, "creation_date", "C", "creation date"},
		{FileList::SortMode::FileName, "file_name", "N", "file name"},
		{FileList::SortMode::Random, "random", "R", "random"},
		{FileList::SortMode::FileSize, "file_size", "S", "file size"},
	};
	for (const SortCase& testCase : cases) {
		Expect(std::string(jpegview_linux::SortModeSettingName(testCase.mode)) == testCase.setting,
			"sort mode setting mapping is incorrect");
		Expect(std::string(jpegview_linux::SortModeShortLabel(testCase.mode)) == testCase.label,
			"sort mode short label mapping is incorrect");
		Expect(std::string(jpegview_linux::SortModeDescription(testCase.mode)) == testCase.description,
			"sort mode description mapping is incorrect");
		FileList::SortMode parsed = FileList::SortMode::FileSize;
		Expect(jpegview_linux::ParseSortMode(testCase.setting, parsed), "known sort mode was not parsed");
		Expect(parsed == testCase.mode, "sort mode parser returned the wrong mode");
	}
	FileList::SortMode unchanged = FileList::SortMode::FileName;
	Expect(!jpegview_linux::ParseSortMode("not-a-sort-mode", unchanged), "unknown sort mode was accepted");
	Expect(unchanged == FileList::SortMode::FileName, "unknown sort mode changed the output value");
}

std::string FormatLocalTime(std::time_t timestamp, const char* format) {
	std::tm localTime{};
	Expect(localtime_r(&timestamp, &localTime) != nullptr, "cannot convert test timestamp to local time");
	char output[64]{};
	Expect(std::strftime(output, sizeof(output), format, &localTime) != 0,
		"cannot format test timestamp");
	return output;
}

void MakeTestExecutable(const fs::path& path) {
	std::error_code error;
	fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write |
		fs::perms::owner_exec, fs::perm_options::replace, error);
	if (error) throw TestFailure("cannot mark test command executable: " + error.message());
}

bool WaitForPath(const fs::path& path, std::chrono::milliseconds timeout) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	do {
		std::error_code error;
		if (fs::exists(path, error) && !error) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (std::chrono::steady_clock::now() < deadline);
	return fs::exists(path);
}

bool WaitForChildExit(pid_t child, std::chrono::milliseconds timeout, int& status) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	do {
		const pid_t waited = waitpid(child, &status, WNOHANG);
		if (waited == child) return true;
		if (waited < 0 && errno != EINTR) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
}

std::shared_ptr<const jpegview_linux::Image> MakeTinyTestImage() {
	auto image = std::make_shared<jpegview_linux::Image>();
	const std::uint8_t pixels[] = {
		0, 0, 255, 255, 0, 255, 0, 255,
		255, 0, 0, 255, 255, 255, 255, 255,
	};
	if (!image->StoreBGRA(pixels, 2, 2)) throw TestFailure("cannot create test image pixels");
	return image;
}

void TestExternalProcessStructuredArgumentsAndFailures() {
	std::vector<std::uint8_t> output;
	std::string error;
	Expect(jpegview_linux::RunExternalCommandWithOutput(
		{"printf", {"%s", "path with spaces and ; punctuation"}}, output, 128, error),
		"structured command did not complete");
	Expect(std::string(output.begin(), output.end()) == "path with spaces and ; punctuation",
		"structured command arguments were split or interpreted by a shell");

	TemporaryDirectory temporary;
	const fs::path missing = temporary.path() / "missing command";
	Expect(!jpegview_linux::RunExternalCommand({missing.string(), {}}, error),
		"missing executable was reported as successful");
	Expect(error.find("not installed") != std::string::npos,
		"missing executable failure did not explain that the command is unavailable");
	Expect(!jpegview_linux::RunExternalCommand({"sh", {"-c", "exit 23"}}, error),
		"nonzero child exit was reported as successful");
	Expect(error.find("failed") != std::string::npos,
		"nonzero child exit did not produce a structured failure message");
	Expect(!jpegview_linux::RunExternalCommandWithOutput(
		{"printf", {"%s", "more than two bytes"}}, output, 2, error),
		"external output limit was not enforced");
	Expect(output.empty(), "oversized external output was left published to the caller");
}

void TestExternalProcessCancellationReapsSlowChild() {
	TemporaryDirectory temporary;
	const fs::path marker = temporary.path() / "child started";
	const std::string script = "printf started > \"$1\"; exec sleep 30";
	std::atomic<bool> keepRunning{true};
	auto operation = std::async(std::launch::async, [&] {
		std::string error;
		const bool succeeded = jpegview_linux::RunExternalCommand(
			{"sh", {"-c", script, "test-child", marker.string()}}, error,
			[&] { return keepRunning.load(std::memory_order_acquire); });
		return std::make_pair(succeeded, error);
	});
	const bool childStarted = WaitForPath(marker, std::chrono::seconds(2));
	keepRunning.store(false, std::memory_order_release);
	Expect(operation.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
		"cancellation did not reap a blocked child before the deadline");
	const auto result = operation.get();
	Expect(childStarted && !result.first && result.second.find("cancelled") != std::string::npos,
		"slow child cancellation did not publish its canceled result");
}

void TestExternalProcessCancellationKillsDescendants() {
	TemporaryDirectory temporary;
	const fs::path script = temporary.path() / "spawn descendant";
	const fs::path ready = temporary.path() / "descendant started";
	const fs::path descendantPidFile = temporary.path() / "descendant pid";
	WriteText(script,
		"#!/bin/sh\n"
		"(trap '' TERM; while :; do sleep 1; done) &\n"
		"printf '%s' \"$!\" > \"$JPEGVIEW_TEST_GROUP_CHILD_PID\"\n"
		"printf ready > \"$JPEGVIEW_TEST_GROUP_READY\"\n"
		"wait\n");
	MakeTestExecutable(script);
	ScopedEnvironment childPidEnvironment("JPEGVIEW_TEST_GROUP_CHILD_PID",
		descendantPidFile.string());
	ScopedEnvironment readyEnvironment("JPEGVIEW_TEST_GROUP_READY", ready.string());
	std::atomic<bool> keepRunning{true};
	auto operation = std::async(std::launch::async, [&] {
		std::string error;
		const bool succeeded = jpegview_linux::RunExternalCommand(
			{script.string(), {}}, error,
			[&] { return keepRunning.load(std::memory_order_acquire); });
		return std::make_pair(succeeded, error);
	});
	const bool childStarted = WaitForPath(ready, std::chrono::seconds(2));
	long descendantPidValue = -1;
	{
		std::ifstream input(descendantPidFile);
		input >> descendantPidValue;
	}
	const pid_t descendantPid = static_cast<pid_t>(descendantPidValue);
	const pid_t processGroup = descendantPid > 0 ? ::getpgid(descendantPid) : -1;
	keepRunning.store(false, std::memory_order_release);
	const bool completed = operation.wait_for(std::chrono::seconds(2)) ==
		std::future_status::ready;
	std::pair<bool, std::string> result{false, "child operation did not finish"};
	if (completed) result = operation.get();
	const auto descendantStopped = [descendantPid] {
		if (descendantPid <= 0) return false;
		std::ifstream statusFile("/proc/" + std::to_string(descendantPid) + "/stat");
		std::string status;
		if (!statusFile || !std::getline(statusFile, status)) return true;
		const std::size_t commandEnd = status.rfind(')');
		if (commandEnd == std::string::npos || commandEnd + 2 >= status.size()) return false;
		return status[commandEnd + 2] == 'Z' || status[commandEnd + 2] == 'X';
	};
	const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!descendantStopped() && std::chrono::steady_clock::now() < stopDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	const bool descendantWasStopped = descendantStopped();
	if (processGroup > 0) (void)::kill(-processGroup, SIGKILL);
	Expect(childStarted && completed && !result.first &&
		result.second.find("cancelled") != std::string::npos && descendantWasStopped,
		"process-group cancellation left a spawned descendant running");
}

void TestFileOperationSaveAndBatchPolicies() {
	TemporaryDirectory temporary;
	const std::shared_ptr<const jpegview_linux::Image> image = MakeTinyTestImage();
	const fs::path output = temporary.path() / "processed image.png";
	WriteText(output, "existing output");
	jpegview_linux::SaveImageOperation rejectedSave;
	rejectedSave.output = output;
	rejectedSave.image = image;
	jpegview_linux::FileOperationPayload rejectedPayload{std::move(rejectedSave)};
	auto alwaysContinue = [] { return true; };
	const auto rejected = jpegview_linux::ExecuteFileOperation(rejectedPayload, 4,
		alwaysContinue);
	Expect(!rejected.success && !rejected.cancelled &&
		ReadBytes(output) == std::vector<std::uint8_t>({'e', 'x', 'i', 's', 't', 'i', 'n', 'g',
			' ', 'o', 'u', 't', 'p', 'u', 't'}),
		"unconfirmed image overwrite changed the existing output");

	jpegview_linux::SaveImageOperation confirmedSave;
	confirmedSave.output = output;
	confirmedSave.selectedSourcePath = output;
	confirmedSave.image = image;
	confirmedSave.overwriteConfirmed = true;
	Expect(::chmod(output.c_str(), 0604) == 0,
		"cannot set output mode for the confirmed-save fixture");
	jpegview_linux::FileOperationPayload confirmedPayload{std::move(confirmedSave)};
	const auto saved = jpegview_linux::ExecuteFileOperation(confirmedPayload, 5,
		alwaysContinue);
	const std::vector<std::uint8_t> savedBytes = ReadBytes(output);
	struct stat savedStatus{};
	Expect(saved.success && saved.replacedSelectedSource && saved.path == output &&
		savedBytes.size() > 8 &&
		savedBytes[0] == 0x89 && savedBytes[1] == 'P' && savedBytes[2] == 'N' &&
		savedBytes[3] == 'G' && ::stat(output.c_str(), &savedStatus) == 0 &&
		(savedStatus.st_mode & 0777) == 0604,
		"confirmed image save did not atomically replace the target and preserve its mode");

	const fs::path symlinkTarget = temporary.path() / "symlink target.png";
	const fs::path symlinkOutput = temporary.path() / "symlink output.png";
	WriteText(symlinkTarget, "prior symlink target");
	fs::create_symlink(symlinkTarget.filename(), symlinkOutput);
	jpegview_linux::SaveImageOperation symlinkSave;
	symlinkSave.output = symlinkOutput;
	symlinkSave.selectedSourcePath = symlinkTarget;
	symlinkSave.image = image;
	symlinkSave.overwriteConfirmed = true;
	const auto symlinkSaved = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(symlinkSave)}, 5, alwaysContinue);
	Expect(symlinkSaved.success && symlinkSaved.replacedSelectedSource &&
		fs::is_symlink(symlinkOutput) &&
		ReadBytes(symlinkTarget).size() > 8 && ReadBytes(symlinkTarget)[0] == 0x89,
		"atomic image save replaced the output symlink instead of updating its target");

	const fs::path hardLinkOutput = temporary.path() / "hard-link output.png";
	const std::vector<std::uint8_t> beforeHardLinkSave = ReadBytes(symlinkTarget);
	fs::create_hard_link(symlinkTarget, hardLinkOutput);
	jpegview_linux::SaveImageOperation hardLinkSave;
	hardLinkSave.output = hardLinkOutput;
	hardLinkSave.selectedSourcePath = symlinkTarget;
	hardLinkSave.image = image;
	hardLinkSave.overwriteConfirmed = true;
	const auto hardLinkSaved = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(hardLinkSave)}, 5, alwaysContinue);
	Expect(hardLinkSaved.success && !hardLinkSaved.replacedSelectedSource &&
		ReadBytes(symlinkTarget) == beforeHardLinkSave,
		"replacing a separate hard-link name was reported as replacing the selected path");

	const fs::path danglingTarget = temporary.path() / "dangling target.png";
	const fs::path danglingOutput = temporary.path() / "dangling output.png";
	fs::create_symlink(danglingTarget.filename(), danglingOutput);
	jpegview_linux::SaveImageOperation danglingSave;
	danglingSave.output = danglingOutput;
	danglingSave.image = image;
	const auto danglingSaved = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(danglingSave)}, 5, alwaysContinue);
	Expect(danglingSaved.success && fs::is_symlink(danglingOutput) &&
		fs::exists(danglingTarget) && ReadBytes(danglingTarget).size() > 8,
		"atomic image save did not preserve a dangling symlink output");

	const fs::path defaultModeProbe = temporary.path() / "save mode probe";
	const int modeProbeDescriptor = ::open(defaultModeProbe.c_str(),
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	struct stat modeProbeStatus{};
	Expect(modeProbeDescriptor >= 0 &&
		::fstat(modeProbeDescriptor, &modeProbeStatus) == 0,
		"cannot determine the process default mode for a new image save");
	if (modeProbeDescriptor >= 0) (void)::close(modeProbeDescriptor);
	std::error_code modeProbeError;
	fs::remove(defaultModeProbe, modeProbeError);
	Expect(!modeProbeError, "cannot remove image-save mode probe");
	const fs::path newOutput = temporary.path() / "new output.png";
	jpegview_linux::SaveImageOperation newSave;
	newSave.output = newOutput;
	newSave.image = image;
	const auto newlySaved = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(newSave)}, 5, alwaysContinue);
	struct stat newOutputStatus{};
	Expect(newlySaved.success && ::stat(newOutput.c_str(), &newOutputStatus) == 0 &&
		(newOutputStatus.st_mode & 0777) == (modeProbeStatus.st_mode & 0777),
		"new image save did not preserve the normal umask-derived output mode");

	const fs::path failedOutput = temporary.path() / "failed output.png";
	WriteText(failedOutput, "keep on encoder failure");
	auto invalidImage = std::make_shared<jpegview_linux::Image>();
	invalidImage->width = 2;
	invalidImage->height = 2;
	jpegview_linux::SaveImageOperation failedSave;
	failedSave.output = failedOutput;
	failedSave.image = std::move(invalidImage);
	failedSave.overwriteConfirmed = true;
	const auto failedAtomicSave = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(failedSave)}, 5, alwaysContinue);
	Expect(!failedAtomicSave.success && ReadBytes(failedOutput) ==
		std::vector<std::uint8_t>({'k','e','e','p',' ','o','n',' ','e','n','c','o','d','e','r',' ','f','a','i','l','u','r','e'}),
		"failed image encoding damaged the existing output");
	std::error_code saveIteratorError;
	for (const fs::directory_entry& entry : fs::directory_iterator(temporary.path(),
		saveIteratorError)) {
		Expect(entry.path() == output || entry.path() == symlinkTarget ||
			entry.path() == symlinkOutput || entry.path() == hardLinkOutput ||
			entry.path() == danglingTarget ||
			entry.path() == danglingOutput || entry.path() == newOutput ||
			entry.path() == failedOutput,
			"image save left a temporary output beside its destination");
	}
	Expect(!saveIteratorError, "cannot inspect image-save temporary cleanup");

	const auto invalidTransform = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{
			jpegview_linux::LosslessTransformOperation{output,
				jpegview_linux::LosslessJpegOperation::Rotate90}}, 5, alwaysContinue);
	Expect(!invalidTransform.success && !invalidTransform.cancelled &&
		ReadBytes(output) == savedBytes,
		"lossless JPEG service accepted a non-JPEG source or changed its contents");

	const fs::path sources = temporary.path() / "source files";
	fs::create_directories(sources);
	const fs::path first = sources / "first.jpg";
	const fs::path second = sources / "second.jpg";
	const fs::path third = sources / "third.jpg";
	const fs::path occupied = sources / "occupied.jpg";
	const fs::path renamed = sources / "renamed.jpg";
	const fs::path copied = sources / "new folder" / "third-copy.jpg";
	WriteText(first, "first");
	WriteText(second, "second");
	WriteText(third, "third");
	WriteText(occupied, "keep occupied target");
	Expect(::chmod(third.c_str(), 0640) == 0,
		"cannot set source permissions for the batch-copy fixture");
	std::error_code modificationTimeError;
	fs::last_write_time(third, fs::file_time_type::clock::now() - std::chrono::hours(24),
		modificationTimeError);
	Expect(!modificationTimeError,
		"cannot set source modification time for the batch-copy fixture");
	const auto thirdModificationTime = fs::last_write_time(third);
	jpegview_linux::BatchCopyOperation batch;
	batch.preferredCurrentPath = second;
	auto item = [](const fs::path& source, const fs::path& destination, bool copy) {
		jpegview_linux::BatchCopyItem result;
		result.source = source;
		result.destination = destination;
		result.copy = copy;
		result.selected = true;
		return result;
	};
	batch.items.push_back(item(first, occupied, false));
	batch.items.push_back(item(second, renamed, false));
	batch.items.push_back(item(third, copied, true));
	batch.items.push_back(item(sources / "missing.jpg",
		sources / "new folder" / "missing-copy.jpg", true));
	jpegview_linux::FileOperationPayload batchPayload{std::move(batch)};
	const auto partial = jpegview_linux::ExecuteFileOperation(batchPayload, 6,
		alwaysContinue);
	Expect(partial.success && partial.batch.failed == 2 && partial.batch.completed == 2 &&
		partial.batch.renamed == 1 && partial.batch.copied == 1 &&
		partial.batch.createdDirectories == 1 &&
		partial.batch.preferredCurrentPath == renamed,
		"batch operation did not report its partial failure and completed items");
	Expect(fs::exists(first) && !fs::exists(second) && fs::exists(renamed) &&
		fs::exists(third) && fs::exists(copied) && ReadBytes(occupied) ==
			std::vector<std::uint8_t>({'k','e','e','p',' ','o','c','c','u','p','i','e','d',' ','t','a','r','g','e','t'}),
		"batch operation overwrote a target or rolled back completed filesystem work");
	struct stat copiedStatus{};
	Expect(::stat(copied.c_str(), &copiedStatus) == 0 &&
		(copiedStatus.st_mode & 0777) == 0640,
		"atomic batch copy did not preserve source permissions");
	Expect(fs::last_write_time(copied) == thirdModificationTime,
		"atomic batch copy did not preserve the source modification time");
	std::error_code iteratorError;
	for (const fs::directory_entry& entry : fs::directory_iterator(copied.parent_path(),
		iteratorError)) {
		Expect(entry.path() == copied,
			"batch copy left a temporary or partial image in the destination directory");
	}
	Expect(!iteratorError, "cannot inspect the batch-copy destination directory");

	struct RenameHookState {
		bool forceLinkFallback = false;
		bool createLateDestination = false;
		std::string lateDestinationContents;
	};
	auto renameHook = [](const fs::path&, const fs::path& destination,
		bool* forceLinkFallback, void* opaque) {
		auto& state = *static_cast<RenameHookState*>(opaque);
		if (forceLinkFallback != nullptr) {
			*forceLinkFallback = state.forceLinkFallback;
		}
		if (state.createLateDestination) {
			WriteText(destination, state.lateDestinationContents);
			state.createLateDestination = false;
		}
	};

	const fs::path fallbackTarget = temporary.path() / "fallback symlink target.jpg";
	const fs::path fallbackSource = temporary.path() / "fallback source.jpg";
	const fs::path fallbackDestination = temporary.path() / "fallback destination.jpg";
	WriteText(fallbackTarget, "symlink target bytes");
	fs::create_symlink(fallbackTarget.filename(), fallbackSource);
	RenameHookState fallbackState;
	fallbackState.forceLinkFallback = true;
	jpegview_linux::SetBatchRenameBeforeMoveTestHookForTesting(renameHook,
		&fallbackState);
	jpegview_linux::BatchCopyOperation fallbackBatch;
	fallbackBatch.items.push_back(item(fallbackSource, fallbackDestination, false));
	const auto fallbackRename = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(fallbackBatch)}, 6,
		alwaysContinue);
	jpegview_linux::SetBatchRenameBeforeMoveTestHookForTesting(nullptr, nullptr);
	Expect(fallbackRename.batch.renamed == 1 && fallbackRename.batch.failed == 0 &&
		!fs::exists(fallbackSource) && fs::is_symlink(fallbackDestination) &&
		fs::read_symlink(fallbackDestination) == fallbackTarget.filename() &&
		ReadBytes(fallbackTarget) == std::vector<std::uint8_t>({
			's', 'y', 'm', 'l', 'i', 'n', 'k', ' ', 't', 'a', 'r', 'g', 'e', 't',
			' ', 'b', 'y', 't', 'e', 's'}),
		"no-replace fallback did not move a symlink itself while preserving its target");

	const fs::path lateSource = temporary.path() / "late source.jpg";
	const fs::path lateDestination = temporary.path() / "late destination.jpg";
	WriteText(lateSource, "source remains");
	RenameHookState lateTargetState;
	lateTargetState.createLateDestination = true;
	lateTargetState.lateDestinationContents = "late target remains";
	jpegview_linux::SetBatchRenameBeforeMoveTestHookForTesting(renameHook,
		&lateTargetState);
	jpegview_linux::BatchCopyOperation lateTargetBatch;
	lateTargetBatch.items.push_back(item(lateSource, lateDestination, false));
	const auto lateTargetRename = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(lateTargetBatch)}, 6,
		alwaysContinue);
	jpegview_linux::SetBatchRenameBeforeMoveTestHookForTesting(nullptr, nullptr);
	Expect(lateTargetRename.batch.renamed == 0 && lateTargetRename.batch.failed == 1 &&
		lateTargetRename.batch.completed == 0 && fs::exists(lateSource) &&
		ReadBytes(lateSource) == std::vector<std::uint8_t>({
			's', 'o', 'u', 'r', 'c', 'e', ' ', 'r', 'e', 'm', 'a', 'i', 'n', 's'}) &&
		ReadBytes(lateDestination) == std::vector<std::uint8_t>({
			'l', 'a', 't', 'e', ' ', 't', 'a', 'r', 'g', 'e', 't', ' ', 'r', 'e', 'm', 'a', 'i', 'n', 's'}),
		"batch rename replaced a target created after the initial existence check");

	const fs::path fallbackLateSource = temporary.path() / "fallback late source.jpg";
	const fs::path fallbackLateDestination = temporary.path() / "fallback late destination.jpg";
	WriteText(fallbackLateSource, "fallback source remains");
	RenameHookState fallbackLateState;
	fallbackLateState.forceLinkFallback = true;
	fallbackLateState.createLateDestination = true;
	fallbackLateState.lateDestinationContents = "fallback late target remains";
	jpegview_linux::SetBatchRenameBeforeMoveTestHookForTesting(renameHook,
		&fallbackLateState);
	jpegview_linux::BatchCopyOperation fallbackLateBatch;
	fallbackLateBatch.items.push_back(item(fallbackLateSource,
		fallbackLateDestination, false));
	const auto fallbackLateRename = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(fallbackLateBatch)}, 6,
		alwaysContinue);
	jpegview_linux::SetBatchRenameBeforeMoveTestHookForTesting(nullptr, nullptr);
	Expect(fallbackLateRename.batch.renamed == 0 &&
		fallbackLateRename.batch.failed == 1 && fs::exists(fallbackLateSource) &&
		ReadBytes(fallbackLateSource) == std::vector<std::uint8_t>({
			'f', 'a', 'l', 'l', 'b', 'a', 'c', 'k', ' ', 's', 'o', 'u', 'r', 'c', 'e',
			' ', 'r', 'e', 'm', 'a', 'i', 'n', 's'}) &&
		ReadBytes(fallbackLateDestination) == std::vector<std::uint8_t>({
			'f', 'a', 'l', 'l', 'b', 'a', 'c', 'k', ' ', 'l', 'a', 't', 'e', ' ', 't',
			'a', 'r', 'g', 'e', 't', ' ', 'r', 'e', 'm', 'a', 'i', 'n', 's'}),
		"hard-link fallback replaced a destination created after the existence check");
}

void TestBatchCancellationStopsBetweenFiles() {
	TemporaryDirectory temporary;
	const fs::path first = temporary.path() / "first.jpg";
	const fs::path second = temporary.path() / "second.jpg";
	const fs::path renamed = temporary.path() / "first-renamed.jpg";
	const fs::path secondRenamed = temporary.path() / "second-renamed.jpg";
	WriteText(first, "first");
	WriteText(second, "second");
	jpegview_linux::BatchCopyOperation batch;
	batch.preferredCurrentPath = first;
	for (const auto& pair : {std::make_pair(first, renamed),
		std::make_pair(second, secondRenamed)}) {
		jpegview_linux::BatchCopyItem item;
		item.source = pair.first;
		item.destination = pair.second;
		item.selected = true;
		batch.items.push_back(std::move(item));
	}
	jpegview_linux::FileOperationPayload payload{std::move(batch)};
	const auto result = jpegview_linux::ExecuteFileOperation(payload, 7, [&] {
		return !fs::exists(renamed);
	});
	Expect(result.cancelled && !result.success && result.batch.completed == 1 &&
		result.batch.renamed == 1 && fs::exists(renamed) && fs::exists(second) &&
		!fs::exists(secondRenamed),
		"batch cancellation did not stop between files while preserving completed work");
}

void TestBatchCopyTemporaryRemainsPrivateUntilPublication() {
	TemporaryDirectory temporary;
	const fs::path directory = temporary.path() / "copy-private";
	fs::create_directories(directory);
	const fs::path source = directory / "source.jpg";
	const fs::path destination = directory / "copy.jpg";
	std::vector<std::uint8_t> contents(2u * 1024u * 1024u, 0x5a);
	WriteBytes(source, contents);
	Expect(::chmod(source.c_str(), 0644) == 0,
		"cannot set source permissions for private-copy test");

	struct CopyPauseState {
		std::mutex mutex;
		std::condition_variable changed;
		bool paused = false;
		bool release = false;
		std::size_t copiedBytes = 0;
		fs::path temporary;
	};
	CopyPauseState pause;
	auto pauseAfterChunk = [](const fs::path& temporaryPath,
		std::size_t copiedBytes, void* opaque) {
		auto& state = *static_cast<CopyPauseState*>(opaque);
		std::unique_lock<std::mutex> lock(state.mutex);
		if (state.paused) return;
		state.temporary = temporaryPath;
		state.copiedBytes = copiedBytes;
		state.paused = true;
		state.changed.notify_all();
		state.changed.wait(lock, [&state] { return state.release; });
	};
	jpegview_linux::BatchCopyOperation batch;
	jpegview_linux::BatchCopyItem copy;
	copy.source = source;
	copy.destination = destination;
	copy.selected = true;
	copy.copy = true;
	batch.items.push_back(copy);
	jpegview_linux::SetBatchCopyChunkTestHookForTesting(pauseAfterChunk, &pause);
	auto operation = std::async(std::launch::async, [batch = std::move(batch)]() mutable {
		return jpegview_linux::ExecuteFileOperation(
			jpegview_linux::FileOperationPayload{std::move(batch)}, 1,
			[] { return true; });
	});
	bool reachedCopyBarrier = false;
	std::size_t bytesAtBarrier = 0;
	fs::path temporaryAtBarrier;
	{
		std::unique_lock<std::mutex> lock(pause.mutex);
		reachedCopyBarrier = pause.changed.wait_for(lock, std::chrono::seconds(2),
			[&pause] { return pause.paused; });
		if (reachedCopyBarrier) {
			bytesAtBarrier = pause.copiedBytes;
			temporaryAtBarrier = pause.temporary;
		}
	}
	struct stat intermediateStatus{};
	const bool privateDuringCopy = reachedCopyBarrier && bytesAtBarrier > 0 &&
		bytesAtBarrier < contents.size() &&
		::stat(temporaryAtBarrier.c_str(), &intermediateStatus) == 0 &&
		static_cast<std::size_t>(intermediateStatus.st_size) == bytesAtBarrier &&
		(intermediateStatus.st_mode & 0777) == 0600;
	{
		std::lock_guard<std::mutex> lock(pause.mutex);
		pause.release = true;
	}
	pause.changed.notify_all();
	const bool operationCompleted = operation.wait_for(std::chrono::seconds(3)) ==
		std::future_status::ready;
	jpegview_linux::SetBatchCopyChunkTestHookForTesting(nullptr, nullptr);
	if (!operationCompleted) {
		Expect(false, "batch copy did not finish after its test barrier was released");
	}
	const auto copied = operation.get();
	struct stat destinationStatus{};
	Expect(privateDuringCopy && copied.success && copied.batch.copied == 1 &&
		ReadBytes(destination) == contents &&
		::stat(destination.c_str(), &destinationStatus) == 0 &&
		(destinationStatus.st_mode & 0777) == 0644,
		"batch-copy contents became public before transfer completed or metadata was lost");
	std::error_code iteratorError;
	for (const fs::directory_entry& entry : fs::directory_iterator(directory,
		iteratorError)) {
		Expect(entry.path() == source || entry.path() == destination,
			"completed batch copy left a temporary sibling behind");
	}
	Expect(!iteratorError, "cannot inspect completed batch-copy directory");

	const fs::path lateDirectory = temporary.path() / "copy-late-target";
	fs::create_directories(lateDirectory);
	const fs::path lateSource = lateDirectory / "source.jpg";
	const fs::path lateDestination = lateDirectory / "copy.jpg";
	WriteBytes(lateSource, {1, 2, 3, 4});
	std::string lateContents = "late target remains";
	auto createLateCopyTarget = [](const fs::path& target, void* opaque) {
		WriteText(target, *static_cast<const std::string*>(opaque));
	};
	jpegview_linux::BatchCopyItem lateCopy;
	lateCopy.source = lateSource;
	lateCopy.destination = lateDestination;
	lateCopy.selected = true;
	lateCopy.copy = true;
	jpegview_linux::BatchCopyOperation lateBatch;
	lateBatch.items.push_back(lateCopy);
	jpegview_linux::SetBatchCopyBeforePublishTestHookForTesting(
		createLateCopyTarget, &lateContents);
	const auto lateCopyResult = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(lateBatch)}, 2,
		[] { return true; });
	jpegview_linux::SetBatchCopyBeforePublishTestHookForTesting(nullptr, nullptr);
	Expect(lateCopyResult.batch.copied == 0 && lateCopyResult.batch.failed == 1 &&
		lateCopyResult.batch.completed == 0 && ReadBytes(lateSource) ==
		std::vector<std::uint8_t>({1, 2, 3, 4}) &&
		ReadBytes(lateDestination) == std::vector<std::uint8_t>({
			'l', 'a', 't', 'e', ' ', 't', 'a', 'r', 'g', 'e', 't', ' ', 'r', 'e', 'm', 'a', 'i', 'n', 's'}),
		"batch-copy publication replaced a destination created after its check");
	iteratorError.clear();
	for (const fs::directory_entry& entry : fs::directory_iterator(lateDirectory,
		iteratorError)) {
		Expect(entry.path() == lateSource || entry.path() == lateDestination,
			"late-target batch-copy failure left a temporary sibling behind");
	}
	Expect(!iteratorError, "cannot inspect late-target batch-copy directory");

	const fs::path canceledDirectory = temporary.path() / "copy-canceled";
	fs::create_directories(canceledDirectory);
	const fs::path canceledSource = canceledDirectory / "source.jpg";
	const fs::path canceledDestination = canceledDirectory / "copy.jpg";
	WriteBytes(canceledSource, contents);
	std::atomic<bool> keepCopying{true};
	const auto cancelAfterChunk = [](const fs::path&, std::size_t, void* opaque) {
		static_cast<std::atomic<bool>*>(opaque)->store(false, std::memory_order_release);
	};
	jpegview_linux::BatchCopyOperation canceledBatch;
	jpegview_linux::BatchCopyItem canceledItem;
	canceledItem.source = canceledSource;
	canceledItem.destination = canceledDestination;
	canceledItem.selected = true;
	canceledItem.copy = true;
	canceledBatch.items.push_back(canceledItem);
	jpegview_linux::SetBatchCopyChunkTestHookForTesting(cancelAfterChunk,
		&keepCopying);
	const auto canceled = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(canceledBatch)}, 2,
		[&keepCopying] {
			return keepCopying.load(std::memory_order_acquire);
		});
	jpegview_linux::SetBatchCopyChunkTestHookForTesting(nullptr, nullptr);
	Expect(canceled.cancelled && !canceled.success && canceled.batch.completed == 0 &&
		fs::exists(canceledSource) && !fs::exists(canceledDestination) &&
		ReadBytes(canceledSource) == contents,
		"canceled batch copy published partial data or changed its source");
	iteratorError.clear();
	for (const fs::directory_entry& entry : fs::directory_iterator(canceledDirectory,
		iteratorError)) {
		Expect(entry.path() == canceledSource,
			"canceled batch copy left its private temporary sibling behind");
	}
	Expect(!iteratorError, "cannot inspect canceled batch-copy directory");
}

void TestFileOperationClipboardTemporaryCleanupAndFallback() {
	TemporaryDirectory temporary;
	const fs::path bin = temporary.path() / "bin";
	fs::create_directories(bin);
	const fs::path capturedClipboard = temporary.path() / "captured clipboard.png";
	const fs::path inputClipboard = temporary.path() / "paste source.png";
	const fs::path copier = bin / "wl-copy";
	const fs::path paster = bin / "wl-paste";
	WriteText(copier, "#!/bin/sh\ncat > \"$JPEGVIEW_TEST_CLIPBOARD_CAPTURE\"\n");
	WriteText(paster, "#!/bin/sh\ncat \"$JPEGVIEW_TEST_CLIPBOARD_INPUT\"\n");
	MakeTestExecutable(copier);
	MakeTestExecutable(paster);
	ScopedEnvironment path("PATH", bin.string() + ":/usr/bin:/bin");
	ScopedEnvironment wayland("WAYLAND_DISPLAY", "jpegview-test-wayland");
	ScopedEnvironment capture("JPEGVIEW_TEST_CLIPBOARD_CAPTURE", capturedClipboard.string());
	ScopedEnvironment input("JPEGVIEW_TEST_CLIPBOARD_INPUT", inputClipboard.string());
	const std::shared_ptr<const jpegview_linux::Image> image = MakeTinyTestImage();
	jpegview_linux::CopyImageOperation copy;
	copy.image = image;
	jpegview_linux::FileOperationPayload copyPayload{std::move(copy)};
	const auto copied = jpegview_linux::ExecuteFileOperation(copyPayload, 8,
		[] { return true; });
	Expect(copied.success && copied.detachedChild > 0,
		"clipboard copy did not preserve the external clipboard owner process");
	int childStatus = 0;
	Expect(WaitForChildExit(static_cast<pid_t>(copied.detachedChild),
		std::chrono::seconds(2), childStatus) && WIFEXITED(childStatus) &&
		WEXITSTATUS(childStatus) == 0,
		"clipboard helper did not finish after consuming its encoded PNG");
	const std::vector<std::uint8_t> capturedBytes = ReadBytes(capturedClipboard);
	Expect(capturedBytes.size() > 8 && capturedBytes[0] == 0x89 &&
		capturedBytes[1] == 'P' && capturedBytes[2] == 'N' && capturedBytes[3] == 'G',
		"clipboard helper did not receive a complete PNG");
	jpegview_linux::ImageWriteOptions options;
	std::string writeError;
	Expect(jpegview_linux::WriteImage(inputClipboard, image->bgra.data(), image->width,
		image->height, options, writeError), "cannot create PNG paste fixture: " + writeError);
	const std::vector<std::uint8_t> expected = ReadBytes(inputClipboard);
	const auto pasted = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{jpegview_linux::PasteImageOperation{}}, 9,
		[] { return true; });
	Expect(pasted.success && fs::exists(pasted.temporaryFile) &&
		ReadBytes(pasted.temporaryFile) == expected,
		"clipboard paste did not preserve its asynchronous temporary image");
	const jpegview_linux::WorkContext activeReadContext =
		jpegview_linux::MakePathWorkContext(pasted.temporaryFile,
			jpegview_linux::SourceWorkPriority::Foreground, [] { return true; });
	jpegview_linux::SourceWorkLease activeRead =
		jpegview_linux::SourceWorkCoordinator::Global().Acquire(
			activeReadContext, pasted.temporaryFile);
	Expect(static_cast<bool>(activeRead),
		"cannot acquire the source lease for temporary image cleanup coverage");
	jpegview_linux::FileOperationService cleanupService;
	const std::uint64_t cleanupId = cleanupService.Request(
		jpegview_linux::FileOperationPayload{
			jpegview_linux::RemoveTemporaryFilesOperation{
				pasted.temporaryFile, pasted.temporaryDirectory}}, 10);
	const bool cleanupWaiting = jpegview_linux::SourceWorkCoordinator::Global().WaitForSnapshot(
		[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
			return snapshot.activeForeground == 1 && snapshot.waitingForeground == 1;
		}, std::chrono::seconds(2));
	Expect(cleanupId != 0 && cleanupWaiting &&
		fs::exists(pasted.temporaryFile),
		"temporary cleanup raced a reader that still owned the source path");
	activeRead.Reset();
	Expect(cleanupService.WaitUntilIdle(std::chrono::seconds(2)),
		"temporary cleanup did not resume after source admission was released");
	const auto cleaned = cleanupService.TakeReady();
	Expect(cleaned.has_value() && cleaned->success &&
		!fs::exists(pasted.temporaryFile) &&
		!fs::exists(pasted.temporaryDirectory),
		"clipboard temporary image and directory were not removed");
	cleanupService.Stop();

	jpegview_linux::LaunchDesktopOperation launch;
	launch.fallbacks.push_back({(bin / "missing opener").string(), {}});
	launch.fallbacks.push_back({"/bin/true", {}});
	const auto launched = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{std::move(launch)}, 11, [] { return true; });
	Expect(launched.success && launched.detachedChild > 0,
		"desktop launch did not fall back from the missing first executable");
	Expect(WaitForChildExit(static_cast<pid_t>(launched.detachedChild),
		std::chrono::seconds(2), childStatus) && WIFEXITED(childStatus) &&
		WEXITSTATUS(childStatus) == 0,
		"fallback desktop command did not complete successfully");
}

void TestLosslessOperationsPublishOnlySuccessfulTemporaryOutputs() {
	TemporaryDirectory temporary;
	const fs::path bin = temporary.path() / "bin";
	const fs::path images = temporary.path() / "images with spaces";
	fs::create_directories(bin);
	fs::create_directories(images);
	const fs::path transform = bin / "jpegtran";
	WriteText(transform,
		"#!/bin/sh\n"
		"output=\n"
		"while [ \"$#\" -gt 0 ]; do\n"
		"  if [ \"$1\" = \"-outfile\" ]; then shift; output=$1; break; fi\n"
		"  shift\n"
		"done\n"
		"[ -n \"$output\" ] || exit 2\n"
		"printf transformed > \"$output\" || exit 3\n"
		"[ \"${JPEGVIEW_TEST_JPEGTRAN_FAIL:-0}\" = 1 ] && exit 9\n"
		"exit 0\n");
	MakeTestExecutable(transform);
	ScopedEnvironment path("PATH", bin.string() + ":/usr/bin:/bin");
	ScopedEnvironment fail("JPEGVIEW_TEST_JPEGTRAN_FAIL", "1");
	const fs::path source = images / "source image.jpg";
	const fs::path crop = images / "crop output.jpg";
	WriteText(source, "original source bytes");
	Expect(::chmod(source.c_str(), 0640) == 0,
		"cannot set source mode for lossless transform fixture");

	const auto failedTransform = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{
			jpegview_linux::LosslessTransformOperation{source,
				jpegview_linux::LosslessJpegOperation::Rotate90}}, 41,
		[] { return true; });
	Expect(!failedTransform.success && ReadBytes(source) ==
		std::vector<std::uint8_t>({'o','r','i','g','i','n','a','l',' ','s','o','u','r','c','e',' ','b','y','t','e','s'}),
		"failed lossless transform replaced the original source");
	std::error_code iteratorError;
	for (const fs::directory_entry& entry : fs::directory_iterator(images, iteratorError)) {
		Expect(entry.path() == source,
			"failed lossless transform left its temporary output behind");
	}
	Expect(!iteratorError, "cannot inspect failed lossless transform cleanup");

	fail.Clear();
	const auto transformed = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{
			jpegview_linux::LosslessTransformOperation{source,
				jpegview_linux::LosslessJpegOperation::Rotate90}}, 42,
		[] { return true; });
	struct stat sourceStatus{};
	Expect(transformed.success && transformed.path == source &&
		ReadBytes(source) == std::vector<std::uint8_t>({'t','r','a','n','s','f','o','r','m','e','d'}) &&
		::stat(source.c_str(), &sourceStatus) == 0 && (sourceStatus.st_mode & 0777) == 0640,
		"successful lossless transform did not atomically publish pixels with source permissions");

	WriteText(crop, "existing crop bytes");
	Expect(::chmod(crop.c_str(), 0604) == 0,
		"cannot set destination mode for lossless crop fixture");
	jpegview_linux::LosslessCropOperation cropOperation;
	cropOperation.source = source;
	cropOperation.output = crop;
	cropOperation.width = 1;
	cropOperation.height = 1;
	cropOperation.overwriteConfirmed = true;
	Expect(::setenv("JPEGVIEW_TEST_JPEGTRAN_FAIL", "1", 1) == 0,
		"cannot reactivate lossless crop failure fixture");
	const auto failedCrop = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{cropOperation}, 43, [] { return true; });
	Expect(!failedCrop.success && ReadBytes(crop) ==
		std::vector<std::uint8_t>({'e','x','i','s','t','i','n','g',' ','c','r','o','p',' ','b','y','t','e','s'}),
		"failed lossless crop replaced its confirmed destination");
	iteratorError.clear();
	for (const fs::directory_entry& entry : fs::directory_iterator(images, iteratorError)) {
		Expect(entry.path() == source || entry.path() == crop,
			"failed lossless crop left its temporary output behind");
	}
	Expect(!iteratorError, "cannot inspect failed lossless crop cleanup");

	fail.Clear();
	const auto cropped = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{cropOperation}, 44, [] { return true; });
	struct stat cropStatus{};
	Expect(cropped.success && cropped.path == crop &&
		ReadBytes(crop) == std::vector<std::uint8_t>({'t','r','a','n','s','f','o','r','m','e','d'}) &&
		::stat(crop.c_str(), &cropStatus) == 0 && (cropStatus.st_mode & 0777) == 0604,
		"successful lossless crop did not publish its temporary result and prior destination mode");
	const fs::path modeProbe = images / "mode probe";
	const int modeProbeDescriptor = ::open(modeProbe.c_str(),
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	struct stat modeProbeStatus{};
	Expect(modeProbeDescriptor >= 0 &&
		::fstat(modeProbeDescriptor, &modeProbeStatus) == 0,
		"cannot determine the process default file mode for new lossless output");
	if (modeProbeDescriptor >= 0) (void)::close(modeProbeDescriptor);
	std::error_code removeModeProbeError;
	fs::remove(modeProbe, removeModeProbeError);
	Expect(!removeModeProbeError, "cannot remove lossless output mode probe");
	const fs::path newCrop = images / "new crop.jpg";
	cropOperation.output = newCrop;
	cropOperation.overwriteConfirmed = false;
	const auto newCropped = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{cropOperation}, 45, [] { return true; });
	struct stat newCropStatus{};
	Expect(newCropped.success && ::stat(newCrop.c_str(), &newCropStatus) == 0 &&
		(newCropStatus.st_mode & 0777) == (modeProbeStatus.st_mode & 0777),
		"new lossless crop did not preserve the process default output permissions");
}

void TestFileOperationServiceStopCancelsChildWithoutEventLoop() {
	TemporaryDirectory temporary;
	const fs::path bin = temporary.path() / "bin";
	const fs::path images = temporary.path() / "images";
	fs::create_directories(bin);
	fs::create_directories(images);
	const fs::path marker = temporary.path() / "jpegtran started";
	const fs::path transform = bin / "jpegtran";
	WriteText(transform, "#!/bin/sh\nprintf started > \"$JPEGVIEW_TEST_CHILD_MARKER\"\nexec sleep 30\n");
	MakeTestExecutable(transform);
	const fs::path source = images / "source.jpg";
	WriteText(source, "original jpeg fixture");
	ScopedEnvironment path("PATH", bin.string() + ":/usr/bin:/bin");
	ScopedEnvironment childMarker("JPEGVIEW_TEST_CHILD_MARKER", marker.string());
	jpegview_linux::FileOperationService service;
	const std::uint64_t id = service.Request(
		jpegview_linux::FileOperationPayload{jpegview_linux::LosslessTransformOperation{
			source, jpegview_linux::LosslessJpegOperation::Rotate90}}, 12);
	Expect(id != 0, "file operation service refused a new transform request");
	const bool childStarted = WaitForPath(marker, std::chrono::seconds(2));
	bool temporaryIsPrivate = false;
	std::error_code temporaryIteratorError;
	for (const fs::directory_entry& entry : fs::directory_iterator(images,
		temporaryIteratorError)) {
		if (entry.path() == source) continue;
		struct stat temporaryStatus{};
		temporaryIsPrivate = entry.path().extension() == ".tmp" &&
			::stat(entry.path().c_str(), &temporaryStatus) == 0 &&
			(temporaryStatus.st_mode & 0777) == 0600;
	}
	Expect(childStarted && temporaryIsPrivate && !temporaryIteratorError,
		"lossless operation exposed its in-progress image temporary to other users or scans");
	const auto stopStart = std::chrono::steady_clock::now();
	service.Stop();
	const auto stopTime = std::chrono::steady_clock::now() - stopStart;
	Expect(childStarted, "file operation worker did not launch the blocked child");
	Expect(stopTime < std::chrono::seconds(2),
		"application-close service shutdown waited for the blocked child to finish naturally");
	Expect(ReadBytes(source) == std::vector<std::uint8_t>({'o','r','i','g','i','n','a','l',' ',
		'j','p','e','g',' ','f','i','x','t','u','r','e'}),
		"canceled lossless transform published an incomplete replacement");
	std::error_code iteratorError;
	for (const fs::directory_entry& entry : fs::directory_iterator(images, iteratorError)) {
		Expect(entry.path() == source, "canceled transform left a temporary sibling file");
	}
	Expect(!iteratorError, "cannot inspect lossless transform temporary cleanup");
}

struct LosslessCropPublicationBarrier {
	std::mutex mutex;
	std::condition_variable changed;
	bool reached = false;
	bool released = false;
};

void PauseLosslessCropBeforePublication(void* context) {
	auto* barrier = static_cast<LosslessCropPublicationBarrier*>(context);
	std::unique_lock<std::mutex> lock(barrier->mutex);
	barrier->reached = true;
	barrier->changed.notify_all();
	barrier->changed.wait(lock, [barrier] { return barrier->released; });
}

#ifdef JPEGVIEW_CACHE_BUDGET_TEST_HOOKS
struct ImageSavePublicationBarrier {
	std::mutex mutex;
	std::condition_variable changed;
	bool reached = false;
	bool released = false;
};

void PauseImageSaveBeforePublication(void* context) {
	auto* barrier = static_cast<ImageSavePublicationBarrier*>(context);
	std::unique_lock<std::mutex> lock(barrier->mutex);
	barrier->reached = true;
	barrier->changed.notify_all();
	barrier->changed.wait(lock, [barrier] { return barrier->released; });
}

void TestImageSaveDoesNotReplaceUnconfirmedLateTarget() {
	TemporaryDirectory temporary;
	const fs::path output = temporary.path() / "late-target.png";
	ImageSavePublicationBarrier barrier;
	jpegview_linux::SetImageSavePublicationTestHookForTesting(
		PauseImageSaveBeforePublication, &barrier);
	jpegview_linux::SaveImageOperation save;
	save.output = output;
	save.image = MakeTinyTestImage();
	auto operation = std::async(std::launch::async, [save = std::move(save)]() mutable {
		return jpegview_linux::ExecuteFileOperation(
			jpegview_linux::FileOperationPayload{std::move(save)}, 52,
			[] { return true; });
	});
	bool reachedPublication = false;
	{
		std::unique_lock<std::mutex> lock(barrier.mutex);
		reachedPublication = barrier.changed.wait_for(lock, std::chrono::seconds(2),
			[&barrier] { return barrier.reached; });
	}
	const std::string lateText = "created during encoding";
	const std::vector<std::uint8_t> lateContents(lateText.begin(), lateText.end());
	if (reachedPublication) WriteBytes(output, lateContents);
	{
		std::lock_guard<std::mutex> lock(barrier.mutex);
		barrier.released = true;
		barrier.changed.notify_all();
	}
	const bool completed = operation.wait_for(std::chrono::seconds(2)) ==
		std::future_status::ready;
	jpegview_linux::SetImageSavePublicationTestHookForTesting(nullptr, nullptr);
	Expect(completed, "image save did not leave its controlled publication boundary");
	const jpegview_linux::FileOperationResult result = operation.get();
	Expect(reachedPublication && !result.success && ReadBytes(output) == lateContents,
		"unconfirmed image save replaced a destination created after its initial existence check");
	std::error_code iteratorError;
	for (const fs::directory_entry& entry : fs::directory_iterator(temporary.path(),
		iteratorError)) {
		Expect(entry.path() == output,
			"late-target save left its staged image beside the destination");
	}
	Expect(!iteratorError, "cannot inspect late-target image-save cleanup");
}
#endif

void TestLosslessCropPublicationWaitsForDestinationAdmission() {
	TemporaryDirectory temporary;
	const fs::path bin = temporary.path() / "bin";
	const fs::path images = temporary.path() / "images";
	fs::create_directories(bin);
	fs::create_directories(images);
	const fs::path transform = bin / "jpegtran";
	WriteText(transform,
		"#!/bin/sh\n"
		"output=\n"
		"while [ \"$#\" -gt 0 ]; do\n"
		"  if [ \"$1\" = \"-outfile\" ]; then shift; output=$1; break; fi\n"
		"  shift\n"
		"done\n"
		"[ -n \"$output\" ] || exit 2\n"
		"printf cropped > \"$output\" || exit 3\n");
	MakeTestExecutable(transform);
	const fs::path source = images / "source.jpg";
	const fs::path output = images / "crop.jpg";
	WriteText(source, "source jpeg fixture");
	WriteText(output, "old crop output");
	ScopedEnvironment path("PATH", bin.string() + ":/usr/bin:/bin");
	LosslessCropPublicationBarrier barrier;
	jpegview_linux::SetLosslessCropPublicationTestHookForTesting(
		PauseLosslessCropBeforePublication, &barrier);
	auto& coordinator = jpegview_linux::SourceWorkCoordinator::Global();
	jpegview_linux::LosslessCropOperation crop;
	crop.source = source;
	crop.output = output;
	crop.width = 1;
	crop.height = 1;
	crop.overwriteConfirmed = true;
	auto operation = std::async(std::launch::async, [crop] {
		return jpegview_linux::ExecuteFileOperation(
			jpegview_linux::FileOperationPayload{crop}, 46, [] { return true; });
	});
	bool reachedPublicationBoundary = false;
	{
		std::unique_lock<std::mutex> lock(barrier.mutex);
		reachedPublicationBoundary = barrier.changed.wait_for(lock,
			std::chrono::seconds(2), [&barrier] { return barrier.reached; });
	}
	const jpegview_linux::WorkContext outputReadContext =
		jpegview_linux::MakePathWorkContext(output,
			jpegview_linux::SourceWorkPriority::Foreground, [] { return true; });
	jpegview_linux::SourceWorkLease outputReader;
	bool readerAcquired = false;
	bool originalOutputStillVisible = false;
	bool waitingForOutputAdmission = false;
	bool stillWaiting = false;
	jpegview_linux::SourceWorkSnapshot publicationWaitSnapshot;
	if (reachedPublicationBoundary) {
		outputReader = coordinator.Acquire(outputReadContext, output);
		readerAcquired = static_cast<bool>(outputReader);
		if (readerAcquired) {
			originalOutputStillVisible = ReadBytes(output) ==
				std::vector<std::uint8_t>({'o','l','d',' ','c','r','o','p',' ','o','u','t','p','u','t'});
		}
	}
	{
		std::lock_guard<std::mutex> lock(barrier.mutex);
		barrier.released = true;
		barrier.changed.notify_all();
	}
	if (readerAcquired) {
		waitingForOutputAdmission = coordinator.WaitForSnapshot(
			[](const jpegview_linux::SourceWorkSnapshot& snapshot) {
				return snapshot.activeForeground == 1 && snapshot.activeCpu == 0 &&
					snapshot.waitingForeground == 1;
			}, std::chrono::seconds(2));
		publicationWaitSnapshot = coordinator.Snapshot();
		stillWaiting = operation.wait_for(std::chrono::milliseconds(0)) !=
			std::future_status::ready;
	}
	outputReader.Reset();
	const bool resumedAfterRelease = operation.wait_for(std::chrono::seconds(2)) ==
		std::future_status::ready;
	jpegview_linux::SetLosslessCropPublicationTestHookForTesting(nullptr, nullptr);
	Expect(resumedAfterRelease,
		"lossless crop did not resume after destination admission became available");
	const auto result = operation.get();
	std::ostringstream publicationWaitFailure;
	publicationWaitFailure << "lossless crop publication did not wait for its admitted destination reader "
		<< "(publicationBoundary/readerAcquired/coordinatorWait/futureBlocked/outputPreserved="
		<< reachedPublicationBoundary << '/' << readerAcquired << '/'
		<< waitingForOutputAdmission << '/' << stillWaiting << '/'
		<< originalOutputStillVisible << ", foreground/cpu/waiting="
		<< publicationWaitSnapshot.activeForeground << '/'
		<< publicationWaitSnapshot.activeCpu << '/'
		<< publicationWaitSnapshot.waitingForeground << ", result="
		<< result.success << ", message=" << result.message << ')';
	Expect(reachedPublicationBoundary && readerAcquired && waitingForOutputAdmission && stillWaiting &&
		originalOutputStillVisible && result.success && result.path == output &&
		ReadBytes(output) == std::vector<std::uint8_t>({'c','r','o','p','p','e','d'}),
		publicationWaitFailure.str());
}

void TestFileOperationServicePublishesCompletionsAndResumes() {
	jpegview_linux::FileOperationService service;
	const std::uint64_t firstId = service.Request(
		jpegview_linux::FileOperationPayload{jpegview_linux::LaunchDesktopOperation{
			{{"/bin/true", {}}}}}, 21);
	Expect(firstId != 0 && service.Busy(),
		"file operation service did not accept its first asynchronous request");
	Expect(service.WaitUntilIdle(std::chrono::seconds(2)),
		"file operation service did not finish the detached command before its deadline");
	Expect(service.Busy(),
		"completed file operation stopped reporting busy before its result was consumed");
	const auto first = service.TakeReady();
	Expect(first.has_value() && first->id == firstId && first->ownerGeneration == 21 &&
		first->kind == jpegview_linux::FileOperationKind::LaunchDesktop && first->success,
		"file operation service did not publish the matching successful result");
	Expect(!service.Busy(),
		"consuming the file operation result did not return the service to idle");
	const std::uint64_t secondId = service.Request(
		jpegview_linux::FileOperationPayload{jpegview_linux::LaunchDesktopOperation{
			{{"/bin/true", {}}}}}, 22);
	Expect(secondId > firstId && service.WaitUntilIdle(std::chrono::seconds(2)),
		"file operation service did not accept work after delivering its previous result");
	const auto second = service.TakeReady();
	Expect(second.has_value() && second->id == secondId &&
		second->ownerGeneration == 22 && second->success,
		"file operation service published a stale or incomplete second result");
	service.Stop();
}

void TestFileOperationServiceReportsCompletedIrreversibleWorkAfterCancel() {
	const auto verifyExit = [](bool successfulExit) {
		TemporaryDirectory temporary;
		const fs::path bin = temporary.path() / "bin";
		fs::create_directories(bin);
		const fs::path marker = temporary.path() / "print started";
		const fs::path release = temporary.path() / "allow print exit";
		const fs::path printer = bin / "lp";
		WriteText(printer,
			"#!/bin/sh\n"
			"printf started > \"$JPEGVIEW_TEST_PRINT_MARKER\"\n"
			"attempt=0\n"
			"while [ \"$attempt\" -lt 100 ]; do\n"
			"  if [ -f \"$JPEGVIEW_TEST_PRINT_RELEASE\" ]; then\n"
			"    exit \"$JPEGVIEW_TEST_PRINT_EXIT_CODE\"\n"
			"  fi\n"
			"  attempt=$((attempt + 1))\n"
			"  sleep 0.01\n"
			"done\n"
			"exit 9\n");
		MakeTestExecutable(printer);
		ScopedEnvironment path("PATH", bin.string() + ":/usr/bin:/bin");
		ScopedEnvironment printMarker("JPEGVIEW_TEST_PRINT_MARKER", marker.string());
		ScopedEnvironment printRelease("JPEGVIEW_TEST_PRINT_RELEASE", release.string());
		ScopedEnvironment printExit("JPEGVIEW_TEST_PRINT_EXIT_CODE",
			successfulExit ? "0" : "9");
		jpegview_linux::PrintImageOperation print;
		print.image = MakeTinyTestImage();
		jpegview_linux::FileOperationService service;
		const std::uint64_t id = service.Request(
			jpegview_linux::FileOperationPayload{std::move(print)}, 23);
		const bool started = id != 0 && WaitForPath(marker, std::chrono::seconds(2));
		Expect(!started ||
			jpegview_linux::SourceWorkCoordinator::Global().Snapshot().activeCpu == 0,
			"print operation retained a CPU permit while waiting for the printer");
		if (id != 0) service.Cancel(id);
		WriteText(release, "finish the started print command");
		Expect(started, "print operation did not start before the cancellation test");
		Expect(service.WaitUntilIdle(std::chrono::seconds(2)),
			"started print operation did not finish after cancellation was requested");
		const auto result = service.TakeReady();
		Expect(result.has_value() && result->id == id &&
			result->success == successfulExit && !result->cancelled &&
			(result->failure.has_value() == !successfulExit),
			"an irreversible print exit was hidden by a late cancellation request");
		service.Stop();
	};
	verifyExit(true);
	verifyExit(false);
}

void TestBatchCopyPatternExpansionAndPreview() {
	const fs::path source = fs::path("/tmp/photos/holiday12.JPG");
	const std::time_t timestamp = 1706933106; // 2024-02-03 04:05:06 UTC.
	const fs::path pictures = fs::path("/tmp/Pictures");
	const std::string pattern = R"(folder\%x-%n-%2x-%3x-%9x-%f-%F-%e-%h-%min-%d-%m-%y-%2y-%3M-%M-%pictures%)";
	const std::string expanded = jpegview_linux::ExpandBatchPattern(pattern, 4, source, timestamp, pictures);
	const std::string expected = "folder/5-12-05-005-000000005-holiday12.JPG-holiday12-JPG-" +
		FormatLocalTime(timestamp, "%H") + "-" + FormatLocalTime(timestamp, "%M") + "-" +
		FormatLocalTime(timestamp, "%d") + "-" + FormatLocalTime(timestamp, "%m") + "-" +
		FormatLocalTime(timestamp, "%Y") + "-" + FormatLocalTime(timestamp, "%y") + "-" +
		FormatLocalTime(timestamp, "%b") + "-" + FormatLocalTime(timestamp, "%B") + "-/tmp/Pictures";
	Expect(expanded == expected, "batch copy pattern expansion is incorrect: " + expanded);
	Expect(jpegview_linux::ExpandBatchPattern("%f", 0, fs::path("no-extension"), 0) == "no-extension",
		"batch copy expansion mishandled a filename without an extension");

	jpegview_linux::BatchCopyItem first;
	first.source = fs::path("/tmp/photos/a12.jpg");
	first.modificationTime = timestamp;
	first.selected = true;
	jpegview_linux::BatchCopyItem skipped;
	skipped.source = fs::path("/tmp/photos/b13.jpg");
	jpegview_linux::BatchCopyItem renamed;
	renamed.source = fs::path("/tmp/photos/c14.jpg");
	renamed.selected = true;
	std::vector<jpegview_linux::BatchCopyItem> items{first, skipped, renamed};
	jpegview_linux::UpdateBatchCopyPreview("out/%2x-%f", items);
	Expect(items[0].destinationText == "out/01-a12.jpg", "batch copy preview text is incorrect");
	Expect(items[0].destination == fs::path("/tmp/photos/out/01-a12.jpg"),
		"batch copy relative destination was not resolved against the source directory");
	Expect(items[0].copy, "batch copy preview did not classify a different directory as a copy");
	Expect(items[1].destination.empty() && items[1].destinationText.empty() && !items[1].copy,
		"unselected batch copy item was included in the preview");
	Expect(items[2].destinationText == "out/02-c14.jpg" && items[2].copy,
		"batch copy selected index did not ignore unselected items");
	Expect(jpegview_linux::BatchCopyDestination("renamed-%f", items[2], 0) ==
		fs::path("/tmp/photos/renamed-c14.jpg"), "same-directory rename target is incorrect");
	items[0].selected = false;
	items[2].selected = false;
	items[0].destination = fs::path("stale");
	items[0].destinationText = "stale";
	items[0].copy = true;
	jpegview_linux::UpdateBatchCopyPreview("", items);
	Expect(items[0].destination.empty() && items[0].destinationText.empty() && !items[0].copy,
		"empty batch pattern did not clear stale preview state");
	Expect(jpegview_linux::FormatBatchDate(0).empty(), "zero batch timestamp should format as empty");
	Expect(jpegview_linux::FormatBatchDate(timestamp) ==
		FormatLocalTime(timestamp, "%Y-%m-%d %H:%M:%S"), "batch date formatting is incorrect");
}

void TestBatchCopyDialogController() {
	std::vector<jpegview_linux::BatchCopyItem> items;
	for (int index = 0; index < 8; ++index) {
		jpegview_linux::BatchCopyItem item;
		item.source = fs::path("/tmp/photos/image" + std::to_string(index) + ".jpg");
		items.push_back(std::move(item));
	}
	jpegview_linux::BatchCopyDialogController dialog;
	dialog.Open(items, 5, "%F-copy.%e", 3);
	Expect(dialog.IsOpen() && dialog.PatternFocused() && dialog.Cursor() == 5 && dialog.Scroll() == 3,
		"batch dialog did not initialize focus, cursor, and visible scroll state");
	dialog.MoveCursor(1, 3);
	Expect(dialog.Cursor() == 5, "batch dialog moved the list cursor while its pattern had focus");
	dialog.TogglePatternFocus();
	dialog.MoveCursor(1, 3);
	Expect(dialog.Cursor() == 6 && dialog.Scroll() == 4,
		"batch dialog did not reveal a keyboard-moved cursor");
	dialog.ToggleItem(6);
	Expect(dialog.Items()[6].selected && dialog.Items()[6].destinationText == "image6-copy.jpg" &&
		dialog.Message().find("1 selected") != std::string::npos,
		"batch dialog did not update selection, preview, and summary together");
	dialog.SelectAll(true);
	Expect(std::all_of(dialog.Items().begin(), dialog.Items().end(),
		[](const jpegview_linux::BatchCopyItem& item) { return item.selected; }) &&
		dialog.Message().find("8 selected") != std::string::npos,
		"batch dialog select-all did not refresh preview state");
	dialog.ScrollBy(100, 3);
	Expect(dialog.Scroll() == 5, "batch dialog scrolling exceeded its final full page");
	dialog.ScrollBy(-100, 3);
	Expect(dialog.Scroll() == 0, "batch dialog scrolling exceeded its first page");
	dialog.FocusItem(2);
	Expect(dialog.Cursor() == 2, "batch dialog hover focus did not select a valid row");
	dialog.FocusItem(99);
	Expect(dialog.Cursor() == 2, "batch dialog hover focus accepted an invalid row");

	dialog.Open({}, 0, u8"copy-写真", 4);
	dialog.BackspacePattern();
	Expect(dialog.Pattern() == u8"copy-写", "batch dialog Backspace split a UTF-8 code point");
	dialog.AppendPattern(u8"像");
	Expect(dialog.Pattern() == u8"copy-写像", "batch dialog did not append Unicode pattern text");
	dialog.TogglePatternFocus();
	dialog.AppendPattern("ignored");
	Expect(dialog.Pattern() == u8"copy-写像", "batch dialog edited the pattern without pattern focus");
	dialog.Close();
	Expect(!dialog.IsOpen() && !dialog.PatternFocused(), "batch dialog did not clear open/focus state");
}

void TestDesktopApplicationParsingAndExecExpansion() {
	TemporaryDirectory temporary;
	const fs::path desktopFile = temporary.path() / "viewer.desktop";
	WriteText(desktopFile,
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Test\\sViewer\n"
		"Exec=test-viewer --title \"hello world\" %f\n"
		"MimeType=image/jpeg;image/png;\n"
		"Terminal=true\n"
		"\n"
		"[Desktop Action Open]\n"
		"Name=Ignored\n");

	jpegview_linux::OpenWithApplication application;
	Expect(jpegview_linux::ReadDesktopApplication(desktopFile, "image/jpeg", application),
		"valid desktop entry was rejected");
	Expect(application.name == "Test Viewer" && application.terminal,
		"desktop entry name or terminal flag was parsed incorrectly");
	Expect(application.exec.find("hello world") != std::string::npos, "desktop Exec quoting was lost");

	Expect(!jpegview_linux::ReadDesktopApplication(desktopFile, "image/gif", application),
		"desktop entry with an incompatible MIME type was accepted");
	WriteText(temporary.path() / "hidden.desktop",
		"[Desktop Entry]\nType=Application\nName=Hidden\nExec=viewer %f\nMimeType=image/jpeg;\nHidden=true\n");
	Expect(!jpegview_linux::ReadDesktopApplication(temporary.path() / "hidden.desktop", "image/jpeg", application),
		"hidden desktop entry was accepted");

	Expect(jpegview_linux::UnescapeDesktopValue("a\\sb\\n\\t\\\\") == "a b\n\t\\",
		"desktop value escaping was parsed incorrectly");
	Expect(jpegview_linux::MimeTypeMatches("image/*", "image/png"), "MIME wildcard did not match");
	Expect(jpegview_linux::MimeTypeMatches("image/x-ms-bmp", "image/bmp"), "MIME alias did not match");
	Expect(jpegview_linux::MimeTypeMatches("*/*", "image/png"), "universal MIME wildcard did not match");
	Expect(!jpegview_linux::MimeTypeMatches("image/jpeg", "image/png"), "incompatible MIME type matched");
	Expect(jpegview_linux::MimeTypeForExtension(".jpg") == "image/jpeg", "JPEG MIME mapping is incorrect");

	const fs::path image = temporary.path() / "photos" / "a file.jpg";
	application.name = "Test Viewer";
	application.desktopFile = desktopFile;
	application.exec = "viewer --name \"%n\" %u %% %d";
	const std::vector<std::string> tokens = jpegview_linux::TokenizeDesktopExec(
		"viewer --label \"hello world\" 'single quoted' escaped\\ space");
	Expect(tokens == std::vector<std::string>({"viewer", "--label", "hello world", "single quoted", "escaped space"}),
		"desktop Exec tokenization is incorrect");
	const std::vector<std::string> arguments = jpegview_linux::DesktopExecArguments(application, image);
	Expect(arguments.size() == 6 && arguments[0] == "viewer" && arguments[1] == "--name" &&
		arguments[2] == "a file.jpg" && arguments[3].find("file:///tmp") == 0 &&
		arguments[4] == "%" && arguments[5] == fs::absolute(image.parent_path()).lexically_normal().string(),
		"desktop Exec field expansion is incorrect");

	application.exec = "viewer --flag";
	const std::vector<std::string> implicitFile = jpegview_linux::DesktopExecArguments(application, image);
	Expect(implicitFile.size() == 3 && implicitFile[2] == fs::absolute(image).lexically_normal().string(),
		"desktop Exec did not append an image when no field code was present");
}

void TestDefaultViewerRegistration() {
	TemporaryDirectory temporary;
	const fs::path dataHome = temporary.path() / "data";
	const fs::path configHome = temporary.path() / "config";
	const fs::path executable = temporary.path() / "JPEG View%\".AppImage";
	const fs::path mimeApps = configHome / "mimeapps.list";
	fs::create_directories(configHome);
	WriteText(mimeApps,
		"[Added Associations]\n"
		"image/jpeg=existing-viewer.desktop;\n"
		"\n[Default Applications]\n"
		"image/jpeg=existing-viewer.desktop;\n"
		"text/plain=editor.desktop;\n");
	std::string errorMessage;
	Expect(jpegview_linux::RegisterDefaultViewer(executable, dataHome, configHome, errorMessage),
		"default viewer could not be registered: " + errorMessage);
	const fs::path desktopFile = dataHome / "applications" / "jpegview-linux-user.desktop";
	const std::string desktop = [&desktopFile]() {
		std::ifstream input(desktopFile);
		std::ostringstream contents;
		contents << input.rdbuf();
		return contents.str();
	}();
	std::string escapedExecutable;
	for (const char character : executable.string()) {
		if (character == '\\' || character == '"') escapedExecutable.push_back('\\');
		if (character == '%') escapedExecutable.push_back('%');
		escapedExecutable.push_back(character);
	}
	Expect(desktop.find("Exec=\"" + escapedExecutable + "\" %F") != std::string::npos &&
		desktop.find("%%") != std::string::npos && desktop.find("NoDisplay=true") != std::string::npos,
		"default viewer desktop entry did not safely quote its executable or hide its launcher duplicate");
	Expect(desktop.find("MimeType=image/jpeg;") != std::string::npos &&
		desktop.find("image/tiff;") != std::string::npos &&
		desktop.find("image/jxl;") != std::string::npos,
		"default viewer desktop entry omitted supported MIME types");
	jpegview_linux::OpenWithApplication registeredEntry;
	Expect(!jpegview_linux::ReadDesktopApplication(desktopFile, "image/jpeg", registeredEntry),
		"the NoDisplay default-viewer entry leaked into Open With discovery");
	std::ifstream associationInput(mimeApps);
	std::ostringstream associationContents;
	associationContents << associationInput.rdbuf();
	const std::string associations = associationContents.str();
	Expect(associations.find("[Added Associations]\nimage/jpeg=existing-viewer.desktop;\n") !=
		std::string::npos &&
		associations.find("image/jpeg=jpegview-linux-user.desktop;") != std::string::npos &&
		associations.find("text/plain=editor.desktop;") != std::string::npos &&
		associations.find("image/jxl=jpegview-linux-user.desktop;") != std::string::npos,
		"default viewer registration did not preserve unrelated MIME associations");
	Expect(jpegview_linux::RegisterDefaultViewer(executable, dataHome, configHome, errorMessage),
		"repeated default viewer registration failed: " + errorMessage);
	std::ifstream repeatedInput(mimeApps);
	std::ostringstream repeatedContents;
	repeatedContents << repeatedInput.rdbuf();
	const std::string repeated = repeatedContents.str();
	const std::string mimeDefault = "image/jpeg=jpegview-linux-user.desktop;";
	const std::size_t first = repeated.find(mimeDefault);
	Expect(first != std::string::npos && repeated.find(mimeDefault, first + mimeDefault.size()) == std::string::npos,
		"repeated default viewer registration duplicated a MIME association");
}

void TestExternalCommandPlanning() {
	using jpegview_linux::ClipboardBackend;
	using jpegview_linux::ExternalCommand;
	using jpegview_linux::LosslessJpegOperation;
	const fs::path source = fs::path("/tmp/source photo.jpg");
	const fs::path output = fs::path("/tmp/output.jpg");
	ExternalCommand command = jpegview_linux::LosslessJpegCommand(
		LosslessJpegOperation::Rotate90, source, output);
	Expect(command.executable == "jpegtran" && command.arguments ==
		std::vector<std::string>({"-copy", "all", "-rotate", "90", "-outfile",
			output.string(), source.string()}),
		"lossless JPEG rotation plan has incorrect arguments");
	command = jpegview_linux::LosslessJpegCommand(
		LosslessJpegOperation::FlipVertical, source, output);
	Expect(command.arguments[2] == "-flip" && command.arguments[3] == "vertical",
		"lossless JPEG mirror plan has incorrect arguments");
	command = jpegview_linux::LosslessJpegCropCommand(source, output, 16, 32, 640, 480);
	Expect(command.executable == "jpegtran" && command.arguments ==
		std::vector<std::string>({"-copy", "all", "-crop", "640x480+16+32",
			"-outfile", output.string(), source.string()}),
		"lossless JPEG crop plan has incorrect arguments");
	Expect(!jpegview_linux::LosslessJpegCropCommand(source, output, -1, 0, 1, 1).Valid() &&
		!jpegview_linux::LosslessJpegCropCommand(source, output, 0, 0, 0, 1).Valid(),
		"lossless JPEG crop plan accepted invalid geometry");
	Expect(jpegview_linux::PrintCommand(output).executable == "lp" &&
		jpegview_linux::PrintCommand(output).arguments == std::vector<std::string>({output.string()}),
		"print command plan is incorrect");
	const std::string projectUrl = "https://github.com/qusielle/vibe-jpegview-linux?ref=about&source=app";
	const auto openUrl = jpegview_linux::OpenUrlCommands(projectUrl);
	Expect(openUrl.size() == 2 && openUrl[0].executable == "xdg-open" &&
		openUrl[0].arguments == std::vector<std::string>({projectUrl}) &&
		openUrl[1].executable == "gio" &&
		openUrl[1].arguments == std::vector<std::string>({"open", projectUrl}) &&
		jpegview_linux::OpenUrlCommands("").empty(),
		"URL opener fallback plans are incorrect");

	const auto openFolder = jpegview_linux::OpenContainingFolderCommands(fs::path("/tmp/my folder"));
	Expect(openFolder.size() == 2 && openFolder[0].executable == "xdg-open" &&
		openFolder[1].executable == "gio" && openFolder[1].arguments ==
			std::vector<std::string>({"open", "/tmp/my folder"}),
		"open-folder fallback plans are incorrect");
	jpegview_linux::OpenWithApplication application;
	application.name = "Editor";
	application.exec = "photo-editor --new-window %f";
	application.terminal = true;
	auto openWith = jpegview_linux::OpenWithCommand(application, source, true);
	Expect(openWith.has_value() && openWith->executable == "x-terminal-emulator" &&
		openWith->arguments == std::vector<std::string>({"-e", "photo-editor", "--new-window", source.string()}),
		"terminal Open with plan did not wrap the expanded desktop command");
	openWith = jpegview_linux::OpenWithCommand(application, source, false);
	Expect(openWith.has_value() && openWith->executable == "photo-editor" &&
		openWith->arguments == std::vector<std::string>({"--new-window", source.string()}),
		"direct Open with plan did not split executable and arguments");
	application.exec.clear();
	Expect(!jpegview_linux::OpenWithCommand(application, source, true).has_value(),
		"invalid Open with command produced an execution plan");

	const auto wallpaper = jpegview_linux::WallpaperCommandSequences(source);
	Expect(wallpaper.size() == 3 && wallpaper[0].required.executable == "gsettings" &&
		wallpaper[0].required.arguments[2] == "picture-uri" &&
		wallpaper[0].required.arguments.back() == "file:///tmp/source%20photo.jpg" &&
		wallpaper[0].afterSuccess.size() == 1 &&
		wallpaper[0].afterSuccess[0].arguments[2] == "picture-uri-dark" &&
		wallpaper[1].required.executable == "feh" &&
		wallpaper[2].required.executable == "nitrogen",
		"wallpaper backend plan order or GNOME follow-up is incorrect");
	const auto trash = jpegview_linux::TrashCommands(source);
	Expect(trash.size() == 2 && trash[0].arguments ==
		std::vector<std::string>({"trash", source.string()}) &&
		trash[1].executable == "trash-put",
		"trash helper plans are incorrect");

	Expect(jpegview_linux::SelectClipboardBackend(true, true, true) == ClipboardBackend::Wayland &&
		jpegview_linux::SelectClipboardBackend(false, true, true) == ClipboardBackend::Xclip &&
		jpegview_linux::SelectClipboardBackend(false, true, false) == ClipboardBackend::Wayland &&
		jpegview_linux::SelectClipboardBackend(true, false, false) == ClipboardBackend::None,
		"clipboard backend preference/fallback policy is incorrect");
	const ExternalCommand waylandWrite = jpegview_linux::ClipboardWriteCommand(ClipboardBackend::Wayland);
	const ExternalCommand xclipRead = jpegview_linux::ClipboardReadCommand(ClipboardBackend::Xclip);
	Expect(waylandWrite.executable == "wl-copy" && waylandWrite.arguments ==
		std::vector<std::string>({"--type", "image/png"}) &&
		xclipRead.executable == "xclip" && xclipRead.arguments.back() == "-o" &&
		!jpegview_linux::ClipboardReadCommand(ClipboardBackend::None).Valid(),
		"clipboard helper arguments are incorrect");
}

class ExifFixture {
public:
	ExifFixture() : bytes_({'E', 'x', 'i', 'f', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) {
		Write16(6, 0x4949);
		Write16(8, 42);
		Write32(10, 8);
	}

	std::vector<std::uint8_t> Build(bool includeExifDirectory = true,
		bool malformedExifDirectory = false, bool includeIfd0ShootingFields = false) {
		const std::size_t ifd0EntryCount = 6 + (includeExifDirectory ? 1 : 0) +
			(includeIfd0ShootingFields ? 6 : 0);
		const std::uint32_t ifd0 = AddDirectory(ifd0EntryCount);
		const std::uint32_t exif = includeExifDirectory ? AddDirectory(8) : 0;
		const std::uint32_t gps = AddDirectory(6);
		SetEntry(ifd0, 0, 0x010F, 2, 5, AddString("Acme"));
		SetEntry(ifd0, 1, 0x0110, 2, 6, AddString("Model"));
		SetEntry(ifd0, 2, 0x010E, 2, 12, AddString("A test image"));
		SetEntry(ifd0, 3, 0x0131, 2, 6, AddString("Tester"));
		SetEntry(ifd0, 4, 0x0132, 2, 20, AddString("2024:01:02 03:04:05"));
		std::size_t ifd0Entry = 5;
		if (includeIfd0ShootingFields) {
			SetEntry(ifd0, ifd0Entry++, 0x829A, 5, 1, AddRational(1, 60));
			SetEntry(ifd0, ifd0Entry++, 0x9204, 10, 1, AddRational(-2, 3));
			SetEntryInline16(ifd0, ifd0Entry++, 0x9209, 3, 1, 0);
			SetEntry(ifd0, ifd0Entry++, 0x920A, 5, 1, AddRational(85, 1));
			SetEntry(ifd0, ifd0Entry++, 0x829D, 5, 1, AddRational(14, 10));
			SetEntryInline16(ifd0, ifd0Entry++, 0x8827, 3, 1, 400);
		}
		if (includeExifDirectory) {
			SetEntry(ifd0, ifd0Entry++, 0x8769, 4, 1,
				malformedExifDirectory ? 0xffffffffu : exif);
		}
		SetEntry(ifd0, ifd0Entry, 0x8825, 4, 1, gps);

		if (includeExifDirectory) {
			SetEntry(exif, 0, 0x9003, 2, 20, AddString("2024:02:03 04:05:06"));
			SetEntry(exif, 1, 0x829A, 5, 1, AddRational(1, 125));
			SetEntry(exif, 2, 0x9204, 10, 1, AddRational(-1, 3));
			SetEntryInline16(exif, 3, 0x9209, 3, 1, 1);
			SetEntry(exif, 4, 0x920A, 5, 1, AddRational(50, 1));
			SetEntry(exif, 5, 0x829D, 5, 1, AddRational(28, 10));
			SetEntryInline16(exif, 6, 0x8827, 3, 1, 200);
			std::vector<std::uint8_t> comment = {'A', 'S', 'C', 'I', 'I', 0, 0, 0, 'h', 'e', 'l', 'l', 'o', 0};
			SetEntry(exif, 7, 0x9286, 7, static_cast<std::uint32_t>(comment.size()), AddBytes(comment));
		}

		SetEntryInlineBytes(gps, 0, 0x0001, 2, 2, {'S', 0});
		SetEntry(gps, 1, 0x0002, 5, 3, AddRationals({{12, 1}, {34, 1}, {56, 1}}));
		SetEntryInlineBytes(gps, 2, 0x0003, 2, 2, {'W', 0});
		SetEntry(gps, 3, 0x0004, 5, 3, AddRationals({{98, 1}, {7, 1}, {6, 1}}));
		SetEntryInlineBytes(gps, 4, 0x0005, 1, 1, {1});
		SetEntry(gps, 5, 0x0006, 5, 1, AddRational(30, 1));
		return bytes_;
	}

private:
	void Write16(std::size_t position, std::uint16_t value) {
		Ensure(position + 2);
		bytes_[position] = static_cast<std::uint8_t>(value & 0xff);
		bytes_[position + 1] = static_cast<std::uint8_t>(value >> 8);
	}

	void Write32(std::size_t position, std::uint32_t value) {
		Ensure(position + 4);
		for (int byte = 0; byte < 4; ++byte) bytes_[position + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
	}

	void Ensure(std::size_t size) {
	if (bytes_.size() < size) bytes_.resize(size, 0);
	}

	std::uint32_t AddDirectory(std::size_t count) {
		const std::uint32_t relative = static_cast<std::uint32_t>(bytes_.size() - 6);
		const std::size_t start = bytes_.size();
		bytes_.resize(bytes_.size() + 2 + count * 12 + 4, 0);
		Write16(start, static_cast<std::uint16_t>(count));
		return relative;
	}

	std::uint32_t AddBytes(const std::vector<std::uint8_t>& bytes) {
		const std::uint32_t relative = static_cast<std::uint32_t>(bytes_.size() - 6);
		bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
		return relative;
	}

	std::uint32_t AddString(const std::string& text) {
		std::vector<std::uint8_t> bytes(text.begin(), text.end());
		bytes.push_back(0);
		return AddBytes(bytes);
	}

	std::uint32_t AddRational(std::int32_t numerator, std::int32_t denominator) {
		const std::uint32_t relative = static_cast<std::uint32_t>(bytes_.size() - 6);
		const std::size_t start = bytes_.size();
		bytes_.resize(bytes_.size() + 8, 0);
		Write32(start, static_cast<std::uint32_t>(numerator));
		Write32(start + 4, static_cast<std::uint32_t>(denominator));
		return relative;
	}

	std::uint32_t AddRationals(const std::vector<std::pair<std::int32_t, std::int32_t>>& values) {
		const std::uint32_t relative = static_cast<std::uint32_t>(bytes_.size() - 6);
		for (const auto& value : values) AddRational(value.first, value.second);
		return relative;
	}

	std::size_t EntryPosition(std::uint32_t directory, std::size_t index) const {
		return 6 + directory + 2 + index * 12;
	}

	void SetEntry(std::uint32_t directory, std::size_t index, std::uint16_t tag,
		std::uint16_t type, std::uint32_t count, std::uint32_t value) {
		const std::size_t position = EntryPosition(directory, index);
		Write16(position, tag);
		Write16(position + 2, type);
		Write32(position + 4, count);
		Write32(position + 8, value);
	}

	void SetEntryInline16(std::uint32_t directory, std::size_t index, std::uint16_t tag,
		std::uint16_t type, std::uint32_t count, std::uint16_t value) {
		SetEntry(directory, index, tag, type, count, value);
	}

	void SetEntryInlineBytes(std::uint32_t directory, std::size_t index, std::uint16_t tag,
		std::uint16_t type, std::uint32_t count, std::initializer_list<std::uint8_t> values) {
		const std::size_t position = EntryPosition(directory, index);
		Write16(position, tag);
		Write16(position + 2, type);
		Write32(position + 4, count);
		std::size_t offset = position + 8;
		for (std::uint8_t value : values) bytes_[offset++] = value;
	}

	std::vector<std::uint8_t> bytes_;
};

std::vector<std::uint8_t> InsertJpegSegment(const std::vector<std::uint8_t>& jpeg,
	std::uint8_t marker, const std::vector<std::uint8_t>& payload) {
	Expect(jpeg.size() >= 2 && jpeg[0] == 0xff && jpeg[1] == 0xd8, "test JPEG has no SOI marker");
	Expect(payload.size() + 2 <= 65535, "test JPEG segment is too large");
	std::vector<std::uint8_t> result{0xff, 0xd8, 0xff, marker,
		static_cast<std::uint8_t>((payload.size() + 2) >> 8),
		static_cast<std::uint8_t>((payload.size() + 2) & 0xff)};
	result.insert(result.end(), payload.begin(), payload.end());
	result.insert(result.end(), jpeg.begin() + 2, jpeg.end());
	return result;
}

void TestExifAndJpegCommentParsing() {
	TemporaryDirectory temporary;
	const fs::path jpeg = temporary.path() / "metadata.jpg";
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	options.jpegQuality = 100;
	std::string error;
	Expect(jpegview_linux::WriteImage(jpeg, pixels.data(), 2, 2, options, error),
		"cannot create EXIF fixture JPEG: " + error);
	const std::vector<std::uint8_t> withExif = InsertJpegSegment(ReadBytes(jpeg), 0xe1,
		ExifFixture().Build(true, false, true));
	const std::vector<std::uint8_t> withComment = InsertJpegSegment(withExif, 0xfe,
		std::vector<std::uint8_t>{'t', 'e', 's', 't', ' ', 'c', 'o', 'm', 'm', 'e', 'n', 't'});
	WriteBytes(jpeg, withComment);

	jpegview_linux::ExifInfo info;
	std::string comment;
	Expect(jpegview_linux::ReadJpegMetadata(jpeg, info, comment), "EXIF fixture was not recognized as JPEG");
	Expect(info.hasExif, "valid EXIF directory was not detected");
	Expect(info.cameraModel == "Acme Model", "camera make/model was parsed incorrectly");
	Expect(info.imageDescription == "A test image", "image description was parsed incorrectly");
	Expect(info.software == "Tester", "software metadata was parsed incorrectly");
	Expect(info.dateTime == "2024:01:02 03:04:05", "IFD0 date was parsed incorrectly");
	Expect(info.acquisitionDate == "2024:02:03 04:05:06", "EXIF acquisition date was parsed incorrectly");
	Expect(info.exposureTime == "1/125", "exposure time was parsed incorrectly");
	Expect(info.hasExposureBias, "exposure bias was not detected");
	ExpectNear(info.exposureBias, -1.0 / 3.0, 0.0001, "exposure bias was parsed incorrectly");
	Expect(info.hasFlash && info.flashFired, "flash metadata was parsed incorrectly");
	Expect(info.hasFocalLength, "focal length was not detected");
	ExpectNear(info.focalLength, 50.0, 0.0001, "focal length was parsed incorrectly");
	Expect(info.hasFNumber, "f-number was not detected");
	ExpectNear(info.fNumber, 2.8, 0.0001, "f-number was parsed incorrectly");
	Expect(info.isoSpeed == 200, "ISO metadata was parsed incorrectly");
	Expect(info.userComment == "hello", "user comment was parsed incorrectly");
	Expect(info.hasGps && info.gpsLocation == "-12.58222, -98.11833" &&
		std::abs(info.gpsLatitude + 12.5822222222) < 0.0000001 &&
		std::abs(info.gpsLongitude + 98.1183333333) < 0.0000001,
		"GPS metadata was parsed incorrectly");
	Expect(info.hasAltitude, "GPS altitude was not detected");
	ExpectNear(info.altitude, -30.0, 0.0001, "GPS altitude was parsed incorrectly");
	Expect(comment == "test comment", "JPEG comment was parsed incorrectly");

	const fs::path streaming = temporary.path() / "streaming.jpg";
	Expect(::mkfifo(streaming.c_str(), 0600) == 0, "cannot create streaming JPEG fixture");
	const int stream = ::open(streaming.c_str(), O_RDWR | O_CLOEXEC);
	Expect(stream >= 0, "cannot open streaming JPEG fixture");
	auto streamingRead = std::async(std::launch::async, [&streaming] {
		jpegview_linux::ExifInfo streamedInfo;
		std::string streamedComment;
		return std::make_pair(
			jpegview_linux::ReadJpegMetadata(streaming, streamedInfo, streamedComment),
			streamedComment);
	});
	const std::array<std::uint8_t, 12> header = {
		0xff, 0xd8, 0xff, 0xfe, 0x00, 0x06, 't', 'e', 's', 't', 0xff, 0xda
	};
	Expect(::write(stream, header.data(), header.size()) ==
		static_cast<ssize_t>(header.size()), "cannot write streaming JPEG fixture");
	const bool stoppedAtScan = streamingRead.wait_for(std::chrono::seconds(2)) ==
		std::future_status::ready;
	::close(stream);
	const auto streamed = streamingRead.get();
	Expect(stoppedAtScan && streamed.first && streamed.second == "test",
		"JPEG metadata reader consumed compressed scan data instead of stopping at its header");

	const fs::path plain = temporary.path() / "plain.jpg";
	Expect(jpegview_linux::WriteImage(plain, pixels.data(), 2, 2, options, error), "cannot create plain JPEG");
	info = {};
	comment.clear();
	Expect(!jpegview_linux::ReadJpegMetadata(plain, info, comment), "plain JPEG unexpectedly reported metadata");
	Expect(!info.hasExif && comment.empty(), "plain JPEG unexpectedly contained metadata");

	const fs::path malformed = temporary.path() / "malformed.jpg";
	WriteBytes(malformed, {0xff, 0xd8, 0xff, 0xe1, 0x00, 0x0a, 'E', 'x', 'i', 'f', 0, 0});
	info = {};
	comment.clear();
	Expect(!jpegview_linux::ReadJpegMetadata(malformed, info, comment), "malformed metadata should be treated as absent");
	Expect(!info.hasExif, "malformed metadata was incorrectly accepted as EXIF");
}

void TestExifIfd0ShootingFieldFallback() {
	TemporaryDirectory temporary;
	const fs::path base = temporary.path() / "base.jpg";
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	options.jpegQuality = 100;
	std::string error;
	Expect(jpegview_linux::WriteImage(base, pixels.data(), 2, 2, options, error),
		"cannot create IFD0 fallback fixture JPEG: " + error);
	const std::vector<std::uint8_t> jpeg = ReadBytes(base);

	const auto verifyFallback = [&](const std::string& name, bool includeExifDirectory,
		bool malformedExifDirectory) {
		const fs::path filename = temporary.path() / name;
		WriteBytes(filename, InsertJpegSegment(jpeg, 0xe1,
			ExifFixture().Build(includeExifDirectory, malformedExifDirectory, true)));
		jpegview_linux::ExifInfo info;
		std::string comment;
		Expect(jpegview_linux::ReadJpegMetadata(filename, info, comment),
			"IFD0 shooting fields were rejected from " + name);
		Expect(info.hasExif && info.exposureTime == "1/60" && info.hasExposureBias &&
			std::abs(info.exposureBias + 2.0 / 3.0) < 0.0001 &&
			info.hasFlash && !info.flashFired && info.hasFocalLength &&
			std::abs(info.focalLength - 85.0) < 0.0001 && info.hasFNumber &&
			std::abs(info.fNumber - 1.4) < 0.0001 && info.isoSpeed == 400,
			"valid IFD0 shooting fields were not used when ExifIFD was absent or malformed");
	};
	verifyFallback("without-exif-ifd.jpg", false, false);
	verifyFallback("malformed-exif-ifd.jpg", true, true);
}

void TestTiffMetadataReaderBoundedRandomAccess() {
	TemporaryDirectory temporary;
	const std::vector<std::uint8_t> exifPayload =
		ExifFixture().Build(true, false, true);
	const std::vector<std::uint8_t> tiffBytes(exifPayload.begin() + 6,
		exifPayload.end());
	std::size_t bytesRead = 0;
	const jpegview_linux::TiffMetadataReadAt readAt =
		[&tiffBytes, &bytesRead](std::size_t offset, std::uint8_t* destination,
			std::size_t length) {
			if (offset > tiffBytes.size() || length > tiffBytes.size() - offset) return false;
			std::memcpy(destination, tiffBytes.data() + offset, length);
			bytesRead += length;
			return true;
		};
	jpegview_linux::ExifInfo info;
	constexpr std::size_t logicalSourceSize = 64u * 1024u * 1024u;
	Expect(jpegview_linux::ReadTiffMetadataFromSource(logicalSourceSize, readAt, 0, info),
		"TIFF metadata parser failed with a bounded random-access source");
	Expect(bytesRead < logicalSourceSize && info.exposureTime == "1/125" &&
		info.isoSpeed == 200,
		"TIFF metadata parser read the entire backing source or changed parsed values");

	const jpegview_linux::TiffMetadataReadAt unreadableDirectory =
		[&readAt](std::size_t offset, std::uint8_t* destination, std::size_t length) {
			return offset != 8 && readAt(offset, destination, length);
		};
	info = {};
	Expect(!jpegview_linux::ReadTiffMetadataFromSource(tiffBytes.size(),
		unreadableDirectory, 0, info) && !info.hasExif,
		"failed TIFF directory count read was accepted as an empty directory");

	const std::array<std::uint8_t, 8> exifExposure = {1, 0, 0, 0, 125, 0, 0, 0};
	const auto exposurePosition = std::search(tiffBytes.begin(), tiffBytes.end(),
		exifExposure.begin(), exifExposure.end());
	Expect(exposurePosition != tiffBytes.end(),
		"TIFF fixture did not contain its ExifIFD exposure value");
	const std::size_t exposureOffset = static_cast<std::size_t>(
		exposurePosition - tiffBytes.begin());
	const jpegview_linux::TiffMetadataReadAt unreadableExifValues =
		[&readAt, &tiffBytes, exposureOffset](std::size_t offset,
			std::uint8_t* destination, std::size_t length) {
			if (offset == exposureOffset ||
				(length == 2 && offset + 2 <= tiffBytes.size() &&
					tiffBytes[offset] == 200 && tiffBytes[offset + 1] == 0)) return false;
			return readAt(offset, destination, length);
		};
	info = {};
	Expect(jpegview_linux::ReadTiffMetadataFromSource(tiffBytes.size(),
		unreadableExifValues, 0, info) && info.exposureTime == "1/60" &&
		info.isoSpeed == 400 && info.focalLength == 50.0,
		"failed ExifIFD value reads overwrote valid per-field IFD0 fallbacks");

	bool canceledDuringRead = false;
	jpegview_linux::WorkContext canceledWhileReading;
	canceledWhileReading.shouldContinue = [&canceledDuringRead] {
		return !canceledDuringRead;
	};
	const jpegview_linux::TiffMetadataReadAt cancelDuringRead =
		[&readAt, &canceledDuringRead, exposureOffset](std::size_t offset,
			std::uint8_t* destination, std::size_t length) {
			const bool read = readAt(offset, destination, length);
			if (offset == exposureOffset) canceledDuringRead = true;
			return read;
		};
	info = {};
	Expect(!jpegview_linux::ReadTiffMetadataFromSource(tiffBytes.size(),
		cancelDuringRead, 0, info, canceledWhileReading) && canceledDuringRead,
		"TIFF parser reported successful metadata after cancellation during a value read");

	const fs::path filename = temporary.path() / "capture.dng";
	WriteBytes(filename, tiffBytes);
	info = {};
	Expect(jpegview_linux::ReadTiffMetadataFile(filename, info) && info.hasExif &&
		info.cameraModel == "Acme Model" && info.exposureTime == "1/125" &&
		info.isoSpeed == 200,
		"TIFF file reader did not seek to and parse metadata without an Exif APP1 wrapper");

	jpegview_linux::WorkContext canceled;
	canceled.shouldContinue = [] { return false; };
	info = {};
	Expect(!jpegview_linux::ReadTiffMetadataFromSource(logicalSourceSize, readAt, 0,
		info, canceled), "TIFF metadata parser ignored source cancellation");
}

void TestImageMetadataReaderDispatchesByContent() {
	TemporaryDirectory temporary;
	const std::vector<std::uint8_t> exifPayload =
		ExifFixture().Build(true, false, true);
	const std::vector<std::uint8_t> tiffBytes(exifPayload.begin() + 6,
		exifPayload.end());

	const fs::path dng = temporary.path() / "capture.dng";
	WriteBytes(dng, tiffBytes);
	jpegview_linux::ExifInfo info;
	std::string comment;
	Expect(jpegview_linux::ReadImageMetadata(dng, info, comment) &&
		info.hasExif && info.cameraModel == "Acme Model" &&
		info.exposureTime == "1/125" && info.isoSpeed == 200,
		"selected DNG metadata was not read from its TIFF content");
	const jpegview_linux::SourceDescriptor dngSource =
		jpegview_linux::DescribeImageSource(dng);
	jpegview_linux::ExifMetadataWorker metadataWorker;
	const std::uint64_t metadataGeneration = metadataWorker.Request(dngSource);
	const bool metadataWorkerIdle =
		metadataWorker.WaitUntilIdle(std::chrono::seconds(3));
	const std::vector<jpegview_linux::ExifMetadataResult> workerResults =
		metadataWorker.TakeReady();
	Expect(metadataGeneration != 0 && metadataWorkerIdle && workerResults.size() == 1 &&
		workerResults.front().generation == metadataGeneration &&
		workerResults.front().source == dngSource.Key() &&
		workerResults.front().metadataAvailable &&
		workerResults.front().metadata.cameraModel == "Acme Model",
		"default metadata worker did not route the selected DNG through TIFF parsing");

	const fs::path tiffNamedJpeg = temporary.path() / "capture.jpg";
	WriteBytes(tiffNamedJpeg, tiffBytes);
	info = {};
	comment.clear();
	Expect(jpegview_linux::ReadImageMetadata(tiffNamedJpeg, info, comment) &&
		info.hasExif && info.cameraModel == "Acme Model" &&
		info.exposureTime == "1/125",
		"metadata dispatch trusted a JPEG suffix over TIFF content");

	const fs::path jpegNamedDng = temporary.path() / "capture.dng";
	const fs::path jpegBase = temporary.path() / "base.jpg";
	ImageWriteOptions options;
	options.jpegQuality = 100;
	std::string error;
	const std::vector<std::uint8_t> pixels = TestPixels();
	Expect(jpegview_linux::WriteImage(jpegBase, pixels.data(), 2, 2, options, error),
		"cannot create mismatched-extension JPEG fixture: " + error);
	WriteBytes(jpegNamedDng, InsertJpegSegment(ReadBytes(jpegBase), 0xe1,
		ExifFixture().Build()));
	info = {};
	comment.clear();
	Expect(jpegview_linux::ReadImageMetadata(jpegNamedDng, info, comment) &&
		info.hasExif && info.cameraModel == "Acme Model",
		"metadata dispatch trusted a DNG suffix over JPEG content");

	const fs::path unknown = temporary.path() / "unknown.jpg";
	WriteBytes(unknown, {1, 2, 3, 4});
	info.hasExif = true;
	info.cameraModel = "stale metadata";
	comment = "stale comment";
	Expect(!jpegview_linux::ReadImageMetadata(unknown, info, comment) &&
		!info.hasExif && info.cameraModel.empty() && comment.empty(),
		"unsupported content retained metadata from the previous source");
}

std::vector<std::uint8_t> BuildPngWithExif(const std::vector<std::uint8_t>& tiff,
	bool animated, bool afterImageData = false) {
	std::vector<std::uint8_t> png = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	AppendPngChunk(png, "IHDR", {0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0});
	if (animated) {
		AppendPngChunk(png, "acTL", {0, 0, 0, 1, 0, 0, 0, 0});
		std::vector<std::uint8_t> frameControl;
		AppendBigEndian32(frameControl, 0);
		AppendBigEndian32(frameControl, 1);
		AppendBigEndian32(frameControl, 1);
		AppendBigEndian32(frameControl, 0);
		AppendBigEndian32(frameControl, 0);
		AppendBigEndian16(frameControl, 1);
		AppendBigEndian16(frameControl, 10);
		frameControl.push_back(0);
		frameControl.push_back(0);
		AppendPngChunk(png, "fcTL", frameControl);
	}
	if (!afterImageData) AppendPngChunk(png, "eXIf", tiff);
	const std::vector<std::uint8_t> scanline = {0, 0, 0, 0};
	uLongf compressedSize = compressBound(scanline.size());
	std::vector<std::uint8_t> compressed(compressedSize);
	Expect(compress2(compressed.data(), &compressedSize, scanline.data(),
		scanline.size(), Z_BEST_COMPRESSION) == Z_OK,
		"could not compress test PNG image data");
	compressed.resize(compressedSize);
	AppendPngChunk(png, "IDAT", compressed);
	if (afterImageData) AppendPngChunk(png, "eXIf", tiff);
	AppendPngChunk(png, "IEND", {});
	return png;
}

void TestPngAndApngExifMetadata() {
	TemporaryDirectory temporary;
	const std::vector<std::uint8_t> payload = ExifFixture().Build(true, false, true);
	const std::vector<std::uint8_t> tiff(payload.begin() + 6, payload.end());
	const fs::path pngPath = temporary.path() / "capture.png";
	WriteBytes(pngPath, BuildPngWithExif(tiff, false));
	jpegview_linux::ExifInfo info;
	std::string comment;
	Expect(jpegview_linux::ReadImageContentFormat(pngPath) ==
		jpegview_linux::ImageContentFormat::Png &&
		jpegview_linux::ReadImageMetadata(pngPath, info, comment) && info.hasExif &&
		info.cameraModel == "Acme Model" && info.exposureTime == "1/125" &&
		info.isoSpeed == 200,
		"PNG eXIf metadata was not dispatched and parsed from its TIFF payload");

	const jpegview_linux::SourceDescriptor pngSource =
		jpegview_linux::DescribeImageSource(pngPath);
	jpegview_linux::ExifMetadataWorker metadataWorker;
	const std::uint64_t generation = metadataWorker.Request(pngSource);
	const bool workerIdle = metadataWorker.WaitUntilIdle(std::chrono::seconds(3));
	const auto workerResults = metadataWorker.TakeReady();
	Expect(generation != 0 && workerIdle && workerResults.size() == 1 &&
		workerResults.front().generation == generation &&
		workerResults.front().source == pngSource.Key() &&
		workerResults.front().metadataAvailable &&
		workerResults.front().metadata.exposureTime == "1/125",
		"metadata worker did not publish the PNG eXIf fields for the selected source");

	const fs::path apngPath = temporary.path() / "animated.png";
	WriteBytes(apngPath, BuildPngWithExif(tiff, true));
	info = {};
	Expect(jpegview_linux::ReadImageContentFormat(apngPath) ==
		jpegview_linux::ImageContentFormat::Apng &&
		jpegview_linux::ReadImageMetadata(apngPath, info, comment) && info.hasExif &&
		info.cameraModel == "Acme Model",
		"APNG eXIf metadata was not read through the PNG metadata path");

	const fs::path afterImagePath = temporary.path() / "after-image-data.png";
	WriteBytes(afterImagePath, BuildPngWithExif(tiff, false, true));
	info = {};
	Expect(jpegview_linux::ReadImageMetadata(afterImagePath, info, comment) &&
		info.hasExif && info.cameraModel == "Acme Model",
		"PNG reader did not find eXIf after the image data chunk");

	std::vector<std::uint8_t> largeExif = tiff;
	largeExif.resize(300u * 1024u, 0);
	const fs::path largeExifPath = temporary.path() / "large-exif.png";
	WriteBytes(largeExifPath, BuildPngWithExif(largeExif, false));
	Expect(jpegview_linux::ReadImageContentFormat(largeExifPath) ==
		jpegview_linux::ImageContentFormat::Unknown &&
		jpegview_linux::ReadImageMetadata(largeExifPath, info, comment) &&
		info.hasExif && info.cameraModel == "Acme Model",
		"metadata dispatch did not recover a PNG whose ancillary chunk exceeded the format probe");

	auto badCrc = BuildPngWithExif(tiff, false);
	const std::array<std::uint8_t, 4> exifType = {'e', 'X', 'I', 'f'};
	const auto typePosition = std::search(badCrc.begin(), badCrc.end(),
		exifType.begin(), exifType.end());
	Expect(typePosition != badCrc.end(), "test PNG did not contain its eXIf chunk");
	const std::size_t typeOffset = static_cast<std::size_t>(typePosition - badCrc.begin());
	const std::uint32_t chunkLength = ReadBigEndian32(badCrc, typeOffset - 4);
	badCrc[typeOffset + 4 + chunkLength] ^= 1;
	const fs::path badCrcPath = temporary.path() / "bad-crc.png";
	WriteBytes(badCrcPath, badCrc);
	info = {};
	Expect(!jpegview_linux::ReadImageMetadata(badCrcPath, info, comment) && !info.hasExif,
		"PNG reader accepted eXIf metadata with a corrupt chunk CRC");

	const fs::path malformedPath = temporary.path() / "malformed.png";
	auto malformed = BuildPngWithExif(tiff, false);
	malformed[typeOffset - 4] = 0x7f;
	malformed[typeOffset - 3] = 0xff;
	malformed[typeOffset - 2] = 0xff;
	malformed[typeOffset - 1] = 0xff;
	WriteBytes(malformedPath, malformed);
	info.hasExif = true;
	info.cameraModel = "stale metadata";
	Expect(!jpegview_linux::ReadImageMetadata(malformedPath, info, comment) &&
		!info.hasExif && info.cameraModel.empty(),
		"PNG reader accepted an eXIf chunk extending beyond the file");
}

void AppendLittleEndian32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
	bytes.push_back(static_cast<std::uint8_t>(value >> 16));
	bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

void AppendWebpChunk(std::vector<std::uint8_t>& output, const std::string& type,
	const std::vector<std::uint8_t>& data) {
	Expect(type.size() == 4, "test WebP chunk type is not four bytes");
	output.insert(output.end(), type.begin(), type.end());
	AppendLittleEndian32(output, static_cast<std::uint32_t>(data.size()));
	output.insert(output.end(), data.begin(), data.end());
	if ((data.size() & 1u) != 0) output.push_back(0);
}

std::vector<std::uint8_t> BuildWebpWithExif(const std::vector<std::uint8_t>& tiff,
	bool withExifPrefix, bool includeOddChunk) {
	std::vector<std::uint8_t> webp = {
		'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
	AppendWebpChunk(webp, "VP8X", {0x08, 0, 0, 0, 1, 0, 0, 1, 0, 0});
	AppendWebpChunk(webp, "VP8 ", {0});
	if (includeOddChunk) AppendWebpChunk(webp, "JUNK", {0x5a});
	std::vector<std::uint8_t> exif;
	if (withExifPrefix) {
		exif.insert(exif.end(), {'E', 'x', 'i', 'f', 0, 0});
	}
	exif.insert(exif.end(), tiff.begin(), tiff.end());
	if ((exif.size() & 1u) == 0) exif.push_back(0);
	AppendWebpChunk(webp, "EXIF", exif);
	const std::uint32_t riffSize = static_cast<std::uint32_t>(webp.size() - 8);
	webp[4] = static_cast<std::uint8_t>(riffSize);
	webp[5] = static_cast<std::uint8_t>(riffSize >> 8);
	webp[6] = static_cast<std::uint8_t>(riffSize >> 16);
	webp[7] = static_cast<std::uint8_t>(riffSize >> 24);
	return webp;
}

void TestWebpExifMetadata() {
	TemporaryDirectory temporary;
	const std::vector<std::uint8_t> payload = ExifFixture().Build(true, false, true);
	const std::vector<std::uint8_t> tiff(payload.begin() + 6, payload.end());
	const fs::path wrongExtension = temporary.path() / "capture.image-data";
	WriteBytes(wrongExtension, BuildWebpWithExif(tiff, false, true));
	jpegview_linux::ExifInfo info;
	std::string comment;
	Expect(jpegview_linux::ReadImageContentFormat(wrongExtension) ==
		jpegview_linux::ImageContentFormat::WebP &&
		jpegview_linux::ReadImageMetadata(wrongExtension, info, comment) &&
		info.hasExif && info.cameraModel == "Acme Model" &&
		info.exposureTime == "1/125" && info.isoSpeed == 200,
		"WebP EXIF metadata was not selected by content for a file with the wrong suffix");

	const jpegview_linux::SourceDescriptor source =
		jpegview_linux::DescribeImageSource(wrongExtension);
	jpegview_linux::ExifMetadataWorker metadataWorker;
	const std::uint64_t generation = metadataWorker.Request(source);
	const bool workerIdle = metadataWorker.WaitUntilIdle(std::chrono::seconds(3));
	const auto workerResults = metadataWorker.TakeReady();
	Expect(generation != 0 && workerIdle && workerResults.size() == 1 &&
		workerResults.front().generation == generation &&
		workerResults.front().source == source.Key() &&
		workerResults.front().metadataAvailable &&
		workerResults.front().metadata.exposureTime == "1/125",
		"metadata worker did not publish WebP EXIF for the selected source");

	const fs::path prefixedExif = temporary.path() / "capture.webp";
	WriteBytes(prefixedExif, BuildWebpWithExif(tiff, true, false));
	info = {};
	Expect(jpegview_linux::ReadImageMetadata(prefixedExif, info, comment) &&
		info.hasExif && info.cameraModel == "Acme Model" &&
		info.focalLength == 50.0,
		"WebP reader did not accept an Exif-prefixed TIFF payload");

	auto malformedSize = BuildWebpWithExif(tiff, false, false);
	malformedSize[4] = 0xff;
	malformedSize[5] = 0xff;
	malformedSize[6] = 0xff;
	malformedSize[7] = 0x7f;
	const fs::path malformedSizePath = temporary.path() / "bad-container.webp";
	WriteBytes(malformedSizePath, malformedSize);
	info.hasExif = true;
	info.cameraModel = "stale metadata";
	Expect(!jpegview_linux::ReadImageMetadata(malformedSizePath, info, comment) &&
		!info.hasExif && info.cameraModel.empty(),
		"WebP reader accepted a RIFF size extending beyond the file");

	auto malformedChunk = BuildWebpWithExif(tiff, false, false);
	const std::array<std::uint8_t, 4> exifType = {'E', 'X', 'I', 'F'};
	const auto typePosition = std::search(malformedChunk.begin(), malformedChunk.end(),
		exifType.begin(), exifType.end());
	Expect(typePosition != malformedChunk.end(),
		"test WebP did not contain its EXIF chunk");
	const std::size_t typeOffset = static_cast<std::size_t>(
		typePosition - malformedChunk.begin());
	malformedChunk[typeOffset + 4] = 0xff;
	malformedChunk[typeOffset + 5] = 0xff;
	malformedChunk[typeOffset + 6] = 0xff;
	malformedChunk[typeOffset + 7] = 0xff;
	const fs::path malformedChunkPath = temporary.path() / "bad-chunk.webp";
	WriteBytes(malformedChunkPath, malformedChunk);
	info = {};
	Expect(!jpegview_linux::ReadImageMetadata(malformedChunkPath, info, comment) &&
		!info.hasExif,
		"WebP reader accepted an EXIF chunk extending beyond the RIFF container");
}

void TestFileOperationFolderExifDatesUpdatesOnlyRegularImages() {
	TemporaryDirectory temporary;
	const fs::path folder = temporary.path() / "images";
	fs::create_directories(folder);
	const fs::path jpeg = folder / "capture.jpg";
	const fs::path malformed = folder / "malformed.jpg";
	const fs::path imageNamedDirectory = folder / "directory.jpg";
	const fs::path nonImage = folder / "notes.txt";
	const std::vector<std::uint8_t> pixels = TestPixels();
	ImageWriteOptions options;
	options.jpegQuality = 100;
	std::string error;
	Expect(jpegview_linux::WriteImage(jpeg, pixels.data(), 2, 2, options, error),
		"cannot create folder EXIF fixture JPEG: " + error);
	WriteBytes(jpeg, InsertJpegSegment(ReadBytes(jpeg), 0xe1, ExifFixture().Build()));
	SetModificationTimeNanoseconds(jpeg, 1000000000, 0);
	WriteBytes(malformed, {0xff, 0xd8, 0xff, 0xe1, 0x00, 0x0a, 'E', 'x', 'i', 'f', 0, 0});
	SetModificationTimeNanoseconds(malformed, 1000000000, 0);
	fs::create_directories(imageNamedDirectory);
	WriteText(nonImage, "not an image");
	std::time_t expected = 0;
	Expect(jpegview_linux::ParseLocalExifTimestamp("2024:02:03 04:05:06", expected),
		"could not convert the fixture's EXIF timestamp");

	const auto result = jpegview_linux::ExecuteFileOperation(
		jpegview_linux::FileOperationPayload{
			jpegview_linux::TouchFolderExifDatesOperation{folder}}, 31,
		[] { return true; });
	struct stat updated{};
	Expect(result.success && result.updatedFiles == 1 && result.path == folder &&
		::stat(jpeg.c_str(), &updated) == 0 && updated.st_mtim.tv_sec == expected,
		"folder EXIF update did not change exactly the regular JPEG with a valid date");
	Expect(::stat(malformed.c_str(), &updated) == 0 &&
		updated.st_mtim.tv_sec == 1000000000 && fs::is_directory(imageNamedDirectory) &&
		fs::exists(nonImage),
		"folder EXIF update changed a malformed source, image-named directory, or non-image");
}
const TestCase kTests[] = {
	{"image-storage-transforms-and-validation", &TestImageStorageTransformsAndValidation},
	{"image-crop-copies-half-open-rectangle", &TestImageCropCopiesHalfOpenRectangle},
	{"crop-selection-model-geometry-and-manipulation", &TestCropSelectionModelGeometryAndManipulation},
	{"lossless-jpeg-crop-availability-policy", &TestLosslessJpegCropAvailabilityPolicy},
	{"crop-selection-view-mapping-and-hit-testing", &TestCropSelectionViewMappingAndHitTesting},
	{"image-resize-filters-and-limits", &TestImageResizeFiltersAndLimits},
	{"image-auto-contrast-invariants", &TestImageAutoContrastInvariants},
	{"picture-levels-model-and-processing", &TestPictureLevelsModelAndProcessing},
	{"color-cast-matches-scalar-pixel-oracle", &TestColorCastMatchesScalarPixelOracle},
	{"image-document-rejects-stale-operations-and-owns-current-pixels", &TestImageDocumentRejectsStaleOperationsAndOwnsCurrentPixels},
	{"image-operation-worker-runs-crop-and-flattens-captured-frame", &TestImageOperationWorkerRunsCropAndFlattensCapturedFrame},
	{"image-operation-worker-matches-transforms-and-pixel-pipelines", &TestImageOperationWorkerMatchesTransformsAndPixelPipelines},
	{"free-rotation-preview-bounds-large-source-pixels", &TestFreeRotationPreviewBoundsLargeSourcePixels},
	{"in-place-save-materializes-lazy-document-and-rebinds-source", &TestInPlaceSaveMaterializesLazyDocumentAndRebindsSource},
	{"image-operation-worker-supersedes-and-survives-failures", &TestImageOperationWorkerSupersedesAndSurvivesFailures},
	{"picture-levels-store-round-trip", &TestPictureLevelsStoreRoundTrip},
	{"settings-round-trip-and-malformed-values", &TestSettingsRoundTripAndMalformedValues},
	{"runtime-settings-owner-commit-semantics", &TestRuntimeSettingsOwnerCommitSemantics},
	{"advanced-configuration-model-categories-and-round-trips", &TestAdvancedConfigurationModelCategoriesAndRoundTrips},
	{"advanced-configuration-model-validation-and-cancellation", &TestAdvancedConfigurationModelValidationAndCancellation},
	{"transparency-pattern-values-and-tile-colors", &TestTransparencyPatternValuesAndTileColors},
	{"settings-path-selection", &TestSettingsPathSelection},
	{"sort-mode-mappings", &TestSortModeMappings},
	{"batch-copy-pattern-expansion-and-preview", &TestBatchCopyPatternExpansionAndPreview},
	{"batch-copy-dialog-controller", &TestBatchCopyDialogController},
	{"external-process-structured-arguments-and-failures", &TestExternalProcessStructuredArgumentsAndFailures},
	{"external-process-cancellation-reaps-slow-child", &TestExternalProcessCancellationReapsSlowChild},
	{"external-process-cancellation-kills-descendants", &TestExternalProcessCancellationKillsDescendants},
	{"file-operation-save-and-batch-policies", &TestFileOperationSaveAndBatchPolicies},
	{"image-save-does-not-replace-unconfirmed-late-target", &TestImageSaveDoesNotReplaceUnconfirmedLateTarget},
	{"batch-cancellation-stops-between-files", &TestBatchCancellationStopsBetweenFiles},
	{"batch-copy-temporary-remains-private-until-publication", &TestBatchCopyTemporaryRemainsPrivateUntilPublication},
	{"file-operation-clipboard-temporary-cleanup-and-fallback", &TestFileOperationClipboardTemporaryCleanupAndFallback},
	{"lossless-operations-publish-only-successful-temporary-outputs", &TestLosslessOperationsPublishOnlySuccessfulTemporaryOutputs},
	{"lossless-crop-publication-waits-for-destination-admission", &TestLosslessCropPublicationWaitsForDestinationAdmission},
	{"file-operation-service-publishes-completions-and-resumes", &TestFileOperationServicePublishesCompletionsAndResumes},
	{"file-operation-service-completed-irreversible-work-survives-cancel", &TestFileOperationServiceReportsCompletedIrreversibleWorkAfterCancel},
	{"file-operation-service-stop-cancels-child-without-event-loop", &TestFileOperationServiceStopCancelsChildWithoutEventLoop},
	{"desktop-application-parsing-and-expansion", &TestDesktopApplicationParsingAndExecExpansion},
	{"default-viewer-registration", &TestDefaultViewerRegistration},
	{"external-command-planning", &TestExternalCommandPlanning},
	{"gps-map-provider-url-validation-and-coordinate-formatting", &TestGpsMapProviderUrlValidationAndCoordinateFormatting},
	{"exif-and-jpeg-comment-parsing", &TestExifAndJpegCommentParsing},
	{"exif-ifd0-shooting-field-fallback", &TestExifIfd0ShootingFieldFallback},
	{"tiff-metadata-bounded-random-access", &TestTiffMetadataReaderBoundedRandomAccess},
	{"image-metadata-reader-dispatches-by-content", &TestImageMetadataReaderDispatchesByContent},
	{"png-and-apng-exif-metadata", &TestPngAndApngExifMetadata},
	{"webp-exif-metadata", &TestWebpExifMetadata},
	{"file-operation-folder-exif-dates-update-only-regular-images", &TestFileOperationFolderExifDatesUpdatesOnlyRegularImages},
};

} // namespace

const TestSuite& GetImageOperationsSuite() {
	static const TestSuite suite{"image_operations", kTests, sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
