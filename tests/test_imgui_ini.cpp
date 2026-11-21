// SPDX-License-Identifier: GPL-3.0-or-later
#include "ashita/imgui_ini.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{

// A headless ImGui context, as test_ashita.cpp makes: no GPU, textures answered after each frame.
ImGuiContext* headless()
{
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO&      io  = ImGui::GetIO();
    io.DisplaySize    = ImVec2(1280, 720);
    io.DeltaTime      = 1.0f / 60.0f;
    io.IniFilename    = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetPlatformIO().Renderer_TextureMaxWidth  = 4096;
    ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
    return ctx;
}

void endFrame()
{
    ImGui::Render();
    for (ImTextureData* t : ImGui::GetPlatformIO().Textures)
    {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates)
        {
            t->SetTexID(static_cast<ImTextureID>(1));
            t->SetStatus(ImTextureStatus_OK);
        }
        else if (t->Status == ImTextureStatus_WantDestroy)
        {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

// One frame of mobdb's info bar: it never sets a position itself, so where it opens is ImGui's saved setting or the
// default; `moveTo` stands in for the user dragging it. Returns where the window is.
ImVec2 infoBarFrame(const ImVec2* moveTo = nullptr)
{
    ImGui::NewFrame();
    ImGui::Begin("mobdb_infobar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("Goblin Thug Lv.12");
    if (moveTo)
    {
        ImGui::SetWindowPos(*moveTo);
    }
    const ImVec2 pos = ImGui::GetWindowPos();
    ImGui::End();
    endFrame();
    return pos;
}

std::string slurp(const fs::path& p)
{
    std::ifstream     in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("imgui ini: addon windows live in <ashita root>/config/imgui.ini, as Ashita keeps them")
{
    CHECK(xi::ashita::imguiIniPath("/game") == fs::path("/game/config/imgui.ini"));
    CHECK(xi::ashita::imguiIniPath({}).empty());
}

TEST_CASE("imgui ini: a moved addon window opens where it was left after a restart (#111)")
{
    const fs::path dir = fs::temp_directory_path() / "xi_test_imgui_ini";
    fs::remove_all(dir);
    fs::create_directories(dir / "config");
    const fs::path file = xi::ashita::imguiIniPath(dir);
    const ImVec2   moved(612, 404);

    {
        xi::ashita::ImGuiIniFile ini;
        ImGuiContext*            ctx = headless();
        ini.use(file);
        const ImVec2 first = infoBarFrame();
        CHECK((first.x != moved.x || first.y != moved.y));
        infoBarFrame(&moved);
        infoBarFrame();
        ImGui::DestroyContext(ctx); // ImGui saves to IniFilename on shutdown, as the client's exit does
    }
    REQUIRE(fs::exists(file));
    CHECK(slurp(file).find("[Window][mobdb_infobar]\nPos=612,404") != std::string::npos);

    {
        xi::ashita::ImGuiIniFile ini;
        ImGuiContext*            ctx = headless();
        ini.use(file);
        const ImVec2 again = infoBarFrame();
        CHECK(again.x == moved.x);
        CHECK(again.y == moved.y);
        ImGui::DestroyContext(ctx);
    }
    fs::remove_all(dir);
}

TEST_CASE("imgui ini: switching Ashita folders saves to the old file and loads the new one")
{
    const fs::path dir = fs::temp_directory_path() / "xi_test_imgui_ini_switch";
    fs::remove_all(dir);
    fs::create_directories(dir / "a" / "config");
    fs::create_directories(dir / "b" / "config");

    xi::ashita::ImGuiIniFile ini;
    ImGuiContext*            ctx = headless();
    ini.use(xi::ashita::imguiIniPath(dir / "a"));
    const ImVec2 moved(321, 123);
    infoBarFrame(&moved);
    ini.use(xi::ashita::imguiIniPath(dir / "b"));
    CHECK(slurp(dir / "a" / "config" / "imgui.ini").find("Pos=321,123") != std::string::npos);
    CHECK(ImGui::GetIO().IniFilename == ini.path().c_str());
    ini.use({});
    CHECK(ImGui::GetIO().IniFilename == nullptr);
    ImGui::DestroyContext(ctx);
    fs::remove_all(dir);
}
