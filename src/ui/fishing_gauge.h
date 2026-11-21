// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The fishing minigame's on-screen parts as the game picks them (docs/commands.md "Fishing").
#include <algorithm>

namespace xi::ui
{

// The arrow's effect base in file 3727 (schedulers mai0..maib; CODE 0x10089fb0 by table 0x1035b598): (2 x gold - dir
// + 1) x 3, where dir 0 is a right arrow and 1 a left one. The scheduler is mai[base] when it shows, mai[base + 1] on a
// hit and mai[base + 2] when it goes (a wrong key, or its window over).
constexpr int fishArrowFx(bool gold, int dir)
{
    return (2 * (gold ? 1 : 0) - dir + 1) * 3;
}

// The stamina gauge's fill (CODE 0x102026a0 draws the actor's +0x3a percentage with the party HP gauge 0x101cac80):
// the shown stamina (fishGaugeEase) in whole percent, 0..100.
constexpr int fishGaugePercent(int stamina, int max)
{
    return max > 0 ? std::clamp(stamina * 100 / max, 0, 100) : 0;
}

// The gauge's shown stamina follows the real one (CODE 0x10089ba1, each minigame frame): it moves 0.15 of the way,
// or all of it once that step would be under one point either way.
constexpr int fishGaugeEase(int shown, int stamina)
{
    const float step = static_cast<float>(stamina - shown) * 0.15f;
    return step > -1.0f && step < 1.0f ? stamina : static_cast<int>(static_cast<float>(shown) + step);
}

// The cast's first 3 s (0xb4 ticks in actor +0x1e8, set at CODE 0x10090ba2): Enter / Esc can't reel the line back in
// yet (CODE 0x1008914f).
constexpr int kFishCastLock = 0xB4;

// The hook check's delay when the server sends none in S2C 0x037 +0x4A (CODE 0x10090b79): 900..1500 ticks.
constexpr int fishDefaultHookWait(int roll)
{
    return roll % 0x259 + 0x384;
}

} // namespace xi::ui
