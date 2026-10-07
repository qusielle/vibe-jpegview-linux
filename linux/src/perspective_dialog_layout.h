#pragma once

#include "sdl_abi.h"

#include <array>

namespace jpegview_linux {

struct PerspectiveCorrectionDialogLayout {
	SDL_Rect dialog{};
	std::array<SDL_Rect, 2> sliders{};
	std::array<SDL_Rect, 3> toggles{};
	std::array<SDL_Rect, 2> buttons{};
	SDL_Rect message{};
	int helpY = -1;
	bool compact = false;
	bool veryCompact = false;
};

PerspectiveCorrectionDialogLayout BuildPerspectiveCorrectionDialogLayout(
	int windowWidth, int windowHeight);

} // namespace jpegview_linux
