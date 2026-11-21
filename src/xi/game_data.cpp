// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/game_data.h"

#include "xi/install.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xi::install
{

namespace fs = std::filesystem;

namespace
{

fs::path homeDir()
{
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path(".");
}

bool present(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "ROM", "ROM2".."ROM99" / "sound", "sound2".. (case-insensitive: installs copied from Windows may differ).
bool numbered(const std::string& name, const char* stem)
{
    const auto n  = lower(name);
    const auto st = std::string(stem);
    if (n.rfind(st, 0) != 0)
    {
        return false;
    }
    const auto rest = n.substr(st.size());
    return std::ranges::all_of(rest, [](unsigned char c) { return std::isdigit(c) != 0; });
}

// The FINAL FANTASY XI folder's usual places under a drive root (a Wine drive_c or a Windows drive).
const char* const kUnderDrive[] = {
    "Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI",
    "Program Files/PlayOnline/SquareEnix/FINAL FANTASY XI",
    "PlayOnline/SquareEnix/FINAL FANTASY XI",
    "SquareEnix/FINAL FANTASY XI",
};

struct Entry
{
    std::uint64_t size  = 0;
    std::int64_t  mtime = 0; // ns
};

std::int64_t mtimeNs(const fs::path& p)
{
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0)
    {
        return 0;
    }
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec;
}

// Copy one file to `dst` (through a temporary name), reflinked where the filesystem can; true if reflinked.
bool copyOne(const fs::path& src, const fs::path& dst)
{
    fs::create_directories(dst.parent_path());
    const auto tmp = fs::path(dst.string() + ".importing");
    const int  in  = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
    {
        throw std::runtime_error(fmt::format("cannot read {}: {}", src.string(), std::strerror(errno)));
    }
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0)
    {
        ::close(in);
        throw std::runtime_error(fmt::format("cannot write {}: {}", tmp.string(), std::strerror(errno)));
    }
    bool reflinked = ::ioctl(out, FICLONE, in) == 0;
    bool ok        = reflinked;
    if (!ok)
    {
        // A plain copy (copy_file_range lets the kernel do it, and still shares extents on some filesystems).
        struct stat st{};
        ok = ::fstat(in, &st) == 0;
        auto left = ok ? static_cast<std::uint64_t>(st.st_size) : 0;
        while (ok && left > 0)
        {
            const auto n = ::copy_file_range(in, nullptr, out, nullptr, left, 0);
            if (n <= 0)
            {
                ok = false;
                break;
            }
            left -= static_cast<std::uint64_t>(n);
        }
        if (!ok)
        {
            // Last resort: read and write.
            ok = ::lseek(in, 0, SEEK_SET) == 0 && ::ftruncate(out, 0) == 0 && ::lseek(out, 0, SEEK_SET) == 0;
            char buf[1 << 16];
            for (ssize_t n; ok && (n = ::read(in, buf, sizeof buf)) > 0;)
            {
                ok = ::write(out, buf, static_cast<std::size_t>(n)) == n;
            }
        }
    }
    // Keep the source's modification time, so a later import can tell what changed.
    struct stat st{};
    if (ok && ::fstat(in, &st) == 0)
    {
        const timespec times[2] = { st.st_atim, st.st_mtim };
        ::futimens(out, times);
    }
    ::close(in);
    if (::close(out) != 0)
    {
        ok = false;
    }
    if (!ok)
    {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw std::runtime_error(fmt::format("copying {} failed: {}", src.string(), std::strerror(errno)));
    }
    fs::rename(tmp, dst);
    return reflinked;
}

std::map<std::string, Entry> readManifest(const fs::path& file)
{
    std::map<std::string, Entry> out;
    std::ifstream                in(file);
    std::string                  line;
    while (std::getline(in, line))
    {
        const auto a = line.find('\t');
        const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos)
        {
            continue;
        }
        Entry e;
        e.size              = std::strtoull(line.substr(0, a).c_str(), nullptr, 10);
        e.mtime             = std::strtoll(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
        out[line.substr(b + 1)] = e;
    }
    return out;
}

bool isUserFile(const std::string& rel)
{
    return lower(rel).rfind("user/", 0) == 0;
}

} // namespace

fs::path dataGameDir()
{
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x)
    {
        return fs::path(x) / "ffxi-linux" / "game";
    }
    return homeDir() / ".local" / "share" / "ffxi-linux" / "game";
}

bool neededGameFile(const fs::path& rel)
{
    auto       it = rel.begin();
    if (it == rel.end())
    {
        return false;
    }
    const auto top   = it->string();
    const auto topLc = lower(top);
    const bool deeper = std::next(it) != rel.end();
    if (!deeper)
    {
        return topLc == "vtable.dat" || topLc == "ftable.dat" || topLc == "ffximain.dll" || topLc == "patch.cfg";
    }
    if (numbered(top, "rom") || numbered(top, "sound"))
    {
        return true;
    }
    if (topLc == "user")
    {
        const auto name = lower(rel.filename().string());
        return name.rfind("mcr", 0) == 0 && (name.ends_with(".dat") || name.ends_with(".ttl") || name.ends_with(".sys"));
    }
    return false;
}

std::optional<fs::path> gameFolderIn(const fs::path& from)
{
    if (present(from / "FFXiMain.dll"))
    {
        return from;
    }
    const fs::path game = fs::path("SquareEnix") / "FINAL FANTASY XI";
    for (const auto& root : { from / "drive_c", from / "pfx" / "drive_c", from })
    {
        for (const char* under : kUnderDrive)
        {
            if (present(root / under / "FFXiMain.dll"))
            {
                return root / under;
            }
        }
        // Other launchers' folders: Program Files*/<A>/SquareEnix/... and Program Files*/<A>/<B>/SquareEnix/... (e.g.
        // HorizonXI's Program Files/HorizonXI/Game). Two levels only, so a whole drive is never walked.
        for (const char* pf : { "Program Files (x86)", "Program Files" })
        {
            std::error_code ec;
            for (const auto& a : fs::directory_iterator(root / pf, ec))
            {
                if (!a.is_directory(ec))
                {
                    continue;
                }
                if (present(a.path() / game / "FFXiMain.dll"))
                {
                    return a.path() / game;
                }
                std::error_code ec2;
                for (const auto& b : fs::directory_iterator(a.path(), ec2))
                {
                    if (b.is_directory(ec2) && present(b.path() / game / "FFXiMain.dll"))
                    {
                        return b.path() / game;
                    }
                }
            }
        }
    }
    // Steam's own copy (FINAL FANTASY XI is app 230330, installed as steamapps/common/FFXINA).
    for (const auto& p : { from / "steamapps" / "common" / "FFXINA" / game, from / "common" / "FFXINA" / game })
    {
        if (present(p / "FFXiMain.dll"))
        {
            return p;
        }
    }
    return std::nullopt;
}

DetectPlaces DetectPlaces::fromEnvironment()
{
    DetectPlaces p;
    p.home = homeDir();
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x)
    {
        p.dataHome = x;
    }
    else
    {
        p.dataHome = p.home / ".local" / "share";
    }
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    {
        p.configHome = x;
    }
    else
    {
        p.configHome = p.home / ".config";
    }
    p.mountBases = { "/mnt", "/media" };
    if (const char* user = std::getenv("USER"); user && *user)
    {
        p.mountBases.push_back(fs::path("/media") / user);
        p.mountBases.push_back(fs::path("/run/media") / user);
    }
    return p;
}

std::vector<fs::path> steamLibraryFolders(const fs::path& vdf)
{
    // KeyValues text: ... "0" { "path" "/home/me/.local/share/Steam" ... } "1" { "path" "/mnt/games/SteamLibrary" ...
    std::vector<fs::path>   out;
    std::ifstream           in(vdf);
    static const std::regex path(R"re(^\s*"path"\s+"((?:[^"\\]|\\.)*)")re", std::regex::icase);
    std::string             line;
    while (std::getline(in, line))
    {
        std::smatch m;
        if (std::regex_search(line, m, path))
        {
            std::string v;
            const auto  raw = m[1].str();
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                v += raw[i] == '\\' && i + 1 < raw.size() ? raw[++i] : raw[i]; // "\\" -> "\"
            }
            out.emplace_back(v);
        }
    }
    return out;
}

namespace
{

fs::path expandHome(std::string v, const fs::path& home)
{
    // YAML values may be quoted.
    if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front())
    {
        v = v.substr(1, v.size() - 2);
    }
    if (v == "~")
    {
        return home;
    }
    if (v.rfind("~/", 0) == 0)
    {
        return home / v.substr(2);
    }
    return v;
}

std::string trim(const std::string& s)
{
    const auto a = s.find_first_not_of(" \t\r");
    const auto b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void addPrefix(std::vector<NamedPrefix>& out, const std::string& name, const fs::path& p)
{
    if (p.empty())
    {
        return;
    }
    for (const auto& e : out)
    {
        if (e.prefix == p)
        {
            return;
        }
    }
    out.push_back({ name, p });
}

} // namespace

std::vector<NamedPrefix> lutrisPrefixes(const fs::path& gamesDir, const fs::path& home)
{
    // Only the few keys we need, read line by line (no YAML parser): `name: X` at the top level, and every
    // `prefix: P` / `exe: P` at any depth (game:, script: game:, installer tasks).
    std::vector<NamedPrefix> out;
    std::error_code          ec;
    std::vector<fs::path>    files;
    for (const auto& f : fs::directory_iterator(gamesDir, ec))
    {
        if (f.path().extension() == ".yml" || f.path().extension() == ".yaml")
        {
            files.push_back(f.path());
        }
    }
    std::ranges::sort(files);
    static const std::regex kv(R"(^(\s*)(?:-\s+)?(name|prefix|exe):\s*(.*)$)");
    for (const auto& file : files)
    {
        std::ifstream         in(file);
        std::string           name = file.stem().string();
        std::vector<fs::path> paths;
        std::string           line;
        while (std::getline(in, line))
        {
            std::smatch m;
            if (!std::regex_match(line, m, kv))
            {
                continue;
            }
            const auto value = trim(m[3].str());
            if (value.empty())
            {
                continue;
            }
            if (m[2] == "name")
            {
                if (m[1].length() == 0)
                {
                    name = expandHome(value, {}).string();
                }
            }
            else if (m[2] == "prefix")
            {
                paths.push_back(expandHome(value, home));
            }
            else
            {
                paths.push_back(expandHome(value, home).parent_path());
            }
        }
        for (const auto& p : paths)
        {
            addPrefix(out, name, p);
        }
    }
    return out;
}

std::vector<NamedPrefix> faugusPrefixes(const fs::path& gamesJson)
{
    std::vector<NamedPrefix> out;
    std::ifstream            in(gamesJson);
    if (!in)
    {
        return out;
    }
    const auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_array())
    {
        return out;
    }
    for (const auto& g : doc)
    {
        if (!g.is_object())
        {
            continue;
        }
        auto str = [&](const char* k) {
            const auto it = g.find(k);
            return it != g.end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        const auto title = str("title");
        addPrefix(out, title, str("prefix"));
        if (const auto exe = str("path"); !exe.empty())
        {
            addPrefix(out, title, fs::path(exe).parent_path());
        }
    }
    return out;
}

std::vector<NamedPrefix> bottlesPrefixes(const fs::path& bottlesDir)
{
    std::vector<NamedPrefix> out;
    std::error_code          ec;
    for (const auto& b : fs::directory_iterator(bottlesDir, ec))
    {
        if (b.is_directory(ec))
        {
            out.push_back({ b.path().filename().string(), b.path() });
        }
    }
    std::ranges::sort(out, {}, &NamedPrefix::name);
    return out;
}

std::vector<FoundInstall> detectInstalls()
{
    return detectInstalls(DetectPlaces::fromEnvironment());
}

std::vector<FoundInstall> detectInstalls(const DetectPlaces& places)
{
    struct Root
    {
        fs::path    path;
        std::string source;
    };
    std::vector<Root> roots;
    const auto&       home = places.home;
    std::error_code   ec;

    // Steam: every library in libraryfolders.vdf, for each Steam install (native, the ~/.steam link, Flatpak).
    std::vector<fs::path> libraries;
    for (const auto& steam : { places.dataHome / "Steam", home / ".steam" / "steam", home / ".steam" / "root",
                               home / ".var/app/com.valvesoftware.Steam/.local/share/Steam" })
    {
        libraries.push_back(steam);
        for (const auto& lib : steamLibraryFolders(steam / "steamapps" / "libraryfolders.vdf"))
        {
            libraries.push_back(lib);
        }
    }
    for (const auto& lib : libraries)
    {
        roots.push_back({ lib / "steamapps" / "compatdata" / "230330", "Steam (Proton prefix)" });
        roots.push_back({ lib, "Steam" });
    }

    for (const auto& dir : { places.dataHome / "lutris" / "games", places.configHome / "lutris" / "games" })
    {
        for (const auto& p : lutrisPrefixes(dir, home))
        {
            roots.push_back({ p.prefix, "Lutris: " + p.name });
        }
    }
    for (const auto& file :
         { places.configHome / "faugus-launcher" / "games.json", places.dataHome / "faugus-launcher" / "games.json" })
    {
        for (const auto& p : faugusPrefixes(file))
        {
            roots.push_back({ p.prefix, "Faugus: " + p.name });
        }
    }
    for (const auto& dir :
         { places.dataHome / "bottles" / "bottles", home / ".var/app/com.usebottles.bottles/data/bottles/bottles" })
    {
        for (const auto& p : bottlesPrefixes(dir))
        {
            roots.push_back({ p.prefix, "Bottles: " + p.name });
        }
    }

    roots.push_back({ home / ".wine", "Wine (~/.wine)" });
    // Wine prefixes under ~/Games, one or two levels down (~/Games/X, ~/Games/faugus/X, ~/Games/lutris/X ...).
    for (const auto& a : fs::directory_iterator(home / "Games", ec))
    {
        if (!a.is_directory(ec))
        {
            continue;
        }
        roots.push_back({ a.path(), "Wine prefix under ~/Games" });
        std::error_code ec2;
        for (const auto& b : fs::directory_iterator(a.path(), ec2))
        {
            if (b.is_directory(ec2) && b.path().filename() != "drive_c")
            {
                roots.push_back({ b.path(), "Wine prefix under ~/Games" });
            }
        }
    }
    for (const auto& base : places.mountBases)
    {
        std::error_code ec2;
        for (const auto& m : fs::directory_iterator(base, ec2))
        {
            if (m.is_directory(ec2))
            {
                roots.push_back({ m.path(), "mounted drive" });
            }
        }
    }

    const auto                ours = fs::weakly_canonical(places.dataHome / "ffxi-linux" / "game", ec);
    std::vector<FoundInstall> out;
    std::set<fs::path>        seen;
    for (const auto& r : roots)
    {
        const auto dir = gameFolderIn(r.path);
        if (!dir)
        {
            continue;
        }
        std::error_code ec2;
        const auto      canon = fs::weakly_canonical(*dir, ec2);
        if (canon == ours || !seen.insert(canon).second)
        {
            continue;
        }
        FoundInstall f;
        f.dir     = *dir;
        f.version = installedClientVersion(*dir).value_or("");
        f.problem = gameDirProblem(*dir);
        f.source  = r.source;
        out.push_back(std::move(f));
    }
    return out;
}

// ---- Checking a copy ----

namespace
{

// Same bytes? (sizes already equal). Reads both in 1 MB blocks.
bool sameContents(const fs::path& a, const fs::path& b)
{
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb)
    {
        return false;
    }
    std::vector<char> ba(1 << 20);
    std::vector<char> bb(1 << 20);
    while (fa && fb)
    {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), static_cast<std::size_t>(fa.gcount())) != 0)
        {
            return false;
        }
    }
    return fa.eof() && fb.eof();
}

bool topLevel(const std::string& rel)
{
    return rel.find('/') == std::string::npos;
}

// The install an import came from: line 2 of import.version.
std::optional<fs::path> recordedInstall(const fs::path& dir)
{
    std::ifstream in(dir / "import.version");
    std::string   version;
    std::string   from;
    if (!std::getline(in, version) || !std::getline(in, from) || from.empty())
    {
        return std::nullopt;
    }
    return fs::path(from);
}

const char* stateName(FileState s)
{
    switch (s)
    {
        case FileState::Missing: return "MISSING";
        case FileState::SizeDiffers: return "SIZE";
        case FileState::ContentDiffers: return "DIFFERS";
        case FileState::Outdated: return "OUTDATED";
        case FileState::Stale: return "STALE";
    }
    return "?";
}

const char* stateHelp(FileState s)
{
    switch (s)
    {
        case FileState::Missing: return "not in the copy";
        case FileState::SizeDiffers: return "the copy has the wrong size";
        case FileState::ContentDiffers: return "the copy's bytes differ from the install's";
        case FileState::Outdated: return "changed in the install since the import (a game patch?)";
        case FileState::Stale: return "no longer in the install";
    }
    return "";
}

} // namespace

std::size_t VerifyReport::count(FileState s) const
{
    return static_cast<std::size_t>(std::ranges::count(issues, s, &FileIssue::state));
}

std::size_t VerifyReport::requiredMissing() const
{
    return static_cast<std::size_t>(
        std::ranges::count_if(issues, [](const FileIssue& i) { return i.required && i.state == FileState::Missing; }));
}

bool VerifyReport::broken() const
{
    return count(FileState::Missing) + count(FileState::SizeDiffers) + count(FileState::ContentDiffers) > 0;
}

bool requiredGameFile(const fs::path& rel)
{
    const auto parts = std::vector<fs::path>(rel.begin(), rel.end());
    if (parts.size() == 1)
    {
        const auto n = lower(parts[0].string());
        return n == "ffximain.dll" || n == "vtable.dat" || n == "ftable.dat";
    }
    if (parts.size() == 2 && numbered(parts[0].string(), "rom"))
    {
        // ROM2/VTABLE2.DAT, ROM2/FTABLE2.DAT ...
        const auto folder = lower(parts[0].string());
        const auto n      = folder.substr(3);
        const auto file   = lower(parts[1].string());
        return !n.empty() && (file == "vtable" + n + ".dat" || file == "ftable" + n + ".dat");
    }
    return false;
}

namespace
{

// The needed files under `from` (neededGameFile), by relative path, with size and modification time.
std::map<std::string, Entry> listNeeded(const fs::path& from, std::error_code& ec)
{
    std::map<std::string, Entry> now;
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        const auto rel = fs::relative(it->path(), from);
        if (it->is_directory())
        {
            // Skip whole folders the client never reads (mov/, Tools/ ...) without walking them.
            const auto top = lower(rel.begin()->string());
            if (it.depth() == 0 && !numbered(rel.begin()->string(), "rom") && !numbered(rel.begin()->string(), "sound") &&
                top != "user")
            {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file() || !neededGameFile(rel))
        {
            continue;
        }
        now[rel.generic_string()] = Entry{ static_cast<std::uint64_t>(it->file_size()), mtimeNs(it->path()) };
    }
    return now;
}

} // namespace

VerifyReport verifyGameData(const fs::path& dir, const VerifyOptions& options)
{
    VerifyReport rep;
    rep.dir      = dir;
    rep.contents = options.compareContents;
    std::set<std::string> reported;
    std::set<std::string> looked; // lower-case paths, so a file differing only in case counts once
    auto                  add = [&](const std::string& rel, FileState s) {
        if (reported.insert(rel).second)
        {
            rep.issues.push_back({ rel, s, s == FileState::Missing && requiredGameFile(rel) });
        }
    };

    const auto manifestPath = dir / "import.manifest";
    rep.manifest            = present(manifestPath);
    const auto manifest     = rep.manifest ? readManifest(manifestPath) : std::map<std::string, Entry>{};

    std::optional<fs::path> install = options.install;
    if (!install && rep.manifest)
    {
        install = recordedInstall(dir);
    }
    std::error_code              ec;
    std::map<std::string, Entry> inInstall;
    if (install && gameDirProblem(*install).empty() &&
        fs::weakly_canonical(*install, ec) != fs::weakly_canonical(dir, ec))
    {
        inInstall = listNeeded(*install, ec);
        if (!ec)
        {
            rep.install = install;
        }
    }

    // What the copy has (the import's manifest, else the folder itself: FFXI_ROOT pointing at an install).
    std::map<std::string, Entry> copy = manifest;
    if (!rep.manifest)
    {
        copy = listNeeded(dir, ec);
    }

    // The required files first, so they are reported as required even with no manifest or install.
    std::set<std::string> required = { "FFXiMain.dll", "VTABLE.DAT", "FTABLE.DAT" };
    for (const auto* m : { &copy, &inInstall })
    {
        for (const auto& [rel, e] : *m)
        {
            if (requiredGameFile(rel))
            {
                required.insert(rel);
            }
        }
    }
    // A ROMn folder in the copy or the install needs its own tables, even if the folder came over without them.
    std::set<std::string> romFolders;
    for (const auto* m : { &copy, &inInstall })
    {
        for (const auto& [rel, e] : *m)
        {
            const auto slash = rel.find('/');
            if (slash != std::string::npos && numbered(rel.substr(0, slash), "rom"))
            {
                romFolders.insert(rel.substr(0, slash));
            }
        }
    }
    for (const auto& f : romFolders)
    {
        const auto n = f.substr(3);
        if (!n.empty())
        {
            for (const char* table : { "/VTABLE", "/FTABLE" })
            {
                std::string rel = f;
                rel += table;
                rel += n;
                rel += ".DAT";
                required.insert(std::move(rel));
            }
        }
    }
    for (const auto& rel : required)
    {
        looked.insert(lower(rel));
        // Case-insensitive: installs copied from Windows may differ in case.
        bool found = present(dir / rel);
        for (const auto& [r, e] : copy)
        {
            if (!found && lower(r) == lower(rel))
            {
                found = present(dir / r);
            }
        }
        if (!found)
        {
            add(rel, FileState::Missing);
        }
    }
    if (!present(dir / "ROM") && !present(dir / "rom"))
    {
        add("ROM/", FileState::Missing);
        rep.issues.back().required = true;
    }

    // Every file the copy should have.
    for (const auto& [rel, e] : copy)
    {
        if (reported.contains(rel))
        {
            continue;
        }
        looked.insert(lower(rel));
        const auto p = dir / rel;
        if (!present(p))
        {
            add(rel, FileState::Missing);
            continue;
        }
        const auto size = fs::file_size(p, ec);
        if (ec || (rep.manifest && size != e.size))
        {
            add(rel, FileState::SizeDiffers);
            continue;
        }
        if (!rep.install)
        {
            continue;
        }
        const auto src = inInstall.find(rel);
        if (src == inInstall.end())
        {
            if (!isUserFile(rel))
            {
                add(rel, FileState::Stale);
            }
            continue;
        }
        if (isUserFile(rel))
        {
            continue; // macro files are ours to edit
        }
        if (rep.manifest && (src->second.size != e.size || src->second.mtime != e.mtime))
        {
            add(rel, FileState::Outdated);
            continue;
        }
        if (src->second.size != size)
        {
            add(rel, FileState::SizeDiffers);
            continue;
        }
        if ((options.compareContents || topLevel(rel)) && !sameContents(p, *rep.install / rel))
        {
            add(rel, FileState::ContentDiffers);
        }
    }
    // Files in the install the copy does not have (not imported, or added by a patch since).
    for (const auto& [rel, e] : inInstall)
    {
        if (!copy.contains(rel) && !reported.contains(rel))
        {
            looked.insert(lower(rel));
            add(rel, rep.manifest ? FileState::Outdated : FileState::Missing);
        }
    }
    rep.checked = looked.size();
    std::ranges::sort(rep.issues, [](const FileIssue& a, const FileIssue& b) {
        return std::tie(a.state, b.required, a.rel) < std::tie(b.state, a.required, b.rel);
    });
    return rep;
}

std::string reportSummary(const VerifyReport& r)
{
    std::string out = fmt::format("{} files checked", r.checked);
    if (r.issues.empty())
    {
        return out + ": all ok";
    }
    std::vector<std::string> parts;
    if (const auto n = r.count(FileState::Missing))
    {
        const auto req = r.requiredMissing();
        parts.push_back(req ? fmt::format("{} missing ({} required)", n, req) : fmt::format("{} missing", n));
    }
    if (const auto n = r.count(FileState::SizeDiffers))
    {
        parts.push_back(fmt::format("{} wrong size", n));
    }
    if (const auto n = r.count(FileState::ContentDiffers))
    {
        parts.push_back(fmt::format("{} different", n));
    }
    if (const auto n = r.count(FileState::Outdated))
    {
        parts.push_back(fmt::format("{} out of date", n));
    }
    if (const auto n = r.count(FileState::Stale))
    {
        parts.push_back(fmt::format("{} no longer in the install", n));
    }
    out += ": ";
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        out += (i ? ", " : "") + parts[i];
    }
    return out;
}

std::vector<std::string> formatReport(const VerifyReport& r, std::size_t perState)
{
    std::vector<std::string> out;
    std::string against = r.install ? "against " + r.install->string() : std::string("(no install to compare with)");
    out.push_back(fmt::format("Checked {} {}{}:", r.dir.string(), against,
                              !r.install   ? ""
                              : r.contents ? ", every file byte for byte"
                                           : ", by size (index files byte for byte; --verify compares every file)"));
    for (const auto s : { FileState::Missing, FileState::SizeDiffers, FileState::ContentDiffers, FileState::Outdated,
                          FileState::Stale })
    {
        std::size_t shown = 0;
        std::size_t total = 0;
        for (const auto& i : r.issues)
        {
            if (i.state != s)
            {
                continue;
            }
            ++total;
            if (shown < perState)
            {
                out.push_back(fmt::format("  {:9} {}{}  ({})", stateName(s), i.rel, i.required ? "  [required]" : "",
                                          stateHelp(s)));
                ++shown;
            }
        }
        if (total > shown)
        {
            out.push_back(fmt::format("  {:9} ... and {} more", stateName(s), total - shown));
        }
    }
    out.push_back("  " + reportSummary(r));
    if (r.count(FileState::Outdated) + r.count(FileState::Stale) + r.count(FileState::Missing) +
            r.count(FileState::SizeDiffers) + r.count(FileState::ContentDiffers) >
        0)
    {
        out.push_back(r.install
                          ? fmt::format("  Fix: xi-setup import \"{}\"", r.install->string())
                          : std::string("  Fix: patch the install with PlayOnline, then xi-setup import \"FOLDER\""));
    }
    return out;
}

ImportResult importGame(const fs::path& from, const fs::path& to,
                        const std::function<void(const ImportProgress&)>& progress)
{
    if (const auto problem = gameDirProblem(from); !problem.empty())
    {
        throw std::runtime_error(problem);
    }
    std::error_code ec;
    if (fs::weakly_canonical(from, ec) == fs::weakly_canonical(to, ec))
    {
        throw std::runtime_error("the source and the destination are the same folder");
    }

    // What the install has now (only the files the client reads).
    const auto now = listNeeded(from, ec);
    if (ec)
    {
        throw std::runtime_error(fmt::format("cannot list {}: {}", from.string(), ec.message()));
    }

    fs::create_directories(to);
    const auto manifestPath = to / "import.manifest";
    const auto before       = readManifest(manifestPath);

    ImportResult   res;
    ImportProgress prog;
    prog.filesTotal = now.size();
    for (const auto& [rel, e] : now)
    {
        prog.bytesTotal += e.size;
    }
    std::ofstream manifest(fs::path(manifestPath.string() + ".new"));
    for (const auto& [rel, e] : now)
    {
        const auto dst  = to / rel;
        const auto prev = before.find(rel);
        bool       same = prev != before.end() && prev->second.size == e.size && prev->second.mtime == e.mtime && present(dst) &&
                    fs::file_size(dst, ec) == e.size;
        if (isUserFile(rel) && present(dst))
        {
            ++res.kept; // never overwrite macros: ours may have been edited (macro_dir = install)
            same = true;
        }
        else if (same)
        {
            ++res.unchanged;
        }
        else
        {
            prog.current = rel;
            if (progress)
            {
                progress(prog);
            }
            res.reflink |= copyOne(from / rel, dst);
            ++res.copied;
            res.bytes += e.size;
        }
        ++prog.filesDone;
        prog.bytesDone += e.size;
        manifest << e.size << '\t' << e.mtime << '\t' << rel << '\n';
    }
    // Files an earlier import brought in that the install no longer has (macro files stay).
    for (const auto& [rel, e] : before)
    {
        if (!now.contains(rel) && !isUserFile(rel))
        {
            if (fs::remove(to / rel, ec))
            {
                ++res.removed;
            }
        }
    }
    manifest.close();
    fs::rename(fs::path(manifestPath.string() + ".new"), manifestPath);
    res.version = installedClientVersion(to).value_or("");
    std::ofstream(to / "import.version") << res.version << '\n' << from.string() << '\n';
    prog.current.clear();
    if (progress)
    {
        progress(prog);
    }
    return res;
}

} // namespace xi::install
