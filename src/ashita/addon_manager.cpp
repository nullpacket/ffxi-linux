// SPDX-License-Identifier: GPL-3.0-or-later
// AddonManager: one LuaJIT state per addon, Ashita's events dispatched to them in load order (ashita.h).
#include "ashita/ashita.h"
#include "ashita/input_map.h"
#include "ashita/lua_util.h"
#include "ashita/plugin_host.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>

#include <unistd.h>

namespace xi::ashita
{

const char* profileVersion(Profile p)
{
    return p == Profile::HorizonXI ? "4.3.1.2" : "4.3.2.1";
}

std::optional<Profile> parseProfile(std::string_view s)
{
    if (s == "latest" || s.starts_with("4.3.2"))
    {
        return Profile::Latest;
    }
    if (s == "horizonxi" || s == "hxi" || s.starts_with("4.3.1"))
    {
        return Profile::HorizonXI;
    }
    return std::nullopt;
}

Profile detectProfile(const std::filesystem::path& root)
{
    std::error_code ec;
    const auto      libs = root / "addons" / "libs";
    return std::filesystem::exists(libs / "typedefs", ec) || std::filesystem::exists(libs / "lin", ec) ? Profile::HorizonXI : Profile::Latest;
}

namespace
{

int traceback(lua_State* L)
{
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object)", 1);
    return 1;
}

// pcall with a traceback; the error is logged and returned.
std::optional<std::string> pcall(lua_State* L, int nargs, int nresults)
{
    const int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    const int rc = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (rc != 0)
    {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
        lua_pop(L, 1);
        return err;
    }
    return std::nullopt;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

AddonManager::AddonManager(std::filesystem::path root, Profile profile, Resources res, Host host)
: root_(std::move(root))
, profile_(profile)
, res_(std::move(res))
, host_(std::move(host))
{
    spdlog::info("ashita: addons from {} (as Ashita {})", root_.string(), profileVersion(profile_));
    plugins_ = makePluginHost(*this);
}

AddonManager::~AddonManager()
{
    unloadAll();
    if (plugins_)
    {
        std::lock_guard lock(m_);
        plugins_->unloadAll();
    }
}

std::vector<std::filesystem::path> pluginFolders(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> dirs{ root / "plugins" };
    std::error_code                    ec;
    if (const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec); !ec)
    {
        dirs.push_back(exe.parent_path() / "plugins");
    }
    return dirs;
}

bool AddonManager::loadPlugin(const std::string& name, const std::string& args)
{
    std::lock_guard lock(m_);
    std::string     error;
    bool            ok = false;
    if (!plugins_)
    {
        error = pluginSupportNote();
    }
    else
    {
        ok = plugins_->load(name, args, error);
    }
    if (ok)
    {
        say(fmt::format("[Plugins] Loaded plugin: {}", name));
    }
    else
    {
        spdlog::warn("ashita: /load {}: {}", name, error);
        say(fmt::format("[Plugins] Failed to load plugin '{}': {}", name, error), kErrorMode);
    }
    flush();
    return ok;
}

bool AddonManager::unloadPlugin(const std::string& name)
{
    std::lock_guard lock(m_);
    const bool      ok = plugins_ && plugins_->unload(name);
    say(ok ? fmt::format("[Plugins] Unloaded plugin: {}", name)
           : fmt::format("[Plugins] Plugin '{}' is not loaded.", name));
    flush();
    return ok;
}

std::vector<std::string> AddonManager::loadedPlugins() const
{
    std::lock_guard lock(m_);
    return plugins_ ? plugins_->loaded() : std::vector<std::string>{};
}

// Ashita's plugin commands: /load NAME [ARGS], /unload NAME, /reload NAME, /plugins (the list).
bool AddonManager::pluginCommand(const std::string& line)
{
    const auto l   = lower(line);
    auto       arg = [&](std::size_t n) {
        auto r = line.substr(n);
        r.erase(0, r.find_first_not_of(' '));
        return r;
    };
    if (l.starts_with("/load "))
    {
        const auto rest = arg(6);
        const auto sp   = rest.find(' ');
        loadPlugin(rest.substr(0, sp), sp == std::string::npos ? std::string() : arg(6 + sp + 1));
        return true;
    }
    if (l.starts_with("/unload "))
    {
        unloadPlugin(arg(8));
        return true;
    }
    if (l.starts_with("/reload "))
    {
        const auto name = arg(8);
        unloadPlugin(name);
        loadPlugin(name);
        return true;
    }
    if (l == "/plugins")
    {
        const auto names = loadedPlugins();
        say(fmt::format("[Plugins] Loaded plugins ({}): {}", names.size(), fmt::join(names, ", ")));
        if (!plugins_)
        {
            say(fmt::format("[Plugins] {}", pluginSupportNote()));
        }
        flush();
        return true;
    }
    return false;
}

void AddonManager::say(const std::string& text, int mode)
{
    pendingChat_.emplace_back(text, mode);
}

bool AddonManager::load(const std::string& nameIn)
{
    std::lock_guard lock(m_);
    for (const auto& a : addons_)
    {
        if (lower(a->name) == lower(nameIn))
        {
            say(fmt::format("[Addons] '{}' is already loaded.", a->name));
            flush();
            return false;
        }
    }
    // Folder names keep their case (HXUI, tCrossBar); Windows finds them whatever case is typed.
    std::string     name = nameIn;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root_ / "addons", ec))
    {
        if (e.is_directory(ec) && lower(e.path().filename().string()) == lower(nameIn))
        {
            name = e.path().filename().string();
        }
    }
    const auto dir  = root_ / "addons" / name;
    auto       file = dir / (name + ".lua");
    if (!std::filesystem::exists(file, ec))
    {
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        {
            if (lower(e.path().filename().string()) == lower(name + ".lua"))
            {
                file = e.path();
            }
        }
    }
    clearErrors(name);
    if (!std::filesystem::exists(file, ec))
    {
        noteError(name, fmt::format("no {}", file.string()));
        say(fmt::format("[Addons] Failed to load addon: '{}' (no {})", nameIn, file.string()));
        flush();
        return false;
    }
    auto a  = std::make_unique<Addon>();
    a->name = name;
    a->dir  = dir;
    a->L    = luaL_newstate();
    luaL_openlibs(a->L);
    openNative(a->L, this, a.get());
    lua_State* L = a->L;
    if (luaL_loadbuffer(L, kPrelude, std::strlen(kPrelude), "=prelude") != 0)
    {
        spdlog::error("ashita: prelude: {}", lua_tostring(L, -1));
        lua_close(L);
        return false;
    }
    if (const auto err = pcall(L, 0, 0))
    {
        spdlog::error("ashita: prelude: {}", *err);
        lua_close(L);
        return false;
    }
    addons_.push_back(std::move(a));
    Addon& ad = *addons_.back();
    if (luaL_loadfile(L, file.c_str()) != 0)
    {
        noteError(name, lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)");
        say(fmt::format("[Addons] Failed to load addon: '{}': {}", name, lua_tostring(L, -1)));
        spdlog::error("ashita: {}: {}", name, lua_tostring(L, -1));
        lua_close(L);
        addons_.pop_back();
        return false;
    }
    if (const auto err = pcall(L, 0, 0))
    {
        noteError(name, *err);
        say(fmt::format("[Addons] Failed to load addon: '{}'", name));
        spdlog::error("ashita: {}: {}", name, *err);
        lua_close(L);
        addons_.pop_back();
        flush();
        return false;
    }
    ad.loaded = true;
    lua_getglobal(L, "__xi_event");
    lua_pushstring(L, "load");
    // Ashita hands the load event the command line that loaded the addon (autologin reads its arguments from it).
    lua_pushstring(L, ("/addon load " + name).c_str());
    if (const auto err = pcall(L, 2, 0))
    {
        spdlog::error("ashita: {} load: {}", name, *err);
    }
    spdlog::info("ashita: loaded {}", name);
    flush();
    return true;
}

bool AddonManager::unload(const std::string& nameIn)
{
    std::lock_guard lock(m_);
    const std::string name = nameIn;
    const auto it = std::find_if(addons_.begin(), addons_.end(), [&](const auto& a) { return lower(a->name) == lower(name); });
    if (it == addons_.end())
    {
        say(fmt::format("[Addons] '{}' is not loaded.", name));
        return false;
    }
    lua_State* L = (*it)->L;
    lua_getglobal(L, "__xi_event");
    lua_pushstring(L, "unload");
    lua_newtable(L);
    if (const auto err = pcall(L, 2, 0))
    {
        spdlog::error("ashita: {} unload: {}", name, *err);
    }
    imguiClose(L);
    lua_close(L);
    addons_.erase(it);
    spdlog::info("ashita: unloaded {}", name);
    flush();
    return true;
}

void AddonManager::unloadAll()
{
    std::lock_guard lock(m_);
    while (!addons_.empty())
    {
        unload(addons_.back()->name);
    }
}

std::vector<std::string> AddonManager::errors(const std::string& name) const
{
    std::lock_guard lock(m_);
    const auto      it = errors_.find(lower(name));
    return it == errors_.end() ? std::vector<std::string>{} : std::vector<std::string>(it->second.begin(), it->second.end());
}

void AddonManager::clearErrors(const std::string& name)
{
    std::lock_guard lock(m_);
    errors_.erase(lower(name));
}

void AddonManager::noteError(const std::string& name, std::string message)
{
    std::lock_guard lock(m_);
    // FFXI colour codes (0x1E / 0x1F + a byte) out.
    std::string clean;
    for (std::size_t i = 0; i < message.size(); ++i)
    {
        if ((message[i] == '\x1E' || message[i] == '\x1F') && i + 1 < message.size())
        {
            ++i;
            continue;
        }
        clean += message[i];
    }
    auto& q = errors_[lower(name)];
    q.push_back(std::move(clean));
    while (q.size() > 20)
    {
        q.pop_front();
    }
}

std::vector<std::string> AddonManager::loaded() const
{
    std::lock_guard          lock(m_);
    std::vector<std::string> v;
    for (const auto& a : addons_)
    {
        v.push_back(a->name);
    }
    return v;
}

void AddonManager::runBootScript(const std::filesystem::path& script)
{
    const auto    file = script.empty() ? root_ / "scripts" / "default.txt" : script;
    std::ifstream in(file);
    if (!in)
    {
        return;
    }
    std::string line;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        {
            line.pop_back();
        }
        const auto l = lower(line);
        if (l.starts_with("/addon load ") || l.starts_with("/load "))
        {
            if (l.starts_with("/load "))
            {
                // A plugin: loaded when this build has it natively (`addons` is the Lua layer itself).
                if (lower(line.substr(6)) != "addons")
                {
                    pluginCommand(line);
                }
                continue;
            }
            load(line.substr(12));
        }
    }
}

void AddonManager::flush()
{
    if (!inUpdate_ && frame_.world == nullptr)
    {
        // Before the first frame there is nowhere to send them yet; keep them.
    }
    // Addon chat goes into the log one plain line per line break (FFXI colour codes dropped). Each is remembered, so
    // text_in sees it as injected; an addon's error report (mode kErrorMode) is not shown to text_in at all, which
    // would loop when the error is in a text_in callback.
    for (auto& [text, mode] : pendingChat_)
    {
        std::string line;
        auto        emit = [&] {
            if (!line.empty())
            {
                printed_.push_back({ line, mode == kErrorMode });
                if (host_.chat)
                {
                    host_.chat(line, mode == kErrorMode ? 1 : mode);
                }
            }
            line.clear();
        };
        for (std::size_t k = 0; k < text.size(); ++k)
        {
            const char c = text[k];
            if (c == '\x1E' || c == '\x1F')
            {
                ++k;
            }
            else if (c == '\n' || c == '\x07')
            {
                emit();
            }
            else if (c != '\r')
            {
                line.push_back(c);
            }
        }
        emit();
    }
    pendingChat_.clear();
    while (printed_.size() > 200)
    {
        printed_.pop_front();
    }
    for (auto& [line, mode] : pendingCommands_)
    {
        if (host_.command)
        {
            host_.command(line, mode);
        }
    }
    pendingCommands_.clear();
}

void AddonManager::update(Frame frame)
{
    std::lock_guard lock(m_);
    loadPendingFonts();
    frame_ = std::move(frame);
    ++frameNo_;
    inUpdate_ = true;
    if (plugins_)
    {
        plugins_->frame();
    }
    for (const auto& a : addons_)
    {
        lua_getglobal(a->L, "__xi_frame");
        if (const auto err = pcall(a->L, 0, 0))
        {
            spdlog::error("ashita: {} tasks: {}", a->name, *err);
        }
    }
    // text_in for the log lines that arrived since the last frame.
    if (frame_.world)
    {
        for (const auto& line : frame_.world->chat)
        {
            if (line.serial > chatSeen_)
            {
                bool injected = false, skip = false;
                if (line.from.empty())
                {
                    if (const auto it = std::find_if(printed_.begin(), printed_.end(), [&](const Printed& p) { return p.text == line.text; });
                        it != printed_.end())
                    {
                        injected = true;
                        skip     = it->error;
                        printed_.erase(it);
                    }
                }
                if (chatPrimed_ && !skip)
                {
                    textIn(line, injected);
                }
                chatSeen_ = std::max(chatSeen_, line.serial);
            }
        }
        chatPrimed_ = true; // the lines of the first frame are old; every later one is new
    }
    flush();
    inUpdate_ = false;
}

void AddonManager::present()
{
    std::lock_guard lock(m_);
    // Ashita players never see Dear ImGui's own error tooltip or asserts; addon mistakes are recovered from below.
    ImGui::GetIO().ConfigErrorRecoveryEnableTooltip = false;
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert  = false;
    for (const auto& a : addons_)
    {
        ImGuiErrorRecoveryState state;
        ImGui::ErrorRecoveryStoreState(&state);
        imguiFrameStart(a->L);
        lua_State* L = a->L;
        lua_getglobal(L, "__xi_event");
        lua_pushstring(L, "d3d_beginscene");
        lua_newtable(L);
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "is_rendering_backbuffer");
        if (const auto err = pcall(L, 2, 0))
        {
            spdlog::error("ashita: {} d3d_beginscene: {}", a->name, *err);
        }
        lua_getglobal(L, "__xi_event");
        lua_pushstring(L, "d3d_present");
        lua_newtable(L);
        if (const auto err = pcall(L, 2, 0))
        {
            spdlog::error("ashita: {} d3d_present: {}", a->name, *err);
        }
        lua_getglobal(L, "__xi_event");
        lua_pushstring(L, "d3d_endscene");
        lua_newtable(L);
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "is_rendering_backbuffer");
        if (const auto err = pcall(L, 2, 0))
        {
            spdlog::error("ashita: {} d3d_endscene: {}", a->name, *err);
        }
        ImGui::ErrorRecoveryTryToRecoverState(&state); // an addon that left a window open does not break the next
        lua_getglobal(L, "__xi_draw");
        if (const auto err = pcall(L, 0, 0))
        {
            spdlog::error("ashita: {} draw: {}", a->name, *err);
        }
    }
    flush();
}

// /addon load|unload|reload|list (Ashita's Addons plugin); /addons is the same.
bool AddonManager::addonCommand(const std::string& line)
{
    std::vector<std::string> w;
    for (std::size_t i = 0; i < line.size();)
    {
        while (i < line.size() && line[i] == ' ')
        {
            ++i;
        }
        const auto j = line.find(' ', i);
        if (i < line.size())
        {
            w.push_back(line.substr(i, j == std::string::npos ? std::string::npos : j - i));
        }
        i = j == std::string::npos ? line.size() : j;
    }
    if (w.empty() || (lower(w[0]) != "/addon" && lower(w[0]) != "/addons"))
    {
        return false;
    }
    const std::string sub = w.size() > 1 ? lower(w[1]) : "list";
    if (sub == "load" && w.size() > 2)
    {
        load(w[2]);
    }
    else if (sub == "unload" && w.size() > 2)
    {
        unload(w[2]);
    }
    else if (sub == "reload" && w.size() > 2)
    {
        unload(w[2]);
        load(w[2]);
    }
    else if (sub == "unloadall")
    {
        unloadAll();
    }
    else
    {
        const auto names = loaded();
        say(fmt::format("[Addons] Loaded addons ({}): {}", names.size(), fmt::join(names, ", ")));
        say("[Addons] /addon load|unload|reload <name>, /addon list");
    }
    flush();
    return true;
}

bool AddonManager::command(const std::string& line, int mode)
{
    std::string l = line;
    return commandEdit(l, mode);
}

bool AddonManager::commandEdit(std::string& line, int mode)
{
    std::lock_guard lock(m_);
    if (addonCommand(line) || pluginCommand(line))
    {
        return true;
    }
    bool injected = false;
    if (const auto it = std::find(injectedLines_.begin(), injectedLines_.end(), line); it != injectedLines_.end())
    {
        injected = true;
        injectedLines_.erase(it);
    }
    // Plugins see the line first (INFERRED: Ashita's addons are themselves a plugin, run after the ones that ask
    // for an earlier priority, as Shorthand does).
    if (plugins_ && plugins_->command(line, mode, injected))
    {
        flush();
        return true;
    }
    bool blocked = false;
    for (const auto& a : addons_)
    {
        lua_State* L = a->L;
        lua_getglobal(L, "__xi_event");
        lua_pushstring(L, "command");
        lua_newtable(L);
        lua_pushnumber(L, mode);
        lua_setfield(L, -2, "mode");
        lua_pushstring(L, line.c_str());
        lua_setfield(L, -2, "command");
        lua_pushboolean(L, injected);
        lua_setfield(L, -2, "injected");
        lua_pushboolean(L, blocked);
        lua_setfield(L, -2, "blocked");
        if (const auto err = pcall(L, 2, 1))
        {
            spdlog::error("ashita: {} command: {}", a->name, *err);
            continue;
        }
        lua_getfield(L, -1, "blocked");
        blocked = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
        if (blocked)
        {
            break;
        }
    }
    flush();
    return blocked;
}

bool AddonManager::textOut(std::string& line, int mode)
{
    std::lock_guard lock(m_);
    bool            blocked = false;
    for (const auto& a : addons_)
    {
        lua_State* L = a->L;
        lua_getglobal(L, "__xi_event");
        lua_pushstring(L, "text_out");
        lua_newtable(L);
        lua_pushnumber(L, mode);
        lua_setfield(L, -2, "mode");
        lua_pushnumber(L, mode);
        lua_setfield(L, -2, "mode_modified");
        lua_pushstring(L, line.c_str());
        lua_setfield(L, -2, "message");
        lua_pushstring(L, line.c_str());
        lua_setfield(L, -2, "message_modified");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "injected");
        lua_pushboolean(L, blocked);
        lua_setfield(L, -2, "blocked");
        if (const auto err = pcall(L, 2, 1))
        {
            spdlog::error("ashita: {} text_out: {}", a->name, *err);
            continue;
        }
        lua_getfield(L, -1, "message_modified");
        if (lua_isstring(L, -1))
        {
            line = lua_tostring(L, -1);
        }
        lua_getfield(L, -2, "blocked");
        blocked = lua_toboolean(L, -1) != 0;
        lua_pop(L, 3);
    }
    flush();
    return blocked;
}

namespace
{
// The game's chat modes (Ashita's text_in e.mode) for the session's chat kinds; INFERRED from addons' tables
// (chatmon, timestamp): say 1, shout 2, tell 12 in / 4 out, party 5, linkshell 6, emote 7, yell 11, linkshell2 214,
// system 121, battle 20.
int chatMode(const client::ChatLine& l, const std::string& me)
{
    const bool mine = l.from == me;
    switch (l.kind)
    {
        case 0: return mine ? 9 : 1;
        case 1: return mine ? 10 : 2;
        case 3: return mine ? 4 : 12;
        case 4: return mine ? 13 : 5;
        case 5: return mine ? 14 : 6;
        case 8: return mine ? 15 : 7;
        case 26: return 11;
        case 27: return mine ? 213 : 214;
        case client::kCombatChat:
        case client::kCombatOwn:
        case client::kCombatHurt: return 20;
        case client::kNpcChat: return 150;
        default: return 121;
    }
}
} // namespace

void AddonManager::textIn(const client::ChatLine& line, bool injected)
{
    std::string full = line.text;
    if (!line.from.empty())
    {
        switch (line.kind)
        {
            case 4: full = "(" + line.from + ") " + line.text; break;
            case 5:
            case 27: full = "<" + line.from + "> " + line.text; break;
            case 3: full = line.from + ">> " + line.text; break;
            default: full = line.from + " : " + line.text; break;
        }
    }
    const int   mode     = chatMode(line, frame_.world ? frame_.world->charName : std::string());
    std::string message  = full;
    bool        blocked  = false;
    for (const auto& a : addons_)
    {
        lua_State* L = a->L;
        lua_getglobal(L, "__xi_has_event");
        lua_pushstring(L, "text_in");
        if (pcall(L, 1, 1) || !lua_toboolean(L, -1))
        {
            lua_settop(L, 0);
            continue;
        }
        lua_pop(L, 1);
        lua_getglobal(L, "__xi_event");
        lua_pushstring(L, "text_in");
        lua_newtable(L);
        lua_pushnumber(L, mode);
        lua_setfield(L, -2, "mode");
        lua_pushnumber(L, mode);
        lua_setfield(L, -2, "mode_modified");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "indent");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "indent_modified");
        lua_pushstring(L, full.c_str());
        lua_setfield(L, -2, "message");
        lua_pushstring(L, message.c_str());
        lua_setfield(L, -2, "message_modified");
        lua_pushboolean(L, injected);
        lua_setfield(L, -2, "injected");
        lua_pushboolean(L, blocked);
        lua_setfield(L, -2, "blocked");
        if (const auto err = pcall(L, 2, 1))
        {
            spdlog::error("ashita: {} text_in: {}", a->name, *err);
            continue;
        }
        lua_getfield(L, -1, "message_modified");
        if (lua_isstring(L, -1))
        {
            message = lua_tostring(L, -1);
        }
        lua_getfield(L, -2, "blocked");
        blocked = lua_toboolean(L, -1) != 0;
        lua_pop(L, 3);
    }
    if (blocked || message != full)
    {
        edits_[line.serial] = LineEdit{ blocked, message };
        editOrder_.push_back(line.serial);
        while (editOrder_.size() > 1000)
        {
            edits_.erase(editOrder_.front());
            editOrder_.pop_front();
        }
    }
}

std::optional<AddonManager::LineEdit> AddonManager::lineEdit(uint64_t serial) const
{
    std::lock_guard lock(m_);
    const auto      it = edits_.find(serial);
    if (it == edits_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

bool AddonManager::packet(bool incoming, PacketEvent& e)
{
    std::lock_guard lock(m_);
    const char*     name = incoming ? "packet_in" : "packet_out";
    if (plugins_)
    {
        plugins_->packet(incoming, e);
    }
    std::string     data(reinterpret_cast<const char*>(e.data.data()), e.data.size());
    std::string     modified = data;
    const std::string chunk(reinterpret_cast<const char*>(e.chunk.data()), e.chunk.size());
    for (const auto& a : addons_)
    {
        lua_State* L = a->L;
        lua_getglobal(L, "__xi_packet");
        lua_pushstring(L, name);
        lua_pushnumber(L, e.id);
        lua_pushnumber(L, static_cast<double>(data.size()));
        lua_pushlstring(L, data.data(), data.size());
        lua_pushlstring(L, modified.data(), modified.size());
        lua_pushboolean(L, e.injected);
        lua_pushboolean(L, e.blocked);
        lua_pushlstring(L, chunk.data(), chunk.size());
        if (const auto err = pcall(L, 8, 2))
        {
            spdlog::error("ashita: {} {}: {}", a->name, name, *err);
            continue;
        }
        if (lua_isstring(L, -2))
        {
            std::size_t n = 0;
            const char* s = lua_tolstring(L, -2, &n);
            modified.assign(s, n);
        }
        e.blocked = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
    }
    if (modified != data)
    {
        e.data.assign(reinterpret_cast<const std::byte*>(modified.data()), reinterpret_cast<const std::byte*>(modified.data()) + modified.size());
    }
    if (inUpdate_ || frameNo_ == 0)
    {
        return !e.blocked;
    }
    // Commands / chat an addon queued from the session thread wait for the next update().
    return !e.blocked;
}

bool AddonManager::packetIn(PacketEvent& e)
{
    return packet(true, e);
}

bool AddonManager::packetOut(PacketEvent& e)
{
    return packet(false, e);
}

bool AddonManager::mouse(uint32_t message, int x, int y, int delta)
{
    std::lock_guard lock(m_);
    bool            blocked = false;
    for (const auto& a : addons_)
    {
        lua_State* L = a->L;
        lua_getglobal(L, "__xi_mouse"); // the font / primitive managers' drag, then the mouse events
        lua_newtable(L);
        lua_pushnumber(L, message);
        lua_setfield(L, -2, "message");
        lua_pushnumber(L, x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, delta);
        lua_setfield(L, -2, "delta");
        lua_pushboolean(L, blocked);
        lua_setfield(L, -2, "blocked");
        lua_pushboolean(L, shift_);
        if (const auto err = pcall(L, 2, 1))
        {
            spdlog::error("ashita: {} mouse: {}", a->name, *err);
            continue;
        }
        lua_getfield(L, -1, "blocked");
        blocked = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
    }
    flush();
    return blocked;
}

bool AddonManager::key(uint32_t vk, bool down, bool repeat)
{
    std::lock_guard lock(m_);
    if (vk == kVkShift)
    {
        shift_ = down;
    }
    bool blocked = false;
    for (const auto& a : addons_)
    {
        lua_State* L = a->L;
        lua_getglobal(L, "__xi_event");
        lua_pushstring(L, "key");
        lua_newtable(L);
        lua_pushnumber(L, vk);
        lua_setfield(L, -2, "wparam");
        // lparam: bit 30 the previous state (a repeat), bit 31 key up (WM_KEYDOWN / WM_KEYUP).
        lua_pushnumber(L, (repeat ? 0x40000000u : 0u) | (down ? 0u : 0x80000000u));
        lua_setfield(L, -2, "lparam");
        lua_pushboolean(L, blocked);
        lua_setfield(L, -2, "blocked");
        if (const auto err = pcall(L, 2, 1))
        {
            spdlog::error("ashita: {} key: {}", a->name, *err);
            continue;
        }
        lua_getfield(L, -1, "blocked");
        blocked = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
    }
    flush();
    return blocked;
}

} // namespace xi::ashita
