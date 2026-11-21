// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

// Name label icons (docs/ui-format-notes.md §10.8): the one icon the game puts before a player's name and the Job
// Master stars over it, from the PC update (S2C 0x00D) or, for us, the server status (S2C 0x037) flags.
namespace xi::client
{

struct NameFlags
{
    bool    pol       = false; // PlayOnlineFlag (0x00D +0x20 bit 16, 0x037 +0x28 bit 24)
    bool    linkshell = false; // LinkShellFlag (bit 17 / bit 25)
    bool    linkDead  = false; // LinkDeadFlag (bit 18 / bit 26)
    bool    away      = false; // AwayFlag (bit 14 / bit 7)
    bool    lfg       = false; // LfgFlag, seeking a party (bit 11 / bit 4)
    uint8_t gmLevel   = 0;     // GmLevel (bits 24..26 / 29..31)
    bool    gmIcon    = false; // GmIconFlag: no GM icon, the others instead (0x00D +0x24 bit 28 / 0x037 +0x2C bit 31)
    bool    invisible = false; // InvisFlag (0x00D +0x20 bit 29 / 0x037 +0x2C bit 15): no icons
    bool    bazaar    = false; // BazaarFlag (0x00D +0x20 bit 31 / 0x037 +0x2C bit 29)
    bool    autoParty = false; // AutoPartyFlag (0x00D +0x24 bit 31 / 0x037 +0x34 bit 2)
    bool    lfgMaster = false; // LfgMasterFlag, recruiting (0x00D +0x28 bit 1 / 0x037 +0x38 bit 0)
    bool    newChar   = false; // NewCharacterFlag (0x00D +0x28 bit 23 / 0x037 +0x38 bit 3)
    bool    mentor    = false; // MentorFlag (0x00D +0x28 bit 24 / 0x037 +0x38 bit 4)
    bool    trial     = false; // TrialFlag (0x00D +0x33 bit 1 / 0x037 +0x38 bit 1)
    bool    jobMaster = false; // JobMasterFlag (0x00D +0x33 bit 6 / 0x037 +0x58 bit 7): the stars line
    uint8_t ballistaTeam = 0;  // BallistaTeam (0x00D +0x29 / 0x037 +0x39)
    uint8_t ballistaInfo = 0;  // BallistaInfo (0x00D +0x32)

    // From a 0x00D General block (needs at least 0x34 bytes).
    static NameFlags fromPcUpdate(uint32_t flags1, uint32_t flags2, uint32_t flags3, uint8_t ballistaInfo, uint8_t flags4);
    // From a 0x037 (flags0 @0x28, flags1 @0x2C, flags2 @0x34, flags3 @0x38, flags4 @0x58).
    static NameFlags fromServerStatus(uint32_t flags0, uint32_t flags1, uint32_t flags2, uint32_t flags3, uint8_t flags4);
};

// The bytes before the name (CODE 0x10097770 -> 0x10097430): one icon code, the highest of the game's priority chain,
// else 0x92 (the linkshell pearl) when a linkshell is worn; empty when none or the player is invisible. Codes are "font
// fontshp" shapes code - 0x20; 0xAC is followed by 0xAD (0x10086140).
std::string nameIcons(const NameFlags& f);

} // namespace xi::client
