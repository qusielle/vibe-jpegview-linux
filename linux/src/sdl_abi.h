#pragma once

// The Linux viewer deliberately uses only the small, stable part of SDL2's
// public ABI that it needs.  This keeps the source buildable on a clean
// Ubuntu installation without requiring SDL2 development headers.  The
// runtime library is still bundled into the AppImage by package-appimage.sh.

#include <cstddef>
#include <cstdint>

using Uint8 = std::uint8_t;
using Uint16 = std::uint16_t;
using Uint32 = std::uint32_t;
using Sint32 = std::int32_t;

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

struct SDL_Rect {
	int x;
	int y;
	int w;
	int h;
};

struct SDL_Keysym {
	Sint32 scancode;
	Sint32 sym;
	Uint16 mod;
	Uint32 unused;
};

struct SDL_KeyboardEvent {
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	Uint8 state;
	Uint8 repeat;
	Uint8 padding2;
	Uint8 padding3;
	SDL_Keysym keysym;
};

struct SDL_WindowEvent {
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	Uint8 event;
	Uint8 padding1;
	Uint8 padding2;
	Uint8 padding3;
	Sint32 data1;
	Sint32 data2;
};

struct SDL_MouseMotionEvent {
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	Uint32 which;
	Uint32 state;
	Sint32 x;
	Sint32 y;
	Sint32 xrel;
	Sint32 yrel;
};

struct SDL_MouseButtonEvent {
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	Uint32 which;
	Uint8 button;
	Uint8 state;
	Uint8 clicks;
	Uint8 padding1;
	Sint32 x;
	Sint32 y;
};

struct SDL_TextInputEvent {
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	char text[32];
};

// SDL 2.0.18 and later append precise coordinates to this event.  The viewer
// only consumes the SDL 2.0.10-compatible prefix through direction.
struct SDL_MouseWheelEvent {
	Uint32 type;
	Uint32 timestamp;
	Uint32 windowID;
	Uint32 which;
	Sint32 x;
	Sint32 y;
	Uint32 direction;
};

struct SDL_DropEvent {
	Uint32 type;
	Uint32 timestamp;
	char* file;
	Uint32 windowID;
};

union SDL_Event {
	Uint32 type;
	SDL_KeyboardEvent key;
	SDL_WindowEvent window;
	SDL_MouseMotionEvent motion;
	SDL_MouseButtonEvent button;
	SDL_MouseWheelEvent wheel;
	SDL_TextInputEvent text;
	SDL_DropEvent drop;
	Uint8 padding[56];
};

// These are the layouts exposed by SDL 2.0.10.  Keep the checks here because
// a stale or accidentally SDL3-derived declaration can otherwise compile and
// only fail when SDL writes an event into the union.
static_assert(sizeof(SDL_Keysym) == 16, "unexpected SDL_Keysym layout");
static_assert(sizeof(SDL_KeyboardEvent) == 32, "unexpected SDL_KeyboardEvent layout");
static_assert(sizeof(SDL_WindowEvent) == 24, "unexpected SDL_WindowEvent layout");
static_assert(sizeof(SDL_MouseMotionEvent) == 36, "unexpected SDL_MouseMotionEvent layout");
static_assert(sizeof(SDL_MouseButtonEvent) == 28, "unexpected SDL_MouseButtonEvent layout");
static_assert(sizeof(SDL_MouseWheelEvent) == 28, "unexpected SDL_MouseWheelEvent layout");
static_assert(sizeof(SDL_TextInputEvent) == 44, "unexpected SDL_TextInputEvent layout");
static_assert(offsetof(SDL_DropEvent, file) == sizeof(Uint32) * 2, "unexpected SDL drop offset");
static_assert(offsetof(SDL_KeyboardEvent, keysym) == 16, "unexpected SDL keyboard offset");
static_assert(offsetof(SDL_MouseMotionEvent, x) == 20, "unexpected SDL motion offset");
static_assert(offsetof(SDL_MouseWheelEvent, y) == 20, "unexpected SDL wheel offset");
static_assert(sizeof(SDL_Event) == 56, "unexpected SDL_Event layout");

enum : Uint32 {
	SDL_INIT_VIDEO = 0x00000020u,
	SDL_WINDOW_SHOWN = 0x00000004u,
	SDL_WINDOW_RESIZABLE = 0x00000020u,
	SDL_WINDOW_MAXIMIZED = 0x00000040u,
	SDL_WINDOW_ALLOW_HIGHDPI = 0x00002000u,
	SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001u,
	SDL_RENDERER_ACCELERATED = 0x00000002u,
	SDL_RENDERER_PRESENTVSYNC = 0x00000004u,
	SDL_PIXELFORMAT_ARGB8888 = 372645892u,
	SDL_TEXTUREACCESS_STREAMING = 1u,
	SDL_BLENDMODE_BLEND = 1u,
};

enum : Uint32 {
	SDL_QUIT = 0x100u,
	SDL_WINDOWEVENT = 0x200u,
	SDL_KEYDOWN = 0x300u,
	SDL_MOUSEMOTION = 0x400u,
	SDL_MOUSEBUTTONDOWN = 0x401u,
	SDL_MOUSEBUTTONUP = 0x402u,
	SDL_MOUSEWHEEL = 0x403u,
	SDL_TEXTINPUT = 0x303u,
	SDL_DROPFILE = 0x1000u,
	SDL_DROPTEXT = 0x1001u,
	SDL_DROPBEGIN = 0x1002u,
	SDL_DROPCOMPLETE = 0x1003u,
};

enum : Uint8 {
	SDL_WINDOWEVENT_RESIZED = 0x05u,
	SDL_WINDOWEVENT_SIZE_CHANGED = 0x06u,
	SDL_WINDOWEVENT_MAXIMIZED = 0x08u,
	SDL_WINDOWEVENT_RESTORED = 0x09u,
	SDL_BUTTON_LEFT = 1u,
	SDL_BUTTON_RIGHT = 3u,
};

enum : Sint32 {
	SDLK_ESCAPE = 27,
	SDLK_BACKSPACE = 8,
	SDLK_DELETE = 127,
	SDLK_RETURN = 13,
	SDLK_SPACE = 32,
	SDLK_0 = '0',
	SDLK_1 = '1',
	SDLK_f = 'f',
	SDLK_o = 'o',
	SDLK_q = 'q',
	SDLK_r = 'r',
	SDLK_EQUALS = '=',
	SDLK_MINUS = '-',
	SDLK_LEFT = 1073741904,
	SDLK_RIGHT = 1073741903,
	SDLK_UP = 1073741906,
	SDLK_DOWN = 1073741905,
	SDLK_KP_PLUS = 1073741911,
	SDLK_KP_MINUS = 1073741910,
	SDLK_PAGEUP = 1073741899,
	SDLK_PAGEDOWN = 1073741902,
	SDLK_HOME = 1073741898,
	SDLK_END = 1073741901,
	SDLK_F7 = 1073741888,
	SDLK_F8 = 1073741889,
	SDLK_F9 = 1073741890,
	SDLK_F2 = 1073741883,
	SDLK_F3 = 1073741884,
	SDLK_F4 = 1073741885,
	SDLK_F5 = 1073741886,
	SDLK_F6 = 1073741887,
	SDLK_F11 = 1073741892,
	SDLK_F12 = 1073741893,
};

extern "C" {
int SDL_Init(Uint32 flags);
void SDL_Quit();
const char* SDL_GetError();
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window* window);
int SDL_SetWindowFullscreen(SDL_Window* window, Uint32 flags);
void SDL_MaximizeWindow(SDL_Window* window);
void SDL_RestoreWindow(SDL_Window* window);
void SDL_GetWindowSize(SDL_Window* window, int* w, int* h);
void SDL_SetWindowSize(SDL_Window* window, int w, int h);
void SDL_SetWindowTitle(SDL_Window* window, const char* title);
void SDL_SetWindowBordered(SDL_Window* window, int bordered);
int SDL_SetClipboardText(const char* text);
void SDL_StartTextInput();
void SDL_StopTextInput();
void SDL_free(void* memory);
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags);
int SDL_SetHint(const char* name, const char* value);
void SDL_DestroyRenderer(SDL_Renderer* renderer);
SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h);
void SDL_DestroyTexture(SDL_Texture* texture);
int SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch);
int SDL_SetTextureBlendMode(SDL_Texture* texture, int blendMode);
int SDL_SetTextureAlphaMod(SDL_Texture* texture, Uint8 alpha);
int SDL_RenderClear(SDL_Renderer* renderer);
int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst);
int SDL_RenderDrawLine(SDL_Renderer* renderer, int x1, int y1, int x2, int y2);
int SDL_RenderDrawRect(SDL_Renderer* renderer, const SDL_Rect* rect);
int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect);
void SDL_RenderPresent(SDL_Renderer* renderer);
int SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_PollEvent(SDL_Event* event);
void SDL_Delay(Uint32 ms);
Uint32 SDL_GetTicks();
}
