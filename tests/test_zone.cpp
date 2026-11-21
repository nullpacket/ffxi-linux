// SPDX-License-Identifier: GPL-3.0-or-later
// Regression tests against the user's own FFXI install (skipped when FFXI_ROOT is unset or missing).
// Expected values were verified against the client code, LandSandBoat and the live server (docs/dat-format.md).
#include "xi/install.h"
#include "dat/effect.h"
#include "dat/chunk.h"
#include "dat/dat_index.h"
#include "dat/texture.h"
#include "dat/ui.h"
#include "dat/zone_map.h"
#include "dat/zone.h"
#include "gfx/cloth.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fmt/format.h>
#include <map>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

const xi::dat::ZoneData& zone230()
{
    static const xi::dat::ZoneData z = [] {
        const auto keys = xi::dat::ZoneKeys::fromDll(install()->installDir() / "FFXiMain.dll");
        return xi::dat::loadZone(*install(), 230, keys);
    }();
    return z;
}

} // namespace

TEST_CASE("event sub-maps (op 0x75 00) are zone-format models")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Northern San d'Oria's events ask for 359 / 360 / 361 (events 7, 37, 38, 50, 62): files 459..461.
    const auto keys = xi::dat::ZoneKeys::fromDll(install()->installDir() / "FFXiMain.dll");
    CHECK(xi::dat::zoneModelFileId(361) == 461);
    for (const uint16_t model : std::initializer_list<uint16_t>{ 359, 360, 361 })
    {
        const auto z = xi::dat::loadZone(*install(), model, keys);
        CHECK(!z.placements.empty());
        CHECK(!z.models.empty());
    }
}

TEST_CASE("zone LOD names: the last character above a space picks h / m / l (CODE 0x1017aef0)")
{
    CHECK(xi::dat::lodVariant("_ron_w10_m      ", 'h') == "_ron_w10_h      ");
    CHECK(xi::dat::lodVariant("treel", 'm') == "treem");
    CHECK(!xi::dat::lodVariant("_ron_w10_x      ", 'h'));
    CHECK(!xi::dat::lodVariant("h               ", 'm')); // index 0 is never a LOD letter
    CHECK(xi::dat::lodStem("abcm") == xi::dat::lodStem("abch"));
}

TEST_CASE("West Ronfaure's trees name their medium mesh and have high / low ones (record LOD distances)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto keys = xi::dat::ZoneKeys::fromDll(install()->installDir() / "FFXiMain.dll");
    const auto z    = xi::dat::loadZone(*install(), xi::dat::zoneModelId(100), keys);
    int        lod  = 0;
    for (const auto& pl : z.placements)
    {
        const auto h = xi::dat::lodVariant(pl.name, 'h');
        if (pl.name.starts_with("_ron_w10_m") && h && z.models.contains(*h))
        {
            ++lod;
            CHECK(pl.lodDist[0] == Catch::Approx(30.0f));
            CHECK(pl.lodDist[1] == Catch::Approx(50.0f));
        }
    }
    CHECK(lod > 0);
}

TEST_CASE("zone 230 geometry decodes to the verified totals")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto& z = zone230();
    CHECK(z.placements.size() == 1675);
    CHECK(z.models.size() == 256);
    std::size_t tris = 0, unresolved = 0;
    for (const auto& p : z.placements)
    {
        const auto it = z.models.find(p.name);
        if (it == z.models.end())
        {
            ++unresolved;
            continue;
        }
        for (const auto& pc : it->second.pieces)
        {
            tris += pc.triangles.size() / 3;
        }
    }
    CHECK(unresolved == 0);
    CHECK(tris == 204889);
    CHECK(z.collision.size() == 106462);
}

TEST_CASE("zone 230 textures all decode")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    std::vector<xi::dat::Chunk> chunks;
    REQUIRE(xi::dat::parseChunks(zone230().file, chunks));
    int ok = 0;
    for (const auto& c : chunks)
    {
        if (c.type == 0x20 || c.type == 0x1B)
        {
            CHECK_NOTHROW(xi::dat::parseTexture(c.payload));
            ++ok;
        }
    }
    CHECK(ok == 68);
}

TEST_CASE("zone 230 entity names come from DAT 6720+zone")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto names = xi::dat::loadEntityNames(*install(), 230);
    CHECK(names.at(0x10e6002) == "Aubejart");
    CHECK(names.at(0x10e6003) == "Ephauge");
}

TEST_CASE("zone lines: ids, compass and trigger box")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::ZoneLine* north = nullptr;
    const xi::dat::ZoneLine* east  = nullptr;
    for (const auto& l : zone230().zoneLines)
    {
        if (l.name() == "z6e4")
        {
            north = &l;
        }
        if (l.name() == "z6e0")
        {
            east = &l;
        }
    }
    REQUIRE(north);
    REQUIRE(east);
    CHECK(north->pos[2] > 50.0f); // Northern San d'Oria gate: +z is north
    CHECK(east->pos[0] > 100.0f); // East Ronfaure gate: +x is east
    // The game's test (CODE 0x10175820): centred box, half the stored size on every axis. The passage floor is at game
    // y -2.0 (the box: centre -8.47, size 15), the box 2 deep along z.
    CHECK(north->contains(0.0f, -2.0f, 58.0f));
    CHECK_FALSE(north->contains(0.0f, -2.0f, 50.0f));
    CHECK_FALSE(north->contains(0.0f, -17.0f, 58.0f)); // below the box
    // A fast step over the thin box (from z 56 to 60 in one go) still counts.
    const float from[3] = { 0.0f, -2.0f, 56.0f }, to[3] = { 0.0f, -2.0f, 60.0f };
    CHECK(north->crossedBy(from, to));
    const float away[3] = { 10.0f, -2.0f, 56.0f }, away2[3] = { 10.0f, -2.0f, 60.0f };
    CHECK_FALSE(north->crossedBy(away, away2));
}

TEST_CASE("collision floor under the LSB spawn point is y = -2")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Vertical ray at (-0.63, 54.37): find the floor (normal pointing up, i.e. -y in game space).
    const float x = -0.63f, z = 54.37f;
    float       best = 1e9f;
    for (const auto& t : zone230().collision)
    {
        if (t.normal[1] > -0.5f)
        {
            continue;
        }
        const auto& a = t.v[0];
        const auto& b = t.v[1];
        const auto& c = t.v[2];
        const float d  = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (std::abs(d) < 1e-9f)
        {
            continue;
        }
        const float w0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
        const float w1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0)
        {
            continue;
        }
        const float y = w0 * a[1] + w1 * b[1] + w2 * c[1];
        if (y > -5.0f && y < best)
        {
            best = y;
        }
    }
    CHECK(std::abs(best - (-2.0f)) < 0.01f);
}

#include "dat/character.h"
#include "dat/items.h"
#include "dat/messages.h"
#include "dat/motion.h"
#include "dat/event.h"
#include "dat/event_runner.h"
#include "dat/effect.h"
#include "dat/spells.h"

TEST_CASE("Hume male skeleton and idle/run motions load")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto asset = xi::dat::loadPcAsset(*install(), 1, 0, { 0x1000, 0x2008, 0x3008, 0x4008, 0x5008, 0, 0, 0 });
    REQUIRE(asset);
    CHECK(asset->skeleton.bones.size() == 94);
    CHECK(asset->vertices.size() / 3 == 1882);
    const auto motions = xi::dat::loadCharacterMotions(*install(), asset->motionFiles);
    CHECK(motions.idle.pieces.size() == 3);
    CHECK(motions.run.pieces.size() == 3);
    CHECK(motions.run.frames() == 11);

    // Posed skeleton stays upright: the head (most negative y, Y down) is ~1.7 above the feet in idle.
    std::vector<xi::dat::RowMatrix> posed;
    xi::dat::poseSkeleton(asset->skeleton, { &motions.idle }, 3.0f, posed);
    float minY = 0.0f;
    for (const auto& m : posed)
    {
        minY = std::min(minY, m[13]);
    }
    CHECK(minY < -1.5f);
    CHECK(minY > -2.0f);
}

TEST_CASE("motion cross-fade endpoints match the plain poses")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto asset = xi::dat::loadPcAsset(*install(), 1, 0, { 0x1000, 0x2008, 0x3008, 0x4008, 0x5008, 0, 0, 0 });
    REQUIRE(asset);
    const auto                      m = xi::dat::loadCharacterMotions(*install(), asset->motionFiles);
    std::vector<xi::dat::RowMatrix> idle, run, b0, b1;
    xi::dat::poseSkeleton(asset->skeleton, { &m.idle }, 4.0f, idle);
    xi::dat::poseSkeleton(asset->skeleton, { &m.run }, 2.0f, run);
    xi::dat::poseSkeletonBlend(asset->skeleton, &m.idle, 4.0f, &m.run, 2.0f, 0.0f, b0);
    xi::dat::poseSkeletonBlend(asset->skeleton, &m.idle, 4.0f, &m.run, 2.0f, 1.0f, b1);
    REQUIRE(b0.size() == idle.size());
    float e0 = 0, e1 = 0;
    for (std::size_t i = 0; i < idle.size(); ++i)
    {
        for (std::size_t k = 0; k < 16; ++k)
        {
            e0 = std::max(e0, std::abs(b0[i][k] - idle[i][k]));
            e1 = std::max(e1, std::abs(b1[i][k] - run[i][k]));
        }
    }
    CHECK(e0 < 1e-4f);
    CHECK(e1 < 1e-4f);
}

TEST_CASE("PC battle motions come from the weapon class files")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Hume male with a sword (model 22, class 1): pieces 0/1 at base + 2601 + 1, piece 2 at base + 2729 + 1.
    const auto hume = xi::dat::loadPcAsset(*install(), 1, 0, { 0x1000, 0x2008, 0x3008, 0x4008, 0x5008, 0x6016, 0x7000, 0x8000 });
    REQUIRE(hume);
    CHECK(hume->weaponClass == 1);
    CHECK(hume->battleMotionFiles == std::vector<uint32_t>{ 9674, 9802 });
    const auto m = xi::dat::loadCharacterMotions(*install(), hume->motionFiles, hume->battleMotionFiles);
    CHECK(m.battle.pieces.size() == 3);
    CHECK(m.attacks.size() == 3);
    CHECK_FALSE(m.draw.empty());
    CHECK_FALSE(m.sheathe.empty());

    // Galka with a great axe (model 95): the class is a per-race index (6 here, 5 on a Hume).
    const auto galka = xi::dat::loadPcAsset(*install(), 8, 0, { 0x101C, 0x201C, 0x3006, 0x401C, 0x5002, 0x605F, 0x7000, 0x8000 });
    REQUIRE(galka);
    CHECK(galka->weaponClass == 6);
    CHECK(galka->battleMotionFiles == std::vector<uint32_t>{ 26352 + 2601 + 6, 26352 + 2729 + 6 });
    // Face then head..feet: the face file holds the voices (`vdam` ...), the body file the impact sound `sdam`.
    REQUIRE(galka->gearFiles.size() == 6);
    CHECK(galka->gearFiles[0] == 26360);       // face 0
    CHECK(galka->gearFiles[2] == 26648 + 0x1C); // body 0x01C
}

TEST_CASE("fishing rods have no mesh of their own in the ranged models")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Rods are ranged models 0..11 (LSB item_equipment MId; Willow Fishing Rod = 10). For Hume male these are files
    // 9416 + n: an Os2 `wep2` of three coincident vertices and no primitives, so nothing of the rod draws from the item.
    const auto hume = xi::dat::loadPcAsset(*install(), 1, 0, { 0x1000, 0x2008, 0x3008, 0x4008, 0x5008, 0x6000, 0x7000, 0x800A });
    REQUIRE(hume);
    std::size_t rodTris = 0;
    for (const auto& part : hume->parts)
    {
        rodTris += part.wep == 2 ? part.count : 0;
    }
    CHECK(rodTris == 0);
}

TEST_CASE("battle messages render from the game's text tables")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::BattleText text(*install());
    using Lines = std::vector<std::string>;
    // Melee hit: player actor (no article), mob target ("the"), damage in number slot 1.
    CHECK(text.format(1, { "Botone", "Forest Hare", true, false, { 0, 37, 0, 0 } }) == Lines{ "Botone hits the Forest Hare for 37 points of damage." });
    CHECK(text.format(1, { "Forest Hare", "Botone", false, true, { 0, 1, 0, 0 } }) == Lines{ "The Forest Hare hits Botone for 1 point of damage." });
    // Critical hit: two display lines.
    CHECK(text.format(67, { "Botone", "Forest Hare", true, false, { 0, 80, 0, 0 } }).size() == 2);
    // The `05 k` names (file 7021): skills and /check ratings.
    CHECK(text.miscName(1) == "hand-to-hand");
    CHECK(text.miscName(66) == "like easy prey");
    // Skill-up (0x029: slot 0 = skill id, slot 1 = tenths).
    CHECK(text.format(38, { "Botone", "Botone", true, true, { 6, 2, 0, 0 } }) == Lines{ "Botone's great axe skill rises 0.2 points." });
    // Weapon skill name (ability table) and spell name.
    CHECK(text.abilityName(64) == "Raging Axe");
    CHECK(text.spellName(1) == "Cure");
    CHECK(text.format(43, { "Botone", "", true, false, { 0, 64, 0, 0 } }) == Lines{ "Botone readies Raging Axe." });
    // Monster TP moves (LSB mob skill ids, 256 and up) come from file 7035 at id - 256 (issue #56).
    CHECK(text.monsterAbilityName(257) == "Foot Kick");
    CHECK(text.monsterAbilityName(301) == "Dream Flower");
    CHECK(text.monsterAbilityName(323) == "Wild Carrot");
    CHECK(text.monsterAbilityName(334) == "Sharp Sting");
    CHECK(text.monsterAbilityName(256).empty()); // "#0" placeholder
    CHECK(text.format(43, { "Goblin Digger", "", false, false, { 0, 334, 0, 0 } }) == Lines{ "The Goblin Digger readies Sharp Sting." });
    CHECK(text.format(101, { "Tiny Mandragora", "", false, false, { 323, 0, 0, 0 } }) == Lines{ "The Tiny Mandragora uses Wild Carrot." });
    CHECK_FALSE(text.has(0)); // "dummy"
}

TEST_CASE("item names and equipment data come from the item DATs")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::ItemTable items(*install());
    const auto spatha = items.find(16611);
    REQUIRE(spatha);
    CHECK(spatha->name == "Bee Spatha +1");
    CHECK(spatha->stackSize == 1);
    const auto fire = items.find(4096);
    REQUIRE(fire);
    CHECK(fire->stackSize == 12); // Fire Crystal
    CHECK(spatha->equipment());
    CHECK(spatha->slots == 0x03); // main | sub
    const auto mail = items.find(12661);
    REQUIRE(mail);
    CHECK(mail->name == "Solid Mail");
    CHECK(mail->slots == 0x20); // body
    CHECK(mail->level == 10);
    const auto crystal = items.find(4096);
    REQUIRE(crystal);
    CHECK(crystal->name == "Fire Crystal");
    CHECK_FALSE(crystal->equipment());
    const auto scroll = items.find(4608);
    REQUIRE(scroll);
    CHECK(scroll->usable());
    CHECK(scroll->logName == "scroll of Cure");
    const auto coupon = items.find(536); // Adventurer Coupon: Rare
    REQUIRE(coupon);
    CHECK((coupon->flags & 0x8000) != 0);
    // #45: races, weapon skill, charges and targets.
    CHECK(mail->races == 0x1FE); // all races
    CHECK(spatha->skill == 3);   // sword
    const auto twicer = items.find(18216);
    REQUIRE(twicer);
    CHECK(twicer->skill == 6); // great axe
    const auto grip = items.find(19032); // Water Grip
    REQUIRE(grip);
    CHECK(grip->skill == 0);
    CHECK(grip->slots == 0x02);
    const auto ring = items.find(14672); // Tavnazian Ring: one charge a day
    REQUIRE(ring);
    CHECK(ring->maxCharges == 1);
    CHECK(ring->recast == 86400);
    CHECK(mail->maxCharges == 0);
    const auto potion = items.find(4112);
    REQUIRE(potion);
    CHECK(potion->targets == 0x01);
    CHECK((potion->flags & 0x0400) != 0); // usable from the menu
    INFO(scroll->description);
    CHECK(scroll->description.find("Cure") != std::string::npos);
    INFO(mail->description);
    CHECK_FALSE(mail->description.empty());
    // Icons: 32x32, in every item file range.
    for (const uint16_t id : std::initializer_list<uint16_t>{ 4096, 4608, 12661, 16611 })
    {
        const auto icon = items.icon(id);
        REQUIRE(icon);
        CHECK(icon->width == 32);
        CHECK(icon->height == 32);
        // Opaque texels store alpha 0x80 (the PS2 convention), never more: under the interface's MODULATE2X a vertex
        // alpha of 0x80 leaves them at half, so icons are drawn with blending off (UiPainter::icon, issue #90).
        const auto rgba = xi::dat::toRgba(*icon);
        int        maxA = 0, opaque = 0;
        for (std::size_t i = 3; i < rgba.size(); i += 4)
        {
            maxA = std::max<int>(maxA, rgba[i]);
            opaque += rgba[i] == 0x80;
        }
        CHECK(maxA == 0x80);
        CHECK(opaque > 32 * 32 / 4);
    }
}

TEST_CASE("pet commands are ability type 2 (file 85)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::AbilityTable abilities(*install());
    // Fight 69, Heel 70, Assault 88, Release 90, Deploy 138: the Pet Commands list; Provoke 35, Call Beast 85: Job Abilities.
    for (const uint16_t id : std::initializer_list<uint16_t>{ 69, 70, 88, 90, 138 })
    {
        const auto* a = abilities.find(id);
        REQUIRE(a);
        CHECK(a->type == 2);
    }
    for (const uint16_t id : std::initializer_list<uint16_t>{ 35, 85 })
    {
        const auto* a = abilities.find(id);
        REQUIRE(a);
        CHECK(a->type == 1);
    }
}

TEST_CASE("action ranges come from file 81 (mgc_ +0x45, comm +0x10)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    CHECK(xi::dat::rangeYalms(12) == 20);
    CHECK(xi::dat::rangeYalms(15) == 255);
    const xi::dat::SpellTable spells(*install());
    REQUIRE(spells.find(1));
    CHECK(spells.find(1)->range == 20);     // Cure
    const xi::dat::AbilityTable abilities(*install());
    CHECK(abilities.commandRange(32) == 3);        // Fast Blade (weapon skill 32)
    CHECK(abilities.commandRange(512 + 35) == 16); // Provoke
    CHECK(abilities.commandRange(512 + 16) == 255); // Mighty Strikes: no limit
}

TEST_CASE("command target masks come from file 81 (comm +0x0A)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::AbilityTable abilities(*install());
    CHECK(abilities.commandTargets(32) == 0x20);       // Fast Blade: enemies
    CHECK(abilities.commandTargets(512 + 35) == 0x20); // Provoke: enemies
    CHECK(abilities.commandTargets(512 + 16) == 0x01); // Mighty Strikes: yourself
}

TEST_CASE("spell data decodes (file 86)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::SpellTable spells(*install());
    const auto*               cure = spells.find(1);
    REQUIRE(cure);
    CHECK(cure->type == 1);     // white magic
    CHECK(cure->element == 6);  // light
    CHECK(cure->skill == 33);   // healing magic
    CHECK(cure->mp == 8);
    CHECK(cure->levels[3] == 1); // WHM
    CHECK(cure->levels[5] == 3); // RDM
    CHECK(cure->levels[7] == 5); // PLD
    CHECK(cure->usableBy(3, 1));
    CHECK_FALSE(cure->usableBy(1, 99)); // WAR
    const auto* stone = spells.find(159);
    REQUIRE(stone);
    CHECK(stone->levels[4] == 1); // BLM Stone at 1
    CHECK(stone->targets & 0x20);
    // List icons: element icon (file 86 +0x28) and the spell's own (file 81 "mgc_" +0x42).
    CHECK(cure->icon == 6);
    CHECK(cure->listIcon == 86);
    CHECK(stone->icon == 11);
    CHECK(stone->listIcon == 210);
    REQUIRE(spells.find(220));
    CHECK(spells.find(220)->listIcon == 223); // Poison
    // Area radius ("mgc_" +0x46 / +0x47): -ga spells 10 yalms, single-target ones none.
    CHECK(stone->aoeRadius == 0);
    CHECK(cure->aoeRadius == 0);
    REQUIRE(spells.find(189));
    CHECK(spells.find(189)->aoeRadius == 10); // Stonega
    CHECK(spells.find(7)->aoeRadius == 10);   // Curaga
}

TEST_CASE("zone dialog (NPC text) renders")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto f = xi::dat::zoneDialogFile(230);
    REQUIRE(f);
    CHECK(*f == 6650);
    const auto text = xi::dat::DialogTable::parse(install()->read(*f));
    using Lines     = std::vector<std::string>;
    CHECK(xi::dat::formatDialog(text.raw(0), {}) == Lines{ "The San d'Oria residential area is ahead. You'll find your Mog House within." });
    // A menu: prompt, then one line per option.
    // 0x0B (the options follow) is a marker line of its own.
    CHECK(xi::dat::formatDialog(text.raw(2), {}) == Lines{ "Care to learn about Mog Houses?", "\x0B", "Sure!", "No, thanks." });
    // An option chosen by a number: "renting a room in [San d'Oria/Bastok/Windurst...]" (message 10, number 1 = Bastok).
    xi::dat::DialogArgs args;
    args.numbers[0] = 1;
    const auto lines = xi::dat::formatDialog(text.raw(10), args);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines[0].find("Bastok") != std::string::npos);
    // Hantileon's chocobo lines: 0x08 our name, 01 05 84 op 0xC3's record name, 01 05 85 its value picks "[male/female]".
    xi::dat::DialogArgs c;
    c.player        = "Testone";
    c.eventNames[0] = { "Pecky", 1 };
    CHECK(xi::dat::formatDialog(text.raw(11191), c) == Lines{ "Hello, Testone." });
    CHECK(xi::dat::formatDialog(text.raw(11193), c) == Lines{ "Let us analyze Pecky's current condition..." });
    const auto sex = xi::dat::formatDialog(text.raw(11195), c);
    REQUIRE_FALSE(sex.empty());
    CHECK(sex[0].find("female!") != std::string::npos);
    CHECK(sex[0].find('[') == std::string::npos);
    // A fishing catch (S2C 0x027, Port Windurst 11624): 1C 00 the catcher's name (String1), 01 05 24 the item by num 0.
    const auto pw = xi::dat::zoneDialogFile(240);
    REQUIRE(pw);
    const auto          pwText = xi::dat::DialogTable::parse(install()->read(*pw));
    xi::dat::DialogArgs fish;
    fish.strings[0] = "Botfour";
    fish.numbers[0] = 4472; // crayfish
    fish.lookup     = [](uint16_t table, uint32_t value) { return table == 0x24 && value == 4472 ? std::string("a crayfish") : std::string(); };
    const auto caught = xi::dat::formatDialog(pwText.raw(11624), fish);
    REQUIRE_FALSE(caught.empty());
    CHECK(caught[0] == "Botfour caught a crayfish, but cannot carry any more items.");
}

TEST_CASE("interface sprite sheets and textures load (§14)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const uint32_t files[] = { xi::dat::kUiMenuFile, xi::dat::uiSkinFile(0) };
    const auto     ui      = xi::dat::UiResources::load(*install(), files);
    CHECK(ui.sheets.size() == 42); // 41 in the menu file + the skin's "win00"
    CHECK(ui.textures.contains("menu keytophd"));
    CHECK(ui.textures.contains("menu newtex")); // from the skin
    // HP digit colours: one quad per sprite, cut from the font texture.
    const auto* hp = ui.sprite("menu hpcol", 0);
    REQUIRE(hp);
    REQUIRE(hp->size() == 1);
    CHECK((*hp)[0].texture == "font font");
    CHECK((*hp)[0].w == 10);
    CHECK((*hp)[0].h == 11);
    CHECK((*hp)[0].u == 77);
    CHECK((*hp)[0].v == 26);
    REQUIRE(ui.sheets.contains("menu windowps"));
    CHECK(ui.sheets.at("menu windowps").sprites.size() == 845);
    // Every quad names a texture the two files provide, except "menu ustats" (only "menu ustatshd" ships).
    std::size_t missing = 0;
    for (const auto& [name, sheet] : ui.sheets)
    {
        for (const auto& sprite : sheet.sprites)
        {
            for (const auto& q : sprite)
            {
                missing += !ui.textures.contains(q.texture) && q.texture != "menu ustats";
            }
        }
    }
    CHECK(missing == 0);

    // Layouts: the main menu ("menu menuwind"), anchored to the right edge, 15 items starting with Status.
    CHECK(ui.layouts.size() == 404);
    REQUIRE(ui.layouts.contains("menu menuwind"));
    const auto& mm = ui.layouts.at("menu menuwind");
    CHECK(mm.window.x == 384);
    CHECK(mm.window.y == 48);
    CHECK(mm.window.w == 112);
    CHECK(mm.window.h == 202);
    CHECK(mm.window.anchor == 1);
    CHECK(mm.elements.size() == 15);
    const auto* status = mm.element(1);
    REQUIRE(status);
    CHECK(status->x == 16);
    CHECK(status->w == 88);
    const auto* label = status->subFor(0);
    REQUIRE(label);
    CHECK(label->sheet == "menu windowps");
    CHECK(label->shape == 13); // the "Status" button
    CHECK(status->text1 == 321);
    // "menu frames" is the language-neutral name of the English "menu framesus".
    const auto& yesno = ui.layouts.at("menu yesno");
    REQUIRE_FALSE(yesno.window.subs.empty());
    CHECK(yesno.window.subs[0].sheet == "menu framesus");

    // Help lines (the elements' first string id) and short titles (the second): files 55651 / 55652.
    const auto help = xi::dat::StringTable::parse(install()->read(55651));
    CHECK(help.get(321) == "View status.");
    CHECK(help.get(322) == "Equip weapons and armor.");
    // Area names by zone id: full (55465) and short (55661), as the party list shows them.
    CHECK(xi::dat::StringTable::parse(install()->read(55465)).get(230) == "Southern San d'Oria");
    CHECK(xi::dat::StringTable::parse(install()->read(55661)).get(230) == "S.San d'Oria");
}

TEST_CASE("zone map records and projection come from the user's FFXiMain.dll (§14)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::ZoneMaps maps(*install(), install()->installDir() / "FFXiMain.dll");
    REQUIRE(maps.size() > 800);
    const auto* r = maps.find(230, 0); // Southern San d'Oria
    REQUIRE(r);
    CHECK(r->fileId() == 5476);
    CHECK(r->scale == 4);
    CHECK(r->requiredKeyItem() == 385); // Map of the San d'Oria area (LSB MAP_OF_THE_SAN_DORIA_AREA)
    // A point by the South Auction House entrance (the client's own arithmetic gives pixel 300, 294 = grid I-9).
    const auto [u, v] = r->project(15.2f, -29.3f);
    CHECK(u == 300.0f);
    CHECK(v == 294.0f);
    const auto tex = maps.load(*r);
    REQUIRE(tex);
    CHECK(tex->width == 512);
}

TEST_CASE("8 bpp zone maps decode upright, and the player lands in the client's grid cell (#79)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::ZoneMaps maps(*install(), install()->installDir() / "FFXiMain.dll");
    const auto*             r = maps.find(117, 0); // Tahrongi Canyon: an 8 bpp paletted map (flags 0xB1), file 5330
    REQUIRE(r);
    CHECK(r->fileId() == 5330);
    const auto tex = maps.load(*r);
    REQUIRE(tex);
    REQUIRE(tex->width == 512);
    REQUIRE(tex->height == 512);
    // Upright, the title scroll is at the top and the compass rose at the bottom right (stored bottom-up, they come
    // out the other way round): count the compass's dark strokes in the same columns near the top and the bottom.
    const auto rgba = xi::dat::toRgba(*tex);
    const auto dark = [&](uint32_t y0, uint32_t y1) {
        int n = 0;
        for (uint32_t y = y0; y < y1; ++y)
        {
            for (uint32_t x = 380; x < 480; ++x)
            {
                const auto* px = &rgba[(std::size_t{ y } * tex->width + x) * 4];
                n += 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2] < 150 ? 1 : 0;
            }
        }
        return n;
    };
    CHECK(dark(372, 492) > dark(20, 140) * 3 / 2);
    // The bug report's position (443.5, -41.0) is grid K-8 in the client's own grid reference: 32-pixel cells from 16.
    const auto [u, v] = r->project(443.5f, -40.98f);
    CHECK(static_cast<int>(u - 16) / 32 == 10); // K
    CHECK(static_cast<int>(v - 16) / 32 + 1 == 8);
}

TEST_CASE("texture decode: non-DXT rows are stored bottom-up and come out top-down (CODE 0x10039d80)")
{
    // A 1x2 texture, row 0 as stored = palette 1 (red), row 1 = palette 2 (blue); 8 bpp then 32 bpp.
    const auto make = [](uint16_t bits) {
        std::vector<std::byte> p(0x39, std::byte{ 0 });
        p[0]           = std::byte{ 0x91 }; // kind 1
        const auto put = [&](std::size_t at, uint32_t v) {
            for (int i = 0; i < 4; ++i)
            {
                p[at + i] = std::byte{ static_cast<uint8_t>(v >> (8 * i)) };
            }
        };
        put(0x11, 40);
        put(0x11 + 4, 1);
        put(0x11 + 8, 2);
        p[0x11 + 0xE] = std::byte{ static_cast<uint8_t>(bits) };
        put(0x11 + 0x24, 32);
        if (bits == 8)
        {
            std::vector<std::byte> pal(256 * 4, std::byte{ 0 });
            pal[1 * 4 + 2] = std::byte{ 0xFF }; // BGRA: red
            pal[2 * 4 + 0] = std::byte{ 0xFF }; // blue
            p.insert(p.end(), pal.begin(), pal.end());
            p.push_back(std::byte{ 1 });
            p.push_back(std::byte{ 2 });
        }
        else
        {
            for (const uint32_t bgra : { 0x00FF0000u, 0x000000FFu }) // red row, then blue row
            {
                for (int i = 0; i < 4; ++i)
                {
                    p.push_back(std::byte{ static_cast<uint8_t>(bgra >> (8 * i)) });
                }
            }
        }
        p.resize(p.size() + 16, std::byte{ 0 });
        return p;
    };
    for (const uint16_t bits : { uint16_t{ 8 }, uint16_t{ 32 } })
    {
        const auto tex  = xi::dat::parseTexture(make(bits));
        const auto rgba = xi::dat::toRgba(tex);
        REQUIRE(rgba.size() == 8);
        CHECK(rgba[2] == 0xFF); // top row: blue (the last stored row)
        CHECK(rgba[0] == 0);
        CHECK(rgba[4] == 0xFF); // bottom row: red
        CHECK(rgba[6] == 0);
    }
}

TEST_CASE("weapon skill motions: the animation id picks per-race files, the main scheduler the stems (§10.8)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Galka (race 8), Fast Blade (animation 1): files 37836 / 38092; "main" plays b00? then b02?.
    const auto sets = xi::dat::loadWeaponSkillMotions(*install(), 8, 1);
    REQUIRE(sets.size() == 2);
    CHECK(sets[0].pieces.size() == 3);
    CHECK(sets[1].pieces.size() == 3);
    CHECK(sets[1].frames() > sets[0].frames()); // the strike is longer than the wind-up
}

TEST_CASE("monster TP move motions come from the effect file's main scheduler (§10.8)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Foot Kick (animation 1, effect file 3901) plays "wz20", a full motion name stored in the effect file.
    const auto sets = xi::dat::loadMonsterSkillMotions(*install(), 1, {});
    REQUIRE_FALSE(sets.empty());
    CHECK(sets[0].frames() > 0);
}

TEST_CASE("key item names come from d_msg 55695 by the id field, not the entry order")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto t = xi::dat::KeyItemTable::parse(install()->read(xi::dat::KeyItemTable::kFile));
    CHECK(t.size() > 2000);
    REQUIRE(t.find(35));
    CHECK(t.find(35)->name == "Adventurer's Certificate"); // entry 1639
    REQUIRE(t.find(1));
    CHECK(t.find(1)->name == "Zeruhn report");
    CHECK(t.find(1)->description.find('\n') == std::string::npos);
}

TEST_CASE("emote motions: race base + 2984 + group, stems by name (§10.9)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // /bow (1) for Hume male: file 10056 (bow0, bow1) and 10120 (bow2).
    CHECK(xi::dat::loadEmoteMotion(*install(), 1, 1).pieces.size() == 3);
    // Dance1 (65) for Galka.
    CHECK(xi::dat::loadEmoteMotion(*install(), 8, 65).pieces.size() == 3);
    // /smile has no motion stem.
    CHECK(xi::dat::loadEmoteMotion(*install(), 1, 15).empty());
}

TEST_CASE("quest and mission names: d_msg 55706.. by log, id in field 0")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto bastok = xi::dat::LogNameTable::parse(install()->read(xi::dat::LogNameTable::kQuestFiles[1]));
    REQUIRE(bastok.find(15));
    CHECK(bastok.find(15)->name == "Smoke on the Mountain");
    const auto sandy = xi::dat::LogNameTable::parse(install()->read(xi::dat::LogNameTable::kMissionFiles[0]));
    REQUIRE(sandy.find(3));
    CHECK(sandy.find(3)->name == "The Rescue Drill");
    const auto zilart = xi::dat::LogNameTable::parse(install()->read(xi::dat::LogNameTable::kMissionFiles[3]));
    CHECK_FALSE(zilart.find(1)); // a Japanese placeholder entry is skipped
}

TEST_CASE("event scripts: Leuveret's dialog and The Pickpocket's menu (docs/event-format.md)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    using xi::dat::EventVm;
    // Leuveret (zone 230, UniqueNo 0x010E6014), LSB startEvent(621): three messages, each waiting for the window.
    auto s = xi::dat::EventScript::find(*install(), 230, 0x010E6014, 621);
    REQUIRE(s);
    EventVm vm(*s, {});
    std::vector<uint32_t> messages;
    for (int i = 0; i < 20; ++i)
    {
        const auto st = vm.run();
        if (st.stop == EventVm::Stop::Message)
        {
            messages.push_back(st.message);
        }
        else if (st.stop != EventVm::Stop::WaitClose && st.stop != EventVm::Stop::Face)
        {
            CHECK(st.stop == EventVm::Stop::End);
            break;
        }
    }
    CHECK(messages == std::vector<uint32_t>{ 7882, 7883, 7884 });
    // The Pickpocket (0x010E6021), event 550: a message, then a Yes/No menu; "No" (option 1) ends with g1 = 1.
    auto p = xi::dat::EventScript::find(*install(), 230, 0x010E6021, 550);
    REQUIRE(p);
    EventVm m(*p, {});
    auto first = m.run();
    while (first.stop == EventVm::Stop::Face)
    {
        first = m.run();
    }
    CHECK(first.message == 8342);
    CHECK(m.run().stop == EventVm::Stop::WaitClose);
    const auto menu = m.run();
    REQUIRE(menu.stop == EventVm::Stop::Menu);
    CHECK(menu.message == 8343);
    m.choose(1);
    auto st = m.run();
    while (st.stop == EventVm::Stop::Message || st.stop == EventVm::Stop::WaitClose || st.stop == EventVm::Stop::Face)
    {
        st = m.run();
    }
    CHECK(st.stop == EventVm::Stop::End);
    CHECK(m.result() == 1);
}

TEST_CASE("entity names for zones from 256 come from 86235 + zone")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto names = xi::dat::loadEntityNames(*install(), 258); // Rala Waterways
    REQUIRE(!names.empty());
    for (const auto& [id, name] : names)
    {
        CHECK(((id >> 12) & 0xFFF) == 258u);
        break;
    }
}

TEST_CASE("engaged walk / run take the weapon class file's upper-body pieces")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Hume male with a main-hand weapon model (the class files come from the weapon's info chunk, §10.6).
    const auto asset = xi::dat::loadPcAsset(*install(), 1, 0, { 0x1000, 0x2008, 0x3008, 0x4008, 0x5008, 0x6000 + 1, 0, 0 });
    REQUIRE(asset);
    if (asset->battleMotionFiles.empty())
    {
        SKIP("no weapon class files for this look");
    }
    const auto m = xi::dat::loadCharacterMotions(*install(), asset->motionFiles, asset->battleMotionFiles);
    CHECK(!m.battleRun.empty());
    CHECK(m.battleRun.pieces.size() == m.run.pieces.size());
}

TEST_CASE("Os2 materials: light scale, env and specular reflections (dat-format §10.3)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Hume male: head 223 (file 7335, `hum_sun`, env), body 36 (file 7404, metal, specular), face 1 (light scale).
    const auto asset = xi::dat::loadPcAsset(*install(), 1, 1, { 0x1000 + 223, 0x2000 + 36, 0x3008, 0x4008, 0x5008, 0, 0, 0 });
    REQUIRE(asset);
    bool env = false, spec = false, bright = false;
    for (const auto& p : asset->parts)
    {
        env    = env || (p.env && p.strength > 0.2f);
        spec   = spec || (p.spec && p.strength > 0.0f);
        bright = bright || p.lightScale > 1.0f;
    }
    CHECK(env);
    CHECK(spec);
    CHECK(bright);
    CHECK(!asset->envTexture.empty());                    // the head file's `evev` chunk
    CHECK(asset->specTexture.starts_with("cubemap spec")); // file 0's highlight
}

TEST_CASE("zone dialog from 256: 85335 + zone, English")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto f = xi::dat::zoneDialogFile(256);
    REQUIRE(f);
    CHECK(*f == 85591u);
    const auto t = xi::dat::DialogTable::parse(install()->read(*f));
    CHECK(t.size() > 1000);
}

TEST_CASE("standard messages (file 7031, LSB MsgStd) with text arguments")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto t = xi::dat::DialogTable::parse(install()->read(7031));
    using Lines  = std::vector<std::string>;
    CHECK(xi::dat::formatDialog(t.raw(14), {}) == Lines{ "That request cannot be processed." });
    xi::dat::DialogArgs args;
    args.strings[2] = "Bottwo";
    args.strings[3] = "540";
    CHECK(xi::dat::formatDialog(t.raw(88), args) == Lines{ "Dice roll! Bottwo rolls 540!" }); // MsgStd::DiceRoll
}

TEST_CASE("emote lines come from file 7025 with names, gender and article")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto t = xi::dat::DialogTable::parse(install()->read(7025));
    using Lines  = std::vector<std::string>;
    xi::dat::DialogArgs a;
    a.actor  = "Bottwo";
    a.target = "Forest Hare";
    CHECK(xi::dat::formatDialog(t.raw(2 * 1), a) == Lines{ "Bottwo bows courteously to the Forest Hare." });
    a.targetNoArticle = true;
    a.target          = "Botthree";
    CHECK(xi::dat::formatDialog(t.raw(2 * 1), a) == Lines{ "Bottwo bows courteously to Botthree." });
    a.female = true;
    CHECK(xi::dat::formatDialog(t.raw(2 * 6 + 1), a) == Lines{ "Bottwo shakes her head." });
    CHECK(xi::dat::formatDialog(t.raw(2 * 5 + 1), a) == Lines{ "Bottwo's eyes brim over with tears." });
}

TEST_CASE("zone dialog item names: the fishing catch line (TALKNUMWORK2) and \"You obtain\"")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    auto text = std::make_shared<xi::dat::BattleText>(*install());
    text->setItems(std::make_shared<const xi::dat::ItemTable>(*install()));
    using Lines = std::vector<std::string>;
    xi::dat::DialogArgs args;
    args.lookup     = [&](uint16_t table, uint32_t value) { return text->dialogName(table, value); };
    args.strings[0] = "Bottwo";
    args.numbers[0] = 4472; // Crayfish
    const auto ronfaure = xi::dat::DialogTable::parse(install()->read(6420 + 100));
    CHECK(xi::dat::formatDialog(ronfaure.raw(7292), args) == Lines{ "Bottwo caught a crayfish!" });
    const auto sandoria = xi::dat::DialogTable::parse(install()->read(6420 + 230));
    CHECK(xi::dat::formatDialog(sandoria.raw(6444), args) == Lines{ "You obtain a crayfish!" });
    args.numbers[1] = 3;
    CHECK(xi::dat::formatDialog(sandoria.raw(6444), args) == Lines{ "You obtain 3 crayfish!" });
    args.numbers[0] = 35; // a key item
    CHECK(xi::dat::formatDialog(sandoria.raw(6441), args) == Lines{ "Obtained key item: Adventurer's Certificate." });
}

TEST_CASE("auto-translate phrases (55665) expand in chat")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto at = xi::dat::AutoTranslate::parse(install()->read(xi::dat::AutoTranslate::kFile));
    CHECK(at.size() > 2700); // 2685 phrases + 42 group names
    CHECK(at.find(0x02020101) == "Nice to meet you.");
    const std::string chat = "Hi! \xFD\x02\x02\x01\x01\xFD See you.";
    CHECK(at.expand(chat) == "Hi! {Nice to meet you.} See you.");
    // The log keeps a token between the game's gaiji brackets, so typed braces stay plain.
    CHECK(at.expandMarked(chat) == "Hi! \xEF\x27Nice to meet you.\xEF\x28 See you.");
    CHECK(xi::dat::AutoTranslate::plainMarks(at.expandMarked(chat) + " {typed}") == "Hi! {Nice to meet you.} See you. {typed}");
    CHECK(at.encode("Hi! {nice to meet you.} {not a phrase}") == "Hi! \xFD\x02\x02\x01\x01\xFD {not a phrase}");
}

TEST_CASE("auto-translate tokens: item / key item escapes and chat-line editing")
{
    using xi::dat::AutoTranslate;
    namespace ce = xi::dat::chatedit;
    CHECK(AutoTranslate::phraseToken(0x02020101) == std::string("\xFD\x02\x02\x01\x01\xFD", 6));
    CHECK(AutoTranslate::itemToken(0x1234) == std::string("\xFD\x07\x02\x12\x34\xFD", 6));
    CHECK(AutoTranslate::itemToken(0x0011) == std::string("\xFD\x09\x02\xFF\x11\xFD", 6)); // high byte 0
    CHECK(AutoTranslate::itemToken(0x1200) == std::string("\xFD\x0A\x02\x12\xFF\xFD", 6)); // low byte 0
    CHECK(AutoTranslate::keyItemToken(0x0023) == std::string("\xFD\x15\x02\xFF\x23\xFD", 6));
    for (const uint16_t id : { uint16_t{ 0x0011 }, uint16_t{ 0x1200 }, uint16_t{ 0x1234 } })
    {
        const auto t = AutoTranslate::tokenAt(AutoTranslate::itemToken(id), 0);
        REQUIRE(t);
        CHECK(t->kind == AutoTranslate::Kind::Item);
        CHECK(t->id == id);
        const auto k = AutoTranslate::tokenAt(AutoTranslate::keyItemToken(id), 0);
        REQUIRE(k);
        CHECK(k->kind == AutoTranslate::Kind::KeyItem);
        CHECK(k->id == id);
    }
    const auto p = AutoTranslate::tokenAt(AutoTranslate::phraseToken(0x02020101), 0);
    REQUIRE(p);
    CHECK(p->kind == AutoTranslate::Kind::Phrase);
    CHECK(p->id == 0x02020101);

    // "ab" + token + "c": the cursor steps over the token whole; Backspace / Delete remove it whole.
    std::string line = "ab" + AutoTranslate::phraseToken(0x02020101) + "c";
    CHECK(ce::stepRight(line, 2) == 8);
    CHECK(ce::stepLeft(line, 8) == 2);
    CHECK(ce::stepLeft(line, 9) == 8);
    CHECK(ce::wordStart(line, 9) == 8);  // the word after a token starts at the token's end
    CHECK(ce::wordStart("hello wor", 9) == 6);
    std::string a = line;
    CHECK(ce::eraseLeft(a, 8) == 2);
    CHECK(a == "abc");
    std::string b = line;
    CHECK(ce::eraseRight(b, 2) == 2);
    CHECK(b == "abc");
}

TEST_CASE("auto-translate groups and @ references resolve to names")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const xi::dat::BattleText text(*install());
    const auto*               at = text.autoTranslate();
    REQUIRE(at);
    REQUIRE(at->groups().size() == 42);
    CHECK(at->groups()[0].name == "Greetings");
    CHECK(at->groups()[0].entries.front().text == "Nice to meet you.");
    // Jobs "@J..", places "@A68" (Jugner Forest), spells "@C2B" (Protect), abilities: no raw references remain.
    CHECK(at->find(0x02021401).front() != '@');
    CHECK(at->find(0x02021b01) == "Protect");
    for (const auto& g : at->groups())
    {
        for (const auto& e : g.entries)
        {
            CHECK(e.text.front() != '@');
        }
    }
    const auto hits = at->search("nice to");
    REQUIRE(!hits.empty());
    CHECK(hits.front().id == 0x02020101);
    CHECK(text.expandAutoTranslate(xi::dat::AutoTranslate::keyItemToken(35)) == "{Adventurer's Certificate}");
}

TEST_CASE("effect file 37836 (Galka Fast Blade): blocks, generators, meshes, keyframes")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    using namespace xi::dat;
    const EffectFile fx(*install(), 37836);
    const auto*      main = fx.find(FxType::Scheduler, fxName("main"));
    REQUIRE(main);
    REQUIRE(fx.find(FxType::Generator, fxName("g000"), main->block));
    const auto* d3m = fx.find(FxType::D3m, fxName("0113"));
    REQUIRE(d3m);
    const auto* m = fx.mesh(*d3m);
    REQUIRE(m);
    CHECK(!m->vertices.empty());
    CHECK(m->vertices.size() % 3 == 0);
    REQUIRE(!m->groups.empty());
    CHECK(fx.texture(m->groups[0].texName) != nullptr);
    const auto* d3a = fx.find(FxType::D3a, fxName("0110"));
    REQUIRE(d3a);
    CHECK(fx.mesh(*d3a)->vertices.size() == 6); // one quad (§M.3)
    const auto* k000 = fx.find(FxType::KeyFrame, fxName("k000"));
    REQUIRE(k000);
    const auto curve = FxCurve::parse(k000->payload);
    REQUIRE(curve.keys.size() == 3);
    CHECK(curve.eval(0.675f, 0.0f, false) == Catch::Approx(0.502f).margin(0.001));
    CHECK(curve.eval(1.0f, 0.0f, false) == Catch::Approx(0.0f));
}

TEST_CASE("zone 230 doors: two leaves per id and a Rid box at the door entity")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // `_6eq`, Door:Rosel's Armour (LSB door_id 1902458463, at (89.089, -4.301, 37.999)): dat-format §5.8.
    const uint32_t id     = static_cast<uint32_t>('_') | 'e' << 16 | 'q' << 24 | '6' << 8;
    int            leaves = 0;
    for (const auto& pl : zone230().placements)
    {
        leaves += pl.doorId == id ? 1 : 0;
    }
    CHECK(leaves == 2);
    const xi::dat::ZoneLine* box = nullptr;
    for (const auto& b : zone230().doorBoxes)
    {
        box = b.id == id ? &b : box;
    }
    REQUIRE(box);
    CHECK(std::abs(box->pos[0] - 89.089f) < 0.05f);
    CHECK(std::abs(box->pos[1] - -4.301f) < 0.05f);
    CHECK(std::abs(box->pos[2] - 37.999f) < 0.05f);
    CHECK(box->size[2] < 0.5f); // thin across the doorway
}

TEST_CASE("the San d'Oria intro (event 503) runs every actor's threads to the end")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // event-format §6: shots s043.. from DAT 30834 and fades from 30904; messages closed and menus answered at once.
    struct Host : xi::dat::EventHost
    {
        std::map<uint32_t, xi::dat::EventVec> pos;
        std::vector<std::string>              schedulers;
        int                                   messages = 0;
        xi::dat::EventRunner*                 runner   = nullptr;
        uint32_t player() const override { return 0x1000; }
        bool exists(uint32_t) const override { return true; }
        void request(uint32_t) override {}
        xi::dat::EventVec position(uint32_t id) const override
        {
            const auto it = pos.find(id);
            return it != pos.end() ? it->second : xi::dat::EventVec{};
        }
        void place(uint32_t id, const xi::dat::EventVec& p, std::optional<float>, bool) override { pos[id] = p; }
        void face(uint32_t, float) override {}
        void walking(uint32_t, float) override {}
        void hide(uint32_t, bool) override {}
        void motion(uint32_t, uint32_t, uint32_t, int, int) override {}
        bool motionBusy(uint32_t) const override { return false; }
        void idle(uint32_t, uint32_t) override {}
        void lookAt(uint32_t, uint32_t, int) override {}
        void lookAngles(uint32_t, int, int) override {}
        void lookStop(uint32_t) override {}
        void costume(uint32_t, int, uint16_t) override {}
        void scale(uint32_t, float) override {}
        void scheduler(uint32_t file, uint32_t, uint32_t, uint32_t fcc) override
        {
            schedulers.push_back(std::to_string(file) + ":" + xi::dat::fxNameString(fcc));
        }
        void stopScheduler(uint32_t, uint32_t, uint32_t) override {}
        bool schedulerRunning(uint32_t, uint32_t, uint32_t) const override { return false; }
        void cameraMode(int) override {}
        void music(int, uint16_t) override {}
        void sound(uint32_t) override {}
        void message(uint32_t, uint32_t) override { ++messages; }
        void menu(uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void update(uint32_t) override { runner->updated({}); }
    } host;
    auto file = std::make_shared<const xi::dat::EventFile>(xi::dat::EventFile::load(*install(), 230));
    xi::dat::EventRunner runner(file, 0x7FFFFFF0u, 503, {}, host);
    host.runner = &runner;
    REQUIRE(runner.valid());
    for (int frame = 0; frame < 60 * 600 && !runner.finished(); ++frame)
    {
        runner.tick(1.0f);
        runner.closeMessage();
        runner.choose(0);
    }
    CHECK(runner.finished());
    CHECK(runner.skipped().empty());
    CHECK(host.messages == 44);
    REQUIRE(host.schedulers.size() >= 2);
    CHECK(host.schedulers[0] == "30834:s043");
    CHECK(host.schedulers[1] == "30904:fdi1");
    // A guard was placed at z = 65 and set marching toward z = 15 (entry 2 of 0x010E6068), then frozen when its shot
    // ended (op 0x2F): here, with messages closing at once, it has only just left.
    CHECK(host.pos[0x010E6068].z < 65.0f);
    CHECK(host.pos[0x010E6068].z > 15.0f);
}

TEST_CASE("an event update answered without parameters (S2C 0x052 mode 1) lets the script go on: the RoV opening, 231 / 30035")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // event-format §6: shots s043.. from DAT 30834 and fades from 30904; messages closed and menus answered at once.
    struct Host : xi::dat::EventHost
    {
        std::map<uint32_t, xi::dat::EventVec> pos;
        std::vector<std::string>              schedulers;
        int                                   messages = 0, updates = 0;
        xi::dat::EventRunner*                 runner   = nullptr;
        uint32_t player() const override { return 0x1000; }
        bool exists(uint32_t) const override { return true; }
        void request(uint32_t) override {}
        xi::dat::EventVec position(uint32_t id) const override
        {
            const auto it = pos.find(id);
            return it != pos.end() ? it->second : xi::dat::EventVec{};
        }
        void place(uint32_t id, const xi::dat::EventVec& p, std::optional<float>, bool) override { pos[id] = p; }
        void face(uint32_t, float) override {}
        void walking(uint32_t, float) override {}
        void hide(uint32_t, bool) override {}
        void motion(uint32_t, uint32_t, uint32_t, int, int) override {}
        bool motionBusy(uint32_t) const override { return false; }
        void idle(uint32_t, uint32_t) override {}
        void lookAt(uint32_t, uint32_t, int) override {}
        void lookAngles(uint32_t, int, int) override {}
        void lookStop(uint32_t) override {}
        void costume(uint32_t, int, uint16_t) override {}
        void scale(uint32_t, float) override {}
        void scheduler(uint32_t file, uint32_t, uint32_t, uint32_t fcc) override
        {
            schedulers.push_back(std::to_string(file) + ":" + xi::dat::fxNameString(fcc));
        }
        void stopScheduler(uint32_t, uint32_t, uint32_t) override {}
        bool schedulerRunning(uint32_t, uint32_t, uint32_t) const override { return false; }
        void cameraMode(int) override {}
        void music(int, uint16_t) override {}
        void sound(uint32_t) override {}
        void message(uint32_t, uint32_t) override { ++messages; }
        void menu(uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void update(uint32_t) override { ++updates; } // LSB: no onEventUpdate, so only 0x052 answers
    } host;
    auto file = std::make_shared<const xi::dat::EventFile>(xi::dat::EventFile::load(*install(), 231));
    xi::dat::EventRunner runner(file, 0x7FFFFFF0u, 30035, {}, host);
    host.runner = &runner;
    REQUIRE(runner.valid());
    for (int frame = 0; frame < 60 * 600 && !runner.finished(); ++frame)
    {
        runner.tick(1.0f);
        runner.released(); // the server's answer, a frame later
        runner.closeMessage();
        runner.choose(0);
    }
    CHECK(runner.finished());
    CHECK(host.updates >= 1);
    CHECK(host.messages >= 3);
}

TEST_CASE("event ops 0xB6 (costume, one per frame) and 0x79 / 0x7B (look-at)")
{
    struct Host : xi::dat::EventHost
    {
        std::vector<std::string> log;
        uint32_t player() const override { return 0x1000; }
        bool exists(uint32_t) const override { return true; }
        void request(uint32_t) override {}
        xi::dat::EventVec position(uint32_t) const override { return {}; }
        void place(uint32_t, const xi::dat::EventVec&, std::optional<float>, bool) override {}
        void face(uint32_t, float) override {}
        void walking(uint32_t, float) override {}
        void hide(uint32_t, bool) override {}
        void motion(uint32_t, uint32_t, uint32_t, int, int) override {}
        bool motionBusy(uint32_t) const override { return false; }
        void idle(uint32_t, uint32_t) override {}
        void lookAt(uint32_t id, uint32_t t, int pt) override { log.push_back(fmt::format("look {:x} {:x} {}", id, t, pt)); }
        void lookAngles(uint32_t id, int p, int y) override { log.push_back(fmt::format("angles {:x} {} {}", id, p, y)); }
        void lookStop(uint32_t id) override { log.push_back(fmt::format("stop {:x}", id)); }
        void costume(uint32_t id, int slot, uint16_t v) override { log.push_back(fmt::format("costume {:x} {} {}", id, slot, v)); }
        void scale(uint32_t id, float k) override { log.push_back(fmt::format("scale {:x} {:.2f}", id, k)); }
        void scheduler(uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void stopScheduler(uint32_t, uint32_t, uint32_t) override {}
        bool schedulerRunning(uint32_t, uint32_t, uint32_t) const override { return false; }
        void cameraMode(int) override {}
        void music(int, uint16_t) override {}
        void sound(uint32_t) override {}
        void message(uint32_t, uint32_t) override {}
        void menu(uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void update(uint32_t) override {}
    } host;
    xi::dat::EventFile f;
    auto&              b = f.blocks.emplace_back();
    b.id                 = 0x7FFFFFF0u;
    b.entryOff           = { 0 };
    b.entryId            = { 37 };
    b.consts             = { 31, 20, 0, 150, 512, 0 };
    // b6 0b: race #31, face #20, head..feet #20, main / sub #0; b6 0f #150; 79 00 PLAYER npc; 79 02 PLAYER #512 #0; 7b PLAYER; 21
    b.code = { 0xB6, 0x0B, 0x00, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x02, 0x80, 0x02, 0x80,
               0xB6, 0x0F, 0x03, 0x80,
               0x79, 0x00, 0xF0, 0xFF, 0xFF, 0x7F, 0x20, 0x70, 0x0E, 0x01,
               0x79, 0x02, 0xF0, 0xFF, 0xFF, 0x7F, 0x04, 0x80, 0x05, 0x80,
               0x7B, 0xF0, 0xFF, 0xFF, 0x7F,
               0x21 };
    xi::dat::EventRunner runner(std::make_shared<const xi::dat::EventFile>(f), 0x7FFFFFF0u, 37, {}, host);
    REQUIRE(runner.valid());
    runner.tick(1.0f);
    REQUIRE(host.log.size() == 9); // the costume, then the frame ends
    CHECK(host.log[0] == "costume 1000 -1 31");
    CHECK(host.log[1] == "costume 1000 0 20");
    CHECK(host.log[8] == "costume 1000 7 0");
    runner.tick(1.0f);
    REQUIRE(host.log.size() == 10);
    CHECK(host.log[9] == "scale 1000 1.50");
    runner.tick(1.0f);
    CHECK(runner.finished());
    REQUIRE(host.log.size() == 13);
    CHECK(host.log[10] == "look 1000 10e7020 6");
    CHECK(host.log[11] == "angles 1000 512 0");
    CHECK(host.log[12] == "stop 1000");
}

TEST_CASE("chocobo races 32..36: the choc skeleton, five slot files and the chi idle")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Southern San d'Oria's stable chocobo (0x00E kind 7, face 32, body 1): yellow, no saddle.
    const auto yellow = xi::dat::loadPcAsset(*install(), 32, 0, { 0, 1, 0, 0, 0, 0, 0, 0 });
    REQUIRE(yellow);
    CHECK(yellow->skeleton.bones.size() == 58);
    CHECK(yellow->vertices.size() > 300);
    CHECK(yellow->motionFiles == std::vector<uint32_t>{ 55295 });
    const auto m = xi::dat::loadCharacterMotions(*install(), yellow->motionFiles);
    CHECK(m.idle.pieces.size() == 1); // chi0
    CHECK(m.idle.frames() == 31);
    CHECK(!m.run.empty());
    for (uint8_t race = 33; race <= 36; ++race)
    {
        CHECK(xi::dat::loadPcAsset(*install(), race, 0, { 0, 7, 0, 0, 0, 0, 0, 0 }).has_value());
    }
}

TEST_CASE("draw / sheathe motions follow the weapon's sheath style")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Galka (race 8) swords, class 1: style 0 ("in 0") draws with inc?, style 1 ("in 1") with ind? (sheath_re.md);
    // style 2 has no scheduler and falls back to style 0.
    const uint32_t base = 26352; // Galka (dat-format §10.1)
    const std::vector<uint32_t> cls{ base + 2601 + 1, base + 2729 + 1 };
    const auto s0 = xi::dat::loadCharacterMotions(*install(), {}, cls, 0);
    const auto s1 = xi::dat::loadCharacterMotions(*install(), {}, cls, 1);
    const auto s2 = xi::dat::loadCharacterMotions(*install(), {}, cls, 2);
    REQUIRE(!s0.draw.empty());
    REQUIRE(!s1.draw.empty());
    CHECK(s0.drawStem == "inc");
    CHECK(s1.drawStem == "ind");
    CHECK(s2.drawStem == "inc");
    CHECK(s1.sheatheStem == "otd");
    CHECK(!s1.sheathe.empty());
}

TEST_CASE("a drawn weapon follows the slot the battle stance holds in the hand")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Hume male, Bronze Dagger (look 164) and Bronze Sword (268). The dagger's mesh hangs from sheath slot 10, which
    // the dagger class's btl leaves at the hip; that class holds slot 17 in the grip. The sword class holds the sword's
    // own slot 5 in the grip, so it needs no switch.
    auto drawn = [](uint16_t look, int& bone) {
        const auto a = xi::dat::loadPcAsset(*install(), 1, 1, { 0, 8, 8, 8, 8, look, 0, 0 });
        REQUIRE(a);
        bone = a->mainWeaponBone;
        const auto m = xi::dat::loadCharacterMotions(*install(), a->motionFiles, a->battleMotionFiles, a->sheathStyle);
        return xi::dat::drawnWeaponBone(a->skeleton, m.battle, a->mainWeaponBone);
    };
    int bone = -1;
    CHECK(drawn(164, bone) == 17);
    CHECK(bone == 10);
    CHECK(drawn(268, bone) == -1);
    CHECK(bone == 5);
    // Every race's dagger ends up on the grip bone in the battle stance (Elvaan M / F included).
    for (uint8_t race = 1; race <= 8; ++race)
    {
        CAPTURE(race);
        const auto a = xi::dat::loadPcAsset(*install(), race, 1, { 0, 8, 8, 8, 8, 164, 0, 0 });
        REQUIRE(a);
        const auto m    = xi::dat::loadCharacterMotions(*install(), a->motionFiles, a->battleMotionFiles, a->sheathStyle);
        const int  held = xi::dat::drawnWeaponBone(a->skeleton, m.battle, a->mainWeaponBone);
        const int  b    = held >= 0 ? held : a->mainWeaponBone;
        std::vector<xi::dat::RowMatrix> posed;
        xi::dat::poseSkeleton(a->skeleton, { &m.battle }, 0.0f, posed);
        const int   grip = a->skeleton.points[0x7F].bone;
        const auto& p    = posed[static_cast<std::size_t>(b)];
        const auto& g    = posed[static_cast<std::size_t>(grip)];
        CHECK(std::hypot(p[12] - g[12], p[13] - g[13], p[14] - g[14]) < 0.1f);
    }
}

TEST_CASE("a mount's camera height comes from its info chunk byte 14")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    CHECK(xi::dat::mountCameraHeight(*install(), 39275) == 0.0f);  // rented chocobo: 0xFF
    CHECK(xi::dat::mountCameraHeight(*install(), 102705) == 0.0f); // the first mount: 0xFF
    CHECK(xi::dat::mountCameraHeight(*install(), 102717) == Catch::Approx(1.5f));
    // The camera's model heights: the Sk2 bounds sets (Hume M: standing 1.9, riding 2.1, lying 0.6).
    const auto a = xi::dat::loadPcAsset(*install(), 1, 1, { 0, 8, 8, 8, 8, 0, 0, 0 });
    REQUIRE(a);
    CHECK(a->skeleton.boundsHeight(0) == Catch::Approx(1.9f));
    CHECK(a->skeleton.boundsHeight(1) == Catch::Approx(2.1f));
    CHECK(a->skeleton.boundsHeight(2) == Catch::Approx(0.6f));
}

TEST_CASE("worn meshes hide the hair, cuff and boot groups they cover (Os2 0x8010 hide groups)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Elvaan F (race 4), face 14: head 18 is a hat hiding hair groups 1..3 (header byte 3 = 0x04); feet model 8 hides
    // the trousers' bottoms (group 6, byte 3 = 0x32). Hiding drops triangles, so the hat look has fewer than without.
    const auto hat  = xi::dat::loadPcAsset(*install(), 4, 14, { 18, 8, 8, 8, 8, 0, 0, 0 });
    const auto bare = xi::dat::loadPcAsset(*install(), 4, 14, { 0, 8, 8, 8, 8, 0, 0, 0 });
    REQUIRE(hat);
    REQUIRE(bare);
    CHECK(hat->hiddenGroups == ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 6)));
    CHECK((bare->hiddenGroups & 0xE) == 0);
}

TEST_CASE("Os2 class 1 (cloth / hair soft bodies) meshes are drawn")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // The Gigas model (271, file 1571) carries two class-1 meshes (`hf_s`, `hf_1`: its loincloth): 144 more corners
    // than its class-0 parts alone (9,720).
    const auto gigas = xi::dat::loadNpcAsset(*install(), 271);
    REQUIRE(gigas);
    CHECK(gigas->vertices.size() == 9864);
    REQUIRE(gigas->cloth.size() == 2);
    CHECK(gigas->clothParticles == 40);
}

TEST_CASE("Os2 class 1 soft bodies sway, settle and stay bounded")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // Model 73 (file 1373): one 88-particle robe; model 271: the Gigas' two loincloth meshes.
    for (const uint16_t id : { uint16_t{ 73 }, uint16_t{ 271 } })
    {
        const auto a = xi::dat::loadNpcAsset(*install(), id);
        REQUIRE(a);
        REQUIRE(a->clothParticles > 0);
        xi::gfx::ClothModel m{ a->cloth, a->skeleton.spheres, static_cast<uint32_t>(a->skeleton.bones.size()), a->clothParticles };
        // Bind pose, feet at the render origin (game -> render flips y and z).
        const glm::mat4 g(glm::vec4(1, 0, 0, 0), glm::vec4(0, -1, 0, 0), glm::vec4(0, 0, -1, 0), glm::vec4(0, 0, 0, 1));
        auto palette = [&](const glm::vec3& at) {
            std::vector<glm::mat4> pal;
            for (const auto& b : a->skeleton.bindWorld)
            {
                pal.push_back(glm::translate(glm::mat4(1.0f), at) * g * glm::make_mat4(b.data()));
            }
            return pal;
        };
        auto positions = [&](const std::vector<glm::mat4>& pal) {
            std::vector<glm::vec3> out;
            for (uint32_t i = 0; i < m.particles; ++i)
            {
                out.emplace_back(pal[m.base + i][3]);
            }
            return out;
        };
        std::vector<glm::mat4> rest = palette(glm::vec3(0.0f));
        xi::gfx::restCloth(m, rest);
        const auto restPos = positions(rest);

        // Standing still for 10 s at 60 fps: finite, near the rest shape (hangs under gravity), and settled.
        xi::gfx::ClothSim      sim;
        std::vector<glm::mat4> pal;
        std::vector<glm::vec3> last;
        float                  maxFromRest = 0.0f, lastMove = 0.0f;
        for (int f = 0; f < 600; ++f)
        {
            pal = palette(glm::vec3(0.0f));
            xi::gfx::simulateCloth(m, sim, pal, glm::vec3(0.0f), 1.0f / 60.0f);
            const auto now = positions(pal);
            lastMove       = 0.0f;
            for (std::size_t i = 0; i < now.size(); ++i)
            {
                REQUIRE(std::isfinite(now[i].x));
                REQUIRE(std::isfinite(now[i].y));
                REQUIRE(std::isfinite(now[i].z));
                maxFromRest = std::max(maxFromRest, glm::distance(now[i], restPos[i]));
                if (!last.empty())
                {
                    lastMove = std::max(lastMove, glm::distance(now[i], last[i]));
                }
            }
            last = now;
        }
        CHECK(maxFromRest < 1.0f); // a robe / loincloth keeps its shape
        CHECK(lastMove < 1e-3f);   // and comes to rest

        // Walking at 4 yalms / s along render +x for 1 s: the free particles trail behind the skinned rest (they lag),
        // and catch up again once the walk stops.
        float lag = 0.0f;
        for (int f = 1; f <= 60; ++f)
        {
            const glm::vec3 at(4.0f * static_cast<float>(f) / 60.0f, 0.0f, 0.0f);
            pal = palette(at);
            xi::gfx::simulateCloth(m, sim, pal, at, 1.0f / 60.0f);
            const auto now = positions(pal);
            float      sum = 0.0f;
            int        n   = 0;
            for (const auto& mesh : m.meshes)
            {
                for (std::size_t i = 0; i < mesh.particles.size(); ++i)
                {
                    if (!mesh.particles[i].anchor)
                    {
                        sum += (restPos[mesh.first + i].x + at.x) - now[mesh.first + i].x;
                        ++n;
                    }
                }
            }
            lag = n ? sum / static_cast<float>(n) : 0.0f;
            for (const auto& q : now)
            {
                REQUIRE(glm::distance(q, glm::vec3(at.x, 1.0f, 0.0f)) < 4.0f); // bounded: near the body
            }
        }
        CHECK(lag > 0.005f);
        const glm::vec3 stop(4.0f, 0.0f, 0.0f);
        for (int f = 0; f < 600; ++f)
        {
            pal = palette(stop);
            xi::gfx::simulateCloth(m, sim, pal, stop, 1.0f / 60.0f);
        }
        const auto settled = positions(pal);
        float      back    = 0.0f;
        for (std::size_t i = 0; i < settled.size(); ++i)
        {
            back = std::max(back, glm::distance(settled[i], restPos[i] + stop));
        }
        CHECK(back <= maxFromRest + 0.05f);

        // Body collision (goal 22): the springs tied to the skeleton's spheres keep their free ends out of them.
        int   ties = 0;
        float deepest = 0.0f;
        for (std::size_t mi = 0; mi < m.meshes.size(); ++mi)
        {
            const auto& c = m.meshes[mi];
            for (std::size_t si = 0; si < c.springs.size(); ++si)
            {
                const int tie = sim.tie[mi][si];
                if (tie < 0)
                {
                    continue;
                }
                ++ties;
                const auto&     sp = m.spheres[static_cast<std::size_t>(tie)];
                const glm::vec3 ctr(pal[static_cast<std::size_t>(sp.bone)] * glm::vec4(sp.offset[0], sp.offset[1], sp.offset[2], 1.0f));
                for (const auto& [end, pinned] : { std::pair{ c.springs[si].a, c.springs[si].pinA }, std::pair{ c.springs[si].b, c.springs[si].pinB } })
                {
                    if (!pinned && !c.particles[end].anchor)
                    {
                        deepest = std::max(deepest, sp.radius - glm::distance(sim.p[c.first + end], ctr));
                    }
                }
            }
        }
        CHECK(!m.spheres.empty());
        CHECK(ties > 0);
        CHECK(deepest < 0.02f);

        // The scene wind (goal 22): gusting for 10 s, the free particles drift toward render -x (game (-1, 0, 0)) and
        // stay near the body; a roofed floor (wind 0) leaves them as without wind.
        xi::gfx::ClothSim  windy = sim;
        xi::gfx::ClothWind wind;
        float              drift = 0.0f, peak = 0.0f;
        for (int f = 0; f < 600; ++f)
        {
            xi::gfx::stepClothWind(wind, 1.0f / 60.0f);
            peak = std::max(peak, wind.strength);
            pal  = palette(stop);
            xi::gfx::simulateCloth(m, windy, pal, stop, 1.0f / 60.0f, wind.strength);
            const auto now = positions(pal);
            for (const auto& q : now)
            {
                REQUIRE(glm::distance(q, glm::vec3(stop.x, 1.0f, 0.0f)) < 4.0f);
            }
            float sum = 0.0f;
            for (std::size_t i = 0; i < now.size(); ++i)
            {
                sum += now[i].x - settled[i].x;
            }
            drift = std::min(drift, sum / static_cast<float>(now.size()));
        }
        CHECK(peak > 0.009f);
        CHECK(peak < 0.033f); // (4 + rand() % 10) x 0.0025
        CHECK(drift < -0.002f);
        fmt::print("cloth model {}: {} particles, max from rest {:.3f}, settled step {:.5f}, walking lag {:.3f}, after the walk {:.3f}, "
                   "wind drift {:.3f}\n",
                   id, m.particles, maxFromRest, lastMove, lag, back, drift);
    }
}

TEST_CASE("motion keys blend the short way round between frames (60 fps)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // NPC model file 1700 (model 400) flips the quaternion sign between many consecutive keys of wlk0; a half-frame
    // sample must lie between its two keys, not swing through the near-zero quaternion (the 60 fps glitch).
    const auto walk = xi::dat::loadMotionSet(*install(), { 1700 }, "wlk");
    REQUIRE(!walk.empty());
    const auto& m = walk.pieces.front();
    auto angle = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        return 2.0f * std::acos(std::min(1.0f, d));
    };
    int checked = 0;
    for (int f = 0; f + 1 < walk.frames(); ++f)
    {
        std::vector<xi::dat::Motion::BonePose> p0(128), ph(128), p1(128);
        m.sample(static_cast<float>(f), p0);
        m.sample(static_cast<float>(f) + 0.5f, ph);
        m.sample(static_cast<float>(f + 1), p1);
        for (std::size_t b = 0; b < p0.size(); ++b)
        {
            if (!p0[b].driven)
            {
                continue;
            }
            const float whole = angle(p0[b].rotation, p1[b].rotation);
            CHECK(angle(p0[b].rotation, ph[b].rotation) <= whole * 0.5f + 1e-3f);
            ++checked;
        }
    }
    CHECK(checked > 100);
}

namespace
{

// An EventHost that records what the newer ops (event-format §6.8) ask for; everything else is accepted silently.
struct OpsHost : xi::dat::EventHost
{
    std::vector<std::string>              log;
    std::map<uint32_t, xi::dat::EventVec> pos;
    std::map<uint32_t, bool>              snapped;
    xi::dat::EventRunner*                 runner = nullptr;
    uint32_t player() const override { return 0x1000; }
    bool exists(uint32_t) const override { return true; }
    void request(uint32_t) override {}
    xi::dat::EventVec position(uint32_t id) const override
    {
        const auto it = pos.find(id);
        return it != pos.end() ? it->second : xi::dat::EventVec{};
    }
    void place(uint32_t id, const xi::dat::EventVec& p, std::optional<float>, bool snap) override
    {
        pos[id]     = p;
        snapped[id] = snap;
    }
    void face(uint32_t id, float y) override { log.push_back(fmt::format("face {:x} {:.3f}", id, y)); }
    void walking(uint32_t, float) override {}
    void hide(uint32_t, bool) override {}
    void motion(uint32_t, uint32_t, uint32_t, int, int) override {}
    bool motionBusy(uint32_t) const override { return false; }
    void idle(uint32_t id, uint32_t f) override { log.push_back(fmt::format("idle {:x} {}", id, xi::dat::fxNameString(f))); }
    void lookAt(uint32_t, uint32_t, int) override {}
    void lookAngles(uint32_t, int, int) override {}
    void lookStop(uint32_t) override {}
    void costume(uint32_t, int, uint16_t) override {}
    void scale(uint32_t, float) override {}
    void scheduler(uint32_t file, uint32_t c, uint32_t, uint32_t f) override
    {
        log.push_back(fmt::format("scheduler {} {} {:x}", file, xi::dat::fxNameString(f), c));
    }
    void stopScheduler(uint32_t, uint32_t, uint32_t) override {}
    bool schedulerRunning(uint32_t, uint32_t, uint32_t) const override { return false; }
    void cameraMode(int) override {}
    void music(int, uint16_t) override {}
    void sound(uint32_t) override {}
    void message(uint32_t, uint32_t) override {}
    void menu(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void update(uint32_t) override
    {
        if (runner)
        {
            runner->updated({});
        }
    }
    void positionUpdate(const xi::dat::EventVec&, float) override { update(0); }
    float yaw(uint32_t) const override { return 3.14159265f; }
    void  chocobo(uint32_t id, const std::array<uint16_t, 6>& l) override
    {
        log.push_back(fmt::format("chocobo {:x} {} {} {} {} {} {}", id, l[0], l[1], l[2], l[3], l[4], l[5]));
    }
    void animation(uint32_t id, uint8_t v) override { log.push_back(fmt::format("animation {:x} {}", id, v)); }
    void emote(uint32_t id, uint32_t, int e, int m) override { log.push_back(fmt::format("emote {:x} {} {}", id, e, m)); }
    void environment(int h, int w) override { log.push_back(fmt::format("environment {} {}", h, w)); }
    void restoreEnvironment() override { log.push_back("restore"); }
    void volume(uint32_t m, float l, float t) override { log.push_back(fmt::format("volume {} {:.2f} {}", m, l, t)); }
    void mapMarker(int z, int m, float x, float y, const std::string& s) override { log.push_back(fmt::format("marker {} {} {:.1f} {:.1f} {}", z, m, x, y, s)); }
};

} // namespace

TEST_CASE("event op 0x9D: a ref table read, an inline string copied and compared (event-format §6.8)")
{
    struct Messages : OpsHost
    {
        std::vector<uint32_t> ids;
        void message(uint32_t m, uint32_t) override { ids.push_back(m); }
    } host;
    xi::dat::EventFile f;
    auto&              b = f.blocks.emplace_back();
    b.id                 = 0x7FFFFFF0u;
    b.entryOff           = { 0 };
    b.entryId            = { 40 };
    b.consts             = { 0, 1, 777, 999 };
    b.code               = {
        0x9D, 0x00, 47, 0x00, 0x00, 0x00, 0x01, 0x80, // l0 = the ref table@47 [1] (#2 = 777)
        0x9D, 0x01, 51, 0x00, 0x08, 0x00, 0x00, 0x80, // l8..l11 = the 16 bytes @51 [0] ("Abc")
        0x9D, 0x08, 0x08, 0x00, 0x00, 'A', 'b', 'c', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 43, 0x00, // equal: on, else to 43
        0x48, 0x00, 0x00, 0x21,                       // message l0, end
        0x48, 0x03, 0x80, 0x21,                       // 43: message 999 (the strings differed), end
        0x00, 0x80, 0x02, 0x80,                       // 47: refs #0, #2
        'A', 'b', 'c', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // 51
    xi::dat::EventRunner runner(std::make_shared<const xi::dat::EventFile>(f), 0x7FFFFFF0u, 40, {}, host);
    host.runner = &runner;
    REQUIRE(runner.valid());
    for (int i = 0; i < 10 && !runner.finished(); ++i)
    {
        runner.tick(1.0f);
        runner.closeMessage();
    }
    CHECK(runner.finished());
    CHECK(runner.skipped().empty());
    CHECK(host.ids == std::vector<uint32_t>{ 777 });
}

TEST_CASE("event ops: trigonometry, positions, bit fields, actors and environment (event-format §6.8)")
{
    OpsHost            host;
    xi::dat::EventFile f;
    auto&              b = f.blocks.emplace_back();
    b.id                 = 0x7FFFFFF0u;
    b.entryOff           = { 0 };
    b.entryId            = { 40 };
    // consts #0..#9: 1024 (a quarter turn), 2000, 4, 7, 5, 0xFF, 8, 3, 0, 2048
    b.consts = { 1024, 2000, 4, 7, 5, 0xFF, 8, 3, 0, 2048 };
    b.code   = {
        0x16, 0x00, 0x00, 0x00, 0x80, 0x01, 0x80,                         // l0 = -2000 sin(90 deg)
        0x17, 0x01, 0x00, 0x09, 0x80, 0x01, 0x80,                         // l1 = 2000 cos(180 deg)
        0x3A, 0xF0, 0xFF, 0xFF, 0x7F, 0x02, 0x00,                         // l2 = the player's facing (pi): 2048
        0x3C, 0x10, 0x00, 0x02, 0x80, 0x04, 0x80,                         // set bit 4 of l16..
        0x3C, 0x10, 0x00, 0x06, 0x80, 0x04, 0x80,                         // .. and bit 8
        0x3D, 0x10, 0x00, 0x02, 0x80, 0x04, 0x80,                         // clear bit 4: l16 = 0x100
        0x40, 0x02, 0x80, 0x03, 0x80, 0x03, 0x00, 0x07, 0x80,             // l3 bits 4..7 = 3: 0x30
        0x41, 0x02, 0x80, 0x03, 0x80, 0x03, 0x00, 0x04, 0x00,             // l4 = l3 bits 4..7: 3
        0x33, 0x01,                                                       // moves keep their height
        0x36, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80,                         // self to (2, 2, 2), not snapped
        0x3B, 0xF0, 0xFF, 0xFF, 0x7F, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, // l5..l7 = the position x 1000
        0x39, 0x00, 0x80,                                                 // face a quarter turn
        0x6B, 0xF0, 0xFF, 0xFF, 0x7F, 'i', 'd', 'l', '1',                 // the player's idle motion
        0x4C, 0x4D, 0x8E, 0x4F, 0x07, 0x80,                               // animations 8, 9, 45, 3 + 18
        0x6E, 0xF0, 0xFF, 0xFF, 0x7F, 0x04, 0x80,                         // emote 5
        0x77, 0x06, 0x80, 0x05, 0x80,                                     // hour 8, weather kept
        0x69, 0x01, 0x04, 0x80,                                           // channels 1 | 4 off
        0x6A, 0x00, 0x80, 0x06, 0x80, 0x07, 0x80,                         // channels 1 | 2 to 1.024 over 8 ticks
        0x7D, 0x07, 0x80,                                                 // `main` of DAT 0x13F8 + 3 on the player
        0x8B, 0x08, 0x80, 0x08, 0x80, 0x01, 0x80, 0x01, 0x80, 'A', '_', 'B', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x78,                                                             // restore
        0xB6, 0x0E, 0x08, 0x80, 0x07, 0x80, 0x02, 0x80, 0x07, 0x80, 0x06, 0x80, 0x04, 0x80, 0x02, 0x80, // chocobo
        0x21 };
    xi::dat::EventRunner runner(std::make_shared<const xi::dat::EventFile>(f), 0x7FFFFFF0u, 40, {}, host);
    host.runner = &runner;
    REQUIRE(runner.valid());
    for (int i = 0; i < 10 && !runner.finished(); ++i)
    {
        runner.tick(1.0f);
    }
    CHECK(runner.finished());
    CHECK(runner.skipped().empty());
    CHECK(host.snapped[0x1000] == false);
    CHECK(host.pos[0x1000].x == Catch::Approx(2.0f));
    // chocobo: colour 3 -> race 35; head @8 = 3, body @6 = 4, hands @12 = 5, legs @14 = 4, feet @10 = 8.
    const std::vector<std::string> want = { "face 1000 1.571", "idle 1000 idl1", "animation 1000 8", "animation 1000 9", "animation 1000 45",
                                            "animation 1000 21", "emote 1000 5 0", "environment 8 -1", "volume 5 0.00 0", "volume 3 1.02 8",
                                            fmt::format("scheduler {} main 1000", 0x13F8 + 3), "marker 0 0 2.0 2.0 A B", "restore",
                                            "chocobo 1000 35 3 4 5 4 8" };
    CHECK(host.log == want);

    // The locals they wrote, as message ids (op 0x48 ref).
    xi::dat::EventFile g = f;
    auto&              c = g.blocks[0].code;
    c.pop_back();
    for (const uint8_t l : std::array<uint8_t, 9>{ 0, 1, 2, 3, 4, 5, 6, 7, 16 })
    {
        c.insert(c.end(), { 0x48, l, 0x00 });
    }
    c.push_back(0x21);
    struct Messages : OpsHost
    {
        std::vector<int32_t> ids;
        void message(uint32_t m, uint32_t) override { ids.push_back(static_cast<int32_t>(m)); }
    } h2;
    xi::dat::EventRunner r2(std::make_shared<const xi::dat::EventFile>(g), 0x7FFFFFF0u, 40, {}, h2);
    h2.runner = &r2;
    for (int i = 0; i < 40 && !r2.finished(); ++i)
    {
        r2.tick(1.0f);
        r2.closeMessage();
    }
    CHECK(r2.finished());
    CHECK(h2.ids == std::vector<int32_t>{ -2000, -2000, 2048, 0x30, 3, 2000, 2000, 2000, 0x100 });
}

TEST_CASE("mission and quest cutscenes run with no unknown ops (event-format §6.8)")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    // The nation intros (San d'Oria, Bastok, Windurst) and other mission / quest scenes in the cities.
    const std::vector<std::pair<uint16_t, uint16_t>> scenes = { { 230, 503 }, { 231, 535 }, { 234, 1 },     { 236, 1 },   { 238, 11 },
                                                                { 240, 11 },  { 241, 367 }, { 234, 502 },   { 238, 906 }, { 243, 10070 },
                                                                { 230, 0 },   { 237, 782 }, { 232, 4 },     { 230, 23 },  { 245, 86 } };
    for (const auto& [zone, event] : scenes)
    {
        OpsHost host;
        xi::dat::EventRunner runner(std::make_shared<const xi::dat::EventFile>(xi::dat::EventFile::load(*install(), zone)), 0x7FFFFFF0u, event, {},
                                    host);
        host.runner = &runner;
        REQUIRE(runner.valid());
        for (int frame = 0; frame < 60 * 600 && !runner.finished(); ++frame)
        {
            runner.tick(1.0f);
            runner.closeMessage();
            runner.choose(0);
        }
        INFO("zone " << zone << " event " << event);
        CHECK(runner.finished());
        CHECK(runner.skipped().empty());
    }
}

TEST_CASE("target arrows: sheet 6 \"anc anc_l\" has six 20 x 32 frames with the tip at the anchor")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const uint32_t files[] = { xi::dat::kUiMenuFile };
    const auto     ui      = xi::dat::UiResources::load(*install(), files);
    for (std::size_t i = 0; i < 6; ++i)
    {
        const auto* s = ui.sprite("anc anc_l", i);
        REQUIRE(s);
        REQUIRE(s->size() == 1);
        const auto& q = (*s)[0];
        CHECK(q.texture == "anc anc");
        CHECK(q.xy[0] == 10);
        CHECK(q.xy[1] == -32);
        CHECK(q.xy[7] == 0); // the bottom edge sits on the anchor point
    }
    CHECK_FALSE(ui.sprite("anc anc_l", 6));
}

#include "client/emotes.h"
#include "dat/emote_list.h"

TEST_CASE("Emote List: file 96 entries, folders and their labels from 55675")
{
    if (!install())
    {
        SKIP("FFXI_ROOT not set");
    }
    const auto list = xi::dat::EmoteList::parse(install()->read(xi::dat::EmoteList::kFile));
    const auto text = xi::dat::StringTable::parse(install()->read(xi::dat::EmoteList::kTextFile));
    REQUIRE(list.entries().size() == 111);
    const auto top = list.children(0);
    REQUIRE(top.size() == 51);
    CHECK(top.front()->command == "aim");
    CHECK(text.get(top.front()->label) == "/aim");
    // /bell, /jobemote and /sitchair are folders.
    int folders = 0;
    for (const auto* e : top)
    {
        folders += e->folder() ? 1 : 0;
    }
    CHECK(folders == 3);
    const auto chairs = list.children(38);
    REQUIRE(chairs.size() == 13);
    CHECK(chairs[1]->line() == "/sitchair 1");
    CHECK(text.get(chairs[1]->help) == "Sit in an imperial chair.");
    CHECK(list.children(4).size() == 25);  // bell notes C3 .. C5
    CHECK(list.children(23).size() == 22); // job emotes war .. run
}

TEST_CASE("emote Param: bell notes and job emotes as LSB checks them")
{
    CHECK(xi::client::emoteParam(73, "C3") == 6);
    CHECK(xi::client::emoteParam(73, "C3+") == 7);
    CHECK(xi::client::emoteParam(73, "B4") == 6 + 23);
    CHECK(xi::client::emoteParam(73, "C5") == 30);
    CHECK(xi::client::emoteParam(73, "D5") == 0);
    CHECK(xi::client::emoteParam(74, "war") == 0x1F);
    CHECK(xi::client::emoteParam(74, "run") == 0x1E + 22);
    CHECK(xi::client::emoteParam(8, "x") == 0);
    CHECK(xi::client::emoteId("/upset") == 26);
}
