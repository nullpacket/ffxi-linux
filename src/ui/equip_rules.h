// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/items.h"

#include <cstdint>
#include <optional>

// Equipment and item-list rules of the game's menus (goal #45; docs/ui-format-notes.md §10.12).
namespace xi::ui
{

// What the game checks before an item counts as wearable (CODE 0x10131f80): the race bit (own race from the look),
// the main job's bit, the main job's level, the superior level, and no slot the item covers blocked by encumbrance
// (S2C 0x01B +0x60, low 16 bits).
struct EquipContext
{
    uint8_t  mainJob       = 0;
    uint8_t  mainLevel     = 0;
    uint8_t  race          = 0;
    uint8_t  superiorLevel = 0;
    uint16_t blockedSlots  = 0;
};

[[nodiscard]] inline bool canEquip(const dat::ItemInfo& it, const EquipContext& c)
{
    return it.equipment() && ((it.races >> c.race) & 1) && ((it.jobs >> c.mainJob) & 1) && it.level <= c.mainLevel &&
           it.superiorLevel <= c.superiorLevel && (it.slots & c.blockedSlots) == 0;
}

// Whether the item goes in equip slot `slot` (CODE 0x101320b0): its slot mask, with none of its slots blocked.
[[nodiscard]] inline bool fitsSlot(const dat::ItemInfo& it, uint8_t slot, uint16_t blockedSlots)
{
    return it.equipment() && (it.slots & blockedSlots) == 0 && ((it.slots >> slot) & 1);
}

// Weapon skills the game counts as two-handed for the sub slot (CODE 0x1015a9a0): hand-to-hand, great sword, great
// axe, scythe, polearm, great katana, staff.
[[nodiscard]] inline bool twoHandedSkill(uint8_t skill)
{
    return skill == 1 || skill == 4 || skill == 6 || skill == 7 || skill == 8 || skill == 10 || skill == 12;
}

// A weapon that can also go in the main hand, put in the sub slot (CODE 0x10132700): the game's own refusals, as
// d_msg 55646 lines: 139 no dual-wield job (NIN, THF, DNC or BLU as main or support), 152 no main weapon, 153 a
// two-handed main weapon. 0 = allowed. Shields and grips (no main-hand bit) are not checked here.
[[nodiscard]] inline int subWeaponRefusal(const dat::ItemInfo& it, const std::optional<dat::ItemInfo>& main, uint8_t mainJob, uint8_t subJob)
{
    if (it.type != 4 || !(it.slots & 1))
    {
        return 0;
    }
    auto dualWield = [](uint8_t j) { return j == 13 || j == 6 || j == 19 || j == 16; };
    if (!dualWield(mainJob) && !dualWield(subJob))
    {
        return 139;
    }
    if (!main)
    {
        return 152;
    }
    if (twoHandedSkill(main->skill))
    {
        return 153;
    }
    return 0;
}

// The equipment list's row colours (CODE 0x10130470, 0x80RRGGBB with 0x80 = 1.0): worn green, wearable white, not
// wearable (or in a wardrobe the account has not unlocked) grey.
enum class EquipRow
{
    Worn,
    Wearable,
    NotWearable,
};

// The Items list's row colours (CODE 0x101f0290): worn gear and the worn linkshells green, things the menu can use
// (flag 0x0400 on a non-equipment item, crystals, item types 0x16 / 0x17) yellow, the rest white.
enum class ItemRow
{
    Normal,
    Worn,
    Usable,
};

[[nodiscard]] inline ItemRow itemRow(const dat::ItemInfo& it, bool worn)
{
    if (worn)
    {
        return ItemRow::Worn;
    }
    const bool menuUse = !it.equipment() && (it.flags & 0x0400) != 0;
    return menuUse || it.type == 8 || it.type == 0x16 || it.type == 0x17 ? ItemRow::Usable : ItemRow::Normal;
}

// The item info window's second line (CODE 0x1013cdf0): weapons the skill label then the races label, armour the slot
// label then the races label, as indices into d_msg 55653 (-1 = none). Races (0x1015ad10): an exact mask first, else
// the first entry sharing a bit. Slots (0x1015ac00) and skills (0x1015a9e0) by table order.
[[nodiscard]] inline int raceLabel(uint16_t races)
{
    static constexpr uint16_t kMasks[] = { 0x1FE, 0x12A, 0xD4, 0x06, 0x02, 0x04, 0x18, 0x08, 0x10, 0x60, 0x20, 0x40, 0x100, 0x80 };
    for (int i = 0; i < 14; ++i)
    {
        if (kMasks[i] == races)
        {
            return i;
        }
    }
    for (int i = 0; i < 14; ++i)
    {
        if (kMasks[i] & races)
        {
            return i;
        }
    }
    return -1;
}

[[nodiscard]] inline int slotLabel(uint16_t slots)
{
    static constexpr uint16_t kMasks[] = { 0x10, 0x20, 0x80, 0x40, 0x2000, 0x800, 0x200, 0x8000, 0x400, 0x100 };
    for (int i = 0; i < 10; ++i)
    {
        if (kMasks[i] & slots)
        {
            return 14 + i;
        }
    }
    return -1;
}

[[nodiscard]] inline int skillLabel(uint8_t skill)
{
    static constexpr uint8_t kSkills[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 25, 26, 27, 41, 42, 45, 48 };
    for (int i = 0; i < 19; ++i)
    {
        if (kSkills[i] == skill)
        {
            return 24 + i;
        }
    }
    return -1;
}

// The Rare / Ex / temporary marks right of the info window's name (CODE 0x1013cb80; "menu windowps" shapes from the
// table at 0x1036fa2c): 293 the exdata mark (not drawn here), 294 Rare (flag 0x8000), 295 Ex (0x4000), 296 an item in
// the temporary items (container 3), in that order.
struct InfoMarks
{
    bool rare = false, ex = false, temporary = false;
};

[[nodiscard]] inline InfoMarks infoMarks(const dat::ItemInfo& it, uint8_t container)
{
    return { (it.flags & 0x8000) != 0, (it.flags & 0x4000) != 0, container == 3 };
}

} // namespace xi::ui
