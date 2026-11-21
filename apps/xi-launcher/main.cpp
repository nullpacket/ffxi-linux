// SPDX-License-Identifier: GPL-3.0-or-later
// xi-launcher (goal 44): server profiles, the client's settings and graphics presets, and one-click Connect, so nobody
// has to remember xi-client's command line. Everything it sets maps onto existing flags / client.ini keys.
//   xi-launcher [--config FILE]                        the settings file (default ~/.config/ffxi-linux/launcher.ini)
// Test hooks (used by the scripted checks; harmless otherwise):
//   --profile N|NAME  --page servers|graphics|display|tools|log  --preset NAME  --connect
//   --first-run (show the first-run import even with game data)  --first-run-import FOLDER (pick FOLDER and import)
//   --screenshot FILE --after SEC (one capture)  --quit-after SEC
#include "dat/texture.h"
#include "launcher/first_run.h"
#include "launcher/process.h"
#include "launcher/profile.h"
#include "net/socket.h"
#include "xi/ashita_dir.h"
#include "xi/game_data.h"
#include "xi/install.h"

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace xi::launcher;

namespace
{

// ---------------------------------------------------------------- theme

// Colours after the game's own windows: deep navy panels, a crystal-blue accent, a pale gold for headings.
const ImVec4 kGold{ 0.93f, 0.84f, 0.58f, 1.0f };
const ImVec4 kCrystal{ 0.55f, 0.75f, 1.0f, 1.0f };
const ImVec4 kWarn{ 1.0f, 0.78f, 0.35f, 1.0f };
const ImVec4 kError{ 1.0f, 0.45f, 0.45f, 1.0f };
const ImVec4 kOk{ 0.55f, 0.9f, 0.6f, 1.0f };
const ImVec4 kMuted{ 0.62f, 0.66f, 0.78f, 1.0f };

void applyTheme()
{
    ImGuiStyle& st       = ImGui::GetStyle();
    st.WindowRounding    = 6;
    st.ChildRounding     = 6;
    st.FrameRounding     = 4;
    st.GrabRounding      = 4;
    st.TabRounding       = 5;
    st.PopupRounding     = 5;
    st.WindowPadding     = { 12, 12 };
    st.FramePadding      = { 8, 5 };
    st.ItemSpacing       = { 9, 7 };
    st.ScrollbarRounding = 6;
    st.WindowBorderSize  = 1;
    st.ChildBorderSize   = 1;
    auto* c              = st.Colors;
    c[ImGuiCol_Text]                 = { 0.92f, 0.93f, 0.97f, 1 };
    c[ImGuiCol_TextDisabled]         = { 0.45f, 0.48f, 0.58f, 1 };
    c[ImGuiCol_WindowBg]             = { 0.055f, 0.075f, 0.16f, 1 };
    c[ImGuiCol_ChildBg]              = { 0.07f, 0.10f, 0.21f, 1 };
    c[ImGuiCol_PopupBg]              = { 0.06f, 0.08f, 0.18f, 0.98f };
    c[ImGuiCol_Border]               = { 0.34f, 0.42f, 0.70f, 0.55f };
    c[ImGuiCol_FrameBg]              = { 0.12f, 0.16f, 0.32f, 1 };
    c[ImGuiCol_FrameBgHovered]       = { 0.18f, 0.24f, 0.46f, 1 };
    c[ImGuiCol_FrameBgActive]        = { 0.22f, 0.30f, 0.56f, 1 };
    c[ImGuiCol_TitleBg]              = { 0.06f, 0.08f, 0.18f, 1 };
    c[ImGuiCol_TitleBgActive]        = { 0.10f, 0.14f, 0.30f, 1 };
    c[ImGuiCol_CheckMark]            = kCrystal;
    c[ImGuiCol_SliderGrab]           = { 0.45f, 0.62f, 0.95f, 1 };
    c[ImGuiCol_SliderGrabActive]     = kCrystal;
    c[ImGuiCol_Button]               = { 0.16f, 0.22f, 0.44f, 1 };
    c[ImGuiCol_ButtonHovered]        = { 0.24f, 0.33f, 0.62f, 1 };
    c[ImGuiCol_ButtonActive]         = { 0.30f, 0.42f, 0.76f, 1 };
    c[ImGuiCol_Header]               = { 0.16f, 0.22f, 0.44f, 1 };
    c[ImGuiCol_HeaderHovered]        = { 0.22f, 0.30f, 0.56f, 1 };
    c[ImGuiCol_HeaderActive]         = { 0.28f, 0.38f, 0.70f, 1 };
    c[ImGuiCol_Separator]            = { 0.34f, 0.42f, 0.70f, 0.5f };
    c[ImGuiCol_Tab]                  = { 0.10f, 0.14f, 0.30f, 1 };
    c[ImGuiCol_TabHovered]           = { 0.24f, 0.33f, 0.62f, 1 };
    c[ImGuiCol_TabSelected]          = { 0.20f, 0.28f, 0.54f, 1 };
    c[ImGuiCol_TabSelectedOverline]  = kCrystal;
    c[ImGuiCol_ScrollbarBg]          = { 0.05f, 0.07f, 0.15f, 1 };
    c[ImGuiCol_ScrollbarGrab]        = { 0.20f, 0.27f, 0.50f, 1 };
}

void heading(const char* text)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void hint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void tooltipIfDisabled(bool disabled, const std::string& why)
{
    if (disabled && !why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", why.c_str());
    }
}

// std::string with ImGui's InputText (a resize callback).
bool inputText(const char* label, std::string& s, ImGuiInputTextFlags flags = 0)
{
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputText(
        label, s.data(), s.capacity() + 1, flags,
        [](ImGuiInputTextCallbackData* d) {
            if (d->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                auto* str = static_cast<std::string*>(d->UserData);
                str->resize(static_cast<std::size_t>(d->BufTextLen));
                d->Buf = str->data();
            }
            return 0;
        },
        &s);
}

bool inputPort(const char* label, uint16_t& port)
{
    int v = port;
    if (ImGui::InputInt(label, &v, 0, 0))
    {
        port = static_cast<uint16_t>(std::clamp(v, 1, 65535));
        return true;
    }
    return false;
}

// "default" checkbox + slider for an optional value.
bool optionalSlider(const char* label, std::optional<float>& v, float def, float lo, float hi, const char* fmt = "%.2f")
{
    bool changed = false;
    bool set     = v.has_value();
    ImGui::PushID(label);
    if (ImGui::Checkbox("##set", &set))
    {
        v       = set ? std::optional<float>(def) : std::nullopt;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!set);
    float x = v.value_or(def);
    if (ImGui::SliderFloat(label, &x, lo, hi, set ? fmt : "default"))
    {
        v       = x;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------- capabilities

struct Caps
{
    bool        known = false;
    bool        dlss = false, dlssBuilt = false, fsr = false, fsrBuilt = false, rayQuery = false, vulkan = false;
    std::string gpu, driver, gameData, gameProblem, gameVersion, commit, hdDir, error, pretend;
    bool        compressDat = false;
    std::size_t hdTextures  = 0;
};

Caps readCaps(const std::string& client)
{
    Caps c;
    const auto [code, out] = runCapture({ client, "--capabilities" }, {}, 15000);
    if (code != 0)
    {
        c.error = fmt::format("`{} --capabilities` failed (exit {})", client, code);
        return c;
    }
    try
    {
        const auto j  = nlohmann::json::parse(out);
        c.known       = true;
        c.dlss        = j.value("dlss", false);
        c.dlssBuilt   = j.value("dlss_built", false);
        c.fsr         = j.value("fsr", false);
        c.fsrBuilt    = j.value("fsr_built", false);
        c.pretend     = j.value("gpu_pretend", "");
        c.rayQuery    = j.value("ray_query", false);
        c.vulkan      = j.value("vulkan", false);
        c.gpu         = j.value("gpu", "");
        c.driver      = j.value("driver", "");
        c.commit      = j.value("commit", "");
        c.compressDat = j.value("compress_dat", false);
        c.gameData    = j["game_data"].is_string() ? j["game_data"].get<std::string>() : "";
        c.gameProblem = j.value("game_problem", "");
        c.gameVersion = j["game_version"].is_string() ? j["game_version"].get<std::string>() : "";
        c.hdDir       = j["hd_textures"].value("dir", "");
        c.hdTextures  = j["hd_textures"].value("textures", std::size_t{ 0 });
    }
    catch (const std::exception& e)
    {
        c.error = std::string("capabilities: ") + e.what();
    }
    return c;
}

// ---------------------------------------------------------------- the app

enum class Page
{
    Servers,
    Graphics,
    Display,
    Tools,
    Log,
};

struct LogLine
{
    std::string text;
    int         level = 0; // 0 info, 1 warning, 2 error
};

struct App
{
    fs::path          configPath;
    LauncherConfig    cfg;
    int               selected = 0;
    Page              page     = Page::Servers;
    bool              pageForced = false;
    std::string       clientExe;
    std::string       toolDir; // where xi-setup / xi-dat live
    Caps              caps;
    std::map<int, std::string> sessionPasswords; // typed this session, by profile index (never written to disk)
    std::map<int, bool>        keyringHas;        // a password is in the keyring for this profile
    bool              keyringOk = false;
    std::string       status;
    ImVec4            statusColour = kMuted;
    bool              dirty        = false;
    std::chrono::steady_clock::time_point dirtyAt;

    Process                 client;
    std::deque<LogLine>     clientLog;
    std::vector<std::string> clientWarnings;
    std::optional<int>      clientExit;
    bool                    scrollClient = true;

    Process             tool;
    std::string         toolName;
    std::deque<LogLine> toolLog;
    std::optional<int>  toolExit;
    std::string         hdZones = "100,230,231,232,240,252";
    std::string         importFrom;
    std::vector<xi::install::FoundInstall> installs;
    bool                installsScanned = false;
    std::vector<xi::ashitadir::FoundAshita> ashitas;
    bool                ashitasScanned = false;
    std::string         ashitaFrom;

    // The first-run import (#135): shown instead of the pages while there is no usable game data.
    struct FirstRun
    {
        bool                                   active       = false;
        bool                                   connectAfter = false; // Connect was pressed: connect once imported
        bool                                   scanned      = false;
        std::vector<xi::install::FoundInstall> choices;
        int                                    pick = -1;
        std::string                            folder, folderProblem;
        xi::launcher::FirstRunImport           import;
        std::mutex                             browseMutex; // the folder dialog answers on another thread
        std::optional<std::string>             browsed;
    } firstRun;
    // LSB's compress.dat (#136): where the client finds it, and a file the user picks (Tools, first run).
    struct CompressDat
    {
        bool                           scanned = false;
        xi::install::CompressDatStatus status;
        std::string                    file, message;
        bool                           messageOk = false;
        std::mutex                     browseMutex; // the file dialog answers on another thread
        std::optional<std::string>     browsed;
    } cdat;
    SDL_Window* window = nullptr;

    std::future<std::vector<std::pair<std::string, bool>>> reachJob;
    std::vector<std::pair<std::string, bool>>              reach;
    std::string                                             reachFor;

    Profile& profile()
    {
        selected = std::clamp(selected, 0, static_cast<int>(cfg.profiles.size()) - 1);
        return cfg.profiles[static_cast<std::size_t>(selected)];
    }
    Settings& editing()
    {
        auto& p = profile();
        return p.ownSettings ? p.settings : cfg.global;
    }
    void markDirty()
    {
        dirty   = true;
        dirtyAt = std::chrono::steady_clock::now();
    }
    void setStatus(std::string s, ImVec4 c = kMuted)
    {
        status       = std::move(s);
        statusColour = c;
    }
};

int levelOf(const std::string& line)
{
    if (line.find("[error]") != std::string::npos || line.find("[critical]") != std::string::npos)
    {
        return 2;
    }
    if (line.find("[warning]") != std::string::npos)
    {
        return 1;
    }
    return 0;
}

void pushLog(std::deque<LogLine>& log, const std::string& line)
{
    log.push_back({ line, levelOf(line) });
    while (log.size() > 4000)
    {
        log.pop_front();
    }
}

// The client's own warnings worth a banner: what fell back, what is missing.
std::optional<std::string> notableWarning(const std::string& line)
{
    static const std::array<std::pair<const char*, const char*>, 8> kKnown{ {
        { "no DLSS", "DLSS is not available: the client renders without it." },
        { "has no DLSS", "This build has no DLSS: the client renders without it." },
        { "no ray queries", "This GPU / driver has no ray tracing: --rt / --pathtrace are off." },
        { "no game install found", "No game data found: run Tools > Import game data." },
        { "compress.dat not found", "LSB's res/compress.dat not found: Tools > compress.dat." },
        { "login failed", "Login failed: check the account and password." },
        { "lobby error", "The login server refused the connection." },
        { "no reply", "The server did not answer." },
    } };
    for (const auto& [needle, msg] : kKnown)
    {
        if (line.find(needle) != std::string::npos)
        {
            return std::string(msg);
        }
    }
    return std::nullopt;
}

void drawLog(const char* id, std::deque<LogLine>& log, bool& follow, float height)
{
    ImGui::BeginChild(id, { 0, height }, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(log.size()));
    while (clip.Step())
    {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
        {
            const auto& l = log[static_cast<std::size_t>(i)];
            if (l.level)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, l.level == 2 ? kError : kWarn);
            }
            ImGui::TextUnformatted(l.text.c_str());
            if (l.level)
            {
                ImGui::PopStyleColor();
            }
        }
    }
    if (follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
    {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

// The client and tools to run: beside the launcher (installed), then this build, then PATH.
std::string findProgram(const std::string& name, const std::string& configured = {})
{
    if (!configured.empty())
    {
        return configured;
    }
    std::error_code ec;
    const auto      self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec)
    {
        const auto beside = self.parent_path() / name;
        if (fs::exists(beside, ec))
        {
            return beside.string();
        }
    }
    return name;
}

fs::path overlayPath(const fs::path& configPath, int index)
{
    return configPath.parent_path() / "launcher" / fmt::format("profile-{}.ini", index);
}

void refreshKeyring(App& a)
{
    a.keyringHas.clear();
    if (!a.keyringOk)
    {
        return;
    }
    for (int i = 0; i < static_cast<int>(a.cfg.profiles.size()); ++i)
    {
        const auto& p = a.cfg.profiles[static_cast<std::size_t>(i)];
        if (p.savePassword && !p.user.empty())
        {
            a.keyringHas[i] = Keyring::lookup(p.host, p.user).has_value();
        }
    }
}

std::optional<std::string> passwordFor(App& a, int index)
{
    if (const auto it = a.sessionPasswords.find(index); it != a.sessionPasswords.end() && !it->second.empty())
    {
        return it->second;
    }
    const auto& p = a.cfg.profiles[static_cast<std::size_t>(index)];
    if (p.savePassword && a.keyringOk && !p.user.empty())
    {
        if (auto pw = Keyring::lookup(p.host, p.user))
        {
            return pw;
        }
    }
    // A launcher started with XI_PASS (scripted checks) passes it on unchanged.
    if (const char* env = std::getenv("XI_PASS"); env && *env)
    {
        return std::string(env);
    }
    return std::nullopt;
}

// No game data the client could use (the profile's own folder, else FFXI_ROOT / the imported copy / ~/.wine).
bool needFirstRun(App& a)
{
    return !xi::launcher::haveGameData(a.profile().gameDir, xi::install::findGameDir());
}

void connect(App& a)
{
    if (a.client.running())
    {
        return;
    }
    if (needFirstRun(a))
    {
        a.firstRun.active       = true;
        a.firstRun.connectAfter = true;
        a.setStatus("No game data yet: import it first, then the launcher connects.", kWarn);
        return;
    }
    auto&       p  = a.profile();
    const auto& s  = effectiveSettings(a.cfg, p);
    const auto  pw = passwordFor(a, a.selected);
    // The profile's client.ini overlay (only when it sets something).
    fs::path   overlay;
    const auto ini = overlayIni(s);
    if (!ini.empty())
    {
        overlay = overlayPath(a.configPath, a.selected);
        std::error_code ec;
        fs::create_directories(overlay.parent_path(), ec);
        FILE* f = std::fopen(overlay.c_str(), "w");
        if (f)
        {
            std::fputs(ini.c_str(), f);
            std::fclose(f);
        }
    }
    auto args = buildArgs(p, s, overlay, pw.has_value());
    args.insert(args.begin(), a.clientExe);
    a.clientLog.clear();
    a.clientWarnings.clear();
    a.clientExit.reset();
    pushLog(a.clientLog, "$ " + [&] {
        std::string line;
        for (const auto& x : args)
        {
            line += (line.empty() ? "" : " ") + (x.find(' ') != std::string::npos || x.empty() ? "\"" + x + "\"" : x);
        }
        return line;
    }());
    std::vector<std::pair<std::string, std::string>> env;
    if (pw)
    {
        env.emplace_back("XI_PASS", *pw); // never on the command line
    }
    // The client's output goes to a log file the launcher follows, so the game keeps running if the launcher closes.
    const fs::path logFile = a.configPath.parent_path() / "launcher" / "client.log";
    {
        std::error_code ec;
        fs::create_directories(logFile.parent_path(), ec);
    }
    if (!a.client.start(args, env, logFile.string()))
    {
        a.setStatus(a.client.error(), kError);
        pushLog(a.clientLog, "[error] " + a.client.error());
        return;
    }
    a.setStatus(fmt::format("Started the client for {} (pid {}).", p.name, a.client.pid()), kOk);
    a.page = Page::Log;
    a.pageForced = true;
}

void runTool(App& a, std::string name, std::vector<std::string> argv)
{
    if (a.tool.running())
    {
        return;
    }
    a.toolLog.clear();
    a.toolExit.reset();
    a.toolName = std::move(name);
    std::string line;
    for (const auto& x : argv)
    {
        line += (line.empty() ? "" : " ") + x;
    }
    pushLog(a.toolLog, "$ " + line);
    if (!a.tool.start(argv))
    {
        pushLog(a.toolLog, "[error] " + a.tool.error());
    }
}

// ---------------------------------------------------------------- pages

void serversPage(App& a)
{
    auto& p = a.profile();
    heading("Server profile");
    if (inputText("Name", p.name)) a.markDirty();
    if (inputText("Host", p.host)) a.markDirty();
    ImGui::SetNextItemWidth(110);
    if (inputPort("Login (auth)", p.authPort)) a.markDirty();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (inputPort("Data", p.dataPort)) a.markDirty();
    ImGui::SetNextItemWidth(110);
    if (inputPort("View", p.viewPort)) a.markDirty();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (inputPort("Search", p.searchPort)) a.markDirty();
    hint("LandSandBoat's defaults are 54231 (login), 54230 (data), 54001 (view) and 54002 (search).");

    const bool checking = a.reachJob.valid();
    ImGui::BeginDisabled(checking);
    if (ImGui::Button(checking ? "Checking..." : "Check server"))
    {
        a.reach.clear();
        a.reachFor = p.host;
        a.reachJob = std::async(std::launch::async, [host = p.host, ports = std::array{ p.authPort, p.dataPort, p.viewPort, p.searchPort }] {
            static constexpr const char* kNames[] = { "login", "data", "view", "search" };
            std::vector<std::pair<std::string, bool>> out;
            for (std::size_t i = 0; i < ports.size(); ++i)
            {
                bool ok = false;
                try
                {
                    xi::net::TcpStream::connect(xi::net::Endpoint::resolve(host, ports[i]));
                    ok = true;
                }
                catch (const std::exception&)
                {
                }
                out.emplace_back(fmt::format("{} {}", kNames[i], ports[i]), ok);
            }
            return out;
        });
    }
    ImGui::EndDisabled();
    for (const auto& [name, ok] : a.reach)
    {
        ImGui::SameLine();
        ImGui::TextColored(ok ? kOk : kError, "%s %s", name.c_str(), ok ? "open" : "closed");
    }
    if (ImGui::Checkbox("Server sends a rename list (HorizonXI)", &p.entityRenames))
        a.markDirty();
    hint("Asks the server for its dynamic-entity names after each zone-in (--entity-renames). Leave it off for LSB, "
         "which doesn't answer the request.");

    heading("Account");
    if (inputText("Account", p.user)) a.markDirty();
    if (inputText("Character", p.character)) a.markDirty();
    hint("Leave Character empty to choose in the game's lobby. Leave Account empty to log in from the client's own form.");
    auto& pw = a.sessionPasswords[a.selected];
    inputText("Password", pw, ImGuiInputTextFlags_Password);
    ImGui::BeginDisabled(!a.keyringOk);
    if (ImGui::Checkbox("Remember in the desktop keyring", &p.savePassword))
    {
        a.markDirty();
        if (!p.savePassword && !p.user.empty())
        {
            Keyring::clear(p.host, p.user);
            a.keyringHas[a.selected] = false;
        }
    }
    tooltipIfDisabled(!a.keyringOk, "No desktop keyring (secret-tool, Secret Service) is available: passwords are only kept for this session.");
    ImGui::EndDisabled();
    if (p.savePassword && a.keyringOk)
    {
        ImGui::SameLine();
        ImGui::BeginDisabled(pw.empty() || p.user.empty());
        if (ImGui::Button("Save password"))
        {
            if (Keyring::store(p.host, p.user, pw))
            {
                a.keyringHas[a.selected] = true;
                a.setStatus("Password saved in the desktop keyring.", kOk);
            }
            else
            {
                a.setStatus("The keyring refused it (locked or unavailable); the password is kept for this session only.", kWarn);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(a.keyringHas[a.selected] ? kOk : kMuted, a.keyringHas[a.selected] ? "in the keyring" : "not in the keyring");
    }
    hint("Passwords are never written to launcher.ini or put on the client's command line; the client gets it through "
         "its environment (XI_PASS).");

    heading("Game data");
    if (inputText("Game folder", p.gameDir)) a.markDirty();
    hint(a.caps.gameData.empty() ? "Empty: the client looks for FFXI_ROOT, then the imported copy (Tools > Import), then ~/.wine."
                                 : fmt::format("Empty: the client uses {}{}.", a.caps.gameData,
                                               a.caps.gameVersion.empty() ? "" : " (version " + a.caps.gameVersion + ")")
                                       .c_str());

    heading("Settings for this profile");
    if (ImGui::Checkbox("This profile has its own settings", &p.ownSettings))
    {
        if (p.ownSettings)
        {
            p.settings = a.cfg.global; // start from the global ones
        }
        a.markDirty();
    }
    hint(p.ownSettings ? "Graphics and Display & game edit this profile's own settings."
                       : "Graphics and Display & game edit the global settings, shared by every profile without its own.");
}

void graphicsPage(App& a)
{
    auto&       g      = a.editing().graphics;
    const auto& caps   = a.caps;
    const bool  noRt   = caps.known && !caps.rayQuery;
    const bool  noDlss = caps.known && !caps.dlss && !caps.fsr; // --dlss falls back to FSR where DLSS can't run (#139)
    const bool  noFsr  = caps.known && !caps.fsr;
    const std::string rtWhy =
        "This GPU / driver has no Vulkan ray queries (AMD RX 6000 / Intel Arc and newer have them): the client "
        "would render without ray tracing.";
    const std::string dlssWhy =
        "Neither upscaler can run here: DLSS needs an NVIDIA RTX GPU and a DLSS build, FSR the FidelityFX SDK at "
        "build time (XI_FSR) and a Vulkan 1.3 GPU.";
    const std::string fsrWhy =
        caps.fsrBuilt ? "This GPU / driver lacks what FSR 3.1's shaders need." : "This build has no FSR (XI_FSR).";
    const Graphics before = g;

    heading("Presets");
    const auto current = matchPreset(g);
    for (std::size_t i = 0; i < std::size(kPresets); ++i)
    {
        const Preset pr       = kPresets[i];
        const bool   isCur    = current && *current == pr;
        const auto   pg       = presetGraphics(pr, g);
        const bool   needRt   = pg.rt || !pg.rtBoost.empty() || pg.pathtrace;
        const bool   needDlss = !pg.dlss.empty();
        if (i % 4 != 0)
        {
            ImGui::SameLine();
        }
        if (isCur)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.30f, 0.42f, 0.76f, 1 });
        }
        if (ImGui::Button(presetName(pr), { 170, 0 }))
        {
            g = pg;
        }
        if (isCur)
        {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered())
        {
            const auto flags = graphicsArgs(pg);
            std::string line;
            for (const auto& f : flags)
            {
                line += (line.empty() ? "" : " ") + f;
            }
            std::string note;
            if ((needRt && noRt) || (needDlss && noDlss))
            {
                note = "\nParts of this need what this machine lacks; the client falls back without them.";
            }
            ImGui::SetTooltip("%s\n%s%s", presetDescription(pr), line.empty() ? "(no flags)" : line.c_str(), note.c_str());
        }
    }
    hint(current ? fmt::format("Current: {}. {}", presetName(*current), presetDescription(*current)).c_str()
                 : "Current: custom.");

    heading("Upscaling: DLSS 4 / FSR 3.1");
    {
        static constexpr const char* kModes[] = { "off", "quality", "balanced", "performance", "ultra-performance", "dlaa" };
        int cur = 0;
        for (int i = 1; i < 6; ++i)
        {
            cur = g.dlss == kModes[i] ? i : cur;
        }
        ImGui::BeginDisabled(noDlss);
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("Mode", &cur, kModes, 6))
        {
            g.dlss = cur == 0 ? "" : kModes[cur];
        }
        ImGui::EndDisabled();
        tooltipIfDisabled(noDlss, dlssWhy);
        static constexpr const char* kUpscalers[] = { "DLSS where it runs, else FSR", "FSR 3.1 always" };
        int                          which        = g.upscaler == "fsr" ? 1 : 0;
        ImGui::SameLine();
        ImGui::BeginDisabled(noFsr);
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("Upscaler", &which, kUpscalers, 2))
        {
            g.upscaler = which == 1 ? "fsr" : "";
        }
        ImGui::EndDisabled();
        tooltipIfDisabled(noFsr, fsrWhy);
        hint(
            caps.known && !caps.dlss && caps.fsr
                ? "Upscales the 3D scene (the interface stays sharp); DLAA keeps full resolution (FSR: native). This GPU runs "
                  "FSR 3.1 (DLSS is NVIDIA RTX only); ray tracing then uses the client's own denoiser."
                : "Upscales the 3D scene (the interface stays sharp); DLAA keeps full resolution (FSR: native). DLSS needs an "
                  "NVIDIA RTX GPU and also denoises ray tracing (Ray Reconstruction); FSR 3.1 runs on any GPU (AMD, Intel, "
                  "NVIDIA).");
    }

    heading("Ray-traced lighting");
    ImGui::BeginDisabled(noRt);
    ImGui::Checkbox("Ray tracing (--rt)", &g.rt);
    tooltipIfDisabled(noRt, rtWhy);
    ImGui::BeginDisabled(!g.rt);
    ImGui::SameLine();
    ImGui::Checkbox("Occlusion", &g.rtAo);
    ImGui::SameLine();
    ImGui::Checkbox("Shadows", &g.rtShadows);
    ImGui::SameLine();
    ImGui::Checkbox("Bounce light", &g.rtGi);
    ImGui::SameLine();
    ImGui::Checkbox("Water reflections", &g.rtReflections);
    ImGui::EndDisabled();
    {
        static constexpr const char* kBoost[] = { "off", "low", "high", "max" };
        int cur = g.rtBoost == "low" ? 1 : g.rtBoost == "high" ? 2 : g.rtBoost == "max" ? 3 : 0;
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("Boost (--rt-boost)", &cur, kBoost, 4))
        {
            g.rtBoost = cur == 0 ? "" : kBoost[cur];
        }
        tooltipIfDisabled(noRt, rtWhy);
    }
    ImGui::SetNextItemWidth(260);
    optionalSlider("Strength", g.rtStrength, 0.7f, 0.0f, 2.0f);
    ImGui::SetNextItemWidth(260);
    optionalSlider("Shadow cap", g.rtShadowCap, 0.4f, 0.0f, 1.0f);
    ImGui::SetNextItemWidth(260);
    optionalSlider("Reflections", g.rtReflect, 1.0f, 0.0f, 3.0f);
    ImGui::EndDisabled();
    hint("Plain ray tracing is subtle and keeps the game's look; Boost turns it up (and turns every effect on when Ray "
         "tracing itself is off).");

    heading("Post-processing");
    ImGui::Checkbox("Post effects (--post)", &g.post);
    ImGui::BeginDisabled(!g.post);
    ImGui::SameLine();
    ImGui::Checkbox("HDR", &g.postHdr);
    ImGui::SameLine();
    ImGui::Checkbox("Bloom", &g.postBloom);
    ImGui::SameLine();
    ImGui::Checkbox("Light shafts", &g.postGodrays);
    ImGui::SameLine();
    ImGui::Checkbox("Fog", &g.postFog);
    ImGui::SetNextItemWidth(260);
    ImGui::SliderFloat("Bloom strength", &g.postBloomStrength, 0, 5, "%.1f");
    ImGui::SetNextItemWidth(260);
    ImGui::SliderFloat("Shaft strength", &g.postShaftsStrength, 0, 5, "%.1f");
    ImGui::SetNextItemWidth(260);
    ImGui::SliderFloat("Fog strength", &g.postFogStrength, 0, 5, "%.1f");
    ImGui::EndDisabled();

    heading("Path tracing");
    ImGui::BeginDisabled(noRt);
    ImGui::Checkbox("Path-traced lighting (--pathtrace)", &g.pathtrace);
    tooltipIfDisabled(noRt, rtWhy);
    ImGui::BeginDisabled(!g.pathtrace);
    ImGui::SetNextItemWidth(260);
    ImGui::SliderInt("Bounces", &g.ptBounces, 1, 3);
    ImGui::SetNextItemWidth(260);
    ImGui::SliderFloat("Blend with the original", &g.ptBlend, 0, 1, "%.2f");
    ImGui::SetNextItemWidth(260);
    ImGui::SliderFloat("Exposure (EV)", &g.ptExposure, -4, 4, "%.1f");
    ImGui::SetNextItemWidth(260);
    ImGui::SliderFloat("Baked shading kept", &g.ptBaked, 0, 1, "%.2f");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    heading("HD textures");
    ImGui::Checkbox("AI-upscaled textures (--hd-textures)", &g.hdTextures);
    ImGui::BeginDisabled(!g.hdTextures);
    inputText("Cache folder", g.hdDir);
    ImGui::EndDisabled();
    hint(fmt::format("Cache: {} ({} textures built). Build more on the Tools page.",
                     g.hdDir.empty() ? (caps.hdDir.empty() ? std::string("~/.cache/ffxi-linux/hd-textures") : caps.hdDir) : g.hdDir,
                     caps.hdTextures)
             .c_str());

    if (!(before == g))
    {
        a.markDirty();
    }

    heading("Graphics flags");
    std::string line;
    for (const auto& f : graphicsArgs(g))
    {
        line += (line.empty() ? "" : " ") + f;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kCrystal);
    ImGui::TextWrapped("%s", line.empty() ? "(none: the original look)" : line.c_str());
    ImGui::PopStyleColor();
}

void displayPage(App& a)
{
    auto&      s      = a.editing();
    auto&      d      = s.display;
    auto&      o      = s.game;
    const auto before = s;

    heading("Window");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Width", &d.width, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Height", &d.height, 0, 0);
    hint("0 = client.ini's size (1600x900 by default).");
    {
        static constexpr const char* kModes[] = { "client.ini", "fullscreen", "windowed", "borderless fullscreen", "borderless window" };
        int cur = d.mode + 1;
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("Mode", &cur, kModes, 5))
        {
            d.mode = cur - 1;
        }
    }
    {
        int count = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&count);
        std::vector<std::string> names{ "default (secondary if there is one)", "primary" };
        for (int i = 0; i < count; ++i)
        {
            const char* n = SDL_GetDisplayName(ids[i]);
            names.push_back(fmt::format("{}: {}", i, n ? n : "display"));
        }
        SDL_free(ids);
        int cur = d.display.empty() ? 0 : d.display == "primary" ? 1 : 0;
        for (int i = 0; i < count; ++i)
        {
            cur = d.display == std::to_string(i) ? i + 2 : cur;
        }
        ImGui::SetNextItemWidth(360);
        if (ImGui::BeginCombo("Display", names[static_cast<std::size_t>(cur)].c_str()))
        {
            for (int i = 0; i < static_cast<int>(names.size()); ++i)
            {
                if (ImGui::Selectable(names[static_cast<std::size_t>(i)].c_str(), i == cur))
                {
                    d.display = i == 0 ? "" : i == 1 ? "primary" : std::to_string(i - 2);
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Scene width", &d.bgWidth, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Scene height", &d.bgHeight, 0, 0);
    hint("The 3D scene's resolution (0 = the window size). Higher = sharper, lower = faster.");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Menu width", &d.menuWidth, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Menu height", &d.menuHeight, 0, 0);
    hint("The interface's canvas (0 = two thirds of the window). Larger = smaller menus.");
    {
        static constexpr const char* kSkins[] = { "client.ini", "1", "2", "3", "4", "5", "6", "7", "8" };
        ImGui::SetNextItemWidth(160);
        ImGui::Combo("Window type", &d.windowSkin, kSkins, 9);
    }

    heading("Audio");
    {
        bool   set = o.soundVolume >= 0;
        if (ImGui::Checkbox("##sv", &set)) o.soundVolume = set ? 80 : -1;
        ImGui::SameLine();
        ImGui::BeginDisabled(!set);
        int v = std::max(o.soundVolume, 0);
        ImGui::SetNextItemWidth(260);
        if (ImGui::SliderInt("Sound effects", &v, 0, 100, set ? "%d" : "client.ini")) o.soundVolume = v;
        ImGui::EndDisabled();
        set = o.musicVolume >= 0;
        if (ImGui::Checkbox("##mv", &set)) o.musicVolume = set ? 50 : -1;
        ImGui::SameLine();
        ImGui::BeginDisabled(!set);
        v = std::max(o.musicVolume, 0);
        ImGui::SetNextItemWidth(260);
        if (ImGui::SliderInt("Music", &v, 0, 100, set ? "%d" : "client.ini")) o.musicVolume = v;
        ImGui::EndDisabled();
    }

    heading("Game");
    {
        static constexpr const char* kKeys[] = { "client.ini", "Normal (numeric keypad)", "Compact (WASD / IJKL)", "Compact 2 (+ Q/E turn)" };
        int cur = o.keyboard + 1;
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("Keyboard", &cur, kKeys, 4)) o.keyboard = cur - 1;
        static constexpr const char* kProto[] = { "client.ini", "auto", "2.1", "2.2" };
        cur = o.loginProtocol == "auto" ? 1 : o.loginProtocol == "2.1" ? 2 : o.loginProtocol == "2.2" ? 3 : 0;
        ImGui::SetNextItemWidth(160);
        if (ImGui::Combo("Login protocol", &cur, kProto, 4)) o.loginProtocol = cur == 0 ? "" : kProto[cur];
        inputText("Macro folder", o.macroDir);
        hint("Empty = client.ini (by default our own copy of the game's macro files); \"install\" shares the Windows client's.");
    }

    heading("Ashita addons");
    {
        static constexpr const char* kAddons[] = { "client.ini", "off", "on" };
        int cur = o.addons + 1;
        ImGui::SetNextItemWidth(160);
        if (ImGui::Combo("Addons", &cur, kAddons, 3)) o.addons = cur - 1;
        ImGui::BeginDisabled(o.addons != 1);
        inputText("Ashita folder", o.ashitaDir);
        static constexpr const char* kVer[] = { "client.ini / auto", "latest", "horizonxi" };
        int vc = o.ashitaVersion == "latest" ? 1 : o.ashitaVersion == "horizonxi" ? 2 : 0;
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Ashita release", &vc, kVer, 3)) o.ashitaVersion = vc == 0 ? "" : kVer[vc];
        inputText("Load at start", o.ashitaLoad);
        ImGui::EndDisabled();
        hint("Addons to load (comma-separated); the in-game Config > Add-ons page manages them per character too.");
    }

    heading("Extra command-line options");
    inputText("Extra", s.extraArgs);

    if (!(before == s))
    {
        a.markDirty();
    }
}

// LSB's compress.dat (#136): the file the client uses, or how to get one; a picked file is checked and copied into
// the client's data folder (xi::launcher::useCompressDat), so it is found without LSB_DIR.
void compressDatPanel(App& a)
{
    auto& c = a.cdat;
    {
        std::lock_guard lock(c.browseMutex);
        if (c.browsed)
        {
            c.file = *c.browsed;
            c.browsed.reset();
        }
    }
    if (!c.scanned)
    {
        c.status  = xi::install::compressDatStatus();
        c.scanned = true;
    }
    if (c.status.found)
    {
        ImGui::TextColored(kOk, "compress.dat: %s", c.status.found->c_str());
    }
    else
    {
        ImGui::TextColored(kError, "compress.dat: not found");
        hint("The client needs LandSandBoat's res/compress.dat (in the server checkout, or copied out of its Docker "
             "image) to read the server's packets. Pick it below, or set XI_COMPRESS_DAT / LSB_DIR.");
    }
    for (const auto& k : c.status.skipped)
    {
        ImGui::TextColored(kWarn, "Skipped %s", k.c_str());
    }
    inputText("compress.dat file", c.file);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##cdat"))
    {
        static const SDL_DialogFileFilter kFilters[] = { { "compress.dat", "dat" }, { "All files", "*" } };
        SDL_ShowOpenFileDialog([](void* user, const char* const* list, int) {
            auto* cd = static_cast<App::CompressDat*>(user);
            if (list && list[0])
            {
                std::lock_guard lock(cd->browseMutex);
                cd->browsed = std::string(list[0]);
            }
        }, &c, a.window, kFilters, 2, nullptr, false);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(c.file.empty());
    if (ImGui::Button("Use this file"))
    {
        const char* home   = std::getenv("HOME");
        const auto  err    = xi::launcher::useCompressDat(c.file, home ? home : "", xi::install::dataCompressDat());
        c.messageOk        = err.empty();
        c.message          = err.empty() ? "Copied to " + xi::install::dataCompressDat().string() : err;
        c.scanned          = false;
        a.caps.compressDat = err.empty() || a.caps.compressDat;
    }
    ImGui::EndDisabled();
    if (!c.message.empty())
    {
        ImGui::TextColored(c.messageOk ? kOk : kError, "%s", c.message.c_str());
    }
    hint(fmt::format("A picked file is checked (2048 bytes, a complete code table) and copied to {}.",
                     xi::install::dataCompressDat().string())
             .c_str());
}

// The first-run import (#135): pick an install (or a folder), import it with progress, show the file check.
void firstRunPage(App& a)
{
    auto&      fr   = a.firstRun;
    const auto st   = fr.import.state();
    const bool busy = st == xi::launcher::FirstRunImport::State::Running;
    const auto to   = xi::install::dataGameDir();
    if (!fr.scanned)
    {
        fr.choices = xi::launcher::firstRunChoices(xi::install::detectInstalls());
        fr.pick    = !fr.choices.empty() && fr.choices.front().problem.empty() ? 0 : -1;
        fr.scanned = true;
    }
    {
        std::lock_guard lock(fr.browseMutex);
        if (fr.browsed)
        {
            fr.folder = *fr.browsed;
            fr.browsed.reset();
        }
    }

    heading("Import the game data");
    ImGui::TextWrapped(
        "ffxi-linux plays with the data of a FINAL FANTASY XI install you own; none is included. Pick an "
        "install and the launcher copies the files the client reads into %s (reflinked where the disk "
        "allows, so it costs no space). Nothing in the install is changed. Pick one whose version matches your "
        "server's (an unmodified install where you can).",
        to.c_str());

    heading("Installs found on this machine");
    if (fr.choices.empty())
    {
        ImGui::TextColored(kWarn, "No FINAL FANTASY XI install was found.");
        ImGui::TextWrapped(
            "Looked in ~/.wine, Wine prefixes under ~/Games, Steam (app 230330), Lutris, Faugus, Bottles "
            "and mounted drives. Get an install one of these ways, then press Scan again, or pick its "
            "folder below:");
        ImGui::BulletText("copy the FINAL FANTASY XI folder from a Windows PC, or mount its drive;");
        ImGui::BulletText("install the game under Wine (or Lutris / Faugus / Bottles) and let PlayOnline update it;");
        ImGui::BulletText("install it from Steam and run it once so it patches;");
        ImGui::BulletText("use a private server's own client distribution.");
        hint("The setup guide, docs/setup.md (section 3, \"The game data\"), has the details.");
    }
    for (int i = 0; i < static_cast<int>(fr.choices.size()); ++i)
    {
        const auto& f      = fr.choices[static_cast<std::size_t>(i)];
        const bool  usable = f.problem.empty();
        ImGui::PushID(i);
        ImGui::BeginDisabled(busy || !usable);
        if (ImGui::RadioButton("##pick", fr.pick == i))
        {
            fr.pick = i;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(usable ? ImGui::GetStyle().Colors[ImGuiCol_Text] : kMuted, "%s  %s  (%s)%s",
                           f.version.empty() ? "(no version)" : f.version.c_str(), f.dir.c_str(), f.source.c_str(),
                           usable ? "" : ("  [" + f.problem + "]").c_str());
        ImGui::PopID();
    }
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Scan again"))
    {
        fr.scanned = false;
    }
    ImGui::EndDisabled();

    heading("Or another folder");
    inputText("Folder", fr.folder);
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Browse..."))
    {
        SDL_ShowOpenFolderDialog([](void* user, const char* const* list, int) {
            auto* run = static_cast<App::FirstRun*>(user);
            if (list && list[0])
            {
                std::lock_guard lock(run->browseMutex);
                run->browsed = std::string(list[0]);
            }
        }, &fr, a.window, nullptr, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Use this folder"))
    {
        const char* home   = std::getenv("HOME");
        const auto  picked = xi::launcher::resolvePickedFolder(fr.folder, home ? home : "");
        fr.folderProblem   = picked.problem;
        if (picked.dir)
        {
            auto it =
                std::find_if(fr.choices.begin(), fr.choices.end(), [&](const auto& f) { return f.dir == *picked.dir; });
            if (it == fr.choices.end())
            {
                xi::install::FoundInstall f;
                f.dir     = *picked.dir;
                f.version = xi::install::installedClientVersion(f.dir).value_or("");
                f.source  = "picked";
                it        = fr.choices.insert(fr.choices.begin(), std::move(f));
            }
            fr.pick = static_cast<int>(it - fr.choices.begin());
        }
    }
    ImGui::EndDisabled();
    if (!fr.folderProblem.empty())
    {
        ImGui::TextColored(kError, "%s", fr.folderProblem.c_str());
    }
    hint("The FINAL FANTASY XI folder itself, or a Wine prefix or drive that holds it.");

    ImGui::Spacing();
    ImGui::BeginDisabled(busy || fr.pick < 0);
    if (ImGui::Button("Import"))
    {
        fr.import.start(fr.choices[static_cast<std::size_t>(fr.pick)].dir, to);
        a.setStatus("Importing the game data...", kMuted);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Skip for now"))
    {
        fr.active       = false;
        fr.connectAfter = false;
        a.setStatus("No game data: Tools > Import game data does the same import later.", kWarn);
    }
    ImGui::EndDisabled();

    if (busy)
    {
        const auto  p = fr.import.progress();
        const float frac =
            p.bytesTotal ? static_cast<float>(static_cast<double>(p.bytesDone) / static_cast<double>(p.bytesTotal))
                         : 0.0f;
        ImGui::ProgressBar(frac, { -1, 0 }, fmt::format("{}/{} files", p.filesDone, p.filesTotal).c_str());
        ImGui::TextColored(kMuted, "%s", p.current.c_str());
        return;
    }
    if (st == xi::launcher::FirstRunImport::State::Idle)
    {
        return;
    }
    const bool ok = fr.import.succeeded();
    ImGui::TextColored(ok ? kOk : kError, "%s", xi::launcher::firstRunOutcome(fr.import).c_str());
    if (const auto rep = fr.import.report(); rep && !rep->issues.empty())
    {
        ImGui::BeginChild("report", { 0, 180 }, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& line : xi::install::formatReport(*rep, 10))
        {
            ImGui::TextUnformatted(line.c_str());
        }
        ImGui::EndChild();
    }
    if (!ok)
    {
        hint("Pick another install, or update that one (PlayOnline in its prefix) and import again. A required file "
             "missing means the client cannot start with this copy.");
        return;
    }
    if (ImGui::Button(fr.connectAfter ? "Continue and connect" : "Continue"))
    {
        fr.active = false;
        a.caps    = readCaps(a.clientExe); // the game data changed
        a.setStatus("Game data imported.", kOk);
        if (fr.connectAfter)
        {
            fr.connectAfter = false;
            connect(a);
        }
    }
}

void toolsPage(App& a)
{
    const bool busy = a.tool.running();
    heading("This machine");
    if (a.caps.known)
    {
        ImGui::Text("GPU: %s", a.caps.gpu.empty() ? "none found" : a.caps.gpu.c_str());
        ImGui::Text("Driver: %s", a.caps.driver.c_str());
        ImGui::TextColored(a.caps.rayQuery ? kOk : kWarn, "Ray tracing: %s", a.caps.rayQuery ? "available" : "not available");
        ImGui::TextColored(a.caps.dlss ? kOk : kWarn, "DLSS: %s",
                           a.caps.dlss ? "available" : a.caps.dlssBuilt ? "needs an NVIDIA RTX GPU" : "not in this build");
        ImGui::TextColored(a.caps.fsr ? kOk : kWarn, "FSR 3.1: %s",
                           a.caps.fsr        ? "available"
                           : a.caps.fsrBuilt ? "this GPU / driver lacks what it needs"
                                             : "not in this build");
        if (!a.caps.pretend.empty())
        {
            ImGui::TextColored(kWarn, "XI_GPU_PRETEND=%s: the client treats this GPU as that vendor's (a test mode)",
                               a.caps.pretend.c_str());
        }
        ImGui::TextColored(a.caps.gameProblem.empty() ? kOk : kError, "Game data: %s",
                           a.caps.gameProblem.empty() ? (a.caps.gameData + (a.caps.gameVersion.empty() ? "" : " (" + a.caps.gameVersion + ")")).c_str()
                                                      : a.caps.gameProblem.c_str());
    }
    else
    {
        ImGui::TextColored(kError, "%s", a.caps.error.empty() ? "Client capabilities unknown." : a.caps.error.c_str());
    }
    if (ImGui::Button("Refresh"))
    {
        a.caps         = readCaps(a.clientExe);
        a.cdat.scanned = false;
    }

    heading("LSB compress.dat");
    compressDatPanel(a);

    heading("Setup checks");
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Check setup"))
    {
        runTool(a, "xi-setup check", { a.toolDir.empty() ? "xi-setup" : a.toolDir + "/xi-setup", "check", "--host", a.profile().host });
    }
    ImGui::EndDisabled();
    hint("Game data, LSB files and the server's ports for the selected profile's host.");

    heading("Import game data");
    hint("Copies the files the client reads from an install into its own data folder (reflinked where the disk allows, "
         "so it costs no space). Pick an unmodified install whose version matches your server.");
    if (!a.installsScanned || ImGui::Button("Scan for installs"))
    {
        a.installs        = xi::install::detectInstalls();
        a.installsScanned = true;
    }
    for (const auto& f : a.installs)
    {
        ImGui::PushID(f.dir.c_str());
        ImGui::BeginDisabled(busy || !f.problem.empty());
        if (ImGui::SmallButton("Import"))
        {
            runTool(a, "xi-setup import", { a.toolDir.empty() ? "xi-setup" : a.toolDir + "/xi-setup", "import", f.dir.string() });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(f.problem.empty() ? ImGui::GetStyle().Colors[ImGuiCol_Text] : kMuted, "%s  %s  (%s)%s",
                           f.version.empty() ? "(no version)" : f.version.c_str(), f.dir.c_str(), f.source.c_str(),
                           f.problem.empty() ? "" : ("  [" + f.problem + "]").c_str());
        ImGui::PopID();
    }
    inputText("Other folder", a.importFrom);
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || a.importFrom.empty());
    if (ImGui::Button("Import from it"))
    {
        runTool(a, "xi-setup import", { a.toolDir.empty() ? "xi-setup" : a.toolDir + "/xi-setup", "import", a.importFrom });
    }
    ImGui::EndDisabled();

    heading("Ashita addons");
    {
        // The Ashita folder (issue #73): the profile's own, else the game folder the client uses; addons go in addons/.
        const auto& o    = a.editing().game;
        const auto  game = a.caps.gameData.empty() ? xi::install::dataGameDir() : std::filesystem::path(a.caps.gameData);
        const auto  root = xi::ashitadir::effectiveRoot(o.addons == 0 ? std::string("off") : o.ashitaDir, game);
        if (root.empty())
        {
            hint("Addons are off for this profile (Display & game > Ashita addons).");
        }
        else
        {
            ImGui::TextUnformatted(("Folder: " + (root / "addons").string()).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("Open folder"))
            {
                xi::ashitadir::ensureLayout(root);
                runTool(a, "open folder", { "xdg-open", (root / "addons").string() });
            }
            ImGui::EndDisabled();
            if (xi::ashitadir::libsPresent(root))
            {
                hint("Ashita's libraries are in place. Put addon folders here (addons/<name>/<name>.lua); in game, Config > "
                     "Add-ons loads them.");
            }
            else
            {
                ImGui::TextColored(kError, "%s",
                                   "Ashita's shared libraries are missing: install the bundled ones, or copy them "
                                   "from an install below.");
            }
            // The bundled standard addons and libraries (#138); the client also installs them on start.
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("Install bundled addons"))
            {
                runTool(a, "xi-setup addons",
                        { a.toolDir.empty() ? "xi-setup" : a.toolDir + "/xi-setup", "addons", "--to", root.string() });
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            hint("the standard Ashita addons that work here; adds only what the folder doesn't have");
            if (!a.ashitasScanned || ImGui::Button("Scan for Ashita installs"))
            {
                a.ashitas        = xi::ashitadir::detectAshitaInstalls(root);
                a.ashitasScanned = true;
            }
            for (const auto& f : a.ashitas)
            {
                ImGui::PushID(f.dir.c_str());
                ImGui::BeginDisabled(busy);
                if (ImGui::SmallButton("Copy libraries"))
                {
                    runTool(a, "xi-setup addons",
                            { a.toolDir.empty() ? "xi-setup" : a.toolDir + "/xi-setup", "addons", f.dir.string(), "--to", root.string() });
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::Text("%s  %s", f.label.c_str(), f.dir.c_str());
                ImGui::PopID();
            }
            inputText("Copy from folder", a.ashitaFrom);
            ImGui::SameLine();
            ImGui::BeginDisabled(busy || a.ashitaFrom.empty());
            if (ImGui::Button("Copy from it"))
            {
                runTool(a, "xi-setup addons",
                        { a.toolDir.empty() ? "xi-setup" : a.toolDir + "/xi-setup", "addons", a.ashitaFrom, "--to", root.string() });
            }
            ImGui::EndDisabled();
        }
    }

    heading("HD textures");
    inputText("Zones", a.hdZones);
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Build for these zones"))
    {
        runTool(a, "hdtex build", { a.toolDir.empty() ? "xi-dat" : a.toolDir + "/xi-dat", "hdtex", "build", "--zones", a.hdZones });
    }
    ImGui::SameLine();
    if (ImGui::Button("Build missing (from play)"))
    {
        runTool(a, "hdtex build --pending", { a.toolDir.empty() ? "xi-dat" : a.toolDir + "/xi-dat", "hdtex", "build", "--pending" });
    }
    ImGui::SameLine();
    if (ImGui::Button("Build all zones"))
    {
        runTool(a, "hdtex build --all", { a.toolDir.empty() ? "xi-dat" : a.toolDir + "/xi-dat", "hdtex", "build", "--all" });
    }
    if (ImGui::Button("Install the upscaler"))
    {
        // The one-line install from docs/enhanced-graphics.md (Real-ESRGAN ncnn-vulkan release, no root).
        runTool(a, "install upscaler",
                { "sh", "-c",
                  "set -e; d=\"${HOME}/.cache/ffxi-linux/hd-textures/tools\"; mkdir -p \"$d\"; cd \"$d\"; "
                  "if [ -x realesrgan/realesrgan-ncnn-vulkan ]; then echo 'already installed'; exit 0; fi; "
                  "curl -L -o r.zip https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip; "
                  "unzip -o -d realesrgan r.zip; chmod +x realesrgan/realesrgan-ncnn-vulkan; rm r.zip; echo installed" });
    }
    ImGui::EndDisabled();
    hint("\"Build missing\" upscales the textures the client saved while playing with XI_HDTEX_DUMP=1.");

    heading(a.toolName.empty() ? "Output" : ("Output: " + a.toolName).c_str());
    if (busy)
    {
        ImGui::TextColored(kCrystal, "Running...");
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop"))
        {
            a.tool.stop();
        }
    }
    else if (a.toolExit)
    {
        ImGui::TextColored(*a.toolExit == 0 ? kOk : kError, "Finished (exit %d).", *a.toolExit);
    }
    bool follow = true;
    drawLog("toollog", a.toolLog, follow, std::max(160.0f, ImGui::GetContentRegionAvail().y - 4));
}

void logPage(App& a)
{
    heading("Client");
    if (a.client.running())
    {
        ImGui::TextColored(kOk, "Running (pid %d).", a.client.pid());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop the client"))
        {
            a.client.stop();
        }
    }
    else if (a.clientExit)
    {
        ImGui::TextColored(*a.clientExit == 0 ? kOk : kError, "The client exited (code %d).", *a.clientExit);
    }
    else
    {
        ImGui::TextColored(kMuted, "Not started.");
    }
    for (const auto& w : a.clientWarnings)
    {
        ImGui::TextColored(kWarn, "! %s", w.c_str());
    }
    ImGui::Checkbox("Follow", &a.scrollClient);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy all"))
    {
        std::string all;
        for (const auto& l : a.clientLog)
        {
            all += l.text + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }
    drawLog("clientlog", a.clientLog, a.scrollClient, std::max(160.0f, ImGui::GetContentRegionAvail().y - 4));
}

void sidebar(App& a)
{
    ImGui::BeginChild("profiles", { 250, 0 }, ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextUnformatted("Servers");
    ImGui::PopStyleColor();
    ImGui::Separator();
    for (int i = 0; i < static_cast<int>(a.cfg.profiles.size()); ++i)
    {
        const auto& p     = a.cfg.profiles[static_cast<std::size_t>(i)];
        const auto  label = fmt::format("{}{}##p{}", p.name, i == a.cfg.defaultProfile ? "  (default)" : "", i);
        if (ImGui::Selectable(label.c_str(), i == a.selected, 0, { 0, 34 }))
        {
            a.selected = i;
            a.reach.clear();
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 22);
        ImGui::Indent(22);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::Text("%s%s%s", p.user.empty() ? "" : (p.user + "@").c_str(), p.host.c_str(),
                    p.character.empty() ? "" : ("  " + p.character).c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent(22);
    }
    ImGui::Separator();
    if (ImGui::Button("Add"))
    {
        a.cfg.profiles.emplace_back();
        a.cfg.profiles.back().name = fmt::format("Server {}", a.cfg.profiles.size());
        a.selected                 = static_cast<int>(a.cfg.profiles.size()) - 1;
        a.markDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy"))
    {
        auto copy = a.profile();
        copy.name += " (copy)";
        copy.savePassword = false;
        a.cfg.profiles.push_back(copy);
        a.selected = static_cast<int>(a.cfg.profiles.size()) - 1;
        a.markDirty();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(a.cfg.profiles.size() <= 1);
    if (ImGui::Button("Remove"))
    {
        ImGui::OpenPopup("remove?");
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("remove?"))
    {
        ImGui::Text("Remove \"%s\"?", a.profile().name.c_str());
        if (ImGui::Button("Remove it"))
        {
            auto& p = a.profile();
            if (p.savePassword && a.keyringOk && !p.user.empty())
            {
                Keyring::clear(p.host, p.user);
            }
            a.cfg.profiles.erase(a.cfg.profiles.begin() + a.selected);
            a.sessionPasswords.erase(a.selected);
            a.cfg.defaultProfile = std::clamp(a.cfg.defaultProfile - (a.cfg.defaultProfile > a.selected ? 1 : 0), 0,
                                              static_cast<int>(a.cfg.profiles.size()) - 1);
            a.selected = std::min(a.selected, static_cast<int>(a.cfg.profiles.size()) - 1);
            a.markDirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Default"))
    {
        a.cfg.defaultProfile = a.selected;
        a.markDirty();
    }
    // Connect, at the bottom.
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 112));
    const bool running = a.client.running();
    ImGui::PushStyleColor(ImGuiCol_Button, running ? ImVec4{ 0.45f, 0.20f, 0.20f, 1 } : ImVec4{ 0.20f, 0.42f, 0.78f, 1 });
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, running ? ImVec4{ 0.58f, 0.26f, 0.26f, 1 } : ImVec4{ 0.28f, 0.52f, 0.92f, 1 });
    if (ImGui::Button(running ? "Stop the game" : "Connect", { -1, 54 }))
    {
        if (running)
        {
            a.client.stop();
        }
        else
        {
            connect(a);
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    const auto& p = a.profile();
    ImGui::TextWrapped("%s", p.user.empty() ? "Logs in from the client's form."
                             : passwordFor(a, a.selected) ? "Logs in automatically."
                                                         : "No password: type it in the client's form.");
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

std::optional<Preset> presetByName(std::string_view n)
{
    for (const Preset p : kPresets)
    {
        std::string a = presetName(p), b(n);
        auto        low = [](std::string s) {
            std::string out;
            for (const char c : s)
            {
                if (std::isalnum(static_cast<unsigned char>(c)))
                {
                    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
            return out;
        };
        if (low(a) == low(b))
        {
            return p;
        }
    }
    return std::nullopt;
}

SDL_Window* makeWindow()
{
    // Like the client's windows: XI_DISPLAY picks the display (an index), XI_NO_FOCUS keeps it from taking focus.
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "ffxi-linux launcher");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 1180);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 780);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    if (const char* nf = std::getenv("XI_NO_FOCUS"); nf && std::strcmp(nf, "0") != 0)
    {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
    }
    if (const char* d = std::getenv("XI_DISPLAY"); d && *d)
    {
        int            count = 0;
        SDL_DisplayID* ids   = SDL_GetDisplays(&count);
        char*          end   = nullptr;
        const long     idx   = std::strtol(d, &end, 10);
        if (end != d && idx >= 0 && idx < count)
        {
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(ids[idx]));
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(ids[idx]));
        }
        SDL_free(ids);
    }
    SDL_Window* w = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return w;
}

void screenshot(SDL_Renderer* r, const std::string& file)
{
    SDL_Surface* s = SDL_RenderReadPixels(r, nullptr);
    if (!s)
    {
        return;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!rgba)
    {
        return;
    }
    std::vector<uint8_t> px(static_cast<std::size_t>(rgba->w) * static_cast<std::size_t>(rgba->h) * 4);
    for (int y = 0; y < rgba->h; ++y)
    {
        std::memcpy(px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba->w) * 4,
                    static_cast<const uint8_t*>(rgba->pixels) + static_cast<std::ptrdiff_t>(y) * rgba->pitch, static_cast<std::size_t>(rgba->w) * 4);
    }
    xi::dat::writePng(file, static_cast<uint32_t>(rgba->w), static_cast<uint32_t>(rgba->h), px);
    SDL_DestroySurface(rgba);
}

} // namespace

int main(int argc, char** argv)
{
    App         a;
    std::string wantProfile, wantPage, wantPreset, shotFile, firstRunImport;
    bool        wantConnect = false, forceFirstRun = false;
    float       shotAfter = 2.0f, quitAfter = -1.0f;
    a.configPath = defaultConfigPath();
    for (int i = 1; i < argc; ++i)
    {
        const std::string k   = argv[i];
        auto              val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (k == "--config") a.configPath = val();
        else if (k == "--profile") wantProfile = val();
        else if (k == "--page") wantPage = val();
        else if (k == "--preset") wantPreset = val();
        else if (k == "--connect") wantConnect = true;
        else if (k == "--first-run")
            forceFirstRun = true;
        else if (k == "--first-run-import")
            firstRunImport = val();
        else if (k == "--screenshot") shotFile = val();
        else if (k == "--after") shotAfter = std::stof(val());
        else if (k == "--quit-after") quitAfter = std::stof(val());
        else if (k == "--help" || k == "-h")
        {
            fmt::print("usage: xi-launcher [--config FILE]\n");
            return 0;
        }
    }
    a.cfg       = loadConfig(a.configPath);
    a.selected  = a.cfg.defaultProfile;
    a.clientExe = findProgram("xi-client", a.cfg.clientPath);
    {
        const fs::path c(a.clientExe);
        a.toolDir = c.has_parent_path() ? c.parent_path().string() : std::string();
    }
    a.caps      = readCaps(a.clientExe);
    a.keyringOk = Keyring::available();
    refreshKeyring(a);

    // Test hooks.
    if (!wantProfile.empty())
    {
        for (int i = 0; i < static_cast<int>(a.cfg.profiles.size()); ++i)
        {
            if (a.cfg.profiles[static_cast<std::size_t>(i)].name == wantProfile || std::to_string(i) == wantProfile)
            {
                a.selected = i;
            }
        }
    }
    if (const auto pr = presetByName(wantPreset))
    {
        a.editing().graphics = presetGraphics(*pr, a.editing().graphics);
        a.markDirty();
    }
    // No usable game data: the first-run import comes up instead of the pages (#135).
    a.firstRun.active = forceFirstRun || needFirstRun(a) || !firstRunImport.empty();
    if (!firstRunImport.empty())
    {
        a.firstRun.folder = firstRunImport;
        const char* home  = std::getenv("HOME");
        if (const auto picked = xi::launcher::resolvePickedFolder(firstRunImport, home ? home : ""); picked.dir)
        {
            a.firstRun.import.start(*picked.dir, xi::install::dataGameDir());
        }
    }

    static const std::map<std::string, Page> kPages{ { "servers", Page::Servers }, { "graphics", Page::Graphics },
                                                     { "display", Page::Display }, { "tools", Page::Tools }, { "log", Page::Log } };
    if (const auto it = kPages.find(wantPage); it != kPages.end())
    {
        a.page       = it->second;
        a.pageForced = true;
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fmt::print(stderr, "SDL_Init: {}\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = makeWindow();
    if (!window)
    {
        fmt::print(stderr, "SDL_CreateWindow: {}\n", SDL_GetError());
        return 1;
    }
    a.window               = window;
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
    {
        fmt::print(stderr, "SDL_CreateRenderer: {}\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    const float scale = SDL_GetWindowDisplayScale(window);
    io.FontGlobalScale = 1.0f;
    ImFontConfig fc;
    // In window units: the renderer scales to the framebuffer, and ImGui rasterises the glyphs at its density.
    fc.SizePixels = 16.0f;
    (void)scale;
    // A system sans font when fontconfig's usual paths have one, else ImGui's own.
    const char* fonts[] = { "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                            "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" };
    bool loaded = false;
    for (const char* f : fonts)
    {
        std::error_code ec;
        if (fs::exists(f, ec))
        {
            static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2000, 0x206F, 0 };
            loaded = io.Fonts->AddFontFromFileTTF(f, fc.SizePixels, nullptr, ranges) != nullptr;
            if (loaded)
            {
                break;
            }
        }
    }
    if (!loaded)
    {
        io.Fonts->AddFontDefault(&fc);
    }
    applyTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    const auto started = std::chrono::steady_clock::now();
    bool       shot = shotFile.empty(), quit = false, connectDone = !wantConnect;
    while (!quit)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window)))
            {
                quit = true;
            }
        }
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        if (!connectDone && t > 1.0f)
        {
            connectDone = true;
            connect(a);
        }
        // Child output.
        for (auto& l : a.client.poll())
        {
            if (const auto w = notableWarning(l); w && std::find(a.clientWarnings.begin(), a.clientWarnings.end(), *w) == a.clientWarnings.end())
            {
                a.clientWarnings.push_back(*w);
            }
            pushLog(a.clientLog, l);
        }
        if (!a.clientExit && a.client.pid() > 0 && !a.client.running())
        {
            a.clientExit = a.client.exitCode();
            for (auto& l : a.client.poll())
            {
                pushLog(a.clientLog, l);
            }
            a.setStatus(fmt::format("The client exited (code {}).", a.clientExit.value_or(-1)), a.clientExit == 0 ? kOk : kWarn);
        }
        for (auto& l : a.tool.poll())
        {
            pushLog(a.toolLog, l);
        }
        if (!a.toolExit && a.tool.pid() > 0 && !a.tool.running())
        {
            a.toolExit = a.tool.exitCode();
            for (auto& l : a.tool.poll())
            {
                pushLog(a.toolLog, l);
            }
            if (a.toolName.rfind("xi-setup import", 0) == 0 || a.toolName.rfind("hdtex", 0) == 0)
            {
                a.caps = readCaps(a.clientExe); // the game data / cache changed
            }
        }
        if (a.reachJob.valid() && a.reachJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            a.reach = a.reachJob.get();
        }
        // Save half a second after the last change.
        if (a.dirty && std::chrono::steady_clock::now() - a.dirtyAt > std::chrono::milliseconds(500))
        {
            a.dirty = false;
            if (!saveConfig(a.cfg, a.configPath))
            {
                a.setStatus("Could not save " + a.configPath.string(), kError);
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        if (static bool once = false; !once && std::getenv("XI_LAUNCHER_DEBUG"))
        {
            once = true;
            int ow = 0, oh = 0, ww = 0, wh = 0, pw = 0, ph = 0;
            float rsx = 0, rsy = 0;
            SDL_GetCurrentRenderOutputSize(renderer, &ow, &oh);
            SDL_GetWindowSize(window, &ww, &wh);
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            SDL_GetRenderScale(renderer, &rsx, &rsy);
            fmt::print("display {}x{} fb {}x{} | out {}x{} win {}x{} px {}x{} rs {} {} | {}\n", io.DisplaySize.x, io.DisplaySize.y,
                       io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y, ow, oh, ww, wh, pw, ph, rsx, rsy, SDL_GetCurrentVideoDriver());
        }
        ImGui::NewFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        // Title strip.
        ImGui::PushStyleColor(ImGuiCol_Text, kGold);
        ImGui::SetWindowFontScale(1.35f);
        ImGui::TextUnformatted("FINAL FANTASY XI");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kCrystal);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
        ImGui::TextUnformatted("ffxi-linux launcher");
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetWindowWidth() - 420);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
        ImGui::TextColored(kMuted, "%s%s", a.caps.gpu.empty() ? "" : a.caps.gpu.c_str(),
                           a.caps.commit.empty() ? "" : ("  |  build " + a.caps.commit).c_str());
        ImGui::Separator();

        sidebar(a);
        ImGui::SameLine();
        ImGui::BeginChild("main", { 0, -26 }, ImGuiChildFlags_None);
        auto& p = a.profile();
        if (a.firstRun.active)
        {
            firstRunPage(a);
            heading("LSB's compress.dat");
            compressDatPanel(a);
        }
        else if (ImGui::BeginTabBar("pages"))
        {
            auto tab = [&](const char* name, Page pg) {
                const auto flags = a.pageForced && a.page == pg ? ImGuiTabItemFlags_SetSelected : 0;
                const bool open  = ImGui::BeginTabItem(name, nullptr, flags);
                if (open)
                {
                    if (!a.pageForced)
                    {
                        a.page = pg;
                    }
                    ImGui::BeginChild("page", { 0, 0 }, ImGuiChildFlags_None);
                    switch (pg)
                    {
                        case Page::Servers: serversPage(a); break;
                        case Page::Graphics: graphicsPage(a); break;
                        case Page::Display: displayPage(a); break;
                        case Page::Tools: toolsPage(a); break;
                        case Page::Log: logPage(a); break;
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            };
            tab("Server", Page::Servers);
            tab(p.ownSettings ? "Graphics (this profile)" : "Graphics", Page::Graphics);
            tab(p.ownSettings ? "Display & game (this profile)" : "Display & game", Page::Display);
            tab("Tools", Page::Tools);
            tab(a.client.running() ? "Game log (running)###log" : "Game log###log", Page::Log);
            a.pageForced = false;
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::TextColored(a.statusColour, "%s", a.status.empty() ? ("Settings: " + a.configPath.string()).c_str() : a.status.c_str());
        ImGui::End();

        ImGui::Render();
        // HiDPI: ImGui draws in window units; SDL scales them (and the clip rects) to the framebuffer, as ImGui's own
        // SDL3 renderer example does.
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 10, 14, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!shot && t > shotAfter)
        {
            screenshot(renderer, shotFile);
            shot = true;
        }
        SDL_RenderPresent(renderer);
        if (quitAfter > 0 && t > quitAfter && !a.client.running())
        {
            quit = true;
        }
    }
    if (a.dirty)
    {
        saveConfig(a.cfg, a.configPath);
    }
    // Closing the launcher leaves a running game alone (it is in its own process group); a tool is stopped.
    a.client.detach();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
