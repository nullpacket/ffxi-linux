// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/settings.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("display settings read an Ashita boot file's [ffxi.registry] keys")
{
    const auto path = std::filesystem::temp_directory_path() / "xi_settings_test.ini";
    {
        std::ofstream f(path);
        f << "[ashita.window.startpos]\nx = -1\n\n"
             "[ffxi.registry]\n0000 = 6\n0001 = 1920\n0002 = 1080\n0003 = 4096\n0004 = 4096\n0034 = 1\n"
             "0037 = 1280\n0038 = -1\n0042 = C:\\Program Files (x86)\\SquareEnix\\FINAL FANTASY XI\n"
             "[sandbox.paths]\n0001 = 7\n[ffxi-linux]\nwindow_skin = 3\nkeyboard = 1\nsound_volume = 0\nmusic_volume = 250\n";
    }
    xi::DisplaySettings s;
    s.loadIni(path);
    std::filesystem::remove(path);
    CHECK(s.windowWidth == 1920);
    CHECK(s.windowHeight == 1080);
    CHECK(s.bgWidth == 4096);
    CHECK(s.bgHeight == 4096);
    CHECK(s.windowMode == 1);
    CHECK(s.menuWidth == 1280);
    CHECK(s.menuHeight == 0); // -1 keeps the default
    CHECK(s.windowSkin == 3);
    CHECK(s.keyboard == 1);
    CHECK(s.soundVolume == 0);   // off
    CHECK(s.musicVolume == 100); // clamped

    int w = 0, h = 0;
    CHECK(xi::DisplaySettings::parseSize("2560x1440", w, h));
    CHECK(w == 2560);
    CHECK(h == 1440);
    CHECK_FALSE(xi::DisplaySettings::parseSize("2560", w, h));

    // Menu resolution 1280x720 in a 1920x1080 window: menus at 1.5x, canvas 1280x720.
    s.menuHeight = 720;
    const auto c = s.canvasFor(1920, 1080);
    CHECK(c.scale == 1.5f);
    CHECK(c.width == 1280.0f);
    CHECK(c.height == 720.0f);
}

TEST_CASE("saveOwn sets one [ffxi-linux] key and keeps the rest of the file")
{
    const auto path = std::filesystem::temp_directory_path() / "xi_settings_save.ini";
    {
        std::ofstream f(path);
        f << "; comment\n[ffxi.registry]\n0001 = 1920\n\n[ffxi-linux]\nwindow_skin = 2\n\n[other]\nkeyboard = 9\n";
    }
    REQUIRE(xi::DisplaySettings::saveOwn(path, "window_skin", 5));
    REQUIRE(xi::DisplaySettings::saveOwn(path, "keyboard", 1));
    xi::DisplaySettings s;
    s.loadIni(path);
    CHECK(s.windowWidth == 1920);
    CHECK(s.windowSkin == 5);
    CHECK(s.keyboard == 1);
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text == "; comment\n[ffxi.registry]\n0001 = 1920\n\n[ffxi-linux]\nwindow_skin = 5\nkeyboard = 1\n\n[other]\nkeyboard = 9\n");
    std::filesystem::remove(path);
    // No file yet: created with the section.
    REQUIRE(xi::DisplaySettings::saveOwn(path, "keyboard", 1));
    xi::DisplaySettings t;
    t.loadIni(path);
    CHECK(t.keyboard == 1);
    std::filesystem::remove(path);
}
