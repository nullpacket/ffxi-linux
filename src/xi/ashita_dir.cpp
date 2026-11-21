// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/ashita_dir.h"

#include <cstdlib>
#include <fmt/format.h>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace xi::ashitadir
{
namespace
{

fs::path homeDir()
{
    const char* h = std::getenv("HOME");
    return h && *h ? fs::path(h) : fs::path(".");
}

bool present(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

bool hasLibs(const fs::path& dir)
{
    return present(dir / "addons" / "libs" / "settings.lua");
}

// Copy a file when the target is missing or differs in size / modification time; true when written.
bool copyIfChanged(const fs::path& src, const fs::path& dst)
{
    std::error_code ec;
    if (fs::exists(dst, ec) && fs::file_size(src) == fs::file_size(dst) &&
        fs::last_write_time(src) == fs::last_write_time(dst))
    {
        return false;
    }
    fs::create_directories(dst.parent_path());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    fs::last_write_time(dst, fs::last_write_time(src));
    return true;
}

std::uint64_t copyTree(const fs::path& from, const fs::path& to)
{
    std::uint64_t n = 0;
    for (const auto& e : fs::recursive_directory_iterator(from))
    {
        if (e.is_regular_file())
        {
            n += copyIfChanged(e.path(), to / fs::relative(e.path(), from)) ? 1 : 0;
        }
    }
    return n;
}

} // namespace

fs::path effectiveRoot(const std::string& configured, const fs::path& gameDir)
{
    const char*       e   = std::getenv("XI_ASHITA_DIR");
    const std::string set = e && *e ? std::string(e) : configured;
    if (set == "off" || set == "none")
    {
        return {}; // addons off
    }
    return set.empty() ? gameDir : fs::path(set);
}

std::string ensureLayout(const fs::path& root)
{
    if (root.empty())
    {
        return "no game folder";
    }
    for (const auto* sub : { "addons/libs", "config/addons", "scripts" })
    {
        std::error_code ec;
        fs::create_directories(root / sub, ec);
        if (ec)
        {
            return fmt::format("cannot create {}: {}", (root / sub).string(), ec.message());
        }
    }
    return "";
}

bool libsPresent(const fs::path& root)
{
    return hasLibs(root) && present(root / "addons" / "libs" / "common.lua");
}

std::string libsMissingHint(const fs::path& root)
{
    return fmt::format(
        "Ashita's Lua libraries are missing from {}: run `xi-setup addons` (installs the bundled ones) or "
        "`xi-setup addons FOLDER` (copies them from your HorizonXI or Ashita install).",
        (root / "addons" / "libs").string());
}

fs::path ashitaFolderIn(const fs::path& from)
{
    for (const auto& p : { from, from / "Game", from / "drive_c" / "Program Files" / "HorizonXI" / "Game",
                           from / "drive_c" / "Program Files (x86)" / "HorizonXI" / "Game" })
    {
        if (hasLibs(p))
        {
            return p;
        }
    }
    // Ashita folders in a Wine prefix: C:\Ashita*, the users' desktops.
    std::error_code ec;
    std::vector<fs::path> bases{ from / "drive_c" };
    for (const auto& u : fs::directory_iterator(from / "drive_c" / "users", ec))
    {
        bases.push_back(u.path() / "Desktop");
    }
    for (const auto& b : bases)
    {
        std::error_code ec2;
        for (const auto& d : fs::directory_iterator(b, ec2))
        {
            const auto name = d.path().filename().string();
            if (name.rfind("Ashita", 0) == 0 && hasLibs(d.path()))
            {
                return d.path();
            }
        }
    }
    return {};
}

std::vector<FoundAshita> detectAshitaInstalls(const fs::path& exclude)
{
    const auto            home = homeDir();
    std::vector<fs::path> roots{ home / ".wine" };
    std::error_code       ec;
    for (const auto& a : fs::directory_iterator(home / "Games", ec))
    {
        if (!a.is_directory(ec))
        {
            continue;
        }
        roots.push_back(a.path());
        std::error_code ec2;
        for (const auto& b : fs::directory_iterator(a.path(), ec2))
        {
            if (b.is_directory(ec2) && b.path().filename() != "drive_c")
            {
                roots.push_back(b.path());
            }
        }
    }
    for (const auto& g : fs::directory_iterator(home / "git", ec))
    {
        if (g.path().filename().string().rfind("Ashita-v4", 0) == 0)
        {
            roots.push_back(g.path());
        }
    }
    std::error_code ec3;
    const auto      skip = exclude.empty() ? fs::path() : fs::weakly_canonical(exclude, ec3);
    std::vector<FoundAshita> out;
    std::set<fs::path>       seen;
    for (const auto& r : roots)
    {
        const auto dir = ashitaFolderIn(r);
        if (dir.empty())
        {
            continue;
        }
        std::error_code ec4;
        const auto      canon = fs::weakly_canonical(dir, ec4);
        if ((!skip.empty() && canon == skip) || !seen.insert(canon).second)
        {
            continue;
        }
        const bool hxi = dir.string().find("HorizonXI") != std::string::npos || present(dir / "addons" / "libs" / "bitmap.lua");
        out.push_back({ dir, hxi ? "HorizonXI" : "Ashita v4" });
    }
    return out;
}

CopyResult copyFromInstall(const fs::path& from, const fs::path& root, const std::vector<std::string>& addons)
{
    if (!hasLibs(from))
    {
        throw std::runtime_error(fmt::format("{} has no addons/libs/settings.lua", from.string()));
    }
    if (const auto err = ensureLayout(root); !err.empty())
    {
        throw std::runtime_error(err);
    }
    CopyResult res;
    res.libFiles = copyTree(from / "addons" / "libs", root / "addons" / "libs");
    if (std::error_code ec; fs::exists(from / "plugins" / "sdk" / "Ashita.h", ec))
    {
        res.sdkFiles = copyTree(from / "plugins" / "sdk", root / "plugins" / "sdk");
    }
    for (const auto& name : addons)
    {
        const auto src = from / "addons" / name;
        const auto dst = root / "addons" / name;
        if (!present(src) || present(dst))
        {
            res.skipped.push_back(name);
            continue;
        }
        copyTree(src, dst);
        res.addons.push_back(name);
    }
    return res;
}

fs::path bundledDir()
{
    std::vector<fs::path> cands;
    if (const char* e = std::getenv("XI_ASHITA_BUNDLE"); e && *e)
    {
        cands.emplace_back(e);
    }
    std::error_code ec;
    if (const auto exe = fs::read_symlink("/proc/self/exe", ec); !ec)
    {
        cands.push_back(exe.parent_path() / ".." / "share" / "ffxi-linux" / "ashita-addons");
    }
#ifdef XI_INSTALL_ADDON_DIR
    cands.emplace_back(XI_INSTALL_ADDON_DIR);
#endif
#ifdef XI_SOURCE_ADDON_DIR
    cands.emplace_back(XI_SOURCE_ADDON_DIR);
#endif
    for (const auto& c : cands)
    {
        if (hasLibs(c))
        {
            return c.lexically_normal();
        }
    }
    return {};
}

CopyResult installBundled(const fs::path& bundle, const fs::path& root)
{
    if (!hasLibs(bundle))
    {
        throw std::runtime_error(fmt::format("{} has no addons/libs/settings.lua", bundle.string()));
    }
    if (const auto err = ensureLayout(root); !err.empty())
    {
        throw std::runtime_error(err);
    }
    CopyResult res;
    const auto libsFrom = bundle / "addons" / "libs";
    const auto libsTo   = root / "addons" / "libs";
    for (const auto& e : fs::recursive_directory_iterator(libsFrom))
    {
        const auto dst = libsTo / fs::relative(e.path(), libsFrom);
        if (e.is_regular_file() && !present(dst))
        {
            fs::create_directories(dst.parent_path());
            fs::copy_file(e.path(), dst);
            ++res.libFiles;
        }
    }
    // addons/.bundled: the bundled addons installed before, one per line.
    const auto            markerPath = root / "addons" / ".bundled";
    std::set<std::string> before;
    {
        std::ifstream in(markerPath);
        for (std::string line; std::getline(in, line);)
        {
            if (!line.empty())
            {
                before.insert(line);
            }
        }
    }
    std::set<std::string> names;
    for (const auto& e : fs::directory_iterator(bundle / "addons"))
    {
        if (e.is_directory() && e.path().filename() != "libs")
        {
            names.insert(e.path().filename().string());
        }
    }
    for (const auto& name : names) // sorted
    {
        const auto dst = root / "addons" / name;
        if (before.count(name) != 0 || present(dst))
        {
            res.skipped.push_back(name);
            continue;
        }
        copyTree(bundle / "addons" / name, dst);
        res.addons.push_back(name);
    }
    if (!res.addons.empty())
    {
        before.insert(res.addons.begin(), res.addons.end());
        std::ofstream out(markerPath, std::ios::trunc);
        for (const auto& n : before)
        {
            out << n << '\n';
        }
        if (!out)
        {
            throw std::runtime_error(fmt::format("cannot write {}", markerPath.string()));
        }
    }
    return res;
}

} // namespace xi::ashitadir
