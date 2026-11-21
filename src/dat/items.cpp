// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/items.h"

#include "xi/bytes.h"

#include <bit>
#include <cstdlib>
#include <filesystem>

namespace xi::dat
{

namespace
{

constexpr std::size_t kRecord = 0xC00;

// English item files (general, usable, weapons, armour, automaton, then the later ranges). Each starts at the id of
// its first record and holds file size / 0xC00 records (§12).
constexpr uint32_t kItemFiles[] = { 73, 74, 75, 76, 77, 55667, 55668, 55669, 55670, 55671, 55672, 55673 };

std::vector<std::byte> unrotate(std::span<const std::byte> in)
{
    std::vector<std::byte> out(in.size());
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const auto b = static_cast<uint8_t>(in[i]);
        out[i]       = static_cast<std::byte>(static_cast<uint8_t>((b >> 5) | (b << 3)));
    }
    return out;
}

} // namespace

ItemTable::ItemTable(const DatIndex& index)
: index_(index)
{
    for (const auto f : kItemFiles)
    {
        const auto p = index.path(f);
        if (!p)
        {
            continue;
        }
        const auto size = std::filesystem::file_size(*p);
        if (size == 0 || size % kRecord != 0)
        {
            continue;
        }
        const auto first = unrotate(index.read(f, 0, 4));
        if (first.size() < 4)
        {
            continue;
        }
        ranges_.push_back({ f, load<uint32_t>(first, 0), static_cast<uint32_t>(size / kRecord) });
    }
}

ItemInfo ItemTable::decode(std::span<const std::byte> record)
{
    ItemInfo it;
    if (record.size() < 0x40)
    {
        return it;
    }
    it.id   = static_cast<uint16_t>(load<uint32_t>(record, 0));
    it.flags = load<uint16_t>(record, 0x04);
    it.type  = load<uint16_t>(record, 0x08);
    it.stackSize = load<uint16_t>(record, 0x06);
    it.targets   = load<uint16_t>(record, 0x0C);
    if (it.equipment())
    {
        it.level         = load<uint16_t>(record, 0x0E);
        it.slots         = load<uint16_t>(record, 0x10);
        it.races         = load<uint16_t>(record, 0x12);
        it.jobs          = load<uint32_t>(record, 0x14);
        it.superiorLevel = load<uint16_t>(record, 0x18);
        // The headers differ from here (DATA: weapons are 0x38 bytes, armour 0x2C): weapons hold damage +0x1C, delay
        // +0x1E, skill +0x22 and the charges +0x28; armour the shield size +0x1A and the charges +0x1C.
        const bool weapon = it.type == 4;
        if (weapon)
        {
            it.skill = load<uint8_t>(record, 0x22);
        }
        it.maxCharges = load<uint8_t>(record, weapon ? 0x28 : 0x1C);
        it.recast     = load<uint32_t>(record, weapon ? 0x2C : 0x20);
    }
    // The string block sits after a type-specific header: u32 field count n, then (u32 offset, u32 type) pairs whose
    // first offset is 4 + 8n; the first field is the name, stored 28 bytes after its offset (as in d_msg).
    for (std::size_t p = 0x0E; p + 12 < 0x200 && p + 12 < record.size(); p += 2)
    {
        const uint32_t n = load<uint32_t>(record, p);
        if (n < 1 || n > 9 || load<uint32_t>(record, p + 4) != 4 + 8 * n)
        {
            continue;
        }
        // Fields (string = type 0): name, a flag, log name singular, log name plural, description.
        std::vector<std::string> strings;
        for (uint32_t f = 0; f < n; ++f)
        {
            const uint32_t off  = load<uint32_t>(record, p + 4 + 8 * f);
            const uint32_t type = load<uint32_t>(record, p + 8 + 8 * f);
            const std::size_t at = p + off + 28;
            if (type != 0 || at >= record.size())
            {
                strings.emplace_back();
                continue;
            }
            const auto* str = reinterpret_cast<const char*>(record.data() + at);
            strings.emplace_back(str, strnlen(str, record.size() - at));
        }
        it.name      = !strings.empty() ? strings[0] : std::string();
        it.logName   = strings.size() > 2 ? strings[2] : it.name;
        it.logPlural = strings.size() > 3 ? strings[3] : it.logName;
        it.description = strings.size() > 4 ? strings[4] : std::string();
        break;
    }
    return it;
}

std::vector<std::byte> ItemTable::record(uint16_t id) const
{
    for (const auto& r : ranges_)
    {
        if (id >= r.first && id < r.first + r.count)
        {
            return unrotate(index_.read(r.fileId, std::uint64_t{ id - r.first } * kRecord, kRecord));
        }
    }
    return {};
}

std::optional<Texture> ItemTable::icon(uint16_t id) const
{
    const auto rec = record(id);
    if (rec.size() < 0x284 || load<uint32_t>(rec, 0) != id)
    {
        return std::nullopt;
    }
    const auto size = load<uint32_t>(rec, 0x280);
    if (size == 0 || 0x284 + size > rec.size())
    {
        return std::nullopt;
    }
    try
    {
        return parseTexture(std::span(rec).subspan(0x284, size));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::optional<ItemInfo> ItemTable::find(uint16_t id) const
{
    {
        std::lock_guard lock(mutex_);
        if (const auto it = cache_.find(id); it != cache_.end())
        {
            return it->second;
        }
    }
    const auto rec = record(id);
    if (rec.empty())
    {
        return std::nullopt;
    }
    auto info = decode(rec);
    if (info.id != id)
    {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    return cache_.emplace(id, std::move(info)).first->second;
}

std::optional<Texture> statusIcon(const DatIndex& index, uint16_t iconId, uint32_t file)
{
    constexpr std::size_t kStatusRecord = 0x1800;
    const auto rec = index.read(file, std::uint64_t{ iconId } * kStatusRecord, kStatusRecord);
    if (rec.size() < 0x284)
    {
        return std::nullopt;
    }
    const auto size = load<uint32_t>(rec, 0x280);
    if (size == 0 || 0x284 + size > rec.size())
    {
        return std::nullopt;
    }
    try
    {
        return parseTexture(std::span(rec).subspan(0x284, size));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

void decodeDataBlock(std::span<std::byte> data)
{
    if (data.size() < 13)
    {
        return;
    }
    auto      bits  = [&](std::size_t i) { return std::popcount(static_cast<uint8_t>(data[i])); };
    const int n     = bits(2) - bits(11) + bits(12);
    const int shift = std::abs(n % 5);
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        if (i == 2 || i == 11 || i == 12)
        {
            continue; // the key bytes stay as they are
        }
        const auto b = static_cast<uint8_t>(data[i]);
        uint8_t    v = 0;
        switch (shift)
        {
            case 0: v = static_cast<uint8_t>((b << 1) | (b >> 7)); break;
            case 1: v = static_cast<uint8_t>((b >> 1) | (b << 7)); break;
            case 2: v = static_cast<uint8_t>((b << 2) | (b >> 6)); break;
            case 3: v = static_cast<uint8_t>((b >> 2) | (b << 6)); break;
            default: v = static_cast<uint8_t>((b << 3) | (b >> 5)); break;
        }
        data[i] = static_cast<std::byte>(v);
    }
}

std::optional<StatusInfo> decodeStatusInfo(std::span<std::byte> header)
{
    if (header.size() < 0x30)
    {
        return std::nullopt;
    }
    decodeDataBlock(header);
    StatusInfo s;
    s.id        = load<uint16_t>(header, 0);
    s.canCancel = header[2] != std::byte{ 0 };
    s.hideTimer = header[3] == std::byte{ 0xFF };
    // The description: the string table at +4 (u32 count, then u32 offsets from the table's start; each string a
    // 0x1C-byte header and the NUL-terminated text, INFERRED from the records: every one has count 1, offset 0x0C, so
    // the text is at +0x2C).
    const auto off = std::size_t{ 4 } + load<uint32_t>(header, 8) + 0x1C;
    if (load<uint32_t>(header, 4) >= 1 && off < header.size())
    {
        for (std::size_t i = off; i < header.size() && header[i] != std::byte{ 0 }; ++i)
        {
            s.description.push_back(static_cast<char>(header[i]));
        }
    }
    return s;
}

std::optional<StatusInfo> statusInfo(const DatIndex& index, uint16_t statusId)
{
    constexpr std::size_t kStatusRecord = 0x1800;
    constexpr std::size_t kHeader       = 0x1FC; // the length 0x10226ca0 decodes
    auto                  rec           = index.read(87, std::uint64_t{ statusId } * kStatusRecord, kHeader);
    if (rec.size() < kHeader)
    {
        return std::nullopt;
    }
    auto s = decodeStatusInfo(rec);
    if (!s || s->id != statusId)
    {
        return std::nullopt;
    }
    return s;
}

} // namespace xi::dat
