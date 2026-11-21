// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// SDL input as the Win32 window messages Ashita addons see in their `key` and `mouse` events (WNDPROC numbers).
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

#include <cstdint>

namespace xi::ashita
{

inline constexpr uint32_t kVkShift = 0x10; // VK_SHIFT: either Shift key, as WM_KEYDOWN reports it

inline constexpr uint32_t kWmMouseMove   = 0x200;
inline constexpr uint32_t kWmLButtonDown = 0x201;
inline constexpr uint32_t kWmLButtonUp   = 0x202;
inline constexpr uint32_t kWmRButtonDown = 0x204;
inline constexpr uint32_t kWmRButtonUp   = 0x205;
inline constexpr uint32_t kWmMButtonDown = 0x207;
inline constexpr uint32_t kWmMButtonUp   = 0x208;
inline constexpr uint32_t kWmMouseWheel  = 0x20A;
inline constexpr int      kWheelDelta    = 120; // WHEEL_DELTA: one notch

// The Windows virtual-key code of an SDL key (the US layout's VK_OEM_* for punctuation); 0 when Windows has none.
// Left / right Shift, Ctrl and Alt are the generic VK_SHIFT / VK_CONTROL / VK_MENU, as WM_KEYDOWN gives them.
uint32_t vkFromSdl(SDL_Keycode key);

// The WM_*BUTTON* message of an SDL mouse button (left / right / middle); 0 for the others.
uint32_t buttonMessage(uint8_t button, bool down);

} // namespace xi::ashita
