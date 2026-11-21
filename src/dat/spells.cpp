// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/spells.h"

#include "dat/chunk.h"
#include "xi/bytes.h"

#include <algorithm>

namespace xi::dat
{

namespace
{

constexpr std::size_t kRecord = 0x400;

uint8_t rotl(uint8_t b, unsigned r)
{
    r &= 7;
    return r == 0 ? b : static_cast<uint8_t>((b << r) | (b >> (8 - r)));
}

} // namespace

int rangeYalms(uint8_t index)
{
    static constexpr uint8_t kYalms[16] = { 0, 1, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 25, 30, 255 };
    return kYalms[index & 15];
}

SpellTable::SpellTable(const DatIndex& index)
: SpellTable(parse(index.read(86)))
{
    addListIcons(index.read(81));
}

void SpellTable::addListIcons(std::span<const std::byte> file)
{
    std::vector<Chunk> chunks;
    parseChunks(file, chunks);
    for (const auto& c : chunks)
    {
        if (c.type != 0x49 || c.name != "mgc_")
        {
            continue;
        }
        constexpr std::size_t kRec = 0x64;
        for (std::size_t i = 1; i < spells_.size() && (i + 1) * kRec <= c.payload.size(); ++i)
        {
            const auto raw = c.payload.subspan(i * kRec, kRec);
            for (unsigned r = 0; r < 8; ++r)
            {
                auto at = [&](std::size_t k) { return rotl(static_cast<uint8_t>(raw[k]), r); };
                if (static_cast<std::size_t>(at(0) | at(1) << 8) == i)
                {
                    spells_[i].listIcon = static_cast<uint16_t>(at(0x42) | at(0x43) << 8);
                    static constexpr uint8_t kRadius[16] = { 0, 1, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 25, 30, 255 };
                    spells_[i].aoeRadius = at(0x47) != 0 ? std::min<uint8_t>(kRadius[at(0x46) & 15], 40) : 0; // clamp 0..40
                    spells_[i].range    = static_cast<int16_t>(rangeYalms(at(0x45)));
                    break;
                }
            }
        }
    }
}

SpellTable SpellTable::parse(std::span<const std::byte> file)
{
    SpellTable t;
    const std::size_t n = file.size() / kRecord;
    t.spells_.resize(n);
    std::array<std::byte, 0x30> rec{};
    for (std::size_t i = 1; i < n; ++i)
    {
        const auto raw = file.subspan(i * kRecord, kRecord);
        // The rotation is the one that turns the record's first u16 into its own index, with every job level either
        // 0xFF or a real level. (INFERRED from the data; the client's decoder is not traced.)
        for (unsigned r = 0; r < 8; ++r)
        {
            for (std::size_t k = 0; k < rec.size(); ++k)
            {
                rec[k] = static_cast<std::byte>(rotl(static_cast<uint8_t>(raw[k]), r));
            }
            if (load<uint16_t>(rec, 0) != i)
            {
                continue;
            }
            bool levelsOk = true;
            for (std::size_t j = 0; j < 24; ++j)
            {
                const auto l = static_cast<uint8_t>(rec[0x0E + j]);
                levelsOk &= l == 0xFF || l <= 99;
            }
            if (!levelsOk)
            {
                continue;
            }
            auto& s    = t.spells_[i];
            s.id       = static_cast<uint16_t>(i);
            s.type     = load<uint16_t>(rec, 0x02);
            s.element  = load<uint16_t>(rec, 0x04);
            s.targets  = load<uint16_t>(rec, 0x06);
            s.skill    = load<uint16_t>(rec, 0x08);
            s.mp       = load<uint16_t>(rec, 0x0A);
            s.castTime = static_cast<uint8_t>(rec[0x0C]);
            s.recast   = static_cast<uint8_t>(rec[0x0D]);
            s.icon     = static_cast<uint8_t>(rec[0x28]);
            for (std::size_t j = 0; j < 24; ++j)
            {
                s.levels[j] = static_cast<uint8_t>(rec[0x0E + j]);
            }
            s.valid = true;
            break;
        }
    }
    return t;
}

AbilityTable::AbilityTable(const DatIndex& index)
: AbilityTable(parse(index.read(85)))
{
    addCommands(index.read(81));
}

void AbilityTable::addCommands(std::span<const std::byte> file)
{
    std::vector<Chunk> chunks;
    parseChunks(file, chunks);
    for (const auto& c : chunks)
    {
        if (c.type != 0x53 || c.name != "comm")
        {
            continue;
        }
        constexpr std::size_t kRec = 0x30;
        commandRange_.assign(c.payload.size() / kRec, -1);
        commandTargets_.assign(c.payload.size() / kRec, 0);
        for (std::size_t i = 0; i < commandRange_.size(); ++i)
        {
            const auto raw = c.payload.subspan(i * kRec, kRec);
            for (unsigned r = 0; r < 8; ++r)
            {
                auto at = [&](std::size_t k) { return rotl(static_cast<uint8_t>(raw[k]), r); };
                // The id is the index and the last byte 0xFF (DATA: every record).
                if (static_cast<std::size_t>(at(0) | at(1) << 8) == i && at(kRec - 1) == 0xFF)
                {
                    commandRange_[i]   = static_cast<int16_t>(rangeYalms(at(0x10)));
                    commandTargets_[i] = static_cast<uint16_t>(at(0x0A) | at(0x0B) << 8);
                    break;
                }
            }
        }
    }
}

AbilityTable AbilityTable::parse(std::span<const std::byte> file)
{
    AbilityTable t;
    const std::size_t n = file.size() / kRecord;
    t.abilities_.resize(n);
    std::array<std::byte, 0x2A> rec{};
    for (std::size_t i = 1; i < n; ++i)
    {
        const auto raw = file.subspan(i * kRecord, kRecord);
        // The rotation that makes the record's u16 id equal its index and its name (+0x0A) printable: ids such as
        // 0x22 read the same under two rotations (INFERRED).
        for (unsigned r = 0; r < 8; ++r)
        {
            for (std::size_t k = 0; k < rec.size(); ++k)
            {
                rec[k] = static_cast<std::byte>(rotl(static_cast<uint8_t>(raw[k]), r));
            }
            if (load<uint16_t>(rec, 0) != i)
            {
                continue;
            }
            bool printable = true;
            for (std::size_t k = 0x0A; k < rec.size() && rec[k] != std::byte{ 0 }; ++k)
            {
                const auto c = static_cast<uint8_t>(rec[k]);
                printable &= c >= 0x20 && c < 0x7F;
            }
            if (!printable)
            {
                continue;
            }
            auto& a    = t.abilities_[i];
            a.id       = static_cast<uint16_t>(i);
            a.type     = static_cast<uint8_t>(rec[2]);
            a.mp       = load<uint16_t>(rec, 0x04);
            a.recastId = load<uint16_t>(rec, 0x06);
            a.targets  = load<uint16_t>(rec, 0x08);
            a.valid    = true;
            break;
        }
    }
    return t;
}

} // namespace xi::dat
