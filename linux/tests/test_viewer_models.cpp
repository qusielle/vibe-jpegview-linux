#include "test_harness.h"
#include "test_support.h"
#include "free_rotation_model.h"
#include "perspective_correction_model.h"
#include "perspective_dialog_layout.h"

#include <limits>

namespace {

void TestViewportModesAndGeometry() {
	jpegview_linux::Viewport viewport;
	Expect(std::string(viewport.ScaleMode()) == "fit_no_enlarge", "viewport default scale mode changed");

	viewport.Fit(100, 50, 500, 300);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "fit-no-enlarge scaled a small image up");
	ExpectRect(viewport.Destination(100, 50, 500, 300), 200, 125, 100, 50,
		"small image was not centered at original size");

	viewport.Fit(1000, 500, 500, 300);
	ExpectNear(viewport.Zoom(), 0.5, 0.0001, "large image fit used the wrong scale");
	ExpectRect(viewport.Destination(1000, 500, 500, 300), 0, 25, 500, 250,
		"fitted image geometry is incorrect");

	viewport.Fit(1000, 500, 500, 300, true, false);
	Expect(std::string(viewport.ScaleMode()) == "fill", "fill mode was not recorded");
	ExpectNear(viewport.Zoom(), 0.6, 0.0001, "fill mode used the wrong scale");
	ExpectRect(viewport.Destination(1000, 500, 500, 300), -50, 0, 600, 300,
		"fill-and-crop geometry is incorrect");

	viewport.Fit(1000, 600, 500, 300);
	ExpectRect(viewport.Destination(1000, 600, 500, 300), 0, 0, 500, 300,
		"matching-aspect oversized image retained an artificial border");

	viewport.LoadScaleMode("fit", false, 1.0);
	Expect(viewport.IsFitToWindow() && !viewport.NoEnlarge(), "fit mode incorrectly prevents enlargement");
	Expect(std::string(viewport.ScaleMode()) == "fit", "fit mode did not round-trip");
	viewport.LoadScaleMode("fill_no_enlarge", false, 1.0);
	Expect(viewport.FillWithCrop() && viewport.NoEnlarge(), "fill-no-enlarge mode did not load");
	viewport.LoadScaleMode("unknown", false, 1.0);
	Expect(std::string(viewport.ScaleMode()) == "fit_no_enlarge", "unknown scale mode did not use safe default");
}

void TestFreeRotationDialogControllerTracksPreviewAndApplyOwnership() {
	using Controller = jpegview_linux::FreeRotationDialogController;
	Controller controller;
	Expect(!controller.Open(0, 10) && !controller.IsOpen(),
		"free-rotation dialog accepted invalid source dimensions");
	Expect(controller.Open(1200, 800) && controller.IsOpen() &&
		controller.CurrentPhase() == Controller::Phase::Editing &&
		controller.SourceWidth() == 1200 && controller.SourceHeight() == 800,
		"free-rotation dialog did not open for the selected image");
	const std::uint64_t firstSession = controller.SessionId();
	const std::uint64_t initialRevision = controller.PreviewRevision();
	Expect(firstSession != 0 && initialRevision != 0 &&
		controller.Parameters().clockwiseDegrees == 0.0 &&
		controller.Parameters().autoCrop &&
		controller.Parameters().preserveAspectRatio &&
		controller.Parameters().showGrid &&
		controller.MatchesPreview(firstSession, initialRevision),
		"free-rotation defaults or initial preview identity changed");

	Expect(controller.SetAngleDegrees(12.5) &&
		!controller.MatchesPreview(firstSession, initialRevision),
		"editing the angle did not invalidate the previous preview revision");
	const std::uint64_t changedRevision = controller.PreviewRevision();
	Expect(!controller.SetAngleDegrees(12.5) &&
		controller.PreviewRevision() == changedRevision &&
		!controller.SetAngleDegrees(std::numeric_limits<double>::infinity()) &&
		controller.SetAutoCrop(false) && controller.SetPreserveAspectRatio(false) &&
		controller.SetGridVisible(false),
		"free-rotation edits did not preserve finite values and revision ordering");
	Expect(controller.NudgeAngle(500.0) &&
		controller.Parameters().clockwiseDegrees == 180.0,
		"free-rotation angle nudges did not clamp at the supported slider limit");
	Expect(controller.BeginApply() && controller.IsApplying() &&
		!controller.MatchesPreview(firstSession, controller.PreviewRevision()) &&
		!controller.SetAngleDegrees(25.0) && !controller.BeginApply(),
		"applying rotation did not freeze settings and invalidate pending preview work");
	controller.ResumeEditing("temporary upload failure");
	Expect(!controller.IsApplying() && controller.IsOpen() &&
		controller.Message() == "temporary upload failure",
		"failed application did not restore the editable dialog state");
	controller.SetMessage("preview ready");
	Expect(controller.Message() == "preview ready" &&
		controller.BeginApply(), "free-rotation dialog did not accept a later retry");
	controller.CompleteApply();
	Expect(!controller.IsOpen() && controller.CurrentPhase() == Controller::Phase::Closed,
		"successful apply did not close the free-rotation session");
	Expect(controller.Open(40, 20) && controller.SessionId() != firstSession &&
		!controller.MatchesPreview(firstSession, initialRevision),
		"reopened free-rotation session accepted a completion from its previous owner");
	controller.Close();
}

void TestPerspectiveCorrectionDialogControllerTracksPreviewAndApplyOwnership() {
	using Controller = jpegview_linux::PerspectiveCorrectionDialogController;
	Controller controller;
	Expect(!controller.Open(1, 300) && !controller.IsOpen(),
		"perspective dialog accepted a source axis too narrow for projective correction");
	Expect(controller.Open(1600, 900) && controller.IsOpen() &&
		controller.CurrentPhase() == Controller::Phase::Editing &&
		controller.SourceWidth() == 1600 && controller.SourceHeight() == 900,
		"perspective dialog did not open for a valid source");
	const std::uint64_t firstSession = controller.SessionId();
	const std::uint64_t initialRevision = controller.PreviewRevision();
	Expect(firstSession != 0 && initialRevision != 0 &&
		controller.Parameters().transform.leftDeltaFraction == 0.0 &&
		controller.Parameters().transform.rightDeltaFraction == 0.0 &&
		controller.Parameters().transform.autoCrop &&
		!controller.Parameters().transform.preserveAspectRatio &&
		controller.Parameters().showGrid &&
		controller.MatchesPreview(firstSession, initialRevision),
		"perspective dialog defaults or initial preview identity changed");

	Expect(controller.SetLeftDeltaFraction(0.1) &&
		!controller.MatchesPreview(firstSession, initialRevision),
		"left-edge editing did not invalidate the previous preview revision");
	const std::uint64_t leftRevision = controller.PreviewRevision();
	Expect(!controller.SetLeftDeltaFraction(0.1) &&
		controller.PreviewRevision() == leftRevision &&
		!controller.SetLeftDeltaFraction(std::numeric_limits<double>::infinity()) &&
		controller.SetRightDeltaFraction(-0.1) &&
		controller.SetAutoCrop(false) &&
		controller.SetPreserveAspectRatio(true) &&
		controller.SetGridVisible(false),
		"perspective edits did not preserve finite inputs or revision ordering");
	Expect(controller.SetLeftDeltaFraction(0.9) &&
		controller.Parameters().transform.leftDeltaFraction ==
			jpegview_linux::kMaximumPerspectiveCorrectionFraction &&
		controller.SetRightDeltaFraction(-0.9) &&
		controller.Parameters().transform.rightDeltaFraction ==
			-jpegview_linux::kMaximumPerspectiveCorrectionFraction,
		"perspective edge movement exceeded the geometry's supported bounds");
	const double beforeInvalidNudge = controller.Parameters().transform.leftDeltaFraction;
	Expect(!controller.NudgeLeftDelta(std::numeric_limits<double>::quiet_NaN()) &&
		controller.Parameters().transform.leftDeltaFraction == beforeInvalidNudge &&
		controller.NudgeLeftDelta(-1.0) &&
		controller.Parameters().transform.leftDeltaFraction ==
			-jpegview_linux::kMaximumPerspectiveCorrectionFraction,
		"perspective nudges accepted nonfinite values or failed to clamp");

	Expect(controller.BeginApply() && controller.IsApplying() &&
		!controller.MatchesPreview(firstSession, controller.PreviewRevision()) &&
		!controller.SetRightDeltaFraction(0.2) && !controller.BeginApply(),
		"applying perspective correction did not freeze parameters and preview work");
	controller.ResumeEditing("temporary upload failure");
	Expect(!controller.IsApplying() && controller.IsOpen() &&
		controller.Message() == "temporary upload failure",
		"failed perspective application did not restore editable dialog state");
	controller.SetMessage("preview ready");
	Expect(controller.Message() == "preview ready" && controller.BeginApply(),
		"perspective dialog did not accept a later apply retry");
	controller.CompleteApply();
	Expect(!controller.IsOpen() && controller.CurrentPhase() == Controller::Phase::Closed,
		"successful perspective correction did not close its editor session");
	Expect(controller.Open(40, 20) && controller.SessionId() != firstSession &&
		!controller.MatchesPreview(firstSession, initialRevision),
		"reopened perspective session accepted a completion from its former owner");
	controller.Close();
}

void TestPerspectiveCorrectionDialogLayoutAdaptsToSmallWindows() {
	using namespace jpegview_linux;
	const auto fitsWindow = [](const SDL_Rect& rect, int width, int height) {
		return rect.x >= 0 && rect.y >= 0 && rect.w > 0 && rect.h > 0 &&
			rect.x + rect.w <= width && rect.y + rect.h <= height;
	};

	const PerspectiveCorrectionDialogLayout compact =
		BuildPerspectiveCorrectionDialogLayout(640, 240);
	Expect(compact.compact && compact.dialog.x == 16 && compact.dialog.y == 8 &&
		compact.dialog.w == 608 && compact.dialog.h == 224 &&
		!compact.veryCompact,
		"perspective editor did not use the available area in a compact window");
	Expect(fitsWindow(compact.dialog, 640, 240),
		"compact perspective dialog escaped the resized window");
	for (const SDL_Rect& rect : compact.sliders) {
		Expect(fitsWindow(rect, 640, 240),
			"compact perspective slider escaped the resized window");
	}
	for (const SDL_Rect& rect : compact.toggles) {
		Expect(fitsWindow(rect, 640, 240),
			"compact perspective toggle escaped the resized window");
	}
	for (const SDL_Rect& rect : compact.buttons) {
		Expect(fitsWindow(rect, 640, 240),
			"compact perspective action button escaped the resized window");
	}
	Expect(compact.sliders[0].y + compact.sliders[0].h < compact.sliders[1].y &&
		compact.toggles[0].y == compact.toggles[1].y &&
		compact.toggles[1].y == compact.toggles[2].y &&
		compact.toggles[0].x + compact.toggles[0].w < compact.toggles[1].x &&
		compact.toggles[1].x + compact.toggles[1].w < compact.toggles[2].x,
		"compact perspective controls overlap instead of fitting on separate rows");
	Expect(compact.message.y >= compact.toggles[0].y + compact.toggles[0].h &&
		compact.message.y + compact.message.h < compact.buttons[0].y &&
		compact.buttons[0].x + compact.buttons[0].w < compact.buttons[1].x &&
		compact.helpY >= compact.dialog.y &&
		compact.helpY + 8 <= compact.dialog.y + compact.dialog.h,
		"compact perspective status, help, or actions do not fit in the dialog");

	const PerspectiveCorrectionDialogLayout resized =
		BuildPerspectiveCorrectionDialogLayout(1024, 768);
	Expect(!resized.compact && fitsWindow(resized.dialog, 1024, 768) &&
		resized.dialog.w == 620 && resized.dialog.h == 390,
		"perspective dialog did not restore its normal layout after window growth");
	Expect(resized.helpY >= resized.toggles[2].y + resized.toggles[2].h &&
		resized.message.y >= resized.toggles[2].y + resized.toggles[2].h,
		"normal perspective status and help overlap the option controls");

	const PerspectiveCorrectionDialogLayout dense =
		BuildPerspectiveCorrectionDialogLayout(640, 200);
	Expect(dense.compact && dense.message.h == 0 && dense.helpY == -1 &&
		fitsWindow(dense.buttons[0], 640, 200) &&
		fitsWindow(dense.buttons[1], 640, 200),
		"dense perspective layout hid required buttons or drew status outside its space");

	struct WindowCase {
		int width;
		int height;
		bool veryCompact;
		bool showHelp;
	};
	const WindowCase cases[] = {
		{640, 154, true, false},
		{640, 157, true, false},
		{640, 158, false, false},
		{640, 160, false, false},
		{640, 200, false, false},
		{320, 240, false, false},
		{160, 120, true, false},
	};
	for (const WindowCase& window : cases) {
		const PerspectiveCorrectionDialogLayout layout =
			BuildPerspectiveCorrectionDialogLayout(window.width, window.height);
		Expect(layout.veryCompact == window.veryCompact &&
			fitsWindow(layout.dialog, window.width, window.height),
			"perspective layout escaped or selected the wrong mode for a boundary window");
		for (const SDL_Rect& rect : layout.sliders) {
			Expect(fitsWindow(rect, window.width, window.height),
				"perspective slider escaped a supported small window");
		}
		for (const SDL_Rect& rect : layout.toggles) {
			Expect(fitsWindow(rect, window.width, window.height),
				"perspective option escaped a supported small window");
		}
		for (const SDL_Rect& rect : layout.buttons) {
			Expect(fitsWindow(rect, window.width, window.height),
				"perspective action escaped a supported small window");
		}
		Expect(layout.sliders[0].y + layout.sliders[0].h <= layout.sliders[1].y &&
			layout.sliders[1].y + layout.sliders[1].h <= layout.toggles[0].y &&
			layout.toggles[0].y == layout.toggles[1].y &&
			layout.toggles[1].y == layout.toggles[2].y &&
			layout.toggles[0].x + layout.toggles[0].w < layout.toggles[1].x &&
			layout.toggles[1].x + layout.toggles[1].w < layout.toggles[2].x &&
			layout.toggles[0].y + layout.toggles[0].h <= layout.buttons[0].y &&
			layout.buttons[0].x + layout.buttons[0].w < layout.buttons[1].x,
			"perspective controls overlap in a supported small window");
		Expect((layout.helpY >= 0) == window.showHelp &&
			(layout.helpY < 0 || layout.helpY + 8 <=
				layout.dialog.y + layout.dialog.h),
			"perspective help text does not fit the selected compact layout");
	}
}

void TestViewportManualZoomPanAndRestore() {
	jpegview_linux::Viewport viewport;
	viewport.LoadScaleMode("manual", true, 100.0);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "persisted transient zoom was restored instead of actual size");
	viewport.LoadScaleMode("manual", true, 0.0001);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "persisted transient zoom replaced actual size");

	viewport.ActualSize();
	Expect(viewport.IsActualSize(), "actual-size viewport was not identified as actual size");
	viewport.ZoomAt(2.0, 150, 75, 200, 100, 400, 200);
	Expect(!viewport.IsActualSize(), "zoomed viewport was incorrectly identified as actual size");
	ExpectNear(viewport.Zoom(), 2.0, 0.0001, "anchored zoom did not update scale");
	ExpectRect(viewport.Destination(200, 100, 400, 200), 50, 25, 400, 200,
		"anchored zoom did not preserve the point beneath the pointer");
	viewport.Pan(10.0, -5.0);
	ExpectRect(viewport.Destination(200, 100, 400, 200), 60, 20, 400, 200,
		"viewport pan did not update the destination");
	Expect(std::string(viewport.ScaleMode()) == "manual", "zoomed or panned viewport was not manual");
	const jpegview_linux::ViewportSnapshot manual = viewport.Snapshot();
	viewport.ActualSize();
	viewport.Pan(48.0, -48.0);
	Expect(viewport.IsActualSize() && viewport.OffsetX() == 48.0 && viewport.OffsetY() == -48.0,
		"actual-size keyboard-style pan did not preserve scale or update offsets");
	viewport.ActualSize();
	viewport.Pan(10000.0, 10000.0);
	viewport.ClampToView(1000, 600, 500, 300);
	ExpectRect(viewport.Destination(1000, 600, 500, 300), 0, 0, 1000, 600,
		"viewport clamping did not retain the top-left image edge");
	viewport.Pan(-20000.0, -20000.0);
	viewport.ClampToView(1000, 600, 500, 300);
	ExpectRect(viewport.Destination(1000, 600, 500, 300), -500, -300, 1000, 600,
		"viewport clamping did not retain the bottom-right image edge");
	viewport.Pan(100.0, 100.0);
	viewport.ClampToView(100, 50, 500, 300);
	ExpectRect(viewport.Destination(100, 50, 500, 300), 200, 125, 100, 50,
		"viewport clamping did not center image axes smaller than the view");

	viewport.Fit(800, 600, 400, 300);
	viewport.Restore(manual, 320, 200, 640, 480);
	ExpectNear(viewport.Zoom(), 2.0, 0.0001, "manual snapshot did not restore zoom");
	ExpectNear(viewport.OffsetX(), 0.0, 0.0001, "manual restore did not reset horizontal pan");
	ExpectNear(viewport.OffsetY(), 0.0, 0.0001, "manual restore did not reset vertical pan");

	viewport.Fit(800, 600, 400, 300, false, true);
	const jpegview_linux::ViewportSnapshot fitted = viewport.Snapshot();
	viewport.ActualSize();
	viewport.Restore(fitted, 1000, 500, 500, 300);
	ExpectNear(viewport.Zoom(), 0.5, 0.0001, "fit snapshot did not recompute for new geometry");
	Expect(viewport.IsFitToWindow() && viewport.NoEnlarge(), "fit snapshot flags were not restored");

	const double zoomBeforeInvalidInput = viewport.Zoom();
	viewport.ZoomAt(2.0, 0, 0, 0, 100, 500, 300);
	viewport.ZoomAt(-1.0, 0, 0, 100, 100, 500, 300);
	ExpectNear(viewport.Zoom(), zoomBeforeInvalidInput, 0.0001, "invalid zoom input changed viewport state");
}

void TestZoomNavigatorGeometryAndPanning() {
	using namespace jpegview_linux;
	const ZoomNavigatorLayout layout = CalculateZoomNavigatorLayout(1600, 1200,
		0, 0, 1280, 800);
	Expect(layout.hotArea.x == 976 && layout.hotArea.y == 8 &&
		layout.hotArea.width == 296 && layout.hotArea.height == 222 &&
		layout.image.x == layout.hotArea.x && layout.image.y == layout.hotArea.y &&
		layout.image.width == 296 && layout.image.height == 222,
		"zoom navigator did not use its responsive corner layout and image aspect ratio");
	const ZoomNavigatorLayout portrait = CalculateZoomNavigatorLayout(900, 1600,
		10, 5, 1280, 800);
	Expect(portrait.image.height == portrait.hotArea.height &&
		portrait.image.width < portrait.hotArea.width &&
		portrait.image.x > portrait.hotArea.x,
		"portrait overview was not fitted and centered inside its hot area");
	Expect(ImageNeedsZoomNavigator(1600, 1200, 1280, 800) &&
		!ImageNeedsZoomNavigator(800, 600, 1280, 800) &&
		!ImageNeedsZoomNavigator(0, 600, 1280, 800),
		"navigator visibility did not follow image overflow");

	const NormalizedImageRect visible = CalculateVisibleImageRect(
		-160, -200, 1600, 1200, 0, 0, 1280, 800);
	Expect(visible.valid, "zoomed image viewport was not mapped onto the source image");
	ExpectNear(visible.left, 0.1, 0.0001, "navigator viewport left edge is incorrect");
	ExpectNear(visible.top, 1.0 / 6.0, 0.0001, "navigator viewport top edge is incorrect");
	ExpectNear(visible.right, 0.9, 0.0001, "navigator viewport right edge is incorrect");
	ExpectNear(visible.bottom, 5.0 / 6.0, 0.0001, "navigator viewport bottom edge is incorrect");
	const ZoomNavigatorRect mapped = MapVisibleRectToNavigator(visible, layout.image);
	Expect(mapped.x == 1006 && mapped.y == 45 && mapped.width == 236 && mapped.height == 148,
		"navigator visible rectangle was mapped to the wrong thumbnail pixels");
	const ZoomNavigatorPoint mappedPoint = NavigatorPointToImage(1035, 119, layout.image);
	ExpectNear(mappedPoint.x, 59.0 / 296.0, 0.0001,
		"navigator click did not map to the source-image horizontal fraction");
	ExpectNear(mappedPoint.y, 0.5, 0.0001,
		"navigator click did not map to the source-image vertical fraction");

	const ZoomNavigatorPan centering = CalculateNavigatorCenterPan(
		0.5, 0.5, 0.1, 0.5, 0.8, 2.0 / 3.0, 1600, 1200);
	ExpectNear(centering.x, 160.0, 0.0001,
		"navigator click did not clamp the requested center to the visible image bounds");
	ExpectNear(centering.y, 0.0, 0.0001,
		"navigator click moved an axis whose complete image was already visible");
	const ZoomNavigatorPan drag = CalculateNavigatorDragPan(90, -20,
		layout.image, 1600, 1200);
	ExpectNear(drag.x, -90.0 * 1600.0 / 296.0, 0.0001,
		"navigator drag did not scale pan distance by the image overview");
	ExpectNear(drag.y, 20.0 * 1200.0 / 222.0, 0.0001,
		"navigator drag did not scale vertical pan distance by the overview");
	Expect(!CalculateVisibleImageRect(3000, 0, 1600, 1200, 0, 0, 1280, 800).valid &&
		MapVisibleRectToNavigator({}, layout.image).width == 0 &&
		NavigatorPointToImage(0, 0, {}).x == 0.0 &&
		CalculateNavigatorDragPan(10, 10, {}, 1600, 1200).x == 0.0,
		"navigator geometry accepted invalid or fully off-screen input");
}

void TestMagnifyingGlassModelDefaultsBoundsAndWheelDirections() {
	using namespace jpegview_linux;
	MagnifyingGlassModel model;
	Expect(!model.Enabled() && model.Width() == MagnifyingGlassModel::kDefaultWidth &&
		model.Height() == MagnifyingGlassModel::kDefaultHeight &&
		model.ZoomLevel() == MagnifyingGlassModel::kDefaultZoomLevel,
		"magnifying glass did not start with its transient defaults");
	MagnifyingGlassModel restored;
	restored.SetParameters(425, 215, 0.725, 1200, 800);
	Expect(!restored.Enabled() && restored.Width() == 425 && restored.Height() == 215 &&
		std::abs(restored.ZoomLevel() - 0.725) < 1e-12,
		"magnifying-glass parameters did not restore independently of enabled state");
	restored.SetParameters(1, 99999, std::numeric_limits<double>::quiet_NaN(), 400, 200);
	Expect(restored.Width() == MagnifyingGlassModel::kMinimumWidth && restored.Height() == 180 &&
		restored.ZoomLevel() == MagnifyingGlassModel::kDefaultZoomLevel,
		"restored magnifying-glass parameters were not validated and constrained to the view");
	model.Toggle();
	Expect(model.Enabled(), "magnifying glass toggle did not enable the lens");
	model.SetEnabled(false);
	Expect(!model.Enabled(), "magnifying glass enabled state could not be cleared");

	const MagnifyingGlassWheelModifiers plain;
	model.HandleWheel(MagnifyingGlassWheelDirection::Down, plain, 1200, 800);
	Expect(model.Width() == 380 && model.Height() == 190,
		"plain wheel down did not grow both lens dimensions");
	model.HandleWheel(MagnifyingGlassWheelDirection::Up, plain, 1200, 800);
	Expect(model.Width() == 350 && model.Height() == 175,
		"plain wheel up did not shrink both lens dimensions");

	MagnifyingGlassWheelModifiers control;
	control.control = true;
	model.HandleWheel(MagnifyingGlassWheelDirection::Down, control, 1200, 800);
	Expect(model.Width() == 350 && model.Height() == 190,
		"Ctrl+wheel changed a dimension other than lens height");
	model.HandleWheel(MagnifyingGlassWheelDirection::Up, control, 1200, 800);
	MagnifyingGlassWheelModifiers alt;
	alt.alt = true;
	model.HandleWheel(MagnifyingGlassWheelDirection::Down, alt, 1200, 800);
	Expect(model.Width() == 380 && model.Height() == 175,
		"Alt+wheel changed a dimension other than lens width");
	model.HandleWheel(MagnifyingGlassWheelDirection::Up, alt, 1200, 800);

	MagnifyingGlassWheelModifiers shift;
	shift.shift = true;
	model.HandleWheel(MagnifyingGlassWheelDirection::Down, shift, 1200, 800);
	ExpectNear(model.ZoomLevel(), 0.525, 0.000001,
		"Shift+wheel down did not increase the zoom-level value");
	model.HandleWheel(MagnifyingGlassWheelDirection::Up, shift, 1200, 800);
	ExpectNear(model.ZoomLevel(), MagnifyingGlassModel::kDefaultZoomLevel, 0.000001,
		"Shift+wheel up did not decrease the zoom-level value");
	for (int i = 0; i < 40; ++i) {
		model.HandleWheel(MagnifyingGlassWheelDirection::Down, shift, 1200, 800);
	}
	ExpectNear(model.ZoomLevel(), MagnifyingGlassModel::kMaximumZoomLevel, 0.000001,
		"zoom level exceeded its upper bound");
	for (int i = 0; i < 40; ++i) {
		model.HandleWheel(MagnifyingGlassWheelDirection::Up, shift, 1200, 800);
	}
	ExpectNear(model.ZoomLevel(), MagnifyingGlassModel::kMinimumZoomLevel, 0.000001,
		"zoom level exceeded its lower bound");

	MagnifyingGlassModel bounded;
	for (int i = 0; i < 40; ++i) {
		bounded.HandleWheel(MagnifyingGlassWheelDirection::Down, plain, 400, 200);
	}
	Expect(bounded.Width() == 360 && bounded.Height() == 180,
		"lens dimensions exceeded 90 percent of feasible parent dimensions");
	for (int i = 0; i < 40; ++i) {
		bounded.HandleWheel(MagnifyingGlassWheelDirection::Up, plain, 400, 200);
	}
	Expect(bounded.Width() == MagnifyingGlassModel::kMinimumWidth &&
		bounded.Height() == MagnifyingGlassModel::kMinimumHeight,
		"lens dimensions fell below their minimums");
	for (int i = 0; i < 40; ++i) {
		bounded.HandleWheel(MagnifyingGlassWheelDirection::Down, plain, 100, 50);
	}
	Expect(bounded.Width() == MagnifyingGlassModel::kMinimumWidth &&
		bounded.Height() == MagnifyingGlassModel::kMinimumHeight,
		"small parent dimensions incorrectly forced the lens below its minimum size");
}

void TestMagnifyingGlassGeometryMapsNativeTextureAndPadsEdges() {
	using namespace jpegview_linux;
	const MagnifyingGlassRect displayedImage{100, 50, 500, 250};
	const MagnifyingGlassGeometry centered = CalculateMagnifyingGlassGeometry(
		350.0, 175.0, displayedImage, 1000, 500, 350, 175,
		MagnifyingGlassModel::kDefaultZoomLevel);
	Expect(centered.valid, "magnifying glass rejected a pointer inside the displayed image");
	Expect(centered.lensRect.x == 175 && centered.lensRect.y == 88 &&
		centered.lensRect.width == 350 && centered.lensRect.height == 175,
		"magnifying glass lens was not centered on the pointer");
	Expect(centered.sourceRect.x == 325 && centered.sourceRect.y == 162 &&
		centered.sourceRect.width == 350 && centered.sourceRect.height == 176,
		"2x default lens crop did not map through the displayed-image scale");
	Expect(centered.contentDestinationRect.x == centered.lensRect.x &&
		centered.contentDestinationRect.y == centered.lensRect.y &&
		centered.contentDestinationRect.width == centered.lensRect.width &&
		centered.contentDestinationRect.height == centered.lensRect.height,
		"unclipped source crop did not fill the lens content area");

	const MagnifyingGlassGeometry scaledTexture = CalculateMagnifyingGlassGeometry(
		350.0, 175.0, displayedImage, 2000, 1000, 350, 175,
		MagnifyingGlassModel::kDefaultZoomLevel);
	Expect(scaledTexture.valid && scaledTexture.sourceRect.x == 650 &&
		scaledTexture.sourceRect.y == 325 && scaledTexture.sourceRect.width == 700 &&
		scaledTexture.sourceRect.height == 350,
		"native source crop did not account for texture-to-destination scaling");

	const MagnifyingGlassRect fullImage{0, 0, 1000, 500};
	const MagnifyingGlassGeometry left = CalculateMagnifyingGlassGeometry(
		0.0, 250.0, fullImage, 1000, 500, 350, 175, 0.5);
	const MagnifyingGlassGeometry right = CalculateMagnifyingGlassGeometry(
		999.0, 250.0, fullImage, 1000, 500, 350, 175, 0.5);
	const MagnifyingGlassGeometry top = CalculateMagnifyingGlassGeometry(
		500.0, 0.0, fullImage, 1000, 500, 350, 175, 0.5);
	const MagnifyingGlassGeometry bottom = CalculateMagnifyingGlassGeometry(
		500.0, 499.0, fullImage, 1000, 500, 350, 175, 0.5);
	Expect(left.valid && left.sourceRect.x == 0 && left.sourceRect.width < 350 &&
		left.contentDestinationRect.x > left.lensRect.x &&
		left.contentDestinationRect.x + left.contentDestinationRect.width <=
		left.lensRect.x + left.lensRect.width,
		"left source edge was not clipped and padded inside the lens");
	Expect(right.valid && right.sourceRect.x + right.sourceRect.width == 1000 &&
		right.contentDestinationRect.x == right.lensRect.x &&
		right.contentDestinationRect.x + right.contentDestinationRect.width <
		right.lensRect.x + right.lensRect.width,
		"right source edge was not clipped and padded inside the lens");
	Expect(top.valid && top.sourceRect.y == 0 && top.sourceRect.height < 176 &&
		top.contentDestinationRect.y > top.lensRect.y &&
		top.contentDestinationRect.y + top.contentDestinationRect.height <=
		top.lensRect.y + top.lensRect.height,
		"top source edge was not clipped and padded inside the lens");
	Expect(bottom.valid && bottom.sourceRect.y + bottom.sourceRect.height == 500 &&
		bottom.contentDestinationRect.y == bottom.lensRect.y &&
		bottom.contentDestinationRect.y + bottom.contentDestinationRect.height <
		bottom.lensRect.y + bottom.lensRect.height,
		"bottom source edge was not clipped and padded inside the lens");
	Expect(!CalculateMagnifyingGlassGeometry(1000.0, 100.0, fullImage, 1000, 500,
		350, 175, 0.5).valid &&
		!CalculateMagnifyingGlassGeometry(0.0, 250.0, fullImage, 1000, 500,
		350, 175, 0.1).valid,
		"magnifying glass geometry accepted an outside pointer or invalid zoom level");
}

void TestPendingViewportIntentsReplayAfterDimensions() {
	TemporaryDirectory temporary;
	const fs::path filename = temporary.path() / "pending-view.jpg";
	WriteText(filename, "source identity fixture");
	const jpegview_linux::SourceKey source =
		jpegview_linux::DescribeImageSource(filename).Key();
	const jpegview_linux::ViewportSnapshot savedManual{false, false, false, 2.0, 2.0};

	jpegview_linux::Viewport viewport;
	viewport.ActualSize();
	ExpectNear(viewport.Zoom(), 1.0, 1e-12,
		"outgoing image did not begin at Actual Size");
	jpegview_linux::RecentImageLoadState recentLoad;
	recentLoad.BeginLoad(filename, savedManual);
	jpegview_linux::PendingImageIntents pendingIntents;
	pendingIntents.Begin(filename, source, 41);
	viewport.Restore(savedManual, 0, 0, 1000, 800);
	ExpectNear(viewport.Zoom(), 2.0, 1e-12,
		"pending incoming snapshot did not replace outgoing Actual Size");

	const jpegview_linux::ViewportIntent fit{
		jpegview_linux::ViewportIntentType::Fit, true, false};
	jpegview_linux::ApplyViewportIntent(viewport, fit, 0, 0, 1000, 800);
	Expect(viewport.IsFitToWindow() && viewport.FillWithCrop() && !viewport.NoEnlarge(),
		"zero-dimension Fit did not record fit, crop, and enlargement intent");
	Expect(recentLoad.UpdatePendingViewport(filename, viewport.Snapshot()) &&
		pendingIntents.QueueViewport(filename, source, 41, fit),
		"pending Fit intent was not retained with its load");

	jpegview_linux::ViewportIntent zoom;
	zoom.type = jpegview_linux::ViewportIntentType::ZoomByFactor;
	zoom.value = 1.2;
	zoom.mouseX = 500;
	zoom.mouseY = 400;
	jpegview_linux::ApplyViewportIntent(viewport, zoom, 0, 0, 1000, 800);
	Expect(recentLoad.UpdatePendingViewport(filename, viewport.Snapshot()) &&
		pendingIntents.QueueViewport(filename, source, 41, zoom),
		"pending zoom intent was not retained after Fit");
	jpegview_linux::ViewportIntent pan;
	pan.type = jpegview_linux::ViewportIntentType::Pan;
	pan.deltaX = 23.0;
	pan.deltaY = -11.0;
	jpegview_linux::ApplyViewportIntent(viewport, pan, 0, 0, 1000, 800);
	Expect(recentLoad.UpdatePendingViewport(filename, viewport.Snapshot()) &&
		pendingIntents.QueueViewport(filename, source, 41, pan),
		"pending pan intent was not retained after zoom");
	const auto pending = recentLoad.TakePendingLoad(filename);
	const auto pendingActions = pendingIntents.Take(source, 41);
	Expect(pending.has_value() && pending->intentBaseSnapshot.zoom == 2.0 &&
		pendingActions.has_value() && pendingActions->actions.size() == 3,
		"pending commands did not retain the incoming snapshot and ordered commands");

	jpegview_linux::Viewport continued;
	continued.Restore(pending->intentBaseSnapshot, 4000, 3000, 1000, 800);
	for (const jpegview_linux::PendingImageIntent& action : pendingActions->actions) {
		Expect(action.type == jpegview_linux::PendingImageIntentType::Viewport,
			"viewport-only test received a transform action");
		const jpegview_linux::ViewportIntent& intent = action.viewport;
		jpegview_linux::ApplyViewportIntent(continued, intent, 4000, 3000, 1000, 800);
		if (intent.type == jpegview_linux::ViewportIntentType::ZoomByFactor ||
			intent.type == jpegview_linux::ViewportIntentType::ZoomPreset ||
			intent.type == jpegview_linux::ViewportIntentType::Pan) {
			continued.ClampToView(4000, 3000, 1000, 800);
		}
	}
	Expect(!continued.IsFitToWindow() && continued.FillWithCrop() == false &&
		continued.NoEnlarge() == false,
		"Fit followed by manual zoom did not finish in manual mode");
	ExpectNear(continued.Zoom(), 0.32, 1e-12,
		"fill then zoom replay used outgoing or stale incoming geometry");
	ExpectNear(continued.OffsetX(), 23.0, 1e-12,
		"pending pan was lost when earlier typed viewport commands were replayed");
	ExpectNear(continued.OffsetY(), -11.0, 1e-12,
		"pending vertical pan was lost when earlier typed viewport commands were replayed");

	jpegview_linux::Viewport relative;
	relative.SetFitRelativeZoomMode(true);
	relative.ActualSize();
	const jpegview_linux::ViewportSnapshot incomingFit{true, false, true, 1.0, 1.0};
	jpegview_linux::RecentImageLoadState relativeLoad;
	relativeLoad.BeginLoad(filename, incomingFit);
	pendingIntents.Begin(filename, source, 42);
	relative.Restore(incomingFit, 0, 0, 1000, 800);
	const jpegview_linux::ViewportIntent relativeFit{
		jpegview_linux::ViewportIntentType::Fit, false, true};
	jpegview_linux::ApplyViewportIntent(relative, relativeFit, 0, 0, 1000, 800);
	relativeLoad.UpdatePendingViewport(filename, relative.Snapshot());
	pendingIntents.QueueViewport(filename, source, 42, relativeFit);
	jpegview_linux::ViewportIntent preset;
	preset.type = jpegview_linux::ViewportIntentType::ZoomPreset;
	preset.value = 4.0;
	preset.mouseX = 500;
	preset.mouseY = 400;
	jpegview_linux::ApplyViewportIntent(relative, preset, 0, 0, 1000, 800);
	relativeLoad.UpdatePendingViewport(filename, relative.Snapshot());
	pendingIntents.QueueViewport(filename, source, 42, preset);
	const auto relativePending = relativeLoad.TakePendingLoad(filename);
	const auto relativeActions = pendingIntents.Take(source, 42);
	Expect(relativePending.has_value(), "fit-relative pending load was lost");
	relative.Restore(relativePending->intentBaseSnapshot, 4000, 3000, 1000, 800);
	for (const jpegview_linux::PendingImageIntent& action : relativeActions->actions) {
		const jpegview_linux::ViewportIntent& intent = action.viewport;
		jpegview_linux::ApplyViewportIntent(relative, intent, 4000, 3000, 1000, 800);
		if (intent.type == jpegview_linux::ViewportIntentType::ZoomByFactor ||
			intent.type == jpegview_linux::ViewportIntentType::ZoomPreset ||
			intent.type == jpegview_linux::ViewportIntentType::Pan) {
			relative.ClampToView(4000, 3000, 1000, 800);
		}
	}
	ExpectNear(relative.FitRelativeZoomBase(), 0.25, 1e-12,
		"resolved fit-relative base used the outgoing image dimensions");
	ExpectNear(relative.Zoom(), 1.0, 1e-12,
		"Fit followed by a fit-relative preset did not use the incoming fit base");
	ExpectNear(relative.Snapshot().relativeZoom, 4.0, 1e-12,
		"fit-relative preset intent was not retained through header completion");

	jpegview_linux::Viewport manualStep;
	manualStep.ActualSize();
	jpegview_linux::RecentImageLoadState manualStepLoad;
	manualStepLoad.BeginLoad(filename, savedManual);
	pendingIntents.Begin(filename, source, 43);
	manualStep.Restore(savedManual, 0, 0, 1000, 800);
	const jpegview_linux::ViewportIntent step{
		jpegview_linux::ViewportIntentType::ZoomByFactor, false, true, 1.2};
	jpegview_linux::ApplyViewportIntent(manualStep, step, 0, 0, 1000, 800);
	manualStepLoad.UpdatePendingViewport(filename, manualStep.Snapshot());
	pendingIntents.QueueViewport(filename, source, 43, step);
	const auto manualPending = manualStepLoad.TakePendingLoad(filename);
	const auto manualActions = pendingIntents.Take(source, 43);
	Expect(manualPending.has_value(), "manual-step pending load was lost");
	manualStep.Restore(manualPending->intentBaseSnapshot, 4000, 3000, 1000, 800);
	for (const jpegview_linux::PendingImageIntent& action : manualActions->actions) {
		const jpegview_linux::ViewportIntent& intent = action.viewport;
		jpegview_linux::ApplyViewportIntent(manualStep, intent, 4000, 3000, 1000, 800);
		if (intent.type == jpegview_linux::ViewportIntentType::ZoomByFactor ||
			intent.type == jpegview_linux::ViewportIntentType::ZoomPreset ||
			intent.type == jpegview_linux::ViewportIntentType::Pan) {
			manualStep.ClampToView(4000, 3000, 1000, 800);
		}
	}
	ExpectNear(manualStep.Zoom(), 2.4, 1e-12,
		"manual step zoom did not start from the incoming saved 2x view");
}

void TestViewportNavigationResetsTransientZoom() {
	jpegview_linux::Viewport viewport;
	viewport.Fit(1000, 600, 500, 300, false, true);
	viewport.ZoomAt(2.0, 250, 150, 1000, 600, 500, 300);
	Expect(std::string(viewport.ScaleMode()) == "manual", "zoom did not affect the current image");
	const jpegview_linux::ViewportSnapshot fitNavigation = viewport.NavigationSnapshot();
	Expect(fitNavigation.fitToWindow && !fitNavigation.fillWithCrop && fitNavigation.noEnlarge,
		"transient zoom replaced the fit navigation mode");
	Expect(std::string(viewport.NavigationScaleMode()) == "fit_no_enlarge",
		"navigation scale-mode serialization followed transient zoom state");
	viewport.Restore(fitNavigation, 2000, 1000, 500, 300);
	ExpectNear(viewport.Zoom(), 0.25, 0.0001, "next image retained transient zoom instead of fitting");

	viewport.Fit(1000, 500, 500, 300, true, false);
	viewport.Pan(30.0, 20.0);
	const jpegview_linux::ViewportSnapshot fillNavigation = viewport.NavigationSnapshot();
	Expect(fillNavigation.fitToWindow && fillNavigation.fillWithCrop && !fillNavigation.noEnlarge,
		"transient pan replaced the fill navigation mode");

	viewport.ActualSize();
	viewport.ZoomAt(3.0, 250, 150, 200, 100, 500, 300);
	const jpegview_linux::ViewportSnapshot actualSizeNavigation = viewport.NavigationSnapshot();
	Expect(!actualSizeNavigation.fitToWindow,
		"actual-size navigation was incorrectly converted to a fit mode");
	ExpectNear(actualSizeNavigation.zoom, 1.0, 0.0001,
		"zooming replaced actual size as the navigation zoom");
	viewport.Restore(actualSizeNavigation, 400, 200, 500, 300);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "next image retained zoom instead of actual size");

	viewport.LoadScaleMode("manual", true, 2.5);
	ExpectNear(viewport.Zoom(), 1.0, 0.0001, "legacy manual setting was restored instead of actual size");
	ExpectNear(viewport.NavigationZoom(), 1.0, 0.0001,
		"legacy manual setting was allowed to propagate to subsequent images");
}

void TestViewportFitRelativeZoomMode() {
	using jpegview_linux::Viewport;
	Viewport viewport;
	Expect(!viewport.FitRelativeZoomMode() &&
		std::abs(viewport.ZoomStepMultiplier() - 1.2) < 1e-12 &&
		std::abs(viewport.ZoomTargetForPreset(2.0) - 2.0) < 1e-12,
		"fit-relative zoom changed the default source-pixel scale behavior");
	viewport.SetFitRelativeZoomMode(true);
	viewport.Fit(4000, 3000, 1000, 800, false, true);
	ExpectNear(viewport.FitRelativeZoomBase(), 0.25, 1e-12,
		"fit-relative zoom base did not use the full window-fit scale");
	ExpectNear(viewport.ZoomTargetForPreset(4.0), 1.0, 1e-12,
		"zoom preset was not resolved relative to the fit scale");
	Expect(std::abs(viewport.ZoomStepMultiplier() - 1.1) < 1e-12,
		"fit-relative zoom steps did not use a stable ratio");
	Viewport nearFitAnchor;
	nearFitAnchor.SetFitRelativeZoomMode(true);
	nearFitAnchor.Fit(4000, 3000, 1000, 800, false, true);
	nearFitAnchor.ZoomAt(0.995, 500, 400, 4000, 3000, 1000, 800);
	ExpectNear(nearFitAnchor.Zoom(), nearFitAnchor.FitRelativeZoomBase(), 1e-12,
		"fit-relative zoom did not snap a near-100% target to the fit anchor");
	Viewport fitAnchor;
	fitAnchor.SetFitRelativeZoomMode(true);
	fitAnchor.Fit(4000, 3000, 1000, 800, false, true);
	fitAnchor.ZoomAt(0.95, 500, 400, 4000, 3000, 1000, 800);
	fitAnchor.ZoomAt(fitAnchor.ZoomStepMultiplier(), 500, 400,
		4000, 3000, 1000, 800, true);
	ExpectNear(fitAnchor.Zoom(), fitAnchor.FitRelativeZoomBase(), 1e-12,
		"fit-relative zoom step did not pause at the fit-relative 100% anchor");
	fitAnchor.ZoomAt(fitAnchor.ZoomStepMultiplier(), 500, 400,
		4000, 3000, 1000, 800, true);
	ExpectNear(fitAnchor.Zoom(), fitAnchor.FitRelativeZoomBase() * 1.1, 1e-12,
		"fit-relative zoom could not continue after pausing at the 100% anchor");
	Viewport sourceScale;
	sourceScale.Fit(4000, 3000, 1000, 800, false, true);
	sourceScale.ZoomAt(3.98, 500, 400, 4000, 3000, 1000, 800);
	ExpectNear(sourceScale.Zoom(), 0.25 * 3.98, 1e-12,
		"fit-relative snapping changed a near-100% target in default mode");
	sourceScale.Fit(4000, 3000, 1000, 800, false, true);
	sourceScale.ZoomAt(0.95, 500, 400, 4000, 3000, 1000, 800);
	sourceScale.ZoomAt(sourceScale.ZoomStepMultiplier(), 500, 400,
		4000, 3000, 1000, 800, true);
	ExpectNear(sourceScale.Zoom(), 0.25 * 0.95 * 1.2, 1e-12,
		"fit-relative pause behavior changed the default source-pixel zoom mode");
	Viewport smallImage;
	smallImage.SetFitRelativeZoomMode(true);
	smallImage.Fit(100, 50, 500, 300, false, true);
	ExpectNear(smallImage.Zoom(), 1.0, 1e-12,
		"fit-no-enlarge unexpectedly changed the normal displayed size of a small image");
	ExpectNear(smallImage.FitRelativeZoomBase(), 5.0, 1e-12,
		"fit-relative base did not use the window-fit scale when fit mode avoids enlargement");
	smallImage.ActualSize();
	ExpectNear(smallImage.Zoom(), 5.0, 1e-12,
		"fit-relative 100% did not use the full-window fit scale for a small image");
	Expect(smallImage.ZoomReadout() == "100% (500%)",
		"fit-relative readout did not retain the source-pixel scale for a small image");
	smallImage.ZoomAt(smallImage.ZoomStepMultiplier(), 250, 150,
		100, 50, 500, 300);
	ExpectNear(smallImage.Zoom(), 5.5, 1e-12,
		"fit-relative step did not grow by the configured ratio");

	const double fourHundredPercent = viewport.ZoomTargetForPreset(4.0);
	viewport.ZoomAt(fourHundredPercent / viewport.Zoom(), 500, 400,
		4000, 3000, 1000, 800);
	ExpectNear(viewport.Zoom(), 1.0, 1e-12,
		"400% fit-relative zoom did not produce source-pixel size for this image");
	Expect(viewport.ZoomReadout() == "400% (100%)",
		"fit-relative readout omitted the source-pixel percentage");
	const jpegview_linux::ViewportSnapshot nextImage = viewport.NavigationSnapshot();
	ExpectNear(nextImage.relativeZoom, 4.0, 1e-12,
		"navigation snapshot did not retain the current fit-relative zoom ratio");
	viewport.Restore(nextImage, 2000, 1000, 1000, 800);
	ExpectNear(viewport.FitRelativeZoomBase(), 0.5, 1e-12,
		"fit-relative base was not recalculated for the next image");
	ExpectNear(viewport.Zoom(), 2.0, 1e-12,
		"navigation did not preserve the relative zoom on an image of another size");
	Expect(viewport.ZoomReadout() == "400% (200%)",
		"readout did not retain the actual source-pixel percentage for a different image");
	viewport.ActualSize();
	ExpectNear(viewport.Zoom(), 0.5, 1e-12,
		"fit-relative 100% did not use the current image's fit scale");
	Expect(viewport.IsActualSize() && viewport.ZoomReadout() == "100% (50%)",
		"fit-relative actual-size state or readout was incorrect");

	viewport.SetFitRelativeZoomMode(false);
	Expect(viewport.ZoomReadout() == "50%" &&
		std::abs(viewport.ZoomTargetForPreset(2.0) - 2.0) < 1e-12,
		"disabling fit-relative zoom did not restore the source-scale readout and presets");
	viewport.ActualSize();
	ExpectNear(viewport.Zoom(), 1.0, 1e-12,
		"source-pixel actual size changed after disabling fit-relative zoom");
}

void TestResizeModelAspectRatioValidationAndFilters() {
	jpegview_linux::ResizeModel model;
	model.Reset(400, 200);
	Expect(model.OriginalWidth() == 400 && model.OriginalHeight() == 200,
		"resize model did not retain original dimensions");
	Expect(model.FieldText(jpegview_linux::ResizeModel::kPercentField) == "100",
		"resize model did not initialize percentage");
	Expect(model.Filter() == 2 && std::string(model.FilterName()) == "SHARPEN LOW",
		"resize model default filter changed");

	model.FieldText(jpegview_linux::ResizeModel::kPercentField) = "50";
	Expect(model.UpdateFrom(jpegview_linux::ResizeModel::kPercentField), "valid resize percentage was rejected");
	Expect(model.FieldText(jpegview_linux::ResizeModel::kWidthField) == "200" &&
		model.FieldText(jpegview_linux::ResizeModel::kHeightField) == "100",
		"percentage did not update proportional dimensions");

	model.FieldText(jpegview_linux::ResizeModel::kWidthField) = "100";
	Expect(model.UpdateFrom(jpegview_linux::ResizeModel::kWidthField), "valid resize width was rejected");
	Expect(model.FieldText(jpegview_linux::ResizeModel::kPercentField) == "25" &&
		model.FieldText(jpegview_linux::ResizeModel::kHeightField) == "50",
		"width did not update percentage and proportional height");

	model.FieldText(jpegview_linux::ResizeModel::kHeightField) = "100";
	Expect(model.UpdateFrom(jpegview_linux::ResizeModel::kHeightField), "valid resize height was rejected");
	int width = 0;
	int height = 0;
	Expect(model.Target(width, height) && width == 200 && height == 100,
		"resize target did not reflect edited height");

	model.FieldText(jpegview_linux::ResizeModel::kWidthField) = "65535";
	Expect(!model.UpdateFrom(jpegview_linux::ResizeModel::kWidthField), "oversized pixel count was accepted");
	Expect(!model.ValidationMessage().empty(), "oversized resize did not provide validation feedback");
	model.FieldText(jpegview_linux::ResizeModel::kWidthField) = "12px";
	Expect(!model.Target(width, height), "partially numeric resize target was accepted");

	model.Reset(1, 1);
	model.FieldText(jpegview_linux::ResizeModel::kPercentField) = "0.1";
	Expect(!model.UpdateFrom(jpegview_linux::ResizeModel::kPercentField),
		"percentage producing a zero-sized image was accepted");
	model.FieldText(jpegview_linux::ResizeModel::kPercentField) = "nan";
	Expect(!model.UpdateFrom(jpegview_linux::ResizeModel::kPercentField), "non-finite percentage was accepted");

	model.CycleFilter(1);
	Expect(model.Filter() == 3, "resize filter did not advance");
	model.CycleFilter(1);
	Expect(model.Filter() == 0, "resize filter did not wrap forward");
	model.CycleFilter(-1);
	Expect(model.Filter() == 3, "resize filter did not wrap backward");
}

void TestResizeDialogController() {
	jpegview_linux::ResizeDialogController dialog;
	dialog.Open(400, 200);
	Expect(dialog.IsOpen() && dialog.FocusedField() == jpegview_linux::ResizeModel::kPercentField,
		"resize dialog did not open on its percentage field");
	dialog.AppendText("50abc.5");
	Expect(dialog.Model().FieldText(jpegview_linux::ResizeModel::kPercentField) == "51" &&
		dialog.Model().FieldText(jpegview_linux::ResizeModel::kWidthField) == "202" &&
		dialog.Model().FieldText(jpegview_linux::ResizeModel::kHeightField) == "101",
		"resize dialog did not filter text or update coupled dimensions");
	dialog.MoveFocus(1);
	Expect(dialog.FocusedField() == jpegview_linux::ResizeModel::kWidthField,
		"resize dialog did not move focus forward");
	dialog.AppendText("100");
	int width = 0;
	int height = 0;
	Expect(dialog.Target(width, height) && width == 100 && height == 50,
		"resize dialog did not replace a primed field and expose its target");
	dialog.SelectAll();
	Expect(dialog.Model().FieldText(jpegview_linux::ResizeModel::kWidthField).empty(),
		"resize dialog select-all did not clear the focused numeric field");
	dialog.AppendText("80");
	dialog.Backspace();
	Expect(dialog.Target(width, height) && width == 8 && height == 4,
		"resize dialog Backspace did not recalculate its target");
	dialog.MoveFocus(-1);
	Expect(dialog.FocusedField() == jpegview_linux::ResizeModel::kPercentField,
		"resize dialog did not move focus backward");
	dialog.SelectField(jpegview_linux::ResizeModel::kFilterField);
	const int previousFilter = dialog.Model().Filter();
	dialog.CycleFilter(1);
	Expect(dialog.FocusedField() == jpegview_linux::ResizeModel::kFilterField &&
		dialog.Model().Filter() == (previousFilter + 1) % jpegview_linux::ResizeModel::kFilterCount,
		"resize dialog did not cycle its filter in place");
	dialog.SetMessage("external failure");
	Expect(dialog.Message() == "external failure", "resize dialog did not retain adapter failure feedback");
	dialog.Close();
	Expect(!dialog.IsOpen() && dialog.Message().empty(),
		"resize dialog did not clear transient state when closed");
}

void TestCropSizeDialogController() {
	using jpegview_linux::CropSizeDialogController;
	CropSizeDialogController dialog;
	dialog.Open(320, 200, true);
	Expect(dialog.IsOpen() && dialog.FocusedField() == CropSizeDialogController::kWidthField &&
		dialog.WidthText() == "320" && dialog.HeightText() == "200" && dialog.UsesScreenPixels(),
		"fixed crop-size dialog did not open with its persisted values and units");
	dialog.AppendText("64px");
	dialog.MoveFocus(-1);
	Expect(dialog.FocusedField() == CropSizeDialogController::kHeightField,
		"fixed crop-size dialog did not cycle focus backward");
	dialog.AppendText("48");
	int width = 0;
	int height = 0;
	bool screenPixels = false;
	Expect(dialog.Apply(width, height, screenPixels) && width == 64 && height == 48 && screenPixels,
		"fixed crop-size dialog did not filter and apply valid dimensions");
	dialog.ToggleUnits();
	Expect(dialog.Apply(width, height, screenPixels) && !screenPixels,
		"fixed crop-size dialog did not preserve the selected pixel unit");
	dialog.SelectField(CropSizeDialogController::kWidthField);
	dialog.SelectAll();
	dialog.AppendText("65536");
	dialog.SelectField(CropSizeDialogController::kHeightField);
	dialog.SelectAll();
	dialog.AppendText("65535");
	Expect(!dialog.Apply(width, height, screenPixels) && !dialog.Message().empty(),
		"fixed crop-size dialog accepted an out-of-range dimension");
	dialog.SelectField(CropSizeDialogController::kWidthField);
	dialog.SelectAll();
	dialog.AppendText("65535");
	Expect(dialog.Apply(width, height, screenPixels) && width == 65535 && height == 65535,
		"fixed crop-size dialog rejected its maximum dimension");
	dialog.SelectField(CropSizeDialogController::kWidthField);
	dialog.Backspace();
	Expect(dialog.WidthText().empty(), "backspace did not clear a newly focused fixed crop field");
	dialog.Close();
	Expect(!dialog.IsOpen() && dialog.Message().empty(),
		"fixed crop-size dialog did not clear transient state on close");
}

void TestArchivePasswordDialogModel() {
	using jpegview_linux::ArchivePasswordDialogModel;
	ArchivePasswordDialogModel dialog;
	Expect(!dialog.IsOpen() && dialog.ArchivePath().empty() && dialog.DisplayText().empty(),
		"archive password dialog did not start closed and empty");

	dialog.Begin("/images/private.zip");
	Expect(dialog.IsOpen() && dialog.ArchivePath() == "/images/private.zip" &&
		dialog.Error().empty(), "archive password dialog did not begin for its archive");
	Expect(dialog.AppendText("p"), "archive password dialog rejected ASCII text");
	Expect(dialog.AppendText("\xc3\xa9"), "archive password dialog rejected valid multibyte UTF-8");
	Expect(dialog.AppendText("\xf0\x9f\x94\x90"), "archive password dialog rejected a four-byte UTF-8 character");
	const std::string bullet = "\xe2\x80\xa2";
	Expect(dialog.DisplayText() == bullet + bullet + bullet,
		"archive password display did not mask one bullet per UTF-8 code point");
	Expect(dialog.DisplayText().find('p') == std::string::npos &&
		dialog.DisplayText().find("\xc3\xa9") == std::string::npos,
		"archive password display exposed entered characters");

	const std::string incompleteUtf8("\xc3", 1);
	Expect(!dialog.AppendText(incompleteUtf8), "archive password dialog accepted incomplete UTF-8");
	Expect(dialog.DisplayText() == bullet + bullet + bullet,
		"malformed UTF-8 partially changed the entered password");
	const std::string embeddedNul("x\0y", 3);
	Expect(!dialog.AppendText(embeddedNul), "archive password dialog accepted embedded NUL bytes");
	Expect(dialog.DisplayText() == bullet + bullet + bullet,
		"embedded NUL input partially changed the entered password");

	Expect(dialog.Backspace() && dialog.DisplayText() == bullet + bullet,
		"archive password backspace did not remove a full multibyte code point");
	Expect(dialog.Backspace() && dialog.DisplayText() == bullet,
		"archive password backspace did not remove the preceding multibyte code point");
	Expect(dialog.Backspace() && dialog.DisplayText().empty(),
		"archive password backspace did not remove the final ASCII character");
	Expect(!dialog.Backspace(), "archive password backspace reported a change for an empty field");

	Expect(dialog.AppendText("first"), "archive password dialog rejected retry text");
	std::optional<std::string> submitted = dialog.Submit();
	Expect(submitted.has_value() && *submitted == "first",
		"archive password submit did not transfer the entered password");
	Expect(!dialog.IsOpen() && dialog.ArchivePath().empty() && dialog.Error().empty() &&
		dialog.DisplayText().empty(), "archive password submit did not close and reset dialog state");
	Expect(!dialog.Submit().has_value(), "closed archive password dialog submitted a second time");
	Expect(!dialog.AppendText("late"), "closed archive password dialog accepted text");

	dialog.Begin("/images/private.7z", "The password was incorrect");
	Expect(dialog.IsOpen() && dialog.ArchivePath() == "/images/private.7z" &&
		dialog.Error() == "The password was incorrect" && dialog.DisplayText().empty(),
		"archive password retry did not show its error with a fresh empty entry");
	Expect(dialog.AppendText("retry") && dialog.Error().empty(),
		"archive password retry did not clear stale error feedback when edited");
	submitted = dialog.Submit();
	Expect(submitted.has_value() && *submitted == "retry",
		"archive password retry did not submit the newly entered password");

	dialog.Begin("/images/private.rar");
	Expect(dialog.AppendText("cancel-me"), "archive password dialog rejected cancel-test text");
	dialog.Cancel();
	Expect(!dialog.IsOpen() && dialog.ArchivePath().empty() && dialog.Error().empty() &&
		dialog.DisplayText().empty() && !dialog.Submit().has_value(),
		"archive password cancel did not clear and close the dialog");

	dialog.Begin("/images/empty-password.zip");
	submitted = dialog.Submit();
	Expect(submitted.has_value() && submitted->empty() && !dialog.IsOpen(),
		"archive password dialog could not submit an intentionally empty password");
}

void TestContextMenuCompactionAndSelection() {
	using jpegview_linux::MenuItem;
	const std::vector<jpegview_linux::MenuItem> complete = {
		{"Open", 10},
		{nullptr, 0, true},
		{"Advanced heading", 0, false, false, true, nullptr, true},
		{"Advanced command", 20, false, false, true, "Ctrl+A", true},
		{"Disabled", 30, false, false, false},
		{"Close", 40},
	};
	const std::vector<jpegview_linux::MenuItem> compact =
		jpegview_linux::CompactMenuItems(complete, -1, "Show Advanced Options");
	Expect(compact.size() == 5, "compact menu retained advanced entries or lost core entries");
	Expect(std::string(compact[2].label) == "Show Advanced Options" && compact[2].command == -1,
		"compact menu did not insert the one-off advanced command at the first advanced item");
	Expect(std::count_if(compact.begin(), compact.end(), [](const jpegview_linux::MenuItem& item) {
		return item.command == -1;
	}) == 1, "compact menu inserted the advanced command more than once");
	Expect(std::none_of(compact.begin(), compact.end(), [](const jpegview_linux::MenuItem& item) {
		return item.advanced;
	}), "compact menu retained an advanced item");

	Expect(jpegview_linux::NextMenuSelection(compact, -1, 1) == 0,
		"menu selection did not start at first command");
	Expect(jpegview_linux::NextMenuSelection(compact, 0, 1) == 2,
		"menu selection did not skip separator");
	Expect(jpegview_linux::NextMenuSelection(compact, 2, 1) == 4,
		"menu selection did not skip disabled item");
	Expect(jpegview_linux::NextMenuSelection(compact, 4, 1) == 0,
		"menu selection did not wrap forward");
	Expect(jpegview_linux::NextMenuSelection(compact, 0, -1) == 4,
		"menu selection did not wrap backward");
	Expect(jpegview_linux::NextMenuSelection({{nullptr, 0, true}, {"Disabled", 1, false, false, false}},
		-1, 1) == -1, "menu with no actionable items returned a selection");

	const std::vector<MenuItem> separatedSections = {
		{nullptr, 0, true},
		{"Next", 50},
		{nullptr, 0, true},
		{"Navigation", 0, false, false, true, nullptr, true},
		{"Loop recursively", 51, false, false, true, nullptr, true},
		{nullptr, 0, true},
		{"Transform image", 0, false, false, true, nullptr, true},
		{"Unsupported transform", 52, false, false, false, nullptr, true},
		{nullptr, 0, true},
		{"Actual size", 53},
		{nullptr, 0, true},
		{nullptr, 0, true},
	};
	const std::vector<MenuItem> compactSections = jpegview_linux::CompactMenuItems(
		separatedSections, -1, "Show Advanced Options");
	Expect(compactSections.size() == 5 && compactSections[0].command == 50 &&
		compactSections[1].separator && compactSections[2].command == -1 &&
		compactSections[3].separator && compactSections[4].command == 53,
		"compacting advanced sections left empty, repeated, or trailing separators");
}

void TestContextMenuCatalogAndState() {
	using jpegview_linux::ContextMenuState;
	using jpegview_linux::MenuItem;
	const auto findCommand = [](const std::vector<MenuItem>& items, int command) -> const MenuItem* {
		const auto found = std::find_if(items.begin(), items.end(), [command](const MenuItem& item) {
			return item.command == command;
		});
		return found == items.end() ? nullptr : &*found;
	};
	const auto findLabel = [](const std::vector<MenuItem>& items, const std::string& label) -> const MenuItem* {
		const auto found = std::find_if(items.begin(), items.end(), [&label](const MenuItem& item) {
			return item.label == label;
		});
		return found == items.end() ? nullptr : &*found;
	};
	const auto immediatelyBefore = [](const std::vector<MenuItem>& items, int firstCommand,
		int secondCommand) {
		const auto first = std::find_if(items.begin(), items.end(), [firstCommand](const MenuItem& item) {
			return item.command == firstCommand;
		});
		return first != items.end() && first + 1 != items.end() &&
			(first + 1)->command == secondCommand;
	};

	ContextMenuState state;
	const std::vector<MenuItem> compact = jpegview_linux::BuildContextMenu(state, false);
	Expect(findCommand(compact, jpegview_linux::kCommandOpenGpsLocation) == nullptr,
		"GPS map action was shown without image coordinates");
	state.gpsLocationAvailable = true;
	const std::vector<MenuItem> gpsProviderUnavailable =
		jpegview_linux::BuildContextMenu(state, false);
	const MenuItem* disabledGpsAction = findCommand(gpsProviderUnavailable,
		jpegview_linux::kCommandOpenGpsLocation);
	Expect(disabledGpsAction != nullptr && !disabledGpsAction->enabled,
		"GPS map action was not disabled for an invalid provider template");
	state.gpsMapProviderValid = true;
	const std::vector<MenuItem> gpsAvailable = jpegview_linux::BuildContextMenu(state, false);
	const MenuItem* enabledGpsAction = findCommand(gpsAvailable,
		jpegview_linux::kCommandOpenGpsLocation);
	const MenuItem* gpsInfoItem = findCommand(gpsAvailable, IDM_SHOW_FILEINFO);
	Expect(enabledGpsAction != nullptr && enabledGpsAction->enabled &&
		enabledGpsAction->label == "Open GPS location in map",
		"GPS map action was not offered when coordinates and provider were valid");
	Expect(gpsInfoItem != nullptr && enabledGpsAction == gpsInfoItem + 1,
		"GPS map command was not placed beside the EXIF information action");
	state.gpsLocationAvailable = false;
	const MenuItem* goToUnavailable = findCommand(compact,
		jpegview_linux::kCommandGoToImageNumber);
	Expect(goToUnavailable != nullptr && !goToUnavailable->enabled &&
		goToUnavailable->shortcut == "Ctrl+G",
		"go-to-image command was not disabled without an active file list");
	state.fileListAvailable = true;
	const std::vector<MenuItem> withFileList = jpegview_linux::BuildContextMenu(state, false);
	const MenuItem* goToAvailable = findCommand(withFileList,
		jpegview_linux::kCommandGoToImageNumber);
	Expect(goToAvailable != nullptr && goToAvailable->enabled,
		"go-to-image command was not enabled for an active file list");
	state.fileListAvailable = false;
	const MenuItem* compactAdvancedConfiguration = findCommand(compact,
		jpegview_linux::kCommandAdvancedConfiguration);
	Expect(findCommand(compact, jpegview_linux::kContextMenuShowAdvanced) != nullptr,
		"compact context menu omitted the one-off advanced-options command");
	Expect(findCommand(compact, IDM_PRINT) == nullptr && findCommand(compact, IDM_LOOP_FOLDER) == nullptr &&
		findCommand(compact, IDM_ZOOM_400) == nullptr && findCommand(compact, IDM_SLIDESHOW_START) == nullptr,
		"compact context menu exposed advanced catalog entries");
	Expect(findCommand(compact, IDM_OPEN) != nullptr && findCommand(compact, IDM_NEXT) != nullptr &&
		findCommand(compact, IDM_ZOOM_100) != nullptr && findCommand(compact, IDM_EXIT) != nullptr,
		"compact context menu lost a primary command");
	Expect(findCommand(compact, IDM_ZOOM_100)->shortcut == "Space",
		"context menu omitted the default actual-size Space shortcut");
	state.fitRelativeZoomMode = true;
	state.fitRelativeZoomBase = 0.25;
	state.fitToWindow = false;
	state.zoom = 0.25;
	const std::vector<MenuItem> fitRelativeMenu =
		jpegview_linux::BuildContextMenu(state, true);
	const MenuItem* fitRelativeSize = findCommand(fitRelativeMenu, IDM_ZOOM_100);
	Expect(fitRelativeSize != nullptr && fitRelativeSize->label == "  Fit-relative size (100 %)" &&
		fitRelativeSize->checked,
		"context menu did not label and check the fit-relative 100% command");
	state.zoom = 0.257;
	const std::vector<MenuItem> notAtRelativeSize =
		jpegview_linux::BuildContextMenu(state, true);
	const MenuItem* notAtRelativeSizeItem = findCommand(notAtRelativeSize, IDM_ZOOM_100);
	Expect(notAtRelativeSizeItem != nullptr && !notAtRelativeSizeItem->checked,
		"fit-relative 100% checkmark used an absolute tolerance for a small scale");
	state.fitRelativeZoomMode = false;
	state.fitRelativeZoomBase = 1.0;
	state.fitToWindow = true;
	state.zoom = 1.0;
	Expect(findCommand(compact, IDM_HELP) != nullptr && findCommand(compact, IDM_HELP)->enabled &&
		findCommand(compact, IDM_HELP)->shortcut == "F1",
		"compact context menu omitted the built-in help command");
	Expect(findCommand(compact, jpegview_linux::kCommandEditPictureLevels) != nullptr,
		"compact context menu omitted the picture-level editor");
	Expect(immediatelyBefore(compact, jpegview_linux::kCommandAdvancedConfiguration, IDM_HELP) &&
		compactAdvancedConfiguration != nullptr &&
		compactAdvancedConfiguration->label == "Advanced configuration..." &&
		compactAdvancedConfiguration->enabled,
		"advanced configuration was not immediately before Help in the compact menu");
	Expect(jpegview_linux::kCommandAdvancedConfiguration !=
		jpegview_linux::kCommandToggleMagnifyingGlass,
		"advanced configuration reused the magnifying-glass keyboard command ID");
	const MenuItem* compactSelectionMode = findCommand(compact,
		jpegview_linux::kCommandToggleSelectionMode);
	Expect(compactSelectionMode != nullptr && compactSelectionMode->label == "Crop selection mode" &&
		!compactSelectionMode->checked && compactSelectionMode->shortcut == "Ctrl+E",
		"compact context menu did not expose the disabled-by-default crop selection mode");
	const MenuItem* magnifyingGlass = findCommand(compact,
		jpegview_linux::kCommandToggleMagnifyingGlass);
	Expect(magnifyingGlass != nullptr && magnifyingGlass->label == "Magnifying glass" &&
		magnifyingGlass->shortcut == "Z" && !magnifyingGlass->checked &&
		!magnifyingGlass->enabled,
		"context menu did not expose the image-dependent magnifying-glass toggle");
	const MenuItem* pixelColorSampler = findCommand(compact,
		jpegview_linux::kCommandTogglePixelColorSampler);
	Expect(pixelColorSampler != nullptr && pixelColorSampler->label == "Pixel color sampler" &&
		!pixelColorSampler->checked && pixelColorSampler->enabled,
		"context menu did not expose the disabled-by-default pixel sampler toggle");
	const MenuItem* doublePageMode = findCommand(compact,
		jpegview_linux::kCommandToggleDoublePageMode);
	const MenuItem* mangaReadingOrder = findCommand(compact,
		jpegview_linux::kCommandToggleMangaReadingOrder);
	Expect(doublePageMode != nullptr && doublePageMode->label == "Double page mode" &&
		doublePageMode->shortcut == "D" && !doublePageMode->checked && !doublePageMode->enabled &&
		mangaReadingOrder != nullptr && mangaReadingOrder->label == "Double page manga mode" &&
		mangaReadingOrder->shortcut == "J" && !mangaReadingOrder->checked &&
		!mangaReadingOrder->enabled,
		"context menu did not expose the image-dependent double-page modes");
	Expect(findCommand(compact, jpegview_linux::kCommandPreviousSiblingFolder) == nullptr &&
		findCommand(compact, jpegview_linux::kCommandNextSiblingFolder) == nullptr,
		"compact context menu exposed advanced sibling-folder navigation commands");

	state.playbackMode = jpegview_linux::PlaybackMode::Movie;
	state.animationAvailable = true;
	state.movieFramesPerSecond = 50.0;
	state.infoVisible = true;
	state.filenameVisible = true;
	state.navigationPanelEnabled = false;
	state.navigationPanelAutoReveal = false;
	state.thumbnailPanelVisible = true;
	state.navigationMode = FileList::NavigationMode::LoopSubDirectories;
	state.sortMode = FileList::SortMode::LastModificationTime;
	state.sortAscending = false;
	state.imageAvailable = true;
	state.freeRotationAvailable = true;
	state.perspectiveCorrectionAvailable = true;
	state.magnifyingGlassEnabled = true;
	state.pixelColorSamplerEnabled = true;
	state.doublePageModeEnabled = true;
	state.mangaReadingOrderEnabled = true;
	state.losslessJpegAvailable = true;
	state.spacebarNavigatesImages = true;
	state.autoCorrectionEnabled = true;
	state.pictureLevelsAvailable = true;
	state.selectionModeEnabled = true;
	state.localDensityEnabled = true;
	state.keepPictureLevels = true;
	state.pictureLevelsSaved = true;
	state.fitToWindow = false;
	state.zoom = 1.0;
	state.fullscreen = true;
	state.borderless = true;
	state.alwaysOnTop = true;
	state.transitionEffect = IDM_EFFECT_BLEND;
	state.transitionDurationMs = 1000;
	state.openWithApplicationNames = {"Photo Editor", u8"写真工具"};
	const std::vector<MenuItem> advanced = jpegview_linux::BuildContextMenu(state, true);
	const MenuItem* expandedAdvancedConfiguration = findCommand(advanced,
		jpegview_linux::kCommandAdvancedConfiguration);
	Expect(immediatelyBefore(advanced, jpegview_linux::kCommandAdvancedConfiguration, IDM_HELP) &&
		expandedAdvancedConfiguration != nullptr &&
		expandedAdvancedConfiguration->label == "Advanced configuration..." &&
		expandedAdvancedConfiguration->enabled,
		"advanced configuration was not immediately before Help in the expanded menu");
	Expect(findCommand(advanced, jpegview_linux::kCommandToggleDoublePageMode)->checked &&
		findCommand(advanced, jpegview_linux::kCommandToggleMangaReadingOrder)->checked,
		"context menu did not reflect active double-page and manga states");
	Expect(findCommand(advanced, IDM_NEXT)->shortcut == "Left/PgDn" &&
		findCommand(advanced, IDM_PREV)->shortcut == "Right/PgUp",
		"manga context-menu navigation shortcuts did not reflect reversed reading order");

	Expect(findCommand(advanced, jpegview_linux::kContextMenuShowAdvanced) == nullptr,
		"expanded context menu retained the advanced-options command");
	Expect(findCommand(advanced, IDM_PRINT) != nullptr && findCommand(advanced, IDM_LOOP_FOLDER) != nullptr &&
		findCommand(advanced, IDM_ZOOM_400) != nullptr && findCommand(advanced, IDM_SLIDESHOW_START) != nullptr,
		"expanded context menu omitted an advanced catalog section");
	const MenuItem* previousSibling = findCommand(advanced,
		jpegview_linux::kCommandPreviousSiblingFolder);
	const MenuItem* nextSibling = findCommand(advanced,
		jpegview_linux::kCommandNextSiblingFolder);
	Expect(previousSibling != nullptr && previousSibling->label == "  Previous sibling folder" &&
		previousSibling->shortcut == "Alt+Left" && nextSibling != nullptr &&
		nextSibling->label == "  Next sibling folder" && nextSibling->shortcut == "Alt+Right",
		"expanded context menu omitted sibling-folder navigation items or their shortcuts");
	Expect(findCommand(advanced, IDM_SHOW_FILEINFO)->checked &&
		findCommand(advanced, IDM_SHOW_FILENAME)->checked &&
		!findCommand(advanced, IDM_SHOW_NAVPANEL)->checked &&
		findCommand(advanced, jpegview_linux::kCommandToggleThumbnailPanel)->checked &&
		findCommand(advanced, jpegview_linux::kCommandToggleZoomNavigator)->checked,
		"context menu did not reflect panel visibility state");
	Expect(findCommand(advanced, jpegview_linux::kCommandToggleMagnifyingGlass)->enabled &&
		findCommand(advanced, jpegview_linux::kCommandToggleMagnifyingGlass)->checked,
		"context menu did not reflect the active magnifying-glass state");
	Expect(findCommand(advanced, jpegview_linux::kCommandTogglePixelColorSampler)->checked,
		"context menu did not reflect the active pixel color sampler preference");
	Expect(findCommand(advanced, jpegview_linux::kCommandToggleSelectionMode)->checked,
		"context menu did not reflect enabled crop selection mode");
	state.showZoomNavigator = false;
	const std::vector<MenuItem> navigatorHidden = jpegview_linux::BuildContextMenu(state, true);
	Expect(findCommand(navigatorHidden, jpegview_linux::kCommandToggleZoomNavigator) != nullptr &&
		!findCommand(navigatorHidden, jpegview_linux::kCommandToggleZoomNavigator)->checked,
		"context menu did not reflect the disabled zoom navigator setting");
	Expect(findCommand(advanced, IDM_LOOP_RECURSIVELY)->checked &&
		findCommand(advanced, IDM_SORT_MOD_DATE)->checked &&
		findCommand(advanced, IDM_SORT_DESCENDING)->checked &&
		findLabel(advanced, "Current order: D (modification date)") != nullptr,
		"context menu did not reflect navigation and ordering state");
	Expect(findCommand(advanced, IDM_CHANGESIZE)->enabled &&
		findCommand(advanced, IDM_ROTATE)->enabled &&
		findCommand(advanced, IDM_PERSPECTIVE)->enabled &&
		findCommand(advanced, IDM_ROTATE_90_LOSSLESS)->enabled &&
		findCommand(advanced, IDM_AUTO_CORRECTION)->checked &&
		findCommand(advanced, IDM_SAVE_PARAMETERS)->enabled &&
		findCommand(advanced, IDM_BACKUP_PARAMDB)->enabled &&
		findCommand(advanced, IDM_RESTORE_PARAMDB)->enabled &&
		findCommand(advanced, IDM_SET_AS_DEFAULT_VIEWER)->enabled &&
		findCommand(advanced, jpegview_linux::kCommandEditPictureLevels)->enabled &&
		findCommand(advanced, IDM_LDC)->checked && findCommand(advanced, IDM_KEEP_PARAMETERS)->checked &&
		!findCommand(advanced, IDM_SAVE_PARAM_DB)->enabled &&
		!findCommand(advanced, IDM_CLEAR_PARAM_DB)->enabled,
		"context menu did not enable image-dependent commands");
	state.keepPictureLevels = false;
	const std::vector<MenuItem> editableDatabase = jpegview_linux::BuildContextMenu(state, true);
	Expect(findCommand(editableDatabase, IDM_SAVE_PARAM_DB)->enabled &&
		findCommand(editableDatabase, IDM_CLEAR_PARAM_DB)->enabled,
		"parameter database actions stayed disabled after keep-between-images was turned off");
	Expect(findCommand(advanced, IDM_ZOOM_100)->checked &&
		findCommand(advanced, IDM_ZOOM_100)->shortcut.empty() &&
		findCommand(advanced, IDM_FULL_SCREEN_MODE)->checked &&
		findCommand(advanced, IDM_HIDE_TITLE_BAR)->checked &&
		findCommand(advanced, IDM_ALWAYS_ON_TOP)->checked,
		"context menu did not reflect viewport and window state");
	Expect(findCommand(advanced, IDM_EFFECT_BLEND)->checked &&
		findCommand(advanced, IDM_EFFECTTIME_SLOW)->checked &&
		findCommand(advanced, IDM_MOVIE_50_FPS)->checked,
		"context menu did not reflect playback settings");
	Expect(findLabel(advanced, "  Photo Editor") != nullptr &&
		findCommand(advanced, IDM_FIRST_OPENWITH_CMD)->label == "  Photo Editor" &&
		findCommand(advanced, IDM_FIRST_OPENWITH_CMD + 1)->label == u8"  写真工具",
		"context menu did not own or number dynamic Open with labels");
	Expect(!findCommand(advanced, IDM_EDIT_GLOBAL_CONFIG)->enabled,
		"unsupported settings command unexpectedly became actionable");

	ContextMenuState unavailable;
	unavailable.parameterDatabaseAvailable = false;
	const std::vector<MenuItem> disabled = jpegview_linux::BuildContextMenu(unavailable, true);
	Expect(!findCommand(disabled, IDM_CHANGESIZE)->enabled &&
		!findCommand(disabled, IDM_ROTATE)->enabled &&
		!findCommand(disabled, IDM_PERSPECTIVE)->enabled &&
		!findCommand(disabled, IDM_ROTATE_90_LOSSLESS)->enabled &&
		!findCommand(disabled, IDM_AUTO_CORRECTION)->enabled &&
		!findCommand(disabled, IDM_SAVE_PARAMETERS)->enabled &&
		!findCommand(disabled, IDM_BACKUP_PARAMDB)->enabled &&
		!findCommand(disabled, IDM_RESTORE_PARAMDB)->enabled,
		"image-dependent commands were enabled without an image");
	Expect(findLabel(disabled, "  (no configured applications)") != nullptr,
		"empty Open with state omitted its disabled placeholder");
	ContextMenuState archiveImage;
	archiveImage.archiveMember = true;
	archiveImage.imageAvailable = true;
	archiveImage.openWithApplicationNames.clear();
	const std::vector<MenuItem> archiveMenu = jpegview_linux::BuildContextMenu(archiveImage, true);
	Expect(!findCommand(archiveMenu, IDM_PRINT)->enabled &&
		!findCommand(archiveMenu, IDM_BATCH_COPY)->enabled &&
		!findCommand(archiveMenu, IDM_TOUCH_IMAGE)->enabled &&
		!findCommand(archiveMenu, IDM_TOUCH_IMAGE_EXIF_FOLDER)->enabled &&
		!findCommand(archiveMenu, IDM_SET_WALLPAPER_ORIG)->enabled &&
		findCommand(archiveMenu, IDM_SET_WALLPAPER_DISPLAY)->enabled,
		"filesystem-only actions were not disabled for an archive image");
}

void TestContextMenuMnemonics() {
	using jpegview_linux::ContextMenuState;
	using jpegview_linux::MenuItem;
	const auto expectStableSharedMnemonics = [](const ContextMenuState& menuState) {
		const std::vector<MenuItem> compactMenu = jpegview_linux::BuildContextMenu(menuState, false);
		const std::vector<MenuItem> expandedMenu = jpegview_linux::BuildContextMenu(menuState, true);
		for (const MenuItem& compactItem : compactMenu) {
			if (compactItem.command == 0 ||
				compactItem.command == jpegview_linux::kContextMenuShowAdvanced) continue;
			const std::size_t matchingItems = static_cast<std::size_t>(std::count_if(
				expandedMenu.begin(), expandedMenu.end(), [&compactItem](const MenuItem& candidate) {
					return candidate.command == compactItem.command &&
						candidate.label == compactItem.label;
				}));
			Expect(matchingItems == 1,
				"compact command did not have exactly one matching expanded item: " + compactItem.label);
			const auto expandedItem = std::find_if(expandedMenu.begin(), expandedMenu.end(),
				[&compactItem](const MenuItem& candidate) {
					return candidate.command == compactItem.command &&
						candidate.label == compactItem.label;
				});
			Expect(expandedItem != expandedMenu.end(),
				"compact command was missing from the expanded context-menu catalog: " +
				compactItem.label);
			if (expandedItem != expandedMenu.end()) {
				Expect(expandedItem->mnemonic == compactItem.mnemonic &&
					expandedItem->mnemonicOffset == compactItem.mnemonicOffset,
					"shared context-menu item changed its mnemonic between compact and expanded views: " +
					compactItem.label);
			}
		}
		const auto findAdvancedConfiguration = [](const std::vector<MenuItem>& menu) {
			return std::find_if(menu.begin(), menu.end(), [](const MenuItem& item) {
				return item.command == jpegview_linux::kCommandAdvancedConfiguration;
			});
		};
		const auto compactAdvancedConfiguration = findAdvancedConfiguration(compactMenu);
		const auto expandedAdvancedConfiguration = findAdvancedConfiguration(expandedMenu);
		Expect(compactAdvancedConfiguration != compactMenu.end() &&
			expandedAdvancedConfiguration != expandedMenu.end() &&
			compactAdvancedConfiguration->mnemonic == expandedAdvancedConfiguration->mnemonic &&
			compactAdvancedConfiguration->mnemonicOffset == expandedAdvancedConfiguration->mnemonicOffset,
			"Advanced configuration changed its mnemonic between compact and expanded views");
	};

	ContextMenuState defaultState;
	expectStableSharedMnemonics(defaultState);
	ContextMenuState populatedState;
	populatedState.playbackMode = jpegview_linux::PlaybackMode::Slideshow;
	populatedState.animationAvailable = true;
	populatedState.imageAvailable = true;
	populatedState.losslessJpegAvailable = true;
	populatedState.pictureLevelsAvailable = true;
	populatedState.cropSelectionAvailable = true;
	populatedState.losslessJpegCropAvailable = true;
	populatedState.openWithApplicationNames = {"Photo Editor", u8"写真工具"};
	expectStableSharedMnemonics(populatedState);
	ContextMenuState archiveState = populatedState;
	archiveState.archiveMember = true;
	archiveState.openWithApplicationNames.clear();
	expectStableSharedMnemonics(archiveState);
	ContextMenuState unavailableState;
	unavailableState.parameterDatabaseAvailable = false;
	expectStableSharedMnemonics(unavailableState);

	ContextMenuState state;
	const std::vector<MenuItem> compact = jpegview_linux::BuildContextMenu(state, false);
	const auto open = std::find_if(compact.begin(), compact.end(), [](const MenuItem& item) {
		return item.command == IDM_OPEN;
	});
	Expect(open != compact.end() && open->mnemonic == 'o' && open->mnemonicOffset == 0,
		"Open image did not receive the expected O mnemonic");
	const int openIndex = static_cast<int>(std::distance(compact.begin(), open));
	Expect(jpegview_linux::MenuMnemonicMatches(compact, 'O') == std::vector<int>({openIndex}),
		"mnemonic matching was not case-insensitive or included ambiguous compact entries");
	for (const MenuItem& item : compact) {
		const bool actionable = !item.separator && item.command != 0 && item.enabled;
		if (!actionable) {
			Expect(item.mnemonic == '\0' && item.mnemonicOffset == -1,
				"separator, heading, or disabled item received a mnemonic");
			continue;
		}
		Expect(item.mnemonic >= 'a' && item.mnemonic <= 'z' && item.mnemonicOffset >= 0 &&
			static_cast<std::size_t>(item.mnemonicOffset) < item.label.size() &&
			std::tolower(static_cast<unsigned char>(item.label[
				static_cast<std::size_t>(item.mnemonicOffset)])) == item.mnemonic,
			"actionable item did not retain the exact character position used by its underline");
	}

	std::vector<MenuItem> repeated;
	for (int command = 1; command <= 6; ++command) {
		repeated.emplace_back("Item", command);
	}
	jpegview_linux::AssignMenuMnemonics(repeated);
	const char repeatedLetter = repeated.front().mnemonic;
	const std::vector<int> matches = jpegview_linux::MenuMnemonicMatches(repeated, repeatedLetter);
	Expect(matches.size() == 2 && matches.front() == 0,
		"mnemonic assignment did not share a letter after the candidate alphabet was exhausted");
	Expect(jpegview_linux::NextMenuMnemonicSelection(repeated, repeatedLetter, -1) == matches[0] &&
		jpegview_linux::NextMenuMnemonicSelection(repeated, repeatedLetter, matches[0]) == matches[1] &&
		jpegview_linux::NextMenuMnemonicSelection(repeated, repeatedLetter, matches[1]) == matches[0],
		"ambiguous mnemonic presses did not cycle matching commands in menu order");
	Expect(jpegview_linux::MenuMnemonicMatches(repeated, '!').empty() &&
		jpegview_linux::NextMenuMnemonicSelection(repeated, '!', -1) == -1,
		"non-letter mnemonic returned a selectable menu item");
}

void TestCropContextMenuCommandsAndModes() {
	using jpegview_linux::ContextMenuState;
	using jpegview_linux::CropSelectionMode;
	using jpegview_linux::MenuItem;
	ContextMenuState state;
	state.cropContextMenu = true;
	state.cropSelectionAvailable = true;
	state.losslessJpegCropAvailable = true;
	state.cropMode = CropSelectionMode::FixedAspect;
	state.cropAspectWidth = 16;
	state.cropAspectHeight = 9;
	state.userCropAspectWidth = 14;
	state.userCropAspectHeight = 11;
	const std::vector<MenuItem> items = jpegview_linux::BuildContextMenu(state, false);
	const auto find = [&items](int command) -> const MenuItem* {
		const auto found = std::find_if(items.begin(), items.end(), [command](const MenuItem& item) {
			return item.command == command;
		});
		return found == items.end() ? nullptr : &*found;
	};
	Expect(find(IDM_CROP_SEL) != nullptr && find(IDM_CROP_SEL)->enabled &&
		find(IDM_LOSSLESS_CROP_SEL) != nullptr && find(IDM_LOSSLESS_CROP_SEL)->enabled &&
		find(IDM_COPY_SEL) != nullptr && find(IDM_COPY_SEL)->enabled &&
		find(IDM_ZOOM_SEL) != nullptr && find(IDM_ZOOM_SEL)->enabled,
		"crop menu omitted an enabled selection action");
	Expect(find(IDM_CROPMODE_FREE) != nullptr &&
		find(IDM_CROPMODE_16_9) != nullptr && find(IDM_CROPMODE_16_9)->checked &&
		find(IDM_CROPMODE_USER) != nullptr &&
		find(IDM_CROPMODE_USER)->label == "  User aspect (14 : 11)",
		"crop menu omitted a mode, checked aspect, or configured user ratio");
	const MenuItem* cropSelectionMode = find(jpegview_linux::kCommandToggleSelectionMode);
	Expect(cropSelectionMode != nullptr && !cropSelectionMode->checked &&
		cropSelectionMode->shortcut == "Ctrl+E" && items.front().command ==
		jpegview_linux::kCommandToggleSelectionMode,
		"selection context menu did not put the crop-mode toggle first and unchecked");
	state.selectionModeEnabled = true;
	const std::vector<MenuItem> enabledModeItems = jpegview_linux::BuildContextMenu(state, false);
	const auto enabledMode = std::find_if(enabledModeItems.begin(), enabledModeItems.end(),
		[](const MenuItem& item) {
			return item.command == jpegview_linux::kCommandToggleSelectionMode;
		});
	Expect(enabledMode != enabledModeItems.end() && enabledMode->checked,
		"selection context menu did not check the active crop mode");
	state.losslessJpegCropAvailable = false;
	state.cropSelectionAvailable = false;
	const std::vector<MenuItem> unavailable = jpegview_linux::BuildContextMenu(state, true);
	const auto findUnavailable = [&unavailable](int command) -> const MenuItem* {
		const auto found = std::find_if(unavailable.begin(), unavailable.end(), [command](const MenuItem& item) {
			return item.command == command;
		});
		return found == unavailable.end() ? nullptr : &*found;
	};
	Expect(findUnavailable(IDM_CROP_SEL) != nullptr && !findUnavailable(IDM_CROP_SEL)->enabled &&
		findUnavailable(IDM_LOSSLESS_CROP_SEL) != nullptr &&
		!findUnavailable(IDM_LOSSLESS_CROP_SEL)->enabled &&
		findUnavailable(IDM_ZOOM_SEL) != nullptr && !findUnavailable(IDM_ZOOM_SEL)->enabled,
		"crop menu left selection actions enabled after the selection became unavailable");
	Expect(findUnavailable(jpegview_linux::kContextMenuShowAdvanced) == nullptr,
		"crop-only menu was incorrectly compacted into advanced options");
}

void TestContextMenuColumnLayoutAndNavigation() {
	using jpegview_linux::MenuColumn;
	using jpegview_linux::MenuItem;
	const std::vector<MenuItem> items = {
		{"Top", 10},
		{nullptr, 0, true},
		{"Heading", 0},
		{"Alpha", 20},
		{"Disabled", 30, false, false, false},
		{"Beta", 40},
		{nullptr, 0, true},
		{"Gamma", 50},
		{"Delta", 60},
		{"Last", 70},
	};
	const std::vector<MenuColumn> columns =
		jpegview_linux::LayoutMenuColumns(items, 43, 18, 7);
	Expect(columns.size() == 4, "long menu did not split into height-limited columns");
	const std::vector<MenuColumn> expected = {
		{0, 3, 43}, {3, 5, 36}, {5, 8, 43}, {8, 10, 36},
	};
	std::size_t nextItem = 0;
	for (std::size_t index = 0; index < columns.size(); ++index) {
		Expect(columns[index].begin == expected[index].begin &&
			columns[index].end == expected[index].end &&
			columns[index].height == expected[index].height,
			"column layout did not preserve sequential item ranges and separator heights");
		Expect(columns[index].begin == nextItem && columns[index].height <= 43,
			"column layout left a gap/overlap or exceeded the available content height");
		nextItem = columns[index].end;
	}
	Expect(nextItem == items.size(), "column layout omitted trailing menu items");

	const std::vector<MenuItem> shortMenu = {{"One", 1}, {nullptr, 0, true}, {"Two", 2}};
	const std::vector<MenuColumn> exactFit =
		jpegview_linux::LayoutMenuColumns(shortMenu, 43, 18, 7);
	Expect(exactFit.size() == 1 && exactFit[0].height == 43,
		"menu that exactly fits the available height was unnecessarily split");
	Expect(jpegview_linux::LayoutMenuColumns(shortMenu, 42, 18, 7).size() == 2,
		"menu overflowing by one pixel did not continue in a second column");
	const std::vector<MenuColumn> emptyLayout =
		jpegview_linux::LayoutMenuColumns({}, 1, 18, 7);
	Expect(emptyLayout.size() == 1 && emptyLayout[0].begin == 0 &&
		emptyLayout[0].end == 0 && emptyLayout[0].height == 0,
		"empty menu did not return a valid empty column");

	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[2], 5, 1) == 7,
		"Down did not skip a separator within its column");
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[2], 7, 1) == 5,
		"Down did not wrap within its column");
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[2], 5, -1) == 7,
		"Up did not wrap backward within its column");
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[2], -1, -1) == 7,
		"Up with no current item did not enter at the bottom of its column");
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, columns[1], 3, 1) == 3,
		"vertical navigation did not remain on the only enabled item in a column");
	Expect(jpegview_linux::NextMenuSelectionInColumn(items, {4, 5, 18}, 4, 1) == -1,
		"column containing only a disabled item returned a selection");

	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, 3, 1, 18, 7) == 5,
		"Right did not enter the adjacent column at the closest row");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, 7, -1, 18, 7) == 3,
		"Left did not choose the nearest actionable item in the previous column");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, 0, -1, 18, 7) == -1,
		"Left wrapped past the first column");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, 9, 1, 18, 7) == -1,
		"Right wrapped past the last column");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, -1, 1, 18, 7) == 0,
		"Right with no selection did not start at the first column");
	Expect(jpegview_linux::AdjacentMenuSelection(items, columns, -1, -1, 18, 7) == 8,
		"Left with no selection did not start at the last column");

	const std::vector<MenuItem> withEmptyColumn = {
		{"Enabled before", 1}, {"Disabled only", 2, false, false, false}, {"Enabled after", 3},
	};
	const std::vector<MenuColumn> sparseColumns = {{0, 1, 18}, {1, 2, 18}, {2, 3, 18}};
	Expect(jpegview_linux::AdjacentMenuSelection(withEmptyColumn, sparseColumns, 0, 1, 18, 7) == 2,
		"horizontal navigation did not skip a column with no actionable items");
}

void TestOverlayLayoutUsesContentWidthAndComfortableMargins() {
	jpegview_linux::OverlayLayout filename = jpegview_linux::FilenameOverlayLayout(60, 800);
	Expect(filename.x == 4 && filename.y == 4 && filename.width == 72 && filename.height == 20,
		"short filename overlay was not content-sized with six-pixel margins");
	Expect(filename.textWidth == 60, "filename overlay text area did not match content width");

	filename = jpegview_linux::FilenameOverlayLayout(900, 800);
	Expect(filename.width == 792 && filename.textWidth == 780,
		"long filename overlay did not clamp to the window insets");

	jpegview_linux::OverlayLayout info =
		jpegview_linux::InformationOverlayLayout(100, 3, 500, 300, false);
	Expect(info.x == 4 && info.y == 4 && info.width == 136 && info.height == 66,
		"EXIF overlay was not content-sized");
	Expect(info.textWidth == 100 && info.visibleLines == 3 && info.contentHeight == 66,
		"EXIF overlay margins, toggle room, or visible line count are incorrect");
	info = jpegview_linux::InformationOverlayLayout(100, 3, 500, 300, false,
		4, 6, 18, 20, true);
	Expect(info.width == 268 && info.height == 126 && info.textWidth == 100 &&
		info.visibleLines == 3 && info.contentHeight == 66,
		"expanded EXIF overlay did not reserve the Windows-sized histogram area");

	info = jpegview_linux::InformationOverlayLayout(100, 20, 500, 40, true);
	Expect(info.y == 28, "EXIF overlay did not move below visible filename overlay");
	Expect(info.height == 32 && info.visibleLines == 1,
		"EXIF overlay did not clamp vertically to a small window");

	filename = jpegview_linux::FilenameOverlayLayout(20, 4);
	Expect(filename.width == 1 && filename.textWidth == 1,
		"overlay layout did not remain valid for an extremely narrow window");
}

void TestViewerChromePaintPlans() {
	const jpegview_linux::OverlayLayout filenameLayout{4, 4, 112, 20, 100, 1};
	jpegview_linux::OverlayPaintPlan overlay = jpegview_linux::FilenameOverlayPaint(
		filenameLayout, "[1/2] image.jpg", 11, 6);
	Expect(overlay.panel.x == 4 && overlay.panel.y == 4 && overlay.panel.width == 112 &&
		overlay.panel.height == 20 && overlay.background.alpha == 205 &&
		overlay.text.size() == 1 && overlay.text[0].x == 10 && overlay.text[0].y == 8 &&
		overlay.text[0].color.red == 255,
		"filename overlay paint plan changed panel style or text alignment");
	const jpegview_linux::OverlayLayout infoLayout{4, 28, 160, 48, 100, 2, 48};
	jpegview_linux::InformationOverlayPaintPlan infoPaint = jpegview_linux::InformationOverlayPaint(infoLayout,
		{"heading", "details", "hidden"}, 18, 6);
	Expect(infoPaint.overlay.text.size() == 2 && infoPaint.overlay.text[0].y == 34 &&
		infoPaint.overlay.text[1].y == 52 && infoPaint.overlay.text[0].color.red == 255 &&
		infoPaint.overlay.text[1].color.red == 243 && infoPaint.overlay.text[1].color.green == 242 &&
		infoPaint.overlay.text[1].color.blue == 231 && infoPaint.spectrumButton.width == 18 &&
		infoPaint.spectrumLines.size() == 2 &&
		infoPaint.spectrumLines[0].y1 < infoPaint.spectrumLines[0].y2 &&
		jpegview_linux::Contains(infoPaint.spectrumButton, infoPaint.spectrumButton.x,
			infoPaint.spectrumButton.y) &&
		!jpegview_linux::Contains(infoPaint.spectrumButton,
			infoPaint.spectrumButton.x + infoPaint.spectrumButton.width,
			infoPaint.spectrumButton.y),
		"information overlay paint plan ignored visible lines or emphasis colors");
	infoPaint = jpegview_linux::InformationOverlayPaint(infoLayout,
		{"heading", "Location: 51.50000, -0.12000"}, 18, 6,
		false, nullptr, false, 1);
	Expect(infoPaint.gpsLocationLineIndex == 1 &&
		infoPaint.gpsLocationLink.x == 10 && infoPaint.gpsLocationLink.y == 52 &&
		infoPaint.gpsLocationLink.width == 100 && infoPaint.gpsLocationLink.height == 18 &&
		infoPaint.overlay.text[1].color.red == 120 &&
		infoPaint.overlay.text[1].color.green == 205 &&
		jpegview_linux::Contains(infoPaint.gpsLocationLink, 10, 52) &&
		!jpegview_linux::Contains(infoPaint.gpsLocationLink, 110, 52),
		"GPS location link paint plan did not identify and bound the clickable row");
	jpegview_linux::InformationOverlayPaintPlanCache gpsLinkCache;
	const auto gpsLinkBuilder = [&infoLayout] {
		return jpegview_linux::InformationOverlayPaint(infoLayout,
			{"heading", "Location: 51.50000, -0.12000"}, 18, 6,
			false, nullptr, false, 1);
	};
	const auto& gpsHovered = gpsLinkCache.GetOrBuild("gps-link", 10, 52, gpsLinkBuilder);
	Expect(gpsHovered.overlay.text[1].color.red == 255 &&
		gpsHovered.overlay.text[1].color.green == 205,
		"hovering the GPS link did not highlight its cached location row");
	const auto& gpsIdle = gpsLinkCache.GetOrBuild("gps-link", 0, 0, gpsLinkBuilder);
	Expect(gpsIdle.overlay.text[1].color.red == 120 &&
		gpsIdle.overlay.text[1].color.green == 205,
		"leaving the GPS link did not restore its link color");
	jpegview_linux::GrayscaleSpectrum spectrum{};
	spectrum[64] = 16;
	spectrum[192] = 4;
	const jpegview_linux::OverlayLayout spectrumLayout = jpegview_linux::InformationOverlayLayout(
		100, 3, 500, 300, false, 4, 6, 18, 20, true);
	infoPaint = jpegview_linux::InformationOverlayPaint(spectrumLayout,
		{"heading", "details", "hidden"}, 18, 6, true, &spectrum, true);
	Expect(infoPaint.spectrumLines.size() == 5 && infoPaint.spectrumButton.x == 248 &&
		infoPaint.spectrumButton.y == 46 && infoPaint.spectrumLines[2].x1 == 10 &&
		infoPaint.spectrumLines[2].y1 == 120 && infoPaint.spectrumLines[2].x2 == 266 &&
		infoPaint.spectrumLines[2].color.red == 255 &&
		infoPaint.spectrumLines[3].x1 == 74 && infoPaint.spectrumLines[3].y1 == 70 &&
		infoPaint.spectrumLines[3].y2 == 120 &&
		infoPaint.spectrumLines[4].x1 == 202 && infoPaint.spectrumLines[4].y1 == 95 &&
		infoPaint.spectrumLines[4].y2 == 120 &&
		infoPaint.spectrumLines[0].y1 > infoPaint.spectrumLines[0].y2 &&
		infoPaint.spectrumLines[0].color.red == 255,
		"expanded EXIF spectrum or collapse button was laid out incorrectly");
	int planBuilds = 0;
	jpegview_linux::InformationOverlayPaintPlanCache planCache;
	const auto planBuilder = [&planBuilds, &infoLayout] {
		++planBuilds;
		return jpegview_linux::InformationOverlayPaint(infoLayout,
			{"heading", "details"}, 18, 6);
	};
	const jpegview_linux::InformationOverlayPaintPlan& idlePlan =
		planCache.GetOrBuild("same-geometry", 0, 0, planBuilder);
	const int idleButtonRed = idlePlan.spectrumLines[0].color.red;
	const int idleButtonGreen = idlePlan.spectrumLines[0].color.green;
	const jpegview_linux::InformationOverlayPaintPlan& hoveredPlan =
		planCache.GetOrBuild("same-geometry", idlePlan.spectrumButton.x,
			idlePlan.spectrumButton.y, planBuilder);
	Expect(planBuilds == 1 && hoveredPlan.spectrumButton.x == idlePlan.spectrumButton.x &&
		hoveredPlan.spectrumLines[0].color.red == 255 &&
		(idleButtonRed != hoveredPlan.spectrumLines[0].color.red ||
			idleButtonGreen != hoveredPlan.spectrumLines[0].color.green),
		"hovering the histogram control rebuilt its layout or failed to recolor the cached icon");
	(void)planCache.GetOrBuild("same-geometry", 0, 0, planBuilder);
	const auto resizedPlan = planCache.GetOrBuild("resized", 0, 0,
		[&planBuilds, &spectrumLayout] {
			++planBuilds;
			return jpegview_linux::InformationOverlayPaint(spectrumLayout,
				{"heading", "details"}, 18, 6, true);
		});
	Expect(planBuilds == 2 && resizedPlan.overlay.panel.width == spectrumLayout.width,
		"overlay geometry changes did not rebuild the cached text and paint plan exactly once");

	jpegview_linux::NavigationPanelPaint navigation = jpegview_linux::BuildNavigationPanelPaint(
		800, 600, 341, 585, true, FileList::SortMode::LastModificationTime, 7, 18, 11);
	Expect(navigation.panel.x == 198 && navigation.panel.y == 568 &&
		navigation.panel.width == 403 && navigation.panel.height == 32 &&
		navigation.opacity == 255 && navigation.buttons.size() == 12,
		"navigation paint plan did not keep compact geometry or include both display-mode controls");
	Expect(navigation.buttons[0].rect.x == 204 && navigation.buttons[3].rect.x == 297 &&
		navigation.buttons[4].rect.x == 328 && navigation.buttons[5].rect.x == 367 &&
		navigation.buttons[7].rect.x == 437 && navigation.buttons[9].rect.x == 499 &&
		navigation.buttons[10].rect.x == 538 && navigation.buttons[11].rect.x == 569,
		"navigation paint plan lost section spacing");
	Expect(navigation.buttons[0].command == IDM_FIRST && navigation.buttons[0].lines.size() == 4 &&
		navigation.buttons[0].lines[0].x1 == 211 && navigation.buttons[0].lines[0].y1 == 578 &&
		navigation.buttons[0].lines[0].y2 == 590 &&
		navigation.buttons[0].lines[2].x1 == 223 &&
		navigation.buttons[0].lines[2].x2 == 218 &&
		navigation.buttons[0].foreground.red == 243 &&
		navigation.buttons[0].foreground.green == 242 &&
		navigation.buttons[0].foreground.blue == 231,
		"first-image navigation icon geometry is incorrect");
	Expect(navigation.buttons[4].command == jpegview_linux::kNavigationSortModeCommand &&
		navigation.buttons[4].hovered && navigation.buttons[4].text.size() == 1 &&
		navigation.buttons[4].text[0].text == "D" &&
		navigation.buttons[4].foreground.red == 255 &&
		navigation.buttons[4].foreground.green == 205 &&
		navigation.buttons[4].foreground.blue == 0,
		"navigation sort button did not expose state and hover in its paint plan");
	Expect(navigation.buttons[5].lines.empty() && navigation.buttons[5].text.size() == 1 &&
		navigation.buttons[5].text[0].text == "1:1" &&
		navigation.buttons[6].outlines.size() == 1 &&
		navigation.buttons[6].outlines[0].width == 14 &&
		navigation.buttons[7].lines.size() == 6 &&
		navigation.buttons[7].lines[0].x1 == 442 &&
		navigation.buttons[7].lines[0].x2 == 451 &&
		navigation.buttons[8].lines.size() == 6 &&
		navigation.buttons[9].command == jpegview_linux::kCommandToggleSelectionMode &&
		navigation.buttons[9].lines.size() == 8 &&
		navigation.buttons[10].command == jpegview_linux::kCommandToggleDoublePageMode &&
		navigation.buttons[10].outlines.size() == 2 &&
		navigation.buttons[11].command == jpegview_linux::kCommandToggleMangaReadingOrder &&
		navigation.buttons[11].outlines.size() == 2 && navigation.buttons[11].lines.size() == 7,
		"fit or rotation controls did not use the original Windows action glyphs");
	navigation = jpegview_linux::BuildNavigationPanelPaint(
		800, 600, -1, -1, true, FileList::SortMode::FileName, 7, 24, 11,
		false, false, false, true);
	Expect(navigation.buttons[5].text.size() == 1 &&
		navigation.buttons[5].text[0].text == "100%",
		"navigation panel did not identify fit-relative 100% instead of source-pixel 1:1");
	navigation = jpegview_linux::BuildNavigationPanelPaint(
		800, 600, -1, -1, false, FileList::SortMode::FileName, 7, 18, 11, true, true, true);
	Expect(navigation.opacity == 128 && navigation.buttons[0].foreground.alpha == 128 &&
		navigation.buttons[5].lines.size() == 12 && navigation.buttons[5].text.empty() &&
		navigation.buttons[4].text[0].text == "N" &&
		navigation.buttons[9].foreground.red == 255 &&
		navigation.buttons[9].foreground.green == 205 &&
		navigation.buttons[9].foreground.blue == 0 &&
		navigation.buttons[10].foreground.red == 255 &&
		navigation.buttons[11].foreground.red == 255,
		"fit-action navigation icon or name-order label is incorrect");

	Expect(jpegview_linux::NavigationTooltip(IDM_FULL_SCREEN_MODE, true, false,
		FileList::SortMode::FileName) == "Full screen mode (F11)" &&
		jpegview_linux::NavigationTooltip(IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, true, false,
			FileList::SortMode::FileName) == "Actual size of image (Space)" &&
		jpegview_linux::NavigationTooltip(IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, false, false,
			FileList::SortMode::FileName) == "Fit image to screen (Space)" &&
		jpegview_linux::NavigationTooltip(IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, false, false,
			FileList::SortMode::FileName, false, false, false, true) == "Fit image to screen" &&
		jpegview_linux::NavigationTooltip(IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, true, false,
			FileList::SortMode::FileName, false, false, false, true) == "Actual size of image" &&
		jpegview_linux::NavigationTooltip(IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS, true, false,
			FileList::SortMode::FileName, false, false, false, false, true) ==
			"Fit-relative zoom (100%) (Space)" &&
		jpegview_linux::NavigationTooltip(IDM_FULL_SCREEN_MODE, true, true,
			FileList::SortMode::FileName) == "Window mode (F11)" &&
		jpegview_linux::NavigationTooltip(IDM_NEXT, true, false,
			FileList::SortMode::FileName, false, false, true) == "Show next image (Left)" &&
		jpegview_linux::NavigationTooltip(IDM_PREV, true, false,
			FileList::SortMode::FileName, false, false, true) == "Show previous image (Right)" &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kNavigationSortModeCommand, true, false,
			FileList::SortMode::LastModificationTime).find("click for file name") != std::string::npos &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kCommandToggleSelectionMode, true, false,
			FileList::SortMode::FileName, false) == "Enable crop selection mode (Ctrl+E)" &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kCommandToggleSelectionMode, true, false,
			FileList::SortMode::FileName, true) == "Disable crop selection mode (Ctrl+E)" &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kCommandToggleDoublePageMode, true, false,
			FileList::SortMode::FileName, false, false) == "Enable double page mode (D)" &&
		jpegview_linux::NavigationTooltip(jpegview_linux::kCommandToggleMangaReadingOrder,
			true, false, FileList::SortMode::FileName, false, true, true) ==
			"Disable double page manga mode (J)",
		"navigation tooltip did not reflect fullscreen or sort state");
	const jpegview_linux::UiRect anchor{2, 5, 40, 40};
	overlay = jpegview_linux::NavigationTooltipPaint(anchor, "tip", 21, 11, 100, 60);
	Expect(overlay.panel.x == 4 && overlay.panel.y == 34 && overlay.panel.width == 37 &&
		overlay.panel.height == 22 && overlay.background.alpha == 215 &&
		overlay.text[0].x == 12,
		"navigation tooltip paint plan did not clamp or flip around its anchor");
	Expect(jpegview_linux::Contains(navigation.buttons[0].rect,
		navigation.buttons[0].rect.x, navigation.buttons[0].rect.y) &&
		!jpegview_linux::Contains(navigation.buttons[0].rect,
			navigation.buttons[0].rect.x + navigation.buttons[0].rect.width,
			navigation.buttons[0].rect.y),
		"viewer chrome hit testing did not use half-open rectangle bounds");
}

void TestThumbnailPanelLayoutPreloadAndSizing() {
	Expect(jpegview_linux::ThumbnailRowHeight(164, 1) == 112,
		"default thumbnail row height changed");
	Expect(jpegview_linux::ThumbnailRowHeight(48, 1) == 35,
		"narrow thumbnail panel did not create compact rows");
	Expect(jpegview_linux::ThumbnailRowHeight(240, 1) == 163,
		"wide thumbnail panel did not scale its rows with panel width");

	jpegview_linux::ThumbnailPanelLayout layout =
		jpegview_linux::CalculateThumbnailPanelLayout(800, 600, true, 164);
	Expect(layout.panelWidth == 164 && layout.imageX == 164 &&
		layout.imageWidth == 636 && layout.imageHeight == 600,
		"visible thumbnail panel did not reserve image viewport space");
	layout = jpegview_linux::CalculateThumbnailPanelLayout(800, 600, false, 164);
	Expect(layout.panelWidth == 0 && layout.imageX == 0 &&
		layout.imageWidth == 800 && layout.imageHeight == 600,
		"hidden thumbnail panel reduced the image viewport");
	layout = jpegview_linux::CalculateThumbnailPanelLayout(80, 40, true, 164);
	Expect(layout.panelWidth == 79 && layout.imageX == 79 &&
		layout.imageWidth == 1 && layout.imageHeight == 40,
		"thumbnail panel consumed the entire narrow image viewport");

	const std::vector<jpegview_linux::ThumbnailSlot> slots =
		jpegview_linux::ThumbnailPanelSlots(10, 5, 500, 100);
	Expect(slots.size() == 5, "thumbnail panel did not create one row per visible neighbor");
	for (std::size_t index = 0; index < slots.size(); ++index) {
		Expect(slots[index].fileIndex == index + 3 && slots[index].y == static_cast<int>(index) * 100,
			"thumbnail rows do not follow the active file-list order");
	}
	Expect(slots[2].current && slots[2].fileIndex == 5 && slots[2].y == 200,
		"current thumbnail was not centered in the panel");
	Expect(std::count_if(slots.begin(), slots.end(), [](const jpegview_linux::ThumbnailSlot& slot) {
		return slot.current;
	}) == 1, "thumbnail panel marked more than one current image");
	const std::vector<jpegview_linux::ThumbnailSlot> spreadSlots =
		jpegview_linux::ThumbnailPanelSlots(10, 5, 500, 100, std::nullopt, 6);
	Expect(spreadSlots.size() == 5, "spread thumbnail layout omitted visible rows");
	Expect(spreadSlots[2].current && !spreadSlots[2].doublePagePartner &&
		spreadSlots[3].fileIndex == 6 && !spreadSlots[3].current &&
		spreadSlots[3].doublePagePartner && !spreadSlots[4].doublePagePartner,
		"thumbnail panel did not distinguish the active double-page partner");
	Expect(std::count_if(spreadSlots.begin(), spreadSlots.end(), [](const auto& slot) {
		return slot.current || slot.doublePagePartner;
	}) == 2, "thumbnail panel did not highlight exactly both pages in the spread");
	Expect(jpegview_linux::ThumbnailIndexVisible(100, 5, 2, 520, 100) &&
		jpegview_linux::ThumbnailIndexVisible(100, 5, 8, 520, 100) &&
		!jpegview_linux::ThumbnailIndexVisible(100, 5, 1, 520, 100) &&
		!jpegview_linux::ThumbnailIndexVisible(100, 5, 9, 520, 100),
		"thumbnail visibility did not include clipped edge rows and exclude offscreen retained rows");
	Expect(!jpegview_linux::ThumbnailIndexVisible(0, 0, 0, 500, 100) &&
		!jpegview_linux::ThumbnailIndexVisible(4, 4, 0, 500, 100) &&
		!jpegview_linux::ThumbnailIndexVisible(4, 0, 4, 500, 100) &&
		!jpegview_linux::ThumbnailIndexVisible(4, 0, 0, 0, 100),
		"thumbnail visibility accepted an empty, invalid, or hidden panel request");
	const std::vector<jpegview_linux::ThumbnailSlot> markedSlots =
		jpegview_linux::ThumbnailPanelSlots(10, 5, 500, 100, 7);
	Expect(markedSlots[2].current && !markedSlots[2].marked && markedSlots[4].fileIndex == 7 &&
		markedSlots[4].marked && !markedSlots[4].current,
		"thumbnail panel did not distinguish the marked image from the current image");
	const std::vector<jpegview_linux::ThumbnailSlot> unmarkedVisibleSlots =
		jpegview_linux::ThumbnailPanelSlots(10, 5, 100, 100, 0);
	Expect(std::none_of(unmarkedVisibleSlots.begin(), unmarkedVisibleSlots.end(),
		[](const jpegview_linux::ThumbnailSlot& slot) { return slot.marked; }),
		"thumbnail panel marked an image outside the visible rows");

	const std::vector<jpegview_linux::ThumbnailSlot> firstSlots =
		jpegview_linux::ThumbnailPanelSlots(4, 0, 300, 100);
	Expect(firstSlots.size() == 2 && firstSlots[0].fileIndex == 0 && firstSlots[0].y == 100 &&
		firstSlots[0].current && firstSlots[1].fileIndex == 1 && firstSlots[1].y == 200,
		"thumbnail panel did not keep the first image centered without wrapping");
	Expect(jpegview_linux::ThumbnailPanelSlots(0, 0, 300, 100).empty(),
		"empty file list produced thumbnail rows");
	Expect(jpegview_linux::ThumbnailPanelSlots(4, 4, 300, 100).empty(),
		"invalid current index produced thumbnail rows");

	const std::vector<std::size_t> preload = jpegview_linux::ThumbnailPreloadOrder(7, 3, 7);
	Expect(preload == std::vector<std::size_t>({3, 2, 4, 1, 5, 0, 6}),
		"thumbnail preload order is not nearest-current-first");
	const std::vector<std::size_t> limited = jpegview_linux::ThumbnailPreloadOrder(20, 10, 4);
	Expect(limited == std::vector<std::size_t>({10, 9, 11, 8}),
		"thumbnail preload order did not honor the memory-cache limit");
	Expect(jpegview_linux::ThumbnailPreloadOrder(4, 0, 8) ==
		std::vector<std::size_t>({0, 1, 2, 3}),
		"thumbnail preload order failed at the first file");
	Expect(jpegview_linux::ThumbnailCacheCapacity(164, 112, 1,
		16u * 1024u * 1024u, 64) == 64,
		"thumbnail cache did not honor its entry limit");
	Expect(jpegview_linux::ThumbnailCacheCapacity(800, 536, 1,
		16u * 1024u * 1024u, 64) == 39,
		"thumbnail cache did not scale down for wide thumbnails");
	Expect(jpegview_linux::ThumbnailCacheCapacity(100, 103, 1, 1, 64) == 1,
		"thumbnail cache did not retain one entry below its pixel budget");
	Expect(jpegview_linux::ThumbnailCacheCapacity(0, 100, 1, 10000, 64) == 0 &&
		jpegview_linux::ThumbnailCacheCapacity(100, 2, 1, 10000, 64) == 0 &&
		jpegview_linux::ThumbnailCacheCapacity(100, 100, 1, 0, 64) == 0 &&
		jpegview_linux::ThumbnailCacheCapacity(100, 100, 1, 10000, 0) == 0,
		"thumbnail cache accepted invalid dimensions or limits");

	jpegview_linux::ThumbnailSize size = jpegview_linux::FitThumbnailSize(400, 200, 100, 80);
	Expect(size.width == 100 && size.height == 50,
		"wide thumbnail did not preserve its aspect ratio");
	size = jpegview_linux::FitThumbnailSize(100, 400, 80, 100);
	Expect(size.width == 25 && size.height == 100,
		"tall thumbnail did not preserve its aspect ratio");
	size = jpegview_linux::FitThumbnailSize(40, 20, 100, 80);
	Expect(size.width == 40 && size.height == 20,
		"small thumbnail was enlarged");
	Expect(jpegview_linux::FitThumbnailSize(0, 20, 100, 80).width == 0,
		"invalid image dimensions produced a thumbnail size");

	const jpegview_linux::ThumbnailRect thumbnail =
		jpegview_linux::ThumbnailImageRect(164, 109, 164, 200, 112, 1);
	Expect(thumbnail.x == 0 && thumbnail.y == 201 &&
		thumbnail.width == 164 && thumbnail.height == 109,
		"thumbnail row added horizontal margins or incorrect vertical margins");
}

void TestThumbnailRepositoryInterfaceUsesInMemoryImplementation() {
	std::unique_ptr<jpegview_linux::ThumbnailRepository> repository =
		jpegview_linux::MakeInMemoryThumbnailRepository();
	Expect(repository != nullptr,
		"thumbnail repository factory did not provide an implementation");
	repository->SetGeometry(8, 4);
	auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
	image->key = jpegview_linux::SourceKey("repository-interface");
	image->width = 4;
	image->height = 2;
	image->bgra.assign(4u * 2u * 4u, 127);
	const std::shared_ptr<const jpegview_linux::PreparedThumbnailImage> retained = image;
	Expect(repository->Store(retained) ==
		jpegview_linux::ThumbnailPixelStoreOutcome::Stored &&
		repository->Find(retained->key) == retained &&
		repository->Diagnostics().imageCount == 1 &&
		repository->Diagnostics().pixelBytes == retained->bgra.size(),
		"the in-memory thumbnail repository did not satisfy the shared repository contract");
	std::size_t retired = 0;
	repository->SetGeometry(4, 2, [&](const auto& imageToRetire) {
		if (imageToRetire == retained) ++retired;
	});
	Expect(retired == 1 && repository->Find(retained->key) == nullptr &&
		repository->Diagnostics().imageCount == 0 &&
		repository->Diagnostics().pixelBytes == 0,
		"the repository interface did not preserve geometry invalidation and retirement");
}

void TestThumbnailPixelRetentionAndTextureWindow() {
	using Image = jpegview_linux::PreparedThumbnailImage;
	using Repository = jpegview_linux::InMemoryThumbnailRepository;
	const auto MakeImage = [](const jpegview_linux::SourceKey& key,
		int width, int height, std::uint8_t value) {
		auto image = std::make_shared<Image>();
		image->key = key;
		image->width = width;
		image->height = height;
		if (width > 0 && height > 0) {
			image->bgra.assign(static_cast<std::size_t>(width) *
				static_cast<std::size_t>(height) * 4, value);
		}
		return std::shared_ptr<const Image>(std::move(image));
	};

	Repository repository;
	std::size_t retiredOnGeometryChange = 0;
	repository.SetGeometry(164, 109);
	const jpegview_linux::SourceKey firstKey("first-source");
	const auto firstImage = MakeImage(firstKey, 164, 109, 17);
	Expect(repository.Store(firstImage) ==
		jpegview_linux::ThumbnailPixelStoreOutcome::Stored &&
		repository.Find(firstKey) == firstImage &&
		repository.Diagnostics().imageCount == 1 &&
		repository.Diagnostics().pixelBytes == 164u * 109u * 4u,
		"thumbnail pixel repository did not retain prepared pixels and account their exact bytes");
	Expect(repository.Store(firstImage) ==
		jpegview_linux::ThumbnailPixelStoreOutcome::AlreadyPresent &&
		repository.Find(firstKey) == firstImage,
		"duplicate thumbnail preparation replaced the retained source allocation");

	const auto malformed = MakeImage(jpegview_linux::SourceKey("malformed"), 2, 2, 0);
	auto shortPixels = std::make_shared<Image>(*malformed);
	shortPixels->bgra.pop_back();
	Expect(repository.Store(shortPixels) ==
		jpegview_linux::ThumbnailPixelStoreOutcome::InvalidImage &&
		!repository.Find(shortPixels->key),
		"thumbnail repository retained a malformed pixel allocation");
	Expect(repository.Store(MakeImage(jpegview_linux::SourceKey("oversized"), 165, 1, 0)) ==
		jpegview_linux::ThumbnailPixelStoreOutcome::InvalidImage,
		"thumbnail repository accepted pixels outside its configured geometry");

	const auto released = repository.Erase(firstKey);
	Expect(released == firstImage && !repository.Find(firstKey) &&
		repository.Diagnostics().imageCount == 0 &&
		repository.Diagnostics().pixelBytes == 0,
		"erasing a thumbnail did not transfer its pixel owner or release accounting");
	Expect(repository.Store(firstImage) == jpegview_linux::ThumbnailPixelStoreOutcome::Stored,
		"thumbnail repository could not retain a source after an exact-key replacement");
	const jpegview_linux::SourceKey replacementKey("replacement-source");
	Expect(repository.Store(MakeImage(replacementKey, 1, 1, 42)) ==
		jpegview_linux::ThumbnailPixelStoreOutcome::Stored &&
		!repository.Find(jpegview_linux::SourceKey("old-source")) &&
		repository.Find(replacementKey) != nullptr,
		"source replacement reused thumbnail pixels under the former source identity");

	std::vector<jpegview_linux::SourceKey> keys;
	keys.reserve(15000);
	for (std::size_t index = 0; index < 15000; ++index) {
		keys.emplace_back("retained-" + std::to_string(index));
	}
	Repository largeRepository;
	largeRepository.SetGeometry(164, 109);
	for (const auto& key : keys) {
		Expect(largeRepository.Store(MakeImage(key, 1, 1, 7)) ==
			jpegview_linux::ThumbnailPixelStoreOutcome::Stored,
			"thumbnail pixel repository stopped retaining an active-list image");
	}
	const auto largeDiagnostics = largeRepository.Diagnostics();
	Expect(largeDiagnostics.imageCount == keys.size() &&
		largeDiagnostics.pixelBytes == keys.size() * 4 &&
		largeRepository.Find(keys.front()) != nullptr &&
		largeRepository.Find(keys.back()) != nullptr,
		"15,000 retained thumbnail pixels were lost or misaccounted");

	const std::vector<std::size_t> middleWindow =
		jpegview_linux::ThumbnailTextureWindowIndices(15000, 7500, 900, 112, true);
	const std::vector<std::size_t> firstWindow =
		jpegview_linux::ThumbnailTextureWindowIndices(15000, 0, 900, 112, true);
	const std::vector<std::size_t> lastWindow =
		jpegview_linux::ThumbnailTextureWindowIndices(15000, 14999, 900, 112, true);
	Expect(!middleWindow.empty() && middleWindow.size() <= 40 &&
		middleWindow.front() > 0 && middleWindow.back() < 15000 &&
		!firstWindow.empty() && firstWindow.front() == 0 && firstWindow.back() < 40 &&
		!lastWindow.empty() && lastWindow.back() == 14999 && lastWindow.front() > 14959,
		"thumbnail texture window scaled with catalog size or failed at a list boundary");
	const std::vector<std::size_t> pinnedWindow =
		jpegview_linux::ThumbnailTextureWindowIndices(15000, 7500, 900, 112, true, 14999);
	Expect(std::binary_search(pinnedWindow.begin(), pinnedWindow.end(), 14999) &&
		pinnedWindow.size() == middleWindow.size() + 1 &&
		jpegview_linux::ThumbnailTextureWindowIndices(15000, 7500, 900, 112, false).empty() &&
		jpegview_linux::ThumbnailTextureWindowIndices(15000, 7500, 900, 112,
			false, 14999) == std::vector<std::size_t>({14999}),
		"hidden thumbnail panel or confirmation preview pin selected the wrong texture set");
	const std::vector<std::size_t> revisitedWindow =
		jpegview_linux::ThumbnailTextureWindowIndices(15000, 0, 900, 112, true);
	Expect(largeRepository.Find(keys.front()) != nullptr &&
		largeRepository.Find(keys.back()) != nullptr &&
		!revisitedWindow.empty() && revisitedWindow.size() <= 40 &&
		largeRepository.Diagnostics().imageCount == keys.size(),
		"moving the bounded texture window discarded reusable thumbnail pixels");

	std::size_t retiredOnClear = 0;
	largeRepository.SetGeometry(240, 159, [&retiredOnGeometryChange](const auto&) {
		++retiredOnGeometryChange;
	});
	Expect(retiredOnGeometryChange == keys.size() &&
		largeRepository.Diagnostics().imageCount == 0 &&
		largeRepository.Diagnostics().pixelBytes == 0,
		"incompatible thumbnail geometry did not release every retained pixel allocation");
	largeRepository.Store(MakeImage(keys.front(), 1, 1, 9));
	largeRepository.Clear([&retiredOnClear](const auto&) { ++retiredOnClear; });
	Expect(retiredOnClear == 1 && largeRepository.Diagnostics().imageCount == 0,
		"thumbnail repository clear did not hand its final pixel owner to retirement");
}

void TestThumbnailCacheSchedulingAndEviction() {
	const auto Configure = [](jpegview_linux::ThumbnailCacheScheduler& scheduler,
		const std::vector<std::string>& keys, std::size_t current, std::size_t capacity) {
		scheduler.SetCatalog(keys, capacity);
		scheduler.SetCurrent(current);
		scheduler.SetGeometry(100, 60);
	};
	const auto TakeOne = [](jpegview_linux::ThumbnailCacheScheduler& scheduler,
		std::uint32_t now, const std::function<bool(
			const jpegview_linux::ThumbnailLoadRequest&)>& permitted = {})
		-> std::optional<jpegview_linux::ThumbnailLoadRequest> {
		auto requests = scheduler.TakeNext(now, 1, permitted);
		if (requests.empty()) return std::nullopt;
		return requests.front();
	};

	jpegview_linux::ThumbnailCacheScheduler gated;
	Configure(gated, {"current", "visible", "distant"}, 0, 3);
	Expect(gated.TakeNext(0, 1, [](const jpegview_linux::ThumbnailLoadRequest&) {
		return false;
	}).empty() && gated.PendingCount() == 3,
		"a blocked thumbnail admission consumed its pending request");
	const auto visibleRequest = TakeOne(gated, 0, [](const auto& request) {
		return request.fileIndex < 2;
	});
	Expect(visibleRequest.has_value() && visibleRequest->key == "current" &&
		gated.Complete(*visibleRequest, 0, 0).empty(),
		"a permitted visible thumbnail could not pass the interaction admission gate");
	const auto nextVisible = TakeOne(gated, 0, [](const auto& request) {
		return request.fileIndex < 2;
	});
	Expect(nextVisible.has_value() && nextVisible->key == "visible" &&
		gated.Complete(*nextVisible, 0, 0).empty() &&
		!TakeOne(gated, 0, [](const auto& request) { return request.fileIndex < 2; }).has_value() &&
		gated.PendingCount() == 1,
		"distant thumbnail admission bypassed the visible-index policy or consumed its queue row");

	const std::vector<std::string> keys = {"a", "b", "c", "d", "e"};
	jpegview_linux::ThumbnailCacheScheduler retained;
	Configure(retained, keys, 2, keys.size());
	Expect(retained.PendingCount() == keys.size(),
		"full-list thumbnail retention unexpectedly evicted an entry");
	for (std::size_t completed = 0; completed < keys.size(); ++completed) {
		const auto retainedRequest = TakeOne(retained, 0);
		Expect(retainedRequest.has_value(),
			"full-list thumbnail retention stopped before every file was cached");
		Expect(retained.Complete(*retainedRequest, 0, 0).empty(),
			"full-list thumbnail retention evicted a completed thumbnail");
	}
	retained.SetCurrent(4);
	const std::vector<std::string> reverseKeys(keys.rbegin(), keys.rend());
	Expect(retained.SetCatalog(reverseKeys).empty(),
		"sorting evicted thumbnail pixels for sources retained in the catalog");
	retained.SetCurrent(0);
	Expect(retained.CacheSize() == keys.size() && retained.PendingCount() == 0,
		"navigation or sorting dropped thumbnails retained for the active file list");

	jpegview_linux::ThumbnailCacheScheduler boundaries;
	Configure(boundaries, {"first", "second", "middle", "last"}, 0, 4);
	auto boundaryBatch = boundaries.TakeNext(0, 4);
	Expect(boundaryBatch.size() == 4 && boundaryBatch[0].fileIndex == 0 &&
		boundaryBatch[1].fileIndex == 1 && boundaryBatch[2].fileIndex == 2 &&
		boundaryBatch[3].fileIndex == 3,
		"thumbnail scheduling did not advance inward from the first catalog boundary");
	jpegview_linux::ThumbnailCacheScheduler lastBoundary;
	Configure(lastBoundary, {"first", "second", "middle", "last"}, 3, 4);
	boundaryBatch = lastBoundary.TakeNext(0, 4);
	Expect(boundaryBatch.size() == 4 && boundaryBatch[0].fileIndex == 3 &&
		boundaryBatch[1].fileIndex == 2 && boundaryBatch[2].fileIndex == 1 &&
		boundaryBatch[3].fileIndex == 0,
		"thumbnail scheduling did not advance inward from the last catalog boundary");

	jpegview_linux::ThumbnailCacheScheduler scheduler;
	Configure(scheduler, keys, 2, 3);
	Expect(scheduler.PendingCount() == 3,
		"thumbnail scheduler did not bound its work set to cache capacity");
	auto request = TakeOne(scheduler, 100);
	Expect(request.has_value() && request->fileIndex == 2 && request->key == "c",
		"thumbnail scheduler did not start with the current image");
	Expect(scheduler.Complete(*request, 100).empty() && scheduler.IsCached("c"),
		"thumbnail scheduler did not record completed work");
	Expect(!TakeOne(scheduler, 124).has_value(), "thumbnail scheduler ignored its decode pacing deadline");
	request = TakeOne(scheduler, 125);
	Expect(request.has_value() && request->key == "b",
		"thumbnail scheduler did not prefer the preceding equidistant image");
	scheduler.Complete(*request, 125);
	request = TakeOne(scheduler, 150);
	Expect(request.has_value() && request->key == "d",
		"thumbnail scheduler did not continue in nearest-first order");
	scheduler.Complete(*request, 150);
	Expect(scheduler.CacheSize() == 3 && scheduler.PendingCount() == 0 &&
		scheduler.TakeNext(175, keys.size()).empty(),
		"thumbnail scheduler cache/work accounting is incorrect");

	const auto beforeCatalogShrink = scheduler.OperationCounts();
	const std::vector<std::string> evicted = scheduler.SetCatalog(keys, 2);
	scheduler.SetCurrent(2);
	Expect(evicted == std::vector<std::string>({"d"}) && scheduler.IsCached("c") &&
		scheduler.IsCached("b") && !scheduler.IsCached("d") && scheduler.PendingCount() == 0 &&
		scheduler.TakeNext(175, keys.size()).empty() &&
		scheduler.OperationCounts().trimEntriesVisited > beforeCatalogShrink.trimEntriesVisited,
		"shrinking the thumbnail window requeued a source outside the eligible neighborhood");
	scheduler.SetCatalog(keys, 3);
	request = TakeOne(scheduler, 175);
	Expect(request.has_value() && request->key == "d",
		"growing the thumbnail window did not admit its newly eligible neighbor");
	const jpegview_linux::ThumbnailLoadRequest stale = *request;
	const std::vector<std::string> reorderedKeys(keys.rbegin(), keys.rend());
	scheduler.SetCatalog(reorderedKeys, 3);
	scheduler.SetCurrent(4);
	Expect(scheduler.Complete(stale, 151).empty() && !scheduler.IsCached("d"),
		"thumbnail scheduler accepted work from an obsolete catalog order");

	jpegview_linux::ThumbnailCacheScheduler finiteWindow;
	Configure(finiteWindow, keys, 2, 3);
	const auto initialWindow = finiteWindow.TakeNext(0, keys.size());
	Expect(initialWindow.size() == 3 && initialWindow[0].key == "c" &&
		initialWindow[1].key == "b" && initialWindow[2].key == "d",
		"finite thumbnail capacity did not select the nearest-first working window");
	for (const auto& item : initialWindow) finiteWindow.Complete(item, 0, 0);
	Expect(finiteWindow.CacheSize() == 3 && finiteWindow.PendingCount() == 0 &&
		!finiteWindow.IsCached("a") && !finiteWindow.IsCached("e") &&
		finiteWindow.TakeNext(0, keys.size()).empty(),
		"finite thumbnail window did not quiesce after filling its nearest eligible entries");
	const auto beforeWindowMove = finiteWindow.OperationCounts();
	const std::vector<std::string> navigationEvictions = finiteWindow.SetCurrent(4);
	Expect(navigationEvictions == std::vector<std::string>({"b"}) &&
		finiteWindow.PendingCount() == 1 &&
		finiteWindow.OperationCounts().trimEntriesVisited > beforeWindowMove.trimEntriesVisited,
		"navigation did not replace an out-of-window cache entry with newly eligible work");
	request = TakeOne(finiteWindow, 0);
	Expect(request.has_value() && request->key == "e" &&
		finiteWindow.Complete(*request, 0, 0).empty() && finiteWindow.PendingCount() == 0,
		"moving the current image did not make farther thumbnail work progress");
	finiteWindow.SetCatalog(keys, 4);
	request = TakeOne(finiteWindow, 0);
	Expect(request.has_value() && request->key == "b" &&
		finiteWindow.Complete(*request, 0, 0).empty(),
		"increasing thumbnail capacity did not admit the next nearest file");
	finiteWindow.SetCatalog(keys, 5);
	request = TakeOne(finiteWindow, 0);
	Expect(request.has_value() && request->key == "a" &&
		finiteWindow.Complete(*request, 0, 0).empty() && finiteWindow.PendingCount() == 0,
		"increasing thumbnail capacity did not make the farthest file eligible");

	scheduler.Clear();
	Expect(scheduler.CacheSize() == 0 && scheduler.PendingCount() == 0,
		"thumbnail scheduler clear retained cache or work state");
	Configure(scheduler, {"wrap-a", "wrap-b"}, 0, 2);
	request = TakeOne(scheduler, 0xfffffffau);
	Expect(request.has_value() && request->key == "wrap-a", "wraparound pacing fixture did not start");
	scheduler.Complete(*request, 0xfffffffau, 10);
	Expect(!TakeOne(scheduler, 3).has_value(), "thumbnail pacing deadline fired early across tick wraparound");
	request = TakeOne(scheduler, 4);
	Expect(request.has_value() && request->key == "wrap-b",
		"thumbnail pacing deadline did not fire at tick wraparound");
	scheduler.Complete(*request, 4);
	const std::vector<std::string> zeroCapacityEvictions =
		scheduler.SetCatalog({"wrap-a"}, 0);
	Expect(zeroCapacityEvictions.size() == 2 && scheduler.CacheSize() == 0,
		"zero-capacity thumbnail cache retained its protected entry");

	jpegview_linux::ThumbnailCacheScheduler external;
	Configure(external, keys, 2, 2);
	Expect(external.Store("c").empty() && external.Store("b").empty() &&
		external.Store("e").empty() && external.IsCached("c") && external.IsCached("b") &&
		!external.IsCached("e"),
		"thumbnail scheduler retained externally prepared pixels outside its working window");

	jpegview_linux::ThumbnailCacheScheduler failed;
	Configure(failed, {"bad", "good"}, 0, 2);
	const auto failedRequest = TakeOne(failed, 0);
	Expect(failedRequest.has_value() && failedRequest->key == "bad",
		"thumbnail failure fixture did not select its first source");
	failed.Fail(*failedRequest, 0, 0);
	Expect(failed.IsFailed("bad") && !failed.IsCached("bad") &&
		TakeOne(failed, 0).has_value() && !TakeOne(failed, 0).has_value(),
		"thumbnail decode failure was recorded as cached pixels or retried in the same plan");
	failed.SetCatalog({"good"});
	failed.SetCurrent(0);
	Expect(!failed.IsFailed("bad") && TakeOne(failed, 0).has_value(),
		"thumbnail failure state retained a source removed from the active list");

	jpegview_linux::ThumbnailCacheScheduler geometry;
	Configure(geometry, {"unchanged", "removed"}, 0, 2);
	const auto survivesNavigation = TakeOne(geometry, 0);
	Expect(survivesNavigation.has_value(), "navigation fixture did not admit its first thumbnail");
	geometry.SetCurrent(1);
	Expect(geometry.IsCurrent(*survivesNavigation) &&
		geometry.Complete(*survivesNavigation, 0, 0).empty() &&
		geometry.IsCached("unchanged"),
		"navigation invalidated useful in-flight work for an unchanged catalog");
	const auto cancelledGeometry = TakeOne(geometry, 0);
	Expect(cancelledGeometry.has_value(), "geometry fixture did not admit its remaining thumbnail");
	geometry.Retry(*cancelledGeometry);
	const auto staleGeometry = TakeOne(geometry, 0);
	Expect(staleGeometry.has_value() && staleGeometry->key == cancelledGeometry->key,
		"cancelled thumbnail work was not returned to the pending cursor");
	geometry.SetGeometry(200, 120);
	Expect(!geometry.IsCurrent(*staleGeometry) &&
		geometry.Complete(*staleGeometry, 0, 0).empty() && geometry.CacheSize() == 0,
		"thumbnail scheduler accepted completion from obsolete geometry");
	const auto staleRemoval = TakeOne(geometry, 0);
	Expect(staleRemoval.has_value(), "catalog-removal fixture did not admit a thumbnail");
	Expect(geometry.Store("removed").empty(),
		"catalog-removal fixture could not retain its completed source");
	const std::vector<std::string> deletedTexture = geometry.SetCatalog({"unchanged"});
	Expect(!geometry.IsCurrent(*staleRemoval) &&
		geometry.Complete(*staleRemoval, 0, 0).empty() &&
		!geometry.IsCached("removed") && deletedTexture == std::vector<std::string>({"removed"}),
		"thumbnail scheduler accepted a deleted catalog entry");

	std::vector<std::string> largeCatalog;
	largeCatalog.reserve(15000);
	for (std::size_t index = 0; index < 15000; ++index) {
		largeCatalog.push_back("large-" + std::to_string(index));
	}
	jpegview_linux::ThumbnailCacheScheduler large;
	large.SetCatalog(std::move(largeCatalog));
	large.SetCurrent(0);
	large.SetGeometry(100, 60);
	const auto beforeCurrentUpdates = large.OperationCounts();
	for (std::size_t index = 0; index < 1000; ++index) large.SetCurrent(index);
	const auto afterCurrentUpdates = large.OperationCounts();
	Expect(afterCurrentUpdates.catalogEntriesVisited == beforeCurrentUpdates.catalogEntriesVisited &&
		afterCurrentUpdates.currentEntriesVisited == 0 &&
		afterCurrentUpdates.currentUpdates == beforeCurrentUpdates.currentUpdates + 1000 &&
		large.PendingCount() == 15000,
		"thumbnail navigation inspected or rebuilt the large catalog");
	const auto boundedBatch = large.TakeNext(0, 3);
	Expect(boundedBatch.size() == 3 && boundedBatch[0].fileIndex == 999 &&
		boundedBatch[1].fileIndex == 998 && boundedBatch[2].fileIndex == 1000 &&
		large.PendingCount() == 14997,
		"large-catalog nearest-first cursor did not produce only the requested bounded batch");
	const auto beforeLargeFill = large.OperationCounts();
	for (std::size_t index = 0; index < 15000; ++index) {
		Expect(large.Store("large-" + std::to_string(index)).empty(),
			"whole-catalog thumbnail retention evicted an entry while filling its cache");
	}
	const auto afterLargeFill = large.OperationCounts();
	Expect(large.CacheSize() == 15000 &&
		afterLargeFill.trimEntriesVisited == beforeLargeFill.trimEntriesVisited,
		"filling an in-capacity whole-catalog cache scanned cached entries during Trim");
	const auto beforeLargeNavigation = large.OperationCounts();
	for (std::size_t index = 0; index < 1000; ++index) large.SetCurrent(index);
	const auto afterLargeNavigation = large.OperationCounts();
	Expect(afterLargeNavigation.trimEntriesVisited == beforeLargeNavigation.trimEntriesVisited &&
		afterLargeNavigation.currentUpdates == beforeLargeNavigation.currentUpdates + 1000 &&
		large.CacheSize() == 15000,
		"navigation scanned or evicted entries from a populated whole-catalog cache");
}

void TestThumbnailCatalogReplacementIdentity() {
	jpegview_linux::ThumbnailCatalogRevisionTracker revisions;
	jpegview_linux::ThumbnailCacheScheduler scheduler;
	constexpr std::uint64_t equalMutationRevision = 0;
	if (revisions.NeedsUpdate(equalMutationRevision)) {
		scheduler.SetCatalog({"old-a", "old-b"});
		revisions.MarkUpdated(equalMutationRevision);
	}
	scheduler.SetCurrent(0);
	scheduler.SetGeometry(100, 60);
	const auto oldRequest = scheduler.TakeNext(0, 1);
	Expect(oldRequest.size() == 1 && oldRequest.front().key == "old-a",
		"initial thumbnail catalog fixture did not serve its first source");

	revisions.NoteReplacement();
	Expect(revisions.NeedsUpdate(equalMutationRevision),
		"whole-list replacement with an equal per-instance mutation revision was not detected");
	if (revisions.NeedsUpdate(equalMutationRevision)) {
		scheduler.SetCatalog({"new-a", "new-b"});
		revisions.MarkUpdated(equalMutationRevision);
	}
	scheduler.SetCurrent(1);
	const auto replacementRequest = scheduler.TakeNext(0, 1);
	Expect(replacementRequest.size() == 1 && replacementRequest.front().key == "new-b" &&
		!scheduler.IsCurrent(oldRequest.front()) && scheduler.IsCurrent(replacementRequest.front()),
		"thumbnail scheduler did not serve the replacement catalog with the same mutation revision");
}

void TestThumbnailSourceIdentityReplacementInvalidates() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "same-path.png";
	const fs::path replacement = temporary.path() / "replacement.tmp";
	WriteBytes(source, {1, 2, 3, 4});
	FileList files({temporary.path().string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 1 && files.Current() == source,
		"same-path thumbnail replacement fixture did not enter the file list");
	const jpegview_linux::SourceKey originalKey = files.DescriptorAt(0)->Key();

	jpegview_linux::ThumbnailCacheScheduler scheduler;
	scheduler.SetSourceCatalog({originalKey});
	scheduler.SetCurrent(0);
	scheduler.SetGeometry(100, 60);
	Expect(scheduler.Store(originalKey).empty() && scheduler.IsCached(originalKey),
		"same-path thumbnail replacement fixture could not retain its initial thumbnail");

	WriteBytes(replacement, {4, 3, 2, 1});
	std::error_code renameError;
	fs::rename(replacement, source, renameError);
	Expect(!renameError && fs::file_size(source) == 4,
		"same-size file replacement fixture could not replace its inode");
	Expect(files.Reload(source), "same-path file replacement could not reload its file list");
	const jpegview_linux::SourceKey replacementKey = files.DescriptorAt(0)->Key();
	Expect(replacementKey != originalKey,
		"same-size file replacement did not change the file-list source key");
	scheduler.SetSourceCatalog({replacementKey});
	Expect(!scheduler.IsCached(originalKey) && !scheduler.IsCached(replacementKey),
		"thumbnail cache retained old pixels after a same-size inode replacement and reload");
}

void TestThumbnailBulkEvictionUsesExactKeys() {
	constexpr std::size_t entryCount = 15000;
	std::vector<jpegview_linux::SourceKey> original;
	original.reserve(entryCount);
	for (std::size_t index = 0; index < entryCount; ++index) {
		jpegview_linux::SourceIdentity identity;
		identity.device = 1;
		identity.inode = index + 1;
		identity.size = 128;
		identity.modifiedSeconds = 1700000000;
		identity.modifiedNanoseconds = static_cast<std::int64_t>(index);
		identity.valid = true;
		original.emplace_back("/bulk/thumb-" + std::to_string(index) + ".png", identity);
	}
	jpegview_linux::ThumbnailCacheScheduler scheduler;
	scheduler.SetSourceCatalog(original, entryCount);
	scheduler.SetSourceCurrent(0);
	scheduler.SetGeometry(100, 60);
	std::unordered_map<jpegview_linux::SourceKey, std::size_t,
		jpegview_linux::SourceKeyHash> rendererCache;
	rendererCache.reserve(entryCount * 2);
	for (std::size_t index = 0; index < entryCount; ++index) {
		Expect(scheduler.Store(original[index]).empty(),
			"bulk eviction fixture unexpectedly trimmed its scheduled thumbnail keys");
		rendererCache.emplace(original[index], index);
	}

	std::vector<jpegview_linux::SourceKey> replacement;
	replacement.reserve(entryCount);
	std::vector<jpegview_linux::SourceKey> replacementByIndex(entryCount);
	std::unordered_set<jpegview_linux::SourceKey, jpegview_linux::SourceKeyHash> expectedOld;
	for (std::size_t index = 0; index < entryCount; ++index) {
		if ((index % 2) != 0) {
			replacement.push_back(original[index]);
			continue;
		}
		expectedOld.insert(original[index]);
		auto identity = original[index].backingIdentity;
		identity.inode += entryCount;
		const jpegview_linux::SourceKey newIdentity(original[index].logicalPath, identity);
		replacement.push_back(newIdentity);
		replacementByIndex[index] = newIdentity;
		rendererCache.emplace(newIdentity, index + entryCount);
	}
	const std::vector<jpegview_linux::SourceKey> evicted =
		scheduler.SetSourceCatalog(std::move(replacement), entryCount);
	std::unordered_set<jpegview_linux::SourceKey, jpegview_linux::SourceKeyHash> actualOld(
		evicted.begin(), evicted.end());
	Expect(actualOld == expectedOld,
		"source catalog replacement did not return the exact old identities for changed paths");
	const jpegview_linux::ThumbnailCacheEvictionCounts counts =
		jpegview_linux::EraseThumbnailCacheEntries(rendererCache, evicted,
			[](std::size_t&) {});
	Expect(counts.keyLookups == evicted.size() &&
		counts.entriesErased == evicted.size() && rendererCache.size() == entryCount &&
		actualOld.size() == entryCount / 2,
		"bulk thumbnail invalidation work scaled with cache entries instead of evicted keys");
	for (std::size_t index = 0; index < entryCount; ++index) {
		const bool changed = (index % 2) == 0;
		const bool staleKeyPresent = rendererCache.find(original[index]) != rendererCache.end();
		Expect(staleKeyPresent == !changed,
			"bulk eviction did not remove exactly the stale source identities");
		if (changed) {
			Expect(rendererCache.find(replacementByIndex[index]) != rendererCache.end(),
				"same-path replacement texture was erased with the stale identity");
		}
	}
}

void TestDisplayCacheReportsDeletedSourceIdentity() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "deleted-source.png";
	WriteBytes(source, {1, 2, 3, 4});
	FileList files({temporary.path().string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 1 && files.Current() == source,
		"deleted-source worker fixture did not enter its file list");
	const jpegview_linux::SourceDescriptor descriptor = *files.DescriptorAt(0);
	const fs::path stagedRecreation = temporary.path() / "recreated-source.tmp";
	WriteBytes(stagedRecreation, {5, 6, 7, 8});
	const auto request = jpegview_linux::MakeDisplayImageRequest(descriptor,
		DisplayCacheTestImage(4, 4), 0, 2, 2, false);
	std::mutex mutex;
	std::condition_variable changed;
	bool started = false;
	bool release = false;
	const auto processor = [&](const jpegview_linux::DisplayImageRequest&) {
		{
			std::unique_lock<std::mutex> lock(mutex);
			started = true;
			changed.notify_all();
			changed.wait(lock, [&] { return release; });
		}
		return jpegview_linux::DisplayImageCache::ImagePtr{};
	};
	jpegview_linux::DisplayImageCache cache(64, 1, processor);
	ScopedConditionRelease releaseProcessor(mutex, changed, release);
	cache.RequestBackground(request);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return started;
		});
	}
	std::error_code removeError;
	const bool removed = fs::remove(source, removeError);
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	changed.notify_all();
	Expect(reachedBarrier && removed && !removeError &&
		cache.WaitUntilIdle(std::chrono::seconds(2)),
		"deleted-source cache worker did not reach and leave its deterministic barrier");
	const auto notices = cache.TakeChangedSources();
	Expect(notices.size() == 1 && notices.front().previous == descriptor.Key() &&
		notices.front().observed.LogicalPath() == descriptor.LogicalPath() &&
		!notices.front().observed.Valid() &&
		!notices.front().observed.BackingIdentity().valid &&
		cache.TakeCompleted(1).empty(),
		"deleted source did not evict its prior cache identity with an invalid descriptor notice");
	const jpegview_linux::SourceChangeNotice notice = notices.front();
	Expect(files.RefreshSourceDescriptor(notice.previous, notice.observed) &&
		!files.DescriptorAt(0)->Key().Valid(),
		"file list did not apply a worker notice for its exact deleted-source key");
	std::error_code recreateError;
	fs::rename(stagedRecreation, source, recreateError);
	Expect(!recreateError && fs::file_size(source) == 4,
		"deleted-source fixture could not atomically install a distinct replacement inode");
	const jpegview_linux::SourceDescriptor recreated =
		jpegview_linux::DescribeImageSource(source);
	const jpegview_linux::SourceKey missingKey = files.DescriptorAt(0)->Key();
	Expect(recreated.Valid() && recreated.Key() != notice.previous,
		"recreated source did not produce a distinct valid descriptor");
	Expect(files.RefreshSourceDescriptor(missingKey, recreated) &&
		files.DescriptorAt(0)->Key() == recreated.Key(),
		"file list did not accept an exact refresh from its invalid missing-source key");
	Expect(!files.RefreshSourceDescriptor(notice.previous, notice.observed) &&
		files.DescriptorAt(0)->Key() == recreated.Key(),
		"delayed worker notice overwrote a newer descriptor for the same logical path");
}

void TestDecodedCacheReportsDeletedSourceAfterDecodeFailure() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "deleted-during-decode.png";
	WriteBytes(source, {1, 2, 3, 4});
	const jpegview_linux::SourceDescriptor descriptor =
		jpegview_linux::DescribeImageSource(source);
	std::mutex mutex;
	std::condition_variable changed;
	bool started = false;
	bool release = false;
	const auto decoder = [&](const fs::path&, DecodedImage&, std::string& error) {
		{
			std::unique_lock<std::mutex> lock(mutex);
			started = true;
			changed.notify_all();
			changed.wait(lock, [&] { return release; });
		}
		error = "source disappeared during decode";
		return false;
	};
	jpegview_linux::DecodedImageCache cache(64, decoder);
	ScopedConditionRelease releaseDecoder(mutex, changed, release);
	cache.RequestBackground(descriptor, {});
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return started;
		});
	}
	std::error_code removeError;
	const bool removed = fs::remove(source, removeError);
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	changed.notify_all();
	Expect(reachedBarrier && removed && !removeError &&
		cache.WaitUntilIdle(std::chrono::seconds(2)),
		"decoded-cache worker did not reach and leave its deterministic deletion barrier");
	const std::vector<jpegview_linux::SourceChangeNotice> notices =
		cache.TakeChangedSources();
	Expect(notices.size() == 1 && notices.front().previous == descriptor.Key() &&
		notices.front().observed.LogicalPath() == descriptor.LogicalPath() &&
		!notices.front().observed.Valid() && cache.CachedImages() == 0,
		"failed decode after source deletion did not report its invalid observed identity");
}

void TestSelectedDecodeRetriesAfterObservedSpreadCancellation() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "promoted-selected-source.png";
	WriteText(source, "selected source cancellation retry");
	const jpegview_linux::SourceDescriptor descriptor =
		jpegview_linux::DescribeImageSource(source);
	std::mutex mutex;
	std::condition_variable changed;
	bool decoderStarted = false;
	bool cancellationObserved = false;
	bool releaseCancelledDecoder = false;
	std::atomic<int> decoderCalls{0};
	std::atomic<int> spreadCallbacks{0};
	std::atomic<int> selectedCallbacks{0};
	std::atomic<bool> selectedProducedImage{false};
	jpegview_linux::DecodedImageCache cache(64,
		[&](const fs::path&, DecodedImage& image, std::string&) {
			const int call = ++decoderCalls;
			if (call == 1) {
				std::unique_lock<std::mutex> lock(mutex);
				decoderStarted = true;
				changed.notify_all();
				jpegview_linux::WorkContext context =
					jpegview_linux::ResolveWorkContext(source,
						jpegview_linux::SourceWorkPriority::Foreground);
				while (context.Continue()) {
					changed.wait_for(lock, std::chrono::milliseconds(2));
				}
				cancellationObserved = true;
				changed.notify_all();
				changed.wait(lock, [&] { return releaseCancelledDecoder; });
				return false;
			}
			image = *CachedTestImage(4);
			return true;
		});
	cache.RequestBackground(descriptor,
		[&](const fs::path&, const jpegview_linux::DecodedImageCache::ImagePtr&) {
			++spreadCallbacks;
		}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	bool reachedDecoder = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedDecoder = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return decoderStarted; });
	}
	const bool canceled = reachedDecoder && cache.CancelActiveSpreadRequest(descriptor);
	bool observedCancellation = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		observedCancellation = changed.wait_for(lock, std::chrono::seconds(2),
			[&] { return cancellationObserved; });
	}
	if (observedCancellation) {
		cache.RequestSelectedSource(descriptor,
			[&](const jpegview_linux::SourceDescriptor&,
				const jpegview_linux::DecodedImageCache::ImagePtr& image,
				const jpegview_linux::WorkerFailure&) {
				++selectedCallbacks;
				selectedProducedImage.store(static_cast<bool>(image));
			}, jpegview_linux::PerfWorkClass::ActiveImageSpread);
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseCancelledDecoder = true;
	}
	changed.notify_all();
	const bool idle = cache.WaitUntilIdle(std::chrono::seconds(3));
	Expect(reachedDecoder && canceled && observedCancellation && idle &&
		decoderCalls.load() == 2 && spreadCallbacks.load() == 0 &&
		selectedCallbacks.load() == 1 && selectedProducedImage.load() &&
		cache.Find(descriptor) != nullptr,
		"a cancelled spread decoder's failure replaced the newly selected request instead of retrying it");
}

void TestThumbnailSourceNoticeRoutesThroughCurrentRefresh() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "current-thumbnail-source.png";
	const fs::path replacement = temporary.path() / "current-thumbnail-replacement.tmp";
	WriteBytes(source, {1, 2, 3, 4});
	FileList files({temporary.path().string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 1 && files.Current() == source,
		"thumbnail notice fixture did not select its source image");
	const jpegview_linux::SourceDescriptor original = *files.DescriptorAt(0);
	std::mutex mutex;
	std::condition_variable changed;
	bool started = false;
	bool release = false;
	const auto processor = [&](const jpegview_linux::ThumbnailPreparationRequest&) {
		{
			std::unique_lock<std::mutex> lock(mutex);
			started = true;
			changed.notify_all();
			changed.wait(lock, [&] { return release; });
		}
		return jpegview_linux::ThumbnailPreparationWorker::ImagePtr{};
	};
	jpegview_linux::ThumbnailPreparationWorker worker(processor);
	jpegview_linux::ThumbnailPreparationRequest request;
	request.key = original.Key();
	request.maximumWidth = 32;
	request.maximumHeight = 24;
	request.logicalSource = source;
	request.sourceDescriptor = original;
	Expect(worker.Request(request), "thumbnail notice worker rejected its current source");
	ScopedConditionRelease releaseProcessor(mutex, changed, release);
	bool reachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		reachedBarrier = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return started;
		});
	}
	WriteBytes(replacement, {9, 8, 7, 6});
	std::ifstream keepOriginalInode(source, std::ios::binary);
	Expect(static_cast<bool>(keepOriginalInode),
		"could not keep the old current source inode during thumbnail replacement");
	std::error_code renameError;
	fs::rename(replacement, source, renameError);
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	changed.notify_all();
	Expect(reachedBarrier && !renameError && worker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker did not observe a staged current-source replacement");
	const auto results = worker.TakeCompleted(1);
	Expect(results.size() == 1 && results.front().key == original.Key() &&
		results.front().observedSource.Valid() &&
		results.front().observedSource.Key() != original.Key() && !results.front().image,
		"thumbnail worker did not emit the exact stale-source notice for the replaced file");
	const jpegview_linux::SourceRefreshOutcome outcome = jpegview_linux::RefreshFileListSource(
		files, results.front().key, results.front().observedSource);
	Expect(outcome.applied && outcome.selectedSourceChanged && files.Current() == source &&
		files.DescriptorAt(files.CurrentIndex())->Key() ==
			results.front().observedSource.Key(),
		"thumbnail current-source notice was not routed into the current-image reload path");
}

void TestThumbnailInvalidDescriptorRecapturesRecreatedSource() {
	TemporaryDirectory temporary;
	const fs::path source = temporary.path() / "recreated-thumbnail.png";
	WriteTinyImage(source);
	FileList files({temporary.path().string()}, FileList::SortMode::FileName, true, false);
	Expect(files.Size() == 1 && files.Current() == source,
		"invalid-thumbnail-descriptor fixture did not select its image");
	const jpegview_linux::SourceDescriptor original = *files.DescriptorAt(0);
	std::error_code removeError;
	fs::remove(source, removeError);
	Expect(!removeError, "could not remove the thumbnail source before descriptor invalidation");
	const jpegview_linux::SourceDescriptor missing =
		jpegview_linux::DescribeImageSource(source);
	const jpegview_linux::SourceRefreshOutcome missingRefresh =
		jpegview_linux::RefreshFileListSource(files, original.Key(), missing);
	Expect(!missing.Valid() && !missing.LogicalPath().empty() &&
		missingRefresh.applied && missingRefresh.selectedSourceChanged &&
		files.DescriptorAt(0)->Key() == missing.Key(),
		"file list did not retain the exact invalid descriptor after deletion");

	std::atomic<int> processorCalls{0};
	const auto processor = [&processorCalls](
		const jpegview_linux::ThumbnailPreparationRequest& request) {
			++processorCalls;
			auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			image->key = request.key;
			image->width = 1;
			image->height = 1;
			image->bgra = {1, 2, 3, 255};
			return image;
		};
	jpegview_linux::ThumbnailPreparationWorker worker(processor);
	jpegview_linux::ThumbnailPreparationRequest request;
	request.key = missing.Key();
	request.maximumWidth = 16;
	request.maximumHeight = 16;
	request.logicalSource = source;
	request.sourceDescriptor = missing;
	Expect(worker.Request(request),
		"thumbnail worker rejected a nonempty invalid source descriptor");
	Expect(worker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker did not finish the still-missing source check");
	const auto stillMissingResults = worker.TakeCompleted(1);
	Expect(stillMissingResults.size() == 1 &&
		stillMissingResults.front().key == missing.Key() &&
		stillMissingResults.front().observedSource.LogicalPath().empty() &&
		!stillMissingResults.front().image && processorCalls == 0,
		"still-missing source published pixels under its invalid thumbnail key");
	WriteTinyImage(source);
	const jpegview_linux::SourceDescriptor recreated =
		jpegview_linux::DescribeImageSource(source);
	Expect(recreated.Valid() && recreated.Key() != missing.Key(),
		"thumbnail source was not recreated with a fresh valid identity");
	Expect(worker.Request(request) && worker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker did not finish invalid-descriptor recapture after recreation");
	const auto results = worker.TakeCompleted(1);
	Expect(results.size() == 1 && results.front().key == missing.Key() &&
		results.front().observedSource.Valid() &&
		results.front().observedSource.Key() == recreated.Key() &&
		results.front().observedSource.LogicalPath() == source &&
		!results.front().image && processorCalls == 0,
		"recreated source was decoded or published under the old invalid thumbnail key");
	const jpegview_linux::SourceRefreshOutcome recreatedRefresh =
		jpegview_linux::RefreshFileListSource(files, results.front().key,
			results.front().observedSource);
	Expect(recreatedRefresh.applied && recreatedRefresh.selectedSourceChanged &&
		files.DescriptorAt(files.CurrentIndex())->Key() == recreated.Key() &&
		!files.RefreshSourceDescriptor(missing.Key(), results.front().observedSource),
		"exact-key refresh did not move the active list from invalid to recreated source identity");

	std::atomic<int> syntheticProcessorCalls{0};
	jpegview_linux::ThumbnailPreparationWorker syntheticWorker(
		[&syntheticProcessorCalls](const jpegview_linux::ThumbnailPreparationRequest& synthetic) {
			++syntheticProcessorCalls;
			auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			image->key = synthetic.key;
			image->width = 1;
			image->height = 1;
			image->bgra = {4, 3, 2, 255};
			return image;
		});
	jpegview_linux::ThumbnailPreparationRequest syntheticRequest;
	syntheticRequest.key = jpegview_linux::SourceKey("synthetic-thumbnail");
	syntheticRequest.maximumWidth = 1;
	syntheticRequest.maximumHeight = 1;
	syntheticRequest.logicalSource = "/virtual/synthetic-thumbnail";
	Expect(syntheticWorker.Request(syntheticRequest) &&
		syntheticWorker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker changed behavior for a truly empty synthetic descriptor");
	const auto syntheticResults = syntheticWorker.TakeCompleted(1);
	Expect(syntheticResults.size() == 1 && syntheticResults.front().image &&
		syntheticResults.front().observedSource.LogicalPath().empty() &&
		syntheticProcessorCalls == 1,
		"empty synthetic descriptor was incorrectly recaptured or rejected");
}

void TestThumbnailBackgroundPreparation() {
	Expect(jpegview_linux::CanReuseDisplayPixelsForThumbnail(1920, 1080, 4u * 1024u * 1024u) &&
		!jpegview_linux::CanReuseDisplayPixelsForThumbnail(8000, 6000, 4u * 1024u * 1024u) &&
		!jpegview_linux::CanReuseDisplayPixelsForThumbnail(0, 1080, 4u * 1024u * 1024u),
		"thumbnail display-source bound accepted an invalid or oversized frame");
	auto source = std::make_shared<jpegview_linux::PreparedDisplayImage>();
	source->width = 4;
	source->height = 4;
	source->bgra = MakeIndexedImage(4, 4).bgra;
	source->bgra[3] = 0;
	source->hasTransparency = true;
	jpegview_linux::ThumbnailPreparationWorker realWorker;
	Expect(realWorker.Request({jpegview_linux::SourceKey("scaled"), source, 2, 2, 0,
		jpegview_linux::PerfWorkClass::DistantSpeculation, {}, 0, 0, {}, 0, {}}),
		"thumbnail worker rejected valid display-ready pixels");
	Expect(realWorker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker did not finish source-area downsampling");
	const auto scaled = realWorker.TakeCompleted(1);
	const std::string scaledResultKey = scaled.empty() ? "<empty>" :
		scaled[0].key.logicalPath;
	const bool hasScaledPixels = !scaled.empty() && static_cast<bool>(scaled[0].image);
	Expect(scaled.size() == 1 && scaled[0].image && scaled[0].key == "scaled",
		"thumbnail worker did not return its display-source request (count=" +
		std::to_string(scaled.size()) + ", key=" + scaledResultKey + ", pixels=" +
		(hasScaledPixels ? "yes" : "no") + ")");
	Expect(scaled.size() == 1 && scaled[0].image &&
		scaled[0].image->width == 2 && scaled[0].image->height == 2 &&
		scaled[0].image->bgra.size() == 16,
		"thumbnail worker returned incorrect derived pixel dimensions");
	Expect(scaled.size() == 1 && scaled[0].image && scaled[0].image->hasTransparency &&
		scaled[0].workClass ==
		jpegview_linux::PerfWorkClass::DistantSpeculation,
		"thumbnail worker lost transparency or work-class metadata");
	Expect(realWorker.Request({jpegview_linux::SourceKey("scaled-visible"), source, 2, 2, 0,
		jpegview_linux::PerfWorkClass::VisibleThumbnail, {}, 0, 0, {}, 0, {}}) &&
		realWorker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker did not process the visible-row attribution fixture");
	const auto visibleScaled = realWorker.TakeCompleted(1);
	Expect(visibleScaled.size() == 1 && visibleScaled[0].image &&
		visibleScaled[0].workClass ==
		jpegview_linux::PerfWorkClass::VisibleThumbnail,
		"thumbnail worker did not retain visible-row attribution through resampling");

	std::mutex orderMutex;
	std::condition_variable orderChanged;
	bool blockerStarted = false;
	bool releaseBlocker = false;
	std::vector<std::string> order;
	jpegview_linux::ThumbnailPreparationWorker prioritized(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			{
				std::unique_lock<std::mutex> lock(orderMutex);
				order.push_back(request.key.logicalPath);
				if (request.key == "blocker") {
					blockerStarted = true;
					orderChanged.notify_all();
					orderChanged.wait(lock, [&] { return releaseBlocker; });
				}
			}
			auto result = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			result->key = request.key;
			result->width = result->height = 1;
			result->bgra.assign(4, 255);
			return result;
		});
		Expect(prioritized.Request({jpegview_linux::SourceKey("blocker"), source, 2, 2, 9,
			jpegview_linux::PerfWorkClass::VisibleThumbnail, {}, 0, 0, {}, 0, {}}),
		"thumbnail worker rejected its blocking request");
	bool blockerStartedInTime = false;
	{
		std::unique_lock<std::mutex> lock(orderMutex);
		blockerStartedInTime = orderChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return blockerStarted; });
	}
	const bool queuedNeighbors = blockerStartedInTime &&
		prioritized.Request({jpegview_linux::SourceKey("far"), source, 2, 2, 5,
			jpegview_linux::PerfWorkClass::VisibleThumbnail, {}, 0, 0, {}, 0, {}}) &&
		prioritized.Request({jpegview_linux::SourceKey("near"), source, 2, 2, 1,
			jpegview_linux::PerfWorkClass::VisibleThumbnail, {}, 0, 0, {}, 0, {}});
	{
		std::lock_guard<std::mutex> lock(orderMutex);
		releaseBlocker = true;
	}
	orderChanged.notify_all();
	Expect(blockerStartedInTime, "thumbnail worker did not start in the background");
	Expect(queuedNeighbors, "thumbnail worker rejected queued neighbors");
	const auto prioritizedDeadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < prioritizedDeadline &&
		prioritized.GetDiagnostics().completedResults < 2) {
		std::this_thread::yield();
	}
	const auto firstPrioritized = prioritized.TakeCompleted(1);
	Expect(firstPrioritized.size() == 1 &&
		prioritized.WaitUntilIdle(std::chrono::seconds(2)),
		"consuming thumbnail completions did not release the next prioritized request");
	{
		std::lock_guard<std::mutex> lock(orderMutex);
		Expect(order == std::vector<std::string>({"blocker", "near", "far"}),
			"thumbnail worker did not prefer the closest queued neighbor");
	}
	const auto remainingPrioritized = prioritized.TakeCompleted(2);
	Expect(remainingPrioritized.size() == 2,
		"thumbnail worker did not publish every prepared neighbor");
	for (const auto& result : firstPrioritized) prioritized.Retire(result.image);
	for (const auto& result : remainingPrioritized) prioritized.Retire(result.image);

	auto activeSource = std::make_shared<jpegview_linux::PreparedDisplayImage>();
	activeSource->width = 4;
	activeSource->height = 4;
	activeSource->bgra.assign(4u * 4u * 4u, 12);
	auto queuedSource = std::make_shared<jpegview_linux::PreparedDisplayImage>();
	queuedSource->width = 3;
	queuedSource->height = 2;
	queuedSource->bgra.assign(3u * 2u * 4u, 24);
	std::mutex retainedMutex;
	std::condition_variable retainedChanged;
	bool retainedWorkerStarted = false;
	bool releaseRetainedWorker = false;
	jpegview_linux::PerfContext retainedContext;
	jpegview_linux::ThumbnailPreparationWorker retainedWorker(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			retainedContext = jpegview_linux::CurrentPerfContext();
			if (request.key == "active-source") {
				std::unique_lock<std::mutex> lock(retainedMutex);
				retainedWorkerStarted = true;
				retainedChanged.notify_all();
				retainedChanged.wait(lock, [&] { return releaseRetainedWorker; });
			}
			auto result = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			result->key = request.key;
			result->width = result->height = 1;
			result->bgra.assign(4, 255);
			return result;
		});
	Expect(retainedWorker.Request({jpegview_linux::SourceKey("active-source"), activeSource, 2, 2, 0,
		jpegview_linux::PerfWorkClass::VisibleThumbnail, {}, 0, 0, {}, 0, {}}) &&
		retainedWorker.Request({jpegview_linux::SourceKey("queued-source"), queuedSource, 2, 2, 1,
			jpegview_linux::PerfWorkClass::VisibleThumbnail, {}, 0, 0, {}, 0, {}}),
		"thumbnail worker rejected retained-source accounting fixtures");
	bool retainedWorkerReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(retainedMutex);
		retainedWorkerReachedBarrier = retainedChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return retainedWorkerStarted; });
	}
	const jpegview_linux::ThumbnailPreparationDiagnostics retainedStats =
		retainedWorker.GetDiagnostics();
	{
		std::lock_guard<std::mutex> lock(retainedMutex);
		releaseRetainedWorker = true;
	}
	retainedChanged.notify_all();
	Expect(retainedWorkerReachedBarrier &&
		retainedWorker.WaitUntilIdle(std::chrono::seconds(2)),
		"active thumbnail source did not leave its bounded retained-byte barrier");
	Expect(retainedStats.active == 1 && retainedStats.queued == 1 &&
		retainedStats.retainedSourceBytes == 4u * 4u * 4u + 3u * 2u * 4u,
		"thumbnail diagnostics omitted a source retained by active or queued work");
	Expect(retainedContext.workClass == jpegview_linux::PerfWorkClass::VisibleThumbnail &&
		retainedContext.execution == jpegview_linux::PerfExecution::WorkerThread,
		"visible-thumbnail work did not retain worker-thread attribution while processing");

	std::mutex staleMutex;
	std::condition_variable staleChanged;
	bool staleStarted = false;
	bool releaseStale = false;
	jpegview_linux::ThumbnailPreparationWorker cancellable(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			std::unique_lock<std::mutex> lock(staleMutex);
			staleStarted = true;
			staleChanged.notify_all();
			staleChanged.wait(lock, [&] { return releaseStale; });
			auto result = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			result->key = request.key;
			return result;
		});
	Expect(cancellable.Request({jpegview_linux::SourceKey("stale"), source, 2, 2, 0,
		jpegview_linux::PerfWorkClass::VisibleThumbnail, {}, 1, 4, {}, 0, {}}),
		"thumbnail worker rejected cancellation fixture");
	bool staleStartedInTime = false;
	{
		std::unique_lock<std::mutex> lock(staleMutex);
		staleStartedInTime = staleChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return staleStarted; });
	}
	if (staleStartedInTime) cancellable.Clear();
	{
		std::lock_guard<std::mutex> lock(staleMutex);
		releaseStale = true;
	}
	staleChanged.notify_all();
	Expect(staleStartedInTime, "stale thumbnail work did not start");
	Expect(cancellable.WaitUntilIdle(std::chrono::seconds(2)) &&
		cancellable.TakeCompleted(1).empty(),
		"cleared thumbnail work published a stale result");
	Expect(cancellable.Request({jpegview_linux::SourceKey("stale"), source, 2, 2, 0,
		jpegview_linux::PerfWorkClass::VisibleThumbnail, {}, 2, 5, {}, 0, {}}) &&
		cancellable.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker rejected replacement work after cancellation");
	const auto replaced = cancellable.TakeCompleted(1);
	Expect(replaced.size() == 1 && replaced.front().image &&
		jpegview_linux::ThumbnailPreparationResultMatches(
			replaced.front(), 2, 5, 0, "stale", 2, 2) &&
		!jpegview_linux::ThumbnailPreparationResultMatches(
			replaced.front(), 1, 5, 0, "stale", 2, 2) &&
		!jpegview_linux::ThumbnailPreparationResultMatches(
			replaced.front(), 2, 4, 0, "stale", 2, 2) &&
		!jpegview_linux::ThumbnailPreparationResultMatches(
			replaced.front(), 2, 5, 1, "stale", 2, 2) &&
		!jpegview_linux::ThumbnailPreparationResultMatches(
			replaced.front(), 2, 5, 0, "stale", 3, 2),
		"thumbnail worker result did not preserve index/catalog/geometry identity");

	std::mutex selectiveMutex;
	std::condition_variable selectiveChanged;
	bool visibleStarted = false;
	bool releaseVisible = false;
	const auto thumbnailRequest = [&](const std::string& key,
		jpegview_linux::PerfWorkClass workClass, std::size_t priority) {
		jpegview_linux::ThumbnailPreparationRequest request;
		request.key = key;
		request.source = source;
		request.maximumWidth = request.maximumHeight = 2;
		request.priority = priority;
		request.workClass = workClass;
		return request;
	};
	jpegview_linux::ThumbnailPreparationWorker selective(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			if (request.key == "visible-blocker") {
				std::unique_lock<std::mutex> lock(selectiveMutex);
				visibleStarted = true;
				selectiveChanged.notify_all();
				selectiveChanged.wait(lock, [&] { return releaseVisible; });
			}
			auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			image->key = request.key;
			image->width = image->height = 1;
			image->bgra.assign(4, 255);
			return image;
		});
	Expect(selective.Request(thumbnailRequest("visible-blocker",
		jpegview_linux::PerfWorkClass::VisibleThumbnail, 0)),
		"selective thumbnail worker rejected its visible blocker");
	bool visibleReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(selectiveMutex);
		visibleReachedBarrier = selectiveChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return visibleStarted; });
	}
	const bool selectiveQueued = visibleReachedBarrier &&
		selective.Request(thumbnailRequest("queued-distant",
			jpegview_linux::PerfWorkClass::DistantSpeculation, 2)) &&
		selective.Request(thumbnailRequest("queued-visible",
			jpegview_linux::PerfWorkClass::VisibleThumbnail, 1));
	const auto removedDistant = selective.Cancel({
		jpegview_linux::PerfWorkClass::DistantSpeculation});
	{
		std::lock_guard<std::mutex> lock(selectiveMutex);
		releaseVisible = true;
	}
	selectiveChanged.notify_all();
	Expect(selectiveQueued && removedDistant.size() == 1 &&
		removedDistant.front().key == "queued-distant" && removedDistant.front().cancelled,
		"interaction cancellation removed visible thumbnail work or retained queued distant work");
	Expect(selective.WaitUntilIdle(std::chrono::seconds(2)),
		"selectively retained visible thumbnail requests did not finish");
	const std::set<jpegview_linux::PerfWorkClass> visibleOnly{
		jpegview_linux::PerfWorkClass::VisibleThumbnail};
	const auto visibleResults = selective.TakeCompleted(3, visibleOnly);
	Expect(visibleResults.size() == 2 && std::all_of(visibleResults.begin(),
		visibleResults.end(), [](const auto& result) {
			return !result.cancelled && result.image;
		}),
		"visible thumbnail completions did not pass the interaction upload gate");

	std::mutex activeCancelMutex;
	std::condition_variable activeCancelChanged;
	bool distantStarted = false;
	bool releaseDistant = false;
	std::shared_ptr<std::atomic<bool>> activeCancellation;
	jpegview_linux::ThumbnailPreparationWorker activeDistant(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			std::unique_lock<std::mutex> lock(activeCancelMutex);
			activeCancellation = request.cancellation;
			distantStarted = true;
			activeCancelChanged.notify_all();
			activeCancelChanged.wait(lock, [&] { return releaseDistant; });
			auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			image->key = request.key;
			return image;
		});
	ScopedConditionRelease activeDistantRelease(
		activeCancelMutex, activeCancelChanged, releaseDistant);
	Expect(activeDistant.Request(thumbnailRequest("active-distant",
		jpegview_linux::PerfWorkClass::DistantSpeculation, 0)),
		"active distant thumbnail cancellation fixture was rejected");
	{
		std::unique_lock<std::mutex> lock(activeCancelMutex);
		Expect(activeCancelChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return distantStarted; }),
			"active distant thumbnail did not reach its cancellation barrier");
	}
	const auto activeCancellationResult = activeDistant.Cancel({
		jpegview_linux::PerfWorkClass::DistantSpeculation});
	const bool activeCancelRequested = activeCancellation && activeCancellation->load();
	{
		std::lock_guard<std::mutex> lock(activeCancelMutex);
		releaseDistant = true;
	}
	activeCancelChanged.notify_all();
	Expect(activeCancellationResult.size() == 1 &&
		activeCancellationResult.front().key == "active-distant" &&
		activeCancellationResult.front().cancelled && activeCancelRequested &&
		activeDistant.WaitUntilIdle(std::chrono::seconds(2)),
		"active distant thumbnail did not receive cooperative cancellation");
	const auto canceledActiveResult = activeDistant.TakeCompleted(1, {
		jpegview_linux::PerfWorkClass::DistantSpeculation});
	Expect(canceledActiveResult.empty(),
		"cooperatively canceled thumbnail published a duplicate retry result");
}

void TestThumbnailCompletionQueueCountBackpressure() {
	auto makeRequest = [](std::size_t index) {
		jpegview_linux::ThumbnailPreparationRequest request;
		request.key = jpegview_linux::SourceKey("thumbnail-count-" +
			std::to_string(index));
		request.maximumWidth = request.maximumHeight = 512;
		request.priority = index;
		request.workClass = jpegview_linux::PerfWorkClass::DistantSpeculation;
		request.logicalSource = "/virtual/thumbnail-count-" + std::to_string(index);
		request.fileIndex = index;
		return request;
	};
	std::atomic<std::size_t> processorCalls{0};
	std::mutex startMutex;
	std::condition_variable startChanged;
	bool firstStarted = false;
	bool releaseFirst = false;
	jpegview_linux::ThumbnailPreparationWorker worker(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			++processorCalls;
			if (request.key.logicalPath == "thumbnail-count-1") {
				std::unique_lock<std::mutex> lock(startMutex);
				firstStarted = true;
				startChanged.notify_all();
				startChanged.wait(lock, [&] { return releaseFirst; });
			}
			auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			image->key = request.key;
			image->width = request.maximumWidth;
			image->height = request.maximumHeight;
			image->workClass = request.workClass;
			image->bgra.assign(static_cast<std::size_t>(image->width) *
				static_cast<std::size_t>(image->height) * 4, 255);
			return image;
		});
	const bool firstAccepted = worker.Request(makeRequest(1));
	bool reachedBarrier = false;
	if (firstAccepted) {
		std::unique_lock<std::mutex> lock(startMutex);
		reachedBarrier = startChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return firstStarted; });
	}
	const bool restAccepted = reachedBarrier && worker.Request(makeRequest(2)) &&
		worker.Request(makeRequest(3));
	{
		std::lock_guard<std::mutex> lock(startMutex);
		releaseFirst = true;
	}
	startChanged.notify_all();
	Expect(firstAccepted && reachedBarrier && restAccepted,
		"thumbnail worker rejected requests needed to characterize completion backpressure");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < deadline &&
		worker.GetDiagnostics().completedImages < 2) {
		std::this_thread::yield();
	}
	const auto full = worker.GetDiagnostics();
	Expect(processorCalls.load() == 2 && full.completedResults == 2 &&
		full.completedImages == 2 && full.completedBytes == 2u * 1024u * 1024u &&
		full.reservedCompletionImages == 2 &&
		full.reservedCompletionBytes == 2u * 1024u * 1024u && full.queued == 1 &&
		!worker.WaitUntilIdle(std::chrono::milliseconds(100)),
		"thumbnail workers outran the two-result completion allowance");
	auto first = worker.TakeCompleted(1);
	Expect(first.size() == 1 && first.front().image &&
		worker.WaitUntilIdle(std::chrono::seconds(2)) && processorCalls.load() == 3,
		"consuming a thumbnail result did not release a completion reservation");
	worker.Retire(first.front().image);
	auto remaining = worker.TakeCompleted(2);
	Expect(remaining.size() == 2 &&
		worker.GetDiagnostics().reservedCompletionImages == 0,
		"thumbnail completion reservations remained held after consumption");
	for (const auto& result : remaining) worker.Retire(result.image);
}

void TestThumbnailCompletionByteBackpressureAndCancellation() {
	constexpr std::size_t mebibyte = 1024u * 1024u;
	auto makeRequest = [](const std::string& identity, int width, int height,
		std::size_t priority) {
		jpegview_linux::ThumbnailPreparationRequest request;
		request.key = jpegview_linux::SourceKey(identity);
		request.maximumWidth = width;
		request.maximumHeight = height;
		request.priority = priority;
		request.workClass = jpegview_linux::PerfWorkClass::DistantSpeculation;
		request.logicalSource = "/virtual/" + identity;
		return request;
	};
	std::atomic<std::size_t> processorCalls{0};
	std::mutex startMutex;
	std::condition_variable startChanged;
	bool firstStarted = false;
	bool releaseFirst = false;
	jpegview_linux::ThumbnailPreparationWorker worker(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			++processorCalls;
			if (request.key.logicalPath == "thumbnail-12m") {
				std::unique_lock<std::mutex> lock(startMutex);
				firstStarted = true;
				startChanged.notify_all();
				startChanged.wait(lock, [&] { return releaseFirst; });
			}
			auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			image->key = request.key;
			image->width = request.maximumWidth;
			image->height = request.maximumHeight;
			image->workClass = request.workClass;
			image->bgra.assign(static_cast<std::size_t>(image->width) *
				static_cast<std::size_t>(image->height) * 4, 255);
			return image;
		});
	const auto twelveMiB = makeRequest("thumbnail-12m", 2048, 1536, 1);
	const auto eightMiB = makeRequest("thumbnail-8m", 2048, 1024, 2);
	const auto tiny = makeRequest("thumbnail-tiny", 1, 1, 3);
	const bool firstAccepted = worker.Request(twelveMiB);
	bool reachedBarrier = false;
	if (firstAccepted) {
		std::unique_lock<std::mutex> lock(startMutex);
		reachedBarrier = startChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return firstStarted; });
	}
	const bool restAccepted = reachedBarrier && worker.Request(eightMiB) && worker.Request(tiny);
	{
		std::lock_guard<std::mutex> lock(startMutex);
		releaseFirst = true;
	}
	startChanged.notify_all();
	Expect(firstAccepted && reachedBarrier && restAccepted,
		"thumbnail byte-budget fixture failed to queue valid work");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < deadline &&
		worker.GetDiagnostics().completedImages < 2) {
		std::this_thread::yield();
	}
	const auto full = worker.GetDiagnostics();
	Expect(processorCalls.load() == 2 && full.completedResults == 2 &&
		full.completedImages == 2 && full.completedBytes == 12u * mebibyte + 4 &&
		full.reservedCompletionImages == 2 &&
		full.reservedCompletionBytes == 12u * mebibyte + 4 && full.queued == 1 &&
		full.reservedCompletionBytes <=
			jpegview_linux::ThumbnailPreparationWorker::kMaximumCompletedBytes,
		"thumbnail completion pixels exceeded 16 MiB or failed to skip a blocked larger result");
	const std::set<jpegview_linux::PerfWorkClass> distant{
		jpegview_linux::PerfWorkClass::DistantSpeculation};
	const auto canceled = worker.Cancel(distant);
	Expect(canceled.size() == 3 && worker.WaitUntilIdle(std::chrono::seconds(2)) &&
		processorCalls.load() == 2 && worker.GetDiagnostics().completedResults == 0 &&
		worker.GetDiagnostics().reservedCompletionImages == 0 &&
		worker.GetDiagnostics().reservedCompletionBytes == 0,
		"thumbnail cancellation while backpressured admitted queued work or retained reservations");
}

void TestThumbnailOversizedAndAllocationFailure() {
	auto makeRequest = [](const std::string& identity, int width, int height,
		std::size_t priority) {
		jpegview_linux::ThumbnailPreparationRequest request;
		request.key = jpegview_linux::SourceKey(identity);
		request.maximumWidth = width;
		request.maximumHeight = height;
		request.priority = priority;
		request.workClass = jpegview_linux::PerfWorkClass::DistantSpeculation;
		request.logicalSource = "/virtual/" + identity;
		return request;
	};
	std::atomic<std::size_t> oversizedCalls{0};
	jpegview_linux::ThumbnailPreparationWorker oversized(
		[&oversizedCalls](const jpegview_linux::ThumbnailPreparationRequest&) {
			++oversizedCalls;
			return jpegview_linux::ThumbnailPreparationWorker::ImagePtr{};
		});
	const auto tooLarge = makeRequest("thumbnail-over-allowance", 2048, 2049, 1);
	Expect(!oversized.Request(tooLarge) &&
		oversized.WaitUntilIdle(std::chrono::milliseconds(100)) &&
		oversizedCalls.load() == 0 && oversized.GetDiagnostics().queued == 0 &&
		oversized.GetDiagnostics().reservedCompletionImages == 0,
		"oversized thumbnail work was admitted but could never reserve completion bytes");

	constexpr std::size_t mebibyte = 1024u * 1024u;
	std::atomic<std::size_t> allocationCalls{0};
	jpegview_linux::ThumbnailPreparationWorker allocation(
		[&allocationCalls](const jpegview_linux::ThumbnailPreparationRequest& request) {
			if (++allocationCalls == 1) throw std::bad_alloc();
			auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			image->key = request.key;
			image->width = request.maximumWidth;
			image->height = request.maximumHeight;
			image->bgra.assign(static_cast<std::size_t>(image->width) *
				static_cast<std::size_t>(image->height) * 4, 255);
			return image;
		});
	const auto failed = makeRequest("thumbnail-allocation-failure", 2048, 1536, 1);
	const auto succeeds = makeRequest("thumbnail-allocation-recovery", 2048, 1024, 2);
	Expect(allocation.Request(failed) && allocation.Request(succeeds) &&
		allocation.WaitUntilIdle(std::chrono::seconds(3)) && allocationCalls.load() == 2,
		"thumbnail allocation failure retained bytes or stopped later worker processing");
	const auto diagnostics = allocation.GetDiagnostics();
	Expect(diagnostics.completedResults == 2 && diagnostics.completedImages == 1 &&
		diagnostics.completedBytes == 8u * mebibyte &&
		diagnostics.reservedCompletionImages == 2 &&
		diagnostics.reservedCompletionBytes == 8u * mebibyte,
		"thumbnail failure did not release its byte reservation while retaining bounded result count");
	const auto results = allocation.TakeCompleted(2);
	Expect(results.size() == 2 && !results.front().image && results.back().image &&
		allocation.GetDiagnostics().reservedCompletionImages == 0,
		"thumbnail worker did not publish failure metadata and recoverable pixels in order");
	Expect(results.front().failure.kind == jpegview_linux::WorkerFailureKind::Exception &&
		!results.front().failure.message.empty(),
		"thumbnail processor exception was not exposed as a structured worker failure");
	for (const auto& result : results) allocation.Retire(result.image);
}

void TestThumbnailShutdownRetiresWithoutEventLoop() {
	struct DestructionProbe {
		std::mutex mutex;
		std::condition_variable changed;
		std::size_t count = 0;
		bool offCaller = true;
	};
	const std::thread::id caller = std::this_thread::get_id();
	auto probe = std::make_shared<DestructionProbe>();
	{
		jpegview_linux::ThumbnailPreparationWorker worker(
			[probe, caller](const jpegview_linux::ThumbnailPreparationRequest& request) {
				auto* image = new jpegview_linux::PreparedThumbnailImage();
				image->key = request.key;
				image->width = request.maximumWidth;
				image->height = request.maximumHeight;
				image->bgra.assign(static_cast<std::size_t>(image->width) *
					static_cast<std::size_t>(image->height) * 4, 255);
				return jpegview_linux::ThumbnailPreparationWorker::ImagePtr(image,
					[probe, caller](const jpegview_linux::PreparedThumbnailImage* retired) {
						{
							std::lock_guard<std::mutex> lock(probe->mutex);
							++probe->count;
							probe->offCaller = probe->offCaller &&
								std::this_thread::get_id() != caller;
						}
						probe->changed.notify_all();
						delete retired;
					});
			});
		for (std::size_t index = 0; index < 2; ++index) {
			jpegview_linux::ThumbnailPreparationRequest request;
			request.key = jpegview_linux::SourceKey("thumbnail-shutdown-" +
				std::to_string(index));
			request.maximumWidth = request.maximumHeight = 256;
			request.priority = index;
			request.logicalSource = "/virtual/thumbnail-shutdown-" +
				std::to_string(index);
			Expect(worker.Request(request),
				"thumbnail shutdown fixture failed to admit a result");
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (std::chrono::steady_clock::now() < deadline &&
			worker.GetDiagnostics().completedImages != 2) {
			std::this_thread::yield();
		}
		Expect(worker.GetDiagnostics().completedImages == 2,
			"thumbnail shutdown fixture did not fill its bounded completion queue");
		worker.Shutdown();
		worker.Shutdown();
		jpegview_linux::ThumbnailPreparationRequest afterShutdown;
		afterShutdown.key = jpegview_linux::SourceKey("thumbnail-after-shutdown");
		afterShutdown.maximumWidth = afterShutdown.maximumHeight = 16;
		afterShutdown.logicalSource = "/virtual/thumbnail-after-shutdown";
		Expect(!worker.Request(afterShutdown),
			"thumbnail worker accepted new preparation after terminal shutdown");
	}
	std::lock_guard<std::mutex> lock(probe->mutex);
	Expect(probe->count == 2 && probe->offCaller,
		"thumbnail worker destruction required event-loop consumption to retire completed pixels");
}

void TestThumbnailRetirementDoesNotDeduplicateReusedAddress() {
	using Image = jpegview_linux::PreparedThumbnailImage;
	using ImagePtr = jpegview_linux::ThumbnailPreparationWorker::ImagePtr;
	alignas(std::max_align_t) unsigned char storage[1024];
	static_assert(sizeof(Image) <= sizeof(storage), "thumbnail retirement fixture storage is too small");
	static_assert(alignof(Image) <= alignof(std::max_align_t),
		"thumbnail retirement fixture needs stronger alignment");
	std::mutex mutex;
	std::condition_variable changed;
	bool firstLifetimeEnded = false;
	bool releaseFirstDeleter = false;
	bool secondDestroyed = false;
	std::thread::id secondDestroyedOn;
	const std::thread::id caller = std::this_thread::get_id();
	jpegview_linux::ThumbnailPreparationWorker worker(
		[](const jpegview_linux::ThumbnailPreparationRequest&) { return ImagePtr{}; });
	ScopedConditionRelease releaseFirstOnExit(mutex, changed, releaseFirstDeleter);

	Image* firstRaw = new (storage) Image();
	const void* reusedAddress = firstRaw;
	ImagePtr first(firstRaw, [&](const Image* retired) {
		const_cast<Image*>(retired)->~Image();
		std::unique_lock<std::mutex> lock(mutex);
		firstLifetimeEnded = true;
		changed.notify_all();
		changed.wait_for(lock, std::chrono::seconds(3), [&] {
			return releaseFirstDeleter;
		});
	});
	worker.Retire(first);
	first.reset();
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return firstLifetimeEnded;
		}), "thumbnail retirement did not reach the address-reuse barrier");
	}

	Image* secondRaw = new (storage) Image();
	Expect(secondRaw == reusedAddress,
		"thumbnail retirement fixture did not reuse the first image's address");
	ImagePtr second(secondRaw, [&](const Image* retired) {
		const_cast<Image*>(retired)->~Image();
		{
			std::lock_guard<std::mutex> lock(mutex);
			secondDestroyedOn = std::this_thread::get_id();
			secondDestroyed = true;
		}
		changed.notify_all();
	});
	worker.Retire(second);
	second.reset();
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseFirstDeleter = true;
	}
	changed.notify_all();
	bool secondFinished = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		secondFinished = changed.wait_for(lock, std::chrono::seconds(2), [&] {
			return secondDestroyed;
		});
	}
	Expect(secondFinished && secondDestroyedOn != caller,
		"a new thumbnail owner at a reused address was discarded or destroyed on the caller");
}

void TestThumbnailQueueDisplacementReturnsSchedulerWork() {
	std::mutex retirementMutex;
	std::condition_variable retirementChanged;
	bool retirementStarted = false;
	bool releaseRetirement = false;
	jpegview_linux::ThumbnailPreparationWorker worker(
		[](const jpegview_linux::ThumbnailPreparationRequest& request) {
			auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			image->key = request.key;
			image->width = image->height = 1;
			image->bgra.assign(4, 255);
			return image;
		});
	auto retiringSource = std::shared_ptr<jpegview_linux::PreparedDisplayImage>(
		new jpegview_linux::PreparedDisplayImage,
		[&](jpegview_linux::PreparedDisplayImage* image) {
			std::unique_lock<std::mutex> lock(retirementMutex);
			retirementStarted = true;
			retirementChanged.notify_all();
			retirementChanged.wait_for(lock, std::chrono::seconds(3),
				[&] { return releaseRetirement; });
			delete image;
		});
	retiringSource->width = retiringSource->height = 1;
	retiringSource->bgra.assign(4, 255);
	ScopedConditionRelease releaseRetirementOnExit(
		retirementMutex, retirementChanged, releaseRetirement);

	jpegview_linux::ThumbnailPreparationRequest warmup;
	warmup.key = jpegview_linux::SourceKey("retirement-warmup");
	warmup.source = retiringSource;
	warmup.maximumWidth = warmup.maximumHeight = 100;
	Expect(worker.Request(warmup).accepted,
		"retirement fixture worker rejected its warmup request");
	warmup.source.reset();
	retiringSource.reset();
	bool retirementReachedBarrier = false;
	{
		std::unique_lock<std::mutex> lock(retirementMutex);
		retirementReachedBarrier = retirementChanged.wait_for(lock,
			std::chrono::seconds(2), [&] { return retirementStarted; });
	}
	Expect(retirementReachedBarrier,
		"thumbnail source retirement did not reach its bounded barrier");
	const auto warmupResult = worker.TakeCompleted(1);
	Expect(warmupResult.size() == 1 && warmupResult.front().image &&
		warmupResult.front().key == "retirement-warmup",
		"thumbnail worker did not finish warmup before entering source retirement");

	const std::vector<std::string> keys = {"a", "b", "c", "d", "e", "f"};
	jpegview_linux::ThumbnailCacheScheduler scheduler;
	scheduler.SetCatalog(keys);
	scheduler.SetCurrent(0);
	scheduler.SetGeometry(100, 60);
	for (const char* key : {"a", "b", "c"}) scheduler.Store(key);
	const auto admittedFiles = scheduler.TakeNext(0, 1);
	Expect(admittedFiles.size() == 1 && admittedFiles.front().key == "d",
		"displacement fixture did not select d as its first file-backed request");
	const jpegview_linux::ThumbnailLoadRequest dLoad = admittedFiles.front();
	const auto makeFileRequest = [](const jpegview_linux::ThumbnailLoadRequest& load) {
		jpegview_linux::ThumbnailPreparationRequest request;
		request.key = load.key;
		request.logicalSource = load.key.logicalPath;
		request.maximumWidth = load.maximumWidth;
		request.maximumHeight = load.maximumHeight;
		request.priority = 6;
		request.workClass = jpegview_linux::PerfWorkClass::VisibleThumbnail;
		request.catalogRevision = load.catalogRevision;
		request.geometryRevision = load.geometryRevision;
		request.fileIndex = load.fileIndex;
		return request;
	};
	const jpegview_linux::ThumbnailPreparationAdmission queuedD =
		worker.Request(makeFileRequest(dLoad));
	Expect(queuedD.accepted && !queuedD.displaced &&
		worker.GetDiagnostics().queued == 1,
		"scheduler-admitted d did not remain in the bounded worker queue");
	scheduler.SetCurrent(5);
	auto reuseSource = std::make_shared<jpegview_linux::PreparedDisplayImage>();
	reuseSource->width = reuseSource->height = 1;
	reuseSource->bgra.assign(4, 255);
	const auto requestFromPreparedFrame = [&](const std::string& key,
		std::size_t fileIndex, std::size_t priority) {
		auto request = makeFileRequest(dLoad);
		request.key = key;
		request.logicalSource.clear();
		request.source = reuseSource;
		request.fileIndex = fileIndex;
		request.priority = priority;
		return request;
	};
	const jpegview_linux::ThumbnailPreparationAdmission queuedF =
		worker.Request(requestFromPreparedFrame("f", 5, 0));
	const jpegview_linux::ThumbnailPreparationAdmission queuedE =
		worker.Request(requestFromPreparedFrame("e", 4, 2));
	Expect(queuedF.accepted && !queuedF.displaced && queuedE.accepted &&
		queuedE.displaced && queuedE.displaced->cancelled &&
		queuedE.displaced->key == "d" && queuedE.displaced->fileIndex == dLoad.fileIndex &&
		queuedE.displaced->catalogRevision == dLoad.catalogRevision &&
		queuedE.displaced->geometryRevision == dLoad.geometryRevision &&
		worker.GetDiagnostics().queued == 2,
		"prepared-frame requests did not report the displaced queued d identity");
	scheduler.Retry({queuedE.displaced->fileIndex, queuedE.displaced->key,
		queuedE.displaced->catalogRevision, queuedE.displaced->geometryRevision,
		queuedE.displaced->maximumWidth, queuedE.displaced->maximumHeight});
	Expect(scheduler.PendingCount() == 3,
		"displaced d was not returned to the scheduler's pending work set");
	{
		std::lock_guard<std::mutex> lock(retirementMutex);
		releaseRetirement = true;
	}
	retirementChanged.notify_all();
	Expect(worker.WaitUntilIdle(std::chrono::seconds(2)),
		"prepared-frame requests did not finish after source retirement resumed");
	auto preparedResults = worker.TakeCompleted(10);
	Expect(preparedResults.size() == 2 && std::all_of(preparedResults.begin(),
		preparedResults.end(), [](const auto& result) {
			return !result.cancelled && result.image &&
				(result.key == "e" || result.key == "f");
		}),
		"prepared-frame displacement fixture did not complete e and f");
	for (const auto& result : preparedResults) scheduler.Store(result.key);
	scheduler.SetCurrent(dLoad.fileIndex);
	const auto returnedToD = scheduler.TakeNext(0, 1);
	Expect(returnedToD.size() == 1 && returnedToD.front().key == "d" &&
		scheduler.IsCurrent(returnedToD.front()),
		"returning to d did not make its displaced request retryable");
	const jpegview_linux::ThumbnailPreparationAdmission retriedD =
		worker.Request(makeFileRequest(returnedToD.front()));
	Expect(retriedD.accepted && !retriedD.displaced &&
		worker.WaitUntilIdle(std::chrono::seconds(2)),
		"scheduler could not resubmit displaced d to the thumbnail worker");
	const auto completedD = worker.TakeCompleted(1);
	Expect(completedD.size() == 1 && completedD.front().key == "d" &&
		completedD.front().image && !completedD.front().cancelled &&
		jpegview_linux::ThumbnailPreparationResultMatches(completedD.front(),
			scheduler.CatalogRevision(), scheduler.GeometryRevision(), dLoad.fileIndex,
			"d", dLoad.maximumWidth, dLoad.maximumHeight) &&
		scheduler.Store("d").empty() && scheduler.IsCached("d"),
		"retried d did not complete and enter the thumbnail cache");
}

void TestThumbnailFileBackedPreparationAndShutdown() {
	TemporaryDirectory temporary;
	const fs::path jpeg = temporary.path() / "file-backed.jpg";
	std::vector<std::uint8_t> pixels(4u * 4u * 4u, 255);
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * 4 + x) * 4;
			pixels[offset] = static_cast<std::uint8_t>(x * 40);
			pixels[offset + 1] = static_cast<std::uint8_t>(y * 40);
		}
	}
	ImageWriteOptions options;
	options.jpegQuality = 100;
	std::string error;
	Expect(jpegview_linux::WriteImage(jpeg, pixels.data(), 4, 4, options, error),
		"cannot create file-backed thumbnail JPEG fixture: " + error);
	const fs::path ppm = temporary.path() / "file-backed.ppm";
	WriteText(ppm, "P3\n4 2\n255\n255 0 0 0 255 0 0 0 255 255 255 0\n"
		"0 255 255 255 0 255 128 128 128 0 0 0\n");
	const fs::path invalid = temporary.path() / "invalid.png";
	WriteText(invalid, "not an image");

	const auto fileRequest = [](const fs::path& source, int maximumWidth,
		int maximumHeight, std::uint64_t geometryRevision,
		std::uint64_t catalogRevision, std::size_t fileIndex) {
		jpegview_linux::ThumbnailPreparationRequest request;
		request.key = source.string();
		request.maximumWidth = maximumWidth;
		request.maximumHeight = maximumHeight;
		request.workClass = jpegview_linux::PerfWorkClass::VisibleThumbnail;
		request.logicalSource = source;
		request.geometryRevision = geometryRevision;
		request.catalogRevision = catalogRevision;
		request.fileIndex = fileIndex;
		return request;
	};
	jpegview_linux::ThumbnailPreparationWorker worker;
	Expect(worker.Request(fileRequest(jpeg, 2, 2, 11, 7, 3)) &&
		worker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker did not decode a file-backed JPEG request");
	auto result = worker.TakeCompleted(1);
	Expect(result.size() == 1 && result.front().image &&
		result.front().image->width == 2 && result.front().image->height == 2 &&
		result.front().image->bgra.size() == 16 &&
		jpegview_linux::ThumbnailPreparationResultMatches(
			result.front(), 7, 11, 3, jpeg.string(), 2, 2),
		"file-backed JPEG thumbnail lost its reduced decode or request identity");
#if JPEGVIEW_HAVE_WEBP
	const fs::path mislabeledWebP = temporary.path() / "file-backed-webp.jpg";
	options.webpQuality = 82;
	error.clear();
	Expect(jpegview_linux::WriteImageWithFormat(mislabeledWebP, ".webp", pixels.data(), 4, 4,
		options, error), "cannot create JPEG-named WebP thumbnail fixture: " + error);
	Expect(jpegview_linux::ReadImageContentFormat(mislabeledWebP) ==
		jpegview_linux::ImageContentFormat::WebP,
		"JPEG-named thumbnail fixture did not contain WebP bytes");
	Expect(worker.Request(fileRequest(mislabeledWebP, 2, 2, 14, 7, 6)) &&
		worker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker did not decode WebP content with a JPEG extension");
	result = worker.TakeCompleted(1);
	Expect(result.size() == 1 && result.front().image &&
		result.front().image->width == 2 && result.front().image->height == 2 &&
		result.front().image->bgra.size() == 16,
		"JPEG-named WebP did not use generic decode before thumbnail resampling");
#endif
	Expect(worker.Request(fileRequest(ppm, 2, 1, 12, 7, 4)) &&
		worker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker did not decode a file-backed non-JPEG request");
	result = worker.TakeCompleted(1);
	Expect(result.size() == 1 && result.front().image &&
		result.front().image->width == 2 && result.front().image->height == 1 &&
		result.front().image->bgra.size() == 8,
		"file-backed non-JPEG thumbnail was not resampled to its target geometry");
	Expect(worker.Request(fileRequest(invalid, 2, 2, 13, 7, 5)) &&
		worker.WaitUntilIdle(std::chrono::seconds(2)),
		"thumbnail worker rejected an invalid-source request before reporting failure");
	result = worker.TakeCompleted(1);
	Expect(result.size() == 1 && !result.front().image &&
		jpegview_linux::ThumbnailPreparationResultMatches(
			result.front(), 7, 13, 5, invalid.string(), 2, 2),
		"invalid source was not returned as a distinct decode failure");

	std::mutex shutdownMutex;
	std::condition_variable shutdownChanged;
	bool activeReadStarted = false;
	bool releaseActiveRead = false;
	bool destroyStarted = false;
	bool destroyFinished = false;
	auto activeWorker = std::make_unique<jpegview_linux::ThumbnailPreparationWorker>(
		[&](const jpegview_linux::ThumbnailPreparationRequest& request) {
			std::unique_lock<std::mutex> lock(shutdownMutex);
			activeReadStarted = true;
			shutdownChanged.notify_all();
			shutdownChanged.wait(lock, [&] { return releaseActiveRead; });
			auto image = std::make_shared<jpegview_linux::PreparedThumbnailImage>();
			image->key = request.key;
			image->width = image->height = 1;
			image->bgra.assign(4, 255);
			return image;
		});
	Expect(activeWorker->Request({jpegview_linux::SourceKey("shutdown-active"), nullptr, 1, 1, 0,
		jpegview_linux::PerfWorkClass::VisibleThumbnail, "/virtual/shutdown-active", 1, 1, {}, 0, {}}),
		"thumbnail worker rejected the active-shutdown fixture");
	{
		std::unique_lock<std::mutex> lock(shutdownMutex);
		Expect(shutdownChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return activeReadStarted; }),
			"active thumbnail request did not reach its shutdown barrier");
	}
	std::thread destroyer([&] {
		{
			std::lock_guard<std::mutex> lock(shutdownMutex);
			destroyStarted = true;
		}
		shutdownChanged.notify_all();
		activeWorker.reset();
		{
			std::lock_guard<std::mutex> lock(shutdownMutex);
			destroyFinished = true;
		}
		shutdownChanged.notify_all();
	});
	bool destroyStartedInTime = false;
	bool destroyReturnedBeforeRelease = false;
	{
		std::unique_lock<std::mutex> lock(shutdownMutex);
		destroyStartedInTime = shutdownChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return destroyStarted; });
		destroyReturnedBeforeRelease = shutdownChanged.wait_for(lock,
			std::chrono::milliseconds(100), [&] { return destroyFinished; });
		releaseActiveRead = true;
	}
	shutdownChanged.notify_all();
	bool destroyFinishedInTime = false;
	{
		std::unique_lock<std::mutex> lock(shutdownMutex);
		destroyFinishedInTime = shutdownChanged.wait_for(lock, std::chrono::seconds(2),
			[&] { return destroyFinished; });
	}
	destroyer.join();
	Expect(destroyStartedInTime && !destroyReturnedBeforeRelease && destroyFinishedInTime,
		"thumbnail worker destruction did not join an active request without event-loop work");
}

void TestThumbnailDownsamplingAntialiasing() {
	std::vector<std::uint8_t> checkerboard(8u * 8u * 4u, 255);
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			const std::uint8_t value = (x + y) % 2 == 0 ? 0 : 255;
			const std::size_t offset = (static_cast<std::size_t>(y) * 8 + x) * 4;
			checkerboard[offset] = value;
			checkerboard[offset + 1] = value;
			checkerboard[offset + 2] = value;
		}
	}
	std::vector<std::uint8_t> filtered;
	Expect(jpegview_linux::DownsampleThumbnailBgra(checkerboard, 8, 8, 2, 2, filtered),
		"thumbnail antialiasing rejected valid downsampling dimensions");
	Expect(filtered.size() == 16, "thumbnail antialiasing returned an incorrectly sized image");
	for (std::size_t offset = 0; offset < filtered.size(); offset += 4) {
		Expect(filtered[offset] == 128 && filtered[offset + 1] == 128 &&
			filtered[offset + 2] == 128 && filtered[offset + 3] == 255,
			"high-frequency thumbnail detail was sampled instead of area-filtered");
	}

	const std::vector<std::uint8_t> transparentEdge = {
		0, 0, 255, 255,
		255, 0, 0, 0,
	};
	Expect(jpegview_linux::DownsampleThumbnailBgra(transparentEdge, 2, 1, 1, 1, filtered),
		"thumbnail antialiasing rejected a transparent edge");
	Expect(filtered == std::vector<std::uint8_t>({0, 0, 255, 128}),
		"thumbnail antialiasing introduced a color fringe at a transparent edge");

	Expect(jpegview_linux::DownsampleThumbnailBgra(transparentEdge, 2, 1, 2, 1, filtered) &&
		filtered == transparentEdge, "same-size thumbnails were modified");

	std::vector<std::uint8_t> opaquePixels(7u * 5u * 4u);
	for (int index = 0; index < 7 * 5; ++index) {
		const std::size_t offset = static_cast<std::size_t>(index) * 4;
		opaquePixels[offset] = static_cast<std::uint8_t>((index * 31 + 3) % 256);
		opaquePixels[offset + 1] = static_cast<std::uint8_t>((index * 17 + 19) % 256);
		opaquePixels[offset + 2] = static_cast<std::uint8_t>((index * 13 + 79) % 256);
		opaquePixels[offset + 3] = 255;
	}
	const std::vector<std::uint8_t> opaqueExpected = {
		133, 75, 110, 255, 79, 114, 140, 255, 136, 153, 169, 255,
		156, 97, 123, 255, 161, 92, 153, 255, 57, 131, 139, 255};
	Expect(jpegview_linux::DownsampleThumbnailBgra(opaquePixels, 7, 5, 3, 2, filtered) &&
		filtered == opaqueExpected,
		"scalar opaque thumbnail output changed for an irregular source geometry");
	std::vector<std::uint8_t> opaqueFastPath;
	Expect(jpegview_linux::DownsampleThumbnailBgra(opaquePixels, 7, 5, 3, 2,
		opaqueFastPath, {}, false) && opaqueFastPath == opaqueExpected,
		"opaque thumbnail path changed scalar output for an irregular source geometry");

	int cancellationChecks = 0;
	Expect(!jpegview_linux::DownsampleThumbnailBgra(checkerboard, 8, 8, 2, 2,
		filtered, [&cancellationChecks] { return ++cancellationChecks < 1; }) &&
		filtered.empty(),
		"thumbnail downsampling did not stop at a cooperative row boundary");
	Expect(!jpegview_linux::DownsampleThumbnailBgra(transparentEdge, 2, 1, 3, 1, filtered),
		"thumbnail-only downsampler accepted enlargement");
	Expect(!jpegview_linux::DownsampleThumbnailBgra({1, 2, 3, 4}, 2, 2, 1, 1, filtered),
		"thumbnail downsampler accepted a truncated source buffer");
	Expect(!jpegview_linux::DownsampleThumbnailBgra({}, 0, 0, 0, 0, filtered),
		"thumbnail downsampler accepted empty dimensions");
}

void TestGrayscaleSpectrumCalculationAndScaling() {
	const std::vector<std::uint8_t> pixels = {
		0, 0, 0, 255,
		255, 255, 255, 255,
		0, 0, 255, 255,
		0, 255, 0, 255,
		255, 0, 0, 255,
	};
	const jpegview_linux::GrayscaleSpectrum spectrum =
		jpegview_linux::BuildGrayscaleSpectrum(pixels, 5, 1);
	Expect(spectrum[0] == 1 && spectrum[31] == 1 && spectrum[63] == 1 &&
		spectrum[159] == 1 && spectrum[255] == 1,
		"weighted grayscale histogram did not match Windows JPEGView channel weights");
	Expect(jpegview_linux::BuildGrayscaleSpectrum({1, 2, 3}, 1, 1) ==
		jpegview_linux::GrayscaleSpectrum{} &&
		jpegview_linux::BuildGrayscaleSpectrum(pixels, 0, 1) ==
		jpegview_linux::GrayscaleSpectrum{},
		"grayscale histogram accepted invalid image storage or dimensions");

	std::vector<std::uint8_t> largeImage(1000 * 1000 * 4, 127);
	for (std::size_t offset = 3; offset < largeImage.size(); offset += 4) {
		largeImage[offset] = 255;
	}
	const jpegview_linux::GrayscaleSpectrum sampled =
		jpegview_linux::BuildGrayscaleSpectrum(largeImage, 1000, 1000);
	Expect(sampled[127] == 40000 &&
		std::accumulate(sampled.begin(), sampled.end(), std::uint64_t{0}) == 40000,
		"large-image histogram did not use Windows JPEGView's bounded grid sampling");

	jpegview_linux::GrayscaleSpectrum distribution{};
	distribution[80] = 16;
	distribution[200] = 4;
	const auto heights = jpegview_linux::GrayscaleSpectrumBarHeights(distribution, 50);
	Expect(heights[80] == 50 && heights[200] == 25 && heights[0] == 0 &&
		jpegview_linux::GrayscaleSpectrumBarHeights(distribution, 0) ==
		std::array<int, jpegview_linux::kSpectrumBinCount>{},
		"grayscale spectrum bars did not use square-root scaling or handle empty height");
	std::size_t cancellationChecks = 0;
	const auto canceled = jpegview_linux::TryBuildGrayscaleSpectrum(largeImage, 1000, 1000,
		[&cancellationChecks] { return ++cancellationChecks < 3; });
	Expect(!canceled && cancellationChecks == 3,
		"grayscale histogram did not stop at a sampled-row cancellation boundary");
	const auto uncanceled = jpegview_linux::TryBuildGrayscaleSpectrum(
		largeImage, 1000, 1000, [] { return true; });
	Expect(uncanceled.has_value() && *uncanceled == sampled,
		"cancellable full-source histogram changed the existing sampled output");
}

void TestImageSpectrumWorkerGenerationDeduplicationAndShutdown() {
	jpegview_linux::Image image;
	image.width = image.originalWidth = 8;
	image.height = image.originalHeight = 4;
	image.bgra.resize(static_cast<std::size_t>(image.width * image.height * 4));
	for (std::size_t offset = 0; offset < image.bgra.size(); offset += 4) {
		const std::uint8_t value = static_cast<std::uint8_t>((offset / 4) * 7);
		image.bgra[offset] = value;
		image.bgra[offset + 1] = static_cast<std::uint8_t>(value / 2);
		image.bgra[offset + 2] = static_cast<std::uint8_t>(255 - value);
		image.bgra[offset + 3] = 255;
	}
	const jpegview_linux::ImageSpectrumKey key{
		jpegview_linux::SourceKey("/spectrum/full-source.jpg",
			jpegview_linux::SourceIdentity{1, 2, 3, 4, 5, true}),
		9, 0, jpegview_linux::EffectiveImageProcessingParams(
			jpegview_linux::ImageProcessingParams{}, false), false, 0};
	jpegview_linux::ImageProcessingParams inactiveProcessing;
	inactiveProcessing.colorCorrection = 0.25;
	inactiveProcessing.contrastCorrection = 0.5;
	inactiveProcessing.deepShadows = 0.75;
	inactiveProcessing.unsharpRadius = 4.0;
	inactiveProcessing.unsharpThreshold = 10.0;
	jpegview_linux::ImageSpectrumKey inactiveState = key;
	inactiveState.processing = jpegview_linux::EffectiveImageProcessingParams(
		inactiveProcessing, false);
	jpegview_linux::ImageSpectrumKey activeState = key;
	activeState.frameIndex = 1;
	jpegview_linux::ImageSpectrumKey processingState = key;
	jpegview_linux::ImageProcessingParams changedProcessing;
	changedProcessing.contrast = 0.2;
	processingState.processing = jpegview_linux::EffectiveImageProcessingParams(
		changedProcessing, false);
	jpegview_linux::ImageSpectrumKey subEpsilonState = key;
	changedProcessing.contrast = 0.0000000005;
	subEpsilonState.processing = jpegview_linux::EffectiveImageProcessingParams(
		changedProcessing, false);
	Expect(inactiveState == key && activeState != key && processingState != key &&
		subEpsilonState != key,
		"histogram identity split on inactive controls or ignored frame and processing changes");
	const jpegview_linux::GrayscaleSpectrum expected =
		jpegview_linux::BuildGrayscaleSpectrum(image.bgra, image.width, image.height);
	const auto immutableImage = std::make_shared<const jpegview_linux::Image>(image);
	jpegview_linux::ImageSpectrumWorker worker;
	const std::uint64_t generation = worker.Request(immutableImage, key);
	Expect(generation != 0 && worker.Request(immutableImage, key) == generation,
		"identical histogram requests were not deduplicated by source and document state");
	Expect(worker.WaitUntilIdle(std::chrono::seconds(2)),
		"full-source histogram worker did not finish within its bounded deadline");
	const auto result = worker.TakeReady();
	Expect(result.has_value() && result->spectrum.has_value() &&
		*result->spectrum == expected &&
		jpegview_linux::IsCurrentImageSpectrumResult(*result, generation, key),
		"histogram worker did not preserve the existing processed-pixel calculation");
	Expect(worker.Request(immutableImage, key) == generation &&
		!worker.TakeReady().has_value(),
		"repeated viewport-independent requests restarted a completed histogram");
	worker.Stop();
	Expect(worker.Request(immutableImage, key) == 0,
		"stopped histogram worker accepted a request without scheduling it");
	jpegview_linux::UiCompletionWakeup().Consume();

	std::mutex mutex;
	std::condition_variable startedCondition;
	std::condition_variable releaseCondition;
	bool firstStarted = false;
	bool releaseFirst = false;
	std::atomic<int> computations{0};
	jpegview_linux::ImageSpectrumWorker replacingWorker(
		[&](const jpegview_linux::Image& source,
			const std::function<bool()>&) -> std::optional<jpegview_linux::GrayscaleSpectrum> {
			const int computation = ++computations;
			if (computation == 1) {
				std::unique_lock<std::mutex> lock(mutex);
				firstStarted = true;
				startedCondition.notify_all();
				if (!releaseCondition.wait_for(lock, std::chrono::seconds(2),
					[&releaseFirst] { return releaseFirst; })) return std::nullopt;
			}
			return jpegview_linux::BuildGrayscaleSpectrum(
				source.bgra, source.width, source.height);
		});
	const std::uint64_t obsoleteGeneration = replacingWorker.Request(immutableImage, key);
	{
		std::unique_lock<std::mutex> lock(mutex);
		Expect(startedCondition.wait_for(lock, std::chrono::seconds(2),
			[&firstStarted] { return firstStarted; }),
			"replacement test did not reach its controlled worker boundary");
	}
	const std::uint64_t duplicateGeneration = replacingWorker.Request(immutableImage, key);
	jpegview_linux::ImageSpectrumKey replacementKey = key;
	++replacementKey.documentRevision;
	const std::uint64_t currentGeneration =
		replacingWorker.Request(immutableImage, replacementKey);
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseFirst = true;
	}
	releaseCondition.notify_all();
	Expect(duplicateGeneration == obsoleteGeneration &&
		currentGeneration > obsoleteGeneration &&
		replacingWorker.WaitUntilIdle(std::chrono::seconds(2)),
		"new document revision did not replace the obsolete in-flight histogram");
	const auto currentResult = replacingWorker.TakeReady();
	Expect(computations.load() == 2 && currentResult.has_value() &&
		currentResult->generation == currentGeneration &&
		currentResult->key == replacementKey &&
		jpegview_linux::IsCurrentImageSpectrumResult(
			*currentResult, currentGeneration, replacementKey) &&
		!jpegview_linux::IsCurrentImageSpectrumResult(
			*currentResult, currentGeneration, activeState) &&
		!jpegview_linux::IsCurrentImageSpectrumResult(
			*currentResult, currentGeneration, processingState) &&
		!jpegview_linux::IsCurrentImageSpectrumResult(
			*currentResult, currentGeneration, subEpsilonState) &&
		!jpegview_linux::IsCurrentImageSpectrumResult(
			*currentResult, obsoleteGeneration, key),
		"obsolete histogram output escaped generation and source-state validation");
	replacingWorker.CancelAndWait();
	replacingWorker.Stop();
	jpegview_linux::UiCompletionWakeup().Consume();

	std::atomic<int> failureComputations{0};
	jpegview_linux::ImageSpectrumWorker failureWorker(
		[&failureComputations](const jpegview_linux::Image& source,
			const std::function<bool()>& keepGoing)
			-> std::optional<jpegview_linux::GrayscaleSpectrum> {
			if (++failureComputations == 1) {
				throw std::runtime_error("controlled histogram failure");
			}
		return jpegview_linux::TryBuildGrayscaleSpectrum(source.bgra,
			source.width, source.height, keepGoing);
		});
	const std::uint64_t failedGeneration = failureWorker.Request(immutableImage, key);
	Expect(failedGeneration != 0 &&
		failureWorker.WaitUntilIdle(std::chrono::seconds(2)),
		"throwing histogram processor did not finish within its bounded deadline");
	const auto failedResult = failureWorker.TakeReady();
	jpegview_linux::ImageSpectrumKey retryKey = key;
	++retryKey.documentRevision;
	const std::uint64_t retryGeneration = failureWorker.Request(immutableImage, retryKey);
	Expect(failedResult.has_value() && failedResult->failure.kind ==
		jpegview_linux::WorkerFailureKind::Exception && retryGeneration > failedGeneration &&
		failureWorker.WaitUntilIdle(std::chrono::seconds(2)),
		"histogram exception escaped the worker or prevented a later valid request");
	const auto retryResult = failureWorker.TakeReady();
	Expect(retryResult.has_value() && retryResult->spectrum.has_value() &&
		!retryResult->failure.Failed(),
		"histogram worker did not resume after publishing a structured failure");
	failureWorker.Stop();
	jpegview_linux::UiCompletionWakeup().Consume();
}

void TestImageInfoFormatting() {
	Expect(jpegview_linux::FormatImagePosition(0, 123) == "1/123",
		"single-page position formatting changed");
	Expect(jpegview_linux::FormatImagePosition(0, 123, 1) == "1-2/123",
		"double-page position did not include both visible image numbers");
	Expect(jpegview_linux::FormatImagePosition(8, 123, 7) == "8-9/123",
		"double-page position did not present the pair in file-list order");
	Expect(jpegview_linux::FormatImagePosition(0, 1, 0) == "1/1" &&
		jpegview_linux::FormatImagePosition(0, 1, 1) == "1/1",
		"single-image position formatting added an invalid spread partner");
	Expect(jpegview_linux::FormatImagePosition(0, 0).empty() &&
		jpegview_linux::FormatImagePosition(3, 3).empty(),
		"invalid image-list positions should not produce a visible count");
	Expect(jpegview_linux::FormatImageDimensionsAndSize(1920, 1080, "2.5 MB") ==
		"1920 X 1080, 2.5 MB",
		"image dimensions and file size were not compacted into one line");
	Expect(jpegview_linux::FormatImageDimensionsAndSize(640, 480, {}) == "640 X 480",
		"missing file size left punctuation in the dimensions line");
	Expect(jpegview_linux::FormatModificationDateLine("2026-09-19 12:34:56") ==
		"2026-09-19 12:34:56",
		"modification date popup text still includes a label");
	Expect(jpegview_linux::FormatFileSize(1536) == "1.5 KB",
		"file-size formatting changed while moving it into the information model");
	Expect(jpegview_linux::FormatFileSize(1023) == "1023 B" &&
		jpegview_linux::FormatFileSize(10 * 1024) == "10 KB" &&
		jpegview_linux::FormatFileSize(3ull * 1024 * 1024 * 1024) == "3.0 GB",
		"file-size formatting changed at a unit or precision boundary");

	using jpegview_linux::WindowTitleContext;
	WindowTitleContext context;
	context.position = "2-3/12";
	context.currentIndex = 1;
	context.imageCount = 12;
	context.filename = "photo-%s.jpeg";
	context.filenameStem = "photo-%s";
	context.extension = "jpeg";
	context.fullPath = "/photos/day/photo-%s.jpeg";
	context.directory = "/photos/day";
	context.width = 8001;
	context.height = 6000;
	context.fileSize = 10u * 1024u * 1024u;
	context.applicationVersion = "1.4.2+dev29";
	Expect(jpegview_linux::FormatWindowTitle(
		jpegview_linux::kDefaultWindowTitlePattern, context) ==
		"[2-3/12] photo-%s.jpeg (8001x6000, 10 MB) - JPEGView",
		"default window-title pattern did not preserve the current title layout");
	Expect(jpegview_linux::FormatWindowTitle("%q", context) ==
		jpegview_linux::FormatWindowTitle(jpegview_linux::kDefaultWindowTitlePattern, context),
		"invalid window-title patterns did not safely fall back to the default");
	Expect(jpegview_linux::FormatWindowTitle(
		"%f [%i/%n] %p %F %e %P %D %w %h %s %b %m %a %v %%", context) ==
		"photo-%s.jpeg [2/12] 2-3/12 photo-%s jpeg /photos/day/photo-%s.jpeg "
		"/photos/day 8001 6000 10 MB 10485760 8001x6000, 10 MB JPEGView "
		"1.4.2+dev29 %",
		"window-title token expansion changed or recursively parsed replacement text");
	context.fileSize.reset();
	Expect(jpegview_linux::FormatWindowTitle("%w x %h (%m)|%s|%b", context) ==
		"8001 x 6000 (8001x6000)||",
		"window-title dimensions left size punctuation when file size was unavailable");
	Expect(jpegview_linux::NormalizeWindowTitlePattern(" \t ") ==
		jpegview_linux::kDefaultWindowTitlePattern &&
		jpegview_linux::NormalizeWindowTitlePattern("  [%f]  ") == "[%f]",
		"window-title pattern normalization did not trim or restore the default");
	std::string patternError;
	Expect(jpegview_linux::ValidateWindowTitlePattern("%f [%p] %%", &patternError) &&
		patternError.empty(), "valid window-title pattern was rejected");
	Expect(!jpegview_linux::ValidateWindowTitlePattern("%f %q", &patternError) &&
		patternError.find("%q") != std::string::npos,
		"unknown window-title code was accepted without an explanation");
	Expect(!jpegview_linux::ValidateWindowTitlePattern("dangling%", &patternError) &&
		patternError.find("trailing %") != std::string::npos,
		"incomplete window-title code was accepted without an explanation");
	Expect(!jpegview_linux::ValidateWindowTitlePattern("two\nlines", &patternError) &&
		!jpegview_linux::ValidateWindowTitlePattern(
			std::string(jpegview_linux::kMaximumWindowTitlePatternBytes + 1, 'x'), &patternError),
		"window-title pattern accepted line breaks or an oversized value");
	const std::string titleLegend = std::string(jpegview_linux::kWindowTitlePatternHelpLine1) +
		" " + jpegview_linux::kWindowTitlePatternHelpLine2 + " " +
		jpegview_linux::kWindowTitlePatternHelpLine3;
	for (const char* token : {"%p", "%i", "%n", "%f", "%F", "%e", "%P", "%D",
		"%w", "%h", "%s", "%b", "%m", "%a", "%v", "%%"}) {
		Expect(titleLegend.find(token) != std::string::npos,
			std::string("Advanced configuration title legend omits ") + token);
	}
	Expect(titleLegend.find("%i one-based image index") != std::string::npos &&
		titleLegend.find("%F filename stem (no extension)") != std::string::npos &&
		titleLegend.find("%e extension (no dot)") != std::string::npos &&
		titleLegend.find("%w/%h original pixel width/height") != std::string::npos,
		"Advanced configuration title legend did not clarify index, filename, extension, or dimension codes");
	context.fileSize = 0;
	Expect(jpegview_linux::FormatWindowTitle("%s|%b|%m", context) ==
		"0 B|0|8001x6000, 0 B",
		"window-title pattern did not distinguish a zero-byte file from an unavailable size");

	jpegview_linux::SourceIdentity archiveIdentity;
	archiveIdentity.size = 9u * 1024u * 1024u;
	archiveIdentity.valid = true;
	const std::uint64_t memberSize = 1536u * 1024u;
	const jpegview_linux::SourceDescriptor archiveMember =
		jpegview_linux::DescribeArchiveMember("/photos/set.zip/image.jpg",
			archiveIdentity, memberSize, 0, false);
	WindowTitleContext archiveContext;
	archiveContext.width = 100;
	archiveContext.height = 80;
	archiveContext.fileSize = archiveMember.Metadata().fileSize;
	Expect(archiveMember.Metadata().archiveMember &&
		jpegview_linux::FormatWindowTitle("%s|%b", archiveContext) == "1.5 MB|1572864" &&
		jpegview_linux::FormatImageDimensionsAndSize(archiveContext.width,
			archiveContext.height,
			jpegview_linux::FormatFileSize(archiveMember.Metadata().fileSize)) ==
			"100 X 80, 1.5 MB" &&
		archiveMember.Metadata().fileSize != archiveMember.BackingIdentity().size,
		"title or information formatting used the archive container size for a member");

	int modeledSourceReads = 0;
	int titleBuilds = 0;
	int infoBuilds = 0;
	int appliedTitleChanges = 0;
	jpegview_linux::WindowTitleFormatCache titleCache;
	jpegview_linux::ImageInfoLineCache infoCache;
	jpegview_linux::AppliedWindowTitle appliedTitle;
	WindowTitleContext cachedContext = context;
	cachedContext.filename = "same-image.jpg";
	const auto requestTitle = [&](const std::string& revision) -> const std::string& {
		return titleCache.GetOrBuild(revision, [&] {
			++titleBuilds;
			++modeledSourceReads;
			return jpegview_linux::FormatWindowTitle("[%p] %f %m", cachedContext);
		});
	};
	const auto requestInfo = [&](const std::string& revision)
		-> const std::vector<std::string>& {
		return infoCache.GetOrBuild(revision, [&] {
			++infoBuilds;
			++modeledSourceReads;
			return std::vector<std::string>{cachedContext.filename,
				jpegview_linux::FormatImageDimensionsAndSize(cachedContext.width,
					cachedContext.height, "10 MB")};
		});
	};
	const std::string initialRevision = "catalog=1;source=image-a;index=1";
	for (int pan = 0; pan < 250; ++pan) {
		const std::string title = requestTitle(initialRevision);
		if (appliedTitle.Update(title)) ++appliedTitleChanges;
		const auto& lines = requestInfo(initialRevision);
		Expect(lines.size() == 2 && lines.front() == "same-image.jpg",
			"cached overlay formatting changed during a repeated pan");
	}
	Expect(modeledSourceReads == 2 && titleBuilds == 1 && infoBuilds == 1 &&
		appliedTitleChanges == 1,
		"repeated pan rebuilt title or metadata text, or reapplied an unchanged title");

	cachedContext.filename = "reloaded-image.jpg";
	const std::string reloadRevision = "catalog=2;source=image-b;index=1";
	const std::string reloadedTitle = requestTitle(reloadRevision);
	if (appliedTitle.Update(reloadedTitle)) ++appliedTitleChanges;
	const auto& reloadedLines = requestInfo(reloadRevision);
	Expect(modeledSourceReads == 4 && titleBuilds == 2 && infoBuilds == 2 &&
		appliedTitleChanges == 2 && reloadedLines.front() == "reloaded-image.jpg" &&
		reloadedTitle.find("reloaded-image.jpg") != std::string::npos,
		"catalog/source reload did not refresh cached title and information text");
}

void TestSystemFontResolutionAndUnicodeRendering() {
	TemporaryDirectory temporary;
	const fs::path home = temporary.path() / "home";
	const fs::path config = temporary.path() / "config";
	fs::create_directories(config / "xfce4/xfconf/xfce-perchannel-xml");
	fs::create_directories(config / "gtk-3.0");
	WriteText(config / "gtk-3.0/settings.ini",
		"[Settings]\ngtk-font-name=GTK Choice 11\n");
	WriteText(config / "xfce4/xfconf/xfce-perchannel-xml/xsettings.xml",
		"<channel><property value=\"Tahoma &amp; Friends 12\" type=\"string\" name=\"FontName\"/></channel>\n");
	ScopedEnvironment overrideFont("JPEGVIEW_FONT", "");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Tahoma & Friends 12",
		"XFCE system font was not preferred or its XML value was not decoded");

	fs::remove(config / "xfce4/xfconf/xfce-perchannel-xml/xsettings.xml");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "GTK Choice 11",
		"GTK system font was not used when XFCE settings were absent");
	fs::remove(config / "gtk-3.0/settings.ini");
	fs::create_directories(home);
	WriteText(home / ".gtkrc-2.0", "gtk-font-name = \"Legacy Choice 9\"\n");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Legacy Choice 9",
		"GTK 2 system font was not parsed");
	fs::remove(home / ".gtkrc-2.0");
	fs::create_directories(config / "xsettingsd");
	WriteText(config / "xsettingsd/xsettingsd.conf", "Gtk/FontName 'Xsettings Choice 10'\n");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Xsettings Choice 10",
		"xsettingsd system font was not parsed");
	fs::remove(config / "xsettingsd/xsettingsd.conf");
	WriteText(config / "kdeglobals", "[General]\nfont=KDE Choice,11,-1,5,50,0,0,0,0,0\n");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "KDE Choice 11",
		"KDE system font family and size were not parsed");
	fs::remove(config / "kdeglobals");
	Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Sans 10",
		"missing desktop font settings did not use the documented fallback");
	{
		ScopedEnvironment explicitOverride("JPEGVIEW_FONT", "  Override Choice 13  ");
		Expect(jpegview_linux::ResolveDesktopFontDescription(home, config) == "Override Choice 13",
			"JPEGVIEW_FONT did not override desktop settings or was not trimmed");
	}

	jpegview_linux::SystemFont font("Sans 10");
	Expect(font.LineHeight() == jpegview_linux::Terminus9LineHeight() &&
		font.TextWidth("iiii") == font.TextWidth("WWWW") &&
		font.TextWidth("Terminus 9") == jpegview_linux::Terminus9TextWidth("Terminus 9"),
		"printable ASCII did not use the fixed-width 9-point bitmap metrics");
	Expect(jpegview_linux::Terminus9CanRender("Mixed Case 123") &&
		!jpegview_linux::Terminus9CanRender(u8"café"),
		"bitmap-font coverage did not distinguish printable ASCII from Unicode");
	const jpegview_linux::BitmapFontGlyph& uppercase = jpegview_linux::Terminus9Glyph('A');
	const jpegview_linux::BitmapFontGlyph& lowercase = jpegview_linux::Terminus9Glyph('a');
	const jpegview_linux::BitmapGlyphInkBounds narrowGlyph =
		jpegview_linux::Terminus9GlyphInkBounds('i');
	Expect(jpegview_linux::Terminus9LineHeight() == 13 && uppercase.advance == 6 &&
		uppercase.width == 6 && uppercase.height == 12,
		"9-point font did not use the embedded 12-pixel monochrome bitmap strike");
	Expect(narrowGlyph.width > 0 && narrowGlyph.width <
		jpegview_linux::Terminus9Glyph('i').advance && narrowGlyph.left >= 0 &&
		narrowGlyph.left + narrowGlyph.width <= jpegview_linux::Terminus9Glyph('i').width,
		"bitmap glyph ink bounds included blank advance-cell pixels");
	Expect(uppercase.pixelOffset != lowercase.pixelOffset,
		"9-point bitmap font mapped lowercase letters to uppercase glyphs");
	const std::uint8_t expectedAdvance = uppercase.advance;
	for (unsigned int character = 32; character <= 126; ++character) {
		const jpegview_linux::BitmapFontGlyph& glyph = jpegview_linux::Terminus9Glyph(
			static_cast<unsigned char>(character));
		Expect(glyph.advance == expectedAdvance,
			"9-point Terminus bitmap glyphs do not use a fixed-width advance");
		const std::uint8_t* pixels = jpegview_linux::Terminus9GlyphPixels(glyph);
		const std::size_t pixelCount = static_cast<std::size_t>(glyph.width) * glyph.height;
		Expect(std::all_of(pixels, pixels + pixelCount, [](std::uint8_t value) {
			return value == 0 || value == 255;
		}), "embedded Terminus glyph contains antialiased pixel values");
	}
	const jpegview_linux::RasterizedText asciiRaster = font.Rasterize("Mixed Case");
	Expect(asciiRaster.width > 0 && asciiRaster.height == jpegview_linux::Terminus9LineHeight() &&
		!asciiRaster.argb.empty() &&
		std::any_of(asciiRaster.argb.begin(), asciiRaster.argb.end(), [](std::uint32_t pixel) {
			return (pixel >> 24) == 255;
		}) &&
		std::all_of(asciiRaster.argb.begin(), asciiRaster.argb.end(), [](std::uint32_t pixel) {
			return (pixel >> 24) == 0 || (pixel >> 24) == 255;
		}), "9-point ASCII bitmap was not rasterized with crisp one-bit coverage");
	const jpegview_linux::RasterizedText raster = font.Rasterize(u8"Привет — 日本語");
	// Unicode text uses the independently sized system font, not the embedded
	// bitmap font whose fixed line height is returned by SystemFont::LineHeight.
	Expect(raster.width > 0 && raster.height > 0 && !raster.argb.empty(),
		"system font did not rasterize non-Latin UTF-8 text");
	Expect(std::any_of(raster.argb.begin(), raster.argb.end(), [](std::uint32_t pixel) {
		return (pixel >> 24) != 0;
	}), "non-Latin system-font text rasterized as an empty image");
	const std::string invalidUtf8 = std::string("valid") + static_cast<char>(0xff);
	Expect(font.TextWidth(invalidUtf8) > 0 && !font.Rasterize(invalidUtf8).argb.empty(),
		"system font did not replace malformed UTF-8 safely");
}

void TestPlaybackSchedulerTimingAndModes() {
	using jpegview_linux::PlaybackActionType;
	using jpegview_linux::PlaybackMode;
	jpegview_linux::PlaybackScheduler scheduler;
	scheduler.ConfigureImage({20, 30}, 2, true, 100);
	Expect(scheduler.AnimationPlaying() && scheduler.NextTick() == 120,
		"animated image did not schedule its first frame delay");
	Expect(scheduler.Tick(119).type == PlaybackActionType::None,
		"animation advanced before its frame deadline");
	Expect(scheduler.Tick(120).type == PlaybackActionType::ShowFrame &&
		scheduler.FrameIndex() == 1 && scheduler.NextTick() == 150,
		"animation did not advance or schedule the second frame");
	Expect(scheduler.Tick(150).type == PlaybackActionType::ShowFrame &&
		scheduler.FrameIndex() == 0 && scheduler.CompletedLoops() == 1,
		"animation did not wrap after its first loop");
	scheduler.Tick(170);
	Expect(scheduler.Tick(200).type == PlaybackActionType::None &&
		!scheduler.AnimationPlaying() && scheduler.CompletedLoops() == 2,
		"finite animation did not stop after its declared loop count");
	const jpegview_linux::PlaybackAction resumed = scheduler.Resume(300);
	Expect(resumed.type == PlaybackActionType::ShowFrame && resumed.frameIndex == 0 &&
		scheduler.AnimationPlaying() && scheduler.NextTick() == 320,
		"animation resume did not rewind a completed sequence");
	scheduler.FrameDisplayFailed();
	Expect(!scheduler.AnimationPlaying() && scheduler.NextTick() == 0,
		"failed animation frame did not stop scheduling");
	jpegview_linux::PlaybackScheduler failedSlideshow;
	failedSlideshow.StartSlideshow(0.1, 500);
	failedSlideshow.ConfigureImage({20, 20}, 0, true, 500);
	failedSlideshow.SetImageReady(false, 510);
	failedSlideshow.FrameDisplayFailed();
	Expect(failedSlideshow.Tick(599).type == PlaybackActionType::None &&
		failedSlideshow.Tick(600).type == PlaybackActionType::NextImage,
		"failed animation frame left slideshow permanently unready");

	jpegview_linux::PlaybackScheduler repeatedReady;
	repeatedReady.ConfigureImage({20, 20}, 0, true, 100);
	repeatedReady.SetImageReady(false, 110);
	repeatedReady.SetImageReady(true, 200);
	const std::uint32_t readyFrameDeadline = repeatedReady.NextTick();
	repeatedReady.SetImageReady(true, 210);
	Expect(readyFrameDeadline == 220 && repeatedReady.NextTick() == readyFrameDeadline &&
		repeatedReady.Tick(readyFrameDeadline).type == PlaybackActionType::ShowFrame &&
		repeatedReady.FrameIndex() == 1,
		"repeated renderer-ready notifications postponed an animation frame indefinitely");

	jpegview_linux::PlaybackScheduler flattenedAnimation;
	flattenedAnimation.ConfigureImage({20, 20}, 0, true, 100);
	flattenedAnimation.StartMovie(25.0, 100);
	flattenedAnimation.SetImageReady(false, 110);
	flattenedAnimation.ConfigureStillImage(120);
	Expect(flattenedAnimation.Mode() == PlaybackMode::None &&
		!flattenedAnimation.HasAnimation() && !flattenedAnimation.AnimationPlaying() &&
		!flattenedAnimation.NextDeadline().has_value(),
		"flattening a saved animation should stop active playback and clear its frames");
	flattenedAnimation.StartMovie(25.0, 130);
	Expect(flattenedAnimation.NextDeadline() == 170 &&
		flattenedAnimation.Tick(170).type == PlaybackActionType::NextImage,
		"movie playback could not restart after an animated source was flattened");
	flattenedAnimation.StartSlideshow(0.1, 180);
	Expect(flattenedAnimation.NextDeadline() == 280 &&
		flattenedAnimation.Tick(280).type == PlaybackActionType::NextImage,
		"slideshow playback could not restart after an animated source was flattened");

	scheduler.ConfigureImage({}, 0, false, 400);
	scheduler.StartMovie(25.0, 400);
	Expect(scheduler.Mode() == PlaybackMode::Movie && scheduler.NextTick() == 440 &&
		scheduler.Tick(439).type == PlaybackActionType::None &&
		scheduler.Tick(440).type == PlaybackActionType::NextImage,
		"movie mode did not advance a static image at its frame interval");
	scheduler.StartMovie(1000.0, 500);
	Expect(scheduler.MovieFramesPerSecond() == 100.0 && scheduler.NextTick() == 510,
		"movie speed or minimum interval was not clamped");

	scheduler.StartSlideshow(0.05, 1000);
	Expect(scheduler.Mode() == PlaybackMode::Slideshow &&
		scheduler.SlideshowSeconds() == 0.1 &&
		scheduler.Tick(1099).type == PlaybackActionType::None &&
		scheduler.Tick(1100).type == PlaybackActionType::NextImage,
		"slideshow delay was not clamped or honored");
	scheduler.NotifyInteraction(1200);
	Expect(scheduler.Tick(1250).type == PlaybackActionType::None,
		"interaction did not postpone slideshow advancement");
	scheduler.Stop(1300);
	Expect(scheduler.Mode() == PlaybackMode::None && scheduler.SlideshowSeconds() == 0.0,
		"stopping playback did not clear active mode state");

	jpegview_linux::PlaybackScheduler pendingSlideshow;
	pendingSlideshow.StartSlideshow(1.0, 0);
	Expect(pendingSlideshow.NextDeadline() == 1000,
		"slideshow did not expose its idle event-loop deadline");
	pendingSlideshow.SetImageReady(false, 100);
	Expect(!pendingSlideshow.NextDeadline().has_value() &&
		pendingSlideshow.Tick(5000).type == PlaybackActionType::None,
		"slideshow advanced while the current JPEG header was pending");
	pendingSlideshow.SetImageReady(true, 5000);
	Expect(pendingSlideshow.NextDeadline() == 6000 &&
		pendingSlideshow.Tick(5999).type == PlaybackActionType::None &&
		pendingSlideshow.Tick(6000).type == PlaybackActionType::NextImage,
		"slideshow deadline did not restart when the cold image committed");

	jpegview_linux::PlaybackScheduler stoppedAtBoundary;
	stoppedAtBoundary.StartSlideshow(0.1, 100);
	Expect(stoppedAtBoundary.Tick(200).type == PlaybackActionType::NextImage,
		"slideshow did not become due before the folder boundary");
	stoppedAtBoundary.SetImageReady(false, 200);
	stoppedAtBoundary.Stop(210);
	stoppedAtBoundary.SetImageReady(true, 210);
	Expect(stoppedAtBoundary.Mode() == PlaybackMode::None &&
		!stoppedAtBoundary.NextDeadline().has_value() &&
		stoppedAtBoundary.Tick(500).type == PlaybackActionType::None,
		"stopping a slideshow at a failed folder boundary left an expired deadline");
	stoppedAtBoundary.StartSlideshow(0.1, 500);
	Expect(stoppedAtBoundary.NextDeadline() == 600 &&
		stoppedAtBoundary.Tick(599).type == PlaybackActionType::None &&
		stoppedAtBoundary.Tick(600).type == PlaybackActionType::NextImage,
		"slideshow could not be restarted after reaching a non-wrapping boundary");

	jpegview_linux::PlaybackScheduler pendingMovie;
	pendingMovie.StartMovie(25.0, 0);
	Expect(pendingMovie.NextDeadline() == 40,
		"movie mode did not expose its event-loop deadline");
	pendingMovie.SetImageReady(false, 10);
	Expect(!pendingMovie.NextDeadline().has_value() &&
		pendingMovie.Tick(1000).type == PlaybackActionType::None,
		"movie advanced while the current JPEG header was pending");
	pendingMovie.SetImageReady(true, 1000);
	Expect(pendingMovie.Tick(1039).type == PlaybackActionType::None &&
		pendingMovie.Tick(1040).type == PlaybackActionType::NextImage,
		"movie interval did not restart when the cold image committed");

	jpegview_linux::PlaybackScheduler modalAnimation;
	modalAnimation.ConfigureImage({20, 30}, 0, true, 100);
	modalAnimation.SetTemporarilyPaused(true, 110);
	modalAnimation.SetImageReady(false, 120);
	modalAnimation.SetImageReady(true, 5000);
	Expect(modalAnimation.AnimationPlaying() && modalAnimation.FrameIndex() == 0 &&
		!modalAnimation.NextDeadline().has_value() &&
		modalAnimation.Tick(5000).type == PlaybackActionType::None,
		"temporary modal pause did not suppress ready animation advancement");
	modalAnimation.SetTemporarilyPaused(false, 5100);
	Expect(modalAnimation.NextDeadline() == 5120 &&
		modalAnimation.Tick(5119).type == PlaybackActionType::None &&
		modalAnimation.Tick(5120).type == PlaybackActionType::ShowFrame &&
		modalAnimation.FrameIndex() == 1,
		"animation did not resume from its committed frame after a modal pause");

	jpegview_linux::PlaybackScheduler modalSlideshow;
	modalSlideshow.StartSlideshow(1.0, 0);
	modalSlideshow.SetTemporarilyPaused(true, 100);
	Expect(!modalSlideshow.NextDeadline().has_value() &&
		modalSlideshow.Tick(5000).type == PlaybackActionType::None,
		"temporary modal pause left an expired slideshow deadline active");
	modalSlideshow.SetTemporarilyPaused(false, 5000);
	Expect(modalSlideshow.NextDeadline() == 6000 &&
		modalSlideshow.Tick(5999).type == PlaybackActionType::None &&
		modalSlideshow.Tick(6000).type == PlaybackActionType::NextImage,
		"slideshow did not restart its interval after a modal pause");

	jpegview_linux::PlaybackScheduler modalMovie;
	modalMovie.StartMovie(25.0, 0);
	modalMovie.SetTemporarilyPaused(true, 10);
	Expect(!modalMovie.NextDeadline().has_value() &&
		modalMovie.Tick(1000).type == PlaybackActionType::None,
		"temporary modal pause left an expired movie deadline active");
	modalMovie.SetTemporarilyPaused(false, 1000);
	Expect(modalMovie.NextDeadline() == 1040 &&
		modalMovie.Tick(1040).type == PlaybackActionType::NextImage,
		"movie mode did not restart its interval after a modal pause");

	jpegview_linux::PlaybackScheduler wrapping;
	wrapping.ConfigureImage({20, 20}, 0, true, 0xfffffff5u);
	Expect(wrapping.NextTick() == 9 &&
		wrapping.Tick(8).type == PlaybackActionType::None &&
		wrapping.Tick(9).type == PlaybackActionType::ShowFrame,
		"animation deadline comparison failed across tick wraparound");
	wrapping.StartSlideshow(0.1, 0xfffffff0u);
	Expect(wrapping.Tick(83).type == PlaybackActionType::None &&
		wrapping.Tick(84).type == PlaybackActionType::NextImage,
		"slideshow elapsed time failed across tick wraparound");
}

void TestPlaybackSchedulerAnimationFrameControls() {
	using jpegview_linux::PlaybackActionType;
	using jpegview_linux::PlaybackMode;
	jpegview_linux::PlaybackScheduler stepped;
	stepped.ConfigureImage({25, 90, 130}, 0, true, 100);
	Expect(stepped.StepAnimationFrame(1, 101).type == PlaybackActionType::ShowFrame &&
		stepped.FrameIndex() == 1 && stepped.AnimationManuallyPaused() &&
		!stepped.AnimationPlaying() && !stepped.NextDeadline().has_value(),
		"manual next-frame did not select one frame and freeze all animation deadlines");
	Expect(stepped.Tick(10000).type == PlaybackActionType::None &&
		stepped.StepAnimationFrame(1, 10001).frameIndex == 2 &&
		stepped.StepAnimationFrame(1, 10002).frameIndex == 0 &&
		stepped.StepAnimationFrame(-1, 10003).frameIndex == 2,
		"manual frame stepping did not wrap in both directions or remain frozen");
	Expect(stepped.ToggleAnimationPlayback(10004).type == PlaybackActionType::None &&
		!stepped.AnimationManuallyPaused() && stepped.AnimationPlaying() &&
		stepped.FrameIndex() == 2 && stepped.NextDeadline() == 10134,
		"resuming a manually selected last frame rewound it or used the wrong frame delay");
	Expect(stepped.ToggleAnimationPlayback(10005).type == PlaybackActionType::None &&
		stepped.AnimationManuallyPaused() && !stepped.NextDeadline().has_value() &&
		stepped.Tick(20000).type == PlaybackActionType::None,
		"manual freeze did not suppress later animation ticks");
	stepped.SetImageReady(false, 20001);
	stepped.SetImageReady(true, 21000);
	Expect(stepped.AnimationManuallyPaused() && !stepped.AnimationPlaying() &&
		!stepped.NextDeadline().has_value(),
		"renderer readiness restored playback against an explicit user freeze");
	stepped.ToggleAnimationPlayback(21001);
	Expect(stepped.AnimationPlaying() && stepped.NextDeadline() == 21131,
		"resuming after a renderer-ready transition did not start a fresh frame interval");

	jpegview_linux::PlaybackScheduler movie;
	movie.ConfigureImage({20, 20}, 1, true, 0);
	movie.StartMovie(25.0, 0);
	Expect(movie.StepAnimationFrame(1, 1).frameIndex == 1 &&
		movie.Mode() == PlaybackMode::Movie && movie.AnimationManuallyPaused() &&
		!movie.NextDeadline().has_value() &&
		movie.Tick(1000).type == PlaybackActionType::None,
		"manual frame stepping allowed Movie mode to advance the folder");
	movie.ToggleAnimationPlayback(1000);
	Expect(movie.AnimationPlaying() && movie.NextDeadline() == 1040 &&
		movie.Tick(1039).type == PlaybackActionType::None &&
		movie.Tick(1040).type == PlaybackActionType::NextImage,
		"resuming a manually stepped Movie frame did not preserve Movie FPS and loop behavior");
	Expect(movie.ToggleAnimationPlayback(1041).type == PlaybackActionType::None &&
		movie.AnimationManuallyPaused() && !movie.NextDeadline().has_value() &&
		movie.Tick(10000).type == PlaybackActionType::None,
		"manual freeze did not suppress Movie folder advancement after finite playback ended");
	movie.ToggleAnimationPlayback(10001);
	Expect(!movie.AnimationPlaying() && movie.NextDeadline() == 10041 &&
		movie.Tick(10040).type == PlaybackActionType::None &&
		movie.Tick(10041).type == PlaybackActionType::NextImage,
		"resuming a manually held finite Movie did not restart its folder interval");

	jpegview_linux::PlaybackScheduler finite;
	finite.ConfigureImage({20, 30}, 1, true, 0);
	finite.StepAnimationFrame(1, 1);
	finite.ToggleAnimationPlayback(2);
	Expect(finite.FrameIndex() == 1 && finite.NextDeadline() == 32 &&
		finite.Tick(32).type == PlaybackActionType::None &&
		!finite.AnimationPlaying() && finite.CompletedLoops() == 1,
		"manually stepping reset the finite sequence's remaining loop run incorrectly");
	const auto restarted = finite.Resume(40);
	Expect(restarted.type == PlaybackActionType::ShowFrame && restarted.frameIndex == 0 &&
		finite.FrameIndex() == 0 && finite.AnimationPlaying() &&
		finite.CompletedLoops() == 0 && finite.NextDeadline() == 60,
		"a naturally exhausted finite animation did not restart as a new loop run");

	jpegview_linux::PlaybackScheduler delays;
	delays.ConfigureImage({100, 250}, 0, true, 0);
	delays.StartMovie(25.0, 0);
	delays.StepAnimationFrame(1, 1);
	Expect(delays.FrameDelayMs() == 250 && delays.OriginalFrameDelayMs() == 250 &&
		delays.AdjustAnimationDelay(50, 20) && delays.Mode() == PlaybackMode::None &&
		delays.AnimationPlaying() && delays.FrameDelayMs() == 300 &&
		delays.AnimationDelayOverrideMs() == 300 && delays.NextDeadline() == 320,
		"slowing an animation did not create a per-image uniform delay or leave Movie mode");
	Expect(delays.Tick(319).type == PlaybackActionType::None &&
		delays.Tick(320).frameIndex == 0 && delays.NextDeadline() == 620,
		"the adjusted delay did not apply uniformly to each frame");
	Expect(delays.AdjustAnimationDelay(std::numeric_limits<int>::min(), 400) &&
		delays.FrameDelayMs() == 10 && delays.NextDeadline() == 410 &&
		delays.AdjustAnimationDelay(std::numeric_limits<int>::max(), 500) &&
		delays.FrameDelayMs() == 60000 && delays.NextDeadline() == 60500,
		"animation delay adjustment did not saturate safely at its supported limits");
	Expect(delays.ResetAnimationDelay(600) && !delays.AnimationDelayOverrideMs().has_value() &&
		delays.FrameDelayMs() == 100 && delays.NextDeadline() == 700 &&
		!delays.ResetAnimationDelay(700),
		"restoring native delays did not clear the override or retain original frame timing");

	jpegview_linux::PlaybackScheduler unavailable;
	unavailable.ConfigureImage({100}, 0, false, 0);
	Expect(unavailable.StepAnimationFrame(1, 1).type == PlaybackActionType::None &&
		!unavailable.AdjustAnimationDelay(50, 2) &&
		!unavailable.ResetAnimationDelay(3),
		"still images accepted animation-only frame or delay controls");
}

void TestEventLoopInvalidationDeadlinesWakeupsAndMotion() {
	using jpegview_linux::FrameInvalidationReason;
	using jpegview_linux::FrameInvalidator;
	FrameInvalidator invalidator;
	Expect(!invalidator.NeedsRender() && invalidator.Consume() == 0,
		"a clean presentation requested an idle redraw");
	invalidator.Mark(FrameInvalidationReason::Input);
	invalidator.Mark(FrameInvalidationReason::Viewport);
	invalidator.Mark(FrameInvalidationReason::Input);
	Expect(invalidator.NeedsRender() && invalidator.Consume() ==
		(jpegview_linux::FrameInvalidationBit(FrameInvalidationReason::Input) |
			jpegview_linux::FrameInvalidationBit(FrameInvalidationReason::Viewport)) &&
		!invalidator.NeedsRender(),
		"frame invalidation did not coalesce reasons or clear after presentation");

	Expect(jpegview_linux::EventWaitTimeoutMs(100, {}, 100) == 100 &&
		jpegview_linux::EventWaitTimeoutMs(100, {140, 125}, 100) == 25 &&
		jpegview_linux::EventWaitTimeoutMs(125, {125}, 100) == 0 &&
		jpegview_linux::EventWaitTimeoutMs(0xfffffff0u, {0x10u}, 100) == 32 &&
		jpegview_linux::EventWaitTimeoutMs(100, {}, 0) == 1,
		"event wait deadline selection lost the finite fallback or tick-wrap behavior");
	const auto uploadDeadline =
		jpegview_linux::DisplayUploadContinuationDeadline(100, true);
	Expect(uploadDeadline == 108u &&
		jpegview_linux::EventWaitTimeoutMs(100, {*uploadDeadline}, 100) == 8 &&
		!jpegview_linux::DisplayUploadContinuationDeadline(100, false).has_value() &&
		jpegview_linux::DisplayUploadContinuationDeadline(0xfffffffcu, true) == 4u,
		"eligible banded uploads did not schedule an early wrap-safe continuation deadline");

	jpegview_linux::CoalescedCompletionWakeup wakeup;
	int posts = 0;
	wakeup.SetPostFunction([&posts] {
		++posts;
		return true;
	});
	Expect(wakeup.Notify() && wakeup.Notify() && posts == 1 && wakeup.Pending(),
		"completion wakeups did not coalesce while the SDL event was pending");
	wakeup.Consume();
	Expect(wakeup.Notify() && posts == 2,
		"completion wakeup was not rearmed after the event loop consumed it");
	wakeup.Consume();
	wakeup.SetPostFunction([&posts] {
		++posts;
		return false;
	});
	Expect(!wakeup.Notify() && wakeup.Pending() && wakeup.Notify() && posts == 3,
		"failed event posting did not preserve pending work for the finite fallback");
	wakeup.Consume();
	Expect(!wakeup.Notify() && wakeup.Pending() && posts == 4,
		"completion wakeup did not retry after fallback queue consumption");
	wakeup.SetPostFunction({});
	Expect(!wakeup.Notify() && wakeup.Pending(),
		"an unconfigured completion wakeup did not preserve work for fallback polling");
	wakeup.Consume();
	Expect(!wakeup.Pending(),
		"fallback polling could not clear an unconfigured completion notification");

	jpegview_linux::MouseMotionSample motion{10, 20, 3, -4, 1};
	Expect(jpegview_linux::CoalesceMouseMotion(motion,
		jpegview_linux::MouseMotionSample{17, 15, 7, -5, 1}) &&
		motion.x == 17 && motion.y == 15 && motion.xrel == 10 && motion.yrel == -9,
		"consecutive pan motion was not accumulated at its latest pointer position");
	Expect(!jpegview_linux::CoalesceMouseMotion(motion,
		jpegview_linux::MouseMotionSample{22, 20, 5, 5, 0}) &&
		motion.x == 17 && motion.y == 15 && motion.xrel == 10 && motion.yrel == -9,
		"motion coalescing crossed a mouse-button state boundary");
	motion.xrel = std::numeric_limits<std::int32_t>::max() - 1;
	Expect(jpegview_linux::CoalesceMouseMotion(motion,
		jpegview_linux::MouseMotionSample{24, 22, 10, 0, 1}) &&
		motion.xrel == std::numeric_limits<std::int32_t>::max(),
		"large motion accumulation overflowed the SDL relative-coordinate range");
}

void TestModalEventRouterPrecedence() {
	using jpegview_linux::ModalEventRoute;
	using jpegview_linux::ModalEventState;
	using jpegview_linux::ResolveModalEventRoute;
	ModalEventState state;
	Expect(ResolveModalEventRoute(state) == ModalEventRoute::Viewer,
		"no active modal should route events to the viewer");

	const std::vector<std::pair<bool*, ModalEventRoute>> precedence{
		{&state.archivePassword, ModalEventRoute::ArchivePassword},
		{&state.confirmation, ModalEventRoute::Confirmation},
		{&state.help, ModalEventRoute::Help},
		{&state.about, ModalEventRoute::About},
		{&state.advancedConfiguration, ModalEventRoute::AdvancedConfiguration},
		{&state.fileDialog, ModalEventRoute::FileDialog},
		{&state.batchCopy, ModalEventRoute::BatchCopy},
		{&state.resize, ModalEventRoute::Resize},
		{&state.freeRotation, ModalEventRoute::FreeRotation},
		{&state.perspectiveCorrection, ModalEventRoute::PerspectiveCorrection},
		{&state.fixedCropSize, ModalEventRoute::FixedCropSize},
		{&state.goToImageNumber, ModalEventRoute::GoToImageNumber},
		{&state.unsharpMask, ModalEventRoute::UnsharpMask},
		{&state.pictureLevels, ModalEventRoute::PictureLevels},
		{&state.contextMenu, ModalEventRoute::ContextMenu},
	};
	for (const auto& entry : precedence) *entry.first = true;
	for (const auto& entry : precedence) {
		Expect(ResolveModalEventRoute(state) == entry.second,
			"the active modal with the highest precedence should own the event");
		*entry.first = false;
	}

	for (const auto& entry : precedence) {
		*entry.first = true;
		Expect(ResolveModalEventRoute(state) == entry.second,
			"each modal should route to its own handler when it is the only active modal");
		*entry.first = false;
	}
}

void TestGoToImageNumberModel() {
	jpegview_linux::GoToImageNumberModel model;
	model.Open(15000, 249);
	Expect(model.IsOpen() && model.Text() == "250" && model.ImageCount() == 15000,
		"go-to dialog did not prime the current one-based image number");
	Expect(model.AppendText("7") && model.Text() == "7" &&
		model.Submit() == std::optional<std::size_t>(6),
		"first typed digit did not replace the primed value or convert to a zero-based index");

	model.Open(15000, 0);
	model.SelectAll();
	Expect(model.AppendText("15000") && model.Submit() == std::optional<std::size_t>(14999),
		"the final image number was rejected or mapped to the wrong list index");

	model.Open(15000, 0);
	model.SelectAll();
	Expect(model.AppendText("0") && !model.Submit().has_value() &&
		model.Message().find("1 to 15000") != std::string::npos,
		"zero was accepted as a one-based image number");
	model.SelectAll();
	Expect(model.AppendText("15001") && !model.Submit().has_value(),
		"an image number beyond the active list was accepted");
	model.SelectAll();
	Expect(!model.Submit().has_value() && model.Message() == "Enter an image number.",
		"empty go-to input did not report a validation error");
	model.SelectAll();
	Expect(!model.AppendText("2x") && model.Text().empty(),
		"go-to input accepted non-digit characters or partially appended malformed text");
	model.SelectAll();
	Expect(model.AppendText("99999999999999999999") && !model.Submit().has_value(),
		"an overflowing image number was accepted");

	model.Open(0, 0);
	Expect(!model.IsOpen() && !model.Submit().has_value(),
		"go-to dialog opened for an empty file list");
	model.Open(4, 2);
	model.Backspace();
	Expect(model.Text().empty(), "backspace did not clear the primed current image number");
	model.Close();
	Expect(!model.IsOpen(), "go-to dialog remained open after cancellation");
}

void TestPixelColorSamplerReadsBgraAsRgba() {
	const std::vector<std::uint8_t> pixels = {
		0x56, 0x34, 0x12, 0x78,
		0xff, 0x00, 0xab, 0xcd,
	};
	const std::optional<jpegview_linux::PixelColorRgba> first =
		jpegview_linux::SampleBgraPixel(pixels, 2, 1, 0, 0);
	Expect(first.has_value() && first->red == 0x12 && first->green == 0x34 &&
		first->blue == 0x56 && first->alpha == 0x78 &&
		jpegview_linux::FormatPixelColorRgba(*first) == "#12345678",
		"pixel sampler did not map BGRA storage to uppercase RGBA hex");
	const std::optional<jpegview_linux::PixelColorRgba> second =
		jpegview_linux::SampleBgraPixel(pixels, 2, 1, 1, 0);
	Expect(second.has_value() &&
		jpegview_linux::FormatPixelColorRgba(*second) == "#AB00FFCD",
		"pixel sampler returned the wrong edge pixel or alpha channel");
	Expect(!jpegview_linux::SampleBgraPixel(pixels, 2, 1, -1, 0).has_value() &&
		!jpegview_linux::SampleBgraPixel(pixels, 2, 1, 2, 0).has_value() &&
		!jpegview_linux::SampleBgraPixel(pixels, 0, 1, 0, 0).has_value(),
		"pixel sampler accepted invalid dimensions or out-of-bounds coordinates");
	Expect(!jpegview_linux::SampleBgraPixel({0, 1, 2}, 1, 1, 0, 0).has_value(),
		"pixel sampler read a truncated pixel buffer");
	Expect(!jpegview_linux::SampleBgraPixel({}, 2147483647, 2147483647, 0, 0).has_value(),
		"pixel sampler accepted dimensions whose byte count overflows size_t");
}

void TestPixelColorSamplerModelUpdatePinAndCopy() {
	const std::vector<std::uint8_t> pixels = {
		0x56, 0x34, 0x12, 0xff,
		0xff, 0x00, 0xab, 0xcd,
	};
	jpegview_linux::PixelColorSamplerInput input;
	input.enabled = true;
	input.ownerGeneration = 4;
	input.documentRevision = 2;
	input.pixelOwner = &pixels;
	input.bgra = &pixels;
	input.imageWidth = 2;
	input.imageHeight = 1;
	input.pixelX = 0;
	input.pixelY = 0;
	input.pointerX = 20;
	input.pointerY = 30;
	input.windowWidth = 200;
	input.windowHeight = 150;
	input.labelWidth = 100;
	input.lineHeight = 16;
	input.imageArea = {0, 0, 100, 100};
	input.destination = {10, 20, 80, 40};

	jpegview_linux::PixelColorSamplerModel model;
	model.Update(input);
	const auto& initial = model.PaintPlan();
	Expect(initial.hex == "#123456FF" && initial.label == "DOC #123456FF" &&
		initial.panel.x == 36 && initial.panel.y == 46 &&
		initial.panel.width == 134 && initial.panel.height == 28 &&
		initial.swatch.x == 42 && initial.swatch.y == 53 &&
		initial.swatch.width == 14 && initial.swatch.height == 14,
		"sampler update did not prepare a correctly placed readout and swatch");

	model.PointerMoved(initial.panel.x + 1, initial.panel.y + 1);
	Expect(model.PaintPlan().pinned &&
		model.CopyTextAt(initial.panel.x + 1, initial.panel.y + 1) == "#123456FF" &&
		!model.CopyTextAt(initial.panel.x + initial.panel.width,
			initial.panel.y + 1).has_value(),
		"sampler did not pin and copy only when clicked inside the readout");
	input.pointerX = 85;
	input.pointerY = 75;
	input.pixelX = 1;
	model.Update(input);
	Expect(model.PaintPlan().pinned && model.PaintPlan().hex == "#123456FF",
		"sampler changed a pinned readout while the pointer remained over it");

	model.PointerMoved(190, 140);
	input.pointerX = 190;
	input.pointerY = 140;
	model.Update(input);
	Expect(!model.PaintPlan().pinned && model.PaintPlan().hex == "#AB00FFCD" &&
		model.PaintPlan().panel.x == 44 && model.PaintPlan().panel.y == 100,
		"sampler did not resume pointer-following after leaving the pinned readout");

	input.documentRevision++;
	model.PointerMoved(model.PaintPlan().panel.x + 1, model.PaintPlan().panel.y + 1);
	input.pointerX = 40;
	input.pointerY = 45;
	input.pixelX = 0;
	model.Update(input);
	Expect(!model.PaintPlan().pinned && model.PaintPlan().hex == "#123456FF",
		"sampler retained pinning or color across a document revision");
	input.enabled = false;
	model.Update(input);
	Expect(model.PaintPlan().hex.empty() && model.PaintPlan().panel.width == 0 &&
		!model.CopyTextAt(40, 45).has_value(),
		"disabled sampler retained visible or copyable state");
}

void TestPixelColorSamplerDecodeDemandRequiresCommittedIdleHover() {
	jpegview_linux::PixelColorSamplerDecodeDemand demand;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"sampler requested source pixels before a committed hover");
	demand.ownerCommitted = true;
	demand.postCommitPointerMotion = true;
	demand.pointerOverImage = true;
	Expect(jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"eligible hover did not request missing source pixels");
	demand.ownerCommitted = false;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"sampler admitted source decoding for an uncommitted image");
	demand.ownerCommitted = true;
	demand.postCommitPointerMotion = false;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"stationary startup cursor caused optional source decoding");
	demand.postCommitPointerMotion = true;
	demand.pointerOverImage = false;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"sampler admitted source decoding while the pointer was outside the image");
	demand.pointerOverImage = true;
	demand.pointerButtonsDown = true;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"sampler admitted full decoding while the pointer was dragging");
	demand.pointerButtonsDown = false;
	demand.hasSampleablePixels = true;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"sampler requested a second source when sampleable pixels were already available");
	demand.hasSampleablePixels = false;
	demand.requestPending = true;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"sampler duplicated an in-flight source decode");
	demand.requestPending = false;
	demand.requestFailed = true;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"sampler retried a failed source decode without a new owner");
}

void TestPixelColorSamplerDecodeDemandRejectsStationaryHeldButtons() {
	jpegview_linux::PixelColorSamplerDecodeDemand demand;
	demand.ownerCommitted = true;
	demand.postCommitPointerMotion = true;
	demand.pointerOverImage = true;
	Expect(jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"idle hover did not admit source decoding before a stationary press");
	// A press after the last motion leaves its button state false. The current
	// mask must reject decoding even without another motion event.
	for (unsigned int button = 0; button < 32; ++button) {
		demand.currentPointerButtons = std::uint32_t{1} << button;
		Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
			"stationary held button admitted optional source decoding");
	}
	demand.currentPointerButtons = 3;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"two stationary held buttons admitted optional source decoding");
	demand.currentPointerButtons = 2;
	Expect(!jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"releasing one button admitted decoding while another remained held");
	demand.currentPointerButtons = 0;
	Expect(jpegview_linux::ShouldDecodePixelColorSamplerSource(demand),
		"releasing all buttons did not restore idle-hover decode eligibility");
}

void TestPixelColorSamplerUsesSourceColorsAfterProcessingAndGeometryEdits() {
	const std::uint8_t sourcePixels[] = {
		0x56, 0x34, 0x12, 0xff,
		0xcc, 0xbb, 0xaa, 0xff,
	};
	jpegview_linux::Image source;
	Expect(source.StoreBGRA(sourcePixels, 2, 1),
		"sampler source fixture could not be initialized");
	jpegview_linux::ImageProcessingParams levels;
	levels.gamma = 2.0;
	jpegview_linux::Image processed = source;
	Expect(processed.ApplyProcessing(levels, false),
		"sampler processed fixture could not apply gamma");
	const auto processedColor = jpegview_linux::SampleBgraPixel(
		processed.bgra, processed.width, processed.height, 0, 0);
	Expect(processedColor.has_value() &&
		jpegview_linux::FormatPixelColorRgba(*processedColor) != "#123456FF",
		"gamma fixture did not change the visible presentation color");

	jpegview_linux::PixelColorSamplerModel model;
	jpegview_linux::PixelColorSamplerInput input;
	input.enabled = true;
	input.ownerGeneration = 8;
	input.documentRevision = 1;
	input.pixelOwner = &source;
	input.bgra = &source.bgra;
	input.imageWidth = source.width;
	input.imageHeight = source.height;
	input.pixelX = 0;
	input.pixelY = 0;
	input.pointerX = 20;
	input.pointerY = 30;
	input.windowWidth = 200;
	input.windowHeight = 150;
	input.labelWidth = 100;
	input.lineHeight = 16;
	input.imageArea = {0, 0, 100, 100};
	input.destination = {10, 20, 80, 40};
	model.Update(input);
	Expect(model.PaintPlan().hex == "#123456FF",
		"sampler reported levels-processed pixels instead of source color");

	jpegview_linux::Image rotated = source;
	Expect(rotated.Rotate(true), "sampler rotated source fixture failed");
	jpegview_linux::Image rotatedPresentation = rotated;
	Expect(rotatedPresentation.ApplyProcessing(levels, false),
		"sampler rotated presentation could not apply gamma");
	input.documentRevision++;
	input.pixelOwner = &rotated;
	input.bgra = &rotated.bgra;
	input.imageWidth = rotated.width;
	input.imageHeight = rotated.height;
	input.pixelX = 0;
	input.pixelY = 1;
	input.destination = {10, 20, 40, 80};
	model.Update(input);
	Expect(model.PaintPlan().hex == "#AABBCCFF",
		"sampler lost source colors or document coordinates after rotation");

	jpegview_linux::Image cropped;
	Expect(rotated.CopyCrop(0, 1, 1, 2, cropped),
		"sampler crop fixture failed");
	jpegview_linux::Image croppedPresentation = cropped;
	Expect(croppedPresentation.ApplyProcessing(levels, false),
		"sampler cropped presentation could not apply gamma");
	input.documentRevision++;
	input.pixelOwner = &cropped;
	input.bgra = &cropped.bgra;
	input.imageWidth = cropped.width;
	input.imageHeight = cropped.height;
	input.pixelX = 0;
	input.pixelY = 0;
	input.destination = {10, 20, 40, 40};
	model.Update(input);
	Expect(model.PaintPlan().hex == "#AABBCCFF",
		"sampler lost source colors or document coordinates after crop");
}

void TestRendererWindowResourceOwnership() {
	using Owner = jpegview_linux::RendererWindowResources<RendererResourceTestWindow,
		RendererResourceTestRenderer, DestroyRendererResourceTestWindow,
		DestroyRendererResourceTestRenderer>;
	gRendererResourceDestructionOrder.clear();
	{
		Owner resources;
		resources.WindowResource() = new RendererResourceTestWindow();
		resources.RendererResource() = new RendererResourceTestRenderer();
		Owner moved(std::move(resources));
		Expect(resources.WindowResource().Get() == nullptr &&
			resources.RendererResource().Get() == nullptr,
			"moving the SDL resource owner should transfer both handles");
		moved.Reset();
		Expect(gRendererResourceDestructionOrder == std::vector<int>({2, 1}),
			"renderer resources must destroy the renderer before its window");
		moved.Reset();
		Expect(gRendererResourceDestructionOrder == std::vector<int>({2, 1}),
			"resetting an empty resource owner must not destroy handles twice");
	}
	Expect(gRendererResourceDestructionOrder == std::vector<int>({2, 1}),
		"resource-owner destruction after explicit reset must be harmless");
}
const TestCase kTests[] = {
	{"viewport-modes-and-geometry", &TestViewportModesAndGeometry},
	{"free-rotation-dialog-controller-tracks-preview-and-apply-ownership", &TestFreeRotationDialogControllerTracksPreviewAndApplyOwnership},
	{"perspective-correction-dialog-controller-tracks-preview-and-apply-ownership", &TestPerspectiveCorrectionDialogControllerTracksPreviewAndApplyOwnership},
	{"perspective-correction-dialog-layout-adapts-to-small-windows", &TestPerspectiveCorrectionDialogLayoutAdaptsToSmallWindows},
	{"viewport-manual-zoom-pan-and-restore", &TestViewportManualZoomPanAndRestore},
	{"pending-viewport-intents-replay-after-dimensions", &TestPendingViewportIntentsReplayAfterDimensions},
	{"zoom-navigator-geometry-and-panning", &TestZoomNavigatorGeometryAndPanning},
	{"magnifying-glass-model-defaults-bounds-and-wheel-directions", &TestMagnifyingGlassModelDefaultsBoundsAndWheelDirections},
	{"magnifying-glass-geometry-native-scaling-and-edge-padding", &TestMagnifyingGlassGeometryMapsNativeTextureAndPadsEdges},
	{"viewport-navigation-resets-transient-zoom", &TestViewportNavigationResetsTransientZoom},
	{"viewport-fit-relative-zoom-mode", &TestViewportFitRelativeZoomMode},
	{"resize-model-aspect-ratio-validation-and-filters", &TestResizeModelAspectRatioValidationAndFilters},
	{"resize-dialog-controller", &TestResizeDialogController},
	{"crop-size-dialog-controller", &TestCropSizeDialogController},
	{"archive-password-dialog-model", &TestArchivePasswordDialogModel},
	{"context-menu-compaction-and-selection", &TestContextMenuCompactionAndSelection},
	{"context-menu-catalog-and-state", &TestContextMenuCatalogAndState},
	{"context-menu-letter-mnemonics", &TestContextMenuMnemonics},
	{"crop-context-menu-commands-and-modes", &TestCropContextMenuCommandsAndModes},
	{"context-menu-column-layout-and-navigation", &TestContextMenuColumnLayoutAndNavigation},
	{"overlay-layout-content-width-and-margins", &TestOverlayLayoutUsesContentWidthAndComfortableMargins},
	{"viewer-chrome-paint-plans", &TestViewerChromePaintPlans},
	{"thumbnail-panel-layout-preload-and-sizing", &TestThumbnailPanelLayoutPreloadAndSizing},
	{"thumbnail-repository-interface-in-memory-implementation", &TestThumbnailRepositoryInterfaceUsesInMemoryImplementation},
	{"thumbnail-pixel-retention-and-texture-window", &TestThumbnailPixelRetentionAndTextureWindow},
	{"thumbnail-cache-scheduling-and-eviction", &TestThumbnailCacheSchedulingAndEviction},
	{"thumbnail-catalog-replacement-identity", &TestThumbnailCatalogReplacementIdentity},
	{"thumbnail-source-identity-replacement-invalidates", &TestThumbnailSourceIdentityReplacementInvalidates},
	{"thumbnail-bulk-eviction-uses-exact-keys", &TestThumbnailBulkEvictionUsesExactKeys},
	{"display-cache-reports-deleted-source-identity", &TestDisplayCacheReportsDeletedSourceIdentity},
	{"decoded-cache-reports-deleted-source-after-decode-failure", &TestDecodedCacheReportsDeletedSourceAfterDecodeFailure},
	{"selected-decode-retries-after-observed-spread-cancellation", &TestSelectedDecodeRetriesAfterObservedSpreadCancellation},
	{"thumbnail-source-notice-routes-through-current-refresh", &TestThumbnailSourceNoticeRoutesThroughCurrentRefresh},
	{"thumbnail-invalid-descriptor-recaptures-recreated-source", &TestThumbnailInvalidDescriptorRecapturesRecreatedSource},
	{"thumbnail-background-preparation", &TestThumbnailBackgroundPreparation},
	{"thumbnail-completion-queue-count-backpressure", &TestThumbnailCompletionQueueCountBackpressure},
	{"thumbnail-completion-byte-backpressure-and-cancellation", &TestThumbnailCompletionByteBackpressureAndCancellation},
	{"thumbnail-oversized-and-allocation-failure", &TestThumbnailOversizedAndAllocationFailure},
	{"thumbnail-shutdown-retires-without-event-loop", &TestThumbnailShutdownRetiresWithoutEventLoop},
	{"thumbnail-retirement-address-reuse-uses-owner-identity", &TestThumbnailRetirementDoesNotDeduplicateReusedAddress},
	{"thumbnail-queue-displacement-returns-scheduler-work", &TestThumbnailQueueDisplacementReturnsSchedulerWork},
	{"thumbnail-file-backed-preparation-and-shutdown", &TestThumbnailFileBackedPreparationAndShutdown},
	{"thumbnail-downsampling-antialiasing", &TestThumbnailDownsamplingAntialiasing},
	{"grayscale-spectrum-calculation-and-scaling", &TestGrayscaleSpectrumCalculationAndScaling},
	{"image-spectrum-worker-generation-deduplication-and-shutdown", &TestImageSpectrumWorkerGenerationDeduplicationAndShutdown},
	{"image-info-formatting", &TestImageInfoFormatting},
	{"system-font-resolution-and-unicode-rendering", &TestSystemFontResolutionAndUnicodeRendering},
	{"playback-scheduler-timing-and-modes", &TestPlaybackSchedulerTimingAndModes},
	{"playback-scheduler-animation-frame-controls", &TestPlaybackSchedulerAnimationFrameControls},
	{"event-loop-invalidation-deadlines-wakeups-and-motion", &TestEventLoopInvalidationDeadlinesWakeupsAndMotion},
	{"modal-event-router-precedence", &TestModalEventRouterPrecedence},
	{"go-to-image-number-model", &TestGoToImageNumberModel},
	{"pixel-color-sampler-bgra-rgba-and-bounds", &TestPixelColorSamplerReadsBgraAsRgba},
	{"pixel-color-sampler-model-update-pin-and-copy", &TestPixelColorSamplerModelUpdatePinAndCopy},
	{"pixel-color-sampler-decode-demand-requires-committed-idle-hover", &TestPixelColorSamplerDecodeDemandRequiresCommittedIdleHover},
	{"pixel-color-sampler-decode-demand-rejects-stationary-held-buttons", &TestPixelColorSamplerDecodeDemandRejectsStationaryHeldButtons},
	{"pixel-color-sampler-source-colors-after-processing-and-edits", &TestPixelColorSamplerUsesSourceColorsAfterProcessingAndGeometryEdits},
	{"renderer-window-resource-ownership", &TestRendererWindowResourceOwnership},
};

} // namespace

const TestSuite& GetViewerModelsSuite() {
	static const TestSuite suite{"viewer_models", kTests, sizeof(kTests) / sizeof(kTests[0])};
	return suite;
}
