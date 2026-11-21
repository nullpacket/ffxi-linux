// SPDX-License-Identifier: GPL-3.0-or-later
// Native stand-ins for the memory signatures Ashita addons scan FFXiMain.dll for (docs/ashita-compat.md, "Native
// stand-ins for memory signatures"). Addons find code by byte pattern, read the addresses its instructions hold, then
// read or write the game objects those point at, often through `ffi.cast` (so the memory has to be real). Here:
//  - the game image: the user's own FFXiMain.dll, unpacked and mapped as data at its own base (0x10000000), so a
//    pattern is found where the game has it and every address inside the image is readable and writable;
//  - an arena of low memory (below 2 GB: addons keep pointers in uint32_t) for the objects the game would have on its
//    heap, and for signature copies when the image cannot be mapped;
//  - stubs: x64 jumps written over the few game functions addons call (the config get / set), to C++ functions here.
// prelude_scan.lua fills the emulated objects from the client every frame and routes addon writes to the client.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lua_State;

namespace xi::ashita
{

// What the client shows, once a frame (render thread). Defaults: nothing open, no event, interface shown.
struct ClientUi
{
    std::string topMenu;              // the topmost window's layout name as the game names it ("menu    map0"), or ""
    bool        eventActive     = false; // an event (cutscene / NPC talk) is running
    bool        interfaceHidden = false; // the interface is hidden (the game's hide-interface toggle)
    bool        chatExpanded    = false; // the log window is fully expanded
    bool        eventMessage    = false; // an event message window is up (the game's 0x1047d3ec)
};
void setClientUi(ClientUi ui);
[[nodiscard]] ClientUi clientUi();

// The game's configuration values (Config / client.ini ids, ui-format-notes §8). Unset members answer as "no such id".
struct ConfigEntry
{
    int32_t value = 0, type = 0, min = 0, max = -1, def = 0;
};
struct ConfigHooks
{
    std::function<std::optional<int32_t>(int32_t id)>     get;
    std::function<bool(int32_t id, int32_t value)>         set;
    std::function<std::optional<ConfigEntry>(int32_t id)> entry;
    std::function<void(int32_t value)>                     autoOffline; // config 65's auto-offline timer
};
void setConfigHooks(ConfigHooks hooks);

// Client settings that addons change by patching the game's memory (docs/ashita-compat.md, "Native settings"). A
// patch's signature answers only when `supports` says yes for every setting it stands for (else 0, a failed scan, so
// the addon reports it cannot work rather than appearing to). `get` is the client's current value (what the addon
// reads; nullopt: the game's default), `set` receives each change an addon writes. Names and values:
//   draw_distance.world, draw_distance.entity  factors (1 = the client's own; the game multiplies its clip / fog and
//                                               entity distances by them)
//   aspect.factor                              height / (width x 0.75); 1 = the game's 4:3 projection
//   clear_colour                               D3DCOLOR (ARGB) behind the scene
//   name_colour.0 .. name_colour.22            D3DCOLOR of each name-colour kind (the game's table, 0x80808080 each)
//   window.party.visible, window.target.visible, window.alliance1.visible, window.alliance2.visible   1 shown, 0 hidden
//   fps.divisor                                frames per second = 60 / n (the game's default 2)
//   chat.instant                               1 = incoming chat shows without the game's delay
//   chat.filter.0 .. chat.filter.2             the chat filter bit words (as C2S 0x0DB carries them)
//   camera.distance.min / .max                 follow distance band (3 / 6), camera.battle_distance.min / .max (7.2 /
//                                              8.6) when engaged, camera.pan.horizontal / .vertical (0.0279 rad /
//                                              0.107 a tick), camera.battle_range
//   map.all, map.all.view                      1 = every map shows without its key item (the Map menu / the marker
//                                              view); a code patch, so 0 again when its addon unloads
//   log.window1.* / log.window2.*              the log objects (ui/log_object.h): height (+0x34, the client's own
//                                              shown height in pixels; set: an addon's write, -1 when it unloads),
//                                              lines, max_lines, min_lines, text_top (above the screen's bottom), shown
struct NativeHooks
{
    std::function<bool(std::string_view name)>                  supports;
    std::function<std::optional<double>(std::string_view name)> get;
    std::function<void(std::string_view name, double value)>    set;
};
void setNativeHooks(NativeHooks hooks);

// The client's macro palette, for the game's FsMacroContainer / FsMacroController functions addons call (XIUI's
// macro bar, macrofix). `index` is 0..19 on the current book and page: 0..9 the Ctrl row (keys 1..0), 10..19 the Alt
// row. Books and pages are 0-based here. Unset `name`: the macro signatures answer 0.
struct MacroHooks
{
    std::function<bool()>                                                       canUse;  // not typing, not in an event
    std::function<std::string(int index)>                                       name;    // up to 8 characters
    std::function<std::string(int index, int line)>                             line;    // line 0..5
    std::function<void(int row, int key)>                                       run;     // row 0 Ctrl / 1 Alt, key 0..9
    std::function<void(int index, const std::string&, const std::vector<std::string>&)> set; // title, lines; empty = clear
    std::function<bool(int book)>                                               setBook; // 0..39
    std::function<bool(int page)>                                               setPage; // 0..9
    std::function<bool()>                                                       running;
    std::function<void()>                                                       stop;
};
void setMacroHooks(MacroHooks hooks);

// The game's clock for its time functions (game_time.h). `gameTime`: the game's raw time (the last zone-in's 0x00A
// GameTime plus the seconds since; unset or nullopt: the system clock less the Vana'diel epoch, which is what LSB
// sends); `text`: string table 10's entry `id` (the date / time formats).
struct TimeSource
{
    std::function<std::optional<uint32_t>()> gameTime;
    std::function<std::string(int id)>        text;
};
void setTimeSource(TimeSource source);

// What the game's target helpers read (fancychat's `<bt>`, `<r>`, `<scan>`): unset members answer as the game does
// when there is nothing (no battle target, no teller, no name).
struct TargetHooks
{
    // SeekBattleActor: the lowest-index living monster our party has claimed: its index and server id.
    std::function<std::optional<std::pair<uint16_t, uint32_t>>()> battleTarget;
    std::function<std::string()>                                   lastTeller; // ntSysGetLastTeller: "" none
    std::function<std::string(uint16_t index)>                     zoneName;   // GetMonsterName: the zone's name table
};
void setTargetHooks(TargetHooks hooks);

// The DLL to map. Unset: $FFXI_ROOT/FFXiMain.dll. Read on the first scan; later calls change nothing once mapped.
void setGameImage(const std::filesystem::path& ffxiMainDll);

struct ImageInfo
{
    uintptr_t base = 0; // 0 when the image could not be mapped (no DLL, or its range taken)
    std::size_t size = 0;
};
[[nodiscard]] ImageInfo gameImage(); // maps it on first use

// The first `count`-th (0-based) match of an Ashita pattern ("8B0D????????85C9") in the image, or 0.
[[nodiscard]] uintptr_t findPattern(std::string_view pattern, int count = 0);

// Zeroed low memory (below 2 GB, readable, writable, executable for stubs). The same `key` gives the same block for
// the life of the process; an empty key always gives a new block.
[[nodiscard]] uintptr_t lowAlloc(std::size_t size, const std::string& key = {});

// True when [addr, addr + size) is inside the image or the arena (reads and writes there cannot fault).
[[nodiscard]] bool validRange(uintptr_t addr, std::size_t size);

// The game functions an addon may call, each answered by a C++ function here.
enum class Stub
{
    ConfigGet,   // int32 (int32 id)
    ConfigSet,   // int32 (int32 id, int32 value)
    ConfigEntry, // int32 (this, int32, int32 id): the address of an FsConfigSubject, or 0
    AutoOffline, // int32 (int32 value)
    // FsMacroController / FsMacroContainer (this first, as the game's __thiscall):
    MacroCanUse,  // bool (this)
    MacroClear,   // bool (this, index)
    MacroName,    // const char* (this, index)
    MacroLine,    // const char* (this, index, line)
    MacroRun,     // void (this, u8 mod 1 Ctrl / 2 Alt, u8 index)
    MacroSetBook, // int32 (this, book)
    MacroSetPage, // int32 (this, 2 x page)
    MacroSet,     // void (this + 380 x index + 4, title, 6 lines of 61 bytes)
    // Others:
    EventMessageKey, // void (this, i16 key, i16): TkEventMsg2::OnKeyDown, a key on the event message (5 = Enter)
    Chime,           // void (i32 id, float power, bool force): YmSePlayChime
    EquipEx,         // u8 (u8 isSubJob, u16 jobType, u16 index, u8 id): sends C2S 0x102 (BLU spell slots)
    // The game's clock (game_time.h; the `ffxi.time` library):
    GameTimeGet,     // u32 (): ntGameTimeGet
    GameTimeDiff,    // u32 (u32 t): ntGameTimeDiff
    GameWeekday,     // u32 (u32 t): ntGameDayWeekGet; GameDay .. GameMoonPercent the same
    GameDay,
    GameMonth,
    GameYear,
    GameMinutes,
    GameHours,
    GameMoon,
    GameMoonPercent,
    UnixTime,        // u32 (): xiGetTime
    TimeString,      // const char* (u32 unixTime, u8 flags): YkGetTimeString
    LocalTime,       // tm* (u32 unixTime): sqLocalTime
    // The game's target helpers (fancychat's targets.lua):
    BattleActor,     // XiAtelBuff* (): SeekBattleActor (+0x74 index, +0x78 server id), or null
    MonsterName,     // const char* (u16 index): GetMonsterName, or null
    LastTeller,      // const char* (): ntSysGetLastTeller, or null
    TrackingPos,     // GC_TRACKING_NOW* (): gcTrackingPosGet, or null while nothing is tracked
};
// The FsMacroContainer object the macro stubs were given (MacroSet's index is found from it).
void setMacroObject(uintptr_t addr);
// Writes a jump to `stub` at `addr` (inside the image or the arena). `keep` bytes after the first two are left as they
// are, then 4 zero bytes (a short jump steps over both), for functions whose first instruction an addon also reads
// (config's `this`; the zeros let an x64 `uint32_t**` cast read the kept address as a whole pointer).
bool writeStub(uintptr_t addr, Stub stub, int keep = 0);

// The natives prelude_scan.lua uses, into the table on top of the stack (__xi).
void openScan(lua_State* L);

} // namespace xi::ashita
