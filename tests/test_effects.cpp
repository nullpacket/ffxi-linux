// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/install.h"
#include "client/effect_ops.h"
#include "client/effects.h"
#include "dat/dat_index.h"
#include "dat/effect.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>

namespace
{
const xi::dat::DatIndex* fxInstall()
{
    static std::unique_ptr<xi::dat::DatIndex> idx = [] {
        const char* root = xi::install::gameRootOrNull();
        return root ? std::make_unique<xi::dat::DatIndex>(root) : nullptr;
    }();
    return idx.get();
}
} // namespace

TEST_CASE("effects: the mesh combiner clamps each texture stage, as Direct3D does (issue #83)")
{
    using Fx = xi::client::EffectSystem;
    // A D3m vertex colour of c8 c8 c8 80 (the common one, §M.2) under element colour 80 40 10 80 on a white texel.
    const glm::vec4 vtx(0xc8 / 255.0f, 0xc8 / 255.0f, 0xc8 / 255.0f, 0x80 / 255.0f);
    const glm::vec4 elem(0x80 / 255.0f, 0x40 / 255.0f, 0x10 / 255.0f, 0x80 / 255.0f);
    const auto [c, st] = Fx::fxStages(vtx, elem, true);
    const glm::vec4 out = Fx::fxCombine(c, glm::vec4(1.0f), st);
    // Stage 0 saturates at white (2 x 0.78), so stage 1 keeps the element's hue: about (1.0, 0.5, 0.13).
    CHECK(out.r == Catch::Approx(1.0f));
    CHECK(out.g == Catch::Approx(0x40 * 2 / 255.0f).margin(0.01f));
    CHECK(out.b == Catch::Approx(0x10 * 2 / 255.0f).margin(0.01f));
    CHECK(out.a == Catch::Approx(1.0f));
    // The single clamp at the end it replaces washed it toward white: vtx x elem x 4 = (1.0, 0.79, 0.2).
    const glm::vec4 once = glm::clamp(glm::vec4(glm::vec3(vtx) * glm::vec3(elem) * 4.0f, 1.0f), 0.0f, 1.0f);
    CHECK(once.g > out.g + 0.25f);
    // At the neutral vertex colour (0x80, the Ghrah orb's meshes) both agree: the element colour x 2 x the texel.
    const glm::vec4 n(0x80 / 255.0f);
    const auto [nc, nst] = Fx::fxStages(n, elem, true);
    const glm::vec4 half(0.5f, 0.5f, 0.5f, 0.25f);
    const glm::vec4 a = Fx::fxCombine(nc, half, nst);
    CHECK(a.r == Catch::Approx(0x80 / 255.0f * 2 * 0.5f * 0x80 / 255.0f * 2).margin(0.01f));
    CHECK(a.a == Catch::Approx(0.25f * 0x80 / 255.0f * 2 * 0x80 / 255.0f * 4).margin(0.01f));
    // Untextured: one stage, MODULATE2X / MODULATE4X with the element colour; blend 0x44 makes the alpha 2X.
    const auto [uc, ust] = Fx::fxStages(vtx, elem, false);
    CHECK(ust == glm::vec4(1.0f));
    CHECK(Fx::fxCombine(uc, glm::vec4(1.0f), ust).g == Catch::Approx(vtx.g * elem.g * 2.0f));
    CHECK(Fx::fxStages(vtx, elem, false, true).first.a == Catch::Approx(vtx.a * elem.a * 2.0f));
    // TFACTOR alpha: truncated to a byte; with a D3m in blend 0x44, 0x7F and above become 0xFF. The Ghrah orb's
    // core and shell (blend 0x44) peak at alpha 0.4999997 x 255 = 127: fully opaque in their bright phase.
    CHECK(Fx::fxElementAlpha(127.49f, true) == 1.0f);
    CHECK(Fx::fxElementAlpha(127.49f, false) == Catch::Approx(127.0f / 255.0f));
    CHECK(Fx::fxElementAlpha(126.9f, true) == Catch::Approx(126.0f / 255.0f));
    CHECK(Fx::fxElementAlpha(300.0f, false) == 1.0f);
}

TEST_CASE("effects: a dismounted chocobo / mount leaves as its coff scheduler paces it")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    // 39275 (rented chocobo): ch1 for 100 ticks, run, 12 yalms / s for 72 ticks from tick 102, faded over ticks 150..174.
    const auto c = xi::dat::leavePlan(xi::dat::EffectFile(*fxInstall(), 39275));
    REQUIRE(c);
    CHECK(c->getDown);
    CHECK(c->runAt == Catch::Approx(100.0f / 60.0f));
    CHECK(c->moveAt == Catch::Approx(102.0f / 60.0f));
    CHECK(c->moveFor == Catch::Approx(1.2f));
    CHECK(c->speed == Catch::Approx(12.0f));
    CHECK(c->fadeAt == Catch::Approx(2.5f));
    CHECK(c->end() == Catch::Approx(174.0f / 60.0f));
    // 102705 (the first mount): no ch1, runs at once at 10 yalms / s.
    const auto m = xi::dat::leavePlan(xi::dat::EffectFile(*fxInstall(), 102705));
    REQUIRE(m);
    CHECK_FALSE(m->getDown);
    CHECK(m->runAt == 0.0f);
    CHECK(m->speed == Catch::Approx(10.0f));
}

TEST_CASE("effects: lookups search each enclosing block's subtree (Lower Jeuno lamp _l00: ligh/evst finds lt08/lton)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::EffectFile f(*fxInstall(), 345);
    int                       checked = 0;
    for (const auto& r : f.resources())
    {
        if (r.type == xi::dat::FxType::Scheduler && r.name == xi::dat::fxName("evst") && f.underBlock(r.block, xi::dat::fxName("_l00")))
        {
            const auto* lton = f.find(xi::dat::FxType::Scheduler, xi::dat::fxName("lton"), r.block);
            REQUIRE(lton);
            CHECK(f.underBlock(lton->block, xi::dat::fxName("_l00")));
            CHECK(f.underBlock(lton->block, xi::dat::fxName("lt08")));
            ++checked;
        }
    }
    CHECK(checked == 1);
}

TEST_CASE("effects: file 0 hit1 (the melee hit spark) spawns textured particles at the target")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        if (id == 1)
        {
            return xi::client::FxActor{ .pos = { 0, 0, 0 }, .yaw = 0.0f, .height = 1.8f, .point = {} };
        }
        if (id == 2)
        {
            return xi::client::FxActor{ .pos = { 2, 0, 0 }, .yaw = 3.14159f, .height = 1.8f, .point = {} };
        }
        return std::nullopt;
    };
    REQUIRE(fx.play(0, "hit1", 1, 2));
    std::size_t most = 0;
    for (int frame = 0; frame < 30; ++frame)
    {
        fx.update(1.0f / 60.0f, actors);
        most = std::max(most, fx.particleCount());
    }
    CHECK(most > 0);
    std::vector<xi::client::EffectSystem::Batch> batches;
    fx.play(0, "hit1", 1, 2);
    fx.update(1.0f / 60.0f, actors);
    fx.update(1.0f / 60.0f, actors);
    fx.build(batches, { 1, 0, 0 }, { 0, 1, 0 });
    std::size_t verts = 0;
    bool        textured = false;
    for (const auto& b : batches)
    {
        verts += b.vertices.size();
        textured = textured || b.texture;
    }
    CHECK(verts > 0);
    CHECK(textured);
    int frames = 0;
    for (; frames < 3000 && fx.active(); ++frames)
    {
        fx.update(1.0f / 60.0f, actors);
    }
    WARN("frames until idle: " << frames << ", particles " << fx.particleCount());
    CHECK_FALSE(fx.active()); // everything ends
}

TEST_CASE("effects: a weapon skill file's main runs (37836 Galka Fast Blade)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ .pos = { id == 2 ? 2.0f : 0.0f, 0, 0 }, .yaw = 0.0f, .height = 1.8f, .point = {} };
    };
    REQUIRE(fx.play(37836, "main", 1, 2));
    std::size_t most = 0;
    for (int frame = 0; frame < 200; ++frame)
    {
        fx.update(1.0f / 60.0f, actors);
        most = std::max(most, fx.particleCount());
    }
    CHECK(most > 0);
}

TEST_CASE("effects: file 0 crtl branches on the result's info (expression VM)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ .pos = { id == 1 ? 0.0f : 2.0f, 0, 0 }, .yaw = 0.0f, .height = 1.8f, .point = {} };
    };
    auto peak = [&](int info, uint32_t attacker = 1) {
        xi::client::EffectSystem fx(*fxInstall());
        xi::client::FxAction     action{ { { 2, {} } }, attacker };
        action.results[0].vars[0x2B] = info;
        REQUIRE(fx.play(0, "crtl", 1, 2, 0.0f, &action));
        std::size_t most = 0;
        for (int frame = 0; frame < 40; ++frame)
        {
            fx.update(1.0f / 60.0f, actors);
            most = std::max(most, fx.particleCount());
        }
        return most;
    };
    CHECK(peak(0) == 0); // not a critical: nothing
    const auto pc = peak(2), mob = peak(2, 0x01000002);
    CHECK(pc > 0);  // critical by a PC (var 0x38): hi14
    CHECK(mob > 0); // by a monster: hi29
    CHECK(pc != mob);
}

TEST_CASE("effects: a spell's main loops over every target (op 0x31 / 0x32)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ .pos = { static_cast<float>(id), 0, 0 }, .yaw = 0.0f, .height = 1.8f, .point = {} };
    };
    auto peak = [&](int targets) {
        xi::client::EffectSystem fx(*fxInstall());
        xi::client::FxAction     action;
        for (int t = 0; t < targets; ++t)
        {
            action.results.push_back({ static_cast<uint32_t>(2 + t), {} });
        }
        REQUIRE(fx.play(2801, "main", 1, 2, 0.0f, &action));
        std::size_t most = 0;
        for (int frame = 0; frame < 240; ++frame)
        {
            fx.update(1.0f / 60.0f, actors);
            most = std::max(most, fx.particleCount());
        }
        return most;
    };
    const auto one = peak(1), three = peak(3);
    CHECK(one > 0);
    CHECK(three > 2 * one); // tgt0 runs on each target, 4 ticks apart
}

TEST_CASE("effects: file 0 hi10 (water added effect) flashes the target blue and back (op 0x2A)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ { static_cast<float>(id), 0, 0 }, 0.0f, 1.8f, {} };
    };
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.play(0, "hi10", 1, 2));
    for (int frame = 0; frame < 25; ++frame)
    {
        fx.update(1.0f / 60.0f, actors);
    }
    const auto mid = fx.actorTint(2);
    CHECK(mid.r == Catch::Approx(0x60 / 128.0f)); // 60 80 c0 80 is R, G, B, A
    CHECK(mid.b == Catch::Approx(0xC0 / 128.0f));
    CHECK(fx.actorTint(1) == glm::vec4(1.0f));     // the caster is untouched
    for (int frame = 0; frame < 120; ++frame)
    {
        fx.update(1.0f / 60.0f, actors);
    }
    CHECK(fx.actorTint(2) == glm::vec4(1.0f));
}

TEST_CASE("effects: a melee hit flinches the target; a 3-target spell shows each result (ops 0x25, 0x2B)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ { static_cast<float>(id), 0, 0 }, 0.0f, 1.8f, {} };
    };
    {
        // A melee hit (resolution 0) on a Galka: file 0 `dada` -> `dam0` -> the target's own `damg` (op 0x09, roles
        // swapped) -> op 0x21 flinches its caster, i.e. the one hit.
        xi::client::EffectSystem fx(*fxInstall());
        fx.setActorFiles([](uint32_t id) { return id == 2 ? std::vector<uint32_t>{ 26352, 26353, 26354, 26355, 26356 } : std::vector<uint32_t>{}; });
        const xi::client::FxAction hit{ { { 2, {} } }, 1 };
        REQUIRE(fx.play(0, "dada", 1, 2, 0.0f, &hit));
        std::vector<xi::client::FxMotion> m;
        for (int frame = 0; frame < 60; ++frame)
        {
            fx.update(1.0f / 60.0f, actors);
            for (auto& x : fx.takeMotions())
            {
                m.push_back(x);
            }
        }
        CHECK(std::any_of(m.begin(), m.end(), [](const xi::client::FxMotion& x) { return x.flinch && x.actor == 2; }));
    }
    {
        xi::client::EffectSystem fx(*fxInstall());
        xi::client::FxAction     action;
        action.tag = 7ull << 16;
        for (uint32_t t = 2; t < 5; ++t)
        {
            action.results.push_back({ t, {} });
        }
        REQUIRE(fx.play(2801, "main", 1, 2, 0.0f, &action));
        std::vector<uint64_t> shown;
        for (int frame = 0; frame < 600; ++frame)
        {
            fx.update(1.0f / 60.0f, actors);
            const auto c = fx.takeCommits();
            shown.insert(shown.end(), c.begin(), c.end());
        }
        std::sort(shown.begin(), shown.end());
        shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
        CHECK(shown == std::vector<uint64_t>{ (7ull << 16) | 1, (7ull << 16) | 2, (7ull << 16) | 3 });
    }
}

TEST_CASE("effects: a teleport's ops 0x47 colour its `ice ` overlay, not the body, until the zone resets")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ { static_cast<float>(id), 0, 0 }, 0.0f, 1.8f, {} };
    };
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.play(2952, "tsp0", 1, 1));
    float lowestBody = 1.0f, lowestOverlay = 1.0f;
    bool  overlay    = false;
    for (int frame = 0; frame < 900; ++frame)
    {
        fx.update(1.0f / 60.0f, actors);
        lowestBody = std::min(lowestBody, fx.actorTint(1).a);
        if (const auto ov = fx.actorOverlay(1))
        {
            overlay       = true;
            lowestOverlay = std::min(lowestOverlay, ov->colour.a);
        }
    }
    CHECK(lowestBody == 1.0f); // colour slot 1 (CODE 0x1005b1d4 -> 0x1005f5f0 slot 1) is the overlay's
    CHECK(overlay);
    CHECK(lowestOverlay < 0.05f);
    fx.reset();
    CHECK(fx.actorTint(1) == glm::vec4(1.0f));
    CHECK(!fx.active());
}

TEST_CASE("effects: a monster's pop0 fades it in from black (op 0x29)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ { static_cast<float>(id), 0, 0 }, 0.0f, 1.8f, {} };
    };
    xi::client::EffectSystem fx(*fxInstall());
    fx.setActorFiles([](uint32_t id) { return id == 5 ? std::vector<uint32_t>{ 1568 } : std::vector<uint32_t>{}; }); // Forest Hare
    REQUIRE(fx.playFromActor(5, xi::dat::fxName("pop0"), 5, 5));
    for (int frame = 0; frame < 4; ++frame)
    {
        fx.update(1.0f / 60.0f, actors);
    }
    CHECK(fx.actorTint(5).a < 0.1f); // black and invisible at first
    for (int frame = 0; frame < 120; ++frame)
    {
        fx.update(1.0f / 60.0f, actors);
    }
    CHECK(fx.actorTint(5) == glm::vec4(1.0f)); // 86 ticks later: normal
}

TEST_CASE("effects: a random branch runs one record of its group (op 0x3D; the hare's vdam cries once at most)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ { static_cast<float>(id), 0, 0 }, 0.0f, 1.8f, {} };
    };
    xi::client::EffectSystem fx(*fxInstall());
    fx.setActorFiles([](uint32_t) { return std::vector<uint32_t>{ 1568 }; });
    int silent = 0, cries = 0;
    for (int run = 0; run < 40; ++run)
    {
        REQUIRE(fx.playFromActor(5, xi::dat::fxName("vdam"), 5, 5));
        for (int frame = 0; frame < 10; ++frame)
        {
            fx.update(1.0f / 60.0f, actors);
        }
        const auto n = fx.takeSounds().size();
        CHECK(n <= 1);
        (n == 0 ? silent : cries) += 1;
    }
    CHECK(silent > 0); // 3 of the 7 records are no-ops
    CHECK(cries > 0);
}

TEST_CASE("effects: Selbina's zone file starts its ambient generators (flag 0x1000, not under weat)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    // DAT 348 = zone 248: wind, smoke, sea, lights, mode/pl (DATA: 89 flagged world-origin generators), plus the three
    // seagull schedulers that start themselves (s001..s003, op 0x52).
    CHECK(fx.startZone(348, [](xi::dat::FxName) { return nullptr; }) == 92);
    CHECK(fx.active());
}

TEST_CASE("effects: zone effect batches come in depth-key order (Port Bastok harbour layers)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    // A stand-in mesh for every Mmb (one triangle), so each layer makes a batch.
    static const xi::dat::FxMesh tri{ .vertices = { { 0, 0, 0, 0, 0, { 0x80, 0x80, 0x80, 0x80 } }, { 1, 0, 0, 1, 0, { 0x80, 0x80, 0x80, 0x80 } }, { 0, 0, 1, 0, 1, { 0x80, 0x80, 0x80, 0x80 } } },
                                      .groups = { { "", 0, 3 } },
                                      .frames = false, .uvPixels = false, .morph = {} };
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.startZone(336, [](xi::dat::FxName) { return &tri; }) > 0);
    fx.setDayFraction(0.5f);
    for (int frame = 0; frame < 5; ++frame)
    {
        fx.update(1.0f / 60.0f, {});
    }
    std::vector<xi::client::EffectSystem::Batch> batches;
    fx.build(batches, { 1, 0, 0 }, { 0, 1, 0 });
    REQUIRE(!batches.empty());
    CHECK(batches.front().depthKey == 5000.0f); // `down`, the black base, first
    for (std::size_t i = 1; i < batches.size(); ++i)
    {
        CHECK(batches[i - 1].depthKey >= batches[i].depthKey);
    }
}

TEST_CASE("effects: the weather's sky generators start and are replaced on a weather change")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    const auto base = fx.startZone(348, [](xi::dat::FxName) { return nullptr; });
    REQUIRE(base == 92); // 89 generators + 3 seagull schedulers
    for (int frame = 0; frame < 2; ++frame)
    {
        fx.update(1.0f / 60.0f, {});
    }
    const auto zoneGens = fx.generatorCount();
    fx.setWeather("suny");
    fx.setWeather("suny"); // the same weather again: nothing changes
    // Selbina weat/suny (DATA): sun0, cld1, cld2, moon, kasa, star, sta1, pole; lens flares lf01 / kas1 (type 0x39) skipped.
    CHECK(fx.generatorCount() == zoneGens + 8);
    fx.setWeather("clod"); // cld1, cld2, sun1 (lf03 skipped)
    CHECK(fx.generatorCount() == zoneGens + 3);
    fx.setWeather("nope"); // unknown -> suny
    CHECK(fx.generatorCount() == zoneGens + 8);
}

TEST_CASE("effects: zone sound particles play their Sep at the spawn point (West Ronfaure ambience)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.startZone(200, [](xi::dat::FxName) { return nullptr; }) > 0);
    fx.setSky({ -274.876f, -39.835f, 105.808f }, { 0, -1, 0 }, { 0, 1, 0 }); // Botone's spot: emitters cull by camera distance
    std::size_t positional = 0;
    for (int frame = 0; frame < 600; ++frame) // 10 s
    {
        fx.update(1.0f / 60.0f, {});
        for (const auto& s : fx.takeSounds())
        {
            positional += s.at.has_value() && s.sep.size() >= 0x10 ? 1 : 0;
        }
    }
    CHECK(positional > 10);
}

TEST_CASE("effects: a point light's intensity follows its L1 0x7c / L2 0x5b curve (zone 5 plk1)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.startZone(105, [](xi::dat::FxName) { return nullptr; }) > 0);
    fx.setDayFraction(0.9f);
    fx.setSky({ 575.5f, -31.0f, -101.5f }, { 0, -1, 0 }, { 0, 1, 0 }); // next to it (emitters cull by camera distance)
    float lo = 1e9f, hi = 0.0f;
    for (int frame = 0; frame < 1200; ++frame)
    {
        fx.update(1.0f / 60.0f, {});
        for (const auto& l : fx.pointLights())
        {
            if (l.key == xi::dat::fxName("plk1"))
            {
                lo = std::min(lo, glm::length(l.colour));
                hi = std::max(hi, glm::length(l.colour));
            }
        }
    }
    WARN("plk1 light " << lo << " .. " << hi);
    REQUIRE(hi > 0.0f);
    CHECK(hi > lo * 1.05f); // the curve (li00: base -> 10 -> 9 over the life) moves it
}

TEST_CASE("effects: L2 0x69 tints a particle by the scene light every frame (Phanauet Channel wa02)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    static const xi::dat::FxMesh tri{ .vertices = { { 0, 0, 0, 0, 0, { 0x80, 0x80, 0x80, 0x80 } }, { 1, 0, 0, 1, 0, { 0x80, 0x80, 0x80, 0x80 } }, { 0, 0, 1, 0, 1, { 0x80, 0x80, 0x80, 0x80 } } },
                                      .groups = { { "", 0, 3 } },
                                      .frames = false, .uvPixels = false, .morph = {} };
    auto brightness = [](const xi::client::EffectSystem& fx) {
        std::vector<xi::client::EffectSystem::Batch> batches;
        fx.build(batches, { 1, 0, 0 }, { 0, 1, 0 });
        double sum = 0;
        for (const auto& b : batches)
        {
            for (const auto& v : b.vertices)
            {
                // Both texture stages on a white texel: the element colour sits in stage 1 for textured meshes.
                const glm::vec4 c = xi::client::EffectSystem::fxCombine(v.color, glm::vec4(1.0f), v.stage);
                sum += c.r + c.g + c.b;
            }
        }
        return sum;
    };
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.startZone(101, [](xi::dat::FxName) { return &tri; }) > 0);
    fx.setLights(glm::vec3(1.0f), glm::vec3(1.0f));
    for (int frame = 0; frame < 120; ++frame)
    {
        fx.update(1.0f / 60.0f, {});
    }
    const double lit = brightness(fx);
    fx.setLights(glm::vec3(1.0f), glm::vec3(0.25f));
    fx.update(1.0f / 600.0f, {});
    const double dim = brightness(fx);
    WARN("brightness " << lit << " -> " << dim);
    REQUIRE(lit > 0);
    CHECK(dim < lit * 0.9); // already-living particles darken at once
}

TEST_CASE("effects: zone sound particles hear by L1 0x4c's distances, near first, plain distance (West Ronfaure)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    // L1 0x4c (CODE 0x10050201): rec+8 -> p+0x1b0, rec+4 -> p+0x1b4; 0x10036ba0 silences beyond +0x1b4 and fades from
    // +0x1b0, so rec+8 is the near distance (DATA: 222 `seu1` 40 / 20). World-placed generators measure plainly.
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.startZone(200, [](xi::dat::FxName) { return nullptr; }) > 0);
    fx.setSky({ -274.876f, -39.835f, 105.808f }, { 0, -1, 0 }, { 0, 1, 0 });
    std::size_t n = 0;
    for (int frame = 0; frame < 600; ++frame)
    {
        fx.update(1.0f / 60.0f, {});
        for (const auto& s : fx.takeSounds())
        {
            if (s.at)
            {
                ++n;
                CHECK(s.near <= s.far);
                CHECK(s.distanceMode == 1);
            }
        }
    }
    CHECK(n > 10);
}

TEST_CASE("effects: fork 7D's particle and generator ops all have a coverage status")
{
    using xi::client::FxList;
    using xi::client::FxOpStatus;
    for (const int op : { 0x42, 0x46, 0x4a, 0x9b, 0x7b, 0x92, 0x79, 0x88, 0x8a, 0x9d, 0x8e, 0x89, 0x9e, 0x94, 0x7e, 0x7f, 0x98, 0x57,
                              0x7a, 0x9c, 0xff })
    {
        CHECK(xi::client::fxOpStatus(FxList::L1, static_cast<uint8_t>(op)) != FxOpStatus::Unhandled);
    }
    for (const int op : { 0x48, 0x2f, 0x6e, 0x65, 0x47 })
    {
        CHECK(xi::client::fxOpStatus(FxList::L2, static_cast<uint8_t>(op)) != FxOpStatus::Unhandled);
    }
    for (const int op : { 0x0e, 0x0f, 0x10, 0x17 })
    {
        CHECK(xi::client::fxOpStatus(FxList::L0, static_cast<uint8_t>(op)) == FxOpStatus::Handled);
    }
    CHECK(xi::client::fxOpStatus(FxList::L1, 0xff) == FxOpStatus::GameNoOp); // past the jump table
}

TEST_CASE("effects: a spell whose generators turn by L0 0x0f runs with finite particles (2822 main)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    const xi::client::FxActorLookup actors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ .pos = { id == 1 ? 0.0f : 3.0f, 0, 0 }, .yaw = 0.5f, .height = 1.8f, .point = {} };
    };
    REQUIRE(fx.play(2822, "main", 1, 2));
    std::size_t most   = 0;
    bool        finite = true;
    for (int frame = 0; frame < 240; ++frame)
    {
        fx.update(1.0f / 60.0f, actors);
        most = std::max(most, fx.particleCount());
        std::vector<xi::client::EffectSystem::Batch> batches;
        fx.build(batches, { 1, 0, 0 }, { 0, 1, 0 });
        for (const auto& b : batches)
        {
            for (const auto& v : b.vertices)
            {
                finite = finite && std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z);
            }
        }
    }
    CHECK(most > 0);
    CHECK(finite);
}

TEST_CASE("effects: blend records pick their states by the low byte; 0x64 / 0x68 force TFACTOR alpha (issue #103)")
{
    using Fx    = xi::client::EffectSystem;
    using Blend = Fx::Batch::Blend;
    // CODE 0x10046710 switches on (mode & 0xff). Port Windurst's windmill sails (zone file 340, generators fan1..fan4,
    // D3m `_wi_`) use 0x8064: SRCALPHA / INVSRCALPHA with TFACTOR alpha min(0x8064 >> 7 & ~1, 255) = 0xFF.
    CHECK(Fx::fxBlendClass(0x8064) == Blend::Alpha);
    CHECK(Fx::fxBlendClass(0x7f64) == Blend::Alpha);
    CHECK(Fx::fxBlendClass(0x8068) == Blend::Additive);
    CHECK(Fx::fxBlendClass(0x0044) == Blend::Alpha);
    CHECK(Fx::fxBlendClass(0x0048) == Blend::Additive);
    CHECK(Fx::fxBlendClass(0x0142) == Blend::Subtract);
    REQUIRE(Fx::fxBlendElementAlpha(0x8064));
    CHECK(*Fx::fxBlendElementAlpha(0x8064) == 1.0f);
    CHECK(*Fx::fxBlendElementAlpha(0x7f64) == Catch::Approx(0xFE / 255.0f));
    CHECK(*Fx::fxBlendElementAlpha(0x2068) == Catch::Approx(0x40 / 255.0f));
    CHECK(*Fx::fxBlendElementAlpha(0x4062) == Catch::Approx(0x40 / 255.0f));
    CHECK(!Fx::fxBlendElementAlpha(0x0044));
    CHECK(!Fx::fxBlendElementAlpha(0x0048));
    // So a sail texel at the data's alpha 0x88 under the D3m's vertex alpha 0x80 comes out opaque.
    const glm::vec4 vtx(0xc8 / 255.0f, 0xc8 / 255.0f, 0xc8 / 255.0f, 0x80 / 255.0f);
    const glm::vec4 elem(0x80 / 255.0f, 0x80 / 255.0f, 0x80 / 255.0f, *Fx::fxBlendElementAlpha(0x8064));
    const auto [c, st] = Fx::fxStages(vtx, elem, true);
    CHECK(Fx::fxCombine(c, glm::vec4(1.0f, 1.0f, 1.0f, 0x88 / 255.0f), st).a == 1.0f);
}

TEST_CASE("effects: Port Windurst's windmill sails draw alpha-blended at full alpha (issue #103)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    static const xi::dat::FxMesh tri{ .vertices = { { 0, 0, 0, 0, 0, { 0x80, 0x80, 0x80, 0x80 } }, { 1, 0, 0, 1, 0, { 0x80, 0x80, 0x80, 0x80 } }, { 0, 0, 1, 0, 1, { 0x80, 0x80, 0x80, 0x80 } } },
                                      .groups = { { "", 0, 3 } },
                                      .frames = false, .uvPixels = false, .morph = {} };
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.startZone(340, [](xi::dat::FxName) { return &tri; }) > 0);
    fx.setDayFraction(0.5f);
    for (int frame = 0; frame < 5; ++frame)
    {
        fx.update(1.0f / 60.0f, {});
    }
    std::vector<xi::client::EffectSystem::Batch> batches;
    fx.build(batches, { 1, 0, 0 }, { 0, 1, 0 });
    // The sails (texture `fusha   t_win03f`, blend 0x8064) were taken for additive (0x8064 != 0x64) at their own alpha
    // 0x80: see-through and glowing. Now: SRCALPHA / INVSRCALPHA with TFACTOR alpha 0xFF (stage 1 alpha 4 x 1.0).
    std::size_t sails = 0;
    for (const auto& b : batches)
    {
        if (b.texture && b.texture->name.find("t_win03f") != std::string::npos)
        {
            ++sails;
            CHECK(b.blend == xi::client::EffectSystem::Batch::Blend::Alpha);
            REQUIRE(!b.vertices.empty());
            CHECK(b.vertices.front().stage.a == Catch::Approx(4.0f));
        }
    }
    CHECK(sails > 0);
}
