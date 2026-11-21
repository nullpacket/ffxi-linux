// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/window.h"

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace xi::gfx
{

void prepareVideoDriver(const std::string& display)
{
    const char* session = std::getenv("XDG_SESSION_TYPE");
    if (!display.empty() && session && std::strcmp(session, "wayland") == 0 && std::getenv("DISPLAY"))
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    }
}

int windowSlot(const char* name)
{
    if (!name || !*name)
    {
        return -1;
    }
    const std::string n(name);
    if (n == "tl" || n == "0") return 0;
    if (n == "tr" || n == "1") return 1;
    if (n == "bl" || n == "2") return 2;
    if (n == "br" || n == "3") return 3;
    spdlog::warn("XI_WINDOW_SLOT '{}' is not tl / tr / bl / br", n);
    return -1;
}

std::string windowSlotFor(const char* slots, const std::string& character)
{
    // "Alice=tl,Bob=tr,...": the slot of `character` (case-insensitive), or "".
    if (!slots || character.empty())
    {
        return {};
    }
    std::string list(slots);
    size_t      at = 0;
    while (at <= list.size())
    {
        const size_t end  = std::min(list.find(',', at), list.size());
        const std::string item = list.substr(at, end - at);
        if (const size_t eq = item.find('='); eq != std::string::npos && item.size() - eq > 1)
        {
            const std::string who = item.substr(0, eq);
            if (who.size() == character.size() &&
                std::equal(who.begin(), who.end(), character.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
            {
                return item.substr(eq + 1);
            }
        }
        at = end + 1;
    }
    return {};
}

SDL_Window* createWindow(const char* title, int width, int height, const std::string& display, bool takeFocus, int mode)
{
    if (const char* nf = std::getenv("XI_NO_FOCUS"); nf && std::strcmp(nf, "0") != 0)
    {
        takeFocus = false;
    }
    if (!takeFocus)
    {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    SDL_DisplayID target = 0;
    if (!display.empty())
    {
        int            count    = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        const auto     primary  = SDL_GetPrimaryDisplay();
        for (int i = 0; i < count; ++i)
        {
            SDL_Rect r{};
            SDL_GetDisplayBounds(displays[i], &r);
            spdlog::info("display {}: '{}' {}x{} at {},{}{}", i, SDL_GetDisplayName(displays[i]), r.w, r.h, r.x, r.y, displays[i] == primary ? " (primary)" : "");
        }
        if (display == "primary")
        {
            target = primary;
        }
        else if (display == "secondary")
        {
            for (int i = 0; i < count && !target; ++i)
            {
                if (displays[i] != primary)
                {
                    target = displays[i];
                }
            }
        }
        else
        {
            const int idx = std::atoi(display.c_str());
            if (idx >= 0 && idx < count)
            {
                target = displays[idx];
            }
        }
        SDL_free(displays);
        if (!target)
        {
            spdlog::warn("display '{}' not found; using the default", display);
        }
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, mode == 1);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, mode == 3);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    if (!takeFocus)
    {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
        spdlog::info("window will not take keyboard/mouse focus");
    }
    if (target)
    {
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(target));
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(target));
        SDL_Rect r{};
        SDL_GetDisplayBounds(target, &r);
        spdlog::info("window on display '{}' ({}x{} at {},{}), video driver {}", SDL_GetDisplayName(target), r.w, r.h, r.x, r.y, SDL_GetCurrentVideoDriver());
    }
    // XI_WINDOW_SLOT = tl / tr / bl / br: a windowed client fills that quarter of the display (frame included), so up to
    // four test clients side by side do not overlap.
    const int slot = windowSlot(std::getenv("XI_WINDOW_SLOT"));
    SDL_Rect  quarter{};
    if (slot >= 0 && mode != 0 && mode != 2)
    {
        const SDL_DisplayID d = target ? target : SDL_GetPrimaryDisplay();
        SDL_Rect            r{};
        if (!SDL_GetDisplayUsableBounds(d, &r))
        {
            SDL_GetDisplayBounds(d, &r);
        }
        quarter = SDL_Rect{ r.x + (slot & 1) * (r.w / 2), r.y + (slot >> 1) * (r.h / 2), r.w / 2, r.h / 2 };
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, quarter.x);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, quarter.y);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, quarter.w);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, quarter.h);
    }
    SDL_Window* w = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!w)
    {
        throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    }
    if (quarter.w > 0)
    {
        // Positions and sizes are the client area's; take the frame (title bar, borders) out of the quarter.
        SDL_SyncWindow(w);
        int top = 0, left = 0, bottom = 0, right = 0;
        if (!SDL_GetWindowBordersSize(w, &top, &left, &bottom, &right) || top + bottom == 0)
        {
            top = 32; // XWayland may report no frame before the window is mapped; KDE's title bar is about this tall
        }
        SDL_SetWindowSize(w, std::max(320, quarter.w - left - right), std::max(200, quarter.h - top - bottom));
        SDL_SetWindowPosition(w, quarter.x + left, quarter.y + top);
        SDL_SyncWindow(w);
        spdlog::info("window slot {}: {}x{} at {},{} (frame {} {} {} {})", slot, quarter.w - left - right, quarter.h - top - bottom,
                     quarter.x + left, quarter.y + top, top, left, bottom, right);
    }
    if (mode == 0 || mode == 2)
    {
        // 0: exclusive at the closest display mode; 2: borderless at the desktop resolution.
        SDL_DisplayMode closest{};
        const SDL_DisplayID d = target ? target : SDL_GetDisplayForWindow(w);
        if (mode == 0 && SDL_GetClosestFullscreenDisplayMode(d, width, height, 0.0f, true, &closest))
        {
            SDL_SetWindowFullscreenMode(w, &closest);
        }
        else
        {
            SDL_SetWindowFullscreenMode(w, nullptr);
        }
        SDL_SetWindowFullscreen(w, true);
        SDL_SyncWindow(w);
        spdlog::info("window mode {}: fullscreen{}", mode, mode == 0 ? fmt::format(" {}x{}", closest.w, closest.h) : " (desktop)");
    }
    return w;
}

} // namespace xi::gfx
