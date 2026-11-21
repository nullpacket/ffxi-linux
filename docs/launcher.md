# xi-launcher

A desktop launcher for the client (goal 44): server profiles, every graphics option and preset, the display and game
settings, setup tools, and one **Connect** button, so nobody has to remember xi-client's command line. Everything it
sets maps onto existing flags and client.ini keys, so the command line keeps working without it.

```
xi-launcher                      # from the build dir: build/xi-launcher; installed: xi-launcher (menu: "FINAL FANTASY XI")
xi-launcher --config FILE        # another settings file (default ~/.config/ffxi-linux/launcher.ini)
```

## Pages

- **Servers (left):** the profiles. Add, Copy, Remove (asks first), Default (the one selected at start). **Connect**
  starts the client with the selected profile; while it runs the button stops it.
- **Server:** name, host, the four ports (LandSandBoat's defaults 54231 login, 54230 data, 54001 view, 54002 search),
  **Check server** (TCP connects to each port), account, character (empty = choose in the lobby), password, the game
  folder (empty = the client's own lookup: FFXI_ROOT, the imported copy, ~/.wine), and whether the profile has its own
  settings or uses the global ones.
- **Graphics:** the presets of [graphics-guide.md](graphics-guide.md) as one-click buttons (Original, Original sharp,
  Subtle, Enhanced, Dramatic, Remaster, Remaster half-way; hover one for its flags), then every option: the upscaler (DLSS where it runs, else FSR 3.1, or FSR always) and its mode, ray
  tracing and its effects, Boost, strength / shadow cap / reflections (unticked = the client's default), post effects
  and their strengths, path tracing and its controls, HD textures. Options this build or GPU cannot do are greyed out
  with the reason on hover (from `xi-client --capabilities`). The resulting flags are shown at the bottom.
- **Display & game:** window size, mode, display, scene and menu resolution, window type, sound and music volume,
  keyboard layout, login protocol, macro folder, Ashita addons (client.ini / off / on with the folder, release and
  addons to load), and extra command-line options.
- **First run** (no usable game data: no `FFXI_ROOT`, no imported copy, nothing in `~/.wine`, and the selected
  profile sets no game folder): the pages are replaced by **Import the game data**: the installs found on this
  machine (`detectInstalls()`: version, folder and where it was found; usable ones first, newest version picked), a
  folder to type or **Browse...** (the folder itself, or a Wine prefix / drive holding it), **Import** (copies into
  `~/.local/share/ffxi-linux/game` in the launcher, with a progress bar, then the file check of `xi-setup check`) and
  **Skip for now**. A copy with a required file missing shows the report and stays on the page. **Continue** returns
  to the pages; when **Connect** brought the page up, it connects once the import is done. Test hooks:
  `--first-run` (show it even with game data) and `--first-run-import FOLDER` (pick FOLDER and import at start).
- **Tools:** what the client found (GPU, driver, ray tracing, DLSS, FSR 3.1, an `XI_GPU_PRETEND` test mode, game data), **LSB compress.dat** (the file the
  client uses, or why none is usable; a file typed or picked with **Browse...** is checked and copied to
  `~/.local/share/ffxi-linux/compress.dat` by **Use this file**, as `xi-setup compress-dat FILE` does; the first-run
  page shows the same panel), **Check setup**
  (`xi-setup check` against the profile's host), **Import game data** (installs found on this machine, or any folder;
  `xi-setup import`), **Ashita addons** (the addons folder, normally `~/.local/share/ffxi-linux/game/addons`, with an
  **Open folder** button; whether Ashita's libraries are there; **Install bundled addons** (the standard set the client
  ships: `xi-setup addons`), **Copy libraries** from each HorizonXI / Ashita install found, or any folder:
  `xi-setup addons FOLDER`), **HD textures** (`xi-dat hdtex build` for chosen zones, the pending ones,
  or all) and **Install the upscaler**. Output shows live; Stop ends a running tool. Display & game > Ashita addons
  "off" writes `ashita_dir = off`; "on" with an empty folder uses the game folder.
- **Game log:** the client's output, live, with its notable warnings (no DLSS, no ray tracing, no game data, login
  failures) above it.

## Where things go

| What | Where |
|---|---|
| Profiles and settings | `~/.config/ffxi-linux/launcher.ini` (`[launcher]`, `[global]`, `[profile N]`; never a password) |
| Passwords (optional) | The desktop keyring (Secret Service via `secret-tool`), keyed by host and account; otherwise only for the session |
| A profile's client.ini settings | `~/.config/ffxi-linux/launcher/profile-N.ini`, passed with `--config` (applied after client.ini) |
| The client's output | `~/.config/ffxi-linux/launcher/client.log` (the client keeps running if the launcher closes) |

The password reaches the client in its environment (`XI_PASS`), never on its command line, where any process listing
could read it. With a password the client logs in by itself (`--autoconnect`); without one it shows its own form.

## For the command line

New xi-client options the launcher uses, available to anyone:

| Option | Meaning |
|---|---|
| `--capabilities` | Print JSON of what this build / GPU / machine can do (DLSS, FSR, ray queries, GPU, driver, missing features, game data, version, HD cache) and exit |
| `--auth-port N`, `--data-port N`, `--view-port N`, `--search-port N` | The server's ports (LSB's defaults otherwise) |
| `XI_PASS` (environment) | The password, when `--pass` is not given |

## Code

- `src/launcher/profile.*`: profiles, settings, presets, the flag builder (`buildArgs`, `graphicsArgs`), the client.ini
  overlay, launcher.ini reading / writing. Unit tests: `tests/test_launcher.cpp`.
- `src/launcher/process.*`: running the client and tools (posix_spawn, own process group, output to a pipe or a log
  file read back live), `runCapture`, the keyring.
- `apps/xi-launcher/main.cpp`: the window (SDL3's renderer + ImGui, HiDPI via `SDL_SetRenderScale`), the pages, test
  hooks (`--profile`, `--page`, `--preset`, `--connect`, `--screenshot FILE --after SEC`, `--quit-after SEC`;
  `XI_LAUNCHER_DEBUG=1` prints the window / framebuffer sizes).
- `src/gfx/capabilities.*`: the GPU probe behind `--capabilities` (a throw-away Vulkan instance; no window).

## Tested (2026-10-22)

Live on this machine (RTX 4090, Wayland, display 3): every page drawn at HiDPI; a profile for a test character with the
**Enhanced** preset connected with one click: the client got `--rt-boost --post` (its log: "rt: strength 1.30, shadow
cap 0.85 ...", "post: hdr bloom godrays fog"), logged in by itself with the password from the environment, zoned into
Southern San d'Oria, and the launcher followed its log until it exited. No password appeared in launcher.ini, the
overlay, the log or the command line. Not tried live: saving to the keyring (this machine's wallet is often locked;
the launcher then keeps the password for the session only), the Tools page's import and HD builds through the UI
(the same commands run fine from a shell).
