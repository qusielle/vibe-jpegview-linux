#include "editing_dialog_renderer.h"

#include "chrome_renderer.h"
#include "text_renderer.h"

namespace jpegview_linux {
namespace {

constexpr int kTextScale = 1;

} // namespace

void EditingDialogRendererAdapter::Fill(const SDL_Rect& rect,
	Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
	if (renderer_ == nullptr) return;
	SDL_SetRenderDrawColor(renderer_, red, green, blue, alpha);
	SDL_RenderFillRect(renderer_, &rect);
}

void EditingDialogRendererAdapter::DrawButton(const DialogButtonPaint& button,
	bool compact) {
	Fill(button.rect, button.hovered ? 52 : 28,
		button.hovered ? 78 : 28, button.hovered ? 108 : 28, 220);
	chromeRenderer_.DrawRect(button.rect, 125, 145, 165);
	textRenderer_.Draw(button.label, button.rect.x + (compact ? 12 : 6),
		button.rect.y + 10, kTextScale, 255, 255, 255);
}

void EditingDialogRendererAdapter::Render(const UnsharpMaskDialogPaint& paint) {
	if (!paint.visible || renderer_ == nullptr) return;
	Fill(paint.dialog, 8, 12, 18, 248);
	chromeRenderer_.DrawRect(paint.dialog, 205, 210, 220);
	textRenderer_.Draw("APPLY UNSHARP MASK", paint.dialog.x + 20,
		paint.dialog.y + 18, kTextScale, 245, 245, 250);
	for (const UnsharpSliderPaint& slider : paint.sliders) {
		textRenderer_.Draw(slider.label, slider.rect.x, slider.rect.y + 2,
			kTextScale, 220, 225, 232);
		textRenderer_.Draw(slider.value, slider.rect.x + 72, slider.rect.y + 2,
			kTextScale, 190, 200, 212);
		const int left = slider.rect.x + 156;
		const int right = slider.rect.x + slider.rect.w - 12;
		const int trackY = slider.rect.y + 25;
		chromeRenderer_.DrawLine(left, trackY, right, trackY, 95, 108, 124);
		const SDL_Rect knob{slider.knobX - 4, trackY - 5, 9, 11};
		Fill(knob, 120, 190, 235, 255);
	}
	for (const DialogButtonPaint& button : paint.buttons) {
		Fill(button.rect, button.hovered ? 66 : 36,
			button.hovered ? 82 : 48, button.hovered ? 104 : 62, 245);
		chromeRenderer_.DrawRect(button.rect, 130, 145, 165);
		const int labelX = button.rect.x +
			(button.rect.w - textRenderer_.TextWidth(button.label, kTextScale)) / 2;
		textRenderer_.Draw(button.label, labelX, button.rect.y + 8,
			kTextScale, 235, 240, 245);
	}
}

void EditingDialogRendererAdapter::Render(const ResizeDialogPaint& paint) {
	if (!paint.visible || renderer_ == nullptr) return;
	Fill(paint.dialog, 12, 12, 12, 232);
	chromeRenderer_.DrawRect(paint.dialog, 190, 190, 190);
	textRenderer_.Draw("RESIZE IMAGE", paint.dialog.x + 20, paint.dialog.y + 16,
		kTextScale, 255, 255, 255);
	textRenderer_.Draw("ORIGINAL SIZE", paint.dialog.x + 20, paint.dialog.y + 43,
		kTextScale, 180, 195, 215);
	textRenderer_.Draw(paint.originalSize, paint.dialog.x + 190,
		paint.dialog.y + 43, kTextScale, 220, 220, 220);
	for (const ResizeFieldPaint& field : paint.fields) {
		Fill(field.rect, 30, 30, 30, 225);
		chromeRenderer_.DrawRect(field.rect, field.focused ? 100 : 75,
			field.focused ? 130 : 75, field.focused ? 165 : 75);
		textRenderer_.Draw(field.label, paint.dialog.x + 20,
			field.rect.y + 9, kTextScale, 205, 215, 230);
		textRenderer_.Draw(field.value, field.rect.x + 8, field.rect.y + 9,
			kTextScale, 255, 255, 255);
		if (!field.suffix.empty()) {
			textRenderer_.Draw(field.suffix, field.rect.x + field.rect.w + 10,
				field.rect.y + 9, kTextScale, 185, 185, 185);
		}
	}
	if (!paint.message.empty()) {
		textRenderer_.Draw(paint.message, paint.dialog.x + 20,
			paint.dialog.y + paint.dialog.h - 82, kTextScale, 235, 180, 130);
	}
	textRenderer_.Draw("TAB: NEXT FIELD   ARROWS: CHANGE FILTER/FIELD   ENTER: APPLY   ESC: CANCEL",
		paint.dialog.x + 20, paint.dialog.y + paint.dialog.h - 62,
		kTextScale, 160, 160, 160);
	for (const DialogButtonPaint& button : paint.buttons) DrawButton(button, true);
}

void EditingDialogRendererAdapter::Render(const FixedCropSizeDialogPaint& paint) {
	if (!paint.visible || renderer_ == nullptr) return;
	Fill(paint.dialog, 12, 12, 12, 232);
	chromeRenderer_.DrawRect(paint.dialog, 190, 190, 190);
	textRenderer_.Draw("SET FIXED CROP SIZE", paint.dialog.x + 20,
		paint.dialog.y + 16, kTextScale, 255, 255, 255);
	for (const CropSizeFieldPaint& field : paint.fields) {
		Fill(field.rect, 30, 30, 30, 225);
		chromeRenderer_.DrawRect(field.rect, field.focused ? 100 : 75,
			field.focused ? 130 : 75, field.focused ? 165 : 75);
		textRenderer_.Draw(field.label, paint.dialog.x + 20,
			field.rect.y + 10, kTextScale, 205, 215, 230);
		textRenderer_.Draw(field.value, field.rect.x + 8,
			field.rect.y + 10, kTextScale, 255, 255, 255);
	}
	for (const CropSizeUnitPaint& unit : paint.units) {
		Fill(unit.rect, unit.selected ? 45 : 28, unit.selected ? 68 : 28,
			unit.selected ? 92 : 28, 220);
		chromeRenderer_.DrawRect(unit.rect, unit.selected ? 120 : 75,
			unit.selected ? 150 : 75, unit.selected ? 190 : 75);
		textRenderer_.Draw(unit.label, unit.rect.x + 10, unit.rect.y + 9,
			kTextScale, 235, 235, 235);
	}
	textRenderer_.Draw("Screen-pixel sizes follow zoom; image-pixel sizes use source pixels.",
		paint.dialog.x + 20, paint.dialog.y + 201, kTextScale, 160, 160, 160);
	if (!paint.message.empty()) {
		textRenderer_.Draw(paint.message, paint.dialog.x + 20,
			paint.dialog.y + 222, kTextScale, 235, 180, 130);
	}
	for (const DialogButtonPaint& button : paint.buttons) DrawButton(button, true);
}

void EditingDialogRendererAdapter::Render(const BatchCopyDialogPaint& paint) {
	if (!paint.visible || renderer_ == nullptr) return;
	Fill(paint.dialog, 12, 12, 12, 224);
	chromeRenderer_.DrawRect(paint.dialog, 190, 190, 190);
	textRenderer_.Draw("BATCH RENAME/COPY OF FILES", paint.dialog.x + 18,
		paint.dialog.y + 14, kTextScale);
	textRenderer_.Draw(paint.directory, paint.dialog.x + 18,
		paint.dialog.y + 38, kTextScale, 170, 170, 170);

	Fill(paint.list, 25, 25, 25, 215);
	chromeRenderer_.DrawRect(paint.list, 75, 75, 75);
	textRenderer_.Draw("SEL", paint.list.x + 8, paint.list.y + 7,
		kTextScale, 170, 170, 170);
	textRenderer_.Draw("OLD NAME", paint.list.x + 42, paint.list.y + 7,
		kTextScale, 170, 170, 170);
	textRenderer_.Draw("DATE", paint.list.x + 245, paint.list.y + 7,
		kTextScale, 170, 170, 170);
	textRenderer_.Draw("NEW NAME (>> COPY)", paint.list.x + 380, paint.list.y + 7,
		kTextScale, 170, 170, 170);
	for (std::size_t rowIndex = 0; rowIndex < paint.rows.size(); ++rowIndex) {
		const BatchCopyRowPaint& row = paint.rows[rowIndex];
		const int rowTop = paint.list.y + 22 + static_cast<int>(rowIndex) * 24;
		if (row.current) {
			Fill(SDL_Rect{paint.list.x + 2, rowTop, paint.list.w - 4, 22},
				45, 82, 120, 205);
		}
		const Uint8 selectionShade = row.selected ? 255 : 150;
		textRenderer_.Draw(row.selected ? "[X]" : "[ ]", paint.list.x + 8,
			rowTop + 6, kTextScale, selectionShade, selectionShade, selectionShade);
		textRenderer_.Draw(row.source, paint.list.x + 42, rowTop + 6,
			kTextScale, 235, 235, 235);
		textRenderer_.Draw(row.date, paint.list.x + 245, rowTop + 6,
			kTextScale, 210, 210, 210);
		textRenderer_.Draw(row.destination, paint.list.x + 380, rowTop + 6,
			kTextScale, row.copy ? 255 : 220, 220, row.copy ? 150 : 220);
	}

	textRenderer_.Draw("PLACEHOLDERS", paint.rightX, paint.dialog.y + 68,
		kTextScale, 190, 210, 235);
	textRenderer_.Draw("%x  consecutive number   %Nx  padded number", paint.rightX,
		paint.dialog.y + 92, kTextScale, 205, 205, 205);
	textRenderer_.Draw("%n  number from filename  %f  original filename", paint.rightX,
		paint.dialog.y + 110, kTextScale, 205, 205, 205);
	textRenderer_.Draw("%F  filename without ext  %e  extension", paint.rightX,
		paint.dialog.y + 128, kTextScale, 205, 205, 205);
	textRenderer_.Draw("%d %m %y  day/month/year   %2y  short year", paint.rightX,
		paint.dialog.y + 146, kTextScale, 205, 205, 205);
	textRenderer_.Draw("%h %min  hour/minute       %M %3M  month text", paint.rightX,
		paint.dialog.y + 164, kTextScale, 205, 205, 205);
	textRenderer_.Draw("%pictures%  HOME/Pictures or XDG_PICTURES_DIR", paint.rightX,
		paint.dialog.y + 182, kTextScale, 205, 205, 205);
	textRenderer_.Draw("TARGET PATTERN (use / for folders)", paint.rightX,
		paint.dialog.y + 216, kTextScale, 190, 210, 235);
	Fill(paint.pattern, 30, 30, 30, 220);
	chromeRenderer_.DrawRect(paint.pattern, paint.patternFocused ? 100 : 75,
		paint.patternFocused ? 130 : 75, paint.patternFocused ? 165 : 75);
	textRenderer_.Draw(paint.patternText, paint.pattern.x + 8,
		paint.pattern.y + 10, kTextScale);
	if (!paint.message.empty()) {
		textRenderer_.Draw(paint.message, paint.rightX,
			paint.dialog.y + paint.dialog.h - 88, kTextScale, 235, 180, 130);
	}
	for (const DialogButtonPaint& button : paint.buttons) DrawButton(button, false);
}

} // namespace jpegview_linux
