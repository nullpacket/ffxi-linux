// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// What the Config menu's Add-ons page (goal 47) knows about the installed Ashita addons, without loading them: each
// addon's header fields, the per-character startup list, and the settings files Ashita's settings lib writes.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace xi::ashita
{

// An addon's `addon.name / author / version / desc / link` lines (Ashita v4 addons set them at the top of the main
// file), and the release it needs: libraries only HorizonXI's Ashita ships (bitmap, color, lin, typedefs) -> "HorizonXI".
struct AddonInfo
{
    std::string           name; // the folder name (as typed for /addon load)
    std::filesystem::path dir;
    std::filesystem::path mainFile;
    std::string           author, version, description, link;
    std::string           needs; // "" (any release) or "HorizonXI"
};

// The header of one addon's main file (its text).
AddonInfo parseAddonHeader(const std::string& source);
// Every folder under `root`/addons with a main file (<name>.lua, any case), sorted by name, case-insensitively.
std::vector<AddonInfo> scanAddons(const std::filesystem::path& root);

// The addons a character starts with: one name a line, `#` starts a comment, blank lines skipped (the file is
// ~/.config/ffxi-linux/addons/<Character>.txt; the client reads it after the boot script).
std::vector<std::string> readStartupList(const std::filesystem::path& file);
bool writeStartupList(const std::filesystem::path& file, const std::vector<std::string>& names);

// One value of a settings file written by Ashita's settings lib (`settings["font"]["font_height"] = 12;`).
struct SettingField
{
    enum class Type
    {
        Bool,
        Number,
        String,
        Other, // tables (T{ ... } with values), nil, expressions: shown, not editable
    };
    std::vector<std::string> path;  // keys from the top: { "font", "font_height" }
    Type                     type = Type::Other;
    std::string              value; // as written: 12, 4294901760, true, "Arial" (quoted, escapes kept)
    std::size_t              line = 0; // 0-based line in the file
    [[nodiscard]] std::string key() const; // "font.font_height"
    // The value as shown: strings unquoted; numbers named like a colour as 0xAARRGGBB.
    [[nodiscard]] std::string display() const;
};
std::vector<SettingField> readSettings(const std::filesystem::path& file);
// Writes `value` (a literal of the field's type: a number, true / false, or a string, quoted here) into its line and
// saves the file; false when the field's line no longer matches.
bool writeSetting(const std::filesystem::path& file, const SettingField& field, const std::string& value);
// The settings files of an addon for a character, by name: <root>/config/addons/<addon>/<Char>_<id>/*.lua, then
// the defaults folder's.
std::vector<std::filesystem::path> settingsFiles(const std::filesystem::path& root, const std::string& addon, const std::string& character,
                                                 uint32_t charId);

} // namespace xi::ashita
