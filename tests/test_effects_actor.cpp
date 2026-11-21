// SPDX-License-Identifier: GPL-3.0-or-later
// Goal 7, part 7A: the scheduler's actor ops (effect-format "Coverage, part 7A").
#include "xi/install.h"
#include "client/effects.h"
#include "client/session.h"
#include "dat/character.h"
#include "dat/dat_index.h"
#include "dat/motion.h"
#include "dat/effect.h"
#include "ui/fishing_gauge.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
} // namespace

TEST_CASE("effects-only NPC models: init ops 0x8D-0x96 dress a PC look, 0x98 / 0x99 alias and scale")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Model 361 = file 1661 (block `mith`): race 7 (Mithra), face 2, head 0x3f, body 0x37, hands 0x3c, legs 0x58,
    // feet 0x3c, main 0xd8 (DATA).
    const auto l = xi::dat::effectLook(*install(), 1661);
    REQUIRE(l);
    CHECK(l->race == 7);
    CHECK(l->slots[0] == 2);
    CHECK(l->slots[1] == 0x3f);
    CHECK(l->slots[2] == 0x37);
    CHECK(l->slots[6] == 0xd8);
    CHECK(l->model == -1);
    const auto a = xi::dat::loadNpcAsset(*install(), 361);
    REQUIRE(a);
    CHECK_FALSE(a->vertices.empty());
    REQUIRE_FALSE(a->effectFiles.empty());
    CHECK(a->effectFiles.front() == 1661);
    // File 2060 (`ura_`): op 0x98 = model 790, op 0x99 = 1.5.
    const auto u = xi::dat::effectLook(*install(), 2060);
    REQUIRE(u);
    CHECK(u->race == -1);
    CHECK(u->model == 790);
    CHECK(u->scale == Catch::Approx(1.5f).epsilon(1e-3));
    const auto b = xi::dat::loadNpcAsset(*install(), 760);
    if (b)
    {
        CHECK(b->scale == Catch::Approx(1.5f).epsilon(1e-3));
    }
}

namespace
{
xi::client::FxActorLookup twoActors()
{
    return [](uint32_t id) -> std::optional<xi::client::FxActor> {
        if (id == 1)
        {
            return xi::client::FxActor{ .pos = { 0, 0, 0 }, .yaw = 0.0f, .height = 1.8f, .point = {} };
        }
        if (id == 2)
        {
            return xi::client::FxActor{ .pos = { 3, 0, 0 }, .yaw = 3.14159f, .height = 1.8f, .point = {} }; // facing 1
        }
        return std::nullopt;
    };
}
void run(xi::client::EffectSystem& fx, const xi::client::FxActorLookup& a, float seconds)
{
    for (float t = 0; t < seconds; t += 1.0f / 60.0f)
    {
        fx.update(1.0f / 60.0f, a);
    }
}
} // namespace

TEST_CASE("effect op 0x5E: knockback by the result's level slides the target away and plays bf0")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*install());
    xi::client::FxAction     action;
    action.actor = 1;
    action.results.push_back({ 2, {} });
    action.results[0].vars[0x2C] = 3 << 2; // knockback level 3 (r+0xA >> 2): 1/6 per tick, 10 ticks, decay 0.15
    REQUIRE(fx.play(3444, "main", 1, 2, 0.0f, &action));
    const auto a      = twoActors();
    bool       motion = false;
    for (int i = 0; i < 300; ++i)
    {
        fx.update(1.0f / 60.0f, a);
        for (const auto& m : fx.takeMotions())
        {
            motion = motion || (m.knockback && m.actor == 2 && m.stem == "bf0");
        }
    }
    CHECK(motion);
    const auto d = fx.actorDisplacement(2);
    // The hold (10 ticks, the step that ends it included) x 1/6 = 1.83, then 1/6 x 0.85 / 0.15 = 0.94 more: about
    // 2.77 along +x (away from actor 1).
    CHECK(d.x == Catch::Approx(2.77f).margin(0.1f));
    CHECK(std::abs(d.z) < 1e-3f);
}

TEST_CASE("effect ops 0x79 / 0x8C: a bomb's ini2 picks the bt2 / id2 motion sets")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*install());
    REQUIRE(fx.play(1582, "ini2", 1, 1));
    run(fx, twoActors(), 2.0f);
    const auto* st = fx.stance(1);
    REQUIRE(st);
    CHECK(st->idle == "id2");
    CHECK(st->battle == "bt2");
}

TEST_CASE("effect op 0x40: a texture overlay on the caster for its duration")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*install());
    REQUIRE(fx.play(1311, "aper", 1, 2));
    const auto a = twoActors();
    run(fx, a, 140.0f / 60.0f); // op 0x40 `nami` at t = 137 for 100 ticks
    const auto ov = fx.actorOverlay(1);
    REQUIRE(ov);
    CHECK(ov->texture != nullptr);
    CHECK(ov->additive);
    run(fx, a, 2.0f);
    CHECK_FALSE(fx.actorOverlay(1));
}

TEST_CASE("effect op 0x6E: the caster walks along its facing at +8 yalms a second for the duration (39275 cdam)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    xi::client::EffectSystem fx(*install());
    REQUIRE(fx.play(39275, "cdam", 1, 1));
    run(fx, twoActors(), 4.0f);
    // 8 yalms a second for 120 ticks: 16 along actor 1's facing (+x).
    const auto d = fx.actorDisplacement(1);
    CHECK(d.x == Catch::Approx(16.0f).margin(0.3f));
    CHECK(std::abs(d.z) < 1e-3f);
}

TEST_CASE("effect ops 0x75 / 0xA3: +8 != 0 hides the caster's part wepN, 0 shows it (issue #74)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto a = twoActors();
    {
        // Aern (model 1373, file 2673) init: `hiof` hides the body (wep4) at once; 60 frames later `rgof` hides the
        // crystals / knife (wep6 / wep5) and `hion` shows the body again.
        xi::client::EffectSystem fx(*install());
        REQUIRE(fx.play(2673, "init", 1, 1));
        run(fx, a, 0.5f);
        CHECK((fx.hiddenParts(1) >> 4 & 1) == 1);
        run(fx, a, 2.0f);
        CHECK((fx.hiddenParts(1) >> 4 & 1) == 0);
        CHECK((fx.hiddenParts(1) >> 5 & 1) == 1);
        CHECK((fx.hiddenParts(1) >> 6 & 1) == 1);
    }
    {
        // A treasure chest (model 258, file 1558): the box (wep3 / wep4) shows, its variants (wep5..7) are hidden.
        xi::client::EffectSystem fx(*install());
        REQUIRE(fx.play(1558, "init", 1, 1));
        run(fx, a, 1.0f);
        CHECK(fx.hiddenParts(1) == 0xE0);
    }
}

TEST_CASE("S2C 0x00E look kind 6: the model in look slot 9 is a standard NPC model (Awghrah, issue #75)")
{
    xi::client::Look l;
    l.kind   = xi::client::kLookSlot9;
    l.ids[0] = 0x48C; // Awghrah in The Garden of Ru'Hmet (LSB)
    CHECK(xi::client::npcModel(l) == 1164);
    l.ids[0] = 0x948C; // the slot tag bits are dropped (CODE 0x100d0cfb: id & 0xFFF)
    CHECK(xi::client::npcModel(l) == 1164);
    l.kind = 2; // doors have no NPC model
    CHECK(xi::client::npcModel(l) == 0);
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto a = xi::dat::loadNpcAsset(*install(), 1164);
    REQUIRE(a);
    CHECK_FALSE(a->vertices.empty());
}

TEST_CASE("S2C 0x00E helper NPCs (warp01) are not targetable and have no name label (issue #96)")
{
    // LSB's Port Windurst warp01: status 0 at +0x20, entity_flags 2051 (0x803) as a u32 at +0x21, look kind 0 model 50.
    std::array<uint8_t, 0x48> d{};
    d[0x20]               = 0;
    const uint32_t mflags = 2051;
    std::memcpy(d.data() + 0x21, &mflags, 4);
    uint32_t flags1 = 0;
    std::memcpy(&flags1, d.data() + 0x20, 4);
    CHECK(xi::client::flags1TargetOff(flags1));
    CHECK_FALSE(xi::client::flags1TargetOff(0x0000'0300)); // a plain NPC (LSB flags 0x3: no 0x800)

    CHECK(xi::client::modelHidesName(50));
    CHECK(xi::client::modelHidesName(0x3B));
    CHECK(xi::client::modelHidesName(0x9C3));
    CHECK_FALSE(xi::client::modelHidesName(2361)); // Skipper Moogle keeps its name
    CHECK_FALSE(xi::client::modelHidesName(0x3C));

    xi::client::Entity warp;
    warp.name       = "warp01";
    warp.look.kind  = 0;
    warp.look.ids[0] = 50;
    warp.look.valid = true;
    CHECK(warp.labelHidden());
    warp.look.ids[0] = 2361;
    CHECK_FALSE(warp.labelHidden());
    warp.nameHidden = true; // namevis still hides it
    CHECK(warp.labelHidden());
}

TEST_CASE("job abilities leave the caster and target visible with nothing left over (issue #76)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto a = twoActors();
    {
        // Hundred Fists (animation 34, file 4446): `scal` fades the overlay's colour (op 0x46, slot 1) in and out
        // again; the body (slot 0) stays as it is.
        xi::client::EffectSystem fx(*install());
        REQUIRE(fx.play(4446, "main", 1, 1));
        run(fx, a, 1.5f);
        REQUIRE(fx.actorOverlay(1));
        CHECK(fx.actorTint(1).a == Catch::Approx(1.0f));
        run(fx, a, 20.0f);
        CHECK(fx.actorTint(1).a == Catch::Approx(1.0f));
        CHECK_FALSE(fx.actorOverlay(1));
        CHECK(fx.describeGenerators() == "");
    }
    // Every job ability's `main` (S2C 0x028 category 6: file 4412 + animation): afterwards, back in its stance, the
    // caster holds its main / sub weapons again, the caster and the target are fully visible, no overlay is left and
    // every generator has ended. Animation 10 (Hide, Camouflage) leaves the caster faded on purpose (0x29 to alpha
    // 0x27) until the server ends the status.
    int played = 0;
    for (uint32_t anim = 0; anim < 512; ++anim)
    {
        xi::client::EffectSystem fx(*install());
        if (!fx.play(4412 + anim, "main", 1, 2))
        {
            continue;
        }
        ++played;
        fx.stancePick(1, true);
        run(fx, a, 30.0f);
        fx.stancePick(1, false);
        INFO("job ability animation " << anim << " (file " << 4412 + anim << ")");
        if (anim != 10)
        {
            CHECK(fx.actorTint(1).a == Catch::Approx(1.0f));
        }
        CHECK(fx.actorTint(2).a == Catch::Approx(1.0f));
        CHECK((fx.hiddenParts(1) & 3) == 0);
        CHECK_FALSE(fx.actorOverlay(1));
        CHECK_FALSE(fx.actorOverlay(2));
        CHECK(fx.describeGenerators() == "");
    }
    CHECK(played > 100);
}

TEST_CASE("generator size scaling and motion bone scale: the Ghrah orb's element glow shows (issue #83)")
{
    // CODE 0x10053d90: origin bits 20-23 = 4 -> actor A, uniform, (max(width, height) - 1) x weight + 1.
    xi::client::FxActor a{};
    a.size       = { 1.2f, 1.94f };
    const auto s = xi::client::EffectSystem::sizeScale(0x44001d1, 1.0f, a, a);
    CHECK(s.x == Catch::Approx(1.94f));
    CHECK(s.y == Catch::Approx(1.94f));
    CHECK(xi::client::EffectSystem::sizeScale(0x00001d1, 1.0f, a, a) == glm::vec3(1.0f)); // no size mode
    const auto y = xi::client::EffectSystem::sizeScale(0x00200001, 0.5f, a, a);           // mode 2: Y only, half weight
    CHECK(y.x == Catch::Approx(1.0f));
    CHECK(y.y == Catch::Approx(1.47f));
    // CODE 0x100d0810 / 0x100d08e0: width / height x scale / 1.7.
    const auto sz = xi::client::fxActorSize({ -3.4f, 0.0f, 0.85f, -0.85f, 0, 0 }, 1.0f);
    CHECK(sz.x == Catch::Approx(1.0f));
    CHECK(sz.y == Catch::Approx(2.0f));
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Awghrah (model 1161 = file 2461, the orb model 1160): `idl` scales the other forms' bones (4..) to 1e-5, so
    // they vanish (CODE 0x10034700 premultiplies each bone by its accumulated scale).
    const auto m = xi::dat::loadNpcAsset(*install(), 1161);
    REQUIRE(m);
    const auto idl = xi::dat::loadMotionSet(*install(), m->motionFiles, "idl");
    REQUIRE_FALSE(idl.empty());
    std::vector<xi::dat::RowMatrix> pose;
    xi::dat::poseSkeleton(m->skeleton, { &idl }, 0.0f, pose);
    REQUIRE(pose.size() > 4);
    CHECK(std::hypot(pose[4][0], pose[4][1], pose[4][2]) < 1e-3f);
    CHECK(std::hypot(pose[2][0], pose[2][1], pose[2][2]) == Catch::Approx(1.0f).margin(0.35f));
    // The element core (gc00 at scale 0.2, times the orb's size ~1.9) is wider than the orb's inner sphere (radius
    // ~0.14), so it shows through the gaps between the plates.
    const auto size = xi::client::fxActorSize(m->skeleton.bounds[0], m->scale);
    CHECK(std::max(size.x, size.y) > 1.5f);
    xi::client::EffectSystem fx(*install());
    std::vector<uint32_t>    files = m->effectFiles;
    files.insert(files.end(), m->motionFiles.begin(), m->motionFiles.end());
    fx.setActorFiles([files](uint32_t id) { return id == 1 ? files : std::vector<uint32_t>{}; });
    REQUIRE(fx.playFromActor(1, xi::dat::fxName("init"), 1, 1));
    fx.startModelGenerators(1);
    const xi::client::FxActorLookup look = [size](uint32_t id) -> std::optional<xi::client::FxActor> {
        if (id != 1)
        {
            return std::nullopt;
        }
        xi::client::FxActor r{};
        r.size = size;
        return r;
    };
    run(fx, look, 1.0f);
    std::vector<xi::client::EffectSystem::Batch> batches;
    fx.build(batches, { 1, 0, 0 }, { 0, 1, 0 });
    float widest = 0;
    for (const auto& b : batches)
    {
        float lo = 1e9f, hi = -1e9f;
        for (const auto& v : b.vertices)
        {
            lo = std::min(lo, v.pos.x), hi = std::max(hi, v.pos.x);
        }
        widest = std::max(widest, hi - lo);
    }
    CHECK(widest > 0.3f);
}

TEST_CASE("#100: the fishing arrows (file 3727) are screen-space particles placed in front of the camera")
{
    if (!install())
    {
        SKIP("no game data");
    }
    // Particle flags bits 2 + 3 (CODE 0x10045cd7): the local matrix alone, Y flipped, under an identity view. The
    // arrow `al0a` sits at about (-0.09, -0.025, 0.25) in camera space: left of the centre, a quarter yalm ahead.
    for (const char* name : { "mai0", "mai6" }) // silver left arrow, golden left arrow
    {
        xi::client::EffectSystem fx(*install());
        const glm::vec3          cam(10, -5, 20);
        fx.setSky(cam, { 0, -1, 0 }, { 0, -1, 0 });
        const xi::client::FxActorLookup look = [](uint32_t id) -> std::optional<xi::client::FxActor> {
            if (id != 1)
            {
                return std::nullopt;
            }
            xi::client::FxActor r{};
            r.pos = { 100, 0, 100 }; // far from the camera: the arrows must not follow the actor
            return r;
        };
        REQUIRE(fx.playFile(3727, xi::dat::fxName(name), 1, 1));
        run(fx, look, 0.3f);
        std::vector<xi::client::EffectSystem::Batch> batches;
        // Camera right +x, down +y (game space): ahead is right x down = +z.
        fx.build(batches, { 1, 0, 0 }, { 0, 1, 0 });
        std::size_t n = 0;
        glm::vec3   sum(0.0f);
        for (const auto& b : batches)
        {
            for (const auto& v : b.vertices)
            {
                sum += v.pos;
                ++n;
            }
        }
        INFO(name);
        REQUIRE(n > 0);
        const glm::vec3 c = sum / static_cast<float>(n) - cam;
        CHECK(c.z > 0.0f);  // ahead of the camera (the gold arrow's glow mesh has depth of its own)
        CHECK(c.z < 1.0f);
        CHECK(c.x < 0.0f);  // a left arrow, left of the centre
        CHECK(std::abs(c.y) < 0.1f);
    }
}

TEST_CASE("#100: fishing gauge and arrow picks")
{
    using xi::ui::fishArrowFx;
    using xi::ui::fishGaugePercent;
    // Right (dir 0) / left (dir 1), silver / gold: mai3 / mai0 / mai9 / mai6, each with +1 hit and +2 gone.
    STATIC_CHECK(fishArrowFx(false, 0) == 3);
    STATIC_CHECK(fishArrowFx(false, 1) == 0);
    STATIC_CHECK(fishArrowFx(true, 0) == 9);
    STATIC_CHECK(fishArrowFx(true, 1) == 6);
    STATIC_CHECK(fishGaugePercent(1500, 1500) == 100);
    STATIC_CHECK(fishGaugePercent(749, 1500) == 49);
    STATIC_CHECK(fishGaugePercent(0, 1500) == 0);
    STATIC_CHECK(fishGaugePercent(-20, 1500) == 0);
    STATIC_CHECK(fishGaugePercent(10, 0) == 0);
    // The gauge eases: 0.15 of the gap a frame, truncated like the game's ftol; the last few points at once.
    using xi::ui::fishGaugeEase;
    STATIC_CHECK(fishGaugeEase(1500, 1400) == 1485);
    STATIC_CHECK(fishGaugeEase(1400, 1500) == 1415);
    STATIC_CHECK(fishGaugeEase(1406, 1400) == 1400); // 0.9 of a point: snaps
    STATIC_CHECK(fishGaugeEase(1407, 1400) == 1405); // 1.05: 1405.95 truncated
    STATIC_CHECK(fishGaugeEase(5, 0) == 0);
    {
        int shown = 1500; // a hit of 300 shows over a few frames, not at once
        shown     = fishGaugeEase(shown, 1200);
        CHECK(shown == 1455);
        for (int i = 0; i < 60; ++i)
        {
            shown = fishGaugeEase(shown, 1200);
        }
        CHECK(shown == 1200);
    }
    // Hook check with no server delay: 900..1500 ticks; the cast's reel-in lock: 3 s.
    STATIC_CHECK(xi::ui::fishDefaultHookWait(0) == 900);
    STATIC_CHECK(xi::ui::fishDefaultHookWait(600) == 1500);
    STATIC_CHECK(xi::ui::fishDefaultHookWait(601) == 900);
    STATIC_CHECK(xi::ui::kFishCastLock == 180);
    if (!install())
    {
        SKIP("no game data");
    }
    // Every scheduler the picks can name exists in the arrow file.
    xi::client::EffectSystem fx(*install());
    for (const bool gold : { false, true })
    {
        for (const int dir : { 0, 1 })
        {
            for (int step = 0; step < 3; ++step)
            {
                static constexpr char kHex[] = "0123456789ab";
                const int             idx    = fishArrowFx(gold, dir) + step;
                const std::string     name   = std::string("mai") + kHex[idx];
                INFO(name);
                CHECK(fx.playFile(3727, xi::dat::fxName(name), 1, 1));
            }
        }
    }
}

namespace
{
// The files a monster model's effects are looked up in (actor_poses.cpp: effect, battle motion, motion files).
std::vector<uint32_t> monsterFiles(uint16_t model)
{
    const auto asset = xi::dat::loadNpcAsset(*install(), model);
    REQUIRE(asset);
    auto files = asset->effectFiles;
    files.insert(files.end(), asset->battleMotionFiles.begin(), asset->battleMotionFiles.end());
    files.insert(files.end(), asset->motionFiles.begin(), asset->motionFiles.end());
    return files;
}

// Whether any of the cast glow's generators (`gn1?` black, `gn2?` white, file 0) is still alive.
bool glowLeft(const xi::client::EffectSystem& fx, std::string_view school)
{
    return fx.describeGenerators().find(school == "wh" ? "gn2" : "gn1") != std::string::npos;
}
} // namespace

TEST_CASE("#113: the cast glow ends when the spell goes off or is interrupted")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto a = twoActors();
    struct Case
    {
        const char*           who = "";
        std::vector<uint32_t> files;
        const char*           school = "";
        uint32_t              anim   = 0; // spell animation: 2800 + anim `main` is the finish (dat-format §10.8)
    };
    const std::vector<Case> cases{
        { "Goblin (model 488), Paralyze", monsterFiles(488), "wh", 58 },
        { "Goblin (model 488), Diaga", monsterFiles(488), "wh", 33 },
        { "Goblin (model 488), Poison", monsterFiles(488), "bk", 220 },
        { "Hume male (7072), Cure", { 7072 }, "wh", 1 },
        { "Hume male (7072), Stone", { 7072 }, "bk", 159 },
    };
    for (const auto& c : cases)
    {
        INFO(c.who);
        const auto files = c.files;
        auto       fresh = [&](xi::client::EffectSystem& fx) {
            fx.setActorFiles([files](uint32_t id) { return id == 1 ? files : std::vector<uint32_t>{}; });
            // S2C 0x028 type 8: `ca..` from the caster's files, caster = target = the caster (effect-format §C.1).
            REQUIRE(fx.playFromActor(1, xi::dat::fxName(std::string("ca") + c.school), 1, 1));
            run(fx, a, 2.0f);
            REQUIRE(glowLeft(fx, c.school));
        };
        {
            // Type 4: the spell's `main`, caster -> its target; its op 0x3C `sh..` runs `st..`, which must stop the glow
            // that `ca..` started toward the caster itself (it used to wait out its 1800 ticks: 30 s).
            xi::client::EffectSystem fx(*install());
            fresh(fx);
            REQUIRE(fx.play(2800 + c.anim, "main", 1, 2));
            run(fx, a, 4.0f);
            CHECK_FALSE(glowLeft(fx, c.school));
        }
        {
            // Type 8 interrupt: `sp..` from file 0 kills the glow; its live particles finish within about two seconds.
            xi::client::EffectSystem fx(*install());
            fresh(fx);
            REQUIRE(fx.playFromActor(1, xi::dat::fxName(std::string("sp") + c.school), 1, 1));
            run(fx, a, 2.5f);
            CHECK_FALSE(glowLeft(fx, c.school));
        }
    }
}
