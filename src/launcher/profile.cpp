// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/profile.h"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace xi::launcher
{

namespace
{

std::string fmtFloat(float v)
{
    // Shortest text that reads back the same ("0.5", "1.3", "3").
    std::string s = fmt::format("{}", v);
    return s;
}

std::optional<float> parseFloat(std::string_view s)
{
    float v = 0;
    if (s.empty())
    {
        return std::nullopt;
    }
    // std::from_chars for floats is in libstdc++ 11+.
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc())
    {
        return std::nullopt;
    }
    return v;
}

std::optional<int> parseInt(std::string_view s)
{
    int v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || r.ec != std::errc())
    {
        return std::nullopt;
    }
    return v;
}

std::string trim(std::string_view s)
{
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos)
    {
        return {};
    }
    const auto b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

// One table of every settings field: key name and member, visited by the writer and the reader.
template <typename V>
void visitSettings(Settings& s, V&& v)
{
    auto& g = s.graphics;
    v("graphics.dlss", g.dlss);
    v("graphics.upscaler", g.upscaler);
    v("graphics.rt", g.rt);
    v("graphics.rt_ao", g.rtAo);
    v("graphics.rt_shadows", g.rtShadows);
    v("graphics.rt_gi", g.rtGi);
    v("graphics.rt_reflections", g.rtReflections);
    v("graphics.rt_strength", g.rtStrength);
    v("graphics.rt_shadow_cap", g.rtShadowCap);
    v("graphics.rt_reflect", g.rtReflect);
    v("graphics.rt_boost", g.rtBoost);
    v("graphics.post", g.post);
    v("graphics.post_hdr", g.postHdr);
    v("graphics.post_bloom", g.postBloom);
    v("graphics.post_godrays", g.postGodrays);
    v("graphics.post_fog", g.postFog);
    v("graphics.post_bloom_strength", g.postBloomStrength);
    v("graphics.post_shafts_strength", g.postShaftsStrength);
    v("graphics.post_fog_strength", g.postFogStrength);
    v("graphics.pathtrace", g.pathtrace);
    v("graphics.pathtrace_bounces", g.ptBounces);
    v("graphics.pathtrace_blend", g.ptBlend);
    v("graphics.pathtrace_exposure", g.ptExposure);
    v("graphics.pathtrace_baked", g.ptBaked);
    v("graphics.hd_textures", g.hdTextures);
    v("graphics.hd_dir", g.hdDir);
    auto& d = s.display;
    v("display.width", d.width);
    v("display.height", d.height);
    v("display.display", d.display);
    v("display.mode", d.mode);
    v("display.bg_width", d.bgWidth);
    v("display.bg_height", d.bgHeight);
    v("display.menu_width", d.menuWidth);
    v("display.menu_height", d.menuHeight);
    v("display.window_skin", d.windowSkin);
    auto& o = s.game;
    v("game.sound_volume", o.soundVolume);
    v("game.music_volume", o.musicVolume);
    v("game.keyboard", o.keyboard);
    v("game.login_protocol", o.loginProtocol);
    v("game.addons", o.addons);
    v("game.ashita_dir", o.ashitaDir);
    v("game.ashita_version", o.ashitaVersion);
    v("game.ashita_load", o.ashitaLoad);
    v("game.macro_dir", o.macroDir);
    v("extra_args", s.extraArgs);
}

template <typename V>
void visitProfile(Profile& p, V&& v)
{
    v("name", p.name);
    v("host", p.host);
    v("auth_port", p.authPort);
    v("data_port", p.dataPort);
    v("view_port", p.viewPort);
    v("search_port", p.searchPort);
    v("user", p.user);
    v("character", p.character);
    v("game_dir", p.gameDir);
    v("save_password", p.savePassword);
    v("own_settings", p.ownSettings);
    v("entity_renames", p.entityRenames);
}

struct Writer
{
    std::string& out;
    void line(const char* k, std::string_view v) { out += fmt::format("{} = {}\n", k, v); }
    void operator()(const char* k, std::string& v) { line(k, v); }
    void operator()(const char* k, bool& v) { line(k, v ? "1" : "0"); }
    void operator()(const char* k, int& v) { line(k, std::to_string(v)); }
    void operator()(const char* k, uint16_t& v) { line(k, std::to_string(v)); }
    void operator()(const char* k, float& v) { line(k, fmtFloat(v)); }
    void operator()(const char* k, std::optional<float>& v) { line(k, v ? fmtFloat(*v) : ""); }
};

struct Reader
{
    const std::map<std::string, std::string>& kv;
    const std::string*                        get(const char* k) const
    {
        const auto it = kv.find(k);
        return it == kv.end() ? nullptr : &it->second;
    }
    void operator()(const char* k, std::string& v) const
    {
        if (const auto* s = get(k)) v = *s;
    }
    void operator()(const char* k, bool& v) const
    {
        if (const auto* s = get(k)) v = (*s == "1" || *s == "true" || *s == "yes" || *s == "on");
    }
    void operator()(const char* k, int& v) const
    {
        if (const auto* s = get(k); s && parseInt(*s)) v = *parseInt(*s);
    }
    void operator()(const char* k, uint16_t& v) const
    {
        if (const auto* s = get(k); s && parseInt(*s)) v = static_cast<uint16_t>(std::clamp(*parseInt(*s), 1, 65535));
    }
    void operator()(const char* k, float& v) const
    {
        if (const auto* s = get(k); s && parseFloat(*s)) v = *parseFloat(*s);
    }
    void operator()(const char* k, std::optional<float>& v) const
    {
        if (const auto* s = get(k)) v = parseFloat(*s);
    }
};

std::string joinCsv(std::initializer_list<std::pair<bool, const char*>> parts)
{
    std::string out;
    for (const auto& [on, name] : parts)
    {
        if (on)
        {
            out += (out.empty() ? "" : ",") + std::string(name);
        }
    }
    return out;
}

} // namespace

const char* presetName(Preset p)
{
    switch (p)
    {
        case Preset::Original: return "Original";
        case Preset::OriginalSharp: return "Original, sharp";
        case Preset::Subtle: return "Subtle";
        case Preset::Enhanced: return "Enhanced";
        case Preset::Dramatic: return "Dramatic";
        case Preset::Remaster: return "Remaster";
        case Preset::RemasterHalf: return "Remaster, half-way";
    }
    return "?";
}

const char* presetDescription(Preset p)
{
    switch (p)
    {
        case Preset::Original: return "The game as it was.";
        case Preset::OriginalSharp: return "The same look, with DLAA anti-aliasing.";
        case Preset::Subtle: return "Soft ray-traced occlusion and shadows; clearly FFXI.";
        case Preset::Enhanced: return "Deeper shadows, glow and light shafts.";
        case Preset::Dramatic: return "Hazy sunlit sky, deep shadows, mirror-like water.";
        case Preset::Remaster: return "Lighting rebuilt by path tracing, plus HD textures.";
        case Preset::RemasterHalf: return "Half-way between the original and the remaster.";
    }
    return "";
}

Graphics presetGraphics(Preset p, const Graphics& keepFrom)
{
    Graphics g;
    g.hdDir    = keepFrom.hdDir;
    g.upscaler = keepFrom.upscaler; // which upscaler is the machine's choice, not the preset's (#139)
    switch (p)
    {
        case Preset::Original: break;
        case Preset::OriginalSharp: g.dlss = "dlaa"; break;
        case Preset::Subtle: g.rt = true; break;
        case Preset::Enhanced:
            g.rtBoost = "high";
            g.post    = true;
            break;
        case Preset::Dramatic:
            g.rtBoost            = "max";
            g.post               = true;
            g.postBloomStrength  = 3;
            g.postShaftsStrength = 3;
            g.postFogStrength    = 3;
            break;
        case Preset::Remaster:
            g.dlss       = "quality";
            g.pathtrace  = true;
            g.post       = true;
            g.hdTextures = true;
            break;
        case Preset::RemasterHalf:
            g.dlss      = "quality";
            g.pathtrace = true;
            g.ptBlend   = 0.5f;
            g.post      = true;
            break;
    }
    return g;
}

std::optional<Preset> matchPreset(const Graphics& g)
{
    for (const Preset p : kPresets)
    {
        if (presetGraphics(p, g) == g)
        {
            return p;
        }
    }
    return std::nullopt;
}

std::vector<std::string> graphicsArgs(const Graphics& g)
{
    std::vector<std::string> a;
    if (!g.dlss.empty())
    {
        // The mode is the upscaler's; --dlss on a GPU without DLSS (AMD, Intel) starts FSR with it (#139).
        const std::string flag = g.upscaler == "fsr" ? "--fsr" : "--dlss";
        a.push_back(g.dlss == "quality" ? flag : flag + "=" + g.dlss);
    }
    if (g.rt)
    {
        const bool plain = g.rtAo && g.rtShadows && !g.rtGi && !g.rtReflections;
        const bool all   = g.rtAo && g.rtShadows && g.rtGi && g.rtReflections;
        a.push_back(plain ? std::string("--rt")
                    : all ? std::string("--rt=all")
                          : "--rt=" + joinCsv({ { g.rtAo, "ao" }, { g.rtShadows, "shadows" }, { g.rtGi, "gi" }, { g.rtReflections, "reflections" } }));
    }
    if (!g.rtBoost.empty())
    {
        a.push_back(g.rtBoost == "high" ? "--rt-boost" : "--rt-boost=" + g.rtBoost);
    }
    if (g.rtStrength)
    {
        a.push_back("--rt-strength=" + fmtFloat(*g.rtStrength));
    }
    if (g.rtShadowCap)
    {
        a.push_back("--rt-shadow-cap=" + fmtFloat(*g.rtShadowCap));
    }
    if (g.rtReflect)
    {
        a.push_back("--rt-reflect=" + fmtFloat(*g.rtReflect));
    }
    if (g.post)
    {
        const bool all = g.postHdr && g.postBloom && g.postGodrays && g.postFog;
        a.push_back(all ? std::string("--post")
                        : "--post=" + joinCsv({ { g.postHdr, "hdr" }, { g.postBloom, "bloom" }, { g.postGodrays, "godrays" }, { g.postFog, "fog" } }));
        if (g.postBloomStrength != 1)
        {
            a.push_back("--post-bloom=" + fmtFloat(g.postBloomStrength));
        }
        if (g.postShaftsStrength != 1)
        {
            a.push_back("--post-shafts=" + fmtFloat(g.postShaftsStrength));
        }
        if (g.postFogStrength != 1)
        {
            a.push_back("--post-fog=" + fmtFloat(g.postFogStrength));
        }
    }
    if (g.pathtrace)
    {
        a.push_back(g.ptBounces == 2 ? std::string("--pathtrace") : fmt::format("--pathtrace={}", std::clamp(g.ptBounces, 1, 3)));
        if (g.ptBlend != 1)
        {
            a.push_back("--pathtrace-blend=" + fmtFloat(g.ptBlend));
        }
        if (g.ptExposure != 0)
        {
            a.push_back("--pathtrace-exposure=" + fmtFloat(g.ptExposure));
        }
        if (g.ptBaked != 0.3f)
        {
            a.push_back("--pathtrace-baked=" + fmtFloat(g.ptBaked));
        }
    }
    if (g.hdTextures)
    {
        a.push_back(g.hdDir.empty() ? std::string("--hd-textures") : "--hd-textures=" + g.hdDir);
    }
    return a;
}

std::vector<std::string> buildArgs(const Profile& p, const Settings& s, const std::filesystem::path& overlayIni, bool havePassword)
{
    std::vector<std::string> a{ "--host", p.host };
    const Profile def;
    if (p.authPort != def.authPort) a.insert(a.end(), { "--auth-port", std::to_string(p.authPort) });
    if (p.dataPort != def.dataPort) a.insert(a.end(), { "--data-port", std::to_string(p.dataPort) });
    if (p.viewPort != def.viewPort) a.insert(a.end(), { "--view-port", std::to_string(p.viewPort) });
    if (p.searchPort != def.searchPort) a.insert(a.end(), { "--search-port", std::to_string(p.searchPort) });
    if (!p.gameDir.empty()) a.insert(a.end(), { "--root", p.gameDir });
    if (!overlayIni.empty()) a.insert(a.end(), { "--config", overlayIni.string() });
    if (p.entityRenames)
        a.emplace_back("--entity-renames");
    if (!p.user.empty())
    {
        a.insert(a.end(), { "--user", p.user });
        a.insert(a.end(), { "--char", p.character }); // "" = choose in the lobby
        if (havePassword)
        {
            a.push_back("--autoconnect");
        }
    }
    const auto& d = s.display;
    if (d.width > 0 && d.height > 0) a.insert(a.end(), { "--res", fmt::format("{}x{}", d.width, d.height) });
    if (!d.display.empty()) a.insert(a.end(), { "--display", d.display });
    if (d.mode >= 0) a.insert(a.end(), { "--window-mode", std::to_string(std::clamp(d.mode, 0, 3)) });
    if (d.bgWidth > 0 && d.bgHeight > 0) a.insert(a.end(), { "--bg-res", fmt::format("{}x{}", d.bgWidth, d.bgHeight) });
    if (d.menuWidth > 0 && d.menuHeight > 0) a.insert(a.end(), { "--menu-res", fmt::format("{}x{}", d.menuWidth, d.menuHeight) });
    if (d.windowSkin >= 1) a.insert(a.end(), { "--window-skin", std::to_string(std::clamp(d.windowSkin, 1, 8)) });
    for (auto& g : graphicsArgs(s.graphics))
    {
        a.push_back(std::move(g));
    }
    for (auto& e : splitArgs(s.extraArgs))
    {
        a.push_back(std::move(e));
    }
    return a;
}

std::string overlayIni(const Settings& s)
{
    std::string body;
    const auto& o = s.game;
    if (o.soundVolume >= 0) body += fmt::format("sound_volume = {}\n", std::clamp(o.soundVolume, 0, 100));
    if (o.musicVolume >= 0) body += fmt::format("music_volume = {}\n", std::clamp(o.musicVolume, 0, 100));
    if (o.keyboard >= 0) body += fmt::format("keyboard = {}\n", std::clamp(o.keyboard, 0, 2));
    if (!o.loginProtocol.empty()) body += fmt::format("login_protocol = {}\n", o.loginProtocol);
    if (o.addons == 0)
    {
        body += "ashita_dir = off\n"; // no Ashita root, so no addons for this profile (empty = the game folder, #73)
    }
    else if (o.addons == 1)
    {
        if (!o.ashitaDir.empty()) body += fmt::format("ashita_dir = {}\n", o.ashitaDir);
        if (!o.ashitaVersion.empty()) body += fmt::format("ashita_version = {}\n", o.ashitaVersion);
        if (!o.ashitaLoad.empty()) body += fmt::format("ashita_load = {}\n", o.ashitaLoad);
    }
    if (!o.macroDir.empty()) body += fmt::format("macro_dir = {}\n", o.macroDir);
    if (body.empty())
    {
        return {};
    }
    return "; written by xi-launcher for one profile: applied after client.ini (xi-client --config)\n[ffxi-linux]\n" + body;
}

const Settings& effectiveSettings(const LauncherConfig& c, const Profile& p)
{
    return p.ownSettings ? p.settings : c.global;
}

std::vector<std::string> splitArgs(std::string_view text)
{
    std::vector<std::string> out;
    std::string              cur;
    bool                     inQuote = false, have = false;
    for (const char ch : text)
    {
        if (ch == '"')
        {
            inQuote = !inQuote;
            have    = true;
        }
        else if (!inQuote && (ch == ' ' || ch == '\t'))
        {
            if (have)
            {
                out.push_back(std::move(cur));
                cur.clear();
                have = false;
            }
        }
        else
        {
            cur += ch;
            have = true;
        }
    }
    if (have)
    {
        out.push_back(std::move(cur));
    }
    return out;
}

std::filesystem::path defaultConfigPath()
{
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    {
        return std::filesystem::path(x) / "ffxi-linux" / "launcher.ini";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".config" / "ffxi-linux" / "launcher.ini";
}

std::string toIni(const LauncherConfig& c)
{
    std::string out = "; xi-launcher settings (goal 44). Passwords are never stored here: see the desktop keyring.\n";
    auto        cc  = c; // the visitors take references
    out += "[launcher]\n";
    out += fmt::format("default_profile = {}\nclient_path = {}\n", c.defaultProfile, c.clientPath);
    out += "\n[global]\n";
    visitSettings(cc.global, Writer{ out });
    for (std::size_t i = 0; i < cc.profiles.size(); ++i)
    {
        out += fmt::format("\n[profile {}]\n", i);
        auto& p = cc.profiles[i];
        visitProfile(p, Writer{ out });
        if (p.ownSettings)
        {
            visitSettings(p.settings, Writer{ out });
        }
    }
    return out;
}

LauncherConfig fromIni(std::string_view text)
{
    LauncherConfig c;
    std::map<std::string, std::map<std::string, std::string>> sections;
    std::vector<std::string>                                  order;
    std::string                                               current;
    std::istringstream                                        in{ std::string(text) };
    std::string                                               line;
    while (std::getline(in, line))
    {
        const auto t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#')
        {
            continue;
        }
        if (t.front() == '[' && t.back() == ']')
        {
            current = trim(std::string_view(t).substr(1, t.size() - 2));
            if (!sections.count(current))
            {
                order.push_back(current);
            }
            sections[current];
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        sections[current][trim(std::string_view(t).substr(0, eq))] = trim(std::string_view(t).substr(eq + 1));
    }
    if (const auto it = sections.find("launcher"); it != sections.end())
    {
        if (const auto d = it->second.find("default_profile"); d != it->second.end() && parseInt(d->second))
        {
            c.defaultProfile = *parseInt(d->second);
        }
        if (const auto d = it->second.find("client_path"); d != it->second.end())
        {
            c.clientPath = d->second;
        }
    }
    if (const auto it = sections.find("global"); it != sections.end())
    {
        visitSettings(c.global, Reader{ it->second });
    }
    for (const auto& name : order)
    {
        if (name.rfind("profile", 0) != 0)
        {
            continue;
        }
        Profile p;
        visitProfile(p, Reader{ sections[name] });
        if (p.ownSettings)
        {
            visitSettings(p.settings, Reader{ sections[name] });
        }
        c.profiles.push_back(std::move(p));
    }
    if (c.profiles.empty())
    {
        c.profiles.emplace_back();
    }
    c.defaultProfile = std::clamp(c.defaultProfile, 0, static_cast<int>(c.profiles.size()) - 1);
    return c;
}

LauncherConfig loadConfig(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
    {
        return fromIni("");
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return fromIni(ss.str());
}

bool saveConfig(const LauncherConfig& c, const std::filesystem::path& file)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            return false;
        }
        out << toIni(c);
        if (!out)
        {
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

} // namespace xi::launcher
