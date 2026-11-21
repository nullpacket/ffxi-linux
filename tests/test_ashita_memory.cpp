// SPDX-License-Identifier: GPL-3.0-or-later
// The Ashita layer's memory and resource interfaces (src/ashita/prelude_memory.lua over lua_state.cpp), driven headless:
// a small test addon evaluates an expression per command and prints the result, against a hand-built WorldSnapshot.
// The resource checks at the end read the user's install and skip without FFXI_ROOT.
#include "xi/install.h"
#include "ashita/ashita.h"
#include "ashita/lua_state.h"

#include "dat/dat_index.h"
#include "dat/items.h"
#include "dat/messages.h"
#include "dat/spells.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{

using xi::ashita::AddonManager;

constexpr const char* kProbe = R"(addon.name = 'probe';
ashita.events.register('command', 'probe_cb', function (e)
    if (e.command:sub(1, 7) ~= '/probe ') then return; end
    e.blocked = true;
    local f, err = loadstring('return ' .. e.command:sub(8));
    if (f == nil) then print('compile: ' .. tostring(err)); return; end
    local ok, v = pcall(f);
    print(ok and tostring(v) or ('error: ' .. tostring(v)));
end);
)";

struct Probe
{
    std::filesystem::path         root;
    std::vector<std::string>      chat;
    std::unique_ptr<AddonManager> m;

    explicit Probe(xi::ashita::Resources res = {})
    {
        root = std::filesystem::temp_directory_path() / ("xi-ashita-memory-" + std::to_string(::getpid()) + "-" + std::to_string(++serial()));
        std::filesystem::create_directories(root / "addons" / "probe");
        std::ofstream(root / "addons" / "probe" / "probe.lua") << kProbe;
        xi::ashita::Host host;
        host.chat = [this](const std::string& text, int) { chat.push_back(text); };
        m         = std::make_unique<AddonManager>(root, xi::ashita::Profile::Latest, std::move(res), std::move(host));
        REQUIRE(m->load("probe"));
    }
    ~Probe()
    {
        m.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    Probe(const Probe&)            = delete;
    Probe& operator=(const Probe&) = delete;

    static int& serial()
    {
        static int n = 0;
        return n;
    }
    std::string eval(const std::string& expr)
    {
        chat.clear();
        m->command("/probe " + expr, 1);
        return chat.empty() ? "<no output>" : chat.back();
    }
};

// The game data from the user's install (FFXI_ROOT), loaded once; nullopt without it.
std::optional<xi::ashita::Resources> installResources()
{
    static std::optional<xi::ashita::Resources> res = []() -> std::optional<xi::ashita::Resources> {
        const char* rootEnv = xi::install::gameRootOrNull();
        if (!rootEnv || !std::filesystem::exists(rootEnv))
        {
            return std::nullopt;
        }
        static const xi::dat::DatIndex index(rootEnv);
        static const auto              zones = xi::dat::StringTable::parse(index.read(55465));
        xi::ashita::Resources r;
        r.items     = std::make_shared<xi::dat::ItemTable>(index);
        r.text      = std::make_shared<xi::dat::BattleText>(index);
        r.zoneNames = &zones;
        xi::ashita::setGameData(&index);
        return r;
    }();
    return res;
}

xi::client::Entity mob(uint16_t index, uint32_t id, const char* name, glm::vec3 pos)
{
    xi::client::Entity e;
    e.index     = index;
    e.id        = id;
    e.name      = name;
    e.pos       = pos;
    e.hpp       = 80;
    e.isMob     = true;
    e.look.kind = 0;
    return e;
}

// A world: us (Testone, index 0x400) in zone 230 with a two-member party, a pet, a monster targeted, status effects,
// ability timers, some items and a treasure pool item.
xi::ashita::Frame world()
{
    auto w        = std::make_shared<xi::client::WorldSnapshot>();
    w->inZone     = true;
    w->zone       = 230;
    w->charId     = 0x1000;
    w->actIndex   = 0x400;
    w->charName   = "Testone";
    w->hp         = 400;
    w->mp         = 50;
    w->tp         = 1200;
    w->hpp        = 90;
    w->mpp        = 70;
    w->mainJob    = 1;
    w->mainLevel  = 30;
    w->animation  = 1; // engaged
    w->petIndex   = 0x700;
    w->petPresent = true;
    w->petTp      = 345;
    xi::client::PartyMember me, ally;
    me.id         = 0x1000;
    me.index      = 0x400;
    me.name       = "Testone";
    me.hp         = 1; // stale: our slot shows the live vitals
    me.flags      = 0x04;
    me.leader     = true;
    me.zone       = 230;
    ally.id       = 0x1001;
    ally.index    = 0x401;
    ally.name     = "Bottwo";
    ally.hp       = 300;
    ally.tp       = 50;
    ally.zone     = 231;
    ally.order    = 1;
    ally.buffs    = { 2, 0x101 }; // sleep, and an id over 255
    w->party      = { me, ally };
    xi::client::WorldSnapshot::Buff protect;
    protect.icon    = 40;
    protect.timer   = 123456;
    protect.expires = std::chrono::steady_clock::now() + std::chrono::seconds(100);
    xi::client::WorldSnapshot::Buff food;
    food.icon         = 251;
    w->buffs          = { protect, food };
    w->abilityRecasts[0] = { 3600, 0, 0, 0 };   // the two-hour
    w->abilityRecasts[1] = { 300, 0, 5, -10 };  // Provoke's group, 5 minutes
    w->abilityRecastsAt  = std::chrono::steady_clock::now();
    w->containerSize      = {};
    w->containerUsable[0] = 31;
    w->containerUsable[5] = 0; // a locked satchel
    w->containerSizeValid = true;
    xi::client::InventoryItem gil, potion, sword;
    gil.itemId       = 0xFFFF;
    gil.count        = 5000;
    potion.itemId    = 4112;
    potion.count     = 12;
    potion.slot      = 3;
    sword.itemId     = 16535;
    sword.count      = 1;
    sword.slot       = 7;
    sword.lockFlags  = 5;
    sword.attr[0]    = 2;
    w->inventory     = { gil, potion, sword };
    w->equipment[0]  = { 0, 7 };
    w->treasure[2].itemId    = 4112;
    w->treasure[2].count     = 1;
    w->treasure[2].myLot     = 512;
    w->treasure[2].bestLot   = 900;
    w->treasure[2].bestName  = "Bottwo";
    w->treasure[2].seen      = std::chrono::steady_clock::now();
    auto bat     = mob(0x050, 0x1010050, "Forest Hare", { 3.0f, -2.0f, 4.0f });
    bat.claimId  = 0x1000;
    auto ghost   = mob(0x051, 0x1010051, "Hidden", { 0.0f, 0.0f, 0.0f });
    ghost.status = 3;
    xi::client::Entity ally2;
    ally2.index   = 0x401;
    ally2.id      = 0x1001;
    ally2.name    = "Bottwo";
    ally2.isPc    = true;
    ally2.look.kind   = 0xFFFF;
    ally2.look.ids[0] = 0x0703; // Mithra, face 3
    ally2.lsColor     = { 0x10, 0x20, 0x30 };
    xi::client::Entity pet = mob(0x700, 0x1000700, "Carbuncle", { 1.0f, 0.0f, 0.0f });
    pet.isMob     = false;
    w->entities   = { bat, ghost, ally2, pet };
    xi::ashita::Frame f;
    f.world           = w;
    f.self.pos        = { 0.0f, 0.0f, 0.0f };
    f.targetIndex     = 0x050;
    f.subTargetIndex  = 0x401;
    f.subTargetActive = false;
    return f;
}

} // namespace

TEST_CASE("ashita memory: entities", "[ashita]")
{
    Probe p;
    p.m->update(world());
    CHECK(p.eval("GetEntity(0x50).Name") == "Forest Hare");
    CHECK(p.eval("GetEntity(0x50).Distance") == "29");                    // squared: 3^2 + 2^2 + 4^2
    CHECK(p.eval("GetEntity(0x50).Movement.LocalPosition.Y") == "4");     // Ashita's Y runs across the ground
    CHECK(p.eval("GetEntity(0x50).Movement.LocalPosition.Z") == "-2");    // and Z is the height
    CHECK(p.eval("GetEntity(0x50).SpawnFlags") == "16");
    CHECK(p.eval("GetEntity(0x50).ClaimStatus") == "4096");
    CHECK(p.eval("GetEntity(0x50).Type") == "2");
    CHECK(p.eval("GetEntity(0x50) == GetEntity(0x50)") == "true");        // one table per entity per frame
    CHECK(p.eval("GetEntity(0x401).SpawnFlags") == "13");                 // a party member: PC + party + alliance
    CHECK(p.eval("GetEntity(0x401).Race") == "7");
    CHECK(p.eval("GetEntity(0x401).Look.Hair") == "3");
    CHECK(p.eval("GetEntity(0x401).LinkshellColor") == "1056816");
    CHECK(p.eval("GetEntity(0x51).ActorPointer") == "0");                 // hidden by its status
    CHECK(p.eval("AshitaCore:GetMemoryManager():GetEntity():GetRenderFlags0(0x51)") == "0");
    CHECK(p.eval("bit.band(AshitaCore:GetMemoryManager():GetEntity():GetRenderFlags0(0x50), 0x200)") == "512");
    CHECK(p.eval("GetEntity(0x123)") == "nil");
    CHECK(p.eval("AshitaCore:GetMemoryManager():GetEntity():GetServerId(0x123)") == "0");
    // Us and our pet.
    CHECK(p.eval("GetPlayerEntity().Name") == "Testone");
    CHECK(p.eval("GetPlayerEntity().SpawnFlags") == "525");
    CHECK(p.eval("GetPlayerEntity().Status") == "1");
    CHECK(p.eval("GetPlayerEntity().PetTargetIndex") == "1792");
    CHECK(p.eval("AshitaCore:GetMemoryManager():GetEntity():GetPetTargetIndex(0x400)") == "1792");
    CHECK(p.eval("GetEntity(GetPlayerEntity().PetTargetIndex).Name") == "Carbuncle");
    CHECK(p.eval("AshitaCore:GetMemoryManager():GetPlayer():GetPetTP()") == "345");
}

TEST_CASE("ashita memory: party", "[ashita]")
{
    Probe p;
    p.m->update(world());
    const std::string party = "AshitaCore:GetMemoryManager():GetParty():";
    CHECK(p.eval(party + "GetMemberName(0)") == "Testone");
    CHECK(p.eval(party + "GetMemberHP(0)") == "400"); // our live vitals, not the stale list entry
    CHECK(p.eval(party + "GetMemberTP(0)") == "1200");
    CHECK(p.eval(party + "GetMemberTargetIndex(0)") == "1024");
    CHECK(p.eval(party + "GetMemberFlagMask(0)") == "4");
    CHECK(p.eval(party + "GetMemberName(1)") == "Bottwo");
    CHECK(p.eval(party + "GetMemberIsActive(1)") == "1");
    CHECK(p.eval(party + "GetMemberIsActive(2)") == "0");
    CHECK(p.eval(party + "GetMemberZone2(1)") == "231"); // elsewhere: Zone2 set
    CHECK(p.eval(party + "GetMemberZone2(0)") == "0");
    CHECK(p.eval(party + "GetAlliancePartyMemberCount1()") == "2");
    CHECK(p.eval(party + "GetAlliancePartyLeaderServerId1()") == "4096");
    CHECK(p.eval(party + "GetStatusIconsServerId(0)") == "4097");
    CHECK(p.eval(party + "GetStatusIcons(0)[1]") == "2");
    CHECK(p.eval(party + "GetStatusIcons(0)[2]") == "1");   // 0x101: the low byte
    CHECK(p.eval(party + "GetStatusIcons(0)[3]") == "255");
    CHECK(p.eval(party + "GetStatusIconsBitMask(0)") == "4"); // the second icon's high bits (1) at bits 2..3
}

TEST_CASE("ashita memory: player, recasts, target", "[ashita]")
{
    Probe p;
    p.m->update(world());
    const std::string player = "AshitaCore:GetMemoryManager():GetPlayer():";
    CHECK(p.eval(player + "GetBuffs()[1]") == "40");
    CHECK(p.eval(player + "GetBuffs()[3]") == "-1");
    CHECK(p.eval(player + "GetStatusIcons()[2]") == "251");
    CHECK(p.eval(player + "GetStatusIcons()[3]") == "255");
    CHECK(p.eval(player + "GetStatusTimers()[1]") == "123456"); // the raw 0x063 value
    CHECK(p.eval(player + "GetStatusTimers()[2]") == "2147483647");
    const std::string recast = "AshitaCore:GetMemoryManager():GetRecast():";
    CHECK(p.eval(recast + "GetAbilityTimerId(1)") == "5");
    CHECK(p.eval(recast + "GetAbilityTimer(1) > 17900 and " + recast + "GetAbilityTimer(1) <= 18000") == "true"); // 1/60 s
    CHECK(p.eval(recast + "GetAbilityTimerId(0)") == "0");
    CHECK(p.eval(recast + "GetAbilityTimer(0) > 0") == "true");
    CHECK(p.eval(recast + "GetAbilityCalc2(1)") == "-10");
    CHECK(p.eval(recast + "GetAbilityTimer(2)") == "0");
    const std::string target = "AshitaCore:GetMemoryManager():GetTarget():";
    CHECK(p.eval(target + "GetTargetIndex(0)") == "80");
    CHECK(p.eval(target + "GetTargetIndex(1)") == "0");
    CHECK(p.eval(target + "GetIsSubTargetActive()") == "0");
    auto f            = world();
    f.subTargetActive = true;
    p.m->update(f);
    CHECK(p.eval(target + "GetTargetIndex(0)") == "1025"); // the sub-target cursor
    CHECK(p.eval(target + "GetTargetIndex(1)") == "80");   // the main target
    CHECK(p.eval(target + "GetServerId(1)") == "16842832");
    CHECK(p.eval(target + "GetWindowName()") == "Forest Hare");
}

TEST_CASE("ashita memory: inventory and treasure", "[ashita]")
{
    Probe p;
    p.m->update(world());
    const std::string inv = "AshitaCore:GetMemoryManager():GetInventory():";
    CHECK(p.eval(inv + "GetContainerItem(0, 0).Id") == "65535"); // gil
    CHECK(p.eval(inv + "GetContainerItem(0, 0).Count") == "5000");
    CHECK(p.eval(inv + "GetContainerItem(0, 3).Id") == "4112");
    CHECK(p.eval(inv + "GetContainerItem(0, 4).Id") == "0");
    CHECK(p.eval("#" + inv + "GetContainerItem(0, 7).Extra") == "28");
    CHECK(p.eval("struct.unpack('B', " + inv + "GetContainerItem(0, 7).Extra, 1)") == "2");
    CHECK(p.eval(inv + "GetContainerItem(0, 7).Flags") == "5");
    CHECK(p.eval(inv + "GetContainerCount(0)") == "2");
    CHECK(p.eval(inv + "GetContainerCountMax(0)") == "31");
    CHECK(p.eval(inv + "GetContainerCountMax(5)") == "0");
    CHECK(p.eval(inv + "GetEquippedItem(0).Index") == "7");
    CHECK(p.eval(inv + "GetTreasurePoolItem(2).ItemId") == "4112");
    CHECK(p.eval(inv + "GetTreasurePoolItem(2).WinningLot") == "900");
    CHECK(p.eval(inv + "GetTreasurePoolItem(2).TimeToLive > 290") == "true");
    CHECK(p.eval(inv + "GetTreasurePoolItem(0).ItemId") == "0");
    CHECK(p.eval(inv + "GetTreasurePoolItemCount()") == "1");
    CHECK(p.eval("AshitaCore:GetMemoryManager():GetParty():GetMemberTreasureLot(0, 2)") == "512");
}

TEST_CASE("ashita memory: before login", "[ashita]")
{
    Probe p;
    p.m->update({});
    CHECK(p.eval("GetPlayerEntity()") == "nil");
    CHECK(p.eval("AshitaCore:GetMemoryManager():GetParty():GetMemberServerId(0)") == "0");
    CHECK(p.eval("AshitaCore:GetMemoryManager():GetPlayer():GetLoginStatus()") == "0");
    CHECK(p.eval("AshitaCore:GetMemoryManager():GetInventory():GetContainerCountMax(0)") == "0");
}

// Real addons (not part of the repository): XI_ASHITA_ROOT names an Ashita root whose addons/ holds them and libs/.
// Each listed addon (XI_ASHITA_ADDONS, comma-separated; default the six that read the memory interfaces most) is loaded
// into the hand-built world, fed frames with d3d_present, a /check result and an action packet, and must log no error.
TEST_CASE("ashita memory: real addons", "[ashita][addons]")
{
    const char* rootEnv = std::getenv("XI_ASHITA_ROOT");
    if (!rootEnv || !std::filesystem::exists(std::filesystem::path(rootEnv) / "addons"))
    {
        SKIP("XI_ASHITA_ROOT not set");
    }
    auto ring = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(512);
    spdlog::default_logger()->sinks().push_back(ring);
    std::vector<std::string> chat;
    xi::ashita::Host         host;
    host.chat = [&](const std::string& text, int) { chat.push_back(text); };
    AddonManager m(rootEnv, xi::ashita::Profile::HorizonXI, installResources().value_or(xi::ashita::Resources{}), std::move(host));
    std::string list = std::getenv("XI_ASHITA_ADDONS") ? std::getenv("XI_ASHITA_ADDONS") : "tparty,recast,petinfo,timers,luashitacast,checker";
    std::vector<std::string> names;
    for (std::size_t i = 0; i <= list.size();)
    {
        const auto j = std::min(list.find(',', i), list.size());
        if (j > i)
        {
            names.push_back(list.substr(i, j - i));
        }
        i = j + 1;
    }
    ImGui::CreateContext();
    ImGuiIO& io     = ImGui::GetIO();
    io.IniFilename  = nullptr;
    io.DisplaySize  = { 1600, 900 };
    io.DeltaTime   = 1.0f / 60.0f;
    unsigned char* px = nullptr;
    int            tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    m.update(world());
    for (const auto& n : names)
    {
        CHECK(m.load(n));
    }
    auto frames = [&](int count, xi::ashita::Frame f) {
        for (int k = 0; k < count; ++k)
        {
            m.update(f);
            io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th); // fonts the addons asked for were added since
            ImGui::NewFrame();
            m.present();
            ImGui::Render();
        }
    };
    frames(5, world());
    // checker: a /check answer (S2C 0x029: Param1 @0x0C level, Param2 @0x10 type, target index @0x16, message @0x18).
    xi::ashita::PacketEvent check;
    check.id   = 0x029;
    check.data.assign(0x20, std::byte{ 0 });
    check.data[0] = std::byte{ 0x29 };
    check.data[1] = std::byte{ (0x20 / 4) << 1 };
    check.data[0x0C] = std::byte{ 10 };
    check.data[0x16] = std::byte{ 0x50 };
    check.data[0x18] = std::byte{ 0xF9 };
    m.packetIn(check);
    // An action of our pet's (S2C 0x028: actor id @5, as petinfo reads it).
    xi::ashita::PacketEvent act;
    act.id = 0x028;
    act.data.assign(0x40, std::byte{ 0 });
    act.data[0] = std::byte{ 0x28 };
    act.data[1] = std::byte{ (0x40 / 4) << 1 };
    const uint32_t petId = 0x1000700;
    std::memcpy(act.data.data() + 5, &petId, 4);
    m.packetIn(act);
    // Our position update (C2S 0x015), alone in its datagram: LuAshitacast runs its HandleDefault per outgoing chunk.
    xi::ashita::PacketEvent pos;
    pos.id = 0x015;
    pos.data.assign(0x20, std::byte{ 0 });
    pos.data[0] = std::byte{ 0x15 };
    pos.data[1] = std::byte{ (0x20 / 4) << 1 };
    pos.chunk   = pos.data;
    m.packetOut(pos);
    auto sub            = world();
    sub.subTargetActive = true;
    frames(5, sub);
    m.command("/lac list", 1);
    m.command("/recast", 1);
    frames(2, world());
    m.unloadAll();
    ImGui::DestroyContext();
    std::vector<std::string> errors;
    for (const auto& line : ring->last_formatted())
    {
        if (line.find("[error]") != std::string::npos && line.find("ashita") != std::string::npos)
        {
            errors.push_back(line);
        }
        if (line.find("not available in ffxi-linux") != std::string::npos)
        {
            WARN(line);
        }
    }
    const bool verbose = std::getenv("XI_ASHITA_VERBOSE") != nullptr; // the addons' log lines, for a reader
    for (const auto& c : chat)
    {
        if (verbose)
        {
            WARN("chat: " << c);
        }
        else
        {
            UNSCOPED_INFO("chat: " << c);
        }
    }
    for (const auto& e : errors)
    {
        UNSCOPED_INFO(e);
    }
    CHECK(errors.empty());
    auto& sinks = spdlog::default_logger()->sinks();
    sinks.erase(std::remove(sinks.begin(), sinks.end(), ring), sinks.end());
}

TEST_CASE("ashita resources: string tables", "[ashita][data]")
{
    const auto res = installResources();
    if (!res)
    {
        SKIP("FFXI_ROOT not set");
    }
    Probe p(*res);
    p.m->update(world());
    const std::string rm = "AshitaCore:GetResourceManager():";
    CHECK(p.eval(rm + "GetString('jobs.names_abbr', 3)") == "WHM");
    CHECK(p.eval(rm + "GetString('zones.names', 230)") == "Southern San d'Oria");
    CHECK(p.eval(rm + "GetString('zones.names', \"Southern San d'Oria\")") == "230");
    CHECK(p.eval(rm + "GetString('buffs.names', 40)") == "Protect");
    CHECK(p.eval(rm + "GetString('buffs.names', -1)") == "nil");
    CHECK(p.eval(rm + "GetString('monsters.abilities', 257 - 256)") == "Foot Kick");   // LSB mob skill 257
    CHECK(p.eval(rm + "GetString('monsters.abilities', 301 - 256)") == "Dream Flower"); // 301
    CHECK(p.eval(rm + "GetString('monsters.abilities', 'dream flower')") == "45");
    CHECK(p.eval(rm + "GetString('items.names', 4112)") == "Potion");
    CHECK(p.eval(rm + "GetString('keyitems.names', 1) ~= nil") == "true");
    CHECK(p.eval(rm + "GetString('nosuch.table', 1)") == "nil");
    CHECK(p.eval(rm + "GetItemById(4112).Name[1]") == "Potion");
    CHECK(p.eval(rm + "GetStatusIconById(40).Description[1]") == "Protect");
    // Icons as BMP files (32x32, 32-bit): the header and the size field.
    CHECK(p.eval(rm + "GetStatusIconById(40).Bitmap:sub(1, 2)") == "BM");
    CHECK(p.eval(rm + "GetStatusIconById(40).ImageSize") == std::to_string(14 + 40 + 32 * 32 * 4));
    CHECK(p.eval("struct.unpack('I', " + rm + "GetStatusIconById(40).Bitmap, 3)") == std::to_string(14 + 40 + 32 * 32 * 4));
    CHECK(p.eval(rm + "GetItemById(4112).Bitmap:sub(1, 2)") == "BM");
    CHECK(p.eval(rm + "GetItemById(4112).Flags ~= nil") == "true");
}
