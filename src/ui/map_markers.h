// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "client/session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace xi::ui
{
// The map window's Markers list ("menu mapv3", ui-format-notes §6.5): party and alliance members on the map.
struct MapMarkerMember
{
    uint32_t    id = 0;
    std::string name;
    bool        sameParty = false; // our own party (else another alliance party)
    float       x = 0, y = 0, z = 0; // game coordinates
};

// Party and alliance members (not us) in `w.zone` whose position is known, our own party first (CODE 0x101f7960 /
// 0x101f7a70 / 0x101f8cc0): a loaded entity's position, else the last S2C 0x0A0 for this zone.
std::vector<MapMarkerMember> mapMarkerMembers(const client::WorldSnapshot&                                  w,
                                              const std::vector<std::pair<float, const client::Entity*>>& nearby);

// The list's four pages (CODE 0x101fa920 steps them, skipping empty ones): 0 your own markers (only with the map's key
// item, mapframe +0xa46), 1 the event markers (always), 2 party / alliance members, 3 more alliance members.
inline bool mapMarkerPageAvailable(int page, bool haveMap, std::size_t members)
{
    switch (page)
    {
        case 0: return haveMap;
        case 1: return true;
        case 2: return members > 0;
        case 3: return members > 10;
        default: return false;
    }
}
// The next available page from `page` in direction `step` (+1 / -1, wrapping 3 <-> 0); page 1 is always available.
inline int mapMarkerStepPage(int page, int step, bool haveMap, std::size_t members)
{
    for (int i = 0; i < 4; ++i)
    {
        page = (page + step + 4) % 4;
        if (mapMarkerPageAvailable(page, haveMap, members))
        {
            return page;
        }
    }
    return 1;
}

// Marker shapes in "menu keytops3": on the map, item shape slot 3 (own marker) -> 150, 4 (event marker) -> 152,
// 5 (party) -> 154, 6 (alliance) -> 155, 2 (the player) -> 159 (table 0x1038214c: 0 151 159 150 152 154 155). In the
// list, the row icon (CODE 0x101fba70) is page 0: 151 / 150 (empty / placed), page 1: 153 / 152, page 2: 154 below the
// party count else 155, page 3: 155 (table 0x10382178: 151 150 153 152 154 154 155 155).
inline int mapMarkerShape(bool sameParty)
{
    return sameParty ? 154 : 155;
}
inline int mapMarkerListShape(int page, int row, int partyCount)
{
    return page == 2 && row < partyCount ? 154 : 155;
}
inline int mapMarkerSlotShape(int page, bool present)
{
    return page == 0 ? (present ? 150 : 151) : (present ? 152 : 153);
}

// Pages 0 and 1 are kept per map in the character's USER folder as "M<zone %03d><map %02d>.MRK" (CODE 0x101f7e60
// writes it on closing the map when changed, 0x101f7c60 reads it; an older "M%03d%02d_<char id %08d>.MRK" is read when
// the new name is missing). 0x1C0 bytes: u32 magic 0x58494D4B, u32 CRC-32 of the records (0x101c8800: the standard
// reflected CRC-32), then 20 records of 0x16 bytes, page 0 then page 1: {i16 x, i16 y (map pixels), char name[16],
// u8 present, u8 pad}. A 0x1C8-byte file has 8 more header bytes (magic @0, CRC @8, records @0x10).
struct MapMarkerSlot
{
    int16_t     x = 0, y = 0; // map texture pixels (0..512)
    std::string name;         // at most 15 characters
    bool        present = false;
};
using MapMarkerSlots = std::array<MapMarkerSlot, 20>; // [0..9] page 0, [10..19] page 1

constexpr std::size_t kMapMarkerFileSize = 0x1C0;
constexpr uint32_t    kMapMarkerMagic    = 0x58494D4B;

std::string                   mapMarkerFileName(int zone, int mapId);
uint32_t                      mapMarkerCrc(std::span<const std::byte> bytes);
std::optional<MapMarkerSlots> parseMapMarkers(std::span<const std::byte> file);
std::vector<std::byte>        serializeMapMarkers(const MapMarkerSlots& slots);
} // namespace xi::ui
