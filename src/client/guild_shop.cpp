// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/guild_shop.h"

#include "dat/messages.h"

#include <algorithm>
#include <cstring>

namespace xi::client
{

namespace
{

template <typename T>
T load(std::span<const std::byte> d, std::size_t at)
{
    T v{};
    if (at + sizeof(T) <= d.size())
    {
        std::memcpy(&v, d.data() + at, sizeof(T));
    }
    return v;
}

constexpr std::size_t kGuildListMax = 128; // (0x404 - 4) / 8 rows per list (CODE 0x100eade0)

} // namespace

GuildOpen parseGuildOpen(std::span<const std::byte> packet)
{
    return { load<uint8_t>(packet, 4), load<uint32_t>(packet, 8) };
}

std::pair<int, int> guildHours(uint32_t time)
{
    int open = 0;
    while (open < 24 && !(time & (1u << open)))
    {
        ++open;
    }
    int close = open;
    while (close < 24 && (time & (1u << close)))
    {
        ++close;
    }
    return { open, close };
}

std::vector<std::string> guildClosedText(const GuildOpen& g, const dat::XiStringTable* formats, const dat::StringTable* weekdays)
{
    std::vector<std::string> out;
    if (!formats || (g.stat != 1 && g.stat != 2))
    {
        return out;
    }
    std::string text;
    if (g.stat == 1)
    {
        const auto [open, close] = guildHours(g.time);
        text                     = std::string(formats->get(25));
        for (const int v : { open, close })
        {
            if (const auto p = text.find("%d"); p != std::string::npos)
            {
                text.replace(p, 2, std::to_string(v));
            }
        }
    }
    else
    {
        const uint32_t day = g.time & 0x7FFFFFFFu;
        if (day >= 8)
        {
            return out;
        }
        text = std::string(formats->get(26));
        if (const auto p = text.find("%s"); p != std::string::npos)
        {
            text.replace(p, 2, weekdays ? std::string(weekdays->get(day)) : std::string());
        }
    }
    std::size_t from = 0;
    for (;;)
    {
        const auto nl   = text.find('\n', from);
        auto       line = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        if (!line.empty())
        {
            out.push_back(std::move(line));
        }
        if (nl == std::string::npos)
        {
            break;
        }
        from = nl + 1;
    }
    return out;
}

void mergeGuildList(GuildList& list, std::span<const std::byte> packet)
{
    if (packet.size() < 0xF6)
    {
        return;
    }
    const auto stat = load<uint8_t>(packet, 0xF5);
    if (stat & 0x40)
    {
        list.items.clear();
        list.state = 3;
    }
    const auto count = std::min<std::size_t>(load<uint8_t>(packet, 0xF4), 30);
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t o = 4 + i * 8;
        GuildItem         g{ load<uint16_t>(packet, o), load<uint8_t>(packet, o + 2), load<uint8_t>(packet, o + 3), load<uint32_t>(packet, o + 4) };
        const bool        listed = std::any_of(list.items.begin(), list.items.end(), [&](const GuildItem& x) { return x.item == g.item; });
        if (!listed && list.items.size() < kGuildListMax)
        {
            list.items.push_back(g);
        }
    }
    if (stat & 0x80)
    {
        list.state = 1;
    }
}

bool applyGuildStock(GuildList& list, std::span<const std::byte> packet)
{
    const auto item = load<uint16_t>(packet, 4);
    for (auto& g : list.items)
    {
        if (item != 0 && g.item == item)
        {
            g.count = load<uint8_t>(packet, 6);
            return true;
        }
    }
    return false;
}

GuildResultText guildResultText(bool sell, uint16_t itemNo, int8_t trade)
{
    GuildResultText t;
    if (itemNo == 0)
    {
        // Refused: buying, Trade -5 = "you can only hold one" (163), -3 = "please wait longer" (154); else 155.
        t.sys1 = !sell && trade == -5 ? 163 : !sell && trade == -3 ? 154 : 155;
        return t;
    }
    if (trade == 0)
    {
        t.sys1 = sell ? 130 : 129; // "The guild would not buy that from you." / "You cannot make that purchase."
        return t;
    }
    const int n = trade < 0 ? -trade : trade;
    t.itemMsg   = sell ? (n > 1 ? 167 : 168) : (n > 1 ? 165 : 166);
    t.item      = itemNo;
    t.count     = n;
    if (trade < 0)
    {
        t.sys2 = sell ? 132 : 131;
    }
    return t;
}

std::vector<std::byte> guildTradePayload(uint16_t item, uint8_t slot, uint8_t count)
{
    std::vector<std::byte> p(4);
    std::memcpy(p.data(), &item, 2);
    p[2] = static_cast<std::byte>(slot);
    p[3] = static_cast<std::byte>(count);
    return p;
}

} // namespace xi::client
