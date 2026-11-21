// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Spell data (docs/dat-format.md §13).
namespace xi::dat
{

struct SpellInfo
{
    uint16_t                id      = 0;
    uint16_t                type    = 0; // 1 white, 2 black, 3 summoning, 4 ninjutsu, 5 song, 6 blue, 7 geomancy, 8 trust
    uint16_t                element = 0; // 0 fire .. 7 dark
    uint16_t                targets = 0; // 0x01 self, 0x02 player, 0x04 party, 0x08 ally, 0x10 NPC, 0x20 enemy
    uint16_t                skill   = 0; // skill id (33 healing, 36 elemental, ...)
    uint16_t                mp      = 0;
    uint8_t                 castTime = 0; // quarter seconds
    uint8_t                 recast   = 0; // quarter seconds
    uint8_t                 icon     = 0; // list icon: "menu magiconw" shape (record +0x28)
    // The spell's own list icon (file 81 "mgc_" record +0x42): "menu magiconw" shape, or "menu magico2w" shape
    // listIcon - 1000 when >= 1000; 0xFFFF = none (use `icon`).
    uint16_t                listIcon = 0xFFFF;
    // Area of effect (file 81 "mgc_" record +0x46 radius index, +0x47 set for area spells; INFERRED from the data: every
    // -ga / -ra spell has 8 / 1, single-target ones 0 / 0): the radius in yalms, the game's table 0x10364498 at that
    // index (CODE 0x1009adf0; 8 -> 10). 0 = single target.
    uint8_t                 aoeRadius = 0;
    // Casting range in yalms (file 81 "mgc_" +0x45 through rangeYalms; 255 = no limit, -1 = unknown).
    int16_t                 range = -1;
    std::array<uint8_t, 24> levels{};     // per job id; 0xFF = cannot learn
    bool                    valid = false;

    [[nodiscard]] bool usableBy(uint8_t job, uint8_t level) const
    {
        return job < levels.size() && levels[job] != 0xFF && levels[job] <= level;
    }
};

// The game's range table (CODE 0x103644b8, u8 pairs): range index -> yalms; 15 (255) = no limit.
[[nodiscard]] int rangeYalms(uint8_t index);

// File 86: 0x400-byte records, one per spell id; each record's bytes are rotated by a per-record amount (§13).
class SpellTable
{
public:
    explicit SpellTable(const DatIndex& index);
    static SpellTable parse(std::span<const std::byte> file);
    // File 81's "mgc_" chunk (type 0x49; CODE 0x101670e0): 0x64-byte records by spell id, rotated as file 86's; +0x40
    // the element icon (= record +0x28 of file 86), +0x42 the spell's own icon, +0x45 the range index. Fills listIcon
    // and range.
    void addListIcons(std::span<const std::byte> file);

    [[nodiscard]] const SpellInfo* find(uint16_t id) const
    {
        return id < spells_.size() && spells_[id].valid ? &spells_[id] : nullptr;
    }

private:
    SpellTable() = default;
    std::vector<SpellInfo> spells_;
};

struct AbilityInfo
{
    uint16_t id       = 0;
    uint8_t  type     = 0;
    uint16_t mp       = 0;
    uint16_t recastId = 0; // the recast group S2C 0x119 reports (0 = two-hour abilities)
    uint16_t targets  = 0; // as SpellInfo::targets
    bool     valid    = false;
};

// File 85: 0x400-byte records indexed by job-ability id, rotated per record like file 86 (§13.1).
class AbilityTable
{
public:
    explicit AbilityTable(const DatIndex& index);
    static AbilityTable parse(std::span<const std::byte> file);
    // File 81's "comm" chunk (type 0x53): 0x30-byte records by command id (weapon skills 0.., job abilities 512 + id,
    // pet moves ...), rotated as file 86's; +0x02 kind, +0x0A valid targets, +0x0E sub-list, +0x10 the range index (CODE 0x10223cc0 reads
    // the table at 0x1063462c, 0x10222f40 / 0x102232c0 its range).
    void addCommands(std::span<const std::byte> file);

    [[nodiscard]] const AbilityInfo* find(uint16_t id) const
    {
        return id < abilities_.size() && abilities_[id].valid ? &abilities_[id] : nullptr;
    }
    // A command's range in yalms (255 = no limit), -1 when unknown. Weapon skills by their id, job abilities 512 + id.
    [[nodiscard]] int commandRange(uint32_t commandId) const
    {
        return commandId < commandRange_.size() ? commandRange_[commandId] : -1;
    }
    // A command's valid targets (comm +0x0A, as SpellInfo::targets; the mask the ability list's cursor uses, CODE
    // 0x10223d9e), 0 when unknown.
    [[nodiscard]] uint16_t commandTargets(uint32_t commandId) const
    {
        return commandId < commandTargets_.size() ? commandTargets_[commandId] : 0;
    }

private:
    AbilityTable() = default;
    std::vector<AbilityInfo> abilities_;
    std::vector<int16_t>     commandRange_;
    std::vector<uint16_t>    commandTargets_;
};

} // namespace xi::dat
