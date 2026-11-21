// SPDX-License-Identifier: GPL-3.0-or-later
#include "mog_house.h"

#include "characters.h"
#include "gfx/zone_scene.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace xi::app
{

namespace
{

constexpr uint8_t kStorage = 2; // LSB LOC_STORAGE: the mannequins' gear comes from here (0x03b_subcontainer.cpp)
// Mannequin gear slots 0 main .. 7 feet (C2S 0x03B ContainerIndex) -> the item's equip slot bit (ItemInfo::slots).
constexpr uint8_t kGearBit[8] = { 0, 1, 2, 4, 5, 6, 7, 8 };
constexpr const char* kGearName[8] = { "Main", "Sub", "Range", "Head", "Body", "Hands", "Legs", "Feet" };

bool installedHere(const client::InventoryItem& it, uint8_t floor)
{
    return (it.container == 1 || it.container == 9) && (it.attr[1] & 0x40) && (it.attr[1] & 3) == floor;
}

uint32_t actorIdOf(uint8_t container, uint8_t slot)
{
    return MogHouseView::kFurnitureBase + static_cast<uint32_t>(container) * 100u + slot;
}

// Own nation's Mog House or 2F (CODE 0x101cbc50 / 0x100f9580: map 0x121..0x123 or floor != 0).
bool ownRoom(const client::WorldSnapshot& w)
{
    return w.mhFloor != 0 || (w.mhMap >= 0x121 && w.mhMap <= 0x123);
}

} // namespace

MogHouseView::MogHouseView(const dat::DatIndex& index, const dat::ItemTable& items, Strings s)
    : index_(index)
    , items_(items)
    , s_(s)
    , furn_(dat::loadFurnitureTable(index))
{
}

std::string MogHouseView::text(const dat::StringTable* t, uint32_t id, const char* fallback) const
{
    const auto v = t ? t->get(id) : std::string_view();
    return v.empty() ? std::string(fallback) : std::string(v);
}

const client::InventoryItem* MogHouseView::item(const client::WorldSnapshot& w, uint8_t container, uint8_t slot) const
{
    for (const auto& it : w.inventory)
    {
        if (it.container == container && it.slot == slot && it.itemId != 0)
        {
            return &it;
        }
    }
    return nullptr;
}

const dat::FurnitureRow* MogHouseView::furnRow(uint16_t itemId) const
{
    const auto row = itemId >= 0xE00 ? itemId - 0xC00u : static_cast<uint32_t>(itemId);
    return row < furn_.size() && furn_[row].itemId == itemId ? &furn_[row] : nullptr;
}

MogHouseView::Furniture MogHouseView::furniture(const client::WorldSnapshot& w) const
{
    Furniture out;
    if (!w.inMogHouse)
    {
        return out;
    }
    auto add = [&](uint16_t itemId, uint8_t container, uint8_t slot, uint8_t col, uint8_t level, uint8_t gridRow, uint8_t rot,
                   const client::InventoryItem* it, bool hidden) {
        const auto  info = items_.find(itemId);
        const auto* row  = furnRow(itemId);
        const auto  file = dat::furnitureFileId(itemId);
        // Type 10 furnishing, 12 flowerpot, 14 mannequin (CODE 0x101d26e0 scan).
        if (!info || (info->type != 10 && info->type != 12 && info->type != 14) || !row || (!file && info->type != 14))
        {
            return;
        }
        if (hidden)
        {
            // A static model not shown now: built into the scene hidden, so picking it to place (or moving it between
            // floors) does not change the scene's model set and rebuild the room.
            if (info->type != 14 && !(itemId >= 264 && itemId < 300))
            {
                out.hidden.push_back(out.props.size());
                out.props.push_back(dat::ZoneProp{ .fileId = *file });
                out.key = out.key * 1000003u + (static_cast<uint64_t>(*file) << 16 | static_cast<uint64_t>(container) << 8 | slot) + 1;
            }
            return;
        }
        const auto pose = client::mog::furniturePose(*row, col, level, gridRow, rot, w.mhFloor, w.mhMap);
        if (!pose)
        {
            return;
        }
        Piece pc{ itemId, container, slot, *row, col, level, gridRow, rot, std::nullopt, 0 };
        // Collision: the game's box per floor piece (client::mog::furnitureBox).
        if (const auto b = client::mog::furnitureBox(*row, *pose, rot))
        {
            out.boxes.push_back(*b);
        }
        if (const auto b = client::mog::furnitureCameraBox(*row, *pose, rot))
        {
            out.cameraBoxes.push_back(*b);
        }
        if (info->type == 14)
        {
            // Mannequins (CODE 0x101ccf60 type 14): a PC actor of the mannequin's race (exdata +0x12) in the gear S2C 0x026
            // lists; LSB sends face / hair 1, the game's mannequin head is face 31 (LSB 0x026_item_subcontainer.cpp).
            client::Entity e;
            e.id   = actorIdOf(container, slot);
            const uint8_t race = it && it->attr[0x12] ? it->attr[0x12] : static_cast<uint8_t>(itemId - 255);
            e.look.kind  = 0xFFFF;
            e.look.valid = true;
            e.look.ids   = { static_cast<uint16_t>(race << 8 | 31), 0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x7000, 0x8000 };
            if (const auto m = w.mannequins.find(static_cast<uint16_t>(container << 8 | slot)); m != w.mannequins.end())
            {
                for (std::size_t k = 1; k < 9; ++k)
                {
                    e.look.ids[k] = m->second.ids[k];
                }
            }
            e.pos = pose->pos;
            e.dir = static_cast<uint8_t>((rot & 3) * 64);
            e.hpp = 100;
            pc.actorId = e.id;
            out.actors.push_back(e);
            out.mannequinPoses[e.id] = { race, it ? it->attr[0x13] : uint8_t{ 0 } };
        }
        else if (itemId >= 264 && itemId < 300)
        {
            // Statues and the campfire: NPC-format models (skeleton, meshes, idl0) in the furniture file (CODE 0x100ab220
            // kind 2). Table statues (furn flags 4) are drawn as tall as the footprint's longer side (INFERRED).
            client::Entity e;
            e.id           = actorIdOf(container, slot);
            e.look.kind    = kLookFile;
            e.look.valid   = true;
            e.look.ids[0]  = static_cast<uint16_t>(*file & 0xFFFF);
            e.look.ids[1]  = static_cast<uint16_t>(*file >> 16);
            e.pos          = pose->pos;
            e.dir          = static_cast<uint8_t>((rot & 3) * 64); // INFERRED: a quarter turn per step
            e.hpp          = 100;
            e.fitHeight    = (row->flags & 4) ? static_cast<float>(std::max(row->w, row->d)) * 0.6f : 0.0f;
            pc.actorId     = e.id;
            out.actors.push_back(e);
        }
        else
        {
            dat::ZoneProp p;
            p.fileId = *file;
            p.pos[0] = pose->pos.x, p.pos[1] = pose->pos.y, p.pos[2] = pose->pos.z;
            p.yaw    = pose->yaw;
            pc.prop  = out.props.size();
            out.props.push_back(p);
            out.key = out.key * 1000003u + (static_cast<uint64_t>(*file) << 16 | static_cast<uint64_t>(container) << 8 | slot) + 1;
        }
        out.pieces.push_back(pc);
    };
    // Every furnishing in the safes, in inventory order: shown where installed on this floor (or being placed), else a
    // hidden prop, so the scene's model set (its key) stays the same while placing.
    for (const auto& it : w.inventory)
    {
        if (it.itemId == 0 || (it.container != 1 && it.container != 9))
        {
            continue;
        }
        if (placing_ && it.container == placing_->container && it.slot == placing_->slot)
        {
            add(placing_->itemId, placing_->container, placing_->slot, placing_->col, placing_->level, placing_->gridRow, placing_->rot, &it,
                false);
        }
        else
        {
            const bool here = installedHere(it, w.mhFloor);
            add(it.itemId, it.container, it.slot, it.attr[6], it.attr[7], it.attr[8], it.attr[9] & 3, &it, !here);
        }
    }
    return out;
}

void MogHouseView::poseProps(gfx::ZoneScene& scene, const Furniture& f) const
{
    for (const auto i : f.hidden)
    {
        scene.setPropPose(i, glm::vec3(0.0f), 0.0f, false);
    }
    for (const auto& pc : f.pieces)
    {
        if (!pc.prop)
        {
            continue;
        }
        const auto& p = f.props[*pc.prop];
        scene.setPropPose(*pc.prop, glm::vec3(p.pos[0], p.pos[1], p.pos[2]), p.yaw, true);
    }
}

void MogHouseView::addTargets(client::WorldSnapshot& w, const gfx::ZoneScene* scene, const Furniture& f)
{
    if (!w.inMogHouse)
    {
        return;
    }
    // The exit door (CODE 0x101d2870: a client-side door entity named "Door: Back to Town" on door group `_720`).
    if (const auto door = scene ? scene->doorBoxCentre(dat::fxName("_720")) : std::nullopt)
    {
        client::Entity e;
        e.id         = kDoorId;
        e.index      = 0;
        e.name       = "Door: Back to Town";
        e.pos        = *door;
        e.look.kind  = 2;
        e.look.valid = true;
        e.doorId     = dat::fxName("_720");
        e.animation  = doorAnim_;
        e.hpp        = 100;
        w.entities.push_back(e);
    }
    // Statues and mannequins are drawn from the entity list; in layout mode every furnishing is a named target.
    for (const auto& a : f.actors)
    {
        auto e = a;
        if (layout_)
        {
            for (const auto& pc : f.pieces)
            {
                if (pc.actorId == e.id)
                {
                    const auto info = items_.find(pc.itemId);
                    e.name = info ? info->name : "Furniture";
                }
            }
        }
        w.entities.push_back(e);
    }
    if (layout_ || gardening_)
    {
        for (const auto& pc : f.pieces)
        {
            if (!pc.prop)
            {
                continue;
            }
            if (!layout_)
            {
                // Gardening: only the flowerpots (CODE 0x10207090: item type 12, installed).
                const auto info = items_.find(pc.itemId);
                if (!info || info->type != 12)
                {
                    continue;
                }
            }
            const auto&    p = f.props[*pc.prop];
            client::Entity e;
            e.id    = actorIdOf(pc.container, pc.slot);
            e.index = 0;
            const auto info = items_.find(pc.itemId);
            e.name  = info ? info->name : "Furniture";
            e.pos   = { p.pos[0], p.pos[1], p.pos[2] };
            e.hpp   = 100;
            w.entities.push_back(e); // no look: a target only
        }
    }
}

std::pair<std::string, std::vector<uint32_t>> MogHouseView::poseMotion(uint8_t race, uint8_t pose)
{
    // CODE 0x101cd0fa..0x101cd163: the pose p (exdata +0x13) picks stem 'mq' + (p & 7) from table 0x1037e028 (`mq0?` ..
    // `mq7?`) and puts p >> 3 in actor +0x8a0 (0x100ab599). The mannequin actor's state 0x31 (0x100ab543) picks motion
    // loader 5 (0x100d10c0 -> 0x100d1163), which loads file u16 0x1035d84c[race] + (p >> 3) = base + 2984 + 32 + (p >> 3)
    // and its piece 2, 0x1035d85e[race][body variant] + (p >> 3) = that + 64 / + 128 (0x100d1ea0). DATA: the 13 files
    // + 32 .. + 44 hold the `mq` motions (p >> 3 = 0 .. 12).
    static constexpr uint32_t kBase[9] = { 0, 7072, 10248, 13424, 16600, 19776, 19776, 23176, 26352 };
    std::vector<uint32_t> files;
    if (race >= 1 && race <= 8)
    {
        const uint32_t f = kBase[race] + 2984 + 32 + (pose >> 3);
        files            = { f, f + 64 };
    }
    return { fmt::format("mq{}?", pose & 7), files };
}

void MogHouseView::exitTo(uint8_t mode)
{
    // The door opens, then the exit goes (CODE 0x101d5b40 / 0x101d5d40: door+0x880 = the send, 0x100ad110 opens it).
    doorAnim_ = 8;
    exitMode_ = mode;
    exitAt_   = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
}

void MogHouseView::openDoorMenu(const client::WorldSnapshot& w, ui::GameUi& ui)
{
    // 1F with no exit quest done: a yes / no (55653 #46 in our own room, else #47; CODE 0x101d5c22..0x101d5c8b).
    if (w.mhFloor == 0 && w.mhExitBit == 0)
    {
        ui.openAppMenu(kTagExitYesNo, "menu comyn", text(s_.system, ownRoom(w) ? 46 : 47, "Exit your Mog House?"), {});
        return;
    }
    // "menu mogdoor" (CODE 0x101d5e60 draw, 0x101d60f0 select): title 55648 #69, rows #184 / #70 or #241 / #293 / #185.
    const bool floor2 = ownRoom(w) && w.mhExpansion; // 0x101d5dd0: +0x1a
    std::vector<ui::GameUi::AppRow> rows{
        { text(s_.client, 184, "Stay in your room."), "", true, 1 },
        { text(s_.client, ownRoom(w) ? 70 : 241, "Area you entered from."), "", true, 2 },
        { text(s_.client, 293, "Change floors."), "", floor2, 3 },
        { text(s_.client, 185, "Select an area to exit to."), "", !client::mog::exitAreas(w.mhExitBit).empty(), 4 },
    };
    ui.openAppMenu(kTagDoor, "menu mogdoor", "", std::move(rows), {}, text(s_.client, 69, "Which area do you want to exit to?"));
}

void MogHouseView::openAreas(const client::WorldSnapshot& w, ui::GameUi& ui)
{
    std::vector<ui::GameUi::AppRow> rows;
    const auto                      areas = client::mog::exitAreas(w.mhExitBit);
    for (std::size_t i = 0; i < areas.size(); ++i)
    {
        // Zone names (table 0x14 = 55465); Mog Garden dimmed until unlocked (0x101d5dd0 +0x19; LSB: ENABLE_MOG_GARDEN and
        // the garden key item; not known here, so it is offered and the server decides).
        rows.push_back({ text(s_.areas, areas[i], "?"), "", true, static_cast<int>(i + 1) });
    }
    ui.openAppMenu(kTagAreas, "menu mogdoor", "", std::move(rows), {}, text(s_.client, 69, "Which area do you want to exit to?"));
}

void MogHouseView::openLayoutMenu(const client::WorldSnapshot& w, ui::GameUi& ui)
{
    // CODE 0x101d5aa0: no furnishing targeted -> "menu rmlo1" (Place / Clear all / Quit); a furnishing -> "menu rmlo2"
    // (Move / Clear away / Place / Clear all); a mannequin (item type 14) -> "menu rmlo4" (Mannequin / Move / ...).
    const char* layout = "menu rmlo1";
    if (picked_ != 0)
    {
        const auto c  = static_cast<uint8_t>((picked_ - kFurnitureBase) / 100);
        const auto sl = static_cast<uint8_t>((picked_ - kFurnitureBase) % 100);
        const auto* it = item(w, c, sl);
        const auto info = it ? items_.find(it->itemId) : std::nullopt;
        layout = info && info->type == 14 ? "menu rmlo4" : "menu rmlo2";
    }
    ui.openAppMenu(kTagLayout, layout, "", {});
}

std::vector<const client::InventoryItem*> MogHouseView::flowerpots(const client::WorldSnapshot& w) const
{
    // The pot cursor's list (CODE 0x10206f10 / 0x10207090): the safes' items of type 12 that are installed.
    std::vector<const client::InventoryItem*> out;
    for (const auto& it : w.inventory)
    {
        const auto info = it.itemId ? items_.find(it.itemId) : std::nullopt;
        if (info && info->type == 12 && installedHere(it, w.mhFloor))
        {
            out.push_back(&it);
        }
    }
    return out;
}

void MogHouseView::enterGardening(const client::WorldSnapshot& w, client::Session* session)
{
    // "menu myroom" Gardening runs /garden: room state 3 (CODE 0x101d2fb6). No installed pot: 7031 #59 "I can't raise
    // plants without a flowerpot, kupo..."; else the pot cursor opens on the first pot with #61 "Choose a flowerpot,
    // kupo." (0x10206fa0).
    const auto pots = flowerpots(w);
    if (pots.empty())
    {
        if (session)
        {
            session->stdNote(59);
        }
        return;
    }
    gardening_  = true;
    layout_     = false;
    pot_        = actorIdOf(pots.front()->container, pots.front()->slot);
    wantTarget_ = pot_;
    if (session)
    {
        session->stdNote(61);
    }
    spdlog::info("mog house: gardening ({} pots)", pots.size());
}

void MogHouseView::leaveGardening(ui::GameUi& ui)
{
    // Cancel on the pot cursor (CODE 0x10207107): back to state 0, which opens "menu myroom" again (0x101d3097).
    gardening_ = false;
    pot_       = 0;
    ask_       = PlantAsk::None;
    wantTarget_ = 0;
    ui.openMogHouse();
    spdlog::info("mog house: gardening off");
}

void MogHouseView::openPlantMenu(const client::WorldSnapshot& w, ui::GameUi& ui)
{
    const auto* pot = item(w, static_cast<uint8_t>((pot_ - kFurnitureBase) / 100), static_cast<uint8_t>((pot_ - kFurnitureBase) % 100));
    if (!pot)
    {
        return;
    }
    // Confirm on the pot cursor (CODE 0x102071ab): "menu plnt<n>" by the pot's Step and Dried flag.
    plantMenu_ = client::mog::plantMenu(pot->attr[0], (pot->attr[1] & 0x80) != 0);
    if (plantMenu_ != 0)
    {
        ui.openAppMenu(kTagPlant, fmt::format("menu plnt{}", plantMenu_), "", {});
    }
}

void MogHouseView::askPlant(PlantAsk ask, uint16_t question, std::initializer_list<uint16_t> says, ui::GameUi& ui, client::Session* session)
{
    // A yes / no with the question (55653), and the moogle's lines in the log (7031; CODE 0x10206600 / 0x10206640 /
    // 0x10206680 / 0x10206d44 / 0x10206ec6).
    ask_ = ask;
    ui.openAppMenu(kTagPlantYesNo, "menu comyn", text(s_.system, question, "?"), {});
    for (const auto m : says)
    {
        if (session)
        {
            session->stdNote(m);
        }
    }
}

void MogHouseView::openRemodel(const client::WorldSnapshot& w, ui::GameUi& ui)
{
    // "menu myroom" Remodel (CODE 0x101d6440 / 0x101d64b0): "menu roomlist" with No changes (55653 #450) and the four 2F
    // styles #446..#449 = 615 San d'Orian, 616 Bastokan, 617 Windurstian, 618 Mog Patio (dimmed without key item 0xBEB,
    // the Mog Patio design document: 0x10097db0).
    const bool patio = std::binary_search(w.keyItems.begin(), w.keyItems.end(), uint16_t{ 0xBEB });
    std::vector<ui::GameUi::AppRow> rows{
        { text(s_.system, 450, "No changes"), "", true, 0 },
        { text(s_.system, 446, "San d'Orian style"), "", true, 615 },
        { text(s_.system, 447, "Bastokan style"), "", true, 616 },
        { text(s_.system, 448, "Windurstian style"), "", true, 617 },
        { text(s_.system, 449, "Mog Patio"), "", patio, 618 },
    };
    ui.openAppMenu(kTagRemodel, "menu roomlist", "", std::move(rows));
}

void MogHouseView::openMannequin(const client::WorldSnapshot& w, ui::GameUi& ui)
{
    const auto c  = static_cast<uint8_t>((picked_ - kFurnitureBase) / 100);
    const auto sl = static_cast<uint8_t>((picked_ - kFurnitureBase) % 100);
    const auto* m = item(w, c, sl);
    if (!m)
    {
        return;
    }
    // Exdata +0x0A..+0x11: the Storage slots worn on main .. feet (LSB exdata/mannequin.h).
    std::vector<ui::GameUi::AppRow> rows;
    for (uint8_t g = 0; g < 8; ++g)
    {
        const uint8_t at   = m->attr[0x0A + g];
        const auto*   worn = at ? item(w, kStorage, at) : nullptr;
        const auto    info = worn ? items_.find(worn->itemId) : std::nullopt;
        rows.push_back({ kGearName[g], info ? info->name : "-", true, g });
    }
    rows.push_back({ "Remove all", "", true, 8 });
    ui.openAppMenu(kTagMannequin, "menu inventor", text(s_.client, 0, ""), std::move(rows));
}

bool MogHouseView::interact(const client::Entity* target, const client::WorldSnapshot& w, ui::GameUi& ui)
{
    if (!w.inMogHouse)
    {
        return false;
    }
    if (target && target->id == kDoorId)
    {
        if (!exitMode_)
        {
            openDoorMenu(w, ui);
        }
        return true;
    }
    if (layout_ && !placing_)
    {
        picked_ = target && isLocal(target->id) ? target->id : 0;
        openLayoutMenu(w, ui);
        return true;
    }
    return target && isLocal(target->id);
}

void MogHouseView::onChoice(int tag, int value, const client::WorldSnapshot& w, ui::GameUi& ui, client::Session* session)
{
    spdlog::debug("mog house: menu {:#x} -> {}", tag, value);
    if (tag == ui::GameUi::kMyRoomTag)
    {
        // "menu myroom" (CODE 0x10145390 jump table 0x101454ec): 4 Gardening -> /garden, 5 Layout -> /layout, 6 Open /
        // Close Mog House -> C2S 0x0CB kind 1 / 2, 7 Remodel -> "menu roomlist".
        if (value == 4)
        {
            enterGardening(w, session);
        }
        else if (value == 5)
        {
            layout_    = true;
            gardening_ = false;
            picked_    = 0;
            lastMoved_.reset(); // the room's +0x7e6 is cleared on entering layout mode (0x101d2ffe)
            spdlog::info("mog house: layout mode");
            openLayoutMenu(w, ui);
        }
        else if (value == 6 && session)
        {
            session->send(0x0CB, client::mog::myroomIsPacket(w.mhOpen ? 2 : 1, 0, 0));
            spdlog::info("mog house: {} to visitors", w.mhOpen ? "close" : "open");
        }
        else if (value == 7)
        {
            openRemodel(w, ui);
        }
        return;
    }
    switch (tag)
    {
        case kTagExitYesNo:
            if (value == 1)
            {
                exitTo(0);
            }
            break;
        case kTagDoor:
            if (value == 4)
            {
                openAreas(w, ui);
            }
            else if (const auto mode = client::mog::doorTopMode(value, w.mhFloor))
            {
                exitTo(*mode);
            }
            break;
        case kTagAreas:
            if (value < 0)
            {
                openDoorMenu(w, ui); // back to the top list (CODE 0x101d621e)
            }
            else if (const auto mode = client::mog::doorAreaMode(w.mhExitBit, value))
            {
                exitTo(*mode);
            }
            break;
        case kTagLayout:
        {
            const bool furnished = picked_ != 0;
            const auto c  = static_cast<uint8_t>((picked_ - kFurnitureBase) / 100);
            const auto sl = static_cast<uint8_t>((picked_ - kFurnitureBase) % 100);
            const auto* it = furnished ? item(w, c, sl) : nullptr;
            const bool mannequin = it && items_.find(it->itemId) && items_.find(it->itemId)->type == 14;
            // Element ids (CODE 0x101f4300 rmlo1, 0x101f43ae rmlo2, 0x101f449e rmlo4).
            enum
            {
                kNone,
                kPlace,
                kMove,
                kClear,
                kClearAll,
                kQuit,
                kDress
            } act = kNone;
            if (!furnished) act = value == 1 ? kPlace : value == 2 ? kClearAll : value == 3 ? kQuit : kNone;
            else if (!mannequin) act = value == 1 ? kMove : value == 2 ? kClear : value == 3 ? kPlace : value == 4 ? kClearAll : kNone;
            else act = value == 1 ? kDress : value == 2 ? kMove : value == 3 ? kClear : value == 4 ? kPlace : value == 5 ? kClearAll : kNone;
            // Escape only closes the menu: layout mode stays until Quit (INFERRED).
            switch (act)
            {
                case kPlace:
                    // "menu bankmenu": Mog Safe / Mog Safe 2 (2 dimmed without the 2F; CODE 0x101d6880).
                    ui.openAppMenu(kTagSafe, "menu bankmenu", "", {}, w.mhExpansion ? std::vector<int>{} : std::vector<int>{ 2 });
                    break;
                case kMove:
                    if (it)
                    {
                        if (const auto* row = furnRow(it->itemId))
                        {
                            placing_ = Placing{ it->itemId, c, sl, *row, it->attr[6], it->attr[7], it->attr[8], static_cast<uint8_t>(it->attr[9] & 3), false };
                            if (!(row->flags & 1))
                            {
                                lastMoved_ = std::pair{ it->attr[6], it->attr[8] }; // Move sets the room's +0x7e6 (0x101d353d)
                            }
                        }
                    }
                    break;
                case kClear:
                    if (it && session)
                    {
                        session->send(0x0FB, client::mog::bankInPacket(it->itemId, sl, c));
                        picked_ = 0;
                    }
                    break;
                case kClearAll:
                {
                    // Every furnishing goes back to the safe after a yes / no: 55651 #615 (CODE 0x101d3870: table 0xE entry
                    // 0x267 -> the yes / no 0x102109e0, answered by 0x101d3780).
                    ui.openAppMenu(kTagClearAll, "menu comyn", text(s_.help, 615, "Clear away all furniture?"), {});
                    break;
                }
                case kDress:
                    openMannequin(w, ui);
                    break;
                case kQuit:
                    // Leaving layout mode: C2S 0x0FA with ItemNo 0 (LSB: the server recomputes the moghancement).
                    layout_  = false;
                    placing_.reset();
                    picked_  = 0;
                    if (session)
                    {
                        session->send(0x0FA, client::mog::layoutPacket(0, 0, 0, w.mhFloor, 0, 0, 0, 0));
                    }
                    ui.openMogHouse(); // state 0 opens "menu myroom" again (CODE 0x101d3097)
                    spdlog::info("mog house: layout mode off");
                    break;
                case kNone:
                    break;
            }
            break;
        }
        case kTagClearAll:
            if (value == 1 && session)
            {
                for (const auto& it : w.inventory)
                {
                    if (installedHere(it, w.mhFloor))
                    {
                        session->send(0x0FB, client::mog::bankInPacket(it.itemId, it.slot, it.container));
                    }
                }
            }
            break;
        case kTagSafe:
            if (value == 1 || value == 2)
            {
                const uint8_t                   c = value == 1 ? 1 : 9;
                std::vector<ui::GameUi::AppRow> rows;
                for (const auto& it : w.inventory)
                {
                    if (it.container != c || it.itemId == 0 || (it.attr[1] & 0x40))
                    {
                        continue;
                    }
                    const auto info = items_.find(it.itemId);
                    if (!info || (info->type != 10 && info->type != 12 && info->type != 14) || !furnRow(it.itemId))
                    {
                        continue;
                    }
                    rows.push_back({ info->name, "", !(w.mhFloor && info->type == 12), c * 100 + it.slot }); // no flowerpots on 2F
                }
                if (rows.empty())
                {
                    rows.push_back({ "No furnishings to place.", "", false, -2 });
                }
                ui.openAppMenu(kTagPick, "menu inventor", "Place a piece of furniture in your Mog House.", std::move(rows));
            }
            break;
        case kTagPick:
            if (value >= 100)
            {
                const auto  c  = static_cast<uint8_t>(value / 100);
                const auto  sl = static_cast<uint8_t>(value % 100);
                const auto* it = item(w, c, sl);
                const auto* row = it ? furnRow(it->itemId) : nullptr;
                if (row)
                {
                    // Where a piece from the safe starts (CODE 0x101cd354..0x101cd44f, client::mog::newPieceStart): wall
                    // pieces in slot 0 facing its way; floor pieces centred on col 9 row 12, or on the piece last moved.
                    const auto [col, gridRow] = client::mog::newPieceStart(*row, w.mhFloor, lastMoved_);
                    Placing pl{ it->itemId, c, sl, *row, col, 0, gridRow, 0, true };
                    if (row->flags & 1)
                    {
                        pl.rot = client::mog::wallSlotRotation(w.mhFloor, 0);
                    }
                    placing_ = pl;
                    spdlog::info("mog house: placing item {} from {}:{}", it->itemId, c, sl);
                }
            }
            break;
        case kTagMannequin:
            if (value == 8 && session)
            {
                const auto c  = static_cast<uint8_t>((picked_ - kFurnitureBase) / 100);
                const auto sl = static_cast<uint8_t>((picked_ - kFurnitureBase) % 100);
                session->send(0x03B, client::mog::subcontainerPacket(5, c, sl, 0, kStorage, 0));
            }
            else if (value >= 0 && value < 8)
            {
                gearSlot_ = static_cast<uint8_t>(value);
                std::vector<ui::GameUi::AppRow> rows{ { "(Remove)", "", true, 0 } };
                for (const auto& it : w.inventory)
                {
                    if (it.container != kStorage || it.itemId == 0)
                    {
                        continue;
                    }
                    const auto info = items_.find(it.itemId);
                    if (info && (info->slots >> kGearBit[gearSlot_]) & 1)
                    {
                        rows.push_back({ info->name, "", true, it.slot });
                    }
                }
                ui.openAppMenu(kTagMannequinPick, "menu inventor", fmt::format("{}: put on from the Storage.", kGearName[gearSlot_]), std::move(rows));
            }
            break;
        case kTagMannequinPick:
            if (value >= 0 && session)
            {
                const auto  c  = static_cast<uint8_t>((picked_ - kFurnitureBase) / 100);
                const auto  sl = static_cast<uint8_t>((picked_ - kFurnitureBase) % 100);
                const auto* m  = item(w, c, sl);
                const uint8_t worn = m ? m->attr[0x0A + gearSlot_] : 0;
                if (value == 0)
                {
                    if (worn)
                    {
                        session->send(0x03B, client::mog::subcontainerPacket(2, c, sl, gearSlot_, kStorage, worn));
                    }
                }
                else
                {
                    session->send(0x03B, client::mog::subcontainerPacket(1, c, sl, gearSlot_, kStorage, static_cast<uint8_t>(value)));
                }
            }
            break;
        case kTagPlant:
        {
            const auto  pc  = static_cast<uint8_t>((pot_ - kFurnitureBase) / 100);
            const auto  ps  = static_cast<uint8_t>((pot_ - kFurnitureBase) % 100);
            const auto* pot = item(w, pc, ps);
            if (!pot)
            {
                break;
            }
            // The menus' elements (CODE 0x10206800 plnt1, 0x102068d0 plnt2, 0x10206980 plnt3, 0x10206aa0 plnt4,
            // 0x10206b70 plnt5, 0x10206bf0 plnt6); the last element of each is Return.
            enum Act
            {
                kNone,
                kExamine,
                kSow,
                kFeed,
                kHarvest,
                kDry,
                kThrow
            };
            static constexpr Act kActs[7][5] = {
                {},
                { kExamine, kSow, kNone },
                { kExamine, kDry, kThrow, kNone },
                { kExamine, kFeed, kDry, kThrow, kNone },
                { kExamine, kHarvest, kDry, kThrow, kNone },
                { kExamine, kHarvest, kNone },
                { kExamine, kThrow, kNone },
            };
            const Act act = plantMenu_ >= 1 && plantMenu_ <= 6 && value >= 1 && value <= 5 ? kActs[plantMenu_][value - 1] : kNone;
            switch (act)
            {
                case kExamine:
                    // C2S 0x0FD, then the moogle's remark by the pot's state (CODE 0x102063e0).
                    if (session)
                    {
                        session->send(0x0FD, client::mog::plantPotPacket(pot->itemId, ps, pc));
                        session->stdNote(client::mog::plantExamineMessage(pot->attr[0], (pot->attr[1] & 0x80) != 0, static_cast<int>(coin_++)));
                    }
                    break;
                case kSow:
                case kFeed:
                    // "menu bankmenu" (Mog Safe / Mog Safe 2), then that safe's seeds (item type 11, CODE 0x10206ce0) or
                    // crystals (0x1000..0x1007, 0x10206e00).
                    feeding_ = act == kFeed;
                    ui.openAppMenu(kTagPlantSafe, "menu bankmenu", "", {}, w.mhExpansion ? std::vector<int>{} : std::vector<int>{ 2 });
                    break;
                case kHarvest: askPlant(PlantAsk::Harvest, 51, { 78, 85 }, ui, session); break;
                case kDry: askPlant(PlantAsk::Dry, 49, { 77, 86 }, ui, session); break;
                case kThrow: askPlant(PlantAsk::ThrowAway, 52, { 79, 87 }, ui, session); break;
                case kNone: break;
            }
            break;
        }
        case kTagPlantSafe:
            if (value == 1 || value == 2)
            {
                plantSafe_ = value == 1 ? 1 : 9;
                std::vector<ui::GameUi::AppRow> rows;
                for (const auto& it : w.inventory)
                {
                    const auto info = it.container == plantSafe_ && it.itemId ? items_.find(it.itemId) : std::nullopt;
                    const bool fits = info && (feeding_ ? it.itemId >= 0x1000 && it.itemId <= 0x1007 : info->type == 11);
                    if (fits)
                    {
                        rows.push_back({ info->name, it.count > 1 ? std::to_string(it.count) : "", true, it.slot });
                    }
                }
                if (rows.empty())
                {
                    rows.push_back({ feeding_ ? "No crystals." : "No seeds.", "", false, -2 });
                }
                ui.openAppMenu(kTagPlantPick, "menu inventor", "", std::move(rows));
                if (session)
                {
                    session->stdNote(feeding_ ? 76 : 75); // "Choose a crystal to use as fertilizer, kupo." / "... seeds ..."
                }
            }
            break;
        case kTagPlantPick:
            if (value >= 0)
            {
                plantSlot_ = static_cast<uint8_t>(value);
                if (feeding_)
                {
                    askPlant(PlantAsk::Feed, 50, { 84 }, ui, session);
                }
                else
                {
                    askPlant(PlantAsk::Sow, 48, { 83 }, ui, session);
                }
            }
            break;
        case kTagPlantYesNo:
        {
            const auto  pc  = static_cast<uint8_t>((pot_ - kFurnitureBase) / 100);
            const auto  ps  = static_cast<uint8_t>((pot_ - kFurnitureBase) % 100);
            const auto* pot = item(w, pc, ps);
            const auto  ask = std::exchange(ask_, PlantAsk::None);
            if (value != 1 || !pot || !session)
            {
                break;
            }
            switch (ask)
            {
                case PlantAsk::Sow:
                case PlantAsk::Feed:
                    if (const auto* add = item(w, plantSafe_, plantSlot_))
                    {
                        session->send(0x0FC, client::mog::plantAddPacket(pot->itemId, ps, pc, add->itemId, plantSlot_, plantSafe_));
                    }
                    break;
                case PlantAsk::Harvest: session->send(0x0FE, client::mog::plantCropPacket(pot->itemId, ps, pc, false)); break;
                case PlantAsk::ThrowAway: session->send(0x0FE, client::mog::plantCropPacket(pot->itemId, ps, pc, true)); break;
                case PlantAsk::Dry: session->send(0x0FF, client::mog::plantPotPacket(pot->itemId, ps, pc)); break;
                case PlantAsk::None: break;
            }
            break;
        }
        case kTagRemodel:
            // "No changes" closes the list (CODE 0x101d6702); a style asks 55653 #451 "Make changes?" (0x101d672d).
            if (value >= 615 && value <= 618)
            {
                remodelStyle_ = static_cast<uint16_t>(value);
                ui.openAppMenu(kTagRemodelYesNo, "menu comyn", text(s_.system, 451, "Make changes?"), {});
            }
            break;
        case kTagRemodelYesNo:
            // Yes: C2S 0x0CB kind 5 with the style (CODE 0x101d6680; LSB answers "remodeled" and, on 2F, re-zones).
            if (value == 1 && session && remodelStyle_)
            {
                session->send(0x0CB, client::mog::myroomIsPacket(5, 0, remodelStyle_));
                spdlog::info("mog house: remodel to style {}", remodelStyle_);
            }
            remodelStyle_ = 0;
            break;
        default:
            break;
    }
}

bool MogHouseView::placingKey(ui::KeyAction a, const client::WorldSnapshot& w, float camYaw, client::Session* session, ui::GameUi& ui)
{
    using KA = ui::KeyAction;
    if (gardening_ && !placing_)
    {
        // The pot cursor (object 0x10631900 under action context 0x42, the placing keys' context): the arrows step
        // through the installed pots (INFERRED: up / right forward, down / left back), Confirm opens the pot's menu,
        // Cancel leaves gardening.
        const auto pots = flowerpots(w);
        switch (a)
        {
            case KA::CursorUp:
            case KA::CursorDown:
            case KA::CursorLeft:
            case KA::CursorRight:
            {
                if (pots.empty())
                {
                    return true;
                }
                std::size_t at = 0;
                for (std::size_t i = 0; i < pots.size(); ++i)
                {
                    at = actorIdOf(pots[i]->container, pots[i]->slot) == pot_ ? i : at;
                }
                const bool fwd = a == KA::CursorUp || a == KA::CursorRight;
                at             = (at + (fwd ? 1 : pots.size() - 1)) % pots.size();
                pot_           = actorIdOf(pots[at]->container, pots[at]->slot);
                wantTarget_    = pot_;
                return true;
            }
            case KA::Confirm: openPlantMenu(w, ui); return true;
            case KA::Cancel: leaveGardening(ui); return true;
            default: return false;
        }
    }
    if (!placing_)
    {
        return false;
    }
    auto& p  = *placing_;
    const bool wall = (p.row.flags & 1) != 0;
    switch (a)
    {
        case KA::CursorUp:
        case KA::CursorDown:
        case KA::CursorLeft:
        case KA::CursorRight:
        {
            // The game's placing keys (CODE 0x101cdf10, context 0x42): the arrows are directions 0 up / 1 right / 2 down /
            // 3 left, turned round (table 0x10337e04: 2 3 0 1) when the camera looks toward -z (camera +0x4c >= +0x58,
            // CODE 0x101ccc10; INFERRED: those are the eye and look-at z) unless config 193 is 1 (not traced).
            int dir = a == KA::CursorUp ? 0 : a == KA::CursorRight ? 1 : a == KA::CursorDown ? 2 : 3;
            if (!(std::sin(camYaw) > 0.0f))
            {
                dir = (dir + 2) & 3;
            }
            if (wall)
            {
                // Wall pieces go to the neighbouring slot in that direction (the game's table); each slot has its facing.
                p.col = client::mog::wallSlotStep(w.mhFloor, p.col, dir);
                p.rot = client::mog::wallSlotRotation(w.mhFloor, p.col);
                return true;
            }
            // Floor pieces: up = row + 1, right = col + 1 (CODE 0x101cccb9 -> 0x101ccd00, clamped by 0x101cca70).
            // The piece moves anywhere inside the room's grid (0x101cca70 only clamps); whether it may stand there is
            // checked when it is placed.
            const int nc = p.col + client::mog::kPlaceStep[dir][0], nr = p.gridRow + client::mog::kPlaceStep[dir][1];
            if (client::mog::footprintInRoom(p.row, nc, nr, p.rot, w.mhFloor))
            {
                p.col     = static_cast<uint8_t>(nc);
                p.gridRow = static_cast<uint8_t>(nr);
            }
            spdlog::debug("mog house: placing at col {} row {} rot {} ({})", p.col, p.gridRow, p.rot,
                          client::mog::footprintFits(p.row, p.col, p.gridRow, p.rot, w.mhFloor) ? "fits" : "blocked");
            return true;
        }
        case KA::MainMenu:
            // Turning: action 0x8d (keypad minus, the main-menu key) turns a quarter back; action 0x8e turns forward and
            // has no keyboard key by default (gamepad only) (CODE 0x101ce0c5..0x101ce118).
            if (!wall)
            {
                const uint8_t r = static_cast<uint8_t>((p.rot + 3) & 3);
                if (client::mog::footprintInRoom(p.row, p.col, p.gridRow, r, w.mhFloor))
                {
                    p.rot = r;
                }
            }
            return true;
        case KA::Confirm:
            if (!wall && !client::mog::footprintFits(p.row, p.col, p.gridRow, p.rot, w.mhFloor))
            {
                spdlog::info("mog house: cannot place at col {} row {} (blocked)", p.col, p.gridRow);
                return true;
            }
            if (session)
            {
                session->send(0x0FA, client::mog::layoutPacket(p.itemId, p.slot, p.container, w.mhFloor, p.col, wall ? 0 : p.level,
                                                               wall ? 0 : p.gridRow, p.rot));
                const auto at = client::mog::furniturePose(p.row, p.col, p.level, p.gridRow, p.rot, w.mhFloor, w.mhMap);
                spdlog::info("mog house: place item {} at col {} row {} level {} rot {} (game {:.1f}, {:.1f})", p.itemId, p.col, p.gridRow, p.level,
                             p.rot, at ? at->pos.x : 0.0f, at ? at->pos.z : 0.0f);
                if (!p.fresh && !wall)
                {
                    lastMoved_ = std::pair{ p.col, p.gridRow }; // the moved piece's object now stands here
                }
                placing_.reset();
            }
            return true;
        case KA::Cancel:
            placing_.reset();
            return true;
        default:
            return true; // nothing else while placing (the character stays put)
    }
}

void MogHouseView::update(client::WorldSnapshot& w, client::Session* session)
{
    if (w.zone != zoneSeen_ || w.zoneModel != modelSeen_ || !w.inMogHouse)
    {
        zoneSeen_  = w.zone;
        modelSeen_ = w.zoneModel;
        layout_    = false;
        gardening_ = false;
        pot_       = 0;
        placing_.reset();
        picked_    = 0;
        doorAnim_  = 9;
        exitMode_.reset();
    }
    if (exitMode_ && std::chrono::steady_clock::now() >= exitAt_)
    {
        if (session)
        {
            // C2S 0x05E `zmrq` with MyRoomExitBit / Mode (CODE 0x1008adf0; GetMyRoomExitBit 0x10097af0: on 1F the city's
            // bit only for its own room, 0 otherwise; on 2F the server's value).
            const uint8_t bit = w.mhExitBit;
            spdlog::info("mog house: leave, mode {}", *exitMode_);
            session->enterZoneLine(dat::fxName("zmrq"), bit, *exitMode_);
        }
        exitMode_.reset();
    }
}

} // namespace xi::app
