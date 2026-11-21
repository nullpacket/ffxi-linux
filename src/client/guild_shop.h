// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace xi::dat
{
class StringTable;
class XiStringTable;
} // namespace xi::dat

namespace xi::client
{

// Guild shops (issue #94; docs/ui-format-notes.md §10.11). Talking to a guild merchant makes the server send S2C 0x086
// GUILD_OPEN; the game then opens "menu guildsho" (Buy / Sell) and asks for the goods with C2S 0x0AB (buy list) or
// 0x0AD (sell list), answered by S2C 0x083 / 0x085. Buying is C2S 0x0AA, selling C2S 0x0AC, each answered by S2C
// 0x082 / 0x084 with the new stock (CODE 0x100ead20 registers the lists, 0x1020f0e0 the menu's handlers).

// One row of S2C 0x083 / 0x085 (GP_GUILD_ITEM): item, the guild's stock, its maximum, the price. Price bit 31 on a
// sell-list row hides it from the Sell list (it can still be sold from the inventory; CODE 0x101ec9c5).
struct GuildItem
{
    uint16_t item  = 0;
    uint8_t  count = 0;
    uint8_t  max   = 0;
    uint32_t price = 0;

    [[nodiscard]] uint32_t gil() const
    {
        return price & 0x7FFFFFFFu;
    }
    [[nodiscard]] bool hidden() const
    {
        return (price & 0x80000000u) != 0;
    }
};

// A guild list as the game keeps it (game object +0x19de4 + list * 0x408, CODE 0x100eade0): state 0 none, 3 arriving,
// 1 complete; up to 128 rows.
struct GuildList
{
    uint8_t                state = 0;
    std::vector<GuildItem> items;
};

// S2C 0x086 GUILD_OPEN: Stat @4 (0 open, 1 closed, 2 holiday), Time @8. Closed: bits 0..23 are the open hours, bit
// 31 = "passive" (the hour tick closing a shop the player may no longer be looking at). Holiday: the weekday 0..7.
struct GuildOpen
{
    uint8_t  stat = 0;
    uint32_t time = 0;

    [[nodiscard]] bool passive() const
    {
        return (time & 0x80000000u) != 0;
    }
};
[[nodiscard]] GuildOpen parseGuildOpen(std::span<const std::byte> packet);

// The open and close hours of a closed message (CODE 0x101ed479): the first set bit of Time's low 24 bits, then the
// first clear bit after it (24 when the hours run to the end of the day).
[[nodiscard]] std::pair<int, int> guildHours(uint32_t time);

// The lines a closed (Stat 1) or holiday (Stat 2) answer logs (CODE 0x101ed3c0): string table 10 (file 50) #25 "The
// shop is closed.\nThis shop's hours are %d to %d." with guildHours(), or #26 "...%s is this shop's weekly holiday."
// with the weekday's name (55658 #day); one string per line. A holiday weekday past 7 logs nothing.
[[nodiscard]] std::vector<std::string> guildClosedText(const GuildOpen& g, const dat::XiStringTable* formats, const dat::StringTable* weekdays);

// S2C 0x083 / 0x085 into `list` (CODE 0x100eae30): List[30] of 8 bytes @4, Count @0xF4, Stat @0xF5. Stat bit 0x40
// starts a new list (the first of several packets), bit 0x80 ends it. A row for an item already listed is skipped.
void mergeGuildList(GuildList& list, std::span<const std::byte> packet);

// S2C 0x082 / 0x084 (GUILD_BUY / GUILD_SELL): ItemNo @4, Count @6 (the guild's new stock), Trade @7 (signed: the
// quantity bought or sold; negative = a partial fill of that many; 0 or ItemNo 0 = refused). The row's stock follows
// (CODE 0x100eaef0); returns false when the item is not in the list.
bool applyGuildStock(GuildList& list, std::span<const std::byte> packet);

// The log lines of a 0x082 / 0x084 result (CODE 0x101ec210 buy, 0x101ec300 sell). `sys1` / `sys2` are string table 7
// (file 55646) ids, `item` a standard message (file 7031) with Para0 = item and Para1 = count; -1 = none.
struct GuildResultText
{
    int      sys1     = -1; // first line
    int      itemMsg  = -1; // 7031: 165 / 166 "You buy ... from the guild.", 167 / 168 "You sell ... to the guild."
    uint16_t item     = 0;
    int      count    = 0;
    int      sys2     = -1; // after the item line: 131 / 132 "... could not be filled / purchase the full amount."
};
[[nodiscard]] GuildResultText guildResultText(bool sell, uint16_t itemNo, int8_t trade);

// C2S payloads (after the 4-byte header). 0x0AA GUILD_BUY / 0x0AC GUILD_SELL: {u16 ItemNo, u8 PropertyItemIndex
// (0 when buying), u8 ItemNum}. 0x0AB / 0x0AD (the lists) carry nothing.
[[nodiscard]] std::vector<std::byte> guildTradePayload(uint16_t item, uint8_t slot, uint8_t count);

} // namespace xi::client
