// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/ui.h"
#include "ui/ui_draw.h"

// Places and draws a window layout (dat::UiLayout) the way the client does (docs/dat-format.md §14.2–14.3).
namespace xi::ui
{

// The interface canvas: the screen divided by the UI scale, in authored units.
struct UiCanvas
{
    float width  = 640;
    float height = 480;
    float scale  = 1;   // screen pixels per authored unit

    // Screen position of a window's origin. Layouts are authored for a 512x448 canvas: a right-anchored window
    // moves with the right edge (x + W - 512), a bottom-anchored one with the bottom edge (0x10119320).
    [[nodiscard]] ImVec2 windowOrigin(const dat::UiRecord& w) const;
    [[nodiscard]] ImVec2 toScreen(ImVec2 authored) const
    {
        return { authored.x * scale, authored.y * scale };
    }
};

struct LayoutState
{
    int   cursor   = 0;     // element id under the pointing hand (0 = none)
    int   tick     = 0;     // animation tick (the hand steps every 3 ticks)
    bool  frame    = true;  // draw the skin frame
    bool  elements = true;  // draw the elements' sprites (off: window only, for lists drawn by the client)
    // Optional per-element visual state (0 normal, 5 disabled); index = element id.
    const std::vector<int>* states = nullptr;
};

// Draws the frame, the window's own sprites, every element's sprite for its state and the cursor.
// Returns the window origin in screen pixels.
ImVec2 drawLayout(UiPainter& p, const UiCanvas& canvas, const dat::UiLayout& l, const LayoutState& s);

// Element id at a screen point, 0 if none.
int hitTest(const UiCanvas& canvas, const dat::UiLayout& l, ImVec2 screen);

} // namespace xi::ui
