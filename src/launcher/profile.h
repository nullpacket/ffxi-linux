// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// xi-launcher's model (goal 44): server profiles, the settings each starts the client with (or the global defaults),
// the graphics presets of docs/graphics-guide.md, and how all of it becomes xi-client's command line and a client.ini
// overlay. Everything maps onto existing flags / client.ini keys, so the command line keeps working without it.
namespace xi::launcher
{

struct Graphics
{
    std::string dlss;                 // "" off, else quality / balanced / performance / ultra-performance / dlaa
    std::string upscaler; // "" DLSS on an NVIDIA RTX GPU, else FSR (the client falls back itself); "fsr" FSR always
    bool        rt = false;           // --rt
    bool        rtAo = true, rtShadows = true, rtGi = false, rtReflections = false; // --rt=... (ao,shadows = plain --rt)
    std::optional<float> rtStrength;  // --rt-strength= (unset = the client's default / the boost preset's)
    std::optional<float> rtShadowCap; // --rt-shadow-cap=
    std::optional<float> rtReflect;   // --rt-reflect=
    std::string rtBoost;              // "" off, low / high / max
    bool        post = false;         // --post
    bool        postHdr = true, postBloom = true, postGodrays = true, postFog = true; // --post=... (all four = plain --post)
    float       postBloomStrength = 1, postShaftsStrength = 1, postFogStrength = 1;   // --post-bloom= / -shafts= / -fog=
    bool        pathtrace = false;    // --pathtrace
    int         ptBounces = 2;        // --pathtrace=1..3
    float       ptBlend = 1, ptExposure = 0, ptBaked = 0.3f;
    bool        hdTextures = false;   // --hd-textures
    std::string hdDir;                // --hd-textures=DIR ("" = the default cache)

    bool operator==(const Graphics&) const = default;
};

struct Display
{
    int         width = 0, height = 0;     // --res (0 = client.ini / the default)
    std::string display;                   // --display: "" (client's default), primary, secondary or an index
    int         mode = -1;                 // --window-mode 0..3 (-1 = client.ini)
    int         bgWidth = 0, bgHeight = 0; // --bg-res (0 = the window size)
    int         menuWidth = 0, menuHeight = 0; // --menu-res
    int         windowSkin = 0;            // --window-skin 1..8 (0 = client.ini)

    bool operator==(const Display&) const = default;
};

// Settings written to the profile's client.ini overlay (passed with --config); -1 / "" = leave client.ini's value.
struct GameOptions
{
    int         soundVolume = -1, musicVolume = -1; // [ffxi-linux] sound_volume / music_volume 0..100
    int         keyboard = -1;                       // [ffxi-linux] keyboard 0..2
    std::string loginProtocol;                       // [ffxi-linux] login_protocol auto / 2.1 / 2.2
    int         addons = -1;                         // Ashita addons: -1 as client.ini, 0 off (ashita_dir cleared), 1 on
    std::string ashitaDir, ashitaVersion, ashitaLoad; // [ffxi-linux] ashita_dir / ashita_version / ashita_load
    std::string macroDir;                            // [ffxi-linux] macro_dir

    bool operator==(const GameOptions&) const = default;
};

struct Settings
{
    Graphics    graphics;
    Display     display;
    GameOptions game;
    std::string extraArgs; // anything else, appended as typed (split on spaces; "quotes" group)

    bool operator==(const Settings&) const = default;
};

struct Profile
{
    std::string name = "Local server";
    std::string host = "127.0.0.1";
    uint16_t    authPort = 54231, dataPort = 54230, viewPort = 54001, searchPort = 54002;
    std::string user;
    std::string character;     // "" = choose in the lobby
    std::string gameDir;       // --root ("" = FFXI_ROOT / the imported copy / the Wine prefix, as the client finds it)
    bool        savePassword = false; // the password lives in the desktop keyring (Secret Service), never in the file
    bool        ownSettings = false;  // false = the global settings
    // Ask the server for its dynamic-entity rename list after each zone-in (--entity-renames; HorizonXI's S2C 0x1FF,
    // client/entity_names.h). Off by default: LSB does not handle the request (C2S 0x001).
    bool        entityRenames = false;
    Settings    settings;
};

struct LauncherConfig
{
    std::vector<Profile> profiles;
    int                  defaultProfile = 0;
    Settings             global;
    std::string          clientPath; // xi-client to run ("" = beside the launcher, then the build dir, then PATH)
};

// The guide's presets (docs/graphics-guide.md "Recommended combinations").
enum class Preset
{
    Original,
    OriginalSharp,
    Subtle,
    Enhanced,
    Dramatic,
    Remaster,
    RemasterHalf,
};
inline constexpr Preset kPresets[] = { Preset::Original, Preset::OriginalSharp, Preset::Subtle, Preset::Enhanced,
                                       Preset::Dramatic, Preset::Remaster,      Preset::RemasterHalf };
[[nodiscard]] const char* presetName(Preset p);
[[nodiscard]] const char* presetDescription(Preset p);
// The preset's graphics, everything else at its default (the HD texture folder is kept).
[[nodiscard]] Graphics presetGraphics(Preset p, const Graphics& keepFrom = {});
// The preset these graphics are exactly, if any.
[[nodiscard]] std::optional<Preset> matchPreset(const Graphics& g);

// The client's command line for a profile (without the program name, never with the password: that goes in XI_PASS).
// `overlayIni` is the client.ini overlay's path, passed with --config when the settings write one. With a password
// at hand (and an account) the client logs in by itself (--autoconnect).
[[nodiscard]] std::vector<std::string> buildArgs(const Profile& p, const Settings& s, const std::filesystem::path& overlayIni = {},
                                                 bool havePassword = false);
// Just the graphics flags.
[[nodiscard]] std::vector<std::string> graphicsArgs(const Graphics& g);
// The client.ini overlay's text for these settings ("" when nothing differs from client.ini).
[[nodiscard]] std::string overlayIni(const Settings& s);
// The settings a profile starts with: its own or the global ones.
[[nodiscard]] const Settings& effectiveSettings(const LauncherConfig& c, const Profile& p);

// Splits "a b \"c d\"" the way a shell would for plain words and double quotes.
[[nodiscard]] std::vector<std::string> splitArgs(std::string_view text);

// ~/.config/ffxi-linux/launcher.ini ($XDG_CONFIG_HOME respected).
[[nodiscard]] std::filesystem::path defaultConfigPath();
// INI text <-> config. Unknown keys are ignored; a missing file gives one default profile.
[[nodiscard]] std::string toIni(const LauncherConfig& c);
[[nodiscard]] LauncherConfig fromIni(std::string_view text);
[[nodiscard]] LauncherConfig loadConfig(const std::filesystem::path& file);
bool saveConfig(const LauncherConfig& c, const std::filesystem::path& file);

} // namespace xi::launcher
