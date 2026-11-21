// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/settings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xi
{

namespace
{

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

void DisplaySettings::loadIni(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
    {
        throw std::runtime_error("cannot read " + file.string());
    }
    std::string line;
    bool        registry = false, own = false;
    while (std::getline(in, line))
    {
        auto s = trim(line);
        if (s.empty() || s.front() == ';' || s.front() == '#')
        {
            continue;
        }
        if (s.front() == '[')
        {
            registry = s == "[ffxi.registry]";
            own      = s == "[ffxi-linux]";
            continue;
        }
        const auto eq = s.find('=');
        if ((!registry && !own) || eq == std::string_view::npos)
        {
            continue;
        }
        const auto key = trim(s.substr(0, eq));
        const auto val = trim(s.substr(eq + 1));
        if (own && key == "login_protocol")
        {
            loginProtocol = std::string(val);
            continue;
        }
        if (own && (key == "bugreport_url" || key == "bugreport_project" || key == "bugreport_token_file" || key == "bugreport_chat"))
        {
            (key == "bugreport_url" ? bugreportUrl : key == "bugreport_project" ? bugreportProject : key == "bugreport_token_file" ? bugreportTokenFile : bugreportChat) =
                std::string(val);
            continue;
        }
        if (own && key == "macro_dir")
        {
            macroDir = std::string(val);
            continue;
        }
        if (own && (key == "ashita_dir" || key == "ashita_version" || key == "ashita_load" || key == "ashita_script"))
        {
            (key == "ashita_dir" ? ashitaDir : key == "ashita_version" ? ashitaVersion : key == "ashita_load" ? ashitaLoad : ashitaScript) = std::string(val);
            continue;
        }
        int v = -1;
        if (std::from_chars(val.data(), val.data() + val.size(), v).ec != std::errc{} || v < 0)
        {
            continue; // -1 (or text) = leave the default
        }
        if (own)
        {
            if (key == "window_skin") windowSkin = std::clamp(v, 1, 8);
            else if (key == "keyboard") keyboard = std::clamp(v, 0, 2);
            else if (key == "sound_volume") soundVolume = std::clamp(v, 0, 100);
            else if (key == "music_volume") musicVolume = std::clamp(v, 0, 100);
            else if (key.starts_with("cfg_"))
            {
                int id = 0;
                if (std::from_chars(key.data() + 4, key.data() + key.size(), id).ec == std::errc{})
                {
                    gameConfig[id] = v;
                }
            }
            continue;
        }
        if (key == "0001") windowWidth = v;
        else if (key == "0002") windowHeight = v;
        else if (key == "0003") bgWidth = v;
        else if (key == "0004") bgHeight = v;
        else if (key == "0034") windowMode = v;
        else if (key == "0037") menuWidth = v;
        else if (key == "0038") menuHeight = v;
    }
}

bool DisplaySettings::parseSize(std::string_view text, int& w, int& h)
{
    const auto x = text.find_first_of("xX");
    if (x == std::string_view::npos)
    {
        return false;
    }
    int a = 0, b = 0;
    if (std::from_chars(text.data(), text.data() + x, a).ec != std::errc{} ||
        std::from_chars(text.data() + x + 1, text.data() + text.size(), b).ec != std::errc{} || a <= 0 || b <= 0)
    {
        return false;
    }
    w = a;
    h = b;
    return true;
}

DisplaySettings::Canvas DisplaySettings::canvasFor(float w, float h) const
{
    Canvas c;
    const float menuH = menuHeight > 0 ? static_cast<float>(menuHeight) : h / 1.5f;
    c.scale  = std::max(0.25f, h / menuH);
    c.width  = w / c.scale;
    c.height = h / c.scale;
    return c;
}

std::optional<std::filesystem::path> DisplaySettings::defaultFile()
{
    auto p = userFile();
    return p && std::filesystem::exists(*p) ? p : std::nullopt;
}

bool DisplaySettings::saveOwn(const std::filesystem::path& file, std::string_view key, int value)
{
    return saveOwnText(file, key, std::to_string(value));
}

bool DisplaySettings::saveOwnText(const std::filesystem::path& file, std::string_view key, std::string_view value)
{
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        std::string   line;
        while (std::getline(in, line))
        {
            lines.push_back(line);
        }
    }
    const std::string entry = std::string(key) + " = " + std::string(value);
    // The [ffxi-linux] section: replace the key in it, or add the key at its end (after its last non-blank line).
    std::size_t section = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (trim(lines[i]) == "[ffxi-linux]")
        {
            section = i;
            break;
        }
    }
    if (section == lines.size())
    {
        if (!lines.empty() && !trim(lines.back()).empty())
        {
            lines.emplace_back();
        }
        lines.emplace_back("[ffxi-linux]");
        lines.push_back(entry);
    }
    else
    {
        std::size_t insertAt = section + 1;
        bool        done     = false;
        for (std::size_t i = section + 1; i < lines.size(); ++i)
        {
            const auto s = trim(lines[i]);
            if (!s.empty() && s.front() == '[')
            {
                break;
            }
            if (!s.empty())
            {
                insertAt = i + 1;
            }
            const auto eq = s.find('=');
            if (eq != std::string_view::npos && trim(s.substr(0, eq)) == key)
            {
                lines[i] = entry;
                done     = true;
                break;
            }
        }
        if (!done)
        {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), entry);
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    for (const auto& l : lines)
    {
        out << l << '\n';
    }
    return static_cast<bool>(out);
}

std::optional<std::filesystem::path> DisplaySettings::userFile()
{
    std::filesystem::path base;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    {
        base = x;
    }
    else if (const char* home = std::getenv("HOME"))
    {
        base = std::filesystem::path(home) / ".config";
    }
    else
    {
        return std::nullopt;
    }
    return base / "ffxi-linux" / "client.ini";
}

} // namespace xi
