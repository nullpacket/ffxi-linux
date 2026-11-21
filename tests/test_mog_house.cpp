// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/mog_house.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace xi::client;
namespace dat = xi::dat;
using Catch::Approx;

TEST_CASE("mog house: floor furniture stands at its footprint's centre, wall items in fixed slots")
{
    dat::FurnitureRow bed{ 0, 2, 4, 6, 70 }; // Simple Bed, 4 x 6
    auto p = mog::furniturePose(bed, 10, 0, 12, 0, 0, 0x121);
    REQUIRE(p);
    CHECK(p->pos.x == Approx(1.2f));
    CHECK(p->pos.z == Approx(1.8f));
    CHECK_FALSE(p->wall);
    // A quarter turn swaps the footprint; 2F rows start at 13.
    p = mog::furniturePose(bed, 10, 3, 13, 1, 1, 0x267);
    REQUIRE(p);
    CHECK(p->pos.x == Approx(1.8f));
    CHECK(p->pos.y == Approx(-0.3f));
    CHECK(p->pos.z == Approx(1.2f));

    dat::FurnitureRow flag{ 1, 96, 1, 1, 100 };
    p = mog::furniturePose(flag, 3, 0, 0, 3, 0, 0x121);
    REQUIRE(p);
    CHECK(p->wall);
    CHECK(p->pos.x == Approx(-6.233f));
    CHECK(p->pos.z == Approx(0.0f));
    CHECK_FALSE(mog::furniturePose(flag, 5, 0, 0, 0, 0, 0x121));
    // 2F: 92 slots, four heights per place; slot 41 = north wall x 0 at -3.2, slot 91 = south wall x 4 at -1.8.
    CHECK(mog::wallSlots(1) == 92);
    p = mog::furniturePose(flag, 41, 0, 0, 0, 1, 0x267);
    REQUIRE(p);
    CHECK(p->pos.x == Approx(0.0f));
    CHECK(p->pos.y == Approx(-3.2f));
    CHECK(p->pos.z == Approx(8.0f));
    p = mog::furniturePose(flag, 91, 0, 0, 0, 1, 0x267);
    REQUIRE(p);
    CHECK(p->pos.x == Approx(4.0f));
    CHECK(p->pos.y == Approx(-1.8f));
    CHECK(p->pos.z == Approx(-8.0f));
    CHECK(mog::wallSlotRotation(1, 0) == 3);
    CHECK(mog::wallSlotRotation(1, 40) == 0);
    CHECK(mog::wallSlotRotation(1, 60) == 1);
    CHECK(mog::wallSlotRotation(1, 90) == 2);
}

TEST_CASE("mog house: layout cells, packets and the exit door's modes")
{
    CHECK(mog::validFloorCell(0, 0, 0));
    CHECK_FALSE(mog::validFloorCell(10, 10, 0)); // the walkway to the door
    CHECK(mog::validFloorCell(10, 18, 0));
    CHECK(mog::validFloorCell(10, 10, 1));
    dat::FurnitureRow bed{ 0, 2, 4, 6, 70 };
    CHECK(mog::footprintFits(bed, 0, 0, 0, 0));
    CHECK_FALSE(mog::footprintFits(bed, 3, 0, 0, 0)); // cols 3..6 cross into the walkway
    CHECK_FALSE(mog::footprintFits(bed, 0, 20, 0, 0));

    const auto lay = mog::layoutPacket(2, 5, 1, 0, 3, 0, 7, 1);
    REQUIRE(lay.size() == 0x0C);
    CHECK(static_cast<int>(lay[0]) == 2);
    CHECK(static_cast<int>(lay[2]) == 5);
    CHECK(static_cast<int>(lay[3]) == 1);
    CHECK(static_cast<int>(lay[5]) == 3);
    CHECK(static_cast<int>(lay[7]) == 7);
    CHECK(static_cast<int>(lay[8]) == 1);
    const auto sub = mog::subcontainerPacket(1, 1, 4, 3, 2, 9);
    REQUIRE(sub.size() == 0x1C);
    CHECK(static_cast<int>(sub[0]) == 1);
    CHECK(static_cast<int>(sub[8]) == 4);
    CHECK(static_cast<int>(sub[9]) == 3);
    CHECK(static_cast<int>(sub[0x0C]) == 2);
    CHECK(static_cast<int>(sub[0x10]) == 9);

    CHECK_FALSE(mog::doorTopMode(1, 0));
    CHECK(*mog::doorTopMode(2, 0) == 0);
    CHECK(*mog::doorTopMode(3, 0) == 125);
    CHECK(*mog::doorTopMode(3, 1) == 126);
    CHECK(mog::exitAreas(1).size() == 4);
    CHECK(*mog::doorAreaMode(1, 2) == 2);
    CHECK(*mog::doorAreaMode(1, 4) == 127); // Mog Garden
    CHECK_FALSE(mog::doorAreaMode(1, 5));
}

TEST_CASE("mog house: placing keys step wall slots by the game's neighbour table")
{
    // CODE 0x101ccc50 / DATA 0x1037d7f0: 2F slot 0 -> up 4, right 1, down 84, left 0.
    CHECK(mog::wallSlotStep(1, 0, 0) == 4);
    CHECK(mog::wallSlotStep(1, 0, 1) == 1);
    CHECK(mog::wallSlotStep(1, 0, 2) == 84);
    for (uint8_t s = 4; s < 32; ++s)
    {
        CHECK(mog::wallSlotStep(1, mog::wallSlotStep(1, s, 0), 2) == s); // west wall: up then down comes back
    }
    CHECK(mog::wallSlotStep(0, 1, 3) == 2); // 1F: slot 1 left -> 2
    CHECK(mog::kPlaceStep[0][1] == 1);      // up = row + 1
}

TEST_CASE("mog house: furniture blocks as the game's box (footprint, 1 above the floor)")
{
    // CODE 0x1016d490: footprint 0.6 per cell, top fixed 1.0 above the origin (y down), bottom height / 2 below.
    dat::FurnitureRow bed{ 0, 2, 4, 6, 70 };
    const auto        pose = mog::furniturePose(bed, 2, 0, 3, 1, 0, 0);
    REQUIRE(pose);
    const auto b = mog::furnitureBox(bed, *pose, 1);
    REQUIRE(b);
    CHECK(b->size[0] == Approx(3.6f)); // turned: d x w
    CHECK(b->size[2] == Approx(2.4f));
    CHECK(b->pos[1] - b->size[1] * 0.5f == Approx(pose->pos.y - 1.0f));
    CHECK(b->pos[1] + b->size[1] * 0.5f == Approx(pose->pos.y + 0.35f));
    // A rug (height 0.01) blocks the same 1.0 up; level 1 still blocks, level 2 (on a table) and wall items do not.
    dat::FurnitureRow rug{ 0, 256, 3, 2, 1 };
    CHECK(mog::furnitureBox(rug, *mog::furniturePose(rug, 2, 0, 3, 0, 0, 0), 0));
    CHECK(mog::furnitureBox(rug, *mog::furniturePose(rug, 2, 1, 3, 0, 0, 0), 0));
    CHECK_FALSE(mog::furnitureBox(rug, *mog::furniturePose(rug, 2, 2, 3, 0, 0, 0), 0));
    dat::FurnitureRow painting{ 1, 300, 1, 1, 50 };
    if (const auto wp = mog::furniturePose(painting, 0, 0, 0, 0, 0, 0))
    {
        CHECK_FALSE(mog::furnitureBox(painting, *wp, 0));
    }
}

TEST_CASE("mog house: gardening menus, the moogle's remarks and the packets")
{
    // "menu plnt<n>" by Step (DATA 0x103854f4), plnt6 for a dried plant or Step 1 / 11.
    CHECK(mog::plantMenu(0, false) == 1);
    CHECK(mog::plantMenu(1, false) == 6);
    CHECK(mog::plantMenu(2, false) == 2);
    CHECK(mog::plantMenu(4, false) == 3);
    CHECK(mog::plantMenu(7, false) == 3);
    CHECK(mog::plantMenu(10, false) == 4);
    CHECK(mog::plantMenu(11, false) == 6);
    CHECK(mog::plantMenu(5, true) == 6);
    CHECK(mog::plantMenu(12, false) == 0);
    // Examine (CODE 0x102063e0): 7031 #62 empty, #65/#66 growing, #67/#68 wants a crystal, #71/#72 ripe, #139 dried.
    CHECK(mog::plantExamineMessage(0, false, 1) == 62);
    CHECK(mog::plantExamineMessage(3, false, 1) == 66);
    CHECK(mog::plantExamineMessage(4, false, 0) == 67);
    CHECK(mog::plantExamineMessage(10, false, 0) == 71);
    CHECK(mog::plantExamineMessage(9, true, 0) == 139);
    // C2S 0x0FC payload (after the header): PlantItemNo, AddItemNo, PlantItemIndex, AddItemIndex, the two categories.
    const auto add = mog::plantAddPacket(218, 3, 1, 573, 7, 9);
    REQUIRE(add.size() == 8);
    CHECK(static_cast<int>(add[0]) == 218);
    CHECK((static_cast<int>(add[2]) | static_cast<int>(add[3]) << 8) == 573);
    CHECK(static_cast<int>(add[4]) == 3);
    CHECK(static_cast<int>(add[5]) == 7);
    CHECK(static_cast<int>(add[6]) == 1);
    CHECK(static_cast<int>(add[7]) == 9);
    const auto crop = mog::plantCropPacket(218, 3, 1, true);
    REQUIRE(crop.size() == 8);
    CHECK(static_cast<int>(crop[4]) == 1);
    // C2S 0x0CB: Kind, Param1, Param2 (the remodel style).
    const auto is = mog::myroomIsPacket(5, 0, 617);
    REQUIRE(is.size() == 4);
    CHECK(static_cast<int>(is[0]) == 5);
    CHECK((static_cast<int>(is[2]) | static_cast<int>(is[3]) << 8) == 617);
}

TEST_CASE("mog house: a piece from the safe starts centred on col 9 row 12 or on the piece last moved")
{
    dat::FurnitureRow bed{ 0, 2, 4, 6, 70 };
    CHECK(mog::newPieceStart(bed, 0, std::nullopt) == std::pair<uint8_t, uint8_t>{ 7, 9 });
    CHECK(mog::newPieceStart(bed, 0, std::pair<uint8_t, uint8_t>{ 1, 1 }) == std::pair<uint8_t, uint8_t>{ 0, 0 }); // clamped
    CHECK(mog::newPieceStart(bed, 1, std::pair<uint8_t, uint8_t>{ 19, 25 }) == std::pair<uint8_t, uint8_t>{ 16, 20 });
    dat::FurnitureRow flag{ 1, 300, 1, 1, 50 };
    CHECK(mog::newPieceStart(flag, 0, std::pair<uint8_t, uint8_t>{ 5, 5 }) == std::pair<uint8_t, uint8_t>{ 0, 0 });
}
