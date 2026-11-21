// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/mog_house.h"

#include <algorithm>
#include <cstring>

namespace xi::client::mog
{

namespace
{

template <typename T>
void put(std::vector<std::byte>& p, std::size_t at, T v)
{
    std::memcpy(p.data() + at, &v, sizeof(T));
}

// 1F wall slots (DATA 0x1037d9c8, the San d'Oria native room 0x1037da18 at x +-6.233; table chosen by CODE 0x101cca40
// through 0x10337df4): x sign and z.
constexpr float kWall1F[5][2] = { { 1, 4.55f }, { 1, -4.8f }, { -1, -4.8f }, { -1, 0.0f }, { -1, 4.55f } };
constexpr uint8_t kWall1FRot[5] = { 1, 1, 3, 3, 3 };
// 2F wall slots (DATA 0x1037da68, 92 x {x, y, z, 1}): 23 places, each at four heights. West wall x -6.2 (9 places), north
// z 8 (3), east x 6.2 (9), south z -8 (2).
constexpr float kWall2FZ[9]       = { -6.2f, -5.15f, -4.1f, -1.0f, 0.0f, 1.0f, 4.1f, 5.15f, 6.2f };
constexpr float kWall2FHeight[4]  = { -5.6f, -3.2f, -2.5f, -1.8f };
constexpr float kWall2FNorthX[3]  = { -4.0f, 0.0f, 4.0f };
constexpr float kWall2FSouthX[2]  = { -4.0f, 4.0f };

// Exit areas by exit bit (DATA 0x1037ec30; the lists end with 0 and run into each other: list 9 takes the 280 that
// follows it).
constexpr uint16_t kAreas1[] = { 230, 231, 232, 280 };
constexpr uint16_t kAreas2[] = { 234, 235, 236, 280 };
constexpr uint16_t kAreas3[] = { 238, 239, 240, 241, 280 };
constexpr uint16_t kAreas4[] = { 243, 244, 245, 246, 280 };
constexpr uint16_t kAreas5[] = { 48, 50, 280 };
constexpr uint16_t kAreas6[] = { 80 };
constexpr uint16_t kAreas7[] = { 87 };
constexpr uint16_t kAreas8[] = { 94 };
constexpr uint16_t kAreas9[] = { 256, 257, 280 };

std::optional<glm::vec3> wallSlot(uint8_t floor, uint8_t slot, uint16_t mhMap)
{
    if (floor == 0)
    {
        if (slot >= 5)
        {
            return std::nullopt;
        }
        const float x = mhMap == 0x121 ? 6.233f : 6.5f;
        return glm::vec3(kWall1F[slot][0] * x, -3.0f, kWall1F[slot][1]);
    }
    if (slot >= 92)
    {
        return std::nullopt;
    }
    const float y     = kWall2FHeight[slot & 3];
    const int   place = slot >> 2;
    if (place < 9) return glm::vec3(-6.2f, y, kWall2FZ[place]);
    if (place < 12) return glm::vec3(kWall2FNorthX[place - 9], y, 8.0f);
    if (place < 21) return glm::vec3(6.2f, y, kWall2FZ[place - 12]);
    return glm::vec3(kWall2FSouthX[place - 21], y, -8.0f);
}

// Wall slot neighbours by direction 0 up / 1 right / 2 down / 3 left (CODE 0x101ccc50 via 0x101cca20 -> 0x10337de4:
// DATA 0x1037d7dc 1F, 0x1037d7f0 2F; game numbers, not user data).
constexpr uint8_t kWall1FNext[5][4] = {
    { 0, 0, 1, 4 }, { 0, 1, 1, 2 }, { 3, 1, 2, 2 }, { 4, 0, 2, 3 }, { 4, 0, 3, 4 },
};
constexpr uint8_t kWall2FNext[92][4] = {
    { 4, 1, 84, 0 }, { 5, 2, 85, 0 }, { 6, 3, 86, 1 }, { 7, 51, 87, 2 }, { 8, 5, 0, 4 }, { 9, 6, 1, 4 }, { 10, 7, 2, 5 }, { 11, 55, 3, 6 },
    { 12, 9, 4, 8 }, { 13, 10, 5, 8 }, { 14, 11, 6, 9 }, { 15, 59, 7, 10 }, { 16, 13, 8, 12 }, { 17, 14, 9, 12 }, { 18, 15, 10, 13 }, { 19, 63, 11, 14 },
    { 20, 17, 12, 16 }, { 21, 18, 13, 16 }, { 22, 19, 14, 17 }, { 23, 67, 15, 18 }, { 24, 21, 16, 20 }, { 25, 22, 17, 20 }, { 26, 23, 18, 21 }, { 27, 71, 19, 22 },
    { 28, 25, 20, 24 }, { 29, 26, 21, 24 }, { 30, 27, 22, 25 }, { 31, 75, 23, 26 }, { 32, 29, 24, 28 }, { 33, 30, 25, 28 }, { 34, 31, 26, 29 }, { 35, 79, 27, 30 },
    { 36, 33, 28, 32 }, { 37, 34, 29, 32 }, { 38, 35, 30, 33 }, { 39, 83, 31, 34 }, { 36, 40, 37, 32 }, { 36, 41, 38, 33 }, { 37, 42, 39, 34 }, { 38, 43, 87, 35 },
    { 40, 44, 41, 36 }, { 40, 45, 42, 37 }, { 41, 46, 43, 38 }, { 42, 47, 43, 39 }, { 44, 80, 45, 40 }, { 44, 81, 46, 41 }, { 45, 82, 47, 42 }, { 46, 83, 91, 43 },
    { 52, 48, 88, 49 }, { 53, 48, 89, 50 }, { 54, 49, 90, 51 }, { 55, 50, 91, 3 }, { 56, 52, 48, 53 }, { 57, 52, 49, 54 }, { 58, 53, 50, 55 }, { 59, 54, 51, 7 },
    { 60, 56, 52, 57 }, { 61, 56, 53, 58 }, { 62, 57, 54, 59 }, { 63, 58, 55, 11 }, { 64, 60, 56, 61 }, { 65, 60, 57, 62 }, { 66, 61, 58, 63 }, { 67, 62, 59, 15 },
    { 68, 64, 60, 65 }, { 69, 64, 61, 66 }, { 70, 65, 62, 67 }, { 71, 66, 63, 19 }, { 72, 68, 64, 69 }, { 73, 68, 65, 70 }, { 74, 69, 66, 71 }, { 75, 70, 67, 23 },
    { 76, 72, 68, 73 }, { 77, 72, 69, 74 }, { 78, 73, 70, 75 }, { 79, 74, 71, 27 }, { 80, 76, 72, 77 }, { 81, 76, 73, 78 }, { 82, 77, 74, 79 }, { 83, 78, 75, 31 },
    { 44, 80, 76, 81 }, { 45, 80, 77, 82 }, { 46, 81, 78, 83 }, { 47, 82, 79, 35 }, { 85, 88, 84, 0 }, { 86, 89, 84, 1 }, { 87, 90, 85, 2 }, { 39, 91, 86, 3 },
    { 89, 48, 88, 84 }, { 90, 49, 88, 85 }, { 91, 50, 89, 86 }, { 47, 51, 90, 87 },
};

} // namespace

std::size_t wallSlots(uint8_t floor)
{
    return floor == 0 ? 5 : 92;
}

uint8_t wallSlotStep(uint8_t floor, uint8_t slot, int dir)
{
    dir &= 3;
    if (floor == 0)
    {
        return slot < 5 ? kWall1FNext[slot][dir] : 0;
    }
    return slot < 92 ? kWall2FNext[slot][dir] : 0;
}

uint8_t wallSlotRotation(uint8_t floor, uint8_t slot)
{
    if (floor == 0)
    {
        return slot < 5 ? kWall1FRot[slot] : 0;
    }
    return slot < 36 ? 3 : slot < 48 ? 0 : slot < 84 ? 1 : 2;
}

std::optional<Pose> furniturePose(const dat::FurnitureRow& row, uint8_t col, uint8_t level, uint8_t gridRow, uint8_t rot, uint8_t floor,
                                  uint16_t mhMap)
{
    Pose p;
    p.yaw = static_cast<float>(rot & 3) * 1.5707963f; // CODE 0x101cdb00: euler (0, rot * pi / 2, 0)
    if (row.flags & 1)
    {
        const auto at = wallSlot(floor, col, mhMap);
        if (!at)
        {
            return std::nullopt;
        }
        p.pos  = *at;
        p.wall = true;
        return p;
    }
    // CODE 0x101cd940: origin per floor (DATA 0x1037d7b4: 1F (10, 0, 12), 2F (10, 0, 13)), 0.6 per cell, the model at the
    // footprint's centre, 0.1 per level (y is down).
    const auto [w, d] = footprint(row, rot);
    p.pos = { (static_cast<float>(col) - 10.0f) * 0.6f + static_cast<float>(w) * 0.3f, -static_cast<float>(level) * 0.1f,
              (static_cast<float>(gridRow) - (floor ? 13.0f : 12.0f)) * 0.6f + static_cast<float>(d) * 0.3f };
    return p;
}

std::optional<dat::ZoneLine> furnitureBox(const dat::FurnitureRow& row, const Pose& pose, uint8_t rot)
{
    if (pose.wall || pose.pos.y < -0.1f - 1e-4f)
    {
        return std::nullopt;
    }
    const auto [w, d] = footprint(row, rot);
    const float  below = static_cast<float>(row.height) * 0.01f * 0.5f;
    dat::ZoneLine b;
    b.pos[0]  = pose.pos.x;
    b.pos[1]  = pose.pos.y + (below - 1.0f) * 0.5f;
    b.pos[2]  = pose.pos.z;
    b.size[0] = static_cast<float>(w) * 0.6f;
    b.size[1] = 1.0f + below;
    b.size[2] = static_cast<float>(d) * 0.6f;
    return b;
}

std::optional<dat::ZoneLine> furnitureCameraBox(const dat::FurnitureRow& row, const Pose& pose, uint8_t rot)
{
    if (pose.wall || pose.pos.y < -0.1f - 1e-4f)
    {
        return std::nullopt;
    }
    const auto [w, d] = footprint(row, rot);
    const float  h    = std::max(static_cast<float>(row.height) * 0.01f, 0.05f);
    dat::ZoneLine b;
    b.pos[0]  = pose.pos.x;
    b.pos[1]  = pose.pos.y - h * 0.5f; // y is down: from the floor up to h
    b.pos[2]  = pose.pos.z;
    b.size[0] = static_cast<float>(w) * 0.6f;
    b.size[1] = h;
    b.size[2] = static_cast<float>(d) * 0.6f;
    return b;
}

std::pair<uint8_t, uint8_t> footprint(const dat::FurnitureRow& row, uint8_t rot)
{
    return (rot & 1) ? std::pair{ row.d, row.w } : std::pair{ row.w, row.d };
}

bool validFloorCell(uint8_t col, uint8_t row, uint8_t floor)
{
    if (floor)
    {
        return col <= 19 && row <= 25;
    }
    if (row > 23)
    {
        return false;
    }
    if (col <= 5 || (col >= 14 && col <= 19))
    {
        return true;
    }
    return col >= 6 && col <= 13 && row >= 18;
}

bool footprintInRoom(const dat::FurnitureRow& row, int col, int gridRow, uint8_t rot, uint8_t floor)
{
    const auto [w, d] = footprint(row, rot);
    return col >= 0 && gridRow >= 0 && col + std::max<int>(w, 1) <= 20 && gridRow + std::max<int>(d, 1) <= (floor ? 26 : 24);
}

bool footprintFits(const dat::FurnitureRow& row, uint8_t col, uint8_t gridRow, uint8_t rot, uint8_t floor)
{
    const auto [w, d] = footprint(row, rot);
    for (int x = col; x < col + std::max<int>(w, 1); ++x)
    {
        for (int z = gridRow; z < gridRow + std::max<int>(d, 1); ++z)
        {
            if (x > 255 || z > 255 || !validFloorCell(static_cast<uint8_t>(x), static_cast<uint8_t>(z), floor))
            {
                return false;
            }
        }
    }
    return true;
}

std::vector<std::byte> layoutPacket(uint16_t itemId, uint8_t slot, uint8_t container, uint8_t floor, uint8_t col, uint8_t level,
                                    uint8_t gridRow, uint8_t rot)
{
    std::vector<std::byte> p(0x0C);
    put<uint16_t>(p, 0, itemId);
    put<uint8_t>(p, 2, slot);
    put<uint8_t>(p, 3, container);
    put<uint8_t>(p, 4, floor);
    put<uint8_t>(p, 5, col);
    put<uint8_t>(p, 6, level);
    put<uint8_t>(p, 7, gridRow);
    put<uint8_t>(p, 8, rot);
    return p;
}

std::vector<std::byte> bankInPacket(uint16_t itemId, uint8_t slot, uint8_t container)
{
    std::vector<std::byte> p(4);
    put<uint16_t>(p, 0, itemId);
    put<uint8_t>(p, 2, slot);
    put<uint8_t>(p, 3, container);
    return p;
}

std::vector<std::byte> myroomIsPacket(uint8_t kind, uint8_t param1, uint16_t param2)
{
    std::vector<std::byte> p(4);
    put<uint8_t>(p, 0, kind);
    put<uint8_t>(p, 1, param1);
    put<uint16_t>(p, 2, param2);
    return p;
}

std::vector<std::byte> plantAddPacket(uint16_t potItem, uint8_t potSlot, uint8_t potContainer, uint16_t addItem, uint8_t addSlot,
                                      uint8_t addContainer)
{
    std::vector<std::byte> p(8);
    put<uint16_t>(p, 0, potItem);
    put<uint16_t>(p, 2, addItem);
    put<uint8_t>(p, 4, potSlot);
    put<uint8_t>(p, 5, addSlot);
    put<uint8_t>(p, 6, potContainer);
    put<uint8_t>(p, 7, addContainer);
    return p;
}

std::vector<std::byte> plantPotPacket(uint16_t potItem, uint8_t potSlot, uint8_t potContainer)
{
    return bankInPacket(potItem, potSlot, potContainer); // the same {ItemNo, ItemIndex, Category}
}

std::vector<std::byte> plantCropPacket(uint16_t potItem, uint8_t potSlot, uint8_t potContainer, bool throwAway)
{
    std::vector<std::byte> p(8);
    put<uint16_t>(p, 0, potItem);
    put<uint8_t>(p, 2, potSlot);
    put<uint8_t>(p, 3, potContainer);
    put<uint8_t>(p, 4, throwAway ? 1 : 0);
    return p;
}

int plantMenu(uint8_t step, bool dried)
{
    static constexpr int kByStep[12] = { 1, 6, 2, 2, 3, 2, 2, 3, 2, 2, 4, 6 }; // DATA 0x103854f4
    if (step >= 12)
    {
        return 0;
    }
    return dried ? 6 : kByStep[step];
}

uint16_t plantExamineMessage(uint8_t step, bool dried, int coin)
{
    // The jump table 0x102064a4 by Step: base message, then + the coin toss.
    static constexpr uint16_t kByStep[12] = { 62, 63, 65, 65, 67, 65, 65, 67, 65, 69, 71, 73 };
    if (dried)
    {
        return 139;
    }
    if (step >= 12)
    {
        return 0;
    }
    return static_cast<uint16_t>(kByStep[step] + (step ? (coin & 1) : 0));
}

std::pair<uint8_t, uint8_t> newPieceStart(const dat::FurnitureRow& row, uint8_t floor, std::optional<std::pair<uint8_t, uint8_t>> lastMoved)
{
    if (row.flags & 1)
    {
        return { 0, 0 };
    }
    const auto [col0, row0] = lastMoved.value_or(std::pair<uint8_t, uint8_t>{ 9, 12 });
    const int w = std::max<int>(row.w, 1), d = std::max<int>(row.d, 1);
    const int rows = floor ? 26 : 24;
    const int c = std::clamp(col0 - row.w / 2, 0, std::max(0, 20 - w));
    const int r = std::clamp(row0 - row.d / 2, 0, std::max(0, rows - d));
    return { static_cast<uint8_t>(c), static_cast<uint8_t>(r) };
}

std::vector<std::byte> subcontainerPacket(uint8_t kind, uint8_t container, uint8_t slot, uint8_t gearSlot, uint8_t itemContainer,
                                          uint8_t itemSlot)
{
    std::vector<std::byte> p(0x1C);
    put<uint32_t>(p, 0, kind);
    put<uint32_t>(p, 4, container);
    put<uint8_t>(p, 8, slot);
    put<uint8_t>(p, 9, gearSlot);
    put<uint32_t>(p, 0x0C, itemContainer);
    put<uint8_t>(p, 0x10, itemSlot);
    return p;
}

std::span<const uint16_t> exitAreas(uint8_t exitBit)
{
    switch (exitBit)
    {
        case 1: return kAreas1;
        case 2: return kAreas2;
        case 3: return kAreas3;
        case 4: return kAreas4;
        case 5: return kAreas5;
        case 6: return kAreas6;
        case 7: return kAreas7;
        case 8: return kAreas8;
        case 9: return kAreas9;
        default: return {};
    }
}

std::optional<uint8_t> doorTopMode(int row, uint8_t floor)
{
    if (row == 2)
    {
        return 0;
    }
    if (row == 3)
    {
        return static_cast<uint8_t>(floor ? 126 : 125);
    }
    return std::nullopt;
}

std::optional<uint8_t> doorAreaMode(uint8_t exitBit, int row)
{
    const auto areas = exitAreas(exitBit);
    if (row < 1 || static_cast<std::size_t>(row) > areas.size())
    {
        return std::nullopt;
    }
    return static_cast<uint8_t>(areas[static_cast<std::size_t>(row) - 1] == 280 ? 127 : row);
}

} // namespace xi::client::mog
