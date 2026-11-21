// SPDX-License-Identifier: GPL-3.0-or-later
// Config -> Add-ons (goal 47): addon headers, the startup list and settings files (src/ashita/addon_info.*).
#include "ashita/addon_info.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

using namespace xi::ashita;

namespace
{

std::filesystem::path tempDir(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / (std::string("xi_addon_info_") + name);
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

void writeText(const std::filesystem::path& p, const std::string& s)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

std::string readText(const std::filesystem::path& p)
{
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

} // namespace

TEST_CASE("addon info: header fields and the HorizonXI-only libraries")
{
    const auto a = parseAddonHeader("addon.name      = 'distance';\n"
                                    "addon.author    = \"atom0s\";\n"
                                    "addon.version   = '1.0';\n"
                                    "addon.desc      = 'Displays the distance to the target.';\n"
                                    "addon.link      = 'https://ashitaxi.com/';\n"
                                    "require('common');\n");
    CHECK(a.name == "distance");
    CHECK(a.author == "atom0s");
    CHECK(a.version == "1.0");
    CHECK(a.description == "Displays the distance to the target.");
    CHECK(a.link == "https://ashitaxi.com/");
    CHECK(a.needs.empty());

    const auto h = parseAddonHeader("addon.name = 'x';\nlocal lin = require('lin.vector');\n");
    CHECK(h.needs == "HorizonXI");
    const auto esc = parseAddonHeader("addon.desc = 'It\\'s here';\n");
    CHECK(esc.description == "It's here");
}

TEST_CASE("addon info: scanning the addons folder")
{
    const auto root = tempDir("scan");
    writeText(root / "addons" / "Zeta" / "zeta.lua", "addon.name = 'zeta';\naddon.version = '2';\n");
    writeText(root / "addons" / "alpha" / "alpha.lua", "addon.name = 'alpha';\n");
    writeText(root / "addons" / "libs" / "common.lua", "return {};\n"); // no main file: not an addon
    const auto list = scanAddons(root);
    REQUIRE(list.size() == 2);
    CHECK(list[0].name == "alpha");
    CHECK(list[1].name == "Zeta"); // the folder's name, case kept
    CHECK(list[1].version == "2");
    std::filesystem::remove_all(root);
}

TEST_CASE("addon info: the per-character startup list")
{
    const auto dir  = tempDir("startup");
    const auto file = dir / "addons" / "Botone.txt";
    CHECK(readStartupList(file).empty());
    REQUIRE(writeStartupList(file, { "distance", "fps" }));
    auto l = readStartupList(file);
    REQUIRE(l.size() == 2);
    CHECK(l[0] == "distance");
    CHECK(l[1] == "fps");
    writeText(file, "# comment\n  timestamp  \n\nFPS\nfps # again\n");
    l = readStartupList(file);
    REQUIRE(l.size() == 2); // fps twice in different case: once
    CHECK(l[0] == "timestamp");
    CHECK(l[1] == "FPS");
    std::filesystem::remove_all(dir);
}

TEST_CASE("addon info: settings lib files read and written in place")
{
    const auto dir  = tempDir("settings");
    const auto file = dir / "settings.lua";
    writeText(file, "require('common');\n\n"
                    "local settings = T{ };\n"
                    "settings[\"font\"] = T{ };\n"
                    "settings[\"font\"][\"font_height\"] = 12;\n"
                    "settings[\"font\"][\"font_family\"] = \"Arial\";\n"
                    "settings[\"font\"][\"color\"] = 4294901760;\n"
                    "settings[\"font\"][\"visible\"] = true;\n"
                    "settings[\"scale\"] = 1.5;\n"
                    "settings[\"list\"] = T{ 1, 2 };\n"
                    "\nreturn settings;\n");
    auto f = readSettings(file);
    REQUIRE(f.size() == 6);
    CHECK(f[0].key() == "font.font_height");
    CHECK(f[0].type == SettingField::Type::Number);
    CHECK(f[1].type == SettingField::Type::String);
    CHECK(f[1].display() == "Arial");
    CHECK(f[2].display() == "0xFFFF0000"); // a colour
    CHECK(f[3].type == SettingField::Type::Bool);
    CHECK(f[4].value == "1.5");
    CHECK(f[5].type == SettingField::Type::Other);

    REQUIRE(writeSetting(file, f[0], "14"));
    REQUIRE(writeSetting(file, f[1], "Comic \"Sans\""));
    REQUIRE(writeSetting(file, f[3], "false"));
    f = readSettings(file);
    CHECK(f[0].value == "14");
    CHECK(f[1].display() == "Comic \"Sans\"");
    CHECK(f[3].value == "false");
    const auto text = readText(file);
    CHECK(text.find("return settings;") != std::string::npos); // the rest of the file is kept
    CHECK(text.find("require('common');") == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("addon info: settings files, the character's before the defaults")
{
    const auto root = tempDir("files");
    writeText(root / "config" / "addons" / "fps" / "defaults" / "settings.lua", "return {};\n");
    auto files = settingsFiles(root, "fps", "Botone", 2);
    REQUIRE(files.size() == 1);
    CHECK(files[0].parent_path().filename() == "defaults");
    writeText(root / "config" / "addons" / "fps" / "Botone_2" / "settings.lua", "return {};\n");
    files = settingsFiles(root, "fps", "Botone", 2);
    REQUIRE(files.size() == 1);
    CHECK(files[0].parent_path().filename() == "Botone_2");
    std::filesystem::remove_all(root);
}
