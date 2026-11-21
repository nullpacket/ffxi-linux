// SPDX-License-Identifier: GPL-3.0-or-later
#include "ashita/addon_info.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <fstream>
#include <sstream>

namespace xi::ashita
{
namespace
{

std::string lowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream      in(p, std::ios::binary);
    std::ostringstream o;
    o << in.rdbuf();
    return o.str();
}

// A Lua string literal at s[i] ('..' or ".."): its contents with the common escapes undone; i moves past it.
std::optional<std::string> luaString(const std::string& s, std::size_t& i)
{
    if (i >= s.size() || (s[i] != '\'' && s[i] != '"'))
    {
        return std::nullopt;
    }
    const char  q = s[i++];
    std::string out;
    while (i < s.size() && s[i] != q)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            const char c = s[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        }
        else
        {
            out += s[i];
        }
        ++i;
    }
    if (i >= s.size())
    {
        return std::nullopt;
    }
    ++i;
    return out;
}

} // namespace

AddonInfo parseAddonHeader(const std::string& source)
{
    AddonInfo          info;
    std::istringstream in(source);
    std::string        line;
    int                n = 0;
    while (std::getline(in, line) && n++ < 400)
    {
        const auto t = trim(line);
        // addon.name = 'x'; (also addon.name    = "x")
        if (t.rfind("addon.", 0) == 0)
        {
            const auto eq = t.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            const auto  field = lowerCopy(trim(t.substr(6, eq - 6)));
            std::string rest  = trim(t.substr(eq + 1));
            std::size_t i     = 0;
            const auto  v     = luaString(rest, i);
            if (!v)
            {
                continue;
            }
            if (field == "name") info.name = *v;
            else if (field == "author") info.author = *v;
            else if (field == "version") info.version = *v;
            else if (field == "desc" || field == "description") info.description = *v;
            else if (field == "link") info.link = *v;
        }
        // require('lin') / require 'bitmap' ... : HorizonXI's extra libraries.
        for (const char* lib : { "'bitmap'", "\"bitmap\"", "'color'", "\"color\"", "'lin", "\"lin", "'typedefs", "\"typedefs" })
        {
            const auto r = t.find("require");
            if (r != std::string::npos && t.find(lib, r) != std::string::npos)
            {
                info.needs = "HorizonXI";
            }
        }
    }
    return info;
}

std::vector<AddonInfo> scanAddons(const std::filesystem::path& root)
{
    std::vector<AddonInfo> out;
    std::error_code        ec;
    for (const auto& d : std::filesystem::directory_iterator(root / "addons", ec))
    {
        if (!d.is_directory(ec))
        {
            continue;
        }
        const auto            name = d.path().filename().string();
        std::filesystem::path main;
        for (const auto& f : std::filesystem::directory_iterator(d.path(), ec))
        {
            if (lowerCopy(f.path().filename().string()) == lowerCopy(name + ".lua"))
            {
                main = f.path();
            }
        }
        if (main.empty())
        {
            continue; // libs, data folders
        }
        auto info     = parseAddonHeader(readFile(main));
        info.name     = name;
        info.dir      = d.path();
        info.mainFile = main;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const AddonInfo& a, const AddonInfo& b) { return lowerCopy(a.name) < lowerCopy(b.name); });
    return out;
}

std::vector<std::string> readStartupList(const std::filesystem::path& file)
{
    std::vector<std::string> names;
    std::ifstream            in(file);
    std::string              line;
    while (std::getline(in, line))
    {
        if (const auto h = line.find('#'); h != std::string::npos)
        {
            line.erase(h);
        }
        line = trim(line);
        if (!line.empty() && std::none_of(names.begin(), names.end(), [&](const std::string& n) { return lowerCopy(n) == lowerCopy(line); }))
        {
            names.push_back(line);
        }
    }
    return names;
}

bool writeStartupList(const std::filesystem::path& file, const std::vector<std::string>& names)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out << "# Ashita addons this character loads at login (Config -> Add-ons; one name a line).\n";
    for (const auto& n : names)
    {
        out << n << '\n';
    }
    return static_cast<bool>(out);
}

std::string SettingField::key() const
{
    std::string k;
    for (const auto& p : path)
    {
        k += (k.empty() ? "" : ".") + p;
    }
    return k;
}

std::string SettingField::display() const
{
    if (type == Type::String)
    {
        std::size_t i = 0;
        return luaString(value, i).value_or(value);
    }
    if (type == Type::Number && !path.empty())
    {
        const auto last = lowerCopy(path.back());
        if (last.find("color") != std::string::npos || last.find("colour") != std::string::npos)
        {
            try
            {
                const auto v = std::stoull(value);
                return fmt::format("0x{:08X}", static_cast<uint32_t>(v));
            }
            catch (...)
            {
            }
        }
    }
    return value;
}

std::vector<SettingField> readSettings(const std::filesystem::path& file)
{
    std::vector<SettingField> out;
    std::ifstream             in(file);
    std::string               line;
    std::size_t               n = 0;
    for (; std::getline(in, line); ++n)
    {
        auto t = trim(line);
        // settings["a"]["b"] = value;
        if (t.rfind("settings[", 0) != 0)
        {
            continue;
        }
        std::size_t  i = 8;
        SettingField f;
        bool         ok = true;
        while (i < t.size() && t[i] == '[')
        {
            ++i;
            if (const auto k = luaString(t, i))
            {
                f.path.push_back(*k);
            }
            else
            {
                // A numeric index: settings["x"][1]
                const auto close = t.find(']', i);
                if (close == std::string::npos)
                {
                    ok = false;
                    break;
                }
                f.path.push_back(trim(t.substr(i, close - i)));
                i = close;
            }
            if (i >= t.size() || t[i] != ']')
            {
                ok = false;
                break;
            }
            ++i;
        }
        const auto eq = t.find('=', i);
        if (!ok || f.path.empty() || eq == std::string::npos)
        {
            continue;
        }
        std::string v = trim(t.substr(eq + 1));
        if (!v.empty() && v.back() == ';')
        {
            v.pop_back();
            v = trim(v);
        }
        f.value = v;
        f.line  = n;
        if (v == "true" || v == "false")
        {
            f.type = SettingField::Type::Bool;
        }
        else if (!v.empty() && (v[0] == '"' || v[0] == '\''))
        {
            f.type = SettingField::Type::String;
        }
        else if (!v.empty() && (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-' || v[0] == '.'))
        {
            f.type = SettingField::Type::Number;
        }
        if (v.rfind("T{", 0) == 0 && v.find_first_not_of(" T{}") == std::string::npos)
        {
            continue; // an empty table: the parent of later lines, not a value
        }
        out.push_back(std::move(f));
    }
    return out;
}

bool writeSetting(const std::filesystem::path& file, const SettingField& field, const std::string& value)
{
    std::ifstream            in(file);
    std::vector<std::string> lines;
    std::string              line;
    while (std::getline(in, line))
    {
        lines.push_back(line);
    }
    in.close();
    if (field.line >= lines.size())
    {
        return false;
    }
    auto&      l  = lines[field.line];
    const auto eq = l.find('=');
    if (eq == std::string::npos || l.find(field.value, eq) == std::string::npos)
    {
        return false;
    }
    std::string lit = value;
    if (field.type == SettingField::Type::String)
    {
        lit = "\"";
        for (const char c : value)
        {
            if (c == '"' || c == '\\')
            {
                lit += '\\';
            }
            lit += c;
        }
        lit += '"';
    }
    l = l.substr(0, eq) + "= " + lit + ";";
    std::ofstream out(file, std::ios::trunc);
    for (const auto& x : lines)
    {
        out << x << '\n';
    }
    return static_cast<bool>(out);
}

std::vector<std::filesystem::path> settingsFiles(const std::filesystem::path& root, const std::string& addon, const std::string& character,
                                                 uint32_t charId)
{
    std::vector<std::filesystem::path> out;
    std::error_code                    ec;
    const auto                         base = root / "config" / "addons" / addon;
    for (const auto& sub : { fmt::format("{}_{}", character, charId), std::string("defaults") })
    {
        std::vector<std::filesystem::path> files;
        for (const auto& f : std::filesystem::directory_iterator(base / sub, ec))
        {
            if (f.is_regular_file(ec) && f.path().extension() == ".lua")
            {
                files.push_back(f.path());
            }
        }
        std::sort(files.begin(), files.end());
        out.insert(out.end(), files.begin(), files.end());
        if (!out.empty())
        {
            break; // the character's own files when there are any, else the defaults
        }
    }
    return out;
}

} // namespace xi::ashita
