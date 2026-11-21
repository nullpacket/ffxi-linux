// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xi::dat
{
class StringTable;
class XiStringTable;
} // namespace xi::dat

namespace xi::client
{

// The S2C 0x00A (zone-in) fields the game's area line depends on (LSB GP_SERV_COMMAND_LOGIN).
struct ZoneInInfo
{
    uint16_t zone       = 0; // ZoneNo @+0x30
    uint32_t loginState = 0; // LoginState @+0x80: 1 Mog House, 2 game, 3 POL exit, 4 job exit, 5 POL exit to the Mog House
    uint16_t mapNumber  = 0; // MapNumber @+0x42, as sent (before the client's +0x1BC remap of ids >= 0x100)
    uint8_t  mhFloor    = 0; // (MyroomSubMapNumber @+0xA8 >> 1) & 3
    uint16_t mhMap      = 0; // MyroomMapNumber @+0xAA (the Mog House model)
};

// The Config Font Colors id of the area lines: log mode 0x29 -> colour code 0x67 (table 0x1036e898), the code that
// Config row "Actions others evade" (cfg 46) sets; the Log Settings filter of that code is bit 0x2C (0x1012a050).
inline constexpr uint8_t kAreaLineColour = 46;

// The lines the game logs when a zone-in arrives (CODE 0x100fac20 -> 0x100fb590 / 0x100fb620), in order:
// - Mog House (state 1): Mog Garden (MapNumber 0x118) and the Feretory (0x11d) get the area line; otherwise a Mog
//   House with a second floor (floor != 0, or model 0x121..0x123; 0x100f9580) gets "=== Area: Mog House 1F / 2F ==="
//   (55648 #294 + floor; floors 2 and 3 have no text), and a single-floor one gets nothing.
// - States 3, 4 and 5: nothing. Any other state (2 = the game): the area line.
// - The area line: string table 10 (file 50) #0x5E when it is not empty (it is empty in English), then #0x62
//   "=== Area: %s ===" with the zone's name (55465); "=== Area: <id> ===" when the zone has no name.
// `formats` = file 50, `areas` = 55465, `clientText` = 55648; a missing table drops the lines it would make.
std::vector<std::string> zoneInLines(const ZoneInInfo& z, const dat::XiStringTable* formats, const dat::StringTable* areas,
                                     const dat::StringTable* clientText);

} // namespace xi::client
