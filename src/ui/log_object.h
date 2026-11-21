// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace xi::ui
{
// The game's log window objects as addons see them (issue #91; docs/ui-format-notes.md §10.2 "The log objects").
// Two globals hold them: 0x105eee1c log window 1, 0x105eee20 log window 2 (read from the draw-message-window code at
// 0x1012df6d, which sets each one's +0x59 "a line arrived" byte). The object's frame (0x101625f0) works in pixels:
//   +0x34 i32  the shown height: lines x the line height. 0x10162470 adds one line's height (INFERRED: per new line; capped at
//              max lines); while nothing keeps the log open the frame lowers it with the elapsed time toward min lines
//              (never below), and 0x101623c0 takes a line off and rounds up to whole lines. Lines shown (+0x31) =
//              ceil(+0x34 / +0x54). Writing 0 there (fancychat, every frame) leaves no line, so the window shrinks
//              away; the next new line grows it again.
//   +0x4e / +0x50 i16  max / min lines;  +0x54 i16  the line height (16).
//   +0x40 / +0x42 i16  the text rect's left / top (window rect from 0x10118ea0, top + 3); fancychat reads +0x42 and
//              takes a top above its screen height - 19 - 15 that keeps falling as the native log being opened.
// The window's height eases toward its lines (0x10162bd0: 8 px a frame when more than 16 away), so a line that grows
// the log for one frame before an addon zeroes +0x34 again barely shows in the game.
inline constexpr int kLogLineHeight = 16;

// Lines shown for a +0x34 height (0x101625f0: ceil(+0x34 / +0x54), never negative).
constexpr int logLinesForHeight(int height, int lineHeight = kLogLineHeight)
{
    if (height <= 0 || lineHeight <= 0)
    {
        return 0;
    }
    return (height + lineHeight - 1) / lineHeight;
}

// The window's height for its lines (0x101625f0 -> 0x10162bd0: the authored 134 at 8 lines, 16 a line).
constexpr int logWindowHeight(int lines)
{
    return 6 + kLogLineHeight * std::max(0, lines);
}

// How far +0x42 (the text rect's top) sits above the bottom of the menu canvas: the window's bottom is 16 above it
// ("menu logwindo" 16,298 366x134 on the 448 canvas), its top the window's height above that, the text 3 below the
// top. A shrunk-away log is 19 above the bottom (fancychat's "UISizeY - 19"). Published in screen pixels from the
// screen's bottom at one menu unit a pixel (INFERRED: retail answers in its menu resolution, which fancychat compares
// with the viewport height, so they agree only at 1:1; the client keeps that agreement at any UI scale).
constexpr int logTextTopFromBottom(int lines)
{
    return 16 + logWindowHeight(lines) - 3;
}

// An addon's hold on a log window's +0x34. The client cannot tell an addon writing the same value every frame from one
// that wrote it once, so the hold lasts while the addon keeps writing a value other than the client's own (each
// frame's write is seen), and while the client's own value equals it (nothing to tell apart). Once the client's value
// differs and no write comes for `kLogHoldGrace`, the client's own height shows again; the addon's state closing
// (unload) releases it at once.
inline constexpr std::chrono::milliseconds kLogHoldGrace{ 250 };

struct LogHold
{
    std::optional<int>                    height; // the addon's +0x34, while held
    std::chrono::steady_clock::time_point at{};   // when it was last written (or matched the client's)

    void write(int value, std::chrono::steady_clock::time_point now)
    {
        height = value;
        at     = now;
    }
    void release()
    {
        height.reset();
    }
    // The height to draw this frame, given the client's own (`natural`).
    int shown(int natural, std::chrono::steady_clock::time_point now)
    {
        if (!height)
        {
            return natural;
        }
        if (*height == natural)
        {
            at = now;
        }
        else if (now - at > kLogHoldGrace)
        {
            height.reset();
            return natural;
        }
        return *height;
    }
};

} // namespace xi::ui
