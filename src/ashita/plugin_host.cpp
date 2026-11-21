// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita plugins built natively for Linux (issue #34, docs/ashita-compat.md "Plugins"): the host and the IAshitaCore it
// gives them. Compiled only when the Ashita SDK headers were found (cmake/ashita_plugins.cmake); plugin_host_none.cpp
// stands in otherwise.
//
// The interfaces answer from the same per-frame state as the Lua layer's (src/ashita/prelude_memory.lua, whose notes on
// what each value means apply here too); every method this file does not implement answers zero / nothing through the
// generated XiStub_* classes, as a plugin would see an idle client.
#include "ashita/ashita.h"
#include "ashita/plugin_host.h"
#include "dat/items.h"
#include "dat/messages.h"
#include "dat/spells.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <map>
#include <numbers>
#include <unordered_map>

#include <dlfcn.h>
#include <unistd.h>

// After the client's own headers: Wine's windows.h defines many macros.
#include "ashita/plugins/compat.h"
#include "Ashita.h"
#include "xi_sdk_stubs.h"

namespace xi::ashita
{

void pluginSetBagFlags(uint8_t flags); // plugins/win32.cpp: the stand-in for the game's wardrobe flags

// The manager's internals the plugin host reads and queues into (it is the plugins' side of AddonManager).
struct PluginBridge
{
    static const Frame& frame(const AddonManager& m)
    {
        return m.frame_;
    }
    static const Resources& resources(const AddonManager& m)
    {
        return m.res_;
    }
    static const Host& host(const AddonManager& m)
    {
        return m.host_;
    }
    static const std::filesystem::path& root(const AddonManager& m)
    {
        return m.root_;
    }
    static uint64_t frameNumber(const AddonManager& m)
    {
        return m.frameNo_;
    }
    static void say(AddonManager& m, const std::string& text)
    {
        m.say(text);
    }
    // As ChatManager:QueueCommand from an addon: run on the next flush, seen again as an injected command.
    static void queue(AddonManager& m, std::string line, int mode)
    {
        m.injectedLines_.push_back(line);
        m.pendingCommands_.emplace_back(std::move(line), mode);
    }
};

namespace
{

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Writef / Logf are printf-style by the SDK's contract: the format is the plugin's, as under Ashita.
std::string vformat(const char* format, va_list args)
{
    std::array<char, 4096> buf{};
    std::vsnprintf(buf.data(), buf.size(), format, args); // Flawfinder: ignore (the plugin's own format, bounded)
    return std::string(buf.data());
}

class Core;

// ---------------------------------------------------------------------------------------------------------------
// The state the memory interfaces read: the frame's world, entities by index (ourselves included) and the party slots
// as Ashita lays them out (prelude_memory.lua party_slots).
// ---------------------------------------------------------------------------------------------------------------
struct Slot
{
    uint32_t    id    = 0;
    uint16_t    index = 0;
    std::string name;
    uint16_t    zone = 0;
    uint32_t    hp = 0, mp = 0, tp = 0;
    uint8_t     hpp = 0, mpp = 0;
    uint8_t     mainJob = 0, mainLevel = 0, subJob = 0, subLevel = 0;
};

struct State
{
    uint64_t                                     frame = ~uint64_t{ 0 };
    const client::WorldSnapshot*                 world = nullptr;
    std::unordered_map<uint16_t, client::Entity> entities; // by target index
    std::map<uint32_t, Slot>                     party;    // by Ashita's party slot (0 = us, 6.. / 12.. the alliance)
    std::vector<Ashita::FFXI::entity_t>          raw;      // GetRawEntity, filled on demand
    std::vector<uint64_t>                        rawFrame;
    std::array<std::array<Ashita::FFXI::item_t, 81>, 18> items{};
    std::array<Ashita::FFXI::equipmententry_t, 16>       equipped{};
    std::array<int16_t, 32>                              buffs{};
    glm::vec3                                            me{ 0.0f };
};

class Memory;

class Entity final : public XiStub_IEntity
{
public:
    explicit Entity(Core& c)
    : c_(c)
    {
    }
    Ashita::FFXI::entity_t* GetRawEntity(uint32_t index) const override;
    uint32_t                GetEntityMapSize() const override
    {
        return 0x900;
    }
    float GetLocalPositionX(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->pos.x : 0.0f;
    }
    float GetLocalPositionY(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->pos.z : 0.0f; // Ashita's Y runs across the ground; the session keeps the height in y
    }
    float GetLocalPositionZ(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->pos.y : 0.0f;
    }
    float GetLocalPositionYaw(uint32_t index) const override
    {
        return GetHeading(index);
    }
    float GetHeading(uint32_t index) const override
    {
        const auto* e = find(index);
        if (!e)
        {
            return 0.0f;
        }
        double h = e->dir * (2.0 * std::numbers::pi / 256.0);
        if (h > std::numbers::pi)
        {
            h -= 2.0 * std::numbers::pi;
        }
        return static_cast<float>(h);
    }
    uint32_t GetTargetIndex(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->index : 0;
    }
    uint32_t GetServerId(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->id : 0;
    }
    const char* GetName(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->name.c_str() : "";
    }
    float   GetDistance(uint32_t index) const override; // squared, as the game keeps it
    uint8_t GetHPPercent(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->hpp : 0;
    }
    uint8_t GetType(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? type(*e) : 0;
    }
    uint8_t GetRace(uint32_t index) const override
    {
        const auto* e = find(index);
        return e && humanoid(*e) ? static_cast<uint8_t>(e->look.ids[0] >> 8) : 0;
    }
    uint16_t GetLookHair(uint32_t index) const override
    {
        return look(index, 0);
    }
    uint16_t GetLookHead(uint32_t index) const override
    {
        return look(index, 1);
    }
    uint16_t GetLookBody(uint32_t index) const override
    {
        return look(index, 2);
    }
    uint16_t GetLookHands(uint32_t index) const override
    {
        return look(index, 3);
    }
    uint16_t GetLookLegs(uint32_t index) const override
    {
        return look(index, 4);
    }
    uint16_t GetLookFeet(uint32_t index) const override
    {
        return look(index, 5);
    }
    uint16_t GetLookMain(uint32_t index) const override
    {
        return look(index, 6);
    }
    uint16_t GetLookSub(uint32_t index) const override
    {
        return look(index, 7);
    }
    uint16_t GetLookRanged(uint32_t index) const override
    {
        return look(index, 8);
    }
    uint32_t GetRenderFlags0(uint32_t index) const override
    {
        return renderFlags(index, 0);
    }
    uint32_t GetRenderFlags1(uint32_t index) const override
    {
        return renderFlags(index, 1);
    }
    uint32_t GetStatus(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->animation : 0;
    }
    uint32_t GetStatusServer(uint32_t index) const override
    {
        return GetStatus(index);
    }
    uint32_t GetClaimStatus(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->claimId : 0;
    }
    uint32_t GetSpawnFlags(uint32_t index) const override;
    uint16_t GetPetTargetIndex(uint32_t index) const override;
    uint16_t GetTargetedIndex(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->targetIndex : 0;
    }
    uint8_t GetMountId(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->mountIndex : 0;
    }
    float GetModelHitboxSize(uint32_t index) const override
    {
        const auto* e = find(index);
        return e ? e->hitbox : 0.0f;
    }

    [[nodiscard]] const client::Entity* find(uint32_t index) const;

private:
    static bool humanoid(const client::Entity& e)
    {
        return e.isPc || e.look.kind == 1 || e.look.kind == 0xFFFF;
    }
    static uint8_t type(const client::Entity& e)
    {
        if (e.isPc)
        {
            return 0;
        }
        return e.look.kind >= 2 && e.look.kind <= 4 ? 3 : (e.look.kind == 0 ? 2 : 1);
    }
    [[nodiscard]] uint16_t look(uint32_t index, std::size_t slot) const
    {
        const auto* e = find(index);
        if (!e)
        {
            return 0;
        }
        const uint16_t v = e->look.ids[slot];
        return slot == 0 && humanoid(*e) ? static_cast<uint16_t>(v & 0xFF) : v;
    }
    // As prelude_memory.lua render_flags: Flags0 0x200 drawn, 0x2000 targetable; Flags1 0x1000000 targetable (INFERRED).
    [[nodiscard]] uint32_t renderFlags(uint32_t index, int which) const
    {
        const auto* e = find(index);
        if (!e || e->hiddenByStatus(false))
        {
            return 0;
        }
        if (which == 0)
        {
            return e->targetOff ? 0x200u : 0x2200u;
        }
        return e->targetOff ? 0u : 0x1000000u;
    }
    Core& c_;
};

class Party final : public XiStub_IParty
{
public:
    explicit Party(Core& c)
    : c_(c)
    {
    }
    const char* GetMemberName(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->name.c_str() : "";
    }
    uint32_t GetMemberServerId(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->id : 0;
    }
    uint32_t GetMemberServerId2(uint32_t i) const override
    {
        return GetMemberServerId(i);
    }
    uint32_t GetMemberTargetIndex(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->index : 0;
    }
    uint32_t GetMemberHP(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->hp : 0;
    }
    uint32_t GetMemberMP(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->mp : 0;
    }
    uint32_t GetMemberTP(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->tp : 0;
    }
    uint8_t GetMemberHPPercent(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->hpp : 0;
    }
    uint8_t GetMemberMPPercent(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->mpp : 0;
    }
    uint16_t GetMemberZone(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->zone : 0;
    }
    uint8_t GetMemberMainJob(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->mainJob : 0;
    }
    uint8_t GetMemberMainJobLevel(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->mainLevel : 0;
    }
    uint8_t GetMemberSubJob(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->subJob : 0;
    }
    uint8_t GetMemberSubJobLevel(uint32_t i) const override
    {
        const auto* s = slot(i);
        return s ? s->subLevel : 0;
    }
    uint8_t GetMemberIsActive(uint32_t i) const override
    {
        return slot(i) ? 1 : 0;
    }

private:
    [[nodiscard]] const Slot* slot(uint32_t i) const;
    Core&                     c_;
};

class Player final : public XiStub_IPlayer
{
public:
    explicit Player(Core& c)
    : c_(c)
    {
    }
    uint32_t GetHPMax() const override;
    uint32_t GetMPMax() const override;
    uint8_t  GetMainJob() const override;
    uint8_t  GetMainJobLevel() const override;
    uint8_t  GetSubJob() const override;
    uint8_t  GetSubJobLevel() const override;
    int16_t* GetBuffs() const override;
    uint8_t  GetLoginStatus() const override;
    bool     HasAbility(uint32_t id) const override;
    bool     HasSpell(uint32_t id) const override;
    bool     HasWeaponSkill(uint32_t id) const override;
    bool     HasKeyItem(uint32_t id) const override;
    bool     HasAbilityData() const override
    {
        return true;
    }
    bool HasSpellData() const override
    {
        return true;
    }

private:
    Core& c_;
};

// Two entries, as prelude_memory.lua's ITarget: with the sub-target cursor up, entry 0 is the cursor's and entry 1
// the main target; otherwise entry 0 is the target.
class Target final : public XiStub_ITarget
{
public:
    explicit Target(Core& c)
    : c_(c)
    {
    }
    uint32_t GetTargetIndex(uint32_t i) const override;
    uint32_t GetServerId(uint32_t i) const override;
    uint8_t  GetIsSubTargetActive() const override;
    uint8_t  GetIsActive(uint32_t i) const override
    {
        return GetTargetIndex(i) != 0 ? 1 : 0;
    }
    void SetTarget(uint32_t index, bool) const override;

private:
    Core& c_;
};

class Inventory final : public XiStub_IInventory
{
public:
    explicit Inventory(Core& c)
    : c_(c)
    {
    }
    Ashita::FFXI::item_t*           GetContainerItem(uint32_t containerId, uint32_t index) const override;
    uint32_t                        GetContainerCount(uint32_t containerId) const override;
    uint32_t                        GetContainerCountMax(uint32_t containerId) const override;
    Ashita::FFXI::equipmententry_t* GetEquippedItem(uint32_t index) const override;

private:
    Core& c_;
};

class Memory final : public XiStub_IMemoryManager
{
public:
    explicit Memory(Core& c)
    : entity(c)
    , party(c)
    , player(c)
    , target(c)
    , inventory(c)
    {
    }
    IAutoFollow* GetAutoFollow() const override
    {
        return &autoFollow;
    }
    ICastBar* GetCastBar() const override
    {
        return &castBar;
    }
    IEntity* GetEntity() const override
    {
        return &entity;
    }
    IInventory* GetInventory() const override
    {
        return &inventory;
    }
    IParty* GetParty() const override
    {
        return &party;
    }
    IPlayer* GetPlayer() const override
    {
        return &player;
    }
    IRecast* GetRecast() const override
    {
        return &recast;
    }
    ITarget* GetTarget() const override
    {
        return &target;
    }

    mutable Entity             entity;
    mutable Party              party;
    mutable Player             player;
    mutable Target             target;
    mutable Inventory          inventory;
    mutable XiStub_IAutoFollow autoFollow;
    mutable XiStub_ICastBar    castBar;
    mutable XiStub_IRecast     recast;
};

class Chat final : public XiStub_IChatManager
{
public:
    explicit Chat(Core& c)
    : c_(c)
    {
    }
    void ParseCommand(int32_t mode, const char* command) override
    {
        QueueCommand(mode, command);
    }
    void QueueCommand(int32_t mode, const char* command) override;
    void Write(int32_t mode, bool indent, const char* message) override;
    void Writef(int32_t mode, bool indent, const char* format, ...) override
    {
        va_list args;
        va_start(args, format);
        const std::string s = vformat(format, args);
        va_end(args);
        Write(mode, indent, s.c_str());
    }
    void AddChatMessage(int32_t mode, bool indent, const char* message) override
    {
        Write(mode, indent, message);
    }
    // The client's command line already reads auto-translate tokens as their text (GameUi::submitLine), so a command
    // comes through unchanged.
    int32_t ParseAutoTranslate(const char* message, char* buffer, int32_t bufferSize, bool) const override
    {
        if (buffer == nullptr || bufferSize <= 0)
        {
            return 0;
        }
        const auto n =
            static_cast<int32_t>(std::min<std::size_t>(std::strlen(message), static_cast<std::size_t>(bufferSize - 1)));
        std::memcpy(buffer, message, static_cast<std::size_t>(n));
        buffer[n] = '\0';
        return n;
    }

private:
    Core& c_;
};

class Packets final : public XiStub_IPacketManager
{
public:
    explicit Packets(Core& c)
    : c_(c)
    {
    }
    // Ashita passes whole packets (the 4-byte header included), as the Lua layer's AddOutgoingPacket.
    void AddIncomingPacket(uint16_t id, uint32_t len, uint8_t* data) override;
    void AddOutgoingPacket(uint16_t id, uint32_t len, uint8_t* data) override;

private:
    Core& c_;
};

class ResourceMgr final : public XiStub_IResourceManager
{
public:
    explicit ResourceMgr(Core& c)
    : c_(c)
    {
    }
    IAbility*   GetAbilityById(uint32_t id) const override;
    IAbility*   GetAbilityByName(const char* name, uint32_t langId) const override;
    ISpell*     GetSpellById(uint32_t id) const override;
    ISpell*     GetSpellByName(const char* name, uint32_t langId) const override;
    IItem*      GetItemById(uint32_t id) const override;
    IItem*      GetItemByName(const char* name, uint32_t langId) const override;
    uint32_t    GetEntityIndexById(uint32_t id) const override;
    uint32_t    GetEntityIdByIndex(uint32_t index) const override;
    const char* GetEntityNameByIndex(uint32_t index) const override;

private:
    struct AbilityRes
    {
        IAbility    a{};
        std::string name, help;
    };
    struct SpellRes
    {
        ISpell      s{};
        std::string name, help;
    };
    // Items: Ashita keeps every item; an IItem is 2.7 KB (its icon bitmap), so this keeps the last 256 handed out
    // (a plugin reads the record right after asking, as Ashita's own plugins do; INFERRED to be enough).
    struct ItemRes
    {
        IItem       i{};
        std::string name, help, log, plural;
        uint16_t    id = 0;
    };
    Core&                                                             c_;
    mutable std::unordered_map<uint32_t, std::unique_ptr<AbilityRes>> abilities_;
    mutable std::unordered_map<uint32_t, std::unique_ptr<SpellRes>>   spells_;
    mutable std::deque<std::unique_ptr<ItemRes>>                      items_;
    mutable std::unordered_map<std::string, uint16_t>                 itemNames_;
};

class LogMgr final : public XiStub_ILogManager
{
public:
    bool Log(uint32_t level, const char* source, const char* message) override
    {
        // Ashita's levels: 0 none, 1 critical, 2 error, 3 warn, 4 info, 5 debug.
        if (level <= 2)
        {
            spdlog::error("plugin {}: {}", source ? source : "?", message ? message : "");
        }
        else if (level == 3)
        {
            spdlog::warn("plugin {}: {}", source ? source : "?", message ? message : "");
        }
        else
        {
            spdlog::debug("plugin {}: {}", source ? source : "?", message ? message : "");
        }
        return true;
    }
    bool Logf(uint32_t level, const char* source, const char* format, ...) override
    {
        va_list args;
        va_start(args, format);
        const std::string s = vformat(format, args);
        va_end(args);
        return Log(level, source, s.c_str());
    }
    uint32_t GetLogLevel() const override
    {
        return 5;
    }
};

class HostImpl;

class Plugins final : public XiStub_IPluginManager
{
public:
    explicit Plugins(HostImpl& h)
    : h_(h)
    {
    }
    bool     Load(const char* name, const char* args) override;
    bool     Unload(const char* name) override;
    void     UnloadAll() override;
    bool     IsLoaded(const char* name) override;
    void*    Get(const char* name) override;
    void*    Get(uint32_t index) override;
    uint32_t Count() override;

private:
    HostImpl& h_;
};

class Core final : public XiStub_IAshitaCore
{
public:
    Core(AddonManager& m, HostImpl& h)
    : m_(m)
    , memory_(*this)
    , chat_(*this)
    , packets_(*this)
    , resources_(*this)
    , plugins_(h)
    {
        // With the trailing separator, as Ashita answers it ("C:\\Ashita\\"): plugins append "config/..." directly.
        install_ = PluginBridge::root(m).string();
        if (install_.empty() || install_.back() != '/')
        {
            install_ += '/';
        }
    }
    const char* GetInstallPath() const override
    {
        return install_.c_str();
    }
    IProperties* GetProperties() const override
    {
        return &properties_;
    }
    IChatManager* GetChatManager() const override
    {
        return &chat_;
    }
    IConfigurationManager* GetConfigurationManager() const override
    {
        return &config_;
    }
    IFontManager* GetFontManager() const override
    {
        return &fonts_;
    }
    IGuiManager* GetGuiManager() const override
    {
        return &gui_;
    }
    IInputManager* GetInputManager() const override
    {
        return &input_;
    }
    IMemoryManager* GetMemoryManager() const override
    {
        return &memory_;
    }
    IOffsetManager* GetOffsetManager() const override
    {
        return &offsets_;
    }
    IPacketManager* GetPacketManager() const override
    {
        return &packets_;
    }
    IPluginManager* GetPluginManager() const override
    {
        return &plugins_;
    }
    IPolPluginManager* GetPolPluginManager() const override
    {
        return &polPlugins_;
    }
    IPointerManager* GetPointerManager() const override
    {
        return &pointers_;
    }
    IPrimitiveManager* GetPrimitiveManager() const override
    {
        return &primitives_;
    }
    IResourceManager* GetResourceManager() const override
    {
        return &resources_;
    }

    AddonManager& manager() const
    {
        return m_;
    }
    // The state of the current frame (rebuilt when the frame changes).
    State& state() const;

private:
    AddonManager&                        m_;
    std::string                          install_;
    mutable State                        state_;
    mutable Memory                       memory_;
    mutable Chat                         chat_;
    mutable Packets                      packets_;
    mutable ResourceMgr                  resources_;
    mutable Plugins                      plugins_;
    mutable XiStub_IProperties           properties_;
    mutable XiStub_IConfigurationManager config_;
    mutable XiStub_IFontManager          fonts_;
    mutable XiStub_IGuiManager           gui_;
    mutable XiStub_IInputManager         input_;
    mutable XiStub_IOffsetManager        offsets_;
    mutable XiStub_IPolPluginManager     polPlugins_;
    mutable XiStub_IPointerManager       pointers_;
    mutable XiStub_IPrimitiveManager     primitives_;
};

State& Core::state() const
{
    const auto  frameNo = PluginBridge::frameNumber(m_);
    const auto& fr      = PluginBridge::frame(m_);
    auto&       s       = state_;
    if (s.frame == frameNo && s.world == fr.world.get())
    {
        return s;
    }
    s.frame = frameNo;
    s.world = fr.world.get();
    s.entities.clear();
    s.party.clear();
    s.raw.assign(0x900, Ashita::FFXI::entity_t{});
    s.rawFrame.assign(0x900, ~uint64_t{ 0 });
    for (auto& box : s.items)
    {
        box.fill(Ashita::FFXI::item_t{});
    }
    s.equipped.fill(Ashita::FFXI::equipmententry_t{});
    s.buffs.fill(-1);
    s.me = fr.self.pos;
    if (!s.world)
    {
        return s;
    }
    const auto& w = *s.world;
    for (const auto& e : w.entities)
    {
        if (e.index != 0 && e.id != w.charId)
        {
            s.entities[e.index] = e;
        }
    }
    if (w.charId != 0 && w.actIndex != 0)
    {
        // Ourselves as the Lua layer's entities() builds us.
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
        s.entities[w.actIndex] = std::move(self);
    }
    // Party slots: 0 us, 1..5 our party in list order, 6..11 and 12..17 the alliance's other parties.
    std::vector<const client::PartyMember*> mine, other[2];
    for (const auto& p : w.party)
    {
        if (p.id == w.charId)
        {
            continue;
        }
        (p.partyNo == 0 ? mine : other[std::min<std::size_t>(p.partyNo, 2) - 1]).push_back(&p);
    }
    auto byOrder = [](const client::PartyMember* a, const client::PartyMember* b) { return a->order < b->order; };
    auto toSlot  = [](const client::PartyMember& p) {
        return Slot{ p.id,  p.index, p.name,    p.zone,      p.hp,     p.mp,      p.tp,
                     p.hpp, p.mpp,   p.mainJob, p.mainLevel, p.subJob, p.subLevel };
    };
    if (w.charId != 0)
    {
        s.party[0] = Slot{ w.charId, w.actIndex, w.charName, w.zone,      w.hp,     w.mp,      w.tp,
                           w.hpp,    w.mpp,      w.mainJob,  w.mainLevel, w.subJob, w.subLevel };
    }
    std::ranges::sort(mine, byOrder);
    for (std::size_t i = 0; i < mine.size() && i < 5; ++i)
    {
        s.party[static_cast<uint32_t>(i + 1)] = toSlot(*mine[i]);
    }
    for (std::size_t p = 0; p < 2; ++p)
    {
        std::ranges::sort(other[p], byOrder);
        for (std::size_t i = 0; i < other[p].size() && i < 6; ++i)
        {
            s.party[static_cast<uint32_t>(6 * (p + 1) + i)] = toSlot(*other[p][i]);
        }
    }
    // Containers: every slot answers, empty ones with Id 0; slot 0 of the inventory is the gil (Id 0xFFFF).
    for (const auto& it : w.inventory)
    {
        if (it.container < s.items.size() && it.slot < 81)
        {
            auto& d = s.items[it.container][it.slot];
            d.Id    = it.container == 0 && it.slot == 0 ? uint16_t{ 0xFFFF } : it.itemId;
            d.Index = it.slot;
            d.Count = it.count;
            d.Flags = it.lockFlags;
            d.Price = it.price;
            std::memcpy(d.Extra, it.attr.data(), std::min(sizeof(d.Extra), it.attr.size()));
        }
    }
    for (std::size_t slot = 0; slot < s.equipped.size(); ++slot)
    {
        const auto [container, index] = w.equipment[slot];
        s.equipped[slot].Slot         = static_cast<uint32_t>(slot);
        s.equipped[slot].Index        = index == 0 ? 0u : static_cast<uint32_t>(container) * 256u + index;
    }
    for (std::size_t i = 0; i < w.buffs.size() && i < s.buffs.size(); ++i)
    {
        s.buffs[i] = static_cast<int16_t>(w.buffs[i].icon);
    }
    return s;
}

// --- IEntity ---
const client::Entity* Entity::find(uint32_t index) const
{
    auto&      s  = c_.state();
    const auto it = s.entities.find(static_cast<uint16_t>(index));
    return it == s.entities.end() ? nullptr : &it->second;
}

float Entity::GetDistance(uint32_t index) const
{
    const auto* e = find(index);
    if (!e)
    {
        return 0.0f;
    }
    const glm::vec3 d = e->pos - c_.state().me;
    return glm::dot(d, d);
}

uint32_t Entity::GetSpawnFlags(uint32_t index) const
{
    const auto* e = find(index);
    if (!e)
    {
        return 0;
    }
    const auto& s = c_.state();
    if (s.world && e->id == s.world->charId)
    {
        return 0x20D;
    }
    uint32_t f = e->isPc ? 0x01u : (e->isMob ? 0x10u : 0x02u);
    for (const auto& [slot, m] : s.party)
    {
        if (m.id == e->id)
        {
            f |= slot < 6 ? 0x0Cu : 0x08u;
        }
    }
    return f;
}

uint16_t Entity::GetPetTargetIndex(uint32_t index) const
{
    const auto& s = c_.state();
    return s.world && find(index) && find(index)->id == s.world->charId ? s.world->petIndex : uint16_t{ 0 };
}

Ashita::FFXI::entity_t* Entity::GetRawEntity(uint32_t index) const
{
    auto& s = c_.state();
    if (index >= s.raw.size())
    {
        return nullptr;
    }
    const auto* e = find(index);
    if (!e)
    {
        return nullptr;
    }
    auto& r = s.raw[index];
    if (s.rawFrame[index] != s.frame)
    {
        s.rawFrame[index]            = s.frame;
        r                            = Ashita::FFXI::entity_t{};
        r.Movement.LocalPosition.X   = GetLocalPositionX(index);
        r.Movement.LocalPosition.Y   = GetLocalPositionY(index);
        r.Movement.LocalPosition.Z   = GetLocalPositionZ(index);
        r.Movement.LocalPosition.W   = 1.0f;
        r.Movement.LocalPosition.Yaw = GetHeading(index);
        r.Movement.LastPosition      = r.Movement.LocalPosition;
        r.TargetIndex                = e->index;
        r.ServerId                   = e->id;
        std::strncpy(reinterpret_cast<char*>(r.Name), e->name.c_str(), sizeof(r.Name) - 1);
        r.MovementSpeed  = 5.0f;
        r.AnimationSpeed = 1.0f;
        r.Distance       = GetDistance(index);
        r.Heading        = GetHeading(index);
        r.HPPercent      = e->hpp;
        r.Type           = type(*e);
        r.Race           = GetRace(index);
        r.DoorId         = e->doorId;
        uint16_t* look   = &r.Look.Hair;
        for (std::size_t k = 0; k < 9; ++k)
        {
            look[k] = this->look(index, k);
        }
        r.Render.Flags0 = renderFlags(index, 0);
        r.Render.Flags1 = renderFlags(index, 1);
    }
    return &r;
}

// --- IParty ---
const Slot* Party::slot(uint32_t i) const
{
    const auto& p  = c_.state().party;
    const auto  it = p.find(i);
    return it == p.end() ? nullptr : &it->second;
}

// --- IPlayer ---
uint32_t Player::GetHPMax() const
{
    const auto* w = c_.state().world;
    return w ? static_cast<uint32_t>(w->stats.hpMax) : 0;
}
uint32_t Player::GetMPMax() const
{
    const auto* w = c_.state().world;
    return w ? static_cast<uint32_t>(w->stats.mpMax) : 0;
}
uint8_t Player::GetMainJob() const
{
    const auto* w = c_.state().world;
    return w ? w->mainJob : 0;
}
uint8_t Player::GetMainJobLevel() const
{
    const auto* w = c_.state().world;
    return w ? w->mainLevel : 0;
}
uint8_t Player::GetSubJob() const
{
    const auto* w = c_.state().world;
    return w ? w->subJob : 0;
}
uint8_t Player::GetSubJobLevel() const
{
    const auto* w = c_.state().world;
    return w ? w->subLevel : 0;
}
int16_t* Player::GetBuffs() const
{
    return c_.state().buffs.data();
}
uint8_t Player::GetLoginStatus() const
{
    const auto* w = c_.state().world;
    return w && w->inZone ? 2 : 0; // 2 logged in (INFERRED from addons' checks)
}
bool Player::HasAbility(uint32_t id) const
{
    const auto* w = c_.state().world;
    if (!w)
    {
        return false;
    }
    if (id >= 512)
    {
        return std::ranges::find(w->jobAbilities, static_cast<uint16_t>(id - 512)) != w->jobAbilities.end();
    }
    return std::ranges::find(w->weaponSkills, static_cast<uint16_t>(id)) != w->weaponSkills.end();
}
bool Player::HasSpell(uint32_t id) const
{
    const auto* w = c_.state().world;
    return w && std::ranges::find(w->spells, static_cast<uint16_t>(id)) != w->spells.end();
}
bool Player::HasWeaponSkill(uint32_t id) const
{
    const auto* w = c_.state().world;
    return w && std::ranges::find(w->weaponSkills, static_cast<uint16_t>(id)) != w->weaponSkills.end();
}
bool Player::HasKeyItem(uint32_t id) const
{
    const auto* w = c_.state().world;
    return w && std::ranges::binary_search(w->keyItems, static_cast<uint16_t>(id));
}

// --- ITarget ---
uint32_t Target::GetTargetIndex(uint32_t i) const
{
    const auto& fr = PluginBridge::frame(c_.manager());
    if (fr.subTargetActive)
    {
        return i == 1 ? fr.targetIndex : (i == 0 ? fr.subTargetIndex : 0u);
    }
    return i == 0 ? fr.targetIndex : 0u;
}
uint32_t Target::GetServerId(uint32_t i) const
{
    return c_.GetMemoryManager()->GetEntity()->GetServerId(GetTargetIndex(i));
}
uint8_t Target::GetIsSubTargetActive() const
{
    return PluginBridge::frame(c_.manager()).subTargetActive ? 1 : 0;
}
void Target::SetTarget(uint32_t index, bool) const
{
    const auto& h = PluginBridge::host(c_.manager());
    if (h.target)
    {
        h.target(static_cast<uint16_t>(index));
    }
}

// --- IInventory ---
Ashita::FFXI::item_t* Inventory::GetContainerItem(uint32_t containerId, uint32_t index) const
{
    auto& s = c_.state();
    if (containerId >= s.items.size() || index >= 81)
    {
        return nullptr;
    }
    auto& it = s.items[containerId][index];
    it.Index = static_cast<uint16_t>(index);
    return &it;
}
uint32_t Inventory::GetContainerCount(uint32_t containerId) const
{
    const auto& s = c_.state();
    if (containerId >= s.items.size())
    {
        return 0;
    }
    uint32_t n = 0;
    for (std::size_t k = containerId == 0 ? 1 : 0; k < 81; ++k)
    {
        n += s.items[containerId][k].Id != 0 ? 1u : 0u;
    }
    return n;
}
// ContainerMaxCapacity: S2C 0x01C's usable count (1 + the slots; 0 for a container the character cannot use).
uint32_t Inventory::GetContainerCountMax(uint32_t containerId) const
{
    const auto* w = c_.state().world;
    if (!w || !w->containerSizeValid || containerId >= w->containerUsable.size())
    {
        return 0;
    }
    return w->containerUsable[containerId];
}
Ashita::FFXI::equipmententry_t* Inventory::GetEquippedItem(uint32_t index) const
{
    auto& s = c_.state();
    return index < s.equipped.size() ? &s.equipped[index] : nullptr;
}

// --- IChatManager ---
void Chat::QueueCommand(int32_t mode, const char* command)
{
    if (command != nullptr)
    {
        PluginBridge::queue(c_.manager(), command, mode);
    }
}
void Chat::Write(int32_t, bool, const char* message)
{
    if (message != nullptr)
    {
        PluginBridge::say(c_.manager(), message);
    }
}

// --- IPacketManager ---
void Packets::AddIncomingPacket(uint16_t, uint32_t len, uint8_t* data)
{
    const auto& h = PluginBridge::host(c_.manager());
    if (h.inject && data != nullptr && len >= 4)
    {
        const auto* b = reinterpret_cast<const std::byte*>(data);
        h.inject(std::vector<std::byte>(b, b + len));
    }
}
void Packets::AddOutgoingPacket(uint16_t id, uint32_t len, uint8_t* data)
{
    const auto& h = PluginBridge::host(c_.manager());
    if (h.send && data != nullptr)
    {
        const auto* b = reinterpret_cast<const std::byte*>(data);
        h.send(id, len > 4 ? std::vector<std::byte>(b + 4, b + len) : std::vector<std::byte>{});
    }
}

// --- IResourceManager ---
// Abilities as Ashita numbers them (the Lua layer's xi.ability): weapon skills by id, job abilities 512 + id.
IAbility* ResourceMgr::GetAbilityById(uint32_t id) const
{
    if (const auto it = abilities_.find(id); it != abilities_.end())
    {
        return it->second ? &it->second->a : nullptr;
    }
    const auto& r    = PluginBridge::resources(c_.manager());
    const auto  nm   = r.text ? r.text->abilityName(id) : std::string_view{};
    auto&       slot = abilities_[id];
    if (nm.empty())
    {
        return nullptr;
    }
    slot             = std::make_unique<AbilityRes>();
    slot->name       = std::string(nm);
    slot->help       = r.text ? std::string(r.text->abilityHelp(id)) : std::string();
    auto& a          = slot->a;
    a.Id             = static_cast<uint16_t>(id);
    const auto* info = r.abilities && id >= 512 ? r.abilities->find(static_cast<uint16_t>(id - 512)) : nullptr;
    a.Type           = info ? info->type : static_cast<uint8_t>(id < 512 ? 3 : 0);
    a.ManaCost       = info ? info->mp : uint16_t{ 0 };
    a.RecastTimerId  = info ? info->recastId : uint16_t{ 0 };
    a.Targets        = info ? info->targets : (r.abilities ? r.abilities->commandTargets(id) : uint16_t{ 0 });
    const int range  = r.abilities ? r.abilities->commandRange(id) : -1;
    a.Range          = static_cast<int8_t>(std::clamp(range, 0, 127));
    a.EOE            = 0xFF;
    for (auto& n : a.Name)
    {
        n = slot->name.c_str();
    }
    for (auto& d : a.Description)
    {
        d = slot->help.c_str();
    }
    return &a;
}
IAbility* ResourceMgr::GetAbilityByName(const char* name, uint32_t) const
{
    const std::string want = lower(name ? name : "");
    for (uint32_t id = 0; id < 2048; ++id)
    {
        if (auto* a = GetAbilityById(id); a && lower(a->Name[0]) == want)
        {
            return a;
        }
    }
    return nullptr;
}
ISpell* ResourceMgr::GetSpellById(uint32_t id) const
{
    if (const auto it = spells_.find(id); it != spells_.end())
    {
        return it->second ? &it->second->s : nullptr;
    }
    const auto& r    = PluginBridge::resources(c_.manager());
    const auto* info = r.spells && id < 0x10000 ? r.spells->find(static_cast<uint16_t>(id)) : nullptr;
    auto&       slot = spells_[id];
    if (!info)
    {
        return nullptr;
    }
    slot          = std::make_unique<SpellRes>();
    slot->name    = r.text ? std::string(r.text->spellName(id)) : std::string();
    slot->help    = r.text ? std::string(r.text->spellHelp(id)) : std::string();
    auto& s       = slot->s;
    s.Index       = info->id;
    s.Id          = info->id;
    s.Type        = info->type;
    s.Element     = info->element;
    s.Targets     = info->targets;
    s.Skill       = info->skill;
    s.ManaCost    = info->mp;
    s.CastTime    = info->castTime;
    s.RecastDelay = info->recast;
    s.Range       = static_cast<int8_t>(std::clamp<int>(info->range, 0, 127));
    s.AreaRange   = info->aoeRadius;
    for (std::size_t j = 0; j < info->levels.size() && j < 24; ++j)
    {
        s.LevelRequired[j] = info->levels[j] == 0xFF ? int16_t{ -1 } : static_cast<int16_t>(info->levels[j]);
    }
    s.EOE = 0xFF;
    for (auto& n : s.Name)
    {
        n = slot->name.c_str();
    }
    for (auto& d : s.Description)
    {
        d = slot->help.c_str();
    }
    return &s;
}
ISpell* ResourceMgr::GetSpellByName(const char* name, uint32_t) const
{
    const std::string want = lower(name ? name : "");
    for (uint32_t id = 0; id < 1024; ++id)
    {
        if (auto* s = GetSpellById(id); s && lower(s->Name[0]) == want)
        {
            return s;
        }
    }
    return nullptr;
}
IItem* ResourceMgr::GetItemById(uint32_t id) const
{
    const auto& r = PluginBridge::resources(c_.manager());
    // Item 0 is the blank record (".") in the data; Ashita has no item there.
    if (!r.items || id == 0 || id > 0xFFFF)
    {
        return nullptr;
    }
    for (const auto& it : items_)
    {
        if (it->id == id)
        {
            return &it->i;
        }
    }
    const auto info = r.items->find(static_cast<uint16_t>(id));
    if (!info || info->name.empty() || info->name == ".")
    {
        return nullptr;
    }
    auto res        = std::make_unique<ItemRes>();
    res->id         = static_cast<uint16_t>(id);
    res->name       = info->name;
    res->help       = info->description;
    res->log        = info->logName;
    res->plural     = info->logPlural;
    auto& i         = res->i;
    i.Id            = id;
    i.Flags         = info->flags;
    i.StackSize     = info->stackSize;
    i.Type          = info->type;
    i.Targets       = info->targets;
    i.Level         = info->level;
    i.Slots         = info->slots;
    i.Races         = info->races;
    i.Jobs          = info->jobs;
    i.SuperiorLevel = info->superiorLevel;
    i.MaxCharges    = info->maxCharges;
    i.RecastDelay   = info->recast;
    i.Skill         = info->skill;
    for (std::size_t k = 0; k < 3; ++k)
    {
        i.Name[k]            = res->name.c_str();
        i.Description[k]     = res->help.c_str();
        i.LogNameSingular[k] = res->log.c_str();
        i.LogNamePlural[k]   = res->plural.c_str();
    }
    items_.push_front(std::move(res));
    if (items_.size() > 256)
    {
        items_.pop_back();
    }
    return &items_.front()->i;
}
IItem* ResourceMgr::GetItemByName(const char* name, uint32_t) const
{
    const auto& r = PluginBridge::resources(c_.manager());
    if (!r.items || name == nullptr)
    {
        return nullptr;
    }
    if (itemNames_.empty())
    {
        for (uint32_t id = 1; id < 0x10000; ++id)
        {
            if (const auto it = r.items->find(static_cast<uint16_t>(id)); it && !it->name.empty() && it->name != ".")
            {
                itemNames_.try_emplace(lower(it->name), static_cast<uint16_t>(id));
            }
        }
    }
    const auto f = itemNames_.find(lower(name));
    return f == itemNames_.end() ? nullptr : GetItemById(f->second);
}
uint32_t ResourceMgr::GetEntityIndexById(uint32_t id) const
{
    for (const auto& [index, e] : c_.state().entities)
    {
        if (e.id == id)
        {
            return index;
        }
    }
    return 0;
}
uint32_t ResourceMgr::GetEntityIdByIndex(uint32_t index) const
{
    return c_.GetMemoryManager()->GetEntity()->GetServerId(index);
}
const char* ResourceMgr::GetEntityNameByIndex(uint32_t index) const
{
    return c_.GetMemoryManager()->GetEntity()->GetName(index);
}

// ---------------------------------------------------------------------------------------------------------------
// The host: loaded plugins in priority order.
// ---------------------------------------------------------------------------------------------------------------
struct Loaded
{
    std::string            name; // as the plugin names itself
    std::filesystem::path  file;
    void*                  handle  = nullptr;
    IPlugin*               plugin  = nullptr;
    export_DestroyPlugin_f destroy = nullptr;
    uint32_t               flags   = 0;
};

class HostImpl final : public PluginHost
{
public:
    explicit HostImpl(AddonManager& m)
    : m_(m)
    , core_(m, *this)
    {
    }
    ~HostImpl() override
    {
        unloadAll();
    }
    HostImpl(const HostImpl&)            = delete;
    HostImpl& operator=(const HostImpl&) = delete;

    bool load(const std::string& name, const std::string& args, std::string& error) override;
    bool unload(const std::string& name) override;
    void unloadAll() override
    {
        while (!loaded_.empty())
        {
            unload(loaded_.back().name);
        }
    }
    [[nodiscard]] std::vector<std::string> loaded() const override
    {
        std::vector<std::string> v;
        for (const auto& p : loaded_)
        {
            v.push_back(p.name);
        }
        return v;
    }
    [[nodiscard]] bool isLoaded(const std::string& name) const override
    {
        return find(name) != nullptr;
    }
    bool command(std::string& line, int mode, bool injected) override;
    void packet(bool incoming, PacketEvent& e) override;
    void frame() override;

    [[nodiscard]] const Loaded* find(const std::string& name) const
    {
        const auto n = lower(name);
        for (const auto& p : loaded_)
        {
            if (lower(p.name) == n || lower(p.file.stem().string()) == n)
            {
                return &p;
            }
        }
        return nullptr;
    }
    [[nodiscard]] const std::vector<Loaded>& list() const
    {
        return loaded_;
    }

private:
    AddonManager&       m_;
    Core                core_;
    LogMgr              log_;
    std::vector<Loaded> loaded_;
    uint32_t            nextId_ = 1;
};

bool HostImpl::load(const std::string& name, const std::string& args, std::string& error)
{
    if (find(name))
    {
        error = fmt::format("'{}' is already loaded.", name);
        return false;
    }
    std::filesystem::path file;
    std::error_code       ec;
    for (const auto& dir : pluginFolders(PluginBridge::root(m_)))
    {
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        {
            if (lower(e.path().filename().string()) == lower(name) + ".so")
            {
                file = e.path();
                break;
            }
        }
        if (!file.empty())
        {
            break;
        }
    }
    if (file.empty())
    {
        error = fmt::format(
            "no native build of plugin '{}' (looked for {}.so in the plugins folders; docs/ashita-compat.md "
            "\"Plugins\" says how to build one).",
            name, lower(name));
        return false;
    }
    void* handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        const char* why = dlerror();
        error           = fmt::format("could not load {}: {}", file.string(), why ? why : "?");
        return false;
    }
    // On Windows a .def file exports the plain names; built here they keep their C++ (mangled) names.
    auto sym = [&](const char* plain, const char* mangled) {
        void* p = dlsym(handle, plain);
        return p ? p : dlsym(handle, mangled);
    };
    auto version =
        reinterpret_cast<export_GetInterfaceVersion_f>(sym("expGetInterfaceVersion", "_Z22expGetInterfaceVersionv"));
    auto create  = reinterpret_cast<export_CreatePlugin_f>(sym("expCreatePlugin", "_Z15expCreatePluginPKc"));
    auto destroy = reinterpret_cast<export_DestroyPlugin_f>(sym("expDestroyPlugin", "_Z16expDestroyPluginPv"));
    if (!version || !create)
    {
        dlclose(handle);
        error = fmt::format("{} is not an Ashita plugin (no expCreatePlugin / expGetInterfaceVersion).", file.string());
        return false;
    }
    if (std::abs(version() - ASHITA_INTERFACE_VERSION) > 1e-6)
    {
        const double v = version();
        dlclose(handle);
        error = fmt::format("{} was built for interface {:.2f}; this client answers {:.2f}.", file.string(), v,
                            ASHITA_INTERFACE_VERSION);
        return false;
    }
    IPlugin* plugin = create(args.c_str());
    if (!plugin)
    {
        dlclose(handle);
        error = fmt::format("{}: expCreatePlugin returned nothing.", file.string());
        return false;
    }
    const uint32_t id = nextId_++;
    if (!plugin->Initialize(&core_, &log_, id))
    {
        if (destroy)
        {
            destroy(plugin);
        }
        dlclose(handle);
        error = fmt::format("plugin '{}' failed to initialize.", name);
        return false;
    }
    Loaded l;
    l.name    = plugin->GetName() ? plugin->GetName() : name;
    l.file    = file;
    l.handle  = handle;
    l.plugin  = plugin;
    l.destroy = destroy;
    l.flags   = plugin->GetFlags();
    spdlog::info("ashita: plugin {} {:.2f} by {} loaded from {}", l.name, plugin->GetVersion(),
                 plugin->GetAuthor() ? plugin->GetAuthor() : "?", file.string());
    loaded_.push_back(std::move(l));
    // Ashita runs plugins by priority (INFERRED: the lower value first; Shorthand asks for -5 to see commands early).
    std::ranges::stable_sort(
        loaded_, [](const Loaded& a, const Loaded& b) { return a.plugin->GetPriority() < b.plugin->GetPriority(); });
    return true;
}

bool HostImpl::unload(const std::string& name)
{
    const auto* p = find(name);
    if (!p)
    {
        return false;
    }
    const auto it = loaded_.begin() + (p - loaded_.data());
    Loaded     l  = std::move(*it);
    loaded_.erase(it);
    l.plugin->Release();
    if (l.destroy)
    {
        l.destroy(l.plugin);
    }
    else
    {
        delete l.plugin;
    }
    dlclose(l.handle);
    spdlog::info("ashita: plugin {} unloaded", l.name);
    return true;
}

bool HostImpl::command(std::string& line, int mode, bool injected)
{
    if (loaded_.empty())
    {
        return false;
    }
    // A buffer the plugin may edit in place (Shorthand strips "/raw " from it), as Ashita hands it the game's.
    std::vector<char> buf(line.begin(), line.end());
    buf.resize(std::max<std::size_t>(buf.size() + 1, 1024), '\0');
    const auto plugins = loaded_; // a plugin may load / unload others from its handler
    for (const auto& p : plugins)
    {
        if ((p.flags & static_cast<uint32_t>(Ashita::PluginFlags::UseCommands)) == 0)
        {
            continue;
        }
        if (p.plugin->HandleCommand(mode, buf.data(), injected))
        {
            line.assign(buf.data());
            return true;
        }
    }
    line.assign(buf.data());
    return false;
}

void HostImpl::packet(bool incoming, PacketEvent& e)
{
    if (loaded_.empty() || e.data.size() < 4)
    {
        return;
    }
    const std::vector<std::byte> original = e.data;
    for (const auto& p : loaded_)
    {
        if ((p.flags & static_cast<uint32_t>(Ashita::PluginFlags::UsePackets)) == 0)
        {
            continue;
        }
        const auto* data     = reinterpret_cast<const uint8_t*>(original.data());
        auto*       modified = reinterpret_cast<uint8_t*>(e.data.data());
        const auto  size     = static_cast<uint32_t>(e.data.size());
        const auto* chunk    = reinterpret_cast<const uint8_t*>(e.chunk.data());
        const auto  chunkLen = static_cast<uint32_t>(e.chunk.size());
        const bool  block =
            incoming
                ? p.plugin->HandleIncomingPacket(e.id, size, data, modified, chunkLen, chunk, e.injected, e.blocked)
                : p.plugin->HandleOutgoingPacket(e.id, size, data, modified, chunkLen, chunk, e.injected, e.blocked);
        e.blocked = e.blocked || block;
    }
}

void HostImpl::frame()
{
    // The wardrobes' "enabled" bits, which Shorthand reads from the game's memory through a signature (win32.cpp
    // answers it): bit (container - 9) for wardrobes 3-8 (containers 11-16) the character can use.
    const auto* w     = PluginBridge::frame(m_).world.get();
    uint8_t     flags = 0;
    for (uint32_t c = 11; w && w->containerSizeValid && c <= 16; ++c)
    {
        if (w->containerUsable[c] > 0)
        {
            flags = static_cast<uint8_t>(flags | (1u << (c - 9)));
        }
    }
    pluginSetBagFlags(flags);
}

// --- IPluginManager ---
bool Plugins::Load(const char* name, const char* args)
{
    std::string error;
    return name && h_.load(name, args ? args : "", error);
}
bool Plugins::Unload(const char* name)
{
    return name && h_.unload(name);
}
void Plugins::UnloadAll()
{
    h_.unloadAll();
}
// "addons" is always there: the Lua layer is the addons plugin.
bool Plugins::IsLoaded(const char* name)
{
    return name && (lower(name) == "addons" || h_.isLoaded(name));
}
void* Plugins::Get(const char* name)
{
    const auto* p = name ? h_.find(name) : nullptr;
    return p ? p->plugin : nullptr;
}
void* Plugins::Get(uint32_t index)
{
    return index < h_.list().size() ? h_.list()[index].plugin : nullptr;
}
uint32_t Plugins::Count()
{
    return static_cast<uint32_t>(h_.list().size());
}

} // namespace

std::unique_ptr<PluginHost> makePluginHost(AddonManager& manager)
{
    return std::make_unique<HostImpl>(manager);
}

const char* pluginSupportNote()
{
    return "";
}

} // namespace xi::ashita
