# ffxi-linux

A native Linux client for FINAL FANTASY XI, written in C++20 with Vulkan. It connects to
[LandSandBoat](https://github.com/LandSandBoat/server)-based private servers and draws the game from the data files of
your own FINAL FANTASY XI install.

- **It is not affiliated with or endorsed by Square Enix.** FINAL FANTASY and FINAL FANTASY XI are registered
  trademarks of Square Enix Holdings Co., Ltd.; the names are used here only to say which game's data the client
  reads.
- **No game data is included.** Nothing from Square Enix is in this repository, its releases or its packages. You need
  your own install; `xi-setup import` copies the files the client reads from it ([docs/setup.md](docs/setup.md)).
- **Private servers only.** It cannot connect to Square Enix's official servers and does not try to.

**New here?** [docs/setup.md](docs/setup.md) walks through the whole setup: dependencies, build, game data, a
server and the launcher.

## Status

**`xi-client` plays the game end to end on an LSB server**, from the game's own data and interface.
- **World:** zones with collision, zone lines, weather lighting, sky, sun, moon, lens flares, the sea and other zone
  effects, doors that open and block, lifts that carry you, ships and airships on their paths, seagulls.
- **Characters:** real models and animations for players, NPCs (children included) and monsters, battle and event
  motions, footsteps by floor and footwear.
- **Play:** combat, magic, abilities, weapon skills, items and equipment, trade, shops, synthesis, party, linkshells,
  fishing, auction house, delivery box, the Search window, the Mog House (storage, job change, furniture), about 150
  text commands.
- **Events:** NPC dialog and cutscenes from the game's own event scripts: camera shots, fades, actors walking and
  gesturing.
- **Sound:** effects, zone music, ambience loops by weather and time of day (indoors too), interface sounds.

## What you need

- **Linux with a Vulkan 1.3 driver** (Mesa RADV / ANV, AMDVLK, or NVIDIA's). Wayland or X11; on Wayland the client
  runs through XWayland so it can place its window. AMD and Intel GPUs are supported (every graphics option except
  NVIDIA's DLSS, which falls back to FSR 3.1) but not yet tested on real hardware; ray tracing needs an NVIDIA RTX,
  AMD RX 6000 or Intel Arc GPU or newer (`docs/fsr.md`).
- **The game's data from your own FINAL FANTASY XI install** (see "Getting the game data" below). `xi-setup import`
  copies the files the client reads into its own folder once; after that the install is not needed. Nothing from
  Square Enix is in this repo or its packages.
- **A LandSandBoat server** ([LandSandBoat/server](https://github.com/LandSandBoat/server)), on this machine or your
  network, set up as its own docs describe. You also need its checkout on this machine: the client reads
  `res/compress.dat` from it.
- **Build tools and libraries:** a C++20 compiler (GCC 13 or newer, or Clang 17 or newer), CMake 3.25+, Ninja, and the
  packages below. Configuring the first time downloads Dear ImGui, LuaJIT, LuaSocket, LuaSec and stb from GitHub, so it needs a
  network connection.

| Distribution | Packages |
|---|---|
| Arch / CachyOS | `sudo pacman -S --needed base-devel cmake ninja pkgconf git sdl3 vulkan-icd-loader vulkan-headers vulkan-memory-allocator shaderc glm fmt spdlog nlohmann-json openssl zlib ffmpeg catch2` |
| Debian 13 / Ubuntu 25.04 or newer | `sudo apt install build-essential cmake ninja-build pkg-config git libsdl3-dev libvulkan-dev libvulkan-memory-allocator-dev glslc libglm-dev libfmt-dev libspdlog-dev nlohmann-json3-dev libssl-dev zlib1g-dev libavcodec-dev libavutil-dev catch2` |
| Fedora 42 or newer | `sudo dnf install gcc-c++ cmake ninja-build pkgconf git SDL3-devel vulkan-loader-devel vulkan-headers VulkanMemoryAllocator-devel glslc glm-devel fmt-devel spdlog-devel json-devel openssl-devel zlib-ng-compat-devel libavcodec-free-devel catch-devel` |

The Arch list is what this project is built with. The Debian and Fedora lists were checked by clean builds in
`debian:stable` (13.7) and `fedora:latest` (44) containers on 2026-10-20: no warnings, every test passing, and the
.deb / .rpm made there installing on a fresh container of the same release (Ubuntu and older Fedora releases are
untested; SDL 3 is only packaged in recent releases). Music and many sounds are ATRAC3, which FFmpeg decodes; Arch's,
Debian 13's and Fedora 44's (`libavcodec-free`) FFmpeg all include it.

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

- The default build type is RelWithDebInfo (optimised, with debug symbols); `Release` is the same without symbols.
- `-DXI_SANITIZE=ON` builds with AddressSanitizer and UBSan (for work on the parsers); `-DXI_BUILD_TESTS=OFF` skips the
  tests (they need Catch2 v3).
- Most tests read the game's files: they find the imported copy (`xi-setup import`, next section) or `FFXI_ROOT`, and
  show as skipped without either.
- The build produces `build/xi-client` (the game), `build/xi-setup` (first-run checks, accounts, characters),
  `build/xi-headless` (a windowless test bot), `build/xi-viewer` and `build/xi-dat` (asset tools).
- The build has no compiler warnings; `-DXI_WERROR=ON` keeps it that way (warnings become errors).
- Optional NVIDIA DLSS 4 (RTX GPUs): `tools/fetch_dlss_sdk.sh DIR`, then configure with `-DXI_DLSS=ON -DXI_DLSS_SDK=DIR`
  and run the client with `--dlss` (see `docs/dlss.md`). Off by default. The SDK is NVIDIA's, under NVIDIA's own
  license: it is not in this repository, and builds made with it are for your own use, not for passing on.
- FSR 3.1 (AMD FidelityFX, MIT; any GPU) is on by default: configure fetches the pinned FidelityFX SDK into the build
  folder (or uses `-DXI_FSR_SDK=DIR` from `tools/fetch_fsr_sdk.sh DIR`) and needs `glslangValidator`; `-DXI_FSR=OFF`
  leaves it out (`docs/fsr.md`).

### Install and packages

```
cmake --install build --prefix ~/.local        # or /usr/local (with sudo)
cpack --config build/CPackConfig.cmake -B dist # local packages; nothing is uploaded anywhere
```

- The install puts the programs in `bin/` (the launcher as `ffxi-linux`), the shaders in `share/ffxi-linux/shaders/`
  and a desktop entry in `share/applications/`. The programs find the shaders next to themselves (the build tree), in
  `../share/ffxi-linux/shaders` (an install, even moved or unpacked elsewhere), or in `XI_SHADER_DIR`.
- `cpack` always makes a `.tar.gz` of that tree. Built on Debian / Ubuntu it also makes a `.deb`, built on Fedora an
  `.rpm`, with their dependencies taken from the libraries the programs link; build a package on the release it is
  for.
- Installed, `ffxi-linux` reads your defaults from `~/.config/ffxi-linux/local.env` (the same format as
  `scripts/local.env`; the example is installed in `share/doc/ffxi-linux/`).

## First run

1. **Import the game data.** `./build/xi-setup import` finds FINAL FANTASY XI installs on this machine (Wine prefixes
   in `~/.wine` and under `~/Games`, Steam libraries, Lutris / Faugus / Bottles games, Windows drives mounted under
   `/mnt`, `/media`), copies the files the client reads into `~/.local/share/ffxi-linux/game` (`$XDG_DATA_HOME`
   honoured), and checks the copy against the install (`--verify`: every file byte for byte; docs/setup.md 3.2):
   ```
   ./build/xi-setup import --list                       # what it found, with each install's version
   ./build/xi-setup import                              # the one usable install (it asks you to pick if several)
   ./build/xi-setup import "/path/to/FINAL FANTASY XI"  # or name one: the folder itself, a Wine prefix or a drive
   ```
   - What it copies: `VTABLE.DAT` / `FTABLE.DAT`, the `ROM*` folders, the `sound*` folders, `FFXiMain.dll` (tables
     the client reads from the game code), `patch.cfg` (the version) and the game's macro files (`USER/*/mcr*`). Not
     the movies, tools, DLLs or executables. About 13.5 GB.
   - On btrfs and XFS the copy is a reflink: instant, and it takes no extra disk space while the files are shared.
   - Run it again after patching the game: only changed files are copied, and macro files you edited in the copy are
     never overwritten.
   - Every app and the tests look there first. `FFXI_ROOT=DIR` (or `--root DIR`) still uses another folder instead.
2. **Point at LSB.** Set `LSB_DIR` to your LSB checkout (`~/server`, `~/git/server`, `~/LandSandBoat` and
   `~/git/LandSandBoat` are found without it), or `XI_COMPRESS_DAT` to the file itself; or keep a copy of its
   `res/compress.dat` with `./build/xi-setup compress-dat FILE` (the launcher's Tools page does the same).
3. **Check:** `./build/xi-setup check` (add `--host ADDRESS` for a server on another machine) lists the game folder,
   the client version it is patched to, `compress.dat` and the server's ports, with what to fix.
4. **The server's settings** (LSB `settings/login.lua`, `settings/network.lua`):
   - The client sends the version your install is patched to (from its `patch.cfg`). With LSB's default
     `VER_LOCK = 2`, `CLIENT_VER` must not be newer than that: update the game, or set `CLIENT_VER` to the version
     `xi-setup check` prints. `--client-ver` overrides what the client sends.
   - `ACCOUNT_CREATION` and `CHARACTER_CREATION` must be on to create them from here (both are on by default).
   - For a server on another machine, its zones must advertise an address this machine can reach (LSB's zone
     settings); the client goes wherever the server tells it.
5. **An account and a character.** Use ones you already have, or create them:
   ```
   ./build/xi-setup create-account --user myname --pass mypassword
   ./build/xi-setup create-char --user myname --pass mypassword --name Myname --race 7 --face 3 --job 3 --nation 2
   ```
   Races 1-8 (Hume M/F, Elvaan M/F, Tarutaru M/F, Mithra, Galka), faces 0-15, `--size 0..2`, starting jobs 1-6
   (WAR MNK WHM BLM RDM THF), nations 0-2 (San d'Oria, Bastok, Windurst). `./build/xi-setup` alone prints the options.
6. **Play:** `scripts/xi-client.sh`, then fill in the login form (Up/Down or Tab between fields, Enter to connect). A new
   character starts with its nation's opening cutscene (Enter continues each message).
   - Your own defaults (the paths above, the server's address, a pre-filled login) go in `scripts/local.env` (or `~/.config/ffxi-linux/local.env`): copy
     `scripts/local.env.example`. On the command line: `--host H --user U --pass P --char NAME`, `--autoconnect` to
     skip the form, and `--char ""` to choose in the game's lobby (Select Character, the arrows, Enter).
   - The window opens on your secondary monitor if you have one (`XI_DISPLAY=primary`, a display index, or
     `--display`). `XI_NO_FOCUS=1` opens a window that never takes keyboard or mouse focus.
     `XI_WINDOW_SLOT=tl|tr|bl|br` fills one quarter of the display (several clients side by side);
     `XI_WINDOW_SLOTS="Alice=tl,Bob=tr"` picks the quarter by character name.
   - `--login-protocol auto|2.1|2.2` (or `login_protocol` in `client.ini`'s `[ffxi-linux]` section, or
     `XI_LOGIN_PROTOCOL`): LSB servers from October 2026 need 2.2, older ones 2.1. The default, `auto`, tries 2.2 and
     falls back to what the server asks for (docs/protocol-login.md §1.4). xi-headless and xi-setup take the same flag.
   - Settings are kept in `~/.config/ffxi-linux/client.ini` (see "Display settings" below), screenshots and macros
     under `~/.local/share/ffxi-linux/` and `~/.config/ffxi-linux/macros/`.

If something is wrong the client says what before opening a window; `XI_LOG=debug` logs more.

## Getting the game data

ffxi-linux never ships Square Enix's files: each player imports their own, from a FINAL FANTASY XI install they own.
Any up-to-date install will do; once imported, it can be removed. [docs/setup.md](docs/setup.md) has each route in
more detail, with what the client reads and how to check it.

- **Copy it from a Windows PC.** Copy the whole `FINAL FANTASY XI` folder (usually
  `C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI`) to this machine, or mount the Windows drive, and run
  `xi-setup import "/path/to/FINAL FANTASY XI"` (or the drive's mount point).
- **Install it once under Wine.** Install Wine, run the official installer from Square Enix's site in a Wine prefix,
  then start PlayOnline Viewer there once and let it download and apply the game's updates (this takes hours the first
  time). Then `xi-setup import` finds the prefix (`~/.wine`, or pass its path). Launchers such as Lutris or Faugus can
  manage the prefix; Steam's FINAL FANTASY XI (Proton) is found too.
- **Match the server's version.** The import records the version your install is patched to (`patch.cfg`); an LSB
  server checks it against `login.CLIENT_VER` (see First run). Use an unmodified retail install; private servers'
  launchers may change files.
- **Older data than LSB's shows wrong server messages.** LSB numbers its zone messages (`scripts/zones/*/IDs.lua`)
  for the client version in its `settings/default/login.lua` (`CLIENT_VER`). Lowering `CLIENT_VER` in the server's own
  `settings/login.lua` lets older data log in, but the numbers stay: with data older than that, lines such as
  "Obtained: <item>." come out as their neighbours ("Obtained 218 gil." for item 218, "Lost key item" for "Obtained key
  item"). `xi-setup check` and the client's log warn about it.
- **Updating the game data.** Patch the install the data came from, then import again (only changed files are copied):
  1. Start PlayOnline Viewer in that install's Wine prefix and let it check and apply the game's updates (for a
     Faugus or Lutris prefix, run its PlayOnline entry; with plain Wine:
     `WINEPREFIX=~/.wine wine "C:\Program Files (x86)\PlayOnline\SquareEnix\PlayOnlineViewer\pol.exe"`).
  2. `./build/xi-setup import --list` shows each install's version; then `./build/xi-setup import "/path/to/FINAL FANTASY XI"`.
  3. `./build/xi-setup check` should no longer warn about the version.

## Playing

- **Launcher:** `xi-launcher` (installed: "ffxi-linux launcher" in the menu) keeps server profiles, the graphics presets and
  every option, and connects with one click; passwords go in the desktop keyring, never in a file
  ([docs/launcher.md](docs/launcher.md)). Everything it does is also a plain xi-client option.
- **Keyboard:** the game's own default bindings, in its two layouts (full tables in `docs/input-format.md`):
  - **Normal** (default; full-size keyboards): numeric keypad 8/2/4/6 move, arrow keys turn the camera, PageUp/PageDown
    (or Num 9 / Num 3) zoom, Num 7 autorun, Num / walk/run, Num 5 first-person view, Num * heal (rest), Num − main menu,
    Enter confirm, Esc cancel, Print Screen screenshot (saved to `~/.local/share/ffxi-linux/screenshots/`), Scroll Lock
    hides the windows.
  - **Compact** (`keyboard = 1` in `[ffxi-linux]`, or `--keyboard compact`; for keyboards without a keypad): everything
    above plus WASD to move, IJKL for the camera (menu cursor while a menu is open; Shift+IJKL always the camera),
    `.`/`,` zoom, Y confirm, N cancel, R autorun, Z walk/run, V view, H heal, `-` main menu, `[` screenshot, `]` hide windows.
  - **Shortcuts** (both layouts): Ctrl or Alt + A attack, I items, M magic, J abilities, W weapon skills, E equipment,
    C check, H heal, V view, Z walk/run, Y/N confirm/cancel; S, P, T, L, X, U, Q open the chat line with /say, /party,
    /tell, /linkshell, /linkshell2, /unity, /assist.
- **Camera (mouse):** hold right mouse to orbit, mouse wheel to zoom.
- **Interface:** the game's own windows, loaded from its data files: chat log (bottom left), party and target windows (bottom right), and the main menu.
- **Main menu:** Num − (Compact: `-`; Insert also works) opens it. Arrow keys move the pointing hand, Enter selects, Escape goes back. The mouse works too: hover to point, click to select, right-click to go back.
- **Chat:** Space or Enter starts a line, Enter sends it. Text is said by default; `/sh` (`/shout`) shouts, `/p` (`/party`) talks to your party, `/l` / `/l2` to your linkshells, `/y` (`/yell`) yells, `/t name text` (`/tell`) sends a tell. Server `!` commands work if the account is a GM. The mouse wheel over the log scrolls it; Num + (Compact F) selects the log window, then Up/Down scroll and Enter shows the full-screen log.
- **Target:** Tab cycles through named entities within 30 yalms (yellow highlight, Target window); F1 targets yourself and F2-F6 your party members; Esc clears the target.
- **Fight:** Ctrl+A (or Alt+A) attacks the target, or disengages if you are already fighting. While you are engaged your character turns to face the target. The chat box shows the game's own battle messages.
- **Weapon skills, abilities and magic:** main menu → Abilities (Job Abilities, Weapon Skills) and Magic list the ones you have. Weapon skills unlock at 1000 TP while you are engaged. Spells are shown if your job and level can cast them; attack spells go on your target, other spells on yourself or the player you have targeted.
- **Knocked out:** the "Back to Home Point" window; Enter asks "Return to Home Point?". A Raise or Tractor cast on you opens a Yes/No prompt.
- **Talk:** Enter (Compact: Y) talks to the targeted NPC; what they say appears in the chat box. NPCs whose talk is an event run the game's event script: press Enter to continue each message (a ▼ marker shows in the log), and pick from menus with the arrows and Enter.
- **Main menu page 2:** Left/Right in the main menu flips to Missions, Quests, Key Items, … Config, Current Time, Shut Down, Log Out.
- **Shops:** talking to a vendor opens Buy / Sell. Buy lists the goods (Enter, then Left/Right or PageUp/PageDown sets how many, then Buy); Sell lists your items, shows the vendor's price, and sells the whole stack.
- **Linkshell:** main menu → Linkshell → pick your pearl → Equip turns it on or off; `/l` (`/linkshell`) and `/l2` talk on it.
- **Synthesis:** main menu → Synthesis → Synthesize: pick a crystal, put ingredients in the boxes (Enter on a box), then OK. The result and skill-ups appear in the log; Synthesis → History lists this session's recipes and refills the window from your inventory to repeat one. (The ingredient window borrows the trade window's layout.)
- **Quests / Missions:** page 2 → Quests lists each area's current and completed quests; Missions → Nations / Zilart lists completed missions and the current one. Descriptions show in the help window.
- **Key Items:** main menu page 2 → Key Items lists every key item you hold, with its description in the help window.
- **Config:** every page of the game's Config works or is dimmed with a reason, e.g. Windows → Window Type 1–8 reskins
  every window, Misc. 2 → Keyboard Size switches between Full (Normal), Compact 1 and Compact 2, and Misc. 2 → Movement /
  Commands rebinds keys (press the new key on a row). Settings are saved to `~/.config/ffxi-linux/client.ini`
  (`[ffxi-linux]` section). A dimmed hand marks the current setting.
- **Sound:** the game's sound effects (spells, swings, hits, cries) and zone music (day / night / battle). `sound_volume` and
  `music_volume` (0..100) in `[ffxi-linux]`; `/mutebgm`, `/mutese`, `/battlebgm`. `XI_NO_AUDIO=1` runs silent.
- **Commands:** about 150 of the game's text commands work (chat channels, targeting, actions, party, emotes, `/random`, `/check*`, `/anon`, `/follow`, `/lockon`, `/help` with the game's usage texts, …); see `docs/commands.md`. Unknown ones show "...A command error occurred." like the game.
- **Ashita addons:** Ashita v4 Lua addons run unchanged (HXUI, XIUI, LuAshitacast, timestamp, distance, tparty,
  treasurepool, statustimers, timers ...). They live in the game folder's `addons` folder, normally
  `~/.local/share/ffxi-linux/game/addons/`: the client creates it on first run, laid out like an Ashita install
  (`addons/<name>/<name>.lua`, Ashita's shared libraries in `addons/libs`, settings in `config/addons`, boot scripts in
  `scripts`). The client comes with the standard Ashita v4 addons and libraries that work in it
  (`third_party/ashita-addons`, see its README for the upstream addons left out and why) and puts them there on start, adding only
  what the folder doesn't have yet: it never overwrites your files, and an addon you delete stays deleted. `xi-setup
  addons` does the same on demand; `xi-setup addons FOLDER` copies the libraries from your HorizonXI or Ashita install
  instead (`xi-setup addons --list` shows those it finds; `--addon NAME` also copies chosen addons, never overwriting
  ones you already have). The launcher's Tools page does both and opens the folder. Then drop addon folders into `addons/` and use Config > Add-ons in game (load, unload, reload, start at
  login). `ashita_dir` in `~/.config/ffxi-linux/client.ini`'s `[ffxi-linux]` section (or `XI_ASHITA_DIR`) points at
  another Ashita folder instead, and `ashita_dir = off` turns addons off; the client never writes to your Windows
  install. `ashita_load = distance, fps` loads addons at
  start, `ashita_script` runs a boot script's `/addon load` lines, and `/addon load|unload|reload <name>` and
  `/addon list` work in game. `ashita_version = latest | horizonxi | auto` picks which Ashita release the client
  answers as (auto: HorizonXI's when its extra libs are there). Plugins are not loaded as Windows DLLs; ones with
  published source (Shorthand, for one) can be rebuilt for Linux against your Ashita install's SDK and load with
  `/load` (docs/setup.md, "Ashita plugins"). What works and what does not is in `docs/ashita-compat.md`.
- **Macros:** hold Ctrl or Alt to see the palette; Ctrl/Alt+1..0 runs a macro. Macros live in `~/.config/ffxi-linux/macros/<Character>.txt`:
  ```
  [ctrl 1] Cure
  /ma "Cure" <me>
  /wait 3
  /p Cured!
  ```
- **Party:** main menu → Party (Leave; Accept or Decline an invitation), or `/pcmd add <name>`, `/join`, `/decline`, `/pcmd leave`.
- **Trade:** target an NPC or a player, main menu → Trade (with a player: they accept the same way). Pick items into the boxes (PageUp/PageDown changes the count); on the gil field Left/Right add or remove 1 gil, PageUp/PageDown 100. Then OK.
- **Map:** main menu → Map shows the current area's map with your position and facing; Left/Right shows the zone's other maps.
- **Status effects** show as icons under the help line; they blink in their last 30 seconds.
- **Equipment and items:** main menu → Equipment shows the slot grid; Enter on a slot lists what fits (or "(Remove)"). Main menu → Items lists the inventory; Enter, then Use (potions, food, scrolls; gear is equipped) or Drop. `/sort` merges stacks.
- Spells and abilities that are recasting show their remaining time.
- **Search:** main menu → Search: Area, Name, Job, Country, Race, Level, Rank or a comment genre lists matching
  players (Unity too); Enter on one offers Tell or Invite. Edit Comment sets your search comment. `/sea` prints the same
  to the log. `/blacklist` opens the blacklist; tells and chat from listed players are hidden.
- **Mog House:** walk into your residential area's Mog House entrance. Talk to the Moogle (or main menu → Mog House)
  for storage, the delivery box, Change Jobs and Layout (place furniture, dress mannequins); the door offers the exits
  and the second floor. Furniture and mannequins are drawn.
- **Cutscenes:** play with their camera, fades and actors; Enter continues each message.
- **Quit:** type `/logout` (or `/shutdown`), or close the window; either logs out first; the server's logout timer takes about 30 seconds. A monster hitting you cancels the logout, as in the original client.

F11 shows a debug overlay: position, Vana'diel time, weather id, draw stats and nearby entities. Purple pillars mark zone lines.

**Display settings** work like Ashita's: FFXI's registry keys in an INI file's `[ffxi.registry]` section.
Window size (0001/0002), background resolution (0003/0004, the 3D render size), window mode (0034) and menu
resolution (0037/0038, the interface size). See `config/client.ini.example`. Put a copy at
`~/.config/ffxi-linux/client.ini`, or pass any file with `--config` (an Ashita boot file such as `horizonxi.ini` works).
One-off overrides: `--res 1920x1080 --bg-res 3840x2160 --menu-res 1280x720 --window-mode 2`. The game's eight window
styles ("Window type" in its Config) are `window_skin = 1..8` in an `[ffxi-linux]` section, or `--window-skin N`.
`--dlss[=quality|balanced|performance|ultra-performance|dlaa]` renders the scene through DLSS 4 in a DLSS build;
without the flag nothing about rendering changes (`docs/dlss.md`). `--fsr[=quality|balanced|performance|ultra-performance|native]`
does the same with AMD's FSR 3.1 on any GPU, and `--dlss` on a GPU without DLSS uses it (`docs/fsr.md`).
`--rt[=ao,shadows,gi,reflections|all]` adds ray-traced ambient occlusion, sun / moon and lamp shadows, a light bounce
and water reflections on GPUs with Vulkan ray queries (NVIDIA RTX, AMD RX 6000, Intel Arc and newer), kept close to the game's own look; `--rt-strength=0..1`
tones it down, and with `--dlss` DLSS Ray Reconstruction denoises it. Without the flag nothing changes
(`docs/raytracing.md`).


## Other tools

```
./build/xi-headless --user U --pass P --char NAME --seconds 30 --shout "hello"
```

`xi-headless` logs a character in without a window and prints what it sees (`--host H`, `--client-ver V`,
`--compress-dat PATH`, `--verbose`). `xi-dat` inspects the game's files (`./build/xi-dat` lists its commands) and
`xi-viewer` draws textures, zones and models from them.

## Layout

| Path | |
|---|---|
| `docs/protocol-login.md` | Auth / data / view lobby protocol, byte-exact, cited against LSB source |
| `docs/protocol-map.md` | Map UDP protocol: framing, Blowfish variant, Huffman compression, zone-in, chat, zoning |
| `src/xi/` | Byte helpers, MD5, FFXI Blowfish, Huffman codec, settings, finding the game and LSB (`install.h`) |
| `src/net/` | TCP/UDP sockets, TLS client |
| `src/lobby/` | Auth + lobby state machine → map handoff; account and character creation |
| `src/map/` | Map session: sequencing, encryption, compression, keepalive, logout |
| `apps/xi-headless/` | CLI driver (headless bot) |
| `apps/xi-setup/` | First-run checks, account and character creation |
| `scripts/xi-client.sh` | Launcher (installed as `ffxi-linux`); reads `scripts/local.env` and `~/.config/ffxi-linux/local.env` (your defaults, not committed) |
| `packaging/` | The desktop entry the install adds |
| `apps/xi-client/` | The client: login UI, zone, entities, movement, chat |
| `apps/xi-viewer/` | Asset viewer: `textures`, `cubes`, `zone` modes, `--screenshot` |
| `apps/xi-dat/` | DAT inspection: `path`, `chunks`, `textures`, `zone` |
| `src/gfx/` | Vulkan context, bindless textures, mesh/marker renderers, zone scene |
| `src/client/` | Network session thread + world snapshot |
| `src/ui/` | The game's own interface (menus, windows, fonts) drawn from its data |
| `src/audio/` | Sound effects, music and ambience |
| `src/dat/` | DAT index, chunk walker, textures, zone mesh/placement decoding |
| `tools/zone_to_obj.py` | Reference zone decoder → OBJ |
| `tools/gen_blowfish_table.py` | Generates the Blowfish P/S init table from pi |

## Notes

- Nothing from Square Enix is in this repo:
  - The Huffman table (`compress.dat`) is read at runtime from the LSB checkout, or a copy you make of it (LSB is
    GPLv3).
  - The zone de-obfuscation tables are read from your own `FFXiMain.dll` and located by SHA-256.
  - The Blowfish init table is generated from first principles.
- The map server replies exactly once per datagram and answers a stale ack with its cached previous reply,
  so the client keeps one datagram in flight (lockstep). Pipelining loses packets such as the logout 0x00B.
- Game assets will always be loaded from the user's own install and never committed.

## License

ffxi-linux is free software under the GNU General Public License, version 3 or (at your option) any later version
(`SPDX-License-Identifier: GPL-3.0-or-later`); see [LICENSE](LICENSE). The libraries and tools it uses, and their
licenses, are listed in [THIRD-PARTY.md](THIRD-PARTY.md). The game and its data are not covered: they belong to
Square Enix and are read from your own install.
