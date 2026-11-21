# Setting up ffxi-linux

From nothing to logged in: what to install, how to build, how to bring in the game data from your own FINAL FANTASY
XI install, and how to point the client at a server. The README has the short version; this page has every step and
what to do when one fails.

ffxi-linux is a native Linux client for FINAL FANTASY XI that connects to **LandSandBoat-based private servers**. It
is not affiliated with or endorsed by Square Enix, it cannot connect to Square Enix's official servers, and it ships
**no game data**: everything it draws and plays comes from an install you own. FINAL FANTASY and FINAL FANTASY XI are
registered trademarks of Square Enix Holdings Co., Ltd.

## 1. What you need

- **Linux with a Vulkan driver:** Mesa (RADV for AMD, ANV for Intel) or NVIDIA's driver. Wayland or X11; on Wayland
  the client runs through XWayland so it can place its window. `vulkaninfo --summary` (from `vulkan-tools`) should
  list your GPU.
- **About 14 GB of disk** for the imported game data (less on btrfs / XFS, where the copy is a reflink).
- **Your own FINAL FANTASY XI install**, patched to a version the server accepts (section 3).
- **A LandSandBoat server**, on this machine or one you can reach, and its checkout on this machine (section 4: the
  client reads one file from it).
- **To build:** a C++20 compiler (GCC 13+ or Clang 17+), CMake 3.25+, Ninja and the libraries below. The first
  configure downloads Dear ImGui, LuaJIT, LuaSocket, LuaSec and stb, so it needs a network connection.

| Distribution | Packages |
|---|---|
| Arch / CachyOS | `sudo pacman -S --needed base-devel cmake ninja pkgconf git sdl3 vulkan-icd-loader vulkan-headers vulkan-memory-allocator shaderc glm fmt spdlog nlohmann-json openssl zlib ffmpeg catch2` |
| Debian 13 / Ubuntu 25.04 or newer | `sudo apt install build-essential cmake ninja-build pkg-config git libsdl3-dev libvulkan-dev libvulkan-memory-allocator-dev glslc libglm-dev libfmt-dev libspdlog-dev nlohmann-json3-dev libssl-dev zlib1g-dev libavcodec-dev libavutil-dev catch2` |
| Fedora 42 or newer | `sudo dnf install gcc-c++ cmake ninja-build pkgconf git SDL3-devel vulkan-loader-devel vulkan-headers VulkanMemoryAllocator-devel glslc glm-devel fmt-devel spdlog-devel json-devel openssl-devel zlib-ng-compat-devel libavcodec-free-devel catch-devel` |

SDL 3 is packaged only in recent releases, so older Debian, Ubuntu and Fedora releases need SDL 3 built from source.
The game's music and many of its sounds are ATRAC3, which FFmpeg decodes; the FFmpeg of all three distributions above
includes it. Link FFmpeg and OpenSSL as the distribution ships them (shared libraries); see THIRD-PARTY.md.

## 2. Build

```
git clone <this repository> ffxi-linux && cd ffxi-linux
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

- The programs: `build/xi-client` (the game), `build/xi-launcher` (profiles and options, section 5),
  `build/xi-setup` (import, checks, accounts), `build/xi-headless` (a windowless client), `build/xi-viewer` and
  `build/xi-dat` (asset tools).
- Most tests read the game data. Before the import (section 3) they show as skipped; run `ctest` again afterwards.
- To install: `cmake --install build --prefix ~/.local` puts the programs in `~/.local/bin` (the launcher script as
  `ffxi-linux`), the shaders in `~/.local/share/ffxi-linux/shaders` and the menu entries **ffxi-linux** and
  **ffxi-linux launcher**. `cpack --config build/CPackConfig.cmake -B dist` makes a `.tar.gz`, and a `.deb` or `.rpm`
  when built on Debian / Ubuntu or Fedora.

## 3. The game data

The client reads the game's own files. `xi-setup import` copies the ones it needs out of your install into its own
folder, `~/.local/share/ffxi-linux/game` (`$XDG_DATA_HOME` is honoured). After the import the install itself is not
needed, and nothing is ever written back to it.

### 3.1 Getting an install

Pick whichever route you have:

- **An existing Windows install.** Copy the whole `FINAL FANTASY XI` folder (normally
  `C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI`) to this machine, or mount the Windows drive and
  import straight from it.
- **A new install under Wine.** Install Wine, run Square Enix's official installer in a Wine prefix (`~/.wine` by
  default), then start PlayOnline Viewer in that prefix and let it download and apply the game's updates. The first
  update takes hours. Lutris, Faugus or Bottles can manage the prefix for you.
- **Steam.** FINAL FANTASY XI on Steam (app 230330) installs under Proton; its prefix is
  `~/.local/share/Steam/steamapps/compatdata/230330/pfx`. Run it once through Steam so PlayOnline patches it.
- **A private server's client distribution.** Some private servers ship their own copy of the client. It works if its
  data matches the server you connect to (section 3.4). Some of these launchers change game files, so prefer an
  unmodified install where you can.

### 3.2 Importing

```
./build/xi-setup import --list                       # the installs it found, with each one's patch version
./build/xi-setup import                              # the one usable install (it asks if there are several)
./build/xi-setup import "/path/to/FINAL FANTASY XI"  # or name one: the folder, a Wine prefix or a mounted drive
```

- **Where it looks:** `--list` shows each install with where it was found:
  - **Steam:** app 230330 in every Steam library (`steamapps/libraryfolders.vdf`; native, `~/.steam` and Flatpak
    Steam), both its Proton prefix (`compatdata/230330`) and its own copy (`common/FFXINA`);
  - **Lutris, Faugus, Bottles:** the prefix of every game in their game lists (`lutris/games/*.yml`,
    `faugus-launcher/games.json`) and every bottle (native and Flatpak);
  - **Wine:** `~/.wine`, and Wine prefixes one or two levels under `~/Games`;
  - **Windows drives** mounted under `/mnt`, `/media` or `/run/media/$USER`.

  Inside a prefix or drive it tries PlayOnline's usual folders, then any
  `Program Files*/<folder>[/<folder>]/SquareEnix/FINAL FANTASY XI`, which covers private-server launchers that install
  elsewhere. Name the folder yourself if yours is somewhere else.
- **What it copies:** `VTABLE.DAT` / `FTABLE.DAT` (the file indexes), the `ROM*` folders (models, zones, textures,
  interface, event scripts), the `sound*` folders, `FFXiMain.dll` (the client reads a few tables out of it, found by
  fingerprint), `patch.cfg` (the patch version) and the game's macro files. It does not copy movies, tools, other DLLs
  or executables. About 13.5 GB.
- **Checking the copy:** after copying, the import checks every file against the install and prints a report:
  - `MISSING`: not in the copy;
  - `SIZE`: the copy has the wrong size;
  - `DIFFERS`: the bytes differ from the install's;
  - `OUTDATED`: changed in the install since the import, or added by a patch;
  - `STALE`: no longer in the install.

  Files the client can't start without (`FFXiMain.dll`, `VTABLE.DAT`, `FTABLE.DAT` and each `ROMn` folder's
  `VTABLEn.DAT` / `FTABLEn.DAT`) are marked `[required]`. A copy with anything missing or wrong makes the import exit
  with status 1. By default the index files are compared byte for byte and the rest by size; `--verify` compares every
  file byte for byte, which reads both copies (minutes for the full 13.5 GB).
- **Updating:** patch the install the data came from, then run the import again. Only changed files are copied, and
  macro files you edited in the copy are kept.
- **Somewhere else:** `FFXI_ROOT=DIR` (or `--root DIR`) makes every program use another folder instead. Without it,
  the programs look in the imported copy first, then `~/.wine`.

The launcher's **Tools** page has the same import (installs found on this machine, or any folder you pick).

**First run:** when the launcher starts with no usable game data (no `FFXI_ROOT`, no imported copy, nothing in
`~/.wine`, and the selected profile sets no game folder of its own), it shows the import first: the installs found on
this machine (version and where each was found, newest usable one picked), or a folder you type or browse to. Import
copies with a progress bar, then shows the same file check as above; Continue goes on to the launcher (and connects,
if you had pressed Connect). Skip for now leaves it for later; pressing Connect without game data brings it back.

### 3.3 Checking

```
./build/xi-setup check                    # add --host ADDRESS for a server on another machine
```

It lists the game folder and its patch version, `compress.dat` (section 4), the server's ports, and what to fix for
each. For an imported copy it runs the same file check as the import, against the install it came from: it reports
files lost from the copy, and files the install has changed since (patch, then import again). `xi-setup check --verify`
compares every file byte for byte. The client itself says what is missing before it opens a window; `XI_LOG=debug` logs more.

### 3.4 Matching the server's version

- The client sends the version your data is patched to (from `patch.cfg`). An LSB server compares it with
  `CLIENT_VER` in its `settings/login.lua`: with LSB's default `VER_LOCK = 2`, `CLIENT_VER` must not be newer than
  your data. Update the game, or set the server's `CLIENT_VER` to the version `xi-setup check` prints.
- Data older than the server's own target logs in once `CLIENT_VER` is lowered, but LSB numbers its zone messages for
  its target version, so some server messages come out as their neighbours ("Obtained 218 gil." for an item).
  `xi-setup check` and the client's log warn about it. The fix is newer game data.

## 4. The server and `compress.dat`

- **The server.** Set up a LandSandBoat server as [its own documentation](https://github.com/LandSandBoat/server)
  describes. For a server on another machine, its zones must advertise an address this machine can reach.
- **`compress.dat`.** The game's network compression table is not shipped here; the client reads LandSandBoat's
  `res/compress.dat` (2048 bytes, in the server checkout or its Docker image). It looks, in order, at
  `XI_COMPRESS_DAT` (the file), `$LSB_DIR/res`, its own copy `~/.local/share/ffxi-linux/compress.dat`, then
  `~/server`, `~/git/server`, `~/LandSandBoat` and `~/git/LandSandBoat` (each `/res/compress.dat`), and uses the
  first file that is a valid table; one that isn't is skipped and reported.
  - `./build/xi-setup compress-dat` says which file the client uses, or what to do.
  - `./build/xi-setup compress-dat FILE` checks FILE and copies it to `~/.local/share/ffxi-linux/compress.dat`, so the
    client finds it without `LSB_DIR` or a checkout (for a server on another machine, copy the file from that
    machine's checkout first).
  - The launcher does the same in **Tools > LSB compress.dat** and on the first-run page (type or **Browse...** to the
    file, then **Use this file**).
  - `xi-setup check` reports it with the paths it tried.
- **Account and character.** Use ones you have, or make them (the server's `ACCOUNT_CREATION` and
  `CHARACTER_CREATION` settings are on by default):
  ```
  ./build/xi-setup create-account --user myname --pass mypassword
  ./build/xi-setup create-char --user myname --pass mypassword --name Myname --race 7 --face 3 --job 3 --nation 2
  ```
  `./build/xi-setup` alone prints every option (races 1-8, faces 0-15, starting jobs 1-6, nations 0-2).
- **Login protocol.** LSB servers from October 2026 use login protocol 2.2, older ones 2.1. The default, `auto`,
  tries 2.2 and falls back to what the server asks for; `--login-protocol 2.1|2.2` (or `login_protocol` in
  `client.ini`) fixes it.

## 5. Playing

- **The launcher** (`xi-launcher`; installed, **ffxi-linux launcher** in the menu): add a server profile (address,
  account, character, login protocol, game folder), pick a graphics preset, and press **Connect**. Passwords go in the
  desktop keyring (Secret Service) if you save them, never in a file. See [launcher.md](launcher.md).
- **Without it:** `scripts/xi-client.sh --host ADDRESS --user U --char NAME` (or `ffxi-linux` when installed). Your
  defaults can go in `~/.config/ffxi-linux/local.env` (copy `scripts/local.env.example`).
- **Settings** are in `~/.config/ffxi-linux/client.ini` (`config/client.ini.example` lists them). The keyboard
  layouts, the commands and the graphics flags are in [input-format.md](input-format.md), [commands.md](commands.md)
  and [graphics-guide.md](graphics-guide.md).

## 6. Optional extras

### 6.1 NVIDIA DLSS

On an RTX GPU the client can render through DLSS 4 (`--dlss`). DLSS is NVIDIA's, under NVIDIA's license, and is never
part of this repository or its releases: fetch the SDK yourself and build with it.

```
tools/fetch_dlss_sdk.sh "$HOME/src/dlss-sdk"
cmake -S . -B build -G Ninja -DXI_DLSS=ON -DXI_DLSS_SDK="$HOME/src/dlss-sdk"
```

A DLSS build links NVIDIA's SDK, so keep it for your own use rather than passing the binaries on. Details, and what
`--dlss` does to each graphics option, in [dlss.md](dlss.md).

### 6.2 Ashita addons

Ashita v4 Lua addons run in the client unchanged. The client ships the standard Ashita v4 addons and their shared
Lua libraries that work in it (`third_party/ashita-addons`; its README lists the upstream addons left
out: Windows-only, built into the client (such as timestamp and autologin), crashing it, or doing nothing in it). On start it copies them into the game
folder's `addons/`, adding only what is not there yet; `xi-setup addons` does the same on demand. To use the
libraries of an Ashita (or private-server) install you have instead, run `xi-setup addons FOLDER` (`xi-setup addons
--list` shows the installs it finds). Put any other addon folders in `addons/` yourself, and load addons in game with
Config > Add-ons or `/addon load NAME`. What works and what does not is in [ashita-compat.md](ashita-compat.md).

### 6.3 Ashita plugins

Ashita plugins are Windows DLLs; the client does not load DLLs. A plugin whose source is published can be rebuilt for
Linux instead:

1. `xi-setup addons FOLDER` (from your Ashita install) also copies Ashita's plugin SDK (`plugins/sdk`) into the game folder.
2. Install Wine's Win32 headers (the package that provides `/usr/include/wine/windows`, such as `wine` on Arch). Only
   their types are used; nothing of Wine runs.
3. Configure with the plugin's source: `cmake -B build -DXI_ASHITA_PLUGIN_SOURCES=/path/to/Plugin`, and build.
4. In game, `/load plugin` loads `build/plugins/plugin.so`.

Without the SDK or the headers, the client builds without plugin support and `/load` says so. The Ashita SDK has its
own license (THIRD-PARTY.md). Building a plugin, and which plugins have been tried, are in
[ashita-compat.md](ashita-compat.md) ("Plugins").

## 7. When something fails

| Symptom | Likely cause and fix |
|---|---|
| The client reports no game data before opening a window | No import yet, or `FFXI_ROOT` points at the wrong folder: section 3.2. |
| The server refuses the login over the version | Data older than the server's `CLIENT_VER`: section 3.4. |
| `compress.dat` is reported missing | No LSB checkout where the client looks: `xi-setup compress-dat FILE` (or the launcher's Tools page), or set `LSB_DIR` / `XI_COMPRESS_DAT` (section 4). |
| `compress.dat` is "skipped" | That file is not LSB's 2048-byte table (truncated, or another file): point at the real `res/compress.dat`. |
| The lobby works but the zone never loads | The server advertises a zone address this machine cannot reach: the server's zone settings. |
| No music, or silent effects | An FFmpeg without ATRAC3: use the distribution's FFmpeg (section 1). |
| The window opens on the wrong monitor | `XI_DISPLAY=primary`, a display index, or `--display`. |

`./build/xi-setup check` covers the first three in one go.
