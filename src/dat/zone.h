// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/collision.h"
#include "dat/dat_index.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xi::dat
{

// The two 256-byte tables the client uses to de-obfuscate zone data (docs/dat-format.md §5). They are read
// from the user's own FFXiMain.dll — never shipped with this project — and located by SHA-256 fingerprint,
// so any client build that carries them works (verified: lsb-dev retail build and HorizonXI build).
struct ZoneKeys
{
    std::array<uint8_t, 256> xorTable{};  // FFXiMain 0x10376ac0 (lsb-dev build)
    std::array<uint8_t, 256> swapTable{}; // FFXiMain 0x10376de8

    static ZoneKeys fromDll(const std::filesystem::path& ffxiMainDll);
};

// In-place de-obfuscation of chunk payloads (§5.2, §5.4).
void decodeMmb(std::span<std::byte> payload, const ZoneKeys& keys);
void decodeMzb(std::span<std::byte> payload, const ZoneKeys& keys);

struct MeshVertex
{
    float    pos[3];
    float    normal[3];
    uint32_t color; // D3DCOLOR (B, G, R, A bytes); 0x80 = full intensity
    float    uv[2];
};

struct MeshPiece
{
    std::string             texture; // 16-char name as stored; all spaces = untextured
    uint8_t                 flags = 0; // piece flag bits 28..31 (meaning not yet known)
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t>   triangles; // triangle list (strips already expanded, degenerates removed)
};

struct Model
{
    std::string            name; // 16-char model name
    std::vector<MeshPiece> pieces;
};

struct Placement
{
    std::string name; // 16-char Mmb name
    float       pos[3];
    float       rot[3]; // radians, applied X then Y then Z
    float       scale[3];
    // Point lights on this mesh: 1-based indices into ZoneData::lightKeys, 0 = none (record +0x54..+0x60; CODE
    // 0x1017cab8; effect-format §Z point lights).
    std::array<uint8_t, 4> lights{};
    // Door id (record +0x34, a fourcc such as `_6w0`; 0 = none): the placements of a door share it, its parts in
    // record order (CODE 0x1017a03f / 0x1017a860; docs/dat-format.md §5 doors). The 0x00E DoorId names it.
    uint32_t doorId = 0;
    // Record +0x38 / +0x3C / +0x40 (CODE 0x1017a03f stores them squared, the second as max(d1, d2)): the high mesh is
    // drawn within d1 of the camera, the medium one within max(d1, d2), the low one beyond (0x1017c8d0); d3 scales the
    // draw distance for placements that set instance +0xb (INFERRED which).
    float lodDist[3]{};
};

// LOD variants (CODE 0x1017aef0, dat-format §6.3): a placement / model name whose last character above a space is
// `h`, `m` or `l` names one of three meshes; lodVariant swaps that character (nullopt when the name has none).
// The game draws <stem>h near, <stem>m in between and <stem>l far, each falling back to the others that exist.
std::optional<std::string> lodVariant(const std::string& name, char which);
// The name with its LOD character blanked (equal for every variant of one model), or nullopt.
inline std::optional<std::string> lodStem(const std::string& name)
{
    return lodVariant(name, '?');
}

// Parse a de-obfuscated Mmb / Mzb payload.
Model                  parseMmb(std::span<const std::byte> payload);
std::vector<Placement> parseMzb(std::span<const std::byte> payload);
// The Mzb's point-light keys (header +0x18 table), in index order.
std::vector<uint32_t> parseMzbLights(std::span<const std::byte> payload);

// Zone-line trigger from the zone's Rid chunk (type 54), DATA-derived (docs/dat-format.md §8):
// 64-byte records from payload+0x30; +0x10 centre x/y/z, +0x20 yaw (radians), +0x28 size x/y/z,
// +0x34 fourcc id (sent as RectID in C2S 0x05E), +0x38 fourcc of the arrival point in the target zone.
struct ZoneLine
{
    uint32_t id      = 0; // fourcc as stored (little-endian u32), e.g. "z6e4"
    uint32_t arrival = 0;
    float    pos[3]{};
    float    yaw = 0.0f;
    float    size[3]{};

    [[nodiscard]] std::string name() const
    {
        return std::string(reinterpret_cast<const char*>(&id), 4);
    }
    // Point (game coordinates) inside the trigger box.
    [[nodiscard]] bool contains(float x, float y, float z) const;
    // Entered between two game-space positions: either end inside, or the step crosses the box (CODE 0x10175820).
    [[nodiscard]] bool crossedBy(const float* from, const float* to) const;
};

std::vector<ZoneLine> parseRid(std::span<const std::byte> payload);
// The Rid's door collision boxes: records whose id starts with '_' (§5.8; same fields as ZoneLine, y = the centre).
std::vector<ZoneLine> parseRidDoorBoxes(std::span<const std::byte> payload);
// A lift's Rid region (id starting with '@'): the box riders stand in, and the platform height (game y) of each state:
// centre y + s16 / 256 from code-record +0x34, +0x36 ... (§5.8; CODE 0x1017abe0).
struct LiftRegion
{
    ZoneLine           box;
    std::vector<float> heights;
};
std::vector<LiftRegion> parseRidLifts(std::span<const std::byte> payload);

// Mog House furniture (RE notes furniture_re.md): the `furn` table (file 32922, 1024 rows {u16 flags, u16 item,
// u8 w, u8 d, u16 height / 100}; row = item, or item - 0xC00 from 0xE00) and the item's model file (CODE 0x101cc9b0).
struct FurnitureRow
{
    uint16_t flags = 0, itemId = 0; // flags: 0 floor, 1 wall, 2 surface, 4 on a table, 12 flowerpot
    uint8_t  w = 0, d = 0;          // footprint in 0.6-unit cells
    uint16_t height = 0;            // 0.01 units
};
std::vector<FurnitureRow> loadFurnitureTable(const DatIndex& index);
std::optional<uint32_t>   furnitureFileId(uint16_t itemId);
// A static model placed into a zone (furniture): the file's Mmb (`fune`, else `_fun`, else the first), at a game-space
// position turned `yaw` about the vertical.
struct ZoneProp
{
    uint32_t fileId = 0;
    float    pos[3]{};
    float    yaw = 0;
};

struct ZoneData
{
    std::vector<ZoneLine>          zoneLines;
    std::vector<ZoneLine>          doorBoxes; // Rid door boxes (§5.8), solid while the door is closed
    std::vector<LiftRegion>        lifts;     // Rid lift regions (§5.8)
    std::vector<CollisionTriangle> collision; // world-space, game coordinates
    std::unordered_map<std::string, Model> models; // by model name (first wins on duplicates)
    // Mmb chunk name (4 chars, e.g. `sea0`) -> its model name: zone effects draw Mmbs by chunk name
    // (effect-format §G zone effects, CODE 0x100494a0).
    std::unordered_map<std::string, std::string> modelByChunk;
    // The Mzb's point-light table (header +0x18: up to 256 records of 0x4c, the key = the light generator's 4-char
    // name, e.g. `pl03`); placements index it.
    std::vector<uint32_t> lightKeys;
    std::vector<Placement>                 placements;
    std::vector<std::byte>                 file; // raw DAT (textures are parsed from it by the caller)
};

// The zone model DAT for a model id (S2C 0x00A: MapNumber + 0x1BC from 0x100, or the Mog House's MyroomMapNumber):
// id + 100 below 600, else id + 83191 (CODE 0x10178ca0; DATA: 700 = Western Adoulin -> 83891 `_256`, 0x121 = San
// d'Oria Mog House -> 389). zoneModelId maps a zone id (= MapNumber outside a Mog House) to its model id.
uint32_t zoneModelFileId(uint16_t model);
inline uint16_t zoneModelId(uint16_t zone)
{
    return zone >= 0x100 ? static_cast<uint16_t>(zone + 0x1BC) : zone;
}

// Per-zone entity name table (DATA, verified on zone 230): file 6720 + zone for zones 0..255, 86235 + zone for 256..303, records of
// char name[28] + u32 entity id. The client names NPCs/mobs from this; the server often sends no name.
std::unordered_map<uint32_t, std::string> loadEntityNames(const DatIndex& index, uint16_t zone);
ZoneData loadZone(const DatIndex& index, uint16_t model, const ZoneKeys& keys); // model: zoneModelId(zone) or a Mog House

} // namespace xi::dat
