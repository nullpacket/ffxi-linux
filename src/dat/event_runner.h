// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"

#include <array>
#include <map>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Cutscenes (docs/event-format.md §6): every actor block of the zone's event file with an entry for the event runs
// its threads at once. Each actor has 16 thread slots; each frame it runs its highest-priority one (lowest number)
// until that thread yields (CODE 0x100bcc40). Threads start others (ops 0x27 / 0x29), wait (0x1C, 0x2A ...), move,
// turn and animate actors, run effect schedulers (camera shots, fades) and show messages. The client does the
// visible part through EventHost.
namespace xi::dat
{

// One actor's block (CODE 0x100bd0d0): its entries, consts and code.
struct EventBlock
{
    uint32_t              id = 0; // UniqueNo; 0x7FFFFFF0 = the player's own block
    std::vector<uint16_t> entryOff, entryId;
    std::vector<uint32_t> consts;
    std::vector<uint8_t>  code;

    // The entry for `eventId` (exact match), else -1.
    [[nodiscard]] int entryFor(uint16_t eventId) const;
};

struct EventFile
{
    std::vector<EventBlock> blocks;

    static EventFile load(const DatIndex& index, uint16_t zone);
    [[nodiscard]] const EventBlock* block(uint32_t id) const;
};

// Game space (x east, y down, z north), as the server's positions.
struct EventVec
{
    float x = 0, y = 0, z = 0;
};

// What the event drives. Entity ids are server ids (UniqueNo); the player is EventHost::player().
class EventHost
{
public:
    virtual ~EventHost() = default;

    [[nodiscard]] virtual uint32_t player() const = 0;
    // Spawned (and its model known): the event waits for its actors (op 0x80; CODE 0x100aedd0).
    [[nodiscard]] virtual bool exists(uint32_t id) const = 0;
    // A participating entity the client does not have yet: ask the server (C2S 0x016 with its index, id & 0x3FF).
    virtual void request(uint32_t id) = 0;

    [[nodiscard]] virtual EventVec position(uint32_t id) const = 0;
    // Yaw in radians: 0 = +x, a = atan2(-dz, dx) (the game's facing convention; 4096 = 2 pi in the script).
    virtual void place(uint32_t id, const EventVec& pos, std::optional<float> yaw, bool snapToGround) = 0;
    virtual void face(uint32_t id, float yaw) = 0;
    // The actor moved this frame (its walk motion plays).
    // `id` moved this frame at `speed` units a second (op 0x32's; it picks the walk or run motion).
    virtual void walking(uint32_t id, float speed) = 0;
    virtual void hide(uint32_t id, bool hidden) = 0;

    // One-shot motion `fcc` (4 chars, first char lowest) on `id` toward `target`; then back to its idle motion.
    // `file` = op 0x5B's motion DAT (-1: none); `raceSet` = op 0x66's n, resolved per actor by the client (-1: none).
    virtual void motion(uint32_t id, uint32_t target, uint32_t fcc, int file, int raceSet) = 0;
    [[nodiscard]] virtual bool motionBusy(uint32_t id) const = 0;
    virtual void idle(uint32_t id, uint32_t fcc) = 0;

    // Head look-at (ops 0x79 / 0x7B; 0x1E / 0x4A look too): at `target`'s skeleton point, or at fixed angles (4096 = 2 pi:
    // pitch, yaw offset from the body) 10 units out from the feet; lookStop lets the actor's own look-at take over again.
    virtual void lookAt(uint32_t id, uint32_t target, int point) = 0;
    virtual void lookAngles(uint32_t id, int pitch, int yaw) = 0;
    virtual void lookStop(uint32_t id) = 0;
    // Op 0xB6 costume: race (slot -1), look slot 0..8 (face, head, body, hands, legs, feet, main, sub, ranged; raw look
    // words as in S2C 0x00D), a single model id (slot 9); scale (1 = as modelled). They last until the server's next look.
    virtual void costume(uint32_t id, int slot, uint16_t value) = 0;
    virtual void scale(uint32_t id, float scale) = 0;

    // Effect scheduler `fcc` from DAT `file` (camera shots, fades): start / stop / still running.
    virtual void scheduler(uint32_t file, uint32_t caster, uint32_t target, uint32_t fcc) = 0;
    virtual void stopScheduler(uint32_t file, uint32_t caster, uint32_t fcc) = 0;
    [[nodiscard]] virtual bool schedulerRunning(uint32_t file, uint32_t caster, uint32_t fcc) const = 0;
    // Op 0x46: 1 enter the cutscene camera, 0 leave it (3: with a fixed point).
    virtual void cameraMode(int mode) = 0;
    virtual void music(int slot, uint16_t id) = 0;
    virtual void sound(uint32_t id) = 0;
    // Op 0xC3 (CODE 0x100bbf30): name record `slot` (0..7) = a 16-byte name and a value, for the dialog's 01 05 84 / 85.
    virtual void nameRecord(std::size_t /*slot*/, const std::string& /*name*/, uint32_t /*value*/) {}

    // Dialog: show a message (speaker 0 = none), a menu (prompt + options, `hideMask` hides options), an event update
    // (C2S 0x05B mode 1). The runner waits for closeMessage() / choose() / updated().
    virtual void message(uint32_t message, uint32_t speaker) = 0;
    // The menu's default option is `defaultOption` (counting hidden ones; op 0x24 @+3, CODE 0x100b2520).
    virtual void menu(uint32_t message, uint32_t hideMask, uint32_t defaultOption, uint32_t speaker) = 0;
    virtual void update(uint32_t result) = 0;

    // --- the rest of the cutscene ops (docs/event-format.md §6.8); hosts that draw nothing keep the defaults ---
    // Op 0xB6 0D / 0E: a chocobo look {race 32 + colour, head, body, hands (the tail), legs, feet}, as a ridden one.
    virtual void chocobo(uint32_t, const std::array<uint16_t, 6>&) {}
    // Facing in radians as face() (op 0x3A reads it).
    [[nodiscard]] virtual float yaw(uint32_t) const
    {
        return 0.0f;
    }
    // The entity's animation byte (S2C 0x00E +0x1F) for the event (ops 0x4C / 0x4D / 0x8E / 0x8F / 0x4F; doors: 8 / 45
    // open, 9 / 46 close).
    virtual void animation(uint32_t, uint8_t) {}
    // Op 0x6C: the model's alpha (0x80 = opaque) moves to `alpha` over `ticks`.
    virtual void fade(uint32_t, int /*alpha*/, float /*ticks*/) {}
    // Op 0x6E: emote `id` (LSB Emote, < 97) in `mode` (0 = with a log line, 1 = motion only ...); emoteBusy: op 0x99 / 0x6E.
    virtual void emote(uint32_t, uint32_t /*target*/, int /*id*/, int /*mode*/) {}
    [[nodiscard]] virtual bool emoteBusy(uint32_t) const
    {
        return false;
    }
    // Ops 0x77 / 0x78: the event's Vana'diel hour (0..23, -1 keep) and weather (S2C 0x057 ids, -1 keep); restore both.
    virtual void environment(int /*hour*/, int /*weather*/) {}
    virtual void restoreEnvironment() {}
    // Ops 0x69 / 0x6A: sound channels `mask` (1 zone effect sounds, 2 sound effects, 4 ambience, 8 all) to `level`
    // (0..1) over `ticks` (0 = at once). The event's end restores them.
    virtual void volume(uint32_t /*mask*/, float /*level*/, float /*ticks*/) {}
    // Op 0x75 00: show sub-map n of the zone (CODE 0x10178810); 0x47 00: C2S 0x05C {x, y, z, dir}.
    virtual void subMap(uint16_t) {}
    virtual void positionUpdate(const EventVec&, float /*yaw*/) {}
    // Ops 0xC8 / 0x8B / 0x8A: the map window ("menu mapframe"): open map `map` of `zone`; event marker `slot` (0..9, the
    // Markers list's page 1) at (x, z) named `label` on the map under the player of `zone`; close.
    virtual void mapShow(int /*zone*/, int /*map*/) {}
    virtual void mapMarker(int /*zone*/, int /*slot*/, float /*x*/, float /*z*/, const std::string& /*label*/) {}
    virtual void mapClose() {}
    // Op 0xB4 (CODE 0x100b0930): 02 the target window ("menu targetwi") in event mode with `name` and a gauge 0..100
    // (target object +0x66); 03 closes it (gauge -1).
    virtual void eventTarget(const std::string& /*name*/, int /*gauge*/) {}
    // 07 / 0A: the cast-time (`which` 0, "menu casttime" mode 4) / event-time (1, "menu eventtim") window counting
    // `ticks` (1/60 s) down; 08 / 0B close it (ticks -1); 09 / 0C read what is left.
    virtual void eventTimer(int /*which*/, int /*ticks*/) {}
    [[nodiscard]] virtual int eventTimerLeft(int /*which*/) const
    {
        return 0;
    }
    // 14: the event map window's view (map object 0x101fac20: x, y, scale, over `seconds`, flags); 15 waits while it
    // is open.
    virtual void mapView(float /*a*/, float /*b*/, float /*c*/, float /*seconds*/, int /*flags*/) {}
    [[nodiscard]] virtual bool mapOpen() const
    {
        return false;
    }
};

class EventRunner
{
public:
    // `owner`: the block that started it (the NPC's UniqueNo, or 0x7FFFFFF0 for the player's own events).
    EventRunner(std::shared_ptr<const EventFile> file, uint32_t owner, uint16_t eventId, const std::array<uint32_t, 8>& params, EventHost& host);

    // Advances everything by `ticks` (60 per second).
    void tick(float ticks);

    [[nodiscard]] bool finished() const
    {
        return finished_;
    }
    // False when the owner has no entry for the event (nothing to run).
    [[nodiscard]] bool valid() const
    {
        return valid_;
    }
    // EndPara for C2S 0x05B: g1, or 0x40000000 after a cancelled menu.
    [[nodiscard]] uint32_t result() const
    {
        return cancelled_ ? 0x40000000u : globals_[1];
    }
    [[nodiscard]] bool waitingMessage() const
    {
        return messageOpen_;
    }
    void closeMessage()
    {
        messageOpen_ = false;
    }
    // The menu's option (0-based, counting hidden ones); -1 = cancelled.
    void choose(int option);
    void updated(const std::array<uint32_t, 8>& params);
    // The server answered the update without new parameters (S2C 0x052 mode 1): the wait ends, the work values stay.
    void released()
    {
        updatePending_ = false;
    }
    // The event's strings (S2C 0x033 String[4] @0x10 / 0x05D @0x28, handlers 0x100a02f0 / 0x100a0040): both packets
    // copy them into the saved names that op 0xB4 01 reads.
    void setSavedNames(const std::array<std::string, 4>& names)
    {
        for (std::size_t n = 0; n < 4; ++n)
        {
            savedNames_[n] = {};
            std::memcpy(savedNames_[n].data(), names[n].data(), std::min<std::size_t>(names[n].size(), 15));
        }
    }
    // A one-line trace of what runs (XI_EVENT_TRACE).
    void setTrace(bool on)
    {
        trace_ = on;
    }
    // Ops stepped over unknown, by opcode (b6 sub-ops as 0xB600 | sub): what is left to implement.
    [[nodiscard]] const std::map<uint16_t, int>& skipped() const
    {
        return skipped_;
    }
    // Ops whose effect is known but only touches game state this client has no use for (UI flags, entity display bits
    // whose meaning is open; §6.8), by opcode.
    [[nodiscard]] const std::map<uint16_t, int>& stateOnly() const
    {
        return stateOnly_;
    }

private:
    // Op 0xB4 01 / 04: four saved 16-byte names (CODE 0x104882b8, 256 bytes apart).
    std::array<std::array<uint32_t, 4>, 4> savedNames_{};
    std::map<uint16_t, int> skipped_, stateOnly_;
    struct Slot
    {
        int                      prio = 0xFF; // 0xFF = free
        std::size_t              pc   = 0;
        int                      entry = -1;
        int                      sub   = 0;   // multi-frame op state
        float                    wait  = -1;  // op 0x1C / 0x6F counter
        EventVec                 target;      // op 0x1F / 0x5A / 0x31 move target
        float                    timeout = 0;
        std::vector<std::size_t> stack;       // op 0x1A gosub
    };
    struct Actor
    {
        const EventBlock*           block  = nullptr;
        uint32_t                    entity = 0;
        std::array<uint32_t, 0x50>  locals{};
        std::array<Slot, 16>        slots;
        bool                        suspended = false;
        float                       speed     = 5.0f; // units per second (op 0x32)
        bool                        started   = false;
        bool                        noSnap    = false; // op 0x33 01: moves keep their height (entity +0x120 bit 21)
        int                         mainEntry = -1;
    };

    void   startActor(Actor& a);
    // Runs a thread until it yields; false when it ended (or the event did).
    void   run(Actor& a, Slot& s, float ticks);
    Actor* actorFor(uint32_t entity);
    [[nodiscard]] uint32_t entityOperand(const Actor& a, uint32_t v) const;
    [[nodiscard]] uint32_t read(const Actor& a, uint16_t ref) const;
    void                   write(Actor& a, uint16_t ref, uint32_t value);
    [[nodiscard]] int      length(const EventBlock& b, std::size_t at) const;
    bool                   startThread(Actor& from, uint32_t entity, int prio, int entry);

    std::shared_ptr<const EventFile> file_;
    EventHost&                       host_;
    uint16_t                         eventId_ = 0;
    std::vector<Actor>               actors_;
    std::array<uint32_t, 96>         globals_{};
    // Op 0x9D 02 / 0C: 64 pointers to tables in a block's code (CODE 0x104897f8, the block 0x104896e8).
    std::array<std::pair<const EventBlock*, std::size_t>, 64> workPtrs_{};
    bool                             finished_ = false, valid_ = false, cancelled_ = false;
    bool                             messageOpen_ = false, menuOpen_ = false, updatePending_ = false;
    float                            waitStart_ = 0; // ticks spent waiting for actors to spawn
    bool                             trace_     = false;
    uint32_t                         rng_       = 0x12345678;
};

} // namespace xi::dat
