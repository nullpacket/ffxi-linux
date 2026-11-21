# Ashita compatibility

Goal 31: Ashita v4 addons run unchanged in the client. `src/ashita/` gives each addon its own LuaJIT state
with the `ashita` / `AshitaCore` API Ashita's addon loader provides (an embedded prelude over a small native module), and
answers as one of two Ashita releases. This page is the reference for what those two releases are, how they differ, what
the layer covers, and what it cannot do. Checked 2026-10-20.

## The two releases

| | Latest | HorizonXI |
|---|---|---|
| Ashita.dll / Ashita-cli.exe | **4.3.2.1** (upstream `AshitaXI/Ashita-v4beta` main, 2026-09-11) | **4.3.1.2** (HorizonXI launcher 1.9.0, `version.json`) |
| `ashita.addons_version` / `ashita.interface_version` | 4.20 / 4.30 | 4.20 / 4.30 |
| `plugins/sdk` (39 headers) | identical | identical |
| `addons/libs` | 115 files | the same 115, unchanged, plus 16 of HorizonXI's own |
| Bundled addons | 81 | 87 with a main file (122 folders: HorizonXI's own and the user's) |
| Profile in the layer | `Profile::Latest` (`latest`, `4.3.2*`) | `Profile::HorizonXI` (`horizonxi`, `hxi`, `4.3.1*`) |

Versions are read from the DLLs' `VS_FIXEDFILEINFO`. The user's local checkout `~/git/Ashita-v4beta` is older,
**4.3.0.2**; it was read but not used as "latest" (upstream was fetched as a tarball into the scratchpad instead).

What differs, by diffing the trees (line endings ignored):

- **Libraries.** HorizonXI adds `bitmap.lua`, `color.lua`, `lin/` (ffxi, imgui, jobs, packets, set, text, zones) and
  `typedefs/` (entity, events, font, imgui, packets, party, player). Every other lib file is byte-identical, including
  `annotations/`. `detectProfile()` picks HorizonXI when `addons/libs/typedefs` or `addons/libs/lin` exists.
- **Plugin events.** `IPluginManager:RaiseEvent` / `IPolPluginManager:RaiseEvent` gained the
  `(event_name, event_data, event_size)` overload in the annotations of both releases; 4.3.0.2 has only the two-argument
  form. Both forms return nil: plugin events are not passed between the Lua layer and native plugins yet (section
  "Plugins").
- **Addon folders in both** (66): 10 addons differ in content (chatfix, crosshair, fastswap, freemem, hideui, links,
  peekaboo, renamer, truesight, watchdog), and the differences are version numbers, copyright years,
  `require 'x'` vs `require('x')` and comments. 26 of HorizonXI's folders have no main file (ahgo, autojoin, blucheck,
  mapdot, nomad, noname ... and two backups), so those addons exist only in latest. No addon in either release depends
  on an API the other lacks.
- **Only in latest:** actionparse, activemon, affinity, ahcolors, autologin, cartographer, chamcham, chime, clearcolor,
  hideobs, invmon, namecolors, quicksets, stepdialog, trimspells.
- **Only in HorizonXI:** its own addons (HXUI / XIUI, tCrossBar, tHotBar, statustimers, partybuffs, treasurepool,
  luashitacast, timers, mobdb ...), several of them in mixed-case folders.

So the API surface is the same; a profile changes the version strings addons can read (`__xi.version`), the libs the
root provides, and nothing else.

## Coverage

Use counts are how many addons of the two releases (deduplicated by folder) call the event or function.

### Events

| Event | Addons | Layer |
|---|---|---|
| `command` | 89 | yes, `AddonManager::command` (typed mode 1, macro 2; `/addon` itself first) |
| `load` / `unload` | 78 / 46 | yes |
| `d3d_present` / `d3d_beginscene` | 68 / 7 | yes, inside the client's ImGui frame (`present()`); an addon left inside `Begin` is recovered |
| `packet_in` / `packet_out` | 61 / 23 | yes, session thread; `data`, `data_modified`, `data_raw`, `data_modified_raw`, `chunk_*`, `injected`, `blocked` |
| `text_in` | 18 | yes, per new log line (by `ChatLine::serial`); changed / hidden lines kept for the log (`lineEdit`) |
| `text_out` | 2 | yes |
| `mouse` / `key` | 11 / 5 | yes, from the client's SDL events before the game sees them (`ClientApp::addonInput`): Win32 message numbers (`WM_MOUSEMOVE` 0x200, buttons 0x201-0x208, `WM_MOUSEWHEEL` 0x20A with ±120 a notch), window coordinates (the addons' draw space); keys as Windows virtual keys (`ashita/input_map.h`; either Shift is `VK_SHIFT`), `lparam` repeat / up bits. A blocked press, move, wheel or key-down stops there; releases always reach the game. Presses and the wheel over an ImGui window stay that window's |
| `d3d_endscene` | 1 | yes, after `d3d_present` |
| `plugin_event` | 6 | registers, never raised (plugin events are not bridged yet; section "Plugins") |
| `xinput_button` / `xinput_state` / `dinput_button` / `dinput_state` | 5 / 2 / 4 / 2 | register, not raised (gamepad events) |
| `key_state`, `key_data` | 1 each | register, not raised |

Callbacks run as coroutines, as in Ashita: one may `coroutine.sleep` (LuAshitacast does inside packet_in) and the rest
of it runs later as a task. `load` gets the command line that loaded the addon (autologin reads its arguments).

### AshitaCore

| Getter | Addons | Layer |
|---|---|---|
| `GetMemoryManager` | 60 | yes: party, player, entity, target, inventory, recast, cast bar, auto-follow from the client's state (prelude_memory.lua) |
| `GetResourceManager` | 42 | yes: items, spells, abilities, statuses, zone names from the DATs |
| `GetChatManager` | 28 | yes: `QueueCommand`, `Write` / `AddChatMessage`; the input line is not exposed (empty) |
| `GetPacketManager` | 19 | yes: `AddOutgoingPacket`, `AddIncomingPacket`; `QueuePacket` warns |
| `GetPointerManager` / `GetOffsetManager` | 17 / 1 | stubs (0) |
| `GetFontManager` / `GetPrimitiveManager` | 12 / 5 | yes, drawn with ImGui after `d3d_present`; fonts by fontconfig name |
| `GetGuiManager` | 5 (+ every `require('imgui')`) | yes, ~200 ImGui functions (lua_imgui.cpp) |
| `GetConfigurationManager` | 6 | yes, ini files under `config/` |
| `GetProperties`, `GetPolPluginManager`, `GetInputManager`, `GetLogManager` | 1-5 | yes / stubs; the window handle is 0x1000 and focus follows the client window |
| `GetPluginManager` | 10 | the one plugin there always is, "Addons" (`Get('addons')`, `IsLoaded('addons')`, its interface version); RaiseEvent does nothing |
| `GetDirect3DDevice` | 9 | the D3D8 stand-in ("Direct3D 8 and gdifonts") |
| `GetDataManager`, `GetAddonManager` | 4 / 2 | interfaces whose methods are stubs |

A method an interface lacks answers through a stub that logs its name once and returns 0; any other key (a lowercase
field such as `player.isZoning`, which HXUI tests) reads nil, as in Ashita.

### `ashita.*`

`ashita.fs`, `time`, `misc`, `regex`, `bits`, `tasks` are implemented as annotated (`misc.execute`, `open_url`,
`play_sound` warn and return false). `ashita.memory` works on real memory: the user's own FFXiMain.dll mapped as data
at its base, and native stand-ins for the game objects the common signatures lead to (see "Native stand-ins for memory
signatures" below); any other pattern returns 0, as a failed scan does in Ashita.

The unannotated aliases real addons use are there too: `ashita.memory.read_uint` / `read_int` / `write_uint` /
`write_int`, `ashita.time.qpc` / `qpf` / `glt` / `gst` / `tick`, `ashita.fs.current_dir` / `equals` / `get_install_dir`.

What Ashita's LuaJIT (32-bit Windows, LUAJIT_ENABLE_LUA52COMPAT) gives addons and the layer adds back:

- **Paths and modules as on Windows.** `\` works as a separator everywhere (io, dofile / loadfile, os, ashita.fs), and a
  path or module that does not exist as spelt is matched without regard to case (`tTimers` for `ttimers`,
  `require 'imguiWrap'` for imguiwrap.lua, `require('lib\\constants')`). Addon folders load whatever case is typed.
  `GetInstallPath()` ends in a separator, as Ashita's does.
- **ffi.cdef as Ashita's LuaJIT reads it:** integer suffixes (`0x8000FFFFL`, `10UL`) are dropped, a declaration
  repeated by a second file is skipped piece by piece, and (prelude_d3d.lua) C `long` is 32 bits.
- **Lua 5.2 compatibility:** `table.pack` / `table.unpack`, `math.log(x, base)`, `string.rep(s, n, sep)`; Lua 5.3's
  `utf8` library; Ashita v3's `_addon` name for `addon`.
- **Win32 through ffi.C** answered by stand-ins that succeed: `MultiByteToWideChar` / `WideCharToMultiByte` (iconv:
  Shift_JIS, UTF-8), `GetModuleHandleA` (a zeroed block), `SetProcessWorkingSetSize`, `GetFocus` /
  `GetForegroundWindow`, `GetTickCount`, `Sleep`; `ffi.load('winmm')` (`timeGetTime`, `PlaySound` succeeds silently);
  user32 / kernel32 pieces for fancychat (prelude_d3d.lua). Further stand-ins register with `register_ffi_c(name, fn)`
  and `__xi_ffi_load[dll]`.
- **LuaSocket and LuaSec**, built from source against the client's LuaJIT (CMake fetches luasocket 3.1.0 and luasec
  1.3.2): `socket.core`, `mime.core`, `ssl.core` / `context` / `x509` / `config` through `package.preload`; the Lua halves
  are the ones in Ashita's `addons/libs`. HTTP and HTTPS requests from an addon were checked live (status 200).
- **ImGui as Ashita's GuiManager:** about 230 functions, every `ImGuiStyle` field through `GetStyle()` (ImVec2s as
  `{ x, y }`), `Colors` writing through, `AddFontFromFileTTF` (a Windows font path becomes the closest installed face) and
  `PushFont` with it, draw lists with text in a font, clip rects and paths. An `End()` with no `Begin()` open is
  ignored (HXUI calls it every frame), and Dear ImGui's own error tooltip and asserts are off.
- **Font objects** take Ashita's colour tags (`|cAARRGGBB|` ... `|r`) and a negative position counts from the right /
  bottom edge.
- **Shift + left drag** (issue #86) moves a font or primitive object that is not locked (`locked = false`, the fonts
  library's default): the topmost shown object under the mouse, or its root parent for a child font; the new position
  is absolute, and the addon saves it as it saves any position (reading `position_x` / `position_y` back). This is the
  convention Ashita addons document ("Hold Shift+Click and drag to move display"); the core's own code is not
  available, so the details are INFERRED. Addons with their own drag (equipmon) do it in their `key` / `mouse` events.
  The game's own windows do not move (retail has no such feature).

### Not supported, and why

- **Plugins as Windows DLLs** (`plugins/*.dll`): never loaded. Open-source plugins are rebuilt for Linux from their
  source instead and `/load` runs those (section "Plugins"); `plugin_event` is not raised.
- **Raw game memory beyond the stand-ins.** About a third of the bundled addons patch or read FFXiMain.dll's memory
  through pointers found by `ashita.memory.find` (filters, instantchat, hideparty, drawdistance, mipmap, aspect ...).
  The game's 32-bit code never runs here, so a patch to it changes nothing; those signatures answer 0 and the addons
  report "Failed to locate ..." and stop, as they do in Ashita when a scan fails. The signatures whose objects the
  client can stand in for are answered natively (next section).
- **Direct3D beyond sprites and textures**: the device, D3DX textures and sprites and gdifonts are stood in for (section
  "Direct3D 8 and gdifonts"); vertex buffers, shaders, render states and `DrawPrimitive*` answer `E_NOTIMPL`.
- **Sounds** (`ashita.misc.play_sound`, winmm `PlaySound`, the chime addon's ids): not played; the calls succeed.
- **Other Win32 calls through `ffi.C`** beyond the stand-ins above: the symbol is missing in the Linux process, and the
  addon reports the error (logged once per name).
- **Native Lua modules other than LuaSocket / LuaSec** (any other `.dll` in `package.cpath`).
- **Addons that call game functions they scanned for** (renamer, watchdog, filterscan): there is no game code to call;
  they end the process, so they are left out of the tests.

## Native stand-ins for memory signatures

Addons find game code by byte pattern (`ashita.memory.find`), read the addresses its instructions hold, then read or
write the objects those lead to, often through `ffi.cast`, so the memory has to be real and below 4 GB (they keep
pointers in `uint32_t`). The client owns all that state, so the layer answers the common signatures with memory it
fills itself (src/ashita/scan_registry.{h,cpp}, src/ashita/prelude_scan.lua):

- **The game image.** The user's FFXiMain.dll (`setGameImage`, else `$FFXI_ROOT/FFXiMain.dll`) is mapped as data at its
  own base 0x10000000, its LZSS-packed `.text` unpacked in memory. A pattern is found where the
  game has it, and every address inside the image (code operands, globals in `.data`) reads and writes. Nothing of it
  runs. If the range is taken or there is no DLL, the emulated signatures below still answer, from a copy of their
  bytes whose wildcard operands point at emulated memory.
- **Emulated objects.** Low memory (`MAP_32BIT`) for what the game keeps on its heap: a global the signature leads to is
  pointed at an emulated object, filled from the client every frame (before the frame's tasks) and once at the scan.
- **Stubs.** The game functions addons call (the config addon's get / set / entry / SetAutoOffline) get an x64 jump
  written over their entry, to C++ functions backed by `setConfigHooks` (the client's Config storage). A function whose
  first operand an addon also reads (config's `this`) keeps it, followed by 4 zero bytes.
- **Rules.** `find` answers an emulated signature; any other pattern answers 0 (`XI_ASHITA_SCAN=all` answers every
  pattern found in the image, for experiments; `XI_ASHITA_SCAN=off` restores the earlier emulation, for comparisons).
  `ashita.memory.read_*` / `write_*` / `read_array` / `read_string` outside the image and the emulated memory answer 0 /
  do nothing, so an unchecked failed scan read through them cannot fault (an `ffi.cast` dereference of 0 still does).
  `IPointerManager` answers Ashita's named pointers from the root's `config/ashita/ashita.pointers.ini` and
  `custom.pointers.ini`, and the addons' own `Add`.
- **The client's side** (apps/xi-client/addons.cpp): `setClientUi` once a frame (the top window's layout name, event
  running, interface hidden, log expanded), `setConfigHooks` once.

x64 caveats: an addon's `uint32_t**` (or any pointer type) dereference reads 8 bytes, so a 4-byte game pointer is read
whole only when the 4 bytes after it are zero; the emulated objects and copies keep those zero (ime's operand copies,
config's stub), but a global next to another non-zero global in the image would read as a garbage pointer. Structs an
addon declares with `uintptr_t` members are laid out x64-wide; the config entry is filled in that layout (key +8, value
+12, type +16, min +28, max +32, default +36).

| Signature | Addons (latest / HorizonXI) | In 30260604 | Stands in for | Filled from |
|---|---|---|---|---|
| `8B480C85C974??8B510885D274??3B05` (+0x10) | HXI libs/lin/ffxi.lua: HXUI, XIUI, balloon, fancychat, metrics, rest, skillchains, tCrossBar, thotbar | 0x1015fcf2; g = 0x105eed6c (interface object 0x105eed18 + 0x54) | g -> menu object; +4 -> header; +0x46 the top window's name (16 chars, "menu    map0") | `ClientUi::topMenu` |
| `A0????????84C0741AA1????????85C0741166A1????????663B05????????0F94C0C3` | same | 0x10086f60; g = 0x10487e48 | g (u8) = 1 during an event | `ClientUi::eventActive` |
| `8B4424046A016A0050B9????????E8????????F6D81BC040C3` (+10) | same | 0x10160650; o = 0x105eed18 | o + 0xB4 (u8) = 1 with the interface hidden | `ClientUi::interfaceHidden` |
| `83EC??B9????????E8????????0FBF4C24??84C0` (+4) | tCrossBar, thotbar (IsChatExpanded) | 0x10001d90; o = 0x105eed18 | o + 0xF1 (u8) = the log expanded | `ClientUi::chatExpanded` |
| `8B0D????????8B410C8B49108D04808D04808D04808D04C1C3` (+2) | skillchains, statustimers, tCrossBar, thotbar, ttimers, timers (REALUTCSTAMP) | 0x100dbf90; g = 0x10492e10 | g -> object; +0x0C Unix seconds, +0x10 milliseconds | the system clock |
| `894124E9????????8B46??6A006A00508BCEE8` (+0x19) | skillchains, tCrossBar, thotbar, ttimers | 0x1022427b; o = 0x106343a0 | the ability recast table (8-byte entries, +4 i16 modifier) | left 0: no modifiers (IRecast answers the timers) |
| `C1E1032BC8B0018D????????????B9????????F3A55F5E5B` (+10) | blusets, chains, pupsets, skillchains, tCrossBar, thotbar | 0x100fcad0; disp 0x40c44 | the S2C 0x044 copies' offset in the inventory object | the image (copy: 0x40c44) |
| named pointer `inventory`: `A1????????8D9488????????8990????????E9` (+1) | the same, through `GetPointerManager():Get('inventory')` | 0x100eb485; g = 0x104dfd98 | g -> inventory object; +0x40c44 / +0x40ce0 = S2C 0x044 for the main / support job (BLU SetSpells @+4) | `WorldSnapshot::extendedJob` (session parses 0x044) |
| `A1????????85C05E74????80` (+1) | libs/scaling.lua | 0x1018b57c; g = 0x104568fc | g -> object; +0x10 / +0x12 window, +0x14 / +0x16 menu size (u16) | `Frame` window and menu size |
| `B0015EC390518B4C24088D4424005068` (+0x34) | luashitacast, hgather | 0x100dc59b; g = 0x104ae348 | g + 0x0C = the clock (Vana'diel minutes x 2.4 - 92514960) | the system clock, LSB's epoch |
| `66A1????????663D????72` (+2) | luashitacast, hgather | 0x1021fe18; g = 0x10485fe0 | g (u8) = the weather | `WorldSnapshot::weather` |
| `8B8C24040100008B90????????0BD18990????????8B15????????8B82` (+9, +0x17) | luashitacast, nomad | 0x100fc0c8; offset 0x3c35c, g = 0x104dfd98 | g -> zone object; + offset: 0x100 in a Mog House | `WorldSnapshot::inMogHouse` |
| `A1????????8B88B4000000C1E907F6C101E9` (+1) | luashitacast | | g -> object; +0xB4 (u8) wardrobe flags | 0xFF: every wardrobe (INFERRED for LSB) |
| `8B0D????????85C974??8B44240450E8????????C383C8FFC3` | config, mobdb | 0x10193760; this g = 0x1062fbac | get(id) | `ConfigHooks::get` (stub) |
| `85C974??8B4424088B5424045052E8????????C383C8FFC3` (-6) | config | 0x10193740 | set(id, value) | `ConfigHooks::set` (stub) |
| `8B490485C974108B4424048D14808D04508D0481C2040033C0C20400` | config | 0x10195390 | the FsConfigSubject of an id | `ConfigHooks::entry` (stub) |
| `568B74240885F674??B889888888F7EE` | config | 0x1019c0b0 | SetAutoOffline(value) | `ConfigHooks::autoOffline` (stub) |
| `83EC08A1????????5333DB56` (+0x2F) | ime | | the IME check it patches | a copy takes the writes |
| `8B0D????????81EC0401000053568B` (+2) | ime | | g -> input object (+0xF0EC, +0xF10C) | emulated, zero-padded copy |
| `8B0D????????83EC0C8A51188B410884D256` (+2) | ime | | g -> language object; +8 = the client language | 1 (NA) |

### Native settings: patches become client settings

Many addons patch the game: they write a float into a constant its code reads, NOP a delay, or flip a window's
visible byte. The game's code never runs here, so a patch alone would change nothing; instead each such signature is a
watch on the memory it touches, tied to a client setting (`setNativeHooks`: `supports`, `get`, `set`). The client's
value is written there each frame (so the addon reads what the client shows), and a write by the addon reaches the
client's `set`. A signature answers only when the client supports every setting it stands for; otherwise it answers 0
and the addon says it could not find what it needs, which is true. `XI_ASHITA_NATIVE=all` treats every setting as
supported when the client gives no hooks (for experiments and the smoke test).

| Addon | Signature (operand) | Memory | Setting (value) |
|---|---|---|---|
| drawdistance | `8BC1487408D80D` (+7, +0x0F) | two fmul factors (0x10456ab8 / 0x10456aa8, set at run time) | `draw_distance.world`, `draw_distance.entity` (factor, 1) |
| aspect | `A1????????85C074??D9442404D80D????????D80D` (+1) | g -> camera; +0x2F0 float | `aspect.factor` (h / (0.75 w)) |
| clearcolor | `A1????????0508030000C3` (+1) | g -> D3D window; +0x308 | `clear_colour` (D3DCOLOR) |
| namecolors | `B917000000B880808080BF` (+0x0B) | 23 D3DCOLORs (0x104e1b98) | `name_colour.0` .. `.22` (0x80808080) |
| hideparty | `66C78182000000????C7818C000000????????C781900000` (+0x19, +0x23), `A1????????8B0D????????89442424A1????????33DB89` (+1, +7) | g -> primitive; +8 -> object; +0x69 / +0x6A | `window.party.visible`, `window.target.visible`, `window.alliance1.visible`, `window.alliance2.visible` |
| fps, balloon | `81EC000100003BC174218B0D` (+0x0C) | g -> object; +0x30 u32 | `fps.divisor` (60 / n fps; 2) |
| instantchat | `8BF174??8B46340FBF4E5485C07D??894E34` (+0x1F) | `add eax, 0x14` (83 C0 14) or NOPs | `chat.instant` (1 while NOPed) |
| filters | `C3C74004000000008B0D????????81C1` (+0x0A, offset at +0x10) | g -> object; + offset: 3 u32 | `chat.filter.0` .. `.2` (C2S 0x0DB's words); always a copy, the operand followed by zeros and the offset 0x10000, as filters reads it through `uint32_t**` |
| xicamera | `D8C9D9C0D8C1D9C2D80D????????D9C3DCC0D8EB` (+0x0A), `D9442410D825????????51D80D` (+6), `5152D8442424D905????????D8C1` (+8), `D8C1D8CAD95C2450D805????????D8C9` (+0x0A), `D84C24208B068BCED80D` / `D84C24248B168BCED80D` (+0x0A), `D8C9D99C24DC000000DDD8D9442450D8442428D83D` (+0x15, followed when repointed) | float constants 0x10329d38 = 3, 0x1032a3e8 = 6, 0x1032a3a8 = 7.2, 0x1032a3a4 = 8.6, 0x1032a3ec = 0.0279, 0x1032a3e4 = 0.107 | `camera.distance.min` / `.max`, `camera.battle_distance.min` / `.max`, `camera.pan.horizontal` / `.vertical`, `camera.battle_range`; its operand repoints (zoom set-up, walk animation, battle sound, jitter) answer with `camera.distance.max` |
| balloon | `8B4424046A016A0050B9????????E8????????F6D81BC040C3` (+10) | o + 0xB4 | `interface.hidden` (else it follows `ClientUi`) |
| balloon | `80A0B2000000FBC605????????00` (+9) | u8 | `camera.user_control` |
| macrofix, XIUI | `2B46103BC3????????????68????????B9`, `2B46103BC3????68????????B9` (+5 NOPs; +3 F9 90), `83C41084C074??...3AC3`, `83C41084C074??...807E0C02`, `83C41084C0????...8B461485C0` (two matches) | the palette's code | `macro.instant` (the hold delay NOPed), `window.macro_palette.visible` (0 when XIUI hides it) |
| allmaps | `50??????????83C4048886????????0FBF8E????????8A`, `50??????????83C4048886????????0FBF96????????8A` (+1) | `call` (E8) of the key item test (0x101f7090 -> 0x10097db0) whose result is the map window's "have this map" flag (+0xA46); key item 0x17F skips it. The first is the Map menu's (0x101f9d09), the second the marker / waypoint view's (0x101f98e8); `mov eax, 1` (B8 01 00 00 00) = every map | `map.all`, `map.all.view` (1 while patched) |

The client side (apps/xi-client/addon_native.cpp) supports every setting above except `sound.chime`: the game's chime
ids are 4-character names whose mapping to sound effect ids was not traced. Each defaults to the client's own
behaviour and changes only when an addon writes:

- `draw_distance.world` / `.entity` multiply the zone's / the actors' fog distances (render.cpp, after the
  clipping-plane option);
- `aspect.factor` is the camera's +0x2F0; the projection becomes 1 / (factor x 0.791), as the game computes it
  (gfx::aspectFactorOverride);
- `clear_colour` replaces the frame's clear value (the sky still draws over it, as in the game);
- `name_colour.N` overrides ui::nameColour for the kinds it draws: 0 players (our own name too), 4 NPCs, 5 unclaimed
  monsters, 6 claimed by us / our party, 7 claimed by others, 9 dead (INFERRED from the table's default colours);
- `window.*.visible` hide GameUi's party, target (with its buffs) and macro palette; the alliance windows are not drawn
  yet, so theirs is only kept;
- `fps.divisor` caps the frame loop at 60 / n a second (no cap unless set);
- `chat.filter.N` are S2C 0x0B4's SAVE_CONF words; a change sends C2S 0x0DB as the Config Chat Filters page does;
- the camera names are client::GameCamera::Tuning (the follow band, the lock-on band and cone, the key rates);
- `chat.instant` / `macro.instant` are supported and change nothing: the log and the palette have no delay to remove;
- `map.all` / `map.all.view` show every map without its key item (GameUi::allMaps); our one map window serves the
  menu and the marker view, so either opens it;
- the macro hooks use GameUi's macro books (macroName / macroLine / setMacro, which rewrites the macro file, book and
  set, runMacro, the macro queue).

A code patch's setting returns to what the game's own bytes mean when its addon unloads (the addon's state closes;
allmaps restores its bytes then, but nothing would read them again).

Each change an addon makes is logged ("ashita native: NAME = VALUE"). Seen live in the windowed client (BotA in
West Ronfaure): drawdistance's setworld 0.25 pulling the fog in, hideparty hiding the party and target windows,
xicamera's `/cam d 14` and back, fps 6 giving 10 frames a second and 1 giving 60, namecolors' settings turning monster
names red and ours cyan, macrofix's patch, clearcolor's colour, filters' save / clear / load, the aspect addon
(which reads the window from the boot configuration 0001 / 0002 and, at 16:9, asks for what the client already draws),
and the macro functions XIUI calls answering BotA's macro book (Ctrl 1 "Provoke", Alt 1's line, Alt+1 run) with XIUI
and macrofix loaded.

### The macro palette and other game functions

XIUI's libs/ffxi/macros.lua (and so XIUI itself, which fails to load without it) calls FsMacroController /
FsMacroContainer through ffi: canUseMacro, clearMacro, getName, getLine, runMacro, setBook, setPage, setMacro, and
reads stopMacro's running field. Each gets a stub to the client's macro book (`setMacroHooks`; index 0..19 = the Ctrl
row 1..0 then the Alt row, books and pages 0-based); the container object g -> +4 + 380 x index is emulated, and the
running field shows the client's macro queue (writing -1 stops it). setMacro's 6 x 61-byte block is as long as the
lines the addon gave, so it is read until a block is not text followed by NUL padding.

Other functions: stepdialog / balloon's TkEventMsg2 (its object present while `event.message.waiting`;
OnKeyDown -> `event.message.key`, 5 = Enter), chime's YmSePlayChime (-> `sound.chime`, the 4-character id), and the
C2S 0x102 sender blusets / pupsets call in "safe" mode (BLU, jobType 0x1000: queued and sent the next frame through the
addon packet path with the current set from S2C 0x044; other jobs answer 0).

### The game's clock

The Ashita `ffxi.time` library (fishaid's restock countdown, and any addon that requires it) scans 13 of the game's
time functions and stops with "Failed to locate required pointer(s)" unless all are found (issue #88). Each is now a
stub running the game's own arithmetic (src/ashita/game_time.h, traced in FFXiMain.dll):

| Function | Address | What it computes (t = the game's raw time, Earth seconds since the Vana'diel epoch) |
|---|---|---|
| ntGameTimeGet | 0x10103910 | the clock object's seconds + an offset: 0x00A sets the clock from ntTimeSec (+0x38) and the offset to GameTime (+0x3C) less it (0x100fadd2..0x101038d0), so it is the last zone-in's GameTime plus the seconds since |
| ntGameTimeDiff | 0x10103980 | t - now while now <= t; else t - now - 1 if that is at most 0x7FFFFFFF (unsigned), else 0 |
| ntGameYearGet / MonthGet / DayGet / DayWeekGet | 0x101039b0 / 0x101039c0 / 0x101039e0 / 0x10103a10 | t / 1244160; t % 1244160 / 103680; t % 103680 / 3456; t % 27648 / 3456 |
| ntGameHourGet / MinGet | 0x10103a40 / 0x10103a60 | t % 3456 / 144; t % 144 x 60 / 144 |
| ntGameMoonGet | 0x10103a90 | t / 3456 / 7 % 12 |
| ntGameMoonPerGet | 0x10103ad0 | r = t / 3456 % 84; x = (r - 4) x 200 / 84 (float), +-0.5 and truncated: -x before day 4, x to day 46, 200 - x after |
| xiGetTime | 0x100dbd40 | the CRT's time() |
| sqLocalTime | 0x101c88c0 | localtime; from 0x7FE81780 on, 28-year cycles are taken off the time and added to tm_year |
| YkGetTimeString | 0x101c8b80 | a date and time from string table 10's formats, chosen by the PlayOnline locale object (0x10456a28: +0x18 bit 0 the time format, bits 1-2 the date order; dates 0x3B-0x3D by name, 0x4D-0x4F by number, times 0x3E / 0x3F, 0x40 for hours and minutes (flag 8), placeholders 0x48-0x4C); flag 1 the month by name (0x60 / 0x61 with flag 4), 2 the date only, 0x18 the time only, else "date time" |

The game time comes from the client (WorldSnapshot::gameTime, S2C 0x00A +0x3C, plus the seconds since); before the
first zone-in it is the system clock less 1009810800, which is what LSB sends. INFERRED: the locale (no client setting
traced to the object yet; an English client's month-first date and 12-hour time), and the meaning of string table
10's format codes (0x8B number, 0x8C string, 0x83 + 0x86 / 0x89 compare equal / less with a then-part ending in 0x84,
0x82 choose among 0x8D-separated texts), read from the formats themselves, not from the interpreter (0x101530b0).

### fancychat

fancychat (issue #87) loads and runs. What it needed:

| Signature | Traced | Stand-in |
|---|---|---|
| SeekBattleActor `66A1????????83EC18...` | 0x10077cb0: our party's server ids (ourselves included), then the lowest-index actor with a model whose claim (+0x188) is one of them and that is not dead (+0x170 not 2 / 3) | a block whose CHAR_ID (+0x74 index, +0x78 server id) is that monster from the client's entities, or null |
| GetMonsterName `53568B35...` | 0x1008a810: the zone's name table record (char name[28] + u32 id) whose id's low 10 bits are the index | the client's zone name table (dat::loadEntityNames) |
| ntSysGetLastTeller `8B0D????????85C97503...` | 0x100d9a30: the newest of 8 16-byte teller names (count at +0x19e), or null | the last tell's sender (WorldSnapshot::lastTellFrom) |
| gcTrackingPosGet `A1????????85C074??F7` | 0x100f9400: the zone's tracking block (+0x3ae98) while State 1 with an index and level, else null | null: the client has no wide scan tracking yet |
| draw message windows `A1????????C6405901...` (+1, +0x0B) | 0x1012df6d: two globals (0x105eee1c / 0x105eee20) -> the log objects; +0x34 the shown height in pixels (lines x +0x54), +0x4e / +0x50 max / min lines, +0x42 the text top (ui-format-notes.md §10.2 "The log objects") | the client's log windows: their shown height, line limits and text top each frame; an addon's write of +0x34 (fancychat clears it every frame) is the height the client draws while the addon keeps writing, released when it unloads (issue #91) |
| `A1????????3BF07E??8BF0` (+1), `BF????????F3??0FBF4C24` (+1) | the D3DVIEWPORT8 at 0x105eede4 and its copy at 0x105eedfc (X, Y, Width, Height, MinZ, MaxZ) | Width / Height = the window's size each frame |
| `A0????????53565784C08BF1` (+1) | 0x1047d3ec: 1 while an event message window is up (set 0x100b274d, cleared 0x100ae2f1) | ClientUi::eventMessage (the cutscene's waiting message) |
| `8935????????81C6????????56E8` (+2 &zone, +8 offset 0x4638) | zone + 0x4638: the network window's counters, +0 bytes a second sent, +4 received (0x100fca10, 3 s) | WorldSnapshot::netSendRate / netRecvRate |

Two of its scans answer 0 as they do in Ashita on this DLL: `B9????????50E8????????8BF085F674??8B46` with count 1 (the
pattern matches once) and `E8????????84C075??A1????????85C074??668378` (it reads +0x23 into a value it never uses).
Also for fancychat: `imgui.GetIO()` gives DisplaySize / MousePos with .x / .y as well as [1] / [2], and KeysDown /
KeysData (`.Down`) indexed by Windows virtual-key code; its gdifonttexture build's `SetFontFolder` (the folder's TTF /
OTF files by family and style, searched before fontconfig: Consolas, its icon font) and `CreateTextureColor` (text runs
coloured by `\\§AARRGGBBç\\`, `\\§--------ç\\` back to FontColor; INFERRED from fancychat's own text, not the DLL).

Still answering 0: fancychat's open-window list and menu-description pointer (as in Ashita, above), XIUI's castcost list-box cursor
(KaListBox), mobtracker, watchdog's entity name table, mipmap, instantah, fastswap, trimspells, quicksets, filterless,
noname, paranormal, cartographer, mapdot, nomad's entity filter, ahgo / ahcolors, nolock, PUP's 0x102.
Altogether the two addon sets use 128 distinct patterns; 85 of them now have a stand-in (allmaps' two since #85, the clock's 13 since #88, fancychat's 9 since #87).

Smoke test (one process per addon, both roots, the same build): before = `XI_ASHITA_SCAN=off`; after = the stand-ins
with no client hooks (what the smoke test has); all = `XI_ASHITA_NATIVE=all`, as if the client supported every setting
and the macro palette.

| | latest: before → after → all | HorizonXI: before → after → all |
|---|---|---|
| Crashed | 6 (config, ime, crosshair, filterscan, renamer, watchdog) → 4 → 4 | 7 (… bard, targetlines) → 5 → 5 |
| Clean (no error or stub) | 46 → 49 → 59 | 41 → 46 → 52 |
| With errors | 32 → 30 → 20 | 38 → 36 → 30 |
| Failed to load | 2 (chime, stepdialog) → 2 → 0 | 16 → 16 → 16 |

config and ime no longer crash; the remaining crashes come before or apart from memory scans (d3d8 device / textures).
With every setting supported, chime and stepdialog load, and XIUI gets past its macro library (it then stops at
`libwinmm.so`, its sound module); HXUI, tCrossBar and ttimers stop at `gdifonttexture.dll` before any scan.
The headless tests (tests/test_ashita_scan.cpp) read every emulated signature the way its addons do, through
`ashita.memory` and `ffi`, with and without the game image; they call the config, macro, event message, chime and
0x102 functions through the stubs, and check the patch addons' writes reach the client's settings and back.

## Direct3D 8 and gdifonts

`src/ashita/d3d8_emu.{h,cpp}` and `src/ashita/prelude_d3d.lua` stand in for what the bundled addons draw with:
HorizonXI's HXUI, partybuffs, mobdb, points, statustimers (icons), fancychat, tCrossBar, ttimers and zonename.

**What they use** (read from HorizonXI 4.3.1.2's addons and `libs/d3d8`, the same in 4.3.2.1):

| Piece | Calls | Users |
|---|---|---|
| `AshitaCore:GetDirect3DDevice()` through `libs/d3d8.lua` `get_device()` | the device pointer, passed back to D3DX | all of them |
| `ffi.C.D3DXCreateSprite`; `ID3DXSprite` `Begin` / `Draw` / `End` (`DrawTransform` too) | quads of a texture's rectangle, scaled, rotated about a centre, translated, tinted | partybuffs, mobdb, points, every gdifonts user |
| `ffi.C.D3DXCreateTextureFromFileA` / `...ExA` / `...InMemory` / `...InMemoryEx` | PNG / JPEG from addon folders; the resource manager's status / item icons as BMP in memory, colour key `0xFF000000` (size -1 in ttimers) | HXUI, tCrossBar, ttimers, statustimers, mobdb, fancychat |
| `IDirect3DTexture8` `GetLevelDesc` / `LockRect` / `UnlockRect` / `Release` | the size; pixels | tCrossBar, ttimers, gdifonts |
| `imgui.Image` / `ImageWithBg` / `ImageButton`, draw list `AddImage` / `AddImageRounded` / `AddImageQuad` | the texture as `tonumber(ffi.cast('uint32_t', tex))`, or the pointer | HXUI (24 places), statustimers, tCrossBar, mobdb, fancychat |
| `ffi.load('.../gdifonts/gdifonttexture.dll')` | `CreateFontManager`, `CreateTexture` (`GdiFontData_t`: box, em height, outline, flags, colours, gradient, family, UTF-8 text), `CreateRectTexture` (`GdiRectData_t`), `GetFontAvailable`, `Enable/DisableTextureDump` | HXUI, fancychat, tCrossBar, ttimers, zonename (Thorny's gdifonts) |
| `ffi.load('user32')` / `('kernel32')`, `ffi.C.GetKeyState` | clipboard, foreground window / process id, `GlobalAlloc` | fancychat, statustimers |

**How it is done here:**

- **Real COM objects.** The addons call methods through `lpVtbl` from LuaJIT, so the device, textures and sprites are C
  structures with vtables of C functions, in the slot order `libs/d3d8/interfaces/*.lua` and `d3dx8.lua` declare
  (device: `GetDisplayMode` 8, `CreateTexture` 20, `BeginScene` / `EndScene` 34 / 35, `GetViewport` 41; texture:
  `GetLevelDesc` 14, `LockRect` 16, `UnlockRect` 17; sprite: `Begin` 4, `Draw` 5, `DrawTransform` 6, `End` 7). Other
  slots answer `E_NOTIMPL`.
- **Below 4 GiB.** Addons hand a texture to ImGui as `tonumber(ffi.cast('uint32_t', tex))`, a 32-bit pointer in
  Ashita. Every object here lives in `mmap(MAP_32BIT)` memory, so that number is still the object's address and the
  ImGui functions find the texture by it.
- **Windows `long`.** Ashita's LuaJIT is 32-bit Windows (LLP64): `libs/win32types.lua` makes `DWORD` / `LONG` `unsigned
  long` / `long`. On Linux these are 64 bits, which would move every field of `RECT`, `D3DSURFACE_DESC`,
  `D3DVIEWPORT8` and `D3DCOLOR`. `prelude_d3d.lua`'s `ffi.cdef` reads `long` as `int32_t` and `unsigned long` as
  `uint32_t` (`long long` is kept), for every addon. Pointers stay 64-bit, so `D3DLOCKED_RECT` and
  `GdiFontReturn_t` have x64 layouts on both sides.
- **Textures** are ImGui 1.92 user textures (`RegisterUserTexture`), created when first drawn, updated after
  `UnlockRect`, and released through `WantDestroyNextFrame` once ImGui's Vulkan backend is done with them (a sweep
  before each addon's draw). Images decode with stb_image (PNG, JPEG, BMP, TGA, GIF), keep the file's size (not
  rounded to powers of two; INFERRED from how the addons size their draws), and the colour key turns matching opaque
  pixels transparent. File paths are Windows paths (`\`, any case) resolved against the real files.
- **Sprites** queue quads at `Draw` and put them on ImGui's foreground draw list at `End`, in order: above the game's
  interface and above ImGui windows, as Ashita's `d3d_present` drawing ends up over the game. `Draw` follows
  `D3DXMatrixTransformation2D(NULL, 0, scaling, centre, rotation, translation)`: scale, rotate about the centre in
  scaled space, translate. Colours are `D3DCOLOR` tints.
- **gdifonttexture.dll** (`~/git/gdifonttexture`'s `GdiFontManager.cpp` gives the semantics): text is laid out as GDI+
  `AddString` does it. The em height is `FontHeight`, the baseline is at the font's OS/2 `usWinAscent`, lines are
  `BoxWidth` wide and wrap at spaces, and `\n` breaks. The outline is a pen `OutlineWidth` wide, half outside and half
  inside, drawn first; the fill (solid, or one of the 8 gradient directions) goes on top. Everything is rasterised at
  2x with stb_truetype on the face fontconfig picks for the family (Arial -> Liberation Sans, Consolas -> Noto Sans
  Mono), then box-filtered down. The texture is trimmed to the columns used, and from the layout's top to the last row
  used, as the DLL scans its canvas. Rounded rectangles are filled, then stroked, with 4x4 samples a pixel.
  `GetFontAvailable` answers true for any family fontconfig resolves, because a substitute is what gets drawn.
  Texture dumps are not written. Underline and strikeout positions are INFERRED (0.12 em below / 0.28 em above the
  baseline).

**Seen live** (windowed client, BotC in Southern San d'Oria, HorizonXI profile):

- **HXUI:**
  - Its patch-notes window drew its D3D images: the HXUI logo and the "PATCH 1.1 RELEASE" badge, through
    `imgui.Image`.
  - With two prelude gaps patched in a scratch copy (`player.isZoning` read through the interface stub,
    `table.pack`), its HUD drew: player bar, EXP bar (gradient textures from `libs/bitmap` BMPs plus bookend PNGs),
    inventory dots, and gdifonts numbers and labels over the bars ("31", "24", "WHM 1 / NON 0", "EXP (0 / 500)").
    The windows sit at ImGui's first-use spot until moved, as on a first Ashita run.
- **tCrossBar:** its crossbar drew: sprite slots and the controller glyph PNGs.
- **statustimers:** its configuration window opened (`/stt`).
- **Scratch probe addon:**
  - five status icons from the resource manager went through `D3DXCreateTextureFromFileInMemoryEx` with the black key
    (32 x 32 each), then were drawn both by `imgui.Image` and by an `ID3DXSprite` at 2x;
  - HXUI's `hxui.png` loaded by a Windows path;
  - HorizonXI's own gdifonts drew a gradient, outlined text and an outlined rounded rectangle with text over it.

## Where addons live (issue #73)

The Ashita root defaults to the game folder (`findGameDir()`: normally the imported copy,
`~/.local/share/ffxi-linux/game`). On start the client creates `addons/`, `addons/libs/`, `config/addons/` and
`scripts/` there (src/xi/ashita_dir.*), so addon folders dropped into `addons/` show up on the Add-ons page with no
configuration. `XI_ASHITA_DIR`, then client.ini `ashita_dir`, override it; `off` (or `none`) in either turns addons off.
The bundled set (#138, `third_party/ashita-addons`: the standard Ashita v4 addons and libraries at Ashita-v4beta
877d9e7, minus the 37 its README lists: Windows-only, built into the client, crashing it, or doing nothing in it) is
installed
there on start by `xi::ashitadir::installBundled`: library files and addon folders the root doesn't have yet, never
overwriting; `addons/.bundled` remembers the addons it added, so one the user deletes is not added back. `xi-setup
addons` (no folder) does the same; the bundle is found through `XI_ASHITA_BUNDLE`, `<exe>/../share/ffxi-linux/
ashita-addons`, the install prefix, then the source tree.
Ashita's shared libraries (`addons/libs`: settings, imgui, common, ffxi, d3d8, gdifonts ...) can instead be copied from an install
the user has by `xi-setup addons FROM|--install [--to DIR] [--list] [--addon NAME]...` (HorizonXI's Game folder, Ashita v4
folders in Wine prefixes under ~/.wine and ~/Games, or Ashita-v4* source checkouts; changed library files are updated,
addons the user already has are never overwritten), or by the launcher's Tools page. Without them the Add-ons page
and the log say how to get them. No third-party addon other than the bundled set is committed (the CI's
forbidden-file check accepts only the files `third_party/ashita-addons/MANIFEST.sha256` lists, unchanged).

**Window positions (issue #111).** ImGui keeps every window's position, size and collapsed state in
`<root>/config/imgui.ini`, the file Ashita v4 writes (HorizonXI's `Game/config/imgui.ini` has the same format, so it can
be copied over). Addons such as mobdb never store their own window position: they rely on this file, so a window the user
dragged opens there again next time. ImGui writes the file a few seconds after a window moves and when the client exits;
src/ashita/imgui_ini.* points ImGui at it when the addons start (and at the new root if the Ashita release is switched).
With addons off nothing is saved. The client's own ImGui windows (debug overlays) share the file, as they would in
Ashita.

## Managing addons in game (goal 47)

Config -> Add-ons lists the addons, loads / unloads / reloads them, keeps a per-character startup list, edits the
settings lib's files and switches the release the layer answers as (Auto / Latest / HorizonXI). Details:
ui-format-notes.md §8.1. Addon headers, the startup list and settings files: src/ashita/addon_info.*.

## Plugins (issue #34)

Ashita plugins are C++ DLLs built against `plugins/sdk` (`IAshitaCore`, interface 4.30, the same headers in 4.3.2.1 and
HorizonXI's 4.3.1.2). Checked 2026-11-03.

**Decision (the user's, 2026-11-03): plugins are rebuilt for Linux from their source, against the SDK headers and the
client's own `IAshitaCore`. Windows DLLs are not loaded.** Plugins whose source is not published get a native
replacement in the client or an addon, never a decompiled copy. Shorthand was rebuilt first and runs in the client.

### What the releases ship and what players load

- **Ashita 4.3.2.1** (upstream `plugins/`): addons, hardwaremouse, minimap, screenshot, thirdparty, toon, winefix.
  Its `polplugins/`: extraslots, quicky, sandbox.
- **HorizonXI 4.3.1.2** (`Game/plugins/`): the same plus Deeps, FindAll, LegacyAC, Nameplate, PacketFlow, Sequencer,
  Shorthand, Stylist and truefps. Its `polplugins/` add pivot (XIPivot).
- **HorizonXI's `scripts/default.txt`** loads addons, screenshot, HardwareMouse, Minimap, Nameplate, PacketFlow,
  sequencer, thirdparty, truefps and shorthand.

Authors and sources: the DLLs' version strings, the plugins' GitHub repositories (license as GitHub reports it).
"Closed" means no source is published.

| Plugin | What it does | Source | Covered in the client | Needed | Approach |
|---|---|---|---|---|---|
| addons | runs Lua addons | closed (atom0s) | the Lua layer itself | - | done (goal 31) |
| **Shorthand** | relaxed `/ma`, `/ja`, `/ws`, `/item`, `/ta` names and targets, `//name` | GPL-3.0 (ThornyFFXI/Shorthand) | **rebuilt, runs** | yes | done (native build) |
| screenshot | screenshot key, JPEG / PNG | closed | the client's own (Print Screen, keymap `Screenshot`) | no | - |
| HardwareMouse | game mouse through graphics proxies | closed | not needed: SDL draws the cursor | no | - |
| winefix | Wine fixes for the Windows client | closed | not needed: there is no Wine | no | - |
| truefps | game speed at high frame rates | MIT (SQLCommit/TrueFPS) | not needed: the client's timing does not depend on frame rate | no | - |
| Nameplate | fixes retail nameplate drawing | GPL-3.0 (Shirk/Nameplate) | not needed: the client draws its own names | no | - |
| PacketFlow | less waiting in C2S packet output | binaries only | the client's own send loop (400 ms like retail, docs/lsb-packet-timing.md) | maybe | native option |
| Sequencer | stops gear-swap addons' actions registering twice | no license | INFERRED covered: the session numbers every packet it sends, injected ones too | no | - |
| Minimap | minimap overlay | closed | no (only the map window and the compass) | yes | native feature |
| thirdparty | lets outside programs send commands / keys (memory-mapped files) | closed | no | maybe | native feature |
| Deeps | damage meter | GPL-3.0 (davisdane2/Deeps-ashitav4beta, from relliko/Deeps) | no | yes | native build |
| FindAll | search every character's storage; full inventory on zone-in | MIT (ThornyFFXI/FindAll) | no | yes | native build |
| Stylist | override how characters look; stop gear "blinking" | GPL-3.0 (ThornyFFXI/Stylist) | no | maybe | native build |
| LegacyAC | Ashitacast v3 (XML gear swaps) | binaries only | LuAshitacast (an addon) runs instead | low | addon |
| toon | cel shading on characters | closed | no | low | native feature |
| Multisend, Lootwhore, Packer, Bellhop, Crossbar | multibox commands, auto-lot, inventory sorting, porter moogle, controller macros (not bundled) | GPL-3.0 (ThornyFFXI) | no | low | native build |
| sandbox (POL) | redirects the game's files and registry | closed | not needed: the client reads its own game folder | no | - |
| quicky (POL) | quicker PlayOnline login | closed | not needed: the client logs in directly | no | - |
| extraslots (POL) | more character slots in the lobby | closed | the client's lobby lists what the server sends | no | - |
| pivot (POL) | XIPivot DAT overlays | source published (Shirk/XIPivot), no license | partly: `--hd-textures` (graphics-guide.md) | maybe | native feature |

### Ways to run them, and why this one

- **Loading the Windows DLLs in-process** (a PE loader with Win32 / D3D8 shims). The DLLs are 32-bit x86 (PE32), and
  the client is a 64-bit Linux process, so they cannot run in it. That would take a separate 32-bit process with a
  Wine-sized Win32 layer. Most of them also patch or hook FFXiMain.dll's code and the Direct3D 8 device, and neither
  exists here. Rejected.
- **Rebuilding open-source plugins against the SDK headers** (chosen). Plugin and client are compiled from the same
  headers, so they agree on every class layout. A plugin uses only the interfaces it asks for, so the client answers
  those from its own state, as the Lua layer does. Third-party source stays where the user cloned it.
- **Reimplementing** what has no source (Minimap, thirdparty, toon) as client features or addons.

### Building a plugin

1. `xi-setup addons` copies the SDK (`plugins/sdk` of the Ashita or HorizonXI install) into the game folder. Copying
   it there by hand also works.
2. The build also needs Wine's Win32 headers (`/usr/include/wine/windows`, the `wine` package). Only their types and
   macros are used; nothing of Wine runs.
3. Configure with the plugin's source folder:
   `cmake -B build -DXI_ASHITA_PLUGIN_SOURCES=/path/to/Shorthand` (a `;` list builds several). Then build as usual
   (`tools/build.sh build`). `XI_ASHITA_SDK` names another SDK folder.
4. The plugin lands in `build/plugins/shorthand.so`. `/load shorthand` finds it there or in `<ashita root>/plugins`
   (any case). A boot script's `/load` line loads it too.

Without the SDK or Wine's headers, the client builds without plugin support and `/load` says so. This is the case
in CI: its image has no Wine headers.

At configure time, `tools/ashita_sdk_port.py` copies the SDK into the build folder and prepares it:
- it drops the DOS end-of-file bytes;
- it turns off the size checks on the game's 32-bit structures (pointers are 8 bytes here);
- it generates `xi_sdk_stubs.h`, which has a do-nothing implementation of every SDK interface.

It copies each plugin's source the same way:
- Windows path separators in `#include` lines and in path strings become `/`;
- `#include` names are matched to the files' real case;
- a sibling `common/` folder (Thorny's plugins share one) comes along.

`src/ashita/plugins/` holds the rest:
- `compat.h`, force-included, supplies the MSVC extensions.
- `win32.cpp` implements the few Win32 functions plugins call, and `exports.list` exports them from the client.
- `alloc.cpp` gives every plugin an `operator new` that zero-fills memory. Plugins written for MSVC count on that:
  Shorthand's `SettingsHelper` tests a pointer it never sets. Plugins link with `-Bsymbolic`, so this
  `operator new` applies only inside the plugin.

### What the client's IAshitaCore answers

`src/ashita/plugin_host.cpp` answers from the same per-frame state as the Lua layer, with the same conventions
(prelude_memory.lua):
- **Memory:** IEntity (positions, names, server ids, looks, render and spawn flags, distance squared, and a
  `GetRawEntity` record filled from those), IParty (slots 0-17), IPlayer (jobs, buffs, known spells, abilities and
  weapon skills), ITarget (main target and sub-target), IInventory (container items, sizes, equipment).
- **Resources:** items, spells and abilities from the game data. Items are built on demand, and the last 256 stay
  valid: an IItem is 2.7 KB with its icon.
- **Chat:** `Write`, `Writef` and `AddChatMessage` go to the log. `QueueCommand` runs the line with the mode the
  plugin gave, so Shorthand's mode-0 lines are not seen as typed again. `ParseAutoTranslate` copies the text.
- **Packets:** `AddOutgoingPacket` and `AddIncomingPacket`.
- **Plugin manager:** answers about the loaded plugins and always has "addons".
- **Install path:** the Ashita root, with a trailing `/`.
- **Everything else** answers zero or nothing: fonts, primitives, ImGui, configuration, pointers, offsets, Direct3D
  and the `Direct3D*` callbacks.

How plugins see the client:
- **Order:** plugins see commands and packets before the Lua addons (INFERRED: Ashita's addons are themselves a
  plugin). Among themselves they run lowest priority value first.
- **Editing commands:** a plugin may rewrite a command line in place. Shorthand strips its `/raw ` prefix, and the
  game then runs the new text.
- **Server ids as targets:** action commands and `/target` take a server id as their target
  (`/ma "Cure" 16793618`), which is what Shorthand queues once it has picked a target (INFERRED to be retail's).
- **Scanning FFXiMain.dll:** plugins that scan the game's code get a small stand-in image instead, kept below 2 GB
  because they store pointers in 32-bit DWORDs. It holds the signatures the supported plugins look for: Shorthand's
  wardrobe flags, kept current from the container sizes. A plugin whose `Initialize` fails on a missing signature
  reports "failed to initialize".

### Shorthand in the client (2026-11-03)

Tested live as BotC (WHM) in Southern San d'Oria:
- `/load shorthand` wrote its `config/Shorthand/default.xml` and loaded.
- `/ma cure botthree` cast Cure on BotC: a lower-case name, no quotes, and a full name as the target.
- `/ma cure <me>` did the same.
- `/ta vaque` targeted the NPC Vaquelage (a partial name).
- `//cure` with that NPC targeted made Shorthand send Cure at the NPC. Its own target check accepts friendly NPCs for
  Cure, and the server answered "You cannot attack that target".

Not checked:
- `/shh impact` and the other spell unlocks edit the game's spell memory through a pointer the client does not
  answer, so they do nothing. The settings are kept.
- `packetws`.
- Gear checks for spells that need gear (Honor March and the like).

### Remaining plugins by priority, one follow-up each

1. **Minimap**: a native minimap overlay built from the zone map data and markers the client already has (map window
   §6.5, compass). It is closed source and loaded by HorizonXI's boot script.
2. **Deeps**: rebuild the GPL fork. The host needs IGuiManager (ImGui through the client's context) and IFontManager
   (the Lua layer's gdifonts / font objects).
3. **FindAll**: rebuild. Needs IGuiManager or its chat-log mode. Its "full inventory on zone-in" memory patch is not
   needed if the session already holds every container.
4. **Stylist**: rebuild. Needs writes to `GetRawEntity(...)->Look` carried back into the drawn entity, for example as
   an injected look update that plugins do not see again.
5. **thirdparty**: a native command input for outside programs (a local socket or FIFO), replacing the
   memory-mapped files. Check first which tools users actually drive this way.
6. **Plugin events and text events in the host**: `RaiseEvent` between plugins and addons, `HandleIncomingText` /
   `HandleOutgoingText` (the `UseText` flag), and the `Direct3DPresent` frame callback. Most other rebuilds need
   these.
7. **CI coverage of the plugin host**: add Wine's headers (or a minimal Win32 type header written for this) to the CI
   image, so the host and `tests/test_ashita_plugins.cpp` build and run in the pipeline.
8. **PacketFlow**: decide whether a faster-send option is wanted. The client sends like retail now.
9. **pivot (XIPivot overlays)**: read XIPivot's overlay folders as a DAT override, next to `--hd-textures`.
10. **toon**: a cel-shading option in the renderer.
11. **Thorny's other plugins** (Multisend, Lootwhore, Packer, Bellhop, Crossbar): rebuild on request. Multisend
    needs shared memory between clients, and Crossbar needs Direct3D drawing.
12. **LegacyAC**: none. LuAshitacast covers the users. Revisit only if its source appears.

## Tests

`tests/test_ashita.cpp` drives `AddonManager` headless with a fake host and hand-written test addons in a temp root:
struct.pack / unpack, ashita.bits, command blocking and an erroring addon next to a working one, text_in / text_out,
packet_in changes through `data_modified` and `data_modified_raw` and blocking, chained addons, outgoing / injected
packets, tasks and `coroutine.sleep` / `sleepf`, `/addon load|list|reload|unload`, the boot script, both profiles'
version strings and detection, files and ini configuration, stub warnings, the settings library round trip (when
Ashita's `addons/libs` are found: `XI_ASHITA_LIBS`, else `~/git/Ashita-v4beta/addons/libs`; skipped otherwise), and
`d3d_present` drawing an ImGui window (headless context with dynamic textures).

`tests/test_ashita_plugins.cpp` (built only with the plugin SDK) loads `tests/ashita_plugin_xitest.cpp`, a small plugin
built like a real one. It checks the following:
- `/load` in any case, and `/unload`;
- a command answered through IParty / IResourceManager / IChatManager;
- a queued line keeping its mode and coming back as injected;
- in-place rewriting (`/raw`);
- incoming packet changes and outgoing blocking;
- the boot script's `/load` lines;
- a plugin that has no native build.

`tests/test_ashita_d3d.cpp` covers the Direct3D stand-ins:
- textures live below 4 GiB and release cleanly;
- D3DX textures from a file (Windows path, other case) and from memory (size -1, colour key);
- `GetLevelDesc`, `LockRect` / `UnlockRect` and the 32-bit `long` layouts;
- sprite quads scaled, translated and turned;
- `imgui.Image` / `ImageWithBg` / `ImageButton` / draw-list images becoming ImGui user textures;
- HorizonXI's own gdifonts rendering outlined text and a rounded rectangle, and drawing them at `d3d_present`. This last
  test needs the install: `XI_HXI_ADDONS`, else a Wine prefix under `~/Games` holding a HorizonXI install, linked into a temp root; it skips
  otherwise.

### Smoke test of real addons

The hidden test `"[.smoke]"` loads every addon of an Ashita root on its own, runs six frames with ImGui and
`d3d_present`, sends it `/<name>`, `/<name> help`, an incoming 0x00D, an outgoing 0x015, a log line and a `text_out`,
unloads it, and writes one TSV row per finding (addon, loaded, kind, detail):

```
XI_ASHITA_SMOKE_ROOT=/path/to/root XI_ASHITA_SMOKE_PROFILE=latest XI_ASHITA_SMOKE_OUT=out.tsv build/xi_tests "[.smoke]"
```

(`XI_ASHITA_SMOKE_ONLY=name` runs one addon.)

Use a copy of the addons (they write their settings under the root's `config/`), and one process per addon, because an
addon that dereferences a failed memory scan ends the process. Results on 2026-10-20 (branch 9a1320f + tests):

| Root | Addons | No error | Stubs only | Errors | Failed to load | Crashed |
|---|---|---|---|---|---|---|
| Latest 4.3.2.1 | 81 | 40 | 1 | 40 | 16 | 2 (config, ime) |
| HorizonXI 4.3.1.2 | 87 (+7 case-only main files) | 39 | 6 | 42 | 27 | 2 (config, ime) |

"No error" means it loaded, ran and unloaded without a Lua error or a stubbed call in those frames; it says nothing about
whether its feature works without a game process. luashitacast loads clean.

### Load errors by cause (the layer's to-do list)

| Cause | Addons | Fix |
|---|---|---|
| `ffi.cdef` integer suffixes: x64 LuaJIT rejects `0x8000FFFFL`, `10UL`, `10LL` ("malformed number"; libs/win32types.lua) | latest 14, HorizonXI 18: activemon, affinity, autologin, chamcham, chime, crosshair, equipmon, filterscan, hideobs, move, namecolors, renamer, stepdialog, watchdog; bard, fancychat, hgather, hxiclam, mobdb, mobtracker, partybuffs, points, statustimers, targetlines, ttimers, zonename and two socket addons | wrap `ffi.cdef` (and `ffi.typeof` / `ffi.new` strings) to strip `L`/`UL`/`LL` suffixes from integer literals (Ashita's 32-bit LuaJIT accepts them) |
| Addon folder / main file looked up case-sensitively (`load()` lower-cases the name) | HXUI, XIUI, tCrossBar; Audible, Deathtoll-Lite, balloon, rWarnings, skillchains, thotbar, xiui | find the folder and `<name>.lua` case-insensitively, as on Windows |
| Failed scans dereferenced through `ffi.cast` → SIGSEGV | config, ime | done for these two: their signatures are emulated ("Native stand-ins for memory signatures"); a SIGSEGV guard around addon calls would still help an unchecked addon |
| Global `debug` replaced by an addon; the prelude's `xpcall(cb, debug.traceback)` then fails on every event | parse | capture `debug.traceback` in a local at prelude start |
| `ashita.memory.read_*` return nil for addresses outside the emulated memory | xicamera (arithmetic on nil) | done: reads outside the image and the emulated memory answer 0 |
| Missing aliases `ashita.time.qpc` / `qpf` (and `glt`, `gst`, `tick`; `fs.current_dir`, `equals`, `get_install_dir`; `memory.read_uint` / `read_int` / `write_uint` / `write_int`) | repeater; read_uint by 42 addons | add them |
| `require('lib\\constants')`: backslashes in module names | simplelog | map `\` to `/` in the module searcher |
| ImGui: `ShowStyleEditor`, `GetStyleColorVec4`; `GetStyle()` lacks the ImVec2 fields | imguistyle, rsvp | bind them |
| `socket.core` (luasocket's native module) | metrics, retroach | allow the system's Lua 5.1 luasocket in `package.cpath`, or leave unsupported |
| Win32 via `ffi.C` | freemem (`SetProcessWorkingSetSize`), dynamic_entity_renamer (`GetModuleHandleA`) | unsupported (or no-op stubs for a few kernel32 calls) |
| `module 'imguidef' not found` | gearinfo | the addon's own missing file; not the layer |
| Addon bugs that Ashita shows too (`chat` used without `require`, `cmd:any` on nil args) | invisible, campbuddy | none |

Every row above that names a fix is done (2026-10-20), as are the two smaller issues the unit tests found (the first
chat line now reaches text_in: the manager keeps a "primed" flag; a failed load's message is flushed at once). Lines an
addon writes to the log reach text_in marked `injected`, and an addon's own error report does not reach text_in at all
(simplelog's text_in error had looped through it).

### Live in the windowed client: the bundled set (2026-11-07)

The bundled set (#138: 72 upstream addons and dynamic_entity_renamer) was loaded in the windowed client from a copy
of `third_party/ashita-addons` (`XI_ASHITA_DIR`, `XI_ASHITA_LOAD`), 15 at a time, on main 81a2d49: BotC zoned
in to Southern San d'Oria, waited 25 s, picked two targets with Tab, ran `/addon list`, then logged out, about 90 s
per batch. The three addons known to crash (renamer, watchdog, filterscan) were left out first.

| Run | Crashed | Loaded and ran with no error | Loaded; reported an error | Failed to load |
|---|---|---|---|---|
| 73 | 0 | 59 | 13 (ahcolors, ahgo, cartographer, fastswap, filterless, instantah, mapdot, mipmap, nomad, noname, paranormal, quicksets, trimspells: "Failed to locate ... pointer") | 1 (chime) |

crosshair, which crashed in the 2026-10-20 headless smoke test, and dynamic_entity_renamer both loaded, ran through
the zone-in, targeting and logout, and logged nothing. Several batches had to be retried because the map server
didn't answer the zone-in (`no reply to 0x00A login`, lobby 201, the known LSB watchdog restarts). A run with no
addons failed the same way, so the server caused it, not an addon.

**What each does in the client (the audit after the run).** A clean run does not prove the feature works, so each
addon that ran clean was read against the layer (prelude, addon_native.cpp, the tables above). An addon stays only if
its main effect comes through a supported path; 28 went (the bundle's README gives each reason):

- The 13 "Failed to locate ... pointer" addons and chime: they stop at load and do nothing.
- Writes the client never reads: casper, truesight (entity render flags), skeletonkey (an entity's status), chamcham
  (model colours in actor memory). `GetEntity` hands out a fresh table every frame, and `IEntity:Set*` are no-ops.
- No-ops here: cleancs, hideui (the managers' `SetVisible` does nothing); chatmon (addon sounds are not played);
  instantchat, macrofix (the log and palette have no delay); minimapmon (no Minimap plugin); move, hideobs (Win32
  window calls).
- enternity edits the log line, but the dialogue box waits on the event, not on the log text (not checked live).
- chatfix is harmful with LSB: it shifts chat packet 0x017 by a byte for an old server layout, cutting messages.

Kept, by path:
- Native settings: fps, aspect, allmaps, hideparty, drawdistance, clearcolor, namecolors, filters, config (Config
  values).
- Packets in or out, read or changed and parsed by the client: actionparse, autojoin, autorespond, blucheck, blumon,
  blusets, cfhblock, checker, craftmon, debuff, gateway (door animation 0x1F), nokb (knockback in 0x028), peekaboo,
  sexchange, singlerace, logincmd, petinfo, recast.
- Text: changecall, onevent, stfu, tokens, logs, links.
- Drawing (ImGui, fonts, primitives, textures): activemon, clock, crosshair, distance, equipmon, imguistyle, invmon,
  itemwatch, tparty.
- Commands: repeater, and stepdialog (the TkEventMsg2 stand-in).

dynamic_entity_renamer did nothing here: it renames through `IEntity:SetName`, which is a no-op. The client now does
its job natively (docs/protocol-map.md 7.6: LSB's spawn-name layouts and HorizonXI's 0x1FF list), so it was dropped
from the bundle, which ships 44 upstream addons.

### Live in the windowed client (2026-10-20)

Every addon of both releases was loaded in the windowed client, 15 at a time, with BotC logged in to Southern
San d'Oria (target picked with Tab, `/addon list`, a screenshot, logout); the logs give the table below. Three addons that
call game functions they scanned for (renamer, watchdog, filterscan) end the process and were left out.

| Release | Run | Loaded and ran with no error | Ran; only stubbed calls logged | Loaded; reported an error | Failed to load |
|---|---|---|---|---|---|
| Latest 4.3.2.1 | 78 | 61 | 2 (autologin, blusets) | 14 | 1 (chime) |
| HorizonXI 4.3.1.2 | 85 | 68 | 8 (HXUI, XIUI, blusets, fishaid, mobtracker, nolock, points, statustimers) | 6 | 3 (fancychat, gearinfo, yield) |

- **The errors** are, all but one, "Failed to locate ... pointer": addons that patch the game's code (ahcolors, ahgo,
  cartographer, fastswap, filterless, instantah, mapdot, mipmap, nomad, noname, paranormal, quicksets,
  trimspells, chime, fancychat's targets) for something the client has no setting for; they stop as they do in Ashita
  when a scan fails. simplelog logs one text_in error for a line before its profile loads.
- **The other two failures are in the addons themselves:** gearinfo needs a module `imguidef` and yield needs
  `ffxi.enums`, and neither is in either release.
- **Seen working on screen:** HXUI's HUD (player / target bars, party list, patch notes with its images), XIUI's player
  and target bars with its macro bar, tCrossBar's crossbar, zonename's zone title, distance, fps, tparty, treasurepool,
  timestamp's log stamps, chat output of every addon's help.
- **LuAshitacast** loads the per-character profile and its HandleDefault set really swapped gear: BotC's own Onion Rod
  and San d'Orian Ring went on (the server's char_equip showed main and ring 1 filled, and the rod drew in her hand),
  and the "remove" set took them off again.
- **Sockets:** a test addon's `socket.http` and `socket.ssl.https` requests both returned 200.
- Helper tests (BotA, BotD): drawdistance, hideparty, xicamera, fps, namecolors, macrofix, clearcolor, filters
  (save / clear / load), aspect, config, ime, stepdialog, and a macro probe running Alt+1 through the game's macro
  functions.

### Live-test copies

For the windowed-client tests, representative addons are copied (never committed) into scratchpad roots: chat /
command (timestamp, chatmon), packet reading (checker, petinfo), ImGui UI (tparty, recast), fonts UI (distance, fps,
clock), settings saving (timestamp, tparty), plus HorizonXI's luashitacast (with a test profile per bot; never a real
character's), partybuffs, treasurepool and statustimers. Each root's `scripts/default.txt` loads its set.
