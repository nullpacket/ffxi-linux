// SPDX-License-Identifier: GPL-3.0-or-later
#include "ashita/input_map.h"

namespace xi::ashita
{

uint32_t vkFromSdl(SDL_Keycode key)
{
    if (key >= SDLK_A && key <= SDLK_Z)
    {
        return 0x41 + static_cast<uint32_t>(key - SDLK_A); // 'A'..'Z'
    }
    if (key >= SDLK_0 && key <= SDLK_9)
    {
        return 0x30 + static_cast<uint32_t>(key - SDLK_0);
    }
    if (key >= SDLK_F1 && key <= SDLK_F12)
    {
        return 0x70 + static_cast<uint32_t>(key - SDLK_F1); // VK_F1..VK_F12
    }
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
    {
        return 0x61 + static_cast<uint32_t>(key - SDLK_KP_1); // VK_NUMPAD1..9
    }
    switch (key)
    {
        case SDLK_BACKSPACE: return 0x08;
        case SDLK_TAB: return 0x09;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return 0x0D;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT: return kVkShift;
        case SDLK_LCTRL:
        case SDLK_RCTRL: return 0x11; // VK_CONTROL
        case SDLK_LALT:
        case SDLK_RALT: return 0x12; // VK_MENU
        case SDLK_PAUSE: return 0x13;
        case SDLK_CAPSLOCK: return 0x14;
        case SDLK_ESCAPE: return 0x1B;
        case SDLK_SPACE: return 0x20;
        case SDLK_PAGEUP: return 0x21;
        case SDLK_PAGEDOWN: return 0x22;
        case SDLK_END: return 0x23;
        case SDLK_HOME: return 0x24;
        case SDLK_LEFT: return 0x25;
        case SDLK_UP: return 0x26;
        case SDLK_RIGHT: return 0x27;
        case SDLK_DOWN: return 0x28;
        case SDLK_PRINTSCREEN: return 0x2C;
        case SDLK_INSERT: return 0x2D;
        case SDLK_DELETE: return 0x2E;
        case SDLK_LGUI: return 0x5B;
        case SDLK_RGUI: return 0x5C;
        case SDLK_APPLICATION: return 0x5D;
        case SDLK_KP_0: return 0x60;
        case SDLK_KP_MULTIPLY: return 0x6A;
        case SDLK_KP_PLUS: return 0x6B;
        case SDLK_KP_MINUS: return 0x6D;
        case SDLK_KP_PERIOD: return 0x6E;
        case SDLK_KP_DIVIDE: return 0x6F;
        case SDLK_NUMLOCKCLEAR: return 0x90;
        case SDLK_SCROLLLOCK: return 0x91;
        case SDLK_SEMICOLON: return 0xBA;
        case SDLK_EQUALS: return 0xBB;
        case SDLK_COMMA: return 0xBC;
        case SDLK_MINUS: return 0xBD;
        case SDLK_PERIOD: return 0xBE;
        case SDLK_SLASH: return 0xBF;
        case SDLK_GRAVE: return 0xC0;
        case SDLK_LEFTBRACKET: return 0xDB;
        case SDLK_BACKSLASH: return 0xDC;
        case SDLK_RIGHTBRACKET: return 0xDD;
        case SDLK_APOSTROPHE: return 0xDE;
        default: return 0;
    }
}

uint32_t buttonMessage(uint8_t button, bool down)
{
    switch (button)
    {
        case SDL_BUTTON_LEFT: return down ? kWmLButtonDown : kWmLButtonUp;
        case SDL_BUTTON_RIGHT: return down ? kWmRButtonDown : kWmRButtonUp;
        case SDL_BUTTON_MIDDLE: return down ? kWmMButtonDown : kWmMButtonUp;
        default: return 0;
    }
}

} // namespace xi::ashita
