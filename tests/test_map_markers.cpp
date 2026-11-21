// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/map_markers.h"

#include <catch2/catch_test_macros.hpp>

// The map window's Markers list (docs/ui-format-notes.md §6.5).
namespace
{
xi::client::PartyMember member(uint32_t id, const char* name, uint8_t partyNo)
{
    xi::client::PartyMember m;
    m.id      = id;
    m.name    = name;
    m.partyNo = partyNo;
    return m;
}
} // namespace

TEST_CASE("party markers: our party first, not us, positions from entities or S2C 0x0A0", "[map]")
{
    xi::client::WorldSnapshot w;
    w.charId = 1;
    w.zone   = 230;
    w.party  = { member(7, "Allied", 1), member(1, "Us", 0), member(2, "Near", 0), member(3, "Far", 0), member(4, "Gone", 0) };
    // Far: only the server's 0x0A0 answer for this zone; Gone: an answer for another zone (it has left).
    w.party[3].mapPosKnown = true;
    w.party[3].mapZone     = 230;
    w.party[3].mapPos      = { 10, 0, -20 };
    w.party[4].mapPosKnown = true;
    w.party[4].mapZone     = 231;
    xi::client::Entity near;
    near.id  = 2;
    near.pos = { 5, 1, 6 };
    xi::client::Entity allied;
    allied.id  = 7;
    allied.pos = { -3, 0, 4 };
    const std::vector<std::pair<float, const xi::client::Entity*>> nearby{ { 1.0f, &near }, { 2.0f, &allied } };

    const auto list = xi::ui::mapMarkerMembers(w, nearby);
    REQUIRE(list.size() == 3);
    CHECK(list[0].name == "Near");
    CHECK(list[0].sameParty);
    CHECK(list[0].x == 5);
    CHECK(list[0].z == 6);
    CHECK(list[1].name == "Far");
    CHECK(list[1].z == -20);
    CHECK(list[2].name == "Allied");
    CHECK_FALSE(list[2].sameParty);
}

TEST_CASE("a member in another alliance party counts as alliance by our own party number", "[map]")
{
    xi::client::WorldSnapshot w;
    w.charId = 1;
    w.zone   = 100;
    w.party  = { member(1, "Us", 2), member(5, "Mate", 2), member(6, "Other", 0) };
    xi::client::Entity a, b;
    a.id = 5;
    b.id = 6;
    const std::vector<std::pair<float, const xi::client::Entity*>> nearby{ { 1.0f, &a }, { 1.0f, &b } };
    const auto list = xi::ui::mapMarkerMembers(w, nearby);
    REQUIRE(list.size() == 2);
    CHECK(list[0].sameParty);
    CHECK_FALSE(list[1].sameParty);
}

TEST_CASE("marker pages: own markers with the map, event markers always, members when known", "[map]")
{
    using xi::ui::mapMarkerPageAvailable;
    using xi::ui::mapMarkerStepPage;
    // CODE 0x101fa920: page 0 needs the map (+0xa46), page 1 is always there, 2 / 3 need members.
    CHECK(mapMarkerPageAvailable(0, true, 0));
    CHECK_FALSE(mapMarkerPageAvailable(0, false, 0));
    CHECK(mapMarkerPageAvailable(1, false, 0));
    CHECK_FALSE(mapMarkerPageAvailable(2, true, 0));
    CHECK(mapMarkerPageAvailable(2, true, 1));
    CHECK_FALSE(mapMarkerPageAvailable(3, true, 10));
    CHECK(mapMarkerPageAvailable(3, true, 11));
    // Solo with the map: 0 <-> 1 only; never an empty list (page 1 always has its ten slots).
    CHECK(mapMarkerStepPage(0, 1, true, 0) == 1);
    CHECK(mapMarkerStepPage(1, 1, true, 0) == 0);
    CHECK(mapMarkerStepPage(0, -1, true, 0) == 1);
    // Without the map: page 1 alone.
    CHECK(mapMarkerStepPage(1, 1, false, 0) == 1);
    // In a party: 1 -> 2 -> 0 (3 skipped with ten members or fewer), and back.
    CHECK(mapMarkerStepPage(1, 1, true, 4) == 2);
    CHECK(mapMarkerStepPage(2, 1, true, 4) == 0);
    CHECK(mapMarkerStepPage(0, -1, true, 4) == 2);
    CHECK(mapMarkerStepPage(2, 1, true, 15) == 3);
}

TEST_CASE("marker shapes follow the game's tables", "[map]")
{
    // Slots 5 / 6 of table 0x1038214c.
    CHECK(xi::ui::mapMarkerShape(true) == 154);
    CHECK(xi::ui::mapMarkerShape(false) == 155);
    // Table 0x10382178: page 0 151 / 150, page 1 153 / 152 (empty / placed), page 2 154 for party rows else 155, page 3 155.
    CHECK(xi::ui::mapMarkerSlotShape(0, false) == 151);
    CHECK(xi::ui::mapMarkerSlotShape(0, true) == 150);
    CHECK(xi::ui::mapMarkerSlotShape(1, false) == 153);
    CHECK(xi::ui::mapMarkerSlotShape(1, true) == 152);
    CHECK(xi::ui::mapMarkerListShape(2, 0, 3) == 154);
    CHECK(xi::ui::mapMarkerListShape(2, 3, 3) == 155);
    CHECK(xi::ui::mapMarkerListShape(3, 0, 6) == 155);
}

TEST_CASE("map marker files: M<zone><map>.MRK, magic, CRC-32 and 20 records", "[map]")
{
    CHECK(xi::ui::mapMarkerFileName(230, 0) == "M23000.MRK");
    CHECK(xi::ui::mapMarkerFileName(7, 12) == "M00712.MRK");
    // The standard CRC-32 check value.
    const std::string probe = "123456789";
    CHECK(xi::ui::mapMarkerCrc(std::as_bytes(std::span(probe.data(), probe.size()))) == 0xCBF43926u);

    xi::ui::MapMarkerSlots slots;
    slots[0]  = { 100, 200, "Camp", true };
    slots[3]  = { 5, 6, "Unused", false };
    slots[12] = { 300, 40, "A very long marker name", true };
    const auto file = xi::ui::serializeMapMarkers(slots);
    REQUIRE(file.size() == 0x1C0);
    CHECK(static_cast<uint8_t>(file[0]) == 0x4B); // "KMIX" read as a little-endian u32
    CHECK(static_cast<uint8_t>(file[3]) == 0x58);
    // Record 0 at +8: x, y, name @+4, present @+0x14.
    CHECK(static_cast<uint8_t>(file[8]) == 100);
    CHECK(static_cast<uint8_t>(file[10]) == 200);
    CHECK(static_cast<char>(file[12]) == 'C');
    CHECK(static_cast<uint8_t>(file[8 + 0x14]) == 1);

    const auto back = xi::ui::parseMapMarkers(file);
    REQUIRE(back);
    CHECK((*back)[0].name == "Camp");
    CHECK((*back)[0].present);
    CHECK((*back)[3].name == "Unused");
    CHECK_FALSE((*back)[3].present);
    CHECK((*back)[12].name == "A very long mar"); // 15 characters and the terminator
    CHECK((*back)[12].x == 300);

    // A damaged file (CRC mismatch) or the wrong size is ignored.
    auto bad = file;
    bad[20] = std::byte{ 0x7F };
    CHECK_FALSE(xi::ui::parseMapMarkers(bad));
    CHECK_FALSE(xi::ui::parseMapMarkers(std::span(file).first(0x100)));
    // The older 0x1C8-byte form: magic @0, CRC @8, records @0x10.
    std::vector<std::byte> old(0x1C8);
    std::copy(file.begin(), file.begin() + 4, old.begin());
    std::copy(file.begin() + 4, file.begin() + 8, old.begin() + 8);
    std::copy(file.begin() + 8, file.end(), old.begin() + 0x10);
    const auto older = xi::ui::parseMapMarkers(old);
    REQUIRE(older);
    CHECK((*older)[12].name == "A very long mar");
}
