// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/game_ui.h"

#include "client/battle_colours.h"
#include "ui/button_label.h"
#include "ui/compass.h"
#include "ui/currencies.h"
#include "ui/equip_rules.h"
#include "ui/fishing_gauge.h"
#include "ui/full_log.h"
#include "ui/item_count.h"
#include "ui/keymap.h"
#include "ui/list_cursor.h"
#include "ui/status_pulse.h"
#include "ui/map_markers.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <SDL3/SDL_keyboard.h>
#include <cstring>

namespace xi::ui
{

namespace
{

const char* jobAbbrev(uint8_t j)
{
    static const char* names[] = { "---", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
                                   "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN" };
    return j < std::size(names) ? names[j] : "?";
}

constexpr UiColor kWhite{ 0x80, 0x80, 0x80, 0x80 };
constexpr UiColor kGrey{ 0x50, 0x50, 0x50, 0x80 };
constexpr UiColor kGold{ 0x80, 0x78, 0x50, 0x80 };
// Item and equipment lists (CODE 0x10130470, 0x101f0290): worn 0x80509050, usable 0x80909040, not wearable 0x80404040.
constexpr UiColor kWornGreen{ 0x50, 0x90, 0x50, 0x80 };
constexpr UiColor kUsableYellow{ 0x90, 0x90, 0x40, 0x80 };
constexpr UiColor kDimGrey{ 0x40, 0x40, 0x40, 0x80 };

// Containers the equipment list and /equip look in (CODE 0x10130b30: the row's bag icon 0 inventory, 1..8 the Mog
// Wardrobes 8, 10..16), in that order.
constexpr uint8_t kEquipBags[] = { 0, 8, 10, 11, 12, 13, 14, 15, 16 };

// "menu storage" (Items -> Switch): element n's container (LSB ids): Mog Safe, Mog Safe 2, Storage, Mog Locker, Mog
// Satchel, Mog Sack, Mog Case, Mog Wardrobe 1..8.
constexpr uint8_t kBagOfElement[16] = { 0, 1, 9, 2, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16 };

// Whether a container exists for us (S2C 0x01C: 1 + its size, 0 = none).
bool bagExists(const client::WorldSnapshot& w, uint8_t c)
{
    return c < w.containerSize.size() && w.containerSize[c] > 1;
}

// Whether Switch can open container c here (INFERRED from LSB's 0x029 rules, docs/ui-format-notes.md §10.12): the
// Mog Safes and Storage in the Mog House, the rest anywhere once they exist.
bool bagReachable(const client::WorldSnapshot& w, uint8_t c)
{
    if (!bagExists(w, c))
    {
        return false;
    }
    return (c != 1 && c != 9 && c != 2) || w.inMogHouse;
}

xi::ui::EquipContext equipContext(const client::WorldSnapshot& w)
{
    xi::ui::EquipContext ctx;
    ctx.mainJob       = w.mainJob;
    ctx.mainLevel     = w.mainLevel;
    ctx.race          = static_cast<uint8_t>(w.look.ids[0] >> 8);
    ctx.superiorLevel = w.stats.suLevel;
    ctx.blockedSlots  = static_cast<uint16_t>(w.encumbrance & 0xFFFF);
    return ctx;
}

bool isWorn(const client::WorldSnapshot& w, uint8_t container, uint8_t slot)
{
    return std::ranges::any_of(w.equipment, [&](const auto& e) { return e.second != 0 && e.first == container && e.second == slot; });
}

std::string countdown(int seconds)
{
    return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

// "menu equip" slot boxes: placeholder sprite 40 + n of "menu framesus" is grid cell n (rows of four: Main Sub Range
// Ammo / Head Neck Ear Ear / Body Hands Ring Ring / Back Waist Legs Feet); this maps the cell to the equip slot.
constexpr uint8_t kEquipCellSlot[16] = { 0, 1, 2, 3, 4, 9, 11, 12, 5, 6, 13, 14, 15, 10, 7, 8 };

// Main menu entries we implement, by the label sprite each element shows ("menu windowps").
constexpr int kLabelStatus = 13, kLabelEquipment = 15, kLabelMagic = 17, kLabelItems = 19, kLabelParty = 20, kLabelAbilities = 21,
              kLabelMap = 23, kLabelTrade = 28, kLabelSynthesis = 680, kLabelSearch = 18, kLabelRegion = 25;
constexpr int kHelpSynthesize = 819, kHelpSynthHistory = 820;
// No valid target for the sub-target cursor (message table 0x103763b0 entries 0x16 / 0x1b): 55646 #90 / #93.
constexpr const char* kNoSpellTarget   = "You must specify a valid target for that spell.";
constexpr const char* kNoAbilityTarget = "You must specify a valid target for that ability.";
constexpr int kLabelLinkshell = 16, kHelpLinkActivate = 290;
// Linkshell items (LSB map/items.h): LINKSHELL 513, PEARLSACK 514, LINKPEARL 515.
bool isLinkshellItem(uint16_t id)
{
    return id >= 513 && id <= 515;
}
// Crystals the server accepts for synthesis (LSB 0x096 validCrystals, item ids in map/items.h).
bool isSynthCrystal(uint16_t id)
{
    return (id >= 4096 && id <= 4103) || id == 4111 || (id >= 4238 && id <= 4245) || (id >= 6506 && id <= 6513);
}
// Party menu entries, by their help text id (file 55651).
constexpr int kHelpLeaveParty = 390, kHelpAcceptInvite = 405, kHelpDeclineInvite = 406, kHelpAddMember = 391, kHelpDisband = 395,
              kHelpChangeLeader = 393, kHelpRemoveMember = 394;
// Main menu page 2 entries, by help id.
constexpr int kHelpMacros = 473; // main menu page 2 "Macros" -> "menu mcrmenu"
constexpr int kHelpCommunication = 926, kHelpCurrentTime = 477, kHelpLogOut = 478, kHelpShutDown = 479, kHelpKeyItems = 470, kHelpMissions = 468, kHelpQuests = 469;
// Quest areas (LSB QuestLog 0..7) and their "current" / "completed" window titles (file 55652).
constexpr int kQuestTitles[8][2] = { { 25, 31 }, { 26, 32 }, { 27, 33 }, { 28, 34 }, { 29, 35 }, { 30, 36 }, { 136, 137 }, { 145, 146 } };
// Config (help ids, file 55651): main menu page 2 row; Config rows Windows / Misc. 2; Window Type 1..8 buttons;
// Keyboard Size Full / Compact 1.
constexpr int kCfgAnon = -1; // pseudo config id: character data public / hidden (C2S 0x0DC flag 0, not a stored value)
constexpr int kHelpConfig = 474, kHelpConfigWindows = 229, kHelpConfigMisc2 = 233, kHelpWindowType1 = 198, kHelpKeyboardFull = 145,
              kHelpKeyboardCompact = 146, kHelpKeyboardCompact2 = 686;
// Element id pairs (page 1 "menu menuwind", page 2 "menu socialme") on the same row (DLL table 0x1036ffa8).
constexpr std::pair<int, int> kMainPageRows[] = { { 1, 1 }, { 2, 2 }, { 3, 3 }, { 4, 4 }, { 5, 6 }, { 6, 7 },
                                                  { 7, 8 }, { 8, 10 }, { 9, 9 }, { 10, 5 }, { 11, 11 }, { 12, 12 } };

// The battle / player command menu ("menu playermo" object, CODE 0x1012b200): per mode, candidate entry codes (table
// 0x1036f3a0, 13 a mode, 28 ends), each kept, dropped or replaced by a filter (0x1012ae60); per code its label sprite
// (sheet keytops3 or windowps) and help line (55651) (table 0x1036f250, 12 bytes a code).
struct BattleCode
{
    uint16_t sprite;
    bool     windowps;
    uint16_t help;
};
constexpr BattleCode kBattleCodes[28] = {
    {},             { 16, false, 31 },   { 23, false, 33 },  { 22, false, 32 },  { 1, false, 22 },   { 1, false, 23 },
    { 363, true, 626 }, { 4, false, 24 }, { 7, false, 25 },  { 10, false, 26 },  { 19, false, 29 },  { 19, false, 30 },
    { 25, false, 27 }, { 13, false, 34 }, { 38, false, 35 }, { 39, false, 36 },  { 79, true, 15 },   { 362, true, 627 },
    { 6, false, 37 },  { 28, true, 38 },  { 28, true, 38 },  { 41, false, 39 },  { 356, false, 596 }, { 357, false, 597 },
    { 358, false, 598 }, { 505, false, 718 }, { 668, true, 719 }, { 783, true, 954 },
};
// 1 attack, 2 call for help, 3 disengage, 4 chat, 5 tell, 6 talk to fellow, 7 abilities, 8 magic, 9 items, 10 examine,
// 11 check, 12 treasure, 13 fish, 14 chocobo dig, 15 dismount, 16 invite, 17 disband fellow, 18 switch target, 19 / 20
// trade, 21 dismount and attack, 22..24 Abyssea / Campaign entries, 25 trust, 26 release trust, 27 seek party.
constexpr uint8_t kBattleModes[8][13] = {
    { 4, 8, 7, 25, 9, 12, 13, 22, 24, 28 },             // 0 no target
    { 4, 14, 12, 15, 28 },                              // 1 mounted
    { 4, 8, 7, 25, 9, 19, 16, 12, 22, 24, 27, 10, 28 }, // 2 a player
    { 4, 14, 16, 12, 15, 27, 10, 28 },                  // 3 a player, mounted
    { 1, 8, 7, 25, 9, 12, 22, 24, 11, 28 },             // 4 a monster
    { 21, 14, 12, 11, 28 },                             // 5 a monster, mounted
    { 18, 8, 7, 25, 9, 3, 12, 24, 11, 28 },             // 6 engaged
    { 6, 8, 7, 25, 9, 20, 17, 12, 26, 28 },             // 7 a fellow
};

// The gil we hold (inventory slot 0 holds the gil, item 0xFFFF).
uint32_t gilHeld(const client::WorldSnapshot& w)
{
    for (const auto& it : w.inventory)
    {
        if (it.container == 0 && it.slot == 0)
        {
            return it.count;
        }
    }
    return 0;
}

} // namespace

std::array<std::optional<uint32_t>, 23>& nameColourOverrides()
{
    static std::array<std::optional<uint32_t>, 23> o;
    return o;
}

int nameColourIndex(const client::Entity& e, const client::WorldSnapshot& w)
{
    auto inParty = [&](uint32_t id) {
        return id == w.charId || std::any_of(w.party.begin(), w.party.end(), [&](const client::PartyMember& m) { return m.id == id; });
    };
    if (e.isPc)
    {
        return 0;
    }
    if (e.isMob && e.hpp == 0)
    {
        return 9;
    }
    if (e.claimId != 0)
    {
        return inParty(e.claimId) ? 6 : 7;
    }
    return e.isMob ? 5 : 4;
}

UiColor nameColour(const client::Entity& e, const client::WorldSnapshot& w)
{
    const int i = nameColourIndex(e, w);
    if (const auto& o = nameColourOverrides()[static_cast<std::size_t>(i)])
    {
        // D3DCOLOR (ARGB); the label keeps its own opacity.
        return UiColor{ static_cast<uint8_t>(*o >> 16), static_cast<uint8_t>(*o >> 8), static_cast<uint8_t>(*o), 0x80 };
    }
    switch (i)
    {
        // Players: white, party members and our own name too (a lossless retail capture; the game takes the actor's
        // +0x78, CODE 0x10082ff0, whose writer was not traced).
        case 0: return UiColor{ 0x80, 0x80, 0x80, 0x80 };
        case 9: return UiColor{ 0x50, 0x50, 0x50, 0x80 }; // a dead monster greys (INFERRED shade)
        case 6: return UiColor{ 0x80, 0x40, 0x40, 0x80 };
        case 7: return UiColor{ 0x78, 0x48, 0x80, 0x80 };
        case 5: return UiColor{ 0x80, 0x80, 0x48, 0x80 };
        default: return UiColor{ 0x58, 0x80, 0x58, 0x80 };
    }
}

GameUi::GameUi(UiRenderer& r)
: r_(r)
{
}

// --- input ---

bool GameUi::assignKey(SDL_Scancode key, bool repeat)
{
    if (stack_.empty() || stack_.back().kind != Kind::KeyAssign || typing_ || num_.active || KeyAssign::menuKey(key))
    {
        return false;
    }
    if (!repeat)
    {
        assignKeys_.push_back(key);
    }
    return true;
}

bool GameUi::keyDown(SDL_Keycode key, bool repeat)
{
    if (num_.active)
    {
        if (key == SDLK_BACKSPACE && !num_.digits.empty())
        {
            num_.digits.pop_back();
        }
        else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && !repeat && num_.doneText)
        {
            num_.active = false;
            auto done   = std::move(num_.doneText);
            done(num_.digits);
        }
        else if (num_.doneText)
        {
            if (key == SDLK_ESCAPE && !repeat)
            {
                num_.active = false;
            }
            return true; // characters arrive through textInput
        }
        else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && !repeat)
        {
            num_.active = false;
            if (!num_.digits.empty() && num_.done)
            {
                num_.done(static_cast<uint32_t>(std::min<unsigned long long>(std::stoull(num_.digits), 999999999ull)));
            }
        }
        else if (key == SDLK_ESCAPE && !repeat)
        {
            num_.active = false;
        }
        else if (key >= SDLK_0 && key <= SDLK_9 && num_.digits.size() < 9)
        {
            num_.digits.push_back(static_cast<char>('0' + (key - SDLK_0)));
        }
        else if (key >= SDLK_KP_1 && key <= SDLK_KP_0 && num_.digits.size() < 9)
        {
            num_.digits.push_back(key == SDLK_KP_0 ? '0' : static_cast<char>('1' + (key - SDLK_KP_1)));
        }
        return true;
    }
    if (sub_.active)
    {
        // Tab / Shift+Tab walk it as the arrows do (the game's target commands 15 / 16 move the cursor while one is up,
        // CODE 0x10158720).
        if (key == SDLK_TAB && (heldMods() & SDL_KMOD_SHIFT))
        {
            sub_.keys.push_back(SDLK_LEFT);
        }
        else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN || key == SDLK_TAB ||
                 ((key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) && !repeat))
        {
            sub_.keys.push_back(key);
        }
        return true;
    }
    if (fish_.active)
    {
        // The minigame takes the cursor keys, Enter (reel in) and Esc (give up).
        if (!repeat && (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE))
        {
            fish_.keys.push_back(key);
        }
        return true;
    }
    if (fish_.casting && fish_.phase == 0 && stack_.empty() && !repeat && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE))
    {
        // Line out, nothing hooked yet: Enter or Esc reels the line back in (CODE 0x10088a50 / 0x10088a80 -> mode 3, 201).
        fish_.keys.push_back(key);
        return true;
    }
    if (loginActive_ && login_)
    {
        auto& f = *login_;
        std::string* fields[] = { &f.host, &f.user, &f.pass, &f.character };
        switch (key)
        {
            case SDLK_UP: f.field = (f.field + 3) % 4; break;
            case SDLK_DOWN:
            case SDLK_TAB: f.field = (f.field + 1) % 4; break;
            case SDLK_BACKSPACE:
                if (!fields[f.field]->empty())
                {
                    fields[f.field]->pop_back();
                }
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (!repeat)
                {
                    loginSubmit_ = true;
                }
                break;
            case SDLK_ESCAPE: return false; // the client quits
            default: break;
        }
        return true;
    }
    if (lobbyActive_)
    {
        switch (key)
        {
            case SDLK_UP: pending_.push_back(Cmd::Up); return true;
            case SDLK_DOWN: pending_.push_back(Cmd::Down); return true;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (!repeat)
                {
                    pending_.push_back(Cmd::Confirm);
                }
                return true;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE: pending_.push_back(Cmd::Back); return true;
            default: return true;
        }
    }
    // F1..F6: target ourselves / party members in party-list order.
    if (!typing_ && !repeat && key >= SDLK_F1 && key <= SDLK_F6)
    {
        partyTargetKey_ = static_cast<int>(key - SDLK_F1) + 1;
        return true;
    }
    // (Ctrl | Alt) + Shift + 1..0 picks the palette's set 1..10 (key actions 0x59..0x61, 0x58; CODE 0x101a3100);
    // Ctrl / Alt + Down / Up the next / previous set while the palette shows (0x101a35e0).
    if (!typing_ && !repeat && stack_.empty())
    {
        const SDL_Keymod mods = heldMods();
        if ((mods & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) && (mods & SDL_KMOD_SHIFT) && key >= SDLK_0 && key <= SDLK_9)
        {
            setMacroSet(key == SDLK_0 ? 10 : static_cast<int>(key - SDLK_0));
            return true;
        }
        if ((mods & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) && (key == SDLK_UP || key == SDLK_DOWN))
        {
            setMacroSet(key == SDLK_DOWN ? macroSet_ % 10 + 1 : (macroSet_ + 8) % 10 + 1);
            return true;
        }
    }
    // Ctrl / Alt + 1..0: macros.
    if (!typing_ && !repeat && key >= SDLK_0 && key <= SDLK_9)
    {
        const SDL_Keymod mods = heldMods();
        const int        n    = key == SDLK_0 ? 10 : static_cast<int>(key - SDLK_0);
        if (mods & SDL_KMOD_CTRL)
        {
            runMacro(0, n);
            return true;
        }
        if (mods & SDL_KMOD_ALT)
        {
            runMacro(1, n);
            return true;
        }
    }
    if (typing_)
    {
        if (atKey(key))
        {
            return true; // the auto-translate windows took it
        }
        cursor_ = std::min(cursor_, input_.size());
        switch (key)
        {
            case SDLK_UP:
            case SDLK_DOWN:
                // Recall a sent line (newest first).
                if (!history_.empty())
                {
                    historyPos_ = key == SDLK_UP ? std::min(historyPos_ + 1, static_cast<int>(history_.size()) - 1) : std::max(historyPos_ - 1, -1);
                    input_      = historyPos_ < 0 ? std::string() : history_[static_cast<std::size_t>(historyPos_)];
                    cursor_     = input_.size();
                }
                return true;
            case SDLK_TAB: atOpen(); return true; // auto-translate
            case SDLK_LEFT: cursor_ = dat::chatedit::stepLeft(input_, cursor_); return true;
            case SDLK_RIGHT: cursor_ = dat::chatedit::stepRight(input_, cursor_); return true;
            case SDLK_HOME: cursor_ = 0; return true;
            case SDLK_END: cursor_ = input_.size(); return true;
            case SDLK_BACKSPACE: cursor_ = dat::chatedit::eraseLeft(input_, cursor_); return true;
            case SDLK_DELETE: cursor_ = dat::chatedit::eraseRight(input_, cursor_); return true;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: pending_.push_back(Cmd::Confirm); return true; // sent from draw(), which has the actions
            case SDLK_ESCAPE:
                typing_    = false;
                inputDone_ = nullptr; // a field's input line: left as it was
                input_.clear();
                cursor_ = 0;
                return true;
            default: return true;
        }
    }
    if (logMode_ == LogMode::Full && stack_.empty())
    {
        // The full-screen log's cursor (CODE 0x10138160): Up / Down move the highlighted row and scroll at the top /
        // bottom row; Left / Right page back / forward, and at the oldest / newest line put the cursor on the top /
        // bottom row. drawChat clamps the scroll to the lines the tab has.
        const int  rows   = std::max(1, logRows_);
        const int  oldest = std::max(0, logLines_ - rows);
        switch (key)
        {
            case SDLK_UP:
                if (logCursor_ < std::min(rows, std::max(1, logLines_)))
                {
                    ++logCursor_;
                }
                else if (chatScroll_ < oldest)
                {
                    ++chatScroll_;
                }
                return true;
            case SDLK_DOWN:
                if (logCursor_ > 1)
                {
                    --logCursor_;
                }
                else if (chatScroll_ > 0)
                {
                    --chatScroll_;
                }
                return true;
            case SDLK_LEFT:
            case SDLK_PAGEUP:
                if (chatScroll_ >= oldest)
                {
                    logCursor_ = std::min(rows, std::max(1, logLines_));
                }
                chatScroll_ = std::min(oldest, chatScroll_ + rows);
                return true;
            case SDLK_RIGHT:
            case SDLK_PAGEDOWN:
                if (chatScroll_ == 0)
                {
                    logCursor_ = 1;
                }
                chatScroll_ = std::max(0, chatScroll_ - rows);
                return true;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: return true; // a speaker's line opens "menu charlnk" in the game (not done)
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE:
                // Cancel closes the full log and selects the log window again (0x1013832a).
                logMode_    = LogMode::Selected;
                chatScroll_ = 0;
                return true;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
            case SDLK_INSERT:
                logMode_    = LogMode::Off;
                chatScroll_ = 0;
                open(Kind::Main);
                return true;
            default: return false;
        }
    }
    if (logMode_ == LogMode::Buffs && stack_.empty())
    {
        // The status icons (#122; ui/status_select.h): the cursor walks them, Confirm cancels the one under it when its
        // record allows (sent from draw()), Cancel deselects (CODE 0x10157e9e).
        switch (key)
        {
            case SDLK_LEFT: buffCursor_ = moveBuffCursor(buffCursor_, buffCount_, BuffKey::Left); return true;
            case SDLK_RIGHT: buffCursor_ = moveBuffCursor(buffCursor_, buffCount_, BuffKey::Right); return true;
            case SDLK_UP: buffCursor_ = moveBuffCursor(buffCursor_, buffCount_, BuffKey::Up); return true;
            case SDLK_DOWN: buffCursor_ = moveBuffCursor(buffCursor_, buffCount_, BuffKey::Down); return true;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (!repeat)
                {
                    buffConfirm_ = true;
                }
                return true;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE: logMode_ = LogMode::Off; return true;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
            case SDLK_INSERT:
                logMode_ = LogMode::Off;
                open(Kind::Main);
                return true;
            default: return false;
        }
    }
    if (logMode_ != LogMode::Off && stack_.empty())
    {
        switch (key)
        {
            case SDLK_UP: chatScroll_ += 1; return true;
            case SDLK_DOWN: chatScroll_ = std::max(0, chatScroll_ - 1); return true;
            case SDLK_PAGEUP: chatScroll_ += logRows_; return true;
            case SDLK_PAGEDOWN: chatScroll_ = std::max(0, chatScroll_ - logRows_); return true;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: openFullLog(); return true;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE: logMode_ = logMode_ == LogMode::Full ? LogMode::Selected : LogMode::Off; return true;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
            case SDLK_INSERT:
                logMode_ = LogMode::Off;
                open(Kind::Main);
                return true;
            case SDLK_LEFT:
            case SDLK_RIGHT: return true;
            default: return false;
        }
    }
    if (!stack_.empty())
    {
        switch (key)
        {
            case SDLK_UP: pending_.push_back(Cmd::Up); return true;
            case SDLK_DOWN: pending_.push_back(Cmd::Down); return true;
            case SDLK_LEFT: pending_.push_back(Cmd::Left); return true;
            case SDLK_RIGHT: pending_.push_back(Cmd::Right); return true;
            case SDLK_PAGEUP: pending_.push_back(Cmd::PageUp); return true;
            case SDLK_PAGEDOWN: pending_.push_back(Cmd::PageDown); return true;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (!repeat)
                {
                    pending_.push_back(Cmd::Confirm);
                }
                return true;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE: pending_.push_back(Cmd::Back); return true;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
            case SDLK_INSERT: stack_.clear(); return true; // the menu key closes everything
            // The chat line opens over the menus (INFERRED from the user's retail play, #71): while it is open the keys
            // go to it; Esc or sending the line gives them back to the menu, which stays as it was. Enter stays the
            // menu's confirm.
            case SDLK_SPACE: startTyping("", true); return true;
            case SDLK_SLASH:
                if (!repeat)
                {
                    startTyping("/", true);
                }
                return true;
            default: return false;
        }
    }
    switch (key)
    {
        case SDLK_MINUS:
        case SDLK_KP_MINUS:
        case SDLK_INSERT: open(Kind::Main); return true;
        case SDLK_PAGEUP: chatScroll_ += 3; return true;
        case SDLK_PAGEDOWN: chatScroll_ = std::max(0, chatScroll_ - 3); return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (repeat)
            {
                return true;
            }
            pending_.push_back(Cmd::Confirm); // knocked out: the home point prompt; Enter never opens the chat line (#72)
            return true;
        case SDLK_SPACE:
            typing_ = true;
            return true;
        case SDLK_SLASH:
            if (!repeat)
            {
                startTyping("/"); // a command: the line opens with the slash typed (text input is off until now)
            }
            return true;
        default: return false;
    }
}

bool GameUi::textInput(std::string_view utf8)
{
    if (num_.active)
    {
        for (const char c : utf8)
        {
            if (num_.doneText && static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F && num_.digits.size() < 15)
            {
                num_.digits.push_back(c);
            }
        }
        return true; // digits arrive through keyDown
    }
    if (loginActive_ && login_)
    {
        std::string* fields[] = { &login_->host, &login_->user, &login_->pass, &login_->character };
        for (const char c : utf8)
        {
            if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F && fields[login_->field]->size() < 60)
            {
                fields[login_->field]->push_back(c);
            }
        }
        return true;
    }
    if (!typing_)
    {
        return false;
    }
    chatInsert(utf8);
    return true;
}

bool GameUi::mouseMove(ImVec2 screen)
{
    mouse_      = screen;
    mouseMoved_ = true;
    return false;
}

bool GameUi::mouseDown(int button, ImVec2 screen)
{
    if (stack_.empty())
    {
        return false;
    }
    mouse_ = screen;
    if (button == 3) // SDL_BUTTON_RIGHT
    {
        pending_.push_back(Cmd::Back);
        return true;
    }
    if (button == 1)
    {
        // Taken only when over the top menu's window; hit testing happens in draw() with the current canvas.
        const auto* l = layout(stack_.back());
        if (!l)
        {
            return false;
        }
        const ImVec2 o = lastCanvas_.windowOrigin(l->window);
        const float  s = lastCanvas_.scale;
        if (screen.x >= o.x && screen.y >= o.y && screen.x < o.x + l->window.w * s && screen.y < o.y + l->window.h * s)
        {
            click_ = true;
            return true;
        }
    }
    return false;
}

bool GameUi::wheel(float dy)
{
    if (!stack_.empty() && isList(stack_.back().kind))
    {
        pending_.push_back(dy > 0 ? Cmd::Up : Cmd::Down);
        return true;
    }
    // Over the log window: scroll it (PageUp/PageDown zoom the camera, as in the game).
    if (mouse_.x >= chatMin_.x && mouse_.x < chatMax_.x && mouse_.y >= chatMin_.y && mouse_.y < chatMax_.y)
    {
        chatScroll_ = std::max(0, chatScroll_ + (dy > 0 ? 3 : -3));
        return true;
    }
    return false;
}

namespace
{

std::string lower(std::string_view s)
{
    std::string out(s);
    for (auto& c : out)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Splits a command line into words; "double quotes" group words ("/ma \"Cure II\" <t>").
std::vector<std::string> words(std::string_view s)
{
    std::vector<std::string> out;
    std::string              cur;
    bool                     quoted = false;
    for (const char c : s)
    {
        if (c == '"')
        {
            quoted = !quoted;
            continue;
        }
        if (c == ' ' && !quoted)
        {
            if (!cur.empty())
            {
                out.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty())
    {
        out.push_back(std::move(cur));
    }
    return out;
}

} // namespace

// C2S 0x04E AUC {Command, AucWorkIndex, Result, ResultStatus, Param[12] @4, Parcel[40] @0x10}: Result and status 0.
std::vector<std::byte> GameUi::aucPacket(uint8_t command, int8_t workIndex)
{
    std::vector<std::byte> p(0x38);
    p[0] = static_cast<std::byte>(command);
    p[1] = static_cast<std::byte>(workIndex);
    return p;
}

void GameUi::aucLoadSorts()
{
    // AUCSORT.DAT beside the macro files (the game's USER/<id> folder): 8 bytes, 0x42 x 20 entries `dir << 4 | key`, the
    // 0x42 counts (CODE 0x101d7420 reads it, 0x101d73d0 writes it; 0x574 bytes).
    if (aucSortsLoaded_)
    {
        return;
    }
    aucSortsLoaded_ = true;
    if (macroDir_.empty())
    {
        return;
    }
    std::ifstream     in(macroDir_ / "AUCSORT.DAT", std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 8 + 0x42 * 20 + 0x42)
    {
        return;
    }
    for (std::size_t c = 0; c < aucSorts_.size(); ++c)
    {
        const auto n = std::min<std::size_t>(static_cast<uint8_t>(b[8 + 0x42 * 20 + c]), 20);
        for (std::size_t k = 0; k < n; ++k)
        {
            aucSorts_[c].push_back(static_cast<uint8_t>(b[8 + c * 20 + k]));
        }
    }
}

void GameUi::aucSaveSorts() const
{
    if (macroDir_.empty())
    {
        return;
    }
    std::vector<char> b(0x574, 0);
    for (std::size_t c = 0; c < aucSorts_.size(); ++c)
    {
        const auto n         = std::min<std::size_t>(aucSorts_[c].size(), 20);
        b[8 + 0x42 * 20 + c] = static_cast<char>(n);
        for (std::size_t k = 0; k < n; ++k)
        {
            b[8 + c * 20 + k] = static_cast<char>(aucSorts_[c][k]);
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(macroDir_, ec);
    std::ofstream(macroDir_ / "AUCSORT.DAT", std::ios::binary).write(b.data(), static_cast<std::streamsize>(b.size()));
}

void GameUi::aucAddTerm(uint32_t type, bool descending, int value)
{
    // CODE 0x101040a0 (sort) / 0x10104010 (filter): one term a key, at most 20; a filter carries its value.
    if (aucQuery_.size() >= 20 || std::ranges::any_of(aucQuery_, [type](const client::AuctionTerm& t) { return t.type == type; }))
    {
        return;
    }
    uint32_t bits = descending ? 2u : 0u;
    if (value >= 0)
    {
        bits |= 1u | (static_cast<uint32_t>(value & 0x7FFF) << 2);
    }
    aucQuery_.push_back({ type, bits });
}

void GameUi::aucRebuildQuery(int category)
{
    // CODE 0x101da250 / 0x101d7330: a fresh query, then the category's stored sorts in order. Level, damage, defense and
    // resistance sort high to low; delay and name low to high (the table at 0x101d73bc).
    aucLoadSorts();
    aucListCategory_ = category;
    aucQuery_.clear();
    if (category < 0)
    {
        return;
    }
    for (const uint8_t b : aucSorts_[static_cast<std::size_t>(category) % aucSorts_.size()])
    {
        const uint32_t key = b & 0x0F;
        if (key == 3 || key == 4)
        {
            continue; // filters are not stored
        }
        aucAddTerm(key, key == 2 || key == 5 || key == 7 || key == 8);
    }
}

void GameUi::aucRequestList(const GameUiActions& act, int category)
{
    if (act.auctionList && category >= 0)
    {
        std::string terms;
        for (const auto& t : aucQuery_)
        {
            terms += fmt::format(" {}:{:#x}", t.type, t.bits);
        }
        spdlog::info("auction: list {} terms{}", category, terms.empty() ? " none" : terms);
        act.auctionList(static_cast<uint8_t>(category), aucQuery_);
    }
}

void GameUi::askNumber(std::string prompt, std::function<void(uint32_t)> done)
{
    num_ = { true, std::move(prompt), {}, std::move(done), {} };
}

void GameUi::askText(std::string prompt, std::function<void(std::string)> done)
{
    num_ = { true, std::move(prompt), {}, {}, std::move(done) };
}

void GameUi::askComment(uint32_t type, const std::function<void(uint16_t, std::vector<std::byte>)>& send)
{
    // C2S 0x0E0 SET_USERMSG {text[128], u32 x4, u32 type @0x90} (CODE 0x100e58b0); empty text clears the comment.
    stack_.clear();
    askText("Comment", [type, send](std::string text) {
        if (!send)
        {
            return;
        }
        std::vector<std::byte> pkt(0x94);
        std::memcpy(pkt.data(), text.data(), std::min<std::size_t>(text.size(), 127));
        const uint32_t t = text.empty() ? 0u : type;
        std::memcpy(pkt.data() + 0x90, &t, 4);
        send(0x0E0, std::move(pkt));
    });
}

// The linkshell id of the pearl / sack worn in linkshell slot `n` (0 or 1): its exdata GroupId (LSB Exdata::Linkshell).
uint32_t GameUi::linkshellId(int n) const
{
    if (!data_ || !data_->world)
    {
        return 0;
    }
    const auto& w        = *data_->world;
    const auto [c, slot] = w.equipment[16 + static_cast<std::size_t>(n)];
    if (slot == 0)
    {
        return 0;
    }
    for (const auto& it : w.inventory)
    {
        if (it.container == c && it.slot == slot)
        {
            return static_cast<uint32_t>(it.attr[0]) | static_cast<uint32_t>(it.attr[1]) << 8 | static_cast<uint32_t>(it.attr[2]) << 16 |
                   static_cast<uint32_t>(it.attr[3]) << 24;
        }
    }
    return 0;
}

void GameUi::runSearch(const std::function<void(const client::SearchQuery&)>& search)
{
    // A pick replaces one criterion and searches at once (CODE 0x1020a580); the results window opens over searchma.
    if (search)
    {
        search(searchQuery_);
    }
    while (!stack_.empty() && stack_.back().kind != Kind::SearchMenu)
    {
        stack_.pop_back();
    }
    open(Kind::SearchResults);
}

void GameUi::drawNumberEntry(UiPainter& p, const UiCanvas& c)
{
    if (!num_.active)
    {
        return;
    }
    const float  s = c.scale;
    const ImVec2 at((c.width - 180) / 2 * s, c.height * 0.40f * s); // canvas size is in authored units
    p.frame(at, ImVec2(180 * s, 34 * s));
    p.textShp(num_.prompt, ImVec2(at.x + 8 * s, at.y + 4 * s), kGold);
    p.textShp((num_.digits.empty() && !num_.doneText ? std::string("0") : num_.digits) + ((tick_ / 20) % 2 ? "_" : " "), ImVec2(at.x + 8 * s, at.y + 17 * s), kWhite);
}

// C2S 0x04D PBX {Command, BoxNo, PostWorkNo, ItemWorkNo, i32 ItemStacks, Result, ResParam1-3, TargetName[16]}.
std::vector<std::byte> GameUi::deliveryPacket(uint8_t command, int8_t box, int8_t post, int8_t item, int32_t stacks, std::string_view name)
{
    std::vector<std::byte> p(0x1C);
    p[0] = static_cast<std::byte>(command);
    p[1] = static_cast<std::byte>(box);
    p[2] = static_cast<std::byte>(post);
    p[3] = static_cast<std::byte>(item);
    std::memcpy(p.data() + 4, &stacks, 4);
    std::memcpy(p.data() + 0x0C, name.data(), std::min<std::size_t>(name.size(), 15));
    return p;
}

bool GameUi::command(const std::string& text, const GameUiData& d, const GameUiActions& act)
{
    const auto w = words(text);
    if (w.empty() || !d.world || !act.action)
    {
        return false;
    }
    const auto& world = *d.world;
    const auto  cmd   = lower(w[0]);
    // A sub-target word: wait for the pick (drawSubTarget); <lastst> is the previous pick.
    {
        const std::string lt = lower(text);
        for (const char* tok : { "<stpc>", "<stnpc>", "<stpt>", "<stal>", "<st>" })
        {
            if (const auto at = lt.find(tok); at != std::string::npos && !sub_.active)
            {
                beginSubTarget(text, text.substr(at, std::strlen(tok)), 0, d);
                return true;
            }
        }
        if (const auto at = lt.find("<lastst>"); at != std::string::npos && sub_.last)
        {
            std::string again = text;
            again.replace(at, 8, fmt::format("<id:{:x}>", sub_.last));
            return command(again, d, act) || (submitLine(again, d, act), true);
        }
    }
    // Target words: <me> ourselves, <p0>-<p5> / <a10>-<a25> party and alliance members, <pet>, <bt> the party's battle
    // target, <t> / <st> the current target; none = the command's default.
    auto targetOf = [&](std::size_t i, bool enemyDefault) -> std::pair<uint32_t, uint16_t> {
        const std::string t = i < w.size() ? lower(w[i]) : std::string();
        // <id:HEX>: a sub-target picked with the cursor.
        if (t.starts_with("<id:") && t.ends_with('>'))
        {
            const auto id = static_cast<uint32_t>(std::strtoul(t.c_str() + 4, nullptr, 16));
            if (id == world.charId)
            {
                return { world.charId, world.actIndex };
            }
            for (const auto& [dist, e] : d.nearby)
            {
                if (e->id == id)
                {
                    return { e->id, e->index };
                }
            }
        }
        // A server id typed as a number ("/ma \"Cure\" 16793618"): what Ashita plugins such as Shorthand queue once they
        // have resolved a target (INFERRED: retail takes a decimal id there).
        if (!t.empty() && t.size() <= 10 && std::ranges::all_of(t, [](char c) { return c >= '0' && c <= '9'; }))
        {
            const auto id = static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 10));
            if (id == world.charId)
            {
                return { world.charId, world.actIndex };
            }
            for (const auto& [dist, e] : d.nearby)
            {
                if (e->id == id)
                {
                    return { e->id, e->index };
                }
            }
        }
        // <p0>..<p5>: our party in list order (<p0> is us); <a10>..<a15>, <a20>..<a25>: the alliance's other parties.
        int party = -1, member = -1;
        if (t.size() == 4 && t[1] == 'p' && t[2] >= '0' && t[2] <= '5' && t[3] == '>')
        {
            party  = 0;
            member = t[2] - '0';
        }
        else if (t.size() == 5 && t[1] == 'a' && (t[2] == '1' || t[2] == '2') && t[3] >= '0' && t[3] <= '5' && t[4] == '>')
        {
            party  = t[2] - '0';
            member = t[3] - '0';
        }
        if (party >= 0)
        {
            std::vector<const client::PartyMember*> list;
            for (const auto& m : world.party)
            {
                if (m.partyNo == party)
                {
                    list.push_back(&m);
                }
            }
            std::ranges::sort(list, [](const auto* a, const auto* b) { return a->order < b->order; });
            if (party == 0 && list.empty() && member == 0)
            {
                return { world.charId, world.actIndex };
            }
            if (member < static_cast<int>(list.size()))
            {
                return { list[static_cast<std::size_t>(member)]->id, list[static_cast<std::size_t>(member)]->index };
            }
            return { world.charId, world.actIndex }; // nobody there: ourselves, as the game does for <p5> in a small party
        }
        if (t == "<pet>" && world.petIndex != 0)
        {
            for (const auto& [dist, e] : d.nearby)
            {
                if (e->index == world.petIndex)
                {
                    return { e->id, e->index };
                }
            }
        }
        if (t == "<bt>")
        {
            // The nearest monster claimed by us or our party.
            for (const auto& [dist, e] : d.nearby)
            {
                if (e->isMob && e->claimId != 0 && e->hpp > 0 &&
                    (e->claimId == world.charId || std::ranges::any_of(world.party, [&](const client::PartyMember& m) { return m.id == e->claimId; })))
                {
                    return { e->id, e->index };
                }
            }
        }
        if (t == "<me>" || (t.empty() && !enemyDefault) || !d.target)
        {
            return { world.charId, world.actIndex };
        }
        return { d.target->id, d.target->index };
    };
    const std::string name = w.size() > 1 ? lower(w[1]) : std::string();
    // /chatmode (/cm) <say|shout|party|linkshell|linkshell2|yell|unity|emote>: the channel for lines without a prefix.
    if (cmd == "/chatmode" || cmd == "/cm")
    {
        static const std::pair<const char*, uint8_t> kModes[] = { { "say", 0 },        { "s", 0 },  { "shout", 1 }, { "sh", 1 }, { "party", 4 },
                                                                  { "p", 4 },          { "linkshell", 5 }, { "l", 5 }, { "linkshell2", 27 },
                                                                  { "l2", 27 },        { "yell", 26 }, { "y", 26 },   { "unity", 33 }, { "u", 33 },
                                                                  { "emote", 8 },      { "em", 8 } };
        for (const auto& [n, k] : kModes)
        {
            if (name == n)
            {
                chatKind_ = k;
                return true;
            }
        }
        return false; // unknown mode: the command error
    }
    // Storage: /satchel /sack /case /wardrobe[2-8] /bank /bank2 /storage /locker /recycle (LSB container ids). The
    // server decides which moves are allowed where (the Mog Safe only in the Mog House, ...).
    {
        static const std::pair<const char*, int> kContainers[] = {
            { "/bank", 1 },      { "/storage", 2 },   { "/locker", 4 },    { "/satchel", 5 },   { "/sack", 6 },      { "/case", 7 },
            { "/wardrobe", 8 },  { "/bank2", 9 },     { "/wardrobe2", 10 }, { "/wardrobe3", 11 }, { "/wardrobe4", 12 }, { "/wardrobe5", 13 },
            { "/wardrobe6", 14 }, { "/wardrobe7", 15 }, { "/wardrobe8", 16 }, { "/recycle", 17 },
        };
        for (const auto& [n, c] : kContainers)
        {
            if (cmd == n)
            {
                stack_.clear();
                open(Kind::Storage, c);
                return true;
            }
        }
    }
    // Mog House: /garden, /layout, /remodel (command ids 0x80 / 0x7E / 0x7F, CODE 0x1007e418..0x1007e7f5): only in the
    // Mog House and, for /layout and /remodel, not while it is open to visitors (the game then prints a refusal, not
    // traced); /remodel also wants the 2F. The Mog House menu's elements do the same.
    if ((cmd == "/garden" || cmd == "/layout" || cmd == "/remodel") && d.world->inMogHouse)
    {
        const bool garden = cmd == "/garden", remodel = cmd == "/remodel";
        if (garden || (!d.world->mhOpen && (!remodel || d.world->mhExpansion)))
        {
            stack_.clear();
            appChoice_ = { kMyRoomTag, garden ? 4 : remodel ? 7 : 5 };
        }
        return true;
    }
    // Delivery box: /deliverybox (/mailbox) opens the incoming box; /sendpost [name] the outgoing one, sending to `name`.
    // The server allows it in residential areas and towns with an auction house or the Mog menu.
    if ((cmd == "/deliverybox" || cmd == "/mailbox" || cmd == "/sendpost") && act.send)
    {
        // LSB's checks: open, close, Query and Confirm carry BoxNo -1; Recv carries ItemWorkNo 1.
        const bool out = cmd == "/sendpost";
        act.send(0x04D, deliveryPacket(out ? 0x0D : 0x0E, -1)); // DeliOpen / PostOpen
        if (out && w.size() > 1)
        {
            sendTo_ = w[1];
            act.send(0x04D, deliveryPacket(0x0C, -1, -1, -1, -1, sendTo_)); // Query: does the name exist
        }
        stack_.clear();
        open(Kind::DeliveryBox, out ? 2 : 1);
        return true;
    }
    // Equipment sets (loadEquipSets): /equipset N equips set N in one C2S 0x051 EQUIPSET_SET {Count, pad[3],
    // Equipment[16] {ItemIndex, EquipKind, Category}}, taking each item from the inventory or a wardrobe;
    // /lockstyleset N locks its look (0x053 Mode 3); "/equipset save N [name]" stores today's gear; no number lists.
    if ((cmd == "/equipset" || cmd == "/lockstyleset") && d.items && act.send)
    {
        auto note = [&](const std::string& t) {
            if (act.note)
            {
                act.note(t);
            }
        };
        if (name == "save" && cmd == "/equipset")
        {
            const int n = w.size() > 2 ? std::atoi(w[2].c_str()) : 0;
            if (n < 1 || n > 100)
            {
                return false;
            }
            EquipSet set;
            for (std::size_t k = 3; k < w.size(); ++k)
            {
                set.name += (set.name.empty() ? "" : " ") + w[k];
            }
            for (uint8_t s = 0; s < 16; ++s)
            {
                const auto [c, at] = world.equipment[s];
                for (const auto& it : world.inventory)
                {
                    if (at != 0 && it.container == c && it.slot == at)
                    {
                        if (const auto info = d.items->find(it.itemId))
                        {
                            set.items.emplace_back(s, lower(info->name));
                        }
                    }
                }
            }
            if (set.name.empty() && equipSets_.contains(n))
            {
                set.name = equipSets_[n].name; // the menu's "save what is worn" keeps the set's name
            }
            note(fmt::format("Equipment set {} saved ({} items).", n, set.items.size()));
            equipSets_[n] = std::move(set);
            saveEquipSets();
            return true;
        }
        const int n = std::atoi(name.c_str());
        if (n == 0)
        {
            for (const auto& [k, set] : equipSets_)
            {
                note(fmt::format("Set {}: {} ({} items)", k, set.name.empty() ? std::string("-") : set.name, set.items.size()));
            }
            if (equipSets_.empty())
            {
                note("No equipment sets (use /equipset save N).");
            }
            return true;
        }
        const auto it = equipSets_.find(n);
        if (it == equipSets_.end())
        {
            return false;
        }
        struct Pick
        {
            uint8_t slot, container, index;
            uint16_t itemId;
        };
        std::vector<Pick> picks;
        for (const auto& [slot, item] : it->second.items)
        {
            for (const auto& inv : world.inventory)
            {
                const bool wardrobe = inv.container == 0 || inv.container == 8 || (inv.container >= 10 && inv.container <= 16);
                const auto info     = wardrobe && inv.slot != 0 ? d.items->find(inv.itemId) : std::nullopt;
                if (info && lower(info->name) == item &&
                    std::ranges::none_of(picks, [&](const Pick& p) { return p.container == inv.container && p.index == inv.slot; }))
                {
                    picks.push_back({ slot, inv.container, inv.slot, inv.itemId });
                    break;
                }
            }
        }
        if (picks.empty())
        {
            return true;
        }
        std::vector<std::byte> pkt(cmd == "/equipset" ? 4 + 16 * 4 : 4 + 16 * 8);
        pkt[0] = static_cast<std::byte>(picks.size());
        for (std::size_t k = 0; k < picks.size() && k < 16; ++k)
        {
            const auto& p = picks[k];
            if (cmd == "/equipset")
            {
                auto* e = pkt.data() + 4 + 4 * k;
                e[0]    = static_cast<std::byte>(p.index);
                e[1]    = static_cast<std::byte>(p.slot);
                e[2]    = static_cast<std::byte>(p.container);
            }
            else // LOCKSTYLE item {ItemIndex, EquipKind, Category, pad, u16 ItemNo, pad}
            {
                auto* e = pkt.data() + 4 + 8 * k;
                e[0]    = static_cast<std::byte>(p.index);
                e[1]    = static_cast<std::byte>(p.slot);
                e[2]    = static_cast<std::byte>(p.container);
                std::memcpy(e + 4, &p.itemId, 2);
            }
        }
        if (cmd == "/lockstyleset")
        {
            pkt[1] = std::byte{ 3 }; // Mode: Set
        }
        act.send(cmd == "/equipset" ? 0x051 : 0x053, std::move(pkt));
        note(cmd == "/equipset" ? fmt::format("Set #{} equipped.", n) : fmt::format("Appearance locked to equipment set {}.", n)); // 55653 #349 / #456
        return true;
    }
    // /partyinfo showtp [on | off] (CODE 0x10079240, sub-command table 0x103579fc): the pet's TP on its line over the
    // party window (config 174 = 1 / 0, else a toggle; 0x102024b0 draws it with 174 bit 0).
    if (cmd == "/partyinfo")
    {
        if (name == "showtp" && act.setConfig)
        {
            const std::string v  = w.size() > 2 ? lower(w[2]) : std::string();
            const bool        on = v == "on" ? true : v == "off" ? false : (cfgValue(d, 174) & 1) == 0;
            act.setConfig(174, on ? 1 : 0);
        }
        return true;
    }
    // /areaeffect (/aoe) [on | off] (CODE 0x100792d0): Config Misc. 3's AoE range graphics (config 192), else a toggle;
    // 55648 #297 / #298.
    if (cmd == "/areaeffect" || cmd == "/aoe")
    {
        const bool on = name == "on" ? true : name == "off" ? false : cfgValue(d, 192) == 0;
        if (act.setConfig)
        {
            act.setConfig(192, on ? 1 : 0);
        }
        if (act.note)
        {
            act.note(on ? "Area of effect display on" : "Area of effect display off");
        }
        return true;
    }
    // Display switches: /statustimer (timers under the status icons; 55648 #316 / #317), /yellsw (yell chat), /assistjsw
    // and /assistesw (the assist channels). "on" / "off", else a toggle.
    {
        bool* flag = cmd == "/statustimer" ? &statusTimers_ : cmd == "/yellsw" ? &hideYell_ : cmd == "/assistjsw" ? &hideAssistJ_
                   : cmd == "/assistesw"   ? &hideAssistE_  : nullptr;
        if (flag == &statusTimers_)
        {
            statusTimers_ = cfgValue(d, 195) != 0; // the same setting as Config Misc. 3's status icon timers (cfg 195)
        }
        if (flag)
        {
            const bool hidden = flag != &statusTimers_; // the chat switches store "hidden"
            const bool on     = name == "on" ? true : name == "off" ? false : (hidden ? *flag : !*flag);
            *flag             = hidden ? !on : on;
            if (flag == &statusTimers_ && act.setConfig)
            {
                act.setConfig(195, on ? 1 : 0);
            }
            if (act.note)
            {
                act.note(cmd == "/statustimer" ? (on ? "Status icon timers are enabled." : "Status icon timers are disabled.")
                                               : fmt::format("{} {}.", cmd == "/yellsw" ? "Yell messages" : "Assist channel messages", on ? "shown" : "hidden"));
            }
            return true;
        }
    }
    // /lsmes (/linkshellmes), /ls2mes (/linkshell2mes): the linkshell's message again.
    if (cmd == "/lsmes" || cmd == "/linkshellmes" || cmd == "/ls2mes" || cmd == "/linkshell2mes")
    {
        const auto& [msg, by] = world.lsMessage[cmd.find('2') != std::string::npos ? 1 : 0];
        if (act.note)
        {
            act.note(msg.empty() ? std::string("No linkshell message.") : fmt::format("{} (posted by {})", msg, by));
        }
        return true;
    }
    // /treasure (/pool): the treasure pool (as the battle menu's Treasure).
    if (cmd == "/treasure" || cmd == "/pool")
    {
        stack_.clear();
        open(Kind::Treasure);
        return true;
    }
    // /clock (the menu's Current Time): Vana'diel time of day and Earth time.
    if (cmd == "/clock" && act.note)
    {
        const double vh  = client::vanaHourOfDay();
        const auto   now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char         earth[32];
        std::strftime(earth, sizeof(earth), "%H:%M", std::localtime(&now));
        act.note(fmt::format("Vana'diel time: {:02}:{:02}  Earth time: {}", static_cast<int>(vh), static_cast<int>((vh - std::floor(vh)) * 60), earth));
        return true;
    }
    // /regionmap (/rmap): the Conquest list; C2S 0x05A REQCONQUEST asks for S2C 0x05E.
    if ((cmd == "/regionmap" || cmd == "/rmap" || cmd == "/besiegedmap" || cmd == "/bmap") && act.send)
    {
        act.send(0x05A, {});
        stack_.clear();
        open(Kind::Conquest, cmd == "/besiegedmap" || cmd == "/bmap" ? 1 : 0);
        return true;
    }
    // /itemsearch "<item>": C2S 0x02C ITEMSEARCH {Language 1 English, pad[3], Name[64]}; the answer (S2C 0x049) opens
    // the item's auction window. The server allows auctions only in zones with an auction house.
    if (cmd == "/itemsearch" && act.send && w.size() > 1)
    {
        std::string item;
        for (std::size_t k = 1; k < w.size(); ++k)
        {
            item += (item.empty() ? "" : " ") + w[k];
        }
        std::vector<std::byte> pkt(0x44);
        pkt[0] = std::byte{ 1 };
        std::memcpy(pkt.data() + 4, item.data(), std::min<std::size_t>(item.size(), 63));
        act.send(0x02C, std::move(pkt));
        return true;
    }
    // /macro book B | set S: the palette's book (1-40) and set (1-10), wrapping (CODE 0x10079e40 -> 0x101a24d0 /
    // 0x101a2440); refused while the macro editor is open.
    if (cmd == "/macro")
    {
        if (std::any_of(stack_.begin(), stack_.end(), [](const Menu& mm) { return isMacroKind(mm.kind) && mm.kind != Kind::MacroMenu && mm.kind != Kind::MacroBooks; }))
        {
            return true;
        }
        const int n = w.size() > 2 ? std::atoi(w[2].c_str()) : 0;
        if (name == "book" && n >= 1)
        {
            return setMacroBook((n - 1) % 40 + 1);
        }
        if (name == "set" && n >= 1)
        {
            return setMacroSet((n - 1) % 10 + 1);
        }
        return false;
    }
    if (cmd == "/sort")
    {
        sortInventory(d, act); // the display order, then C2S 0x03A
        return true;
    }
    if (cmd == "/timestamp")
    {
        // The same setting as Config Windows' timestamps (cfg 181; CODE 0x1007b570): on = HH:MM:SS.
        const bool on = name == "on" ? true : name == "off" ? false : cfgValue(d, 181) == 0;
        if (act.setConfig)
        {
            act.setConfig(181, on ? 2 : 0);
        }
        return true;
    }
    // /blacklist (/blist) alone opens the blacklist window (CODE 0x1007c5f0 -> 0x101deac0); "add" / "delete" go to the
    // chat commands. /mutelist searches every area for flags 2 = 0x20000000 (CODE 0x1007c970).
    if ((cmd == "/blacklist" || cmd == "/blist") && w.size() == 1)
    {
        stack_.clear();
        open(Kind::Blacklist);
        return true;
    }
    if (cmd == "/mutelist" && act.search)
    {
        stack_.clear();
        searchQuery_        = {};
        searchQuery_.flags2 = 0x20000000;
        runSearch(act.search);
        return true;
    }
    if (cmd == "/keyitem" || cmd == "/quest" || cmd == "/mission")
    {
        stack_.clear();
        open(cmd == "/keyitem" ? Kind::KeyItems : cmd == "/quest" ? Kind::Quests : Kind::Missions);
        return true;
    }
    // /lastsynth: the most recent synthesis again, if the materials are still there.
    if (cmd == "/lastsynth")
    {
        if (!synthHistory_.empty() && act.send && refillSynthesis(synthHistory_.front(), world))
        {
            sendSynthesis(act, world);
        }
        else if (act.note)
        {
            act.note("Failed to execute command."); // d_msg 55648 #102
        }
        return true;
    }
    if (cmd == "/ma" || cmd == "/magic" || cmd == "/ninjutsu" || cmd == "/nin" || cmd == "/song" || cmd == "/so")
    {
        for (const auto id : world.spells)
        {
            if (d.text && lower(d.text->spellName(id)) == name)
            {
                const auto* sp       = d.spells ? d.spells->find(id) : nullptr;
                const bool  offensive = sp && (sp->targets & 0x20);
                const auto [tid, tidx] = targetOf(2, offensive);
                act.action(tid, tidx, 3, id);
                return true;
            }
        }
        spdlog::info("{}: '{}' is not among the {} known spells", cmd, name, world.spells.size());
        return true; // unknown spell: swallowed, as the game does ("You cannot use that command")
    }
    // /bstpet [index | name]: Ready (the pet abilities in 0x0AC); no argument lists them with their index numbers.
    if (cmd == "/bstpet")
    {
        std::vector<uint16_t> ready;
        for (const auto n : world.petAbilities)
        {
            ready.push_back(static_cast<uint16_t>(512 + n));
        }
        if (name.empty())
        {
            for (std::size_t k = 0; k < ready.size() && act.note && d.text; ++k)
            {
                act.note(fmt::format("{}: {}", k + 1, d.text->abilityName(512u + ready[k])));
            }
            if (ready.empty() && act.note)
            {
                act.note("No pet abilities.");
            }
            return true;
        }
        const int index = std::atoi(name.c_str());
        for (std::size_t k = 0; k < ready.size(); ++k)
        {
            if ((index > 0 && static_cast<std::size_t>(index) == k + 1) || (d.text && lower(d.text->abilityName(512u + ready[k])) == name))
            {
                const auto [tid, tidx] = targetOf(2, true);
                act.action(tid, tidx, 9, ready[k]);
                return true;
            }
        }
        return true;
    }
    if (cmd == "/ja" || cmd == "/jobability" || cmd == "/pet")
    {
        // /pet also reaches the pet abilities (blood pacts, Ready moves): ability 512 + bit.
        for (const auto n : cmd == "/pet" ? world.petAbilities : std::vector<uint16_t>{})
        {
            const uint16_t id = static_cast<uint16_t>(512 + n);
            if (d.text && lower(d.text->abilityName(512u + id)) == name)
            {
                const auto [tid, tidx] = targetOf(2, true);
                act.action(tid, tidx, 9, id);
                return true;
            }
        }
        for (const auto id : world.jobAbilities)
        {
            if (d.text && lower(d.text->abilityName(512u + id)) == name)
            {
                const auto* ab        = d.abilities ? d.abilities->find(id) : nullptr;
                const bool  offensive = ab && (ab->targets & 0x20);
                const auto [tid, tidx] = targetOf(2, offensive);
                act.action(tid, tidx, 9, id);
                return true;
            }
        }
        return true;
    }
    // /fish: cast (ACTION 0x0E; needs a rod and bait). The hook check and the minigame follow in fishingTick.
    if (cmd == "/fish")
    {
        if (act.action)
        {
            act.action(world.charId, world.actIndex, 0x0E, 0);
        }
        return true;
    }
    // /recast "<ability or spell>": the time until it can be used again, as "Name: m:ss" (FFXiMain.dll's "%d:%02d").
    if (cmd == "/recast")
    {
        auto left = [&](const auto& ready, uint16_t id) {
            const auto it = ready.find(id);
            return it == ready.end() || it->second <= d.now
                       ? 0
                       : static_cast<int>(std::ceil(std::chrono::duration<float>(it->second - d.now).count()));
        };
        auto say = [&](std::string_view label, int seconds) {
            if (act.note)
            {
                act.note(fmt::format("{}: {}:{:02}", label, seconds / 60, seconds % 60));
            }
        };
        for (const auto id : world.jobAbilities)
        {
            if (d.text && lower(d.text->abilityName(512u + id)) == name)
            {
                const auto* ab  = d.abilities ? d.abilities->find(id) : nullptr;
                const auto  grp = world.abilityRecastGroup.find(id);
                say(d.text->abilityName(512u + id),
                    grp != world.abilityRecastGroup.end() ? left(world.abilityRecastReady, grp->second)
                    : ab                                  ? left(world.abilityRecastReady, ab->recastId)
                                                          : 0);
                return true;
            }
        }
        for (const auto id : world.spells)
        {
            if (d.text && lower(d.text->spellName(id)) == name)
            {
                say(d.text->spellName(id), left(world.spellReady, id));
                return true;
            }
        }
        if (act.note)
        {
            act.note("Spelling of name is incorrect."); // d_msg 55648 #101
        }
        return true;
    }
    if (cmd == "/ws" || cmd == "/weaponskill")
    {
        for (const auto id : world.weaponSkills)
        {
            if (d.text && lower(d.text->abilityName(id)) == name && (d.target || w.size() > 2))
            {
                const auto [tid, tidx] = targetOf(2, true); // <t> by default; <bt>, <st> ... work too
                act.action(tid, tidx, 7, id);
                return true;
            }
        }
        return true;
    }
    const bool serverId = !name.empty() && std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; });
    if ((cmd == "/target" || cmd == "/ta") && act.setTarget &&
        ((name.size() > 2 && name.front() == '<' && name != "<bt>" && name != "<t>") || serverId))
    {
        // A target word (<p1>, <a12>, <pet>, <me>) or a server id (what Shorthand queues): that entity.
        act.setTarget(targetOf(1, false).first);
        return true;
    }
    if ((cmd == "/target" || cmd == "/ta") && act.setTarget)
    {
        // The nearest entity whose name starts with the argument (case-insensitive); <bt> = the nearest monster
        // claimed by us or our party.
        for (const auto& [dist, e] : d.nearby)
        {
            const bool battle = name == "<bt>" && e->isMob && e->claimId != 0 &&
                                (e->claimId == world.charId ||
                                 std::any_of(world.party.begin(), world.party.end(), [&](const client::PartyMember& m) { return m.id == e->claimId; }));
            if (battle || (!name.empty() && name != "<bt>" && lower(e->name).rfind(name, 0) == 0))
            {
                act.setTarget(e->id);
                break;
            }
        }
        return true;
    }
    if (cmd == "/map")
    {
        openByName("map");
        return true;
    }
    if (cmd == "/heal" && act.send)
    {
        // C2S 0x0E8 CAMP {Mode u32: 0 toggle, 1 on, 2 off}: "/heal", "/heal on", "/heal off".
        const uint32_t mode = name == "on" ? 1 : name == "off" ? 2 : 0;
        std::vector<std::byte> pkt(4);
        std::memcpy(pkt.data(), &mode, 4);
        act.send(0x0E8, std::move(pkt));
        return true;
    }
    if (cmd == "/attack" || cmd == "/a")
    {
        // Engage the target (or the one named: <t>, <bt>, a sub-target pick). Engaged: another monster becomes the
        // battle target (ChangeTarget, category 0x0F; Switch Target's pick lands here), the same one disengages.
        if (w.size() > 1 || d.target)
        {
            const auto [id, index] = targetOf(1, true);
            const bool picked      = w.size() > 1 && lower(w[1]).starts_with("<id:");
            if (id == world.charId || (picked && id != static_cast<uint32_t>(std::strtoul(w[1].c_str() + 4, nullptr, 16))))
            {
                return true; // nobody to attack (or the pick has gone)
            }
            if (!world.engaged())
            {
                act.action(id, index, 2, 0);
            }
            else if (d.battleTarget != 0 && id != d.battleTarget)
            {
                if (act.setTarget)
                {
                    act.setTarget(id);
                }
                act.action(id, index, 0x0F, 0);
            }
            else
            {
                act.action(id, index, 4, 0);
            }
        }
        return true;
    }
    if ((cmd == "/item" || cmd == "/equip") && d.items)
    {
        // /equip <slot> "Item": slot names as the game's (main, sub, range, ammo, head, neck, ear1, ear2, body, hands,
        // ring1, ring2, back, waist, legs, feet).
        static const std::pair<const char*, uint8_t> kSlots[] = { { "main", 0 },  { "sub", 1 },    { "range", 2 }, { "ranged", 2 }, { "ammo", 3 },
                                                                  { "head", 4 },  { "body", 5 },   { "hands", 6 }, { "legs", 7 },   { "feet", 8 },
                                                                  { "neck", 9 },  { "waist", 10 }, { "ear1", 11 }, { "lear", 11 },  { "ear2", 12 },
                                                                  { "rear", 12 }, { "ring1", 13 }, { "lring", 13 }, { "ring2", 14 }, { "rring", 14 },
                                                                  { "back", 15 } };
        const bool        equip    = cmd == "/equip";
        int               slot     = -1;
        const std::string itemName = equip ? (w.size() > 2 ? lower(w[2]) : std::string()) : name;
        if (equip)
        {
            for (const auto& [n, s] : kSlots)
            {
                if (name == n)
                {
                    slot = s;
                }
            }
            if (slot < 0)
            {
                return true;
            }
            // CODE 0x1007b2c0: no item takes the slot's gear off (55648 #68 "Equipment removed."); an item is looked
            // for in the inventory, then the Mog Wardrobes, and equipped through the menus' routine (#67 "Equipment
            // changed."). The lines are not printed while a macro runs (INFERRED for this client: always printed).
            if (itemName.empty())
            {
                if (equipFromMenu(d, act, 0, 0, static_cast<uint8_t>(slot)) && act.note)
                {
                    act.note("Equipment removed.");
                }
                return true;
            }
            for (const uint8_t bag : kEquipBags)
            {
                for (const auto& it : world.inventory)
                {
                    if (it.container != bag || it.slot == 0)
                    {
                        continue;
                    }
                    const auto info = d.items->find(it.itemId);
                    if (info && lower(info->name) == itemName)
                    {
                        if (equipFromMenu(d, act, it.container, it.slot, static_cast<uint8_t>(slot)) && act.note)
                        {
                            act.note("Equipment changed.");
                        }
                        return true;
                    }
                }
            }
            return true;
        }
        for (const auto& it : world.inventory)
        {
            if (it.container != 0 || it.slot == 0)
            {
                continue;
            }
            const auto info = d.items->find(it.itemId);
            if (!info || lower(info->name) != itemName)
            {
                continue;
            }
            if (act.useItem)
            {
                const auto [tid, tidx] = targetOf(2, false);
                act.useItem(it.container, it.slot, tid, tidx);
            }
            return true;
        }
        return true;
    }
    return false;
}

void GameUi::submitLine(const std::string& typed, const GameUiData& d, const GameUiActions& act, int mode)
{
    std::string line = typed;
    if (act.intercept && act.intercept(line, mode))
    {
        return; // an Ashita plugin or addon took it (src/ashita); a plugin may also have rewritten the line
    }
    static const std::pair<const char*, uint8_t> kPrefixes[] = { { "/say ", 0 },   { "/s ", 0 }, { "/shout ", 1 }, { "/sh ", 1 },
                                                                 { "/party ", 4 }, { "/p ", 4 }, { "/yell ", 26 }, { "/y ", 26 },
                                                                 { "/linkshell2 ", 27 }, { "/l2 ", 27 }, { "/linkshell ", 5 }, { "/l ", 5 } };
    // A command's auto-translate tokens read as their text ("/ma {Cure} <t>"); chat lines keep them as FD codes.
    const bool chatLine = line.empty() || line[0] != '/' || line.rfind("/tell ", 0) == 0 || line.rfind("/t ", 0) == 0 ||
                          line.rfind("/emote ", 0) == 0 || line.rfind("/em ", 0) == 0 ||
                          std::ranges::any_of(kPrefixes, [&](const auto& p) { return line.rfind(p.first, 0) == 0; });
    const auto* at = text_ ? text_->autoTranslate() : nullptr;
    if (!chatLine && at && line.find('\xFD') != std::string::npos)
    {
        std::string plain;
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            if (const auto t = dat::AutoTranslate::tokenAt(line, i))
            {
                plain += at->tokenText(*t);
                i += 5;
                continue;
            }
            plain += line[i];
        }
        submitLine(plain, d, act, mode);
        return;
    }
    if (command(line, d, act))
    {
        return;
    }
    // Chat channel prefixes, as in the game (C2S 0x0B5 kinds): say, shout, party, yell.
    std::string text = line;
    uint8_t     kind = chatKind_;
    for (const auto& [prefix, k] : kPrefixes)
    {
        if (text.rfind(prefix, 0) == 0)
        {
            kind = k;
            text = text.substr(std::strlen(prefix));
            break;
        }
    }
    if (!text.empty() && act.say)
    {
        act.say(text, kind);
    }
}

namespace
{
constexpr std::pair<const char*, uint8_t> kEquipSlots[] = { { "main", 0 },  { "sub", 1 },    { "range", 2 }, { "ranged", 2 }, { "ammo", 3 },
                                                            { "head", 4 },  { "body", 5 },   { "hands", 6 }, { "legs", 7 },   { "feet", 8 },
                                                            { "neck", 9 },  { "waist", 10 }, { "ear1", 11 }, { "lear", 11 },  { "ear2", 12 },
                                                            { "rear", 12 }, { "ring1", 13 }, { "lring", 13 }, { "ring2", 14 }, { "rring", 14 },
                                                            { "back", 15 } };
const char* equipSlotName(uint8_t s)
{
    static const char* names[16] = { "main", "sub", "range", "ammo", "head", "body", "hands", "legs", "feet", "neck", "waist", "ear1", "ear2", "ring1", "ring2", "back" };
    return s < 16 ? names[s] : "?";
}
} // namespace

void GameUi::loadEquipSets(const std::filesystem::path& file)
{
    equipSets_.clear();
    equipSetFile_ = file;
    std::ifstream in(file);
    std::string   line;
    EquipSet*     cur = nullptr;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == ';' || line[0] == '#')
        {
            continue;
        }
        if (line[0] == '[')
        {
            const auto close = line.find(']');
            const int  n     = std::atoi(line.c_str() + 1);
            cur              = n >= 1 && n <= 100 ? &equipSets_[n] : nullptr;
            if (cur)
            {
                *cur      = {};
                cur->name = close == std::string::npos ? std::string() : line.substr(close + 1);
                cur->name.erase(0, cur->name.find_first_not_of(' ') == std::string::npos ? cur->name.size() : cur->name.find_first_not_of(' '));
            }
            continue;
        }
        const auto sp = line.find(' ');
        if (!cur || sp == std::string::npos)
        {
            continue;
        }
        const auto slot = lower(line.substr(0, sp));
        for (const auto& [n, s] : kEquipSlots)
        {
            if (slot == n)
            {
                cur->items.emplace_back(s, lower(line.substr(line.find_first_not_of(' ', sp))));
            }
        }
    }
}

void GameUi::saveEquipSets() const
{
    if (equipSetFile_.empty())
    {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(equipSetFile_.parent_path(), ec);
    std::ofstream out(equipSetFile_);
    out << "; Equipment sets: [N] name, then \"slot item\" lines (/equipset N, /lockstyleset N).\n";
    for (const auto& [n, set] : equipSets_)
    {
        out << '[' << n << "] " << set.name << '\n';
        for (const auto& [slot, item] : set.items)
        {
            out << equipSlotName(slot) << ' ' << item << '\n';
        }
    }
}

void GameUi::flipMainPage()
{
    if (stack_.empty() || (stack_.back().kind != Kind::Main && stack_.back().kind != Kind::Social))
    {
        return;
    }
    auto&      m    = stack_.back();
    const bool to2  = m.kind == Kind::Main;
    int        next = 1;
    for (const auto& [a, b] : kMainPageRows)
    {
        if ((to2 ? a : b) == m.cursor)
        {
            next = to2 ? b : a;
        }
    }
    m.kind   = to2 ? Kind::Social : Kind::Main;
    m.cursor = next;
}

void GameUi::eventMap(int zone, int map)
{
    eventMapZone_ = zone;
    eventMapId_   = map;
    // Op 0x89 opens map -1: in a zone the map under the player (resolved when drawn); a picture (zone <= -100, the map
    // table's illustrations: -217 = picture 117, file 54295 + 117) has only map 0. Closing passes zone 0.
    eventMapUnderPlayer_ = map < 0 && zone > 0;
    if (map < 0 && zone < 0)
    {
        eventMapId_ = 0;
    }
    eventViewFrom_ = eventViewTo_ = {};
    eventViewSeconds_             = 0;
    eventMapTexture_.reset();
    eventMapMarkers_.clear();
}

void GameUi::eventMapMarker(int slot, float x, float z, std::string label)
{
    // The op names the marker with 16 bytes, '_' standing for a space. Retail keeps it as event marker `slot` of the
    // map under the player (page 1 of the Markers list, saved with the map's MRK file): stored once that map is known.
    std::replace(label.begin(), label.end(), '_', ' ');
    std::erase_if(eventMapMarkers_, [slot](const EventMapMarker& m) { return m.slot == slot; });
    eventMapMarkers_.push_back({ x, z, label, slot });
    if (slot >= 0 && slot < 10)
    {
        pendingEventMarkers_.push_back({ eventMapZone_, slot, x, z, std::move(label) });
    }
}

MapMarkerSlots& GameUi::markerSlots(int zone, int mapId) const
{
    auto [it, fresh] = markerSlots_.try_emplace({ zone, mapId });
    if (fresh && !macroDir_.empty())
    {
        std::ifstream          in(macroDir_ / mapMarkerFileName(zone, mapId), std::ios::binary);
        std::vector<std::byte> b;
        for (char ch; in.get(ch);)
        {
            b.push_back(static_cast<std::byte>(ch));
        }
        if (auto slots = parseMapMarkers(b))
        {
            it->second = std::move(*slots);
        }
    }
    return it->second;
}

void GameUi::saveMarkerSlots(int zone, int mapId) const
{
    // Retail writes the file when the map closes after a change (CODE 0x101fa002); here at once.
    if (macroDir_.empty())
    {
        return;
    }
    const auto      bytes = serializeMapMarkers(markerSlots(zone, mapId));
    std::error_code ec;
    std::filesystem::create_directories(macroDir_, ec);
    std::ofstream(macroDir_ / mapMarkerFileName(zone, mapId), std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool GameUi::haveMap(const GameUiData& d) const
{
    const dat::MapRecord* rec  = d.maps ? d.maps->find(mapZone_, mapId_) : nullptr;
    const auto            need = rec ? rec->requiredKeyItem() : 0;
    return rec && (need == 0 || allMaps.map || allMaps.view || std::binary_search(d.world->keyItems.begin(), d.world->keyItems.end(), need));
}

void GameUi::openByName(std::string_view name)
{
    if (name != "main") open(Kind::Main);
    if (name == "main") open(Kind::Main);
    else if (name == "status") open(Kind::Status);
    else if (name == "equip") openEquipment();
    else if (name == "items") open(Kind::Items);
    else if (name == "magic") open(Kind::MagicMenu);
    else if (name == "abilities") open(Kind::Abilities);
    else if (name == "map") open(Kind::Map);
    else if (name == "config") open(Kind::Config);
    else if (name == "search")
    {
        searchQuery_ = {}; // every area (the menu's own open limits it to the current one)
        open(Kind::SearchMenu);
    }
    else if (name == "weaponskills")
    {
        open(Kind::Abilities);
        open(Kind::AbilityList, 1); // the Weapon Skills row's list (shape 160)
    }
    else if (name == "petcommands")
    {
        open(Kind::Abilities);
        open(Kind::AbilityList, 3); // the Pet Commands row's list (shape 163)
    }
    else stack_.clear();
}

std::optional<GameUi::SubRange> GameUi::subTargetRange(const client::WorldSnapshot& w, const dat::BattleText* text, const dat::SpellTable* spells,
                                                       const dat::AbilityTable* abilities) const
{
    if (!sub_.active)
    {
        return std::nullopt;
    }
    // Spells: {1, range (mgc_ +0x45), 0, 1} (CODE 0x1022b3b0); job abilities and weapon skills: {1, range (comm +0x10),
    // 0, 1} (0x10222f40, 0x102232c0). Kind 1 measures us -> the sub-target; byte 2 = 0 drops y.
    const auto wd   = words(sub_.line);
    const auto cmd  = wd.empty() ? std::string() : lower(wd[0]);
    const auto name = wd.size() > 1 ? lower(wd[1]) : std::string();
    auto found = [](int yalms) -> std::optional<SubRange> {
        return yalms >= 0 ? std::optional<SubRange>(SubRange{ yalms, true }) : std::nullopt;
    };
    if (text && (cmd == "/ma" || cmd == "/magic" || cmd == "/ninjutsu" || cmd == "/nin" || cmd == "/song" || cmd == "/so"))
    {
        for (const auto id : w.spells)
        {
            const auto* sp = spells ? spells->find(id) : nullptr;
            if (sp && lower(text->spellName(id)) == name)
            {
                return found(sp->range);
            }
        }
    }
    else if (text && abilities && (cmd == "/ja" || cmd == "/jobability" || cmd == "/pet"))
    {
        for (const auto id : w.jobAbilities)
        {
            if (lower(text->abilityName(512u + id)) == name)
            {
                return found(abilities->commandRange(512u + id));
            }
        }
    }
    else if (text && abilities && (cmd == "/ws" || cmd == "/weaponskill"))
    {
        for (const auto id : w.weaponSkills)
        {
            if (lower(text->abilityName(id)) == name)
            {
                return found(abilities->commandRange(id));
            }
        }
    }
    else
    {
        return SubRange{};
    }
    return std::nullopt;
}

void GameUi::openShortcut(std::string_view name)
{
    // The game's handlers open the window by itself (CODE 0x101ae3b0 equipment, 0x101ae3f0 inventory, 0x101ae5d0
    // magic 0x1022ad80(0, 1, 0x100): the spell list of every type but trust (bit 8 of the exclude mask), 0x101ae560 /
    // 0x101ae580 / 0x101ae5b0 abilities 0x102226f0(type 1 / 3 / 2): "menu ability" as the Abilities rows open it).
    stack_.clear();
    if (name == "items") open(Kind::Items);
    else if (name == "equip") openEquipment();
    else if (name == "magic") open(Kind::Magic, 0);
    else if (name == "abilities") open(Kind::AbilityList, 0);
    else if (name == "weaponskills") open(Kind::AbilityList, 1);
    else if (name == "petcommands") open(Kind::AbilityList, 3);
}

// --- menus ---

const char* GameUi::layoutName(Kind k) const
{
    switch (k)
    {
        case Kind::Main: return "menu menuwind";
        case Kind::Social: return "menu socialme";
        case Kind::LogoutConfirm: return "menu comyn";
        case Kind::Status: return "menu statcom";
        case Kind::Equip: return "menu equip";
        case Kind::EquipList: return "menu inventor";
        case Kind::Items: return "menu inventor";
        case Kind::ItemSort: return "menu itmsortw";
        case Kind::SortConfirm: return "menu sortyn";
        case Kind::ItemBags: return "menu storage";
        case Kind::ItemUse: return "menu iuse";
        case Kind::MagicMenu: return "menu mgcmenu";
        case Kind::Magic: return "menu magic";
        case Kind::Abilities: return "menu abimenu";
        case Kind::AbilitySelect: return "menu abiselec";
        case Kind::MagicSelect: return "menu magselec";
        case Kind::ChatCtrl: return "menu chatctrl";
        case Kind::AbilityList: return "menu ability"; // as "menu magic", titled Abilities (CODE 0x102226f0)
        case Kind::DropConfirm: return "menu yesno";
        case Kind::DropAgain: return "menu comyn";
        case Kind::Trade: return "menu handover";
        case Kind::PlayerTrade: return "menu trade";
        case Kind::Map: return "menu map0";
        case Kind::MapMarkers: return "menu mapv3";
        case Kind::MapMarkerEdit: return "menu mapv2";
        case Kind::PartyPick: return "menu inventor";
        case Kind::TradeItem: return "menu inventor";
        case Kind::TradeQty: return "menu itemctrl";
        case Kind::TradeGil: return "menu moneyctr";
        case Kind::Party: return "menu prty1"; // see layoutFor()
        case Kind::Config: return "menu configwi";
        case Kind::ConfigWindows: return "menu conf5win";
        case Kind::ConfigMisc2: return "menu conf6win";
        case Kind::ConfigWindowsMenu: return "menu conf5m";
        case Kind::ConfigGameplay: return "menu conf2win";
        case Kind::ConfigMisc: return "menu conf3win";
        case Kind::ConfigMisc3: return "menu conf12wi";
        case Kind::ConfigMisc4: return "menu conf13wi";
        case Kind::ConfigCamera: return "menu conf7";
        case Kind::ConfigGlobal: return "menu conf4";
        case Kind::ChatFilter: return "menu cfilter";
        case Kind::EffectFilter: return "menu fxfilter";
        case Kind::ConfigLog1: return "menu conf5w1";
        case Kind::ConfigLog2: return "menu conf5w2";
        case Kind::FontColors: return "menu conftxtc";
        case Kind::FontColorList: return "menu textcol1";
        case Kind::FontColorEdit: return "menu textcol3";
        case Kind::ConfigReset: return "menu confyn";
        case Kind::KeyAssign: return "menu k1assign"; // see layout()
        case Kind::LogSettings: return "menu conf11m";
        case Kind::LogCategory: return "menu conf11l";
        case Kind::LogFilter: return "menu conf11s";
        case Kind::Revive: return "menu comyn";
        case Kind::KeyItems:
        case Kind::Currencies: return "menu evitem";
        case Kind::Quests: return "menu quset";
        case Kind::QuestList: return "menu quest01";
        case Kind::Missions: return "menu missionm";
        case Kind::MissionList: return "menu miss00";
        case Kind::SynthMenu: return "menu cmbmenu";
        case Kind::SynthCrystal: return "menu inventor";
        case Kind::SynthHistory: return "menu cmbhlst";
        case Kind::Synth: return "menu handover";
        case Kind::LinkshellPick: return "menu inventor";
        case Kind::Linkshell: return "menu link2";
        case Kind::Shop: return "menu shop";
        case Kind::ShopBuy: return "menu shopbuy";
        case Kind::EventMenu: return "menu query"; // op 0x24 (CODE 0x1015e9b0)
        case Kind::Storage:
        case Kind::StoragePut:
        case Kind::DeliveryBox:
        case Kind::DeliveryPick:
        case Kind::AucSellPick:
        case Kind::Conquest: return "menu inventor";
        case Kind::BattleMenu: return "menu battlemo"; // see layout()
        case Kind::Treasure: return "menu loot";
        case Kind::TreasureOp: return "menu lootope";
        case Kind::Auction: return "menu auc1";
        case Kind::AucGroups: return "menu auc2";
        case Kind::AucSub: return "menu aucweapo"; // see layout()
        case Kind::AucList:
        case Kind::AucSales: return "menu auclist";
        case Kind::AucItem: return "menu auc3";
        case Kind::AucHistory: return "menu auchisto";
        case Kind::AucSale: return "menu auc4";
        case Kind::AucYesNo: return "menu aucyn";
        case Kind::AucSort: return "menu wepsort"; // see layout()
        case Kind::ShopMain: return "menu shopmain";
        case Kind::SellPick: return "menu inventor";
        case Kind::ShopSell: return "menu shopsell";
        case Kind::GuildMain: return "menu guildsho";
        case Kind::GuildBuy:
        case Kind::GuildSell:
        case Kind::GuildSellPick: return "menu shop"; // the shop list window with the guild's rows (0x106319e8)
        case Kind::GuildBuyQty: return "menu shopbuy";
        case Kind::GuildSellQty: return "menu shopsell";
        case Kind::MogHouse: return "menu myroom";
        case Kind::RegionMenu: return "menu region";
        case Kind::SearchMenu: return "menu searchma";
        case Kind::SearchRefine: return "menu scsibori";
        case Kind::SearchJob: return "menu searchjo";
        case Kind::SearchNation: return "menu searchna";
        case Kind::SearchRace: return "menu searchra";
        case Kind::SearchRank: return "menu searchle";
        case Kind::CommentGenre: return "menu comgenre";
        case Kind::CommentSub: return "menu sccompar"; // see layout()
        case Kind::SearchResults: return "menu scresult";
        case Kind::SearchOption: return "menu scoption";
        case Kind::MuteOption: return "menu acoption";
        case Kind::Blacklist: return "menu blklist";
        case Kind::Communication: return "menu commenu";
        case Kind::EmoteList: return "menu emote";
        case Kind::BlacklistOption: return "menu blkmain";
        case Kind::SearchUnity:
        case Kind::SearchArea:
        case Kind::JobChange: return "menu jobchang";
        case Kind::MogStorage:
        case Kind::JobPick: return "menu inventor";
        case Kind::AppList:
        case Kind::AppElements: return ""; // named by the app (AppMenu::layout), see layout()
        case Kind::MacroMenu: return "menu mcrmenu";
        case Kind::MacroBooks: return "menu mcrselec";
        case Kind::MacroBookOp: return "menu mcrselop";
        case Kind::MacroEdit: return "menu mcr1edit"; // or mcr2edit / the long ones: macroLayoutName
        case Kind::MacroButtonOp: return "menu mcrbedit";
        case Kind::MacroEditor: return "menu macro";
        case Kind::MacroLineOp: return "menu mcrledit";
        case Kind::MacroConfirm: return "menu comyn";
        case Kind::EquipSets: return "menu mcres20";
        case Kind::EquipSetOp: return "menu mcresmn";
        case Kind::EquipSetEdit: return "menu mcresed";
    }
    return "";
}

const dat::UiLayout* GameUi::layout(const Menu& m) const
{
    if (isMacroKind(m.kind))
    {
        return macroLayout(m);
    }
    const auto& ls   = r_.resources().layouts;
    static const char* kAucSub[] = { "menu aucweapo", "menu aucarmor", "menu aucmagic", "menu aucmater",
                                     "menu aucfood",  "menu aucmeals", "menu aucammo",  "menu aucitem" };
    const char* name = m.kind == Kind::Party ? (m.param == 3 ? "menu prty3" : m.param == 2 ? "menu prtyjoin" : m.param == 1 ? "menu prty2" : "menu prty1")
                     : m.kind == Kind::AucSub ? kAucSub[std::clamp(m.param, 0, 7)]
                     : m.kind == Kind::AucSort ? (m.param ? "menu armsort" : "menu wepsort")
                     : m.kind == Kind::BattleMenu ? (m.param ? "menu attackmo" : "menu battlemo")
                     : m.kind == Kind::KeyAssign  ? (m.param ? "menu k2assign" : "menu k1assign")
                     : m.kind == Kind::CommentSub ? (m.param == 4 ? "menu sccomite" : m.param == 5 ? "menu sccomls" : "menu sccompar")
                     : (m.kind == Kind::AppList || m.kind == Kind::AppElements) ? appMenus_.at(static_cast<std::size_t>(m.param)).layout.c_str()
                                              : layoutName(m.kind);
    const auto  it   = ls.find(name);
    if (it == ls.end())
    {
        return nullptr;
    }
    if (m.kind == Kind::SortConfirm || m.kind == Kind::DropConfirm)
    {
        // The yes / no dialog puts its title, 55652 #2 "Items", in the title window (0x102222d0 -> 0x10150bf0).
        auto& l        = stacked_[m.kind == Kind::SortConfirm ? "sortyn" : "dropyn"];
        l              = it->second;
        l.window.text2 = m.kind == Kind::SortConfirm ? kSortConfirmTitle : kDropConfirmTitle;
        return &l;
    }
    if (m.kind == Kind::MapMarkerEdit)
    {
        // "menu mapv2" opens beside the Markers list's selected row: its right edge on the row's left (CODE 0x101fbed0:
        // x = row x - width, y = row y, from 0x101fb8e0 / 0x10119040). Element 2 reads "Delete" for a placed marker:
        // the window's second label, keytops3 35 instead of 29 (0x101fc070 draws [this + 0x18 + present * 4]).
        auto& l = stacked_["mapv2"];
        l       = it->second;
        const auto list = std::find_if(stack_.rbegin(), stack_.rend(), [](const Menu& s) { return s.kind == Kind::MapMarkers; });
        const auto v3   = ls.find("menu mapv3");
        if (list != stack_.rend() && v3 != ls.end())
        {
            if (const auto* row = v3->second.element(static_cast<uint16_t>(list->cursor)))
            {
                l.window.x      = static_cast<int16_t>(v3->second.window.x + row->x - l.window.w);
                l.window.y      = static_cast<int16_t>(v3->second.window.y + row->y);
                l.window.anchor = v3->second.window.anchor;
            }
        }
        if (markerSlots(mapZone_, mapId_)[static_cast<std::size_t>(m.param)].present)
        {
            for (auto& e : l.elements)
            {
                for (auto& sb : e.subs)
                {
                    if (e.id == 2 && sb.shape == 29)
                    {
                        sb.shape = 35;
                    }
                }
            }
        }
        return &l;
    }
    if (m.kind == Kind::Config)
    {
        // "menu configwi" plus this client's "Add-ons" row (id 15) under Gamepad (13), one row (16) lower; the window
        // grows by a row and the wrap-around goes through it (goal 47).
        auto& l = stacked_["configwi+addons"];
        if (l.elements.empty())
        {
            l = it->second;
            if (const auto* gp = it->second.element(13))
            {
                dat::UiRecord e = *gp;
                e.id    = 15;
                e.y     = static_cast<int16_t>(gp->y + 16);
                e.subs.clear();
                e.text1 = -1;
                e.nav   = { 13, 1, 13, 1, 15, 15 };
                l.window.h = static_cast<int16_t>(l.window.h + 16);
                for (auto& el : l.elements)
                {
                    if (el.id == 13)
                    {
                        el.nav[1] = el.nav[3] = 15;
                    }
                    if (el.id == 1)
                    {
                        el.nav[0] = el.nav[2] = 15;
                    }
                }
                l.elements.push_back(std::move(e));
            }
        }
        return &l;
    }
    // Menu registry flag 0x1000 (table 0x103722d8 +0x24 -> menu +0x34; placed by 0x1015e4e1 -> 0x1015f6a0): the window
    // sits 2 above the log window's top (group 0, "menu logwindo"), at least 16 from the top; x as authored. "menu
    // battlemo" / "attackmo" are the playermo object's other layouts.
    static const std::unordered_set<std::string_view> kAboveLog = {
        "menu query",    "menu gmtell",   "menu playermo", "menu battlemo", "menu attackmo", "menu chatctrl", "menu magselec",
        "menu abiselec", "menu myroom",   "menu rmpost",   "menu mogdoor",  "menu mogext",   "menu mogpost",  "menu roomlist",
        "menu storage",  "menu storage2", "menu jobchang", "menu jobcsel",  "menu jobcselu", "menu arealist", "menu shopmain",
        "menu guildsho", "menu merityn",  "menu maplist",  "menu comyn",    "menu resyn",    "menu resynus",  "menu resynde",
        "menu resynfr",  "menu magic",    "menu ability",  "menu emote",    "menu mount",    "menu rmlo1",    "menu rmlo2",
        "menu rmlo4",    "menu plnt1",    "menu plnt2",    "menu plnt3",    "menu plnt4",    "menu plnt5",    "menu plnt6",
        "menu mp_pmode", "menu mp_abisc" };
    // The battle menu: the window of "menu actionm<n>" (n entries; table 0x1036f408, CODE 0x1012b592), the rows of
    // "menu playermo" (x 21, y 5 + 16 a row) without their own labels; the title by mode (55652 #5 Commands, #6 Ready
    // Mode, #7 Battle Mode; CODE 0x1012b541); each row's help from its code. Stacked above the log (playermo's
    // registry flag 0x1000).
    if (m.kind == Kind::BattleMenu)
    {
        const std::size_t n  = std::clamp<std::size_t>(battleCodes_.size(), 1, 10);
        const auto        aw = ls.find(n == 10 ? std::string("menu actiom10") : fmt::format("menu actionm{}", n));
        const auto        pm = ls.find("menu playermo");
        if (aw == ls.end() || pm == ls.end())
        {
            return nullptr;
        }
        auto& l  = stacked_["battlemenu"];
        l        = aw->second;
        l.window.text2 = m.param == 6 ? 7 : m.param == 4 ? 6 : 5;
        l.elements.clear();
        for (std::size_t i = 0; i < battleCodes_.size() && i < pm->second.elements.size(); ++i)
        {
            auto e  = pm->second.elements[i];
            const auto id = static_cast<uint8_t>(i + 1);
            const auto last = static_cast<uint8_t>(battleCodes_.size());
            e.id    = id;
            e.subs.clear();
            e.text1 = kBattleCodes[battleCodes_[i]].help;
            e.nav   = { static_cast<uint8_t>(std::max<int>(1, id - 1)), std::min(last, static_cast<uint8_t>(id + 1)),
                        static_cast<uint8_t>(std::max<int>(1, id - 1)), std::min(last, static_cast<uint8_t>(id + 1)),
                        std::min(last, static_cast<uint8_t>(id + 1)), static_cast<uint8_t>(std::max<int>(1, id - 1)) };
            l.elements.push_back(std::move(e));
        }
        if (logTop_ > 0)
        {
            const UiCanvas c{ lastCanvas_.width, lastCanvas_.height, 1 };
            l.window.x      = static_cast<int16_t>(c.windowOrigin(aw->second.window).x);
            l.window.y      = static_cast<int16_t>(std::max(16.0f, logTop_ - static_cast<float>(l.window.h) - 2.0f));
            l.window.anchor = 0;
        }
        return &l;
    }
    // Under the Equipment grid the item list sits 2 right of it, top-aligned (retail captures; INFERRED placement).
    if (m.kind == Kind::EquipList)
    {
        if (const auto eq = ls.find("menu equip"); eq != ls.end())
        {
            const UiCanvas c{ lastCanvas_.width, lastCanvas_.height, 1 };
            auto [st, fresh] = stacked_.try_emplace("equiplist", it->second);
            const auto o     = c.windowOrigin(eq->second.window);
            st->second.window.x      = static_cast<int16_t>(o.x + static_cast<float>(eq->second.window.w) + 2);
            st->second.window.y      = static_cast<int16_t>(o.y);
            st->second.window.anchor = 0;
            return &st->second;
        }
    }
    if (logTop_ <= 0 || !kAboveLog.contains(std::string_view(name)))
    {
        return &it->second;
    }
    if (stackedLogTop_ != logTop_)
    {
        stacked_.clear();
        stackedLogTop_ = logTop_;
    }
    auto [st, fresh] = stacked_.try_emplace(name, it->second);
    if (fresh)
    {
        const UiCanvas c{ lastCanvas_.width, lastCanvas_.height, 1 };
        st->second.window.x      = static_cast<int16_t>(c.windowOrigin(it->second.window).x);
        st->second.window.y      = static_cast<int16_t>(std::max(16.0f, logTop_ - static_cast<float>(it->second.window.h) - 2.0f));
        st->second.window.anchor = 0;
    }
    return &st->second;
}

bool GameUi::isList(Kind k) const
{
    return k == Kind::EquipList || k == Kind::Items || k == Kind::Magic || k == Kind::AbilityList || k == Kind::TradeItem || k == Kind::PartyPick ||
           k == Kind::KeyItems || k == Kind::Currencies || k == Kind::Quests || k == Kind::QuestList || k == Kind::MissionList ||
           k == Kind::SynthCrystal || k == Kind::SynthHistory || k == Kind::LinkshellPick || k == Kind::Shop ||
           k == Kind::EventMenu || k == Kind::SellPick || k == Kind::Storage || k == Kind::GuildBuy || k == Kind::GuildSell || k == Kind::GuildSellPick ||
           k == Kind::StoragePut || k == Kind::DeliveryBox || k == Kind::DeliveryPick || k == Kind::AucList ||
           k == Kind::AucHistory || k == Kind::AucSales || k == Kind::AucSellPick || k == Kind::Conquest ||
           k == Kind::Treasure || k == Kind::MogStorage || k == Kind::JobPick || k == Kind::SearchArea || k == Kind::SearchResults ||
           k == Kind::SearchUnity || k == Kind::Blacklist || k == Kind::EmoteList || k == Kind::AppList || k == Kind::ChatFilter || k == Kind::EffectFilter || k == Kind::FontColorList || k == Kind::LogFilter || k == Kind::KeyAssign || k == Kind::MapMarkers;
}

bool GameUi::implemented(const Menu& m, const dat::UiRecord& e) const
{
    if (m.kind == Kind::SearchMenu)
    {
        // Area, Name, Job, Country, Race, Level, Rank, Comment, Edit Comment, Unity, Request; Linkshell / Linkshell 2
        // while one is worn (CODE 0x1020ce00 dims them otherwise). Friend: the game dims it while the PlayOnline friend
        // list is empty (0x100e6a40), which it always is here. Master Level (type 7 entry): LSB has no master levels and
        // does not parse the entry. Ballista and Monstrosity: not done.
        return e.id <= 7 || e.id == 11 || e.id == 12 || e.id == 14 || e.id == 16 || (e.id == 9 && linkshellId(0)) ||
               (e.id == 15 && linkshellId(1));
    }
    if (m.kind == Kind::SearchRefine)
    {
        // The same rows (CODE jump table 0x1020cdb8): 11 = Comment, 9 / 13 = Linkshell / Linkshell 2, 12 Unity, 14 Request.
        return e.id <= 7 || e.id == 11 || e.id == 12 || e.id == 14 || (e.id == 9 && linkshellId(0)) || (e.id == 13 && linkshellId(1));
    }
    if (m.kind == Kind::RegionMenu)
    {
        return e.id <= 2; // Conquest, Besieged
    }
    if (m.kind == Kind::ItemUse && e.id == 1)
    {
        // Use: items the menu can use (flag 0x0400; scrolls, food, medicine ...) and enchanted gear (charges).
        const auto c = static_cast<uint8_t>(m.param >> 8), sl = static_cast<uint8_t>(m.param & 0xFF);
        for (const auto& it : data_->world->inventory)
        {
            if (it.container == c && it.slot == sl && data_->items)
            {
                const auto info = data_->items->find(it.itemId);
                if (!info)
                {
                    return false;
                }
                return info->equipment() ? info->maxCharges > 0 : info->usable() || (info->flags & 0x0400) != 0;
            }
        }
        return false;
    }
    if (m.kind == Kind::ItemSort)
    {
        return e.id <= 3; // Auto-sort, Manual Sort, Switch
    }
    if (m.kind == Kind::ItemBags)
    {
        return e.id < 16 && bagReachable(*data_->world, kBagOfElement[e.id]);
    }
    if (m.kind == Kind::Status)
    {
        // Profile .. Crafting Skills, and Currencies (6): CODE 0x10148c91 opens "menu evitem" in mode 1 through the object
        // at 0x106346b4, which exists once the quest / mission file (82) has loaded (0x10222020).
        return e.id <= 6;
    }
    if (m.kind == Kind::SearchOption)
    {
        return e.id == 1 || e.id == 3; // Tell, Invite
    }
    if (m.kind == Kind::BlacklistOption)
    {
        return e.id == 1; // Delete from blacklist
    }
    if (m.kind == Kind::MuteOption)
    {
        // "Remove from the Mute list": mentors only (S2C 0x037 MentorFlag); others get 55651 #1052 "You do not have the
        // ability...". LSB further checks mastery rank 6 (c2s 0x0b7_assist_channel.cpp).
        return data_ && data_->world->mentor;
    }
    if (m.kind == Kind::MogHouse)
    {
        // "menu myroom" elements 1 Storage, 2 Delivery Box, 3 Change Jobs, 4 Gardening, 5 Layout, 6 Open / Close Mog
        // House, 7 Remodel. CODE 0x10145170: while the house is open to visitors, Layout and Remodel are dimmed (LSB
        // refuses a remodel then); Remodel also wants the 2F (0x100f96d0, INFERRED: the MogExpansionFlag).
        const auto& w = *data_->world;
        if (e.id == 5)
        {
            return !w.mhOpen;
        }
        if (e.id == 7)
        {
            return !w.mhOpen && w.mhExpansion;
        }
        return e.id >= 1 && e.id <= 7;
    }
    if (isMacroKind(m.kind))
    {
        return macroImplemented(m, e);
    }
    if (m.kind == Kind::AppElements)
    {
        const auto& dim = appMenus_.at(static_cast<std::size_t>(m.param)).dimmed;
        return std::find(dim.begin(), dim.end(), static_cast<int>(e.id)) == dim.end();
    }
    if (m.kind == Kind::Main)
    {
        if (e.id >= 13)
        {
            return true; // page arrows
        }
        const auto* sub = e.subFor(0);
        const int   lbl = sub ? sub->shape : -1;
        if (lbl == kLabelTrade)
        {
            return data_ && data_->target; // NPCs (handover) and players
        }
        return lbl == kLabelStatus || lbl == kLabelEquipment || lbl == kLabelMagic || lbl == kLabelItems || lbl == kLabelAbilities || lbl == kLabelParty ||
               lbl == kLabelMap || lbl == kLabelSynthesis || lbl == kLabelLinkshell || lbl == kLabelSearch || lbl == kLabelRegion;
    }
    if (m.kind == Kind::Linkshell)
    {
        return e.text1 == kHelpLinkActivate;
    }
    if (m.kind == Kind::SynthMenu)
    {
        return e.text1 == kHelpSynthesize || (e.text1 == kHelpSynthHistory && !synthHistory_.empty());
    }
    if (m.kind == Kind::Party)
    {
        return e.text1 == kHelpLeaveParty || e.text1 == kHelpAcceptInvite || e.text1 == kHelpDeclineInvite || e.text1 == kHelpDisband ||
               e.text1 == kHelpChangeLeader || e.text1 == kHelpRemoveMember ||
               (e.text1 == kHelpAddMember && data_ && data_->target && data_->target->isPc);
    }
    if (m.kind == Kind::Social)
    {
        return e.id >= 13 || e.text1 == kHelpMacros || e.text1 == kHelpCommunication || e.text1 == kHelpCurrentTime || e.text1 == kHelpLogOut || e.text1 == kHelpShutDown || e.text1 == kHelpConfig || e.text1 == kHelpKeyItems || e.text1 == kHelpMissions ||
               e.text1 == kHelpQuests;
    }
    if (m.kind == Kind::Missions)
    {
        return e.id == 1 || e.id == 2; // your nation's missions, Rise of the Zilart
    }
    if (m.kind == Kind::Communication)
    {
        // Friend List (1) is PlayOnline's friend list (polcore, ui-format-notes §7.5): LSB has none, so it stays dimmed.
        return e.id == 2 && data_ && data_->emotes && !data_->emotes->entries().empty();
    }
    if (m.kind == Kind::Config)
    {
        // Every page but Gamepad (it starts FFXiPadConfig.exe; CODE jump table 0x10196444); 15 is this client's Add-ons.
        return (e.id >= 1 && e.id <= 12) || e.id == 15;
    }
    if (m.kind == Kind::ConfigWindowsMenu)
    {
        return e.id >= 1 && e.id <= 3; // shared settings, log window 1, log window 2
    }
    if (m.kind == Kind::ConfigMisc2 && e.id >= 8 && e.id <= 10)
    {
        return true; // the key-assignment lists and the reset (CODE 0x10196a3e..0x10196aa3)
    }
    if (isConfigPage(m.kind))
    {
        // Gamma R / G / B (cfg 55..57): the game greys them without a fullscreen gamma ramp (CODE 0x10002be0); here a
        // last full-screen pass stands in for the ramp (gfx::GammaPass), so they always work.
        const auto* b = cfgBinding(m.kind, e.id);
        if (!b)
        {
            return false;
        }
        // The chat filter's term / player modes need a prohibited.txt with entries (the game: 0x101bca80 > 0).
        if (b->id == 206 && b->value != 0)
        {
            return data_ && data_->chatFilterLists;
        }
        // Dimmed because nothing here can act on them: the movement reference (no mouse-driven movement) and the
        // software keyboard (a PlayOnline on-screen keyboard for pads; SDL text input has its own).
        if (b->id == 144 || b->id == 201)
        {
            return false;
        }
        return true;
    }
    if (m.kind == Kind::FontColors || m.kind == Kind::FontColorEdit || m.kind == Kind::ConfigReset || m.kind == Kind::LogSettings ||
        m.kind == Kind::LogCategory)
    {
        return e.nav[0] != 0xFF;
    }
    if (m.kind == Kind::Map)
    {
        // "Markers" (308) and "Change Map" (690, the zone's other maps); wide scan (309) is not done yet.
        return e.text1 == 690 || e.text1 == 308;
    }
    if (m.kind == Kind::MapMarkerEdit)
    {
        // "menu mapv2" (CODE 0x101fbed0 / 0x101fbfb0): 1 centre on the marker (dimmed while the slot is empty), 2 place /
        // delete, 3 rename.
        return e.id == 2 || e.id == 3 || (e.id == 1 && markerSlots(mapZone_, mapId_)[static_cast<std::size_t>(m.param)].present);
    }
    if (m.kind == Kind::MagicMenu || m.kind == Kind::MagicSelect)
    {
        const int type = magicCategory(e);
        if (type == 0 || !data_ || !data_->spells)
        {
            return false;
        }
        const auto& w = *data_->world;
        for (const auto id : w.spells)
        {
            const auto* sp = data_->spells->find(id);
            if (sp && sp->type == type && (sp->usableBy(w.mainJob, w.mainLevel) || sp->usableBy(w.subJob, w.subLevel)))
            {
                return true;
            }
        }
        return false;
    }
    if (m.kind == Kind::Abilities || m.kind == Kind::AbilitySelect)
    {
        const auto* sub = e.subFor(0);
        if (sub && sub->shape == 162)
        {
            // Ranged Attack: needs a ranged weapon (equip slot 2) and a monster target.
            return data_ && data_->world->equipment[2].second != 0 && data_->target && !data_->target->isPc;
        }
        if (sub && sub->shape == 163)
        {
            // Pet Commands: any learned job ability of type 2 (DATA: file 85 +0x02; Fight, Heel, Assault, Deploy, ...).
            for (const auto id : data_->world->jobAbilities)
            {
                const auto* ab = data_->abilities ? data_->abilities->find(id) : nullptr;
                if (ab && ab->type == 2)
                {
                    return true;
                }
            }
            return false;
        }
        return sub && (sub->shape == 160 || sub->shape == 161 || sub->shape == 164);
    }
    return true;
}

void GameUi::sendSynthesis(const GameUiActions& act, const client::WorldSnapshot& w)
{
    // OK: C2S 0x096 COMBINE_ASK {u8 HashNo, u8 pad, u16 Crystal @2, u8 CrystalIdx @4, u8 Items @5,
    // u16 ItemNo[8] @6, u8 TableNo[8] @0x16, u16 pad}. LSB does not check HashNo.
    std::vector<std::byte> pkt(0x20);
    std::memcpy(pkt.data() + 2, &synthCrystalId_, 2);
    pkt[4]    = static_cast<std::byte>(synthCrystalSlot_);
    uint8_t n = 0;
    for (const auto& [slot, count] : tradeBoxes_)
    {
        if (slot == 0)
        {
            continue;
        }
        uint16_t itemId = 0;
        for (const auto& it : w.inventory)
        {
            if (it.container == 0 && it.slot == slot)
            {
                itemId = it.itemId;
            }
        }
        std::memcpy(pkt.data() + 6 + 2 * n, &itemId, 2);
        pkt[0x16 + n] = static_cast<std::byte>(slot);
        ++n;
    }
    pkt[5] = static_cast<std::byte>(n);
    if (n > 0)
    {
        act.send(0x096, std::move(pkt));
        // Remember the recipe (item ids) for History, newest first, without duplicates, at most 10.
        SynthRecipe r;
        r.crystal     = synthCrystalId_;
        std::size_t k = 0;
        for (const auto& [slot, count] : tradeBoxes_)
        {
            for (const auto& it : w.inventory)
            {
                if (slot != 0 && it.container == 0 && it.slot == slot)
                {
                    r.items[k++] = it.itemId;
                }
            }
        }
        std::erase_if(synthHistory_, [&](const SynthRecipe& o) { return o.crystal == r.crystal && o.items == r.items; });
        synthHistory_.insert(synthHistory_.begin(), r);
        if (synthHistory_.size() > 10)
        {
            synthHistory_.pop_back();
        }
    }
}

bool GameUi::refillSynthesis(const SynthRecipe& r, const client::WorldSnapshot& w)
{
    // One item per box, each from a stack with enough left; false if something is missing.
    std::map<uint8_t, uint32_t> used;
    auto take = [&](uint16_t id) -> uint8_t {
        for (const auto& it : w.inventory)
        {
            if (it.container == 0 && it.slot != 0 && it.itemId == id && used[it.slot] < it.count)
            {
                ++used[it.slot];
                return it.slot;
            }
        }
        return 0;
    };
    const uint8_t                               crystalSlot = take(r.crystal);
    std::array<std::pair<uint8_t, uint32_t>, 8> boxes{};
    bool                                        ok = crystalSlot != 0;
    for (std::size_t b = 0; b < 8; ++b)
    {
        if (r.items[b] != 0)
        {
            const uint8_t slot = take(r.items[b]);
            ok &= slot != 0;
            boxes[b] = { slot, 1 };
        }
    }
    if (ok)
    {
        synthCrystalSlot_ = crystalSlot;
        synthCrystalId_   = r.crystal;
        tradeBoxes_       = boxes;
    }
    return ok;
}

namespace
{
// Font Colors rows per category (CODE tables 0x10379b50 / c30 / d10 / df0: {label, sample, colour, cfg}; texts in 55650).
struct FontRow
{
    uint16_t label, sample;
    int      cfg;
};
const std::vector<FontRow>& fontRows(int category)
{
    static const std::array<std::vector<FontRow>, 4> kRows = { {
        { { 36, 63, 27 }, { 38, 65, 29 }, { 39, 66, 30 }, { 40, 67, 31 }, { 41, 68, 179 }, { 42, 204, 203 }, { 43, 205, 204 }, { 44, 69, 176 },
          { 45, 70, 32 }, { 46, 71, 33 }, { 47, 72, 34 }, { 37, 64, 28 }, { 196, 197, 155 } },
        { { 48, 73, 35 }, { 49, 74, 36 }, { 50, 75, 37 }, { 51, 76, 38 }, { 52, 77, 39 }, { 53, 78, 40 } },
        { { 54, 79, 41 }, { 55, 80, 42 }, { 56, 81, 43 }, { 57, 82, 44 }, { 58, 83, 45 }, { 59, 84, 46 } },
        { { 60, 85, 47 }, { 61, 86, 48 }, { 62, 87, 49 } },
    } };
    return kRows[static_cast<std::size_t>(std::clamp(category, 0, 3))];
}
// Log Settings rows per category (CODE 0x10379ef8 / f68 / fd8 / 0x1037a048: {label, mask bit}).
const std::vector<std::pair<uint16_t, int>>& logRows(int category)
{
    static const std::array<std::vector<std::pair<uint16_t, int>>, 4> kRows = { {
        { { 36, 0 }, { 38, 1 }, { 39, 2 }, { 40, 3 }, { 41, 4 }, { 42, 12 }, { 43, 13 }, { 44, 6 }, { 45, 7 }, { 46, 8 }, { 47, 9 }, { 37, 10 }, { 196, 11 } },
        { { 48, 33 }, { 49, 34 }, { 50, 35 }, { 51, 36 }, { 52, 37 }, { 53, 38 } },
        { { 54, 39 }, { 55, 40 }, { 56, 41 }, { 57, 42 }, { 58, 43 }, { 59, 44 } },
        { { 202, 45 }, { 203, 46 }, { 62, 47 } },
    } };
    return kRows[static_cast<std::size_t>(std::clamp(category, 0, 3))];
}
// The key assignment as saved (cfg 74.. / 106.., DIK codes; missing ids = the defaults).
KeyAssign assignment(const GameUiData& d)
{
    KeyAssign k;
    if (d.gameConfig)
    {
        k.load(*d.gameConfig);
    }
    return k;
}
constexpr int kFontColourIds[] = { 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 155, 176, 179, 203, 204 };
} // namespace

const std::vector<GameUi::CfgBinding>& GameUi::cfgBindings()
{
    // Element -> (config id, value) per page (CODE handlers: conf2win 0x10197010, conf3win 0x1019a5b0, conf6win 0x1019ada0,
    // conf12wi 0x1019b560, conf13wi 0x1019b970, conf7 table 0x1037a0b8, conf4 0x1019bea0, conf5win 0x10198f00).
    auto c = [](Kind k, uint8_t el, uint16_t id, int v, int8_t bit = -1) { return CfgBinding{ k, el, id, v, bit, false, 0, 0 }; };
    auto s = [](Kind k, uint8_t el, uint16_t id, int lo, int hi) { return CfgBinding{ k, el, id, 0, -1, true, lo, hi }; };
    static const std::vector<CfgBinding> table = {
        // Gameplay: auto-target on / off, SE / BGM volume, gamma R / G / B, auto-sort on / off, sort by category / name.
        c(Kind::ConfigGameplay, 1, 7, 0), c(Kind::ConfigGameplay, 2, 7, 1), s(Kind::ConfigGameplay, 3, 9, 0, 100), s(Kind::ConfigGameplay, 4, 10, 0, 100),
        s(Kind::ConfigGameplay, 5, 55, 0, 100), s(Kind::ConfigGameplay, 6, 56, 0, 100), s(Kind::ConfigGameplay, 7, 57, 0, 100),
        c(Kind::ConfigGameplay, 8, 154, 1), c(Kind::ConfigGameplay, 9, 154, 0), c(Kind::ConfigGameplay, 10, 202, 0), c(Kind::ConfigGameplay, 11, 202, 1),
        // Misc.: damage display log / screen+log / screen; character data public / hidden (C2S 0x0DC, pseudo id -1);
        // shadows detailed / simple / none; weather particles on / off; character limit; icon pattern; others' armour.
        c(Kind::ConfigMisc, 1, 19, 0), c(Kind::ConfigMisc, 2, 19, 1), c(Kind::ConfigMisc, 5, 19, 2), c(Kind::ConfigMisc, 3, kCfgAnon, 0),
        c(Kind::ConfigMisc, 4, kCfgAnon, 1), c(Kind::ConfigMisc, 6, 58, 2), c(Kind::ConfigMisc, 7, 58, 0), c(Kind::ConfigMisc, 8, 58, 1),
        c(Kind::ConfigMisc, 9, 59, 1), c(Kind::ConfigMisc, 10, 59, 0), s(Kind::ConfigMisc, 11, 60, 25, 51), c(Kind::ConfigMisc, 12, 153, 1, 2),
        c(Kind::ConfigMisc, 13, 153, 0, 2), c(Kind::ConfigMisc, 14, 165, 0), c(Kind::ConfigMisc, 15, 165, 1),
        // Misc. 2: clipping plane, footsteps on / off, animation rate, keyboard size, aspect ratio, macro palettes.
        s(Kind::ConfigMisc2, 1, 62, 0, 25), c(Kind::ConfigMisc2, 2, 63, 0), c(Kind::ConfigMisc2, 3, 63, 1), s(Kind::ConfigMisc2, 4, 64, 0, 3),
        c(Kind::ConfigMisc2, 5, 66, 0), c(Kind::ConfigMisc2, 6, 66, 1), c(Kind::ConfigMisc2, 11, 66, 2), s(Kind::ConfigMisc2, 7, 67, 0, 5),
        c(Kind::ConfigMisc2, 12, 157, 0), c(Kind::ConfigMisc2, 13, 157, 1), c(Kind::ConfigMisc2, 14, 158, 0), c(Kind::ConfigMisc2, 15, 158, 1),
        // Misc. 3: weapon effects, own equipment look, AoE ranges, target effect, party status icons, icon timers,
        // furniture camera collision.
        c(Kind::ConfigMisc3, 1, 190, 0), c(Kind::ConfigMisc3, 2, 190, 1), c(Kind::ConfigMisc3, 3, 191, 0), c(Kind::ConfigMisc3, 4, 191, 1),
        c(Kind::ConfigMisc3, 5, 192, 1), c(Kind::ConfigMisc3, 6, 192, 0), c(Kind::ConfigMisc3, 7, 153, 0, 7), c(Kind::ConfigMisc3, 8, 153, 1, 7),
        c(Kind::ConfigMisc3, 9, 194, 1), c(Kind::ConfigMisc3, 10, 194, 0), c(Kind::ConfigMisc3, 11, 195, 1), c(Kind::ConfigMisc3, 12, 195, 0),
        c(Kind::ConfigMisc3, 13, 153, 0, 6), c(Kind::ConfigMisc3, 14, 153, 1, 6),
        // Misc. 4: software keyboard off / standard / compact; chat filter terms / players / off.
        c(Kind::ConfigMisc4, 1, 201, 0), c(Kind::ConfigMisc4, 2, 201, 1), c(Kind::ConfigMisc4, 3, 201, 2), c(Kind::ConfigMisc4, 4, 206, 1),
        c(Kind::ConfigMisc4, 5, 206, 2), c(Kind::ConfigMisc4, 6, 206, 0),
        // Mouse / camera: movement reference, edge rotation, free / locked camera, 3rd-person Y / X, 1st-person Y / X.
        c(Kind::ConfigCamera, 1, 144, 0), c(Kind::ConfigCamera, 2, 144, 1), c(Kind::ConfigCamera, 3, 143, 0), c(Kind::ConfigCamera, 4, 143, 1),
        c(Kind::ConfigCamera, 5, 145, 0), c(Kind::ConfigCamera, 6, 145, 1), c(Kind::ConfigCamera, 7, 140, 0), c(Kind::ConfigCamera, 8, 140, 1),
        c(Kind::ConfigCamera, 9, 139, 0), c(Kind::ConfigCamera, 10, 139, 1), c(Kind::ConfigCamera, 11, 142, 0), c(Kind::ConfigCamera, 12, 142, 1),
        c(Kind::ConfigCamera, 13, 141, 0), c(Kind::ConfigCamera, 14, 141, 1),
        // Global: profanity filter on / off; auto-disconnect 10..60 minutes / off.
        c(Kind::ConfigGlobal, 1, 24, 0), c(Kind::ConfigGlobal, 2, 24, 1), c(Kind::ConfigGlobal, 3, 65, 10), c(Kind::ConfigGlobal, 4, 65, 20),
        c(Kind::ConfigGlobal, 5, 65, 30), c(Kind::ConfigGlobal, 6, 65, 40), c(Kind::ConfigGlobal, 7, 65, 50), c(Kind::ConfigGlobal, 8, 65, 60),
        c(Kind::ConfigGlobal, 9, 65, -1),
        // Window settings: multiple log windows vertical / horizontal / off, timestamps HH:MM / HH:MM:SS / off, window
        // type 1..8 (cfg 18 = type - 1), window open / close effect on / off.
        c(Kind::ConfigWindows, 1, 180, 1), c(Kind::ConfigWindows, 2, 180, 2), c(Kind::ConfigWindows, 3, 180, 0), c(Kind::ConfigWindows, 4, 181, 1),
        c(Kind::ConfigWindows, 5, 181, 2), c(Kind::ConfigWindows, 6, 181, 0), c(Kind::ConfigWindows, 7, 18, 0), c(Kind::ConfigWindows, 8, 18, 1),
        c(Kind::ConfigWindows, 9, 18, 2), c(Kind::ConfigWindows, 10, 18, 3), c(Kind::ConfigWindows, 11, 18, 4), c(Kind::ConfigWindows, 12, 18, 5),
        c(Kind::ConfigWindows, 13, 18, 6), c(Kind::ConfigWindows, 14, 18, 7), c(Kind::ConfigWindows, 15, 164, 0), c(Kind::ConfigWindows, 16, 164, 1),
        // Log windows 1 / 2 (config_log_re.md §3; CODE 0x10199340 / 0x101993f0): auto-resize on / off, max lines 4..20,
        // min lines 0..20, column width 0..100 by 10, auto-adjust interval 3..30 s. Window 2's width / interval -1 = as window 1.
        c(Kind::ConfigLog1, 1, 14, 0), c(Kind::ConfigLog1, 2, 14, 1), s(Kind::ConfigLog1, 3, 15, 4, 20), s(Kind::ConfigLog1, 4, 16, 0, 20),
        s(Kind::ConfigLog1, 5, 156, 0, 100), s(Kind::ConfigLog1, 6, 17, 3, 30),
        c(Kind::ConfigLog2, 1, 182, 0), c(Kind::ConfigLog2, 2, 182, 1), s(Kind::ConfigLog2, 3, 183, 4, 20), s(Kind::ConfigLog2, 4, 184, 0, 20),
        s(Kind::ConfigLog2, 5, 197, 0, 100), s(Kind::ConfigLog2, 6, 198, 3, 30),
    };
    static bool tuned = false;
    if (!tuned)
    {
        // Slider details: the character limit and the interval bars start at their minimum; the log sliders print
        // their value; the column width moves by 10.
        tuned = true;
        for (auto& b : const_cast<std::vector<CfgBinding>&>(table))
        {
            if (!b.slider)
            {
                continue;
            }
            b.base   = b.id == 60 || b.id == 17 || b.id == 198 ? b.min : 0;
            b.number = b.page == Kind::ConfigLog1 || b.page == Kind::ConfigLog2;
            b.step   = b.id == 156 || b.id == 197 ? 10 : 1;
        }
    }
    return table;
}

int GameUi::cfgValue(const GameUiData& d, int id)
{
    if (id == 18)
    {
        return std::clamp(d.windowSkin, 1, 8) - 1;
    }
    if (id == 66)
    {
        return d.keyboard;
    }
    if (id == kCfgAnon)
    {
        return d.world && d.world->anonymous ? 1 : 0;
    }
    if (d.gameConfig)
    {
        if (const auto it = d.gameConfig->find(id); it != d.gameConfig->end())
        {
            return it->second;
        }
    }
    return cfgDefault(id);
}

int GameUi::cfgDefault(int id)
{
    // The game's defaults (0x10193b00) for the ids the pages use; the rest are 0.
    switch (id)
    {
        case 9:
        case 10:
        case 156: return 100;
        case 15:
        case 183: return 8;
        case 17: return 5;
        case 55:
        case 56:
        case 57:
        case 60: return 50;
        case 59: return 1;
        case 62: return 25;
        case 64: return 3;
        case 65:
        case 186:
        case 187:
        case 197:
        case 198: return -1;
        case 194:
        case 195: return 1;
        // Font colours, 0x80RRGGBB (config_log_re.md §1).
        case 27: return static_cast<int>(0x80808080u);
        case 28: return static_cast<int>(0x80a05040u);
        case 29: return static_cast<int>(0x80a040a0u);
        case 30: return static_cast<int>(0x8020c0a0u);
        case 31: return static_cast<int>(0x8050ff60u);
        case 32: return static_cast<int>(0x806050a0u);
        case 33: return static_cast<int>(0x80a0d0d0u);
        case 34: return static_cast<int>(0x80808080u);
        case 35: return static_cast<int>(0x806090c0u);
        case 36: return static_cast<int>(0x80a04040u);
        case 37:
        case 38:
        case 40:
        case 43:
        case 44: return static_cast<int>(0x80808080u);
        case 39: return static_cast<int>(0x80808050u);
        case 41: return static_cast<int>(0x8090c0f0u);
        case 42: return static_cast<int>(0x80c08080u);
        case 45: return static_cast<int>(0x80a08040u);
        case 46: return static_cast<int>(0x80707070u);
        case 47: return static_cast<int>(0x80808010u);
        case 48: return static_cast<int>(0x80c060d0u);
        case 49: return static_cast<int>(0x80c0c050u);
        case 155: return static_cast<int>(0x80a04030u);
        case 176: return static_cast<int>(0x80ffaf3fu);
        case 179: return static_cast<int>(0x8000cc00u);
        case 203: return static_cast<int>(0x800050ffu);
        case 204: return static_cast<int>(0x800070ffu);
        default: return 0;
    }
}

int GameUi::lineColourId(uint8_t kind)
{
    // Chat kinds -> the colour rows of "menu conftxtc" (log modes -> colour codes, CODE 0x1036e898 / 0x10129eb0).
    switch (kind)
    {
        case 0: return 27;    // say
        case 1: return 28;    // shout
        case 3: return 29;    // tell
        case 4: return 30;    // party
        case 5: return 31;    // linkshell
        case 8: return 32;    // emote
        case 26: return 155;  // yell
        case 27: return 179;  // linkshell 2
        case 33: return 176;  // unity
        case 0x22: return 203; // assist (J)
        case 0x23: return 204; // assist (E)
        case client::kNpcChat: return 34;
        case client::kCombatHurt: return 36; // HP you lose
        case client::kCombatOwn:
        case client::kCombatChat: return 47; // standard battle messages
        case 0xFF: return 49;                // this client's notes: basic system messages
        default: return 0;
    }
}

UiColor GameUi::cfgColour(const GameUiData& d, int id)
{
    const auto v = static_cast<uint32_t>(cfgValue(d, id));
    return { static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v), 0x80 };
}

int GameUi::lineBit(uint8_t kind)
{
    // Log Settings bits (CODE 0x1012a050): chat 0..13 by channel, battle / system colour ids 35..49 -> 33..47.
    switch (const int id = lineColourId(kind))
    {
        case 27: return 0;
        case 29: return 1;
        case 30: return 2;
        case 31: return 3;
        case 179: return 4;
        case 176: return 6;
        case 32: return 7;
        case 33: return 8;
        case 34: return 9;
        case 28: return 10;
        case 155: return 11;
        case 203: return 12;
        case 204: return 13;
        default: return id >= 35 && id <= 49 ? id - 2 : -1;
    }
}

void GameUi::openFullLog()
{
    // Confirm on the selected log window (0x101619b0, event 5): the full-screen log, on its first tab (+0x54 = 0 at
    // 0x1013768a), at the newest line with the cursor on the bottom row (INFERRED: the cursor's start is not set there).
    logMode_    = LogMode::Full;
    logFilter_  = 0;
    logCursor_  = 1;
    chatScroll_ = 0;
}

void GameUi::stepFullLogTab()
{
    // Event 8 (0x101383ea): the next tab's filter (0x10138840, wrapping), back at the newest line (+0x20 = 0).
    logFilter_  = nextFullLogFilter(fullLogTabs(logWin2_), logFilter_);
    logCursor_  = 1;
    chatScroll_ = 0;
}

bool GameUi::cfgChosen(const CfgBinding& b, const GameUiData& d)
{
    const int v = cfgValue(d, b.id);
    return b.bit >= 0 ? ((v >> b.bit) & 1) == b.value : v == b.value;
}

bool GameUi::isConfigPage(Kind k)
{
    return k == Kind::ConfigWindows || k == Kind::ConfigMisc2 || k == Kind::ConfigGameplay || k == Kind::ConfigMisc || k == Kind::ConfigMisc3 ||
           k == Kind::ConfigMisc4 || k == Kind::ConfigCamera || k == Kind::ConfigGlobal || k == Kind::ConfigLog1 || k == Kind::ConfigLog2;
}

const GameUi::CfgBinding* GameUi::cfgBinding(Kind page, uint16_t element)
{
    for (const auto& b : cfgBindings())
    {
        if (b.page == page && b.element == element)
        {
            return &b;
        }
    }
    return nullptr;
}

int GameUi::configCursor(Kind k, const GameUiData& d) const
{
    const auto* l = layout({ k });
    if (!l)
    {
        return 1;
    }
    // The first choice that matches the current value.
    for (const auto& b : cfgBindings())
    {
        if (b.page == k && !b.slider && cfgChosen(b, d) && l->element(b.element))
        {
            return b.element;
        }
    }
    return 1;
}

int GameUi::magicCategory(const dat::UiRecord& e)
{
    switch (e.text1)
    {
        case 303: return 1; // white magic
        case 304: return 2; // black magic
        case 305: return 5; // songs
        case 306: return 4; // ninjutsu
        case 307: return 3; // summoning
        case 637: return 6; // blue magic
        case 698: return 7; // geomancy
        case 718: return 8; // trust
        default: return 0;
    }
}

int GameUi::visibleRows(const Menu& m) const
{
    if (m.kind == Kind::MapMarkers)
    {
        return 10; // "menu mapv3" elements 11 / 12 are the page arrows
    }
    const auto* l = layout(m);
    // Rows are the selectable elements; page arrows ("menu quest01" 9/10) have nav 0xFF.
    return l ? static_cast<int>(std::count_if(l->elements.begin(), l->elements.end(), [](const dat::UiRecord& e) { return e.nav[0] != 0xFF; })) : 0;
}

void GameUi::open(Kind k, int param)
{
    if (sound_)
    {
        // The battle menu ("menu playermo") opens with 0011 (CODE 0x10158f08).
        sound_(k == Kind::Main || k == Kind::BattleMenu ? 11u : stack_.empty() ? 9u : 10u);
    }
    logMode_ = LogMode::Off; // a menu takes the cursor from the log window
    if (k == Kind::Status)
    {
        statusSub_ = -1;
    }
    if (k == Kind::Items)
    {
        manualSort_ = false;
        manualPick_ = -1;
    }
    stack_.push_back({ k, 1, 0, param });
}

std::vector<GameUi::Row> GameUi::rows(const Menu& m, const GameUiData& d, const GameUiActions& act)
{
    std::vector<Row> out;
    const auto&      w = *d.world;
    auto remaining = [&](const auto& ready, uint16_t id) -> int {
        const auto it = ready.find(id);
        if (it == ready.end() || it->second <= d.now)
        {
            return 0;
        }
        return static_cast<int>(std::ceil(std::chrono::duration<float>(it->second - d.now).count()));
    };
    auto itemName = [&](uint16_t id) {
        const auto info = d.items ? d.items->find(id) : std::nullopt;
        return info && !info->name.empty() ? info->name : fmt::format("item #{}", id);
    };
    switch (m.kind)
    {
        case Kind::Items:
            for (const auto& it : sortedInventory(w))
            {
                Row row;
                row.label    = itemName(it.itemId);
                row.right    = it.count > 1 ? std::to_string(it.count) : "";
                row.iconItem = it.itemId;
                // Row colours (CODE 0x101f0290): worn gear and the worn linkshells (equip slots 16 / 17) green, what the
                // menu can use yellow, the rest white.
                if (const auto info = d.items ? d.items->find(it.itemId) : std::nullopt)
                {
                    const auto kind = ::xi::ui::itemRow(*info, isWorn(w, it.container, it.slot));
                    row.colour      = kind == ItemRow::Worn ? kWornGreen : kind == ItemRow::Usable ? kUsableYellow : kWhite;
                }
                if (manualSort_ && manualPick_ == it.slot)
                {
                    row.colour = kGold; // Manual Sort's first pick
                }
                const int key = it.container << 8 | it.slot;
                row.invKey    = key;
                row.confirm   = [this, &d, &act, key, slot = it.slot] {
                    if (manualSort_)
                    {
                        manualSortPick(d, act, slot);
                        return;
                    }
                    open(Kind::ItemUse, key);
                };
                out.push_back(std::move(row));
            }
            break;
        case Kind::MapMarkers:
        {
            const int page = markerListPage_;
            if (page <= 1)
            {
                // Pages 0 / 1 (items 0..19): ten slots each, every row with its icon (151 / 153 empty, 150 / 152 placed)
                // and the marker's name (CODE 0x101fba70). A page 0 row opens the marker's menu ("menu mapv2",
                // 0x101fbc20 event 5); a page 1 row centres the map on the marker (0x101f8990: this map does not scroll).
                const auto& slots = markerSlots(mapZone_, mapId_);
                for (int i = 0; i < 10; ++i)
                {
                    const auto& s = slots[static_cast<std::size_t>(page * 10 + i)];
                    Row         row;
                    row.label      = s.name;
                    row.iconSprite = std::pair<std::string, int>{ "menu keytops3", mapMarkerSlotShape(page, s.present) };
                    if (page == 0)
                    {
                        row.confirm = [this, i] {
                            open(Kind::MapMarkerEdit, i);
                            // An empty slot starts on "place" (0x101fbf6f: element 1 dimmed, the cursor on 2).
                            stack_.back().cursor = markerSlots(mapZone_, mapId_)[static_cast<std::size_t>(i)].present ? 1 : 2;
                        };
                    }
                    else
                    {
                        row.confirm = [] {};
                    }
                    out.push_back(std::move(row));
                }
                break;
            }
            // Pages 2 / 3 (items 20..39): a row per member, its icon and name; confirming one shows its name on the map
            // and centres the map on it (0x101f8c70; this map does not scroll).
            const auto members = mapMarkerMembers(w, d.nearby);
            const auto party   = std::count_if(members.begin(), members.end(), [](const MapMarkerMember& mm) { return mm.sameParty; });
            for (std::size_t i = static_cast<std::size_t>(page - 2) * 10; i < members.size() && i < static_cast<std::size_t>(page - 1) * 10; ++i)
            {
                Row row;
                row.label      = members[i].name;
                row.iconSprite = std::pair<std::string, int>{ "menu keytops3", mapMarkerListShape(page, static_cast<int>(i % 10), static_cast<int>(party)) };
                row.confirm    = [this, id = members[i].id] { markerSel_ = id; };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::SearchArea:
        {
            Row here;
            here.label   = "This area";
            here.confirm = [this, search = act.search, z = w.zone] {
                searchQuery_.zone = z;
                runSearch(search);
            };
            out.push_back(std::move(here));
            Row all;
            all.label   = "All areas";
            all.confirm = [this, search = act.search] {
                searchQuery_.zone.reset();
                runSearch(search);
            };
            out.push_back(std::move(all));
            break;
        }
        case Kind::SearchUnity:
        {
            // 12 rows (CODE 0x1020ee40): row 0 "Unity" (your own: the player's leader byte +0x40de6, CODE 0x1020f01d), then
            // the 11 leaders, names 55653 #402 + i (0x100a35f0). Picking sets flags 2 = (i & 0x3f) << 22 (0x100e3130) and
            // searches (0x1020eff0). Row 0 searches our own Unity (S2C 0x061 unity_info Faction); dimmed without one.
            const uint32_t own = d.world->stats.unityLeader;
            for (uint32_t i = 0; i < 12; ++i)
            {
                Row row;
                row.label = d.unityText ? std::string(d.unityText->get(i == 0 ? 442 : 402 + i)) : fmt::format("Unity {}", i);
                if (i == 0)
                {
                    row.enabled = own != 0;
                }
                const uint32_t leader = i == 0 ? own : i;
                row.confirm = [this, search = act.search, leader] {
                    searchQuery_.flags2 = (leader & 0x3F) << 22;
                    runSearch(search);
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::EmoteList:
        {
            // "menu emote" (CODE 0x10225250): the entries whose parent is the folder shown (record +6), label and help
            // from the text file, in the order of the 128-entry id table 0x1063463c (0..127 unless the player's own
            // USER/<id>/eix.dat reorders it, CODE 0x10225be0; so record id order, which is the file's). Confirming
            // (CODE 0x102257d0, case 1) never closes the list: a folder opens its entries; a job emote (ids 76..97)
            // not unlocked (S2C 0x11A bit id - 76 clear, 0x10224f40) runs its text command at once ("/jobemote war",
            // sound 0004); a chair 1..12 (ids 99..110) not unlocked prints 55646 #194 (0x10224f90); any other emote
            // raises the sub-target cursor (0x10159370 type 0x7f, starting on the target) and runs on the pick.
            if (!d.emotes)
            {
                break;
            }
            for (const auto* em : d.emotes->children(static_cast<uint16_t>(m.param)))
            {
                Row row;
                row.label = d.emoteText ? std::string(d.emoteText->get(em->label)) : em->line();
                row.help  = d.emoteText ? std::string(d.emoteText->get(em->help)) : std::string();
                if (row.label.empty())
                {
                    row.label = em->line();
                }
                if (em->folder())
                {
                    row.confirm = [this, id = em->id] { open(Kind::EmoteList, id); };
                }
                else
                {
                    const uint32_t jobs   = d.world ? d.world->emoteJobs : 0;
                    const uint32_t chairs = d.world ? d.world->emoteChairs : 0;
                    if (em->id >= 76 && em->id <= 97 && !(jobs >> (em->id - 76) & 1))
                    {
                        row.confirm = [this, line = em->line()] { runLine(line); };
                    }
                    else if (em->id >= 99 && em->id <= 110 && !(chairs >> (em->id - 99) & 1))
                    {
                        row.confirm = [this] { notesOut_.emplace_back("You do not possess that chair."); }; // 55646 #194
                    }
                    else
                    {
                        row.confirm = [this, line = em->line()] { runLine(line + " <st>"); };
                    }
                }
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::Blacklist:
        {
            // "menu blklist" (CODE 0x101de870): one name per row (up to 300); none -> 55646 #103 in the help line.
            for (const auto& b : w.blacklist)
            {
                Row row;
                row.label   = b.second;
                row.confirm = [this, b] {
                    blackPick_ = b;
                    open(Kind::BlacklistOption);
                };
                out.push_back(std::move(row));
            }
            if (out.empty())
            {
                Row row;
                row.label   = "No entries in blacklist."; // 55646 #103
                row.enabled = false;
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::SearchResults:
        {
            if (!d.searchResult)
            {
                Row row;
                row.label   = "Searching...";
                row.enabled = false;
                out.push_back(std::move(row));
                break;
            }
            // The header row adds a condition (help str1 19 "Add condition to search."; CODE 0x1020bc10 opens
            // "menu scsibori"). Its own text is not traced: the refine menu's title stands in.
            {
                Row refine;
                refine.label   = "Refine by Type";
                refine.confirm = [this] { open(Kind::SearchRefine); };
                out.push_back(std::move(refine));
            }
            // Name, then jobs and levels (unless anonymous) and the area when it is not ours (CODE 0x1020a2f0).
            for (const auto& h : d.searchResult->hits)
            {
                Row row;
                row.label = h.name;
                std::string jobs = h.anonymous() ? std::string("(anon)")
                                 : h.subJob     ? fmt::format("{}{}/{}{}", jobAbbrev(h.mainJob), h.mainLevel, jobAbbrev(h.subJob), h.subLevel)
                                                : fmt::format("{}{}", jobAbbrev(h.mainJob), h.mainLevel);
                const auto area = h.zone != w.zone && d.areaShort ? std::string(d.areaShort->get(h.zone)) : std::string();
                row.right       = area.empty() ? jobs : fmt::format("{}  {}", jobs, area);
                row.confirm     = [this, h] {
                    // A Mute List result opens "menu acoption" instead (CODE 0x1020be00: flags 2 == 0x20000000).
                    searchHit_ = h;
                    open(searchQuery_.flags2 == 0x20000000 ? Kind::MuteOption : Kind::SearchOption);
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::ChatFilter:
        {
            // Row table 0x10379a28 {server bit, log-only bit, label in 55650} (CODE 0x10197b70 / 0x10197c20): server bits
            // 0..29 are MessageFilter, 30..61 MessageFilter2 (LSB filters1_t / filters2_t); log-only bits are cfg 52; the
            // last three rows are the system message level (cfg 53, SAVE_CONF byte 1 bits 3..4).
            struct FilterRow
            {
                uint16_t text;
                int8_t   server, local, level;
            };
            static const FilterRow kRows[] = { { 0, 0, 0, 0 }, { 2, -1, 2, 0 }, { 3, -1, 3, 0 }, { 4, -1, 4, 0 }, { 5, -1, 8, 0 }, { 6, 49, 9, 0 }, { 7, 50, 10, 0 }, { 8, -1, 7, 0 }, { 9, 3, 5, 0 }, { 1, 1, 1, 0 }, { 195, 46, -1, 0 }, { 11, 4, -1, 0 }, { 12, 5, -1, 0 }, { 13, 6, -1, 0 }, { 14, 7, -1, 0 }, { 15, 8, -1, 0 }, { 16, 9, -1, 0 }, { 30, 23, -1, 0 }, { 20, 13, -1, 0 }, { 21, 14, -1, 0 }, { 22, 15, -1, 0 }, { 23, 16, -1, 0 }, { 24, 17, -1, 0 }, { 31, 24, -1, 0 }, { 25, 18, -1, 0 }, { 26, 19, -1, 0 }, { 27, 20, -1, 0 }, { 28, 21, -1, 0 }, { 29, 22, -1, 0 }, { 177, 39, -1, 0 }, { 178, 38, -1, 0 }, { 179, 34, -1, 0 }, { 180, 35, -1, 0 }, { 181, 36, -1, 0 }, { 182, 37, -1, 0 }, { 171, 33, -1, 0 }, { 172, 32, -1, 0 }, { 173, 28, -1, 0 }, { 174, 29, -1, 0 }, { 175, 30, -1, 0 }, { 176, 31, -1, 0 }, { 32, 25, -1, 0 }, { 17, 10, -1, 0 }, { 18, 11, -1, 0 }, { 19, 12, -1, 0 }, { 88, 26, -1, 0 }, { 144, 27, -1, 0 }, { 191, 40, -1, 0 }, { 192, 41, -1, 0 }, { 193, 42, -1, 0 }, { 194, 45, -1, 0 }, { 10, 47, 6, 0 }, { 33, -1, -1, 1 }, { 34, -1, -1, 2 }, { 35, -1, -1, 3 } };
            if (!w.saveConfValid)
            {
                Row row;
                row.label   = "Waiting for the server's settings...";
                row.enabled = false;
                out.push_back(std::move(row));
                break;
            }
            const uint32_t f1    = static_cast<uint32_t>(w.saveConf[4]) | w.saveConf[5] << 8 | w.saveConf[6] << 16 | static_cast<uint32_t>(w.saveConf[7]) << 24;
            const uint32_t f2    = static_cast<uint32_t>(w.saveConf[8]) | w.saveConf[9] << 8 | w.saveConf[10] << 16 | static_cast<uint32_t>(w.saveConf[11]) << 24;
            const int      level = (w.saveConf[1] >> 3) & 3;
            const auto     local = static_cast<uint32_t>(cfgValue(d, 52));
            for (const auto& fr : kRows)
            {
                Row row;
                row.label = d.filterText ? std::string(d.filterText->get(fr.text)) : fmt::format("Filter {}", fr.text);
                const bool srv = fr.server >= 0 && ((fr.server < 30 ? f1 >> fr.server : f2 >> (fr.server - 30)) & 1);
                const bool loc = fr.local >= 0 && ((local >> fr.local) & 1);
                const bool lvl = fr.level > 0 && level >= fr.level;
                row.right      = srv || lvl ? "OFF" : loc ? "OFF (log)" : "ON";
                row.confirm    = [fr, w1 = f1, w2 = f2, lv = level, lo = local, srv, loc, set = act.setChatFilters]() mutable {
                    if (!set)
                    {
                        return;
                    }
                    auto flip = [&](bool on) {
                        uint32_t& word = fr.server < 30 ? w1 : w2;
                        const int bit  = fr.server < 30 ? fr.server : fr.server - 30;
                        word           = on ? word | (1u << bit) : word & ~(1u << bit);
                    };
                    if (fr.level > 0)
                    {
                        lv = lv >= fr.level ? fr.level - 1 : fr.level;
                    }
                    else if (fr.server >= 0 && fr.local >= 0)
                    {
                        // Shown -> hidden (server) -> hidden in the log only -> shown.
                        flip(!srv && !loc);
                        lo = srv ? lo | (1u << fr.local) : lo & ~(1u << fr.local);
                    }
                    else if (fr.server >= 0)
                    {
                        flip(!srv);
                    }
                    else if (fr.local >= 0)
                    {
                        lo ^= 1u << fr.local;
                    }
                    set(w1, w2, lv, lo);
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::FontColorList:
        {
            for (const auto& fr : fontRows(m.param))
            {
                Row row;
                row.label   = d.filterText ? std::string(d.filterText->get(fr.label)) : fmt::format("Colour {}", fr.cfg);
                row.confirm = [this, fr, &d] {
                    const auto v = static_cast<uint32_t>(cfgValue(d, fr.cfg));
                    fontEdit_    = { fr.cfg, fr.sample, { static_cast<int>((v >> 16) & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>(v & 0xFF) }, 0 };
                    open(Kind::FontColorEdit);
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::LogFilter:
        {
            // A set bit shows the line kind in that window (CODE 0x10129ae0); masks 185/186 (window 1), 187/188 (window 2).
            const int win = m.param / 4;
            for (const auto& [label, bit] : logRows(m.param % 4))
            {
                const int  id = (win == 0 ? 185 : 187) + (bit >= 32 ? 1 : 0);
                const int  v  = cfgValue(d, id);
                const bool on = ((static_cast<uint32_t>(v) >> (bit % 32)) & 1) != 0;
                Row row;
                row.label   = d.filterText ? std::string(d.filterText->get(label)) : fmt::format("Bit {}", bit);
                row.right   = on ? "ON" : "OFF";
                row.confirm = [id, v, bit, set = act.setConfig] {
                    if (set)
                    {
                        set(id, static_cast<int>(static_cast<uint32_t>(v) ^ (1u << (bit % 32))));
                    }
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::EffectFilter:
        {
            // 18 rows, labels 55650 #153..170 ("All effects during battle", "You -> you" ... "Screen shaking"); row n
            // toggles bit n of cfg 146, set = hidden (CODE 0x10197f10 / 0x10197f30 / 0x10198020).
            const int bits = cfgValue(d, 146);
            for (int n = 0; n < 18; ++n)
            {
                Row row;
                row.label = d.filterText ? std::string(d.filterText->get(153 + n)) : fmt::format("Effect {}", n);
                // The labels' arrow is Shift-JIS 81 A8; the text font has ASCII only.
                for (std::size_t at; (at = row.label.find("\x81\xA8")) != std::string::npos;)
                {
                    row.label.replace(at, 2, "->");
                }
                row.right   = (bits >> n) & 1 ? "OFF" : "ON";
                row.confirm = [n, bits, set = act.setConfig] {
                    if (set)
                    {
                        set(146, bits ^ (1 << n));
                    }
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::KeyAssign:
        {
            // Rows: the key in a "[  ]" box at x + 4, its action label (55650) at x + 0x40 (CODE 0x1019c180). Keys are
            // assigned by pressing them (assignKey()); Confirm does nothing here, as in the game.
            const auto list = m.param ? AssignList::Shortcut : AssignList::Compact;
            const auto keys = assignment(d);
            const auto rws  = KeyAssign::rows(list);
            const int  n    = KeyAssign::shown(list, static_cast<KeyLayout>(std::clamp(d.keyboard, 0, 2)));
            for (int r = 0; r < n; ++r)
            {
                Row row;
                const auto label = rws[static_cast<std::size_t>(r)].label;
                row.label        = d.filterText ? std::string(d.filterText->get(label)) : fmt::format("Key {}", label);
                row.keycap       = KeyAssign::keyTop(keys.key(list, r));
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::MogStorage:
        {
            // The Mog House's containers (LSB ids): Mog Safe, Mog Safe 2, Storage, Mog Locker.
            static const std::pair<const char*, int> kBoxes[] = { { "Mog Safe", 1 }, { "Mog Safe 2", 9 }, { "Storage", 2 }, { "Mog Locker", 4 } };
            for (const auto& [name, c] : kBoxes)
            {
                Row row;
                row.label   = name;
                row.confirm = [this, c = c] { open(Kind::Storage, c); };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::JobPick:
        {
            // Unlocked jobs (S2C 0x01B bit j) with their levels; the current one greyed. Picking sends C2S 0x100
            // {+4 main, +5 support; 0 = unchanged} (CODE 0x100f0e60); the server answers with the new job state.
            static const char* kJobNames[] = { "",        "Warrior",   "Monk",     "White Mage", "Black Mage",   "Red Mage",  "Thief",   "Paladin",
                                               "Dark Knight", "Beastmaster", "Bard", "Ranger",     "Samurai",      "Ninja",     "Dragoon", "Summoner",
                                               "Blue Mage", "Corsair",   "Puppetmaster", "Dancer", "Scholar",     "Geomancer", "Rune Fencer" };
            const bool sub = m.param == 1;
            for (uint8_t j = 1; j < std::size(kJobNames); ++j)
            {
                if (!(w.jobsUnlocked & (1u << j)) || w.jobLevels[j] == 0)
                {
                    continue;
                }
                Row row;
                row.label   = kJobNames[j];
                row.right   = fmt::format("Lv.{}", w.jobLevels[j]);
                row.enabled = j != (sub ? w.subJob : w.mainJob) && !(sub && j == w.mainJob);
                row.confirm = [this, &act, j, sub] {
                    if (act.send)
                    {
                        std::vector<std::byte> pkt(4);
                        pkt[sub ? 1 : 0] = std::byte{ j };
                        act.send(0x100, std::move(pkt));
                    }
                    stack_.clear();
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::Quests:
            // One row per quest area and state, labelled with the game's window titles ("San d'Oria: Current Quests").
            for (int log = 0; log < 8; ++log)
            {
                for (int done = 0; done < 2; ++done)
                {
                    Row row;
                    row.label   = d.titles ? std::string(d.titles->get(static_cast<std::size_t>(kQuestTitles[log][done]))) : std::string();
                    const int p = log * 2 + done;
                    row.confirm = [this, p] { open(Kind::QuestList, p); };
                    out.push_back(std::move(row));
                }
            }
            break;
        case Kind::QuestList:
        {
            // Quests of one area, current (param even) or completed (odd), by id. Aht Urhgan's packet carries other
            // logs past bit 127 (assault, missions), so its quests stop there.
            const int   log  = m.param / 2;
            const auto& bits = (m.param % 2 == 0 ? w.questCurrent : w.questDone)[static_cast<std::size_t>(log)];
            const int   end  = log == 6 ? 128 : 256;
            for (int id = 0; id < end; ++id)
            {
                if (!(bits[static_cast<std::size_t>(id / 32)] & (1u << (id % 32))))
                {
                    continue;
                }
                const auto* q = d.questNames ? (*d.questNames)[static_cast<std::size_t>(log)].find(static_cast<uint32_t>(id)) : nullptr;
                Row         row;
                row.label = q ? q->name : fmt::format("Quest #{}", id);
                row.help  = q ? q->description : std::string();
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::MissionList:
        {
            // Your nation's (param 0) or Zilart's (1) missions: completed ones (port 0xD0), then the current one.
            const uint32_t log     = m.param == 0 ? std::min<uint32_t>(w.nation, 2) : 3;
            const uint32_t current = m.param == 0 ? w.nationMission : w.zilartMission;
            const auto*    names   = d.missionNames ? &(*d.missionNames)[log] : nullptr;
            for (uint32_t id = 0; id < 64; ++id)
            {
                const bool done = w.missionsDone[log * 2 + id / 32] & (1u << (id % 32));
                if (!done && id != current)
                {
                    continue;
                }
                const auto* e = names ? names->find(id) : nullptr;
                Row         row;
                row.label = e ? e->name : fmt::format("Mission #{}", id);
                row.right = id == current && !done ? "Current" : "";
                row.help  = e ? e->description : std::string();
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::KeyItems:
            // Every key item held, by id (the game's category picker, "menu evitem01", is not done).
            for (const auto id : w.keyItems)
            {
                const auto* ki = d.keyItems ? d.keyItems->find(id) : nullptr;
                Row         row;
                row.label = ki ? ki->name : fmt::format("Key item #{}", id);
                if (!row.label.empty())
                {
                    row.label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(row.label[0])));
                }
                row.help = ki ? ki->description : std::string();
                out.push_back(std::move(row));
            }
            break;
        case Kind::Currencies:
            // Mode 1 (CODE 0x10229a60, rows drawn by 0x10229ae0): no rows until S2C 0x113 answers; then every entry of
            // table 0x103862c8 in order, its label (55653) and the value "%d" right-aligned.
            if (!w.currencies.empty())
            {
                for (const auto& c : currencyRows())
                {
                    Row row;
                    row.label = d.unityText ? std::string(d.unityText->get(c.label)) : fmt::format("Currency #{}", c.key);
                    row.right = fmt::format("{}", static_cast<int32_t>(currencyValue(c.key, w.currencies)));
                    row.help  = d.help ? std::string(d.help->get(c.help)) : std::string();
                    out.push_back(std::move(row));
                }
            }
            break;
        case Kind::PartyPick:
            for (const auto& pm : w.party)
            {
                if (pm.id == w.charId || pm.partyNo != 0)
                {
                    continue;
                }
                Row row;
                row.label         = pm.name;
                const auto id     = pm.id;
                const auto idx    = pm.index;
                const auto name   = pm.name;
                const bool remove = m.param == 1;
                row.confirm       = [this, &act, id, idx, name, remove] {
                    if (act.send)
                    {
                        if (remove)
                        {
                            // C2S 0x071 GROUP_STRIKE {UniqueNo, ActIndex, Kind 0, pad, sName[15]}.
                            std::vector<std::byte> pkt(24);
                            std::memcpy(pkt.data(), &id, 4);
                            std::memcpy(pkt.data() + 4, &idx, 2);
                            std::memcpy(pkt.data() + 8, name.data(), std::min<std::size_t>(name.size(), 15));
                            act.send(0x071, std::move(pkt));
                        }
                        else
                        {
                            // C2S 0x077 GROUP_CHANGE2 {sName[16], Kind 0 party, ChangeKind 0 = leader}.
                            std::vector<std::byte> pkt(20);
                            std::memcpy(pkt.data(), name.data(), std::min<std::size_t>(name.size(), 16));
                            act.send(0x077, std::move(pkt));
                        }
                    }
                    stack_.clear();
                };
                out.push_back(std::move(row));
            }
            break;
        case Kind::Storage:
        case Kind::StoragePut:
        {
            const bool put = m.kind == Kind::StoragePut;
            if (!put && m.param != 3) // nothing goes into the temporary items
            {
                Row row;
                row.label   = "Put away...";
                row.help    = "Move an item from your inventory into this container.";
                row.confirm = [this, p = m.param] { open(Kind::StoragePut, p); };
                out.push_back(std::move(row));
            }
            for (const auto& it : w.inventory)
            {
                // Inventory slot 0 is gil.
                if (it.container != (put ? 0 : m.param) || it.itemId == 0 || it.itemId == 0xFFFF ||
                    (put && it.slot == 0))
                {
                    continue;
                }
                Row row;
                row.label         = itemName(it.itemId);
                row.right         = it.count > 1 ? std::to_string(it.count) : "";
                row.iconItem      = it.itemId;
                if (put)
                {
                    // Not worn gear; only gear into a Mog Wardrobe (LSB 0x029 isValidMovement).
                    const bool wardrobe = m.param == 8 || (m.param >= 10 && m.param <= 16);
                    const auto info     = d.items ? d.items->find(it.itemId) : std::nullopt;
                    row.enabled         = !isWorn(w, 0, it.slot) && (!wardrobe || (info && info->equipment()));
                }
                else
                {
                    row.enabled = !isWorn(w, it.container, it.slot); // gear worn from a wardrobe stays
                }
                const auto from   = static_cast<uint8_t>(put ? 0 : m.param);
                const auto to     = static_cast<uint8_t>(put ? m.param : 0);
                const auto slot   = it.slot;
                const uint32_t count = it.count;
                row.confirm       = [&act, from, to, slot, count] {
                    // C2S 0x029 ITEM_MOVE {u32 ItemNum, Category1 (from), Category2 (to), ItemIndex1, ItemIndex2 (82+ = a
                    // free slot)}: the whole stack.
                    if (act.send)
                    {
                        std::vector<std::byte> pkt(8);
                        std::memcpy(pkt.data(), &count, 4);
                        pkt[4] = static_cast<std::byte>(from);
                        pkt[5] = static_cast<std::byte>(to);
                        pkt[6] = static_cast<std::byte>(slot);
                        pkt[7] = std::byte{ 82 };
                        act.send(0x029, std::move(pkt));
                    }
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::DeliveryBox:
        {
            const auto& b   = w.dbox;
            const auto  box = static_cast<int8_t>(m.param);
            int         firstEmpty = -1;
            for (int k = 7; k >= 0; --k)
            {
                if (b.cells[static_cast<std::size_t>(k)].itemId == 0)
                {
                    firstEmpty = k;
                }
            }
            if (box == 1)
            {
                Row row;
                row.label   = b.waiting ? fmt::format("Receive items ({})", b.waiting) : std::string("No new items");
                row.enabled = b.waiting > 0 && firstEmpty >= 0;
                row.confirm = [this, &act, firstEmpty] {
                    if (act.send)
                    {
                        act.send(0x04D, deliveryPacket(0x06, 1, static_cast<int8_t>(firstEmpty), 1)); // Recv into the empty cell
                    }
                };
                out.push_back(std::move(row));
            }
            else
            {
                Row row;
                row.label   = sendTo_.empty() ? std::string("Send to: (use /sendpost <name>)")
                            : b.nameOk == 0   ? fmt::format("Send to: {} (no such character)", sendTo_)
                                              : fmt::format("Send an item to {}...", sendTo_);
                row.enabled = !sendTo_.empty() && b.nameOk != 0 && firstEmpty >= 0;
                row.confirm = [this] { open(Kind::DeliveryPick, 2); };
                out.push_back(std::move(row));
            }
            for (int k = 0; k < 8; ++k)
            {
                const auto& c = b.cells[static_cast<std::size_t>(k)];
                if (c.itemId == 0)
                {
                    continue;
                }
                Row row;
                row.label    = itemName(c.itemId);
                row.right    = c.count > 1 ? std::to_string(c.count) : "";
                row.iconItem = c.itemId;
                row.help     = box == 1 ? fmt::format("From {}. Enter: take it.", c.name)
                             : c.stat == 3 ? fmt::format("Sent to {}. Enter: cancel the delivery.", c.name)
                                           : fmt::format("To {}. Enter: take it back.", c.name);
                const auto slot = static_cast<int8_t>(k);
                const auto stat = c.stat;
                row.confirm     = [this, &act, box, slot, stat] {
                    if (act.send)
                    {
                        // Get (take) from the incoming box; outgoing: Cancel a sent item, Clear one not sent yet.
                        act.send(0x04D, deliveryPacket(box == 1 ? 0x0A : stat == 3 ? 0x04 : 0x0B, box, slot));
                    }
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::AucList:
        {
            if (d.auctionCategory != m.param || !d.auctionItems)
            {
                Row row;
                row.label   = "Loading...";
                row.enabled = false;
                out.push_back(std::move(row));
                break;
            }
            // Each item, and its stack on a second line when it stacks; the number on the right is the stock.
            for (const auto& it : *d.auctionItems)
            {
                const auto info = d.items ? d.items->find(it.itemId) : std::nullopt;
                for (const bool stack : { false, true })
                {
                    if (stack && (!info || info->stackSize <= 1))
                    {
                        continue;
                    }
                    Row row;
                    row.label    = stack ? fmt::format("{} x{}", itemName(it.itemId), info->stackSize) : itemName(it.itemId);
                    row.right    = std::to_string(stack ? it.stack : it.single);
                    row.iconItem = it.itemId;
                    row.confirm  = [this, id = it.itemId, stack] {
                        aucItem_  = id;
                        aucStack_ = stack;
                        open(Kind::AucItem);
                    };
                    out.push_back(std::move(row));
                }
            }
            break;
        }
        case Kind::AucHistory:
        {
            if (!d.auctionSalesReady || !d.auctionSales)
            {
                Row row;
                row.label   = "Loading...";
                row.enabled = false;
                out.push_back(std::move(row));
                break;
            }
            for (const auto& s : *d.auctionSales)
            {
                Row row;
                const std::time_t t = s.date;
                char              day[16]{};
                std::strftime(day, sizeof(day), "%m/%d", std::localtime(&t));
                row.label   = fmt::format("{}  {} gil", day, s.price);
                row.right   = fmt::format("{} > {}", s.seller, s.buyer);
                out.push_back(std::move(row));
            }
            if (d.auctionSales->empty())
            {
                Row row;
                row.label   = "No sales on record.";
                row.enabled = false;
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::AucSales:
            for (int k = 0; k < 7; ++k)
            {
                const auto& s = w.auction.sales[static_cast<std::size_t>(k)];
                Row row;
                if (s.itemId == 0)
                {
                    row.label   = "-";
                    row.enabled = false;
                }
                else
                {
                    row.label    = s.stack ? fmt::format("{} (stack)", itemName(s.itemId)) : itemName(s.itemId);
                    row.right    = fmt::format("{} gil", s.price);
                    row.iconItem = s.itemId;
                    row.confirm  = [this, k, s] {
                        aucItem_  = s.itemId;
                        aucStack_ = s.stack;
                        open(Kind::AucSale, k);
                    };
                }
                out.push_back(std::move(row));
            }
            break;
        case Kind::Treasure:
        {
            // Each item in the pool: our lot (or the best lot and who holds it) and the minutes left of five.
            bool any = false;
            for (std::size_t k = 0; k < w.treasure.size(); ++k)
            {
                const auto& t = w.treasure[k];
                if (t.itemId == 0)
                {
                    continue;
                }
                any = true;
                Row row;
                row.label    = t.count > 1 ? fmt::format("{} x{}", itemName(t.itemId), t.count) : itemName(t.itemId);
                row.iconItem = t.itemId;
                const auto left = std::max<long>(0, 300 - std::chrono::duration_cast<std::chrono::seconds>(d.now - t.seen).count());
                row.right    = t.myLot >= 0 && t.myLot < 1000 ? fmt::format("{} {}:{:02}", t.myLot, left / 60, left % 60)
                             : t.bestLot > 0               ? fmt::format("{} {}:{:02}", t.bestLot, left / 60, left % 60)
                                                           : fmt::format("{}:{:02}", left / 60, left % 60);
                row.help     = t.bestLot > 0 ? fmt::format("Highest lot: {} ({}).", t.bestName, t.bestLot) : std::string("Nobody has lotted.");
                row.confirm  = [this, k] { open(Kind::TreasureOp, static_cast<int>(k)); };
                out.push_back(std::move(row));
            }
            if (!any)
            {
                Row row;
                row.label   = "The treasure pool is empty.";
                row.enabled = false;
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::Conquest:
        {
            if (w.conquest.serial == 0)
            {
                Row row;
                row.label   = "Loading...";
                row.enabled = false;
                out.push_back(std::move(row));
                break;
            }
            auto name = [&](std::size_t i) { return d.regions ? std::string(d.regions->get(i)) : fmt::format("#{}", i); };
            if (m.param == 1)
            {
                // /bmap: Besieged. Al Zahbi's orders, who holds the Astral Candescence, each stronghold's state.
                static const char* kOurs[]    = { "Defend Al Zahbi", "Intercept", "Infiltrate", "?" };
                static const char* kTheirs[]  = { "Trn", "Adv", "Atk", "Ret", "Def", "Prep", "?", "?" };
                static const char* kForts[]   = { "Mamook", "Halvung", "Arrapago" };
                static const char* kHolders[] = { "Al Zahbi", "Mamool Ja", "Trolls", "Undead Swarm" };
                const auto         o          = w.conquest.besiegedOverview;
                Row head;
                head.label   = kHolders[o & 3]; // who holds the Astral Candescence
                head.right   = kOurs[(o >> 2) & 3];
                head.enabled = false;
                out.push_back(std::move(head));
                for (std::size_t k = 0; k < 3; ++k)
                {
                    const auto s = w.conquest.strongholds[k];
                    Row row;
                    row.label   = kForts[k];
                    row.right   = fmt::format("L{} {} {}", (s >> 11) & 0xF, kTheirs[s & 7], (s >> 3) & 0xFF); // level, orders, forces
                    row.enabled = false;
                    out.push_back(std::move(row));
                }
                break;
            }
            {
                Row row;
                row.label   = fmt::format("CP {}", w.conquest.points);
                row.right   = fmt::format("tally {}d", w.conquest.nextTally);
                row.enabled = false;
                out.push_back(std::move(row));
            }
            for (std::size_t r = 0; r < w.conquest.owner.size(); ++r)
            {
                // REGION_TYPE -> 55654: Ronfaure .. Tu'Lia are 4 on; Movalpolos 22, Tavnazian Archipelago 24.
                const std::size_t s = r <= 16 ? r + 4 : r == 17 ? 22 : 24;
                const auto        o = w.conquest.owner[r];
                Row row;
                row.label   = name(s);
                row.right   = o >= 1 && o <= 3 ? name(o - 1) : o == 4 ? std::string("Beastmen") : std::string("-");
                row.enabled = false;
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::AucSellPick:
            for (const auto& it : w.inventory)
            {
                if (it.container != 0 || it.itemId == 0 || it.itemId == 0xFFFF || it.slot == 0)
                {
                    continue;
                }
                Row row;
                row.label    = itemName(it.itemId);
                row.right    = it.count > 1 ? std::to_string(it.count) : "";
                row.iconItem = it.itemId;
                for (const auto& [c, sl] : w.equipment)
                {
                    if (c == 0 && sl == it.slot)
                    {
                        row.enabled = false; // equipped
                    }
                }
                const auto info  = d.items ? d.items->find(it.itemId) : std::nullopt;
                const bool stack = info && info->stackSize > 1 && it.count >= info->stackSize; // a full stack sells as one
                row.confirm      = [this, &act, slot = it.slot, id = it.itemId, stack] {
                    aucSellSlot_ = slot;
                    aucItem_     = id;
                    aucStack_    = stack;
                    askNumber("Asking price (gil)?", [this, &act, slot, id, stack](uint32_t price) {
                        // AskCommit {u32 Commission (the price), u16 ItemWorkIndex, u16 ItemNo, u32 ItemStacks 1 single / 0
                        // stack}: the server answers with the fee.
                        aucPrice_ = price;
                        auto pkt  = aucPacket(0x04, -1);
                        std::memcpy(pkt.data() + 4, &price, 4);
                        const uint16_t idx = slot;
                        std::memcpy(pkt.data() + 8, &idx, 2);
                        std::memcpy(pkt.data() + 0x0A, &id, 2);
                        const uint32_t stacks = stack ? 0 : 1;
                        std::memcpy(pkt.data() + 0x0C, &stacks, 4);
                        if (act.send)
                        {
                            act.send(0x04E, std::move(pkt));
                            aucWaitFee_ = true;
                        }
                    });
                };
                out.push_back(std::move(row));
            }
            break;
        case Kind::DeliveryPick:
        {
            int firstEmpty = -1;
            for (int k = 7; k >= 0; --k)
            {
                if (w.dbox.cells[static_cast<std::size_t>(k)].itemId == 0)
                {
                    firstEmpty = k;
                }
            }
            for (const auto& it : w.inventory)
            {
                if (it.container != 0 || it.itemId == 0 || it.itemId == 0xFFFF || it.slot == 0)
                {
                    continue;
                }
                Row row;
                row.label    = itemName(it.itemId);
                row.right    = it.count > 1 ? std::to_string(it.count) : "";
                row.iconItem = it.itemId;
                for (const auto& [c, sl] : w.equipment)
                {
                    if (c == 0 && sl == it.slot)
                    {
                        row.enabled = false; // equipped
                    }
                }
                const auto slot  = static_cast<int8_t>(it.slot);
                const auto count = static_cast<int32_t>(it.count);
                row.confirm      = [this, &act, slot, count, firstEmpty] {
                    if (act.send && firstEmpty >= 0)
                    {
                        // Set: the stack goes into the outgoing cell; the session sends it when the server agrees.
                        act.send(0x04D, deliveryPacket(0x02, 2, static_cast<int8_t>(firstEmpty), slot, count, sendTo_));
                    }
                    stack_.pop_back();
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::AppList:
        {
            const auto& am = appMenus_.at(static_cast<std::size_t>(m.param));
            for (const auto& r : am.rows)
            {
                Row row;
                row.label   = r.label;
                row.right   = r.right;
                row.enabled = r.enabled;
                row.help    = r.help.empty() ? am.prompt : r.help;
                row.confirm = [this, tag = am.tag, v = r.value, keep = am.keepStack] {
                    appChoice_ = { tag, v };
                    if (!keep)
                    {
                        stack_.clear();
                    }
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::EventMenu:
            for (const auto& [index, text] : eventOptions_)
            {
                Row row;
                row.label     = text;
                const int i   = index;
                row.confirm   = [this, i] {
                    eventChoice_ = i;
                    stack_.clear();
                };
                out.push_back(std::move(row));
            }
            break;
        case Kind::SellPick:
            for (const auto& it : w.inventory)
            {
                if (it.container != 0 || it.slot == 0)
                {
                    continue;
                }
                bool equipped = false;
                for (const auto& [c, i] : w.equipment)
                {
                    equipped |= c == it.container && i == it.slot;
                }
                Row row;
                row.label         = itemName(it.itemId);
                row.right         = it.count > 1 ? std::to_string(it.count) : "";
                row.iconItem      = it.itemId;
                row.enabled       = !equipped;
                const auto s      = it.slot;
                const auto itemId = it.itemId;
                const auto count  = it.count;
                row.confirm       = [this, &act, s, itemId, count] {
                    // C2S 0x084 SHOP_SELL_REQ {u32 ItemNum, u16 ItemNo @4, u8 ItemIndex @6}: the whole stack; the vendor
                    // answers with its price (S2C 0x03D).
                    if (act.send)
                    {
                        std::vector<std::byte> pkt(8);
                        std::memcpy(pkt.data(), &count, 4);
                        std::memcpy(pkt.data() + 4, &itemId, 2);
                        pkt[6] = static_cast<std::byte>(s);
                        act.send(0x084, std::move(pkt));
                    }
                    open(Kind::ShopSell, s);
                };
                out.push_back(std::move(row));
            }
            break;
        case Kind::Shop:
        {
            uint32_t gil = 0; // inventory slot 0 holds the gil (item 0xFFFF)
            for (const auto& it : w.inventory)
            {
                if (it.container == 0 && it.slot == 0)
                {
                    gil = it.count;
                }
            }
            for (std::size_t i = 0; i < w.shop.size(); ++i)
            {
                const auto& g = w.shop[i];
                Row         row;
                row.label     = itemName(g.item);
                row.right     = fmt::format("{} G", g.price);
                row.iconItem  = g.item;
                row.enabled   = gil >= g.price;
                const int at  = static_cast<int>(i);
                row.confirm   = [this, at] {
                    shopQty_ = 1;
                    open(Kind::ShopBuy, at);
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::GuildBuy:
        {
            // The guild's goods (CODE 0x101ec4d0): name, stock, price; dimmed when out of stock or beyond our gil.
            // The stock column is this client's reading of the row's second column (INFERRED).
            const uint32_t gil   = gilHeld(w);
            const auto&    items = w.guild.lists[0].items;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                const auto& g = items[i];
                Row         row;
                row.label    = itemName(g.item);
                row.right    = fmt::format("{:>2}  {} G", g.count, g.gil());
                row.iconItem = g.item;
                row.enabled  = g.count > 0 && gil >= g.gil();
                row.help     = d.help ? std::string(d.help->get(47)) : std::string(); // "Purchase merchandise."
                const int at = static_cast<int>(i);
                row.confirm  = [this, at] {
                    shopQty_ = 1;
                    open(Kind::GuildBuyQty, at);
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::GuildSell:
        {
            // What the guild buys (CODE 0x101ec8c0): first "Select from your inventory." (55648 #44), then each item it
            // shows (price bit 31 hides one), dimmed when we hold none or its stock is full.
            Row first;
            first.label   = d.clientText ? std::string(d.clientText->get(44)) : std::string("Select from your inventory.");
            first.help    = d.help ? std::string(d.help->get(51)) : std::string();
            first.confirm = [this] { open(Kind::GuildSellPick, 0); };
            out.push_back(std::move(first));
            for (const auto& g : w.guild.lists[1].items)
            {
                if (g.hidden())
                {
                    continue;
                }
                const bool held = std::any_of(w.inventory.begin(), w.inventory.end(), [&](const auto& it) { return it.container == 0 && it.slot != 0 && it.itemId == g.item; });
                Row        row;
                row.label    = itemName(g.item);
                row.right    = fmt::format("{:>2}/{}  {} G", g.count, g.max, g.gil());
                row.iconItem = g.item;
                row.enabled  = held && g.count < g.max;
                row.help     = d.help ? std::string(d.help->get(51)) : std::string(); // "Select an item from the guild's buy list."
                const auto item = g.item;
                row.confirm     = [this, item] { open(Kind::GuildSellPick, item); };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::GuildSellPick:
            // Inventory stacks the guild buys (all of them, or one item: param), with the guild's price (CODE
            // 0x101ecd00). Which ones the game lists is INFERRED: unequipped, and the guild's stock not full.
            for (const auto& it : w.inventory)
            {
                if (it.container != 0 || it.slot == 0 || (m.param != 0 && it.itemId != m.param))
                {
                    continue;
                }
                const auto& list = w.guild.lists[1].items;
                const auto  g    = std::find_if(list.begin(), list.end(), [&](const client::GuildItem& x) { return x.item == it.itemId; });
                if (g == list.end())
                {
                    continue;
                }
                bool equipped = false;
                for (const auto& [c, i] : w.equipment)
                {
                    equipped |= c == it.container && i == it.slot;
                }
                Row row;
                row.label    = itemName(it.itemId);
                row.right    = fmt::format("x{}  {} G", it.count, g->gil());
                row.iconItem = it.itemId;
                row.enabled  = !equipped && g->count < g->max;
                row.help     = d.help ? std::string(d.help->get(48)) : std::string(); // "Sell merchandise."
                const auto s     = it.slot;
                const auto count = it.count;
                row.confirm      = [this, s, count] {
                    shopQty_ = std::max<uint32_t>(1, count);
                    open(Kind::GuildSellQty, s);
                };
                out.push_back(std::move(row));
            }
            break;
        case Kind::LinkshellPick:
            for (const auto& it : w.inventory)
            {
                if (it.container != 0 || it.slot == 0 || !isLinkshellItem(it.itemId))
                {
                    continue;
                }
                Row row;
                row.label    = itemName(it.itemId);
                row.iconItem = it.itemId;
                const bool on = (w.equipment[16].first == 0 && w.equipment[16].second == it.slot) || (w.equipment[17].first == 0 && w.equipment[17].second == it.slot);
                row.right     = on ? "Active" : "";
                const auto s  = it.slot;
                row.confirm   = [this, s] {
                    linkSlot_ = s;
                    open(Kind::Linkshell);
                };
                out.push_back(std::move(row));
            }
            break;
        case Kind::SynthHistory:
            // A recipe: its crystal and ingredients; picking it fills the synthesis window from the inventory (one
            // item per box, each from a stack with enough left), if everything is there.
            for (const auto& r : synthHistory_)
            {
                std::string label = itemName(r.crystal);
                for (const auto id : r.items)
                {
                    if (id != 0)
                    {
                        label += ", " + itemName(id);
                    }
                }
                Row row;
                row.label = label;
                // Find slots: the crystal, then each ingredient (a stack may serve several boxes).
                std::map<uint8_t, uint32_t> used;
                auto take = [&](uint16_t id) -> uint8_t {
                    for (const auto& it : w.inventory)
                    {
                        if (it.container == 0 && it.slot != 0 && it.itemId == id && used[it.slot] < it.count)
                        {
                            ++used[it.slot];
                            return it.slot;
                        }
                    }
                    return 0;
                };
                const uint8_t                              crystalSlot = take(r.crystal);
                std::array<std::pair<uint8_t, uint32_t>, 8> boxes{};
                bool                                       ok = crystalSlot != 0;
                for (std::size_t b = 0; b < 8; ++b)
                {
                    if (r.items[b] != 0)
                    {
                        const uint8_t s = take(r.items[b]);
                        ok &= s != 0;
                        boxes[b] = { s, 1 };
                    }
                }
                row.enabled      = ok;
                const auto crystalId = r.crystal;
                row.confirm      = [this, crystalSlot, crystalId, boxes] {
                    synthCrystalSlot_ = crystalSlot;
                    synthCrystalId_   = crystalId;
                    tradeBoxes_       = boxes;
                    stack_.pop_back();
                    open(Kind::Synth);
                };
                out.push_back(std::move(row));
            }
            break;
        case Kind::SynthCrystal:
            for (const auto& it : w.inventory)
            {
                if (it.container != 0 || it.slot == 0 || !isSynthCrystal(it.itemId))
                {
                    continue;
                }
                Row row;
                row.label         = itemName(it.itemId);
                row.right         = it.count > 1 ? std::to_string(it.count) : "";
                row.iconItem      = it.itemId;
                const auto s      = it.slot;
                const auto itemId = it.itemId;
                row.confirm       = [this, s, itemId] {
                    synthCrystalSlot_ = s;
                    synthCrystalId_   = itemId;
                    tradeBoxes_       = {};
                    stack_.pop_back();
                    open(Kind::Synth);
                };
                out.push_back(std::move(row));
            }
            break;
        case Kind::TradeItem:
        {
            // In the synthesis window a stack can fill several boxes (one item each), up to its count; the crystal's
            // own stack keeps one back.
            const bool synth  = stack_.size() >= 2 && stack_[stack_.size() - 2].kind == Kind::Synth;
            const bool player = stack_.size() >= 2 && stack_[stack_.size() - 2].kind == Kind::PlayerTrade;
            for (const auto& it : w.inventory)
            {
                if (it.container != 0 || it.slot == 0)
                {
                    continue;
                }
                bool     equipped = false, used = false;
                uint32_t uses     = it.slot == synthCrystalSlot_ && synth ? 1 : 0;
                for (const auto& [c, i] : w.equipment)
                {
                    equipped |= c == it.container && i == it.slot;
                }
                for (const auto& [slot, count] : tradeBoxes_)
                {
                    used |= slot == it.slot;
                    uses += slot == it.slot ? 1 : 0;
                }
                if (synth)
                {
                    used = uses >= it.count;
                }
                Row row;
                row.label    = itemName(it.itemId);
                row.right    = it.count > 1 ? std::to_string(it.count) : "";
                row.iconItem = it.itemId;
                row.enabled  = !equipped && !used;
                const int box = m.param;
                const auto s  = it.slot;
                const auto itemId = it.itemId;
                if (player)
                {
                    // CODE 0x10150d90: dimmed when equipped, bazaar-listed or locked, or item flags & 0x4000 (Ex);
                    // orange when already in the trade (it can still be picked: it goes back into its box).
                    const auto info = d.items ? d.items->find(it.itemId) : std::nullopt;
                    row.enabled     = !equipped && !(info && (info->flags & 0x4000));
                    if (used)
                    {
                        row.colour = UiColor{ 0x80, 0x40, 0x20, 0x80 };
                    }
                    const uint32_t have = it.count;
                    row.confirm         = [this, &d, &act, box, s, itemId, have] {
                        stack_.pop_back();
                        tradePick(box, s, itemId, have, d, act);
                    };
                    out.push_back(std::move(row));
                    continue;
                }
                row.confirm   = [this, &act, box, s, itemId] {
                    tradeBoxes_[static_cast<std::size_t>(box)] = { s, 1 };
                    stack_.pop_back();
                    if (!stack_.empty() && stack_.back().kind == Kind::PlayerTrade && act.send)
                    {
                        sendTradeBox(act, box, itemId);
                    }
                };
                out.push_back(std::move(row));
            }
            break;
        }
        case Kind::EquipList:
        {
            // The slot's list (CODE 0x10130470 colours, 0x10130b30 rows): every item that fits it in the inventory and
            // the Mog Wardrobes, worn green, wearable white, not wearable grey (wardrobes 3..8 the account lacks too).
            // Choosing the worn one again removes it (0x10132b04: the game's way to take gear off); the rest equip.
            const auto slot = static_cast<uint8_t>(m.param);
            const auto ctx  = equipContext(w);
            const auto setAt = std::find_if(stack_.begin(), stack_.end(), [](const Menu& s) { return s.kind == Kind::EquipSetEdit; });
            const bool setEdit = setAt != stack_.end();
            const int  setNo   = setEdit ? setAt->param : 0;
            for (const uint8_t bag : kEquipBags)
            {
                if (bag != 0 && !bagExists(w, bag))
                {
                    continue;
                }
                for (const auto& it : w.inventory)
                {
                    if (it.container != bag || it.slot == 0)
                    {
                        continue;
                    }
                    const auto info = d.items ? d.items->find(it.itemId) : std::nullopt;
                    if (!info || !info->equipment() || !(info->slots & (1u << slot)))
                    {
                        continue;
                    }
                    // Set mode (Equipment Sets -> Edit): green marks the set's item for the slot (CODE 0x10130c60 checks
                    // the set, 0x101333f0), and choosing stores the item in the set, or takes it out again.
                    const auto setIt  = setEdit ? equipSets_.find(setNo) : equipSets_.end();
                    const auto lname  = lower(info->name);
                    const bool inSet  = setIt != equipSets_.end() && std::ranges::any_of(setIt->second.items, [&](const auto& si) { return si.first == slot && si.second == lname; });
                    const bool worn = setEdit ? inSet : isWorn(w, it.container, it.slot);
                    Row row;
                    row.label    = info->name; // no level column: the info window has it
                    row.iconItem = it.itemId;
                    row.invKey   = it.container << 8 | it.slot;
                    row.colour   = worn ? kWornGreen : canEquip(*info, ctx) && fitsSlot(*info, slot, ctx.blockedSlots) ? kWhite : kDimGrey;
                    const auto c = it.container, s = it.slot;
                    if (setEdit)
                    {
                        row.confirm = [this, setNo, slot, lname, inSet] {
                            auto& items = equipSets_[setNo].items;
                            std::erase_if(items, [&](const auto& si) { return si.first == slot; });
                            if (!inSet)
                            {
                                items.emplace_back(slot, lname);
                            }
                            saveEquipSets();
                            stack_.pop_back();
                        };
                        out.push_back(std::move(row));
                        continue;
                    }
                    row.confirm  = [this, &d, &act, c, s, slot, worn] {
                        if (equipFromMenu(d, act, c, worn ? 0 : s, slot))
                        {
                            stack_.pop_back(); // back to the grid (CODE 0x10132b47)
                        }
                    };
                    out.push_back(std::move(row));
                }
            }
            break;
        }
        case Kind::Magic:
            for (const auto id : w.spells)
            {
                const auto* sp = d.spells ? d.spells->find(id) : nullptr;
                // Param 0 (the battle menu's Magic, Ctrl+M): every type but trust (the game's exclude mask 0x100).
                if (!sp || !(sp->usableBy(w.mainJob, w.mainLevel) || sp->usableBy(w.subJob, w.subLevel)) ||
                    (m.param > 0 ? sp->type != m.param : sp->type == 8))
                {
                    continue;
                }
                const auto name = d.text ? d.text->spellName(id) : std::string_view();
                const int  wait = remaining(w.spellReady, id);
                Row row;
                row.label   = name.empty() ? fmt::format("#{}", id) : std::string(name);
                // Retail's spell list shows only the names; the cost and the recast are in the sub window.
                row.mpCost  = sp->mp;
                // The list icon at the row origin, the name at +0x12 (CODE 0x1022c9c0 -> 0x102208a0): the spell's own
                // icon ("menu magiconw" shape, "menu magico2w" from 1000) unless config option 2 asks for the element
                // icons (not offered here) or it has none; else the element icon.
                row.iconSprite = sp->listIcon == 0xFFFF ? std::pair<std::string, int>{ "menu magiconw", sp->icon }
                               : sp->listIcon >= 1000   ? std::pair<std::string, int>{ "menu magico2w", sp->listIcon - 1000 }
                                                        : std::pair<std::string, int>{ "menu magiconw", sp->listIcon };
                row.next    = countdown(std::max(wait, 0));
                row.help    = d.text ? std::string(d.text->spellHelp(id)) : std::string();
                // The cursor's mask (CODE 0x10102210): the spell's targets, but a song under Pianissimo (status 409) or a
                // geomancy spell under Entrust (584) that only targets yourself reaches the party too (5).
                uint16_t mask = sp->targets;
                const bool pianissimo = std::ranges::any_of(w.buffs, [](const auto& b) { return b.icon == 409; });
                const bool entrust    = std::ranges::any_of(w.buffs, [](const auto& b) { return b.icon == 584; });
                if (mask == 0x01 && ((sp->type == 5 && pianissimo) || (sp->type == 7 && entrust)))
                {
                    mask = 0x05;
                }
                row.enabled = mask != 0 && w.mp >= sp->mp && wait == 0;
                row.confirm = [this, &d, mask, n = std::string(name)] { menuSubTarget("/magic", n, mask, d); };
                out.push_back(std::move(row));
            }
            break;
        case Kind::AbilityList:
            if (m.param == 2)
            {
                // Job traits: a read-only list with descriptions.
                for (const auto id : w.traits)
                {
                    const auto n = d.text ? d.text->abilityName(1536u + id) : std::string_view();
                    Row row;
                    row.label = n.empty() ? fmt::format("#{}", id) : std::string(n);
                    row.help  = d.text ? std::string(d.text->abilityHelp(1536u + id)) : std::string();
                    out.push_back(std::move(row));
                }
            }
            else if (m.param == 1)
            {
                const bool canWs = w.engaged() && w.tp >= 1000; // the cursor picks the target
                for (const auto id : w.weaponSkills)
                {
                    const auto n = d.text ? d.text->abilityName(id) : std::string_view();
                    Row row;
                    row.label   = n.empty() ? fmt::format("#{}", id) : std::string(n);
                    row.help    = d.text ? std::string(d.text->abilityHelp(id)) : std::string();
                    row.enabled = canWs;
                    const uint16_t mask = d.abilities && d.abilities->commandTargets(id) ? d.abilities->commandTargets(id) : 0x20;
                    row.confirm = [this, &d, mask, nm = std::string(n)] { menuSubTarget("/weaponskill", nm, mask, d); };
                    out.push_back(std::move(row));
                }
            }
            else
            {
                // Job Abilities (param 0) leave out the pet commands, which have their own list (param 3): ability
                // type 2 in file 85 (DATA).
                for (const auto id : w.jobAbilities)
                {
                    const auto* ab   = d.abilities ? d.abilities->find(id) : nullptr;
                    if ((ab && ab->type == 2) != (m.param == 3))
                    {
                        continue;
                    }
                    const auto  grp  = w.abilityRecastGroup.find(id);
                    const int   wait = grp != w.abilityRecastGroup.end() ? remaining(w.abilityRecastReady, grp->second)
                                       : ab                              ? remaining(w.abilityRecastReady, ab->recastId)
                                                                         : 0;
                    const auto  n    = d.text ? d.text->abilityName(512u + id) : std::string_view();
                    Row row;
                    row.label   = n.empty() ? fmt::format("#{}", id) : std::string(n);
                    row.right   = wait > 0 ? countdown(wait) : "";
                    row.help    = d.text ? std::string(d.text->abilityHelp(512u + id)) : std::string();
                    row.enabled = wait == 0 && !(ab && w.mp < ab->mp);
                    // The cursor on the ability's valid targets (comm +0x0A; file 85's as a fallback).
                    const uint16_t ct   = d.abilities ? d.abilities->commandTargets(512u + id) : 0;
                    const uint16_t mask = ct ? ct : ab && ab->targets ? ab->targets : 0x01;
                    row.confirm = [this, &d, mask, nm = std::string(n)] { menuSubTarget("/jobability", nm, mask, d); };
                    out.push_back(std::move(row));
                }
            }
            break;
        default: break;
    }
    return out;
}

void GameUi::confirm(const GameUiData& d, const GameUiActions& act)
{
    auto& m = stack_.back();
    const auto& w = *d.world;
    if (isList(m.kind))
    {
        auto       list = rows(m, d, act);
        const auto i    = static_cast<std::size_t>(m.scroll + m.cursor - 1);
        if (i < list.size() && list[i].enabled && list[i].confirm)
        {
            list[i].confirm();
        }
        return;
    }
    const auto* l = layout(m);
    const auto* e = l ? l->element(static_cast<uint16_t>(m.cursor)) : nullptr;
    if (!e)
    {
        return;
    }
    if (isMacroKind(m.kind))
    {
        if (implemented(m, *e))
        {
            const dat::UiRecord el = *e; // macroConfirm may open windows (stack_ grows)
            macroConfirm(m, el);
        }
        return;
    }
    switch (m.kind)
    {
        case Kind::Status:
            // statcom (CODE 0x10148c30): an element other than the one shown closes the open window and shows its own
            // ("menu profile / joblevel / btlskill / mgcskill / trdskill", table 0x103716c8); the cursor stays here.
            if (e->id <= 5)
            {
                statusSub_ = e->id - 1;
            }
            else if (e->id == 6)
            {
                // Currencies (CODE 0x10148c85): close the shown window, then "menu evitem" in mode 1 over statcom, which
                // asks for the values with C2S 0x10F (0x10229d80; header only) and fills its rows from S2C 0x113.
                statusSub_ = -1;
                open(Kind::Currencies);
                if (act.send)
                {
                    act.send(0x10F, {});
                }
            }
            break;
        case Kind::RegionMenu:
            command(e->id == 1 ? "/regionmap" : "/besiegedmap", d, act); // the Conquest / Besieged lists
            break;
        case Kind::ItemSort:
            // "menu itmsortw" (goal #45): 1 Auto-sort (help 269), 2 Manual Sort (270: pick a stack, then the stack to
            // put it with; the same stack again splits one off, help 5 / 6), 3 Switch (1067: "menu storage").
            if (e->id == 1)
            {
                // Auto-sort asks first: "Sort items?" (55653 #61) in "menu sortyn", the cursor on No (#118; CODE
                // 0x101f37a4, ui/yes_no.h).
                open(Kind::SortConfirm);
                stack_.back().cursor = yesNoCursor(kSortConfirmOnYes);
            }
            else if (e->id == 2)
            {
                manualSort_ = true;
                manualPick_ = -1;
                stack_.pop_back();
            }
            else if (e->id == 3 && implemented(m, *e))
            {
                open(Kind::ItemBags);
            }
            break;
        case Kind::SortConfirm:
            // Yes (CODE 0x101f3330): sort, then the focus goes back to the list; No: back to the sort window.
            if (yesNoAnswer(true, false, m.cursor).value_or(false))
            {
                sortInventory(d, act);
                stack_.pop_back(); // the dialog
                if (!stack_.empty() && stack_.back().kind == Kind::ItemSort)
                {
                    stack_.pop_back(); // the sort window: back to the list
                }
            }
            else
            {
                stack_.pop_back();
            }
            break;
        case Kind::ItemBags:
            if (implemented(m, *e) && e->id < 16)
            {
                open(Kind::Storage, kBagOfElement[e->id]);
            }
            break;
        case Kind::SearchMenu:
        case Kind::SearchRefine:
            if ((m.kind == Kind::SearchMenu && (e->id == 9 || e->id == 15)) || (m.kind == Kind::SearchRefine && (e->id == 9 || e->id == 13)))
            {
                // Linkshell: its members (CODE 0x1020d002 / 0x1020d043: criteria LS1 / LS2, search now).
                const bool second = e->id != 9;
                searchQuery_.linkshell  = linkshellId(second ? 1 : 0);
                searchQuery_.linkshell2 = second;
                runSearch(act.search);
                break;
            }
            if ((m.kind == Kind::SearchMenu && e->id == 14) || (m.kind == Kind::SearchRefine && e->id == 12))
            {
                open(Kind::SearchUnity); // CODE 0x1020d0d8: "menu arealist" in Unity mode (0x1020ef50)
                break;
            }
            if ((m.kind == Kind::SearchMenu && e->id == 16) || (m.kind == Kind::SearchRefine && e->id == 14))
            {
                // Request: players asking to join a party (CODE 0x1020d0fe: flags 2 := 0x10000000, search now).
                searchQuery_.flags2 = 0x10000000;
                runSearch(act.search);
                break;
            }
            if (m.kind == Kind::SearchRefine && e->id > 7 && e->id != 11)
            {
                break;
            }
            if (e->id == 1) open(Kind::SearchArea);
            else if (e->id == 3) open(Kind::SearchJob);
            else if (e->id == 4) open(Kind::SearchNation);
            else if (e->id == 5) open(Kind::SearchRace);
            else if (e->id == 7) open(Kind::SearchRank);
            else if (e->id == 11) open(Kind::CommentGenre, 1);
            else if (e->id == 12 && m.kind == Kind::SearchMenu) open(Kind::CommentGenre, 0);
            else if (e->id == 2)
            {
                askText("Name", [this, search = act.search](std::string n) {
                    searchQuery_.name = std::move(n);
                    runSearch(search);
                });
            }
            else if (e->id == 6)
            {
                // Level: min then max, both starting at our main level in the game (scchar; CODE 0x1020d7f0).
                askNumber("Level (minimum)", [this, search = act.search](uint32_t lo) {
                    askNumber("Level (maximum)", [this, search, lo](uint32_t hi) {
                        searchQuery_.minLvl = static_cast<uint8_t>(std::clamp<uint32_t>(lo, 1, 99));
                        searchQuery_.maxLvl = static_cast<uint8_t>(std::clamp<uint32_t>(std::max(hi, lo), 1, 99));
                        runSearch(search);
                    });
                });
            }
            break;
        case Kind::SearchJob:
            searchQuery_.job = static_cast<uint8_t>(e->id); // element id = job id (CODE 0x10107bc0)
            runSearch(act.search);
            break;
        case Kind::SearchNation:
            searchQuery_.nation = static_cast<uint8_t>(e->id - 1); // CODE 0x10107b00
            runSearch(act.search);
            break;
        case Kind::SearchRace:
            // The game sends 1/3/5/7/8 (table 0x10385a0f); LSB filters on 0..4 (search_handler / data_loader).
            searchQuery_.race = static_cast<uint8_t>(e->id - 1);
            runSearch(act.search);
            break;
        case Kind::SearchRank:
            searchQuery_.minRank = searchQuery_.maxRank = static_cast<uint8_t>(e->id); // CODE 0x101078d0
            runSearch(act.search);
            break;
        case Kind::CommentGenre:
        {
            // Genre g = 1..7. Search: comment g << 4 (CODE 0x10107f70). Edit (CODE 0x1020d415): g 1..3 -> sccompar,
            // 4 -> sccomite, 5 -> sccomls (sub type 1..3), 6 -> type 0x61, 7 -> 0x73; then the text.
            const int g = e->id;
            if (m.param == 1)
            {
                searchQuery_.comment = static_cast<uint32_t>(g << 4);
                runSearch(act.search);
            }
            else if (g <= 5)
            {
                open(Kind::CommentSub, g);
            }
            else
            {
                askComment(g == 6 ? 0x61 : 0x73, act.send);
            }
            break;
        }
        case Kind::CommentSub:
            askComment(static_cast<uint32_t>(m.param << 4 | e->id), act.send);
            break;
        case Kind::MuteOption:
            if (e->id == 1 && act.send && w.mentor)
            {
                // C2S 0x0B7 ASSIST_CHANNEL {u8 Kind 0x27 RemoveFromMuteList, u8 pad, sName[15], Mes[1] = ' '} (LSB
                // 0x0b7_assist_channel.h, atom0s XiPackets 0x00B7).
                std::vector<std::byte> pkt(0x14);
                pkt[0] = std::byte{ 0x27 };
                std::memcpy(pkt.data() + 2, searchHit_.name.data(), std::min<std::size_t>(searchHit_.name.size(), 15));
                pkt[0x11] = std::byte{ ' ' };
                act.send(0x0B7, std::move(pkt));
                stack_.clear();
            }
            break;
        case Kind::BlacklistOption:
            if (e->id == 1 && act.send)
            {
                // Delete (CODE 0x101deb70 -> 0x100e0af0(entry, 1)): C2S 0x03D BLACK_EDIT {u32 ID, Name[16], i8 Mode 1}; the
                // server's 0x042 updates the list.
                std::vector<std::byte> pkt(0x18);
                std::memcpy(pkt.data(), &blackPick_.first, 4);
                std::memcpy(pkt.data() + 4, blackPick_.second.data(), std::min<std::size_t>(blackPick_.second.size(), 15));
                pkt[0x14] = std::byte{ 1 };
                act.send(0x03D, std::move(pkt));
                stack_.pop_back();
            }
            break;
        case Kind::SearchOption:
            if (e->id == 1)
            {
                startTyping(fmt::format("/tell {} ", searchHit_.name)); // CODE: pastes "/tell <name> "
            }
            else if (e->id == 3 && act.send)
            {
                // Invite: C2S 0x06E {UniqueNo, ActIndex 0 (not known for a search hit; CODE 0x100e8f00(id, 0, 0)), Kind 0}.
                std::vector<std::byte> pkt(8);
                std::memcpy(pkt.data(), &searchHit_.id, 4);
                act.send(0x06E, std::move(pkt));
                stack_.clear();
            }
            break;
        case Kind::MogHouse:
            if (!implemented(m, *e)) break; // dimmed (Layout / Remodel while open, Remodel without the 2F)
            if (e->id == 1) open(Kind::MogStorage);
            else if (e->id == 2) command("/deliverybox", d, act);
            else if (e->id == 3) open(Kind::JobChange);
            else if (e->id >= 4 && e->id <= 7)
            {
                // Gardening, Layout, Open / Close Mog House, Remodel: the application's (CODE 0x10145390).
                appChoice_ = { kMyRoomTag, static_cast<int>(e->id) };
                stack_.clear();
            }
            break;
        case Kind::AppElements:
            appChoice_ = { appMenus_.at(static_cast<std::size_t>(m.param)).tag, static_cast<int>(e->id) };
            stack_.clear();
            break;
        case Kind::JobChange:
            // Support jobs need the support-job quest (0x01B bit 0).
            if (e->id == 1 || (e->id == 2 && (w.jobsUnlocked & 1))) open(Kind::JobPick, e->id - 1);
            break;
        case Kind::Social:
            if (e->id >= 13)
            {
                flipMainPage(); // the arrows and the title flip the page
            }
            else if (e->text1 == kHelpCurrentTime && act.note)
            {
                command("/clock", d, act);
                stack_.clear();
            }
            else if (e->text1 == kHelpLogOut || e->text1 == kHelpShutDown)
            {
                open(Kind::LogoutConfirm, e->text1 == kHelpShutDown ? 1 : 0);
                stack_.back().cursor = yesNoCursor(kLogoutConfirmOnYes); // opens on No (CODE 0x10160950 / 0x101608c3)
            }
            else if (e->text1 == kHelpConfig)
            {
                open(Kind::Config);
            }
            else if (e->text1 == kHelpKeyItems)
            {
                open(Kind::KeyItems);
            }
            else if (e->text1 == kHelpQuests)
            {
                open(Kind::Quests);
            }
            else if (e->text1 == kHelpMissions)
            {
                open(Kind::Missions);
            }
            else if (e->text1 == kHelpCommunication)
            {
                open(Kind::Communication);
            }
            else if (e->text1 == kHelpMacros)
            {
                open(Kind::MacroMenu);
            }
            break;
        case Kind::Communication:
            if (e->id == 2)
            {
                open(Kind::EmoteList, 0); // CODE 0x10224fc0: "menu emote" at the top level
                if (act.send)
                {
                    act.send(0x119, {}); // the unlocked job emotes and chairs (S2C 0x11A; CODE 0x100fd4b0)
                }
            }
            break;
        case Kind::Missions:
            if (e->id == 1 || e->id == 2)
            {
                open(Kind::MissionList, e->id == 1 ? 0 : 1);
            }
            break;
        case Kind::Config:
        case Kind::ConfigWindowsMenu:
        {
            static const std::pair<int, Kind> kTop[] = { { 1, Kind::ConfigGameplay }, { 2, Kind::ChatFilter }, { 4, Kind::ConfigWindowsMenu }, { 5, Kind::ConfigMisc },
                                                         { 6, Kind::ConfigMisc2 },    { 7, Kind::EffectFilter },    { 8, Kind::ConfigCamera },      { 9, Kind::ConfigGlobal },
                                                         { 11, Kind::ConfigMisc3 },   { 12, Kind::ConfigMisc4 },    { 3, Kind::FontColors },        { 10, Kind::LogSettings } };
            if (m.kind == Kind::ConfigWindowsMenu)
            {
                // Shared settings, log window 1, log window 2 (CODE 0x10198de0).
                const Kind page = e->id == 1 ? Kind::ConfigWindows : e->id == 2 ? Kind::ConfigLog1 : Kind::ConfigLog2;
                if (e->id >= 1 && e->id <= 3)
                {
                    open(page);
                    stack_.back().cursor = configCursor(page, d);
                }
                break;
            }
            if (m.kind == Kind::Config && e->id == 15)
            {
                appChoice_ = { kAddonsTag, 0 }; // the application opens its Add-ons page on top (Config stays open)
                break;
            }
            for (const auto& [id, page] : kTop)
            {
                if (e->id == id)
                {
                    open(page);
                    stack_.back().cursor = configCursor(page, d);
                }
            }
            break;
        }
        case Kind::ConfigWindows:
        case Kind::ConfigMisc2:
        case Kind::ConfigGameplay:
        case Kind::ConfigMisc:
        case Kind::ConfigMisc3:
        case Kind::ConfigMisc4:
        case Kind::ConfigCamera:
        case Kind::ConfigGlobal:
        case Kind::ConfigLog1:
        case Kind::ConfigLog2:
        {
            // A choice sets its config id at once; a slider toggles editing, where left / right change the value live
            // (CODE e.g. conf2win 0x101971b2..0x10197251).
            if (m.kind == Kind::ConfigMisc2 && e->id >= 8 && e->id <= 10)
            {
                // Misc. 2: 8 "menu k1assign", 9 "menu k2assign", 10 "Reset keyboard assignment to default?" (CODE
                // 0x10196a3e / 0x10196a54 / 0x10196a6a).
                open(e->id == 10 ? Kind::ConfigReset : Kind::KeyAssign, e->id == 10 ? 2 : e->id - 8);
                if (e->id == 10)
                {
                    stack_.back().cursor = 2;
                }
                break;
            }
            const auto* b = cfgBinding(m.kind, e->id);
            if (!b)
            {
                break;
            }
            if (!implemented(m, *e))
            {
                break; // dimmed
            }
            if (b->slider)
            {
                sliderEdit_ = sliderEdit_ == e->id ? 0 : e->id;
            }
            else if (b->id == 18 && act.setWindowSkin)
            {
                act.setWindowSkin(b->value + 1);
            }
            else if (b->id == 66 && act.setKeyboard)
            {
                act.setKeyboard(b->value);
            }
            else if (act.setConfig)
            {
                const int cur = cfgValue(d, b->id);
                act.setConfig(b->id, b->bit >= 0 ? (cur & ~(1 << b->bit)) | (b->value << b->bit) : b->value);
            }
            break;
        }
        case Kind::FontColors:
            if (e->id >= 1 && e->id <= 4)
            {
                open(Kind::FontColorList, e->id - 1);
            }
            else if (e->id == 5)
            {
                open(Kind::ConfigReset, 0);
                stack_.back().cursor = 2;
            }
            break;
        case Kind::FontColorEdit:
            if (e->id >= 1 && e->id <= 3)
            {
                fontEdit_.channel = fontEdit_.channel == e->id ? 0 : e->id;
            }
            else
            {
                if (e->id == 4 && act.setConfig)
                {
                    // OK: Set(row cfg, 0x80RRGGBB) (CODE 0x10198c30).
                    const auto& c3 = fontEdit_.rgb;
                    act.setConfig(fontEdit_.cfg, static_cast<int>(0x80000000u | static_cast<uint32_t>(c3[0]) << 16 | static_cast<uint32_t>(c3[1]) << 8 | static_cast<uint32_t>(c3[2])));
                }
                stack_.pop_back();
            }
            break;
        case Kind::ConfigReset:
            if (e->id == 1 && act.setConfig)
            {
                // Yes: the font colours (CODE 0x10195d10) or the log windows' masks (0x10195de0) back to their defaults.
                if (m.param == 0)
                {
                    for (const int id : kFontColourIds)
                    {
                        act.setConfig(id, cfgDefault(id));
                    }
                }
                else if (m.param == 2)
                {
                    // Both key lists back to their defaults (CODE 0x10196ad0 -> 0x1019ca10 / 0x1019cd60).
                    for (const auto list : { AssignList::Compact, AssignList::Shortcut })
                    {
                        const auto rws = KeyAssign::rows(list);
                        for (std::size_t r = 0; r < rws.size(); ++r)
                        {
                            act.setConfig(KeyAssign::cfgId(list, static_cast<int>(r)), KeyAssign::dik(rws[r].key));
                        }
                    }
                }
                else
                {
                    for (const int id : { 185, 186, 187, 188 })
                    {
                        act.setConfig(id, cfgDefault(id));
                    }
                }
            }
            stack_.pop_back();
            break;
        case Kind::LogSettings:
            if (e->id == 1 || e->id == 2)
            {
                open(Kind::LogCategory, e->id - 1);
            }
            else if (e->id == 3)
            {
                open(Kind::ConfigReset, 1);
                stack_.back().cursor = 2;
            }
            break;
        case Kind::LogCategory:
            if (e->id >= 1 && e->id <= 4)
            {
                open(Kind::LogFilter, m.param * 4 + e->id - 1);
            }
            break;
        case Kind::LogoutConfirm:
            if (m.cursor == 1 && act.logout)
            {
                act.logout(m.param == 1);
            }
            stack_.clear();
            break;
        case Kind::Revive:
            // C2S 0x01A: HomepointMenu 0x0B (StatusId 0 = accept), RaiseMenu 0x0D (0 accept, else decline),
            // TractorMenu 0x13 (0 accept, 1 reject). No on the home point prompt just closes it.
            if (act.action && (m.cursor == 1 || m.param != 0))
            {
                const uint16_t id = m.param == 1 ? 0x0D : m.param == 2 ? 0x13 : 0x0B;
                act.action(w.charId, w.actIndex, id, m.cursor == 1 ? 0 : 1);
            }
            stack_.clear();
            break;
        case Kind::Main:
        {
            if (e->id >= 13)
            {
                flipMainPage();
                break;
            }
            const auto* sub = e->subFor(0);
            const int   lbl = sub ? sub->shape : -1;
            if (lbl == kLabelStatus) open(Kind::Status);
            else if (lbl == kLabelEquipment) openEquipment();
            else if (lbl == kLabelMagic) open(Kind::MagicMenu);
            else if (lbl == kLabelItems) open(Kind::Items);
            else if (lbl == kLabelAbilities) open(Kind::Abilities);
            else if (lbl == kLabelParty)
            {
                // Invited -> join menu; leader -> leader menu ("menu prty3"); member -> "menu prty2"; solo -> "menu prty1".
                bool leader = false;
                for (const auto& pm : w.party)
                {
                    leader |= pm.id == w.charId && pm.leader;
                }
                open(Kind::Party, w.partyInvite ? 2 : w.party.empty() ? 0 : leader ? 3 : 1);
            }
            else if (lbl == kLabelMap && w.inMogHouse) open(Kind::MogHouse); // the slot is "Mog House" there (§7.3)
            else if (lbl == kLabelRegion) open(Kind::RegionMenu); // "menu region" (0x101405c1)
            else if (lbl == kLabelSearch)
            {
                // Opening resets the criteria to the current area (CODE 0x1020a1b0(0)).
                searchQuery_      = {};
                searchQuery_.zone = w.zone;
                open(Kind::SearchMenu);
            }
            else if (lbl == kLabelMap) open(Kind::Map);
            else if (lbl == kLabelTrade && (!d.target || d.target->isPc))
            {
                tradeRequest(d.target, d, act); // a player: answer or ask (game_ui_trade.cpp)
            }
            else if (lbl == kLabelSynthesis)
            {
                open(Kind::SynthMenu);
            }
            else if (lbl == kLabelLinkshell)
            {
                open(Kind::LinkshellPick);
            }
            else if (lbl == kLabelTrade && d.target && !d.target->isPc)
            {
                tradeId_    = d.target->id;
                tradeIndex_ = d.target->index;
                tradeBoxes_ = {};
                tradeGil_   = 0;
                open(Kind::Trade);
            }
            break;
        }
        case Kind::Trade:
            if (m.cursor >= 1 && m.cursor <= 8)
            {
                open(Kind::TradeItem, m.cursor - 1);
            }
            else if (m.cursor == 9 && act.send)
            {
                // OK: C2S 0x036 ITEM_TRANSFER {UniqueNo, ItemNumTbl u32[10] @4, PropertyItemIndexTbl u8[10] @0x2C,
                // ActIndex u16 @0x36, ItemNum u8 @0x38}.
                std::vector<std::byte> pkt(0x3C);
                auto put32 = [&](std::size_t o, uint32_t v) { std::memcpy(pkt.data() + o, &v, 4); };
                put32(0, tradeId_);
                uint8_t n = 0;
                if (tradeGil_ > 0)
                {
                    put32(4, tradeGil_); // inventory slot 0 is gil
                    pkt[0x2C] = std::byte{ 0 };
                    ++n;
                }
                for (const auto& [slot, count] : tradeBoxes_)
                {
                    if (slot != 0)
                    {
                        put32(4 + 4u * n, count);
                        pkt[0x2C + n] = static_cast<std::byte>(slot);
                        ++n;
                    }
                }
                std::memcpy(pkt.data() + 0x36, &tradeIndex_, 2);
                pkt[0x38] = static_cast<std::byte>(n);
                if (n > 0)
                {
                    act.send(0x036, std::move(pkt));
                }
                stack_.clear();
            }
            else if (m.cursor == 10)
            {
                stack_.pop_back(); // Cancel
            }
            break;
        case Kind::ShopMain:
            open(m.cursor == 1 ? Kind::Shop : Kind::SellPick);
            break;
        case Kind::GuildMain:
        {
            // Buy (1) / Sell (2) (CODE 0x101ed2e0): a list that has not arrived complete is asked for (C2S 0x0AB / 0x0AD,
            // empty; 0x100eb030 / 0x100eb060) and the list window opens to fill as S2C 0x083 / 0x085 arrive.
            const std::size_t which = m.cursor == 1 ? 0 : 1;
            const auto&       list  = w.guild.lists[which];
            if ((list.state != 1 || list.items.empty()) && act.send)
            {
                act.send(which == 0 ? 0x0AB : 0x0AD, {});
            }
            open(which == 0 ? Kind::GuildBuy : Kind::GuildSell);
            break;
        }
        case Kind::GuildBuyQty:
        {
            const auto& items = w.guild.lists[0].items;
            if (m.cursor == 1 && act.send && static_cast<std::size_t>(m.param) < items.size())
            {
                // C2S 0x0AA GUILD_BUY {u16 ItemNo, u8 PropertyItemIndex = 0, u8 ItemNum} (CODE 0x100eb110).
                act.send(0x0AA, client::guildTradePayload(items[static_cast<std::size_t>(m.param)].item, 0, static_cast<uint8_t>(shopQty_)));
            }
            stack_.pop_back(); // back to the goods
            break;
        }
        case Kind::GuildSellQty:
            if (m.cursor == 1 && act.send)
            {
                // C2S 0x0AC GUILD_SELL {u16 ItemNo, u8 PropertyItemIndex, u8 ItemNum} (CODE 0x101ecf90 -> 0x100eb110).
                for (const auto& it : w.inventory)
                {
                    if (it.container == 0 && it.slot == m.param && it.slot != 0)
                    {
                        act.send(0x0AC, client::guildTradePayload(it.itemId, it.slot, static_cast<uint8_t>(std::min<uint32_t>(shopQty_, it.count))));
                    }
                }
            }
            stack_.pop_back(); // back to the inventory list
            break;
        case Kind::ShopSell:
            if (m.cursor == 1 && act.send && w.sellQuote.slot == m.param)
            {
                // C2S 0x085 SHOP_SELL_SET {u16 SellFlag = 1}: sell at the quoted price. LSB requires the packet just
                // before it to be 0x084, and position updates go out meanwhile, so the request is repeated first.
                for (const auto& it : w.inventory)
                {
                    if (it.container == 0 && it.slot == m.param)
                    {
                        std::vector<std::byte> req(8);
                        const uint32_t count = it.count; // the whole stack, as asked (0x03D's Count is not filled by LSB)
                        std::memcpy(req.data(), &count, 4);
                        std::memcpy(req.data() + 4, &it.itemId, 2);
                        req[6] = static_cast<std::byte>(m.param);
                        act.send(0x084, std::move(req));
                    }
                }
                std::vector<std::byte> pkt(4);
                pkt[0] = std::byte{ 1 };
                act.send(0x085, std::move(pkt));
            }
            stack_.pop_back();
            break;
        case Kind::ShopBuy:
            if (m.cursor == 1 && act.send && static_cast<std::size_t>(m.param) < w.shop.size())
            {
                // C2S 0x083 SHOP_BUY {u32 ItemNum, u16 ShopNo, u16 ShopItemIndex @6, u8 PropertyItemIndex @8 (must be 0)}.
                std::vector<std::byte> pkt(0x0C);
                const uint32_t count = shopQty_;
                const uint16_t index = w.shop[static_cast<std::size_t>(m.param)].index;
                std::memcpy(pkt.data(), &count, 4);
                std::memcpy(pkt.data() + 6, &index, 2);
                act.send(0x083, std::move(pkt));
            }
            stack_.pop_back(); // back to the goods
            break;
        case Kind::Linkshell:
            if (e->text1 == kHelpLinkActivate && act.send)
            {
                // C2S 0x0C4 GROUP_COMLINK_ACTIVE {u16 rgba @0, ItemIndex @2, Category @3, ActiveFlg @4 (1 equip, 0 unequip),
                // pad[3], sComLinkName[15] @8, LinkshellId @0x17 (1 or 2)}. Active already (equip slot 16/17): unequip.
                const bool active1 = w.equipment[16].first == 0 && w.equipment[16].second == linkSlot_ && linkSlot_ != 0;
                const bool active2 = w.equipment[17].first == 0 && w.equipment[17].second == linkSlot_ && linkSlot_ != 0;
                std::vector<std::byte> pkt(0x18);
                pkt[1]    = std::byte{ 0xF0 }; // colour alpha nibble = 15 (LSB rejects anything else)
                pkt[2]    = static_cast<std::byte>(linkSlot_);
                pkt[3]    = std::byte{ 0 }; // inventory
                pkt[4]    = static_cast<std::byte>(active1 || active2 ? 0 : 1);
                pkt[0x17] = static_cast<std::byte>(active2 ? 2 : 1);
                act.send(0x0C4, std::move(pkt));
                stack_.clear();
            }
            break;
        case Kind::SynthMenu:
            if (e->text1 == kHelpSynthesize)
            {
                open(Kind::SynthCrystal);
            }
            else if (e->text1 == kHelpSynthHistory && !synthHistory_.empty())
            {
                open(Kind::SynthHistory);
            }
            break;
        case Kind::Synth:
            if (m.cursor >= 1 && m.cursor <= 8)
            {
                open(Kind::TradeItem, m.cursor - 1);
            }
            else if (m.cursor == 9 && act.send)
            {
                sendSynthesis(act, w);
                stack_.clear();
            }
            else if (m.cursor == 10)
            {
                stack_.pop_back(); // Cancel
            }
            break;
        case Kind::PlayerTrade:
            tradeConfirm(m, d, act); // game_ui_trade.cpp
            break;
        case Kind::MapMarkerEdit:
        {
            // CODE 0x101fbfb0: 1 centre the map on the marker (0x101f8990; this map does not scroll), 2 delete it
            // (0x101f8950) or place it (0x101f86d0: a cursor on the map), 3 rename it (0x101fb940); each closes "menu mapv2".
            const auto slot = static_cast<std::size_t>(m.param);
            const int  zone = mapZone_, mapId = mapId_;
            auto&      s    = markerSlots(zone, mapId)[slot];
            if (!implemented(m, *e))
            {
                break;
            }
            stack_.pop_back();
            if (e->id == 2 && s.present)
            {
                s.present = false;
                saveMarkerSlots(zone, mapId);
            }
            else if (e->id == 2)
            {
                markerPlacing_ = static_cast<int>(slot);
                markerCursorU_ = markerCursorV_ = 256; // the map view's centre (0x101f85b0)
            }
            else if (e->id == 3)
            {
                askText("Name", [this, slot, zone, mapId](std::string n) {
                    markerSlots(zone, mapId)[slot].name = n.substr(0, 15);
                    saveMarkerSlots(zone, mapId);
                });
            }
            break;
        }
        case Kind::Map:
            if (e->text1 == 690)
            {
                pending_.push_front(Cmd::Right); // same as Right: the next map of this zone
            }
            else if (e->text1 == 308)
            {
                // Markers (CODE 0x101f7810 element 1): "menu mapv3". The first time for this map, C2S 0x0D2 {ZoneNo}
                // asks for the members' positions (0x101f8c20 -> 0x100f05f0); S2C 0x0A0 answers per member.
                if (!markerAsked_ && act.send)
                {
                    std::vector<std::byte> p(4);
                    const uint32_t         zone = w.zone;
                    std::memcpy(p.data(), &zone, sizeof zone);
                    act.send(0x0D2, std::move(p));
                    markerAsked_ = true;
                }
                // The list opens on the page it was last on (mapframe +0x106), or the next one with markers.
                const auto members = mapMarkerMembers(w, d.nearby).size();
                if (!mapMarkerPageAvailable(markerListPage_, haveMap(d), members))
                {
                    markerListPage_ = mapMarkerStepPage(markerListPage_, 1, haveMap(d), members);
                }
                markerPage_ = markerListPage_;
                open(Kind::MapMarkers);
            }
            break;
        case Kind::ChatCtrl:
        {
            // CODE 0x1012d1e0: each row starts the input line in a chat mode (0x101ad220(mode, name)); row 2 is a
            // tell to the window's name, by default the current target (+0x14 = -1). Both windows close.
            // Rows 1..7: say, tell, party, linkshell, linkshell 2, yell, shout (game modes 2, 4, 5, 6, 12, 11, 3).
            static constexpr int kRowKind[8] = { -1, 0, -1, 4, 5, 27, 33, 1 };
            const int row = e->id;
            stack_.clear();
            if (row == 2)
            {
                const auto* t = d.target;
                startTyping(t && t->isPc && t->id != w.charId ? fmt::format("/tell {} ", t->name) : std::string("/tell "));
            }
            else if (row >= 1 && row <= 7)
            {
                chatKind_ = static_cast<uint8_t>(kRowKind[row]);
                startTyping("");
            }
            break;
        }
        case Kind::MagicMenu:
        case Kind::MagicSelect:
            if (const int type = magicCategory(*e); type > 0 && implemented(m, *e))
            {
                open(Kind::Magic, type);
            }
            break;
        case Kind::BattleMenu:
        {
            // By entry code (table 0x1036f250).
            const auto* t    = d.target;
            const int   code = e->id >= 1 && e->id <= battleCodes_.size() ? battleCodes_[e->id - 1] : 0;
            switch (code)
            {
                case 1: // attack
                    if (t)
                    {
                        act.action(t->id, t->index, 2, 0); // engage
                    }
                    stack_.clear();
                    break;
                case 18:
                    // Switch Target (CODE 0x1012b9b2): the default range {30 yalms, 3D} (0x10157780) and the sub-target
                    // cursor on monsters only (valid-target bit 0x20, 0x10159370); the pick runs "/attack %d"
                    // (callback 0x1012bda0), which changes the battle target.
                    stack_.clear();
                    beginSubTarget("/attack <stnpc>", "<stnpc>", 0x20, d);
                    break;
                case 3: // disengage (AttackOff; the target as /attackoff sends it)
                    act.action(t ? t->id : w.charId, t ? t->index : w.actIndex, 4, 0);
                    stack_.clear();
                    break;
                case 4: // chat
                    stack_.clear();
                    startTyping("");
                    break;
                case 5: // tell: "/tell <name> " in the input line
                    stack_.clear();
                    startTyping(t ? fmt::format("/tell {} ", t->name) : std::string());
                    break;
                // Abilities opens "menu abiselec", Magic the spell list itself (CODE 0x1012b8ce, 0x1012b8e7).
                case 7: open(Kind::AbilitySelect); break;
                case 8: open(Kind::Magic, 0); break;
                case 25: open(Kind::Magic, 8); break; // trust spells
                case 9: open(Kind::Items); break;
                case 12: open(Kind::Treasure); break;
                case 10:
                case 11:
                    stack_.clear();
                    runLine("/check");
                    break;
                case 16:
                    stack_.clear();
                    if (t)
                    {
                        runLine(fmt::format("/pcmd add {}", t->name));
                    }
                    break;
                case 19:
                    tradeRequest(t, d, act); // as the main menu's Trade
                    break;
                default: break;
            }
            break;
        }
        case Kind::TreasureOp:
            if (act.send && m.param >= 0 && m.param < 10)
            {
                // Lot: C2S 0x041 {TrophyItemIndex, PropertyItemIndex, pad[2]}; Pass: 0x042 {TrophyItemIndex, pad}.
                std::vector<std::byte> pkt(e->id == 1 ? 4 : 2);
                pkt[0] = static_cast<std::byte>(m.param);
                act.send(e->id == 1 ? 0x041 : 0x042, std::move(pkt));
            }
            stack_.pop_back();
            break;
        case Kind::Auction:
            if (e->id == 1)
            {
                open(Kind::AucGroups);
            }
            else if (e->id == 2)
            {
                open(Kind::AucSellPick);
            }
            else if (e->id == 3 && act.send)
            {
                act.send(0x04E, aucPacket(0x05, -1)); // Info: the sale slots come back
                open(Kind::AucSales);
            }
            break;
        case Kind::AucGroups:
        case Kind::AucSub:
        {
            // Element -> LSB aH category (> 0) or a sub-window (-1 - index into the AucSub layouts).
            static const std::array<std::vector<int>, 9> kMap = { {
                { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, -7 }, // weapons (15: ammo & misc.)
                { 16, 17, 22, 18, 19, 23, 20, 21, 26, 24, 25 },          // armor
                { 28, 29, 32, 31, 30, 60, 45 },                          // magic scrolls
                { 38, 39, 40, 41, 42, 43, 44, 63 },                      // materials
                { -6, 59, 51 },                                          // food (1: meals)
                { 52, 53, 54, 55, 56, 57, 58 },                          // meals
                { 15, 47, 48, 62 },                                      // ammo & misc.
                { 46, 50, 36, 49, 37, 61, 64, 65 },                      // others
                { -1, -2, -3, 33, 34, -4, -5, 35, -8 },                  // the groups
            } };
            const auto& row = kMap[m.kind == Kind::AucGroups ? 8 : static_cast<std::size_t>(std::clamp(m.param, 0, 7))];
            const auto  i   = static_cast<std::size_t>(e->id - 1);
            if (i < row.size())
            {
                if (row[i] < 0)
                {
                    open(Kind::AucSub, -1 - row[i]);
                }
                else
                {
                    aucRebuildQuery(row[i]);
                    aucRequestList(act, row[i]);
                    open(Kind::AucList, row[i]);
                }
            }
            break;
        }
        case Kind::AucItem:
        case Kind::AucSale:
            if (e->text1 == 527 && act.auctionHistory) // View recent sales data
            {
                act.auctionHistory(aucItem_, aucStack_);
                open(Kind::AucHistory);
            }
            else if (e->text1 == 528) // Place a bid
            {
                askNumber("Specify your bid.", [this](uint32_t price) {
                    aucPrice_ = price;
                    open(Kind::AucYesNo, 0);
                });
            }
            else if (e->text1 == 530 && act.send) // Remove merchandise from auction
            {
                act.send(0x04E, aucPacket(0x0C, static_cast<int8_t>(m.param)));
                stack_.pop_back();
            }
            else if (e->text1 == 529 && aucListCategory_ >= 0) // Rearrange the order of listed items (CODE 0x101d8f2c)
            {
                // The list's kind (0x101d8db0): categories 1..15 and 48 weapons, 16..26 armor, the rest by name only.
                const int c = aucListCategory_;
                if ((c >= 1 && c <= 15) || c == 48)
                {
                    open(Kind::AucSort, 0);
                }
                else if (c >= 16 && c <= 26)
                {
                    open(Kind::AucSort, 1);
                }
                else
                {
                    // Others toggle a name sort: off when any sort is stored (clear, rebuild), else name ascending.
                    aucLoadSorts();
                    auto& stored = aucSorts_[static_cast<std::size_t>(c) % aucSorts_.size()];
                    if (!stored.empty())
                    {
                        stored.clear();
                        aucRebuildQuery(c);
                    }
                    else
                    {
                        aucAddTerm(9, false);
                        stored.push_back(9);
                    }
                    aucSaveSorts();
                    aucRequestList(act, c);
                    stack_.pop_back(); // back to the list, at its top (the new list replaces the old one)
                    if (!stack_.empty() && stack_.back().kind == Kind::AucList)
                    {
                        stack_.back().cursor = 1;
                        stack_.back().scroll = 0;
                    }
                }
            }
            break;
        case Kind::AucSort:
        {
            // wepsort (CODE 0x101da5c0) / armsort (0x101da780), row 1..6: Return clears the stored sorts; the others add a
            // query term and re-ask the server. Sorts are stored (AUCSORT.DAT) and replayed on the next list; race / job
            // filters only last for this list (0x101d72a0 does not store keys 3 and 4).
            const int c   = aucListCategory_;
            const int row = e->id;
            aucLoadSorts();
            auto& stored = aucSorts_[static_cast<std::size_t>(std::max(c, 0)) % aucSorts_.size()];
            auto  store  = [&](uint8_t key) {
                if (stored.size() < 20 && std::ranges::find_if(stored, [key](uint8_t b) { return (b & 0x0F) == key; }) == stored.end())
                {
                    stored.push_back(key);
                }
            };
            const bool armor = m.param == 1;
            if (row == 1)
            {
                stored.clear();
                aucRebuildQuery(c);
            }
            else if (row == 2)
            {
                aucAddTerm(armor ? 7 : 5, true); // highest defense / damage
                store(armor ? 7 : 5);
            }
            else if (row == 3)
            {
                aucAddTerm(armor ? 8 : 6, armor); // highest elemental resistance / shortest delay
                store(armor ? 8 : 6);
            }
            else if (row == 4)
            {
                aucAddTerm(2, true); // highest required level
                store(2);
            }
            else if (row == 5)
            {
                aucAddTerm(4, false, w.mainJob); // usable by the current job (filter)
            }
            else if (row == 6)
            {
                aucAddTerm(3, false, static_cast<int>(w.look.ids[0] >> 8)); // usable by our race (filter)
            }
            if (row >= 1 && row <= 6)
            {
                aucSaveSorts();
                aucRequestList(act, c);
                // Close the sort window and auc3, back to the list (0x101da740 / 0x101da74f).
                while (!stack_.empty() && stack_.back().kind != Kind::AucList)
                {
                    stack_.pop_back();
                }
                if (!stack_.empty())
                {
                    stack_.back().cursor = 1;
                    stack_.back().scroll = 0;
                }
            }
            break;
        }
        case Kind::AucYesNo:
            if (e->id == 1 && act.send)
            {
                if (m.param == 0)
                {
                    // Bid {u32 BidPrice, u16 ItemNo, u16 pad, u32 ItemStacks 1 single / 0 stack}.
                    auto pkt = aucPacket(0x0E, 0);
                    std::memcpy(pkt.data() + 4, &aucPrice_, 4);
                    std::memcpy(pkt.data() + 8, &aucItem_, 2);
                    const uint32_t stacks = aucStack_ ? 0 : 1;
                    std::memcpy(pkt.data() + 0x0C, &stacks, 4);
                    act.send(0x04E, std::move(pkt));
                }
                else
                {
                    // LotIn {u32 LimitPrice, u16 ItemWorkIndex, u16 pad, u32 ItemStacks} into a free sale slot.
                    int free = -1;
                    for (int k = 6; k >= 0; --k)
                    {
                        if (w.auction.sales[static_cast<std::size_t>(k)].itemId == 0)
                        {
                            free = k;
                        }
                    }
                    if (free >= 0)
                    {
                        auto pkt = aucPacket(0x0B, static_cast<int8_t>(free));
                        std::memcpy(pkt.data() + 4, &aucPrice_, 4);
                        const uint16_t idx = aucSellSlot_;
                        std::memcpy(pkt.data() + 8, &idx, 2);
                        const uint32_t stacks = aucStack_ ? 0 : 1;
                        std::memcpy(pkt.data() + 0x0C, &stacks, 4);
                        act.send(0x04E, std::move(pkt));
                    }
                    else if (act.note)
                    {
                        act.note("You cannot place any more items on auction.");
                    }
                }
                while (!stack_.empty() && stack_.back().kind != Kind::Auction)
                {
                    stack_.pop_back();
                }
            }
            else
            {
                stack_.pop_back();
            }
            break;
        case Kind::Party:
            if (e->text1 == kHelpLeaveParty && act.send)
            {
                act.send(0x06F, std::vector<std::byte>(4)); // GROUP_LEAVE {Kind 0 = party}
                stack_.clear();
            }
            else if (e->text1 == kHelpAddMember && d.target && d.target->isPc && act.send)
            {
                // Invite the target: C2S 0x06E {UniqueNo, ActIndex, Kind 0}.
                std::vector<std::byte> pkt(8);
                std::memcpy(pkt.data(), &d.target->id, 4);
                std::memcpy(pkt.data() + 4, &d.target->index, 2);
                act.send(0x06E, std::move(pkt));
                stack_.clear();
            }
            else if (e->text1 == kHelpChangeLeader || e->text1 == kHelpRemoveMember)
            {
                open(Kind::PartyPick, e->text1 == kHelpRemoveMember ? 1 : 0);
            }
            else if (e->text1 == kHelpDisband && act.send)
            {
                act.send(0x070, std::vector<std::byte>(4)); // GROUP_BREAKUP {Kind 0 = party}
                stack_.clear();
            }
            else if ((e->text1 == kHelpAcceptInvite || e->text1 == kHelpDeclineInvite) && act.answerInvite)
            {
                act.answerInvite(e->text1 == kHelpAcceptInvite);
                stack_.clear();
            }
            break;
        case Kind::Equip:
            // Element sprites 40..55 of "menu framesus" are the slot placeholders, in grid order.
            if (const auto* sub = e->subFor(0); sub && sub->shape >= 40 && sub->shape < 56)
            {
                open(Kind::EquipList, kEquipCellSlot[sub->shape - 40]);
            }
            break;
        case Kind::ItemUse:
        {
            const uint8_t c = static_cast<uint8_t>(m.param >> 8), s = static_cast<uint8_t>(m.param & 0xFF);
            if (m.cursor == 1)
            {
                // Use (help 26): things the menu can use, and enchanted gear; gear is not equipped from here (the
                // Equipment window and /equip do that). An item for others picks its target first (help 4 "Select
                // target to use item on."), as /item "name" <st>.
                if (!implemented(m, *e))
                {
                    return;
                }
                for (const auto& it : w.inventory)
                {
                    if (it.container == c && it.slot == s)
                    {
                        const auto info = d.items ? d.items->find(it.itemId) : std::nullopt;
                        if (info && (info->targets & ~0x01) != 0 && !info->equipment())
                        {
                            menuSubTarget("/item", info->name, info->targets, d);
                        }
                        else if (info)
                        {
                            act.useItem(c, s, w.charId, w.actIndex);
                            stack_.clear();
                        }
                        return;
                    }
                }
            }
            else
            {
                open(Kind::DropConfirm, m.param); // "Drop": "Dispose of item?" first, on No (#119)
                stack_.back().cursor = yesNoCursor(kDropConfirmOnYes);
            }
            break;
        }
        case Kind::DropConfirm:
        case Kind::DropAgain:
        {
            // Yes (element 1): C2S 0x028 ITEM_DUMP {ItemNum u32, Category u8, ItemIndex u8}, the whole stack. A Rare / Ex
            // item or a linkshell asks again first (CODE 0x101f2c70; ui/yes_no.h).
            const uint8_t c = static_cast<uint8_t>(m.param >> 8), s = static_cast<uint8_t>(m.param & 0xFF);
            const bool    yes = yesNoAnswer(true, false, m.cursor).value_or(false);
            if (yes && m.kind == Kind::DropConfirm)
            {
                bool again = false;
                for (const auto& it : w.inventory)
                {
                    if (it.container == c && it.slot == s)
                    {
                        const auto info = d.items ? d.items->find(it.itemId) : std::nullopt;
                        again           = dropAsksAgain(info ? info->flags : 0, it.itemId);
                    }
                }
                if (again)
                {
                    const int param = m.param;
                    stack_.pop_back();
                    open(Kind::DropAgain, param);
                    stack_.back().cursor = yesNoCursor(kDropAgainOnYes);
                    break;
                }
            }
            if (yes && act.send)
            {
                for (const auto& it : w.inventory)
                {
                    if (it.container == c && it.slot == s)
                    {
                        std::vector<std::byte> p(8);
                        const uint32_t n = it.count;
                        std::memcpy(p.data(), &n, 4);
                        p[4] = static_cast<std::byte>(c);
                        p[5] = static_cast<std::byte>(s);
                        act.send(0x028, std::move(p));
                    }
                }
            }
            // Back to the item list either way.
            stack_.pop_back();
            if (!stack_.empty() && stack_.back().kind == Kind::ItemUse)
            {
                stack_.pop_back();
            }
            break;
        }
        case Kind::Abilities:
        case Kind::AbilitySelect:
        {
            // Rows: Job Traits (164), Job Abilities (161), Weapon Skills (160), Ranged Attack, Pet Commands, ... (labels in "menu keytops3").
            const auto* sub = e->subFor(0);
            if (sub && sub->shape == 162 && implemented(m, *e) && d.target)
            {
                act.action(d.target->id, d.target->index, 0x10, 0); // ranged attack
                stack_.clear();
            }
            else if (sub && sub->shape == 164) open(Kind::AbilityList, 2);
            else if (sub && sub->shape == 161) open(Kind::AbilityList, 0);
            else if (sub && sub->shape == 160) open(Kind::AbilityList, 1);
            else if (sub && sub->shape == 163 && implemented(m, *e)) open(Kind::AbilityList, 3);
            break;
        }
        default: break;
    }
}

void GameUi::apply(Cmd c, const GameUiData& d, const GameUiActions& act)
{
    if (stack_.empty())
    {
        return;
    }
    auto& m = stack_.back();
    if (markerPlacing_ >= 0)
    {
        // Placing an own marker (CODE 0x101f86d0 / 0x101f8720): the arrows move the map cursor (8 map pixels a step:
        // INFERRED from the cursor's +0x70 = 8.0), Enter places the marker there and asks its name, Esc cancels.
        constexpr float kStep = 8;
        switch (c)
        {
            case Cmd::Up: markerCursorV_ = std::max(0.0f, markerCursorV_ - kStep); break;
            case Cmd::Down: markerCursorV_ = std::min(511.0f, markerCursorV_ + kStep); break;
            case Cmd::Left: markerCursorU_ = std::max(0.0f, markerCursorU_ - kStep); break;
            case Cmd::Right: markerCursorU_ = std::min(511.0f, markerCursorU_ + kStep); break;
            case Cmd::Confirm:
            {
                const auto slot  = static_cast<std::size_t>(markerPlacing_);
                const int  zone  = mapZone_, mapId = mapId_;
                auto&      s     = markerSlots(zone, mapId)[slot];
                s.x              = static_cast<int16_t>(std::lround(markerCursorU_));
                s.y              = static_cast<int16_t>(std::lround(markerCursorV_));
                s.present        = true;
                markerPlacing_   = -1;
                markerPage_      = 0;
                markerListPage_  = 0;
                saveMarkerSlots(zone, mapId);
                askText("Name", [this, slot, zone, mapId](std::string n) {
                    markerSlots(zone, mapId)[slot].name = n.substr(0, 15);
                    saveMarkerSlots(zone, mapId);
                });
                break;
            }
            case Cmd::Back: markerPlacing_ = -1; break;
            default: break;
        }
        return;
    }
    if (sound_)
    {
        // Confirming a dimmed entry: the error sound (4) instead of confirm (2).
        bool dimmed = false;
        if (c == Cmd::Confirm && isList(m.kind))
        {
            const auto list = rows(m, d, act);
            const auto i    = static_cast<std::size_t>(m.scroll + m.cursor - 1);
            dimmed          = i < list.size() && !list[i].enabled;
        }
        else if (c == Cmd::Confirm)
        {
            const auto* l = layout(m);
            const auto* e = l ? l->element(static_cast<uint16_t>(m.cursor)) : nullptr;
            dimmed        = e && !implemented(m, *e);
        }
        // Each window's sound switches (+0x84 cancel, +0x85 cursor, +0x86 confirm; CODE 0x101182d3 / 0x1011833f /
        // 0x1011836e): the battle menu turns its cancel sound off (0x1012ad72), an event's "menu query" its cursor
        // sound (0x1014dd99).
        const bool cursorMove = c != Cmd::Confirm && c != Cmd::Back;
        if (!(c == Cmd::Back && m.kind == Kind::BattleMenu) && !(cursorMove && m.kind == Kind::EventMenu))
        {
            sound_(c == Cmd::Confirm ? (dimmed ? 4u : 2u) : c == Cmd::Back ? 3u : 1u);
        }
    }
    if (sliderEdit_ != 0 && !isConfigPage(m.kind))
    {
        sliderEdit_ = 0;
    }
    if (sliderEdit_ != 0)
    {
        // Editing a Config slider: left / right step the value and set it at once; select or cancel stop.
        if (c == Cmd::Back)
        {
            sliderEdit_ = 0;
            return;
        }
        if (c == Cmd::Left || c == Cmd::Right)
        {
            if (const auto* b = cfgBinding(m.kind, static_cast<uint16_t>(sliderEdit_)); b && act.setConfig)
            {
                int cur = cfgValue(d, b->id);
                if (cur < b->min)
                {
                    cur = cfgValue(d, b->id == 197 ? 156 : b->id == 198 ? 17 : b->id); // window 2 "as window 1"
                }
                const int v = std::clamp(cur + (c == Cmd::Right ? b->step : -b->step), b->min, b->max);
                act.setConfig(b->id, v);
                // Max and min lines push each other (CODE 0x101993f0).
                const int maxId = b->id == 15 || b->id == 16 ? 15 : 183, minId = maxId + 1;
                if ((b->id == 15 || b->id == 183) && cfgValue(d, minId) > v)
                {
                    act.setConfig(minId, v);
                }
                if ((b->id == 16 || b->id == 184) && cfgValue(d, maxId) < v)
                {
                    act.setConfig(maxId, std::max(v, 4));
                }
            }
            return;
        }
        if (c != Cmd::Confirm)
        {
            return;
        }
    }
    if (m.kind == Kind::FontColorEdit && fontEdit_.channel != 0 && c != Cmd::Confirm)
    {
        // An RGB bar: left / right -4 / +4 within 0..255 (CODE 0x10198a90 / 0x10198b60); cancel stops editing.
        if (c == Cmd::Back)
        {
            fontEdit_.channel = 0;
        }
        else if (c == Cmd::Left || c == Cmd::Right)
        {
            int& ch = fontEdit_.rgb[static_cast<std::size_t>(fontEdit_.channel - 1)];
            ch      = std::clamp(ch + (c == Cmd::Right ? 4 : -4), 0, 255);
        }
        return;
    }
    // Items (#117): Select Window (Num +, selectWindow()) moves between the list and "menu itmsortw"; cancel in the
    // sort window comes back to the list, and in the list ends Manual Sort before it closes the list (item_sort_keys.h).
    if ((m.kind == Kind::Items || m.kind == Kind::ItemSort) && c == Cmd::Back &&
        itemSortFocus(itemSortMove(m.kind == Kind::ItemSort, manualSort_, ItemSortKey::Cancel)))
    {
        return;
    }
    if (tradeApply(m, c, d, act))
    {
        return;
    }
    if (c == Cmd::Back)
    {
        if (m.kind == Kind::EventMenu)
        {
            eventChoice_ = -1; // cancelled
        }
        if (m.kind == Kind::AppList || m.kind == Kind::AppElements)
        {
            appChoice_ = { appMenus_.at(static_cast<std::size_t>(m.param)).tag, -1 };
            if (m.kind == Kind::AppList && appMenus_.at(static_cast<std::size_t>(m.param)).keepStack)
            {
                stack_.pop_back(); // a page goes back one window, as the game's menus do
                return;
            }
        }
        if (m.kind == Kind::DeliveryBox && act.send)
        {
            act.send(0x04D, deliveryPacket(0x0F, -1)); // PostClose
        }
        if (m.kind == Kind::Status && statusSub_ >= 0)
        {
            statusSub_ = -1; // cancel closes the shown window first, then statcom (CODE 0x10148c4b)
            return;
        }
        stack_.pop_back();
        return;
    }
    if (c == Cmd::Confirm)
    {
        confirm(d, act);
        return;
    }
    if (m.kind == Kind::AppList && (c == Cmd::Left || c == Cmd::Right))
    {
        const auto& am = appMenus_.at(static_cast<std::size_t>(m.param));
        const auto  i  = static_cast<std::size_t>(m.scroll + m.cursor - 1);
        if (am.keepStack && i < am.rows.size() && am.rows[i].adjustable && am.rows[i].enabled)
        {
            appAdjust_ = std::tuple{ am.tag, am.rows[i].value, c == Cmd::Right ? 1 : -1 };
            return;
        }
    }
    if (m.kind == Kind::MapMarkers && (c == Cmd::Left || c == Cmd::Right))
    {
        // Left / Right step the page, skipping empty ones (CODE 0x101fa920); the new page's markers show on the map
        // (0x101fa8b0).
        markerListPage_ = mapMarkerStepPage(markerListPage_, c == Cmd::Right ? 1 : -1, haveMap(d), mapMarkerMembers(*d.world, d.nearby).size());
        markerPage_     = markerListPage_;
        m.cursor        = 1;
        m.scroll        = 0;
        return;
    }
    if (isList(m.kind))
    {
        const int n    = static_cast<int>(rows(m, d, act).size());
        const int view = std::max(1, visibleRows(m));
        int       i    = m.scroll + m.cursor - 1;
        // Item lists stop at both ends (#124, list_cursor.h); the event menu keeps wrapping (INFERRED).
        const bool wrap = m.kind == Kind::EventMenu;
        // Right / Page Down page down, Left / Page Up page up (#121, list_cursor.h).
        if (c == Cmd::Up)
            i = listCursorStep(i, n, listKeyStep(ListKey::Up, view), wrap);
        else if (c == Cmd::Down)
            i = listCursorStep(i, n, listKeyStep(ListKey::Down, view), wrap);
        else if (c == Cmd::Left)
            i = listCursorStep(i, n, listKeyStep(ListKey::Left, view));
        else if (c == Cmd::Right)
            i = listCursorStep(i, n, listKeyStep(ListKey::Right, view));
        else if (c == Cmd::PageUp)
            i = listCursorStep(i, n, listKeyStep(ListKey::PageUp, view));
        else if (c == Cmd::PageDown)
            i = listCursorStep(i, n, listKeyStep(ListKey::PageDown, view));
        if (i < m.scroll) m.scroll = i;
        if (i >= m.scroll + view) m.scroll = i - view + 1;
        m.cursor = i - m.scroll + 1;
        return;
    }
    if ((m.kind == Kind::Main || m.kind == Kind::Social) && (c == Cmd::Left || c == Cmd::Right))
    {
        flipMainPage();
        return;
    }
    if (m.kind == Kind::BattleMenu && c == Cmd::Right)
    {
        // Right runs the row code's second handler (table 0x1036f250 +8, CODE 0x1012b755): Chat / Tell open "menu
        // chatctrl" (0x1012aa10), Magic "menu magselec" (0x1012aa30); other rows have none.
        const auto i    = static_cast<std::size_t>(m.cursor - 1);
        const int  code = i < battleCodes_.size() ? battleCodes_[i] : 0;
        if (code == 4 || code == 5)
        {
            open(Kind::ChatCtrl);
        }
        else if (code == 8)
        {
            open(Kind::MagicSelect);
        }
        return;
    }
    if (isMacroKind(m.kind) && macroApply(m, c))
    {
        return;
    }
    if (m.kind == Kind::Map && (c == Cmd::Left || c == Cmd::Right) && d.maps)
    {
        // Left / Right: the zone's other maps (map ids with a record), wrapping.
        for (int step = 1; step < 64; ++step)
        {
            const int id = (mapId_ + (c == Cmd::Right ? step : -step) + 64) % 64;
            if (d.maps->find(d.world->zone, static_cast<int8_t>(id)))
            {
                mapId_ = static_cast<int8_t>(id);
                break;
            }
        }
        return;
    }
    if (m.kind == Kind::ShopBuy && (c == Cmd::Left || c == Cmd::Right || c == Cmd::PageUp || c == Cmd::PageDown))
    {
        // Quantity: Left/Right +-1, PageUp/PageDown +-10, from 1 up to what the gil allows (at most 99).
        uint32_t have = 0;
        for (const auto& it : d.world->inventory)
        {
            if (it.container == 0 && it.slot == 0)
            {
                have = it.count;
            }
        }
        const auto&    shop  = d.world->shop;
        const uint32_t price = static_cast<std::size_t>(m.param) < shop.size() ? std::max<uint32_t>(1, shop[static_cast<std::size_t>(m.param)].price) : 1;
        const int64_t  most  = std::clamp<int64_t>(have / price, 1, 99);
        const int64_t  step  = c == Cmd::Right ? 1 : c == Cmd::Left ? -1 : c == Cmd::PageUp ? 10 : -10;
        shopQty_             = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(shopQty_) + step, 1, most));
        return;
    }
    if ((m.kind == Kind::GuildBuyQty || m.kind == Kind::GuildSellQty) && (c == Cmd::Left || c == Cmd::Right || c == Cmd::PageUp || c == Cmd::PageDown))
    {
        // The guild's quantity (CODE 0x101ec750 / 0x101eceb0 -> the number window 0x1013a7a0): buying, up to a stack,
        // what the gil allows and the guild's stock; selling, up to what the stack holds and the room left in the
        // guild's stock. Left/Right +-1, PageUp/PageDown +-10 (this client's keys).
        const auto& w    = *d.world;
        int64_t     most = 1;
        if (m.kind == Kind::GuildBuyQty)
        {
            const auto& items = w.guild.lists[0].items;
            if (static_cast<std::size_t>(m.param) < items.size())
            {
                const auto& g     = items[static_cast<std::size_t>(m.param)];
                const auto  info  = d.items ? d.items->find(g.item) : std::nullopt;
                const int64_t stack = info ? std::max<int64_t>(1, info->stackSize) : 99;
                most = std::min<int64_t>({ stack, g.gil() ? gilHeld(w) / g.gil() : stack, g.count });
            }
        }
        else
        {
            for (const auto& it : w.inventory)
            {
                if (it.container == 0 && it.slot == m.param && it.slot != 0)
                {
                    const auto& list = w.guild.lists[1].items;
                    const auto  g    = std::find_if(list.begin(), list.end(), [&](const client::GuildItem& x) { return x.item == it.itemId; });
                    most             = std::min<int64_t>(it.count, g != list.end() ? g->max - g->count : it.count);
                }
            }
        }
        most               = std::clamp<int64_t>(most, 1, 99);
        const int64_t step = c == Cmd::Right ? 1 : c == Cmd::Left ? -1 : c == Cmd::PageUp ? 10 : -10;
        shopQty_           = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(shopQty_) + step, 1, most));
        return;
    }
    if (m.kind == Kind::Trade && m.cursor == 11 && c != Cmd::Up && c != Cmd::Down)
    {
        // Gil field: Left/Right +-1, PageUp/PageDown +-100, up to what we have (inventory slot 0).
        uint32_t have = 0;
        for (const auto& it : d.world->inventory)
        {
            if (it.container == 0 && it.slot == 0)
            {
                have = it.count;
            }
        }
        const int64_t step = c == Cmd::Right ? 1 : c == Cmd::Left ? -1 : c == Cmd::PageUp ? 100 : -100;
        tradeGil_          = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(tradeGil_) + step, 0, have));
        return;
    }
    if (m.kind == Kind::Trade && m.cursor >= 1 && m.cursor <= 8 && (c == Cmd::PageUp || c == Cmd::PageDown))
    {
        // PageUp / PageDown: one more / one fewer of the item in this box (up to the stack).
        auto& [slot, count] = tradeBoxes_[static_cast<std::size_t>(m.cursor - 1)];
        for (const auto& it : d.world->inventory)
        {
            if (slot != 0 && it.container == 0 && it.slot == slot)
            {
                count = c == Cmd::PageUp ? std::min<uint32_t>(it.count, count + 1) : std::max<uint32_t>(1, count - 1);
            }
        }
        return;
    }
    const auto* l = layout(m);
    const auto* e = l ? l->element(static_cast<uint16_t>(m.cursor)) : nullptr;
    if (!e)
    {
        return;
    }
    // Navigation bytes: prev, next, up, down, right, left (element ids; 0xFF = none).
    const int dir  = c == Cmd::Up ? 2 : c == Cmd::Down ? 3 : c == Cmd::Right ? 4 : c == Cmd::Left ? 5 : -1;
    if (dir < 0)
    {
        return;
    }
    int next = e->nav[static_cast<std::size_t>(dir)];
    // Top / bottom of a vertical menu wraps around, as in the game.
    if (next == m.cursor && (c == Cmd::Up || c == Cmd::Down))
    {
        int lo = 1000, hi = 0;
        for (const auto& el : l->elements)
        {
            if (el.nav[2] != 0xFF)
            {
                lo = std::min<int>(lo, el.id);
                hi = std::max<int>(hi, el.id);
            }
        }
        next = c == Cmd::Up ? hi : lo;
    }
    if (next != 0xFF && l->element(static_cast<uint16_t>(next)))
    {
        m.cursor = next;
    }
}

void GameUi::updateAppPage(int tag, std::vector<AppRow> rows, std::optional<std::string> windowTitle)
{
    for (auto& m : stack_)
    {
        if (m.kind != Kind::AppList)
        {
            continue;
        }
        auto& am = appMenus_.at(static_cast<std::size_t>(m.param));
        if (am.tag != tag || !am.keepStack)
        {
            continue;
        }
        am.rows = std::move(rows);
        if (windowTitle)
        {
            am.windowTitle = std::move(*windowTitle);
        }
        const int n = static_cast<int>(am.rows.size());
        if (m.scroll + m.cursor > n)
        {
            m.scroll = 0;
            m.cursor = std::max(1, n);
        }
        return;
    }
}

bool GameUi::appPageOpen(int tag) const
{
    return std::any_of(stack_.begin(), stack_.end(), [&](const Menu& m) {
        return m.kind == Kind::AppList && appMenus_.at(static_cast<std::size_t>(m.param)).tag == tag &&
               appMenus_.at(static_cast<std::size_t>(m.param)).keepStack;
    });
}

// --- drawing ---

void GameUi::addonLogHeight(int win, std::optional<int> height)
{
    auto& hold = logHolds_[static_cast<std::size_t>(win & 1)];
    if (height)
    {
        hold.write(*height, std::chrono::steady_clock::now());
    }
    else
    {
        hold.release();
    }
}

void GameUi::drawChat(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    // Log windows (config_log_re.md §3). Window 1 sits bottom-left; with Config "multiple log windows" (cfg 180) window 2
    // goes above it (1, vertical) or to its right (2, horizontal). Each is as wide as its column width setting and as
    // tall as its lines: with auto-resize it shrinks toward its min lines one step per interval once text stops
    // arriving, and grows back to max lines on new text. The full-screen log ("menu fulllog", 366x384, a header row at
    // the top) replaces window 1 while it is open.
    const bool      full = logMode_ == LogMode::Full;
    constexpr float kLine = 16, kPad = 4; // 16 per line, text at left + 4 (CODE 0x101621f0)
    const int       multi = std::clamp(cfgValue(d, 180), 0, 2);
    const auto&     layouts = r_.resources().layouts;
    auto width = [&](std::string_view s) {
        float w = 0;
        for (std::size_t i = 0; i < s.size();)
        {
            w += r_.cellAdvance(UiRenderer::mojiCell(s, i));
        }
        return w;
    };
    // The lines the log keeps, filtered, with their colour, timestamp and Log Settings bit.
    struct Src
    {
        std::string text;
        std::size_t stamp = 0; // leading timestamp characters (drawn in the basic system message colour)
        UiColor     colour;
        int         bit  = -1;
        uint8_t     kind = 0; // client::ChatLine::kind (the full log's channel tabs)
    };
    std::vector<Src> src;
    const auto&      chat  = d.world->chat;
    const auto       first = chat.size() > 200 ? chat.size() - 200 : 0;
    const bool       newLine = !chat.empty() && chat.back().at != chatNewest_; // a line arrived since the last frame
    if (newLine)
    {
        chatNewest_ = chat.back().at;
    }
    const bool lineOpened = typing_ && !logTyping_; // the chat line opened this frame
    logTyping_            = typing_;
    chatScroll_ = logScrollAfter(chatScroll_, newLine, full, lineOpened); // the main log follows the newest line (#104, #105)
    const int        stampMode = std::clamp(cfgValue(d, 181), 0, 2);
    for (std::size_t i = first; i < chat.size(); ++i)
    {
        const auto& line = chat[i];
        if ((hideYell_ && line.kind == 26) || (hideAssistJ_ && line.kind == 0x22) || (hideAssistE_ && line.kind == 0x23))
        {
            continue; // /yellsw, /assistjsw, /assistesw
        }
        // Config Chat Filters, log-only bits (cfg 52; config_re.md): others' lines of these channels stay out of the log.
        if (!line.from.empty() && line.from != d.world->charName)
        {
            int bit = -1;
            switch (line.kind)
            {
                case 0: bit = 0; break;     // say
                case 1: bit = 1; break;     // shout
                case 3: bit = 2; break;     // tell
                case 4: bit = 3; break;     // party
                case 5: bit = 4; break;     // linkshell
                case 8: bit = 5; break;     // emote
                case 33: bit = 7; break;    // unity
                case 27: bit = 8; break;    // linkshell 2
                case 0x22: bit = 9; break;  // assist (J)
                case 0x23: bit = 10; break; // assist (E)
                default: break;
            }
            if (bit >= 0 && ((cfgValue(d, 52) >> bit) & 1))
            {
                continue;
            }
        }
        if (line.message != 0 && cfgValue(d, 19) == 2 && client::damageCategory(line.message) != 0)
        {
            continue; // Config damage display "screen only": the pop-up shows it instead (CODE 0x100ad820)
        }
        if (line.hold && d.shownResults && !d.shownResults->contains(line.hold) &&
            std::chrono::system_clock::now() - line.at < std::chrono::seconds(4))
        {
            continue; // a battle result whose effect has not shown the damage yet (4 s at most)
        }
        // Speaker format by channel, as the game's log shows it: party "(Name) text", linkshell "<Name> text",
        // tells "Name>> text", others "Name : text".
        std::string text = line.text;
        if (!line.from.empty())
        {
            switch (line.kind)
            {
                case 4: text = "(" + line.from + ") " + line.text; break;
                case 5:
                case 27: text = "<" + line.from + "> " + line.text; break;
                case 3: text = line.from + ">> " + line.text; break;
                default: text = line.from + " : " + line.text; break;
            }
        }
        // An Ashita addon may change or hide a line (text_in); its FFXI colour codes (0x1E / 0x1F and a byte) are
        // dropped and its line breaks drawn as spaces.
        if (d.lineEdit)
        {
            if (const auto ed = d.lineEdit(line.serial))
            {
                if (ed->first)
                {
                    continue;
                }
                text.clear();
                for (std::size_t k = 0; k < ed->second.size(); ++k)
                {
                    const char ch = ed->second[k];
                    if (ch == '\x1E' || ch == '\x1F')
                    {
                        ++k;
                        continue;
                    }
                    text.push_back(ch == '\n' || ch == '\x07' || ch == '\r' ? ' ' : ch);
                }
            }
        }
        // Config timestamps (cfg 181: 1 HH:MM, 2 HH:MM:SS): "[time] " before the line (CODE 0x10128c10).
        std::string stamp;
        if (stampMode != 0)
        {
            const std::time_t tt = std::chrono::system_clock::to_time_t(line.at);
            char              buf[16];
            std::strftime(buf, sizeof(buf), stampMode == 1 ? "[%H:%M] " : "[%H:%M:%S] ", std::localtime(&tt));
            stamp = buf;
        }
        const int  colourId = line.colour != 0 ? line.colour : lineColourId(line.kind);
        src.push_back({ stamp + text, stamp.size(), colourId ? cfgColour(d, colourId) : kWhite, line.colour != 0 ? (line.colour >= 35 ? line.colour - 2 : -1) : lineBit(line.kind),
                        line.kind });
    }
    // Routing (CODE 0x10129ae0): a line shows in window 1 when there is one window, when it has no bit or its bit is
    // set in window 1's mask (185 / 186); in window 2 when its bit is set in window 2's mask (187 / 188).
    auto inWindow = [&](const Src& l, int win) {
        if (win == 0 && (multi == 0 || l.bit < 0))
        {
            return true;
        }
        if (l.bit < 0)
        {
            return false;
        }
        const auto mask = static_cast<uint32_t>(cfgValue(d, (win == 0 ? 185 : 187) + (l.bit >= 32 ? 1 : 0)));
        return ((mask >> (l.bit % 32)) & 1) != 0;
    };
    // The full-screen log shows one channel tab's lines (0x10129920): tab 0 window 1's, tab 8 window 2's, the others
    // a channel's.
    logWin2_ = multi != 0;
    if (logFilter_ == 8 && !logWin2_)
    {
        logFilter_ = 0; // window 2 switched off while its tab was shown
    }
    auto inFull = [&](const Src& l) {
        return logFilter_ == 0 ? inWindow(l, 0) : logFilter_ == 8 ? inWindow(l, 1) : fullLogFilterMatches(logFilter_, l.kind);
    };
    const auto& sl    = src;
    const auto  now   = std::chrono::steady_clock::now();
    if (multi == 0)
    {
        logObjects_[1] = {}; // no window 2
    }
    const float uiW   = c.width;
    ImVec2      win1Pos{}, win1Size{};
    for (int win = 0; win < (multi != 0 ? 2 : 1); ++win)
    {
        const bool main = win == 0;
        // Column width (156 / 197; -1 = window 1's): 352 + (UI width - 512) * v / 100 within [352, UI width - 160].
        int widthSetting = cfgValue(d, main ? 156 : 197);
        if (widthSetting < 0)
        {
            widthSetting = cfgValue(d, 156);
        }
        const float textW = full && main ? 352.0f : logColumnWidth(uiW, widthSetting);
        const float kW    = logWindowWidth(textW);
        const float maxW  = kW - 2 * kPad;
        // Wrap this window's lines to its width.
        struct Wrapped
        {
            std::string text;
            UiColor     colour;
            std::size_t stamp = 0;
        };
        std::vector<Wrapped> lines;
        std::size_t          count = 0;
        const auto byteAdvance = [&](unsigned char ch) { return r_.advance(ch); };
        for (const auto& l : sl)
        {
            if (full && main ? !inFull(l) : !inWindow(l, win))
            {
                continue;
            }
            ++count;
            for (auto& row : wrapLogLine(l.text, l.stamp, maxW, width, byteAdvance))
            {
                lines.push_back({ std::move(row.text), l.colour, row.stamp });
            }
        }
        // Lines shown: max lines (15 / 183), or with auto-resize (14 / 182 = 0) shrinking toward min lines (16 / 184)
        // one line per interval (17 / 198, seconds; -1 = window 1's) after the last new line (INFERRED pacing).
        const int maxLines = std::clamp(cfgValue(d, main ? 15 : 183), 4, 20);
        const int minLines = std::clamp(cfgValue(d, main ? 16 : 184), 0, maxLines);
        int       interval = cfgValue(d, main ? 17 : 198);
        if (interval < 0)
        {
            interval = cfgValue(d, 17);
        }
        const auto w = static_cast<std::size_t>(win);
        if (count != logSeen_[w])
        {
            logSeen_[w]  = count;
            logLast_[w]  = now;
            logShown_[w] = static_cast<float>(maxLines);
        }
        const bool keepOpen = cfgValue(d, main ? 14 : 182) != 0 || (main && (typing_ || chatScroll_ > 0 || logMode_ != LogMode::Off || eventWaiting_));
        if (keepOpen)
        {
            logShown_[w] = static_cast<float>(maxLines);
            logLast_[w]  = now;
        }
        else if (now - logLast_[w] > std::chrono::seconds(std::max(3, interval)))
        {
            logShown_[w] = std::max(static_cast<float>(minLines), logShown_[w] - 1.0f);
            logLast_[w]  = now;
        }
        logShown_[w]       = std::min(logShown_[w], static_cast<float>(maxLines));
        // An addon's write of the log object's shown height (+0x34; fancychat clears it every frame to hide the log)
        // overrides the client's own while it holds (ui/log_object.h). The full-screen log is another object.
        const int natural = static_cast<int>(logShown_[w]) * kLogLineHeight;
        const int held    = std::min(logLinesForHeight(logHolds_[w].shown(natural, now)), maxLines);
        const int rowsShown =
            full && main ? static_cast<int>((384.0f - 20 - 2 * kPad - (typing_ ? 22 : 0)) / kLine) : held;
        logObjects_[w]        = { .shown    = true,
                                  .height   = natural,
                                  .lines    = rowsShown,
                                  .maxLines = maxLines,
                                  .minLines = minLines,
                                  .textTop  = logTextTopFromBottom(full && main ? 0 : rowsShown) };
        const float head      = full && main ? 20.0f : 0.0f;
        // The window keeps its height while the chat line is open; its text moves up a row instead (logTextRows, #105).
        const float kH        = full && main ? 384.0f : static_cast<float>(rowsShown) * kLine + 2 * kPad;
        ImVec2      pos{ 16, c.height - 16 - kH };
        if (!main)
        {
            pos = multi == 1 ? ImVec2{ win1Pos.x, win1Pos.y - kH } : ImVec2{ win1Pos.x + win1Size.x, win1Pos.y + win1Size.y - kH };
            pos.x = std::min(pos.x, std::max(0.0f, uiW - kW)); // kept on screen (with both at full width they overlap)
        }
        else
        {
            win1Pos  = pos;
            win1Size = { kW, kH };
            logTop_  = pos.y;
        }
        const ImVec2 o = c.toScreen(pos);
        if (main)
        {
            chatMin_ = o;
            chatMax_ = c.toScreen({ pos.x + kW, pos.y + kH });
            logRows_ = std::max(1, full ? rowsShown : logTextRows(rowsShown, typing_));
        }
        if (rowsShown <= 0 && !(main && typing_))
        {
            continue; // shrunk away
        }
        const auto lw = layouts.find(full && main ? "menu fulllog" : "menu logwindo");
        // Retail draws the log window's frame at all times (Steam captures, 2026-10-19), not only while it is selected.
        if (lw != layouts.end())
        {
            dat::UiLayout l = lw->second;
            l.window.x      = static_cast<int16_t>(pos.x);
            l.window.y      = static_cast<int16_t>(pos.y);
            l.window.w      = static_cast<int16_t>(kW);
            l.window.h      = static_cast<int16_t>(kH);
            l.window.anchor = 0;
            // Element 1 is the page marker ("anc kaipage"), drawn below only when scrolled back or waiting.
            drawLayout(p, c, l, { .cursor = main && logMode_ == LogMode::Selected ? 1 : 0, .tick = tick_, .elements = false });
            if (main && full)
            {
                // The channel tabs along the top edge (0x10137ab0): "menu fep" sprites from the window's left, 9 above
                // its top; the shown tab in its wide sprite (advancing by its width), the others 30 apart.
                float tx = pos.x;
                for (const auto& tab : fullLogTabs(logWin2_))
                {
                    const auto  spr = fullLogTabSprites(tab.label);
                    const bool  sel = tab.filter == logFilter_;
                    const auto& sheets = r_.resources().sheets;
                    const auto  it     = sheets.find("menu fep");
                    const auto  index  = static_cast<std::size_t>(sel ? spr.selected : spr.normal);
                    if (it == sheets.end() || index >= it->second.sprites.size())
                    {
                        break;
                    }
                    const auto& s = it->second.sprites[index];
                    p.sprite(s, c.toScreen({ tx, pos.y - 9 }));
                    float right = 0;
                    for (const auto& q : s)
                    {
                        right = std::max({ right, static_cast<float>(q.xy[2]), static_cast<float>(q.xy[6]) });
                    }
                    tx += sel ? right : 30.0f;
                }
            }
            if (main && !full)
            {
                // The chat mode's label on the top edge ("menu windowps" 276 Say, 277 Shout, 278 the yell label
                // (INFERRED), 279 Tell, 280 Party, 281 Linkshell, 711 Linkshell2, 712 Unity), authored for the 366-wide
                // window and kept at its right end (INFERRED from retail captures: x + width - 366).
                int label = 0;
                switch (chatKind_)
                {
                    case 0: label = 276; break;
                    case 1: label = 277; break;
                    case 26: label = 278; break;
                    case 3: label = 279; break;
                    case 4: label = 280; break;
                    case 5: label = 281; break;
                    case 27: label = 711; break;
                    case 33: label = 712; break;
                    default: break;
                }
                if (label != 0)
                {
                    p.sprite("menu windowps", static_cast<std::size_t>(label), c.toScreen({ pos.x + kW - 366.0f, pos.y }));
                }
            }
        }
        else
        {
            p.sprite("menu windowps", 0, o, {}, 0, kW / 366.0f, kH / 134.0f);
        }
        const int rows   = main && !full ? logTextRows(rowsShown, typing_) : rowsShown;
        int       scroll = main ? (chatScroll_ = std::clamp(chatScroll_, 0, std::max(0, static_cast<int>(lines.size()) - rows))) : 0;
        const int end    = static_cast<int>(lines.size()) - scroll;
        const int begin  = std::max(0, end - rows);
        const UiColor stampColour = cfgColour(d, 49);
        if (main && full)
        {
            // The highlighted row (+0x1c from the bottom; the game marks the line, flag 0x80 at 0x10137d7c). How the
            // game draws the mark is not traced: a pale bar behind the row (INFERRED).
            logLines_  = static_cast<int>(lines.size());
            logCursor_ = std::clamp(logCursor_, 1, std::max(1, std::min(rows, end - begin)));
            if (end > begin)
            {
                const float rowY = head + kPad + static_cast<float>(rows - logCursor_) * kLine;
                p.rect(c.toScreen({ pos.x + kPad - 1, pos.y + rowY }), { kW - 2 * kPad + 2, kLine }, { 0x80, 0x80, 0x80, 0x20 });
            }
        }
        for (int i = begin; i < end; ++i)
        {
            // Bottom-aligned: the newest line sits on the last row. Glyph ink starts 2 rows into the cell.
            const auto&  ln = lines[static_cast<std::size_t>(i)];
            const float  y  = head + kPad + static_cast<float>(rows - (end - begin) + (i - begin)) * kLine - 2;
            const ImVec2 at{ o.x + kPad * c.scale, o.y + y * c.scale };
            const float  x = ln.stamp > 0 ? p.text(std::string_view(ln.text).substr(0, ln.stamp), at, stampColour) : at.x;
            drawLogText(p, std::string_view(ln.text).substr(ln.stamp), { x, at.y }, ln.colour);
        }
        if (main && (chatScroll_ > 0 || eventWaiting_))
        {
            // Scrolled back, or an event waiting for the player: the log's page marker ("anc kaipage", the window's cursor sheet), animated.
            const auto* sheet = r_.resources().sheets.contains("anc kaipage") ? &r_.resources().sheets.at("anc kaipage") : nullptr;
            if (sheet && !sheet->sprites.empty())
            {
                p.sprite(sheet->sprites[static_cast<std::size_t>(tick_ / 3) % sheet->sprites.size()], { o.x + (kW - 14) * c.scale, o.y + (kH - 14) * c.scale });
            }
        }
    }
    if (typing_)
    {
        // The input line ("menu inline", bottom-anchored over the log's last row), with the text cursor blinking.
        const auto& ls = r_.resources().layouts;
        if (const auto il = ls.find("menu inline"); il != ls.end())
        {
            // Like the log, placed by the client: over the log's bottom 22 units (the layouts' 298+134 = 410+22), where
            // the log has moved its text up a row (logTextRows).
            // As wide as log window 1 (its normal column, also under the full log: INFERRED), sharing its left and
            // right edges (issue #108; the open handler 0x101b7ab0 sizes it to the log's +0x32 + 0xe).
            const float   inW = logWindowWidth(logColumnWidth(c.width, cfgValue(d, 156)));
            dat::UiLayout l  = il->second;
            l.window.x       = 16;
            l.window.y       = static_cast<int16_t>(c.height - 16 - static_cast<float>(l.window.h));
            l.window.w       = static_cast<int16_t>(inW);
            l.window.anchor  = 0;
            const ImVec2 io = drawLayout(p, c, l, {});
            // Show the end of long lines, keeping the cursor in view (tokens are never cut).
            std::size_t from = 0;
            cursor_          = std::min(cursor_, input_.size());
            while (from < cursor_ && width(std::string_view(input_).substr(from, cursor_ - from)) > inW - 24)
            {
                from = dat::chatedit::stepRight(input_, from);
            }
            const ImVec2 at{ io.x + 8 * c.scale, io.y + 3 * c.scale };
            p.pushClip(at, { io.x + (inW - 8) * c.scale, io.y + 22 * c.scale });
            drawChatText(p, std::string_view(input_).substr(from), at, kWhite, cursor_ - from);
            p.popClip();
            atDraw(c, p, at);
        }
    }
}

UiColor GameUi::hpColour(int hpp) const
{
    // "menu hpcol" sprites 0..3 (white, yellow, orange, red), vertex 0 colour as loaded (0x10201f60).
    const int   shape = hpp < 25 ? 3 : hpp < 50 ? 2 : hpp < 75 ? 1 : 0;
    const auto* s     = r_.resources().sprite("menu hpcol", static_cast<std::size_t>(shape));
    if (!s || s->empty())
    {
        return kWhite;
    }
    const auto& k = (*s)[0].rgba[0];
    auto rgb = [](uint8_t v) { return static_cast<uint8_t>(v == 0xFF ? 0xFF : v + 1); };
    return { rgb(k[0]), rgb(k[1]), rgb(k[2]), static_cast<uint8_t>(k[3] == 0 || k[3] == 0xFF ? k[3] : k[3] + 1) };
}

void GameUi::drawGaugeSprite(UiPainter& p, ImVec2 at, int fillSprite, int frameSprite, int pct, int& shown, const char* sheet)
{
    const int step = static_cast<int>(static_cast<float>(pct - shown) * 0.15f);
    shown          = step == 0 ? pct : shown + step;
    const float f1 = static_cast<float>(std::min(pct, shown)) / 100.0f;
    const float f2 = static_cast<float>(std::max(pct, shown)) / 100.0f;
    const auto* fill  = r_.resources().sprite(sheet, static_cast<std::size_t>(fillSprite));
    const float fillW = fill && !fill->empty() ? static_cast<float>((*fill)[0].xy[2] - (*fill)[0].xy[0]) : 0.0f;
    const float s     = p.scale();
    const ImVec2 fillAt(at.x + 4 * s, at.y + 1 * s);
    if (f1 > 0)
    {
        p.sprite(sheet, static_cast<std::size_t>(fillSprite), fillAt, kWhite, 0, f1, 1);
    }
    if (f2 > f1)
    {
        p.sprite(sheet, static_cast<std::size_t>(fillSprite), ImVec2(fillAt.x + std::floor(fillW * f1) * s, fillAt.y), { 0x80, 0x80, 0x80, 0x30 }, 0, f2 - f1, 1);
    }
    p.sprite(sheet, static_cast<std::size_t>(frameSprite), at, f1 == 0 ? UiColor{ 0x50, 0x50, 0x50, 0x30 } : kWhite);
}

const dat::UiLayout* GameUi::partyLayout(const GameUiData& d) const
{
    // "menu partywin" swaps its layout to "menu ptw{n}" by party size, or "menu ptw0" (0x1011a820, table 0x10385298):
    // retail shows the "Solo" window when not in a party.
    const auto& w    = *d.world;
    std::size_t rows = 0;
    for (const auto& m : w.party)
    {
        rows += m.partyNo == 0 ? 1 : 0;
    }
    const std::size_t n  = std::min<std::size_t>(rows, 6);
    const auto&       ls = r_.resources().layouts;
    const auto        it = ls.find(fmt::format("menu ptw{}", n));
    return it == ls.end() ? nullptr : &it->second;
}

std::optional<dat::UiLayout> GameUi::partyStackBase(const GameUiData& d) const
{
    // The windows stacked on the party window (target, sub window, ...) sit on its top, or on our pet's line drawn
    // 23 above it (CODE 0x10204105 / 0x10204123: the party object's +0x5e "top" moves to the pet line).
    const auto* pl = partyLayout(d);
    if (!pl)
    {
        return std::nullopt;
    }
    dat::UiLayout base = *pl;
    if (d.world && d.world->petPresent)
    {
        base.window.y = static_cast<int16_t>(base.window.y - kPetLineRaise);
        base.window.h = static_cast<uint16_t>(base.window.h + kPetLineRaise);
    }
    return base;
}

dat::UiLayout GameUi::stackedAbove(const UiCanvas& c, const dat::UiLayout& l, const dat::UiLayout& base)
{
    // 0x1015f6a0: new top = base top - (bottom - top) - 2, at least 16; x unchanged.
    const ImVec2  bo  = c.windowOrigin(base.window);
    const ImVec2  lo  = c.windowOrigin(l.window);
    dat::UiLayout out = l;
    out.window.x      = static_cast<int16_t>(lo.x / c.scale);
    out.window.y      = static_cast<int16_t>(std::max(16.0f, bo.y / c.scale - static_cast<float>(l.window.h) - 2.0f));
    out.window.anchor = 0;
    return out;
}

void GameUi::drawParty(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    // Our party (alliance parties 1-2 have their own windows, not drawn yet); solo = just ourselves.
    const auto&                      w = *d.world;
    std::vector<client::PartyMember> rows;
    for (const auto& m : w.party)
    {
        if (m.partyNo == 0)
        {
            rows.push_back(m);
        }
    }
    if (rows.empty())
    {
        client::PartyMember self;
        self.id   = w.charId;
        self.name = w.charName;
        rows.push_back(self);
    }
    rows.resize(std::min<std::size_t>(rows.size(), 6));
    const auto* pl = partyLayout(d);
    if (!pl)
    {
        return;
    }
    const auto&  l = *pl;
    const ImVec2 o = drawLayout(p, c, l, {});
    if (w.petPresent)
    {
        // Our pet (S2C 0x068), not a party row: a line 23 above the party window (CODE 0x10204102 -> 0x10202510): its
        // name (entity +0x7c) at the corner, the HP gauge at +26,+7 as the rows', and, when config 174 bit 0 is set
        // (off by default; a text command toggles it, CODE 0x10079240), its TP right-aligned at +48,+10 (0x102024b0:
        // grey, blue from 1000; not for models 0xB22..0xB31, 0x10098250). The x is the party window's (INFERRED: the
        // game also keeps it clear of another window, 0x10204064).
        const ImVec2 at(o.x, o.y - kPetLineRaise * c.scale);
        p.textShp(w.petName, at, kWhite);
        auto& shown = gaugeShown_[0xFFFFFFFFull << 8 | 1];
        drawGaugeSprite(p, ImVec2(at.x + 26 * c.scale, at.y + 7 * c.scale), 97, 38, w.petHpp, shown);
        if (cfgValue(d, 174) & 1)
        {
            p.textShp(std::to_string(w.petTp), ImVec2(at.x + 48 * c.scale, at.y + 10 * c.scale),
                      w.petTp >= 1000 ? UiColor{ 0x20, 0x60, 0xD0, 0x80 } : UiColor{ 0x60, 0x60, 0x60, 0x80 }, true);
        }
    }
    // Member i is drawn in element i's rectangle (0x102020e0); offsets below are from its corner.
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        auto m = rows[i];
        if (m.id == w.charId)
        {
            // Our own vitals packet is the freshest source for our row.
            m.hp = w.hp, m.mp = w.mp, m.tp = w.tp, m.hpp = w.hpp, m.mpp = w.mpp, m.zone = w.zone;
            if (m.name.empty())
            {
                m.name = w.charName;
            }
        }
        const auto* e = l.element(static_cast<uint16_t>(i + 1));
        if (!e)
        {
            continue;
        }
        const ImVec2 base(o.x + e->x * c.scale, o.y + e->y * c.scale);
        auto at = [&](float x, float y) { return ImVec2(base.x + x * c.scale, base.y + y * c.scale); };
        const bool away = m.zone != 0 && m.zone != w.zone;
        // Names longer than 8 characters: 7 and an ellipsis (fontshp sprite 108).
        std::string name = m.name;
        const bool  cut  = name.size() > 8;
        if (cut)
        {
            name.resize(7);
        }
        const UiColor nameCol = away ? kGrey : kWhite;
        const float   nw      = p.textShp(name, at(0, 0), nameCol);
        if (cut)
        {
            p.sprite("font fontshp", 108, ImVec2(at(0, 0).x + nw, at(0, 0).y), nameCol);
        }
        if (away)
        {
            // Another zone: its short name in brackets, right-aligned (0x102020e0).
            const auto area = d.areaShort ? d.areaShort->get(m.zone) : std::string_view();
            if (!area.empty())
            {
                p.textShp(fmt::format("({})", area), at(105, 11), kGrey, true);
            }
            continue;
        }
        auto& hpShown = gaugeShown_[static_cast<uint64_t>(m.id) << 8 | 1];
        auto& mpShown = gaugeShown_[static_cast<uint64_t>(m.id) << 8 | 2];
        drawGaugeSprite(p, at(26, 7), 97, 38, m.hpp, hpShown);
        drawGaugeSprite(p, at(49, 17), 98, 39, m.mpp, mpShown);
        p.textShp(std::to_string(m.hp), at(88, 1), hpColour(m.hpp), true);
        p.textShp(std::to_string(m.mp), at(98, 10), kWhite, true);
        p.textShp(std::to_string(m.tp), at(48, 10), m.tp >= 1000 ? UiColor{ 0x20, 0x60, 0xD0, 0x80 } : UiColor{ 0x60, 0x60, 0x60, 0x80 }, true);
    }
}

void GameUi::drawTarget(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    if (!d.target || eventTargetGauge_ >= 0) // an event's target window (op 0xB4 02) replaces it
    {
        return;
    }
    const auto& ls = r_.resources().layouts;
    const auto  it = ls.find("menu targetwi");
    if (it == ls.end())
    {
        return;
    }
    // Registry flags 0x2002401: stacked above the party window (0x1015e4fb), or above our pet's line.
    const auto   pl = partyStackBase(d);
    const auto   l  = pl ? stackedAbove(c, it->second, *pl) : it->second;
    const ImVec2 o  = drawLayout(p, c, l, {});
    auto at = [&](float x, float y) { return ImVec2(o.x + x * c.scale, o.y + y * c.scale); };
    const auto&   t = *d.target;
    const UiColor nameCol = nameColour(t, *d.world);
    // Name: one line at y 12, or split over two lines at 6 and 18 when wider than the window less 13 (0x101c9b50).
    const std::string name = t.name; // the game draws entity+0x7c as it is: empty for nameless NPCs ("blank" from LSB)
    const float       maxW = static_cast<float>(l.window.w - 13);
    if (p.textShpWidth(name) <= maxW)
    {
        p.textShp(name, at(5, 12), nameCol);
    }
    else
    {
        std::size_t split = name.size();
        for (std::size_t i = name.size(); i-- > 0;)
        {
            if (name[i] == ' ' && p.textShpWidth(name.substr(0, i)) <= maxW)
            {
                split = i;
                break;
            }
        }
        p.textShp(name.substr(0, split), at(5, 6), nameCol);
        p.textShp(split < name.size() ? name.substr(split + 1) : "", at(5, 18), nameCol);
    }
    static uint32_t lastTarget = 0;
    auto&           shown      = gaugeShown_[static_cast<uint64_t>(t.id) << 8 | 3];
    if (lastTarget != t.id)
    {
        shown      = t.hpp; // a new target does not animate
        lastTarget = t.id;
    }
    drawGaugeSprite(p, at(28, 29), 97, 38, t.hpp, shown);
    // The lock shape at the window's top left while the target is locked (this+0x5c = windowps sprite 212, set at
    // 0x101507a0 from sheet 1 +0x350; drawn when [0x1057876c+0x5c] & 3).
    if (d.targetLocked)
    {
        p.sprite("menu windowps", 212, at(0, 0));
    }
}

void GameUi::drawMenu(UiPainter& p, const UiCanvas& c, const Menu& m, bool top, const GameUiData& d, const GameUiActions& act)
{
    const auto* l = layout(m);
    if (!l)
    {
        return;
    }
    const auto& w = *d.world;
    LayoutState st;
    st.cursor = top ? m.cursor : 0;
    st.tick   = tick_;
    std::vector<int> states;
    if (!isList(m.kind))
    {
        uint16_t maxId = 0;
        for (const auto& e : l->elements)
        {
            maxId = std::max(maxId, e.id);
        }
        states.assign(maxId + 1u, 0);
        for (const auto& e : l->elements)
        {
            states[e.id] = implemented(m, e) ? 0 : 5;
        }
        if (m.kind == Kind::MogHouse && w.mhOpen && states.size() > 6)
        {
            states[6] = 1; // "Close Mog House": the element's state-1 sprite (windowps 608; CODE 0x1014522b)
        }
        st.states = &states;
    }
    if (isList(m.kind))
    {
        // Rows are drawn below; the hand points at the row under the cursor.
        LayoutState frameOnly = st;
        // "menu quset" rows carry an opaque row sprite (windowps 74) that hides the window; the rows are text here.
        frameOnly.elements = m.kind != Kind::Quests && m.kind != Kind::EventMenu && m.kind != Kind::SearchResults && m.kind != Kind::Blacklist && m.kind != Kind::EmoteList &&
                             !(m.kind == Kind::AppList && appMenus_.at(static_cast<std::size_t>(m.param)).keepStack); // pages: text rows, no item boxes
        const ImVec2 o = drawLayout(p, c, *l, frameOnly);
        const auto   list = rows(m, d, act);
        if (!stack_.empty() && stack_.back().kind == m.kind)
        {
            topRowItems_.clear();
            for (const auto& r : list)
            {
                topRowItems_.push_back(r.enabled ? r.iconItem : 0);
            }
        }
        auto at = [&](float x, float y) { return ImVec2(o.x + x * c.scale, o.y + y * c.scale); };
        for (const auto& e : l->elements)
        {
            const auto i = static_cast<std::size_t>(m.scroll + e.id - 1);
            if (e.id == 0 || e.nav[0] == 0xFF || i >= list.size())
            {
                continue;
            }
            const auto& row   = list[i];
            const bool  sel   = top && e.id == m.cursor;
            // Spell / ability lists: the selected row in 0x80803040 (pink), unusable 0x80404040 (CODE 0x1022c9c0, colour
            // table 0x103860fc).
            const bool  spellList = m.kind == Kind::Magic || m.kind == Kind::AbilityList;
            UiColor     col   = row.enabled ? (sel ? (spellList ? UiColor{ 0x80, 0x30, 0x40, 0x80 } : kGold) : row.colour.value_or(kWhite)) : kGrey;
            float       textX = static_cast<float>(e.x) + 4;
            if (row.iconSprite)
            {
                p.sprite(row.iconSprite->first, static_cast<std::size_t>(row.iconSprite->second), at(static_cast<float>(e.x), static_cast<float>(e.y)));
                textX = static_cast<float>(e.x) + 0x12;
            }
            if (row.iconItem && d.items && m.kind != Kind::Magic && m.kind != Kind::AbilityList)
            {
                if (const auto* icon = itemIcon(d, row.iconItem))
                {
                    p.icon(fmt::format("item {}", row.iconItem), *icon, at(static_cast<float>(e.x), static_cast<float>(e.y) - 1), row.enabled ? kWhite : kGrey, 16);
                }
                textX = static_cast<float>(e.x) + 20;
            }
            if (row.keycap)
            {
                // "[  ]" at x + 4 and the key top centred at x + 0x13; the label at x + 0x40 (CODE 0x1019c180).
                const float y = static_cast<float>(e.y) - 2;
                p.text("[  ]", at(static_cast<float>(e.x) + 4, y), col);
                if (!row.keycap->empty())
                {
                    p.text(*row.keycap, at(static_cast<float>(e.x) + 0x13 - p.textWidth(*row.keycap) / c.scale / 2, y), col);
                }
                textX = static_cast<float>(e.x) + 0x40;
            }
            p.text(row.label, at(textX, static_cast<float>(e.y) - 2), col);
            if (!row.right.empty())
            {
                const float rx = static_cast<float>(l->window.w) - 8 - p.textWidth(row.right) / c.scale;
                p.text(row.right, at(rx, static_cast<float>(e.y) - 2), col);
            }
        }
        if (m.kind == Kind::AppList)
        {
            if (const auto& t = appMenus_.at(static_cast<std::size_t>(m.param)).title; !t.empty())
            {
                p.text(t, at(8, 2), kWhite); // the window's own line above the rows (CODE 0x101d5e60 for "menu mogdoor")
            }
        }
        if (m.kind == Kind::EventMenu && !eventPrompt_.empty())
        {
            p.text(eventPrompt_, at(4, 7 - 2), kWhite); // "menu query": the prompt at x + 4, y + 7 (CODE 0x1014e150)
        }
        if (list.empty())
        {
            p.text("(none)", at(24, 6), kGrey);
        }
        // Font Colors: "menu textcol2" above the list shows the row's sample line in its colour (CODE 0x101987d0).
        if (m.kind == Kind::FontColorList && top)
        {
            const auto& fr = fontRows(m.param);
            const auto  i  = static_cast<std::size_t>(m.scroll + m.cursor - 1);
            const auto& ls = r_.resources().layouts;
            if (const auto sl = ls.find("menu textcol2"); sl != ls.end() && i < fr.size())
            {
                const ImVec2 so = drawLayout(p, c, sl->second, {});
                p.text(d.filterText ? std::string(d.filterText->get(fr[i].sample)) : std::string(), { so.x + 0x10 * c.scale, so.y + 0x14 * c.scale },
                       cfgColour(d, fr[i].cfg));
            }
        }
        // The selected item's info window below the list (Items, Equipment lists; the auction lists too: the auclist
        // object keeps the item info window 0x10578204 up and follows the highlighted row, CODE 0x101d7e3f / 0x101d7e83).
        if (top && (m.kind == Kind::Items || m.kind == Kind::EquipList || m.kind == Kind::AucList || m.kind == Kind::AucSales))
        {
            const auto i = static_cast<std::size_t>(m.scroll + m.cursor - 1);
            if (i < list.size() && list[i].iconItem)
            {
                drawItemInfo(p, c, list[i].iconItem, d, list[i].invKey);
            }
        }
        // Items: "menu itmsortw" stays up beside the list (CODE 0x101f01b0, opened with the list's mode); Right moves
        // into it.
        if (top && m.kind == Kind::Items)
        {
            const Menu panel{ Kind::ItemSort, 0, 0, 0 };
            drawMenu(p, c, panel, false, d, act);
        }
        // Scroll position: "first-last / total" (not in an event's "menu query": CODE 0x1014e150 draws only the prompt
        // and its rows). The Items window shows its count in the title window instead (#107).
        if (static_cast<int>(list.size()) > visibleRows(m) && m.kind != Kind::EventMenu && m.kind != Kind::Items)
        {
            const auto pos = fmt::format("{}/{}", m.scroll + m.cursor, list.size());
            p.text(pos, at(static_cast<float>(l->window.w) - 8 - p.textWidth(pos) / c.scale, static_cast<float>(l->window.h) - 14), kGrey);
        }
        return;
    }
    const ImVec2 o = drawLayout(p, c, *l, st);
    auto at = [&](float x, float y) { return ImVec2(o.x + x * c.scale, o.y + y * c.scale); };
    if (isMacroKind(m.kind))
    {
        drawMacroWindow(p, c, m, o);
    }
    if (m.kind == Kind::BattleMenu)
    {
        // Each row's label sprite at the row (CODE 0x1012b2f0: 0x80808080; the highlighted row 0x80805020 when the
        // window has a highlight time, not modelled).
        for (const auto& e : l->elements)
        {
            if (e.id >= 1 && e.id <= battleCodes_.size())
            {
                const auto& bc = kBattleCodes[battleCodes_[e.id - 1]];
                p.sprite(bc.windowps ? "menu windowps" : "menu keytops3", bc.sprite, at(e.x, e.y));
            }
        }
    }
    if (m.kind == Kind::Config)
    {
        // This client's "Add-ons" row (goal 47): Global's button (windowps 359) with the label rebuilt from the
        // buttons' own letters and shadow (#84; ui/button_label.h). The moji font is the fallback if that fails.
        if (const auto* e = l->element(15))
        {
            if (const auto* s = r_.resources().sprite("menu windowps", 359); s && s->size() >= 2)
            {
                const auto label = buttonLabel(*s, "Add-ons");
                p.sprite(label.empty() ? dat::UiSprite(s->begin(), s->begin() + 2) : label,
                         at(static_cast<float>(e->x), static_cast<float>(e->y)));
                if (label.empty())
                {
                    p.text("Add-ons", at(static_cast<float>(e->x) + 7, static_cast<float>(e->y) - 1), kWhite);
                }
            }
        }
    }
    if (m.kind == Kind::ChatCtrl && d.target && d.target->isPc)
    {
        // Tell's name after its arrow (element 8): by default the current target's (chatctrl +0x14 = -1,
        // CODE 0x1012d249). Cycling other names with Left / Right is not modelled.
        if (const auto* arrow = l->element(8))
        {
            p.text(d.target->name, at(static_cast<float>(arrow->x + arrow->w) + 4, static_cast<float>(arrow->y)), kWhite);
        }
    }
    if (isConfigPage(m.kind))
    {
        // The current choices: windowps sprite 387 at the button's (x + 0x25, y + 4) (CODE 0x101965c0; 412 for three-way
        // groups, 0x10196630); sliders: sprite 384 stretched by value / max (CODE 0x101966b0).
        for (const auto& b : cfgBindings())
        {
            const auto* el = b.page == m.kind ? l->element(b.element) : nullptr;
            if (!el)
            {
                continue;
            }
            if (b.slider)
            {
                int v = cfgValue(d, b.id);
                if (v < b.min)
                {
                    v = cfgValue(d, b.id == 197 ? 156 : b.id == 198 ? 17 : b.id); // window 2: as window 1
                }
                const float f = b.max > b.base ? static_cast<float>(v - b.base) / static_cast<float>(b.max - b.base) : 0.0f;
                const bool  editing = top && sliderEdit_ == b.element;
                // Colour 0x80808080, 0xa0a0a0a0 while editing (CODE 0x101966b0).
                const UiColor bar = editing ? UiColor{ 0xa0, 0xa0, 0xa0, 0xa0 } : UiColor{ 0x80, 0x80, 0x80, 0x80 };
                p.sprite("menu windowps", 384, at(static_cast<float>(el->x), static_cast<float>(el->y)), bar, 0, std::clamp(f, 0.0f, 1.0f), 1.0f);
                if (b.number)
                {
                    p.text(std::to_string(v), at(static_cast<float>(el->x + el->w) + 26, static_cast<float>(el->y) - 1), editing ? kGold : kWhite);
                }
                continue;
            }
            if (cfgChosen(b, d))
            {
                int group = 0;
                for (const auto& other : cfgBindings())
                {
                    group += other.page == b.page && other.id == b.id && other.bit == b.bit ? 1 : 0;
                }
                p.sprite("menu windowps", group == 3 ? 412 : 387, at(static_cast<float>(el->x) + 0x25, static_cast<float>(el->y) + 4), { 0x80, 0x80, 0x80, 0x80 });
            }
        }
    }
    if (m.kind == Kind::FontColorEdit)
    {
        // The sample in the colour being made, and the R / G / B bars (width = channel / 255, tints 0x70800000 /
        // 0x70008000 / 0x70000080; CODE 0x101989a0).
        const auto& e3 = fontEdit_.rgb;
        p.text(d.filterText ? std::string(d.filterText->get(static_cast<std::size_t>(fontEdit_.sample))) : std::string(), at(0x1e, 0x14),
               { static_cast<uint8_t>(e3[0]), static_cast<uint8_t>(e3[1]), static_cast<uint8_t>(e3[2]), 0x80 });
        static const UiColor kBar[3] = { { 0x80, 0x00, 0x00, 0x70 }, { 0x00, 0x80, 0x00, 0x70 }, { 0x00, 0x00, 0x80, 0x70 } };
        for (int k = 0; k < 3; ++k)
        {
            if (const auto* el = l->element(static_cast<uint16_t>(k + 1)))
            {
                const bool  editing = top && fontEdit_.channel == k + 1;
                UiColor     col     = kBar[k];
                col.a               = editing ? 0xa0 : 0x70;
                p.rect(at(static_cast<float>(el->x), static_cast<float>(el->y) + 2), { static_cast<float>(el->w - 4) * static_cast<float>(e3[static_cast<std::size_t>(k)]) / 255.0f, static_cast<float>(el->h) - 4 }, col);
            }
        }
    }
    if (m.kind == Kind::Status && stack_.back().kind != Kind::Currencies)
    {
        // Under the Currencies window (16,48) the status windows give way to its title window at 16,16 (INFERRED).
        drawStatus(p, c, d);
    }
    else if (m.kind == Kind::TradeQty || m.kind == Kind::TradeGil)
    {
        // The value right-aligned in the numctrl field (element 1).
        if (const auto* f = l->element(1))
        {
            p.textShp(std::to_string(tradeCtrl_.value), at(static_cast<float>(f->x + f->w) - 4, static_cast<float>(f->y) + 2), kWhite, true);
        }
    }
    else if (m.kind == Kind::Trade || m.kind == Kind::PlayerTrade || m.kind == Kind::Synth)
    {
        if (m.kind == Kind::PlayerTrade)
        {
            drawPartnerTrade(p, c, *l, d);
            // Our name at (+4, +5), clipped before the gil field; orange once we confirmed (CODE 0x10151300).
            const UiColor nameCol = tradeLocked_ ? UiColor{ 0x80, 0x40, 0x20, 0x80 } : kWhite;
            const float   clipX   = l->element(11) ? static_cast<float>(l->element(11)->x) - 2 : static_cast<float>(l->window.w);
            p.pushClip(at(0, 0), at(clipX, 20));
            p.textShp(w.charName, at(4, 5), nameCol);
            p.popClip();
        }
        if (const auto* g = l->element(11); g && m.kind == Kind::Synth)
        {
            // Synthesis: the crystal, where the trade window has its gil field.
            const auto info = d.items ? d.items->find(synthCrystalId_) : std::nullopt;
            p.text(info ? info->name : std::string(), at(static_cast<float>(g->x) + 2, static_cast<float>(g->y) + 1), kWhite);
        }
        else if (g)
        {
            p.textShp(std::to_string(tradeGil_), at(static_cast<float>(g->x + g->w) - 4, static_cast<float>(g->y) + 2), kWhite, true);
        }
        // Icons and counts in the eight boxes.
        for (std::size_t b = 0; b < 8; ++b)
        {
            const auto& [slot, count] = tradeBoxes_[b];
            const auto* e             = l->element(static_cast<uint16_t>(b + 1));
            if (slot == 0 || !e)
            {
                continue;
            }
            for (const auto& it : w.inventory)
            {
                if (it.container == 0 && it.slot == slot && d.items)
                {
                    if (const auto* icon = itemIcon(d, it.itemId))
                    {
                        p.icon(fmt::format("item {}", it.itemId), *icon, at(e->x, e->y));
                    }
                    if (count > 1)
                    {
                        p.textShp(std::to_string(count), at(static_cast<float>(e->x) + 31, static_cast<float>(e->y) + 20), kWhite, true);
                    }
                    if (top && e->id == m.cursor)
                    {
                        drawItemInfo(p, c, it.itemId, d, it.container << 8 | it.slot);
                    }
                }
            }
        }
    }
    else if (m.kind == Kind::Equip)
    {
        if (w.stats.valid)
        {
            // "Attack" / "Defense" labels and the elemental dots of the window sprite (framesus 13): dots at
            // x = 6 + 44 * column, y = 162 / 176, in the order fire ice / wind earth / lightning water / light dark.
            auto right = [&](const std::string& t, float x, float y) { p.text(t, at(x - p.textWidth(t) / c.scale, y), kWhite); };
            right(std::to_string(w.stats.attack), 86, 146);
            right(std::to_string(w.stats.defense), 178, 146);
            for (std::size_t i = 0; i < 8; ++i)
            {
                const float x = 6 + 44.0f * static_cast<float>(i / 2), y = 160 + 14.0f * static_cast<float>(i % 2);
                right(std::to_string(w.stats.elementDefense[i]), x + 40, y);
            }
        }
        // Item icons in the slot boxes; the selected slot's item in the item info window.
        uint16_t selectedItem = 0;
        int      selectedKey  = -1;
        for (const auto& e : l->elements)
        {
            const auto* sub = e.subFor(0);
            if (!sub || sub->shape < 40 || sub->shape >= 56)
            {
                continue;
            }
            const auto [cont, idx] = w.equipment[kEquipCellSlot[sub->shape - 40]];
            if (idx == 0)
            {
                continue;
            }
            for (const auto& it : w.inventory)
            {
                if (it.container == cont && it.slot == idx && d.items)
                {
                    if (const auto* icon = itemIcon(d, it.itemId))
                    {
                        p.icon(fmt::format("item {}", it.itemId), *icon, at(e.x, e.y));
                    }
                    if (top && e.id == m.cursor)
                    {
                        selectedItem = it.itemId;
                        selectedKey  = it.container << 8 | it.slot;
                    }
                }
            }
        }
        if (top && selectedItem)
        {
            drawItemInfo(p, c, selectedItem, d, selectedKey);
        }
        // The hovered slot's item list stays up beside the grid while the cursor is on it (retail captures); Enter moves
        // the cursor into it.
        if (top)
        {
            for (const auto& e : l->elements)
            {
                const auto* sub = e.subFor(0);
                if (e.id == m.cursor && sub && sub->shape >= 40 && sub->shape < 56)
                {
                    Menu preview;
                    preview.kind  = Kind::EquipList;
                    preview.param = kEquipCellSlot[sub->shape - 40];
                    drawMenu(p, c, preview, false, d, act);
                }
            }
        }
    }
}

void GameUi::drawItemInfo(UiPainter& p, const UiCanvas& c, uint16_t itemId, const GameUiData& d, int key)
{
    const auto info = d.items ? d.items->find(itemId) : std::nullopt;
    if (!info)
    {
        return;
    }
    std::vector<std::string> lines{ info->name };
    std::string              levelLine; // after the description, as in the game
    // Second line for gear (CODE 0x1013cdf0): weapons "(Skill)", armour "[Slot]", then the races, from d_msg 55653.
    if (info->equipment() && d.unityText)
    {
        std::string second;
        const int   first = info->type == 4 ? skillLabel(info->skill) : slotLabel(info->slots);
        if (first >= 0)
        {
            second = std::string(d.unityText->get(static_cast<uint32_t>(first)));
        }
        if (const int r = raceLabel(info->races); r >= 0)
        {
            second += std::string(d.unityText->get(static_cast<uint32_t>(r)));
        }
        if (!second.empty())
        {
            lines.push_back(second);
        }
    }
    if (info->equipment())
    {
        // Level and jobs, composed by the client from the record (bit n of the mask = job n).
        std::string jobs;
        int         count = 0;
        for (uint8_t j = 1; j < 23; ++j)
        {
            if ((info->jobs >> j) & 1)
            {
                jobs += (count++ ? "/" : "") + std::string(jobAbbrev(j));
            }
        }
        levelLine = fmt::format("Lv.{} {}", info->level, count >= 22 ? "All Jobs" : jobs);
    }
    for (std::size_t at = 0; at <= info->description.size() && !info->description.empty();)
    {
        const auto nl = info->description.find('\n', at);
        lines.push_back(info->description.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos)
        {
            break;
        }
        at = nl + 1;
    }
    if (!levelLine.empty())
    {
        lines.push_back(levelLine);
    }
    // "menu iteminfo" holds three text rows; "menu item<N>inf" (names cut to 8 characters past 9) holds N, 16 units each.
    const std::size_t rows = std::clamp<std::size_t>(lines.size(), 3, 12);
    const std::string name = rows <= 3 ? "menu iteminfo" : rows <= 9 ? fmt::format("menu item{}inf", rows) : fmt::format("menu item{}in", rows);
    const auto&       ls   = r_.resources().layouts;
    const auto        it   = ls.find(name);
    if (it == ls.end())
    {
        return;
    }
    // Drawn directly under the inventory window, whatever the layout's anchor says.
    dat::UiLayout l = it->second;
    l.window.anchor = 0;
    // With "menu money" up (Equipment, Items) it starts at the Equipment grid's x, right of it (retail captures).
    if (moneyShown())
    {
        if (const auto eq = ls.find("menu equip"); eq != ls.end())
        {
            l.window.x = static_cast<int16_t>(UiCanvas{ c.width, c.height, 1 }.windowOrigin(eq->second.window).x);
        }
    }
    const ImVec2 o  = drawLayout(p, c, l, {});
    auto at = [&](float x, float y) { return ImVec2(o.x + x * c.scale, o.y + y * c.scale); };
    if (const auto* icon = itemIcon(d, itemId))
    {
        p.icon(fmt::format("item {}", itemId), *icon, at(8, 12));
    }
    // Long lines (job lists) are cut at the window's right edge.
    p.pushClip(at(0, 0), at(static_cast<float>(l.window.w) - 6, static_cast<float>(l.window.h)));
    // The name in white, green while worn (CODE 0x1013cdf0); text from x 0x33 (the English layout, table 0x1036fa4a).
    const bool worn = key >= 0 && isWorn(*d.world, static_cast<uint8_t>(key >> 8), static_cast<uint8_t>(key & 0xFF));
    for (std::size_t i = 0; i < lines.size() && i < rows; ++i)
    {
        p.text(lines[i], at(0x33, 4 + 16.0f * static_cast<float>(i)), i == 0 && worn ? kWornGreen : kWhite);
    }
    p.popClip();
    // Rare / Ex / temporary marks from x 0x13a, y 4, left to right (CODE 0x1013cb80; "menu windowps" 294..296).
    const auto marks = infoMarks(*info, key >= 0 ? static_cast<uint8_t>(key >> 8) : 0);
    float      mx    = 0x13a;
    for (const auto& [on, shape] : { std::pair{ marks.rare, 294 }, std::pair{ marks.ex, 295 }, std::pair{ marks.temporary, 296 } })
    {
        if (!on)
        {
            continue;
        }
        p.sprite("menu windowps", static_cast<std::size_t>(shape), at(mx, 4));
        const auto* sp = r_.resources().sprite("menu windowps", static_cast<std::size_t>(shape));
        // Advance by the sprite's width (its bounding box, sheet shape +0x22 - +0x20).
        int lo = 0, hi = 0;
        for (const auto& q : sp ? *sp : dat::UiSprite{})
        {
            for (std::size_t k = 0; k < 8; k += 2)
            {
                lo = std::min<int>(lo, q.xy[k]);
                hi = std::max<int>(hi, q.xy[k]);
            }
        }
        mx += hi > lo ? static_cast<float>(hi - lo) : 24.0f;
    }
}

void GameUi::drawTargetBuffs(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    // Config Misc. 3 "party member status icons" (cfg 194, CODE 0x10227fa0): the targeted party member's effects (S2C
    // 0x076) in rows of eight 14-unit cells above the log window at log.x + 200, with the name 16 units above.
    if (cfgValue(d, 194) == 0 || !d.target || !d.index)
    {
        return;
    }
    const client::PartyMember* m = nullptr;
    for (const auto& pm : d.world->party)
    {
        if (pm.id == d.target->id && pm.id != d.world->charId && !pm.buffs.empty())
        {
            m = &pm;
        }
    }
    if (!m)
    {
        return;
    }
    constexpr float kCell = 14;
    const auto      rows  = static_cast<float>((m->buffs.size() + 7) / 8);
    const float     x0 = 16 + 200, y0 = logTop_ - 8 - rows * kCell;
    p.text(m->name, c.toScreen({ x0, y0 - 16 }), kWhite);
    for (std::size_t i = 0; i < m->buffs.size(); ++i)
    {
        const auto icon = m->buffs[i];
        const bool typeOne = ((cfgValue(d, 153) >> 2) & 1) != 0; // Config icon pattern (cfg 153 bit 2)
        auto [slot, fresh] = statusIcons_.try_emplace(static_cast<uint32_t>(icon) | (typeOne ? 0x10000u : 0u));
        if (fresh)
        {
            slot->second = dat::statusIcon(*d.index, icon, typeOne ? 39557u : 87u);
        }
        if (slot->second)
        {
            p.icon(fmt::format("status {} {}", icon, typeOne), *slot->second, c.toScreen({ x0 + static_cast<float>(i % 8) * kCell, y0 + static_cast<float>(i / 8) * kCell }), kWhite, kCell);
        }
    }
}

void GameUi::drawCompass(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    // The compass and the clock line above log window 1 (ui/compass.h; docs/ui-format-notes.md §10.13). The game opens
    // them in the field and hides them under full-screen windows (0x1015ec20 with flag 4: INFERRED to be the map and
    // the full-screen log here).
    const auto& w       = *d.world;
    const bool  covered = logMode_ == LogMode::Full || std::any_of(stack_.begin(), stack_.end(), [](const Menu& m) {
        return m.kind == Kind::Map || m.kind == Kind::MapMarkers;
    });
    if (!w.inZone || w.inMogHouse || logTop_ <= 0 || covered)
    {
        return;
    }
    if (w.zone != compassZone_ || w.teleportGeneration != compassGen_)
    {
        compassZone_  = w.zone;
        compassGen_   = w.teleportGeneration;
        compassStart_ = d.now;
    }
    // The fade in: 24 ticks of 1/60 s (0x1021eda0).
    const float f =
        std::clamp(std::chrono::duration<float>(d.now - compassStart_).count() * 60.0f / kCompassFadeTicks, 0.0f, 1.0f);
    const auto   alpha  = static_cast<uint8_t>(f * 0x80);
    const auto   at     = compassPlace(logTop_);
    const ImVec2 centre = c.toScreen({ at.x, at.y });
    const float  sc     = c.scale;

    // The clock line, the day's icon and the weather's (0x1021fd50): drawn in every zone, at full strength.
    if (w.gameTimeAt != std::chrono::steady_clock::time_point{})
    {
        const auto now =
            w.gameTime +
            static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(d.now - w.gameTimeAt).count());
        // Our map square (0x101f71b0): "(?-?)" where this zone has no map.
        std::string square = "(?-?)";
        if (d.maps)
        {
            if (const auto* rec = d.maps->find(w.zone, d.mapId))
            {
                const auto [u, v] = rec->project(d.playerX, d.playerZ);
                square            = mapGridRef(u, v);
                if (square.empty())
                {
                    square = "(?-?)";
                }
            }
        }
        p.textShp(compassClockText(now, square), c.toScreen({ at.x - 0x32, at.y + 0x1A }), kWhite);
        p.sprite("menu framesus", compassDaySprite(now), c.toScreen({ at.x - 0x44, at.y + 0x1A }), kWhite);
    }
    if (const auto wx = compassWeather(w.weather))
    {
        const ImVec2 pos = c.toScreen({ at.x - 0x44, at.y - 0x2A });
        p.sprite("font usgaiji", wx->glyph, pos, kWhite);
        if (wx->strong)
        {
            // The second icon right after the first: the glyph's width (its shape +0x22 - +0x20).
            float wdt = 0;
            if (const auto* g = r_.resources().sprite("font usgaiji", wx->glyph))
            {
                for (const auto& q : *g)
                {
                    wdt = std::max(wdt, static_cast<float>(std::max({ q.xy[0], q.xy[2], q.xy[4], q.xy[6] })));
                }
            }
            p.sprite("font usgaiji", wx->glyph, ImVec2(pos.x + wdt * sc, pos.y), kWhite);
        }
    }
    if (compassHiddenIn(w.zone))
    {
        return; // ships and airships: the clock line only (0x100ad6f0)
    }

    // The dial (0x1021ff10): "menu compass" 0, sized 0.4 x the job's size x the fade, turned with the camera (swinging
    // in from half a turn while it fades) and tipped back; the letters N E S W 140 out on the same dial, upright.
    const auto        jobs = compassRange(w.mainJob, w.subJob, w.subLevel != 0);
    const CompassTurn turn = compassTurn(d.camForwardX, d.camForwardZ, (1.0f - f) * 3.14159265f);
    const float       k    = 0.4f * jobs.size * f;
    const UiColor     mod{ 0x80, 0x80, 0x80, alpha };
    if (const auto* dial = r_.resources().sprite("menu compass", 0))
    {
        p.spriteAffine(*dial, centre, { k * turn.c, -k * turn.s, k * turn.s * kCompassTilt, k * turn.c * kCompassTilt },
                       mod);
    }
    struct Letter
    {
        std::size_t sprite;
        float       lx, ly;
    };
    constexpr Letter kLetters[] = { { 4, 0, -140 }, { 1, 140, 0 }, { 3, 0, 140 }, { 2, -140, 0 } }; // N E S W
    for (const auto& l : kLetters)
    {
        const auto o = compassPoint(l.lx, l.ly, turn, k);
        p.sprite("menu compass", l.sprite, ImVec2(centre.x + o.x * sc, centre.y + o.y * sc), mod);
    }

    // The dots: everyone within reach, 2 a yalm x the fade, coloured by kind ("menu compass" 5).
    auto inParty = [&](uint32_t id) {
        return std::any_of(w.party.begin(), w.party.end(), [&](const client::PartyMember& m) { return m.id == id; });
    };
    for (const auto& e : w.entities)
    {
        if (e.id == w.charId || e.hiddenByStatus(false) || (e.look.valid && e.look.kind >= 2 && e.look.kind <= 4))
        {
            continue; // ourselves, the hidden, doors / lifts / ships (not drawn as actors; INFERRED for 0x10098140)
        }
        CompassDot kind = CompassDot::None;
        if (e.isPc)
        {
            kind = inParty(e.id) ? CompassDot::Ally : CompassDot::Player;
        }
        else if (!e.isMob)
        {
            kind = CompassDot::Npc;
        }
        else if (jobs.monsters)
        {
            kind = e.animation == 2 || e.animation == 3 ? CompassDot::DeadMonster : CompassDot::Monster;
        }
        if (kind == CompassDot::None)
        {
            continue;
        }
        // e.pos is game x / -y / z as the session keeps it: the height difference only needs the sign-free size.
        const float dx = e.pos.x - d.playerX, dz = e.pos.z - d.playerZ, dy = e.pos.y - d.playerY;
        const int   a = compassDotAlpha(std::sqrt(dx * dx + dz * dz), dy, jobs, alpha);
        if (a <= 0)
        {
            continue;
        }
        const uint32_t rgb = compassDotColour(kind);
        const auto     o   = compassWorld(dx, dz, turn, 2.0f * f);
        p.sprite("menu compass", 5, ImVec2(centre.x + o.x * sc, centre.y + o.y * sc),
                 UiColor{ static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb),
                          static_cast<uint8_t>(a) });
    }
}

void GameUi::drawBuffs(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    const auto& buffs = d.world->buffs;
    const auto& ls    = r_.resources().layouts;
    const auto  it    = ls.find("menu buff");
    buffCount_        = 0;
    if (buffs.empty() || !d.index || it == ls.end())
    {
        return;
    }
    // "menu buff": a strip of 24x24 slots, nine per row; only the slots in use are drawn.
    dat::UiLayout l = it->second;
    l.elements.resize(std::min(l.elements.size(), buffs.size()));
    buffCount_     = static_cast<int>(l.elements.size());
    buffCursor_    = std::clamp(buffCursor_, 0, std::max(0, buffCount_ - 1));
    const ImVec2 o = drawLayout(p, c, l, {});
    for (std::size_t i = 0; i < l.elements.size(); ++i)
    {
        const auto& e    = l.elements[i];
        const auto  icon = buffs[i].icon;
        const bool typeOne = ((cfgValue(d, 153) >> 2) & 1) != 0; // Config icon pattern (cfg 153 bit 2)
        auto [slot, fresh] = statusIcons_.try_emplace(static_cast<uint32_t>(icon) | (typeOne ? 0x10000u : 0u));
        if (fresh)
        {
            slot->second = dat::statusIcon(*d.index, icon, typeOne ? 39557u : 87u);
        }
        if (!slot->second)
        {
            continue;
        }
        // Wearing off (15 s or less, with the status icon timers on): the icon's colour pulses between half and full
        // brightness on the window's 40-tick triangle, alpha unchanged (CODE 0x10227ad0; status_pulse.h).
        UiColor mod = kWhite;
        if (cfgValue(d, 195) != 0 && buffs[i].expires)
        {
            const auto left = std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 60>>>(*buffs[i].expires - d.now).count();
            if (statusSecondsLeftCeil(left) <= kStatusPulseSeconds)
            {
                const double ticks = std::chrono::duration<double>(d.now.time_since_epoch()).count() * 60.0;
                mod.r = mod.g = mod.b = statusPulseLevel(statusPulseCounter(ticks));
            }
        }
        p.icon(fmt::format("status {} {}", icon, typeOne), *slot->second, ImVec2(o.x + e.x * c.scale, o.y + e.y * c.scale), mod, 24);
        if (cfgValue(d, 195) != 0 && buffs[i].expires)
        {
            // /statustimer: the time left under the icon ("45", "3m", "1h").
            const auto left = std::max<long>(0, std::chrono::duration_cast<std::chrono::seconds>(*buffs[i].expires - d.now).count());
            const auto t    = left >= 3600 ? fmt::format("{}h", left / 3600) : left >= 60 ? fmt::format("{}m", left / 60) : fmt::format("{}", left);
            p.textShp(t, ImVec2(o.x + (e.x + 12) * c.scale - p.textShpWidth(t) * 0.5f * c.scale, o.y + (e.y + 18) * c.scale), kWhite);
        }
    }
}

const dat::StatusInfo* GameUi::statusInfoFor(const GameUiData& d, uint16_t id)
{
    auto [it, fresh] = statusInfos_.try_emplace(id);
    if (fresh && d.index)
    {
        it->second = dat::statusInfo(*d.index, id);
    }
    return it->second ? &*it->second : nullptr;
}

void GameUi::drawBuffSelection(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    // The status icons have the cursor (Select Window twice, #122): the pointing hand on the icon, its name in the title
    // window and its description on the help line (CODE 0x10227903..0x1022797e: 0x10226fb0 -> the title window
    // 0x1057847c, the record's string table -> the help line 0x105781f8).
    const auto& buffs = d.world->buffs;
    const auto& ls    = r_.resources().layouts;
    const auto  it    = ls.find("menu buff");
    if (buffs.empty() || it == ls.end() || buffCursor_ >= buffCount_)
    {
        if (d.help)
        {
            drawHelp(p, c, {});
        }
        return;
    }
    dat::UiLayout l = it->second;
    l.elements.resize(static_cast<std::size_t>(buffCount_));
    drawLayout(p, c, l,
               { .cursor   = static_cast<int>(l.elements[static_cast<std::size_t>(buffCursor_)].id),
                 .tick     = tick_,
                 .frame    = false,
                 .elements = false });
    const auto  id   = buffs[static_cast<std::size_t>(buffCursor_)].icon;
    const auto* info = statusInfoFor(d, id);
    drawHelp(p, c, info ? info->description : std::string());
    if (const auto tw = ls.find("menu titlewin"); tw != ls.end() && d.text)
    {
        const std::string name(d.text->statusName(id));
        if (!name.empty())
        {
            const ImVec2 to = drawLayout(p, c, tw->second, {});
            p.text(name, ImVec2(to.x + 4 * c.scale, to.y + 8 * c.scale), kWhite);
        }
    }
}

void GameUi::sendTradeBox(const GameUiActions& act, int box, uint16_t itemId)
{
    // C2S 0x034 TRADE_LIST {ItemNum u32, ItemNo u16, ItemIndex u8, TradeIndex u8}.
    const auto& [slot, count] = tradeBoxes_[static_cast<std::size_t>(box)];
    std::vector<std::byte> pkt(8);
    std::memcpy(pkt.data(), &count, 4);
    std::memcpy(pkt.data() + 4, &itemId, 2);
    pkt[6] = static_cast<std::byte>(slot);
    pkt[7] = static_cast<std::byte>(box + 1); // TradeIndex 0 is the gil slot; items use 1..8
    act.send(0x034, std::move(pkt));
}

void GameUi::drawPartnerTrade(UiPainter& p, const UiCanvas& c, const dat::UiLayout& ours, const GameUiData& d)
{
    // "menu gift" (16,144, CODE 0x10138f20): the partner's eight boxes, their gil right-aligned at (right - 14, 8)
    // and their name at (+4, +5), orange once they confirmed. No cursor.
    const auto&          ls = r_.resources().layouts;
    const auto           found = ls.find("menu gift");
    const dat::UiLayout* gl = found != ls.end() ? &found->second : nullptr;
    dat::UiLayout        fallback;
    if (!gl)
    {
        fallback          = ours;
        fallback.window.y = static_cast<int16_t>(ours.window.y + ours.window.h + 2);
        fallback.elements.resize(8);
        gl = &fallback;
    }
    const ImVec2 o  = drawLayout(p, c, *gl, {});
    auto         at = [&](float x, float y) { return ImVec2(o.x + x * c.scale, o.y + y * c.scale); };
    const auto&  t  = d.world->trade;
    for (std::size_t b = 0; b < 8; ++b)
    {
        const auto [item, count] = t.theirs[b + 1]; // indexed by TradeIndex (0 = gil)
        const auto* e            = gl->element(static_cast<uint16_t>(b + 1));
        if (item == 0 || !e)
        {
            continue;
        }
        if (const auto* icon = itemIcon(d, item))
        {
            p.icon(fmt::format("item {}", item), *icon, at(e->x, e->y));
        }
        if (count > 1)
        {
            p.textShp(std::to_string(count), at(static_cast<float>(e->x) + 31, static_cast<float>(e->y) + 20), kWhite, true);
        }
    }
    const float right = static_cast<float>(gl->window.w) - 14;
    p.textShp(std::to_string(t.theirs[0].second), at(right, 8), kWhite, true);
    p.pushClip(at(0, 0), at(right - 60, 20));
    p.textShp(t.partnerName, at(4, 5), t.partnerConfirmed ? UiColor{ 0x80, 0x40, 0x20, 0x80 } : kWhite);
    p.popClip();
}

const dat::Texture* GameUi::itemIcon(const GameUiData& d, uint16_t id)
{
    // Decoded once per item: the GPU copy is cached by the renderer, this avoids re-reading the DAT every frame.
    auto [it, fresh] = itemIcons_.try_emplace(id);
    if (fresh && d.items)
    {
        it->second = d.items->icon(id);
    }
    return it->second ? &*it->second : nullptr;
}

void GameUi::drawNetMeter(UiPainter& p, const UiCanvas& c, bool window)
{
    // The network meter ("menu netstat", right-anchored at 384,16): always shown; its window only while a menu (the help
    // line) is up (retail captures). Contents (CODE 0x10200ef0): the signal arrows ("menu netmeter", frames base + phase:
    // base by the link percentage, 0-3 green >= 60, 4-7 yellow >= 30, 8-11 red; the phase steps 0..3 every 11 - pct / 10
    // draws, 0x10200e80, so the gradient strip under the arrows pulses along them), "%3d%%" at (+20, +10), and S / R
    // (bytes a second sent / received) at (+58, +4) / (+58, +16) with their values at +68, dim (0x60808080). The link
    // percentage is not tracked here: 100 while connected (INFERRED).
    if (window)
    {
        netMeterShown_ = true;
    }
    else if (std::exchange(netMeterShown_, false))
    {
        return; // already drawn with its window this frame
    }
    const auto& ls = r_.resources().layouts;
    const auto  ns = ls.find("menu netstat");
    if (ns == ls.end())
    {
        return;
    }
    const ImVec2 no = window ? drawLayout(p, c, ns->second, {}) : c.windowOrigin(ns->second.window);
    auto at = [&](float x, float y) { return ImVec2(no.x + x * c.scale, no.y + y * c.scale); };
    const UiColor dim{ 0x80, 0x80, 0x80, 0x60 };
    constexpr int pct = 100;
    const int     base = pct < 30 ? 8 : pct < 60 ? 4 : 0;
    if (tick_ % (11 - pct / 10) == 0 && tick_ != netMeterTick_)
    {
        netMeterTick_  = tick_;
        netMeterPhase_ = (netMeterPhase_ + 1) % 4;
    }
    p.sprite("menu netmeter", static_cast<std::size_t>(base + netMeterPhase_), no);
    p.textShp(fmt::format("{:3}%", pct), at(20, 10), dim);
    if (data_ && data_->world)
    {
        p.textShp("S", at(58, 4), dim);
        p.textShp(std::to_string(data_->world->netSendRate), at(68, 4), dim);
        p.textShp("R", at(58, 16), dim);
        p.textShp(std::to_string(data_->world->netRecvRate), at(68, 16), dim);
    }
}

void GameUi::drawGaugeWindow(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    // CODE 0x101392d0 (vtable 0x103361d8 +0x10): the fill "menu windowps" 97 at (+25, +8) HP (x HPP / 100, 0x80808080),
    // (+25, +18) MP (x MPP / 100, 0x8080e080), (+25, +28) TP: < 1000 x TP / 1000 in 0x800060b0; 1000..2999 full, then
    // (TP % 1001) / 1000 over it in 0x8000b060 (green) or 0x802060d0 (blue, >= 2000); 3000 full. Numbers HP / MP / TP
    // "%4d" at +123, rows +7 / +17 / +27, 0xc0808080. The labels and gauge tracks are sheet 4 ("menu framesus") shape 8
    // at (+4, +8): "HP" / "MP" / "TP" font quads and three "menu gauge" bars with end caps.
    const auto& ls = r_.resources().layouts;
    const auto  it = ls.find("menu gaugewin");
    if (it == ls.end() || !d.world)
    {
        return;
    }
    const auto&  w = *d.world;
    const ImVec2 o = drawLayout(p, c, it->second, {});
    auto at = [&](float x, float y) { return ImVec2(o.x + x * c.scale, o.y + y * c.scale); };
    p.sprite("menu framesus", 8, at(4, 8));
    auto fill = [&](float y, float f, UiColor col) {
        if (f > 0)
        {
            p.sprite("menu windowps", 97, at(25, y), col, 0, std::min(f, 1.0f), 1);
        }
    };
    fill(8, static_cast<float>(w.hpp) / 100.0f, { 0x80, 0x80, 0x80, 0x80 });
    fill(18, static_cast<float>(w.mpp) / 100.0f, { 0x80, 0xE0, 0x80, 0x80 });
    const int tp = std::min(static_cast<int>(w.tp), 3000);
    if (tp >= 1000)
    {
        fill(28, 1.0f, { 0x80, 0x80, 0x80, 0x80 });
        if (tp < 3000)
        {
            fill(28, static_cast<float>(tp % 1001) / 1000.0f, tp >= 2000 ? UiColor{ 0x20, 0x60, 0xD0, 0x80 } : UiColor{ 0x00, 0xB0, 0x60, 0x80 });
        }
    }
    else
    {
        fill(28, static_cast<float>(tp) / 1000.0f, { 0x00, 0x60, 0xB0, 0x80 });
    }
    const UiColor num{ 0x80, 0x80, 0x80, 0xC0 };
    p.textShp(std::to_string(w.hp), at(123, 3), num, true);
    p.textShp(std::to_string(w.mp), at(123, 13), num, true);
    p.textShp(std::to_string(tp), at(123, 23), num, true);
}

void GameUi::drawSubWindow(UiPainter& p, const UiCanvas& c, const GameUiData& d, const GameUiActions& act)
{
    if (stack_.empty() || stack_.back().kind != Kind::Magic || !d.clientText)
    {
        return;
    }
    const auto& ls = r_.resources().layouts;
    const auto  sw = ls.find("menu subwindo");
    const auto  tw = ls.find("menu targetwi");
    const auto  pl = partyStackBase(d);
    if (sw == ls.end() || !pl)
    {
        return;
    }
    const auto& m    = stack_.back();
    const auto  list = rows(m, d, act);
    const auto  i    = static_cast<std::size_t>(m.scroll + m.cursor - 1);
    if (i >= list.size() || list[i].mpCost < 0)
    {
        return;
    }
    // Base: the target window when shown (itself stacked on the party window), else the party window.
    const dat::UiLayout base = d.target && tw != ls.end() ? stackedAbove(c, tw->second, *pl) : *pl;
    const ImVec2        o    = drawLayout(p, c, stackedAbove(c, sw->second, base), {});
    auto at = [&](float x, float y) { return ImVec2(o.x + x * c.scale, o.y + y * c.scale); };
    // Rows (+8 and +26): label (file 55648 230 / 231) at +5, value right-aligned at +105.
    // Upright text, as in retail captures.
    auto right = [&](const std::string& t, float y) { p.text(t, at(105 - p.textWidth(t) / c.scale, y), kWhite); };
    p.text(std::string(d.clientText->get(230)), at(5, 6), kWhite);
    right(std::to_string(list[i].mpCost), 6);
    p.text(std::string(d.clientText->get(231)), at(5, 24), kWhite);
    right(list[i].next, 24);
}

void GameUi::updateBattleCodes(const GameUiData& d)
{
    battleCodes_.clear();
    // The battle menu stays up (and keeps its rows) under the windows its Right / Confirm open.
    const auto bm = std::find_if(stack_.rbegin(), stack_.rend(), [](const Menu& m) { return m.kind == Kind::BattleMenu; });
    if (bm == stack_.rend() || !d.world)
    {
        return;
    }
    const auto& w    = *d.world;
    const auto* t    = d.target;
    const int   mode = std::clamp(bm->param, 0, 7);
    for (uint8_t code : kBattleModes[mode])
    {
        if (code == 0 || code == 28)
        {
            break;
        }
        switch (code)
        {
            case 4: // chat (0x1012af03): dropped with an NPC targeted (id 0x01xxxxxx); any other target, ourselves
                    // included, makes it Tell (code 5, the same "Chat" label)
                if (t && (t->id & 0xFF000000u) == 0x01000000u)
                {
                    continue;
                }
                code = t ? 5 : 4;
                break;
            case 10: // examine (0x1012ae9f): kept, ourselves included, unless 0x1008e080 (actor +0x1ff == 1 with
                     // +0x1d0 bits 0 / 8 clear; not a case met with players). On ourselves at a fishing spot it
                     // becomes Fish (code 13; 0x10177640 -> 0x101814e0, zone != 211): not modelled.
                break;
            case 16: // invite (0x1012af6d): never on ourselves (kept only for our own +0x2a0 actor); other players
                     // by their +0x124 bits 20 / 21, modelled as "not already in our party"
                if (!t || !t->isPc || t->id == w.charId ||
                    std::any_of(w.party.begin(), w.party.end(), [&](const client::PartyMember& p) { return p.id == t->id; }))
                {
                    continue;
                }
                break;
            case 19: // trade (0x1012af27): dropped with an NPC targeted (and while our +0x1d0 bit 10 is set, not
                     // modelled); kept for players and ourselves
                if (t && (t->id & 0xFF000000u) == 0x01000000u)
                {
                    continue;
                }
                break;
            case 11: // check: not on players (0x1012aee2 -> 0x1008e080)
                if (t && t->isPc)
                {
                    continue;
                }
                break;
            case 12: // treasure: only with something in the pool (0x1012afe6 -> 0x1013ef20)
                if (std::none_of(w.treasure.begin(), w.treasure.end(), [](const client::WorldSnapshot::TreasureItem& i) { return i.itemId != 0; }))
                {
                    continue;
                }
                break;
            case 13: // fishing, Abyssea / Campaign entries, seek party, release trust: their conditions are not modelled
            case 22:
            case 23:
            case 24:
            case 26:
            case 27:
                continue;
            default: break;
        }
        battleCodes_.push_back(code);
    }
}

bool GameUi::moneyShown() const
{
    // CODE 0x101330ad (Equipment: persona, money, equip) and 0x10139850 (the item list: money) open "menu money".
    return std::any_of(stack_.begin(), stack_.end(), [](const Menu& m) { return m.kind == Kind::Equip || m.kind == Kind::Items || m.kind == Kind::PlayerTrade; });
}

void GameUi::drawMoney(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    // CODE 0x101407f0: the inventory's slot 0 count (gil, 0x100eb3c0(0, 0) +4) right-aligned at (+0x67, +0x1f)
    // (0x10140840 -> 0x101c8ff0). Thousands separators and the " G" as in retail captures (INFERRED format).
    const auto& ls = r_.resources().layouts;
    const auto  it = ls.find("menu money");
    if (it == ls.end() || !d.world)
    {
        return;
    }
    uint32_t gil = 0;
    for (const auto& item : d.world->inventory)
    {
        if (item.container == 0 && item.slot == 0)
        {
            gil = item.count;
        }
    }
    std::string n = std::to_string(gil);
    for (int i = static_cast<int>(n.size()) - 3; i > 0; i -= 3)
    {
        n.insert(static_cast<std::size_t>(i), ",");
    }
    n += " G";
    const ImVec2 o = drawLayout(p, c, it->second, {});
    p.text(n, ImVec2(o.x + 103 * c.scale - p.textWidth(n), o.y + 29 * c.scale), kWhite);
}

void GameUi::drawStatusWindow(UiPainter& p, const UiCanvas& c, const GameUiData& d, int dy)
{
    // "menu statuswi" (16,16 112x190; CODE 0x10147000). The name at (4,7); nothing more until S2C 0x061 arrived.
    const auto& ls = r_.resources().layouts;
    const auto  it = ls.find("menu statuswi");
    if (it == ls.end() || !d.world)
    {
        return;
    }
    const auto&   w = *d.world;
    const auto&   st = w.stats;
    dat::UiLayout l = it->second;
    l.window.y      = static_cast<int16_t>(l.window.y + dy);
    if (const auto o = std::optional<ImVec2>(drawLayout(p, c, l, {})))
    {
        auto at = [&](float x, float y) { return ImVec2(o->x + x * c.scale, o->y + y * c.scale); };
        p.textShp(w.charName, at(4, 7), kWhite);
        if (st.valid)
        {
            // "Lv%d" (55653 #89) at (4, 0x13) and the job's name sprite (windowps 212 + job) 2 after it; the support
            // job the same at (4, 0x1f) when there is one. The main job draws 0x802060a0 while level-synced (not
            // tracked here).
            auto job = [&](int id, int level, float y) {
                const float end = p.textShp(fmt::format("Lv{}", level), at(4, y), kWhite);
                p.sprite("menu windowps", static_cast<std::size_t>(212 + id), ImVec2(at(4, y).x + end + 2 * c.scale, at(4, y).y), kWhite);
            };
            job(w.mainJob, w.mainLevel, 0x13);
            if (w.subJob)
            {
                job(w.subJob, w.subLevel, 0x1f);
            }
            // Item level: "ILv" (4, 0x2b), "%3d/" of 99 + ilvl (or "---/") right-aligned at 0x46, "Su%2d" at 0x4a.
            p.textShp("ILv", at(4, 0x2b), kWhite);
            p.textShp(st.itemLevel ? fmt::format("{:3}/", 99 + st.itemLevel) : std::string("---/"), at(0x46, 0x2b), kWhite, true);
            p.textShp(fmt::format("Su{:2}", st.suLevel), at(0x4a, 0x2b), kWhite);
            // HP / MP: "%4d/" right-aligned at 0x46, the maximum "%d" at 0x48; TP "%4d" right-aligned at 0x4b.
            p.textShp(fmt::format("{:4}/", w.hp), at(0x46, 0x3a), kWhite, true);
            p.textShp(std::to_string(st.hpMax), at(0x48, 0x3a), kWhite);
            p.textShp(fmt::format("{:4}/", w.mp), at(0x46, 0x46), kWhite, true);
            p.textShp(std::to_string(st.mpMax), at(0x48, 0x46), kWhite);
            p.textShp(fmt::format("{:4}", w.tp), at(0x4b, 0x52), kWhite, true);
            // STR..CHR from y 0x60, 13 apart: base "%4d" at 0x32, the bonus right after it, "+%d" in 0x80408040 or
            // "%d" in 0x80804040.
            for (std::size_t i = 0; i < 7; ++i)
            {
                const float y   = 0x60 + 13.0f * static_cast<float>(i);
                const float end = p.textShp(fmt::format("{:4}", st.base[i]), at(0x32, y), kWhite);
                if (const int b = st.bonus[i]; b != 0)
                {
                    p.textShp(b > 0 ? fmt::format("+{}", b) : std::to_string(b), ImVec2(at(0x32, y).x + end, at(0x32, y).y),
                              b > 0 ? UiColor{ 0x40, 0x80, 0x40, 0x80 } : UiColor{ 0x80, 0x40, 0x40, 0x80 });
                }
            }
        }
    }
}

void GameUi::drawStatus(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    // All text in fontshp (0x101c9330 left, 0x101c9700 right-aligned when its 5th argument is 1); sprites from "menu
    // windowps" (sheet 1), "menu keytops3" (2) and "menu framesus" (4) by index (0x101c87e0). Colours 0x80RRGGBB.
    const auto&   ls = r_.resources().layouts;
    const auto&   w  = *d.world;
    const auto&   st = w.stats;
    const UiColor kCapped{ 0x20, 0x60, 0xA0, 0x80 }; // 0x802060a0: capped skills
    auto window = [&](const char* name, std::optional<int16_t> h = std::nullopt) -> std::optional<ImVec2> {
        const auto it = ls.find(name);
        if (it == ls.end())
        {
            return std::nullopt;
        }
        dat::UiLayout l = it->second;
        if (h)
        {
            l.window.h = *h;
        }
        return drawLayout(p, c, l, {});
    };
    auto commas = [](uint32_t v) {
        // 0x101c8ff0: thousands separators (55653 #91 ",").
        std::string t = std::to_string(v);
        for (int i = static_cast<int>(t.size()) - 3; i > 0; i -= 3)
        {
            t.insert(static_cast<std::size_t>(i), ",");
        }
        return t;
    };

    drawStatusWindow(p, c, d, 0);

    // "menu level" (16,208 112x88; CODE 0x101475e0): "Level %d", "EXP", the points, "NEXTLevel", the points needed
    // (55653 #86..88, numbers with commas right-aligned at 90) and the gauge (windowps 97 / 38, 0x101cabf0).
    if (const auto o = window("menu level"); o && st.valid)
    {
        auto at  = [&](float x, float y) { return ImVec2(o->x + x * c.scale, o->y + y * c.scale); };
        auto str = [&](std::size_t id, const char* fallback) { return d.unityText ? std::string(d.unityText->get(id)) : std::string(fallback); };
        const uint32_t exp = static_cast<uint16_t>(st.exp), next = static_cast<uint16_t>(st.expNext);
        p.textShp(fmt::format("{} {}", str(86, "Level"), w.mainLevel), at(8, 10), kWhite);
        p.textShp(str(87, "EXP"), at(15, 22), kWhite);
        p.textShp(commas(exp), at(90, 34), kWhite, true);
        p.textShp(str(88, "NEXTLevel"), at(15, 46), kWhite);
        p.textShp(commas(next), at(90, 58), kWhite, true);
        drawGaugeSprite(p, at(22, 72), 97, 38, next ? static_cast<int>(std::min<uint32_t>(100, exp * 100 / next)) : 0, expShown_);
    }

    // The window statcom shows, at 130,48 (CODE 0x10148c30 opens it by name from table 0x103716c8).
    const float kRow = 14;
    switch (statusSub_)
    {
        case 0: // "menu profile" (CODE 0x101479f0)
            if (const auto o = window("menu profile"); o && st.valid)
            {
                // Nation crest and name (keytops3 74 + nation) at (0x37, 0x12); home point area (55661) at (0x5e, 0x34);
                // rank "%d" right-aligned at 0x40, y 0x44; rank points bar (framesus 103 / 98) at (0x5c, 0x54),
                // rankbar / 4096 (0x10330250).
                auto at = [&](float x, float y) { return ImVec2(o->x + x * c.scale, o->y + y * c.scale); };
                if (st.nation < 3)
                {
                    p.sprite("menu keytops3", 74u + st.nation, at(0x37, 0x12), kWhite);
                }
                if (d.areaShort)
                {
                    p.textShp(d.areaShort->get(st.bindZone), at(0x5e, 0x34), kWhite);
                }
                p.textShp(std::to_string(st.rank), at(0x40, 0x44), kWhite, true);
                drawGaugeSprite(p, at(0x5c, 0x54), 103, 98, std::min(100, st.rankBar * 100 / 4096), rankShown_, "menu framesus");
            }
            break;
        case 1: // "menu joblevel" (CODE 0x10147f60)
            if (const auto o = window("menu joblevel"))
            {
                // Jobs 1..22 that are unlocked, two to a row from y 0x13: name sprite (windowps 212 + job) at 0x0a /
                // 0x82, level right-aligned at 0x6e / 0xe6. The main job's name in 0x80e09620, the support job's in
                // 0x804070a0.
                auto  at = [&](float x, float y) { return ImVec2(o->x + x * c.scale, o->y + y * c.scale); };
                int   n  = 0;
                float y  = 0x13;
                for (int job = 1; job <= 22; ++job)
                {
                    if (!(w.jobsUnlocked & (1u << job)))
                    {
                        continue;
                    }
                    const bool    right = (n++ & 1) != 0;
                    const UiColor col   = job == w.mainJob ? UiColor{ 0xE0, 0x96, 0x20, 0x80 } : job == w.subJob ? UiColor{ 0x40, 0x70, 0xA0, 0x80 } : kWhite;
                    p.sprite("menu windowps", static_cast<std::size_t>(212 + job), at(right ? 0x82 : 0x0a, y), col);
                    p.textShp(std::to_string(w.jobLevels[static_cast<std::size_t>(job)]), at(right ? 0xe6 : 0x6e, y), kWhite, true);
                    y += right ? kRow : 0;
                }
            }
            break;
        case 2: // "menu btlskill" (CODE 0x101483e0): combat skills 1..31
        case 3: // "menu mgcskill" (CODE 0x101485e0): magic skills 32..47
            if (const auto o = window(statusSub_ == 2 ? "menu btlskill" : "menu mgcskill"); o && w.skillsValid)
            {
                // Two to a row from y 0x13: name sprite (keytops3 208 + skill) at 0x0a / 0x82, level right-aligned
                // (combat: 0x78 / 0xf0 for the English client, 0x6e / 0xe6 Japanese; magic: 0x6e / 0xe6) in 0x802060a0
                // when capped. Skills the job lacks (bit 15, level 0) are left out.
                auto        at       = [&](float x, float y) { return ImVec2(o->x + x * c.scale, o->y + y * c.scale); };
                const bool  combat   = statusSub_ == 2;
                const float valueX[] = { combat ? 120.0f : 110.0f, combat ? 240.0f : 230.0f }; // 0x78 / 0x6e, 0xf0 / 0xe6
                int         n        = 0;
                float       y        = 0x13;
                for (int id = combat ? 1 : 32; id <= (combat ? 31 : 47); ++id)
                {
                    const uint16_t v = w.skills[static_cast<std::size_t>(id)];
                    if (!client::skillShown(v))
                    {
                        continue;
                    }
                    const bool right = (n++ & 1) != 0;
                    p.sprite("menu keytops3", static_cast<std::size_t>(208 + id), at(right ? 0x82 : 0x0a, y), kWhite);
                    p.textShp(std::to_string(client::skillLevel(v)), at(valueX[right ? 1 : 0], y), client::skillCapped(v) ? kCapped : kWhite, true);
                    y += right ? kRow : 0;
                }
            }
            break;
        case 4: // "menu trdskill" (CODE 0x101487d0): crafts 48..62
            if (w.skillsValid)
            {
                // One to a row from y 0x13: name (keytops3 208 + skill) at 0x0a, rank (keytops3 193 + rank) at 0x6e
                // (0x5f Japanese) in 0x802060a0 when capped, "(%d)" level right-aligned at 0xf5. 0xFFFF = left out.
                // The window is resized to its rows (0x1011a4d0); the height here is INFERRED: 0x16 + 14 per row.
                int rows = 0;
                for (int id = 48; id <= 62; ++id)
                {
                    rows += w.skills[static_cast<std::size_t>(id)] != 0xFFFF ? 1 : 0;
                }
                if (const auto o = window("menu trdskill", static_cast<int16_t>(0x16 + 14 * rows)))
                {
                    auto  at = [&](float x, float y) { return ImVec2(o->x + x * c.scale, o->y + y * c.scale); };
                    float y  = 0x13;
                    for (int id = 48; id <= 62; ++id)
                    {
                        const uint16_t v = w.skills[static_cast<std::size_t>(id)];
                        if (v == 0xFFFF)
                        {
                            continue;
                        }
                        p.sprite("menu keytops3", static_cast<std::size_t>(208 + id), at(0x0a, y), kWhite);
                        if (client::craftRank(v) < 16)
                        {
                            p.sprite("menu keytops3", 193u + client::craftRank(v), at(0x6e, y), client::skillCapped(v) ? kCapped : kWhite);
                        }
                        p.textShp(fmt::format("({})", client::craftLevel(v)), at(0xf5, y), kWhite, true);
                        y += kRow;
                    }
                }
            }
            break;
        default: break;
    }
}

std::string GameUi::mapGridRef(float u, float v)
{
    // CODE 0x101fa570: the 512-pixel map's grid, 32 pixels a square inside a 16-pixel border: letter 'A' + (u - 16) / 32,
    // number (v - 16) / 32 + 1 (both floored), "(%c-%d)" (0x10384fec).
    if (u < 16 || v < 16 || u >= 496 || v >= 496)
    {
        return {};
    }
    return fmt::format("({:c}-{})", static_cast<char>('A' + static_cast<int>(u - 16) / 32), static_cast<int>(v - 16) / 32 + 1);
}

void GameUi::drawMapGridRef(UiPainter& p, const UiCanvas& c)
{
    // The map writes our grid square into the title window in place of the title (0x101fa652: white at the window's
    // text spot; 0x101eeb15 clears the title).
    const auto& ls = r_.resources().layouts;
    if (const auto tw = ls.find("menu titlewin"); tw != ls.end())
    {
        const ImVec2 to = drawLayout(p, c, tw->second, {});
        p.text(mapGridRef_, ImVec2(to.x + 4 * c.scale, to.y + 8 * c.scale), kWhite);
    }
}

void GameUi::drawHelp(UiPainter& p, const UiCanvas& c, const std::string& text)
{
    const auto& ls = r_.resources().layouts;
    const auto  hw = ls.find("menu helpwind");
    if (hw == ls.end())
    {
        return;
    }
    // The help window grows with the interface width: 252 + (width - 512) (CODE 0x1013a180; the reserved fraction from
    // 0x101601a0 taken as 0), so it runs from x 130 to the Network box.
    dat::UiLayout l = hw->second;
    l.window.w      = static_cast<int16_t>(std::max(252.0f, 252.0f + c.width - 512.0f));
    const ImVec2 o  = drawLayout(p, c, l, {});
    // Its "Help" label ("menu windowps" 361) on the top edge, 78 from the right end (INFERRED from retail captures).
    p.sprite("menu windowps", 361, ImVec2(o.x + (static_cast<float>(l.window.w) - 78) * c.scale, o.y));
    // The title window ("menu titlewin", 16,16 112x30) names the menu in front: its window record's second string id
    // in the window titles (file 55652; CODE 0x10118570 -> 0x10150bf0), none when the layout has none. The Status
    // menu has none: "menu statuswi" sits in its place.
    if (!mapGridRef_.empty())
    {
        drawMapGridRef(p, c);
    }
    else if (!stack_.empty() && (stack_.back().kind == Kind::MacroBooks || stack_.back().kind == Kind::MacroEdit))
    {
        drawMacroTitle(p, c); // the book list and the edit palettes name the book and set (string table 10 #109 / #39)
    }
    else if (!stack_.empty() && stack_.back().kind == Kind::AppList &&
             !appMenus_.at(static_cast<std::size_t>(stack_.back().param)).windowTitle.empty())
    {
        if (const auto tw = ls.find("menu titlewin"); tw != ls.end())
        {
            const ImVec2 to = drawLayout(p, c, tw->second, {});
            p.text(appMenus_.at(static_cast<std::size_t>(stack_.back().param)).windowTitle, ImVec2(to.x + 4 * c.scale, to.y + 8 * c.scale), kWhite);
        }
    }
    else if (!stack_.empty() && data_ && data_->titles && stack_.back().kind != Kind::Status)
    {
        const auto* ml = layout(stack_.back());
        const auto  tw = ls.find("menu titlewin");
        if (ml && ml->window.text2 >= 0 && tw != ls.end())
        {
            // The Currencies window (evitem mode 1) titles itself 55651 #721 "Currencies" (CODE 0x1022a203).
            // The guild lists title themselves 55652 #13 "Guild Shop" (CODE 0x101ed0f0 / 0x101ed120 / 0x101ed170).
            const bool        currencies = stack_.back().kind == Kind::Currencies && data_->help;
            const auto        k          = stack_.back().kind;
            const bool        guild      = k == Kind::GuildBuy || k == Kind::GuildSell || k == Kind::GuildSellPick;
            const std::string title      = currencies ? std::string(data_->help->get(721))
                                           : guild    ? std::string(data_->titles->get(13))
                                                      : std::string(data_->titles->get(static_cast<std::size_t>(ml->window.text2)));
            const ImVec2      to         = drawLayout(p, c, tw->second, {});
            p.text(title, ImVec2(to.x + 4 * c.scale, to.y + 8 * c.scale), kWhite);
            if (k == Kind::Items && data_->world)
            {
                // "Items" then the count, held/capacity, in plain white (#107; ui/item_count.h).
                const auto& w     = *data_->world;
                const auto  held  = std::count_if(w.inventory.begin(), w.inventory.end(), [](const auto& it) { return it.container == 0 && it.slot != 0; });
                const auto  count = itemCountText(static_cast<std::size_t>(held), w.containerSize[0], w.containerSizeValid);
                if (!count.empty())
                {
                    const float x = itemCountX(p.textWidth(title) / c.scale, p.textWidth("          ") / c.scale, p.textWidth(count) / c.scale,
                                               static_cast<float>(tw->second.window.w));
                    p.text(count, ImVec2(to.x + x * c.scale, to.y + 8 * c.scale), kWhite);
                }
            }
        }
    }
    // The network window ("menu netstat", right-anchored at 384,16) beside it.
    drawNetMeter(p, c, true);
    const float  inner = static_cast<float>(l.window.w) - 16;
    const float  w     = p.textWidth(text) / c.scale;
    float        x     = 6; // text at (+6, +8): retail captures (2026-10-19)
    if (w > inner)
    {
        // Marquee: hold, scroll to the end at 30 units/s, hold, restart.
        const float travel = w - inner;
        const float t      = std::fmod(static_cast<float>(tick_) / 60.0f, 2.0f + travel / 30.0f + 2.0f);
        x -= std::clamp((t - 2.0f) * 30.0f, 0.0f, travel);
    }
    p.pushClip(ImVec2(o.x + 6 * c.scale, o.y), ImVec2(o.x + (static_cast<float>(l.window.w) - 6) * c.scale, o.y + static_cast<float>(l.window.h) * c.scale));
    p.text(text, ImVec2(o.x + x * c.scale, o.y + 8 * c.scale), kWhite);
    p.popClip();
}

bool GameUi::drawLogin(ImDrawList& dl, const UiCanvas& c, LoginFields& f)
{
    ++tick_;
    login_       = &f;
    loginActive_ = true;
    UiPainter p(r_, dl, c.scale);
    p.rect(ImVec2(0, 0), { c.width, c.height }, { 0x06, 0x08, 0x10, 0x80 });
    constexpr float W = 300;
    const float     H = f.notice.empty() ? 112.0f : 128.0f;
    const ImVec2    o((c.width - W) * 0.5f * c.scale, (c.height * 0.45f - H * 0.5f) * c.scale);
    p.frame(o, { W, H });
    auto at = [&](float x, float y) { return ImVec2(o.x + x * c.scale, o.y + y * c.scale); };
    p.text("Log In", at(12, 6), kGold);
    static const char* kLabels[] = { "Server", "Account", "Password", "Character" };
    const std::string  values[]  = { f.host, f.user, std::string(f.pass.size(), '*'), f.character.empty() ? std::string() : f.character };
    for (int i = 0; i < 4; ++i)
    {
        const float y   = 26 + 16.0f * static_cast<float>(i);
        const bool  sel = i == f.field;
        p.text(kLabels[i], at(24, y), sel ? kGold : kGrey);
        const float x = p.text(values[i], at(110, y), kWhite);
        if (sel && (tick_ / 30) % 2 == 0)
        {
            p.text("_", ImVec2(x, at(0, y).y), kWhite);
        }
        if (sel)
        {
            p.sprite("anc anc_s", static_cast<std::size_t>(tick_ / 3) % 6, at(4, y + 2));
        }
    }
    p.text(f.character.empty() ? "Enter: connect, then pick a character" : "Enter: connect", at(24, 92), kGrey);
    if (!f.notice.empty())
    {
        p.text(f.notice, at(24, 108), kWhite);
    }
    const bool submit = loginSubmit_ && !f.user.empty() && !f.pass.empty();
    loginSubmit_      = false;
    if (submit)
    {
        loginActive_ = false;
        login_       = nullptr;
    }
    return submit;
}

void GameUi::drawLobby(UiPainter& p, const UiCanvas& c, const GameUiData& d, const GameUiActions& act)
{
    const auto& slots = d.world->lobbySlots;
    std::vector<std::size_t> chars; // occupied slots
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        if (slots[i].occupied)
        {
            chars.push_back(i);
        }
    }
    // Input queued by keyDown, applied here.
    while (!pending_.empty())
    {
        const auto cmd = pending_.front();
        pending_.pop_front();
        const int n = lobbyStage_ == 0 ? 5 : std::max<int>(1, static_cast<int>(chars.size()));
        if (cmd == Cmd::Up) lobbyCursor_ = lobbyCursor_ > 1 ? lobbyCursor_ - 1 : n;
        else if (cmd == Cmd::Down) lobbyCursor_ = lobbyCursor_ < n ? lobbyCursor_ + 1 : 1;
        else if (cmd == Cmd::Back && lobbyStage_ == 1)
        {
            lobbyStage_  = 0;
            lobbyCursor_ = 1;
        }
        else if (cmd == Cmd::Confirm)
        {
            if (lobbyStage_ == 0 && lobbyCursor_ == 1)
            {
                lobbyStage_  = 1; // Select Character
                lobbyCursor_ = 1;
            }
            else if (lobbyStage_ == 1 && !chars.empty() && act.chooseCharacter)
            {
                act.chooseCharacter(chars[static_cast<std::size_t>(lobbyCursor_ - 1)]);
            }
        }
    }
    const auto& ls = r_.resources().layouts;
    const auto  it = ls.find("menu loby2win");
    if (it == ls.end())
    {
        return;
    }
    // The lobby's buttons are additive sprites made for a dark backdrop (the game shows its title scene here).
    p.rect(ImVec2(0, 0), { c.width, c.height }, { 0x06, 0x08, 0x10, 0x80 });
    // The lobby menu's buttons, in row order (element ids 1, 2, 3, 5, 4: Select, Create, Delete, Config, Back);
    // placed bottom-centre: the layout is authored for an 800x600 canvas.
    dat::UiLayout l = it->second;
    l.window.subs.clear();
    const float dx = c.width * 0.5f - 425.0f, dy = c.height - 600.0f;
    static constexpr int kRows[] = { 1, 2, 3, 5, 4 };
    std::vector<int> states(8, 5);
    states[1] = 0; // only Select Character works for now
    for (const auto& e : l.elements)
    {
        const int   row  = static_cast<int>(std::find(std::begin(kRows), std::end(kRows), e.id) - std::begin(kRows)) + 1;
        const auto* sub  = e.subFor(states[std::min<std::size_t>(e.id, 7)]);
        const bool  sel  = lobbyStage_ == 0 && row == lobbyCursor_;
        if (!sub)
        {
            continue;
        }
        const UiColor mod = states[std::min<std::size_t>(e.id, 7)] == 5 && sub->type != 4 ? UiColor{ 0x48, 0x48, 0x48, 0x80 } : UiColor{};
        const ImVec2  at((e.x + dx) * c.scale, (e.y + dy) * c.scale);
        p.sprite(sub->sheet, sub->shape, at, mod);
        if (sel)
        {
            p.sprite("anc anc_s", static_cast<std::size_t>(tick_ / 3) % 6, ImVec2(at.x - 18 * c.scale, at.y + 4 * c.scale));
        }
    }
    if (lobbyStage_ == 1)
    {
        // Character list: a skin frame above the buttons, one row per character with job and level.
        const float  w = 200, h = 16 + 18.0f * static_cast<float>(std::max<std::size_t>(1, chars.size()));
        const ImVec2 o((c.width - w) * 0.5f * c.scale, (dy + 420 - h) * c.scale);
        p.frame(o, { w, h });
        static const char* kJobs[] = { "---", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
                                       "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN" };
        for (std::size_t i = 0; i < chars.size(); ++i)
        {
            const auto&  sl  = slots[chars[i]];
            const bool   sel = static_cast<int>(i) + 1 == lobbyCursor_;
            const ImVec2 row(o.x + 24 * c.scale, o.y + (8 + 18.0f * static_cast<float>(i)) * c.scale);
            p.text(sl.name, row, sel ? kGold : kWhite);
            const auto job = fmt::format("{}{}", sl.mainJob < std::size(kJobs) ? kJobs[sl.mainJob] : "?", sl.mainLvl);
            p.text(job, ImVec2(o.x + (w - 12) * c.scale - p.textWidth(job), row.y), sel ? kGold : kWhite);
            if (sel)
            {
                p.sprite("anc anc_s", static_cast<std::size_t>(tick_ / 3) % 6, ImVec2(o.x + 2 * c.scale, row.y + 2 * c.scale));
            }
        }
        if (chars.empty())
        {
            p.text("No characters.", ImVec2(o.x + 24 * c.scale, o.y + 8 * c.scale), kGrey);
        }
    }
}

void GameUi::drawCastBar(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    // While we cast: "menu casttime" (its sprite reads "Casting"), with the bar (framesus 59) scaled by progress at
    // the element's offset (22, 6). Progress from the spell's cast time (quarter seconds, file 86).
    const auto& w = *d.world;
    const auto* sp = w.castSpell && d.spells ? d.spells->find(w.castSpell) : nullptr;
    const auto& ls = r_.resources().layouts;
    const auto  it = ls.find("menu casttime");
    if (!sp || sp->castTime == 0 || it == ls.end())
    {
        return;
    }
    const float total = static_cast<float>(sp->castTime) * 0.25f;
    const float done  = std::clamp(std::chrono::duration<float>(d.now - w.castStarted).count() / total, 0.0f, 1.0f);
    dat::UiLayout l   = it->second;
    l.elements.clear();
    const ImVec2 o   = drawLayout(p, c, l, {});
    const auto*  bar = r_.resources().sprite("menu framesus", 59);
    if (!bar || bar->size() < 4)
    {
        return;
    }
    // Quad 0 is the bar body (x 26..90): drawn from its left end out to the progress. Quads 1-3: end caps and "%".
    dat::UiSprite body{ (*bar)[0] };
    for (std::size_t i = 0; i < 8; i += 2)
    {
        body[0].xy[i] = static_cast<int16_t>(26 + std::lround((body[0].xy[i] - 26) * done));
    }
    p.sprite(body, o);
    p.sprite(dat::UiSprite(bar->begin() + 1, bar->end()), o);
    p.textShp(std::to_string(static_cast<int>(done * 100)), ImVec2(o.x + 113 * c.scale, o.y + 6 * c.scale), kWhite, true);
}

namespace
{
// C2S 0x110 FISHING_2 {u32 UniqueNo, i32 para, u16 ActIndex, i8 mode, u8 pad, i32 para2}.
std::vector<std::byte> fishingPacket(uint32_t charId, uint16_t actIndex, int8_t mode, int32_t para, uint32_t para2)
{
    std::vector<std::byte> pkt(16);
    std::memcpy(pkt.data(), &charId, 4);
    std::memcpy(pkt.data() + 4, &para, 4);
    std::memcpy(pkt.data() + 8, &actIndex, 2);
    pkt[10] = static_cast<std::byte>(mode);
    std::memcpy(pkt.data() + 12, &para2, 4);
    return pkt;
}
} // namespace

void GameUi::fishingTick(const GameUiData& d, const GameUiActions& act)
{
    // The game's fishing loop for our own character (CODE 0x10089030, run every frame while the status is 56..62:
    // 56 line out, 57 fish on, 58 caught, 59 rod break, 60 line break, 61 monster, 62 stop).
    const auto& w   = *d.world;
    auto&       f   = fish_;
    auto        rnd = [&f] {
        f.rng ^= f.rng << 13;
        f.rng ^= f.rng >> 17;
        f.rng ^= f.rng << 5;
        return static_cast<int>(f.rng & 0x7FFF); // as the CRT's rand()
    };
    constexpr auto fcc = [](const char (&s)[5]) {
        return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8 |
               static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24;
    };
    auto fsh = [&](char n) { return fcc({ 'f', 's', 'h', n, 0 }); };
    auto run  = [&](uint32_t file, uint32_t name) { fishFx_.push_back({ file, name, false }); };
    auto stop = [&](uint32_t name) { fishFx_.push_back({ 0, name, true }); };
    auto send = [&](int8_t mode, int32_t para, uint32_t para2) {
        spdlog::debug("fishing: C2S 0x110 mode {} para {} para2 {} (status {}, stamina {}/{})", mode, para, para2, w.animation,
                      f.stamina, f.max);
        if (act.send)
        {
            act.send(0x110, fishingPacket(w.charId, w.actIndex, mode, para, para2));
        }
    };
    // The arrow effects (effect file 3727 = 0x39f + 2800, schedulers mai0..maib; CODE 0x10089fb0 by table 0x1035b598):
    // base (2 x gold - dir + 1) x 3, + 0 shown, + 1 hit, + 2 gone (a wrong key, or the window over).
    auto arrowFx = [&](int idx) {
        static constexpr char kHex[] = "0123456789ab";
        run(3727, fcc({ 'm', 'a', 'i', kHex[idx], 0 }));
    };
    auto clearArrow = [&] {
        if (f.arrowFx >= 0)
        {
            for (const int idx : { 2, 5, 8, 11 }) // CODE 0x10089f20
            {
                arrowFx(idx);
            }
            f.arrowFx = -1;
        }
    };
    auto release = [&] {
        // The fish object freed (CODE 0x10089030 phase 3): the gauge goes, every fishing scheduler stops.
        f.active = false;
        clearArrow();
        for (char n = '0'; n <= '9'; ++n)
        {
            stop(fsh(n));
        }
    };

    const bool fishing = w.animation >= 56 && w.animation <= 62;
    if (!fishing)
    {
        if (f.casting || f.active)
        {
            release();
        }
        f        = Fishing{ .rng = f.rng };
        f.serial = w.fishSerial; // a 0x115 seen while not fishing starts nothing
        return;
    }
    if (!f.casting)
    {
        f.casting  = true;
        f.phase    = 0;
        f.last     = d.now;
        // The server's hook delay (S2C 0x037 +0x4A, seconds), else the game's own 900..1500 ticks (CODE 0x10090b79).
        f.hookWait = static_cast<float>(w.fishingTimer > 0 ? w.fishingTimer * 60 : fishDefaultHookWait(rnd()));
        f.castLock = static_cast<float>(kFishCastLock);
    }
    // Whole ticks this frame (the game counts its frame time in 1/60 s ticks, at least 1).
    const float frameTicks = std::chrono::duration<float>(d.now - f.last).count() * 60.0f;
    f.tickCarry += frameTicks;
    f.last = d.now;
    const int ticks = static_cast<int>(f.tickCarry);
    f.tickCarry -= static_cast<float>(ticks);

    // A fish on the line (S2C 0x115, CODE 0x1009fc60; one already on keeps its values).
    if (w.fishSerial != f.serial && f.active)
    {
        f.serial = w.fishSerial;
    }
    if (w.fishSerial != f.serial)
    {
        f.serial     = w.fishSerial;
        const auto& fi = w.fish;
        f.active     = true;
        f.max        = fi.stamina;
        f.stamina    = fi.stamina;
        f.shown      = fi.stamina;
        f.arrowDelay = std::max<int>(fi.arrowDelay, 1);
        f.regen      = static_cast<int>(fi.regen) - 0x80;
        f.rate       = std::max<int>(fi.moveFrequency, 1) * 20;
        f.damage     = fi.arrowDamage;
        f.heal       = fi.arrowRegen;
        f.timeLeft   = static_cast<int>(fi.time) * 60;
        f.warned     = false;
        f.large      = (fi.sense & 1) != 0;
        f.keen       = (fi.sense & 2) != 0;
        f.special    = fi.special;
        f.stage      = 0;
        f.keys.clear();
        spdlog::debug("fishing: S2C 0x115 stamina {} arrow_delay {} regen {} move_frequency {} damage {} heal {} time {} sense {}",
                      fi.stamina, fi.arrowDelay, fi.regen, fi.moveFrequency, fi.arrowDamage, fi.arrowRegen, fi.time, fi.sense);
        stop(fsh('0')); // the cast's scheduler ends once a fish is on (CODE 0x100892a4)
        if (f.keen)
        {
            run(3705, fcc("main")); // 0x389 + 2800 'main' (CODE 0x1009fdb6)
        }
    }
    // Presses: Enter reels in (actions 0x3e/0x0e), Esc gives up (0x3e/0x0f), the cursor keys answer an arrow (axis
    // 0x3f/4; the game counts a press, not a held key: 0x10089db0).
    bool enter = false, escape = false;
    int  axis  = 0;
    while (!f.keys.empty())
    {
        const auto k = f.keys.front();
        f.keys.pop_front();
        enter  = enter || k == SDLK_RETURN || k == SDLK_KP_ENTER;
        escape = escape || k == SDLK_ESCAPE;
        if (axis == 0 && (k == SDLK_LEFT || k == SDLK_RIGHT))
        {
            axis = k == SDLK_RIGHT ? 1 : -1;
        }
    }
    if (autoFish_ && f.active)
    {
        if (f.stamina <= 0)
        {
            enter = true;
        }
        else if (f.stage == 2 && f.wait > 0 && f.wait < 12)
        {
            axis = f.dir == 0 ? 1 : -1;
        }
    }

    switch (f.phase)
    {
        case 0: // line out: after the server's delay, ask whether something bit (CODE 0x10089030 phase 0)
            // Enter is remembered (and stops the hook check) even during the cast's lock; Esc only counts after it.
            if (!f.reelQueued)
            {
                f.hookWait -= static_cast<float>(ticks);
                if (f.hookWait <= 0.0f)
                {
                    send(2, 0, 0); // RequestCheckHook
                    f.phase = 1;
                    break;
                }
            }
            f.reelQueued = f.reelQueued || enter;
            if (f.castLock > 0.0f)
            {
                f.castLock -= static_cast<float>(ticks);
                break;
            }
            if (escape || f.reelQueued)
            {
                send(3, 0xC9, 0); // reeled back in before a bite
                f.phase = 3;
            }
            break;
        case 1:
        {
            if (w.animation >= 58) // the server decided (caught, broke, lost, gave up)
            {
                f.phase = 3;
                break;
            }
            if (!f.active)
            {
                break; // waiting for the bite (0x115) or the server's "You didn't catch anything."
            }
            // The minigame (CODE 0x10089780).
            if (f.stamina > 0)
            {
                switch (f.stage)
                {
                    case 0:
                        f.wait = f.idle = f.right = f.wrong = 0;
                        f.stage = 1;
                        // A golden arrow next (CODE 0x10089e10): the 0x115 intuition / (two rolls of 0..50) is at least 1.
                        {
                            const int a = std::max(rnd() % 51, 1), b = std::max(rnd() % 51, 1);
                            f.gold      = f.special / static_cast<uint32_t>(a + b) != 0;
                        }
                        break;
                    case 1: // between arrows
                        f.idle += ticks;
                        if (f.idle > 30 && axis != 0)
                        {
                            // Pulling for nothing: the frame's ticks x arrow_regen / 2 (CODE 0x1008985b). The game's frame
                            // is a tick or more; ours can be far shorter, so a press counts at least one tick (INFERRED:
                            // the game at 60 fps), not the fraction that rounded to nothing on most frames above 60 fps; a
                            // hitch counts 4 at most, as the regen does.
                            f.stamina += static_cast<int>(std::clamp(frameTicks, 1.0f, 4.0f) * static_cast<float>(f.heal) * 0.5f);
                            f.wrong = 0;
                        }
                        f.wait += ticks * f.rate;
                        if (f.wait >= 6000)
                        {
                            f.strike = 0;
                            f.dir    = rnd() & 1;
                            f.wait   = (rnd() % (f.arrowDelay * 2) + 2) * 10;
                            for (const char n : { '7', '8', '9' })
                            {
                                stop(fsh(n));
                            }
                            run(0, fsh(f.dir == 0 ? '9' : '8'));
                            f.arrowFx = fishArrowFx(f.gold, f.dir);
                            arrowFx(f.arrowFx);
                            spdlog::debug("fishing: {} {} arrow (mai{}), {} ticks", f.gold ? "gold" : "silver", f.dir == 0 ? "right" : "left",
                                          f.arrowFx, f.wait);
                            f.idle = f.right = f.wrong = 0;
                            if (f.gold)
                            {
                                f.wait = static_cast<int>(static_cast<float>(f.wait) * 0.7f);
                            }
                            f.stage = 2;
                        }
                        break;
                    case 2: // an arrow is up
                        f.strike -= ticks;
                        if (f.strike < 0)
                        {
                            run(0, fcc(f.large ? "hitl" : "hits")); // the fish strikes (table 0x1035b580)
                            f.strike = (rnd() % 6 + 5) * 60;
                        }
                        f.wait = std::max(f.wait - ticks, 0);
                        if (f.wait != 0)
                        {
                            // Right arrow (dir 0) wants a press to the right, left (dir 1) one to the left.
                            if (axis != 0)
                            {
                                ((axis > 0) == (f.dir == 0) ? f.right : f.wrong) += 1;
                            }
                            if (f.right > f.wrong)
                            {
                                f.stamina -= f.gold ? f.damage * 2 : f.damage;
                                spdlog::debug("fishing: arrow answered, stamina {}/{}", f.stamina, f.max);
                                f.wait = 0;
                                if (f.arrowFx >= 0)
                                {
                                    arrowFx(f.arrowFx + 1);
                                }
                            }
                            else if (f.wrong > f.right)
                            {
                                f.stamina += std::max(f.gold ? static_cast<int>(static_cast<float>(f.heal) * 0.25f) : f.heal, 1); // CODE 0x10089cf6
                                spdlog::debug("fishing: wrong key, stamina {}/{}", f.stamina, f.max);
                                f.wait = 0;
                                if (f.arrowFx >= 0)
                                {
                                    arrowFx(f.arrowFx + 2);
                                }
                            }
                            f.right = f.wrong = 0;
                            break;
                        }
                        for (const char n : { '8', '9' })
                        {
                            stop(fsh(n));
                        }
                        run(0, fsh('7'));
                        f.stage = 0;
                        clearArrow();
                        break;
                    default: break;
                }
                f.stamina = std::clamp(f.stamina + f.regen * std::min(ticks, 4), 0, f.max);
            }
            else
            {
                clearArrow();
            }
            // The gauge eases toward the stamina once a game frame (CODE 0x10089ba1), here once a tick (INFERRED: the
            // game's 60 fps step).
            for (int t = 0; t < std::min(ticks, 4); ++t)
            {
                f.shown = fishGaugeEase(f.shown, f.stamina);
            }
            // Then the player's choice and the clock (CODE 0x100892b0..0x100893e9).
            if (enter)
            {
                send(3, f.max > 0 ? f.stamina * 100 / f.max : 0, f.special); // the fish's stamina left, in %
                f.phase = 2;
            }
            else if (escape)
            {
                send(3, 200, 0); // give up
                f.phase = 2;
            }
            else
            {
                f.timeLeft -= ticks;
                if (f.timeLeft <= 0)
                {
                    send(3, 300, 0); // out of time: "You lost your catch."
                    f.phase = 2;
                }
                else if (!f.warned && f.timeLeft <= 0x1A3)
                {
                    send(5, f.timeLeft / 60, 0); // RequestPotentialTimeout: the warning line
                    f.warned = true;
                }
            }
            break;
        }
        case 2: // the result asked for: wait for the server's status
            if (w.animation >= 58)
            {
                f.phase = 3;
            }
            else if (w.animation != 57)
            {
                for (const char n : { '7', '8', '9' })
                {
                    stop(fsh(n));
                }
                f.phase = 3;
            }
            break;
        default: // 3: once the result's motion (fsh2..fsh6) has played, release the line (RequestRelease)
        {
            clearArrow();
            for (const char n : { '0', '7', '8', '9' })
            {
                stop(fsh(n));
            }
            bool busy = false;
            for (const char n : { '2', '3', '4', '5', '6' })
            {
                busy = busy || (d.fishingRunning && d.fishingRunning(fsh(n)));
            }
            if (!busy && !f.released)
            {
                send(4, 0, 0);
                f.released = true;
                release();
            }
            break;
        }
    }
}

void GameUi::drawFishing(UiPainter& p, const UiCanvas& c, const GameUiData& /*d*/)
{
    if (!fish_.active || fish_.max <= 0)
    {
        return;
    }
    // The fish's stamina (CODE 0x10202660 from 0x102041a1): "Fish" and a party HP gauge stretched 2x, centred
    // horizontally 58 left of the middle and 0.2 of the height down (H/2 - 0.6 x H/2); the label in another colour
    // when the stamina is gone (colour table entry 9 instead of 0; INFERRED: gold). The arrows are effects (file 3727).
    const float  s   = c.scale;
    const ImVec2 at((c.width / 2 - 58) * s, c.height * 0.2f * s);
    const int    pct = fishGaugePercent(fish_.shown, fish_.max);
    p.textShp("Fish", at, pct > 0 ? kWhite : kGold);
    const ImVec2 g(at.x, at.y + 7 * s);
    if (pct > 0)
    {
        p.sprite("menu windowps", 97, ImVec2(g.x + 8 * s, g.y + 1 * s), kWhite, 0, 2.0f * static_cast<float>(pct) / 100.0f, 1);
    }
    p.sprite("menu windowps", 38, g, kWhite, 0, 2, 1);
}

void GameUi::beginSubTarget(std::string line, std::string token, uint16_t targets, const GameUiData& d)
{
    sub_.active  = true;
    sub_.line    = std::move(line);
    sub_.token   = std::move(token);
    sub_.targets = targets;
    sub_.keys.clear();
    // The cursor starts on the current target when it qualifies, else on the nearest candidate.
    const auto ids = subTargetCandidates(d);
    sub_.current   = d.target && std::ranges::find(ids, d.target->id) != ids.end() ? d.target->id : ids.empty() ? 0 : ids.front();
}

bool GameUi::menuSubTarget(std::string_view command, std::string_view name, uint16_t targets, const GameUiData& d)
{
    // The game runs `/magic "%s" %d` (DATA 0x10386970), `/jobability "%s" %d` (0x103861f0) or `/weaponskill "%s" %d`
    // (0x1038621c) on the pick (callbacks 0x1022cca0, 0x10224a50) and closes the list; Esc (0x1022cd20) shows the
    // hidden menus again. Nobody to pick: 55646 #90 (spells, 0x1022c386) / #93 (abilities, 0x10223dc5); the list stays.
    auto resume = std::move(stack_);
    stack_.clear();
    beginSubTarget(fmt::format("{} \"{}\" <st>", command, name), "<st>", targets, d);
    if (sub_.current == 0)
    {
        sub_.active = false;
        stack_      = std::move(resume);
        notesOut_.emplace_back(command == "/magic" ? kNoSpellTarget : kNoAbilityTarget);
        return false;
    }
    sub_.resume = std::move(resume);
    spdlog::info("sub-target: {} (targets 0x{:02x}), cursor on {:x}", sub_.line, targets, sub_.current);
    return true;
}

std::vector<uint32_t> GameUi::subTargetCandidates(const GameUiData& d) const
{
    // <st>: anyone; <stpc>: players; <stnpc>: NPCs and monsters; <stpt> / <stal>: party / alliance members. Nearest
    // first, ourselves (distance 0) included where we qualify. Valid-target bits (sub_.targets) narrow the set
    // further: 0x01 ourselves, 0x02 other players, 0x04 party, 0x08 alliance, 0x10 NPCs, 0x20 monsters.
    const auto& w    = *d.world;
    const auto  tok  = lower(sub_.token);
    auto        inParty = [&](uint32_t id, bool alliance) {
        return std::ranges::any_of(w.party, [&](const client::PartyMember& m) { return m.id == id && (alliance || m.partyNo == 0); });
    };
    auto valid = [&](const client::Entity* e) {
        const uint16_t t = sub_.targets;
        if (t == 0)
        {
            return true;
        }
        if (!e)
        {
            return (t & 0x01) != 0;
        }
        return ((t & 0x02) && e->isPc) || ((t & 0x04) && inParty(e->id, false)) || ((t & 0x08) && inParty(e->id, true)) ||
               ((t & 0x10) && !e->isPc && !e->isMob) || ((t & 0x20) && e->isMob);
    };
    std::vector<uint32_t> out;
    if (tok != "<stnpc>" && valid(nullptr))
    {
        out.push_back(w.charId);
    }
    for (const auto& [dist, e] : d.nearby)
    {
        const bool ok = tok == "<st>"      ? true
                      : tok == "<stpc>"    ? e->isPc
                      : tok == "<stnpc>"   ? !e->isPc
                      : tok == "<stpt>"    ? inParty(e->id, false)
                                           : inParty(e->id, true);
        if (ok && e->id != w.charId && valid(e))
        {
            out.push_back(e->id);
        }
    }
    return out;
}

void GameUi::drawSubTarget(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    if (!sub_.active)
    {
        return;
    }
    // A small window above the target window: the token and the name under the cursor (the game shows a second
    // cursor; its window graphics are not traced).
    std::string name = d.world->charName;
    for (const auto& [dist, e] : d.nearby)
    {
        if (e->id == sub_.current)
        {
            name = e->name;
        }
    }
    const float  s = c.scale;
    const ImVec2 size(150 * s, 30 * s);
    const ImVec2 at((c.width - 172) * s, c.height * 0.62f * s); // canvas size is in authored units
    p.frame(at, size);
    p.textShp(sub_.token, ImVec2(at.x + 8 * s, at.y + 3 * s), kGold);
    p.textShp(sub_.current ? name : std::string("(nobody)"), ImVec2(at.x + 8 * s, at.y + 15 * s), kWhite);
}

void GameUi::drawDead(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    const auto& ls = r_.resources().layouts;
    if (const auto it = ls.find("menu dead"); it != ls.end())
    {
        LayoutState st;
        st.cursor      = stack_.empty() ? 1 : 0;
        st.tick        = tick_;
        const ImVec2 o = drawLayout(p, c, it->second, st);
        // "Time Left:" value: the forced home point's time less 6 minutes, as m:ss at (75, 6) in the window's grey
        // (CODE 0x1012de20: ticks / 60 -> "%d:%02d", 0x101c9330 at left + 0x4b, top + 6, 0x80808080).
        if (const auto* w = d.world; w && w->deadCounter != 0)
        {
            const auto   since = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - w->deadCounterAt).count();
            const long   left  = std::max<long>(0, static_cast<long>(w->deadCounter / 60) - 360 - static_cast<long>(since));
            p.textShp(fmt::format("{}:{:02}", left / 60, left % 60), ImVec2(o.x + 75 * c.scale, o.y + 6 * c.scale), UiColor{ 0x80, 0x80, 0x80, 0x80 });
        }
    }
}

std::vector<client::InventoryItem> GameUi::sortedInventory(const client::WorldSnapshot& w) const
{
    // The inventory's items (container 0, gil in slot 0 left out) in the client's display order: the last sort's order,
    // then anything newer in slot order (the server keeps no order; config_misc_re.md §154).
    std::vector<client::InventoryItem> items;
    for (const auto& it : w.inventory)
    {
        if (it.container == 0 && it.slot != 0)
        {
            items.push_back(it);
        }
    }
    auto rank = [&](uint8_t slot) {
        const auto at = std::find(invOrder_.begin(), invOrder_.end(), slot);
        return at == invOrder_.end() ? static_cast<std::ptrdiff_t>(invOrder_.size()) + slot : at - invOrder_.begin();
    };
    std::stable_sort(items.begin(), items.end(), [&](const client::InventoryItem& a, const client::InventoryItem& b) { return rank(a.slot) < rank(b.slot); });
    return items;
}

bool GameUi::itemSortFocus(ItemSortMove move)
{
    switch (move)
    {
        case ItemSortMove::ToSortWindow:
            manualSort_ = false;
            manualPick_ = -1;
            open(Kind::ItemSort);
            return true;
        case ItemSortMove::ToList: stack_.pop_back(); return true;
        case ItemSortMove::EndManualSort:
            manualSort_ = false;
            manualPick_ = -1;
            return true;
        case ItemSortMove::None: break;
    }
    return false;
}

void GameUi::sortInventory(const GameUiData& d, const GameUiActions& act)
{
    // Sort (CODE 0x1021c340): the display order by category, then item id (cfg 202 = 0), or by name with the same items
    // next to each other (1); then C2S 0x03A so the server merges partial stacks (LSB allows one a second).
    const auto& w      = *d.world;
    const bool  byName = cfgValue(d, 202) == 1;
    std::vector<client::InventoryItem> items;
    for (const auto& it : w.inventory)
    {
        if (it.container == 0 && it.slot != 0)
        {
            items.push_back(it);
        }
    }
    auto info = [&](uint16_t id) { return d.items ? d.items->find(id) : std::nullopt; };
    std::stable_sort(items.begin(), items.end(), [&](const client::InventoryItem& a, const client::InventoryItem& b) {
        if (byName)
        {
            const auto na = info(a.itemId), nb = info(b.itemId);
            const auto sa = na ? na->name : std::string(), sb = nb ? nb->name : std::string();
            if (sa != sb)
            {
                return sa < sb;
            }
            return a.slot < b.slot;
        }
        // INFERRED key: the item record's type (the game's own sort key, [rec-8], is not traced), then the id.
        const auto ia = info(a.itemId), ib = info(b.itemId);
        const int  ta = ia ? ia->type : 0xFFFF, tb = ib ? ib->type : 0xFFFF;
        return ta != tb ? ta < tb : a.itemId != b.itemId ? a.itemId < b.itemId : a.slot < b.slot;
    });
    invOrder_.clear();
    for (const auto& it : items)
    {
        invOrder_.push_back(it.slot);
    }
    if (act.send && std::chrono::steady_clock::now() - lastSort_ > std::chrono::milliseconds(1100))
    {
        lastSort_ = std::chrono::steady_clock::now();
        act.send(0x03A, std::vector<std::byte>(4)); // ITEM_STACK, container 0
    }
}

void GameUi::manualSortPick(const GameUiData& d, const GameUiActions& act, uint8_t slot)
{
    // Manual Sort (help 5 "Select item to move." / 6 "Select item to switch with the first. (Selecting the same item
    // stack will separate one of the items from the stack.)"): the first pick, then the second. The same stack splits
    // one item off (C2S 0x029 ItemNum 1 -> a free slot); another stack of the same item takes the first in (0x029 with
    // its slot, LSB's "unite"); anything else swaps the two places in the list (the display order is the client's).
    if (manualPick_ < 0)
    {
        manualPick_ = slot;
        return;
    }
    const auto first = static_cast<uint8_t>(manualPick_);
    manualPick_      = -1;
    const auto& w    = *d.world;
    const client::InventoryItem *a = nullptr, *b = nullptr;
    for (const auto& it : w.inventory)
    {
        if (it.container == 0 && it.slot == first)
        {
            a = &it;
        }
        if (it.container == 0 && it.slot == slot)
        {
            b = &it;
        }
    }
    if (!a || !b)
    {
        return;
    }
    auto move = [&](uint32_t count, uint8_t to) {
        if (act.send)
        {
            std::vector<std::byte> pkt(8);
            std::memcpy(pkt.data(), &count, 4);
            pkt[4] = std::byte{ 0 };
            pkt[5] = std::byte{ 0 };
            pkt[6] = static_cast<std::byte>(first);
            pkt[7] = static_cast<std::byte>(to);
            act.send(0x029, std::move(pkt));
        }
    };
    if (first == slot)
    {
        if (a->count > 1 && !isWorn(w, 0, first))
        {
            move(1, 82);
        }
        return;
    }
    const auto info = d.items ? d.items->find(a->itemId) : std::nullopt;
    if (a->itemId == b->itemId && info && b->count < info->stackSize)
    {
        move(a->count, slot);
        return;
    }
    // Swap the two in the display order (every item placed first, so both have a place).
    const auto items = sortedInventory(w);
    invOrder_.clear();
    for (const auto& it : items)
    {
        invOrder_.push_back(it.slot);
    }
    const auto pa = std::find(invOrder_.begin(), invOrder_.end(), first), pb = std::find(invOrder_.begin(), invOrder_.end(), slot);
    if (pa != invOrder_.end() && pb != invOrder_.end())
    {
        std::iter_swap(pa, pb);
    }
}

void GameUi::openEquipment()
{
    if (equipRefusal_)
    {
        notesOut_.emplace_back(equipRefusal_);
        return;
    }
    open(Kind::Equip);
}

bool GameUi::equipFromMenu(const GameUiData& d, const GameUiActions& act, uint8_t container, uint8_t index, uint8_t slot)
{
    // CODE 0x10132700 (the Equipment list) and the /equip command use the same routine.
    const auto& w = *d.world;
    if (!act.equip || slot >= 16 || !d.items)
    {
        return false;
    }
    const auto ctx = equipContext(w);
    const auto [wornBag, wornSlot] = w.equipment[slot];
    if (index == 0)
    {
        // Remove: nothing for a slot encumbrance holds, or an empty one.
        if (((ctx.blockedSlots >> slot) & 1) || wornSlot == 0)
        {
            return false;
        }
        act.equip(wornBag, 0, slot);
        return true;
    }
    const client::InventoryItem* item = nullptr;
    for (const auto& it : w.inventory)
    {
        if (it.container == container && it.slot == index)
        {
            item = &it;
        }
    }
    const auto info = item ? d.items->find(item->itemId) : std::nullopt;
    if (!info)
    {
        return false;
    }
    if (slot == 1)
    {
        // A weapon that also goes in the main hand, into the sub slot: 55646 #139 / #152 / #153.
        std::optional<dat::ItemInfo> main;
        if (const auto [mb, ms] = w.equipment[0]; ms != 0)
        {
            for (const auto& it : w.inventory)
            {
                if (it.container == mb && it.slot == ms)
                {
                    main = d.items->find(it.itemId);
                }
            }
        }
        if (const int no = subWeaponRefusal(*info, main, w.mainJob, w.subJob))
        {
            if (act.note)
            {
                act.note(no == 139 ? "You need the Dual Wield ability to equip that as a sub-weapon."
                         : no == 152 ? "You must have a main weapon equipped before equipping a sub-weapon."
                                     : "You can only equip a sub-weapon when your main weapon is a one-handed weapon.");
            }
            return false;
        }
    }
    if (!canEquip(*info, ctx) || !fitsSlot(*info, slot, ctx.blockedSlots) || (container != 0 && !bagExists(w, container)))
    {
        return false;
    }
    // Already there: the same item (by id) from the same container, or the very same stack.
    if (wornSlot != 0 && wornBag == container)
    {
        for (const auto& it : w.inventory)
        {
            if (it.container == wornBag && it.slot == wornSlot && it.itemId == item->itemId)
            {
                return false;
            }
        }
    }
    act.equip(container, index, slot);
    return true;
}

void GameUi::draw(ImDrawList& dl, const UiCanvas& canvas, const GameUiData& d, const GameUiActions& act)
{
    if (d.world)
    {
        // CODE 0x10133060: synthesis (status 44) -> 55646 #78, fishing -> #77. (Its third check, #168 "That cannot be
        // used right now." while flag 0x198cc bit 0 is clear, is a load state not tracked here.)
        equipRefusal_ = d.world->animation == 44 ? "You cannot use that command during synthesis."
                      : fish_.casting            ? "You cannot use that command while fishing."
                                                 : nullptr;
        // Config Gameplay: autosort (cfg 154 = 1) sorts the inventory whenever its item count changes (CODE 0x1021c0c6).
        const auto count = static_cast<std::size_t>(std::count_if(d.world->inventory.begin(), d.world->inventory.end(),
                                                                   [](const client::InventoryItem& it) { return it.container == 0 && it.slot != 0; }));
        if (count != invCountSeen_)
        {
            const bool first = invCountSeen_ == static_cast<std::size_t>(-1);
            invCountSeen_    = count;
            if (!first && cfgValue(d, 154) == 1)
            {
                sortInventory(d, act);
            }
        }
    }
    ++tick_;
    lastCanvas_  = canvas;
    data_        = &d;
    updateBattleCodes(d);
    // Our grid square for the title window while a map of where we are is up: the Map screen on our own map, or an
    // event's map of this zone (CODE 0x101fa570; not for pictures).
    mapGridRef_.clear();
    if (d.world && d.maps)
    {
        const bool menuMap  = mapOpen_ && mapZone_ == d.world->zone && mapId_ == d.mapId &&
                             std::any_of(stack_.begin(), stack_.end(), [](const Menu& m) { return m.kind == Kind::Map; });
        const bool eventMap = eventMapZone_ == d.world->zone && eventMapId_ == d.mapId;
        if (const auto* rec = (menuMap || eventMap) ? d.maps->find(d.world->zone, d.mapId) : nullptr)
        {
            const auto [u, v] = rec->project(d.playerX, d.playerZ);
            mapGridRef_       = mapGridRef(u, v);
        }
    }
    text_        = d.text;
    loginActive_ = false;
    login_       = nullptr;
    if (!d.world)
    {
        return;
    }
    lobbyActive_ = d.world->choosingCharacter;
    if (d.world->choosingCharacter)
    {
        UiPainter lp(r_, dl, canvas.scale);
        drawLobby(lp, canvas, d, act);
        return;
    }
    // S2C 0x03E: an NPC's shop opens its goods list.
    if (d.world->shopSerial != shopSeen_)
    {
        shopSeen_ = d.world->shopSerial;
        stack_.clear();
        open(Kind::ShopMain);
    }
    // Guild shops (S2C 0x086; CODE 0x101ed3c0). Open: "menu guildsho", remembering where we stand (the game refuses
    // with 55646 #69 while certain windows are up; here any open window gives way, as with NPC shops: INFERRED).
    // Closed / holiday: the hours or the holiday into the log and the guild's windows close, unless it is the hour
    // tick's "passive" close (Time bit 31) and no guild window is open.
    auto isGuild = [](Kind k) {
        return k == Kind::GuildMain || k == Kind::GuildBuy || k == Kind::GuildBuyQty || k == Kind::GuildSell || k == Kind::GuildSellPick ||
               k == Kind::GuildSellQty;
    };
    auto closeGuild = [&] {
        while (!stack_.empty() && std::any_of(stack_.begin(), stack_.end(), [&](const Menu& x) { return isGuild(x.kind); }))
        {
            stack_.pop_back();
        }
    };
    const bool guildOpen = std::any_of(stack_.begin(), stack_.end(), [&](const Menu& x) { return isGuild(x.kind); });
    if (d.world->guild.openSerial != guildOpenSeen_)
    {
        guildOpenSeen_ = d.world->guild.openSerial;
        stack_.clear();
        open(Kind::GuildMain);
        guildAtX_ = d.playerX;
        guildAtZ_ = d.playerZ;
    }
    else if (d.world->guild.closeSerial != guildCloseSeen_)
    {
        guildCloseSeen_ = d.world->guild.closeSerial;
        if (guildOpen || !d.world->guild.passive)
        {
            for (const auto& line : d.world->guild.closeLines)
            {
                if (act.note)
                {
                    act.note(line);
                }
            }
            closeGuild();
        }
    }
    else if (guildOpen)
    {
        // Walking away: the guild window closes once we are 6 yalms from where it opened (CODE 0x101ed280: squared
        // distance on x / z against 36.0 at 0x103344b4).
        const float dx = d.playerX - guildAtX_, dz = d.playerZ - guildAtZ_;
        if (dx * dx + dz * dz > 36.0f)
        {
            closeGuild();
        }
    }
    // S2C 0x04C Command 2: the auction counter opened the auction house; ask for our sale slots (WorkCheck).
    if (d.world->auction.openSerial != aucOpenSeen_)
    {
        aucOpenSeen_ = d.world->auction.openSerial;
        stack_.clear();
        open(Kind::Auction);
        if (act.send)
        {
            act.send(0x04E, aucPacket(0x0A, -1));
        }
    }
    // /itemsearch: the server named the item; its auction window (Price History / Bid) opens, as at the counter.
    if (d.world->itemSearchSerial != itemSearchSeen_)
    {
        itemSearchSeen_ = d.world->itemSearchSerial;
        if (d.world->itemSearchId != 0)
        {
            aucItem_         = d.world->itemSearchId;
            aucStack_        = false;
            aucListCategory_ = -1; // no list behind it: auc3's Sort does nothing
            stack_.clear();
            open(Kind::Auction);
            open(Kind::AucItem);
        }
        else if (act.note)
        {
            act.note("Spelling of name is incorrect.");
        }
    }
    // Auction results: the listing fee (then the Yes/No to list), bids, listings, removals.
    const auto& auc = d.world->auction;
    const auto  newResults = std::min<std::size_t>(auc.resultSerial - aucResultSeen_, auc.results.size());
    aucResultSeen_         = auc.resultSerial;
    for (std::size_t r = auc.results.size() - newResults; r < auc.results.size(); ++r)
    {
        const auto [lastCommand, lastResult] = auc.results[r];
        const auto& a  = auc;
        auto        note = [&](const std::string& t) {
            if (act.note)
            {
                act.note(t);
            }
        };
        const auto info = d.items ? d.items->find(aucItem_) : std::nullopt;
        const auto item = info ? info->name : fmt::format("item #{}", aucItem_);
        switch (lastCommand)
        {
            case 0x04: // AskCommit
                if (aucWaitFee_)
                {
                    aucWaitFee_ = false;
                    if (lastResult == 1)
                    {
                        note(fmt::format("Listing {}{} for {} gil; the fee is {} gil.", item, aucStack_ ? " (stack)" : "", aucPrice_, a.fee));
                        open(Kind::AucYesNo, 1);
                    }
                    else
                    {
                        note("That item cannot be placed on auction.");
                    }
                }
                break;
            case 0x0E: // Bid
                note(lastResult == 1 ? fmt::format("You bought {}{} for {} gil.", item, aucStack_ ? " (stack)" : "", aucPrice_)
                                       : fmt::format("Your bid of {} gil for {} was not accepted.", aucPrice_, item));
                break;
            case 0x0B: // LotIn
                note(lastResult == 1 ? "Merchandise placed on auction." : "Failed to place the merchandise on auction.");
                break;
            default: break;
        }
    }
    // S2C 0x0F9: a raise or tractor offer opens its Yes/No prompt once.
    if (d.world->reviveSerial != reviveSeen_)
    {
        reviveSeen_ = d.world->reviveSerial;
        if (d.world->reviveOffer == 1 || d.world->reviveOffer == 2)
        {
            stack_.clear();
            open(Kind::Revive, d.world->reviveOffer);
        }
    }
    // Keys pressed on a key-assignment list (CODE 0x1019c3c0): assign to the row under the cursor; the row that held
    // the key loses it; the row's own key clears it; a key with no key top (or a digit in the shortcut list) beeps.
    while (!assignKeys_.empty())
    {
        const auto key = assignKeys_.front();
        assignKeys_.pop_front();
        if (stack_.empty() || stack_.back().kind != Kind::KeyAssign)
        {
            continue;
        }
        const auto& m    = stack_.back();
        const auto  list = m.param ? AssignList::Shortcut : AssignList::Compact;
        const int   row  = m.scroll + m.cursor - 1;
        auto        keys = assignment(d);
        const auto  out  = keys.assign(list, row, key);
        if (out.result == KeyAssign::Result::Refused || out.result == KeyAssign::Result::Ignored)
        {
            if (sound_ && out.result == KeyAssign::Result::Refused)
            {
                sound_(4u);
            }
            continue;
        }
        if (act.setConfig)
        {
            act.setConfig(KeyAssign::cfgId(list, row), KeyAssign::dik(keys.key(list, row)));
            if (out.moved >= 0)
            {
                act.setConfig(KeyAssign::cfgId(list, out.moved), 0);
            }
        }
        if (sound_)
        {
            sound_(2u);
        }
    }
    if (buffConfirm_)
    {
        // Confirm on a status icon: C2S 0x0F1 BUFFCANCEL {BuffNo u16, pad u16} when the status's record has "can
        // cancel" (CODE 0x10227a10 -> 0x100a04c0); otherwise nothing. The server answers with a new icon list (0x063).
        buffConfirm_      = false;
        const auto& buffs = d.world->buffs;
        if (logMode_ == LogMode::Buffs && buffCursor_ >= 0 && static_cast<std::size_t>(buffCursor_) < buffs.size() &&
            act.send)
        {
            const auto  id   = buffs[static_cast<std::size_t>(buffCursor_)].icon;
            const auto* info = statusInfoFor(d, id);
            if (info && info->canCancel)
            {
                std::vector<std::byte> pkt(4);
                std::memcpy(pkt.data(), &id, 2);
                act.send(0x0F1, std::move(pkt));
            }
        }
    }
    // Commands queued by input, now that the world and actions are at hand.
    while (!pending_.empty())
    {
        const auto cmd = pending_.front();
        pending_.pop_front();
        if (typing_ && cmd == Cmd::Confirm && inputDone_)
        {
            // A field's input line (a macro's name or line, a book's name): the text goes to the field, not the chat.
            auto done  = std::move(inputDone_);
            inputDone_ = nullptr;
            typing_    = false;
            done(input_);
            input_.clear();
            cursor_ = 0;
            continue;
        }
        if (typing_ && cmd == Cmd::Confirm)
        {
            if (!input_.empty() && (history_.empty() || history_.front() != input_))
            {
                history_.push_front(input_);
                if (history_.size() > 50)
                {
                    history_.pop_back();
                }
            }
            historyPos_ = -1;
            submitLine(input_, d, act);
            input_.clear();
            cursor_ = 0;
            typing_ = false;
            continue;
        }
        if (stack_.empty())
        {
            if (cmd == Cmd::Confirm)
            {
                if (d.dead && act.action)
                {
                    open(Kind::Revive, 0); // "Return to Home Point?"
                    stack_.back().cursor = yesNoCursor(kHomePointConfirmOnYes); // opens on No (CODE 0x1012dfbc)
                }
            }
            continue;
        }
        apply(cmd, d, act);
    }
    tradeSync(d, act); // player trade: the windows follow the server (game_ui_trade.cpp)
    if (partyTargetKey_ > 0 && act.setTarget)
    {
        // Same order as the party window: our party (party 0) by member number; solo = ourselves only.
        std::vector<uint32_t> ids;
        for (const auto& m : d.world->party)
        {
            if (m.partyNo == 0)
            {
                ids.push_back(m.id);
            }
        }
        if (ids.empty())
        {
            ids.push_back(d.world->charId);
        }
        // Locked on the monster we fight, the keys do nothing (target commands 6..11 return early, CODE 0x10158720).
        if (static_cast<std::size_t>(partyTargetKey_) <= ids.size() && !d.targetLocked)
        {
            act.setTarget(ids[static_cast<std::size_t>(partyTargetKey_ - 1)]);
        }
        partyTargetKey_ = 0;
    }
    // Sub-target: the cursor keys walk the candidates (nearest first); Enter runs the waiting line.
    if (sub_.active)
    {
        const auto ids = subTargetCandidates(d);
        auto       cur = std::ranges::find(ids, sub_.current);
        if (cur == ids.end())
        {
            sub_.current = ids.empty() ? 0 : ids.front();
            cur          = std::ranges::find(ids, sub_.current);
        }
        while (!sub_.keys.empty())
        {
            const auto k = sub_.keys.front();
            sub_.keys.pop_front();
            if (k == SDLK_ESCAPE)
            {
                sub_.active = false;
                if (stack_.empty())
                {
                    stack_ = std::move(sub_.resume); // back to the list the cursor came from
                }
                sub_.resume.clear();
                break;
            }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
            {
                sub_.active = false;
                sub_.resume.clear();
                if (sub_.current)
                {
                    sub_.last        = sub_.current;
                    std::string line = sub_.line;
                    line.replace(line.find(sub_.token), sub_.token.size(), fmt::format("<id:{:x}>", sub_.current));
                    spdlog::info("sub-target: {}", line);
                    macroQueue_.push_front(std::move(line));
                }
                break;
            }
            if (!ids.empty())
            {
                const bool back = k == SDLK_LEFT || k == SDLK_UP;
                auto       idx  = static_cast<std::size_t>(cur - ids.begin());
                idx             = back ? (idx + ids.size() - 1) % ids.size() : (idx + 1) % ids.size();
                sub_.current    = ids[idx];
                cur             = ids.begin() + static_cast<std::ptrdiff_t>(idx);
            }
        }
    }
    // Macro lines: one per frame, "/wait N" pauses (up to 60 s); a line waiting for its sub-target holds the rest.
    while (!macroQueue_.empty() && d.now >= macroResume_ && !sub_.active)
    {
        const auto q = macroQueue_.front();
        macroQueue_.pop_front();
        if (lower(q.text).starts_with("/wait"))
        {
            const double secs = std::clamp(std::atof(q.text.c_str() + 5), 0.0, 60.0);
            macroResume_      = d.now + std::chrono::milliseconds(static_cast<int>(secs * 1000));
            continue;
        }
        submitLine(q.text, d, act, q.mode);
        break;
    }
    stepMacro(d, act); // the running macro's next line into the queue (/wait, <wait N>, //, an empty line ends it)
    for (; !notesOut_.empty(); notesOut_.pop_front())
    {
        if (act.note)
        {
            act.note(notesOut_.front());
        }
    }
    if (hidden_)
    {
        return; // Hide/Show Window: queued input above still runs
    }
    // Mouse: hovering moves the cursor; a click confirms.
    if (!stack_.empty())
    {
        auto&       m = stack_.back();
        const auto* l = layout(m);
        if (l && (mouseMoved_ || click_))
        {
            if (const int id = hitTest(canvas, *l, mouse_); id > 0)
            {
                m.cursor = id;
                if (click_)
                {
                    confirm(d, act);
                }
            }
        }
    }
    paletteClick_ = click_ && stack_.empty(); // a click on the macro palette runs that key (drawMacroPalette)
    mouseMoved_   = false;
    click_        = false;

    UiPainter p(r_, dl, canvas.scale);
    drawChat(p, canvas, d);
    drawCompass(p, canvas, d);
    if (windowsShown.target)
    {
        drawTargetBuffs(p, canvas, d);
    }
    if (logMode_ == LogMode::Selected && d.help)
    {
        drawHelp(p, canvas, std::string(d.help->get(298))); // "Press the confirm button to display full-screen log window."
    }
    if (!typing_ && windowsShown.macroPalette)
    {
        const SDL_Keymod mods = heldMods();
        if (mods & SDL_KMOD_CTRL)
        {
            drawMacroPalette(p, canvas, 0);
        }
        else if (mods & SDL_KMOD_ALT)
        {
            drawMacroPalette(p, canvas, 1);
        }
    }
    if (windowsShown.target)
    {
        drawTarget(p, canvas, d);
    }
    if (windowsShown.party)
    {
        drawParty(p, canvas, d);
    }
    drawSubWindow(p, canvas, d, act);
    drawBuffs(p, canvas, d);
    if (logMode_ == LogMode::Buffs)
    {
        drawBuffSelection(p, canvas, d);
    }
    if (gaugeWindowShown())
    {
        drawGaugeWindow(p, canvas, d);
    }
    if (moneyShown())
    {
        drawMoney(p, canvas, d);
    }
    // With the Equipment grid the status window shows 32 lower, under the title window (retail captures; INFERRED).
    if (std::any_of(stack_.begin(), stack_.end(), [](const Menu& m) { return m.kind == Kind::Equip; }))
    {
        drawStatusWindow(p, canvas, d, 32);
    }
    drawCastBar(p, canvas, d);
    drawEventWindows(p, canvas, d); // event op 0xB4: target window, cast-time / event-time
    fishingTick(d, act);
    drawFishing(p, canvas, d);
    drawSubTarget(p, canvas, d);
    drawNumberEntry(p, canvas);
    if (!stack_.empty() && stack_.back().kind == Kind::AucYesNo)
    {
        // The question the Yes/No answers (55651 #59 "Place this bid?", #56 "Place this merchandise on auction?").
        const float  s  = canvas.scale;
        const char*  q  = stack_.back().param == 0 ? "Place this bid?" : "Place this merchandise on auction?";
        const ImVec2 at((canvas.width - 300) * s, 30 * s);
        p.frame(at, ImVec2(196 * s, 18 * s));
        p.textShp(q, ImVec2(at.x + 8 * s, at.y + 3 * s), kWhite);
    }
    if (d.dead)
    {
        drawDead(p, canvas, d);
    }
    // Help line for the element under the hand (its first string id; file 55651), as in the game's help window.
    if (!stack_.empty() && d.help)
    {
        const auto& m = stack_.back();
        const auto* l = layout(m);
        const auto* e = l && !isList(m.kind) ? l->element(static_cast<uint16_t>(m.cursor)) : nullptr;
        // Lists: the window record's own help ("menu inventor": 3 "Select an item.") when the row has none.
        const int   id = e ? e->text1 : l && isList(m.kind) ? l->window.text1 : -1;
        std::string listHelp;
        if (isList(m.kind))
        {
            auto       list = rows(m, d, act);
            const auto i    = static_cast<std::size_t>(m.scroll + m.cursor - 1);
            listHelp        = i < list.size() ? list[i].help : std::string();
        }
        if (!listHelp.empty())
        {
            drawHelp(p, canvas, listHelp);
        }
        else if (const auto mh = isMacroKind(m.kind) ? macroHelp(m, d) : std::string(); !mh.empty())
        {
            drawHelp(p, canvas, mh);
        }
        else if (m.kind == Kind::ShopBuy && d.help && m.cursor == 1 && static_cast<std::size_t>(m.param) < d.world->shop.size())
        {
            // "Purchase this merchandise?" with the quantity and total (Left/Right change the quantity).
            const auto price = d.world->shop[static_cast<std::size_t>(m.param)].price;
            drawHelp(p, canvas, fmt::format("{} x{} ({} gil)", d.help->get(464), shopQty_, price * shopQty_));
        }
        else if (m.kind == Kind::GuildBuyQty && d.help && m.cursor == 1 && static_cast<std::size_t>(m.param) < d.world->guild.lists[0].items.size())
        {
            // The guild's goods take the NPC shop's prompt with the quantity and total (Left/Right; INFERRED: the game
            // asks the number in its number window first).
            const auto price = d.world->guild.lists[0].items[static_cast<std::size_t>(m.param)].gil();
            drawHelp(p, canvas, fmt::format("{} x{} ({} gil)", d.help->get(464), shopQty_, price * shopQty_));
        }
        else if (m.kind == Kind::GuildSellQty && d.help && m.cursor == 1)
        {
            uint32_t price = 0;
            for (const auto& it : d.world->inventory)
            {
                if (it.container == 0 && it.slot == m.param)
                {
                    for (const auto& g : d.world->guild.lists[1].items)
                    {
                        price = g.item == it.itemId ? g.gil() : price;
                    }
                }
            }
            drawHelp(p, canvas, fmt::format("{} x{} ({} gil)", d.help->get(466), shopQty_, price * shopQty_));
        }
        else if (m.kind == Kind::ShopSell && d.help && m.cursor == 1)
        {
            // "Sell this merchandise?" with the vendor's quote (S2C 0x03D), once it has arrived.
            const auto& q = d.world->sellQuote;
            drawHelp(p, canvas, q.slot == m.param ? fmt::format("{} ({} gil)", d.help->get(466), q.price) : std::string(d.help->get(466)));
        }
        else if (m.kind == Kind::Revive && d.help)
        {
            // Home point (help 70) and tractor (96) are the game's prompts; the raise wording is this client's (the
            // game's text for it was not found in the d_msg tables).
            drawHelp(p, canvas, m.param == 1 ? std::string("Accept Raise?") : std::string(d.help->get(m.param == 2 ? 96 : 70)));
        }
        else if (m.kind == Kind::ConfigReset && d.help)
        {
            // Font colours / "Reset all log windows...?" / "Reset keyboard assignment to default?" (55650 #143).
            drawHelp(p, canvas,
                     m.param == 2 ? (d.filterText ? std::string(d.filterText->get(143)) : std::string("Reset keyboard assignment to default?"))
                                  : std::string(d.help->get(m.param == 0 ? 238 : 239)));
        }
        else if (m.kind == Kind::AppElements && !appMenus_.at(static_cast<std::size_t>(m.param)).prompt.empty())
        {
            drawHelp(p, canvas, appMenus_.at(static_cast<std::size_t>(m.param)).prompt); // an app-fed yes / no's question
        }
        else if (m.kind == Kind::Config && m.cursor == 15)
        {
            drawHelp(p, canvas, "Enable, disable and configure Ashita add-ons."); // this client's row (goal 47)
        }
        else if (m.kind == Kind::MogHouse && m.cursor == 6 && d.world->mhOpen)
        {
            drawHelp(p, canvas, std::string(d.help->get(667))); // "Close your Mog House to visitors." (CODE 0x10145280)
        }
        else if (m.kind == Kind::LogoutConfirm && d.help)
        {
            drawHelp(p, canvas, std::string(d.help->get(m.param == 1 ? 631 : 72))); // "Shut down?" / "Log out?"
        }
        else if (m.kind == Kind::SortConfirm && d.unityText)
        {
            drawHelp(p, canvas, std::string(d.unityText->get(kSortConfirmQuestion))); // "Sort items?" (file 55653)
        }
        else if (m.kind == Kind::DropConfirm)
        {
            drawHelp(p, canvas,
                     d.unityText ? std::string(d.unityText->get(kDropConfirmQuestion))
                                 : std::string("Dispose of item?"));
        }
        else if (m.kind == Kind::DropAgain)
        {
            std::string what;
            for (const auto& it : d.world->inventory)
            {
                if (it.container == (m.param >> 8) && it.slot == (m.param & 0xFF))
                {
                    const auto info = d.items ? d.items->find(it.itemId) : std::nullopt;
                    what            = info ? info->name : "item";
                }
            }
            // 55649 #48 "Really dispose of the %s?" with the item record's name (CODE 0x101f2d21: [record + 0x1b0]).
            const std::string fmtText = d.dropText ? std::string(d.dropText->get(kDropAgainQuestion))
                                                   : std::string("Really dispose of the %s?");
            const auto        at      = fmtText.find("%s");
            drawHelp(p, canvas,
                     at == std::string::npos ? fmtText : fmtText.substr(0, at) + what + fmtText.substr(at + 2));
        }
        else if (m.kind == Kind::Status && statusSub_ == 0 && m.cursor == 1 && d.charTitles)
        {
            // The Profile window puts "Title: " (55651 #78) and our title (55704) in the help window (CODE 0x10147a1f).
            drawHelp(p, canvas, std::string(d.help->get(78)) + std::string(d.charTitles->get(d.world->stats.title)));
        }
        else if (id >= 0 && static_cast<std::size_t>(id) < d.help->size())
        {
            const auto text = d.help->get(static_cast<std::size_t>(id));
            if (!text.empty())
            {
                drawHelp(p, canvas, std::string(text));
            }
        }
    }
    // Op 0x8B event markers: kept as page 1 slots of the map under the player (CODE 0x101f8900 / 0x101f89e0 with page 1).
    if (!pendingEventMarkers_.empty() && d.maps && d.world)
    {
        for (const auto& pm : pendingEventMarkers_)
        {
            const int             zone  = pm.zone > 0 ? pm.zone : d.world->zone;
            const int             mapId = zone == d.world->zone && d.maps->find(static_cast<uint16_t>(zone), d.mapId) ? d.mapId : 0;
            const dat::MapRecord* rec   = d.maps->find(static_cast<uint16_t>(zone), static_cast<int8_t>(mapId));
            if (!rec)
            {
                continue;
            }
            const auto [u, v] = rec->project(pm.x, pm.z);
            auto& s           = markerSlots(zone, mapId)[static_cast<std::size_t>(10 + pm.slot)];
            s.x               = static_cast<int16_t>(u);
            s.y               = static_cast<int16_t>(v);
            s.name            = pm.label.substr(0, 15);
            s.present         = true;
            saveMarkerSlots(zone, mapId);
        }
        pendingEventMarkers_.clear();
    }
    // Map: the zone's map image, centred, under the menus while the Map menu is open, with our position on it.
    if (std::any_of(stack_.begin(), stack_.end(), [](const Menu& m) { return m.kind == Kind::Map; }) && d.maps)
    {
        // On opening, the map under us (the collision map id, as the client picks it); Left/Right browse the others.
        if (mapZone_ != d.world->zone || !mapOpen_)
        {
            mapZone_    = d.world->zone;
            mapId_      = d.maps->find(mapZone_, d.mapId) ? d.mapId : 0;
            mapShownId_ = -1;
        }
        mapOpen_ = true;
        const dat::MapRecord* rec = d.maps->find(mapZone_, mapId_);
        if (mapShownId_ != mapId_)
        {
            mapTexture_ = rec ? d.maps->load(*rec) : std::nullopt;
            mapShownId_ = mapId_;
        }
        // Without the area's map key item (S2C 0x055) the map is not shown, unless an addon opened every map (allmaps
        // patches both of the game's key item tests; this window serves both the menu and the marker view).
        const auto need    = rec ? rec->requiredKeyItem() : 0;
        const bool haveMap = need == 0 || allMaps.map || allMaps.view ||
                             std::binary_search(d.world->keyItems.begin(), d.world->keyItems.end(), need);
        if (mapTexture_ && rec && haveMap)
        {
            const float  side = std::min(canvas.width, canvas.height) - 32;
            const ImVec2 at((canvas.width - side) * 0.5f * canvas.scale, (canvas.height - side) * 0.5f * canvas.scale);
            // Map texels use alpha 0x80 = opaque; under MODULATE2X a vertex alpha of 0xFF makes them so.
            p.icon(fmt::format("map {} {}", mapZone_, mapId_), *mapTexture_, at, { 0x80, 0x80, 0x80, 0xFF }, side);
            // Player marker: "menu keytops3" sprite 159 (dot and direction wedge), rotated to our heading.
            const auto [u, v] = rec->project(d.playerX, d.playerZ);
            const float px = side / 512.0f * canvas.scale;
            // Party / alliance members (ui-format-notes §6.5): the Markers list page last shown fades its markers in
            // (item flag 1, CODE 0x101fa8b0 / 0x101eeb5a); a member is drawn only on the sub-map it stands on
            // (0x101f7adf). Party slot 5 -> shape 154, alliance slot 6 -> 155; the confirmed row's name shows beside
            // its marker (flag 8 cleared, 0x101f8c70) in label colour 2, 0x606070 (table 0x10382084).
            // A marker shape centred on its point: without item flag 0x10 the position is moved back by half the shape's
            // box (CODE 0x101ee40c).
            auto drawShape = [&](int shapeIndex, ImVec2 pt) {
                const auto* shape = r_.resources().sprite("menu keytops3", static_cast<std::size_t>(shapeIndex));
                if (!shape || shape->empty())
                {
                    return;
                }
                int x0 = 0x7FFF, x1 = -0x8000, y0 = 0x7FFF, y1 = -0x8000;
                for (const auto& q : *shape)
                {
                    for (std::size_t k = 0; k < 8; k += 2)
                    {
                        x0 = std::min<int>(x0, q.xy[k]);
                        x1 = std::max<int>(x1, q.xy[k]);
                        y0 = std::min<int>(y0, q.xy[k + 1]);
                        y1 = std::max<int>(y1, q.xy[k + 1]);
                    }
                }
                p.sprite(*shape, ImVec2(pt.x - static_cast<float>(x1 - x0) / 2 * canvas.scale, pt.y - static_cast<float>(y1 - y0) / 2 * canvas.scale));
            };
            // The item's +0xa / +0xc (16, 1): the label's offset from the marker (INFERRED); label colour 2, 0x606070.
            auto drawLabel = [&](const std::string& name, ImVec2 pt) { p.text(name, ImVec2(pt.x + 8 * canvas.scale, pt.y - 7 * canvas.scale), { 0x60, 0x60, 0x70, 0x80 }); };
            if (markerPage_ == 0 || markerPage_ == 1)
            {
                // Own markers (shape slot 3 -> 150) or event markers (slot 4 -> 152), each with its name (the items keep
                // flag 0x08 clear: CODE 0x101f7ddf).
                const auto& slots = markerSlots(mapZone_, mapId_);
                for (int i = 0; i < 10; ++i)
                {
                    const auto& s = slots[static_cast<std::size_t>(markerPage_ * 10 + i)];
                    if (!s.present)
                    {
                        continue;
                    }
                    const ImVec2 pt(at.x + s.x * px, at.y + s.y * px);
                    drawShape(markerPage_ == 0 ? 150 : 152, pt);
                    drawLabel(s.name, pt);
                }
            }
            else if (markerPage_ >= 2)
            {
                const auto members = mapMarkerMembers(*d.world, d.nearby);
                for (std::size_t i = static_cast<std::size_t>(markerPage_ - 2) * 10; i < members.size() && i < static_cast<std::size_t>(markerPage_ - 1) * 10; ++i)
                {
                    const auto& mm = members[i];
                    if (d.mapIdAt && d.mapIdAt(mm.x, mm.y, mm.z) != mapId_)
                    {
                        continue;
                    }
                    const auto [mu, mv] = rec->project(mm.x, mm.z);
                    if (mu < 0 || mv < 0 || mu >= 512 || mv >= 512)
                    {
                        continue;
                    }
                    const ImVec2 pt(at.x + mu * px, at.y + mv * px);
                    drawShape(mapMarkerShape(mm.sameParty), pt);
                    if (mm.id == markerSel_)
                    {
                        drawLabel(mm.name, pt);
                    }
                }
            }
            if (markerPlacing_ >= 0 && (tick_ / 15) % 2 == 0)
            {
                // The placing cursor: the marker's own shape, blinking (INFERRED: the cursor's animated icon is not traced).
                drawShape(150, ImVec2(at.x + markerCursorU_ * px, at.y + markerCursorV_ * px));
            }
            if (const auto* marker = r_.resources().sprite("menu keytops3", 159); marker && mapId_ == d.mapId && u >= 0 && v >= 0 && u < 512 && v < 512)
            {
                p.spriteRotated(*marker, ImVec2(at.x + u * px, at.y + v * px), static_cast<float>(d.heading) * 3.14159265f / 128.0f);
            }
        }
        else
        {
            drawHelp(p, canvas, "There is no map of this area.");
        }
    }
    if (std::none_of(stack_.begin(), stack_.end(), [](const Menu& m) { return m.kind == Kind::Map; }))
    {
        // Opening the map again starts with no member page shown and asks the server again (CODE 0x101f9600).
        mapOpen_       = false;
        markerPage_    = -1;
        markerSel_     = 0;
        markerAsked_   = false;
        markerPlacing_ = -1;
    }
    // The event map window (cutscene ops 0xC8 / 0x8B, "menu mapframe" 0x10630d68): the map image centred with the
    // event's named markers (the player marker sprite's dot; INFERRED: the frame and the marker sprite are not traced).
    if (eventMapUnderPlayer_ && d.world)
    {
        eventMapId_          = eventMapZone_ == d.world->zone && d.maps && d.maps->find(static_cast<uint16_t>(eventMapZone_), d.mapId) ? d.mapId : 0;
        eventMapUnderPlayer_ = false;
    }
    if (eventMapId_ >= 0 && d.maps)
    {
        const dat::MapRecord* rec = d.maps->find(static_cast<uint16_t>(eventMapZone_), static_cast<int8_t>(eventMapId_));
        if (rec && !eventMapTexture_)
        {
            eventMapTexture_ = d.maps->load(*rec);
        }
        if (rec && eventMapTexture_)
        {
            if (stack_.empty() && !mapGridRef_.empty())
            {
                drawMapGridRef(p, canvas); // no menu's help line drew the title window
            }
            // Op 0xB4 14's view (eventMapView): zoomed about the window's centre and shifted by (x, y) map pixels
            // (INFERRED: the view's own anchor in 0x101ee780 was not traced; no frame: the map draws no border,
            // "menu mapframe" only carries the map cursor's animated icon, which events never set: ui-format-notes §10.9).
            const auto   view  = eventMapViewNow();
            const float  frame = std::min(canvas.width, canvas.height) - 32;
            const float  side  = frame * view.zoom;
            const ImVec2 at((canvas.width - side) * 0.5f * canvas.scale - view.x * side / 512.0f * canvas.scale,
                            (canvas.height - side) * 0.5f * canvas.scale - view.y * side / 512.0f * canvas.scale);
            const ImVec2 clip0((canvas.width - frame) * 0.5f * canvas.scale, (canvas.height - frame) * 0.5f * canvas.scale);
            p.pushClip(clip0, ImVec2(clip0.x + frame * canvas.scale, clip0.y + frame * canvas.scale));
            p.icon(fmt::format("evmap {} {}", eventMapZone_, eventMapId_), *eventMapTexture_, at, { 0x80, 0x80, 0x80, 0xFF }, side);
            const float px     = side / 512.0f * canvas.scale;
            const auto* marker = r_.resources().sprite("menu keytops3", 159);
            for (const auto& m : eventMapMarkers_)
            {
                const auto [u, v] = rec->project(m.x, m.z);
                if (u < 0 || v < 0 || u >= 512 || v >= 512)
                {
                    continue;
                }
                const ImVec2 pt(at.x + u * px, at.y + v * px);
                if (marker && marker->size() > 1)
                {
                    p.sprite(dat::UiSprite{ (*marker)[1] }, pt, { 0x80, 0x80, 0x80, 0xFF });
                }
                p.text(m.label, { pt.x + 8 * canvas.scale, pt.y - 6 * canvas.scale }, kWhite);
            }
            p.popClip();
        }
    }
    // Menus: each drawn unless a later one occupies the same place (Abilities replaces the main menu).
    std::string stackTrace; // XI_UI_STACK_TRACE=1: the stack's layouts and which are hidden, logged on change
    for (std::size_t i = 0; i < stack_.size(); ++i)
    {
        const auto* li = layout(stack_[i]);
        bool        covered = false;
        for (std::size_t j = i + 1; j < stack_.size() && li; ++j)
        {
            const auto* lj = layout(stack_[j]);
            // Same place, or wholly inside the later one (the battle menu's "menu abiselec" under "menu ability").
            covered |= lj && lj->window.x == li->window.x && lj->window.y == li->window.y && lj->window.anchor == li->window.anchor;
            covered |= lj && lj->window.anchor == li->window.anchor && lj->window.x <= li->window.x && lj->window.y <= li->window.y &&
                       lj->window.x + lj->window.w >= li->window.x + li->window.w && lj->window.y + lj->window.h >= li->window.y + li->window.h;
        }
        // The main menu pages hide while one of their windows is open, and come back with it closed (retail captures:
        // Equipment, Status; the menu manager's registry flags do not do it, so the main menu's own code does, INFERRED).
        covered |= (stack_[i].kind == Kind::Main || stack_[i].kind == Kind::Social) && i + 1 < stack_.size();
        // The item list gives way to Switch's container list ("menu storage" at 16,80; INFERRED, as the game swaps the
        // list's contents for the container it shows).
        covered |= stack_[i].kind == Kind::Items && i + 1 < stack_.size() && stack_.back().kind == Kind::ItemBags;
        covered |= stack_[i].kind == Kind::ItemBags && i + 1 < stack_.size(); // and itself to the container opened
        // The Macros entry and book list hide while a book's palettes are being edited (INFERRED, as the main menu).
        covered |= (stack_[i].kind == Kind::MacroMenu || stack_[i].kind == Kind::MacroBooks) &&
                   std::any_of(stack_.begin() + static_cast<std::ptrdiff_t>(i), stack_.end(), [](const Menu& m) { return m.kind == Kind::MacroEdit; });
        if (!covered)
        {
            drawMenu(p, canvas, stack_[i], i + 1 == stack_.size(), d, act);
        }
        if (static const bool trace = std::getenv("XI_UI_STACK_TRACE") != nullptr; trace && li)
        {
            stackTrace += fmt::format(" [{} {},{} {}x{} a{}{}]", li->name, li->window.x, li->window.y, li->window.w, li->window.h, li->window.anchor, covered ? " hidden" : "");
        }
    }
    if (static std::string lastTrace; stackTrace != lastTrace)
    {
        lastTrace = stackTrace;
        if (!stackTrace.empty())
        {
            spdlog::info("ui stack:{}", stackTrace);
        }
    }
    drawNetMeter(p, canvas, false); // without a menu: the meter alone (with one, drawHelp drew it in its window)
}

// --- Event windows (op 0xB4) ---

void GameUi::eventTimer(int which, int ticks)
{
    // Open (CODE 0x1012c100 / 0x1012c430): total +0x14 = left +0x18 = the ticks (< 0 -> 0); -1 here closes (08 / 0B).
    auto& t = eventTimers_[which ? 1 : 0];
    t.open  = ticks >= 0;
    t.total = t.left = static_cast<float>(std::max(0, ticks));
}

int GameUi::eventTimerLeft(int which) const
{
    const auto& t = eventTimers_[which ? 1 : 0];
    return t.open ? static_cast<int>(t.left) : 0;
}

void GameUi::stepEventTimers()
{
    // Each frame's update (casttime 0x1012c020, eventtim 0x1012c340): a window whose time is already 0 closes;
    // else the time drops by the frame's ticks (0x10014cb0: the frame time [0x104568fc]+0x28, at least 1 tick)
    // and stops at 0, so 0:00 shows for one frame. We take the clock's time without the 1-tick floor: at the
    // game's 30 / 60 frames a second it is the same, and a faster frame rate does not speed the clock up.
    const auto  now = std::chrono::steady_clock::now();
    const float dt  = eventTimersStep_ == std::chrono::steady_clock::time_point{}
                          ? 0.0f
                          : std::chrono::duration<float>(now - eventTimersStep_).count() * 60.0f;
    eventTimersStep_ = now;
    for (auto& t : eventTimers_)
    {
        if (!t.open)
        {
            continue;
        }
        if (t.left <= 0.0f)
        {
            t.open = false;
            continue;
        }
        t.left = std::max(0.0f, t.left - dt);
    }
}

void GameUi::eventMapView(float x, float y, float zoom, float seconds, int /*flags*/)
{
    // Map object 0x101fac20: the target view (+0xC8..+0xD0) and its time (+0xD4); 0 sets the view at once.
    eventViewFrom_    = eventMapViewNow();
    eventViewTo_      = { x, y, zoom > 0.0f ? zoom : 1.0f };
    eventViewSeconds_ = std::max(0.0f, seconds);
    eventViewStart_   = std::chrono::steady_clock::now();
    if (eventViewSeconds_ == 0.0f)
    {
        eventViewFrom_ = eventViewTo_;
    }
}

GameUi::MapView GameUi::eventMapViewNow() const
{
    if (eventViewSeconds_ <= 0.0f)
    {
        return eventViewTo_;
    }
    const float k = std::min(1.0f, std::chrono::duration<float>(std::chrono::steady_clock::now() - eventViewStart_).count() / eventViewSeconds_);
    return { eventViewFrom_.x + (eventViewTo_.x - eventViewFrom_.x) * k, eventViewFrom_.y + (eventViewTo_.y - eventViewFrom_.y) * k,
             eventViewFrom_.zoom + (eventViewTo_.zoom - eventViewFrom_.zoom) * k };
}

void GameUi::drawEventWindows(UiPainter& p, const UiCanvas& c, const GameUiData& d)
{
    const auto& ls = r_.resources().layouts;
    // Op 0xB4 02: "menu targetwi" in event mode (target object +0x65 = 1): the script's name in white (+0x68 =
    // 0x80808080) and a gauge of +0x66 (0..100), placed as the normal target window.
    if (const auto it = ls.find("menu targetwi"); eventTargetGauge_ >= 0 && it != ls.end())
    {
        const auto   pl = partyStackBase(d);
        const auto   l  = pl ? stackedAbove(c, it->second, *pl) : it->second;
        const ImVec2 o  = drawLayout(p, c, l, {});
        p.textShp(eventTargetName_, ImVec2(o.x + 5 * c.scale, o.y + 12 * c.scale), kWhite);
        int shown = eventTargetGauge_;
        drawGaugeSprite(p, ImVec2(o.x + 28 * c.scale, o.y + 29 * c.scale), 97, 38, eventTargetGauge_, shown);
    }
    // Ops 0xB4 07 / 0A: the time left (+0x18 x 1/60, truncated), "%d:%02d" (from an hour "%d:%02d:%02d"; CODE
    // 0x101c8f70), at y + 9 in fontshp white: "menu casttime" (mode 4, CODE 0x1012bf10) right-aligned at x + 0x69,
    // "menu eventtim" (CODE 0x1012c200) centred. Both moved to x 16, y 94 while the gauge window is up (0x1011b970 on
    // 0x105781ec), else y 47 (casttime 0x1012c020, eventtim 0x1012c340).
    stepEventTimers();
    for (int which = 0; which < 2; ++which)
    {
        if (!eventTimers_[static_cast<std::size_t>(which)].open)
        {
            continue;
        }
        const int  left = eventTimerLeft(which);
        const auto it = ls.find(which ? "menu eventtim" : "menu casttime");
        if (it == ls.end())
        {
            continue;
        }
        dat::UiLayout l = it->second;
        l.window.x      = 16;
        l.window.y      = static_cast<int16_t>(gaugeWindowShown() ? 94 : 47);
        l.window.anchor = 0;
        const ImVec2 o  = drawLayout(p, c, l, {});
        const int    s  = left / 60;
        const auto   t  = s >= 3600 ? fmt::format("{}:{:02d}:{:02d}", s / 3600, s % 3600 / 60, s % 60) : fmt::format("{}:{:02d}", s / 60, s % 60);
        const float  y  = o.y + 9 * c.scale;
        if (which)
        {
            p.textShp(t, ImVec2(o.x + (static_cast<float>(l.window.w) - p.textShpWidth(t)) * 0.5f * c.scale, y), kWhite);
        }
        else
        {
            p.textShp(t, ImVec2(o.x + 0x69 * c.scale, y), kWhite, true);
        }
    }
}

} // namespace xi::ui
