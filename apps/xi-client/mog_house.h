// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "client/mog_house.h"
#include "client/session.h"
#include "dat/dat_index.h"
#include "dat/items.h"
#include "dat/messages.h"
#include "dat/zone.h"
#include "ui/game_ui.h"
#include "ui/keymap.h"

#include <glm/glm.hpp>

#include <chrono>
#include <initializer_list>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xi::gfx
{
class ZoneScene;
}

// The Mog House in xi-client (docs/protocol-map.md §6.1, ui-format-notes §7.6): installed furniture as scene props and
// actors (statues, mannequins), the client-side exit door "Door: Back to Town" with its "menu mogdoor", and layout mode
// (placing, moving and clearing furniture; dressing mannequins).
namespace xi::app
{

class MogHouseView
{
public:
    // Client-made actors: the door, and furniture by container * 100 + slot (statues and mannequins are drawn; in
    // layout mode every furnishing is a target).
    static constexpr uint32_t kDoorId        = 0x7F7FFF00u;
    static constexpr uint32_t kFurnitureBase = 0x7F800000u;
    static bool isLocal(uint32_t id)
    {
        return id == kDoorId || (id & 0xFF800000u) == kFurnitureBase;
    }

    struct Strings
    {
        const dat::StringTable* client = nullptr; // 55648: the door menu's texts
        const dat::StringTable* system = nullptr; // 55653: the exit yes / no prompts (#46 / #47)
        const dat::StringTable* areas  = nullptr; // 55465: zone names
        const dat::StringTable* help   = nullptr; // 55651: menu help texts (#615 the clear-all question)
    };
    MogHouseView(const dat::DatIndex& index, const dat::ItemTable& items, Strings s);

    // One installed furnishing on this floor.
    struct Piece
    {
        uint16_t          itemId = 0;
        uint8_t           container = 0, slot = 0;
        dat::FurnitureRow row;
        uint8_t           col = 0, level = 0, gridRow = 0, rot = 0;
        std::optional<std::size_t> prop;  // a static model: its scene prop index
        uint32_t          actorId = 0;    // a skinned one (statue, mannequin)
    };
    struct Furniture
    {
        std::vector<dat::ZoneProp>  props;  // static models (built into the scene)
        uint64_t                    key = 0; // which models: positions are posed every frame instead
        std::vector<client::Entity> actors; // statues and mannequins, drawn as characters
        std::vector<Piece>          pieces;
        std::vector<std::size_t>    hidden; // props of furnishings not shown now (posed hidden)
        std::vector<dat::ZoneLine>  boxes;  // collision: the game's furniture boxes (client::mog::furnitureBox; game space)
        std::vector<dat::ZoneLine>  cameraBoxes; // the camera's: full height (client::mog::furnitureCameraBox)
        std::unordered_map<uint32_t, std::pair<uint8_t, uint8_t>> mannequinPoses; // actor id -> race, pose
    };
    // This frame's furniture: the installed furnishings of the floor plus the one being placed, with the placing
    // preview's position.
    [[nodiscard]] Furniture furniture(const client::WorldSnapshot& w) const;
    // Poses every furniture prop (props 0 .. n-1 of the scene), hiding none.
    void poseProps(gfx::ZoneScene& scene, const Furniture& f) const;
    // Adds the door (and, in layout mode, the furnishings) to the entity list as targets.
    void addTargets(client::WorldSnapshot& w, const gfx::ZoneScene* scene, const Furniture& f);
    // A mannequin's pose: the motion stem ("mq0?" ..) and the files to look in.
    static std::pair<std::string, std::vector<uint32_t>> poseMotion(uint8_t race, uint8_t pose);

    // Confirm on a client-made target (or on nothing while in layout mode). Returns true when it was ours.
    bool interact(const client::Entity* target, const client::WorldSnapshot& w, ui::GameUi& ui);
    // The menus' answers (GameUi::takeAppChoice).
    void onChoice(int tag, int value, const client::WorldSnapshot& w, ui::GameUi& ui, client::Session* session);
    // Keys while a furnishing is being placed: arrows move it a cell (in the camera's frame), zoom keys turn it, Confirm
    // places it, Cancel puts it back. In gardening mode the arrows step between the flowerpots, Confirm opens the pot's
    // menu and Cancel leaves gardening. Returns true when taken. `camYaw` is the camera's heading (radians, game space).
    bool placingKey(ui::KeyAction a, const client::WorldSnapshot& w, float camYaw, client::Session* session, ui::GameUi& ui);
    // Whether placingKey wants the keys now (placing a piece, or choosing a flowerpot).
    [[nodiscard]] bool takesKeys() const
    {
        return placing_.has_value() || gardening_;
    }
    // A target the view asks for (the flowerpot under the gardening cursor), once.
    std::optional<uint32_t> takeTarget()
    {
        auto t = wantTarget_;
        wantTarget_.reset();
        return t;
    }
    // Per frame: the door's open animation, the delayed exit after it, and leaving when the zone changes.
    void update(client::WorldSnapshot& w, client::Session* session);

    [[nodiscard]] bool layoutMode() const
    {
        return layout_;
    }
    [[nodiscard]] bool placing() const
    {
        return placing_.has_value();
    }
    [[nodiscard]] bool gardening() const
    {
        return gardening_;
    }
    // Test hook (--mog-layout): enter layout mode.
    void enterLayout()
    {
        layout_ = true;
    }

private:
    enum Tag : int
    {
        kTagDoor = 0x4D44,  // menu mogdoor (top)
        kTagAreas,          // menu mogdoor (area list)
        kTagExitYesNo,      // the 1F yes / no
        kTagLayout,         // rmlo1 / rmlo2 / rmlo4
        kTagSafe,           // menu bankmenu
        kTagPick,           // the furnishings to place
        kTagMannequin,      // the mannequin's gear slots
        kTagMannequinPick,  // what to put on a slot
        kTagClearAll,       // yes / no
        kTagPlant,          // "menu plnt1".."plnt6" (the pot's gardening menu)
        kTagPlantSafe,      // "menu bankmenu" for the seeds / crystal
        kTagPlantPick,      // the seeds / crystals in that safe
        kTagPlantYesNo,     // the gardening questions (55653 #48..#52)
        kTagRemodel,        // "menu roomlist": the 2F styles
        kTagRemodelYesNo,   // "Make changes?"
    };
    // A gardening question waiting for its yes / no.
    enum class PlantAsk
    {
        None,
        Sow,
        Feed,
        Harvest,
        Dry,
        ThrowAway
    };
    struct Placing
    {
        uint16_t          itemId = 0;
        uint8_t           container = 0, slot = 0;
        dat::FurnitureRow row;
        uint8_t           col = 0, level = 0, gridRow = 0, rot = 0;
        bool              fresh = false; // not installed yet
    };
    const client::InventoryItem* item(const client::WorldSnapshot& w, uint8_t container, uint8_t slot) const;
    const dat::FurnitureRow*     furnRow(uint16_t itemId) const;
    void openDoorMenu(const client::WorldSnapshot& w, ui::GameUi& ui);
    void openAreas(const client::WorldSnapshot& w, ui::GameUi& ui);
    void openLayoutMenu(const client::WorldSnapshot& w, ui::GameUi& ui);
    void openMannequin(const client::WorldSnapshot& w, ui::GameUi& ui);
    void exitTo(uint8_t mode);
    // Gardening (CODE 0x101d2fb6 state 3, the pot cursor 0x10631900, menus 0x10206800..0x10206c70).
    std::vector<const client::InventoryItem*> flowerpots(const client::WorldSnapshot& w) const;
    void enterGardening(const client::WorldSnapshot& w, client::Session* session);
    void leaveGardening(ui::GameUi& ui);
    void openPlantMenu(const client::WorldSnapshot& w, ui::GameUi& ui);
    void askPlant(PlantAsk ask, uint16_t question, std::initializer_list<uint16_t> says, ui::GameUi& ui, client::Session* session);
    void openRemodel(const client::WorldSnapshot& w, ui::GameUi& ui);
    std::string text(const dat::StringTable* t, uint32_t id, const char* fallback) const;

    const dat::DatIndex&           index_;
    const dat::ItemTable&          items_;
    Strings                        s_;
    std::vector<dat::FurnitureRow> furn_;
    bool                           layout_ = false;
    std::optional<Placing>         placing_;
    uint32_t                       picked_ = 0;     // the furnishing a layout menu is about (actor id)
    std::optional<std::pair<uint8_t, uint8_t>> lastMoved_; // the floor piece last moved in this layout session (col, row)
    bool                           gardening_ = false;
    uint32_t                       pot_ = 0;        // the flowerpot under the gardening cursor (actor id)
    PlantAsk                       ask_ = PlantAsk::None;
    int                            plantMenu_ = 0;  // which "menu plnt<n>" is open
    uint8_t                        plantSafe_ = 0;  // where the seeds / crystal come from (1 or 9)
    uint8_t                        plantSlot_ = 0;
    bool                           feeding_ = false; // the safe list is for a crystal, not seeds
    uint16_t                       remodelStyle_ = 0;
    std::optional<uint32_t>        wantTarget_;
    uint32_t                       coin_ = 0;       // the examine message's coin toss (CODE 0x10314520 & 1)
    uint8_t                        gearSlot_ = 0;   // the mannequin slot being dressed
    uint8_t                        doorAnim_ = 9;   // 9 closed, 8 opening (Doors)
    std::optional<uint8_t>         exitMode_;       // sent once the door has opened
    std::chrono::steady_clock::time_point exitAt_{};
    uint16_t                       zoneSeen_ = 0;
    uint16_t                       modelSeen_ = 0;
};

} // namespace xi::app
