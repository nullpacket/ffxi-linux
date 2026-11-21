// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/entity_names.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <vector>

namespace xi::client
{
namespace
{

uint8_t byteAt(std::span<const std::byte> d, std::size_t off)
{
    return off < d.size() ? std::to_integer<uint8_t>(d[off]) : 0;
}

uint16_t wordAt(std::span<const std::byte> d, std::size_t off)
{
    return static_cast<uint16_t>(byteAt(d, off) | (byteAt(d, off + 1) << 8));
}

// FUN_100ddd70(dst, 0x10, src, limit, size): strncpy of min(15, size x 4 - limit) characters, stopping at a NUL.
std::string boundedName(std::span<const std::byte> d, std::size_t sizeBytes, std::size_t off, std::size_t limit)
{
    if (sizeBytes <= limit || off >= d.size())
    {
        return {};
    }
    const std::size_t count = std::min<std::size_t>({ 15, sizeBytes - limit, d.size() - off });
    std::string       out;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto c = std::to_integer<char>(d[off + i]);
        if (c == '\0')
        {
            break;
        }
        out.push_back(c);
    }
    return out;
}

std::vector<std::string_view> split(std::string_view s, char ch) // Lua's gmatch('[^ch]+'): empty pieces dropped
{
    std::vector<std::string_view> out;
    std::size_t                   i = 0;
    while (i < s.size())
    {
        const auto j = std::min(s.find(ch, i), s.size());
        if (j > i)
        {
            out.push_back(s.substr(i, j - i));
        }
        i = j + 1;
    }
    return out;
}

} // namespace

std::optional<std::string> entityNameFromUpdate(std::span<const std::byte> d)
{
    if (d.size() < 0x0C)
    {
        return std::nullopt;
    }
    const std::size_t sizeBytes = std::min<std::size_t>(static_cast<std::size_t>(wordAt(d, 0) >> 9) * 4, d.size());
    const uint16_t    idx       = wordAt(d, 0x08);
    const uint8_t     mask      = byteAt(d, 0x0A);
    const uint16_t    kind      = wordAt(d, 0x30) & 7;
    const bool        named     = kind == 0 || kind == 1 || kind == 7 || kind == 5 || kind == 6;
    if (named && (mask & 0x08) && idx >= 0x700)
    {
        return boundedName(d, sizeBytes, 0x34, 0x38);
    }
    if (kind == 1 && (mask & 0x40))
    {
        if (idx >= 0x700)
        {
            return boundedName(d, sizeBytes, 0x44, 0x48);
        }
        if (idx < 0x400 && byteAt(d, 0x44) > 0x20)
        {
            return boundedName(d, sizeBytes, 0x44, 0x48);
        }
    }
    if (idx < 0x400 && (mask & 0x08) && byteAt(d, 0x34) == 1 && byteAt(d, 0x35) > 0x20)
    {
        // FUN_10093360 copies 24 bytes from +0x35 into the actor's name (+0x7c), which shows to its first NUL.
        std::string out;
        for (std::size_t i = 0x35; i < std::min<std::size_t>(0x35 + 24, sizeBytes); ++i)
        {
            const auto c = std::to_integer<char>(d[i]);
            if (c == '\0')
            {
                break;
            }
            out.push_back(c);
        }
        return out;
    }
    if (idx < 0x700)
    {
        return std::string(); // the zone's name DAT (FUN_1008a860)
    }
    return std::nullopt;
}

bool EntityRenames::parse(std::span<const std::byte> d)
{
    std::string text;
    for (std::size_t i = 4; i < d.size(); ++i)
    {
        const auto c = std::to_integer<char>(d[i]);
        if (c == '\0')
        {
            break;
        }
        text.push_back(c);
    }
    const auto parts = split(text, '|');
    if (parts.empty())
    {
        return false;
    }
    int        zone = 0;
    const auto r    = std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), zone);
    if (r.ec != std::errc{} || r.ptr != parts[0].data() + parts[0].size() || zone < 0 || zone > 0xFFFF)
    {
        return false;
    }
    zone_ = static_cast<uint16_t>(zone);
    names_.clear();
    for (std::size_t i = 1; i < parts.size(); ++i)
    {
        const auto kv = split(parts[i], ':');
        if (kv.size() >= 2)
        {
            names_[std::string(kv[0])] = std::string(kv[1]);
        }
    }
    return true;
}

std::optional<std::string> EntityRenames::lookup(uint16_t zone, std::string_view packetName) const
{
    if (!zone_ || *zone_ != zone || packetName.empty())
    {
        return std::nullopt;
    }
    const auto it = names_.find(std::string(packetName.substr(0, packetName.size() - 1)));
    if (it == names_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

} // namespace xi::client
