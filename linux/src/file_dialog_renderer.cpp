#include "file_dialog_renderer.h"

#include "chrome_renderer.h"
#include "file_dialog_model.h"
#include "text_renderer.h"

#include <algorithm>

namespace jpegview_linux {
namespace {

constexpr int kTextScale = 1;
constexpr int kRowHeight = 26;

std::size_t NextUtf8Boundary(const std::string& value, std::size_t position) {
	if (position >= value.size()) return value.size();
	++position;
	while (position < value.size() &&
		(static_cast<unsigned char>(value[position]) & 0xc0u) == 0x80u) ++position;
	return position;
}

} // namespace

void FileDialogRendererAdapter::Fill(const SDL_Rect& rect,
	Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
	if (renderer_ == nullptr) return;
	SDL_SetRenderDrawColor(renderer_, red, green, blue, alpha);
	SDL_RenderFillRect(renderer_, &rect);
}

std::string FileDialogRendererAdapter::ClipText(const std::string& value,
	int maximumWidth) const {
	if (maximumWidth <= 0) return {};
	if (textRenderer_.TextWidth(value, kTextScale) <= maximumWidth) return value;
	const std::string ellipsis = "...";
	if (textRenderer_.TextWidth(ellipsis, kTextScale) > maximumWidth) return {};
	std::size_t end = 0;
	for (std::size_t next = NextUtf8Boundary(value, end); next > end;
		next = NextUtf8Boundary(value, end)) {
		if (textRenderer_.TextWidth(value.substr(0, next) + ellipsis, kTextScale) > maximumWidth) break;
		end = next;
		if (end == value.size()) break;
	}
	return value.substr(0, end) + ellipsis;
}

void FileDialogRendererAdapter::DrawTransparencyBackground(const SDL_Rect& imageRect,
	const SDL_Rect& clipRect, TransparencyPattern pattern) {
	if (renderer_ == nullptr) return;
	const int visibleLeft = std::max(imageRect.x, clipRect.x);
	const int visibleTop = std::max(imageRect.y, clipRect.y);
	const int visibleRight = std::min(imageRect.x + imageRect.w, clipRect.x + clipRect.w);
	const int visibleBottom = std::min(imageRect.y + imageRect.h, clipRect.y + clipRect.h);
	if (visibleRight <= visibleLeft || visibleBottom <= visibleTop) return;

	if (pattern != TransparencyPattern::Checkerboard) {
		const TransparencyPatternColor color = TransparencyPatternTileColor(pattern, 0, 0);
		Fill(SDL_Rect{visibleLeft, visibleTop, visibleRight - visibleLeft,
			visibleBottom - visibleTop}, color.red, color.green, color.blue, 255);
		return;
	}

	const int cellSize = kTransparencyCheckerCellSize;
	const int firstTileX = std::max(0, (visibleLeft - imageRect.x) / cellSize);
	const int firstTileY = std::max(0, (visibleTop - imageRect.y) / cellSize);
	for (int tileY = firstTileY; imageRect.y + tileY * cellSize < visibleBottom; ++tileY) {
		const int tileTop = imageRect.y + tileY * cellSize;
		const int top = std::max(visibleTop, tileTop);
		const int bottom = std::min(visibleBottom, tileTop + cellSize);
		for (int tileX = firstTileX; imageRect.x + tileX * cellSize < visibleRight; ++tileX) {
			const int tileLeft = imageRect.x + tileX * cellSize;
			const int left = std::max(visibleLeft, tileLeft);
			const int right = std::min(visibleRight, tileLeft + cellSize);
			const TransparencyPatternColor color = TransparencyPatternTileColor(pattern, tileX, tileY);
			Fill(SDL_Rect{left, top, right - left, bottom - top},
				color.red, color.green, color.blue, 255);
		}
	}
}

void FileDialogRendererAdapter::DrawPreview(const FileDialogPreviewPaint& preview) {
	Fill(preview.pane, 25, 25, 25, 210);
	chromeRenderer_.DrawRect(preview.pane, 75, 75, 75);
	textRenderer_.Draw("Preview", preview.pane.x + 8, preview.pane.y + 6,
		kTextScale, 190, 205, 220);
	if (preview.texture != nullptr && preview.width > 0 && preview.height > 0) {
		const double scale = std::min({1.0,
			static_cast<double>(preview.imageArea.w) / preview.width,
			static_cast<double>(preview.imageArea.h) / preview.height});
		const int width = std::max(1, static_cast<int>(preview.width * scale));
		const int height = std::max(1, static_cast<int>(preview.height * scale));
		const SDL_Rect destination{preview.imageArea.x + (preview.imageArea.w - width) / 2,
			preview.imageArea.y + (preview.imageArea.h - height) / 2, width, height};
		if (preview.hasTransparency) {
			DrawTransparencyBackground(destination, preview.imageArea,
				preview.transparencyPattern);
		}
		if (renderer_ != nullptr) SDL_RenderCopy(renderer_, preview.texture, nullptr, &destination);
	} else if (!preview.message.empty()) {
		const int textWidth = textRenderer_.TextWidth(preview.message, kTextScale);
		textRenderer_.Draw(ClipText(preview.message, preview.imageArea.w - 12),
			preview.imageArea.x + std::max(6, (preview.imageArea.w - textWidth) / 2),
			preview.imageArea.y + std::max(0, (preview.imageArea.h - 12) / 2),
			kTextScale, 165, 165, 165);
	}
	if (!preview.filename.empty()) {
		textRenderer_.Draw(preview.filename, preview.filenameX, preview.footerY,
			kTextScale, 165, 175, 185);
		if (!preview.dimensions.empty()) {
			textRenderer_.Draw(preview.dimensions, preview.dimensionsX, preview.footerY,
				kTextScale, 165, 175, 185);
		}
	}
}

void FileDialogRendererAdapter::Render(const FileDialogRenderSnapshot& snapshot) {
	if (!snapshot.visible || renderer_ == nullptr) return;
	Fill(snapshot.dialog, 12, 12, 12, 220);
	chromeRenderer_.DrawRect(snapshot.dialog, 190, 190, 190);
	textRenderer_.Draw(snapshot.title, snapshot.dialog.x + 18, snapshot.dialog.y + 14, kTextScale);
	if (snapshot.showTabs) {
		for (const bool recent : {false, true}) {
			const SDL_Rect& rect = recent ? snapshot.recentsTab : snapshot.browseTab;
			const bool active = snapshot.recentTab == recent;
			Fill(rect, active ? 45 : 30, active ? 72 : 30, active ? 104 : 30, 230);
			chromeRenderer_.DrawRect(rect, active ? 100 : 75,
				active ? 130 : 75, active ? 165 : 75);
			const std::string label = recent ? "Recents" : "Browse";
			const int labelX = rect.x + std::max(4,
				(rect.w - textRenderer_.TextWidth(label, kTextScale)) / 2);
			textRenderer_.Draw(label, labelX, rect.y + 7, kTextScale,
				active ? 235 : 175, active ? 240 : 185, active ? 250 : 195);
		}
	}
	textRenderer_.Draw(snapshot.location, snapshot.dialog.x + 18,
		snapshot.dialog.y + 42, kTextScale, 170, 170, 170);
	if (snapshot.showRemoveRecent) {
		const bool enabled = snapshot.recentSelectionAvailable;
		const bool hovered = enabled && snapshot.removeRecentHovered;
		Fill(snapshot.removeRecentButton, enabled ? (hovered ? 52 : 32) : 24,
			enabled ? (hovered ? 78 : 38) : 24,
			enabled ? (hovered ? 108 : 52) : 24, 230);
		chromeRenderer_.DrawRect(snapshot.removeRecentButton, enabled ? 115 : 65,
			enabled ? 135 : 65, enabled ? 155 : 65);
		const int labelX = snapshot.removeRecentButton.x +
			(snapshot.removeRecentButton.w - textRenderer_.TextWidth("Remove", kTextScale)) / 2;
		textRenderer_.Draw("Remove", labelX, snapshot.removeRecentButton.y + 7,
			kTextScale, enabled ? 225 : 115, enabled ? 230 : 115, enabled ? 238 : 115);
	}
	textRenderer_.Draw(snapshot.fieldTitle, snapshot.dialog.x + 18,
		snapshot.dialog.y + 68, kTextScale, 190, 190, 190);
	if (snapshot.showSort) {
		Fill(snapshot.sortButton, 36, 36, 36, 230);
		chromeRenderer_.DrawRect(snapshot.sortButton, 100, 130, 165);
		const std::string sortLabel = snapshot.sortByName ? "Sort: Name" : "Sort: Mod.date";
		const int labelX = snapshot.sortButton.x + std::max(6,
			(snapshot.sortButton.w - textRenderer_.TextWidth(sortLabel, kTextScale)) / 2);
		textRenderer_.Draw(sortLabel, labelX, snapshot.sortButton.y + 3,
			kTextScale, 210, 220, 230);
	}
	Fill(snapshot.input, 30, 30, 30, 220);
	chromeRenderer_.DrawRect(snapshot.input, 100, 130, 165);
	textRenderer_.Draw(snapshot.inputText, snapshot.input.x + 10,
		snapshot.input.y + 6, kTextScale);

	Fill(snapshot.list, 25, 25, 25, 210);
	chromeRenderer_.DrawRect(snapshot.list, 75, 75, 75);
	for (std::size_t rowIndex = 0; rowIndex < snapshot.rows.size(); ++rowIndex) {
		const FileDialogRenderRow& row = snapshot.rows[rowIndex];
		const int rowTop = snapshot.list.y + static_cast<int>(rowIndex) * kRowHeight;
		if (row.selected) {
			Fill(SDL_Rect{snapshot.listContent.x + 2, rowTop + 1,
				snapshot.listContent.w - 4, 24}, 45, 82, 120, 205);
		}
		if (row.recent) {
			const int leftX = snapshot.listContent.x + 10;
			const int pathWidth = std::max(1, (snapshot.listContent.w - 30) / 2);
			const int sizeWidth = textRenderer_.TextWidth(row.sizeText, kTextScale);
			const int rightEdge = snapshot.listContent.x + snapshot.listContent.w - 10;
			const int sizeX = rightEdge - sizeWidth;
			const int filenameRight = row.sizeText.empty() ? rightEdge : sizeX - 8;
			const int filenameWidth = std::max(1,
				filenameRight - (leftX + pathWidth + 8));
			const std::string parent = ClipText(row.parent, pathWidth);
			const std::string filename = ClipText(row.filename, filenameWidth);
			const int filenameX = filenameRight - textRenderer_.TextWidth(filename, kTextScale);
			textRenderer_.Draw(parent, leftX, rowTop + 5, kTextScale,
				row.archive ? 210 : 165, row.archive ? 170 : 175,
				row.archive ? 105 : 190);
			textRenderer_.Draw(filename, filenameX, rowTop + 5, kTextScale,
				row.archive ? 255 : 235, row.archive ? 205 : 235,
				row.archive ? 125 : 235);
			if (!row.sizeText.empty()) {
				textRenderer_.Draw(row.sizeText, sizeX, rowTop + 5,
					kTextScale, 165, 180, 200);
			}
		} else {
			const int rightTextWidth = textRenderer_.TextWidth(row.rightText, kTextScale);
			const int rightTextX = snapshot.listContent.x + snapshot.listContent.w - 10 - rightTextWidth;
			const int labelWidth = row.rightText.empty() ? snapshot.listContent.w - 20 :
				std::max(1, rightTextX - (snapshot.listContent.x + 10) - 12);
			const Uint8 red = row.archive ? 255 : row.directory ? 185 : 235;
			const Uint8 green = row.archive ? 205 : row.directory ? 205 : 235;
			const Uint8 blue = row.archive ? 125 : 235;
			textRenderer_.Draw(ClipText(row.label, labelWidth), snapshot.listContent.x + 10,
				rowTop + 5, kTextScale, red, green, blue);
			if (!row.rightText.empty()) {
				textRenderer_.Draw(row.rightText, rightTextX, rowTop + 5,
					kTextScale, 155, 175, 195);
			}
		}
	}
	if (snapshot.recentListEmpty) {
		textRenderer_.Draw(snapshot.emptyRecentMessage, snapshot.listContent.x + 12,
			snapshot.list.y + 10, kTextScale, 165, 175, 190);
	}
	Fill(snapshot.scrollbarTrack, 34, 34, 34, 230);
	chromeRenderer_.DrawRect(snapshot.scrollbarTrack, 63, 63, 63);
	const bool thumbHovered = snapshot.scrollbarHovered;
	const Uint8 thumbShade = !snapshot.scrollbarScrollable ? 82 :
		(snapshot.scrollbarDragging ? 185 : thumbHovered ? 158 : 128);
	Fill(snapshot.scrollbarThumb, thumbShade, thumbShade, thumbShade, 255);
	const Uint8 thumbBorder = static_cast<Uint8>(std::min(220,
		static_cast<int>(thumbShade) + 35));
	chromeRenderer_.DrawRect(snapshot.scrollbarThumb, thumbBorder,
		thumbBorder, thumbBorder);
	if (snapshot.showPreview) {
		DrawPreview(snapshot.preview);
		const int centerX = snapshot.divider.x + snapshot.divider.w / 2;
		const int centerY = snapshot.divider.y + snapshot.divider.h / 2;
		chromeRenderer_.DrawLine(centerX, snapshot.divider.y + 8,
			centerX, snapshot.divider.y + snapshot.divider.h - 9, 74, 84, 96);
		for (int offset = -6; offset <= 6; offset += 6) {
			chromeRenderer_.DrawLine(centerX - 2, centerY + offset,
				centerX + 2, centerY + offset, 145, 160, 178);
		}
	}
	if (!snapshot.message.empty()) {
		textRenderer_.Draw(snapshot.message, snapshot.dialog.x + 18,
			snapshot.dialog.y + snapshot.dialog.h - 60, kTextScale, 235, 150, 120);
	}
	textRenderer_.Draw(snapshot.shortcutHint, snapshot.dialog.x + 18,
		snapshot.dialog.y + snapshot.dialog.h - 34, kTextScale, 170, 170, 170);
	for (int offset = 5; offset <= 13; offset += 4) {
		chromeRenderer_.DrawLine(snapshot.resizeHandle.x + offset,
			snapshot.resizeHandle.y + snapshot.resizeHandle.h - 2,
			snapshot.resizeHandle.x + snapshot.resizeHandle.w - 2,
			snapshot.resizeHandle.y + offset, 135, 145, 155);
	}
}

} // namespace jpegview_linux
