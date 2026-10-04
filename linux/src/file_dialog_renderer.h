#pragma once

#include "sdl_abi.h"
#include "transparency_pattern.h"

#include <string>
#include <vector>

namespace jpegview_linux {

class ChromeRendererAdapter;
class TextRenderer;

struct FileDialogRenderRow {
	bool selected = false;
	bool recent = false;
	bool archive = false;
	bool directory = false;
	std::string label;
	std::string parent;
	std::string filename;
	std::string sizeText;
	std::string rightText;
};

struct FileDialogPreviewPaint {
	SDL_Rect pane{};
	SDL_Rect imageArea{};
	SDL_Texture* texture = nullptr;
	int width = 0;
	int height = 0;
	bool hasTransparency = false;
	TransparencyPattern transparencyPattern = TransparencyPattern::Black;
	std::string message;
	std::string filename;
	std::string dimensions;
	int filenameX = 0;
	int dimensionsX = 0;
	int footerY = 0;
};

// Renderer-thread presentation snapshot for the custom file dialog. It owns
// strings and row state; the preview texture is borrowed only for this render.
struct FileDialogRenderSnapshot {
	SDL_Rect dialog{};
	SDL_Rect removeRecentButton{};
	SDL_Rect browseTab{};
	SDL_Rect recentsTab{};
	SDL_Rect sortButton{};
	SDL_Rect input{};
	SDL_Rect list{};
	SDL_Rect listContent{};
	SDL_Rect scrollbarTrack{};
	SDL_Rect scrollbarThumb{};
	SDL_Rect divider{};
	SDL_Rect resizeHandle{};
	bool visible = false;
	bool showTabs = false;
	bool recentTab = false;
	bool showRemoveRecent = false;
	bool recentSelectionAvailable = false;
	bool removeRecentHovered = false;
	bool showSort = false;
	bool sortByName = true;
	bool showPreview = false;
	bool recentListEmpty = false;
	bool scrollbarScrollable = false;
	bool scrollbarDragging = false;
	bool scrollbarHovered = false;
	int mouseX = 0;
	int mouseY = 0;
	std::string title;
	std::string location;
	std::string fieldTitle;
	std::string inputText;
	std::string message;
	std::string shortcutHint;
	std::string emptyRecentMessage;
	FileDialogPreviewPaint preview;
	std::vector<FileDialogRenderRow> rows;
};

// SDL paint adapter for the file dialog. File listing, selection, geometry
// ownership, and actions stay with Viewer and FileDialogModel.
class FileDialogRendererAdapter {
public:
	FileDialogRendererAdapter(TextRenderer& textRenderer,
		ChromeRendererAdapter& chromeRenderer) noexcept
		: textRenderer_(textRenderer), chromeRenderer_(chromeRenderer) {}

	void SetRenderer(SDL_Renderer* renderer) noexcept { renderer_ = renderer; }
	void Render(const FileDialogRenderSnapshot& snapshot);

private:
	void Fill(const SDL_Rect& rect, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha = 230);
	void DrawPreview(const FileDialogPreviewPaint& preview);
	void DrawTransparencyBackground(const SDL_Rect& imageRect,
		const SDL_Rect& clipRect, TransparencyPattern pattern);
	std::string ClipText(const std::string& value, int maximumWidth) const;

	SDL_Renderer* renderer_ = nullptr;
	TextRenderer& textRenderer_;
	ChromeRendererAdapter& chromeRenderer_;
};

} // namespace jpegview_linux
