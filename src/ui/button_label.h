// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/ui.h"

#include <string_view>

// Menu button labels in the game's own lettering (issue #84). The game has no font sheet for the small bold italic
// letters of its buttons ("Gameplay", "Global", ...): every label is a ready-made sprite whose quads cut each letter
// out of the "font font" texture, a shadow pass first and then the letters (e.g. "menu windowps" 359, Global:
// quads 0-1 the button, 2-7 the shadow, 8-13 the letters). This builds a label the game doesn't ship the same way.
namespace xi::ui
{

// `button`: a game button sprite laid out like windowps 359 (two button quads, then N shadow quads and N letter
// quads); its first shadow and letter quads give the colours, blending and the origin / shadow offset. Returns the
// button's two quads plus `text` in the same letters; empty if `button` isn't laid out that way or a character of
// `text` isn't in the glyph table (see button_label.cpp; spaces advance).
dat::UiSprite buttonLabel(const dat::UiSprite& button, std::string_view text);

// True when every character of `text` has a glyph.
bool buttonLabelCovers(std::string_view text);

} // namespace xi::ui
