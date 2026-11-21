// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/zone.h"

#include "dat/dll_data.h"

#include "dat/chunk.h"
#include "xi/bytes.h"

#include <openssl/evp.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace xi::dat
{

namespace
{

// SHA-256 and byte sum of each table (the sum is a cheap prefilter for the scan).
constexpr const char* kXorSha  = "73e2b91df64415f36cba96aa3eb1dfa6f7f15d4a3db3e57cb69b01036868ff8c";
constexpr uint32_t    kXorSum  = 31091;
constexpr const char* kSwapSha = "5e740b2dc54c4490fd3418fddc70261e9ec0bc5a4909fc8b2913f07dd128e558";
constexpr uint32_t    kSwapSum = 31638;

// Finds the 256-byte window with the given fingerprint (rolling byte sum, then SHA-256 to confirm).
bool findTable(const std::vector<uint8_t>& file, uint32_t sum, const char* sha, std::array<uint8_t, 256>& out)
{
    const auto t = findFingerprint(file, 256, sum, sha);
    if (t)
    {
        std::copy_n(t->begin(), 256, out.begin());
    }
    return t.has_value();
}

uint8_t u8(std::span<std::byte> p, std::size_t i)
{
    return std::to_integer<uint8_t>(p[i]);
}

std::string name16(std::span<const std::byte> p, std::size_t off)
{
    return std::string(reinterpret_cast<const char*>(p.data() + off), 16);
}

} // namespace

std::optional<std::string> lodVariant(const std::string& name, char which)
{
    for (std::size_t i = std::min<std::size_t>(name.size(), 16); i-- > 1;)
    {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c > 0x20)
        {
            if (c != 'h' && c != 'm' && c != 'l')
            {
                return std::nullopt;
            }
            std::string out = name;
            out[i]          = which;
            return out;
        }
    }
    return std::nullopt;
}

ZoneKeys ZoneKeys::fromDll(const std::filesystem::path& dll)
{
    std::ifstream f(dll, std::ios::binary);
    if (!f)
    {
        throw std::runtime_error(fmt::format("cannot open {}", dll.string()));
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ZoneKeys k;
    if (!findTable(file, kXorSum, kXorSha, k.xorTable) || !findTable(file, kSwapSum, kSwapSha, k.swapTable))
    {
        throw std::runtime_error(fmt::format("{}: zone key tables not found (unsupported client build?)", dll.string()));
    }
    return k;
}

void decodeMmb(std::span<std::byte> p, const ZoneKeys& keys)
{
    if (p.size() < 8)
    {
        throw std::runtime_error("mmb: payload too small");
    }
    const std::size_t len = std::min<std::size_t>(load<uint32_t>(p, 0) & 0xFFFFFF, p.size());
    if (u8(p, 3) >= 5)
    {
        uint32_t key = keys.xorTable[u8(p, 5) ^ 0xF0];
        uint32_t c   = 0;
        for (std::size_t i = 8; i < len; ++i)
        {
            const uint32_t x = key & 0xFF;
            const uint32_t w = (x << 8) | x;
            key += ++c;
            p[i] ^= std::byte{ static_cast<uint8_t>((w >> (key & 7)) & 0xFF) };
            key += ++c;
        }
    }
    if (u8(p, 6) == 0xFF && u8(p, 7) == 0xFF)
    {
        // Block swap stage: from the client code, not yet observed in the data.
        uint8_t           k3   = u8(p, 5) ^ 0xF0;
        uint8_t           k2   = keys.swapTable[k3];
        const std::size_t half = ((len - 8) & ~std::size_t{ 0xF }) / 2;
        for (std::size_t pos = 0; pos < half; pos += 8)
        {
            k3             = static_cast<uint8_t>(k3 + 9);
            const bool bit = k2 & 1;
            k2             = static_cast<uint8_t>(k2 + k3);
            if (bit)
            {
                std::swap_ranges(p.begin() + static_cast<std::ptrdiff_t>(8 + pos), p.begin() + static_cast<std::ptrdiff_t>(16 + pos),
                                 p.begin() + static_cast<std::ptrdiff_t>(8 + pos + half));
            }
        }
    }
}

void decodeMzb(std::span<std::byte> p, const ZoneKeys& keys)
{
    if (p.size() < 8 || u8(p, 3) < 0x1B)
    {
        return;
    }
    const std::size_t len = std::min<std::size_t>(load<uint32_t>(p, 0) & 0xFFFFFF, p.size()) - 8;
    uint32_t          key = keys.xorTable[u8(p, 7) ^ 0xFF];
    uint32_t          c   = 0;
    for (std::size_t pos = 0; pos < len;)
    {
        const uint32_t    k = key & 0xFF;
        key += ++c;
        const std::size_t n = ((k >> 4) & 7) + 16;
        if ((k & 1) && pos + n < len)
        {
            for (std::size_t j = 8 + pos; j < 8 + pos + n; ++j)
            {
                p[j] = ~p[j];
            }
        }
        pos += n;
    }
}

Model parseMmb(std::span<const std::byte> p)
{
    Model      m;
    const auto version = load<uint8_t>(p, 3);
    const auto vtype   = load<uint8_t>(p, 4);
    m.name             = name16(p, 0x10);
    uint32_t count     = version <= 1 ? 9 : load<uint32_t>(p, 0x20);
    const std::size_t stride    = vtype == 2 ? 0x30 : 0x24;
    const std::size_t normalOff = vtype == 2 ? 0x18 : 0x0C;
    const bool        strip     = vtype == 1 || vtype == 3;

    for (uint32_t s = 0; s < count; ++s)
    {
        const auto off = load<uint32_t>(p, 0x3C + 4 * s);
        if (off == 0)
        {
            continue;
        }
        const auto pieces = load<uint32_t>(p, off);
        std::size_t q     = off + 0x20;
        for (uint32_t k = 0; k < pieces; ++k)
        {
            MeshPiece pc;
            pc.texture        = name16(p, q);
            const auto w      = load<uint32_t>(p, q + 16);
            const auto nv     = w & 0x0FFFFFFF;
            pc.flags          = static_cast<uint8_t>(w >> 28);
            const std::size_t vb = q + 20;
            pc.vertices.resize(nv);
            for (uint32_t v = 0; v < nv; ++v)
            {
                const std::size_t o  = vb + v * stride;
                auto&             mv = pc.vertices[v];
                for (int a = 0; a < 3; ++a)
                {
                    mv.pos[a]    = load<float>(p, o + 4 * static_cast<std::size_t>(a));
                    mv.normal[a] = load<float>(p, o + normalOff + 4 * static_cast<std::size_t>(a));
                }
                mv.color = load<uint32_t>(p, o + normalOff + 12);
                mv.uv[0] = load<float>(p, o + normalOff + 16);
                mv.uv[1] = load<float>(p, o + normalOff + 20);
            }
            const std::size_t ib = vb + nv * stride;
            const auto        ni = load<uint32_t>(p, ib);
            std::vector<uint16_t> idx(ni);
            for (uint32_t i = 0; i < ni; ++i)
            {
                idx[i] = load<uint16_t>(p, ib + 4 + 2 * i);
                if (idx[i] >= nv)
                {
                    throw std::runtime_error(fmt::format("mmb {}: index {} out of range ({} vertices)", m.name, idx[i], nv));
                }
            }
            if (strip)
            {
                for (std::size_t i = 0; i + 2 < idx.size(); ++i)
                {
                    const uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
                    if (a == b || b == c || a == c)
                    {
                        continue;
                    }
                    if (i % 2 == 0)
                    {
                        pc.triangles.insert(pc.triangles.end(), { a, b, c });
                    }
                    else
                    {
                        pc.triangles.insert(pc.triangles.end(), { a, c, b });
                    }
                }
            }
            else
            {
                for (std::size_t i = 0; i + 2 < idx.size(); i += 3)
                {
                    pc.triangles.insert(pc.triangles.end(), { idx[i], idx[i + 1], idx[i + 2] });
                }
            }
            q = ib + 4 + ((ni + 1) & ~1u) * 2;
            m.pieces.push_back(std::move(pc));
        }
    }
    return m;
}

std::vector<uint32_t> parseMzbLights(std::span<const std::byte> p)
{
    std::vector<uint32_t> keys;
    const auto            off = p.size() >= 0x1C ? load<uint32_t>(p, 0x18) : 0u;
    for (std::size_t k = 0; off != 0 && k < 256 && off + (k + 1) * 0x4C <= p.size(); ++k)
    {
        const auto key = load<uint32_t>(p, off + k * 0x4C);
        if (key == 0)
        {
            break;
        }
        keys.push_back(key);
    }
    return keys;
}

std::vector<Placement> parseMzb(std::span<const std::byte> p)
{
    const auto             version = load<uint8_t>(p, 3);
    const auto             n       = load<uint32_t>(p, 4) & 0xFFFFFF;
    std::vector<Placement> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const std::size_t r = 0x20 + static_cast<std::size_t>(i) * 100;
        Placement         pl;
        pl.name = name16(p, r);
        if (version > 0x1A)
        {
            for (auto& ch : pl.name)
            {
                ch = static_cast<char>(ch ^ 0x55);
            }
        }
        for (int a = 0; a < 3; ++a)
        {
            pl.pos[a]   = load<float>(p, r + 0x10 + 4 * static_cast<std::size_t>(a));
            pl.rot[a]   = load<float>(p, r + 0x1C + 4 * static_cast<std::size_t>(a));
            pl.scale[a] = load<float>(p, r + 0x28 + 4 * static_cast<std::size_t>(a));
        }
        pl.doorId = load<uint32_t>(p, r + 0x34);
        for (std::size_t k = 0; k < 3; ++k)
        {
            pl.lodDist[k] = load<float>(p, r + 0x38 + 4 * k);
        }
        if (r + 0x64 <= p.size())
        {
            for (std::size_t k = 0; k < 4; ++k)
            {
                pl.lights[k] = static_cast<uint8_t>(std::min<uint32_t>(load<uint32_t>(p, r + 0x54 + 4 * k), 255));
            }
        }
        out.push_back(std::move(pl));
    }
    return out;
}

std::unordered_map<uint32_t, std::string> loadEntityNames(const DatIndex& index, uint16_t zone)
{
    std::unordered_map<uint32_t, std::string> names;
    // 6720 + zone below 256; zones 256..303 (Adoulin onwards) at 86235 + zone (DATA: 86491 holds zone 256's ids).
    const uint32_t file = zone < 256 ? 6720u + zone : 86235u + zone;
    const auto     path = index.path(file);
    if (!path)
    {
        return names;
    }
    const auto data = index.read(file);
    for (std::size_t off = 0; off + 32 <= data.size(); off += 32)
    {
        const auto id   = load<uint32_t>(data, off + 28);
        auto       name = loadString(data, off, 28);
        if (id != 0 && !name.empty())
        {
            names.emplace(id, std::move(name));
        }
    }
    return names;
}

bool ZoneLine::contains(float x, float y, float z) const
{
    // CODE 0x10175820: into the box's frame (T(-centre) * RotY(-yaw) * S(1 / size)), inside when -0.5 <= l < 0.5 on all
    // three axes: centred, half the stored size, no padding.
    const float dx = x - pos[0], dy = y - pos[1], dz = z - pos[2];
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float lx = (dx * c - dz * s) / std::max(std::abs(size[0]), 1e-4f);
    const float ly = dy / std::max(std::abs(size[1]), 1e-4f);
    const float lz = (dx * s + dz * c) / std::max(std::abs(size[2]), 1e-4f);
    return lx >= -0.5f && lx < 0.5f && ly >= -0.5f && ly < 0.5f && lz >= -0.5f && lz < 0.5f;
}

bool ZoneLine::crossedBy(const float* from, const float* to) const
{
    // Either end inside, or the step between them passes through the box (a fast step over a thin box; CODE 0x101767b0
    // tests the segment against the unit cube's faces). Slab test in the box's unit frame.
    if (contains(from[0], from[1], from[2]) || contains(to[0], to[1], to[2]))
    {
        return true;
    }
    const float c = std::cos(yaw), s = std::sin(yaw);
    auto local = [&](const float* p, float* l) {
        const float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
        l[0]           = (dx * c - dz * s) / std::max(std::abs(size[0]), 1e-4f);
        l[1]           = dy / std::max(std::abs(size[1]), 1e-4f);
        l[2]           = (dx * s + dz * c) / std::max(std::abs(size[2]), 1e-4f);
    };
    float a[3], b[3];
    local(from, a);
    local(to, b);
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 3; ++k)
    {
        const float d = b[k] - a[k];
        if (std::abs(d) < 1e-9f)
        {
            if (a[k] < -0.5f || a[k] >= 0.5f)
            {
                return false;
            }
            continue;
        }
        float ta = (-0.5f - a[k]) / d, tb = (0.5f - a[k]) / d;
        if (ta > tb)
        {
            std::swap(ta, tb);
        }
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
        {
            return false;
        }
    }
    return true;
}

std::vector<ZoneLine> parseRidDoorBoxes(std::span<const std::byte> p)
{
    std::vector<ZoneLine> boxes;
    for (std::size_t r = 0x30; r + 64 <= p.size(); r += 64)
    {
        ZoneLine b;
        b.id = load<uint32_t>(p, r + 0x34);
        if ((b.id & 0xFF) != '_')
        {
            continue;
        }
        for (int a = 0; a < 3; ++a)
        {
            b.pos[a]  = load<float>(p, r + 0x10 + 4 * static_cast<std::size_t>(a));
            b.size[a] = load<float>(p, r + 0x28 + 4 * static_cast<std::size_t>(a));
        }
        b.yaw = load<float>(p, r + 0x20);
        boxes.push_back(b);
    }
    return boxes;
}

std::vector<LiftRegion> parseRidLifts(std::span<const std::byte> p)
{
    std::vector<LiftRegion> lifts;
    for (std::size_t r = 0x30; r + 64 <= p.size(); r += 64)
    {
        LiftRegion l;
        l.box.id = load<uint32_t>(p, r + 0x34);
        if ((l.box.id & 0xFF) != '@')
        {
            continue;
        }
        for (int a = 0; a < 3; ++a)
        {
            l.box.pos[a]  = load<float>(p, r + 0x10 + 4 * static_cast<std::size_t>(a));
            l.box.size[a] = load<float>(p, r + 0x28 + 4 * static_cast<std::size_t>(a));
        }
        l.box.yaw = load<float>(p, r + 0x20);
        // Code record = this table's r + 0x10: its +0x34 / +0x36 are r + 0x44 / r + 0x46 (two states seen; DATA zone
        // 237 `@6l0`: 1.962 bottom, -9.983 top).
        for (std::size_t k = 0; k < 2 && r + 0x48 <= p.size(); ++k)
        {
            l.heights.push_back(l.box.pos[1] + static_cast<float>(load<int16_t>(p, r + 0x44 + 2 * k)) / 256.0f);
        }
        lifts.push_back(std::move(l));
    }
    return lifts;
}

std::vector<ZoneLine> parseRid(std::span<const std::byte> p)
{
    std::vector<ZoneLine> lines;
    for (std::size_t r = 0x30; r + 64 <= p.size(); r += 64)
    {
        ZoneLine l;
        l.id      = load<uint32_t>(p, r + 0x34);
        l.arrival = load<uint32_t>(p, r + 0x38);
        // Zone lines: id starts with 'z' and names an arrival point. ('m...' records and arrival points are skipped.)
        if ((l.id & 0xFF) != 'z' || l.arrival == 0)
        {
            continue;
        }
        for (int a = 0; a < 3; ++a)
        {
            l.pos[a]  = load<float>(p, r + 0x10 + 4 * static_cast<std::size_t>(a));
            l.size[a] = load<float>(p, r + 0x28 + 4 * static_cast<std::size_t>(a));
        }
        l.yaw = load<float>(p, r + 0x20);
        lines.push_back(l);
    }
    return lines;
}

uint32_t zoneModelFileId(uint16_t model)
{
    return model >= 600 ? model + 83191u : model + 100u;
}

std::vector<FurnitureRow> loadFurnitureTable(const DatIndex& index)
{
    std::vector<FurnitureRow> rows;
    if (!index.path(32922))
    {
        return rows;
    }
    const auto         file = index.read(32922);
    std::vector<Chunk> chunks;
    parseChunks(file, chunks);
    for (const auto& c : chunks)
    {
        if (c.type == 0x59 && c.name == "furn")
        {
            for (std::size_t at = 0; at + 8 <= c.payload.size(); at += 8)
            {
                rows.push_back({ load<uint16_t>(c.payload, at), load<uint16_t>(c.payload, at + 2), load<uint8_t>(c.payload, at + 4),
                                 load<uint8_t>(c.payload, at + 5), load<uint16_t>(c.payload, at + 6) });
            }
        }
    }
    return rows;
}

std::optional<uint32_t> furnitureFileId(uint16_t id)
{
    if (id < 0x100) return 32927u + id;
    if (id < 0x200) return 51183u + id;
    if (id >= 0xE00 && id < 0x1000) return 59227u + id;
    return std::nullopt;
}

ZoneData loadZone(const DatIndex& index, uint16_t model, const ZoneKeys& keys)
{
    ZoneData z;
    z.file = index.read(zoneModelFileId(model));
    std::vector<Chunk> chunks;
    parseChunks(z.file, chunks);
    for (const auto& c : chunks)
    {
        if (c.type == 54)
        {
            auto l = parseRid(c.payload);
            z.zoneLines.insert(z.zoneLines.end(), l.begin(), l.end());
            const auto d = parseRidDoorBoxes(c.payload);
            z.doorBoxes.insert(z.doorBoxes.end(), d.begin(), d.end());
            auto lf = parseRidLifts(c.payload);
            z.lifts.insert(z.lifts.end(), std::make_move_iterator(lf.begin()), std::make_move_iterator(lf.end()));
            continue;
        }
        if (c.type != 46 && c.type != 28)
        {
            continue;
        }
        std::vector<std::byte> payload(c.payload.begin(), c.payload.end());
        if (c.type == 46)
        {
            decodeMmb(payload, keys);
            auto m = parseMmb(payload);
            z.modelByChunk.try_emplace(c.name, m.name);
            z.models.try_emplace(m.name, std::move(m));
        }
        else if (c.name != "fune") // CMoMzb_Init skips the 'fune' chunk
        {
            decodeMzb(payload, keys);
            if (z.collision.empty())
            {
                z.collision = parseCollision(payload); // first Mzb with collision (§7.1)
            }
            auto pl = parseMzb(payload);
            if (z.lightKeys.empty())
            {
                z.lightKeys = parseMzbLights(payload); // zones have one Mzb with lights (DATA); its placements index it
            }
            z.placements.insert(z.placements.end(), pl.begin(), pl.end());
        }
    }
    return z;
}

} // namespace xi::dat
