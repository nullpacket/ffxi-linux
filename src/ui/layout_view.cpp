// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/layout_view.h"

namespace xi::ui
{

ImVec2 UiCanvas::windowOrigin(const dat::UiRecord& w) const
{
    float x = w.x, y = w.y;
    if (!w.noAnchor && (w.anchor & 1))
    {
        x += width - 512;
    }
    if (!w.noAnchor && (w.anchor & 2))
    {
        y += height - 448;
    }
    return toScreen({ x, y });
}

ImVec2 drawLayout(UiPainter& p, const UiCanvas& canvas, const dat::UiLayout& l, const LayoutState& s)
{
    const ImVec2 o = canvas.windowOrigin(l.window);
    const bool framed = s.frame && l.window.w > 0 && l.window.h > 0;
    if (framed)
    {
        p.frame({ o.x + l.window.frameX * canvas.scale, o.y + l.window.frameY * canvas.scale },
                { static_cast<float>(l.window.w), static_cast<float>(l.window.h) });
    }
    for (const auto& sub : l.window.subs)
    {
        if (sub.type == 0)
        {
            // From quad 1: quad 0 is the background, which the skin frame replaces (0x10117700 -> 0x1011fbb0).
            p.sprite(sub.sheet, sub.shape, o, {}, framed ? 1 : 0);
        }
    }
    for (const auto& e : l.elements)
    {
        if (!s.elements)
        {
            break;
        }
        const int state = s.states && e.id < s.states->size() ? (*s.states)[e.id] : 0;
        const auto* sub = e.subFor(state);
        if (!sub)
        {
            continue;
        }
        // Selected: tinted and nudged up-left by one unit (0x10117700).
        const bool   sel = e.id == s.cursor;
        const ImVec2 at(o.x + static_cast<float>(e.x - (sel ? 1 : 0)) * canvas.scale, o.y + static_cast<float>(e.y - (sel ? 1 : 0)) * canvas.scale);
        // Disabled without its own greyed sprite (type 4): dimmed.
        const UiColor mod = state == 5 && sub->type != 4 ? UiColor{ 0x48, 0x48, 0x48, 0x80 } : sel ? UiColor{ 0x80, 0x80, 0x50, 0x80 } : UiColor{};
        p.sprite(sub->sheet, sub->shape, at, mod);
    }
    if (const auto* e = s.cursor > 0 ? l.element(static_cast<uint16_t>(s.cursor)) : nullptr)
    {
        for (const auto& sub : l.window.subs)
        {
            if (sub.type != 6)
            {
                continue;
            }
            std::size_t shape = sub.shape;
            if (shape == 0)
            {
                // Animated: steps through shapes 0, 1, 2... every 3 ticks.
                const auto* sheet = &p.renderer().resources().sheets;
                const auto  it    = sheet->find(sub.sheet);
                const auto  n     = it != sheet->end() ? it->second.sprites.size() : 1;
                shape             = static_cast<std::size_t>(s.tick / 3) % std::max<std::size_t>(1, n);
            }
            const ImVec2 at(o.x + static_cast<float>(e->x + e->cursorX + l.window.cursorX) * canvas.scale, o.y + static_cast<float>(e->y + e->cursorY + l.window.cursorY) * canvas.scale);
            p.sprite(sub.sheet, shape, at);
        }
    }
    return o;
}

int hitTest(const UiCanvas& canvas, const dat::UiLayout& l, ImVec2 pt)
{
    const ImVec2 o = canvas.windowOrigin(l.window);
    for (const auto& e : l.elements)
    {
        if (e.nav[2] == 0xFF && e.nav[3] == 0xFF)
        {
            continue; // not selectable
        }
        const float x = o.x + e.x * canvas.scale, y = o.y + e.y * canvas.scale;
        if (pt.x >= x && pt.y >= y && pt.x < x + e.w * canvas.scale && pt.y < y + e.h * canvas.scale)
        {
            return e.id;
        }
    }
    return 0;
}

} // namespace xi::ui
