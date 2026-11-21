// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The client's own Ashita folder (issue #73): Ashita addons live in an `addons` folder inside the game folder (normally
// our imported copy, ~/.local/share/ffxi-linux/game), laid out as an Ashita install so addons find their files:
//   addons/            one folder per addon (addons/<name>/<name>.lua)
//   addons/libs/       Ashita's shared Lua libraries (settings, imgui, common, ffxi, d3d8, gdifonts ...)
//   config/addons/     the addons' saved settings (Ashita's settings lib writes there)
//   scripts/           boot scripts (`/addon load` lines)
// client.ini [ffxi-linux] ashita_dir and XI_ASHITA_DIR still point elsewhere when set. The libraries and the standard
// addons come with the client (installBundled, #138); `xi-setup addons FROM` copies them from an Ashita install instead.
namespace xi::ashitadir
{

// The Ashita folder to use: XI_ASHITA_DIR, else `configured` (client.ini ashita_dir), else the game folder. "off" (or
// "none") in either turns addons off: an empty path.
std::filesystem::path effectiveRoot(const std::string& configured, const std::filesystem::path& gameDir);

// Create the folders above under `root` (existing ones are kept). Returns "" or what failed.
std::string ensureLayout(const std::filesystem::path& root);

// Whether `root`/addons/libs holds Ashita's shared libraries (settings.lua and common.lua).
bool libsPresent(const std::filesystem::path& root);

// A line telling the user how to get the libraries, naming `root`.
std::string libsMissingHint(const std::filesystem::path& root);

struct FoundAshita
{
    std::filesystem::path dir;   // the folder holding addons/ (an Ashita install or HorizonXI's Game folder)
    std::string           label; // "HorizonXI", "Ashita v4" ...
};

// Ashita installs on this machine with addons/libs/settings.lua: HorizonXI's Game folder and Ashita folders in Wine
// prefixes under ~/.wine and ~/Games (one or two levels down), and ~/git/Ashita-v4*; `exclude` (our own root) is left out.
std::vector<FoundAshita> detectAshitaInstalls(const std::filesystem::path& exclude = {});

// The folder holding addons/libs inside `from`: `from` itself, its Game folder, or a Wine prefix holding one.
std::filesystem::path ashitaFolderIn(const std::filesystem::path& from);

struct CopyResult
{
    std::uint64_t libFiles = 0;           // files written under addons/libs (new or changed)
    std::uint64_t sdkFiles = 0; // files written under plugins/sdk (the headers native plugin builds need, #34)
    std::vector<std::string> addons;      // addons copied
    std::vector<std::string> skipped;     // addons already present (never overwritten) or missing in `from`
};

// Copy `from`/addons/libs into `root`/addons/libs (new and changed files; the libraries are the install's, so ours follow
// it), the same for `from`/plugins/sdk (Ashita's plugin SDK, whose headers the build reads to compile plugins natively),
// then each of `addons` from `from`/addons/<name> when `root` doesn't have that addon yet (never overwriting an addon
// the user already has). Creates the layout first. Throws on I/O errors.
CopyResult copyFromInstall(const std::filesystem::path& from, const std::filesystem::path& root,
                           const std::vector<std::string>& addons = {});

// The bundled addons (third_party/ashita-addons, #138): the standard Ashita v4 addons and libraries that work in the
// client, the only addons ffxi-linux ships. The folder holding their addons/: XI_ASHITA_BUNDLE, else
// <exe>/../share/ffxi-linux/ashita-addons, the configured install prefix's, then the source tree's. Empty when none of
// those has addons/libs/settings.lua.
std::filesystem::path bundledDir();

// Install the bundle at `bundle` (bundledDir()) into `root`: every library file `root` doesn't have yet, and every
// bundled addon whose folder `root` doesn't have and that an earlier install didn't add (addons/.bundled lists those, so
// an addon the user deleted stays deleted). Never overwrites a file. Creates the layout first. Throws on I/O errors.
CopyResult installBundled(const std::filesystem::path& bundle, const std::filesystem::path& root);

} // namespace xi::ashitadir
