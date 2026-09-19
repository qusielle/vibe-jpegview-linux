#pragma once

#include "sdl_abi.h"

namespace jpegview_linux {

// Linux-only viewer command IDs are negative so they cannot collide with the
// command vocabulary shared with the Windows resource file.
constexpr int kCommandToggleThumbnailPanel = -4;

// Returns the original Windows command ID for a supported SDL key event.
// Escape depends on whether playback is active: it stops playback first and
// exits only when there is nothing to stop.
int CommandForKey(const SDL_KeyboardEvent& event, bool playbackActive);

} // namespace jpegview_linux
