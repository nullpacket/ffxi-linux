// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/button_label.h"

#include <array>
#include <cstdint>
#include <optional>

namespace xi::ui
{
namespace
{

// A letter of the buttons' small bold italic set in "font font" (the top-left block of the texture, ASCII in rows of
// 13 texels). DATA: each texel rect and its placement are copied from the game's own labels, e.g. windowps 359
// "Global" (G l o b a) and windowps 109 "On-screen damage display" (O n - s d): capitals and tall letters sit at the
// label's top (dy 0), the short ones 2 lower, the hyphen 4 lower with one texel before it. adv: the x step to the
// next letter.
struct Glyph
{
    char     c;
    uint16_t u, v, w, h;
    int8_t   dx, dy, adv;
};

constexpr std::array kGlyphs{
    Glyph{ 'A', 77, 26, 9, 11, 0, 0, 9 },   Glyph{ 'G', 20, 39, 9, 12, 0, 0, 9 }, Glyph{ 'O', 102, 39, 9, 12, 0, 0, 9 },
    Glyph{ 'a', 49, 67, 8, 9, 0, 2, 8 },    Glyph{ 'b', 58, 65, 9, 11, 0, 0, 9 }, Glyph{ 'd', 78, 65, 9, 11, 0, 0, 9 },
    Glyph{ 'l', 35, 78, 6, 11, 0, 0, 6 },   Glyph{ 'n', 56, 80, 9, 9, 0, 2, 9 },  Glyph{ 'o', 66, 80, 9, 9, 0, 2, 9 },
    Glyph{ 's', 106, 80, 9, 9, 0, 2, 9 },   Glyph{ '-', 11, 17, 8, 4, 1, 4, 10 },
};
constexpr int kSpaceAdvance = 5; // INFERRED: the gap between words in two-word labels ("Chat Filters")

const Glyph* glyphOf(char c)
{
    for (const auto& g : kGlyphs)
    {
        if (g.c == c)
        {
            return &g;
        }
    }
    return nullptr;
}

const Glyph* glyphAt(const dat::UiQuad& q)
{
    for (const auto& g : kGlyphs)
    {
        if (g.u == q.u && g.v == q.v && g.w == q.w && g.h == q.h)
        {
            return &g;
        }
    }
    return nullptr;
}

void place(dat::UiQuad& q, const Glyph& g, int x, int y)
{
    const int x0 = x + g.dx, y0 = y + g.dy, x1 = x0 + g.w, y1 = y0 + g.h;
    q.xy = { static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1), static_cast<int16_t>(y0),
             static_cast<int16_t>(x0), static_cast<int16_t>(y1), static_cast<int16_t>(x1), static_cast<int16_t>(y1) };
    q.u = g.u, q.v = g.v, q.w = g.w, q.h = g.h;
}

} // namespace

bool buttonLabelCovers(std::string_view text)
{
    for (const char c : text)
    {
        if (c != ' ' && !glyphOf(c))
        {
            return false;
        }
    }
    return !text.empty();
}

dat::UiSprite buttonLabel(const dat::UiSprite& button, std::string_view text)
{
    if (button.size() < 4 || (button.size() - 2) % 2 != 0 || !buttonLabelCovers(text))
    {
        return {};
    }
    const std::size_t n      = (button.size() - 2) / 2;
    const auto&       shadow = button[2];
    const auto&       letter = button[2 + n];
    if (shadow.texture != "font font" || letter.texture != "font font")
    {
        return {};
    }
    // The label's origin: its first letter's left edge and the top of its tall letters.
    const auto* first = glyphAt(letter);
    const int   x0    = letter.xy[0] - (first ? first->dx : 0);
    const int   y0    = letter.xy[1] - (first ? first->dy : 0);
    const int   sx    = shadow.xy[0] - letter.xy[0], sy = shadow.xy[1] - letter.xy[1];

    dat::UiSprite out(button.begin(), button.begin() + 2);
    dat::UiSprite letters;
    int           x = x0;
    for (const char c : text)
    {
        const auto* g = glyphOf(c);
        if (!g)
        {
            x += kSpaceAdvance;
            continue;
        }
        auto s = shadow;
        place(s, *g, x + sx, y0 + sy);
        out.push_back(s);
        auto l = letter;
        place(l, *g, x, y0);
        letters.push_back(l);
        x += g->adv;
    }
    out.insert(out.end(), letters.begin(), letters.end());
    return out;
}

} // namespace xi::ui
