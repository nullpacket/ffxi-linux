// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/install.h"

#include "xi/game_data.h"
#include "xi/huffman.h"

#include <cstdlib>
#include <fstream>
#include <regex>

namespace xi::install
{

namespace
{

std::filesystem::path home()
{
    const char* h = std::getenv("HOME");
    return h ? std::filesystem::path(h) : std::filesystem::path(".");
}

bool present(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

} // namespace

std::vector<std::filesystem::path> defaultGameDirs()
{
    const auto wine = home() / ".wine" / "drive_c";
    return {
        dataGameDir(),
        wine / "Program Files (x86)" / "PlayOnline" / "SquareEnix" / "FINAL FANTASY XI",
        wine / "Program Files" / "PlayOnline" / "SquareEnix" / "FINAL FANTASY XI",
    };
}

std::optional<std::filesystem::path> findGameDir()
{
    if (const char* env = std::getenv("FFXI_ROOT"); env && *env)
    {
        return std::filesystem::path(env);
    }
    for (const auto& d : defaultGameDirs())
    {
        if (present(d / "FFXiMain.dll"))
        {
            return d;
        }
    }
    return std::nullopt;
}

const char* gameRootOrNull()
{
    static const std::string root = [] {
        const auto d = findGameDir();
        return d ? d->string() : std::string();
    }();
    return root.empty() ? nullptr : root.c_str();
}

std::string gameDirProblem(const std::filesystem::path& dir)
{
    if (!present(dir))
    {
        return "no such directory: " + dir.string();
    }
    for (const char* f : { "FFXiMain.dll", "VTABLE.DAT", "FTABLE.DAT" })
    {
        if (!present(dir / f))
        {
            return dir.string() + " has no " + f +
                   "; it must be the FINAL FANTASY XI folder itself (the one with FFXiMain.dll and ROM/)";
        }
    }
    if (!present(dir / "ROM"))
    {
        return dir.string() + " has no ROM folder (the game data)";
    }
    return {};
}

std::optional<std::string> installedClientVersion(const std::filesystem::path& dir)
{
    std::ifstream in(dir / "patch.cfg");
    if (!in)
    {
        return std::nullopt;
    }
    static const std::regex version(R"(^\s*(\d{8}_\d+)\s)");
    std::string             best;
    std::string             line;
    while (std::getline(in, line))
    {
        std::smatch m;
        if (std::regex_search(line, m, version) && m[1].str() > best)
        {
            best = m[1].str();
        }
    }
    if (best.empty())
    {
        return std::nullopt;
    }
    return best;
}

std::filesystem::path dataCompressDat()
{
    // Next to the imported game data: dataGameDir() is <data>/ffxi-linux/game.
    return dataGameDir().parent_path() / "compress.dat";
}

std::vector<std::filesystem::path> compressDatCandidates()
{
    std::vector<std::filesystem::path> out;
    if (const char* env = std::getenv("XI_COMPRESS_DAT"); env && *env)
    {
        out.emplace_back(env);
    }
    // $LSB_DIR (an explicit choice) wins over our copy; the default checkout locations come after it.
    const char* lsb  = std::getenv("LSB_DIR");
    auto        dirs = lsbDirCandidates();
    auto        it   = dirs.begin();
    if (lsb && *lsb && it != dirs.end())
    {
        out.push_back(*it / "res" / "compress.dat");
        ++it;
    }
    out.push_back(dataCompressDat());
    for (; it != dirs.end(); ++it)
    {
        out.push_back(*it / "res" / "compress.dat");
    }
    return out;
}

std::string compressDatProblem(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return present(path) ? "not a file" : "missing";
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return "cannot read it";
    }
    if (size != 2048)
    {
        return "is " + std::to_string(size) + " bytes; LSB's res/compress.dat is 2048";
    }
    try
    {
        (void)xi::Huffman::fromCompressDat(path);
    }
    catch (const std::exception& e)
    {
        return std::string("not a compress table (") + e.what() + ")";
    }
    return {};
}

std::vector<std::filesystem::path> lsbDirCandidates()
{
    std::vector<std::filesystem::path> out;
    if (const char* lsb = std::getenv("LSB_DIR"); lsb && *lsb)
    {
        out.emplace_back(lsb);
    }
    for (const char* d : { "server", "git/server", "LandSandBoat", "git/LandSandBoat" })
    {
        out.push_back(home() / d);
    }
    return out;
}

std::optional<std::filesystem::path> findLsbDir()
{
    for (const auto& d : lsbDirCandidates())
    {
        if (present(d / "settings" / "default" / "login.lua"))
        {
            return d;
        }
    }
    return std::nullopt;
}

std::optional<std::string> lsbTargetClientVersion(const std::filesystem::path& lsbDir)
{
    std::ifstream in(lsbDir / "settings" / "default" / "login.lua");
    if (!in)
    {
        return std::nullopt;
    }
    static const std::regex version(R"(^\s*CLIENT_VER\s*=\s*["'](\d{8}_\d+)["'])");
    std::string             line;
    while (std::getline(in, line))
    {
        std::smatch m;
        if (std::regex_search(line, m, version))
        {
            return m[1].str();
        }
    }
    return std::nullopt;
}

std::string dataVersionWarning(const std::string& installed, const std::string& target)
{
    // Both are "YYYYMMDD_N" of the same width, so they compare as strings.
    if (installed.empty() || target.empty() || installed >= target)
    {
        return {};
    }
    return "game data " + installed + " is older than the client version LSB targets (" + target +
           "), so some server messages (\"Obtained: <item>.\", key items...) show the wrong line. Patch the install "
           "with PlayOnline and run `xi-setup import` again (README, \"Getting the game data\").";
}

CompressDatStatus compressDatStatus()
{
    CompressDatStatus s;
    s.tried = compressDatCandidates();
    for (const auto& p : s.tried)
    {
        if (!present(p))
        {
            continue;
        }
        if (auto problem = compressDatProblem(p); !problem.empty())
        {
            s.skipped.push_back(p.string() + ": " + problem);
            continue;
        }
        if (!s.found)
        {
            s.found = p;
        }
    }
    return s;
}

std::optional<std::filesystem::path> findCompressDat()
{
    return compressDatStatus().found;
}

std::string describeCompressDat(const CompressDatStatus& s)
{
    if (s.found)
    {
        return s.found->string();
    }
    std::string out = "not found. It is LandSandBoat's res/compress.dat (in the server checkout or its Docker image): "
                      "run `xi-setup compress-dat FILE` (or the launcher's Tools > compress.dat) to copy it to " +
                      dataCompressDat().string() + ", or point XI_COMPRESS_DAT at the file or LSB_DIR at the checkout.";
    for (const auto& k : s.skipped)
    {
        out += " Skipped " + k + ".";
    }
    return out;
}

std::string saveCompressDat(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (auto problem = compressDatProblem(from); !problem.empty())
    {
        return from.string() + ": " + problem;
    }
    std::error_code ec;
    if (std::filesystem::equivalent(from, to, ec))
    {
        return {}; // already our copy
    }
    std::filesystem::create_directories(to.parent_path(), ec);
    if (ec)
    {
        return "cannot create " + to.parent_path().string() + ": " + ec.message();
    }
    // Write next to it, then rename, so a half-written copy never replaces a good one.
    auto tmp = to;
    tmp += ".part";
    std::filesystem::copy_file(from, tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        return "cannot copy to " + tmp.string() + ": " + ec.message();
    }
    std::filesystem::rename(tmp, to, ec);
    if (ec)
    {
        std::filesystem::remove(tmp, ec);
        return "cannot replace " + to.string();
    }
    return {};
}

} // namespace xi::install
