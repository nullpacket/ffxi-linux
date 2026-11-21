// SPDX-License-Identifier: GPL-3.0-or-later
// The treasure pool's log lines (issue #115; client/loot_text.h): which standard message each S2C 0x0D2 / 0x0D3
// case picks, and (with the user's install) the lines from file 7031.
#include "client/loot_text.h"
#include "dat/dat_index.h"
#include "dat/items.h"
#include "dat/messages.h"
#include "xi/install.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

namespace
{

const xi::dat::DatIndex* install()
{
    static std::unique_ptr<xi::dat::DatIndex> idx = [] {
        const char* root = xi::install::gameRootOrNull();
        if (!root || !std::filesystem::exists(std::filesystem::path(root) / "VTABLE.DAT"))
        {
            return std::unique_ptr<xi::dat::DatIndex>{};
        }
        return std::make_unique<xi::dat::DatIndex>(root);
    }();
    return idx.get();
}

} // namespace

TEST_CASE("loot: the message each treasure pool packet picks", "[loot]")
{
    using namespace xi::client;
    // 0x0D2: a new item found on a monster, in a chest; nothing for the pool re-sent on zone-in or an empty slot.
    auto found = lootFoundNote(17396, 0, false, "Wild Rabbit", false);
    REQUIRE(found);
    CHECK(found->message == 16);
    CHECK(found->target == "Wild Rabbit");
    CHECK_FALSE(found->targetNoArticle);
    CHECK(lootFoundNote(17396, 0, true, "Treasure Chest", false)->message == 218);
    CHECK_FALSE(lootFoundNote(17396, 1, false, "Wild Rabbit", false));
    CHECK_FALSE(lootFoundNote(0, 0, false, "Wild Rabbit", false));

    // 0x0D3: a lot, a pass (nothing), won, the winner could not take it (us / someone else), lost.
    auto lot = lootSolutionNote(0, 17396, "Botone", false, "Bottwo", 512);
    REQUIRE(lot);
    CHECK(lot->message == 17);
    CHECK(lot->strings[3] == "Bottwo");
    CHECK(lot->strings[2] == "512");
    CHECK_FALSE(lootSolutionNote(0, 17396, "Botone", false, "Bottwo", 0xFFFF));
    auto won = lootSolutionNote(1, 17396, "Botone", false, "", 0);
    REQUIRE(won);
    CHECK(won->message == 18);
    CHECK(won->strings[2] == "Botone");
    CHECK(lootSolutionNote(2, 17396, "Botone", true, "", 0xFFFF)->message == 31);
    CHECK(lootSolutionNote(2, 17396, "Botone", false, "", 0xFFFF)->message == 15);
    CHECK(lootSolutionNote(2, 17396, "", false, "", 0)->message == 164); // LSB's treasureLost
    CHECK(lootSolutionNote(3, 17396, "", false, "", 0)->message == 164);
    CHECK_FALSE(lootSolutionNote(1, 0, "Botone", false, "", 0)); // the slot was already empty
}

TEST_CASE("loot: the lines from the install", "[loot][data]")
{
    const auto* idx = install();
    if (!idx)
    {
        SKIP("no game data");
    }
    using namespace xi::client;
    const auto std  = xi::dat::DialogTable::parse(idx->read(7031));
    auto       text = std::make_shared<xi::dat::BattleText>(*idx);
    text->setItems(std::make_shared<const xi::dat::ItemTable>(*idx));
    const std::function<std::string(uint16_t, uint32_t)> lookup = [text](uint16_t t, uint32_t v) {
        return text->dialogName(t, v);
    };
    auto one = [&](const std::optional<LootNote>& n) {
        REQUIRE(n);
        const auto lines = lootLines(std, *n, lookup);
        REQUIRE(lines.size() == 1);
        return lines[0];
    };
    CHECK(one(lootFoundNote(17396, 0, false, "Wild Rabbit", false)) == "You find a little worm on the Wild Rabbit.");
    CHECK(one(lootFoundNote(17396, 0, true, "Treasure Chest", false)) ==
          "You find a little worm in the Treasure Chest.");
    CHECK(one(lootSolutionNote(0, 17396, "Botone", false, "Bottwo", 512)) ==
          "Bottwo's lot for the Little Worm: 512 points.");
    CHECK(one(lootSolutionNote(1, 17396, "Botone", false, "", 0)) == "Botone obtains a little worm.");
    CHECK(one(lootSolutionNote(3, 17396, "", false, "", 0)) == "A little worm was lost.");
}
