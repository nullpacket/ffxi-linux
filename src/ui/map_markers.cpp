// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/map_markers.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace xi::ui
{
std::vector<MapMarkerMember> mapMarkerMembers(const client::WorldSnapshot&                                  w,
                                              const std::vector<std::pair<float, const client::Entity*>>& nearby)
{
    // Our own party number: members whose (flags & 3) equals ours are "party", the rest "alliance" (CODE 0x101f7b27).
    uint8_t own = 0;
    for (const auto& m : w.party)
    {
        if (m.id == w.charId)
        {
            own = m.partyNo;
        }
    }
    std::vector<MapMarkerMember> out;
    for (const auto& m : w.party)
    {
        if (m.id == w.charId || m.id == 0)
        {
            continue; // the callback skips our own id (0x101f7aac); we are the player marker
        }
        MapMarkerMember mm;
        mm.id        = m.id;
        mm.name      = m.name;
        mm.sameParty = m.partyNo == own;
        bool known   = false;
        for (const auto& [dist, e] : nearby)
        {
            if (e && e->id == m.id)
            {
                mm.x  = e->pos.x;
                mm.y  = e->pos.y;
                mm.z  = e->pos.z;
                known = true;
                if (mm.name.empty())
                {
                    mm.name = e->name;
                }
                break;
            }
        }
        if (!known && m.mapPosKnown && m.mapZone == w.zone)
        {
            mm.x  = m.mapPos.x;
            mm.y  = m.mapPos.y;
            mm.z  = m.mapPos.z;
            known = true;
        }
        if (known)
        {
            out.push_back(std::move(mm));
        }
    }
    std::stable_partition(out.begin(), out.end(), [](const MapMarkerMember& m) { return m.sameParty; });
    return out;
}

std::string mapMarkerFileName(int zone, int mapId)
{
    return fmt::format("M{:03d}{:02d}.MRK", zone, mapId);
}

uint32_t mapMarkerCrc(std::span<const std::byte> bytes)
{
    // CODE 0x101c8800: table 0x1037ce90 (polynomial 0xEDB88320), seed ~0, result inverted.
    uint32_t crc = 0xFFFFFFFFu;
    for (const auto b : bytes)
    {
        crc ^= static_cast<uint8_t>(b);
        for (int k = 0; k < 8; ++k)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

std::optional<MapMarkerSlots> parseMapMarkers(std::span<const std::byte> file)
{
    std::size_t crcAt = 4, recordsAt = 8;
    if (file.size() == kMapMarkerFileSize + 8)
    {
        crcAt     = 8;
        recordsAt = 0x10;
    }
    else if (file.size() != kMapMarkerFileSize)
    {
        return std::nullopt;
    }
    auto u32 = [&](std::size_t o) {
        uint32_t v = 0;
        std::memcpy(&v, file.data() + o, 4);
        return v;
    };
    const auto records = file.subspan(recordsAt, 20 * 0x16);
    if (u32(0) != kMapMarkerMagic || u32(crcAt) != mapMarkerCrc(records))
    {
        return std::nullopt;
    }
    MapMarkerSlots out;
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const auto* r = records.data() + i * 0x16;
        std::memcpy(&out[i].x, r, 2);
        std::memcpy(&out[i].y, r + 2, 2);
        const auto* name = reinterpret_cast<const char*>(r + 4);
        out[i].name.assign(name, strnlen(name, 15));
        out[i].present = static_cast<uint8_t>(r[0x14]) == 1;
    }
    return out;
}

std::vector<std::byte> serializeMapMarkers(const MapMarkerSlots& slots)
{
    std::vector<std::byte> out(kMapMarkerFileSize);
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        auto* r = out.data() + 8 + i * 0x16;
        std::memcpy(r, &slots[i].x, 2);
        std::memcpy(r + 2, &slots[i].y, 2);
        std::memcpy(r + 4, slots[i].name.data(), std::min<std::size_t>(slots[i].name.size(), 15));
        r[0x14] = std::byte{ static_cast<uint8_t>(slots[i].present ? 1 : 0) };
    }
    const uint32_t magic = kMapMarkerMagic;
    const uint32_t crc   = mapMarkerCrc(std::span<const std::byte>(out).subspan(8));
    std::memcpy(out.data(), &magic, 4);
    std::memcpy(out.data() + 4, &crc, 4);
    return out;
}
} // namespace xi::ui
