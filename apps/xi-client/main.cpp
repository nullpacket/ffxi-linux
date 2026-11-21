// SPDX-License-Identifier: GPL-3.0-or-later
// xi-client: native Linux FFXI client (early). Logs into a LandSandBoat server, renders the character's zone
// and nearby entities, moves the character and chats.
//
//   xi-client [--host H] [--user U] [--pass P] [--char NAME] [--root FFXI_DIR] [--client-ver V]
//             [--compress-dat PATH] [--validation] [--autoconnect] [--display primary|secondary|N]
//             [--entity-renames | XI_ENTITY_RENAMES=1] (ask the server for its rename list; client/entity_names.h)
//             [--dlss[=quality|balanced|performance|ultra-performance|dlaa]] (NVIDIA DLSS 4; docs/dlss.md)
//             [--fsr[=quality|balanced|performance|ultra-performance|native]] [--fsr-sharpness=0..1] (AMD FSR 3.1, any
//             GPU; docs/fsr.md) [--upscaler=dlss|fsr|auto|off] (auto: DLSS on an NVIDIA RTX GPU, else FSR)
//             [--rt[=ao,shadows,gi,reflections|all]] [--rt-strength=0..2] (ray-traced lighting; docs/raytracing.md)
//             [--rt-boost[=low|high|max]] [--rt-shadow-cap=0..1] [--rt-reflect=0..3] [--post[=hdr,bloom,godrays,fog,all]]
//             [--post-bloom=0..5] [--post-shafts=0..5] [--post-fog=0..5] (x each effect's default strength)
//             (enhanced graphics, off by default; docs/enhanced-graphics.md)
//             [--pathtrace[=1..3]] [--pathtrace-blend=0..1] [--pathtrace-exposure=EV] [--pathtrace-baked=0..1]
//                                     (path-traced lighting instead of the baked; docs/enhanced-graphics.md)
//             [--hd-textures[=DIR]] (AI-upscaled textures from the local cache; docs/enhanced-graphics.md)
//             [--login-protocol auto|2.1|2.2]  (LSB's xiloader version; also client.ini login_protocol, XI_LOGIN_PROTOCOL)
//
// Controls: the game's default keyboard bindings (src/ui/keymap.h, docs/input-format.md), Normal or Compact layout
// (--keyboard / [ffxi-linux] keyboard). Normal: numeric keypad 8/2/4/6 move, arrows turn the camera, Num - main menu,
// Enter confirms (talks to a targeted NPC; otherwise opens the chat line), Esc cancels / clears the target.
// Compact adds WASD to move, IJKL for camera and menu cursor, Y/N confirm/cancel. Both: Ctrl/Alt + A attack, + I items,
// + M magic, + J abilities, + W weapon skills, + E equipment, + C check, + S/P/T/L chat. Also: Tab cycles targets,
// F1..F6 target yourself / party members, Ctrl/Alt + 1..0 macros, Space opens the chat line, right mouse orbits the
// camera, wheel zooms, F11 debug overlay. Closing the window logs out.

#include "client_app.h"
#include "client_args.h"
#include "dat/hd_textures.h"
#include "gfx/capabilities.h"
#include "gfx/dlss.h"
#include "gfx/fsr.h"
#include "xi/install.h"
#include "xi/settings.h"

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

using namespace xi;
using xi::app::Args;

namespace
{

Args parseArgs(int argc, char** argv)
{
    Args a;
    if (const auto dir = install::findGameDir())
    {
        a.root = dir->string(); // FFXI_ROOT, else our imported copy (xi-setup import), else the default Wine prefix
    }
    if (const auto c = install::findCompressDat())
    {
        a.compressDat = c->string();
    }
    if (const char* r = std::getenv("XI_ENTITY_RENAMES"); r && std::string_view(r) == "1")
    {
        a.entityRenames = true;
    }
    if (const char* d = std::getenv("XI_DISPLAY"))
    {
        a.display = d;
    }
    if (const auto f = DisplaySettings::defaultFile())
    {
        a.display_.loadIni(*f);
    }
    for (int i = 1; i < argc; ++i)
    {
        const std::string k   = argv[i];
        auto              val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (k == "--host") a.host = val();
        else if (k == "--user") a.user = val();
        else if (k == "--pass") a.pass = val();
        else if (k == "--auth-port") a.authPort = static_cast<uint16_t>(std::stoi(val()));
        else if (k == "--data-port") a.dataPort = static_cast<uint16_t>(std::stoi(val()));
        else if (k == "--view-port") a.viewPort = static_cast<uint16_t>(std::stoi(val()));
        else if (k == "--search-port") a.searchPort = static_cast<uint16_t>(std::stoi(val()));
        else if (k == "--char") a.character = val();
        else if (k == "--root") a.root = val();
        else if (k == "--client-ver") a.clientVer = val();
        else if (k == "--login-protocol") a.display_.loginProtocol = val();
        else if (k == "--compress-dat") a.compressDat = val();
        else if (k == "--validation") a.validation = true;
        else if (k == "--dlss") a.dlss = "quality";
        else if (k.rfind("--dlss=", 0) == 0) a.dlss = k.substr(7);
        else if (k == "--fsr")
            a.fsr = "quality";
        else if (k.rfind("--fsr=", 0) == 0)
            a.fsr = k.substr(6);
        else if (k.rfind("--fsr-sharpness=", 0) == 0)
            a.fsrSharpness = std::clamp(std::strtof(k.substr(16).c_str(), nullptr), 0.0f, 1.0f);
        else if (k.rfind("--upscaler=", 0) == 0)
            a.upscaler = k.substr(11);
        else if (k == "--rt") a.rt = true, a.rtEffectsGiven = true;
        else if (k.rfind("--rt=", 0) == 0) a.rt = true, a.rtEffectsGiven = true, a.rtEffects = k.substr(5);
        else if (k.rfind("--rt-strength=", 0) == 0) a.rtStrength = std::clamp(std::strtof(k.substr(14).c_str(), nullptr), 0.0f, 2.0f), a.rtStrengthGiven = true;
        else if (k == "--rt-boost") a.rt = true, a.rtBoost = "high";
        else if (k.rfind("--rt-boost=", 0) == 0) a.rt = true, a.rtBoost = k.substr(11);
        else if (k.rfind("--rt-shadow-cap=", 0) == 0) a.rtShadowCap = std::clamp(std::strtof(k.substr(16).c_str(), nullptr), 0.0f, 1.0f);
        else if (k.rfind("--rt-reflect=", 0) == 0) a.rtReflect = std::clamp(std::strtof(k.substr(13).c_str(), nullptr), 0.0f, 3.0f);
        else if (k == "--post") a.post = "all";
        else if (k.rfind("--post=", 0) == 0) a.post = k.substr(7);
        else if (k.rfind("--post-bloom=", 0) == 0) a.postBloom = std::clamp(std::strtof(k.substr(13).c_str(), nullptr), 0.0f, 5.0f);
        else if (k.rfind("--post-shafts=", 0) == 0) a.postShafts = std::clamp(std::strtof(k.substr(14).c_str(), nullptr), 0.0f, 5.0f);
        else if (k.rfind("--post-fog=", 0) == 0) a.postFog = std::clamp(std::strtof(k.substr(11).c_str(), nullptr), 0.0f, 5.0f);
        else if (k == "--hd-textures") a.hdTextures = xi::dat::hd::defaultDir().string();
        else if (k.rfind("--hd-textures=", 0) == 0) a.hdTextures = k.substr(14);
        else if (k == "--pathtrace") a.pathtrace = true;
        else if (k.rfind("--pathtrace=", 0) == 0) a.pathtrace = true, a.ptBounces = k.substr(12);
        else if (k.rfind("--pathtrace-blend=", 0) == 0) a.ptBlend = std::clamp(std::strtof(k.substr(18).c_str(), nullptr), 0.0f, 1.0f);
        else if (k.rfind("--pathtrace-exposure=", 0) == 0) a.ptExposure = std::clamp(std::strtof(k.substr(21).c_str(), nullptr), -4.0f, 4.0f);
        else if (k.rfind("--pathtrace-baked=", 0) == 0) a.ptBaked = std::clamp(std::strtof(k.substr(18).c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "--display") a.display = val();
        else if (k == "--autoconnect") a.autoconnect = true;
        else if (k == "--entity-renames")
            a.entityRenames = true;
        else if (k == "--screenshot") a.screenshot = val();
        else if (k == "--after") a.after = std::stof(val());
        else if (k == "--walk") a.walk = std::stof(val());
        else if (k == "--say") a.say = val();
        else if (k == "--cam-dist") { a.camDist = std::stof(val()); a.camGiven = true; }
        else if (k == "--cam-pitch") { a.camPitch = std::stof(val()); a.camGiven = true; }
        else if (k == "--label-range") a.labelRange = std::stof(val());
        else if (k == "--tab") a.tab = true;
        else if (k == "--target-name") a.targetName = val();
        else if (k == "--attack") a.attackAt = std::stof(val());
        else if (k == "--chase") a.chase = true;
        else if (k == "--ws-at") a.wsAt = std::stof(val());
        else if (k == "--auto-fight") a.autoFight = true;
        else if (k == "--auto-ranged") a.autoRanged = true;
        else if (k == "--auto-fish") a.autoFish = true;
        else if (k == "--fx") a.fx = val();
        else if (k == "--equip-item") a.equipItem = std::stoi(val());
        else if (k == "--ui-open") a.uiOpen = val();
        else if (k == "--shot-after-ws") a.shotAfterWs = std::stof(val());
        else if (k == "--shot-after-popup") a.shotAfterPopup = std::stof(val());
        else if (k == "--shot-on-layer")
        {
            const std::string v = val();
            const auto        c = v.find(':');
            a.shotOnLayerOp     = std::stoi(v.substr(0, c), nullptr, 0);
            a.shotOnLayerSec    = c == std::string::npos ? 0.0f : std::stof(v.substr(c + 1));
        }
        else if (k == "--shot-after-logout") a.shotAfterLogout = std::stof(val());
        else if (k == "--shot-after-despawn") a.shotAfterDespawn = std::stof(val());
        else if (k == "--ui-keys") a.uiKeys = val();
        else if (k == "--homepoint") a.homepoint = true;
        else if (k == "--cast-at") a.castAt = std::stof(val());
        else if (k == "--talk-at") a.talkAt = std::stof(val());
        else if (k == "--script") a.script = val();
        else if (k == "--ja-at") a.jaAt = std::stof(val());
        else if (k == "--ja-id") a.jaId = std::stoi(val());
        else if (k == "--use-item") a.useItemId = std::stoi(val());
        else if (k == "--ui-sheet") a.uiSheet = val();
        else if (k == "--config") a.display_.loadIni(val());
        else if (k == "--res") DisplaySettings::parseSize(val(), a.display_.windowWidth, a.display_.windowHeight);
        else if (k == "--bg-res") DisplaySettings::parseSize(val(), a.display_.bgWidth, a.display_.bgHeight);
        else if (k == "--menu-res") DisplaySettings::parseSize(val(), a.display_.menuWidth, a.display_.menuHeight);
        else if (k == "--window-mode") a.display_.windowMode = std::stoi(val());
        else if (k == "--window-skin") a.display_.windowSkin = std::clamp(std::stoi(val()), 1, 8);
        else if (k == "--keyboard")
        {
            const std::string v = val();
            a.display_.keyboard = v == "compact" || v == "1" ? 1 : v == "compact2" || v == "2" ? 2 : 0;
        }
        else if (k == "--ui-first") a.uiFirst = std::stoi(val());
        else if (k == "--ui-layout") a.uiLayout.push_back(val());
        else if (k == "--time")
        {
            int hh = 0, mm = 0;
            if (std::sscanf(val().c_str(), "%d:%d", &hh, &mm) == 2)
            {
                a.timeOverride = hh * 3600.0 + mm * 60.0;
            }
        }
        else if (k == "--weather") a.weatherOverride = std::stoi(val());
        else if (k == "--goto")
        {
            float x = 0, z = 0;
            if (std::sscanf(val().c_str(), "%f,%f", &x, &z) == 2)
            {
                a.gotoXZ = glm::vec2(x, z);
            }
        }
        else if (k == "--yaw") { a.camYaw = std::stof(val()) * 3.14159265f / 180.0f; a.camGiven = true; }
    }
    // The password may come from the environment (XI_PASS) so that a launcher need not put it on the command line,
    // where every process listing can read it.
    if (a.pass.empty())
    {
        if (const char* p = std::getenv("XI_PASS"); p && *p)
        {
            a.pass = p;
        }
    }
    // Send the version the install is patched to, unless --client-ver says otherwise (LSB checks login.CLIENT_VER).
    if (const auto v = a.root.empty() ? std::nullopt : install::installedClientVersion(a.root); v && a.clientVer == Args{}.clientVer)
    {
        a.clientVer = *v;
    }
    return a;
}

// --capabilities: what this build, GPU and machine offer, as JSON on stdout, for the launcher (goal 44) to grey out
// the options that would only fall back. Exits without opening a window.
int printCapabilities()
{
    const auto gpu = xi::gfx::probeGpu();
    nlohmann::json j;
    j["commit"]     = XI_GIT_COMMIT;
    j["dlss_built"] = xi::gfx::dlssBuilt();
    j["vulkan"]     = gpu.vulkan;
    j["gpu"]        = gpu.gpu;
    j["vendor_id"]  = gpu.vendorId;
    j["driver"]     = gpu.driver;
    j["ray_query"]  = gpu.rayQuery;
    // DLSS needs the SDK in the build and an NVIDIA RTX GPU (ray queries stand in for "RTX"); NGX itself is only
    // asked when the client starts with --dlss.
    j["dlss"] = xi::gfx::dlssBuilt() && gpu.nvidia && gpu.rayQuery;
    // FSR 3.1 (issue #139) runs on any vendor's GPU with the features gfx::checkDevice lists.
    j["fsr_built"]           = xi::gfx::fsrBuilt();
    j["fsr"]                 = xi::gfx::fsrBuilt() && gpu.fsr;
    j["gpu_pretend"]         = gpu.pretend;
    j["gpu_missing"]         = gpu.missing;
    const auto root = install::findGameDir();
    j["game_data"]  = root ? nlohmann::json(root->string()) : nlohmann::json(nullptr);
    j["game_problem"] = root ? install::gameDirProblem(*root) : std::string("no game install found");
    const auto ver  = root ? install::installedClientVersion(*root) : std::nullopt;
    j["game_version"] = ver ? nlohmann::json(*ver) : nlohmann::json(nullptr);
    const auto cd            = install::compressDatStatus();
    j["compress_dat"]        = cd.found.has_value();
    j["compress_dat_path"]   = cd.found ? nlohmann::json(cd.found->string()) : nlohmann::json(nullptr);
    j["compress_dat_status"] = install::describeCompressDat(cd);
    const auto hd   = xi::dat::hd::defaultDir();
    std::error_code ec;
    std::size_t     entries = 0;
    if (std::filesystem::is_directory(hd / "tex", ec))
    {
        for (const auto& e : std::filesystem::recursive_directory_iterator(hd / "tex", ec))
        {
            const auto name = e.path().filename().string();
            entries += e.is_regular_file(ec) && name.ends_with(".png") && !name.ends_with("_n.png") && !name.ends_with("_r.png") ? 1 : 0;
        }
    }
    j["hd_textures"] = { { "dir", hd.string() }, { "textures", entries } };
    // Paths need not be UTF-8, and dump() throws on bytes that are not.
    std::printf("%s\n", j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (const char* lvl = std::getenv("XI_LOG"))
    {
        spdlog::set_level(spdlog::level::from_str(lvl)); // e.g. XI_LOG=debug
    }
    // The recent log for /bugreport (goal 48): every line logged also goes to a ring in memory.
    app::installBugReportLog();
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--capabilities")
        {
            return printCapabilities();
        }
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h")
        {
            // Printed before anything opens a window (issue #81: --help used to start the game).
            std::fputs(
                "usage: xi-client [--host H] [--user U] [--pass P | $XI_PASS] [--char NAME] [--autoconnect]\n"
                "                 [--root DIR | $FFXI_ROOT] [--display N | $XI_DISPLAY] [--res WxH] [--config FILE]\n"
                "                 [--dlss[=MODE]] [--fsr[=MODE]] [--fsr-sharpness=S] [--upscaler=dlss|fsr|auto|off] [--rt[=EFFECTS]] [--post[=EFFECTS]] [--pathtrace[=N]] [--hd-textures[=DIR]]\n"
                "                 [--capabilities] [--script FILE] [--screenshot FILE --after SEC]\n"
                "Graphics options: docs/graphics-guide.md. Test hooks (--script, --ui-keys, --goto ...): apps/xi-client/main.cpp.\n",
                stdout);
            return 0;
        }
    }
    auto args = parseArgs(argc, argv);
    args.argv.assign(argv, argv + argc);
    if (!args.hdTextures.empty())
    {
        // Goal 41: textures load from the HD cache where it has them; XI_HDTEX_DUMP=1 saves the missing ones for
        // `xi-dat hdtex build --pending`.
        const char* dump = std::getenv("XI_HDTEX_DUMP");
        xi::dat::hd::setRuntimeDir(std::filesystem::path(args.hdTextures), dump && *dump == '1');
        spdlog::info("hd-textures: cache {}{}", args.hdTextures, xi::dat::hd::dumpMissing() ? " (saving missing textures)" : "");
    }
    if (!args.upscaler.empty() || !args.dlss.empty() || !args.fsr.empty())
    {
        // Issue #139: one upscaler, the one this GPU and build can run (--dlss on AMD or Intel falls back to FSR).
        const auto gpu = xi::gfx::probeGpu();
        const auto choice =
            xi::gfx::chooseUpscaler(args.upscaler, args.dlss, args.fsr,
                                    xi::gfx::dlssBuilt() && gpu.nvidia && gpu.rayQuery, xi::gfx::fsrBuilt() && gpu.fsr);
        if (!choice.note.empty())
        {
            spdlog::warn("upscaler: {}", choice.note);
        }
        args.dlss = choice.kind == xi::gfx::UpscalerKind::Dlss ? choice.mode : std::string();
        args.fsr  = choice.kind == xi::gfx::UpscalerKind::Fsr ? choice.mode : std::string();
    }
    const auto rootProblem = args.root.empty() ? std::string("no game install found") : install::gameDirProblem(args.root);
    if (!rootProblem.empty() || args.compressDat.empty())
    {
        fmt::print(stderr,
                   "{}\n`xi-setup check` lists what is missing (`xi-setup import` for the game data, "
                   "`xi-setup compress-dat FILE` for LSB's res/compress.dat).\n",
                   rootProblem.empty()
                       ? "LSB's res/compress.dat " + install::describeCompressDat(install::compressDatStatus())
                       : rootProblem);
        return 2;
    }

    try
    {
        app::ClientApp app(std::move(args));
        return app.run();
    }
    catch (const std::exception& e)
    {
        spdlog::error("{}", e.what());
        return 1;
    }
    return 0;
}
