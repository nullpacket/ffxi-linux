// SPDX-License-Identifier: GPL-3.0-or-later
// The game-state and game-data half of `__xi` (the tables src/ashita/prelude_memory.lua reads): world() and entities()
// build one frame's state as Lua tables; item / spell / ability / status / key_item / zone_name answer the resource
// manager.
#include "ashita/ashita.h"
#include "ashita/lua_util.h"
#include "ashita/lua_state.h"

#include "dat/items.h"
#include "dat/messages.h"
#include "dat/spells.h"

#include "dat/dat_index.h"
#include "dat/texture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_map>

namespace xi::ashita
{
namespace
{

// setGameData's install and what is read from it on demand (any thread: packet events run on the session thread).
struct GameData
{
    std::mutex                                     m;
    const dat::DatIndex*                           index = nullptr;
    std::optional<dat::DialogTable>                monsterAbilities; // file 7035
    std::unordered_map<uint32_t, std::string>      bitmaps;          // item id / 0x10000 | status icon -> BMP bytes
};
GameData& gameData()
{
    static GameData g;
    return g;
}
constexpr uint32_t kMonsterAbilityNamesFile = 7035;

// A 32-bit BMP file (BITMAPFILEHEADER, BITMAPINFOHEADER, bottom-up BGRA rows) of a texture: what Ashita hands addons as
// a resource's Bitmap for SetTextureFromMemory (INFERRED that a plain BMP serves; the game's own icon block is a
// texture chunk, §12).
std::string bmpFile(const dat::Texture& tex)
{
    const auto     rgba  = dat::toRgba(tex);
    const uint32_t w     = tex.width, h = tex.height;
    const uint32_t image = w * h * 4, size = 14 + 40 + image;
    std::string    out(size, '\0');
    auto put16 = [&](std::size_t at, uint32_t v) { out[at] = static_cast<char>(v & 0xFF); out[at + 1] = static_cast<char>(v >> 8 & 0xFF); };
    auto put32 = [&](std::size_t at, uint32_t v) { put16(at, v & 0xFFFF); put16(at + 2, v >> 16); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, size);
    put32(10, 14 + 40);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    put16(26, 1);
    put16(28, 32);
    put32(34, image);
    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            const std::size_t s = (std::size_t{ y } * w + x) * 4, d = 54 + (std::size_t{ h - 1 - y } * w + x) * 4;
            out[d]     = static_cast<char>(rgba[s + 2]);
            out[d + 1] = static_cast<char>(rgba[s + 1]);
            out[d + 2] = static_cast<char>(rgba[s]);
            out[d + 3] = static_cast<char>(rgba[s + 3]);
        }
    }
    return out;
}

void setNum(lua_State* L, const char* k, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}
void setStr(lua_State* L, const char* k, std::string_view v)
{
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, k);
}
void setBool(lua_State* L, const char* k, bool v)
{
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
}
// A table of numbers at keys 0..n-1 (the game's arrays, read with 0-based indexes).
template <typename A>
void setArray(lua_State* L, const char* k, const A& arr)
{
    lua_createtable(L, static_cast<int>(arr.size()), 1);
    for (std::size_t i = 0; i < arr.size(); ++i)
    {
        lua_pushnumber(L, static_cast<double>(arr[i]));
        lua_rawseti(L, -2, static_cast<int>(i));
    }
    lua_setfield(L, -2, k);
}
// A set of ids (id -> true).
void setIds(lua_State* L, const char* k, const std::vector<uint16_t>& v)
{
    lua_createtable(L, 0, static_cast<int>(v.size()));
    for (auto id : v)
    {
        lua_pushboolean(L, 1);
        lua_rawseti(L, -2, id);
    }
    lua_setfield(L, -2, k);
}

double secondsLeft(std::chrono::steady_clock::time_point t, std::chrono::steady_clock::time_point now)
{
    return std::max(0.0, std::chrono::duration<double>(t - now).count());
}

// --- state ---
int frame(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(managerOf(L).frameNumber()));
    return 1;
}

void pushParty(lua_State* L, const client::WorldSnapshot& w)
{
    lua_newtable(L);
    int n = 1;
    for (const auto& p : w.party)
    {
        lua_newtable(L);
        setNum(L, "id", p.id);
        setNum(L, "index", p.index);
        setStr(L, "name", p.name);
        setNum(L, "hp", p.hp);
        setNum(L, "mp", p.mp);
        setNum(L, "tp", p.tp);
        setNum(L, "hpp", p.hpp);
        setNum(L, "mpp", p.mpp);
        setNum(L, "zone", p.zone);
        setNum(L, "mainJob", p.mainJob);
        setNum(L, "mainLevel", p.mainLevel);
        setNum(L, "subJob", p.subJob);
        setNum(L, "subLevel", p.subLevel);
        setNum(L, "partyNo", p.partyNo);
        setBool(L, "leader", p.leader);
        setNum(L, "flags", p.flags);
        setNum(L, "order", p.order);
        lua_createtable(L, static_cast<int>(p.buffs.size()), 0);
        for (std::size_t k = 0; k < p.buffs.size(); ++k)
        {
            lua_pushnumber(L, p.buffs[k]);
            lua_rawseti(L, -2, static_cast<int>(k + 1));
        }
        lua_setfield(L, -2, "buffs");
        lua_rawseti(L, -2, n++);
    }
    lua_setfield(L, -2, "party");
}

void pushInventory(lua_State* L, const client::WorldSnapshot& w)
{
    lua_newtable(L); // inventory[container][slot]
    for (const auto& it : w.inventory)
    {
        lua_rawgeti(L, -1, it.container);
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_rawseti(L, -3, it.container);
        }
        lua_newtable(L);
        setNum(L, "id", it.itemId);
        setNum(L, "count", it.count);
        setNum(L, "flags", it.lockFlags);
        setNum(L, "price", it.price);
        lua_pushlstring(L, reinterpret_cast<const char*>(it.attr.data()), it.attr.size());
        lua_setfield(L, -2, "extra");
        lua_rawseti(L, -2, it.slot);
        lua_pop(L, 1);
    }
    lua_setfield(L, -2, "inventory");
    setArray(L, "containerSize", w.containerSize);
    setArray(L, "containerUsable", w.containerUsable);
    setBool(L, "containerSizeValid", w.containerSizeValid);
    lua_createtable(L, static_cast<int>(w.equipment.size()), 0);
    for (std::size_t s = 0; s < w.equipment.size(); ++s)
    {
        lua_createtable(L, 2, 0);
        lua_pushnumber(L, w.equipment[s].first);
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, w.equipment[s].second);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, static_cast<int>(s));
    }
    lua_setfield(L, -2, "equipment");
}

int world(lua_State* L)
{
    const auto& m  = managerOf(L);
    const auto& fr = m.frame();
    lua_newtable(L);
    if (!fr.world)
    {
        setNum(L, "login", 0);
        return 1;
    }
    const auto& w   = *fr.world;
    const auto  now = std::chrono::steady_clock::now();
    setNum(L, "login", w.inZone ? 2 : (w.choosingCharacter ? 1 : 0));
    setNum(L, "zone", w.zone);
    setNum(L, "charId", w.charId);
    setStr(L, "charName", w.charName);
    setNum(L, "actIndex", w.actIndex);
    setNum(L, "hp", w.hp);
    setNum(L, "mp", w.mp);
    setNum(L, "tp", w.tp);
    setNum(L, "hpp", w.hpp);
    setNum(L, "mpp", w.mpp);
    setNum(L, "hpMax", w.stats.hpMax);
    setNum(L, "mpMax", w.stats.mpMax);
    setNum(L, "mainJob", w.mainJob);
    setNum(L, "mainLevel", w.mainLevel);
    setNum(L, "subJob", w.subJob);
    setNum(L, "subLevel", w.subLevel);
    setNum(L, "exp", w.stats.exp);
    setNum(L, "expNext", w.stats.expNext);
    setNum(L, "attack", w.stats.attack);
    setNum(L, "defense", w.stats.defense);
    setNum(L, "title", w.stats.title);
    setNum(L, "rank", w.stats.rank);
    setNum(L, "rankBar", w.stats.rankBar);
    setNum(L, "nation", w.stats.nation);
    setNum(L, "bindZone", w.stats.bindZone);
    setNum(L, "itemLevel", w.stats.itemLevel);
    setNum(L, "suLevel", w.stats.suLevel);
    setNum(L, "unity", w.stats.unityLeader);
    setNum(L, "speed", w.speed);
    setNum(L, "animation", w.animation);
    setNum(L, "castSpell", w.castSpell);
    setNum(L, "castElapsed", w.castSpell ? std::chrono::duration<double>(now - w.castStarted).count() : 0.0);
    setBool(L, "inMogHouse", w.inMogHouse);
    setNum(L, "petIndex", w.petIndex);
    setBool(L, "petPresent", w.petPresent);
    setNum(L, "petHpp", w.petHpp);
    setNum(L, "petMpp", w.petMpp);
    setNum(L, "petTp", w.petTp);
    setStr(L, "petName", w.petName);
    setNum(L, "weather", w.weather);
    setArray(L, "jobLevels", w.jobLevels);
    setArray(L, "statBase", w.stats.base);
    setArray(L, "statBonus", w.stats.bonus);
    setArray(L, "resist", w.stats.elementDefense);
    setArray(L, "skills", w.skills);
    setIds(L, "keyItems", w.keyItems);
    setIds(L, "spells", w.spells);
    setIds(L, "jobAbilities", w.jobAbilities);
    setIds(L, "weaponSkills", w.weaponSkills);
    setIds(L, "traits", w.traits);
    setIds(L, "petAbilities", w.petAbilities);
    // Our status effects in the packet's order: { icon, seconds left (-1 none), the raw 0x063 expiry }.
    lua_createtable(L, static_cast<int>(w.buffs.size()), 0);
    int n = 1;
    for (const auto& b : w.buffs)
    {
        lua_createtable(L, 3, 0);
        lua_pushnumber(L, b.icon);
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, b.expires ? secondsLeft(*b.expires, now) : -1.0);
        lua_rawseti(L, -2, 2);
        lua_pushnumber(L, b.timer);
        lua_rawseti(L, -2, 3);
        lua_rawseti(L, -2, n++);
    }
    lua_setfield(L, -2, "buffs");
    lua_newtable(L);
    for (const auto& [id, t] : w.spellReady)
    {
        lua_pushnumber(L, secondsLeft(t, now));
        lua_rawseti(L, -2, id);
    }
    lua_setfield(L, -2, "spellRecast");
    lua_newtable(L);
    for (const auto& [id, t] : w.abilityRecastReady)
    {
        lua_pushnumber(L, secondsLeft(t, now));
        lua_rawseti(L, -2, id);
    }
    lua_setfield(L, -2, "abilityRecast");
    // S2C 0x119's 31 slots as sent, with the seconds counted down since: { id, left, calc1, calc2 } at 0..30.
    const double since = std::chrono::duration<double>(now - w.abilityRecastsAt).count();
    lua_createtable(L, 31, 1);
    for (std::size_t k = 0; k < w.abilityRecasts.size(); ++k)
    {
        const auto& r = w.abilityRecasts[k];
        lua_createtable(L, 0, 4);
        setNum(L, "id", r.id);
        setNum(L, "left", std::max(0.0, r.seconds - since));
        setNum(L, "calc1", r.calc1);
        setNum(L, "calc2", r.calc2);
        lua_rawseti(L, -2, static_cast<int>(k));
    }
    lua_setfield(L, -2, "abilityRecasts");
    pushParty(L, w);
    pushInventory(L, w);
    lua_createtable(L, static_cast<int>(w.treasure.size()), 1);
    for (std::size_t s = 0; s < w.treasure.size(); ++s)
    {
        const auto& t = w.treasure[s];
        lua_newtable(L);
        setNum(L, "id", t.itemId);
        setNum(L, "count", t.count);
        setNum(L, "lot", t.myLot);
        setNum(L, "bestLot", t.bestLot);
        setStr(L, "bestName", t.bestName);
        setNum(L, "dropTime", t.startTime);
        // Five minutes to lot from when the client saw it (the game's TimeToLive counts the same; INFERRED in seconds).
        setNum(L, "ttl", t.itemId ? std::max(0.0, 300.0 - std::chrono::duration<double>(now - t.seen).count()) : 0.0);
        lua_rawseti(L, -2, static_cast<int>(s));
    }
    lua_setfield(L, -2, "treasure");
    lua_newtable(L);
    setNum(L, "index", fr.targetIndex);
    setNum(L, "sub", fr.subTargetIndex);
    setBool(L, "subActive", fr.subTargetActive);
    lua_setfield(L, -2, "target");
    return 1;
}

// Ashita's entity Type (entity.h: 0 PC, 1 NPC, 2 NPC with a fixed model, 3 doors and the like) from the look kind
// (LSB look.size: 0 standard model, 1 equipped, 2 door, 3 lift, 4 ship). INFERRED: monsters, being fixed models, are 2.
int entityType(const client::Entity& e)
{
    if (e.isPc)
    {
        return 0;
    }
    if (e.look.kind >= 2 && e.look.kind <= 4)
    {
        return 3;
    }
    return e.look.kind == 1 ? 1 : 2;
}

// Entities by target index: our own entity plus everything the server has spawned.
void pushEntity(lua_State* L, const client::Entity& e, const glm::vec3& me, uint32_t self, const client::WorldSnapshot& w)
{
    lua_newtable(L);
    setNum(L, "id", e.id);
    setNum(L, "index", e.index);
    setStr(L, "name", e.name);
    // Ashita: X / Y across the ground, Z the height (entity.h names the stored order X, Z, Y); the session keeps the
    // height in y.
    setNum(L, "x", e.pos.x);
    setNum(L, "y", e.pos.z);
    setNum(L, "z", e.pos.y);
    // The heading byte as radians in (-pi, pi]. INFERRED: the game's yaw runs the same way as the byte.
    double heading = e.dir * (2.0 * std::numbers::pi / 256.0);
    if (heading > std::numbers::pi)
    {
        heading -= 2.0 * std::numbers::pi;
    }
    setNum(L, "heading", heading);
    setNum(L, "hpp", e.hpp);
    setNum(L, "status", e.animation);
    setNum(L, "serverStatus", e.status);
    setBool(L, "isPc", e.isPc);
    setBool(L, "isMob", e.isMob);
    setBool(L, "hidden", e.hiddenByStatus(false));
    setBool(L, "targetOff", e.targetOff);
    setBool(L, "self", e.id == self);
    setNum(L, "type", entityType(e));
    const bool humanoid = e.isPc || e.look.kind == 1 || e.look.kind == 0xFFFF;
    setNum(L, "race", humanoid ? e.look.ids[0] >> 8 : 0);
    setNum(L, "claimId", e.claimId);
    setNum(L, "targetIndex", e.targetIndex);
    setNum(L, "hitbox", e.hitbox);
    setNum(L, "speed", e.id == self ? w.speed : 50);
    // LinkshellColor: "BGR format" (entity.h), the bytes B, G, R in memory, so 0xRRGGBB read as a number.
    setNum(L, "lsColor", (e.lsColor[0] << 16) | (e.lsColor[1] << 8) | e.lsColor[2]);
    setNum(L, "mount", e.mountIndex);
    setNum(L, "doorId", e.doorId);
    bool party = false, alliance = false;
    for (const auto& p : w.party)
    {
        if (p.id == e.id)
        {
            (p.partyNo == 0 ? party : alliance) = true;
        }
    }
    setBool(L, "party", party);
    setBool(L, "alliance", alliance);
    const glm::vec3 d = e.pos - me;
    setNum(L, "distsq", static_cast<double>(glm::dot(d, d)));
    lua_createtable(L, 9, 0);
    for (int k = 0; k < 9; ++k)
    {
        const uint16_t v = e.look.ids[static_cast<std::size_t>(k)];
        lua_pushnumber(L, k == 0 && humanoid ? (v & 0xFF) : v); // Look.Hair: the face (the race is its own field)
        lua_rawseti(L, -2, k);
    }
    lua_setfield(L, -2, "look");
    setNum(L, "lookKind", e.look.kind);
}

int entities(lua_State* L)
{
    const auto& fr = managerOf(L).frame();
    lua_newtable(L);
    if (!fr.world)
    {
        return 1;
    }
    const auto& w  = *fr.world;
    const auto  me = fr.self.pos;
    for (const auto& e : w.entities)
    {
        if (e.index == 0 || e.id == w.charId)
        {
            continue;
        }
        pushEntity(L, e, me, w.charId, w);
        lua_rawseti(L, -2, e.index);
    }
    if (w.charId != 0 && w.actIndex != 0)
    {
        client::Entity self = fr.self;
        self.id             = w.charId;
        self.index          = w.actIndex;
        self.name           = w.charName;
        self.hpp            = w.hpp;
        self.animation      = w.animation;
        self.isPc           = true;
        self.isMob          = false;
        self.status         = 0;
        self.mountIndex     = w.mountIndex;
        self.lsColor        = w.lsColor;
        if (!self.look.valid)
        {
            self.look = w.look;
        }
        pushEntity(L, self, me, w.charId, w);
        lua_rawseti(L, -2, w.actIndex);
    }
    return 1;
}

// --- game data ---
void pushNames(lua_State* L, const char* k, std::string_view s)
{
    lua_createtable(L, 3, 1);
    for (int i = 0; i <= 3; ++i)
    {
        lua_pushlstring(L, s.data(), s.size());
        lua_rawseti(L, -2, i);
    }
    lua_setfield(L, -2, k);
}
int item(lua_State* L)
{
    const auto& r = managerOf(L).resources();
    const auto  id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
    // Item 0 is the blank record (".") in the data; Ashita has no item there.
    const auto  i = r.items && id != 0 ? r.items->find(id) : std::nullopt;
    if (!i)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    setNum(L, "Id", i->id);
    setNum(L, "Flags", i->flags);
    setNum(L, "StackSize", i->stackSize);
    setNum(L, "Type", i->type);
    setNum(L, "Level", i->level);
    setNum(L, "Slots", i->slots);
    setNum(L, "Jobs", i->jobs);
    setNum(L, "Races", 0x1FE); // INFERRED: all races (the record's race mask is not decoded)
    pushNames(L, "Name", i->name);
    pushNames(L, "Description", i->description);
    pushNames(L, "LogNameSingular", i->logName);
    pushNames(L, "LogNamePlural", i->logPlural);
    return 1;
}
int itemByName(lua_State* L)
{
    auto&             m    = managerOf(L);
    const std::string want = luaL_checkstring(L, 1);
    if (!m.resources().items)
    {
        lua_pushnil(L);
        return 1;
    }
    static std::unordered_map<std::string, uint16_t> byName; // built once (every item record is read)
    if (byName.empty())
    {
        for (uint32_t id = 1; id < 0x10000; ++id)
        {
            if (const auto it = m.resources().items->find(static_cast<uint16_t>(id)); it && !it->name.empty() && it->name != ".")
            {
                std::string k = it->name;
                std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                byName.try_emplace(k, static_cast<uint16_t>(id));
            }
        }
    }
    std::string k = want;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto f = byName.find(k);
    if (f == byName.end())
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, f->second);
    lua_replace(L, 1);
    return item(L);
}
int spell(lua_State* L)
{
    const auto& r  = managerOf(L).resources();
    const auto  id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
    const auto* s  = r.spells ? r.spells->find(id) : nullptr;
    if (!s)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    setNum(L, "Id", s->id);
    setNum(L, "Index", s->id);
    setNum(L, "Type", s->type);
    setNum(L, "Element", s->element);
    setNum(L, "Targets", s->targets);
    setNum(L, "Skill", s->skill);
    setNum(L, "ManaCost", s->mp);
    setNum(L, "CastTime", s->castTime);
    setNum(L, "RecastDelay", s->recast);
    setNum(L, "Range", s->range < 0 ? 0 : s->range);
    setNum(L, "AreaRange", s->aoeRadius);
    lua_createtable(L, 24, 0);
    for (std::size_t j = 0; j < s->levels.size(); ++j)
    {
        lua_pushnumber(L, s->levels[j] == 0xFF ? -1 : s->levels[j]);
        lua_rawseti(L, -2, static_cast<int>(j));
    }
    lua_setfield(L, -2, "LevelRequired");
    pushNames(L, "Name", r.text ? r.text->spellName(id) : std::string_view{});
    pushNames(L, "Description", r.text ? r.text->spellHelp(id) : std::string_view{});
    return 1;
}
// Abilities as Ashita numbers them: weapon skills by id, job abilities 512 + id, pet commands after (the same ids
// as the names file 55701).
int ability(lua_State* L)
{
    const auto& r  = managerOf(L).resources();
    const auto  id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const auto  nm = r.text ? r.text->abilityName(id) : std::string_view{};
    if (nm.empty())
    {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    setNum(L, "Id", id);
    const auto* a = r.abilities && id >= 512 ? r.abilities->find(static_cast<uint16_t>(id - 512)) : nullptr;
    setNum(L, "Type", a ? a->type : (id < 512 ? 3 : 0));
    setNum(L, "ManaCost", a ? a->mp : 0);
    setNum(L, "RecastTimerId", a ? a->recastId : 0);
    setNum(L, "Targets", a ? a->targets : 0);
    const int range = r.abilities ? r.abilities->commandRange(id) : -1;
    setNum(L, "Range", range < 0 ? 0 : range);
    pushNames(L, "Name", nm);
    pushNames(L, "Description", r.text ? r.text->abilityHelp(id) : std::string_view{});
    return 1;
}
int abilityByTimer(lua_State* L)
{
    const auto& r     = managerOf(L).resources();
    const auto  timer = static_cast<uint16_t>(luaL_checkinteger(L, 1));
    for (uint16_t id = 0; r.abilities && id < 1024; ++id)
    {
        if (const auto* a = r.abilities->find(id); a && a->recastId == timer)
        {
            lua_pushnumber(L, 512 + id);
            lua_replace(L, 1);
            return ability(L);
        }
    }
    lua_pushnil(L);
    return 1;
}
// A monster TP move's name by Ashita's 'monsters.abilities' index (the move id less 256); "" when unknown.
int monsterAbility(lua_State* L)
{
    const auto  i = static_cast<std::size_t>(std::max<lua_Integer>(0, luaL_checkinteger(L, 1)));
    auto&       g = gameData();
    std::string s;
    {
        std::lock_guard lock(g.m);
        if (g.index && !g.monsterAbilities)
        {
            try
            {
                g.monsterAbilities = dat::DialogTable::parse(g.index->read(kMonsterAbilityNamesFile));
            }
            catch (const std::exception&)
            {
                g.monsterAbilities = dat::DialogTable{};
            }
        }
        if (g.monsterAbilities && i < g.monsterAbilities->size())
        {
            const auto lines = dat::formatDialog(g.monsterAbilities->raw(i), {});
            s                = lines.empty() || lines[0] == "#0" ? std::string() : lines[0];
        }
    }
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}
// Icons as BMP files: status_bitmap(icon id) from file 87, item_bitmap(item id) from the item record; nil when none.
int pushBitmap(lua_State* L, uint32_t key, const std::function<std::optional<dat::Texture>()>& load)
{
    auto&           g = gameData();
    std::lock_guard lock(g.m);
    auto            it = g.bitmaps.find(key);
    if (it == g.bitmaps.end())
    {
        std::optional<dat::Texture> tex;
        try
        {
            tex = load();
        }
        catch (const std::exception&)
        {
        }
        it = g.bitmaps.emplace(key, tex && tex->width && tex->height ? bmpFile(*tex) : std::string()).first;
    }
    if (it->second.empty())
    {
        lua_pushnil(L);
    }
    else
    {
        lua_pushlstring(L, it->second.data(), it->second.size());
    }
    return 1;
}
int statusBitmap(lua_State* L)
{
    const auto id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
    const auto* index = gameData().index;
    return pushBitmap(L, 0x10000u | id, [&]() -> std::optional<dat::Texture> { return index ? dat::statusIcon(*index, id) : std::nullopt; });
}
int itemBitmap(lua_State* L)
{
    const auto  id    = static_cast<uint16_t>(luaL_checkinteger(L, 1));
    const auto& items = managerOf(L).resources().items;
    return pushBitmap(L, id, [&]() -> std::optional<dat::Texture> { return items ? items->icon(id) : std::nullopt; });
}
int status(lua_State* L)
{
    const auto& r  = managerOf(L).resources();
    const auto  id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const auto  s  = r.text ? r.text->statusName(id) : std::string_view{};
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}
// Key item names (file 55695, through the dialog name table 0x33).
int keyItem(lua_State* L)
{
    const auto& r  = managerOf(L).resources();
    const auto  id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    lua_pushstring(L, r.text ? r.text->dialogName(0x33, id).c_str() : "");
    return 1;
}
int zoneName(lua_State* L)
{
    const auto& r  = managerOf(L).resources();
    const auto  id = static_cast<std::size_t>(luaL_checkinteger(L, 1));
    const auto  s  = r.zoneNames ? r.zoneNames->get(id) : std::string_view{};
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

} // namespace

void setGameData(const dat::DatIndex* index)
{
    auto&           g = gameData();
    std::lock_guard lock(g.m);
    g.index = index;
    g.monsterAbilities.reset();
    g.bitmaps.clear();
}

void openState(lua_State* L)
{
    const luaL_Reg fns[] = {
        { "frame", frame },
        { "world", world },
        { "entities", entities },
        { "item", item },
        { "item_by_name", itemByName },
        { "spell", spell },
        { "ability", ability },
        { "ability_by_timer", abilityByTimer },
        { "monster_ability", monsterAbility },
        { "status_bitmap", statusBitmap },
        { "item_bitmap", itemBitmap },
        { "status", status },
        { "key_item", keyItem },
        { "zone_name", zoneName },
        { nullptr, nullptr },
    };
    for (const auto* f = fns; f->name; ++f)
    {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
}

} // namespace xi::ashita
