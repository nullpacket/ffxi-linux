// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"
#include "dat/texture.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

// Zone maps (the main menu's Map), §14: per-map records in a table inside FFXiMain.dll (VA 0x103821f0 in the reference
// build), read from the user's DLL by fingerprint. Each record names the map texture file and the world -> map transform.
namespace xi::dat
{

struct MapRecord
{
    int16_t  zone     = 0;
    int8_t   mapId    = 0;   // matches the collision placement's map id (§7.4)
    uint8_t  listed   = 0;   // position in the zone's map list, 0 = not listed
    uint8_t  bank     = 0;   // texture file bank: 0 -> 5312 + file, 1 -> 53295 + file, 2 -> 54295 + file
    int8_t   scale    = 0;   // |scale| / 2560 * 512 map pixels per world unit
    int16_t  file     = 0;
    int16_t  offX     = 0, offY = 0;
    uint16_t keyItem  = 0;   // +6: the map key item is 384 + this (385 San d'Oria area ...); 0 = no key item needed,
                             // 255 = special (zone 44 only). DATA: matches LSB's MAP_OF_* ids for every listed zone.

    // The key item id needed to view this map, 0 if none.
    [[nodiscard]] uint16_t requiredKeyItem() const
    {
        return keyItem > 0 && keyItem < 255 ? static_cast<uint16_t>(384 + keyItem) : 0;
    }

    [[nodiscard]] uint32_t fileId() const;
    // Map texture pixel (0..512) of a world position (game x, z; +z is north = up).
    [[nodiscard]] std::pair<float, float> project(float x, float z) const;
};

class ZoneMaps
{
public:
    // Reads the table from `ffxiMainDll`; with no table, every lookup finds nothing.
    ZoneMaps(const DatIndex& index, const std::filesystem::path& ffxiMainDll);

    [[nodiscard]] const MapRecord*      find(uint16_t zone, int8_t mapId) const;
    [[nodiscard]] std::optional<Texture> load(const MapRecord& r) const;
    [[nodiscard]] std::size_t           size() const
    {
        return records_.size();
    }

private:
    const DatIndex&        index_;
    std::vector<MapRecord> records_;
};

} // namespace xi::dat
