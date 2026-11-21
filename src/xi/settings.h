// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// Client display settings, using FFXI's own registry keys so an Ashita boot file works as-is: the [ffxi.registry]
// section of e.g. config/boot/horizonxi.ini ("0001 = 1920"; -1 = default). Keys this client reads:
//   0001/0002  window width/height
//   0003/0004  background resolution: the 3D scene renders at this size and is scaled to the window
//   0034       window mode: 0 fullscreen, 1 windowed, 2 fullscreen windowed (borderless), 3 borderless window
//   0037/0038  menu resolution: the interface canvas, scaled to the window (larger = smaller menus)
// and, in an [ffxi-linux] section, settings of this client:
//   window_skin  the window type 1..8 (in the game, a Config option)
//   keyboard     keyboard layout: 0 Normal (numeric keypad), 1 Compact (no keypad: WASD/IJKL), 2 Compact 2 (+ Q/E turn);
//                the game's config id 66
//   sound_volume sound effects 0..100 (0 = off), music_volume background music 0..100 (0 = off)
namespace xi
{

struct DisplaySettings
{
    int windowWidth  = 1600;
    int windowHeight = 900;
    int bgWidth      = 0; // 0 = the window size
    int bgHeight     = 0;
    int menuWidth    = 0; // 0 = two thirds of the window (menus at 1.5x)
    int menuHeight   = 0;
    int windowMode   = 1;
    int windowSkin   = 1; // [ffxi-linux] window_skin: the game's window type 1..8 (frame and background style)
    int keyboard     = 0; // [ffxi-linux] keyboard: 0 Normal, 1 Compact, 2 Compact 2 (ui::KeyLayout)
    int soundVolume  = 80; // [ffxi-linux] sound_volume 0..100
    int musicVolume  = 50; // [ffxi-linux] music_volume 0..100
    std::string loginProtocol; // [ffxi-linux] login_protocol: auto / 2.1 / 2.2 (empty = the default)
    // [ffxi-linux] Ashita addons (docs/ashita-compat.md): ashita_dir (an Ashita install: addons/, config/),
    // ashita_version (auto / latest / horizonxi), ashita_load (addons, comma-separated), ashita_script (a boot script
    // whose `/addon load` lines run).
    std::string ashitaDir, ashitaVersion, ashitaLoad, ashitaScript;
    // [ffxi-linux] macro_dir: where the macro files (the game's mcr*.dat / mcr.ttl / mcr.sys) live. Empty = our own
    // copy in ~/.config/ffxi-linux/USER/<id> (imported once from the install's USER/<id>); "install" = the install's
    // USER/<id> itself, shared with the Windows client; anything else = that folder (its USER/<id>).
    std::string macroDir;
    // [ffxi-linux] /bugreport (goal 48; private builds): bugreport_url (the GitLab, "none" = keep reports local),
    // bugreport_project (path or id), bugreport_token_file, bugreport_chat (0 = leave players' chat out). Empty = the
    // bug reporter's defaults.
    std::string bugreportUrl, bugreportProject, bugreportTokenFile, bugreportChat;
    // [ffxi-linux] cfg_<id>: the game's other in-game Config values by config id (docs/ui-format-notes.md §8)
    std::map<int, int> gameConfig;

    // Applies the [ffxi.registry] keys of an INI file; keys absent or -1 keep their value. Throws on unreadable files.
    void loadIni(const std::filesystem::path& file);
    // Applies one "WxH" value to a width/height pair; false if malformed.
    static bool parseSize(std::string_view text, int& w, int& h);

    // The interface canvas for a window of `w` x `h` logical pixels: authored units per screen pixel and the canvas
    // size in authored units. The game scales x and y separately; one factor (from the height) keeps sprites square.
    struct Canvas
    {
        float width = 0, height = 0, scale = 1;
    };
    [[nodiscard]] Canvas canvasFor(float w, float h) const;

    // Default config file: $XDG_CONFIG_HOME/ffxi-linux/client.ini (or ~/.config/...), if it exists.
    static std::optional<std::filesystem::path> defaultFile();
    // Where in-game Config changes are saved: the default config file's path, whether or not it exists yet.
    static std::optional<std::filesystem::path> userFile();
    // Sets `key = value` in the [ffxi-linux] section of an INI file, keeping every other line; creates the file,
    // the section or the key as needed. False if the file cannot be written.
    static bool saveOwn(const std::filesystem::path& file, std::string_view key, int value);
    static bool saveOwnText(const std::filesystem::path& file, std::string_view key, std::string_view value);
};

} // namespace xi
