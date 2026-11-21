// SPDX-License-Identifier: GPL-3.0-or-later
// xi-setup: first-run checks, importing the game data, and account / character creation against an LSB server
// (README "First run").
//
//   xi-setup import [FROM] [--to DIR] [--list] [--verify]
//                                                copy the game files the client reads into its own data dir, then
//                                                check the copy (--verify: every file byte for byte)
//   xi-setup addons [--to DIR]                   install the bundled Ashita addons and libraries into <game>/addons
//   xi-setup addons FROM|--install [--to DIR] [--list] [--addon NAME]...
//                                                copy Ashita's Lua libraries (and chosen addons) from an install
//   xi-setup check [--host H] [--verify]
//   xi-setup compress-dat [FILE]                 show which compress.dat the client uses; with FILE, check it and
//                                                copy it into the client's data dir (#136)
//   xi-setup create-account --user U --pass P [--host H]
//   xi-setup create-char --user U --pass P --name Name [--race 1..8] [--face 0..15] [--size 0..2]
//                        [--job 1..6] [--nation 0..2] [--host H]

#include "lobby/lobby.h"
#include "net/socket.h"
#include "xi/ashita_dir.h"
#include "xi/game_data.h"
#include "xi/install.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{

[[noreturn]] void usage()
{
    fmt::print(stderr,
               "usage: xi-setup import [FROM] [--to DIR] [--list] [--verify]\n"
               "       xi-setup addons [--to DIR]   (the bundled addons)\n"
               "       xi-setup addons FROM|--install [--to DIR] [--list] [--addon NAME]...\n"
               "       xi-setup check [--host H] [--verify]\n"
               "       xi-setup compress-dat [FILE]\n"
               "       xi-setup create-account --user U --pass P [--host H]\n"
               "       xi-setup create-char --user U --pass P --name Name [--race 1..8] [--face 0..15] [--size 0..2]\n"
               "                            [--job 1..6] [--nation 0..2] [--host H]\n"
               "  race: 1 Hume M, 2 Hume F, 3 Elvaan M, 4 Elvaan F, 5 Tarutaru M, 6 Tarutaru F, 7 Mithra, 8 Galka\n"
               "  job: 1 WAR, 2 MNK, 3 WHM, 4 BLM, 5 RDM, 6 THF;  nation: 0 San d'Oria, 1 Bastok, 2 Windurst\n"
               "  all: [--login-protocol auto|2.1|2.2] (LSB's xiloader version; default auto)\n");
    std::exit(2);
}

bool reachable(const std::string& host, uint16_t port)
{
    try
    {
        xi::net::TcpStream::connect(xi::net::Endpoint::resolve(host, port));
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// Every check a first run needs; prints one line each and returns the number of problems.
int check(const xi::lobby::Config& cfg, bool verifyContents)
{
    int problems = 0;
    auto ok      = [](const std::string& what) { fmt::print("  ok    {}\n", what); };
    auto bad     = [&](const std::string& what) {
        fmt::print("  FIX   {}\n", what);
        ++problems;
    };

    fmt::print("Game data (FFXI_ROOT, else {}):\n", xi::install::dataGameDir().string());
    const auto game = xi::install::findGameDir();
    if (!game)
    {
        std::string tried;
        for (const auto& d : xi::install::defaultGameDirs())
        {
            tried += "\n          " + d.string();
        }
        bad("no game data: run `xi-setup import` (copies it from an install) or set FFXI_ROOT. Looked at:" + tried);
    }
    else if (const auto problem = xi::install::gameDirProblem(*game); !problem.empty())
    {
        bad(problem);
    }
    else
    {
        ok(game->string());
        xi::install::VerifyOptions opt;
        opt.compareContents = verifyContents;
        const auto rep      = xi::install::verifyGameData(*game, opt);
        if (rep.issues.empty())
        {
            ok(xi::install::reportSummary(rep) +
               (rep.install ? " against " + rep.install->string() : std::string(" (required files present)")));
        }
        else
        {
            // The file lines and the fix, under the summary (formatReport's header and summary lines left out).
            const auto  summary = xi::install::reportSummary(rep);
            const auto  lines   = xi::install::formatReport(rep, 10);
            std::string detail;
            for (std::size_t i = 1; i < lines.size(); ++i)
            {
                if (lines[i] != "  " + summary)
                {
                    detail += "\n        " + lines[i];
                }
            }
            bad(summary + detail);
        }
        if (const auto v = xi::install::installedClientVersion(*game))
        {
            ok("client version " + *v + " (from patch.cfg; LSB's login.CLIENT_VER must not be newer)");
            const auto lsb    = xi::install::findLsbDir();
            const auto target = lsb ? xi::install::lsbTargetClientVersion(*lsb) : std::nullopt;
            if (const auto warn = target ? xi::install::dataVersionWarning(*v, *target) : std::string(); !warn.empty())
            {
                bad(warn);
            }
            else if (target)
            {
                ok("LSB targets client " + *target + " (" + (*lsb / "settings/default/login.lua").string() + ")");
            }
        }
        else
        {
            bad("no version in patch.cfg; pass --client-ver to match LSB's login.CLIENT_VER");
        }
    }

    fmt::print("LSB compress.dat:\n");
    const auto cd = xi::install::compressDatStatus();
    if (cd.found)
    {
        ok(cd.found->string());
    }
    else
    {
        std::string tried;
        for (const auto& p : cd.tried)
        {
            tried += "\n          " + p.string();
        }
        bad(xi::install::describeCompressDat(cd) + " Tried:" + tried);
    }

    fmt::print("LSB server at {}:\n", cfg.host);
    for (const auto& [port, name] : { std::pair{ cfg.authPort, "login (auth)" }, std::pair{ cfg.dataPort, "login (data)" },
                                      std::pair{ cfg.viewPort, "login (view)" }, std::pair{ uint16_t{ 54002 }, "search" } })
    {
        if (reachable(cfg.host, port))
        {
            ok(fmt::format("{} port {}", name, port));
        }
        else
        {
            bad(fmt::format("{} port {} does not answer (is the server running? xi_connect / xi_search)", name, port));
        }
    }
    fmt::print("{}\n", problems ? fmt::format("{} problem(s) to fix.", problems) : std::string("All set."));
    return problems;
}

std::string sizeText(std::uint64_t bytes)
{
    return bytes >= (1ull << 30) ? fmt::format("{:.1f} GB", static_cast<double>(bytes) / (1ull << 30))
                                 : fmt::format("{:.0f} MB", static_cast<double>(bytes) / (1ull << 20));
}

// xi-setup import [FROM] [--to DIR] [--list]: find an install (or use FROM: the FINAL FANTASY XI folder, a Wine prefix
// or a drive root) and copy the files the client reads into our data dir (game_data.h).
int importCommand(int argc, char** argv)
{
    std::string from;
    auto        to   = xi::install::dataGameDir();
    bool        list   = false;
    bool        verify = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--verify")
        {
            verify = true;
        }
        else if (a == "--to" && i + 1 < argc)
        {
            to = argv[++i];
        }
        else if (a == "--list")
        {
            list = true;
        }
        else if (!a.starts_with("--") && from.empty())
        {
            from = a;
        }
        else
        {
            usage();
        }
    }
    std::filesystem::path src;
    if (!from.empty())
    {
        const auto dir = xi::install::gameFolderIn(from);
        if (!dir)
        {
            fmt::print(stderr, "No FINAL FANTASY XI install (FFXiMain.dll) in {} or its usual places under it.\n", from);
            return 1;
        }
        src = *dir;
    }
    const auto found = xi::install::detectInstalls();
    if (list || from.empty())
    {
        fmt::print("Installs found on this machine:\n");
        for (const auto& f : found)
        {
            fmt::print("  {}  {}  ({}){}\n", f.version.empty() ? "(no version)" : f.version, f.dir.string(), f.source,
                       f.problem.empty() ? "" : "  [" + f.problem + "]");
        }
        if (found.empty())
        {
            fmt::print(
                "  none (looked in Steam libraries, Lutris, Faugus and Bottles games, ~/.wine, Wine prefixes under "
                "~/Games, and drives under /mnt, /media, /run/media)\n");
        }
        if (list)
        {
            return 0;
        }
    }
    if (src.empty())
    {
        // Only when there is exactly one usable install: with several (a retail one, a private server's modified copy,
        // backups) the right one is the user's call.
        const xi::install::FoundInstall* best   = nullptr;
        int                              usable = 0;
        for (const auto& f : found)
        {
            if (f.problem.empty())
            {
                best = &f;
                ++usable;
            }
        }
        if (usable > 1)
        {
            fmt::print(stderr, "Several installs found; pick one: xi-setup import \"FOLDER\" (one of the folders above). Use "
                               "an unmodified one whose version matches your server's login.CLIENT_VER.\n");
            return 1;
        }
        if (!best)
        {
            fmt::print(stderr, "No usable install found. Pass its folder: xi-setup import \"/path/to/FINAL FANTASY XI\" "
                               "(or a Wine prefix / drive holding it). README \"Getting the game data\" explains how to "
                               "get one.\n");
            return 1;
        }
        src = best->dir;
    }
    fmt::print("Importing from {}\n          into {}\n", src.string(), to.string());
    int lastPct = -1;
    try
    {
        const auto res = xi::install::importGame(src, to, [&](const xi::install::ImportProgress& p) {
            const int pct = p.bytesTotal ? static_cast<int>(p.bytesDone * 100 / p.bytesTotal) : 100;
            if (pct != lastPct)
            {
                lastPct = pct;
                fmt::print("\r  {:3}%  {}/{} files  {}", pct, p.filesDone, p.filesTotal,
                           p.current.size() > 50 ? "..." + p.current.substr(p.current.size() - 47) : p.current);
                fmt::print("{:20}", "");
                std::fflush(stdout);
            }
        });
        fmt::print("\r  done: {} copied ({}{}), {} unchanged, {} removed, {} macro files kept{:30}\n", res.copied,
                   sizeText(res.bytes), res.reflink ? ", reflinked: no extra disk space" : "", res.unchanged, res.removed,
                   res.kept, "");
        fmt::print("  client version {}\n", res.version.empty() ? "(none in patch.cfg)" : res.version);
        xi::install::VerifyOptions opt;
        opt.install         = src;
        opt.compareContents = verify;
        const auto report   = xi::install::verifyGameData(to, opt);
        for (const auto& line : xi::install::formatReport(report))
        {
            fmt::print("{}\n", line);
        }
        if (to != xi::install::dataGameDir())
        {
            fmt::print("Use it with FFXI_ROOT=\"{}\" (or --root).\n", to.string());
        }
        if (!xi::ashitadir::libsPresent(to))
        {
            fmt::print("Ashita addons go in {}; the client installs the bundled addons and libraries there on start "
                       "(or run `xi-setup addons`).\n",
                       (to / "addons").string());
        }
        if (report.broken())
        {
            fmt::print(stderr, "The copy is incomplete: {}.\n", xi::install::reportSummary(report));
            return 1;
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        fmt::print(stderr, "\nimport failed: {}\n", e.what());
        return 1;
    }
}

// xi-setup addons [FROM|--install] [--to DIR] [--list] [--addon NAME]...: the client's Ashita folder is the game folder
// (issue #73, xi/ashita_dir.h). With no FROM this installs the bundled addons and libraries (#138: the standard Ashita v4
// set less what doesn't work in the client), adding only what the folder doesn't have. With FROM (or --install: the one install
// found) it copies Ashita's shared Lua libraries (addons/libs) from an Ashita install the user has (HorizonXI's Game
// folder, an Ashita v4 folder, a Wine prefix holding one) and, with --addon, chosen addons.
int addonsCommand(int argc, char** argv)
{
    std::string              from;
    std::filesystem::path    to;
    bool                     list    = false;
    bool                     install = false;
    std::vector<std::string> addons;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--to" && i + 1 < argc)
        {
            to = argv[++i];
        }
        else if (a == "--addon" && i + 1 < argc)
        {
            addons.emplace_back(argv[++i]);
        }
        else if (a == "--list")
        {
            list = true;
        }
        else if (a == "--install")
        {
            install = true;
        }
        else if (!a.starts_with("--") && from.empty())
        {
            from = a;
        }
        else
        {
            usage();
        }
    }
    if (to.empty())
    {
        const auto game = xi::install::findGameDir();
        to              = xi::ashitadir::effectiveRoot("", game ? *game : xi::install::dataGameDir());
    }
    if (from.empty() && !install && !list)
    {
        if (!addons.empty())
        {
            fmt::print(stderr, "--addon copies from an install: give FROM or --install.\n");
            return 1;
        }
        const auto bundle = xi::ashitadir::bundledDir();
        if (bundle.empty())
        {
            fmt::print(stderr, "The bundled addons are missing (share/ffxi-linux/ashita-addons beside the programs, or "
                               "XI_ASHITA_BUNDLE). Copy from an install instead: xi-setup addons --install\n");
            return 1;
        }
        fmt::print("Installing the bundled Ashita addons from {}\n                                   into {}\n",
                   bundle.string(), (to / "addons").string());
        try
        {
            const auto  r = xi::ashitadir::installBundled(bundle, to);
            std::string added;
            for (const auto& a : r.addons)
            {
                added += (added.empty() ? "" : ", ") + a;
            }
            fmt::print("  {} library files added; addons added: {} ({} already there or removed by you, left alone)\n",
                       r.libFiles, added.empty() ? "none" : added, r.skipped.size());
            fmt::print("Config -> Add-ons in game loads them.\n");
            return 0;
        }
        catch (const std::exception& e)
        {
            fmt::print(stderr, "addons failed: {}\n", e.what());
            return 1;
        }
    }
    std::filesystem::path src;
    if (!from.empty())
    {
        src = xi::ashitadir::ashitaFolderIn(from);
        if (src.empty())
        {
            fmt::print(stderr, "No Ashita install (addons/libs/settings.lua) in {} or its usual places under it.\n", from);
            return 1;
        }
    }
    const auto found = xi::ashitadir::detectAshitaInstalls(to);
    if (list || from.empty())
    {
        const auto bundle = xi::ashitadir::bundledDir();
        fmt::print("Bundled addons: {}\n", bundle.empty() ? "not found" : bundle.string());
        fmt::print("Ashita installs found on this machine:\n");
        for (const auto& f : found)
        {
            fmt::print("  {:10} {}\n", f.label, f.dir.string());
        }
        if (found.empty())
        {
            fmt::print("  none (looked in ~/.wine, Wine prefixes under ~/Games, Ashita-v4* folders in your source dir)\n");
        }
        if (list)
        {
            return 0;
        }
    }
    if (src.empty())
    {
        if (found.size() != 1)
        {
            fmt::print(stderr, "{}\n", found.empty() ? "No Ashita install found. Pass one: xi-setup addons \"/path/to/Ashita\""
                                                     : "Several Ashita installs found; pick one: xi-setup addons \"FOLDER\"");
            return 1;
        }
        src = found.front().dir;
    }
    fmt::print("Copying Ashita's libraries from {}\n                           into {}\n", src.string(), (to / "addons" / "libs").string());
    try
    {
        const auto r      = xi::ashitadir::copyFromInstall(src, to, addons);
        std::string copied;
        for (const auto& a : r.addons)
        {
            copied += (copied.empty() ? "" : ", ") + a;
        }
        std::string skipped;
        for (const auto& a : r.skipped)
        {
            skipped += (skipped.empty() ? "" : ", ") + a;
        }
        fmt::print("  {} library files written; addons copied: {}{}\n", r.libFiles, copied.empty() ? "none" : copied,
                   skipped.empty() ? "" : " (skipped, already there or not in the install: " + skipped + ")");
        if (r.sdkFiles > 0)
        {
            fmt::print("  {} plugin SDK files written to {} (rebuild the client to build Ashita plugins natively)\n",
                       r.sdkFiles, (to / "plugins" / "sdk").string());
        }
        fmt::print("Put Ashita addon folders in {}; Config -> Add-ons loads them.\n", (to / "addons").string());
        return 0;
    }
    catch (const std::exception& e)
    {
        fmt::print(stderr, "addons failed: {}\n", e.what());
        return 1;
    }
}

// `xi-setup compress-dat [FILE]`: without FILE, where the client finds compress.dat; with FILE, check it and keep a
// copy in the client's data dir so it is found without LSB_DIR.
int compressDatCommand(int argc, char** argv)
{
    if (argc > 3)
    {
        usage();
    }
    if (argc == 3)
    {
        const std::filesystem::path from = argv[2];
        if (const auto err = xi::install::saveCompressDat(from); !err.empty())
        {
            fmt::print(stderr, "xi-setup: {}\n", err);
            return 1;
        }
        fmt::print("Copied {}\n    to {}\n", from.string(), xi::install::dataCompressDat().string());
    }
    const auto st = xi::install::compressDatStatus();
    fmt::print("compress.dat: {}\n", xi::install::describeCompressDat(st));
    return st.found ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string(argv[1]) == "compress-dat")
    {
        return compressDatCommand(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "addons")
    {
        return addonsCommand(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "import")
    {
        return importCommand(argc, argv);
    }
    if (argc < 2)
    {
        usage();
    }
    const std::string           cmd = argv[1];
    xi::lobby::Config           cfg;
    xi::lobby::NewCharacter     ch;
    bool                        verifyContents = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::string k    = argv[i];
        auto              next = [&]() -> std::string {
            if (i + 1 >= argc)
            {
                usage();
            }
            return argv[++i];
        };
        auto num = [&]() { return static_cast<uint8_t>(std::stoi(next())); };
        if (k == "--verify" && cmd == "check")
            verifyContents = true;
        else if (k == "--host")
            cfg.host = next();
        else if (k == "--user") cfg.username = next();
        else if (k == "--pass") cfg.password = next();
        else if (k == "--client-ver") cfg.clientVersion = next();
        else if (k == "--login-protocol")
        {
            const auto v = next();
            if (const auto p = xi::lobby::parseLoginProtocol(v))
            {
                cfg.protocol = *p;
            }
            else
            {
                fmt::print(stderr, "--login-protocol: '{}' is not auto, 2.1 or 2.2\n", v);
                usage();
            }
        }
        else if (k == "--name") ch.name = next();
        else if (k == "--race") ch.race = num();
        else if (k == "--face") ch.face = num();
        else if (k == "--size") ch.size = num();
        else if (k == "--job") ch.job = num();
        else if (k == "--nation") ch.nation = num();
        else usage();
    }
    if (const auto game = xi::install::findGameDir())
    {
        if (const auto v = xi::install::installedClientVersion(*game); v && cfg.clientVersion == xi::lobby::Config{}.clientVersion)
        {
            cfg.clientVersion = *v;
        }
    }

    try
    {
        if (cmd == "check")
        {
            return check(cfg, verifyContents) == 0 ? 0 : 1;
        }
        if (cfg.username.empty() || cfg.password.empty())
        {
            usage();
        }
        if (cmd == "create-account")
        {
            xi::lobby::createAccount(cfg);
            fmt::print("account {} created\n", cfg.username);
            return 0;
        }
        if (cmd == "create-char")
        {
            if (ch.name.empty())
            {
                usage();
            }
            const auto auth = xi::lobby::authenticate(cfg);
            xi::lobby::createCharacter(cfg, auth, ch);
            fmt::print("character {} created on {}\n", ch.name, cfg.username);
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        spdlog::error("{}", e.what());
        return 1;
    }
    usage();
}
