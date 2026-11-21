// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/session.h"

#include "client/battle_colours.h"

#include "client/combat.h"
#include "client/emotes.h"
#include "client/entity_names.h"
#include "client/loot_text.h"
#include "client/zone_banner.h"
#include "dat/zone.h"

#include "map/map_client.h"
#include "xi/bytes.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <sstream>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <tuple>

namespace xi::client
{

namespace
{

constexpr std::size_t kMaxChat = 500;

// FFXI chat text is Shift-JIS with auto-translate tokens; keep printable ASCII for now.
std::string printable(std::string s)
{
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const auto u = static_cast<unsigned char>(s[i]);
        if (u == 0xEF && i + 1 < s.size() && (s[i + 1] == '\x27' || s[i + 1] == '\x28'))
        {
            ++i; // the auto-translate brackets (gaiji EF 27 / EF 28, dat::AutoTranslate::expandMarked) stay
            continue;
        }
        if (u < 0x20 || u >= 0x7F)
        {
            s[i] = '?';
        }
    }
    return s;
}

} // namespace

Session::Session(Config cfg)
: cfg_(std::move(cfg))
{
    world_.status = "starting";
}

Session::~Session()
{
    stop_ = true;
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void Session::start()
{
    thread_ = std::thread([this] { run(); });
}

WorldSnapshot Session::snapshot() const
{
    std::lock_guard lock(mutex_);
    WorldSnapshot   s = world_;
    s.inventory.reserve(inventory_.size());
    for (const auto& [key, item] : inventory_)
    {
        s.inventory.push_back(item);
    }
    s.entities.reserve(entities_.size());
    for (const auto& [id, e] : entities_)
    {
        s.entities.push_back(e);
    }
    return s;
}

void Session::setPlayer(const glm::vec3& gamePos, uint8_t dir, uint16_t targetIndex)
{
    std::lock_guard lock(mutex_);
    playerPos_    = gamePos;
    playerDir_    = dir;
    playerTarget_ = targetIndex;
    playerDirty_  = true;
}

void Session::requestLogout(bool shutdown)
{
    std::vector<std::byte> p(4);
    store<uint16_t>(p, 0, 0);                  // Mode: toggle
    store<uint16_t>(p, 2, shutdown ? 3 : 1);   // Kind: logout / shutdown
    send(0x0E7, std::move(p));
    // The toggle: a second request (either kind) cancels the running countdown (LSB 0x0e7_reqlogout.cpp).
    logoutKind_ = logoutKind_.load() != 0 ? 0 : static_cast<uint16_t>(shutdown ? 3 : 1);
}

void Session::standUp()
{
    std::vector<std::byte> p(4);
    if (const uint16_t kind = logoutKind_.exchange(0); kind != 0)
    {
        store<uint16_t>(p, 0, 2); // Mode: off
        store<uint16_t>(p, 2, kind);
        send(0x0E7, std::move(p));
        note("Logout canceled."); // INFERRED: the game's own line for a cancelled countdown is not traced
        // Also rest off: if the server had already ended the countdown (damage cancels it) and the kneel is a /heal,
        // this stands us up; otherwise LSB just refuses it ("stop healing when not healing").
        p.assign(4, std::byte{ 0 });
    }
    store<uint32_t>(p, 0, 2); // CAMP Mode: off
    send(0x0E8, std::move(p));
}

void Session::say(const std::string& text, uint8_t kind)
{
    std::lock_guard lock(mutex_);
    outgoingChat_.emplace_back(cfg_.text ? cfg_.text->encodeAutoTranslate(text) : text, kind); // "{phrase}" -> FD code
}

double vanaHourOfDay()
{
    using namespace std::chrono;
    const double earthMs = static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    const double vanaSec = (earthMs / 1000.0 - 1009810800.0) * 25.0;
    return std::fmod(vanaSec / 3600.0, 24.0);
}

void Session::enterZoneLine(uint32_t rectId, uint8_t exitBit, uint8_t exitMode)
{
    std::lock_guard lock(mutex_);
    pendingZoneLine_ = rectId;
    pendingExitBit_  = exitBit;
    pendingExitMode_ = exitMode;
}

void Session::action(uint32_t targetId, uint16_t targetIndex, uint16_t actionId, uint32_t param)
{
    std::lock_guard lock(mutex_);
    if (actionId == 0x0E && fakeFishing(0x01A, {}))
    {
        return;
    }
    pendingActions_.push_back({ targetId, targetIndex, actionId, param, std::chrono::steady_clock::now() });
}

bool Session::fakeFishing(uint16_t id, std::span<const std::byte> payload)
{
    static const char* mode = std::getenv("XI_FAKE_FISH");
    if (!mode || lastOwnStatus_.size() <= 0x4A || (id != 0x01A && id != 0x110))
    {
        return false;
    }
    const std::string how  = mode;
    const auto        now  = std::chrono::steady_clock::now();
    auto status = [&](uint8_t animation, uint8_t hookDelay, std::chrono::milliseconds after, std::string note) {
        auto p = lastOwnStatus_;
        store<uint8_t>(p, 0x30, animation);
        store<uint8_t>(p, 0x4A, hookDelay);
        fakeFish_.push_back({ now + after, std::move(p), std::move(note) });
    };
    using std::chrono::milliseconds;
    if (id == 0x01A) // /fish
    {
        if (fakeFishWater_ && !fakeFishWater_())
        {
            fakeFish_.push_back({ now + milliseconds(300), {}, "You can't fish here." });
            return true;
        }
        status(56, 3, milliseconds(300), "(fake fishing) You cast your line.");
        return true;
    }
    if (payload.size() < 16)
    {
        return true;
    }
    const auto m    = load<uint8_t>(payload, 0x0A);
    const auto para = load<int32_t>(payload, 0x04);
    switch (m)
    {
        case 2: // RequestCheckHook
            if (how == "nobite")
            {
                status(62, 0, milliseconds(800), "You didn't catch anything.");
                break;
            }
            {
                // S2C 0x115 FISH: a small fish, moderately tired out (LSB-like values).
                std::vector<std::byte> f(0x18);
                store<uint16_t>(f, 0, static_cast<uint16_t>(0x115 | (0x18 / 4) << 9));
                store<uint16_t>(f, 0x04, how == "monster" ? 2400 : 1500); // stamina
                store<uint16_t>(f, 0x06, 8);                            // arrow_delay
                store<uint16_t>(f, 0x08, 126);                          // regen (below 128: it tires)
                store<uint16_t>(f, 0x0A, 9);                            // move_frequency
                store<uint16_t>(f, 0x0C, 100);                          // arrow_damage
                store<uint16_t>(f, 0x0E, 50);                           // arrow_regen
                store<uint16_t>(f, 0x10, 60);                           // time
                store<uint8_t>(f, 0x12, how == "monster" ? 1 : 0);      // angler_sense: bit 0 large
                store<uint32_t>(f, 0x14, 40);                           // intuition (special)
                status(57, 0, milliseconds(800), how == "monster" ? "Something clamps onto your line ferociously!" : "Something caught the hook!");
                fakeFish_.push_back({ now + milliseconds(820), std::move(f), "" });
            }
            break;
        case 3: // RequestEndMiniGame: para = the fish's stamina left in %, or 200 give up / 201 cancel / 300 out of time
            if (para <= 4 && how == "rodbreak")
            {
                status(59, 0, milliseconds(300), "Your rod breaks.");
            }
            else if (para <= 4)
            {
                status(how == "monster" ? 61 : 58, 0, milliseconds(300),
                       how == "monster" ? "(fake fishing) A monster appears!" : world_.charName + " caught a moat carp!");
            }
            else if (para <= 0x14)
            {
                status(60, 0, milliseconds(300), "You lost your catch due to your lack of skill.");
            }
            else if (para <= 0x64)
            {
                status(60, 0, milliseconds(300), "Your line breaks.");
            }
            else if (para <= 0x100)
            {
                status(62, 0, milliseconds(300), "You give up.");
            }
            else
            {
                status(62, 0, milliseconds(300), "You lost your catch.");
            }
            break;
        case 5: // RequestPotentialTimeout
            fakeFish_.push_back({ now, {}, "You don't know how much longer you can keep this one on the line..." });
            break;
        case 4: // RequestRelease
            status(0, 0, milliseconds(200), "");
            break;
        default: break;
    }
    return true;
}

void Session::useItem(uint8_t container, uint8_t slot, uint32_t targetId, uint16_t targetIndex)
{
    std::lock_guard lock(mutex_);
    pendingItemUses_.push_back({ container, slot, targetId, targetIndex });
}

void Session::equip(uint8_t container, uint8_t slot, uint8_t equipSlot)
{
    std::lock_guard lock(mutex_);
    pendingEquips_.push_back({ slot, equipSlot, container });
}

void Session::note(std::string text)
{
    std::lock_guard lock(mutex_);
    world_.chat.push_back({ 0xFF, "", std::move(text) });
}

const dat::DialogTable& Session::stdText()
{
    if (!stdText_)
    {
        try
        {
            stdText_ = cfg_.index ? dat::DialogTable::parse(cfg_.index->read(7031)) : dat::DialogTable{};
        }
        catch (const std::exception& e)
        {
            spdlog::warn("standard messages (7031): {}", e.what());
            stdText_ = dat::DialogTable{};
        }
    }
    return *stdText_;
}

void Session::stdNote(uint16_t message, int32_t para0, int32_t para1)
{
    std::lock_guard lock(mutex_);
    stdNoteLocked(message, para0, para1, {});
}

void Session::stdNote(uint16_t message, const std::string& string0)
{
    std::lock_guard lock(mutex_);
    stdNoteLocked(message, 0, 0, string0);
}

const Session::AreaTexts& Session::texts()
{
    if (!areaTexts_)
    {
        areaTexts_.emplace();
        if (cfg_.index)
        {
            auto load = [&](auto& slot, uint32_t file, auto parse) {
                try
                {
                    if (cfg_.index->path(file))
                    {
                        slot = parse(cfg_.index->read(file));
                    }
                }
                catch (const std::exception& e)
                {
                    spdlog::warn("client texts: file {}: {}", file, e.what());
                }
            };
            auto table = [](auto f) { return dat::StringTable::parse(f); };
            load(areaTexts_->formats, 50, [](auto f) { return dat::XiStringTable::parse(f); });
            load(areaTexts_->areas, 55465, table);
            load(areaTexts_->clientText, 55648, table);
            load(areaTexts_->sysText, 55646, table);
            load(areaTexts_->weekdays, 55658, table);
        }
    }
    return *areaTexts_;
}

void Session::logZoneIn(uint16_t zone, uint32_t loginState, uint16_t mapNumber, uint8_t mhFloor, uint16_t mhMap)
{
    const auto& t = texts();
    const ZoneInInfo z{ zone, loginState, mapNumber, mhFloor, mhMap };
    for (auto& line : zoneInLines(z, t.formats ? &*t.formats : nullptr, t.areas ? &*t.areas : nullptr, t.clientText ? &*t.clientText : nullptr))
    {
        world_.chat.push_back({ 0xFF, "", std::move(line) });
        world_.chat.back().colour = kAreaLineColour;
        if (world_.chat.size() > kMaxChat)
        {
            world_.chat.erase(world_.chat.begin());
        }
    }
}

void Session::stdNoteLocked(uint16_t message, int32_t para0, int32_t para1, const std::string& string0)
{
    const auto& t = stdText();
    if (message >= t.size())
    {
        return;
    }
    dat::DialogArgs args;
    if (cfg_.text)
    {
        args.lookup = [t = cfg_.text](uint16_t table, uint32_t value) { return t->dialogName(table, value); };
    }
    args.numbers[0] = para0;
    args.numbers[1] = para1;
    args.strings[0] = string0;
    for (auto& l : dat::formatDialog(t.raw(message), args))
    {
        if (!l.empty() && l != "\x0B")
        {
            world_.chat.push_back({ 0xFF, "", std::move(l) });
        }
    }
}

void Session::guildClosedLines(const GuildOpen& g)
{
    const auto& t = texts();
    world_.guild.passive    = g.passive();
    world_.guild.closeLines = guildClosedText(g, t.formats ? &*t.formats : nullptr, t.weekdays ? &*t.weekdays : nullptr);
    ++world_.guild.closeSerial;
}

void Session::guildResultLines(bool sell, std::span<const std::byte> d)
{
    if (d.size() < 8)
    {
        return;
    }
    const auto r = guildResultText(sell, load<uint16_t>(d, 4), static_cast<int8_t>(load<uint8_t>(d, 7)));
    auto       sys = [&](int id) {
        const auto& t = texts();
        if (id >= 0 && t.sysText && static_cast<std::size_t>(id) < t.sysText->size())
        {
            world_.chat.push_back({ 0xFF, "", std::string(t.sysText->get(static_cast<std::size_t>(id))) });
        }
    };
    sys(r.sys1);
    if (r.itemMsg >= 0)
    {
        stdNoteLocked(static_cast<uint16_t>(r.itemMsg), r.item, r.count, {});
    }
    sys(r.sys2);
    if (world_.chat.size() > kMaxChat)
    {
        world_.chat.erase(world_.chat.begin(), world_.chat.begin() + static_cast<std::ptrdiff_t>(world_.chat.size() - kMaxChat));
    }
}

void Session::finishEvent(uint32_t endPara)
{
    std::lock_guard lock(mutex_);
    const auto&     e = world_.event;
    pendingEvents_.push_back({ e.uniqueNo, e.actIndex, e.eventNum, e.eventPara, endPara, 0, std::nullopt, 0 });
}

void Session::updateEvent(uint32_t endPara)
{
    std::lock_guard lock(mutex_);
    const auto&     e = world_.event;
    pendingEvents_.push_back({ e.uniqueNo, e.actIndex, e.eventNum, e.eventPara, endPara, 1, std::nullopt, 0 });
}

void Session::updateEventPosition(uint32_t endPara, const glm::vec3& pos, uint8_t dir)
{
    std::lock_guard lock(mutex_);
    const auto&     e = world_.event;
    pendingEvents_.push_back({ e.uniqueNo, e.actIndex, e.eventNum, e.eventPara, endPara, 1, pos, dir });
}

std::vector<std::string> Session::zoneDialog(uint16_t message, const std::array<int32_t, 8>& numbers)
{
    std::lock_guard lock(mutex_);
    if (!zoneText_ || message >= zoneText_->size())
    {
        return {};
    }
    dat::DialogArgs args;
    args.numbers = numbers;
    // The event's strings (S2C 0x033 / 0x05D) are the message strings 0..3 (both handlers copy them to 0x10488d08,
    // 256 bytes apart, where 0x02A's string goes too).
    std::copy(world_.event.strings.begin(), world_.event.strings.end(), args.strings.begin());
    args.player     = world_.charName;
    args.eventNames = world_.eventNames;
    if (cfg_.text)
    {
        args.lookup = [t = cfg_.text](uint16_t table, uint32_t value) { return t->dialogName(table, value); };
    }
    return dat::formatDialog(zoneText_->raw(message), args);
}

void Session::setEventName(std::size_t slot, std::string name, uint32_t value)
{
    std::lock_guard lock(mutex_);
    if (slot < world_.eventNames.size())
    {
        world_.eventNames[slot] = { std::move(name), value };
    }
}

void Session::eventSay(const std::string& from, uint16_t message, const std::array<int32_t, 8>& numbers)
{
    const auto      lines = zoneDialog(message, numbers);
    std::lock_guard lock(mutex_);
    for (const auto& l : lines)
    {
        if (!l.empty() && l != "\x0B")
        {
            world_.chat.push_back({ kNpcChat, from, l });
        }
    }
}

void Session::send(uint16_t id, std::vector<std::byte> payload)
{
    std::lock_guard lock(mutex_);
    if (fakeFishing(id, payload))
    {
        return;
    }
    pendingRaw_.emplace_back(id, std::move(payload));
}

void Session::sendInjected(uint16_t id, std::vector<std::byte> payload)
{
    std::lock_guard lock(mutex_);
    pendingInjectedOut_.emplace_back(id, std::move(payload));
}

void Session::injectIncoming(std::vector<std::byte> packet)
{
    std::lock_guard lock(mutex_);
    pendingInjectedIn_.push_back(std::move(packet));
}

namespace
{
// C2S 0x04B FRAGMENTS {Command 1 first / 2 more, Result, value1 1 = server message, value2 2 = English, i32 timestamp,
// i32 size_total, i32 offset, i32 data_size}.
std::vector<std::byte> servmesRequest(uint8_t command, int32_t timestamp, int32_t offset)
{
    std::vector<std::byte> p(0x14);
    store<uint8_t>(p, 0, command);
    store<uint8_t>(p, 2, 1);
    store<uint8_t>(p, 3, 2);
    store<int32_t>(p, 4, timestamp);
    store<int32_t>(p, 0x0C, offset);
    return p;
}
} // namespace

void Session::requestServerMessage()
{
    std::lock_guard lock(mutex_);
    servmes_.clear();
    pendingRaw_.emplace_back(0x04B, servmesRequest(1, 0, 0));
}

void Session::inviteToParty(uint32_t id, uint16_t index)
{
    std::vector<std::byte> p(8);
    store<uint32_t>(p, 0, id);
    store<uint16_t>(p, 4, index);
    store<uint8_t>(p, 6, 0); // PartyKind::Party
    send(0x06E, std::move(p));
}

void Session::answerPartyInvite(bool accept)
{
    std::vector<std::byte> p(4);
    store<uint8_t>(p, 0, accept ? 1 : 0);
    {
        std::lock_guard lock(mutex_);
        world_.partyInvite.reset();
    }
    send(0x074, std::move(p));
}

void Session::run()
{
    auto setStatus = [&](const std::string& s) {
        std::lock_guard lock(mutex_);
        world_.status = s;
        spdlog::info("session: {}", s);
    };

    try
    {
        const auto huffman = Huffman::fromCompressDat(cfg_.compressDat);
        setStatus("logging in");
        const auto auth = lobby::authenticate(cfg_.lobby);
        setStatus("selecting character");
        const auto handoff = lobby::enterWorld(cfg_.lobby, auth, [&](const std::vector<lobby::CharacterSlot>& slots) -> std::optional<std::size_t> {
            if (cfg_.chooseInUi && cfg_.character.empty())
            {
                // Hand the list to the interface and wait for its choice (or a quit).
                std::unique_lock lock(mutex_);
                world_.lobbySlots        = slots;
                world_.choosingCharacter = true;
                world_.status            = "select a character";
                choiceCv_.wait(lock, [&] { return choice_.has_value() || stop_.load(); });
                world_.choosingCharacter = false;
                if (!choice_ || *choice_ >= slots.size() || !slots[*choice_].occupied)
                {
                    return std::nullopt;
                }
                return *choice_;
            }
            for (std::size_t i = 0; i < slots.size(); ++i)
            {
                if (slots[i].occupied && (cfg_.character.empty() || slots[i].name == cfg_.character))
                {
                    return i;
                }
            }
            return std::nullopt;
        });
        {
            std::lock_guard lock(mutex_);
            world_.charId   = handoff.charId;
            world_.charName = handoff.charName;
        }

        // Called with mutex_ held (from the packet callback).
        std::unordered_map<uint32_t, std::string> zoneNames; // NPC names from the zone's name DAT
        auto nameOf = [&](uint32_t id) -> std::string {
            if (id == world_.charId)
            {
                return world_.charName;
            }
            if (const auto it = entities_.find(id); it != entities_.end() && !it->second.name.empty())
            {
                return it->second.name;
            }
            if (const auto it = zoneNames.find(id); it != zoneNames.end() && !it->second.empty())
            {
                return it->second;
            }
            return fmt::format("#{:X}", id);
        };
        uint64_t holdTag    = 0; // ChatLine::hold for the result being logged
        auto pushCombat = [&](std::string text, uint8_t kind = kCombatChat, uint8_t colour = 0, uint16_t message = 0) {
            spdlog::debug("combat: {}", text);
            world_.chat.push_back({ kind, "", std::move(text) });
            world_.chat.back().hold    = holdTag;
            world_.chat.back().colour  = colour;
            world_.chat.back().message = message;
            if (world_.chat.size() > kMaxChat)
            {
                world_.chat.erase(world_.chat.begin());
            }
        };

        // The treasure pool's lines (client/loot_text.h; standard messages, file 7031), mutex_ held.
        auto lootNote = [&](const std::optional<LootNote>& n) {
            if (!n)
            {
                return;
            }
            std::function<std::string(uint16_t, uint32_t)> lookup;
            if (cfg_.text)
            {
                lookup = [t = cfg_.text](uint16_t table, uint32_t value) { return t->dialogName(table, value); };
            }
            for (auto& l : lootLines(stdText(), *n, lookup))
            {
                spdlog::debug("loot: {}", l);
                world_.chat.push_back({ 0xFF, "", std::move(l) });
            }
        };

        // A battle message from the game's text tables when loaded, else the built-in subset (mutex_ held).
        // `param` / `value` feed the built-in fallback (damage or Data, and Data2).
        auto combatMessage = [&](uint16_t msg, uint32_t actor, uint32_t target, const std::array<uint32_t, 4>& nums, uint32_t param,
                                 uint32_t value) {
            const auto actorName  = nameOf(actor);
            const auto targetName = nameOf(target);
            auto       isPc       = [&](uint32_t id) {
                if (id == world_.charId)
                {
                    return true;
                }
                const auto it = entities_.find(id);
                return it != entities_.end() && it->second.isPc;
            };
            // Log colour by who is involved, as the game's log does: our side acting, our side being hit, others.
            auto ours = [&](uint32_t id) {
                return id == world_.charId || std::any_of(world_.party.begin(), world_.party.end(), [&](const PartyMember& m) { return m.id == id; });
            };
            const uint8_t kind   = ours(actor) ? kCombatOwn : ours(target) ? kCombatHurt : kCombatChat;
            const uint8_t colour = battleColour(msg, target == world_.charId, actor == world_.charId);
            if (cfg_.text && cfg_.text->has(msg))
            {
                for (auto& line : cfg_.text->format(msg, { actorName, targetName, isPc(actor), isPc(target), nums }))
                {
                    if (!line.empty())
                    {
                        static const bool trace = std::getenv("XI_BATTLE_TRACE") != nullptr; // log every battle line
                        if (trace)
                        {
                            spdlog::info("battle: [{}] {}", msg, line);
                        }
                        pushCombat(std::move(line), kind, colour, msg);
                    }
                }
            }
            else if (auto text = formatBasicMessage(msg, actorName, targetName, param, value))
            {
                pushCombat(std::move(*text), kind, colour, msg);
            }
        };

        // The current zone's NPC text (file 6420 + zone), loaded at zone-in.
        auto npcSays = [&](uint32_t speaker, uint16_t mesNum, const std::array<int32_t, 8>& nums,
                           const std::array<std::string, 2>& strs = {}) {
            const bool    showName = !(mesNum & 0x8000);
            const uint16_t id      = mesNum & 0x7FFF;
            if (!zoneText_ || id >= zoneText_->size())
            {
                pushCombat(fmt::format("[zone text {}]", id));
                return;
            }
            const std::string from = showName ? nameOf(speaker) : std::string();
            dat::DialogArgs args;
            args.numbers = nums;
            args.strings[0] = strs[0];
            args.strings[1] = strs[1];
            args.player     = world_.charName;
            args.eventNames = world_.eventNames;
            if (cfg_.text)
            {
                args.lookup = [t = cfg_.text](uint16_t table, uint32_t value) { return t->dialogName(table, value); };
            }
            for (auto& l : dat::formatDialog(zoneText_->raw(id), args))
            {
                if (!l.empty() && l != "\x0B")
                {
                    world_.chat.push_back({ kNpcChat, from, std::move(l) });
                }
            }
            if (world_.chat.size() > kMaxChat)
            {
                world_.chat.erase(world_.chat.begin(), world_.chat.begin() + static_cast<std::ptrdiff_t>(world_.chat.size() - kMaxChat));
            }
        };

        map::MapClient map(handoff, huffman);
        if (cfg_.packetFilter)
        {
            map.setFilter(cfg_.packetFilter);
        }
        auto           netAt   = std::chrono::steady_clock::now();
        uint64_t       netSent = 0, netRecv = 0;
        const char*    traceEnv = std::getenv("XI_NET_TRACE");
        const bool     netTrace = traceEnv && *traceEnv && *traceEnv != '0';
        map.onPacket([&](const map::SubPacket& sp) {
            std::lock_guard lock(mutex_);
            const auto&     d = sp.data;
            switch (sp.id)
            {
                case 0x00A: // zone-in
                {
                    entities_.clear();
                    renames_.clear();
                    if (cfg_.entityRenames) // ask the server for its rename list (client/entity_names.h)
                    {
                        pendingRaw_.emplace_back(uint16_t{ 0x001 }, std::vector<std::byte>(kRenameRequestPayload));
                    }
                    logoutKind_ = 0; // a countdown does not survive zoning
                    world_.inZone    = true;
                    if (!servmesAsked_) // the game shows the server message once, at the first zone-in
                    {
                        servmesAsked_ = true;
                        servmes_.clear();
                        pendingRaw_.emplace_back(0x04B, servmesRequest(1, 0, 0));
                    }
                    world_.zone      = map.zone();
                    if (d.size() > 0x1C && load<uint8_t>(d, 0x1C) != 0)
                    {
                        world_.speed = load<uint8_t>(d, 0x1C); // GP_SERV_POS_HEAD Speed
                    }
                    if (d.size() >= 0xB0)
                    {
                        const uint32_t loginState = load<uint32_t>(d, 0x80);
                        uint16_t       mapNumber  = load<uint16_t>(d, 0x42);
                        logZoneIn(world_.zone, loginState, mapNumber, static_cast<uint8_t>((load<uint8_t>(d, 0xA8) >> 1) & 3), load<uint16_t>(d, 0xAA));
                        mapNumber                 = mapNumber >= 0x100 ? static_cast<uint16_t>(mapNumber + 0x1BC) : mapNumber;
                        world_.inMogHouse         = loginState == 1 || loginState == 5;
                        world_.mhMap              = load<uint16_t>(d, 0xAA);
                        world_.mhFloor            = static_cast<uint8_t>((load<uint8_t>(d, 0xA8) >> 1) & 3);
                        world_.mhExitBit          = load<uint8_t>(d, 0xAE);
                        world_.zoneModel          = world_.inMogHouse ? world_.mhMap : mapNumber;
                    }
                    else
                    {
                        world_.inMogHouse = false;
                        world_.zoneModel  = world_.zone;
                        logZoneIn(world_.zone, 2, world_.zone, 0, 0);
                    }
                    world_.serverPos = { map.position().x, map.position().y, map.position().z };
                    world_.serverDir = map.position().dir;
                    if (d.size() >= 0xA4)
                    {
                        world_.playTime   = load<uint32_t>(d, 0xA0);
                        world_.playTimeAt = std::chrono::steady_clock::now();
                    }
                    if (d.size() >= 0x40)
                    {
                        world_.gameTime   = load<uint32_t>(d, 0x3C);
                        world_.gameTimeAt = std::chrono::steady_clock::now();
                    }
                    playerPos_       = world_.serverPos;
                    playerDir_       = world_.serverDir;
                    ++world_.teleportGeneration;
                    world_.status = fmt::format("in zone {}", world_.zone);
                    world_.weather = load<uint16_t>(d, 0x68);
                    world_.actIndex = load<uint16_t>(d, 0x08);
                    for (std::size_t m = 0; m < world_.music.size(); ++m)
                    {
                        world_.music[m] = load<uint16_t>(d, 0x56 + 2 * m); // LSB MusicNum[5]
                    }
                    if (cfg_.index && zoneTextZone_ != world_.zone)
                    {
                        zoneTextZone_ = world_.zone;
                        zoneText_.reset();
                        try
                        {
                            zoneNames = dat::loadEntityNames(*cfg_.index, world_.zone);
                        }
                        catch (const std::exception&)
                        {
                            zoneNames.clear();
                        }
                        if (const auto f = dat::zoneDialogFile(world_.zone); f && cfg_.index->path(*f))
                        {
                            try
                            {
                                zoneText_ = dat::DialogTable::parse(cfg_.index->read(*f));
                            }
                            catch (const std::exception& e)
                            {
                                spdlog::warn("zone {} dialog: {}", world_.zone, e.what());
                            }
                        }
                    }
                    world_.look.kind  = 0xFFFF;
                    world_.look.valid = true;
                    for (std::size_t i = 0; i < 9; ++i)
                    {
                        world_.look.ids[i] = load<uint16_t>(d, 0x44 + 2 * i);
                    }
                    // A zone-in cutscene arrives inside 0x00A: EventNum @+0x62, EventPara @+0x64 (non-zero = event
                    // started; the server waits for our EVENTEND).
                    if (d.size() >= 0x68 && (load<uint16_t>(d, 0x62) != 0 || load<uint16_t>(d, 0x64) != 0))
                    {
                        world_.event = { world_.charId, map.actIndex(), load<uint16_t>(d, 0x62), load<uint16_t>(d, 0x64), {} };
                        ++world_.eventSerial;
                    }
                    break;
                }
                case 0x01B: // job info: GP_MYROOM_DANCER get_job_flag @+0x0C, job_lev2[24] @+0x48 (LSB 0x01b_job_info.h)
                    if (d.size() >= 0x60)
                    {
                        world_.jobsUnlocked = load<uint32_t>(d, 0x0C);
                        for (std::size_t j = 0; j < world_.jobLevels.size(); ++j)
                        {
                            world_.jobLevels[j] = load<uint8_t>(d, 0x48 + j);
                        }
                        if (d.size() >= 0x64)
                        {
                            world_.encumbrance = load<uint32_t>(d, 0x60);
                        }
                    }
                    break;
                case 0x02E: // the Moogle's menu (LSB sendMenu(MOOGLE); CODE 0x100a1520): no body
                    ++world_.mogMenuSerial;
                    break;
                case 0x00B: // leaving the zone (zone change or logout)
                    world_.inZone = false;
                    world_.status = "zoning";
                    entities_.clear();
                    break;
                case 0x063: // MISCDATA; type 9 = status icons: type u16 @4, icons u16[32] @8 (0xFF = none),
                            // expiry u32[32] @0x48 = (seconds since the Vana'diel epoch) * 60, wrapping; 0x7FFFFFFF = no timer
                    if (load<uint16_t>(d, 4) == 9 && d.size() >= 0xC8)
                    {
                        const auto nowSys = std::chrono::system_clock::now();
                        const auto vana   = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(nowSys.time_since_epoch()).count() - 1009810800);
                        world_.buffs.clear();
                        for (std::size_t i = 0; i < 32; ++i)
                        {
                            const auto icon = load<uint16_t>(d, 8 + 2 * i);
                            if (icon == 0xFF)
                            {
                                continue;
                            }
                            WorldSnapshot::Buff b;
                            b.icon        = icon;
                            const auto ts = load<uint32_t>(d, 0x48 + 4 * i);
                            b.timer       = ts;
                            if (ts != 0x7FFFFFFF)
                            {
                                const uint32_t left = (ts - vana * 60u) / 60u;
                                b.expires           = std::chrono::steady_clock::now() + std::chrono::seconds(left);
                            }
                            world_.buffs.push_back(b);
                        }
                    }
                    break;
                case 0x025: // ITEM_TRADE_MYLIST: ItemNum @4, ItemNo @8, TradeIndex @0xA, ItemIndex @0xB (our box as accepted;
                            // a refused item comes back as ItemNo 0xFFFF / ItemNum 0)
                    if (d.size() >= 0x0C)
                    {
                        const auto box = load<uint8_t>(d, 0x0A);
                        if (box < world_.trade.mine.size())
                        {
                            const auto n  = load<uint32_t>(d, 4);
                            const bool ok = n > 0 && (load<uint16_t>(d, 8) != 0xFFFF || box == 0);
                            world_.trade.mine[box] = ok ? std::pair<uint8_t, uint32_t>{ load<uint8_t>(d, 0x0B), n } : std::pair<uint8_t, uint32_t>{ 0, 0 };
                            ++world_.trade.mineUpdates;
                        }
                    }
                    break;
                case 0x05A: // MOTIONMES (emote): caster @4, target @8, emote @0x10, mode @0x16 (1 = text only, 2 = motion only)
                    if (d.size() >= 0x18 && load<uint8_t>(d, 0x16) != 1)
                    {
                        // The motion (LSB EmoteMode: 0 all, 1 text, 2 motion).
                        const uint32_t caster = load<uint32_t>(d, 4);
                        const auto     emote  = static_cast<uint8_t>(load<uint16_t>(d, 0x10));
                        if (caster == world_.charId)
                        {
                            world_.emote = emote;
                            ++world_.emotes;
                        }
                        else if (const auto e = entities_.find(caster); e != entities_.end())
                        {
                            e->second.emote = emote;
                            ++e->second.emotes;
                        }
                    }
                    if (d.size() >= 0x18 && load<uint8_t>(d, 0x16) != 2)
                    {
                        const uint32_t caster = load<uint32_t>(d, 4), tgt = load<uint32_t>(d, 8);
                        const auto     emote  = static_cast<uint8_t>(load<uint16_t>(d, 0x10));
                        if (!emoteText_ && cfg_.index)
                        {
                            try
                            {
                                emoteText_ = dat::DialogTable::parse(cfg_.index->read(7025));
                            }
                            catch (const std::exception&)
                            {
                                emoteText_ = dat::DialogTable{};
                            }
                        }
                        const bool targeted = tgt != 0 && tgt != caster;
                        const std::size_t entry = 2u * emote + (targeted ? 0u : 1u);
                        if (emoteText_ && entry < emoteText_->size())
                        {
                            // The game's own line: actor / target names, [his/her] by the actor's race, [the /] by
                            // whether the target is a player.
                            dat::DialogArgs a;
                            a.actor  = nameOf(caster);
                            a.target = targeted ? nameOf(tgt) : std::string();
                            const auto look = caster == world_.charId ? world_.look : entities_.count(caster) ? entities_[caster].look : client::Look{};
                            const auto race = static_cast<uint8_t>(look.ids[0] >> 8);
                            a.female          = look.valid && (look.kind == 0xFFFF || look.kind == 1) && (race == 2 || race == 4 || race == 6 || race == 7);
                            // "the" only for monsters ("the Forest Hare"); players and named NPCs take none.
                            a.targetNoArticle = !(entities_.count(tgt) && entities_[tgt].isMob);
                            std::string text;
                            for (const auto& l : dat::formatDialog(emoteText_->raw(entry), a))
                            {
                                text += (text.empty() ? "" : " ") + l;
                            }
                            world_.chat.push_back({ 8, "", text });
                        }
                        else
                        {
                            world_.chat.push_back({ 8, "", emoteText(emote, nameOf(caster), nameOf(tgt)) });
                        }
                    }
                    break;
                case 0x021: // ITEM_TRADE_REQ: UniqueNo @4, ActIndex @8 (someone wants to trade)
                    // CODE 0x10152080: unless a trade is on, keep the asker and log standard message 24 "<name> wishes
                    // to trade with you." (the server sends no text of its own).
                    if (!world_.trade.active)
                    {
                        world_.trade.requestFrom  = load<uint32_t>(d, 4);
                        world_.trade.requestIndex = load<uint16_t>(d, 8);
                        const auto e = entities_.find(world_.trade.requestFrom);
                        stdNoteLocked(24, 0, 0, e != entities_.end() ? e->second.name : std::string());
                    }
                    break;
                case 0x022: // ITEM_TRADE_RES: UniqueNo @4, Kind u32 @8 (0 start, 1 cancel, 2 confirm, 3 unconfirm, 4-8 errors, 9 done)
                {
                    // CODE 0x10152520: Kind 0 opens the trade with UniqueNo (0x101520f0); the rest go to 0x10152170 by
                    // Kind - 1: 1 (the partner canceled), 4, 5, 7, 8 (errors) log standard message 26 "You cannot trade
                    // with <name> at this time." when they come from the partner or the asker; 2 marks the partner's
                    // confirm (their name turns orange); 3 changes nothing; 6 just closes; 9 logs 30 "Trade complete.".
                    const auto kind = load<uint32_t>(d, 8);
                    const auto from = load<uint32_t>(d, 4);
                    auto&      t    = world_.trade;
                    auto       tradeName = [&](uint32_t id) {
                        const auto e = entities_.find(id);
                        return e != entities_.end() ? e->second.name : (id == t.partner ? t.partnerName : std::string());
                    };
                    auto reset = [&] {
                        const auto serial = t.serial;
                        t                 = {};
                        t.serial          = serial;
                    };
                    if (kind == 0)
                    {
                        reset();
                        t.active       = true;
                        t.serial      += 1;
                        t.partner      = from;
                        t.partnerIndex = load<uint16_t>(d, 0x0C);
                        t.partnerName  = tradeName(from);
                    }
                    else if (kind == 2)
                    {
                        t.partnerConfirmed = from != world_.charId;
                    }
                    else if (kind == 3)
                    {
                    }
                    else if (kind == 9)
                    {
                        stdNoteLocked(30, 0, 0, {});
                        reset();
                    }
                    else if (kind == 6)
                    {
                        reset();
                    }
                    else
                    {
                        if (from != 0 && ((t.active && from == t.partner) || (!t.active && from == t.requestFrom)))
                        {
                            stdNoteLocked(26, 0, 0, tradeName(from));
                        }
                        reset();
                    }
                    break;
                }
                case 0x023: // ITEM_TRADE_LIST: ItemNum @4, TradeCounter @8, ItemNo @0xA, TradeIndex @0xD (the partner's offer)
                    if (d.size() >= 0x0E)
                    {
                        const auto box = load<uint8_t>(d, 0x0D);
                        if (box < world_.trade.theirs.size())
                        {
                            world_.trade.theirs[box] = { load<uint16_t>(d, 0x0A), load<uint32_t>(d, 4) };
                        }
                        world_.trade.counter          = load<uint16_t>(d, 8);
                        world_.trade.partnerConfirmed = false;
                        ++world_.trade.theirsUpdates;
                    }
                    break;
                case 0x0DC: // GROUP_SOLICIT_REQ (invite): UniqueNo @4, ActIndex @8, Kind @0xB, sName[16] @0xC
                    if (d.size() >= 0x0C)
                    {
                        const auto* nm = reinterpret_cast<const char*>(d.data() + 0x0C);
                        world_.partyInvite = WorldSnapshot::Invite{ load<uint32_t>(d, 4), std::string(nm, strnlen(nm, std::min<std::size_t>(16, d.size() - 0x0C))),
                                                                    load<uint8_t>(d, 0x0B) };
                        world_.chat.push_back({ 0xFF, "", fmt::format("{} invites you to {}.", world_.partyInvite->name,
                                                                       world_.partyInvite->kind == 5 ? "an alliance" : "a party") });
                    }
                    break;
                case 0x0A0: // MAP_GROUP: UniqueID @4, zone i16 @8, x / y / z f32 @0xC / 0x10 / 0x14 (answers C2S 0x0D2)
                    if (d.size() >= 0x18)
                    {
                        const auto id = load<uint32_t>(d, 4);
                        for (auto& m : world_.party)
                        {
                            if (m.id == id)
                            {
                                m.mapPosKnown = true;
                                m.mapZone     = load<uint16_t>(d, 8);
                                m.mapPos      = { load<float>(d, 0x0C), load<float>(d, 0x10), load<float>(d, 0x14) };
                                spdlog::info("map: {} is at ({:.1f}, {:.1f}) in zone {}", m.name, m.mapPos.x, m.mapPos.z, m.mapZone);
                            }
                        }
                    }
                    break;
                case 0x0C8: // GROUP_TBL: Kind @4, 20 entries @8 of {UniqueNo u32, ActIndex u16, flags u8, pad, ZoneNo u16, pad}
                {
                    std::vector<PartyMember> next;
                    for (std::size_t i = 0; i < 20 && 8 + 12 * (i + 1) <= d.size(); ++i)
                    {
                        const std::size_t o  = 8 + 12 * i;
                        const auto        id = load<uint32_t>(d, o);
                        if (id == 0)
                        {
                            continue;
                        }
                        PartyMember m;
                        for (const auto& old : world_.party)
                        {
                            if (old.id == id)
                            {
                                m = old;
                            }
                        }
                        const auto flags = load<uint8_t>(d, o + 6);
                        m.id      = id;
                        m.index   = load<uint16_t>(d, o + 4);
                        m.partyNo = flags & 3;
                        m.leader  = (flags & 4) != 0;
                        m.flags   = flags;
                        m.zone    = load<uint16_t>(d, o + 8);
                        next.push_back(std::move(m));
                    }
                    // A table holding only ourselves means we are solo.
                    if (next.size() == 1 && next[0].id == world_.charId)
                    {
                        next.clear();
                    }
                    world_.party = std::move(next);
                    break;
                }
                case 0x0DD: // GROUP_LIST: UniqueNo @4, Hp/Mp/Tp @8/0xC/0x10, GAttr @0x14, ActIndex @0x18, MemberNumber @0x1A,
                            // Hpp/Mpp @0x1D/0x1E, ZoneNo @0x20, jobs @0x22..0x25, Name @0x28 (LSB trims the packet to the
                            // name's length rounded up to 4, plus 4)
                    if (d.size() >= 0x28)
                    {
                        const auto id = load<uint32_t>(d, 4);
                        auto       it = std::find_if(world_.party.begin(), world_.party.end(), [&](const PartyMember& m) { return m.id == id; });
                        if (it == world_.party.end())
                        {
                            world_.party.push_back({});
                            it = world_.party.end() - 1;
                        }
                        auto& m     = *it;
                        m.id        = id;
                        m.hp        = load<uint32_t>(d, 0x08);
                        m.mp        = load<uint32_t>(d, 0x0C);
                        m.tp        = load<uint32_t>(d, 0x10);
                        m.partyNo   = load<uint8_t>(d, 0x14) & 3;
                        m.leader    = (load<uint8_t>(d, 0x14) & 4) != 0;
                        m.flags     = load<uint8_t>(d, 0x14);
                        m.index     = load<uint16_t>(d, 0x18);
                        m.order     = load<uint8_t>(d, 0x1A);
                        m.hpp       = load<uint8_t>(d, 0x1D);
                        m.mpp       = load<uint8_t>(d, 0x1E);
                        m.zone      = load<uint16_t>(d, 0x20);
                        m.mainJob   = load<uint8_t>(d, 0x22);
                        m.mainLevel = load<uint8_t>(d, 0x23);
                        m.subJob    = load<uint8_t>(d, 0x24);
                        m.subLevel  = load<uint8_t>(d, 0x25);
                        const auto* nm = reinterpret_cast<const char*>(d.data() + 0x28);
                        m.name         = std::string(nm, strnlen(nm, std::min<std::size_t>(16, d.size() - 0x28)));
                        std::stable_sort(world_.party.begin(), world_.party.end(),
                                         [](const PartyMember& a, const PartyMember& b) { return std::tie(a.partyNo, a.order) < std::tie(b.partyNo, b.order); });
                    }
                    break;
                case 0x067: // ENTITY_UPDATE / char sync: kind (low 6 bits of +4) 2 = us; riding: +0x18 / +0x1C the chocobo's
                            // CustomProperties (LSB char_sync.cpp, mountutils::packetDefinition)
                    if (d.size() >= 0x20 && (load<uint8_t>(d, 4) & 0x3F) == 2 && load<uint32_t>(d, 8) == world_.charId)
                    {
                        world_.chocoboLook = { load<uint32_t>(d, 0x18), load<uint32_t>(d, 0x1C) };
                        if (d.size() >= 0x28)
                        {
                            world_.mhExpansion = (load<uint8_t>(d, 0x27) & 1) != 0;
                            world_.mhOpen      = load<uint8_t>(d, 0x24) != 0; // MogHouseFlag (own client only)
                        }
                    }
                    break;
                case 0x068: // PET_SYNC: type 4 (low 6 bits of +4), our id @8; with a pet: index @0xC, Hpp @0xE, Mpp @0xF,
                            // Tp @0x10, its target @0x14, name @0x18 (LSB pet_sync.cpp)
                    if (d.size() >= 0x0C && (load<uint8_t>(d, 4) & 0x3F) == 4 && load<uint32_t>(d, 8) == world_.charId)
                    {
                        world_.petPresent = d.size() >= 0x19 && load<uint16_t>(d, 0x0C) != 0;
                        if (world_.petPresent)
                        {
                            world_.petIndex = load<uint16_t>(d, 0x0C);
                            world_.petHpp   = load<uint8_t>(d, 0x0E);
                            world_.petMpp   = load<uint8_t>(d, 0x0F);
                            world_.petTp    = load<uint16_t>(d, 0x10);
                            const auto* nm  = reinterpret_cast<const char*>(d.data() + 0x18);
                            world_.petName  = std::string(nm, strnlen(nm, d.size() - 0x18));
                        }
                    }
                    break;
                case 0x076: // GROUP_EFFECTS: 5 x {UniqueNo, ActIndex, pad, u64 Bits, u8 Buffs[32]} @4; id = Buffs[i] | (Bits >> 2i & 3) << 8
                {
                    for (std::size_t m = 0; m < 5 && 4 + 48 * (m + 1) <= d.size(); ++m)
                    {
                        const std::size_t o  = 4 + 48 * m;
                        const auto        id = load<uint32_t>(d, o);
                        if (id == 0)
                        {
                            continue;
                        }
                        const auto bits = load<uint64_t>(d, o + 8);
                        for (auto& pm : world_.party)
                        {
                            if (pm.id != id)
                            {
                                continue;
                            }
                            pm.buffs.clear();
                            for (std::size_t i = 0; i < 32; ++i)
                            {
                                const auto b = static_cast<uint16_t>(load<uint8_t>(d, o + 16 + i) | ((bits >> (2 * i)) & 3) << 8);
                                if (b != 0xFF)
                                {
                                    pm.buffs.push_back(b);
                                }
                            }
                        }
                    }
                    break;
                }
                case 0x0DF: // GROUP_ATTR: UniqueNo @4, Hp @8, Mp @0xC, Tp @0x10, Hpp @0x16, Mpp @0x17, jobs @0x20..0x23
                    for (auto& m : world_.party)
                    {
                        if (m.id == load<uint32_t>(d, 4) && d.size() >= 0x24)
                        {
                            m.hp        = load<uint32_t>(d, 0x08);
                            m.mp        = load<uint32_t>(d, 0x0C);
                            m.tp        = load<uint32_t>(d, 0x10);
                            m.hpp       = load<uint8_t>(d, 0x16);
                            m.mpp       = load<uint8_t>(d, 0x17);
                            m.mainJob   = load<uint8_t>(d, 0x20);
                            m.mainLevel = load<uint8_t>(d, 0x21);
                            m.subJob    = load<uint8_t>(d, 0x22);
                            m.subLevel  = load<uint8_t>(d, 0x23);
                        }
                    }
                    if (load<uint32_t>(d, 4) == world_.charId && d.size() >= 0x24)
                    {
                        world_.hp        = load<uint32_t>(d, 0x08);
                        world_.mp        = load<uint32_t>(d, 0x0C);
                        world_.tp        = load<uint32_t>(d, 0x10);
                        world_.hpp       = load<uint8_t>(d, 0x16);
                        world_.mpp       = load<uint8_t>(d, 0x17);
                        world_.mainJob   = load<uint8_t>(d, 0x20);
                        world_.mainLevel = load<uint8_t>(d, 0x21);
                        world_.subJob    = load<uint8_t>(d, 0x22);
                        world_.subLevel  = load<uint8_t>(d, 0x23);
                    }
                    break;
                case 0x061: // CLISTATUS: hpmax @4, mpmax @8, jobs @0xC..0xF, exp @0x10/0x12, base stats @0x14, bonuses @0x22,
                            // attack @0x30, defense @0x32, elemental defense @0x34, title @0x44, rank @0x46, rank bar @0x48,
                            // home point @0x4A, nation @0x50, su_lv @0x52, ilvl @0x55
                    if (d.size() >= 0x52)
                    {
                        auto& st   = world_.stats;
                        st.hpMax   = load<int32_t>(d, 0x04);
                        st.mpMax   = load<int32_t>(d, 0x08);
                        st.exp     = load<int16_t>(d, 0x10);
                        st.expNext = load<int16_t>(d, 0x12);
                        for (std::size_t i = 0; i < 7; ++i)
                        {
                            st.base[i]  = load<uint16_t>(d, 0x14 + 2 * i);
                            st.bonus[i] = load<int16_t>(d, 0x22 + 2 * i);
                        }
                        st.attack  = load<int16_t>(d, 0x30);
                        st.defense = load<int16_t>(d, 0x32);
                        for (std::size_t i = 0; i < 8; ++i)
                        {
                            st.elementDefense[i] = load<int16_t>(d, 0x34 + 2 * i);
                        }
                        st.title  = load<uint16_t>(d, 0x44);
                        st.rank   = load<uint16_t>(d, 0x46);
                        st.rankBar  = load<uint16_t>(d, 0x48);
                        st.bindZone = load<uint16_t>(d, 0x4A);
                        st.nation   = load<uint8_t>(d, 0x50);
                        if (d.size() >= 0x56)
                        {
                            st.suLevel   = load<uint8_t>(d, 0x52);
                            st.itemLevel = load<uint8_t>(d, 0x55);
                        }
                        if (d.size() >= 0x5C)
                        {
                            // unity_info @0x58: Faction (the Unity leader 1..11, 0 = none) in bits 0..4 (LSB 0x061_clistatus.h).
                            st.unityLeader = static_cast<uint8_t>(load<uint32_t>(d, 0x58) & 0x1F);
                        }
                        st.valid  = true;
                    }
                    break;
                case 0x062: // CLISTATUS2: CommandRecast[31] @+0x04, skill_base[64] @+0x80
                    world_.skillsValid = parseSkills(d, world_.skills) || world_.skillsValid;
                    break;
                case 0x057: // weather change: StartTime @+4, WeatherNumber @+8
                    world_.weather = load<uint16_t>(d, 0x08);
                    break;
                case 0x05B: // server-forced position: x/height/z @+4, UniqueNo @+0x10, ActIndex @+0x14, Mode @+0x16
                {
                    const auto id   = load<uint32_t>(d, 0x10);
                    const auto mode = d.size() > 0x16 ? load<uint8_t>(d, 0x16) : uint8_t{ 0 };
                    if (id == world_.charId)
                    {
                        world_.serverPos = { load<float>(d, 4), load<float>(d, 8), load<float>(d, 0x0C) };
                        playerPos_       = world_.serverPos;
                        ++world_.teleportGeneration;
                    }
                    else if (auto it = entities_.find(id); it != entities_.end() && (mode <= 1 || mode == 3 || mode == 5 || mode == 6))
                    {
                        it->second.pos = { load<float>(d, 4), load<float>(d, 8), load<float>(d, 0x0C) };
                    }
                    // The appear modes set entity +0x144; its next update plays `pop0` (3) or `pop1` (6) (CODE
                    // 0x1009efd4, 0x1009095b).
                    if (mode == 3 || mode == 6)
                    {
                        world_.posEffects.push_back({ id, mode });
                        if (world_.posEffects.size() > 16)
                        {
                            world_.posEffects.pop_front();
                        }
                        ++world_.posEffectSerial;
                    }
                    break;
                }
                case 0x00D: // other PCs: UniqueNo @4, ActIndex @8, SendFlg @0x0A, dir @0x0B, x/height/z @0x0C, HPP @0x1E, name @0x5A
                case 0x00E: // NPCs/mobs: UniqueNo @4, ActIndex @8, mask @0x0A, dir @0x0B, x/height/z @0x0C, HPP @0x1E, name @0x34
                {
                    const auto id   = load<uint32_t>(d, 4);
                    const auto mask = load<uint8_t>(d, 0x0A);
                    if (id == world_.charId)
                    {
                        break;
                    }
                    if (mask & 0x20) // despawn
                    {
                        entities_.erase(id);
                        break;
                    }
                    auto& e = entities_[id];
                    e.id    = id;
                    e.index = load<uint16_t>(d, 8);
                    e.isPc  = sp.id == 0x00D;
                    if (sp.id == 0x00E && d.size() > 0x20)
                    {
                        // Flags1's low byte (PS2 MonsterFlag bit 0, HideFlag bit 1, SleepFlag bit 2), where LSB writes the
                        // entity status (monsters 1 "update", NPCs 0, cutscene-only 6, invisible 3; pets / trusts on the
                        // player's side are sent as 0). Every update carries it.
                        e.status = load<uint8_t>(d, 0x20);
                        e.isMob  = (e.status & 0x01) != 0;
                        e.hitbox = d.size() > 0x25 ? static_cast<float>(load<uint8_t>(d, 0x25)) / 10.0f : 0.0f;
                    }
                    static const bool entTrace = std::getenv("XI_ENT_TRACE") != nullptr; // XI_ENT_TRACE=1: raw 0x00E bytes +0x1E..+0x2B per update
                    if (entTrace && sp.id == 0x00E && d.size() > 0x2B)
                    {
                        std::string hex;
                        for (std::size_t k = 0x1E; k < 0x2C; ++k) hex += fmt::format("{:02x} ", std::to_integer<int>(d[k]));
                        spdlog::info("entity: {:08x} mask {:02x} name '{}' b1e..2b {}", id, mask, e.name, hex);
                    }
                    if (mask & 0x01)
                    {
                        e.dir = load<uint8_t>(d, 0x0B);
                        e.pos = { load<float>(d, 0x0C), load<float>(d, 0x10), load<float>(d, 0x14) };
                        if (d.size() >= 0x1C)
                        {
                            e.targetIndex = static_cast<uint16_t>(load<uint32_t>(d, 0x18) >> 17); // facetarget
                            e.groundFlag  = ((load<uint32_t>(d, 0x18) >> 15) & 1) != 0;
                        }
                    }
                    // HP/animation only when the update carries them (bit 0x04); position-only updates leave them 0,
                    // which used to zero other players' HP%.
                    if (mask & 0x04)
                    {
                        e.hpp       = load<uint8_t>(d, 0x1E);
                        e.animation = load<uint8_t>(d, 0x1F);
                        if (d.size() >= 0x24)
                        {
                            e.targetOff = flags1TargetOff(load<uint32_t>(d, 0x20)); // CODE 0x1009bfd7
                        }
                        if (e.isPc && d.size() >= 0x48)
                        {
                            e.chocoboIndex   = static_cast<uint8_t>((load<uint32_t>(d, 0x20) >> 5) & 7);
                            e.chocoboLook    = { load<uint32_t>(d, 0x34), load<uint32_t>(d, 0x38) };
                            e.mountIndex     = static_cast<uint8_t>(load<uint32_t>(d, 0x44) >> 4);
                            e.linkshell      = ((load<uint32_t>(d, 0x20) >> 17) & 1) != 0;
                            e.hackMove       = ((load<uint32_t>(d, 0x20) >> 27) & 1) != 0;
                            e.lsColor        = { load<uint8_t>(d, 0x24), load<uint8_t>(d, 0x25), load<uint8_t>(d, 0x26) };
                            e.nameFlags      = NameFlags::fromPcUpdate(load<uint32_t>(d, 0x20), load<uint32_t>(d, 0x24), load<uint32_t>(d, 0x28),
                                                                       load<uint8_t>(d, 0x32), load<uint8_t>(d, 0x33));
                        }
                        // namevis rides with the HP block (LSB entity_update.cpp): 0x01 icon, 0x08 hide name, 0x80 ghost.
                        if (!e.isPc && d.size() > 0x2B)
                        {
                            e.nameHidden = (load<uint8_t>(d, 0x2B) & 0x08) != 0;
                        }
                    }
                    if ((mask & 0x02) && !e.isPc && d.size() >= 0x30)
                    {
                        e.claimId = load<uint32_t>(d, 0x2C);
                    }
                    // Appearance: PCs GrapIDTbl @+0x48 (SendFlg bit 4 = model); NPCs/mobs look_t @+0x30.
                    if (e.isPc && (mask & 0x10) && d.size() >= 0x5A)
                    {
                        e.look.kind  = 0xFFFF;
                        e.look.valid = true;
                        for (std::size_t i = 0; i < 9; ++i)
                        {
                            e.look.ids[i] = load<uint16_t>(d, 0x48 + 2 * i);
                        }
                    }
                    else if (!e.isPc && d.size() >= 0x34)
                    {
                        // look_t: u16 kind (0 standard, 1 equipped, 2 door, 3 lift, 4 ship, 6 model in look slot 9:
                        // Ghrah, automatons; 7 chocobo), u16 model id (or face|race); gear ids follow only for kinds 1 and 7
                        // (else the name is at +0x34). The client switches on kind & 7 (CODE 0x1009c837).
                        e.look.kind   = load<uint16_t>(d, 0x30);
                        e.look.ids    = {};
                        e.look.ids[0] = load<uint16_t>(d, 0x32);
                        if ((e.look.kind == 1 || e.look.kind == 7) && d.size() >= 0x44)
                        {
                            for (std::size_t i = 1; i < 9; ++i)
                            {
                                e.look.ids[i] = load<uint16_t>(d, 0x32 + 2 * i);
                            }
                        }
                        e.look.valid = true;
                        if (e.look.kind >= 2 && e.look.kind <= 4)
                        {
                            e.doorId = d.size() >= 0x38 ? load<uint32_t>(d, 0x34) : 0u;
                            if ((e.look.kind == 3 || e.look.kind == 4) && d.size() >= 0x3E)
                            {
                                e.liftTime   = load<uint32_t>(d, 0x38);
                                e.liftTravel = load<uint16_t>(d, 0x3C);
                            }
                            // Doors, lifts and ships: LSB puts a door id (u32, else the name's first 4 chars) at +0x34, after
                            // u16 unused at +0x32 (entity_update.cpp packet_data_2..4). client::Doors moves the zone parts.
                            spdlog::debug("entity {:x} index {}: look kind {} model {:#x} door id {:#010x} at ({:.1f}, {:.1f}, {:.1f})", e.id, e.index,
                                          e.look.kind, e.look.ids[0], d.size() >= 0x38 ? load<uint32_t>(d, 0x34) : 0u, e.pos.x, e.pos.y, e.pos.z);
                        }
                    }
                    if (e.isPc)
                    {
                        if ((mask & 0x08) && d.size() > 0x5A)
                        {
                            auto       n = loadString(d, 0x5A, std::min<std::size_t>(d.size() - 0x5A, 24));
                            const bool ok =
                                !n.empty() && std::isalpha(static_cast<unsigned char>(n[0])) &&
                                std::all_of(n.begin(), n.end(), [](char ch) { return ch >= 0x20 && ch < 0x7F; });
                            if (ok)
                            {
                                e.name = n;
                            }
                        }
                    }
                    else if (auto n = entityNameFromUpdate(d))
                    {
                        // FUNC_Packet_Incoming_0x000E's name rules (client/entity_names.h): "" = the zone's name DAT (the
                        // client's fallback fills it in). "blank" is LSB's placeholder for NPCs whose retail name is empty
                        // (npc_list name 'blank', polutils name ''): the game would show nothing, so they keep no name.
                        if (*n == "blank")
                        {
                            n->clear();
                        }
                        std::replace(n->begin(), n->end(), '_', ' '); // mob names come as "Goblin_Thug"
                        // A server rename list (S2C 0x1FF, asked for with entity_renames) names dynamic entities by the
                        // name their spawn carried.
                        if (e.index >= 0x700)
                        {
                            const auto raw =
                                loadString(d, 0x34, std::min<std::size_t>(d.size() > 0x34 ? d.size() - 0x34 : 0, 24));
                            e.renameKey = (mask & 0x08) ? raw : e.renameKey;
                            if (const auto r = renames_.lookup(world_.zone, e.renameKey))
                            {
                                *n = *r;
                            }
                        }
                        e.name = *n;
                    }
                    // A player whose name never came (their first update went out before we were in range, e.g. a
                    // GM warp): ask for a full update, C2S 0x016 CHAR_REQ {ActIndex u16}, at most every 3 s
                    // (INFERRED when the game asks; LSB answers with a full 0x00D).
                    if (e.isPc && e.name.empty() && !(mask & 0x20))
                    {
                        const auto nowT = std::chrono::steady_clock::now();
                        auto&      last = nameAsked_[e.index];
                        if (nowT - last > std::chrono::seconds(3))
                        {
                            last = nowT;
                            std::vector<std::byte> req(4);
                            std::memcpy(req.data(), &e.index, 2);
                            pendingRaw_.emplace_back(uint16_t{ 0x016 }, std::move(req));
                        }
                    }
                    break;
                }
                case 0x1FF: // a server's rename list (HorizonXI; client/entity_names.h), applied to the entities already here
                {
                    if (renames_.parse(d))
                    {
                        for (auto& [eid, ent] : entities_)
                        {
                            if (ent.index >= 0x700)
                            {
                                if (const auto r = renames_.lookup(world_.zone, ent.renameKey))
                                {
                                    ent.name = *r;
                                }
                            }
                        }
                    }
                    break;
                }
                case 0x032: // event start: UniqueNo @4, ActIndex @8, EventNum @0xA, EventPara @0xC
                case 0x034: // event start with params: UniqueNo @4, num[8] @8, ActIndex @0x28, EventNum @0x2A, EventPara @0x2C
                {
                    const std::size_t o = sp.id == 0x032 ? 0x08 : 0x28;
                    auto&             ev = world_.event;
                    ev.uniqueNo  = load<uint32_t>(d, 4);
                    ev.actIndex  = load<uint16_t>(d, o);
                    ev.eventNum  = load<uint16_t>(d, o + 2);
                    ev.eventPara = load<uint16_t>(d, o + 4);
                    ev.params    = {};
                    ev.strings   = {};
                    if (sp.id == 0x034)
                    {
                        for (std::size_t k = 0; k < 8; ++k)
                        {
                            ev.params[k] = load<uint32_t>(d, 8 + 4 * k);
                        }
                    }
                    ++world_.eventSerial; // the client runs the script (xi-client main); it finishes the event
                    break;
                }
                case 0x033: // EVENTSTR: UniqueNo @4, ActIndex @8, EventNum @0xA, EventPara @0xC, Mode @0xE,
                            // String[4][16] @0x10, Data[8] @0x50 (LSB 0x033_eventstr.h; handler 0x100a02f0)
                    if (d.size() >= 0x70)
                    {
                        auto& ev     = world_.event;
                        ev.uniqueNo  = load<uint32_t>(d, 4);
                        ev.actIndex  = load<uint16_t>(d, 8);
                        ev.eventNum  = load<uint16_t>(d, 0xA);
                        ev.eventPara = load<uint16_t>(d, 0xC);
                        for (std::size_t k = 0; k < 4; ++k)
                        {
                            ev.strings[k] = loadString(d, 0x10 + 16 * k, 16);
                        }
                        for (std::size_t k = 0; k < 8; ++k)
                        {
                            ev.params[k] = load<uint32_t>(d, 0x50 + 4 * k);
                        }
                        ++world_.eventSerial;
                    }
                    break;
                case 0x05D: // PENDINGSTR: num[9] @4, String[4][16] @0x28; the client keeps only the strings
                            // (handler 0x100a0040), so the update wait still ends with 0x052 / 0x05C
                    if (d.size() >= 0x68)
                    {
                        for (std::size_t k = 0; k < 4; ++k)
                        {
                            world_.event.strings[k] = loadString(d, 0x28 + 16 * k, 16);
                        }
                        ++world_.eventStringsSerial;
                    }
                    break;
                case 0x052: // EVENTUCOFF: Mode u32 @4 (XiPackets world/server/0x0052)
                {
                    const auto mode = load<uint32_t>(d, 4);
                    spdlog::debug("event: 0x052 mode {}", mode);
                    if (mode == 1) // EventRecvPending: the event update was answered
                    {
                        ++world_.eventReleaseSerial;
                    }
                    break;
                }
                case 0x05C: // PENDINGNUM: num[8] @4, the event's new parameters (the server's onEventUpdate)
                {
                    for (std::size_t k = 0; k < 8; ++k)
                    {
                        world_.eventUpdate[k] = load<uint32_t>(d, 4 + 4 * k);
                    }
                    ++world_.eventUpdateSerial;
                    break;
                }
                case 0x026: // ITEM_SUBCONTAINER (a mannequin's gear): is_used @4, container @5, index @6, looks @0xA..0x1B
                            // (race_hair, head .. range), race @0x1C, pose @0x1D (LSB 0x026_item_subcontainer.h)
                    if (d.size() >= 0x1E)
                    {
                        const uint16_t key = static_cast<uint16_t>(load<uint8_t>(d, 5) << 8 | load<uint8_t>(d, 6));
                        if (load<uint8_t>(d, 4) == 0)
                        {
                            world_.mannequins.erase(key);
                        }
                        else
                        {
                            auto& m = world_.mannequins[key];
                            for (std::size_t k = 0; k < 9; ++k)
                            {
                                m.ids[k] = load<uint16_t>(d, 0x0A + 2 * k);
                            }
                            m.race = load<uint8_t>(d, 0x1C);
                            m.pose = load<uint8_t>(d, 0x1D);
                        }
                    }
                    break;
                case 0x01C: // ITEM_MAX: ItemNum u8[18] @4 (1 + each container's size), ItemNum2 u16[18] @0x24 (usable; 0 = off)
                    if (d.size() >= 0x24 + 2 * 18)
                    {
                        for (std::size_t c = 0; c < 18; ++c)
                        {
                            world_.containerSize[c]   = load<uint8_t>(d, 4 + c);
                            world_.containerUsable[c] = load<uint16_t>(d, 0x24 + 2 * c);
                        }
                        world_.containerSizeValid = true;
                    }
                    break;
                case 0x044: // EXTENDED_JOB: Job @4, IsSubJob @5, then the job's data (LSB 0x044_extended_job_*.h)
                    if (d.size() >= 6)
                    {
                        auto& to = world_.extendedJob[load<uint8_t>(d, 5) ? 1 : 0];
                        to       = {};
                        for (std::size_t k = 0; k < to.size() && 4 + k < d.size(); ++k)
                        {
                            to[k] = load<uint8_t>(d, 4 + k);
                        }
                    }
                    break;
                case 0x01F: // ITEM_LIST: ItemNum @4, ItemNo @8, Category @0xA, ItemIndex @0xB, LockFlg @0xC
                case 0x020: // ITEM_ATTR: ItemNum @4, Price @8, ItemNo @0xC, Category @0xE, ItemIndex @0xF
                {
                    const std::size_t o   = sp.id == 0x01F ? 8 : 0x0C;
                    const auto        num = load<uint32_t>(d, 4);
                    const auto        no  = load<uint16_t>(d, o);
                    const auto        cat = load<uint8_t>(d, o + 2);
                    const auto        idx = load<uint8_t>(d, o + 3);
                    const auto        key = static_cast<uint16_t>(cat << 8 | idx);
                    if (key == 0)
                    {
                        inventory_[0] = InventoryItem{ 0xFFFF, num, 0, 0, {} }; // the gil: kept even at 0 (see 0x01E)
                    }
                    else if (num == 0 || no == 0)
                    {
                        inventory_.erase(key);
                    }
                    else
                    {
                        InventoryItem item{ no, num, cat, idx, {} };
                        item.lockFlags = load<uint8_t>(d, o + 4);
                        item.price     = sp.id == 0x020 ? load<uint32_t>(d, 8) : 0;
                        if (sp.id == 0x020 && d.size() >= 0x11 + 24)
                        {
                            for (std::size_t k = 0; k < 24; ++k)
                            {
                                item.attr[k] = load<uint8_t>(d, 0x11 + k);
                            }
                        }
                        else if (const auto old = inventory_.find(key); old != inventory_.end() && old->second.itemId == no)
                        {
                            item.attr = old->second.attr; // 0x01F carries no exdata: keep what 0x020 said
                        }
                        inventory_[key] = item;
                    }
                    break;
                }
                case 0x01E: // ITEM_NUM: ItemNum @4, Category @8, ItemIndex @9
                {
                    const auto key = static_cast<uint16_t>(load<uint8_t>(d, 8) << 8 | load<uint8_t>(d, 9));
                    const auto num = load<uint32_t>(d, 4);
                    if (key == 0)
                    {
                        // The gil (inventory slot 0) stays known at 0: going from 0 gil to some (a trade, a sale)
                        // comes as ITEM_NUM alone.
                        inventory_[0] = InventoryItem{ 0xFFFF, num, 0, 0, {} };
                    }
                    else if (num == 0)
                    {
                        inventory_.erase(key);
                    }
                    else if (const auto it = inventory_.find(key); it != inventory_.end())
                    {
                        it->second.count = num;
                    }
                    break;
                }
                case 0x053: // SYSTEMMES (PutSystemMessage): para @4, para2 @8, Number (LSB MsgStd) @0x0C, the text from
                {           // file 7031. E.g. the /logout countdown every 5 s: 7 "Executing logout in <para> seconds. ..."
                    if (d.size() < 0x0E || !cfg_.index)
                    {
                        break;
                    }
                    if (!stdText_)
                    {
                        try
                        {
                            stdText_ = dat::DialogTable::parse(cfg_.index->read(7031));
                        }
                        catch (const std::exception& e)
                        {
                            spdlog::warn("standard messages (7031): {}", e.what());
                            stdText_ = dat::DialogTable{};
                        }
                    }
                    const uint16_t  mes = load<uint16_t>(d, 0x0C);
                    dat::DialogArgs args;
                    if (cfg_.text)
                    {
                        args.lookup = [t = cfg_.text](uint16_t table, uint32_t value) { return t->dialogName(table, value); };
                    }
                    args.numbers[0] = static_cast<int32_t>(load<uint32_t>(d, 0x04));
                    args.numbers[1] = static_cast<int32_t>(load<uint32_t>(d, 0x08));
                    if (mes < stdText_->size())
                    {
                        for (auto& l : dat::formatDialog(stdText_->raw(mes), args))
                        {
                            if (!l.empty() && l != "\x0B")
                            {
                                world_.chat.push_back({ 0xFF, "", std::move(l) });
                            }
                        }
                    }
                    break;
                }
                case 0x009: // MESSAGE (standard message, LSB MsgStd): UniqueNo @4, ActIndex @8, MesNo @0x0A, Attr @0x0C,
                            // Data[128] @0x0D: parameters as text, "Para0 <n> Para1 <n>" (or "string2 <text>")
                {
                    if (d.size() < 0x0D || !cfg_.index)
                    {
                        break;
                    }
                    if (!stdText_)
                    {
                        try
                        {
                            stdText_ = dat::DialogTable::parse(cfg_.index->read(7031));
                        }
                        catch (const std::exception& e)
                        {
                            spdlog::warn("standard messages (7031): {}", e.what());
                            stdText_ = dat::DialogTable{};
                        }
                    }
                    const uint16_t mes = load<uint16_t>(d, 0x0A);
                    // Data: "ParaN <number>" and "stringN <text>" tokens, space separated.
                    dat::DialogArgs   args;
                    if (cfg_.text)
                    {
                        args.lookup = [t = cfg_.text](uint16_t table, uint32_t value) { return t->dialogName(table, value); };
                    }
                    const std::string data = loadString(d, 0x0D, std::min<std::size_t>(128, d.size() - 0x0D));
                    std::istringstream in(data);
                    std::string        key, value;
                    while (in >> key >> value)
                    {
                        const bool para = key.rfind("Para", 0) == 0, str = key.rfind("string", 0) == 0;
                        const auto slot = para ? std::strtoul(key.c_str() + 4, nullptr, 10) : str ? std::strtoul(key.c_str() + 6, nullptr, 10) : 99;
                        if (para && slot < args.numbers.size())
                        {
                            args.numbers[slot] = static_cast<int32_t>(std::strtol(value.c_str(), nullptr, 10));
                        }
                        else if (str && slot < args.strings.size())
                        {
                            args.strings[slot] = value;
                        }
                    }
                    if (mes < stdText_->size())
                    {
                        for (auto& l : dat::formatDialog(stdText_->raw(mes), args))
                        {
                            if (!l.empty() && l != "\x0B")
                            {
                                world_.chat.push_back({ 0xFF, "", std::move(l) });
                            }
                        }
                    }
                    break;
                }
                case 0x03E: // SHOP_OPEN: ShopListNum @4. The goods follow in 0x03C.
                    world_.shop.clear();
                    ++world_.shopSerial;
                    break;
                case 0x03D: // SHOP_SELL: Price u32 @4, PropertyItemIndex @8, Type @9, Count u32 @0xC
                    if (d.size() >= 0x10)
                    {
                        world_.sellQuote = { load<uint32_t>(d, 4), load<uint8_t>(d, 8), load<uint32_t>(d, 0x0C) };
                        ++world_.sellQuoteSerial;
                    }
                    break;
                case 0x03C: // SHOP_LIST: ShopItemOffsetIndex @4, then 12-byte entries @8 {u32 price, u16 item, u8 index, pad,
                            // u16 skill, u16 guild}
                    for (std::size_t o = 8; o + 12 <= d.size(); o += 12)
                    {
                        const auto item = load<uint16_t>(d, o + 4);
                        if (item != 0)
                        {
                            world_.shop.push_back({ load<uint32_t>(d, o), item, load<uint8_t>(d, o + 6) });
                        }
                    }
                    break;
                case 0x086: // GUILD_OPEN: Stat @4 (0 open, 1 closed, 2 holiday), Time @8 (client/guild_shop.h). Every
                {           // answer empties both lists (CODE 0x100eadb0).
                    const auto g = parseGuildOpen(d);
                    for (auto& l : world_.guild.lists)
                    {
                        l.items.clear();
                    }
                    ++world_.guild.listSerial;
                    if (g.stat == 0)
                    {
                        ++world_.guild.openSerial;
                    }
                    else
                    {
                        guildClosedLines(g);
                    }
                    break;
                }
                case 0x083: // GUILD_BUYLIST / GUILD_SELLLIST: List[30] @4, Count @0xF4, Stat @0xF5 (0x40 first, 0x80 last)
                case 0x085:
                    mergeGuildList(world_.guild.lists[sp.id == 0x083 ? 0 : 1], d);
                    ++world_.guild.listSerial;
                    break;
                case 0x082: // GUILD_BUY / GUILD_SELL: ItemNo @4, Count @6 (the new stock), Trade @7 (signed)
                case 0x084:
                    if (applyGuildStock(world_.guild.lists[sp.id == 0x082 ? 0 : 1], d))
                    {
                        ++world_.guild.listSerial;
                    }
                    guildResultLines(sp.id == 0x084, d);
                    break;
                case 0x0E0: // GROUP_COMLINK: LinkshellNum @4 (1/2), ItemIndex @5 (0 = none), Category @6: the active pearl
                    if (d.size() >= 7 && (load<uint8_t>(d, 4) == 1 || load<uint8_t>(d, 4) == 2))
                    {
                        world_.equipment[15u + load<uint8_t>(d, 4)] = { load<uint8_t>(d, 6), load<uint8_t>(d, 5) };
                    }
                    break;
                case 0x050: // EQUIP_LIST: PropertyItemIndex @4, EquipKind @5, Category @6
                {
                    const auto kind = load<uint8_t>(d, 5);
                    if (kind < world_.equipment.size())
                    {
                        world_.equipment[kind] = { load<uint8_t>(d, 6), load<uint8_t>(d, 4) };
                    }
                    break;
                }
                case 0x051: // GRAP_LIST: own appearance GrapIDTbl[9] @4 (changes with equipment)
                {
                    world_.look.kind  = 0xFFFF;
                    world_.look.valid = true;
                    for (std::size_t i = 0; i < 9; ++i)
                    {
                        world_.look.ids[i] = load<uint16_t>(d, 4 + 2 * i);
                    }
                    break;
                }
                case 0x11A: // EMOTE_LIST: JobEmotes u32 @4, Chairs u16 @8 (LSB 0x11a_emote_list.h)
                {
                    if (d.size() >= 10)
                    {
                        world_.emoteJobs   = load<uint32_t>(d, 4);
                        world_.emoteChairs = load<uint16_t>(d, 8);
                        spdlog::debug("session: emote list: job emotes {:#x}, chairs {:#x}", world_.emoteJobs, world_.emoteChairs);
                    }
                    break;
                }
                case 0x113: // CURRENCIES_1: the Currencies window's values (LSB 0x113_currencies_1.h; ui-format-notes §10.7)
                    world_.currencies.assign(d.begin(), d.end());
                    ++world_.currenciesSeq;
                    break;
                case 0x119: // ABIL_RECAST: Timers[31] @4, 8 bytes each: Timer u16 (seconds left), Calc1 u8, TimerId u8
                {
                    const auto now  = std::chrono::steady_clock::now();
                    auto       prev = std::move(world_.abilityRecastReady);
                    world_.abilityRecastReady.clear();
                    std::vector<uint16_t> started; // timers that are new or restarted since the last update
                    world_.abilityRecasts   = {};
                    world_.abilityRecastsAt = now;
                    for (std::size_t k = 0; k < 31 && 4 + 8 * k + 8 <= d.size(); ++k)
                    {
                        world_.abilityRecasts[k] = { load<uint16_t>(d, 4 + 8 * k), load<uint8_t>(d, 4 + 8 * k + 2), load<uint8_t>(d, 4 + 8 * k + 3),
                                                     load<int16_t>(d, 4 + 8 * k + 4) };
                    }
                    for (std::size_t k = 0; k < 31 && 4 + 8 * k + 4 <= d.size(); ++k)
                    {
                        const auto seconds = load<uint16_t>(d, 4 + 8 * k);
                        const auto id      = load<uint8_t>(d, 4 + 8 * k + 3);
                        if (seconds == 0)
                        {
                            continue;
                        }
                        const auto ready              = now + std::chrono::seconds(seconds);
                        world_.abilityRecastReady[id] = ready;
                        const auto old                = prev.find(id);
                        if (old == prev.end() || ready > old->second + std::chrono::seconds(2))
                        {
                            started.push_back(id);
                        }
                    }
                    if (lastAbility_ && now - lastAbility_->second < std::chrono::seconds(5) && started.size() == 1)
                    {
                        world_.abilityRecastGroup[lastAbility_->first] = started.front();
                        lastAbility_.reset();
                    }
                    break;
                }
                case 0x0AA: // MAGIC_DATA: MagicDataTbl[128] @4, bit n = spell n known
                {
                    world_.spells.clear();
                    for (std::size_t i = 0; i < 128 * 8 && 4 + i / 8 < d.size(); ++i)
                    {
                        if (load<uint8_t>(d, 4 + i / 8) & (1u << (i % 8)))
                        {
                            world_.spells.push_back(static_cast<uint16_t>(i));
                        }
                    }
                    break;
                }
                case 0x0AC: // COMMAND_DATA: bitfields WeaponSkills[64] @+4, JobAbilities[64] @+0x44, PetAbilities[64] @+0x84,
                            // Traits[32] @+0xC4 (bit n = id n)
                {
                    auto bits = [&](std::size_t off, std::vector<uint16_t>& out, std::size_t bytes = 64) {
                        out.clear();
                        for (std::size_t i = 0; i < bytes * 8 && off + i / 8 < d.size(); ++i)
                        {
                            if (load<uint8_t>(d, off + i / 8) & (1u << (i % 8)))
                            {
                                out.push_back(static_cast<uint16_t>(i));
                            }
                        }
                    };
                    bits(0x04, world_.weaponSkills);
                    bits(0x44, world_.jobAbilities);
                    bits(0x84, world_.petAbilities);
                    bits(0xC4, world_.traits, 32); // Traits[32] after PetAbilities[64]
                    break;
                }
                case 0x05F: // MUSIC: slot u16 @4 (the MusicNum index), track u16 @6 (LSB GP_SERV_COMMAND_MUSIC)
                    if (d.size() >= 8 && load<uint16_t>(d, 4) < world_.music.size())
                    {
                        world_.music[load<uint16_t>(d, 4)] = load<uint16_t>(d, 6);
                    }
                    break;
                case 0x037: // SERVERSTATUS: UniqueNo @0x24, server_status (own animation) @0x30
                {
                    if (d.size() > 0x30 && load<uint32_t>(d, 0x24) == world_.charId)
                    {
                        lastOwnStatus_.assign(d.begin(), d.end()); // XI_FAKE_FISH answers with copies of it
                        world_.animation = load<uint8_t>(d, 0x30);
                        if (const auto speed = static_cast<uint16_t>(load<uint32_t>(d, 0x2C) & 0xFFF); speed != 0)
                        {
                            world_.speed = speed; // Flags1 Speed
                        }
                        world_.chocoboIndex = static_cast<uint8_t>((load<uint32_t>(d, 0x28) >> 13) & 7);
                        world_.anonymous    = (load<uint32_t>(d, 0x28) >> 5) & 1;
                        world_.linkshell    = ((load<uint32_t>(d, 0x28) >> 25) & 1) != 0;
                        world_.lsColor      = { load<uint8_t>(d, 0x31), load<uint8_t>(d, 0x32), load<uint8_t>(d, 0x33) };
                        if (d.size() > 0x58)
                        {
                            world_.nameFlags = NameFlags::fromServerStatus(load<uint32_t>(d, 0x28), load<uint32_t>(d, 0x2C), load<uint32_t>(d, 0x34),
                                                                           load<uint32_t>(d, 0x38), load<uint8_t>(d, 0x58));
                        }
                        world_.mountIndex   = d.size() > 0x5B ? load<uint8_t>(d, 0x5B) : 0;
                        if (d.size() > 0x4A)
                        {
                            world_.fishingTimer = load<uint8_t>(d, 0x4A); // fishing_timer: LSB hookDelay while casting
                        }
                        world_.petIndex = static_cast<uint16_t>((load<uint32_t>(d, 0x34) >> 3) & 0xFFFF);
                        if (world_.petIndex == 0)
                        {
                            world_.petPresent = false;
                        }
                        // Flags3 @0x38: bit 4 MentorFlag (LSB char_status.cpp flags3_t).
                        world_.mentor = d.size() > 0x38 && (load<uint8_t>(d, 0x38) & 0x10) != 0;
                        if (d.size() >= 0x40)
                        {
                            world_.deadCounter   = load<uint32_t>(d, 0x3C); // dead_counter1: 60 x seconds left (+6 min)
                            world_.deadCounterAt = std::chrono::steady_clock::now();
                        }
                    }
                    break;
                }
                case 0x115: // FISH: stamina @4, arrow_delay, regen, move_frequency, arrow_damage, arrow_regen, time (u16 each),
                            // angler_sense u8 @0x12, intuition u32 @0x14
                {
                    if (d.size() >= 0x18)
                    {
                        auto& f         = world_.fish;
                        f.stamina       = load<uint16_t>(d, 0x04);
                        f.arrowDelay    = load<uint16_t>(d, 0x06);
                        f.regen         = load<uint16_t>(d, 0x08);
                        f.moveFrequency = load<uint16_t>(d, 0x0A);
                        f.arrowDamage   = load<uint16_t>(d, 0x0C);
                        f.arrowRegen    = load<uint16_t>(d, 0x0E);
                        f.time          = load<uint16_t>(d, 0x10);
                        f.sense         = load<uint8_t>(d, 0x12);
                        f.special       = load<uint32_t>(d, 0x14);
                        ++world_.fishSerial;
                    }
                    break;
                }
                case 0x028: // BATTLE2: bit-packed action
                {
                    if (const auto a = parseBattleAction(d))
                    {
                        world_.actions.push_back(*a);
                        if (world_.actions.size() > 32)
                        {
                            world_.actions.pop_front();
                        }
                        ++world_.actionSerial;
                        // Our own spell (type 4 = magic finish): `info` is the recast in seconds.
                        if (a->actor == world_.charId && a->actionType == 4)
                        {
                            world_.spellReady[static_cast<uint16_t>(a->actionId)] = std::chrono::steady_clock::now() + std::chrono::seconds(a->info);
                        }
                        // Casting state: type 8 starts or interrupts a cast, type 4 finishes it (castChange).
                        if (const auto cast = castChange(*a); cast.applies)
                        {
                            auto apply = [&](uint16_t& castSpell, uint32_t& castsDone) {
                                castSpell = cast.spell;
                                castsDone += cast.released ? 1u : 0u;
                            };
                            if (a->actor == world_.charId)
                            {
                                apply(world_.castSpell, world_.castsDone);
                                world_.castStarted = std::chrono::steady_clock::now();
                            }
                            else if (const auto it = entities_.find(a->actor); it != entities_.end())
                            {
                                apply(it->second.castSpell, it->second.castsDone);
                            }
                        }
                        // Weapon skills (type 3) and monster TP moves (type 11): the result's animation id picks the
                        // motion files (dat-format §10.8).
                        if ((a->actionType == 3 || a->actionType == 11) && !a->targets.empty() && !a->targets[0].results.empty())
                        {
                            const auto anim = a->targets[0].results[0].animation;
                            if (a->actor == world_.charId)
                            {
                                world_.wsAnimation = anim;
                                ++world_.weaponSkillsDone;
                            }
                            else if (const auto it = entities_.find(a->actor); it != entities_.end())
                            {
                                it->second.wsAnimation = anim;
                                ++it->second.weaponSkills;
                            }
                        }
                        // Who hit each target last (physical: melee, ranged, WS, TP moves; and offensive magic), for the flinch
                        // direction; the effect's op 0x25 starts the flinch.
                        if (a->actionType == 1 || a->actionType == 2 || a->actionType == 3 || a->actionType == 11 || a->actionType == 4)
                        {
                            for (const auto& t : a->targets)
                            {
                                bool hit = false;
                                for (const auto& r : t.results)
                                {
                                    // LSB ActionResolution 0 = hit; a heal (message 7 / 24 ...) is not a hit: needs damage.
                                    const bool magicDamage = r.message == 2 || r.message == 252; // MsgBasic magic damage / burst
                                    hit = hit || (r.resolution == 0 && r.param > 0 && t.id != a->actor && (a->actionType != 4 || magicDamage));
                                }
                                if (!hit)
                                {
                                    continue;
                                }
                                if (t.id == world_.charId)
                                {
                                    world_.lastHitBy = a->actor;
                                }
                                else if (const auto it = entities_.find(t.id); it != entities_.end())
                                {
                                    it->second.lastHitBy = a->actor;
                                }
                            }
                        }
                        if (a->actionType == 1 || a->actionType == 3 || a->actionType == 11) // melee round, WS, mob TP move
                        {
                            if (a->actor == world_.charId)
                            {
                                ++world_.swings;
                            }
                            else if (const auto it = entities_.find(a->actor); it != entities_.end())
                            {
                                ++it->second.swings;
                            }
                        }
                        // Finishes whose effect commits the damage (op 0x2B: ranged, WS, magic, items, abilities, TP moves,
                        // pet skills) hold their messages until it does; melee rounds log at once.
                        const bool held = (a->actionType >= 2 && a->actionType <= 6) || a->actionType == 11 || a->actionType == 13;
                        uint16_t   flat = 0;
                        for (const auto& t : a->targets)
                        {
                            for (const auto& r : t.results)
                            {
                                holdTag = held ? (static_cast<uint64_t>(world_.actionSerial) << 16 | ++flat) : 0;
                                const std::array<uint32_t, 4> nums{ a->actionId, r.param, r.addEffectParam, r.spikesParam };
                                combatMessage(r.message, a->actor, t.id, nums, r.param, 0);
                                if (r.hasAddEffect && r.addEffectMessage)
                                {
                                    combatMessage(r.addEffectMessage, a->actor, t.id, nums, r.addEffectParam, 0);
                                }
                                if (r.hasSpikes && r.spikesMessage)
                                {
                                    combatMessage(r.spikesMessage, a->actor, t.id, nums, r.spikesParam, 0);
                                }
                            }
                        }
                        holdTag = 0;
                    }
                    break;
                }
                case 0x029: // BATTLE_MESSAGE: Cas @4, Tar @8, Data @0xC, Data2 @0x10, ActIndexCas @0x14, ActIndexTar @0x16, MessageNum @0x18
                case 0x02D: // BATTLE_MESSAGE2: Cas @4, Tar @8, ActIndexCas @0xC, ActIndexTar @0xE, Data @0x10, Data2 @0x14, MessageNum @0x18
                {
                    const std::size_t o = sp.id == 0x029 ? 0x0C : 0x10;
                    combatMessage(load<uint16_t>(d, 0x18), load<uint32_t>(d, 4), load<uint32_t>(d, 8),
                                  { load<uint32_t>(d, o), load<uint32_t>(d, o + 4), 0, 0 }, load<uint32_t>(d, o), load<uint32_t>(d, o + 4));
                    break;
                }
                case 0x0D2: // TROPHY_LIST: u32 TrophyItemNum @4, TargetUniqueNo @8, Gold @0xC, TrophyItemNo @0x10, TargetActIndex
                            // @0x12, TrophyItemIndex @0x14, Entry @0x15, StartTime @0x18, IsLocallyLotted @0x1C, Point @0x1E,
                            // LootUniqueNo @0x20, LootActIndex @0x24, LootPoint @0x26, LootActName[16] @0x28
                {
                    if (d.size() < 0x38)
                    {
                        break;
                    }
                    const auto slot = load<uint8_t>(d, 0x14);
                    if (slot >= world_.treasure.size())
                    {
                        break;
                    }
                    auto& t    = world_.treasure[slot];
                    t          = {};
                    t.itemId   = load<uint16_t>(d, 0x10);
                    {
                        const uint32_t who = load<uint32_t>(d, 8);
                        const auto     it  = entities_.find(who);
                        lootNote(lootFoundNote(t.itemId, load<uint8_t>(d, 0x15), load<uint8_t>(d, 0x16) != 0,
                                               nameOf(who),
                                               who == world_.charId || (it != entities_.end() && it->second.isPc)));
                    }
                    t.count    = load<uint32_t>(d, 4);
                    t.startTime = load<uint32_t>(d, 0x18);
                    t.seen     = std::chrono::steady_clock::now();
                    if (load<uint16_t>(d, 0x1C) != 0)
                    {
                        t.myLot = static_cast<int16_t>(load<uint16_t>(d, 0x1E));
                    }
                    t.bestLot  = static_cast<int16_t>(load<uint16_t>(d, 0x26));
                    t.bestName = printable(loadString(d, 0x28, 16));
                    break;
                }
                case 0x0D3: // TROPHY_SOLUTION: LootUniqueNo @4, EntryUniqueNo @8, LootActIndex @0xC, LootPoint @0xE, EntryActIndex
                            // @0x10, EntryPoint @0x12, TrophyItemIndex @0x14, JudgeFlg @0x15 (0 a lot, 1 won, 2 won but no room,
                            // 3 lost / nobody), sLootName @0x16 (best lot), sLootName2 @0x26 (who lotted)
                {
                    if (d.size() < 0x36)
                    {
                        break;
                    }
                    const auto slot = load<uint8_t>(d, 0x14);
                    if (slot >= world_.treasure.size())
                    {
                        break;
                    }
                    auto&      t     = world_.treasure[slot];
                    const auto judge = load<uint8_t>(d, 0x15);
                    {
                        const uint32_t winner = load<uint32_t>(d, 4);
                        lootNote(lootSolutionNote(judge, t.itemId,
                                                  winner != 0 ? printable(loadString(d, 0x16, 16)) : std::string(),
                                                  winner != 0 && winner == world_.charId,
                                                  printable(loadString(d, 0x26, 16)), load<uint16_t>(d, 0x12)));
                    }
                    if (judge != 0)
                    {
                        t = {}; // decided: the item leaves the pool (the game's own lines say who got it)
                        break;
                    }
                    t.bestLot  = load<int16_t>(d, 0x0E);
                    t.bestName = printable(loadString(d, 0x16, 16));
                    if (load<uint32_t>(d, 8) == world_.charId)
                    {
                        t.myLot = load<int16_t>(d, 0x12);
                    }
                    break;
                }
                case 0x05E: // CONQUEST: Balance @4, Alliance @5, Regions[27] @0x1A {ranking, ranking, graphics, Owner}, NextTally
                            // @0x8C, ConquestPoints @0x90
                    if (d.size() >= 0x94)
                    {
                        for (std::size_t r = 0; r < world_.conquest.owner.size(); ++r)
                        {
                            world_.conquest.owner[r] = load<uint8_t>(d, 0x1A + 4 * r + 3);
                        }
                        world_.conquest.nextTally = load<uint8_t>(d, 0x8C);
                        world_.conquest.points    = load<uint32_t>(d, 0x90);
                        if (d.size() >= 0xB4)
                        {
                            world_.conquest.besiegedOverview = load<uint32_t>(d, 0xA4);
                            for (std::size_t k = 0; k < 3; ++k)
                            {
                                world_.conquest.strongholds[k] = load<uint32_t>(d, 0xA8 + 4 * k);
                            }
                        }
                        ++world_.conquest.serial;
                    }
                    break;
                case 0x058: // ASSIST: UniqueNo @4, AssistNo @8 (the new target), ActIndex @0xC
                    if (d.size() >= 0x0C && load<uint32_t>(d, 4) == world_.charId)
                    {
                        world_.autoTargetId = load<uint32_t>(d, 8);
                        ++world_.autoTargetSerial;
                    }
                    break;
                case 0x049: // ITEMSEARCH: u16 ItemNo @4, Flag @6, ItemName[64] @8
                    if (d.size() >= 6)
                    {
                        world_.itemSearchId = load<uint16_t>(d, 4);
                        ++world_.itemSearchSerial;
                    }
                    break;
                case 0x04C: // AUC: Command @4, AucWorkIndex @5, Result @6, ResultStatus @7, Param @8 (AskCommit: u32
                            // Commission; Bid: u32 BidPrice, u16 ItemNo, u16 pad, u32 ItemStacks), Parcel @0x14 {Stat,
                            // pad, ItemIndex, pad, Name[16] @0x18, u16 ItemNo @0x28, ItemQuantity @0x2A, ItemCategory,
                            // u32 Price @0x2C}
                {
                    if (d.size() < 0x14)
                    {
                        break;
                    }
                    auto&         a      = world_.auction;
                    const uint8_t cmd    = load<uint8_t>(d, 4);
                    const int8_t  slot   = load<int8_t>(d, 5);
                    const uint8_t result = load<uint8_t>(d, 6);
                    a.results.emplace_back(cmd, result);
                    if (a.results.size() > 16)
                    {
                        a.results.pop_front();
                    }
                    ++a.resultSerial;
                    if (cmd == 0x02) // the counter opened the auction house
                    {
                        ++a.openSerial;
                    }
                    else if (cmd == 0x04 && result == 1) // AskCommit: the fee
                    {
                        a.fee = load<uint32_t>(d, 8);
                    }
                    // A sale slot (WorkCheck answers with Command 0x0C per slot; LotCheck / LotCancel too).
                    if ((cmd == 0x0C || cmd == 0x0D || cmd == 0x05) && slot >= 0 && slot < 7 && d.size() >= 0x30)
                    {
                        auto& s  = a.sales[static_cast<std::size_t>(slot)];
                        s.stat   = load<uint8_t>(d, 0x14);
                        s.itemId = s.stat ? load<uint16_t>(d, 0x28) : 0;
                        s.stack  = load<uint8_t>(d, 0x2A) == 0;
                        s.price  = load<uint32_t>(d, 0x2C);
                    }
                    break;
                }
                case 0x04B: // PBX_RESULT: Command @4, BoxNo @5, PostWorkNo @6, ItemWorkNo @7, i32 ItemStacks @8, Result @0xC,
                            // ResParam1-3 @0xD; State: u32 Stat @0x10, name[16] @0x14, u16 ItemNo @0x30, u32 Stack @0x38
                {
                    if (d.size() < 0x10)
                    {
                        break;
                    }
                    auto&         b      = world_.dbox;
                    const uint8_t cmd    = load<uint8_t>(d, 4);
                    const int8_t  box    = load<int8_t>(d, 5);
                    const int8_t  slot   = load<int8_t>(d, 6);
                    const int8_t  result = load<int8_t>(d, 0x0C);
                    b.lastCommand        = cmd;
                    b.lastResult         = result;
                    ++b.serial;
                    auto cellFromPacket = [&](WorldSnapshot::DeliveryCell& c) {
                        c = {};
                        if (d.size() >= 0x3C && load<uint16_t>(d, 0x30) != 0)
                        {
                            c.itemId = load<uint16_t>(d, 0x30);
                            c.count  = load<uint32_t>(d, 0x38);
                            c.stat   = load<uint32_t>(d, 0x10);
                            c.name   = printable(loadString(d, 0x14, 16));
                        }
                    };
                    // C2S 0x04D PBX {Command, BoxNo, PostWorkNo, ItemWorkNo, i32 ItemStacks, Result, ResParam1-3, name[16]}.
                    auto ask = [&](uint8_t command, int8_t boxNo, int8_t post = -1) {
                        std::vector<std::byte> p(0x1C);
                        store<uint8_t>(p, 0, command);
                        store<int8_t>(p, 1, boxNo);
                        store<int8_t>(p, 2, post);
                        store<int8_t>(p, 3, -1);
                        store<int32_t>(p, 4, -1);
                        pendingRaw_.emplace_back(0x04D, std::move(p));
                    };
                    const bool ok   = result == 1;
                    const bool cell = slot >= 0 && slot < 8;
                    switch (cmd)
                    {
                        case 0x0D: // DeliOpen (outgoing) / PostOpen (incoming): list the cells, count what is new
                        case 0x0E:
                            if (ok)
                            {
                                b.open = box;
                                b.cells = {};
                                ask(0x01, box);
                                ask(0x05, box);
                            }
                            break;
                        case 0x0F: // PostClose
                            b       = WorldSnapshot::DeliveryBox{ .serial = b.serial };
                            break;
                        case 0x01: // Work: one cell
                            if (cell && ok)
                            {
                                cellFromPacket(b.cells[static_cast<std::size_t>(slot)]);
                            }
                            break;
                        case 0x06: // Recv: a new item came into a cell; count again
                            if (cell && ok)
                            {
                                cellFromPacket(b.cells[static_cast<std::size_t>(slot)]);
                                ask(0x05, 1);
                            }
                            break;
                        case 0x05: // Check: items waiting (incoming) or delivered (outgoing: clear them, one Confirm each)
                            if (ok)
                            {
                                b.waiting = static_cast<uint8_t>(std::max<int8_t>(0, load<int8_t>(d, box == 1 ? 0x0E : 0x0F)));
                                for (uint8_t k = 0; box == 2 && k < b.waiting; ++k)
                                {
                                    ask(0x07, -1);
                                }
                            }
                            break;
                        case 0x02: // Set: an item is in an outgoing cell; Send it
                            if (cell && ok)
                            {
                                cellFromPacket(b.cells[static_cast<std::size_t>(slot)]);
                                ask(0x03, box, slot);
                            }
                            break;
                        case 0x03: // Send
                        case 0x04: // Cancel
                            if (cell && ok)
                            {
                                cellFromPacket(b.cells[static_cast<std::size_t>(slot)]);
                            }
                            break;
                        case 0x07: // Confirm: a delivered item left the outgoing box
                            if (cell && ok)
                            {
                                const auto& c    = b.cells[static_cast<std::size_t>(slot)];
                                const auto  item = cfg_.text && d.size() >= 0x32 ? cfg_.text->dialogName(0x23, load<uint16_t>(d, 0x30)) : std::string();
                                world_.chat.push_back({ 0xFF, "", fmt::format("Delivered: {} to {}.", item.empty() ? std::string("an item") : item, c.name) });
                                b.cells[static_cast<std::size_t>(slot)] = {};
                                b.waiting = static_cast<uint8_t>(b.waiting > 0 ? b.waiting - 1 : 0);
                            }
                            break;
                        case 0x0A: // Get: taken into the inventory
                        case 0x0B: // Clear: back to the inventory
                            if (cell && ok)
                            {
                                b.cells[static_cast<std::size_t>(slot)] = {};
                            }
                            break;
                        case 0x0C: // Query: Result 1 the name exists (ResParam1 1 = one of our own characters), 0xFB no such name
                            if (result == 1 || result == -5)
                            {
                                b.nameOk = result == 1 ? 1 : 0;
                            }
                            break;
                        default: break;
                    }
                    break;
                }
                case 0x0B4: // CONFIG: SAVE_CONF @4 (flags, MessageFilter, MessageFilter2, PvpFlg, AreaCode)
                    if (d.size() >= 0x10)
                    {
                        for (std::size_t i = 0; i < 12; ++i)
                        {
                            world_.saveConf[i] = load<uint8_t>(d, 4 + i);
                        }
                        world_.saveConfValid = true;
                    }
                    break;
                case 0x0CC: // LINKSHELL_MESSAGE: bits @5 (linkshell index bits 6-7), sMessage[128] @8, modifier[16] @0x8C,
                            // opType @0x9C (1 post, 2 delete / view, 3 result)
                {
                    if (d.size() < 0x9E)
                    {
                        break;
                    }
                    const auto ls  = static_cast<std::size_t>((load<uint8_t>(d, 5) >> 6) & 1);
                    const auto op  = load<uint16_t>(d, 0x9C);
                    auto       msg = printable(loadString(d, 8, 128));
                    if (op == 1 || op == 2)
                    {
                        const bool changed = world_.lsMessage[ls].first != msg;
                        world_.lsMessage[ls] = { msg, printable(loadString(d, 0x8C, 16)) };
                        if (changed && !msg.empty())
                        {
                            world_.chat.push_back({ static_cast<uint8_t>(ls ? 27 : 5), "", fmt::format("[Linkshell{}] {}", ls ? "2" : "", msg) });
                        }
                    }
                    break;
                }
                case 0x078: // SWITCH_START: UniqueNo @4, AllNum @8, ActIndex @0xC, sName[15] @0xE, Kind @0x1D, Str @0x1E
                case 0x079: // SWITCH_PROC: AllNum @4, VoteNumTbl[9] @8, Kind @0x1A, State @0x1B (2 = closed),
                            // QuestionNum @0x1C, sPropName[15] @0x1D, Str @0x2C
                {
                    const bool start = sp.id == 0x078;
                    if (d.size() < (start ? 0x1Fu : 0x2Du))
                    {
                        break;
                    }
                    const uint8_t kind = load<uint8_t>(d, start ? 0x1D : 0x1A);
                    // Proposal scope -> chat colour: party 1, linkshell 2 / 3, say 5, shout 6.
                    const uint8_t chat = kind == 1 ? 4 : kind == 2 ? 5 : kind == 3 ? 27 : kind == 6 ? 1 : 0;
                    const auto    name = printable(loadString(d, start ? 0x0E : 0x1D, 15));
                    const auto    body = loadString(d, start ? 0x1E : 0x2C, d.size() - (start ? 0x1E : 0x2C));
                    auto lines = [&](const std::string& head) {
                        world_.chat.push_back({ chat, "", head });
                        std::size_t from = 0;
                        while (from < body.size())
                        {
                            const auto to = std::min(body.find('\n', from), body.size());
                            world_.chat.push_back({ chat, "", "  " + printable(body.substr(from, to - from)) });
                            from = to + 1;
                        }
                    };
                    if (start)
                    {
                        world_.lastProposer = name;
                        world_.lastProposal = body;
                        lines(fmt::format("{} proposes (/vote {} <number>):", name, name));
                    }
                    else if (load<uint8_t>(d, 0x1B) == 2)
                    {
                        lines(fmt::format("Results of {}'s proposal:", name));
                    }
                    break;
                }
                case 0x04D: // FRAGMENTS: Command @4, Result, value1 @6 (1 = server message), value2, i32 timestamp @8,
                            // size_total @0x0C, offset @0x10, data_size @0x14, data @0x18
                {
                    if (d.size() < 0x18 || load<uint8_t>(d, 6) != 1)
                    {
                        break;
                    }
                    const auto total  = load<int32_t>(d, 0x0C);
                    const auto offset = load<int32_t>(d, 0x10);
                    const auto n      = std::min<int32_t>(load<int32_t>(d, 0x14), static_cast<int32_t>(d.size()) - 0x18);
                    if (total <= 0 || offset < 0 || n <= 0 || offset + n > total || total > 0x10000)
                    {
                        break;
                    }
                    servmes_.resize(static_cast<std::size_t>(total));
                    std::memcpy(servmes_.data() + offset, d.data() + 0x18, static_cast<std::size_t>(n));
                    if (offset + n < total)
                    {
                        pendingRaw_.emplace_back(0x04B, servmesRequest(2, load<int32_t>(d, 8), offset + n));
                        break;
                    }
                    std::string text(servmes_.c_str()); // NUL-terminated
                    servmes_.clear();
                    std::erase(text, '\r');
                    std::size_t from = 0;
                    while (from <= text.size())
                    {
                        auto to = text.find('\n', from);
                        if (to == std::string::npos)
                        {
                            to = text.size();
                        }
                        if (auto l = printable(text.substr(from, to - from)); !l.empty())
                        {
                            world_.chat.push_back({ 0xFF, "", std::move(l) });
                        }
                        from = to + 1;
                    }
                    break;
                }
                case 0x027: // TALKNUMWORK2: UniqueNo @4, ActIndex @8, MesNum @0xA, Type @0xC, Num1[4] @0x10,
                            // String1[32] @0x20, String2[16] @0x40 (fishing catches, "<name> caught <item>!")
                {
                    if (d.size() < 0x50)
                    {
                        break;
                    }
                    std::array<int32_t, 8> nums{};
                    for (std::size_t k = 0; k < 4; ++k)
                    {
                        nums[k] = load<int32_t>(d, 0x10 + 4 * k);
                    }
                    auto str = [&](std::size_t at, std::size_t n) {
                        const auto* p = reinterpret_cast<const char*>(d.data() + at);
                        return std::string(p, strnlen(p, n));
                    };
                    npcSays(load<uint32_t>(d, 4), load<uint16_t>(d, 0x0A), nums, { str(0x20, 32), str(0x40, 16) });
                    break;
                }
                case 0x036: // TALKNUM: UniqueNo @4, ActIndex @8, MesNum @0xA (bit 15 = no speaker name), Type @0xC
                    npcSays(load<uint32_t>(d, 4), load<uint16_t>(d, 0x0A), {});
                    break;
                case 0x02A: // TALKNUMWORK: UniqueNo @4, num[4] @8, ActIndex @0x18, MesNum @0x1A, Type @0x1C
                {
                    std::array<int32_t, 8> nums{};
                    for (std::size_t k = 0; k < 4; ++k)
                    {
                        nums[k] = load<int32_t>(d, 8 + 4 * k);
                    }
                    npcSays(load<uint32_t>(d, 4), load<uint16_t>(d, 0x1A), nums);
                    break;
                }
                case 0x056: // MISSION: u32 Data[8] @4, u16 Port @0x24 (which log, current or completed)
                {
                    if (d.size() < 0x26)
                    {
                        break;
                    }
                    const uint16_t port = load<uint16_t>(d, 0x24);
                    std::array<uint32_t, 8> data{};
                    for (std::size_t i = 0; i < 8; ++i)
                    {
                        data[i] = load<uint32_t>(d, 4 + 4 * i);
                    }
                    if (port == 0xFFFF)
                    {
                        world_.nation        = data[0];
                        world_.nationMission = data[1];
                        world_.zilartMission = data[2];
                    }
                    else if (port >= 0x50 && port <= 0x88 && port % 8 == 0)
                    {
                        world_.questCurrent[(port - 0x50) / 8] = data;
                    }
                    else if (port >= 0x90 && port <= 0xC8 && port % 8 == 0)
                    {
                        world_.questDone[(port - 0x90) / 8] = data;
                    }
                    else if (port == 0xD0)
                    {
                        world_.missionsDone = data;
                    }
                    break;
                }
                case 0x06F: // COMBINE_ANS: Result u8 @4, Grade s8 @5, Count u8 @6, ItemNo u16 @8
                    if (d.size() >= 0x0A)
                    {
                        world_.synth = { load<uint8_t>(d, 4), static_cast<int8_t>(load<uint8_t>(d, 5)), load<uint8_t>(d, 6), load<uint16_t>(d, 8) };
                        ++world_.synthSerial;
                    }
                    break;
                case 0x055: // SCENARIOITEM: GetItemFlag u32[16] @4, LookItemFlag u32[16] @0x44, TableIndex u16 @0x84;
                            // key item id = TableIndex * 512 + bit
                {
                    if (d.size() >= 0x86)
                    {
                        const uint32_t table = load<uint16_t>(d, 0x84);
                        const uint32_t lo = table * 512, hi = lo + 512;
                        auto&          ki = world_.keyItems;
                        std::erase_if(ki, [&](uint16_t id) { return id >= lo && id < hi; });
                        for (uint32_t bit = 0; bit < 512; ++bit)
                        {
                            if (load<uint32_t>(d, 4 + 4 * (bit / 32)) & (1u << (bit % 32)))
                            {
                                ki.push_back(static_cast<uint16_t>(lo + bit));
                            }
                        }
                        std::sort(ki.begin(), ki.end());
                    }
                    break;
                }
                case 0x0F9: // GP_SERV_COMMAND_RES: UniqueNo @4, ActIndex @8, type @0x0A (0 home point, 1 raise, 2 tractor)
                {
                    if (d.size() >= 0x0C && load<uint32_t>(d, 4) == world_.charId)
                    {
                        world_.reviveOffer = static_cast<uint8_t>(load<uint16_t>(d, 0x0A));
                        ++world_.reviveSerial;
                        spdlog::info("revive offer: type {}", world_.reviveOffer);
                    }
                    break;
                }
                case 0x041: // BLACK_LIST: List[12] {u32 ID, Name[16]} @4, Stat @0xF4 (1 = reset, 2 = last), Num @0xF5
                {
                    if (d.size() < 0xF6)
                    {
                        break;
                    }
                    if (load<uint8_t>(d, 0xF4) & 0x01)
                    {
                        world_.blacklist.clear();
                    }
                    const auto n = std::min<int>(load<int8_t>(d, 0xF5), 12);
                    for (int k = 0; k < n; ++k)
                    {
                        world_.blacklist.emplace_back(load<uint32_t>(d, 4 + 20 * k), printable(loadString(d, 8 + 20 * k, 16)));
                    }
                    break;
                }
                case 0x042: // BLACK_EDIT: {u32 ID, Name[16]} @4, Mode @0x18 (0 added, 1 removed, 2 failed)
                {
                    if (d.size() < 0x19)
                    {
                        break;
                    }
                    const auto mode = load<int8_t>(d, 0x18);
                    const auto name = printable(loadString(d, 8, 16));
                    std::erase_if(world_.blacklist, [&](const auto& b) { return strcasecmp(b.second.c_str(), name.c_str()) == 0; });
                    if (mode == 0)
                    {
                        world_.blacklist.emplace_back(load<uint32_t>(d, 4), name);
                    }
                    world_.blacklistEdit = mode;
                    ++world_.blacklistSerial;
                    break;
                }
                case 0x017: // chat: kind @4, sender @8 (15), message @0x17
                {
                    if (const auto from = printable(loadString(d, 8, 15));
                        std::ranges::any_of(world_.blacklist, [&](const auto& b) { return strcasecmp(b.second.c_str(), from.c_str()) == 0; }))
                    {
                        break; // blacklisted
                    }
                    auto text = loadString(d, 0x17, d.size() - 0x17);
                    if (cfg_.text)
                    {
                        text = cfg_.text->expandAutoTranslateMarked(text); // FD .. FD phrases, between the gaiji brackets
                    }
                    if (cfg_.chatFilter) // Config chat filter (ChatFilter; CODE 0x100e385c)
                    {
                        const auto verdict = cfg_.chatFilter->check(load<uint8_t>(d, 4), printable(loadString(d, 8, 15)), dat::AutoTranslate::plainMarks(printable(text)));
                        if (verdict == ChatFilter::Verdict::Drop)
                        {
                            break;
                        }
                        if (verdict == ChatFilter::Verdict::Deleted)
                        {
                            text = cfg_.deletedText;
                        }
                        text = cfg_.chatFilter->censor(text); // profanity filter
                    }
                    const uint8_t kind = load<uint8_t>(d, 4);
                    if (kind == 6 || kind == 7)
                    {
                        // System messages (LSB's MESSAGE_SYSTEM_1 / _2, e.g. "Fishing is currently disabled"): the game
                        // prints "----== SystemMessage ==----\n%s\n" with no sender (CODE 0x100e38d8 jump table: modes 6
                        // and 7 -> 0x100e3bd8, format 0x10361db0).
                        world_.chat.push_back({ kind, {}, "----== SystemMessage ==----" });
                        world_.chat.push_back({ kind, {}, printable(std::move(text)) });
                    }
                    else
                    {
                        world_.chat.push_back({ kind, printable(loadString(d, 8, 15)), printable(std::move(text)) });
                    }
                    if (world_.chat.back().kind == 3 && !world_.chat.back().from.empty()) // MESSAGE_TELL
                    {
                        ++world_.tellsReceived;
                        world_.lastTellFrom = world_.chat.back().from;
                    }
                    if (world_.chat.size() > kMaxChat)
                    {
                        world_.chat.erase(world_.chat.begin());
                    }
                    break;
                }
                default:
                    break;
            }
        });

        setStatus("zoning in");
        map.zoneIn();

        while (!stop_)
        {
            // /logout and /shutdown (C2S 0x0E7) ran their countdown: the server's 0x00B ends the session.
            if (map.loggedOut())
            {
                break;
            }
            // A map server that stops answering (LSB's watchdog kills and restarts it) never drops our session
            // explicitly; after 45 s of silence give up, as the game's "lost connection" does (time INFERRED).
            if (std::chrono::steady_clock::now() - map.lastReceive() > std::chrono::seconds(45))
            {
                spdlog::error("session: no reply from the map server for 45 s; disconnected");
                setStatus("lost connection to the map server");
                connectionLost_ = true;
                finished_       = true;
                return;
            }
            {
                std::lock_guard lock(mutex_);
                if (playerDirty_)
                {
                    map.setPosition({ playerPos_.x, playerPos_.y, playerPos_.z, playerDir_, playerTarget_ });
                    playerDirty_ = false;
                }
                // Events are run by the client's script runner, which queues their end / updates here.
                // C2S 0x05B EVENTEND: UniqueNo @+4, EndPara @+8, ActIndex @+0xC, Mode @+0xE (0 = End, 1 = update),
                // EventNum @+0x10, EventPara @+0x12.
                while (!pendingEvents_.empty())
                {
                    const auto ev = pendingEvents_.front();
                    pendingEvents_.pop_front();
                    if (ev.pos)
                    {
                        // C2S 0x05C EVENTENDXZY: x, y, z @+4, UniqueNo @+0x10, EndPara @+0x14, EventNum @+0x18,
                        // EventPara @+0x1A, ActIndex @+0x1C, Mode @+0x1E, dir @+0x1F (CODE 0x100a0510).
                        std::array<std::byte, 28> q{};
                        store<float>(q, 0x00, ev.pos->x);
                        store<float>(q, 0x04, ev.pos->y);
                        store<float>(q, 0x08, ev.pos->z);
                        store<uint32_t>(q, 0x0C, ev.uniqueNo);
                        store<uint32_t>(q, 0x10, ev.endPara);
                        store<uint16_t>(q, 0x14, ev.eventNum);
                        store<uint16_t>(q, 0x16, ev.eventPara);
                        store<uint16_t>(q, 0x18, ev.actIndex);
                        store<uint8_t>(q, 0x1A, static_cast<uint8_t>(ev.mode));
                        store<uint8_t>(q, 0x1B, ev.dir);
                        map.queue(0x05C, q);
                        continue;
                    }
                    std::array<std::byte, 16> p{};
                    store<uint32_t>(p, 0x00, ev.uniqueNo);
                    store<uint32_t>(p, 0x04, ev.endPara);
                    store<uint16_t>(p, 0x08, ev.actIndex);
                    store<uint16_t>(p, 0x0A, ev.mode);
                    store<uint16_t>(p, 0x0C, ev.eventNum);
                    store<uint16_t>(p, 0x0E, ev.eventPara);
                    map.queue(0x05B, p);

                }
                // C2S 0x01A ACTION: UniqueNo @+4, ActIndex @+8, ActionID @+0xA, ActionBuf u32[4] @+0xC.
                while (!pendingActions_.empty())
                {
                    const auto a = pendingActions_.front();
                    pendingActions_.pop_front();
                    if (netTrace)
                    {
                        spdlog::info("net: action 0x{:x} reached the network thread in {} ms", a.actionId,
                                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - a.at).count());
                    }
                    std::array<std::byte, 24> p{};
                    store<uint32_t>(p, 0x00, a.targetId);
                    store<uint16_t>(p, 0x04, a.index);
                    store<uint16_t>(p, 0x06, a.actionId);
                    store<uint32_t>(p, 0x08, a.param);
                    if (a.actionId == 9) // job ability: remember it to learn its recast group
                    {
                        lastAbility_ = { static_cast<uint16_t>(a.param), std::chrono::steady_clock::now() };
                    }
                    map.queue(0x01A, p);
                }
                // C2S 0x037 ITEM_USE: UniqueNo @+4, ItemNum @+8, ActIndex @+0xC, PropertyItemIndex @+0xE, Category @+0x10.
                while (!pendingItemUses_.empty())
                {
                    const auto u = pendingItemUses_.front();
                    pendingItemUses_.pop_front();
                    std::array<std::byte, 16> p{};
                    store<uint32_t>(p, 0x00, u.targetId);
                    store<uint32_t>(p, 0x04, 0);
                    store<uint16_t>(p, 0x08, u.targetIndex);
                    store<uint8_t>(p, 0x0A, u.slot);
                    store<uint32_t>(p, 0x0C, u.container);
                    map.queue(0x037, p);
                }
                while (!pendingRaw_.empty())
                {
                    auto [id, payload] = std::move(pendingRaw_.front());
                    pendingRaw_.pop_front();
                    map.queue(id, payload);
                }
                while (!pendingInjectedOut_.empty())
                {
                    auto [id, payload] = std::move(pendingInjectedOut_.front());
                    pendingInjectedOut_.pop_front();
                    map.queue(id, payload, true);
                }
                while (!pendingInjectedIn_.empty())
                {
                    map.inject(std::move(pendingInjectedIn_.front()));
                    pendingInjectedIn_.pop_front();
                }
                while (!fakeFish_.empty() && fakeFish_.front().due <= std::chrono::steady_clock::now())
                {
                    auto ff = std::move(fakeFish_.front());
                    fakeFish_.pop_front();
                    if (!ff.packet.empty())
                    {
                        map.inject(std::move(ff.packet));
                    }
                    if (!ff.note.empty())
                    {
                        world_.chat.push_back({ 0xFF, "", std::move(ff.note) });
                    }
                }
                while (!pendingEquips_.empty())
                {
                    const auto e = pendingEquips_.front();
                    pendingEquips_.pop_front();
                    std::array<std::byte, 4> p{};
                    store<uint8_t>(p, 0, e[0]); // PropertyItemIndex
                    store<uint8_t>(p, 1, e[1]); // EquipKind
                    store<uint8_t>(p, 2, e[2]); // Category (container)
                    map.queue(0x050, p);
                }
                if (pendingZoneLine_)
                {
                    // 0x05E MAPRECT: RectID @+4, x/height/z @+8, ActIndex @+0x14, MyRoomExitBit/Mode @+0x16/0x17.
                    std::array<std::byte, 20> p{};
                    store<uint32_t>(p, 0x00, *pendingZoneLine_);
                    store<float>(p, 0x04, playerPos_.x);
                    store<float>(p, 0x08, playerPos_.y);
                    store<float>(p, 0x0C, playerPos_.z);
                    store<uint16_t>(p, 0x10, map.actIndex());
                    store<uint8_t>(p, 0x12, pendingExitBit_);
                    store<uint8_t>(p, 0x13, pendingExitMode_);
                    map.queue(0x05E, p);
                    map.setPositionUpdates(false);
                    pendingZoneLine_.reset();
                    world_.status = "entering zone line";
                }
                while (!outgoingChat_.empty())
                {
                    const auto [text, kind] = outgoingChat_.front();
                    outgoingChat_.pop_front();
                    map.say(text, kind);
                    // The server does not echo our own say / shout back, nor party and linkshell lines (LSB pushes
                    // those to the other members only: PushPacket(senderId, ...)).
                    if (kind == 0 || kind == 1 || kind == 4 || kind == 5 || kind == 27)
                    {
                        world_.chat.push_back({ kind, world_.charName, printable(cfg_.text ? cfg_.text->expandAutoTranslateMarked(text) : text) });
                    }
                }
            }
            // A short wait so a command the interface queues is picked up within ~10 ms and sent at once (#97); the
            // map client still paces its own sends.
            map.tick(std::chrono::milliseconds(10));
            // Every 3 s: the bytes of that window as per-second rates (CODE 0x100fca10: count x 1000 / 3000 ms).
            if (const auto now = std::chrono::steady_clock::now(); now - netAt >= std::chrono::seconds(3))
            {
                const auto ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - netAt).count());
                std::lock_guard lock(mutex_);
                world_.netSendRate = static_cast<uint32_t>((map.bytesSent() - netSent) * 1000 / std::max<uint64_t>(ms, 1));
                world_.netRecvRate = static_cast<uint32_t>((map.bytesReceived() - netRecv) * 1000 / std::max<uint64_t>(ms, 1));
                netAt   = now;
                netSent = map.bytesSent();
                netRecv = map.bytesReceived();
            }
        }

        if (map.loggedOut())
        {
            setStatus("logged out");
            loggedOutByServer_ = true;
            finished_          = true;
            return;
        }
        {
            // Disengage first: the server cancels the logout rest while we are in combat.
            std::lock_guard lock(mutex_);
            if (world_.engaged())
            {
                std::array<std::byte, 24> p{};
                store<uint16_t>(p, 0x06, 4); // AttackOff
                map.queue(0x01A, p);
            }
            // Resting (/heal): LSB refuses the logout request ("AbnormalStatus"), so stand up first (0x0E8 Mode 2 = off).
            if (world_.animation == 33)
            {
                std::array<std::byte, 4> p{};
                store<uint32_t>(p, 0, 2);
                map.queue(0x0E8, p);
            }
        }
        setStatus("logging out");
        setStatus(map.logout(std::chrono::seconds(40)) ? "logged out" : "logout timed out (the server will drop the session after ~60 s)");
    }
    catch (const std::exception& e)
    {
        setStatus(std::string("error: ") + e.what());
    }
    finished_ = true;
}

} // namespace xi::client
