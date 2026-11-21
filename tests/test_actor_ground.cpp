// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/actor_ground.h"
#include "dat/dat_index.h"
#include "dat/zone.h"
#include "gfx/zone_collision.h"
#include "xi/install.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using xi::client::actorGround;

namespace
{

// Floors at the given heights (render space, y up) under every x / z: the highest one <= maxY.
auto floors(std::vector<float> heights)
{
    return [heights = std::move(heights)](float maxY) {
        std::optional<float> best;
        for (const float h : heights)
        {
            if (h <= maxY && (!best || h > *best))
            {
                best = h;
            }
        }
        return best;
    };
}

} // namespace

TEST_CASE("actor ground (#93): the game's probe takes the highest floor from 2 above to 40 below")
{
    // On the floor, a little into it, or above it: onto the floor.
    CHECK(actorGround(0.0f, floors({ 0.0f })).value() == Approx(0.0f));
    CHECK(actorGround(-1.9f, floors({ 0.0f })).value() == Approx(0.0f));
    CHECK(actorGround(0.5f, floors({ 0.0f })).value() == Approx(0.0f));
    CHECK(actorGround(39.0f, floors({ 0.0f })).value() == Approx(0.0f));
    // The hit nearest the segment's top (CODE 0x1016a8a0): a floor up to 2 above wins, one higher up does not.
    CHECK(actorGround(0.0f, floors({ 0.0f, 1.5f })).value() == Approx(1.5f));
    CHECK(actorGround(0.0f, floors({ 0.0f, 3.0f })).value() == Approx(0.0f));
    // Zone 149 (Davoi) reaches 1.5 up (CODE 0x10178d00).
    CHECK(xi::client::actorGroundAbove(149) == Approx(1.5f));
    CHECK(xi::client::actorGroundAbove(240) == Approx(2.0f));
    CHECK(actorGround(0.0f, floors({ 0.0f, 1.8f }), xi::client::actorGroundAbove(149)).value() == Approx(0.0f));
    // No floor within 40 below, or none at all: the server height stays.
    CHECK_FALSE(actorGround(41.0f, floors({ 0.0f })).has_value());
    CHECK_FALSE(actorGround(5.0f, floors({})).has_value());
}

TEST_CASE("actor ground (#93): Port Windurst's Synergy furnace stands on the ground, not at LSB's -4.5")
{
    const char* root = xi::install::gameRootOrNull();
    if (!root || !std::filesystem::exists(std::filesystem::path(root) / "VTABLE.DAT"))
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::DatIndex idx(root);
    const auto              keys = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
    const auto              zone = xi::dat::loadZone(idx, xi::dat::zoneModelId(240), keys);
    xi::gfx::ZoneCollision  col;
    col.addCollision(zone.collision);
    col.build();
    // LSB data/zones/port_windurst/npcs.yaml 17760459 / 17760457: game (-179, -4.5, 95) and (-176, -4.5, 96), game y
    // down, so render y 4.5 over the 4.0 ground (the top of a low wall nearby).
    for (const auto& [x, z] : { std::pair{ -179.0f, 95.0f }, std::pair{ -176.0f, 96.0f } })
    {
        auto highest = [&](float maxY) { return col.groundHeight(x, -z, maxY); };
        const auto g = actorGround(4.5f, highest, xi::client::actorGroundAbove(240));
        REQUIRE(g.has_value());
        CHECK(*g == Approx(4.0f).margin(0.01f));
    }
}
