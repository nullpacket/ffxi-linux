// SPDX-License-Identifier: GPL-3.0-or-later
// xi-launcher's model (goal 44): presets -> flags, profiles <-> launcher.ini, the client.ini overlay.
#include "launcher/profile.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

using namespace xi::launcher;

namespace
{

std::string joined(const std::vector<std::string>& v)
{
    std::string s;
    for (const auto& x : v)
    {
        s += (s.empty() ? "" : " ") + x;
    }
    return s;
}

bool has(const std::vector<std::string>& v, const std::string& x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

} // namespace

TEST_CASE("launcher: presets give the guide's flags", "[launcher]")
{
    // docs/graphics-guide.md "Recommended combinations".
    CHECK(joined(graphicsArgs(presetGraphics(Preset::Original))).empty());
    CHECK(joined(graphicsArgs(presetGraphics(Preset::OriginalSharp))) == "--dlss=dlaa");
    CHECK(joined(graphicsArgs(presetGraphics(Preset::Subtle))) == "--rt");
    CHECK(joined(graphicsArgs(presetGraphics(Preset::Enhanced))) == "--rt-boost --post");
    CHECK(joined(graphicsArgs(presetGraphics(Preset::Dramatic))) == "--rt-boost=max --post --post-bloom=3 --post-shafts=3 --post-fog=3");
    CHECK(joined(graphicsArgs(presetGraphics(Preset::Remaster))) == "--dlss --post --pathtrace --hd-textures");
    CHECK(joined(graphicsArgs(presetGraphics(Preset::RemasterHalf))) == "--dlss --post --pathtrace --pathtrace-blend=0.5");
    for (const Preset p : kPresets)
    {
        const auto m = matchPreset(presetGraphics(p));
        REQUIRE(m.has_value());
        CHECK(*m == p);
    }
    Graphics custom = presetGraphics(Preset::Enhanced);
    custom.postFogStrength = 2;
    CHECK_FALSE(matchPreset(custom).has_value());
    // "FSR 3.1 always" (#139): the presets' mode with --fsr, and the preset is still the preset.
    Graphics fsr;
    fsr.upscaler = "fsr";
    CHECK(joined(graphicsArgs(presetGraphics(Preset::Remaster, fsr))) == "--fsr --post --pathtrace --hd-textures");
    CHECK(joined(graphicsArgs(presetGraphics(Preset::OriginalSharp, fsr))) == "--fsr=dlaa");
    CHECK(matchPreset(presetGraphics(Preset::Remaster, fsr)) == Preset::Remaster);
}

TEST_CASE("launcher: graphics flags for partial choices", "[launcher]")
{
    Graphics g;
    g.rt            = true;
    g.rtGi          = true;
    g.rtReflections = true;
    CHECK(joined(graphicsArgs(g)) == "--rt=all");
    g.rtAo = false;
    CHECK(joined(graphicsArgs(g)) == "--rt=shadows,gi,reflections");
    g = {};
    g.post        = true;
    g.postGodrays = false;
    g.postFog     = false;
    CHECK(joined(graphicsArgs(g)) == "--post=hdr,bloom");
    g            = {};
    g.pathtrace  = true;
    g.ptBounces  = 3;
    g.ptExposure = -0.5f;
    CHECK(joined(graphicsArgs(g)) == "--pathtrace=3 --pathtrace-exposure=-0.5");
    g             = {};
    g.rt          = true;
    g.rtStrength  = 1.2f;
    g.rtShadowCap = 0.6f;
    CHECK(joined(graphicsArgs(g)) == "--rt --rt-strength=1.2 --rt-shadow-cap=0.6");
    g            = {};
    g.hdTextures = true;
    g.hdDir      = "/data/hd";
    CHECK(joined(graphicsArgs(g)) == "--hd-textures=/data/hd");
}

TEST_CASE("launcher: a profile's command line", "[launcher]")
{
    Profile p;
    p.host      = "192.168.1.20";
    p.authPort  = 55231;
    p.user      = "bot3";
    p.character = "Botthree";
    Settings s;
    s.graphics          = presetGraphics(Preset::Enhanced);
    s.display.width     = 1280;
    s.display.height    = 720;
    s.display.display   = "2";
    s.display.windowSkin = 6;
    s.extraArgs         = "--label-range 60 --say \"hello there\"";
    const auto a = buildArgs(p, s, "/tmp/overlay.ini", true);
    CHECK(joined(a) == "--host 192.168.1.20 --auth-port 55231 --config /tmp/overlay.ini --user bot3 --char Botthree --autoconnect "
                       "--res 1280x720 --display 2 --window-skin 6 --rt-boost --post --label-range 60 --say hello there");
    CHECK(has(a, "hello there")); // one argument
    // Never a password on the command line, and no --autoconnect without one.
    const auto b = buildArgs(p, s, {}, false);
    CHECK_FALSE(has(b, "--autoconnect"));
    CHECK_FALSE(has(b, "--pass"));
    // No account: the client's own login form.
    Profile anon;
    CHECK(joined(buildArgs(anon, Settings{})) == "--host 127.0.0.1");
    // Lobby choice: --char "".
    p.character = "";
    const auto c = buildArgs(p, s, {}, true);
    const auto it = std::find(c.begin(), c.end(), "--char");
    REQUIRE(it != c.end());
    CHECK((it + 1)->empty());
    // The server rename list (#138): only when the profile asks for it.
    CHECK_FALSE(has(c, "--entity-renames"));
    p.entityRenames = true;
    CHECK(has(buildArgs(p, s, {}, true), "--entity-renames"));
}

TEST_CASE("launcher: client.ini overlay", "[launcher]")
{
    Settings s;
    CHECK(overlayIni(s).empty());
    s.game.soundVolume = 40;
    s.game.addons      = 0;
    const auto off     = overlayIni(s);
    CHECK(off.find("[ffxi-linux]") != std::string::npos);
    CHECK(off.find("sound_volume = 40") != std::string::npos);
    CHECK(off.find("ashita_dir = off\n") != std::string::npos);
    s.game.addons        = 1;
    s.game.ashitaDir     = "/home/me/ashita";
    s.game.ashitaVersion = "horizonxi";
    s.game.ashitaLoad    = "distance,fps";
    const auto on        = overlayIni(s);
    CHECK(on.find("ashita_dir = /home/me/ashita") != std::string::npos);
    CHECK(on.find("ashita_version = horizonxi") != std::string::npos);
    CHECK(on.find("ashita_load = distance,fps") != std::string::npos);
}

TEST_CASE("launcher: launcher.ini round trip", "[launcher]")
{
    LauncherConfig c;
    c.global.graphics = presetGraphics(Preset::Dramatic);
    c.global.display.mode = 3;
    Profile a;
    a.name = "Local LSB";
    a.user = "bot3";
    a.character = "Botthree";
    a.savePassword = true;
    Profile b;
    b.name        = "Friend's server";
    b.host        = "xi.example.org";
    b.viewPort    = 54011;
    b.ownSettings = true;
    b.entityRenames                 = true;
    b.settings.graphics           = presetGraphics(Preset::RemasterHalf);
    b.settings.graphics.rtShadowCap = 0.55f;
    b.settings.graphics.upscaler    = "fsr";
    b.settings.game.addons        = 1;
    b.settings.game.ashitaDir     = "/x/y z";
    b.settings.extraArgs          = "--label-range 50";
    c.profiles       = { a, b };
    c.defaultProfile = 1;
    c.clientPath     = "/opt/ffxi-linux/bin/xi-client";
    const auto text  = toIni(c);
    CHECK(text.find("\npassword") == std::string::npos); // never a password field (only save_password, a flag)
    CHECK(text.find("\npass ") == std::string::npos);
    const auto back = fromIni(text);
    REQUIRE(back.profiles.size() == 2);
    CHECK(back.defaultProfile == 1);
    CHECK(back.clientPath == c.clientPath);
    CHECK(back.global == c.global);
    CHECK(back.profiles[0].name == "Local LSB");
    CHECK(back.profiles[0].savePassword);
    CHECK_FALSE(back.profiles[0].ownSettings);
    CHECK(back.profiles[1].host == "xi.example.org");
    CHECK(back.profiles[1].viewPort == 54011);
    CHECK(back.profiles[1].entityRenames);
    CHECK_FALSE(back.profiles[0].entityRenames); // off by default: LSB doesn't answer C2S 0x001
    CHECK(back.profiles[1].settings == b.settings);
    CHECK(effectiveSettings(back, back.profiles[0]) == back.global);
    CHECK(effectiveSettings(back, back.profiles[1]) == back.profiles[1].settings);
    // A missing / empty file: one default profile.
    const auto empty = fromIni("");
    REQUIRE(empty.profiles.size() == 1);
    CHECK(empty.profiles[0].host == "127.0.0.1");
    // Save / load through a file.
    const auto file = std::filesystem::temp_directory_path() / "xi-launcher-test" / "launcher.ini";
    REQUIRE(saveConfig(c, file));
    CHECK(loadConfig(file).profiles[1].settings == b.settings);
    std::filesystem::remove_all(file.parent_path());
}

TEST_CASE("launcher: splitting extra arguments", "[launcher]")
{
    CHECK(splitArgs("").empty());
    CHECK(splitArgs("  --a  b ") == std::vector<std::string>{ "--a", "b" });
    CHECK(splitArgs("--say \"two words\" x") == std::vector<std::string>{ "--say", "two words", "x" });
    CHECK(splitArgs("--char \"\"") == std::vector<std::string>{ "--char", "" });
}
