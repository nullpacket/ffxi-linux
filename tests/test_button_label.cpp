// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/button_label.h"

#include "dat/dat_index.h"
#include "xi/install.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

// Labels in the buttons' own letters (issue #84).
namespace
{
const xi::dat::DatIndex* install()
{
    static std::unique_ptr<xi::dat::DatIndex> idx = [] {
        const char* root = xi::install::gameRootOrNull();
        return root ? std::make_unique<xi::dat::DatIndex>(root) : nullptr;
    }();
    return idx.get();
}

bool sameQuad(const xi::dat::UiQuad& a, const xi::dat::UiQuad& b)
{
    return a.xy == b.xy && a.u == b.u && a.v == b.v && a.w == b.w && a.h == b.h && a.flags == b.flags && a.rgba == b.rgba &&
           a.blend == b.blend && a.texture == b.texture;
}
} // namespace

TEST_CASE("a label the game ships is rebuilt quad for quad", "[button-label]")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const uint32_t files[] = { xi::dat::kUiMenuFile };
    const auto     ui      = xi::dat::UiResources::load(*install(), files);
    const auto*    global  = ui.sprite("menu windowps", 359);
    REQUIRE(global);
    REQUIRE(global->size() == 14);
    const auto built = xi::ui::buttonLabel(*global, "Global");
    REQUIRE(built.size() == global->size());
    for (std::size_t i = 0; i < built.size(); ++i)
    {
        INFO("quad " << i);
        CHECK(sameQuad(built[i], (*global)[i]));
    }
}

TEST_CASE("Add-ons uses letters cut the way the game's labels cut them", "[button-label]")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const uint32_t files[] = { xi::dat::kUiMenuFile };
    const auto     ui      = xi::dat::UiResources::load(*install(), files);
    const auto*    global  = ui.sprite("menu windowps", 359);
    REQUIRE(global);
    const auto label = xi::ui::buttonLabel(*global, "Add-ons");
    REQUIRE(label.size() == 2 + 2 * 7); // the button, 7 shadows, 7 letters
    // Every letter's texel rect is one the game's own labels use ("On-screen damage display" is windowps 109).
    for (std::size_t i = 9; i < label.size(); ++i)
    {
        bool found = false;
        for (const char* sheet : { "menu windowps", "menu keytops3" })
        {
            const auto it = ui.sheets.find(sheet);
            REQUIRE(it != ui.sheets.end());
            for (const auto& s : it->second.sprites)
            {
                for (const auto& q : s)
                {
                    found = found || (q.texture == "font font" && q.u == label[i].u && q.v == label[i].v && q.w == label[i].w && q.h == label[i].h);
                }
            }
        }
        INFO("letter quad " << i);
        CHECK(found);
    }
    // Left to right, inside the 88-wide button.
    CHECK(label[9].xy[0] == 5);
    CHECK(label.back().xy[2] < 88);
    CHECK(xi::ui::buttonLabelCovers("Add-ons"));
    CHECK_FALSE(xi::ui::buttonLabelCovers("Add-ons!"));
}

// Issue #102: the auto-translate window "menu fep" marks its row with its own cursor sub, a bar, not the menus' hand.
TEST_CASE("the auto-translate window's cursor is a bar on the row", "[button-label][autotranslate]")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const uint32_t files[] = { xi::dat::kUiMenuFile };
    const auto     ui      = xi::dat::UiResources::load(*install(), files);
    const auto     it      = ui.layouts.find("menu fep");
    REQUIRE(it != ui.layouts.end());
    const xi::dat::UiSub* cursor = nullptr;
    for (const auto& sub : it->second.window.subs)
    {
        if (sub.type == 6)
        {
            cursor = &sub;
        }
    }
    REQUIRE(cursor);
    CHECK(cursor->sheet == "menu fep");
    CHECK(cursor->shape == 2);
    const auto* bar = ui.sprite(cursor->sheet, cursor->shape);
    REQUIRE(bar);
    REQUIRE(bar->size() == 1);
    CHECK((*bar)[0].xy[2] - (*bar)[0].xy[0] == 92);
    CHECK((*bar)[0].xy[5] - (*bar)[0].xy[1] == 16);
    CHECK((*bar)[0].texture == "menu gauge");
    for (int k = 0; k < 7; ++k)
    {
        const auto* row = it->second.element(static_cast<uint16_t>(k + 2));
        REQUIRE(row);
        CHECK(row->x == 3);
        CHECK(row->y == 20 + 16 * k);
    }
}
