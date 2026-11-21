// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/zone.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The Mog House (docs/protocol-map.md §6.1, RE notes moghouse_re.md / furniture_re.md / mog_layout_re.md): where a
// furnishing stands, the exit door's menu, and the layout packets. No drawing or input here.
namespace xi::client
{

namespace mog
{

// Furniture placement from an item's exdata (bytes 6 / 7 / 8 / 9 = col, level, row, rotation; CODE 0x101cd940).
struct Pose
{
    glm::vec3 pos{ 0.0f }; // game space (y down)
    float     yaw  = 0.0f; // radians about +y
    bool      wall = false;
};
// `floor` 0 = 1F, 1 = 2F; `mhMap` = MyroomMapNumber (the San d'Oria native room's 1F walls stand closer). Wall items
// (furn flags bit 0) take slot `col` of the floor's wall table; nullopt for a slot past the table.
std::optional<Pose> furniturePose(const dat::FurnitureRow& row, uint8_t col, uint8_t level, uint8_t gridRow, uint8_t rot, uint8_t floor,
                                  uint16_t mhMap);
// The box a furnishing blocks the player with (CODE 0x1016ee32 -> 0x1016d490, sizes from CODE 0x101cd2a4), game space,
// yaw 0 with the footprint already turned: footprint w x d cells of 0.6 (the furn row, not the model) centred on the
// piece; vertically from 1.0 above its origin (a constant, the row's height is not used) to height / 2 below. Only
// pieces standing at y >= -0.1 block (floor level 0 / 1; wall items and ones on a table do not), whatever their
// height, so rugs block too. The game tests pieces within 4 of the player.
std::optional<dat::ZoneLine> furnitureBox(const dat::FurnitureRow& row, const Pose& pose, uint8_t rot);
// The camera's box for a floor piece: the footprint from the floor up to the furnishing's full height (height · 0.01).
// The movement box above tops out 1.0 above the floor, level with the camera ray's start, so the camera never met it;
// the game tests the piece's own model collision (0x106306b4 -> 0x100c4af0), which this box stands in for (INFERRED).
std::optional<dat::ZoneLine> furnitureCameraBox(const dat::FurnitureRow& row, const Pose& pose, uint8_t rot);
// Wall slots on a floor (1F: 5; 2F: 92 = 23 places x 4 heights) and a slot's default rotation (DATA 0x1037d960 /
// 0x1037d968: 1F 1, 1, 3, 3, 3; 2F 3 (west wall), 0 (north), 1 (east), 2 (south)).
std::size_t wallSlots(uint8_t floor);
uint8_t     wallSlotRotation(uint8_t floor, uint8_t slot);
// The wall slot next to `slot` in direction 0 up / 1 right / 2 down / 3 left (the game's table, CODE 0x101ccc50).
uint8_t     wallSlotStep(uint8_t floor, uint8_t slot, int dir);
// Placing direction 0 up / 1 right / 2 down / 3 left -> grid step (col, row) (CODE 0x101cccb9).
constexpr int kPlaceStep[4][2] = { { 0, 1 }, { 1, 0 }, { 0, -1 }, { -1, 0 } };

// Floor cells a footprint may cover (LSB 0x0fa_myroom_layout.cpp isValidFloorCell: 1F is a U around the entrance,
// 2F 20 x 26): col (x) 0..19, row (z) 0..23 / 0..25.
bool validFloorCell(uint8_t col, uint8_t row, uint8_t floor);
// The footprint (w, d) turned by `rot` quarter turns.
std::pair<uint8_t, uint8_t> footprint(const dat::FurnitureRow& row, uint8_t rot);
bool footprintFits(const dat::FurnitureRow& row, uint8_t col, uint8_t gridRow, uint8_t rot, uint8_t floor);
// The footprint inside the room's grid (1F 20 x 24, 2F 20 x 26), free cells or not: where a piece being placed may move.
bool footprintInRoom(const dat::FurnitureRow& row, int col, int gridRow, uint8_t rot, uint8_t floor);

// C2S 0x0FA MYROOM_LAYOUT (CODE 0x100f0bb0, size 0x10): ItemNo @4, ItemIndex @6, Category @7, FloorFlg @8, x (col) @9,
// y (level) @0xA, z (row) @0xB, v (rotation) @0xC. ItemNo 0 = leaving layout mode. Payload after the header.
std::vector<std::byte> layoutPacket(uint16_t itemId, uint8_t slot, uint8_t container, uint8_t floor, uint8_t col, uint8_t level,
                                    uint8_t gridRow, uint8_t rot);
// C2S 0x0FB MYROOM_BANKIN (CODE 0x100f0c70, size 8): put a furnishing away: ItemNo @4, ItemIndex @6, Category @7.
std::vector<std::byte> bankInPacket(uint16_t itemId, uint8_t slot, uint8_t container);
// C2S 0x03B SUBCONTAINER (LSB 0x03b_subcontainer.h): a mannequin's gear: Kind @4 (1 equip, 2 unequip, 5 unequip all),
// Category1 @8 / ItemIndex1 @0xC (the mannequin), ContainerIndex @0xD (0 main .. 7 feet), Category2 @0x10 /
// ItemIndex2 @0x14 (the item, from Storage).
std::vector<std::byte> subcontainerPacket(uint8_t kind, uint8_t container, uint8_t slot, uint8_t gearSlot, uint8_t itemContainer,
                                          uint8_t itemSlot);

// C2S 0x0CB MYROOM_IS (CODE 0x100f0970, size 8): Kind @4 (1 open the Mog House to visitors, 2 close it, 5 remodel 2F),
// Param1 @5, Param2 @6 (remodel: the style 615 San d'Orian .. 618 Mog Patio).
std::vector<std::byte> myroomIsPacket(uint8_t kind, uint8_t param1, uint16_t param2);
// Gardening (CODE 0x100f0cd0 / 0x100f0d40 / 0x100f0da0 / 0x100f0e00; LSB 0x0fc..0x0ff): the pot is a Mog Safe item.
// C2S 0x0FC MYROOM_PLANT_ADD (size 0xC): sow seeds or feed a crystal: PlantItemNo @4, AddItemNo @6, PlantItemIndex @8,
// AddItemIndex @9, PlantCategory @0xA, AddCategory @0xB.
std::vector<std::byte> plantAddPacket(uint16_t potItem, uint8_t potSlot, uint8_t potContainer, uint16_t addItem, uint8_t addSlot,
                                      uint8_t addContainer);
// C2S 0x0FD MYROOM_PLANT_CHECK (8, examine) and 0x0FF MYROOM_PLANT_STOP (8, dry): ItemNo @4, ItemIndex @6, Category @7.
std::vector<std::byte> plantPotPacket(uint16_t potItem, uint8_t potSlot, uint8_t potContainer);
// C2S 0x0FE MYROOM_PLANT_CROP (size 0xC): ItemNo @4, ItemIndex @6, Category @7, CancellFlg @8 (0 harvest, 1 throw the
// plant away).
std::vector<std::byte> plantCropPacket(uint16_t potItem, uint8_t potSlot, uint8_t potContainer, bool throwAway);

// A flowerpot's exdata (LSB exdata/flower_pot.h): Step @0 (0 empty, 1 sown .. 10 mature, 11 withered), Dried = byte 1
// bit 7. The gardening menu for it, "menu plnt<n>" (CODE 0x102071ab: table 0x103854f4 by Step, plnt6 when dried):
// 1 Examine / Sow / Return; 2 Examine / Dry / Throw away / Return; 3 Examine / Feed a crystal / Dry / Throw away /
// Return; 4 Examine / Harvest / Dry / Throw away / Return; 6 Examine / Throw away / Return (plnt5, Examine / Harvest /
// Return, is not in the table). 0 = no menu (Step past the table).
int plantMenu(uint8_t step, bool dried);
// What the moogle says on Examine (CODE 0x102063e0, standard messages 7031): dried 139; Step 0 -> 62, 1 -> 63, 2 3 5 6 8
// -> 65, 4 7 (a crystal wanted) -> 67, 9 -> 69, 10 -> 71, 11 -> 73; all but 0 add a coin toss (0 / 1). 0 = none.
uint16_t plantExamineMessage(uint8_t step, bool dried, int coin);

// Where a furnishing taken from the safe starts (CODE 0x101cd354..0x101cd44f): a wall piece in slot 0 with the slot's
// facing; a floor piece at col 9 row 12 (or at the piece last moved in this layout session, the room's +0x7e6, when that
// is a floor piece) turned 0, then shifted by minus half its footprint and clamped into the room (0x101ccd00 ->
// 0x101cca70). Returns col, row (wall: slot, 0).
std::pair<uint8_t, uint8_t> newPieceStart(const dat::FurnitureRow& row, uint8_t floor, std::optional<std::pair<uint8_t, uint8_t>> lastMoved);

// The exit door's "menu mogdoor" (CODE 0x101d5bf0 / 0x101d5e60 / 0x101d60f0; strings 55648).
struct DoorMenu
{
    bool     ownRoom = false;   // 0x101cbc50: in our own Mog House (row 2 "Area you entered from." vs "Leave Mog House.")
    bool     floor2  = false;   // row 3 "Change floors." works (0x101d5dd0: own nation room or 2F, and 2F unlocked)
    bool     garden  = false;   // Mog Garden in the area list (dimmed otherwise; refused with 55646 #190)
    uint8_t  exitBit = 0;       // the area list: table 0x1037ec30[exitBit]
};
// The area list for an exit bit (zone ids; 280 = Mog Garden), DATA table 0x1037ec30.
std::span<const uint16_t> exitAreas(uint8_t exitBit);
// The exit mode a mogdoor choice sends in C2S 0x05E `zmrq` (+0x17): top rows 1 stay (nullopt), 2 -> 0, 3 -> 125 on 1F /
// 126 on 2F; area row i (1-based) -> i, Mog Garden -> 127.
std::optional<uint8_t> doorTopMode(int row, uint8_t floor);
std::optional<uint8_t> doorAreaMode(uint8_t exitBit, int row);

} // namespace mog

} // namespace xi::client
