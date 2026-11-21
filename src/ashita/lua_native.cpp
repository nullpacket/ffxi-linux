// SPDX-License-Identifier: GPL-3.0-or-later
// The native half of the Ashita layer: the `__xi` table the prelude builds Ashita's API on (game state as Lua tables,
// commands, packets, game data, files, regex, the font / primitive drawing) and the `struct` library addons use to
// read and build packets.
#include "ashita/ashita.h"
#include "ashita/lua_util.h"
#include "ashita/scan_registry.h"


#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <regex>
#include <string>

// LuaSocket / LuaSec C modules (CMakeLists.txt: xi_luasocket, xi_luasec).
extern "C" int luaopen_socket_core(lua_State* L);
extern "C" int luaopen_mime_core(lua_State* L);
extern "C" int luaopen_ssl_core(lua_State* L);
extern "C" int luaopen_ssl_context(lua_State* L);
extern "C" int luaopen_ssl_x509(lua_State* L);
extern "C" int luaopen_ssl_config(lua_State* L);

namespace xi::ashita
{

namespace
{
const char* kManagerKey = "__xi_manager";
const char* kAddonKey   = "__xi_addon";
} // namespace

struct Native
{
    static AddonManager& mgr(lua_State* L)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, kManagerKey);
        auto* m = static_cast<AddonManager*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return *m;
    }
    static AddonManager::Addon& addon(lua_State* L)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, kAddonKey);
        auto* a = static_cast<AddonManager::Addon*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return *a;
    }
    static void setNum(lua_State* L, const char* k, double v)
    {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, k);
    }
    static void setStr(lua_State* L, const char* k, std::string_view v)
    {
        lua_pushlstring(L, v.data(), v.size());
        lua_setfield(L, -2, k);
    }
    static void setBool(lua_State* L, const char* k, bool v)
    {
        lua_pushboolean(L, v);
        lua_setfield(L, -2, k);
    }
    // A Windows-style path as Linux finds it: '\\' -> '/', and where the exact path is missing each component matched
    // without regard to case (addons build paths from their own spelling of a folder: tTimers for ttimers). A
    // component with no match keeps its spelling (a file about to be made).
    static std::string resolve(std::string p)
    {
        std::replace(p.begin(), p.end(), '\\', '/');
        std::error_code ec;
        if (p.empty() || std::filesystem::exists(p, ec))
        {
            return p;
        }
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        std::filesystem::path out = p[0] == '/' ? std::filesystem::path("/") : std::filesystem::path();
        bool                  lost = false;
        for (const auto& part : std::filesystem::path(p).relative_path())
        {
            const auto name = part.string();
            if (name.empty() || name == ".")
            {
                continue;
            }
            auto next = out / name;
            if (!lost && !std::filesystem::exists(next, ec))
            {
                lost = true;
                for (const auto& e : std::filesystem::directory_iterator(out.empty() ? std::filesystem::path(".") : out, ec))
                {
                    if (lower(e.path().filename().string()) == lower(name))
                    {
                        next = out / e.path().filename();
                        lost = false;
                        break;
                    }
                }
            }
            out = next;
        }
        return out.string();
    }
    static std::string path(lua_State* L, int i)
    {
        return resolve(luaL_checkstring(L, i));
    }
    static int resolvePath(lua_State* L)
    {
        lua_pushstring(L, resolve(luaL_checkstring(L, 1)).c_str());
        return 1;
    }

    // --- output ---
    static int print(lua_State* L)
    {
        mgr(L).pendingChat_.emplace_back(luaL_optstring(L, 1, ""), 1);
        return 0;
    }
    static int chat(lua_State* L)
    {
        const int mode = static_cast<int>(luaL_optnumber(L, 1, 1));
        if (mode == AddonManager::kErrorMode)
        {
            std::string msg = luaL_optstring(L, 2, "");
            if (const auto b = msg.find("] "); b != std::string::npos && msg.size() > b + 2)
            {
                msg = msg.substr(b + 2); // after "[name] "
            }
            mgr(L).noteError(addon(L).name, std::move(msg));
        }
        mgr(L).pendingChat_.emplace_back(luaL_optstring(L, 2, ""), mode);
        return 0;
    }
    static int queue(lua_State* L)
    {
        auto&       m    = mgr(L);
        std::string line = luaL_checkstring(L, 2);
        m.injectedLines_.push_back(line);
        m.pendingCommands_.emplace_back(std::move(line), static_cast<int>(luaL_optnumber(L, 1, 1)));
        return 0;
    }
    static std::vector<std::byte> bytes(lua_State* L, int i)
    {
        std::vector<std::byte> v;
        if (lua_isstring(L, i))
        {
            std::size_t n = 0;
            const char* s = lua_tolstring(L, i, &n);
            v.resize(n);
            std::memcpy(v.data(), s, n);
        }
        else if (lua_istable(L, i))
        {
            const auto n = lua_objlen(L, i);
            v.resize(n);
            for (std::size_t k = 0; k < n; ++k)
            {
                lua_rawgeti(L, i, static_cast<int>(k + 1));
                v[k] = static_cast<std::byte>(static_cast<uint8_t>(lua_tointeger(L, -1)));
                lua_pop(L, 1);
            }
        }
        return v;
    }
    // AddOutgoingPacket(id, data): data holds the whole packet, header included; the header is rebuilt on send.
    static int send(lua_State* L)
    {
        auto&      m  = mgr(L);
        const auto id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
        auto       v  = bytes(L, 2);
        if (m.host_.send)
        {
            m.host_.send(id, v.size() > 4 ? std::vector<std::byte>(v.begin() + 4, v.end()) : std::vector<std::byte>{});
        }
        return 0;
    }
    static int inject(lua_State* L)
    {
        auto&      m  = mgr(L);
        const auto id = static_cast<uint16_t>(luaL_checkinteger(L, 1));
        auto       v  = bytes(L, 2);
        v.resize(std::max<std::size_t>((v.size() + 3) & ~std::size_t{ 3 }, 4));
        const auto hdr = static_cast<uint16_t>((id & 0x1FF) | ((v.size() / 4) << 9));
        v[0]           = static_cast<std::byte>(hdr & 0xFF);
        v[1]           = static_cast<std::byte>(hdr >> 8);
        if (m.host_.inject)
        {
            m.host_.inject(std::move(v));
        }
        return 0;
    }
    static int setTarget(lua_State* L)
    {
        auto& m = mgr(L);
        if (m.host_.target)
        {
            m.host_.target(static_cast<uint16_t>(luaL_checkinteger(L, 1)));
        }
        return 0;
    }
    static int log(lua_State* L)
    {
        const auto  level = static_cast<int>(luaL_optnumber(L, 1, 4));
        const char* msg   = luaL_optstring(L, 2, "");
        const auto& a     = addon(L);
        switch (level)
        {
            case 1: spdlog::critical("ashita [{}] {}", a.name, msg); break;
            case 2: spdlog::error("ashita [{}] {}", a.name, msg); break;
            case 3: spdlog::warn("ashita [{}] {}", a.name, msg); break;
            case 5: spdlog::debug("ashita [{}] {}", a.name, msg); break;
            default: spdlog::info("ashita [{}] {}", a.name, msg); break;
        }
        return 0;
    }

    // --- files (paths with '\\' work: Ashita addons build Windows paths) ---
    static int fsExists(lua_State* L)
    {
        std::error_code ec;
        lua_pushboolean(L, std::filesystem::exists(path(L, 1), ec));
        return 1;
    }
    static int fsMkdir(lua_State* L)
    {
        std::error_code ec;
        std::filesystem::create_directories(path(L, 1), ec);
        lua_pushboolean(L, !ec);
        return 1;
    }
    static int fsRemove(lua_State* L)
    {
        std::error_code ec;
        lua_pushboolean(L, std::filesystem::remove_all(path(L, 1), ec) > 0);
        return 1;
    }
    static int fsRename(lua_State* L)
    {
        std::error_code ec;
        std::filesystem::rename(path(L, 1), path(L, 2), ec);
        lua_pushboolean(L, !ec);
        return 1;
    }
    static int fsSize(lua_State* L)
    {
        std::error_code ec;
        const auto      n = std::filesystem::file_size(path(L, 1), ec);
        lua_pushnumber(L, ec ? 0.0 : static_cast<double>(n));
        return 1;
    }
    // get_directory(path, mask, subfolders): file names matching a regex mask; nil when the folder is missing.
    static int fsList(lua_State* L)
    {
        std::error_code ec;
        const auto      dir = std::filesystem::path(path(L, 1));
        if (!std::filesystem::is_directory(dir, ec))
        {
            lua_pushnil(L);
            return 1;
        }
        std::optional<std::regex> mask;
        if (lua_isstring(L, 2))
        {
            try
            {
                mask.emplace(lua_tostring(L, 2), std::regex::icase);
            }
            catch (const std::regex_error&)
            {
            }
        }
        const bool subfolders = lua_toboolean(L, 3) != 0;
        lua_newtable(L);
        int  n   = 1;
        auto add = [&](const std::filesystem::directory_entry& e) {
            const auto name = subfolders ? std::filesystem::relative(e.path(), dir, ec).generic_string() : e.path().filename().string();
            if (mask && !std::regex_match(e.path().filename().string(), *mask))
            {
                return;
            }
            lua_pushstring(L, name.c_str());
            lua_rawseti(L, -2, n++);
        };
        if (subfolders)
        {
            for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec))
            {
                add(e);
            }
        }
        else
        {
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            {
                add(e);
            }
        }
        return 1;
    }
    static int fsIsDir(lua_State* L)
    {
        std::error_code ec;
        lua_pushboolean(L, std::filesystem::is_directory(path(L, 1), ec));
        return 1;
    }

    // --- misc ---
    static int clock(lua_State* L)
    {
        using namespace std::chrono;
        static const auto t0 = steady_clock::now();
        lua_pushnumber(L, duration<double>(steady_clock::now() - t0).count());
        return 1;
    }
    static int focused(lua_State* L)
    {
        lua_pushboolean(L, SDL_GetKeyboardFocus() != nullptr);
        return 1;
    }
    static int getClipboard(lua_State* L)
    {
        char* t = SDL_GetClipboardText();
        lua_pushstring(L, t ? t : "");
        SDL_free(t);
        return 1;
    }
    static int setClipboard(lua_State* L)
    {
        lua_pushboolean(L, SDL_SetClipboardText(luaL_optstring(L, 1, "")));
        return 1;
    }

    // --- regex (ashita.regex: ECMAScript patterns) ---
    static std::optional<std::regex> re(lua_State* L, int i)
    {
        try
        {
            return std::regex(luaL_checkstring(L, i));
        }
        catch (const std::regex_error& e)
        {
            spdlog::warn("ashita [{}] bad regex '{}': {}", addon(L).name, lua_tostring(L, i), e.what());
            return std::nullopt;
        }
    }
    static void pushMatch(lua_State* L, const std::smatch& m)
    {
        lua_newtable(L);
        for (std::size_t k = 0; k < m.size(); ++k)
        {
            lua_pushstring(L, m[k].str().c_str());
            lua_rawseti(L, -2, static_cast<int>(k + 1));
        }
    }
    static int reMatch(lua_State* L)
    {
        const std::string s = luaL_checkstring(L, 1);
        const auto        r = re(L, 2);
        std::smatch       m;
        if (!r || !std::regex_match(s, m, *r))
        {
            lua_pushnil(L);
            return 1;
        }
        pushMatch(L, m);
        return 1;
    }
    static int reSearch(lua_State* L)
    {
        const std::string s = luaL_checkstring(L, 1);
        const auto        r = re(L, 2);
        std::smatch       m;
        if (!r || !std::regex_search(s, m, *r))
        {
            lua_pushnil(L);
            return 1;
        }
        pushMatch(L, m);
        return 1;
    }
    static int reReplace(lua_State* L)
    {
        const std::string s = luaL_checkstring(L, 1);
        const auto        r = re(L, 2);
        lua_pushstring(L, r ? std::regex_replace(s, *r, luaL_optstring(L, 3, "")).c_str() : s.c_str());
        return 1;
    }
    static int reSplit(lua_State* L)
    {
        const std::string s = luaL_checkstring(L, 1);
        const auto        r = re(L, 2);
        lua_newtable(L);
        if (!r)
        {
            return 1;
        }
        int n = 1;
        for (std::sregex_token_iterator it(s.begin(), s.end(), *r, -1), end; it != end; ++it)
        {
            lua_pushstring(L, it->str().c_str());
            lua_rawseti(L, -2, n++);
        }
        return 1;
    }

    // --- drawing: font objects and primitives, on the foreground draw list (present()) ---
    // Font faces by family (+ ":bold"); a face not loaded yet is asked for and comes with the next update() (fonts are
    // not added inside an ImGui frame).
    static std::unordered_map<std::string, ImFont*>& fonts()
    {
        static std::unordered_map<std::string, ImFont*> f;
        return f;
    }
    static std::vector<std::pair<std::string, bool>>& fontRequests()
    {
        static std::vector<std::pair<std::string, bool>> r;
        return r;
    }
    static ImFont* font(const std::string& family, bool bold)
    {
        const std::string key = family + (bold ? ":bold" : "");
        if (const auto it = fonts().find(key); it != fonts().end())
        {
            return it->second;
        }
        fonts().emplace(key, nullptr);
        fontRequests().emplace_back(family, bold);
        return nullptr;
    }
    static void loadFonts()
    {
        for (const auto& [family, bold] : fontRequests())
        {
            // fontconfig picks the closest installed face (Arial -> Liberation Sans ...). Only plain names reach the shell.
            std::string name;
            for (const char c : family)
            {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-')
                {
                    name.push_back(c);
                }
            }
            const std::string cmd = "fc-match -f '%{file}' '" + (name.empty() ? std::string("sans") : name) + (bold ? ":bold" : "") + "' 2>/dev/null";
            ImFont*           f   = nullptr;
            if (FILE* p = popen(cmd.c_str(), "r"))
            {
                char       buf[1024] = {};
                const auto n         = fread(buf, 1, sizeof buf - 1, p);
                pclose(p);
                const std::string file(buf, n);
                std::error_code   ec;
                if (!file.empty() && std::filesystem::exists(file, ec) && (file.ends_with(".ttf") || file.ends_with(".otf") || file.ends_with(".TTF")))
                {
                    f = ImGui::GetIO().Fonts->AddFontFromFileTTF(file.c_str());
                }
            }
            fonts()[family + (bold ? ":bold" : "")] = f;
            spdlog::info("ashita: font '{}'{} -> {}", family, bold ? " bold" : "", f ? "loaded" : "default");
        }
        fontRequests().clear();
    }
    // text(x, y, s, family, height, colour, bold, outline colour, outline): draws one line set; returns w, h.
    static int text(lua_State* L)
    {
        const auto  x       = static_cast<float>(luaL_checknumber(L, 1));
        const auto  y       = static_cast<float>(luaL_checknumber(L, 2));
        const char* s       = luaL_optstring(L, 3, "");
        const auto  family  = std::string(luaL_optstring(L, 4, "Arial"));
        // Ashita font heights are GDI point sizes at 96 dpi: pixels = height * 4 / 3.
        const float px      = static_cast<float>(luaL_optnumber(L, 5, 10)) * 4.0f / 3.0f;
        const auto  colour  = static_cast<uint32_t>(luaL_optnumber(L, 6, 0xFFFFFFFF));
        const bool  bold    = lua_toboolean(L, 7) != 0;
        const auto  outline = static_cast<uint32_t>(luaL_optnumber(L, 8, 0));
        const bool  measure = lua_toboolean(L, 9) != 0;
        ImFont*     f       = font(family, bold);
        if (!f)
        {
            f = ImGui::GetFont();
        }
        const ImVec2 size = f->CalcTextSizeA(px, FLT_MAX, 0.0f, s);
        if (!measure)
        {
            // D3DCOLOR ARGB -> ImGui ABGR.
            auto conv = [](uint32_t c) { return (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16); };
            auto* dl  = ImGui::GetForegroundDrawList();
            if (outline >> 24)
            {
                for (const auto& o : { ImVec2{ -1, 0 }, ImVec2{ 1, 0 }, ImVec2{ 0, -1 }, ImVec2{ 0, 1 } })
                {
                    dl->AddText(f, px, ImVec2(x + o.x, y + o.y), conv(outline), s);
                }
            }
            dl->AddText(f, px, ImVec2(x, y), conv(colour), s);
        }
        lua_pushnumber(L, size.x);
        lua_pushnumber(L, size.y);
        return 2;
    }
    // rect(x, y, w, h, colour, border colour, border width, borders): a filled box (font backgrounds, primitives).
    static int rect(lua_State* L)
    {
        const auto x  = static_cast<float>(luaL_checknumber(L, 1));
        const auto y  = static_cast<float>(luaL_checknumber(L, 2));
        const auto w  = static_cast<float>(luaL_checknumber(L, 3));
        const auto h  = static_cast<float>(luaL_checknumber(L, 4));
        const auto c  = static_cast<uint32_t>(luaL_optnumber(L, 5, 0x80000000));
        const auto bc = static_cast<uint32_t>(luaL_optnumber(L, 6, 0));
        const auto bw = static_cast<float>(luaL_optnumber(L, 7, 0));
        auto conv = [](uint32_t v) { return (v & 0xFF00FF00u) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16); };
        auto* dl  = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), conv(c));
        if (bw > 0 && (bc >> 24))
        {
            dl->AddRect(ImVec2(x - bw, y - bw), ImVec2(x + w + bw, y + h + bw), conv(bc), 0.0f, 0, bw);
        }
        return 0;
    }
    static int screen(lua_State* L)
    {
        const auto& fr = mgr(L).frame_;
        lua_pushnumber(L, fr.windowW);
        lua_pushnumber(L, fr.windowH);
        lua_pushnumber(L, fr.menuW);
        lua_pushnumber(L, fr.menuH);
        return 4;
    }
};

namespace
{

// struct.pack / struct.unpack (Roberto Ierusalimschy's struct library, as Ashita bundles it): formats < > ! x b B h H
// i[n] I[n] l L T f d s z c[n]; long and size_t are 4 bytes, as in the 32-bit client.
struct Fmt
{
    bool little = true;
};

std::size_t optSize(const char*& f, std::size_t def)
{
    if (!std::isdigit(static_cast<unsigned char>(*f)))
    {
        return def;
    }
    std::size_t n = 0;
    while (std::isdigit(static_cast<unsigned char>(*f)))
    {
        n = n * 10 + static_cast<std::size_t>(*f++ - '0');
    }
    return n;
}

void putInt(std::string& out, uint64_t v, std::size_t n, bool little)
{
    for (std::size_t k = 0; k < n; ++k)
    {
        const auto shift = 8 * (little ? k : n - 1 - k);
        out.push_back(static_cast<char>(shift < 64 ? (v >> shift) & 0xFF : 0));
    }
}

int structPack(lua_State* L)
{
    const char* f    = luaL_checkstring(L, 1);
    int         arg  = 2;
    bool        little = true;
    std::string out;
    while (*f)
    {
        const char c = *f++;
        switch (c)
        {
            case '<': little = true; break;
            case '>': little = false; break;
            case '=': little = true; break;
            case '!': optSize(f, 4); break;
            case ' ': break;
            case 'x': out.push_back('\0'); break;
            case 'b':
            case 'B': putInt(out, static_cast<uint64_t>(static_cast<int64_t>(luaL_checknumber(L, arg++))), 1, little); break;
            case 'h':
            case 'H': putInt(out, static_cast<uint64_t>(static_cast<int64_t>(luaL_checknumber(L, arg++))), 2, little); break;
            case 'l':
            case 'L':
            case 'T': putInt(out, static_cast<uint64_t>(static_cast<int64_t>(luaL_checknumber(L, arg++))), 4, little); break;
            case 'i':
            case 'I':
            {
                const auto n = optSize(f, 4);
                putInt(out, static_cast<uint64_t>(static_cast<int64_t>(luaL_checknumber(L, arg++))), n, little);
                break;
            }
            case 'f':
            {
                const auto v = std::bit_cast<uint32_t>(static_cast<float>(luaL_checknumber(L, arg++)));
                putInt(out, v, 4, little);
                break;
            }
            case 'd':
            {
                const auto v = std::bit_cast<uint64_t>(luaL_checknumber(L, arg++));
                putInt(out, v, 8, little);
                break;
            }
            case 's':
            case 'z':
            {
                std::size_t n = 0;
                const char* s = luaL_checklstring(L, arg++, &n);
                out.append(s, n);
                out.push_back('\0');
                break;
            }
            case 'c':
            {
                std::size_t       n   = 0;
                const char*       s   = luaL_checklstring(L, arg++, &n);
                const std::size_t len = optSize(f, 1);
                std::string       v(s, std::min(n, len == 0 ? n : len));
                v.resize(len == 0 ? n : len, '\0');
                out += v;
                break;
            }
            default: return luaL_error(L, "struct.pack: invalid format option '%c'", c);
        }
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

uint64_t getInt(const unsigned char* p, std::size_t n, bool little)
{
    uint64_t v = 0;
    for (std::size_t k = 0; k < n; ++k)
    {
        v |= static_cast<uint64_t>(p[little ? k : n - 1 - k]) << (8 * k);
    }
    return v;
}

double signedOf(uint64_t v, std::size_t n)
{
    if (n >= 8)
    {
        return static_cast<double>(static_cast<int64_t>(v));
    }
    if (n == 0)
    {
        return 0; // no bytes read; the shift below would be by -1
    }
    const uint64_t sign = uint64_t{ 1 } << (8 * n - 1);
    return (v & sign) ? static_cast<double>(static_cast<int64_t>(v) - static_cast<int64_t>(sign << 1)) : static_cast<double>(v);
}

int structUnpack(lua_State* L)
{
    const char* f    = luaL_checkstring(L, 1);
    std::size_t len  = 0;
    const auto* data = reinterpret_cast<const unsigned char*>(luaL_checklstring(L, 2, &len));
    auto        pos  = static_cast<std::size_t>(luaL_optinteger(L, 3, 1));
    luaL_argcheck(L, pos > 0, 3, "offset must be 1 or greater");
    --pos;
    bool little = true;
    int  n      = 0;
    auto need   = [&](std::size_t k) {
        if (pos + k > len)
        {
            luaL_error(L, "struct.unpack: data string too short");
        }
    };
    std::size_t lastCount = 0;
    while (*f)
    {
        const char c = *f++;
        switch (c)
        {
            case '<': little = true; break;
            case '>': little = false; break;
            case '=': little = true; break;
            case '!': optSize(f, 4); break;
            case ' ': break;
            case 'x': ++pos; break;
            case 'b':
            case 'B':
            case 'h':
            case 'H':
            case 'l':
            case 'L':
            case 'T':
            case 'i':
            case 'I':
            {
                std::size_t sz = (c == 'b' || c == 'B') ? 1 : (c == 'h' || c == 'H') ? 2 : 4;
                if (c == 'i' || c == 'I')
                {
                    sz = optSize(f, 4);
                }
                need(sz);
                const auto v = getInt(data + pos, sz, little);
                const bool isSigned = c == 'b' || c == 'h' || c == 'l' || c == 'i';
                const double d = isSigned ? signedOf(v, sz) : static_cast<double>(v);
                lua_pushnumber(L, d);
                lastCount = static_cast<std::size_t>(d);
                pos += sz;
                ++n;
                break;
            }
            case 'f':
            {
                need(4);
                lua_pushnumber(L, std::bit_cast<float>(static_cast<uint32_t>(getInt(data + pos, 4, little))));
                pos += 4;
                ++n;
                break;
            }
            case 'd':
            {
                need(8);
                lua_pushnumber(L, std::bit_cast<double>(getInt(data + pos, 8, little)));
                pos += 8;
                ++n;
                break;
            }
            case 's':
            case 'z':
            {
                const auto* e = static_cast<const unsigned char*>(std::memchr(data + pos, 0, len - pos));
                const auto  k = e ? static_cast<std::size_t>(e - (data + pos)) : len - pos;
                lua_pushlstring(L, reinterpret_cast<const char*>(data + pos), k);
                pos += k + (e ? 1 : 0);
                ++n;
                break;
            }
            case 'c':
            {
                std::size_t k = optSize(f, 1);
                if (k == 0)
                {
                    k = lastCount; // c0: the length is the previous value
                    if (n > 0)
                    {
                        lua_pop(L, 1);
                        --n;
                    }
                }
                need(k);
                lua_pushlstring(L, reinterpret_cast<const char*>(data + pos), k);
                pos += k;
                ++n;
                break;
            }
            default: return luaL_error(L, "struct.unpack: invalid format option '%c'", c);
        }
        luaL_checkstack(L, 2, "struct.unpack: too many results");
    }
    lua_pushnumber(L, static_cast<double>(pos + 1));
    return n + 1;
}

int structSize(lua_State* L)
{
    const char* f = luaL_checkstring(L, 1);
    std::size_t n = 0;
    while (*f)
    {
        switch (*f++)
        {
            case 'x':
            case 'b':
            case 'B': n += 1; break;
            case 'h':
            case 'H': n += 2; break;
            case 'l':
            case 'L':
            case 'T':
            case 'f': n += 4; break;
            case 'd': n += 8; break;
            case 'i':
            case 'I': n += optSize(f, 4); break;
            case 'c': n += optSize(f, 1); break;
            default: break;
        }
    }
    lua_pushnumber(L, static_cast<double>(n));
    return 1;
}

} // namespace

AddonManager& managerOf(lua_State* L)
{
    return Native::mgr(L);
}

void loadPendingFonts()
{
    Native::loadFonts();
}

void openNative(lua_State* L, AddonManager* m, void* a)
{
    lua_pushlightuserdata(L, m);
    lua_setfield(L, LUA_REGISTRYINDEX, kManagerKey);
    lua_pushlightuserdata(L, a);
    lua_setfield(L, LUA_REGISTRYINDEX, kAddonKey);

    const luaL_Reg xi[] = {
        { "print", Native::print },
        { "chat", Native::chat },
        { "queue", Native::queue },
        { "send", Native::send },
        { "inject", Native::inject },
        { "set_target", Native::setTarget },
        { "log", Native::log },
        { "fs_exists", Native::fsExists },
        { "fs_mkdir", Native::fsMkdir },
        { "fs_remove", Native::fsRemove },
        { "fs_rename", Native::fsRename },
        { "fs_size", Native::fsSize },
        { "fs_list", Native::fsList },
        { "fs_isdir", Native::fsIsDir },
        { "clock", Native::clock },
        { "get_clipboard", Native::getClipboard },
        { "focused", Native::focused },
        { "resolve", Native::resolvePath },
        { "set_clipboard", Native::setClipboard },
        { "re_match", Native::reMatch },
        { "re_search", Native::reSearch },
        { "re_replace", Native::reReplace },
        { "re_split", Native::reSplit },
        { "text", Native::text },
        { "rect", Native::rect },
        { "screen", Native::screen },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    for (const auto* f = xi; f->name; ++f)
    {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
    lua_pushstring(L, profileVersion(m->profile()));
    lua_setfield(L, -2, "version");
    lua_pushstring(L, m->profile() == Profile::HorizonXI ? "horizonxi" : "latest");
    lua_setfield(L, -2, "profile");
    lua_pushstring(L, m->root().string().c_str());
    lua_setfield(L, -2, "root");
    const auto& ad = *static_cast<AddonManager::Addon*>(a);
    lua_pushstring(L, ad.name.c_str());
    lua_setfield(L, -2, "addon");
    lua_pushstring(L, ad.dir.string().c_str());
    lua_setfield(L, -2, "addon_dir");
    openState(L); // lua_state.cpp: the game-state and game-data functions
    openScan(L);  // scan_registry.cpp: the memory stand-ins
    lua_setglobal(L, "__xi");

    const luaL_Reg st[] = { { "pack", structPack }, { "unpack", structUnpack }, { "size", structSize }, { nullptr, nullptr } };
    lua_newtable(L);
    for (const auto* f = st; f->name; ++f)
    {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
    lua_setglobal(L, "struct");

    // require 'socket.core' / 'mime.core' / 'ssl.core' ...: the linked-in modules (addons/libs has their Lua halves).
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    const luaL_Reg net[] = { { "socket.core", luaopen_socket_core }, { "mime.core", luaopen_mime_core },
                             { "ssl.core", luaopen_ssl_core },       { "ssl.context", luaopen_ssl_context },
                             { "ssl.x509", luaopen_ssl_x509 },       { "ssl.config", luaopen_ssl_config },
                             { nullptr, nullptr } };
    for (const auto* f = net; f->name; ++f)
    {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
    lua_pop(L, 2);
    openImgui(L);
    openD3d(L);
}

} // namespace xi::ashita
