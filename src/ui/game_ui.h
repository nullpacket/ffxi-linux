// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <unordered_set>

#include "client/search.h"
#include "client/session.h"
#include "dat/emote_list.h"
#include "dat/items.h"
#include "dat/macro_file.h"
#include "dat/messages.h"
#include "dat/spells.h"
#include "dat/zone_map.h"
#include "ui/item_sort_keys.h"
#include "ui/yes_no.h"
#include "ui/keymap.h"
#include "ui/layout_view.h"
#include "ui/log_object.h"
#include "ui/map_markers.h"
#include "ui/status_select.h"
#include "ui/ui_draw.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

#include <chrono>
#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <string>
#include <vector>

// The in-game interface built from the game's own window layouts: chat log, party and target windows, the main menu
// and its submenus. Keyboard driven like the original (arrows, Enter, Escape), with mouse hover/click as well.
namespace xi::ui
{

struct GameUiData
{
    const client::WorldSnapshot* world     = nullptr;
    const std::unordered_set<uint64_t>* shownResults = nullptr; // ChatLine::hold values whose effect has landed
    // A log line an Ashita addon changed (text_in), by ChatLine::serial: {hidden, the whole line as shown}.
    std::function<std::optional<std::pair<bool, std::string>>(uint64_t serial)> lineEdit;
    const client::Entity*        target    = nullptr;
    // The monster we are engaged with (0 = none), and whether the target is locked on it: while engaged, Tab, F1..F6
    // and Esc leave the target alone (ui-format-notes §9.2); Switch Target and typed commands still change it.
    uint32_t                     battleTarget = 0;
    bool                         targetLocked = false;
    const dat::ItemTable*        items     = nullptr;
    const dat::BattleText*       text      = nullptr;
    const dat::SpellTable*       spells    = nullptr;
    const dat::AbilityTable*     abilities = nullptr;
    const dat::StringTable*      help      = nullptr; // layout help lines (file 55651, the elements' first string id)
    const dat::KeyItemTable*     keyItems  = nullptr; // key item names (file 55695)
    const dat::StringTable*      titles    = nullptr; // window titles (file 55652)
    const dat::StringTable*      clientText = nullptr; // client texts (file 55648: 230 "MP Cost", 231 "Next")
    const dat::XiStringTable*    formats    = nullptr; // string table 10 (file 50): 39 "Macro Set %s (#%d)", 109 "Macro Book (#%d)"
    const dat::StringTable*      filterText = nullptr; // Config chat / effect filter row labels (file 55650)
    const dat::StringTable*      regions   = nullptr; // nation and region names (file 55654)
    const dat::XiStringTable*    dropText   = nullptr; // XISTRING file 55649: #48 "Really dispose of the %s?"
    const dat::StringTable*      unityText = nullptr; // Unity texts (file 55653): leader i's name is #402 + i, #442 "Unity"
    const dat::StringTable*      charTitles = nullptr; // character titles by title id (file 55704; the Profile's help line)
    const dat::EmoteList*        emotes    = nullptr; // the Emote List's entries (file 96)
    const dat::StringTable*      emoteText = nullptr; // their labels / help (file 55675)
    const std::array<dat::LogNameTable, 8>* questNames   = nullptr; // per quest log (dat::LogNameTable::kQuestFiles)
    const std::array<dat::LogNameTable, 4>* missionNames = nullptr; // nation logs 0..2, Zilart
    const dat::DatIndex*         index     = nullptr; // for status icons (file 87)
    const dat::StringTable*      areaShort = nullptr; // short area names by zone id (file 55661, "S.San d'Oria")
    std::vector<std::pair<float, const client::Entity*>> nearby; // entities by distance (for /target)
    const dat::ZoneMaps*         maps      = nullptr; // zone map images
    // Auction house lists from the search server (GameUiActions::auctionList / auctionHistory): the category the
    // items are for (-1 = none yet), and the latest price history.
    const std::vector<client::AuctionItem>* auctionItems    = nullptr;
    // The Search window's last result (GameUiActions::search); nullptr while one runs.
    const client::SearchResult*             searchResult    = nullptr;
    int                                     auctionCategory = -1;
    const std::vector<client::AuctionSale>* auctionSales    = nullptr;
    bool                                    auctionSalesReady = false;
    float                        playerX = 0, playerZ = 0; // our position (game coordinates) and heading byte
    float                        playerY = 0; // ... and height (game coordinates, as Entity::pos.y)
    uint8_t                      heading = 0;
    // The camera's view direction (game x / z), for the compass.
    float                        camForwardX = 0, camForwardZ = 1;
    int8_t                       mapId   = 0; // collision map id under us: which zone map applies
    // The collision map id at a game position (party markers show only on the map they are on; CODE 0x101f7adf).
    std::function<int(float x, float y, float z)> mapIdAt;
    std::chrono::steady_clock::time_point now{};
    bool                         dead      = false;
    int                          windowSkin = 1; // Config: current Window Type (1..8)
    int                          keyboard   = 0; // Config: current Keyboard Size (0 Full, 1 Compact 1, 2 Compact 2)
    // The game's other Config values by config id (config_re.md); ids missing take the game's default.
    const std::map<int, int>*    gameConfig = nullptr;
    bool                         chatFilterLists = false; // Config Misc. 4: prohibited.txt has entries (cfg 206 1 / 2)
    // Whether a scheduler of our own race's fishing file still runs on us (the result motions fsh2..fsh6).
    std::function<bool(uint32_t name)> fishingRunning;
};

// A fishing effect for the app to run on our own character (GameUi::takeFishingFx).
struct FishingFx
{
    uint32_t file = 0;    // 0 = the race's fishing file (Characters::fishingFiles), else an effect DAT
    uint32_t name = 0;    // scheduler fourcc
    bool     stop = false;
};

struct GameUiActions
{
    std::function<void(uint32_t id, uint16_t index, uint16_t actionId, uint32_t param)>             action; // C2S 0x01A
    std::function<void(uint8_t container, uint8_t slot, uint8_t equipSlot)>                         equip;
    std::function<void(uint8_t container, uint8_t slot, uint32_t targetId, uint16_t targetIndex)>   useItem;
    std::function<void(const std::string& text, uint8_t kind)>                                      say;
    // Every chat line before it runs (Ashita's command / text_out events; mode 1 typed, 2 macro): true when taken.
    // A plugin may rewrite the line in place (it then runs as rewritten).
    std::function<bool(std::string& line, int mode)> intercept;
    std::function<void(uint16_t id, std::vector<std::byte> payload)>                                send; // any C2S packet
    std::function<void(bool accept)>                                                                answerInvite;
    std::function<void(const std::string& text)>                                                    note;   // client line in the log
    std::function<void(uint16_t message, const std::string& string0)>                               stdNote; // standard message (file 7031)
    std::function<void(bool shutdown)>                                                              logout; // log out (or shut down)
    std::function<void(std::size_t slot)>                                                           chooseCharacter; // lobby
    std::function<void(uint32_t id)>                                                                setTarget; // /target
    std::function<void(int type)>                                                                   setWindowSkin; // Config: Window Type 1..8
    std::function<void(int type)>                                                                   setKeyboard;   // Config: Keyboard Size 0/1/2
    std::function<void(int id, int value)>                                                          setConfig;     // Config: a game config id
    // Config Chat Filters: the server's filter words (C2S 0x0DB), the system message level 0..3 and the log-only bits (cfg 52).
    std::function<void(uint32_t filter1, uint32_t filter2, int level, uint32_t local)>              setChatFilters;
    std::function<void(uint8_t category, std::vector<client::AuctionTerm> terms)>                    auctionList;    // ask the search server
    std::function<void(uint16_t itemId, bool stack)>                                                auctionHistory; // ask the search server
    std::function<void(const client::SearchQuery& q)>                                               search;         // the Search window's query
};

// Name colour as the game shows it: other PCs white, party members light blue, NPCs green; monsters yellow when
// unclaimed, red when claimed by us or our party, purple when claimed by someone else.
UiColor nameColour(const client::Entity& e, const client::WorldSnapshot& w);
// The game's name colour table index nameColour stands for (INFERRED from the table's default colours, which the
// namecolors addon lists: 0 players, 4 NPCs, 5 unclaimed monsters, 6 claimed by us / our party, 7 claimed by others,
// 9 dead monsters), and overrides of the table's 23 entries (D3DCOLOR) an addon set; unset entries change nothing.
int                                      nameColourIndex(const client::Entity& e, const client::WorldSnapshot& w);
std::array<std::optional<uint32_t>, 23>& nameColourOverrides();

// The login form (server, account, password, character): drawn with the game's frame and font. The original client
// had PlayOnline here, so this one is styled after the game's windows rather than copied from a layout.
struct LoginFields
{
    std::string host, user, pass, character;
    int         field = 1; // 0 host, 1 account, 2 password, 3 character
    std::string notice;    // why the last session ended ("Lost connection to the server."), shown under the fields
};

class GameUi
{
public:
    // Test scripts: the 0-based row of the open list (as drawn last frame) that shows item `itemId` and can be chosen,
    // or -1 (inventory, trade and other item lists).
    [[nodiscard]] int itemRow(uint16_t itemId) const
    {
        for (std::size_t i = 0; i < topRowItems_.size(); ++i)
        {
            if (topRowItems_[i] == itemId)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Interface sounds (se000/se0000NN.spw, CODE): cursor 1, confirm 2, cancel 3, a menu opened from nothing 9, another
    // window 10, the main menu 11.
    void setSoundHook(std::function<void(uint32_t id)> f)
    {
        sound_ = std::move(f);
    }
    explicit GameUi(UiRenderer& r);

    // Input. Each returns true when the interface took the event (the game should not act on it).
    bool keyDown(SDL_Keycode key, bool repeat);
    bool textInput(std::string_view utf8);
    bool mouseMove(ImVec2 screen);
    bool mouseDown(int button, ImVec2 screen);
    bool wheel(float dy);
    // While a key-assignment list (Config -> Misc. 2) is open, a key press assigns that key to the row under the cursor;
    // false for the keys the list leaves to its navigation (arrows, Enter, Esc, ...). Physical keys, as the game's DIK.
    bool assignKey(SDL_Scancode key, bool repeat = false);

    // The event map window (cutscene ops 0xC8 / 0x8B / 0x8A): map `map` of `zone`, named markers; map < 0 closes it.
    // Op 0x8B's marker is also kept as event marker `slot` (the Markers list's page 1) of the map under the player.
    void eventMap(int zone, int map);
    void eventMapMarker(int slot, float x, float z, std::string label);
    // Opens a menu by name (test hooks): main, status, equip, items, magic, abilities, weaponskills, map.
    void openByName(std::string_view name);
    // A Ctrl / Alt shortcut key's window, opened alone (no main menu under it; handlers registered by CODE 0x101adcf0):
    // items, equip, magic (every spell list but trust), abilities, weaponskills, petcommands.
    void openShortcut(std::string_view name);
    // Enter with a monster targeted: the battle menu (Attack / Abilities / Magic / Items / Treasure / Check).
    // The Mog House menu ("menu myroom"): S2C 0x02E after talking to the Moogle, or the main menu's Mog House slot.
    void openMogHouse()
    {
        stack_.clear();
        open(Kind::MogHouse);
    }
    // The battle / player command menu (object "menu playermo", CODE 0x1012b3e0): mode 2 a player targeted, 4 a
    // monster targeted ("Ready Mode"), 6 while engaged ("Battle Mode").
    void openBattleMenu(int mode)
    {
        stack_.clear();
        open(Kind::BattleMenu, mode);
    }

    // The game's action commands, by name: /ma "Cure" <me>, /ja "Berserk" <me>, /ws "Fast Blade" <t>,
    // /item "Potion" <me>, /equip main "Onion Staff", /attack. Returns false for anything else (chat, other commands).
    bool command(const std::string& text, const GameUiData& d, const GameUiActions& act);

    // Macros (the Ctrl / Alt + 1..0 palette; ui-format-notes §7.9). loadMacroDir reads the game's macro files from
    // `dir` (its USER/<id> folder or our copy of it). loadMacros reads our older text format into the books and writes
    // them as the game's files (one block per macro: "[book B set S ctrl|alt K] Name" then up to six lines).
    void loadMacroDir(const std::filesystem::path& dir);
    void loadMacros(const std::filesystem::path& file);
    // Equipment sets (/equipset N, /lockstyleset N), one block per set:
    //   [3] Fishing            <- set number 1..100, name
    //   range Willow Fishing Rod
    //   ammo Little Worm       <- slot (main sub range ammo head neck ear1 ear2 body hands ring1 ring2 back waist legs feet), item
    // "/equipset save N [name]" writes today's gear as set N.
    void loadEquipSets(const std::filesystem::path& file);
    // Runs macro `key` (1..10) of set 0 (Ctrl) or 1 (Alt).
    void runMacro(int set, int key);
    // The macro palette as addons see it (Ashita's FsMacroContainer stand-ins): row 0 Ctrl / 1 Alt, key 1..10, on the
    // current book and set, as shown (auto-translate as "{...}"). setMacro changes it and writes its set file (empty
    // name and lines: clear).
    [[nodiscard]] std::string macroName(int row, int key) const;
    [[nodiscard]] std::string macroLine(int row, int key, int line) const;
    void                      setMacro(int row, int key, std::string name, std::vector<std::string> lines);
    [[nodiscard]] int         macroBook() const { return macroBook_; } // 1..40
    [[nodiscard]] int         macroSet() const { return macroSet_; }   // 1..10
    bool                      setMacroBook(int book);                  // 1..40
    bool                      setMacroSet(int set);                    // 1..10
    [[nodiscard]] bool        macroRunning() const { return macroRun_.next >= 0; }
    void                      stopMacros() { macroRun_.next = -1; }
    // Windows an addon hid (Ashita's hideparty, XIUI's own macro bar): all shown unless one says otherwise.
    struct WindowsShown
    {
        bool party = true, target = true, alliance1 = true, alliance2 = true, macroPalette = true;
    };
    WindowsShown windowsShown;
    // Every map shows without its key item (Ashita's allmaps): `map` for the Map menu, `view` for markers / waypoints.
    struct AllMaps
    {
        bool map = false, view = false;
    };
    AllMaps allMaps;
    // The log windows as addons see the game's log objects (ui/log_object.h, issue #91): `win` 0 or 1. `height` is the
    // client's own shown height (+0x34, lines x 16), `lines` what is drawn (an addon's hold included), `textTop` how far
    // the text rect's top (+0x42) is above the screen's bottom. An addon's write of +0x34 goes to addonLogHeight;
    // nullopt releases it (the addon unloaded).
    struct LogObjectView
    {
        bool shown    = false; // the window exists (window 2: Config "multiple log windows")
        int  height   = 0;
        int  lines    = 0;
        int  maxLines = 8, minLines = 0;
        int  textTop = 0;
    };
    [[nodiscard]] const LogObjectView& logObject(int win) const
    {
        return logObjects_[static_cast<std::size_t>(win & 1)];
    }
    void addonLogHeight(int win, std::optional<int> height);
    // Runs a line as if typed (queued like a macro line). `mode` is the one the interceptors see (Ashita's command
    // modes: 1 typed, 2 macro, 0 a game menu; an addon or plugin's QueueCommand passes its own).
    void runLine(std::string line, int mode = 2)
    {
        macroQueue_.push_back(QueuedLine{ std::move(line), mode });
    }
    // Opens the chat line with `prefix` already typed (the /say, /party ... shortcut keys). `keepMenus`: the open menus
    // stay up underneath (a line opened from the keyboard); Esc or sending the line hands the keys back to them.
    void startTyping(std::string prefix, bool keepMenus = false)
    {
        if (!keepMenus)
        {
            stack_.clear();
        }
        typing_ = true;
        input_  = std::move(prefix);
        cursor_ = input_.size(); // the line (and its auto-translate) starts after the prefix
        at_     = {};
    }
    // Event scripts: a choice menu in "menu query" (the prompt is its first line; options keep their index), the
    // cursor on visible option `cursor`, and the choice once made (0-based, -1 = cancelled), taken once.
    void openEventMenu(std::string prompt, std::vector<std::pair<int, std::string>> options, int cursor = 0)
    {
        stack_.clear();
        eventPrompt_  = std::move(prompt);
        eventOptions_ = std::move(options);
        eventChoice_.reset();
        open(Kind::EventMenu);
        if (!stack_.empty() && cursor > 0 && cursor < static_cast<int>(eventOptions_.size()))
        {
            // Three rows show; the cursor's row is the last one when it starts below them (CODE 0x1014e020, flag 0).
            auto& m  = stack_.back();
            m.scroll = std::max(0, cursor - 2);
            m.cursor = cursor - m.scroll + 1;
        }
    }
    // Test scripts: the open event menu's options (value, text), or none.
    [[nodiscard]] const std::vector<std::pair<int, std::string>>* eventMenu() const
    {
        return !stack_.empty() && stack_.back().kind == Kind::EventMenu ? &eventOptions_ : nullptr;
    }
    // Test scripts: the open event menu's cursor row (0-based over its visible options).
    [[nodiscard]] int eventMenuRow() const
    {
        return !stack_.empty() && stack_.back().kind == Kind::EventMenu ? stack_.back().scroll + stack_.back().cursor - 1 : 0;
    }
    std::optional<int> takeEventChoice()
    {
        auto c = eventChoice_;
        eventChoice_.reset();
        return c;
    }
    // Menus the application fills (the Mog House's exit door, layout and mannequin menus): `layout` names the game
    // window. A list window shows `rows` (`prompt` in the help bar); an element window (no rows) offers its elements,
    // the `dimmed` ids greyed. The confirmed row's value (an element's id) comes back through takeAppChoice() with
    // `tag`, and the menus close; Escape gives -1. The Mog House menu's Gardening, Layout, Open Mog House and Remodel
    // elements come back as (kMyRoomTag, 4..7).
    struct AppRow
    {
        std::string label;
        std::string right;
        bool        enabled = true;
        int         value   = 0;
        std::string help;               // the help line for this row (else the menu's prompt)
        bool        adjustable = false; // pages: Left / Right come back through takeAppAdjust()
    };
    static constexpr int kMyRoomTag = 0x4D59;
    // Config's "Add-ons" entry (an extra row under Gamepad in "menu configwi"): comes back as (kAddonsTag, 0) with the
    // Config menu left open, and the application opens its pages on top (openAppPage).
    static constexpr int kAddonsTag = 0x4144;
    // `title`: a line drawn inside the window above the rows (windows whose rows start below a title, e.g. "menu mogdoor").
    void openAppMenu(int tag, std::string layout, std::string prompt, std::vector<AppRow> rows, std::vector<int> dimmed = {},
                     std::string title = {})
    {
        if (stack_.empty())
        {
            appMenus_.clear();
        }
        appMenus_.push_back({ tag, std::move(layout), std::move(prompt), std::move(rows), std::move(dimmed), std::move(title) });
        open(appMenus_.back().rows.empty() ? Kind::AppElements : Kind::AppList, static_cast<int>(appMenus_.size() - 1));
    }
    // An application page: a list window over the menus already open (e.g. Config) that stays open when a row is
    // chosen (the choice comes back through takeAppChoice(); the application refreshes it with updateAppPage) and goes
    // back one window on cancel (choice -1). `windowTitle` names it in the title window.
    void openAppPage(int tag, std::string layout, std::string windowTitle, std::vector<AppRow> rows, std::string prompt = {})
    {
        if (stack_.empty())
        {
            appMenus_.clear();
        }
        AppMenu am{ tag, std::move(layout), std::move(prompt), std::move(rows), {}, {} };
        am.keepStack   = true;
        am.windowTitle = std::move(windowTitle);
        appMenus_.push_back(std::move(am));
        open(Kind::AppList, static_cast<int>(appMenus_.size() - 1));
    }
    // New rows for the open page with `tag` (the cursor stays, clamped to the rows).
    void updateAppPage(int tag, std::vector<AppRow> rows, std::optional<std::string> windowTitle = std::nullopt);
    [[nodiscard]] bool appPageOpen(int tag) const;
    std::optional<std::pair<int, int>> takeAppChoice()
    {
        auto c = appChoice_;
        appChoice_.reset();
        return c;
    }
    // Left / Right on an adjustable page row: (tag, row value, -1 / +1).
    std::optional<std::tuple<int, int, int>> takeAppAdjust()
    {
        auto a = appAdjust_;
        appAdjust_.reset();
        return a;
    }
    // While an event waits for the player to close its message: the log's page marker shows.
    void setEventWaiting(bool on)
    {
        eventWaiting_ = on;
    }
    // Hide/Show Window: hides every window (input still works).
    void toggleHidden()
    {
        hidden_ = !hidden_;
    }
    // Main menu pages: swaps the top menu between page 1 and 2, keeping the row (table at 0x1036ffa8).
    void flipMainPage();

    // A menu, or the selected log window, has the cursor keys.
    // Fishing effects started or stopped on our own character since the last call (scheduler names of the race's
    // fishing file, file 0, or of effect file 3727's arrows / 3705's keen-sense flash).
    [[nodiscard]] std::vector<FishingFx> takeFishingFx()
    {
        return std::exchange(fishFx_, {});
    }
    // Test hook: the fishing minigame answers its own arrows and reels in.
    void setAutoFish(bool on)
    {
        autoFish_ = on;
    }
    // Starts the sub-target cursor (CODE 0x10159370) for `line`, which holds `token` (<st>, <stnpc>, ...): the cursor
    // keys and Tab / Shift+Tab pick, Enter runs the line with the token replaced by <id:HEX>, Esc drops it.
    // `targets` (the spell / ability valid-target bits: 0x01 self, 0x02 players, 0x04 party, 0x08 alliance, 0x10 NPCs,
    // 0x20 enemies; 0 = the token's own set) narrows the candidates, as the game's mask table 0x103720d0 does.
    void beginSubTarget(std::string line, std::string token, uint16_t targets, const GameUiData& d);
    [[nodiscard]] bool subTargetActive() const
    {
        return sub_.active;
    }
    // While a command waits for its sub-target (<st>, <stpc>, ...): the entity under the cursor; 0 = none.
    [[nodiscard]] uint32_t subTarget() const
    {
        return sub_.active ? sub_.current : 0;
    }
    // ... and the command line that waits for it (Config AoE range graphics read the spell from it).
    [[nodiscard]] std::string_view subTargetLine() const
    {
        return sub_.active ? std::string_view(sub_.line) : std::string_view();
    }
    // The range record of the command waiting for its sub-target (CODE 0x101576b0 sets it): the range in yalms
    // (255 = no limit) and whether the distance is flat (spells and abilities: y ignored). Commands with no action
    // of their own get the game's default {30 yalms, 3D} (0x10157780, record 0x10335240).
    struct SubRange
    {
        int  yalms = 30;
        bool flat  = false;
    };
    [[nodiscard]] std::optional<SubRange> subTargetRange(const client::WorldSnapshot& w, const dat::BattleText* text, const dat::SpellTable* spells,
                                                         const dat::AbilityTable* abilities) const;
    // For Ashita addons (src/ashita/scan_registry.h ClientUi): the topmost window's layout name ("" when none), the
    // Hide/Show Window state, and the full-screen log.
    [[nodiscard]] std::string topLayout() const
    {
        return stack_.empty() ? std::string() : std::string(layoutName(stack_.back().kind));
    }
    // Every open window's layout name, bottom to top (for /bugreport).
    [[nodiscard]] std::vector<std::string> layoutStack() const
    {
        std::vector<std::string> out;
        for (const auto& m : stack_)
        {
            out.emplace_back(layoutName(m.kind));
        }
        return out;
    }
    [[nodiscard]] bool windowsHidden() const
    {
        return hidden_;
    }
    [[nodiscard]] bool logFull() const
    {
        return logMode_ == LogMode::Full;
    }
    [[nodiscard]] bool menuOpen() const
    {
        return !stack_.empty() || logMode_ != LogMode::Off || fish_.active || sub_.active || num_.active;
    }
    // Our line is out with nothing hooked yet: Confirm / Cancel reel it back in (CODE 0x10088a80 / 0x10088a50, actions
    // 0x3e/0x0e and 0x3e/0x0f), so they go to keyDown although no menu is open.
    [[nodiscard]] bool fishingLineOut() const
    {
        return fish_.casting && fish_.phase == 0 && !fish_.active;
    }
    // Select Window (Num + / Compact F): selects the log window ("menu logwindo"): the cursor keys scroll it,
    // Confirm shows the full-screen log ("menu fulllog"), Cancel steps back. Again: deselects. In the full-screen log
    // it steps to the next channel tab (event 8, CODE 0x101383ea).
    void selectWindow()
    {
        if (logMode_ == LogMode::Full && !typing_)
        {
            stepFullLogTab();
            return;
        }
        // In the Macros windows it opens the options (edit palette event 8, INFERRED to be this key): a key's Copy /
        // Paste / Delete ("menu mcrbedit"; again: the whole set's), or a line's ("menu mcrledit") in the editor.
        if (!typing_ && !stack_.empty())
        {
            auto& top = stack_.back();
            if (top.kind == Kind::MacroEdit)
            {
                macroEditRow_ = top.param;
                macroEditKey_ = std::clamp(top.cursor, 1, 10);
                open(Kind::MacroButtonOp, 1);
                return;
            }
            if (top.kind == Kind::MacroButtonOp)
            {
                top.param = 1 - top.param;
                return;
            }
            if (top.kind == Kind::MacroEditor && top.cursor >= 4 && top.cursor <= 9)
            {
                open(Kind::MacroLineOp, top.cursor - 4);
                return;
            }
            if (top.kind == Kind::Items || top.kind == Kind::ItemSort)
            {
                itemSortFocus(itemSortMove(top.kind == Kind::ItemSort, manualSort_, ItemSortKey::SelectWindow));
                return;
            }
        }
        if (stack_.empty() && !typing_)
        {
            // From the field it steps log window -> status icons -> neither (CODE 0x10157e49; ui/status_select.h).
            const auto next = nextFieldSelect(logMode_ == LogMode::Selected ? FieldSelect::Log
                                              : logMode_ == LogMode::Buffs  ? FieldSelect::Buffs
                                                                            : FieldSelect::None);
            logMode_        = next == FieldSelect::Log     ? LogMode::Selected
                              : next == FieldSelect::Buffs ? LogMode::Buffs
                                                           : LogMode::Off;
            buffCursor_     = 0;
        }
    }
    // Test hook (--ui-keys "mods:ctrl+shift"): modifiers taken as held for the macro keys and the palette.
    void setTestMods(SDL_Keymod m)
    {
        testMods_ = m;
    }
    [[nodiscard]] SDL_Keymod heldMods() const
    {
        return testMods_ != SDL_KMOD_NONE ? testMods_ : focusedModState();
    }
    [[nodiscard]] bool typing() const
    {
        return typing_ || loginActive_ || num_.active; // the number window takes raw keys too
    }

    void draw(ImDrawList& dl, const UiCanvas& canvas, const GameUiData& d, const GameUiActions& act);
    // The game's default for a config id (registrations in 0x10193b00; config_re.md / config_log_re.md).
    [[nodiscard]] static int cfgDefault(int id);
    // Before a session: the login form. Returns true when Enter submits it (account and password filled in).
    bool drawLogin(ImDrawList& dl, const UiCanvas& canvas, LoginFields& f);

private:
    enum class Kind
    {
        Main,
        Social,      // main menu page 2 ("menu socialme"): Missions, Quests, Key Items, ..., Current Time, Shut Down, Log Out
        LogoutConfirm, // "menu comyn": param 0 log out, 1 shut down
        Status,      // "menu statcom": Profile, Job Levels, Combat / Magic / Craft Skills, Monstrosity; with "menu statuswi" and
                     // "menu level" beside it and the chosen window (statusSub_) at 130,48
        Equip,
        EquipList, // param = equip slot
        Items,
        ItemSort,  // "menu itmsortw" right of the Items list: Auto-sort / Manual Sort / Switch (goal #45)
        SortConfirm, // "menu sortyn": Auto-sort's "Sort items?" yes / no, opening on No (#118; ui/yes_no.h)
        ItemBags,  // "menu storage": the containers Switch shows (Mog Safe .. Mog Wardrobe 8) -> Storage
        ItemUse,   // param = container << 8 | slot
        MagicMenu, // categories ("menu mgcmenu")
        Magic,     // param = spell type (1 white, 2 black, 3 summoning, 4 ninjutsu, 5 songs, 6 blue, 7 geomancy, 8 trust;
                   // 0 every type but trust)
        Abilities,
        AbilitySelect, // the battle menu's Abilities ("menu abiselec"): as Abilities without Job Traits
        MagicSelect,   // the battle menu's Magic with Right ("menu magselec", CODE 0x1012aa30): as MagicMenu
        ChatCtrl,      // the battle menu's Chat with Right ("menu chatctrl", CODE 0x1012aa10): pick a chat mode
        AbilityList, // param = 0 job abilities, 1 weapon skills, 2 job traits, 3 pet commands
        DropConfirm, // "Dispose of item?", opening on No (#119; ui/yes_no.h); param = container << 8 | slot
        DropAgain,   // "menu comyn": "Really dispose of the %s?" for Rare / Ex items and linkshells; same param
        Map,         // the current zone's map, with "menu map0" over it
        MapMarkers,  // the map's Markers list ("menu mapv3"): own / event markers, party / alliance; Left / Right: page
        MapMarkerEdit, // an own marker's menu ("menu mapv2"): centre, place / remove, rename; param = slot (0..9)
        Trade,       // giving items to an NPC ("menu handover")
        PlayerTrade, // trading with a player ("menu trade"), opened when the server starts the trade
        TradeItem,   // param = handover box (0..7): pick an inventory item
        TradeQty,    // player trade: how many of a stack ("menu itemctrl"); param = box
        TradeGil,    // player trade: the gil offered ("menu moneyctr")
        PartyPick,   // pick a member: param 0 = make leader (C2S 0x077), 1 = remove (0x071)
        Party,       // param = 0 solo ("menu prty1"), 1 in a party ("menu prty2"), 2 invited ("menu prtyjoin")
        Config,        // "menu configwi": Gameplay, Chat Filters, ..., Windows, ..., Misc. 2, ...
        ConfigWindows, // "menu conf5win" (Window Settings): Window Type 1..8
        ConfigMisc2,   // "menu conf6win" (Misc. 2): Keyboard Size Full / Compact 1 / Compact 2
        ConfigWindowsMenu, // "menu conf5m": shared window settings / log window 1 / 2
        ConfigGameplay,    // "menu conf2win": auto-target, volumes, gamma, item sorting
        ConfigMisc,        // "menu conf3win": damage display, anonymity, shadows, weather, character limit ...
        ConfigMisc3,       // "menu conf12wi": weapon effects, equipment look, AoE ranges, status icons ...
        ConfigMisc4,       // "menu conf13wi": software keyboard, chat filter mode
        ConfigCamera,      // "menu conf7": mouse / camera
        ConfigGlobal,      // "menu conf4": profanity filter, auto-disconnect
        ChatFilter,        // "menu cfilter": 55 message kinds, shown / hidden by the server / hidden in the log only
        EffectFilter,      // "menu fxfilter": battle effects shown / hidden per source -> target pair (cfg 146 bits)
        ConfigLog1,        // "menu conf5w1": log window 1 auto-resize, max / min lines, width, interval (cfg 14..17, 156)
        ConfigLog2,        // "menu conf5w2": the same for log window 2 (cfg 182..184, 197, 198)
        FontColors,        // "menu conftxtc": chat / about you / about others / system, reset
        FontColorList,     // "menu textcol1" (+ the "menu textcol2" sample): param = category 0..3
        FontColorEdit,     // "menu textcol3": the R / G / B bars of one row (fontEdit_)
        ConfigReset,       // "menu confyn": param 0 reset the font colours, 1 the log windows. settings, 2 the key assignment
        KeyAssign,         // "menu k1assign" (param 0, Compact keys) / "menu k2assign" (1, shortcut keys): rows of key + action
        LogSettings,       // "menu conf11m": log window 1 / 2, reset
        LogCategory,       // "menu conf11l": param = window 0 / 1; chat / about you / about others / system
        LogFilter,         // "menu conf11s": param = window * 4 + category; rows toggle the window's mask bits
        Revive,        // "menu comyn": param 0 Return to Home Point?, 1 raise offered, 2 tractor offered (S2C 0x0F9)
        KeyItems,      // "menu evitem": the key items held (S2C 0x055), names and descriptions from d_msg 55695
        Currencies,    // "menu evitem" in mode 1 (Status -> Currencies): S2C 0x113's values, labels 55653 / help 55651
        Quests,        // "menu quset": quest areas x current / completed (window titles 25..36, 136/137, 145/146)
        QuestList,     // "menu quest01": param = area * 2 + (1 = completed); names from d_msg 55706..55713
        Missions,      // "menu missionm": your nation's missions, Rise of the Zilart (the others are dimmed)
        MissionList,   // "menu miss00": param 0 nation, 1 Zilart; names from d_msg 55715..55718
        SynthMenu,     // "menu cmbmenu": Synthesize (help 819); the history list (820) is not done
        SynthCrystal,  // "menu inventor": pick the crystal (LSB's valid crystal ids)
        SynthHistory,  // "menu cmbhlst": recipes used this session (help 820); picking one refills the window
        Synth,         // "menu handover" borrowed: ingredients in boxes 1..8, the crystal in the number field, OK = 0x096
        LinkshellPick, // "menu inventor": linkshell items (shell, pearlsack, pearl; help 294 "Select a linkshell item")
        Linkshell,     // "menu link2": Members (292, not done), Activate / deactivate (290, C2S 0x0C4), Create Linkpearl (293, not done)
        Shop,          // "menu shop": an NPC's goods (S2C 0x03C), opened by S2C 0x03E
        ShopBuy,       // "menu shopbuy": Purchase this merchandise? (464) / Cancel; param = row in WorldSnapshot::shop
        EventMenu,     // an event script's choice menu (op 0x24): "menu query", three rows under the prompt
        Storage,       // "menu inventor": a container's items (param = LSB container id); Enter moves a stack to the inventory
        StoragePut,    // "menu inventor": the inventory's items; Enter moves a stack into container `param`
        DeliveryBox,   // "menu inventor": the delivery box's cells (param 1 incoming, 2 outgoing)
        DeliveryPick,  // "menu inventor": the inventory's items; Enter puts a stack in the outgoing box for sendTo_
        Auction,       // "menu auc1": Bid / Sell / Sales Status (S2C 0x04C Command 2 opens it)
        AucGroups,     // "menu auc2": Weapons, Armor, Magic Scrolls, Medicines, Furnishings, Materials, Food, Crystals, Others
        AucSub,        // param 0..7: "menu aucweapo / aucarmor / aucmagic / aucmater / aucfood / aucmeals / aucammo / aucitem"
        AucList,       // "menu auclist": a category's items and stock (param = LSB aH category)
        AucItem,       // "menu auc3": Price History / Bid for aucItem_
        AucHistory,    // "menu auchisto": the last ten sales
        AucSales,      // "menu auclist": our seven sale slots (Sales Status)
        AucSale,       // "menu auc4": Price History / Remove for sale slot `param`
        AucSellPick,   // "menu inventor": what to put up for sale
        AucYesNo,      // "menu aucyn": param 0 place the bid, 1 place the item on auction
        AucSort,       // "menu wepsort" (param 0) / "menu armsort" (1): Return, then sort / filter rows (auc3's Sort)
        Conquest,      // "menu inventor": regions and who holds them (/regionmap; S2C 0x05E)
        BattleMenu,    // Enter on a monster: "menu battlemo" (param 0) / "menu attackmo" (param 1, engaged)
        Treasure,      // "menu loot": the treasure pool
        TreasureOp,    // "menu lootope": Lot / Pass for pool slot `param`
        ShopMain,      // "menu shopmain": Buy / Sell, opened with a shop
        SellPick,      // "menu inventor": what to sell (C2S 0x084 asks the price)
        ShopSell,      // "menu shopsell": Sell this merchandise? (466) / Cancel → C2S 0x085; param = inventory slot
        GuildMain,     // "menu guildsho": Buy / Sell, opened by S2C 0x086 Stat 0 (a guild merchant)
        GuildBuy,      // "menu shop": the guild's goods (S2C 0x083); asks with C2S 0x0AB
        GuildBuyQty,   // "menu shopbuy": how many (Left/Right) → C2S 0x0AA; param = row in the buy list
        GuildSell,     // "menu shop": what the guild buys (S2C 0x085), first "Select from your inventory."; C2S 0x0AD
        GuildSellPick, // "menu shop": inventory items the guild buys; param = item (0 = any it buys)
        GuildSellQty,  // "menu shopsell": how many (Left/Right) → C2S 0x0AC; param = inventory slot
        RegionMenu,    // "menu region": Conquest, Besieged (Campaign, Colonization dimmed)
        MogHouse,      // "menu myroom": Storage, Delivery Box, Change Jobs, Gardening, Layout, Open Mog House, Remodel
        MogStorage,    // "menu inventor": Mog Safe, Mog Safe 2, Storage, Mog Locker -> Storage
        JobChange,     // "menu jobchang": Main Job / Support Job
        JobPick,       // "menu inventor": the unlocked jobs and levels; param 0 main, 1 support -> C2S 0x100
        SearchMenu,    // "menu searchma": Area, Name, Job, Level (the other rows dimmed); each starts a search
        SearchRefine,  // "menu scsibori" (Refine by Type), from the results' first row: the same pickers on the kept criteria
        SearchArea,    // "menu inventor": this area / all areas
        SearchJob,     // "menu searchjo": element id = job id
        SearchNation,  // "menu searchna": element id - 1 = nation
        SearchRace,    // "menu searchra": element id - 1 = LSB race group (Hume, Elvaan, Tarutaru, Mithra, Galka)
        SearchRank,    // "menu searchle": element id = rank
        CommentGenre,  // "menu comgenre": genre = element id; param 0 edit our comment, 1 search by it
        CommentSub,    // "menu sccompar / sccomite / sccomls" by genre (param); then the text -> C2S 0x0E0
        SearchResults, // "menu scresult": the players found
        SearchOption,  // "menu scoption": Tell / Invite for searchHit_ (Request, Cancel request, PlayOnline dimmed)
        SearchUnity,   // "menu inventor" (the game: "menu arealist" in Unity mode): 12 Unity leaders -> flags 2 = leader << 22
        MuteOption,    // "menu acoption": a Mute List result: "Remove from the Mute list" (dimmed: GMs and mentors only)
        Blacklist,     // "menu blklist" (/blacklist): the blacklisted names (S2C 0x041 / 0x042)
        BlacklistOption, // "menu blkmain": Delete from blacklist -> C2S 0x03D mode 1 for blackPick_
        Communication, // "menu commenu" (main menu page 2): Friend List (dimmed: PlayOnline's) / Emote List
        EmoteList,     // "menu emote": file 96's entries under param (0 = top level, else a folder: /bell, /jobemote, /sitchair)
        AppList,       // an app-fed list (openAppMenu with rows): appMenus_[param]
        AppElements,   // an app-fed element window (openAppMenu without rows): appMenus_[param]
        MacroMenu,     // "menu mcrmenu" (main menu page 2, Macros): Macro Palette / Equipment Sets
        MacroBooks,    // "menu mcrselec": the 40 macro books, 20 a page (param = page 0 / 1)
        MacroBookOp,   // "menu mcrselop": Edit / Rename / Copy / Paste / Delete for book macroOpBook_ (0-based)
        MacroEdit,     // "menu mcr1edit" / "menu mcr2edit" (same place, one shown): the book's set macroSet_; param 0 Ctrl, 1 Alt row
        MacroButtonOp, // "menu mcrbedit" with the cursor: Copy / Paste / Delete; param 1 = the button macroEditKey_, 0 = the set
        MacroEditor,   // "menu macro": the name and six lines of macro macroEditKey_ on row macroEditRow_
        MacroLineOp,   // "menu mcrledit": Copy / Paste / Delete line `param` (0..5) of the macro being edited
        MacroConfirm,  // "menu comyn": param 0 paste book, 1 delete book, 2 paste set, 3 delete set (d_msg 55648 #239 / 240 / 237 / 238)
        EquipSets,     // "menu mcres20" (Macros -> Edit Equip. Set): 20 sets a page, param = page 0..4 (100 sets; goal #45)
        EquipSetOp,    // "menu mcresmn": Edit / Name / Copy / Paste / Delete / Save current / Equip / Lock style for equipSetOp_
        EquipSetEdit,  // "menu mcresed": the set's 16 slots; a slot opens its EquipList in set mode (equipSetEditing_)
    };
    struct Menu
    {
        Kind kind;
        int  cursor = 1; // element id (lists: visible row, 1-based)
        int  scroll = 0; // lists: first row shown
        int  param  = 0;
    };
    enum class Cmd
    {
        Up,
        Down,
        Left,
        Right,
        Confirm,
        Back,
        PageUp,
        PageDown,
    };
    // One entry of a list window.
    struct Row
    {
        std::string           label;
        std::string           right;   // right-aligned (MP cost, count, recast)
        bool                  enabled = true;
        uint16_t              iconItem = 0;
        std::optional<std::pair<std::string, int>> iconSprite; // spell lists: sheet / shape left of the label (+0x12)
        std::string           help;    // shown in the help window while selected
        int                   mpCost = -1; // spells: shown in the sub window ("menu subwindo") as "MP Cost"
        std::string           next;        // ... and the recast left as "Next" (m:ss)
        std::optional<std::string> keycap; // key-assignment rows: the key top in a "[  ]" box left of the label
        std::optional<UiColor> colour;     // the label's colour when not selected (default white)
        int                   invKey = -1;  // item lists: container << 8 | slot of the stack (the info window's marks)
        std::function<void()> confirm;
    };

    [[nodiscard]] const char*       layoutName(Kind k) const;
    // Config sub-windows: the element id of the current setting (Window Type / Keyboard Size).
    [[nodiscard]] int               configCursor(Kind k, const GameUiData& d) const;
    // Config pages (config_re.md §2-4): a choice button sets a config id (or one bit of it) to a value; a slider edits a
    // value in [min, max]. Ids not set take the game's default (registrations in 0x10193b00).
    struct CfgBinding
    {
        Kind     page;
        uint8_t  element;
        uint16_t id;
        int      value  = 0;
        int8_t   bit    = -1;
        bool     slider = false;
        int      min = 0, max = 0;
        int      step = 1;     // sliders: per left / right
        int      base = 0;     // sliders: the bar's empty end (the game draws (value - base) / (max - base))
        bool     number = false; // sliders: the value printed beside the bar
    };
    static const std::vector<CfgBinding>& cfgBindings();
    [[nodiscard]] static int              cfgValue(const GameUiData& d, int id);
    [[nodiscard]] static const CfgBinding* cfgBinding(Kind page, uint16_t element);
    [[nodiscard]] static bool             cfgChosen(const CfgBinding& b, const GameUiData& d);
    [[nodiscard]] static bool             isConfigPage(Kind k);
    // Font colours (config_log_re.md §1): a log line's colour config id (cfg 27..49, 155, 176, 179, 203, 204) by chat
    // kind, the colour itself (0x80RRGGBB, 0x80 = 1.0), and the Log Settings mask bit (0..13, 33..47; -1 none).
    [[nodiscard]] static int              lineColourId(uint8_t kind);
    [[nodiscard]] static UiColor          cfgColour(const GameUiData& d, int id);
    [[nodiscard]] static int              lineBit(uint8_t kind);
    [[nodiscard]] const dat::UiLayout* layout(const Menu& m) const;
    // Layouts moved by the menu registry's stacking flags (0x1000 above the log window), rebuilt as the log moves.
    mutable std::unordered_map<std::string, dat::UiLayout> stacked_;
    mutable float                                          stackedLogTop_ = -1;
    [[nodiscard]] std::vector<Row>  rows(const Menu& m, const GameUiData& d, const GameUiActions& act);
    std::vector<uint16_t>           topRowItems_; // the top list's rows as drawn last: item id, 0 = none / dimmed
    void                            open(Kind k, int param = 0);
    void                            apply(Cmd c, const GameUiData& d, const GameUiActions& act);
    void                            confirm(const GameUiData& d, const GameUiActions& act);
    [[nodiscard]] bool              isList(Kind k) const;
    // Whether an element of a fixed menu does something here (others are drawn dimmed).
    [[nodiscard]] bool              implemented(const Menu& m, const dat::UiRecord& e) const;
    // Spell type of a "menu mgcmenu" element, by its help text id; 0 if none.
    [[nodiscard]] static int        magicCategory(const dat::UiRecord& e);
    const GameUiData*               data_ = nullptr; // during draw(), for implemented()
    [[nodiscard]] int               visibleRows(const Menu& m) const;

    void drawChat(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    void drawParty(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    // The party window's layout: "menu ptw0" (the "Solo" window) with no party and no pet, else "menu ptw{rows}".
    [[nodiscard]] const dat::UiLayout* partyLayout(const GameUiData& d) const;
    // The party window as the windows above it stack on it: raised to our pet's line when we have a pet.
    [[nodiscard]] std::optional<dat::UiLayout> partyStackBase(const GameUiData& d) const;
    static constexpr int kPetLineRaise = 0x17; // the pet line's y above the party window (CODE 0x10204102)
    // Windows the game stacks above another (menu registry flags 0x2000 / 0x4000, 0x1015f6a0): the window moved so its
    // bottom sits 2 units above `base`'s top (at least 16 from the top of the canvas).
    [[nodiscard]] static dat::UiLayout stackedAbove(const UiCanvas& c, const dat::UiLayout& l, const dat::UiLayout& base);
    void drawTarget(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    void drawMenu(UiPainter& p, const UiCanvas& c, const Menu& m, bool top, const GameUiData& d, const GameUiActions& act);
    void drawDead(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    void drawBuffs(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    // "menu gaugewin" (16,48 128x44): our HP / MP / TP, shown with the spell and ability lists (INFERRED from retail).
    void drawGaugeWindow(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    // "menu money" (16,240 112x56, "Current Gil" and the coin in its sprite): our gil, with the Equipment / Items screens.
    void drawMoney(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    [[nodiscard]] bool moneyShown() const;
    // The battle menu's entries for its mode (codes of table 0x1036f250), rebuilt each frame (CODE 0x1012b200).
    void updateBattleCodes(const GameUiData& d);
    std::vector<uint8_t> battleCodes_;
    // "menu subwindo" (384,208 112x42; registry flag 0x4000: above the target window, else the party window): with the
    // spell list, the highlighted spell's MP cost and recast (CODE 0x1014f890 mode 1: 0x1014f9b0 / 0x1014fb50).
    void drawSubWindow(UiPainter& p, const UiCanvas& c, const GameUiData& d, const GameUiActions& act);
    void drawCastBar(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    // Fishing: the hook check after the cast, then the minigame for a hooked fish (S2C 0x115).
    void fishingTick(const GameUiData& d, const GameUiActions& act);
    void drawFishing(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    [[nodiscard]] const dat::Texture* itemIcon(const GameUiData& d, uint16_t id);
    void sendTradeBox(const GameUiActions& act, int box, uint16_t itemId);
    void drawPartnerTrade(UiPainter& p, const UiCanvas& c, const dat::UiLayout& ours, const GameUiData& d);
    // Player trade (game_ui_trade.cpp, trade manager CODE 0x10151510): keys and confirm for "menu trade" and its
    // quantity / gil windows (true: handled), and the sync with the server's trade state.
    bool tradeApply(Menu& m, Cmd c, const GameUiData& d, const GameUiActions& act);
    void tradeConfirm(Menu& m, const GameUiData& d, const GameUiActions& act);
    void tradeSync(const GameUiData& d, const GameUiActions& act);
    void tradePick(int box, uint8_t slot, uint16_t itemId, uint32_t have, const GameUiData& d, const GameUiActions& act);
    void tradeSetBox(int box, uint8_t slot, uint16_t itemId, uint32_t count, const GameUiActions& act);
    void tradeSetGil(uint32_t gil, const GameUiActions& act);
    void tradeCancel(const GameUiData& d, const GameUiActions& act);
    // Trade from a menu (CODE 0x10151a00): accept the target's pending request, or ask them (C2S 0x032).
    void tradeRequest(const client::Entity* target, const GameUiData& d, const GameUiActions& act);
    // The help window with one line of text, scrolling when it is wider than the window.
    void drawHelp(UiPainter& p, const UiCanvas& c, const std::string& text);
    // The network meter; `window` draws its window too (with a menu up). Without, it is skipped when already drawn.
    void drawNetMeter(UiPainter& p, const UiCanvas& c, bool window);
    bool netMeterShown_ = false; // drawn with its window this frame
    int  netMeterPhase_ = 0, netMeterTick_ = -1;
    // The item info window; `key` = container << 8 | slot of the stack shown (-1 = none), for the worn colour and marks.
    void drawItemInfo(UiPainter& p, const UiCanvas& c, uint16_t itemId, const GameUiData& d, int key = -1);
    // The client's animated gauge (0x101caab0): fill sprite scaled by the value, a faint "ghost" between the shown and
    // the real value, and the frame sprite. `shown` eases toward `pct` (15% per frame).
    void drawGaugeSprite(UiPainter& p, ImVec2 at, int fillSprite, int frameSprite, int pct, int& shown, const char* sheet = "menu windowps");
    // The Status menu's windows (CODE 0x10147000 .. 0x101487d0): "menu statuswi", "menu level" and the one statcom shows.
    void drawStatus(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    // "menu statuswi": name, jobs, ILv, HP / MP / TP and STR..CHR; dy moves it down (Equipment shows it at 16,48).
    void drawStatusWindow(UiPainter& p, const UiCanvas& c, const GameUiData& d, int dy);
    int  statusSub_ = -1; // statcom element - 1 whose window is shown (0 Profile .. 4 Craft Skills), -1 none
    int  expShown_ = 0, rankShown_ = 0; // the EXP and rank gauges' animated values
    [[nodiscard]] UiColor hpColour(int hpp) const;

    UiRenderer&       r_;
    std::vector<Menu> stack_;
    std::deque<Cmd>   pending_;
    std::deque<SDL_Scancode> assignKeys_; // key presses for the key-assignment lists, applied in draw()
    bool              typing_ = false;
    SDL_Keymod        testMods_ = SDL_KMOD_NONE;
    bool              paletteClick_ = false;
    std::string       input_;
    // Set while the chat line edits a field (a macro's name or line, a book's name): Enter hands it the text instead
    // of sending it, Esc drops it (CODE 0x101c64d0 opens the game's input line for these).
    std::function<void(std::string)> inputDone_;
    int               chatScroll_ = 0; // lines up from the newest
    bool              logTyping_  = false; // the chat line was open last frame (opening it returns the log to the newest)
    std::chrono::system_clock::time_point chatNewest_{}; // arrival time of the newest log line seen (a new one resets the scroll)
    ImVec2            chatMin_{}, chatMax_{}; // the log window on screen (the wheel scrolls it)
    int               tick_       = 0;
    ImVec2            mouse_{ -1, -1 };
    bool              mouseMoved_ = false;
    bool              click_      = false;
    UiCanvas          lastCanvas_;
    std::unordered_map<uint64_t, int> gaugeShown_; // per (entity, gauge) animated value
    std::unordered_map<uint32_t, std::optional<dat::Texture>> statusIcons_; // icon id | 0x10000 for the "type one" set
    // Macros (docs/ui-format-notes.md §7.9): the game's 40 books x 10 sets x (Ctrl, Alt) x 10 keys, kept in its own
    // files (dat/macro_file.h) in macroDir_. A set is read when first used and written back when changed. Texts are
    // kept as in the files (the game's encoding, auto-translate as FD..FD); macroShown / macroEncoded convert.
    using Macro    = dat::macro::Macro;
    using MacroSet = std::array<std::array<Macro, 10>, 2>; // [0] Ctrl 1..0, [1] Alt 1..0
    mutable std::map<std::pair<int, int>, MacroSet> macroBooks_; // (book 1..40, set 1..10) -> Ctrl / Alt rows
    std::array<std::string, 40>                     macroBookNames_;
    std::filesystem::path                           macroDir_;
    int                                             macroBook_ = 1, macroSet_ = 1;
    [[nodiscard]] MacroSet& macroSetAt(int book, int set) const; // loads it
    [[nodiscard]] const MacroSet& macros() const
    {
        return macroSetAt(macroBook_, macroSet_);
    }
    void                      saveMacroSet(int book, int set);
    void                      saveMacroTitles();
    void                      saveMacroPosition();
    [[nodiscard]] std::string macroShown(std::string_view raw) const;     // file text -> shown
    [[nodiscard]] std::string macroEncoded(std::string_view shown) const; // shown -> file text
    // The running macro (CODE 0x101a1870): its lines (shown), the next one, and a /wait or <wait N> in progress.
    struct MacroRun
    {
        std::array<std::string, 6>            lines;
        int                                   next = -1; // -1: none running
        std::chrono::steady_clock::time_point resume{}, last{};
    } macroRun_;
    void stepMacro(const GameUiData& d, const GameUiActions& act);
    // The Macros windows: the macro / line / book / set held by Copy, and which macro is being edited.
    std::optional<Macro>                    macroClip_;
    std::optional<std::string>              macroLineClip_;
    std::optional<std::array<MacroSet, 10>> macroBookClip_;
    std::optional<MacroSet>                 macroSetClip_;
    int                                     macroEditRow_ = 0, macroEditKey_ = 1, macroOpBook_ = 0;
    mutable dat::UiLayout                   macroOpLayout_; // "menu mcrselop" placed beside the book list's row
    [[nodiscard]] static bool               isMacroKind(Kind k);
    [[nodiscard]] const char*               macroLayoutName(const Menu& m) const;
    [[nodiscard]] const dat::UiLayout*      macroLayout(const Menu& m) const;
    [[nodiscard]] bool                      macroImplemented(const Menu& m, const dat::UiRecord& e) const;
    void                                    macroConfirm(Menu& m, const dat::UiRecord& e);
    bool                                    macroApply(Menu& m, Cmd c); // true: handled
    void                                    drawMacroWindow(UiPainter& p, const UiCanvas& c, const Menu& m, ImVec2 o);
    [[nodiscard]] std::string               macroTitle() const;     // the title window's text, "" = the default
    void                                    drawMacroTitle(UiPainter& p, const UiCanvas& c);
    [[nodiscard]] std::string               macroHelp(const Menu& m, const GameUiData& d) const; // "" = the default
    void                                    macroEditField(std::string initial, std::function<void(std::string)> done);
    struct QueuedLine
    {
        // Implicit: lines are queued as plain strings (mode 2, a macro).
        QueuedLine(std::string t, int m = 2) // NOLINT(google-explicit-constructor)
        : text(std::move(t))
        , mode(m)
        {
        }
        std::string text;
        int         mode = 2;
    };
    std::deque<QueuedLine>               macroQueue_;   // lines waiting to run
    std::deque<std::string>              notesOut_;     // client lines for the log, from row actions without `act`
    bool                                 hidden_ = false; // Hide/Show Window
    using StatusInfoCache                           = std::unordered_map<uint16_t, std::optional<dat::StatusInfo>>;
    enum class LogMode : uint8_t
    {
        Off,
        Selected, // Select Window: the log has the cursor
        Full,     // the full-screen log
        Buffs,    // Select Window again: our status icons have the cursor (#122)
    };
    LogMode                              logMode_  = LogMode::Off;
    // The status icon cursor (Select Window twice, #122): its icon, the icons shown last frame, a Confirm waiting for
    // draw() (which has the actions), and each status's record header from file 87 (dat::statusInfo).
    int                                  buffCursor_  = 0;
    int                                  buffCount_   = 0;
    bool                                 buffConfirm_ = false;
    StatusInfoCache                      statusInfos_;
    const dat::StatusInfo*               statusInfoFor(const GameUiData& d, uint16_t id);
    void                                 drawBuffSelection(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    int                                  logRows_  = 10; // rows the log showed last frame (page size)
    // The full-screen log ("menu fulllog", CODE 0x10138160): its channel tab (filter, +0x54; Select Window steps it)
    // and the highlighted row (+0x1c, counted from the bottom row = 1). logLines_ is how many rows the shown filter has.
    uint8_t                              logFilter_ = 0;
    int                                  logCursor_ = 1;
    int                                  logLines_  = 0;
    bool                                 logWin2_   = false; // Config "multiple log windows" (cfg 180), last frame
    void                                 openFullLog();
    void                                 stepFullLogTab();
    uint32_t                             reviveSeen_ = 0; // last S2C 0x0F9 offer answered with a prompt (WorldSnapshot::reviveSerial)
    // Fishing as the game runs it for our own character (CODE 0x10089030 each frame, the minigame 0x10089780, the fish
    // from S2C 0x115 at 0x1009fc60; docs/commands.md "Fishing"). Times are in the game's 1/60 s ticks.
    struct Fishing
    {
        int      phase     = 0;     // actor+0x13c bits 8..10: 0 line out, 1 waiting / fighting, 2 result asked, 3 ending
        bool     casting   = false; // our status is one of the fishing ones (56..62)
        float    hookWait  = 0;     // ticks until C2S 0x110 mode 2 (from S2C 0x037 +0x4A, seconds)
        float    castLock  = 0;     // ticks until Enter / Esc can reel the line back in (actor +0x1e8)
        bool     reelQueued = false; // Enter pressed while line out: reel in once the lock ends, no hook check (0x1048315c)
        float    tickCarry = 0;     // the fraction of a tick left over from the last frame
        std::chrono::steady_clock::time_point last{}; // the previous frame
        bool     released = false;  // C2S 0x110 mode 4 sent for this cast
        uint32_t serial    = 0;     // last WorldSnapshot::fishSerial taken
        bool     active    = false; // a fish is on the line (the game's object at 0x10487ee8 exists)
        // The fish (0x10487ee8): stamina max / now / shown on the gauge, arrow window factor, regen - 128, arrow rate (move_frequency x 20),
        // arrow damage, arrow regen, ticks left, the warning sent, sense bit 0 (large) / bit 1 (keen), C2S para2.
        int      max = 1, stamina = 0, shown = 0, arrowDelay = 1, regen = 0, rate = 20, damage = 0, heal = 0, timeLeft = 0;
        bool     warned = false, large = false, keen = false, gold = false;
        uint32_t special = 0;
        int      stage = 0, wait = 0, idle = 0, right = 0, wrong = 0; // 0x10487ec8..ed8
        int      dir = 0, strike = 0;                                  // arrow direction (0 right, 1 left), hits / hitl timer
        int      arrowFx = -1;                                         // the arrow's mai effect base (0, 3, 6, 9), -1 none
        std::deque<SDL_Keycode> keys;                                  // presses since the last frame
        uint32_t rng = 0x9E3779B9u;
    } fish_;
    std::vector<FishingFx> fishFx_;
    bool autoFish_ = false;
    // Auction house state: the item in hand (bid or sale), single or stack, the price, the inventory slot being sold.
    uint16_t aucItem_ = 0;
    bool     aucStack_ = false, aucWaitFee_ = false;
    uint32_t aucPrice_ = 0;
    uint8_t  aucSellSlot_ = 0;
    uint32_t aucOpenSeen_ = 0, aucResultSeen_ = 0, itemSearchSeen_ = 0;
    // The auction lists' sort settings, as the game keeps them in AUCSORT.DAT (object 0x10630928: 0x42 categories of up to
    // 20 bytes `dir << 4 | key`, then the counts; 0x574 bytes with two leading pointers; CODE 0x101d72a0 / 0x101d73d0),
    // and the list query being shown: the stored sorts plus filters (race / job) that last until the list is rebuilt.
    std::array<std::vector<uint8_t>, 0x42> aucSorts_{};
    bool                                   aucSortsLoaded_ = false;
    int                                    aucListCategory_ = -1;
    std::vector<client::AuctionTerm>       aucQuery_;
    void aucLoadSorts();
    void aucSaveSorts() const;
    void aucRebuildQuery(int category);
    void aucAddTerm(uint32_t type, bool descending, int value = -1);
    void aucRequestList(const GameUiActions& act, int category);
    static std::vector<std::byte> aucPacket(uint8_t command, int8_t workIndex);
    // A number typed into a small window (prices): digits, Backspace, Enter, Esc.
    struct NumberEntry
    {
        bool                           active = false;
        std::string                    prompt, digits;
        std::function<void(uint32_t)>  done;
        std::function<void(std::string)> doneText; // set: a text field instead (askText)
    } num_;
    void askNumber(std::string prompt, std::function<void(uint32_t)> done);
    void askText(std::string prompt, std::function<void(std::string)> done);
    // The Search window: the criteria (reset to this area when it opens) and the player picked from the results.
    client::SearchQuery searchQuery_;
    client::SearchHit   searchHit_;
    std::pair<uint32_t, std::string> blackPick_; // the blacklist row picked (Kind::BlacklistOption)
    [[nodiscard]] uint32_t linkshellId(int n) const;
    void                runSearch(const std::function<void(const client::SearchQuery&)>& search);
    void                askComment(uint32_t type, const std::function<void(uint16_t, std::vector<std::byte>)>& send);
    void drawNumberEntry(UiPainter& p, const UiCanvas& c);
    std::string sendTo_; // the outgoing delivery box's recipient (/sendpost <name>)
    bool statusTimers_ = false;               // /statustimer: seconds left under the status icons
    // The inventory's display order (Sort / autosort): slots, sorted; items not in it follow in slot order.
    std::vector<uint8_t>                  invOrder_;
    std::size_t                           invCountSeen_ = static_cast<std::size_t>(-1);
    std::chrono::steady_clock::time_point lastSort_{};
    [[nodiscard]] std::vector<client::InventoryItem> sortedInventory(const client::WorldSnapshot& w) const;
    void                                  sortInventory(const GameUiData& d, const GameUiActions& act);
    // Applies an Items list / sort window focus move (item_sort_keys.h); false for ItemSortMove::None.
    bool itemSortFocus(ItemSortMove move);
    // Items, Manual Sort (goal #45): on while picking; the first stack picked (slot, -1 = none yet).
    bool manualSort_ = false;
    int  manualPick_ = -1;
    void manualSortPick(const GameUiData& d, const GameUiActions& act, uint8_t slot);
    // Equipment: equip (index != 0) or remove (index 0) as the game's menus and /equip do (CODE 0x10132700): the
    // client's own refusals first (printed), nothing sent for the item already in that slot. True when sent.
    bool equipFromMenu(const GameUiData& d, const GameUiActions& act, uint8_t container, uint8_t index, uint8_t slot);
    // Why Equipment may not open now (CODE 0x10133060: synthesizing, fishing, status not yet known), set each draw;
    // nullptr = it may. openEquipment() prints it instead of opening.
    const char* equipRefusal_ = nullptr;
    void        openEquipment();
    // Config sliders being edited (left / right change the value live; select or cancel stops): the element id.
    int sliderEdit_ = 0;
    // Font Colors: the row being edited in "menu textcol3" (its config id and label / sample texts) and the bar being
    // changed (0 none, 1..3 = R, G, B).
    struct FontEdit
    {
        int                cfg = 0, sample = 0;
        std::array<int, 3> rgb{};
        int                channel = 0;
    } fontEdit_;
    // Log windows (config_log_re.md §3): the lines shown now with auto-resize, when a line last arrived, per window.
    std::array<float, 2>                                  logShown_{ 8, 8 };
    std::array<LogObjectView, 2>                          logObjects_{};
    std::array<LogHold, 2>                                logHolds_{};
    float                                                 logTop_ = 0; // log window 1's top (authored units), last drawn
    // The compass's fade in (ui/compass.h): restarts on every zone-in.
    uint16_t                              compassZone_ = 0xFFFF;
    uint32_t                              compassGen_  = 0;
    std::chrono::steady_clock::time_point compassStart_{};
    void drawTargetBuffs(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    // The compass and clock above the log (#120).
    void drawCompass(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    std::array<std::size_t, 2>                            logSeen_{ 0, 0 };
    std::array<std::chrono::steady_clock::time_point, 2> logLast_{};
    bool hideYell_ = false, hideAssistJ_ = false, hideAssistE_ = false; // /yellsw, /assistjsw, /assistesw
    // Chat input: lines sent (Up / Down recall them) and Tab completion of auto-translate phrases.
    std::deque<std::string>   history_;
    int                       historyPos_ = -1;
    const dat::BattleText*    text_       = nullptr; // from the latest draw()
    // The chat line's text cursor (a byte position in input_; tokens are stepped over whole) and the auto-translate
    // windows (game_ui_autotranslate.cpp): "menu fep" (candidates for the typed word), "menu fp_cat" (the
    // categories) and "menu fp_cat2" (a category's phrases).
    std::size_t cursor_ = 0;
    struct AtRow
    {
        std::string token; // FD .. FD, or empty for a category row
        std::string text;
        int         group = -1;
    };
    struct AtWindow
    {
        enum class Mode
        {
            Closed,
            Candidates,
            Categories,
            Phrases,
        };
        Mode               mode = Mode::Closed;
        std::vector<AtRow> rows;
        int                sel = 0, top = 0;
        std::size_t        wordFrom = 0; // Candidates: the typed word being replaced, [wordFrom, cursor_)
        int                category = 0; // the Categories row to return to from Phrases
    };
    AtWindow at_;
    std::vector<std::pair<std::string, uint16_t>> atItems_; // item names, sorted (built on first use)
    bool atKey(SDL_Keycode key);   // a key while the chat line is open; true when taken
    void atOpen();                 // Tab: candidates for the typed word, or the categories when there is none
    void atInsert(const std::string& token, std::size_t from);
    void atDraw(const UiCanvas& c, UiPainter& p, ImVec2 lineAt);
    void chatInsert(std::string_view text); // typed text at the cursor
    float drawChatText(UiPainter& p, std::string_view s, ImVec2 at, UiColor colour, std::size_t cursor) const;
    // A log line: the "{phrase}" spans the session expanded from tokens get the green / red brackets.
    float drawLogText(UiPainter& p, std::string_view s, ImVec2 at, UiColor colour) const;
    struct EquipSet
    {
        std::string                                  name;
        std::vector<std::pair<uint8_t, std::string>> items; // equip slot, item name (lower case)
    };
    std::map<int, EquipSet>  equipSets_;
    int                      equipSetOp_      = 0;  // the set the options window acts on (1..100)
    int                      equipSetEditing_ = 0;  // EquipSetEdit / EquipList in set mode: the set (0 = none)
    std::optional<EquipSet>  equipSetClip_;          // Copy -> Paste
    mutable dat::UiLayout    equipSetOpLayout_;      // "menu mcresmn" placed beside the chosen row
    std::filesystem::path    equipSetFile_;
    void                     saveEquipSets() const;
    // Sub-target selection: a line holding <st> / <stpc> / <stnpc> / <stpt> / <stal> waits here; the cursor keys pick,
    // Enter runs the line with the token replaced by <id:HEX>, Esc drops it. <lastst> reuses the last pick.
    struct SubTarget
    {
        bool                    active = false;
        std::string             line, token;
        uint16_t                targets = 0; // valid-target bits narrowing the token's candidates (0 = the token's own)
        uint32_t                current = 0, last = 0;
        std::deque<SDL_Keycode> keys;
        std::vector<Menu>       resume; // the menus hidden under a cursor raised from a list (back on Esc)
    } sub_;
    [[nodiscard]] std::vector<uint32_t> subTargetCandidates(const GameUiData& d) const;
    // A spell / ability / weapon skill chosen from a menu list: hides the menus and raises the sub-target cursor on its
    // valid targets for `/<command> "<name>" <st>` (CODE 0x1022c300, 0x10223d9e); false (menus kept) when nobody
    // qualifies.
    bool menuSubTarget(std::string_view command, std::string_view name, uint16_t targets, const GameUiData& d);
    static std::vector<std::byte> deliveryPacket(uint8_t command, int8_t box, int8_t post = -1, int8_t item = -1, int32_t stacks = -1,
                                                 std::string_view name = {});
    void drawSubTarget(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    std::chrono::steady_clock::time_point macroResume_{}; // after a /wait
    // Submits a line as typed in the chat box (commands first, then channel prefixes and say).
    void submitLine(const std::string& text, const GameUiData& d, const GameUiActions& act, int mode = 1);
    void drawMacroPalette(UiPainter& p, const UiCanvas& c, int set);
    // Lobby (before entering the world): the game's lobby menu, then the character list.
    void drawLobby(UiPainter& p, const UiCanvas& c, const GameUiData& d, const GameUiActions& act);
    int  partyTargetKey_ = 0; // F1..F6 pressed (1..6): target that party row
    bool lobbyActive_ = false;
    bool loginActive_ = false;
    bool loginSubmit_ = false;
    LoginFields* login_ = nullptr;
    int  lobbyStage_  = 0; // 0 lobby menu ("menu loby2win"), 1 character list
    int  lobbyCursor_ = 1; // element id (stage 0) or slot index + 1 (stage 1)
    std::unordered_map<uint16_t, std::optional<dat::Texture>> itemIcons_;
    uint16_t                                      mapZone_ = 0xFFFF;
    int8_t                                        mapId_ = 0, mapShownId_ = -1;
    bool                                          mapOpen_ = false;
    // The Markers list (ui-format-notes §6.5): the page shown on the map (-1 none yet, else retail's page 0..3), the
    // list's page (kept between openings like mapframe +0x106), the member whose name label shows (the confirmed row)
    // and whether C2S 0x0D2 went out for this map.
    int                                           markerPage_ = -1;
    int                                           markerListPage_ = 0;
    uint32_t                                      markerSel_  = 0;
    bool                                          markerAsked_ = false;
    // Own markers (page 0) and event markers (page 1) per map, read from / written to the USER folder's MRK files.
    mutable std::map<std::pair<int, int>, MapMarkerSlots> markerSlots_;
    MapMarkerSlots& markerSlots(int zone, int mapId) const;
    void            saveMarkerSlots(int zone, int mapId) const;
    // Whether the shown map (mapZone_ / mapId_) can be viewed: its key item, or an addon opening every map.
    [[nodiscard]] bool haveMap(const GameUiData& d) const;
    // Placing an own marker (CODE 0x101f86d0 / 0x101f8720): a cursor on the map moved with the arrows, Enter places
    // marker `markerPlacing_` there and asks its name, Esc cancels.
    int                                           markerPlacing_ = -1;
    float                                         markerCursorU_ = 256, markerCursorV_ = 256;
    struct EventMapMarker
    {
        float       x = 0, z = 0;
        std::string label;
        int         slot = -1;
    };
    // Op 0x8B markers waiting for the map under the player to be known (stored on the next drawn frame).
    struct PendingEventMarker
    {
        int         zone = 0, slot = 0;
        float       x = 0, z = 0;
        std::string label;
    };
    std::vector<PendingEventMarker>               pendingEventMarkers_;
    int                                           eventMapZone_ = -1, eventMapId_ = -1;
    bool                                          eventMapUnderPlayer_ = false;
    std::string                                   mapGridRef_; // "(H-8)" while a map of where we are is up
    static std::string mapGridRef(float u, float v);
    void               drawMapGridRef(UiPainter& p, const UiCanvas& c);
    std::optional<dat::Texture>                   eventMapTexture_;
    std::vector<EventMapMarker>                   eventMapMarkers_;
    std::optional<dat::Texture>                   mapTexture_;
    // Trade window state: the NPC, and per box the inventory slot (0 = empty) and count.
    uint32_t                                      tradeId_ = 0;
    uint16_t                                      tradeIndex_ = 0;
    std::array<std::pair<uint8_t, uint32_t>, 8>   tradeBoxes_{};
    uint32_t                                      tradeGil_ = 0;
    uint32_t                                      tradeEchoSeen_ = 0; // last WorldSnapshot::Trade::mineUpdates applied
    // Player trade: OK pressed (state 4) until either offer changes; the offer counters seen at the lock; the trade
    // serial this client closed itself (Cancel, range); the quantity / gil window's value, maximum and digit step.
    bool                                          tradeLocked_ = false;
    uint32_t                                      tradeLockMine_ = 0, tradeLockTheirs_ = 0;
    uint32_t                                      tradeClosedSerial_ = 0;
    struct TradeCtrl
    {
        uint32_t value = 0, max = 0, step = 1;
        int      held  = 0;     // ticks the same key has been held (60: the step goes from 1 to 10)
        uint8_t  slot  = 0;     // quantity: the inventory slot and item
        uint16_t item  = 0;
    } tradeCtrl_;
    // Synthesis: the chosen crystal (inventory slot and item id); the ingredients use tradeBoxes_.
    uint8_t                                       synthCrystalSlot_ = 0;
    uint16_t                                      synthCrystalId_   = 0;
    // Synthesis history (this session): the crystal and ingredient item ids of each recipe sent, newest first.
    struct SynthRecipe
    {
        uint16_t                crystal = 0;
        std::array<uint16_t, 8> items{};
    };
    std::vector<SynthRecipe>                      synthHistory_;
    void sendSynthesis(const GameUiActions& act, const client::WorldSnapshot& w); // C2S 0x096 from the window's state
    bool refillSynthesis(const SynthRecipe& r, const client::WorldSnapshot& w);   // the window's state from a recipe
    uint8_t                                       chatKind_ = 0; // /chatmode: the channel of lines without a prefix
    uint8_t                                       linkSlot_         = 0; // the linkshell item picked (inventory slot)
    uint32_t                                      shopSeen_         = 0; // last WorldSnapshot::shopSerial opened
    uint32_t guildOpenSeen_ = 0, guildCloseSeen_ = 0; // last WorldSnapshot::guild open / close answers acted on
    float    guildAtX_ = 0, guildAtZ_ = 0;             // where we stood when the guild menu opened (CODE 0x101ed280)
    uint32_t                                      shopQty_          = 1; // how many to buy (Left/Right, PageUp/PageDown)
    std::string                                   eventPrompt_;
    std::vector<std::pair<int, std::string>>      eventOptions_;
    std::optional<int>                            eventChoice_;
    struct AppMenu
    {
        int                 tag = 0;
        std::string         layout, prompt;
        std::vector<AppRow> rows;
        std::vector<int>    dimmed;
        std::string         title; // drawn in the window above the rows (e.g. "menu mogdoor", rows from y 20)
        bool                keepStack = false; // a page (openAppPage): a choice leaves it open, cancel pops it
        std::string         windowTitle;       // pages: the title window's text
    };
    std::vector<AppMenu>                          appMenus_; // cleared when no menu is open
    std::optional<std::pair<int, int>>            appChoice_;
    std::optional<std::tuple<int, int, int>>      appAdjust_;
    bool                                          eventWaiting_ = false;
    std::function<void(uint32_t)> sound_;

public:
    // --- Event windows (event op 0xB4, CODE 0x100b0930; ui-format-notes §11) ---
    // 02 / 03: the target window in event mode: `name` and a gauge 0..100 (-1 closes it).
    void eventTarget(std::string name, int gauge)
    {
        eventTargetName_  = std::move(name);
        eventTargetGauge_ = gauge;
    }
    // 07..0C: the cast-time (0) / event-time (1) window counting `ticks` (1/60 s) down; -1 closes it.
    void eventTimer(int which, int ticks);
    [[nodiscard]] int eventTimerLeft(int which) const;
    // 14: the event map window's view: offset (x, y) in map pixels and a zoom, reached over `seconds` (0 = at once).
    void eventMapView(float x, float y, float zoom, float seconds, int flags);
    // The event ended: its windows close.
    void clearEventWindows()
    {
        eventTargetGauge_ = -1;
        eventTimers_      = {};
    }

private:
    void        drawEventWindows(UiPainter& p, const UiCanvas& c, const GameUiData& d);
    // The gauge window is up with the spell and ability lists.
    [[nodiscard]] bool gaugeWindowShown() const
    {
        return !stack_.empty() && (stack_.back().kind == Kind::Magic || stack_.back().kind == Kind::AbilityList);
    }
    std::string eventTargetName_;
    int         eventTargetGauge_ = -1;
    struct EventTimer
    {
        bool  open  = false;
        float total = 0; // +0x14, ticks (1/60 s)
        float left  = 0; // +0x18
    };
    std::array<EventTimer, 2>             eventTimers_{};
    std::chrono::steady_clock::time_point eventTimersStep_{};
    void                                  stepEventTimers();
    struct MapView
    {
        float x = 0, y = 0, zoom = 1;
    };
    MapView                                eventViewFrom_, eventViewTo_;
    float                                  eventViewSeconds_ = 0;
    std::chrono::steady_clock::time_point  eventViewStart_{};
    [[nodiscard]] MapView eventMapViewNow() const;
};

} // namespace xi::ui
