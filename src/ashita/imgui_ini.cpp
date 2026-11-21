// SPDX-License-Identifier: GPL-3.0-or-later
#include "ashita/imgui_ini.h"

#include <imgui.h>

namespace xi::ashita
{

std::filesystem::path imguiIniPath(const std::filesystem::path& ashitaRoot)
{
    return ashitaRoot.empty() ? std::filesystem::path() : ashitaRoot / "config" / "imgui.ini";
}

void ImGuiIniFile::use(const std::filesystem::path& file)
{
    const std::string next = file.string();
    if (next == path_)
    {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (!path_.empty())
    {
        ImGui::SaveIniSettingsToDisk(path_.c_str());
    }
    path_          = next;
    io.IniFilename = path_.empty() ? nullptr : path_.c_str();
    if (!path_.empty())
    {
        ImGui::LoadIniSettingsFromDisk(path_.c_str()); // a missing file is fine: ImGui starts empty and creates it
    }
}

} // namespace xi::ashita
