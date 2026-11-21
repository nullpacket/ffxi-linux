// SPDX-License-Identifier: GPL-3.0-or-later
// Guild shops (issue #94; client/guild_shop.h): the S2C 0x086 / 0x083 / 0x085 / 0x082 / 0x084 parsing, the result
// lines, the C2S payloads, and (with the user's install) the closed / holiday texts and the 7031 purchase line.
#include "client/guild_shop.h"
#include "dat/dat_index.h"
#include "dat/items.h"
#include "dat/messages.h"
#include "xi/install.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <memory>

namespace
{

const xi::dat::DatIndex* install()
{
    static std::unique_ptr<xi::dat::DatIndex> idx = [] {
        const char* root = xi::install::gameRootOrNull();
        if (!root || !std::filesystem::exists(std::filesystem::path(root) / "VTABLE.DAT"))
        {
            return std::unique_ptr<xi::dat::DatIndex>{};
        }
        return std::make_unique<xi::dat::DatIndex>(root);
    }();
    return idx.get();
}

template <typename T>
void put(std::vector<std::byte>& p, std::size_t at, T v)
{
    std::memcpy(p.data() + at, &v, sizeof(T));
}

// A 0x083 / 0x085 packet (0xF8 bytes) with these rows {item, count, max, price} and Stat.
std::vector<std::byte> listPacket(std::initializer_list<xi::client::GuildItem> rows, uint8_t stat)
{
    std::vector<std::byte> p(0xF8);
    std::size_t            i = 0;
    for (const auto& r : rows)
    {
        put<uint16_t>(p, 4 + i * 8, r.item);
        put<uint8_t>(p, 6 + i * 8, r.count);
        put<uint8_t>(p, 7 + i * 8, r.max);
        put<uint32_t>(p, 8 + i * 8, r.price);
        ++i;
    }
    put<uint8_t>(p, 0xF4, static_cast<uint8_t>(i));
    put<uint8_t>(p, 0xF5, stat);
    return p;
}

} // namespace

TEST_CASE("guild open: Stat, hours and the passive bit", "[guild]")
{
    using namespace xi::client;
    std::vector<std::byte> p(12);
    put<uint8_t>(p, 4, 1);
    // LSB packs the hours 8..20 as bits 8..19 (packBitsBE(Time, 0xFFFFFF, open, close - open)), bit 31 = passive.
    put<uint32_t>(p, 8, 0x800FFF00u);
    const auto g = parseGuildOpen(p);
    CHECK(g.stat == 1);
    CHECK(g.passive());
    CHECK(guildHours(g.time) == std::pair{ 8, 20 });
    CHECK(guildHours(0x00FFFFFFu) == std::pair{ 0, 24 }); // open all day
    CHECK(guildHours(0) == std::pair{ 24, 24 });
}

TEST_CASE("guild lists: first / last flags and repeated items", "[guild]")
{
    using namespace xi::client;
    GuildList l;
    // A single-packet list: Stat 0x80 only (LSB: PacketCount + 0x80).
    mergeGuildList(l, listPacket({ { 17000, 5, 20, 100 }, { 17001, 0, 10, 0x80000032u } }, 0x80));
    REQUIRE(l.items.size() == 2);
    CHECK(l.state == 1);
    CHECK(l.items[1].hidden());
    CHECK(l.items[1].gil() == 50);
    // Several packets: 0x40 starts over, a repeated item is skipped, the last one completes.
    mergeGuildList(l, listPacket({ { 1, 1, 1, 1 }, { 2, 2, 2, 2 } }, 0x40));
    CHECK(l.state == 3);
    mergeGuildList(l, listPacket({ { 2, 9, 9, 9 }, { 3, 3, 3, 3 } }, 0x81));
    CHECK(l.state == 1);
    REQUIRE(l.items.size() == 3);
    CHECK(l.items[1].count == 2); // the first row for item 2 stays

    // A purchase result updates the stock.
    std::vector<std::byte> r(8);
    put<uint16_t>(r, 4, 3);
    put<uint8_t>(r, 6, 0);
    put<int8_t>(r, 7, 3);
    CHECK(applyGuildStock(l, r));
    CHECK(l.items[2].count == 0);
    put<uint16_t>(r, 4, 99);
    CHECK_FALSE(applyGuildStock(l, r));
}

TEST_CASE("guild results: which lines", "[guild]")
{
    using namespace xi::client;
    auto t = guildResultText(false, 0, -1); // LSB's refusal
    CHECK(t.sys1 == 155);
    CHECK(t.itemMsg == -1);
    CHECK(guildResultText(false, 0, -5).sys1 == 163);
    CHECK(guildResultText(false, 0, -3).sys1 == 154);
    CHECK(guildResultText(true, 0, -3).sys1 == 155);
    CHECK(guildResultText(false, 17000, 0).sys1 == 129);
    CHECK(guildResultText(true, 17000, 0).sys1 == 130);
    t = guildResultText(false, 17000, 12);
    CHECK(t.itemMsg == 165);
    CHECK(t.count == 12);
    CHECK(t.sys2 == -1);
    CHECK(guildResultText(false, 17000, 1).itemMsg == 166);
    t = guildResultText(true, 17000, -4); // a partial sale of 4
    CHECK(t.itemMsg == 167);
    CHECK(t.count == 4);
    CHECK(t.sys2 == 132);
    CHECK(guildResultText(true, 17000, 1).itemMsg == 168);
}

TEST_CASE("guild trade payload", "[guild]")
{
    const auto p = xi::client::guildTradePayload(0x4321, 7, 12);
    REQUIRE(p.size() == 4);
    CHECK(p[0] == std::byte{ 0x21 });
    CHECK(p[1] == std::byte{ 0x43 });
    CHECK(p[2] == std::byte{ 7 });
    CHECK(p[3] == std::byte{ 12 });
}

TEST_CASE("guild texts from the install", "[guild][data]")
{
    const auto* idx = install();
    if (!idx)
    {
        SKIP("no game data");
    }
    using namespace xi::client;
    const auto formats  = xi::dat::XiStringTable::parse(idx->read(50));
    const auto weekdays = xi::dat::StringTable::parse(idx->read(55658));
    auto       closed   = guildClosedText({ 1, 0x000FFF00u }, &formats, &weekdays);
    REQUIRE(closed.size() == 2);
    CHECK(closed[0] == "The shop is closed.");
    CHECK(closed[1] == "This shop's hours are 8 to 20.");
    auto holiday = guildClosedText({ 2, 4 }, &formats, &weekdays);
    REQUIRE(holiday.size() == 2);
    CHECK(holiday[1] == "Iceday is this shop's weekly holiday.");
    CHECK(guildClosedText({ 2, 9 }, &formats, &weekdays).empty());

    // 7031 #165 "You buy <count> <item>s from the guild." takes Para0 = item, Para1 = count (CODE 0x101ec2a7).
    const auto std = xi::dat::DialogTable::parse(idx->read(7031));
    const auto sys = xi::dat::StringTable::parse(idx->read(55646));
    CHECK(sys.get(155) == "You were unable to carry out that transaction.");
    auto text = std::make_shared<xi::dat::BattleText>(*idx);
    text->setItems(std::make_shared<const xi::dat::ItemTable>(*idx));
    xi::dat::DialogArgs args;
    args.lookup     = [text](uint16_t table, uint32_t value) { return text->dialogName(table, value); };
    args.numbers[0] = 17396; // Little Worm
    args.numbers[1] = 1;
    const auto one  = xi::dat::formatDialog(std.raw(166), args);
    REQUIRE_FALSE(one.empty());
    CHECK(one[0] == "You buy a little worm from the guild.");
    args.numbers[1] = 3;
    const auto three = xi::dat::formatDialog(std.raw(165), args);
    REQUIRE_FALSE(three.empty());
    CHECK(three[0] == "You buy 3 little worms from the guild.");
    const auto sold = xi::dat::formatDialog(std.raw(167), args);
    REQUIRE_FALSE(sold.empty());
    CHECK(sold[0] == "You sell 3 little worms to the guild.");
}
