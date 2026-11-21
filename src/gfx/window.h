// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

struct SDL_Window;

namespace xi::gfx
{

// Creates the Vulkan window. `display`: "" = let the system choose, "primary", "secondary" (first non-primary
// display) or a 0-based display index. Wayland does not let clients place windows, so when a display is requested
// on a Wayland session SDL runs through XWayland (x11 driver), where placement works. Call before SDL_Init.
void prepareVideoDriver(const std::string& display);
// takeFocus = false: the window neither activates when shown nor accepts keyboard focus (development/test runs that must
// not steal the user's keyboard and mouse). Also forced by the environment variable XI_NO_FOCUS=1.
// mode: FFXI's window mode setting (registry 0034): 0 fullscreen (exclusive, the display mode closest to width x height),
// 1 windowed, 2 fullscreen windowed (borderless, desktop resolution), 3 borderless window.
// XI_WINDOW_SLOT ("tl", "tr", "bl", "br" or 0..3) -> 0..3, else -1. With a slot, a windowed client fills that quarter of
// its display (frame included) instead of using width x height.
int windowSlot(const char* name);
// The slot named for `character` in a list such as XI_WINDOW_SLOTS="Alice=tl,Bob=tr" (case-insensitive), or "".
std::string windowSlotFor(const char* slots, const std::string& character);
SDL_Window* createWindow(const char* title, int width, int height, const std::string& display, bool takeFocus = true, int mode = 1);

} // namespace xi::gfx
