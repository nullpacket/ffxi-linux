// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/entity_names.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using xi::client::entityNameFromUpdate;
using xi::client::EntityRenames;

namespace
{

// A 0x00E as LSB's entity_update.cpp builds it: setSize rounds `size` up to 4 bytes (packets/basic.h), and that many
// bytes are sent.
struct Packet
{
    std::vector<std::byte> b;
    explicit Packet(std::size_t size, uint16_t index, uint8_t mask, uint16_t kind)
    : b((size + 3) & ~std::size_t{ 3 })
    {
        put16(0, static_cast<uint16_t>(0x00E | ((b.size() / 4) << 9)));
        put16(0x08, index);
        b[0x0A] = std::byte{ mask };
        if (b.size() >= 0x32)
        {
            put16(0x30, kind);
        }
    }
    void put16(std::size_t off, uint16_t v)
    {
        b[off]     = std::byte(v & 0xFF);
        b[off + 1] = std::byte(v >> 8);
    }
    void text(std::size_t off, const std::string& s)
    {
        std::memcpy(b.data() + off, s.data(), s.size());
    }
};

std::vector<std::byte> list(const std::string& text)
{
    std::vector<std::byte> b(4 + text.size() + 1);
    std::memcpy(b.data() + 4, text.data(), text.size());
    return b;
}

} // namespace

TEST_CASE("entity names: LSB's renamed dynamic entity with an equipped look keeps its name (+0x44)")
{
    // ENTITY_SPAWN, isRenamed, MODEL_EQUIPPED, targid >= 0x700: size 0x56, mask 0x57 (no 0x08), 0x18 = 1, name at 0x44.
    Packet p(0x56, 0x701, 0x57, 1);
    p.b[0x18] = std::byte{ 1 };
    p.put16(0x32, 0x0102); // face / race, then gear words to 0x44
    p.text(0x44, "Shining_Fellow");
    CHECK(entityNameFromUpdate(p.b) == std::optional<std::string>("Shining_Fellow"));
    // The bound is min(15, size - 0x48): 16 bytes here, so 15 characters.
    Packet q(0x56, 0x701, 0x57, 1);
    q.text(0x44, "ABCDEFGHIJKLMNOPQ");
    CHECK(entityNameFromUpdate(q.b) == std::optional<std::string>("ABCDEFGHIJKLMNO"));
}

TEST_CASE("entity names: the later UPDATE_NAME updates (size 0x48) keep the renamed name from +0x34")
{
    // Equipped look too: LSB writes the name over the look's gear words at 0x34; FUNC_Packet_Incoming_0x000E reads
    // it there (kind 1 is in the +0x34 set), and the gear is only read with mask 0x10, which these updates lack.
    Packet p(0x48, 0x701, 0x08 | 0x01, 1);
    p.text(0x34, "Shining_Fellow");
    CHECK(entityNameFromUpdate(p.b) == std::optional<std::string>("Shining_Fellow"));
    Packet q(0x48, 0x702, 0x08, 0); // a standard-look dynamic entity
    q.text(0x34, "Goblin_Thug");
    CHECK(entityNameFromUpdate(q.b) == std::optional<std::string>("Goblin_Thug"));
}

TEST_CASE(
    "entity names: zone NPCs take the +0x35 rename, else the name DAT; dynamic entities without a name keep theirs")
{
    Packet renamed(0x48, 0x123, 0x08, 0); // targid < 1024: 0x34 = 1, the name at 0x35
    renamed.b[0x34] = std::byte{ 1 };
    renamed.text(0x35, "Moogle");
    CHECK(entityNameFromUpdate(renamed.b) == std::optional<std::string>("Moogle"));

    Packet plain(0x48, 0x123, 0x08, 0); // a name at 0x34 without the marker is not taken: the DAT name is
    plain.text(0x34, "Ignored");
    CHECK(entityNameFromUpdate(plain.b) == std::optional<std::string>(""));

    Packet door(0x48, 0x123, 0x08, 2); // a door's id where a name would be
    door.text(0x34, "\x01\x02\x03\x04");
    CHECK(entityNameFromUpdate(door.b) == std::optional<std::string>(""));

    Packet moved(0x24, 0x703, 0x01, 0); // a dynamic entity's position update: no name, unchanged
    CHECK_FALSE(entityNameFromUpdate(moved.b).has_value());

    Packet ship(0x48, 0x704, 0x08, 4); // kinds 2-4 never take a packet name, and dynamic ones get no DAT name
    ship.text(0x34, "Ship");
    CHECK_FALSE(entityNameFromUpdate(ship.b).has_value());

    Packet fellow(0x56, 0x123, 0x40, 1); // kind 1, mask 0x40, targid < 0x400: +0x44 when printable
    fellow.text(0x44, "Fellow");
    CHECK(entityNameFromUpdate(fellow.b) == std::optional<std::string>("Fellow"));
}

TEST_CASE("entity renames: the 0x1FF list renames by the spawn name less its last character, in its zone only")
{
    EntityRenames r;
    CHECK_FALSE(r.parse(list("")));
    CHECK_FALSE(r.parse(list("abc|x:y")));
    REQUIRE(r.parse(list("230|Goblin Thu:Gobbie|Orc:Snout||broken|")));
    CHECK(r.zone() == std::optional<uint16_t>(230));
    CHECK(r.lookup(230, "Goblin Thug") == std::optional<std::string>("Gobbie"));
    CHECK(r.lookup(230, "Orcx") == std::optional<std::string>("Snout"));
    CHECK_FALSE(r.lookup(231, "Goblin Thug").has_value()); // another zone's list does not apply
    CHECK_FALSE(r.lookup(230, "Goblin Thu").has_value());
    CHECK_FALSE(r.lookup(230, "").has_value());
    // A new list replaces the old; clear() (zone change) forgets it.
    REQUIRE(r.parse(list("231|Bee:Wasp")));
    CHECK_FALSE(r.lookup(230, "Goblin Thug").has_value());
    CHECK(r.lookup(231, "Beex") == std::optional<std::string>("Wasp"));
    r.clear();
    CHECK(r.empty());
    CHECK_FALSE(r.lookup(231, "Beex").has_value());
}
