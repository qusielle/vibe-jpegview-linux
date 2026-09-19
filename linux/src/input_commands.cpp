#include "input_commands.h"

#include "../../src/JPEGView/resource.h"

namespace jpegview_linux {

int CommandForKey(const SDL_KeyboardEvent& event, bool playbackActive) {
	const Sint32 key = event.keysym.sym;
	const Uint16 modifiers = event.keysym.mod;
	const bool ctrl = (modifiers & 0x00C0u) != 0;
	const bool shift = (modifiers & 0x0003u) != 0;
	const bool alt = (modifiers & 0x0300u) != 0;
	if (alt && !ctrl && !shift && key == SDLK_r) return IDM_SLIDESHOW_RESUME;
	if (alt) return 0;

	if (key == SDLK_ESCAPE) return playbackActive ? IDM_DEFAULT_ESC : IDM_EXIT;
	if (!ctrl && !shift && key == SDLK_q) return IDM_EXIT;
	if (ctrl && !shift && key == SDLK_o) return IDM_OPEN;
	if (ctrl && !shift && key == SDLK_F2) return IDM_SHOW_FILENAME;
	if (ctrl && !shift && key == 'c') return IDM_COPY_FULL;
	if (ctrl && shift && key == 'c') return IDM_COPY_PATH;
	if (ctrl && !shift && key == 'v') return IDM_PASTE;
	if (ctrl && !shift && key == 'p') return IDM_PRINT;
	if (ctrl && !shift && key == 's') return IDM_SAVE_ALLOW_NO_PROMPT;
	if (ctrl && shift && key == 's') return IDM_SAVE_SCREEN;
	if (ctrl && !shift && key == SDLK_r) return IDM_RELOAD;
	if (ctrl && shift && key == SDLK_r) return IDM_CHANGESIZE;
	if (ctrl && shift && key == 'm') return IDM_TOUCH_IMAGE;
	if (ctrl && shift && key == 'e') return IDM_TOUCH_IMAGE_EXIF;
	if (ctrl && !shift && key == 'n') return IDM_SHOW_NAVPANEL;
	if (ctrl && !shift && key == 't') return kCommandToggleThumbnailPanel;
	if (!ctrl && shift && key == 'n') return IDM_SHOW_FILENAME;
	if (!ctrl && !shift && key == SDLK_F2) return IDM_SHOW_FILEINFO;
	if (!ctrl && !shift && key == SDLK_F3) return IDM_TOGGLE_RESAMPLING_QUALITY;
	if (!ctrl && !shift && key == SDLK_F4) return IDM_KEEP_PARAMETERS;
	if (!ctrl && !shift && key == SDLK_F5) return IDM_AUTO_CORRECTION;
	if (!ctrl && !shift && key == SDLK_F6) return IDM_LDC;
	if (!ctrl && !shift && key == 'c') return IDM_SORT_CREATION_DATE;
	if (!ctrl && !shift && key == 'n') return IDM_SORT_NAME;
	if (!ctrl && !shift && key == 'm') return IDM_SORT_MOD_DATE;
	if (!ctrl && !shift && key == 'z') return IDM_SORT_RANDOM;
	if (!ctrl && !shift && key == SDLK_F7) return IDM_LOOP_FOLDER;
	if (!ctrl && !shift && key == SDLK_F8) return IDM_LOOP_RECURSIVELY;
	if (!ctrl && !shift && key == SDLK_F9) return IDM_LOOP_SIBLINGS;
	if (!ctrl && !shift && key == SDLK_DELETE) return IDM_MOVE_TO_RECYCLE_BIN_CONFIRM;
	if (!ctrl && !shift && key == 'w') return IDM_EXPLORE;

	if (!ctrl && !shift && (key == SDLK_RIGHT || key == SDLK_PAGEDOWN)) return IDM_NEXT;
	if (!ctrl && !shift && (key == SDLK_LEFT || key == SDLK_PAGEUP)) return IDM_PREV;
	if (!ctrl && !shift && key == SDLK_HOME) return IDM_FIRST;
	if (!ctrl && !shift && key == SDLK_END) return IDM_LAST;
	if (!ctrl && !shift && key == SDLK_SPACE) return IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS;
	if (!ctrl && !shift && key == SDLK_RETURN) return IDM_FIT_TO_SCREEN;
	if (!ctrl && !shift && key == SDLK_DOWN) return IDM_ROTATE_90;
	if (!ctrl && !shift && key == SDLK_UP) return IDM_ROTATE_270;
	if (ctrl && key == SDLK_DOWN) return IDM_ZOOM_DEC;
	if (ctrl && key == SDLK_UP) return IDM_ZOOM_INC;
	if (!ctrl && !shift && key == SDLK_F11) return IDM_FULL_SCREEN_MODE;
	if (shift && !ctrl && key == SDLK_F11) return IDM_HIDE_TITLE_BAR;
	if (ctrl && !shift && key == SDLK_F11) return IDM_FIT_WINDOW_TO_IMAGE;
	if (!ctrl && !shift && key == SDLK_F12) return IDM_SPAN_SCREENS;
	if (shift && !ctrl && key == SDLK_F12) return IDM_ALWAYS_ON_TOP;
	if (ctrl && !shift && key == SDLK_RETURN) return IDM_FILL_WITH_CROP;
	if (!ctrl && !shift && key == 'r') return IDM_ROTATE_90_LOSSLESS_CONFIRM;
	if (!ctrl && !shift && key == 't') return IDM_ROTATE_270_LOSSLESS_CONFIRM;
	if (!ctrl && !shift && key == SDLK_0) return IDM_FIT_TO_SCREEN;
	if (!ctrl && !shift && key == SDLK_f) return IDM_FULL_SCREEN_MODE;

	const bool plus = key == SDLK_EQUALS || key == SDLK_KP_PLUS;
	if (plus && !ctrl && !alt) return IDM_ZOOM_INC;
	if ((key == SDLK_MINUS || key == SDLK_KP_MINUS) && !ctrl && !alt) return IDM_ZOOM_DEC;
	return 0;
}

} // namespace jpegview_linux
