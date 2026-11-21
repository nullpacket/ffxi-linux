// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

namespace xi::ashita
{

// Where ImGui keeps window settings (position, size, collapsed state) for addons: <ashita root>/config/imgui.ini, the
// file Ashita v4 writes (HorizonXI's Game/config/imgui.ini). Addons such as mobdb never store their own window position;
// they rely on this file to put a moved window back where it was (issue #111).
std::filesystem::path imguiIniPath(const std::filesystem::path& ashitaRoot);

// Owns the path string ImGuiIO::IniFilename points at, for the current ImGui context. use(file) saves the settings to the
// previous file (if any), points ImGui at `file` and loads it, so windows created after the call take their saved
// placement; an empty path turns saving off. ImGui writes the file a few seconds after a window moves and when the
// context is destroyed.
class ImGuiIniFile
{
public:
    void               use(const std::filesystem::path& file);
    const std::string& path() const
    {
        return path_;
    }

private:
    std::string path_;
};

} // namespace xi::ashita
