// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/zone_map.h"

#include "dat/chunk.h"
#include "dat/dll_data.h"
#include "xi/bytes.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace xi::dat
{

uint32_t MapRecord::fileId() const
{
    // CODE 0x101f81a0; any other bank shows the "no map" page (5522).
    switch (bank)
    {
        case 0: return 5312u + static_cast<uint16_t>(file);
        case 1: return 53295u + static_cast<uint16_t>(file);
        case 2: return 54295u + static_cast<uint16_t>(file);
        default: return 5522u;
    }
}

std::pair<float, float> MapRecord::project(float x, float z) const
{
    // CODE 0x101f7120 / 0x101f7170: truncation toward zero (_ftol), z negated.
    const float k  = static_cast<float>(std::abs(scale)) / 2560.0f * 512.0f;
    const auto  rx = static_cast<int>(x * k + 0.5f);
    const auto  ry = -static_cast<int>(z * k + 0.5f);
    return { static_cast<float>(rx - offX), static_cast<float>(ry - offY) };
}

ZoneMaps::ZoneMaps(const DatIndex& index, const std::filesystem::path& dll)
: index_(index)
{
    try
    {
        // 14-byte records, ending at the first record whose scale byte (+5) is 0. Fingerprint: the first 16 records.
        const auto file = readFile(dll);
        const auto at   = findFingerprintOffset(file, 14 * 16, 12469, "c121f38242f4e1bb44ec9523cdaf699e27c5c775801bd81e7a78d9a28e8dc9f7");
        if (!at)
        {
            spdlog::warn("zone maps: map table not found in {}", dll.string());
            return;
        }
        for (std::size_t o = *at; o + 14 <= file.size() && file[o + 5] != 0; o += 14)
        {
            MapRecord r;
            std::memcpy(&r.zone, &file[o], 2);
            r.mapId  = static_cast<int8_t>(file[o + 2]);
            r.listed = file[o + 3];
            r.bank   = file[o + 4] & 0x0F;
            r.scale  = static_cast<int8_t>(file[o + 5]);
            std::memcpy(&r.keyItem, &file[o + 6], 2);
            std::memcpy(&r.file, &file[o + 8], 2);
            std::memcpy(&r.offX, &file[o + 10], 2);
            std::memcpy(&r.offY, &file[o + 12], 2);
            records_.push_back(r);
        }
        spdlog::info("zone maps: {} map records", records_.size());
    }
    catch (const std::exception& e)
    {
        spdlog::warn("zone maps: {}", e.what());
    }
}

const MapRecord* ZoneMaps::find(uint16_t zone, int8_t mapId) const
{
    for (const auto& r : records_)
    {
        if (static_cast<uint16_t>(r.zone) == zone && r.mapId == mapId) // pictures have negative "zones"
        {
            return &r;
        }
    }
    return nullptr;
}

std::optional<Texture> ZoneMaps::load(const MapRecord& r) const
{
    const auto         file = index_.read(r.fileId());
    std::vector<Chunk> chunks;
    parseChunks(file, chunks);
    for (const auto& c : chunks)
    {
        if (c.type == 0x20)
        {
            try
            {
                return parseTexture(c.payload);
            }
            catch (const std::exception& e)
            {
                spdlog::warn("zone map {}: {}", r.zone, e.what());
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

} // namespace xi::dat
