// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// Event scripts (docs/event-format.md): the bytecode the client runs when the server starts an event (S2C 0x032 /
// 0x034). This interpreter covers dialog: messages, waits for the message window, choice menus, event updates and
// the end of the event; camera, motion and effect opcodes are skipped by their lengths.
namespace xi::dat
{

// One actor's block of a zone's event file, positioned at the entry for one event id.
struct EventScript
{
    std::vector<uint32_t> consts;
    std::vector<uint8_t>  code;
    uint16_t              entry = 0;

    // The event file for a zone (EventNum): 5820 + zone below 256, then 84735 / 56881 / 65611 + zone.
    static uint32_t fileFor(uint16_t zone);
    // The block of `actor` (the NPC's UniqueNo; the player's own events use block 0x7FFFFFF0) with an entry for
    // `eventId` (EventPara). Empty if there is none.
    static std::optional<EventScript> find(const DatIndex& index, uint16_t zone, uint32_t actor, uint16_t eventId);
    // Every (actor, event id) entry of a zone's event file (wildcards and the init entry excluded).
    static std::vector<std::pair<uint32_t, uint16_t>> entries(const DatIndex& index, uint16_t zone);
};

// The length of the instruction at `at` (docs/event-format.md §3); 0 = unknown.
int eventOpLength(const std::vector<uint8_t>& code, std::size_t at);

class EventVm
{
public:
    enum class Stop : uint8_t
    {
        Message,   // show message `message` (speaker: `speaker`, 0 = none, 0xFFFFFFFF = this actor)
        WaitClose, // wait for the player to close the message window, then resume()
        Menu,      // show menu message `message` (prompt, then options); hidden options in `hideMask`; then choose()
        Update,    // send an event update (C2S 0x05B mode 1, EndPara = result()); resume once the server answers
        End,       // the event is over: finish with result()
        Error,     // unknown opcode or runaway script: finish with result()
        Face,      // op 0x1E: this actor turns to face entity `speaker` (0x7FFFFFF0 = the player); run() again
    };
    struct Step
    {
        Stop     stop     = Stop::End;
        uint32_t message  = 0;
        uint32_t speaker  = 0;
        uint32_t hideMask = 0;
    };

    // `params`: the event's eight parameters (S2C 0x034 num[8]; zero for 0x032), g2..g9.
    EventVm(EventScript script, const std::array<uint32_t, 8>& params);

    // Runs until the script needs the client (a message, a wait, a menu, an update or the end).
    Step run();
    // After Menu: the option picked (0-based, counting hidden options); -1 = cancelled.
    void choose(int option);
    // After Update: the server's new parameters (S2C 0x05C).
    void setParams(const std::array<uint32_t, 8>& params);
    // EndPara for C2S 0x05B (g1), or 0x40000000 when a menu was cancelled.
    [[nodiscard]] uint32_t result() const
    {
        return cancelled_ ? 0x40000000u : globals_[1];
    }

private:
    [[nodiscard]] uint32_t read(uint16_t ref) const;
    void                   write(uint16_t ref, uint32_t value);
    [[nodiscard]] uint8_t  u8(std::size_t at) const;
    [[nodiscard]] uint16_t u16(std::size_t at) const;
    [[nodiscard]] uint32_t u32(std::size_t at) const;
    [[nodiscard]] int      length(std::size_t at) const; // 0 = unknown

    EventScript                 s_;
    std::size_t                 pc_ = 0;
    std::array<uint32_t, 0x50>  locals_{};
    std::array<uint32_t, 96>    globals_{};
    std::vector<std::size_t>    stack_;
    bool                        cancelled_ = false;
    uint32_t                    rng_       = 0x12345678;
};

} // namespace xi::dat
