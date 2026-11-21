// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/emote_list.h"

#include <cstring>

namespace xi::dat
{

namespace
{

uint16_t u16(std::span<const std::byte> d, std::size_t o)
{
    return static_cast<uint16_t>(std::to_integer<uint16_t>(d[o]) | std::to_integer<uint16_t>(d[o + 1]) << 8);
}

std::string text(std::span<const std::byte> d, std::size_t o, std::size_t n)
{
    std::string s;
    for (std::size_t i = 0; i < n && d[o + i] != std::byte{ 0 }; ++i)
    {
        s.push_back(static_cast<char>(d[o + i]));
    }
    return s;
}

} // namespace

EmoteList EmoteList::parse(std::span<const std::byte> file)
{
    EmoteList out;
    if (file.size() < 4)
    {
        return out;
    }
    const uint32_t count = u16(file, 0) | static_cast<uint32_t>(u16(file, 2)) << 16;
    for (uint32_t i = 0; i < count && 4 + (i + 1) * 32 <= file.size(); ++i)
    {
        const std::size_t o = 4 + i * 32;
        EmoteEntry        e;
        e.id       = u16(file, o);
        e.label    = u16(file, o + 2);
        e.help     = u16(file, o + 4);
        e.parent   = u16(file, o + 6);
        e.command  = text(file, o + 8, 16);
        e.argument = text(file, o + 24, 8);
        if (e.id != 0) // record 0 is empty (CODE 0x10225220 skips id 0)
        {
            out.entries_.push_back(std::move(e));
        }
    }
    return out;
}

std::vector<const EmoteEntry*> EmoteList::children(uint16_t parent) const
{
    std::vector<const EmoteEntry*> out;
    for (const auto& e : entries_)
    {
        if (e.parent == parent && e.parent != 0xFFFF)
        {
            out.push_back(&e);
        }
    }
    return out;
}

} // namespace xi::dat
