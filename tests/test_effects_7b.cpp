// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/install.h"
#include "client/effect_ops.h"
#include "client/effects.h"
#include "dat/dat_index.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <memory>

// Scheduler ops of effect-format "Coverage, part 7B", run from the installed game's files.
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

const xi::client::FxActorLookup kActors = [](uint32_t id) -> std::optional<xi::client::FxActor> {
    return xi::client::FxActor{ .pos = { id == 2 ? 2.0f : 0.0f, 0, 0 }, .yaw = 0.0f, .height = 1.8f, .point = {} };
};

void run(xi::client::EffectSystem& fx, int ticks)
{
    for (int i = 0; i < ticks; ++i)
    {
        fx.update(1.0f / 60.0f, kActors);
    }
}
} // namespace

TEST_CASE("effects 7B: every part-7B scheduler op has a coverage status other than unhandled (and 7E: 0x6E / 0x7F)")
{
    using xi::client::FxList;
    using xi::client::FxOpStatus;
    for (const int op : { 0xa4, 0xa5, 0x72, 0x7a, 0x62, 0xba, 0xb0, 0x6f, 0x70, 0xb3, 0x19, 0x8a, 0x8b, 0x27, 0xb5, 0xb9, 0xad, 0xac, 0x9b,
                          0x51, 0xb4, 0x7d, 0x7c, 0x7b, 0x7e, 0xbc, 0xb2, 0xae, 0x1d, 0xb6, 0xbf, 0xbd, 0xa6, 0xa7, 0xa8, 0xa9, 0xab, 0x9d,
                          0xa1, 0xb8, 0xc2, 0x6e, 0x7f })
    {
        INFO("op " << op);
        CHECK(xi::client::fxOpStatus(FxList::Scheduler, static_cast<uint8_t>(op)) != FxOpStatus::Unhandled);
    }
}

TEST_CASE("effects 7B: op 0x19 starts `main` of file 2800 + n (file 0 spop -> 3416)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.play(0, "spop", 1, 2));
    run(fx, 2);
    CHECK(fx.runningFile(3416, xi::dat::fxName("main"), 1));
}

TEST_CASE("effects 7B: op 0x72 adds a white flash over the frame and takes it away (zone 116 door stb3)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.play(116, "stb3", 1, 1));
    auto added = [&] {
        for (const auto& l : fx.screenLayers())
        {
            if (!l.image && l.additive)
            {
                return l.colour;
            }
        }
        return glm::vec4(0.0f);
    };
    CHECK(added().a == 0.0f);
    run(fx, 300); // past t = 160 + 120: 80 80 80 80 reached
    CHECK(added().r == Catch::Approx(128.0f / 255.0f).margin(0.01f));
    CHECK(added().a == Catch::Approx(1.0f).margin(0.01f));
    run(fx, 200); // t = 500: faded to 0 (350 .. 470) and cleared (470 .. 480)
    CHECK(added().a == 0.0f);
}

TEST_CASE("effects 7B: op 0x51 darkens the frame (616 lat0: 00 00 00 80 -> alpha = A x B / 128 = 0)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.play(616, "lat0", 1, 1));
    run(fx, 600);
    bool black = false;
    for (const auto& l : fx.screenLayers())
    {
        black = black || (!l.image && !l.additive && l.colour.a > 0.95f && l.colour.r == 0.0f);
    }
    CHECK(black);
    fx.reset();
    CHECK(fx.screenLayers().empty());
}

TEST_CASE("effects 7B: op 0x7C asks for a weather group (title file 23 ex3e: `fine`)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    const auto before = fx.environment().serial;
    REQUIRE(fx.play(23, "ex3e", 1, 1));
    run(fx, 600);
    CHECK(fx.environment().serial != before);
    CHECK(fx.environment().fourcc.size() == 4);
    fx.reset();
    CHECK(fx.environment().fourcc.empty());
}

TEST_CASE("effects 7B: op 0x8A holds a sound in a slot, op 0x8B stops it (cutscene 30734 `6040`)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.play(30734, "6040", 1, 1));
    bool started = false, stopped = false;
    for (int i = 0; i < 1200 && !(started && stopped); ++i)
    {
        run(fx, 1);
        for (const auto& s : fx.takeSounds())
        {
            started = started || s.slot != 0;
        }
        stopped = stopped || !fx.takeSoundStops().empty();
    }
    CHECK(started);
    CHECK(stopped);
}

TEST_CASE("effects 7B: op 0xBC keeps footsteps sounding while standing for its duration (52969 dead)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.play(52969, "dead", 1, 1));
    bool seen = false;
    for (int i = 0; i < 600 && !seen; ++i)
    {
        run(fx, 1);
        seen = fx.stepsForced(1);
    }
    CHECK(seen);
    run(fx, 1200);
    CHECK_FALSE(fx.stepsForced(1));
}

TEST_CASE("effects: an op 0x23 doll follows its target's position (CODE 0x100ac020; file 3937 tgt0 Wild Carrot)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    // The target walks along +x after the doll is made; the particles started on the doll go with it.
    float                               targetX = 2.0f;
    const xi::client::FxActorLookup moving = [&](uint32_t id) -> std::optional<xi::client::FxActor> {
        return xi::client::FxActor{ .pos = { id == 2 ? targetX : 0.0f, 0, 0 }, .yaw = 0.0f, .height = 1.8f, .point = {} };
    };
    xi::client::EffectSystem fx(*fxInstall());
    REQUIRE(fx.play(3937, "tgt0", 1, 2));
    fx.update(1.0f / 60.0f, moving);
    for (int i = 0; i < 40; ++i)
    {
        targetX += 0.25f; // 10 yalms in 40 ticks
        fx.update(1.0f / 60.0f, moving);
    }
    std::vector<xi::client::EffectSystem::Batch> batches;
    fx.build(batches, { 1, 0, 0 }, { 0, 1, 0 });
    double sum = 0;
    size_t n   = 0;
    for (const auto& b : batches)
    {
        for (const auto& v : b.vertices)
        {
            sum += v.pos.x;
            ++n;
        }
    }
    REQUIRE(n > 0);
    CHECK(sum / static_cast<double>(n) > 8.0); // with the target (x 12), not left at x 2
}

TEST_CASE("effects: Selbina's gulls keep their heading and bank by L2 0x64's formula (file 348 kamo; CODE 0x1004b179)")
{
    if (!fxInstall())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*fxInstall());
    fx.setDayFraction((8.0f + 1.0f / 60.0f) / 24.0f); // 08:01, inside s001's window (08:00 + 3 min each hour)
    REQUIRE(fx.play(348, "s001", 0, 0));
    const auto gulls = [&] { return fx.particles(xi::dat::fxName("kamo")); };
    for (int i = 0; i < 400 && gulls().empty(); ++i)
    {
        fx.update(1.0f / 30.0f, kActors); // one game frame: 2 ticks
    }
    REQUIRE_FALSE(gulls().empty());
    auto wrap = [](float a) {
        while (a > 3.14159265f)
        {
            a -= 6.2831853f;
        }
        while (a < -3.14159265f)
        {
            a += 6.2831853f;
        }
        return a;
    };
    auto before = gulls();
    fx.update(1.0f / 30.0f, kActors);
    auto now     = gulls();
    int  checked = 0;
    for (int frame = 0; frame < 600 && checked < 20; ++frame)
    {
        fx.update(1.0f / 30.0f, kActors);
        const auto next = gulls();
        if (next.size() == now.size() && now.size() == before.size())
        {
            for (std::size_t i = 0; i < next.size(); ++i)
            {
                // The game never turns a gull toward its path: the spawn heading (L1 0x09: 0) stays.
                CHECK(next[i].rot.y == 0.0f);
                CHECK(next[i].rot.z == 0.0f);
                const glm::vec3 last = now[i].pos - before[i].pos;
                const glm::vec3 d    = next[i].pos - now[i].pos;
                if (glm::length(glm::vec2(last.x, last.z)) < 1e-4f || glm::length(glm::vec2(d.x, d.z)) < 1e-4f)
                {
                    continue;
                }
                const float turn = wrap(std::atan2(d.z, d.x) - std::atan2(last.z, last.x));
                if (std::abs(turn) > 2e-3f && std::abs(turn) < 0.05f)
                {
                    CHECK(next[i].rot.x == Catch::Approx(wrap(-30.0f * turn)).margin(1e-3));
                    ++checked;
                }
            }
        }
        before = std::move(now);
        now    = next;
    }
    CHECK(checked > 0);
}
