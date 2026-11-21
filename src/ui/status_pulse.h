// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace xi::ui
{
// Status icons about to wear off (the HUD strip, CODE 0x10227ad0; docs/ui-format-notes.md §9.4). With the status icon
// timers option on (cfg 195), an icon whose time left, rounded up to whole seconds, is at most 15 is drawn with its
// colour dimmed and brightened on a shared triangle wave instead of the plain 0x80808080: alpha stays 0x80 and R = G =
// B run 0x40 (half) .. 0x80 (full) and back.

inline constexpr long kStatusPulseSeconds = 15;

// The window keeps one counter (+0x28c, CODE 0x10227983..0x102279dd): it starts at 40 and drops by the frame's 1/60 s
// ticks; below 0 it starts again at 40. `ticks` here is the time in 1/60 s ticks; the counter is 40 - (ticks mod 40).
inline float statusPulseCounter(double ticks)
{
    const double t = ticks - 40.0 * static_cast<double>(static_cast<long long>(ticks / 40.0));
    return 40.0f - static_cast<float>(t);
}

// The colour level for a counter value: the triangle x = 40 - c above 20, else c (0..20); p = x * 0.05 (+0x288);
// level = 0x40 - (int)(p * -64) (0x10227b52..0x10227b7a, float maths and truncation as the game does).
inline uint8_t statusPulseLevel(float counter)
{
    const float x = counter > 20.0f ? 40.0f - counter : counter;
    const float p = x * 0.05f;
    return static_cast<uint8_t>(0x40 - static_cast<int>(p * -64.0f));
}

// Seconds left as the game counts them: ceil((expiry - now) / 1 s), from the 1/60 s difference (+ 59, / 60).
inline long statusSecondsLeftCeil(long long sixtiethsLeft)
{
    return static_cast<long>((sixtiethsLeft + 59) / 60);
}
} // namespace xi::ui
