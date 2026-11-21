// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "client/entity_names.h"
#include "client/name_icons.h"
#include "client/chat_filter.h"
#include "client/combat.h"
#include "client/guild_shop.h"
#include "client/skills.h"
#include "dat/messages.h"
#include "lobby/lobby.h"

#include <glm/glm.hpp>

#include <array>
#include <span>
#include <functional>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xi::client
{

// Positions are in game coordinates (LSB/server space: Y is height, pointing down).
// Appearance as the server sends it (interpreted by the model loader, docs/dat-format.md §10).
//  - PCs (S2C 0x00D GrapIDTbl[9] @+0x48) and our own character (0x00A @+0x44, 0x051): kind = 0xFFFF (equipped),
//    ids = { face|race, head, body, hands, legs, feet, main, sub, ranged }.
//  - NPCs/mobs (0x00E look_t @+0x30): kind = look.size (model type), ids[0] = model id or face|race, ids[1..8] = gear
//    when the kind is "equipped".
struct Look
{
    uint16_t                kind = 0;
    std::array<uint16_t, 9> ids{};
    bool                    valid = false;
};

// S2C 0x00E look kind 6 (CODE 0x1009ccd2): actor type 7 (+0xee), whose model is look slot 9 (+0x10e = id | 0x9000);
// slot 9's low 12 bits name a standard NPC model (0x100d0ce8 -> 0x100c50f0(0, id & 0xFFF)). LSB sends Ghrah
// (Awghrah ...) and automatons this way. INFERRED: an automaton's head / frame on top of it are not composed.
constexpr uint16_t kLookSlot9 = 6;

// The NPC model id a kind 0 or kind 6 look draws, 0 = none.
inline uint16_t npcModel(const Look& l)
{
    if (l.kind == 0)
    {
        return l.ids[0];
    }
    return l.kind == kLookSlot9 ? static_cast<uint16_t>(l.ids[0] & 0xFFF) : uint16_t{ 0 };
}

// Flags1 (0x00D / 0x00E +0x20, u32) bit 19: TargetOffFlag. LSB writes an NPC's entity_flags as a u32 at +0x21, so its
// bit 11 (0x800) lands here.
inline bool flags1TargetOff(uint32_t flags1)
{
    return ((flags1 >> 19) & 1u) != 0;
}

// Standard (kind 0) NPC models the game never names: the 0x00E handler sets the actor's no-name bit (+0x128 bit 3,
// read by the name pass 0x100d0440 through 0x100844e0) for these model ranges (CODE 0x1009c96c..0x1009ca2f). Model 50
// (0x32) is the empty model LSB gives its helper NPCs (warp01, DIRECTOR ...).
inline bool modelHidesName(uint16_t model)
{
    constexpr std::pair<uint16_t, uint16_t> kRanges[] = { { 0x032, 0x03B }, { 0x974, 0x978 }, { 0x737, 0x746 }, { 0x9BA, 0x9BE },
                                                          { 0x3C0, 0x3C9 }, { 0x32E, 0x331 }, { 0x979, 0x97D }, { 0x9BF, 0x9C3 } };
    for (const auto& [lo, hi] : kRanges)
    {
        if (model >= lo && model <= hi)
        {
            return true;
        }
    }
    return false;
}

struct Entity
{
    uint32_t  id    = 0;
    uint16_t  index = 0;
    std::string name;
    std::string renameKey; // a dynamic entity's spawn name (+0x34), what a server rename list names it by
    glm::vec3 pos{ 0.0f };
    uint8_t   dir   = 0;
    uint8_t   hpp   = 0;
    uint8_t   animation = 0; // server animation (0x00D/0x00E +0x1F): 0 idle, 1 engaged, 2/3 dead; doors 8 open, 9 closed
    // Riding (animation 5 chocobo, 85 mount; chocobo_re.md §3a): 0x00D +0x20 bits 5..7 (1 rented, 2..4 own / Noble),
    // +0x34 / +0x38 the own chocobo's looks, +0x44 >> 4 the mount.
    uint8_t   chocoboIndex = 0;
    std::array<uint32_t, 2> chocoboLook{};
    uint8_t   mountIndex = 0;
    float     fitHeight = 0; // client-made actors (Mog House statues): draw scaled to this height (0 = as modelled)
    uint32_t  doorId    = 0; // look kinds 2-4 (door, lift, ship): 0x00E +0x34, a door's fourcc (`_6w0`)
    // Lifts (kind 3) and ships (kind 4): +0x38 Time (earth seconds since the Vana'diel epoch, 1009810800) and +0x3C travel seconds;
    // a move ends at Time + travel (CODE 0x1009cb06; LSB TransportTimestamp).
    uint32_t  liftTime   = 0;
    uint16_t  liftTravel = 0;
    uint32_t  swings    = 0; // melee rounds performed (S2C 0x028 type 1); the renderer plays a swing per change
    uint32_t  lastHitBy = 0; // who hit last (front / back flinch; the flinch itself comes from the effect, op 0x25)
    uint16_t  castSpell = 0; // spell being cast (0x028 type 8), 0 = not casting
    uint32_t  castsDone = 0; // spells finished (0x028 type 4); the renderer plays the release per change
    uint16_t  wsAnimation = 0; // last weapon skill's animation id (0x028 type 3 result animation)
    uint32_t  weaponSkills = 0; // weapon skills performed; the renderer plays the motion per change
    uint8_t   emote  = 0; // last emote performed (S2C 0x05A, LSB enums/emote.h)
    uint32_t  emotes = 0; // emotes performed; the renderer plays the motion per change
    bool      isPc  = false;
    bool      isMob = false;   // 0x00E +0x20 bit 0 (MonsterFlag): a monster (not an NPC)
    bool      nameHidden = false; // 0x00E +0x2B (namevis) bit 0x08: no overhead name
    // Flags1 (0x00D / 0x00E +0x20) bit 19, TargetOffFlag: the handler copies it to the actor's +0x128 bit 4 (CODE
    // 0x1009bfd7) and the target filter 0x10083360 rejects such actors (0x100844b0). LSB: npc entity_flags 0x800.
    bool      targetOff = false;
    // No overhead name: namevis, or a model the game never names (modelHidesName).
    [[nodiscard]] bool labelHidden() const
    {
        return nameHidden || (!isPc && look.valid && look.kind == 0 && modelHidesName(look.ids[0]));
    }
    // 0x00E +0x20, sent with every update (LSB entity_update.cpp; values xi.status): 0 normal, 1 update, 2 disappear,
    // 3 invisible, 6 cutscene only.
    uint8_t   status = 0;
    // 0x00E +0x25 (Flags2.g, LSB: the hitbox size x 10), in yalms; players 0 (LSB sends a PC's in 0x00D, 0 unless in
    // Monstrosity). The range test subtracts both sizes (CODE 0x10150540, actor +0x208).
    float     hitbox = 0.0f;
    // Not drawn, named or targetable: +0x20 bit 1 (HideFlag; LSB statuses 2 disappear, 3 invisible, 6 cutscene only).
    // Cutscenes show cutscene-only actors (INFERRED).
    [[nodiscard]] bool hiddenByStatus(bool inCutscene) const
    {
        return (status & 0x02) != 0 && !(status == 6 && inCutscene);
    }
    // PCs (0x00D, the General block): a linkshell worn (+0x20 bit 17, LinkShellFlag) and its colour (+0x24 r, g, b:
    // LSB sends the 4-bit linkshell colour as c << 4 | 15).
    bool                   linkshell = false;
    std::array<uint8_t, 3> lsColor{};
    NameFlags              nameFlags; // PCs: the name label's icons (0x00D General block)
    uint16_t  targetIndex = 0;    // 0x00D / 0x00E +0x18 bits 17..31 (facetarget): the entity index it targets (/assist)
    uint32_t  claimId = 0;     // 0x00E +0x2C (claim bit 0x02): who has claimed this monster
    // Stand at the server's height, not on the floor (client::actorGround): 0x00D / 0x00E +0x18 bit 15 (GroundFlag ->
    // entity +0x120 bit 20, CODE 0x1009bb2d), or a PC's HackMove (0x00D +0x20 bit 27 -> +0x128 bit 14, CODE 0x1009a456).
    bool      groundFlag = false;
    bool      hackMove   = false;
    [[nodiscard]] bool keepsHeight() const
    {
        return groundFlag || hackMove;
    }
    Look      look;
};

// Chat kind used for combat-log lines (not a server CHAT_MESSAGE_TYPE).
inline constexpr uint8_t kCombatChat = 0xFE;
// Combat lines where we or our party act / are acted on (log colours).
inline constexpr uint8_t kCombatOwn  = 0xFC;
inline constexpr uint8_t kCombatHurt = 0xFB;
// Chat kind for NPC speech (S2C 0x036 / 0x02A).
inline constexpr uint8_t kNpcChat = 0xFD;

// Every chat line gets a serial when it is made (copies keep it), so the log and Ashita's text_in can name a line.
inline uint64_t nextChatSerial()
{
    static std::atomic<uint64_t> n{ 0 };
    return ++n;
}

struct ChatLine
{
    uint8_t     kind = 0; // CHAT_MESSAGE_TYPE: 0 say, 1 shout, 4 party, 8 emote, 26 yell...
    std::string from;
    std::string text;
    std::chrono::system_clock::time_point at = std::chrono::system_clock::now(); // when it arrived (/timestamp)
    // Non-zero: a battle result's message, held until its effect shows the damage (effect op 0x2B):
    // action serial << 16 | (flat result index + 1). See FxAction::tag.
    uint64_t hold = 0;
    // A battle line's colour config id (35..49, Config Font Colors) from its message and who acted on whom
    // (battle_mode_re.md; CODE 0x1009e630); 0 = by kind.
    uint8_t colour = 0;
    uint16_t message = 0; // a battle line's message id (Config damage display "screen only" leaves pop-up ones out)
    uint64_t serial  = nextChatSerial();
};

// An item in one of our containers (S2C 0x01F / 0x020 / 0x01E).
struct InventoryItem
{
    uint16_t itemId    = 0;
    uint32_t count     = 0;
    uint8_t  container = 0; // 0 inventory, 1 safe, 8 wardrobe, ...
    uint8_t  slot      = 0; // index within the container (0 = gil in the inventory)
    std::array<uint8_t, 24> attr{}; // S2C 0x020 +0x11 exdata (furniture placement, signatures ...)
    uint8_t  lockFlags = 0; // LockFlg (0x01F +0x0C, 0x020 +0x10; LSB ItemLockFlg): 5 equipped, 0x13 linkshell / bazaar ...
    uint32_t price     = 0; // 0x020 +0x08: the bazaar price
};

// Own character stats (S2C 0x061 CLISTATUS).
struct CharStats
{
    bool                   valid = false;
    int32_t                hpMax = 0, mpMax = 0;
    int16_t                exp = 0, expNext = 0;
    std::array<uint16_t, 7> base{};  // STR DEX VIT AGI INT MND CHR
    std::array<int16_t, 7>  bonus{}; // from gear and effects
    int16_t                attack = 0, defense = 0;
    std::array<int16_t, 8>  elementDefense{}; // fire, ice, wind, earth, lightning, water, light, dark
    uint16_t               title = 0, rank = 0;
    uint16_t               rankBar = 0;   // rank points toward the next rank, 0..4096 (the Profile bar; CODE 0x101479f0)
    uint16_t               bindZone = 0;  // home point zone (BindZoneNo)
    uint8_t                nation = 0;
    uint8_t                suLevel = 0;   // su_lv: the "Su" level on the Status window
    uint8_t                itemLevel = 0; // ilvl: the item level above 99 (0 = none; the Status window shows 99 + it)
    uint8_t                unityLeader = 0; // S2C 0x061 unity_info Faction: 1..11, 0 = none
};

// A party or alliance member (S2C 0x0C8 membership, 0x0DD details, 0x0DF updates). Includes ourselves.
struct PartyMember
{
    uint32_t    id = 0;
    uint16_t    index = 0;
    std::string name;
    uint32_t    hp = 0, mp = 0, tp = 0;
    uint8_t     hpp = 0, mpp = 0;
    uint16_t    zone = 0;
    uint8_t     mainJob = 0, mainLevel = 0, subJob = 0, subLevel = 0;
    uint8_t     partyNo = 0;     // 0 = our party, 1-2 = other alliance parties
    bool        leader  = false;
    uint8_t     order   = 0;     // MemberNumber (0x0DD +0x1A)
    std::vector<uint16_t> buffs; // status effect ids (S2C 0x076; not for ourselves)
    uint8_t     flags = 0;       // the raw member flags (0x0C8 +6 / 0x0DD GAttr +0x14): party bits 0..1, 0x04 party leader,
                                 // 0x08 alliance leader, 0x10 / 0x20 quartermaster / sync
    // The map's Markers list: the position from S2C 0x0A0 (MAP_GROUP), the server's answer to C2S 0x0D2.
    bool        mapPosKnown = false;
    uint16_t    mapZone     = 0;
    glm::vec3   mapPos{ 0.0f };
};

struct WorldSnapshot
{
    std::string         status;
    bool                inZone = false;
    uint16_t            zone   = 0;
    // The zone model to load (S2C 0x00A, CODE 0x100fac20): MapNumber (+0x42, +0x1BC from 0x100), or in a Mog House
    // MyroomMapNumber (+0xAA); dat::zoneModelFileId gives its file (docs/protocol-map.md, Mog House).
    uint16_t            zoneModel  = 0;
    bool                inMogHouse = false; // LoginState (+0x80) 1 or 5
    uint8_t             mhFloor    = 0;     // (+0xA8 >> 1) & 3: 0 = 1F, 1 = 2F
    uint8_t             mhExitBit  = 0;     // +0xAE: the city whose exits are open (0 = none)
    uint16_t            mhMap      = 0;     // MyroomMapNumber
    uint32_t            mogMenuSerial = 0;  // bumps on S2C 0x02E (talking to the Moogle: open the Mog House menu)
    bool                mhExpansion = false; // S2C 0x067 kind 2 +0x27 bit 0 MogExpansionFlag: 2F unlocked (LSB char_sync.cpp)
    bool                mhOpen      = false; // S2C 0x067 kind 2 +0x24 MogHouseFlag: open to visitors (C2S 0x0CB kind 1 / 2)
    // Mannequins (S2C 0x026 ITEM_SUBCONTAINER, LSB 0x026_item_subcontainer.h): by container << 8 | slot, the looks of
    // the gear on them (race_hair, head .. range) and the race / pose.
    struct MannequinLook
    {
        std::array<uint16_t, 9> ids{};
        uint8_t                 race = 0, pose = 0;
    };
    std::map<uint16_t, MannequinLook> mannequins;
    // S2C 0x01B JOB_INFO: unlocked jobs (bit j = job j; bit 0 = support jobs) and levels by job id (+0x48, 24 jobs).
    uint32_t                 jobsUnlocked = 0;
    std::array<uint8_t, 24>  jobLevels{};
    // 0x01B +0x60 encumbrance: low 16 bits = equip slots the server blocks (LSB m_EquipBlock), high = stats lowered.
    uint32_t                 encumbrance = 0;
    // S2C 0x062 CLISTATUS2: skill values by skill id (client/skills.h); skillsValid once one arrived.
    SkillTable               skills{};
    bool                     skillsValid = false;
    uint32_t            charId = 0;
    uint16_t            actIndex = 0; // our entity index in this zone (S2C 0x00A +0x08)
    std::string         charName;
    uint32_t            teleportGeneration = 0; // bumps on zone-in and server-forced moves
    glm::vec3           serverPos{ 0.0f };      // valid together with teleportGeneration
    uint8_t             serverDir = 0;          // heading at zone-in / forced move
    std::vector<Entity> entities;
    std::vector<ChatLine> chat; // full log (bounded)
    std::string           lastTellFrom; // sender of the last tell received (the /reply shortcut)
    uint32_t tellsReceived = 0; // incoming tells (S2C 0x017 kind 3); the client plays se000040 per change
    // S2C 0x0F9 (GP_SERV_COMMAND_RES) for us: 0 home point, 1 raise, 2 tractor offered. reviveSerial counts them so the
    // interface opens one prompt per offer.
    std::vector<uint16_t> keyItems; // held key items (S2C 0x055), ascending ids
    // Delivery box (S2C 0x04B PBX_RESULT): the open box (0 none, 1 incoming, 2 outgoing), its 8 cells, the items
    // waiting to come in (incoming) or delivered (outgoing), and the latest result.
    struct DeliveryCell
    {
        uint16_t    itemId = 0;
        uint32_t    count  = 0;
        uint32_t    stat   = 0; // 1 set, 3 sent, 5 cancelled (outgoing); 7 received (incoming)
        std::string name;       // sender (incoming) or recipient (outgoing)
    };
    struct DeliveryBox
    {
        int8_t                      open    = 0;
        std::array<DeliveryCell, 8> cells{};
        uint8_t                     waiting = 0;
        uint8_t                     lastCommand = 0;
        int8_t                      lastResult  = 0;
        int8_t                      nameOk      = -1; // Query: 1 the name exists, 0 not, -1 not asked
        uint32_t                    serial      = 0;
    } dbox;
    // Auction house (S2C 0x04C AUC): opened by the counter (Command 2), our sale slots, and the latest result.
    struct AuctionSlot
    {
        uint16_t itemId = 0;
        uint32_t price  = 0;
        bool     stack  = false;
        uint8_t  stat   = 0; // Parcel.Stat: 3 on auction, 0 empty (LSB); sold ones show other values
    };
    struct Auction
    {
        uint32_t                   openSerial   = 0;
        std::array<AuctionSlot, 7> sales{};
        uint32_t                   fee          = 0; // AskCommit: the listing fee
        // Every result in order ({Command, Result}); resultSerial counts them all, `results` keeps the latest 16.
        std::deque<std::pair<uint8_t, uint8_t>> results;
        uint32_t                   resultSerial = 0;
    } auction;
    // S2C 0x058 ASSIST: the server moved our battle target (auto-target after a kill, /assist): the new target id.
    uint32_t autoTargetId     = 0;
    uint32_t autoTargetSerial = 0;
    // Recent battle actions (S2C 0x028), for effects: actionSerial counts all, `actions` keeps the latest 32.
    std::deque<BattleAction> actions;
    uint32_t                 actionSerial = 0;
    // The network window (CODE 0x10200ef0 / 0x100fca10): bytes per second sent (S) and received (R) over the last 3 s.
    uint32_t netSendRate = 0, netRecvRate = 0;
    // S2C 0x05B WPOS with an appear mode (+0x16: 3 POP -> `pop0`, 6 MATERIALIZE -> `pop1`), latest 16.
    struct PosEffect
    {
        uint32_t id   = 0;
        uint8_t  mode = 0;
    };
    std::deque<PosEffect> posEffects;
    uint32_t              posEffectSerial = 0;
    // Treasure pool (S2C 0x0D2 an item enters, 0x0D3 a lot or the outcome): ten slots.
    struct TreasureItem
    {
        uint16_t    itemId = 0;   // 0 = empty
        uint32_t    count  = 0;
        uint32_t    startTime = 0; // server time it dropped (five minutes to lot)
        std::chrono::steady_clock::time_point seen{};
        int16_t     myLot = -1;    // our lot (-1 none, 0 passed? LSB sends 0xFFFF for a pass)
        int16_t     bestLot = 0;   // the highest lot and who holds it
        std::string bestName;
    };
    std::array<TreasureItem, 10> treasure{};
    // Conquest (S2C 0x05E, asked with C2S 0x05A): owner per region 0..18 (LSB REGION_TYPE; 0 none, 1 San d'Oria,
    // 2 Bastok, 3 Windurst, 4 beastmen), our conquest points and the days to the next tally.
    struct Conquest
    {
        std::array<uint8_t, 19> owner{};
        uint32_t                points = 0;
        uint8_t                 nextTally = 0;
        // Besieged, after the conquest block (@0xA4): overview {AstralCandescence:2, AlZahbiOrders:2, ...}, then the
        // Mamook / Halvung / Arrapago strongholds {Orders:3, Forces:8, Level:4, MirrorDestroyed:1, Mirrors:4, Prisoners:4}.
        uint32_t                besiegedOverview = 0;
        std::array<uint32_t, 3> strongholds{};
        uint32_t                serial    = 0;
    } conquest;
    // /itemsearch answers (S2C 0x049 ITEMSEARCH): the item id for the name asked (0 = none).
    uint16_t itemSearchId     = 0;
    uint32_t itemSearchSerial = 0;
    // Linkshell messages (S2C 0x0CC), linkshell 1 / 2: the text and who posted it.
    std::array<std::pair<std::string, std::string>, 2> lsMessage;
    // The latest /nominate proposal seen (S2C 0x078): who asked, and its text ("[question]\n1:option...").
    std::string lastProposer, lastProposal;
    // Blacklist (S2C 0x041 at login, 0x042 per edit): account id and name. Chat from these names is dropped.
    std::vector<std::pair<uint32_t, std::string>> blacklist;
    int8_t   blacklistEdit   = 0; // the last 0x042 result: 0 added, 1 removed, 2 failed
    uint32_t blacklistSerial = 0;
    // Fishing. While the line is out (own animation 56), the seconds until the client asks whether a fish bit (S2C
    // 0x037 +0x4A, LSB hookDelay). S2C 0x115 FISH: the hooked fish's minigame values (docs/commands.md, "Fishing").
    uint8_t fishingTimer = 0;
    uint16_t petIndex    = 0; // our pet's entity index (S2C 0x037 Flags2 @0x34, bits 3..18), 0 = none
    // S2C 0x068 PET_SYNC (message type 4, LSB pet_sync.cpp): our pet's HP% / MP% / TP and name; cleared when the
    // packet comes without a pet (size 0x1C).
    bool        mentor     = false; // S2C 0x037 Flags3 MentorFlag
    bool        petPresent = false;
    uint8_t     petHpp     = 0;
    uint8_t     petMpp     = 0;
    uint16_t    petTp      = 0;
    std::string petName;
    // Knocked out: S2C 0x037 dead_counter1 @0x3C = 60 x the seconds left until the forced home point plus 6 minutes
    // (LSB char_status.cpp), and when it arrived. The KO window shows the time less those 6 minutes as m:ss
    // (CODE 0x1012de20 / 0x1012dff0: deadline - 360 - now). 0 = no counter.
    uint32_t                              deadCounter = 0;
    std::chrono::steady_clock::time_point deadCounterAt{};
    struct Fish
    {
        uint16_t stamina = 0, arrowDelay = 0, regen = 0, moveFrequency = 0, arrowDamage = 0, arrowRegen = 0, time = 0;
        uint8_t  sense   = 0;
        uint32_t special = 0; // echoed in C2S 0x110 para2
    } fish;
    uint32_t fishSerial = 0;
    // Play time in seconds as of the last zone-in (S2C 0x00A +0xA0), and when that arrived.
    uint32_t                              playTime = 0;
    std::chrono::steady_clock::time_point playTimeAt{};
    // The game clock (S2C 0x00A +0x3C GameTime: Earth seconds since the Vana'diel epoch) as of the last zone-in, and
    // when it arrived; the game's ntGameTimeGet is this plus the seconds since (FFXiMain.dll 0x100fadd2..0x101038d0).
    uint32_t                              gameTime = 0;
    std::chrono::steady_clock::time_point gameTimeAt{};
    // NPC shop (S2C 0x03E opens it, 0x03C lists the goods): price, item, and the index C2S 0x083 buys by.
    struct ShopItem
    {
        uint32_t price = 0;
        uint16_t item  = 0;
        uint8_t  index = 0;
    };
    std::vector<ShopItem> shop;
    uint32_t              shopSerial = 0; // counts shop openings
    // The vendor's price for the item being sold (S2C 0x03D after C2S 0x084).
    struct SellQuote
    {
        uint32_t price = 0;
        uint8_t  slot  = 0;
        uint32_t count = 0;
    } sellQuote;
    uint32_t sellQuoteSerial = 0;
    // A guild shop (S2C 0x086 / 0x083 / 0x085 / 0x082 / 0x084; client/guild_shop.h). lists[0] = what the guild sells
    // (the Buy list), lists[1] = what it buys (the Sell list). openSerial counts 0x086 Stat 0 (open the menu);
    // closeSerial counts closed / holiday answers, with the lines to log (unless `passive` and no guild menu is open).
    struct Guild
    {
        std::array<GuildList, 2> lists;
        uint32_t                 listSerial  = 0; // bumps when a list or a stock changes
        uint32_t                 openSerial  = 0;
        uint32_t                 closeSerial = 0;
        bool                     passive     = false;
        std::vector<std::string> closeLines;
    } guild;
    // The event the server started (S2C 0x032 / 0x034 / zone-in), for the client's event script runner; eventSerial
    // counts them. Updates from the server (S2C 0x05C) replace the parameters and bump eventUpdateSerial.
    struct Event
    {
        uint32_t                uniqueNo  = 0;
        uint16_t                actIndex  = 0;
        uint16_t                eventNum  = 0; // the zone whose event file holds the script
        uint16_t                eventPara = 0; // the event id
        std::array<uint32_t, 8> params{};
        // S2C 0x033 String[4], replaced by 0x05D: the event's text arguments (messages' 0x1C n) and saved names.
        std::array<std::string, 4> strings{};
    } event;
    uint32_t                eventSerial = 0;
    uint32_t                eventStringsSerial = 0; // S2C 0x05D replaced event.strings
    // Event op 0xC3's name records (0x10488210: a 16-byte name and a value), kept across events as the game keeps them;
    // zone dialog prints them with 01 05 84 / picks by the value with 01 05 85.
    std::array<std::pair<std::string, uint32_t>, 8> eventNames{};
    std::array<uint32_t, 8> eventUpdate{};
    uint32_t                eventUpdateSerial = 0;
    // S2C 0x052 EVENTUCOFF mode 1 (EventRecvPending): the server has answered an event update (op 0x43), with or
    // without new parameters (LSB sends it after every update; 0x05C only when the script has some).
    uint32_t                eventReleaseSerial = 0;
    // Last synthesis result (S2C 0x06F COMBINE_ANS): LSB SynthesisResult, Grade (HQ tier when > 0), Count, ItemNo.
    struct SynthResult
    {
        uint8_t  result = 0;
        int8_t   grade  = 0;
        uint8_t  count  = 0;
        uint16_t item   = 0;
    } synth;
    uint32_t synthSerial = 0;
    // Quest logs (S2C 0x056 ports 0x50.. current / 0x90.. completed; LSB QuestLog 0 San d'Oria .. 7 Crystal War): 256 bits each.
    std::array<std::array<uint32_t, 8>, 8> questCurrent{}, questDone{};
    // Missions: our nation (0 San d'Oria, 1 Bastok, 2 Windurst), its current mission and Zilart's (port 0xFFFF;
    // 0xFFFF = none), and completed nation / Zilart missions (port 0xD0: two words per log 0..3).
    uint32_t nation = 0, nationMission = 0xFFFF, zilartMission = 0xFFFF;
    std::array<uint32_t, 8> missionsDone{};
    uint8_t               reviveOffer  = 0;
    uint32_t              reviveSerial = 0;
    uint16_t            weather = 0; // server weather id (S2C 0x00A +0x68, 0x057 +0x08)
    // Zone music ids (musicNNN.bgw): day, night, solo battle, party battle, mount (0x00A +0x56; 0x05F changes a slot).
    std::array<uint16_t, 5> music{};

    // Own vitals (S2C 0x0DF): HP/MP/TP, percentages, jobs.
    uint32_t hp = 0, mp = 0, tp = 0;
    uint8_t  hpp = 0, mpp = 0;
    uint8_t  mainJob = 0, mainLevel = 0, subJob = 0, subLevel = 0;
    CharStats stats;
    std::vector<PartyMember> party; // empty when solo
    // An invite waiting for an answer (S2C 0x0DC): who sent it.
    struct Invite
    {
        uint32_t    id = 0;
        std::string name;
        uint8_t     kind = 0; // 0 party, 5 alliance
    };
    std::optional<Invite> partyInvite;
    // Lobby: the account's characters while the session waits for a choice (Config::chooseInUi).
    std::vector<lobby::CharacterSlot> lobbySlots;
    bool                              choosingCharacter = false;
    // Our status effects (S2C 0x063 type 9): icon id and when it wears off (none = no timer).
    struct Buff
    {
        uint16_t                                             icon = 0;
        std::optional<std::chrono::steady_clock::time_point> expires;
        uint32_t timer = 0x7FFFFFFF; // the packet's raw expiry (seconds since the Vana'diel epoch x 60, wrapping)
    };
    std::vector<Buff> buffs;
    // Player trade (S2C 0x021 request, 0x022 state, 0x023 the partner's items).
    struct Trade
    {
        uint32_t requestFrom = 0;  // someone asked to trade (0x021), not answered yet
        uint16_t requestIndex = 0;
        bool     active = false;   // window open (0x022 Start)
        uint32_t serial = 0;       // bumps on every 0x022 Start (kept across trades)
        uint32_t partner = 0;
        uint16_t partnerIndex = 0;
        std::string partnerName;   // as the game keeps it at the start (trade manager +0x7c)
        bool     partnerConfirmed = false; // 0x022 Make from the partner
        uint16_t counter = 0;      // latest TradeCounter (0x023), echoed in our confirm
        std::array<std::pair<uint16_t, uint32_t>, 10> theirs{}; // by TradeIndex (0 = gil, 1..8 items): item id, count
        uint32_t theirsUpdates = 0; // bumps on every 0x023 (the game unlocks both sides then, CODE 0x10152380)
        // Our offer as the server accepted it (0x025), by TradeIndex: inventory slot, count (count 0 = empty or refused).
        std::array<std::pair<uint8_t, uint32_t>, 10> mine{};
        uint32_t mineUpdates = 0; // bumps on every 0x025
    } trade;
    Look     look; // own appearance
    uint8_t  animation = 0; // own server status (S2C 0x037 +0x30): 0 idle, 1 engaged, 2/3 dead, 33 resting...
    // Own movement speed: S2C 0x00A +0x1C (u8) and 0x037 +0x2C (low 12 bits); the game runs Speed x 0.1 yalms a second
    // (CODE 0x10094aaa -> actor +0x15c / +0x98) and walks a third of that (0x10081fe0). LSB's normal value is 50.
    uint16_t speed = 50;
    uint8_t  chocoboIndex = 0; // riding: 0x037 +0x28 bits 13..15 (as Entity::chocoboIndex)
    uint8_t  mountIndex   = 0; // 0x037 +0x5B
    std::array<uint32_t, 2> chocoboLook{}; // our own chocobo's looks (S2C 0x067 char sync +0x18 / +0x1C, as Entity::chocoboLook)
    bool     anonymous    = false; // 0x037 +0x28 bit 5 (AnonymousFlag)
    bool                   linkshell = false; // 0x037 +0x28 bit 25 (LinkShellFlag): our own linkshell worn
    std::array<uint8_t, 3> lsColor{};         // 0x037 +0x31 r, g, b
    // The Emote List's unlocks (S2C 0x11A, asked for with C2S 0x119 when the list opens; CODE 0x10225010): job emotes
    // by job bit (WAR = 0), chairs 1..11 by bit.
    uint32_t emoteJobs   = 0;
    uint16_t emoteChairs = 0;
    // The last S2C 0x113 CURRENCIES_1, header included (asked for with C2S 0x10F when the Currencies window opens;
    // CODE 0x10229d80); `currenciesSeq` counts the replies.
    std::vector<std::byte> currencies;
    uint32_t               currenciesSeq = 0;
    NameFlags              nameFlags;         // our name label's icons (0x037)
    // S2C 0x0B4 CONFIG: the server's SAVE_CONF head (flags u32, chat filters 1 / 2), echoed in C2S 0x0DB.
    std::array<uint8_t, 12> saveConf{};
    bool                    saveConfValid = false;
    uint32_t swings    = 0; // own melee rounds
    uint32_t lastHitBy = 0; // as Entity::lastHitBy, for us
    uint16_t castSpell = 0; // own casting state, as Entity
    std::chrono::steady_clock::time_point castStarted{}; // when our current cast began (for the cast bar)
    uint32_t castsDone = 0;
    uint16_t wsAnimation = 0; // own weapon skills, as Entity
    uint32_t weaponSkillsDone = 0;
    uint8_t  emote = 0; // own emotes, as Entity
    uint32_t emotes = 0;
    // Usable commands (S2C 0x0AC bitfields): weapon skill ids and job ability ids (names: abilities table, +512).
    std::vector<uint16_t> weaponSkills;
    std::vector<uint16_t> jobAbilities;
    std::vector<uint16_t> petAbilities; // 0x0AC PetAbilities: bit n = ability 512 + n (blood pacts, Ready moves)
    std::vector<uint16_t> traits; // job traits (names: abilities table, +1536)
    std::vector<uint16_t> spells; // known spells (S2C 0x0AA bitfield)
    // When our own spells / job abilities are ready again, from the recast seconds in our S2C 0x028 actions (the
    // server sends no spell-recast packet; the client keeps these timers itself).
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> spellReady;
    // Job-ability recasts by recast group (S2C 0x119); abilities map to groups through the ability data (file 85).
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> abilityRecastReady;
    // The same S2C 0x119 timers in packet order (slot 0: the two-hour, id 0), as the game keeps them, and when they came.
    struct AbilityRecast
    {
        uint16_t seconds = 0;
        uint8_t  calc1   = 0;
        uint8_t  id      = 0;
        int16_t  calc2   = 0;
    };
    std::array<AbilityRecast, 31>         abilityRecasts{};
    std::chrono::steady_clock::time_point abilityRecastsAt{};
    // S2C 0x044 EXTENDED_JOB, as the game keeps it (its inventory object + 0x40c44 in 30260604): the 0x9C bytes after
    // the header, [0] for the main job's packet (IsSubJob 0), [1] for the support job's. BLU: SetSpells[20] @+4 (spell
    // id - 512); PUP: the automaton's head / frame / attachments.
    std::array<std::array<uint8_t, 0x9C>, 2> extendedJob{};
    // S2C 0x01C ITEM_MAX: every container's size (ItemNum, 1 + the slots) and usable size (ItemNum2; 0 = not usable).
    std::array<uint8_t, 18>  containerSize{};
    std::array<uint16_t, 18> containerUsable{};
    bool                     containerSizeValid = false;
    // Ability id -> recast group as observed (the group whose timer started right after we used the ability). The
    // ability data's group (file 85 +0x06) disagrees with LSB for many abilities, so observation wins.
    std::unordered_map<uint16_t, uint16_t> abilityRecastGroup;
    std::vector<InventoryItem> inventory; // all containers, sorted by (container, slot)
    // Equipped items by equip slot (0 main .. 15 back): container and slot of the item; slot 0 = nothing (S2C 0x050).
    std::array<std::pair<uint8_t, uint8_t>, 18> equipment{}; // 0..15 gear, 16/17 linkshell 1/2 (LSB SLOT_LINK1/2)
    [[nodiscard]] bool engaged() const
    {
        return animation == 1;
    }
};

// Vana'diel time of day in hours [0, 24), from the system clock: 25x Earth time since the Vana'diel epoch
// (Unix 1009810800), as LandSandBoat computes it (common/vanadiel_clock.h, vana_time.h).
double vanaHourOfDay();

// Runs login + the map connection on a background thread; the render thread exchanges data through
// snapshots and small command calls. All network I/O stays on the session thread.
class Session
{
public:
    struct Config
    {
        lobby::Config lobby;
        std::string   character;   // empty = first character (or the lobby choice with chooseInUi)
        bool          chooseInUi = false; // wait in the lobby for chooseCharacter() when `character` is empty
        std::string   compressDat; // LSB res/compress.dat
        bool entityRenames = false; // ask for the server's rename list after each zone-in (HorizonXI; entity_names.h)
        // Game message text (optional): combat lines use the built-in subset when absent.
        std::shared_ptr<const dat::BattleText> text;
        // Game data for zone dialog (optional; must outlive the session).
        const dat::DatIndex* index = nullptr;
        // Config Misc. 4 chat filter (optional) and the text a player-filtered line shows (55648 #345).
        std::shared_ptr<const ChatFilter> chatFilter;
        std::string                       deletedText = " ***This message has been deleted.***";
        // Sees every sub-packet in and out on the session thread (map::MapClient::Filter): Ashita's packet events.
        std::function<bool(bool incoming, bool injected, uint16_t id, std::vector<std::byte>& bytes, std::span<const std::byte> chunk)> packetFilter;
    };

    explicit Session(Config cfg);
    ~Session(); // logs out (bounded wait) and joins

    void start();

    // Begin logging out; the thread finishes after the server confirms (or times out).
    void requestStop()
    {
        stop_ = true;
        choiceCv_.notify_all();
    }
    // With Config::chooseInUi: the character picked in the lobby (index into WorldSnapshot::lobbySlots).
    void chooseCharacter(std::size_t index)
    {
        {
            std::lock_guard lock(mutex_);
            choice_ = index;
        }
        choiceCv_.notify_all();
    }
    [[nodiscard]] bool finished() const
    {
        return finished_;
    }
    // The server logged us out after a /logout or /shutdown countdown (the client then closes).
    [[nodiscard]] bool loggedOutByServer() const
    {
        return loggedOutByServer_;
    }
    // The map server stopped answering for 45 s; the session has ended.
    [[nodiscard]] bool connectionLost() const
    {
        return connectionLost_;
    }
    // /logout (shutdown false) or /shutdown: C2S 0x0E7 Mode 0 (toggle: a second request cancels), Kind 1 / 3. The
    // server counts down (LEAVEGAME, a system message every 5 s) and sends 0x00B at the end; play goes on meanwhile.
    void requestLogout(bool shutdown);
    // A movement key while kneeling (/heal, or the /logout / /shutdown countdown, which LSB shows as resting): end it
    // as the game does before the character may walk. With a countdown running: C2S 0x0E7 Mode 2 (off) with its Kind
    // (LSB's LEAVEGAME then drops the rest too); else C2S 0x0E8 Mode 2 (rest off). The server's status update stands
    // the character up.
    void standUp();
    [[nodiscard]] bool logoutPending() const
    {
        return logoutKind_.load() != 0;
    }

    [[nodiscard]] WorldSnapshot snapshot() const;

    // Our current position/heading (game coordinates), sent with every map datagram.
    void setPlayer(const glm::vec3& gamePos, uint8_t dir, uint16_t targetIndex = 0);
    void say(const std::string& text, uint8_t kind = 0);
    // Report walking into a zone line (C2S 0x05E); the server answers with 0x00B and the session re-zones.
    // `exitBit` / `exitMode`: a Mog House exit (`zmrq`; mode 0 = back where you came in, 1..4 the city's areas, 125 /
    // 126 = 2F / 1F, 127 Mog Garden; CODE 0x1008adf0).
    void enterZoneLine(uint32_t rectId, uint8_t exitBit = 0, uint8_t exitMode = 0);
    // C2S 0x037 ITEM_USE: use item (container, slot) on a target (yourself for most items).
    void useItem(uint8_t container, uint8_t slot, uint32_t targetId, uint16_t targetIndex);
    // C2S 0x050 EQUIP_SET: put item (container, slot) in equip slot `equipSlot`; slot 0 unequips.
    void equip(uint8_t container, uint8_t slot, uint8_t equipSlot);
    // A client-side line in the chat log (system colour).
    void note(std::string text);
    // A standard message (file 7031, LSB MsgStd id) into the log from the client itself, with Para0 / Para1 (the game's
    // 0x100a1790: the Mog House moogle's lines).
    void stdNote(uint16_t message, int32_t para0 = 0, int32_t para1 = 0);
    // The same with text argument 0 (0x1C 0: a name, as the trade messages 24..30 take it).
    void stdNote(uint16_t message, const std::string& string0);
    // The current event (WorldSnapshot::event): finish it (C2S 0x05B mode 0) or send an update (mode 1), with EndPara.
    void finishEvent(uint32_t endPara);
    void updateEvent(uint32_t endPara);
    // An update with a position (C2S 0x05C mode 1; event op 0x47): game x, y (vertical), z and the heading byte.
    void updateEventPosition(uint32_t endPara, const glm::vec3& pos, uint8_t dir);
    // A zone dialog message (6420 + zone) into the log, spoken by `from` (empty = no name), with these numbers.
    void eventSay(const std::string& from, uint16_t message, const std::array<int32_t, 8>& numbers);
    // A zone dialog message as lines (menus: the prompt, then the options).
    std::vector<std::string> zoneDialog(uint16_t message, const std::array<int32_t, 8>& numbers);
    // Event op 0xC3: name record `slot` (0..7) = `name` with `value`.
    void setEventName(std::size_t slot, std::string name, uint32_t value);
    // Any C2S packet (payload after the 4-byte header), sent on the session thread in order.
    void send(uint16_t id, std::vector<std::byte> payload);
    // The same from an addon (Ashita AddOutgoingPacket: packet_out sees it as injected), and an S2C packet an addon
    // made (AddIncomingPacket; full bytes), handled as if the server sent it.
    void sendInjected(uint16_t id, std::vector<std::byte> payload);
    void injectIncoming(std::vector<std::byte> packet);
    // XI_FAKE_FISH: tells whether we face water, as the server's fishing areas would (called on the main thread).
    void setFakeFishWater(std::function<bool()> faceWater)
    {
        std::lock_guard lock(mutex_);
        fakeFishWater_ = std::move(faceWater);
    }
    // C2S 0x06E: invite a player (UniqueNo, ActIndex, kind 0 party / 5 alliance).
    void inviteToParty(uint32_t id, uint16_t index);
    // C2S 0x074: answer the pending invite (clears WorldSnapshot::partyInvite).
    void answerPartyInvite(bool accept);
    // The server message (/servmes; also asked once at the first zone-in): C2S 0x04B fragments, answered by S2C 0x04D.
    void requestServerMessage();
    // C2S 0x01A ACTION on a target (ActionID: 0 talk, 2 attack, 4 disengage, ...).
    // `param` goes to ActionBuf[0]: the weapon skill / job ability id for ActionID 7 / 9.
    void action(uint32_t targetId, uint16_t targetIndex, uint16_t actionId, uint32_t param = 0);

private:
    std::string servmes_;           // the server message being assembled (S2C 0x04D)
    bool        servmesAsked_ = false;
    void run();

    Config              cfg_;
    std::thread         thread_;
    std::atomic<bool>   stop_{ false };
    std::atomic<bool>   finished_{ false };
    std::atomic<bool>   loggedOutByServer_{ false };
    std::atomic<bool>   connectionLost_{ false };
    std::atomic<uint16_t> logoutKind_{ 0 }; // the /logout (1) or /shutdown (3) countdown we asked for, 0 none

    mutable std::mutex                    mutex_;
    std::condition_variable               choiceCv_;
    std::optional<std::size_t>            choice_;
    WorldSnapshot                         world_;
    std::unordered_map<uint32_t, Entity>  entities_;
    glm::vec3                             playerPos_{ 0.0f };
    uint8_t                               playerDir_  = 0;
    uint16_t                              playerTarget_ = 0;
    bool                                  playerDirty_ = false;
    std::deque<std::pair<std::string, uint8_t>> outgoingChat_;
    std::optional<uint32_t>                     pendingZoneLine_;
    uint8_t                 pendingExitBit_ = 0, pendingExitMode_ = 0;
    struct PendingAction
    {
        uint32_t targetId = 0;
        uint16_t index    = 0;
        uint16_t actionId = 0;
        uint32_t param    = 0;
        std::chrono::steady_clock::time_point at{}; // when the interface asked (XI_NET_TRACE timing, #97)
    };
    std::deque<PendingAction> pendingActions_;
    // The last job ability we used, to attribute the next new recast timer to it.
    std::optional<std::pair<uint16_t, std::chrono::steady_clock::time_point>> lastAbility_;
    std::deque<std::array<uint8_t, 3>> pendingEquips_; // (slot, equipSlot, container)
    std::deque<std::pair<uint16_t, std::vector<std::byte>>> pendingRaw_;
    EntityRenames                                                       renames_;   // S2C 0x1FF (this zone's)
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> nameAsked_; // C2S 0x016 asked, by ActIndex
    std::deque<std::pair<uint16_t, std::vector<std::byte>>> pendingInjectedOut_;
    std::deque<std::vector<std::byte>>                      pendingInjectedIn_;
    // Test hook XI_FAKE_FISH=catch|nobite|rodbreak|monster: the server's side of fishing played by the client (for
    // servers with fishing off): /fish and C2S 0x110 are answered with S2C 0x037 (the last real one, status and
    // hook delay changed) and 0x115 after a short delay, the server's lines as notes. Those packets are not sent.
    struct FakeFish
    {
        std::chrono::steady_clock::time_point due;
        std::vector<std::byte>                packet; // S2C, injected (empty: none)
        std::string                           note;
    };
    std::deque<FakeFish>   fakeFish_;
    std::function<bool()>  fakeFishWater_; // whether we face water (else "You can't fish here.")
    std::vector<std::byte> lastOwnStatus_; // the last S2C 0x037 about us
    bool                   fakeFishing(uint16_t id, std::span<const std::byte> payload); // under mutex_; true: answered
    struct PendingItemUse
    {
        uint8_t  container = 0, slot = 0;
        uint32_t targetId    = 0;
        uint16_t targetIndex = 0;
    };
    std::deque<PendingItemUse> pendingItemUses_;
    std::map<uint16_t, InventoryItem> inventory_;     // key: container << 8 | slot
    struct PendingEvent
    {
        uint32_t uniqueNo  = 0;
        uint16_t actIndex  = 0;
        uint16_t eventNum  = 0;
        uint16_t eventPara = 0;
        uint32_t endPara   = 0;
        uint16_t mode      = 0; // C2S 0x05B Mode: 0 end, 1 update
        std::optional<glm::vec3> pos; // set: C2S 0x05C (mode 1) instead
        uint8_t  dir       = 0;
    };
    std::deque<PendingEvent> pendingEvents_; // EVENTEND packets to send
    std::optional<dat::DialogTable> zoneText_; // the zone's dialog (6420 + zone), guarded by mutex_
    std::optional<dat::DialogTable> stdText_;  // standard system messages (file 7031, LSB MsgStd ids), loaded on first use
    const dat::DialogTable& stdText();         // loads stdText_ (the caller holds mutex_)
    // A standard message into the log (the caller holds mutex_), with Para0 / Para1 and text argument 0.
    void stdNoteLocked(uint16_t message, int32_t para0, int32_t para1, const std::string& string0);
    std::optional<dat::DialogTable> emoteText_; // emote lines (file 7025: 2 * id with a target, 2 * id + 1 without)
    // The zone-in area lines' tables (zone_banner.h), loaded at the first zone-in (the caller holds mutex_).
    struct AreaTexts
    {
        std::optional<dat::XiStringTable> formats;    // string table 10 (file 50)
        std::optional<dat::StringTable>   areas;      // zone names (55465)
        std::optional<dat::StringTable>   clientText; // the client's messages (55648)
        std::optional<dat::StringTable>   sysText;    // string table 7 (55646): system lines such as the guild results
        std::optional<dat::StringTable>   weekdays;   // the Vana'diel weekdays (55658 #0..7)
    };
    std::optional<AreaTexts> areaTexts_;
    const AreaTexts&         texts(); // loads areaTexts_ (the caller holds mutex_)
    // S2C 0x086 / 0x082 / 0x084: the guild shop's log lines (the caller holds mutex_).
    void guildClosedLines(const GuildOpen& g);
    void guildResultLines(bool sell, std::span<const std::byte> packet);
    void logZoneIn(uint16_t zone, uint32_t loginState, uint16_t mapNumber, uint8_t mhFloor, uint16_t mhMap);
    uint16_t                        zoneTextZone_ = 0xFFFF;
};

} // namespace xi::client
