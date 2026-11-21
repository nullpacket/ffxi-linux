// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"
#include "dat/texture.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Item names and equipment data (docs/dat-format.md §12).
namespace xi::dat
{

struct ItemInfo
{
    uint16_t    id = 0;
    std::string name;
    std::string logName;   // as used in messages: "scroll of Cure" (singular), "scrolls of Cure" (plural)
    std::string logPlural;
    std::string description; // help text; lines separated by '\n' (record field 4)
    // Record +0x04, raw: 0x8000 Rare and 0x4000 Ex (the item info window's marks, CODE 0x1013cb80), 0x0400 usable from
    // the menu (0x1015b5f0), 0x1000 no sale as in LSB.
    uint16_t    flags = 0;
    uint16_t    stackSize = 1; // record +0x06
    uint16_t    type  = 0; // record +0x08: 4 weapon, 5 armour, 7 usable, 8 crystal, ...
    uint16_t    targets = 0; // record +0x0C: valid targets (0x01 self, 0x02 player, 0x04 party, ... as spells)
    uint16_t    level = 0; // equipment only
    uint16_t    slots = 0; // equipment only: bit n = equip slot n (0 main, 1 sub, 2 ranged, 3 ammo, 4 head, 5 body, ...)
    uint16_t    races = 0; // equipment only (+0x12): bit n = race n (1 Hume m, 2 Hume f, 3 / 4 Elvaan, 5 / 6 Tarutaru, 7 Mithra, 8 Galka)
    uint32_t    jobs  = 0; // equipment only: bit n = job n
    uint16_t    superiorLevel = 0; // equipment only (+0x18)
    uint8_t     skill = 0;  // weapons only (+0x22): 1 hand-to-hand .. 12 staff, 25 archery ...; 0 = a grip
    uint8_t     maxCharges = 0; // enchanted equipment: armour +0x1C, weapons +0x28
    uint32_t    recast = 0;     // enchanted equipment: seconds between uses (armour +0x20, weapons +0x2C)
    [[nodiscard]] bool equipment() const
    {
        return type == 4 || type == 5;
    }
    [[nodiscard]] bool usable() const
    {
        return type == 7;
    }
};

// Item records are 0xC00 bytes, each byte rotated left by 3 (right by 5), in files that each hold a contiguous id
// range. Records are read one at a time on first use and cached. Thread-safe.
class ItemTable
{
public:
    explicit ItemTable(const DatIndex& index);

    // Nothing for ids outside every item file.
    [[nodiscard]] std::optional<ItemInfo> find(uint16_t id) const;

    // The item's 32x32 icon (record +0x280: u32 size, then a texture payload as in chunk type 0x20, §12).
    [[nodiscard]] std::optional<Texture> icon(uint16_t id) const;

    // Decodes one record (exposed for tests).
    static ItemInfo decode(std::span<const std::byte> record);

private:
    [[nodiscard]] std::vector<std::byte> record(uint16_t id) const; // empty when out of range
    struct Range
    {
        uint32_t fileId = 0;
        uint32_t first  = 0;
        uint32_t count  = 0;
    };
    const DatIndex&                                index_;
    std::vector<Range>                             ranges_;
    mutable std::mutex                             mutex_;
    mutable std::unordered_map<uint16_t, ItemInfo> cache_;
};

// Status effect icons (file 87: 0x1800-byte records indexed by icon id, icon at +0x280 as in item records; §14).
// From file 87 (the status icon set "type two", the default), or `file` 39557 ("type one": Config Misc. icon
// pattern, cfg 153 bit 2; config_ui2_re.md §2); 0x1800-byte records.
std::optional<Texture> statusIcon(const DatIndex& index, uint16_t iconId, uint32_t file = 87);

// A status record's header (file 87, the first 0x1FC bytes of the 0x1800-byte record, bit-rotated by decodeDataBlock):
// +0 u16 status id, +2 u8 "can cancel" (the buff window's Confirm sends C2S 0x0F1 only when it is set; CODE 0x10227a13
// via 0x10226c50), +3 u8 0xFF = no timer text (0x10226c60), +4 the description's string table (§14).
struct StatusInfo
{
    uint16_t    id        = 0;
    bool        canCancel = false;
    bool        hideTimer = false;
    std::string description; // "Defense is enhanced."
};
std::optional<StatusInfo> statusInfo(const DatIndex& index, uint16_t statusId);
// Decodes a record header in place (exposed for tests).
std::optional<StatusInfo> decodeStatusInfo(std::span<std::byte> header);

// The game's data-block decoder (CODE 0x1021e3d0, used on status records with length 0x1FC): the rotation comes from
// bit counts of bytes 2, 11 and 12, n = popcount(b2) - popcount(b11) + popcount(b12), |n % 5| selects rotate left 1,
// right 1, left 2, right 2, left 3; every byte except 2, 11 and 12 is rotated.
void decodeDataBlock(std::span<std::byte> data);

} // namespace xi::dat
