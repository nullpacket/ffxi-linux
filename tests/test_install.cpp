// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/install.h"

#include "xi/game_data.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

// A folder of its own per case: ctest runs the cases of this file in parallel processes.
fs::path scratchInstall(const char* tag)
{
    const auto dir = fs::temp_directory_path() / (std::string("xi_test_install_") + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void touch(const fs::path& p, const char* text = "")
{
    std::ofstream(p) << text;
}

} // namespace

TEST_CASE("first-run checks name what is missing from a game folder")
{
    const auto dir = scratchInstall("checks");
    CHECK(xi::install::gameDirProblem(dir / "nope").starts_with("no such directory"));
    CHECK(xi::install::gameDirProblem(dir).find("FFXiMain.dll") != std::string::npos);
    touch(dir / "FFXiMain.dll");
    touch(dir / "VTABLE.DAT");
    touch(dir / "FTABLE.DAT");
    CHECK(xi::install::gameDirProblem(dir).find("ROM") != std::string::npos);
    fs::create_directories(dir / "ROM");
    CHECK(xi::install::gameDirProblem(dir).empty());
    fs::remove_all(dir);
}

TEST_CASE("the installed client version is the newest version in patch.cfg")
{
    const auto dir = scratchInstall("version");
    CHECK_FALSE(xi::install::installedClientVersion(dir));
    touch(dir / "patch.cfg", "file patch.txt {\n"
                             "30020917_0 833977 62439499 -1977655372 30020917_0/Direct/patch.txt.slc 372355\n"
                             "30260604_0 2968330 217277387 -837573265 30260604_0/Direct/patch.txt.slc 1448303\n"
                             "30251227_0 2968330 217277387 -837573265 30251227_0/Direct/patch.txt.slc 1448303\n"
                             "}\n");
    const auto v = xi::install::installedClientVersion(dir);
    REQUIRE(v);
    CHECK(*v == "30260604_0");
    fs::remove_all(dir);
}

TEST_CASE("the client version LSB targets is CLIENT_VER in its default login settings (#58)")
{
    const auto lsb = fs::temp_directory_path() / "xi_test_lsb_settings"; // its own: ctest runs cases in parallel
    fs::remove_all(lsb);
    CHECK_FALSE(xi::install::lsbTargetClientVersion(lsb));
    fs::create_directories(lsb / "settings" / "default");
    touch(lsb / "settings" / "default" / "login.lua", "xi = xi or {}\n"
                                                      "xi.settings.login =\n"
                                                      "{\n"
                                                      "    -- CLIENT_VER = '20000101_0', an old comment\n"
                                                      "    CLIENT_VER = '30261006_0',\n"
                                                      "    VER_LOCK = 2,\n"
                                                      "}\n");
    const auto v = xi::install::lsbTargetClientVersion(lsb);
    REQUIRE(v);
    CHECK(*v == "30261006_0");
    fs::remove_all(lsb);

    using xi::install::dataVersionWarning;
    CHECK_FALSE(dataVersionWarning("30260604_0", "30261006_0").empty());
    CHECK(dataVersionWarning("30261006_0", "30261006_0").empty());
    CHECK(dataVersionWarning("30261106_0", "30261006_0").empty());
    CHECK(dataVersionWarning("", "30261006_0").empty());
}

TEST_CASE("the game data import takes only the files the client reads")
{
    using xi::install::neededGameFile;
    for (const char* yes : { "VTABLE.DAT", "FTABLE.DAT", "FFXiMain.dll", "patch.cfg", "ROM/27/80.DAT", "ROM9/VTABLE9.DAT",
                             "rom2/1/2.dat", "sound/win/se/se000/se000041.spw", "sound3/win/music/data/music001.bgw",
                             "USER/4/mcr.dat", "USER/4/mcr12.dat", "USER/4/mcr_2.ttl", "USER/4/mcr.sys" })
    {
        CHECK(neededGameFile(yes));
    }
    for (const char* no : { "mov/opening.mp4", "Tools/foo.exe", "TEMP/x", "SYS/x", "patch.txt", "file.txt", "FFXi.dll",
                            "d3d8.dll", "arw_e.ani", "xiloader.exe", "USER/4/config.dat", "ROMX/1.DAT", "soundtrack/x" })
    {
        CHECK_FALSE(neededGameFile(no));
    }
}

TEST_CASE("an import copies the needed files, skips unchanged ones and never overwrites macros")
{
    const auto src = scratchInstall("import");
    const auto dst = fs::temp_directory_path() / "xi_test_import";
    fs::remove_all(dst);
    touch(src / "FFXiMain.dll", "dll");
    touch(src / "VTABLE.DAT", "v");
    touch(src / "FTABLE.DAT", "ff");
    touch(src / "patch.cfg", "file x {\n30260604_0 1 2 3 x 4\n}\n");
    fs::create_directories(src / "ROM" / "1");
    touch(src / "ROM" / "1" / "2.DAT", "data");
    fs::create_directories(src / "mov");
    touch(src / "mov" / "intro.mp4", "skip me");
    fs::create_directories(src / "USER" / "4");
    touch(src / "USER" / "4" / "mcr.dat", "macro");

    auto r = xi::install::importGame(src, dst);
    CHECK(r.copied == 6);
    CHECK(r.version == "30260604_0");
    CHECK(fs::exists(dst / "ROM" / "1" / "2.DAT"));
    CHECK_FALSE(fs::exists(dst / "mov"));
    CHECK(fs::exists(dst / "import.manifest"));

    // Edit our macro copy, change a DAT, drop another: re-import updates only what changed, keeps the macro.
    touch(dst / "USER" / "4" / "mcr.dat", "edited");
    touch(src / "ROM" / "1" / "2.DAT", "patched data");
    fs::remove(src / "FFXiMain.dll");
    fs::create_directories(src / "ROM" / "1");
    touch(src / "FFXiMain.dll", "dll"); // same size; its time changed, so it counts as changed
    r = xi::install::importGame(src, dst);
    CHECK(r.kept == 1);
    CHECK(r.copied >= 1);
    std::ifstream macro(dst / "USER" / "4" / "mcr.dat");
    std::string   text;
    std::getline(macro, text);
    CHECK(text == "edited");
    std::ifstream dat(dst / "ROM" / "1" / "2.DAT");
    std::getline(dat, text);
    CHECK(text == "patched data");

    fs::remove(src / "patch.cfg");
    r = xi::install::importGame(src, dst);
    CHECK(r.removed == 1);
    CHECK_FALSE(fs::exists(dst / "patch.cfg"));

    CHECK(xi::install::gameFolderIn(src) == src);
    fs::remove_all(src);
    fs::remove_all(dst);
}

namespace
{

// A minimal FINAL FANTASY XI folder at `dir` (no real game data: a few bytes per file).
void fakeGame(const fs::path& dir, const char* version = "30260604_0")
{
    fs::create_directories(dir / "ROM" / "1");
    fs::create_directories(dir / "ROM2" / "1");
    touch(dir / "FFXiMain.dll", "dll");
    touch(dir / "VTABLE.DAT", "v");
    touch(dir / "FTABLE.DAT", "f");
    touch(dir / "ROM" / "1" / "2.DAT", "data");
    touch(dir / "ROM2" / "VTABLE2.DAT", "v2");
    touch(dir / "ROM2" / "FTABLE2.DAT", "f2");
    touch(dir / "ROM2" / "1" / "3.DAT", "data2");
    touch(dir / "patch.cfg", (std::string("file x {\n") + version + " 1 2 3 x 4\n}\n").c_str());
}

const fs::path kPlayOnline =
    fs::path("drive_c") / "Program Files (x86)" / "PlayOnline" / "SquareEnix" / "FINAL FANTASY XI";

const xi::install::FoundInstall* findBy(const std::vector<xi::install::FoundInstall>& found, const std::string& source)
{
    for (const auto& f : found)
    {
        if (f.source == source)
        {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Steam's libraryfolders.vdf lists every library folder")
{
    const auto dir = scratchInstall("vdf");
    touch(dir / "libraryfolders.vdf",
          "\"libraryfolders\"\n{\n"
          "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/home/me/.local/share/Steam\"\n"
          "\t\t\"label\"\t\t\"\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"230330\"\t\t\"1\"\n\t\t}\n\t}\n"
          "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/mnt/games/Steam Library\"\n\t}\n"
          "\t\"2\"\n\t{\n\t\t\"PATH\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n");
    const auto libs = xi::install::steamLibraryFolders(dir / "libraryfolders.vdf");
    REQUIRE(libs.size() == 3);
    CHECK(libs[0] == "/home/me/.local/share/Steam");
    CHECK(libs[1] == "/mnt/games/Steam Library");
    CHECK(libs[2] == "D:\\SteamLibrary");
    CHECK(xi::install::steamLibraryFolders(dir / "nope.vdf").empty());
    fs::remove_all(dir);
}

TEST_CASE("Lutris, Faugus and Bottles game lists name their prefixes")
{
    const auto dir = scratchInstall("launchers");
    fs::create_directories(dir / "lutris");
    touch(dir / "lutris" / "ffxi.yml", "name: FINAL FANTASY XI\n"
                                       "game_slug: ffxi\n"
                                       "game:\n"
                                       "  exe: ~/Games/ffxi/drive_c/PlayOnline/pol.exe\n"
                                       "  prefix: '~/Games/ffxi'\n"
                                       "script:\n"
                                       "  installer:\n"
                                       "  - task:\n"
                                       "      name: create_prefix\n"
                                       "      prefix: /opt/prefixes/ffxi\n");
    touch(dir / "lutris" / "notes.txt", "prefix: /ignored\n");
    const auto lutris = xi::install::lutrisPrefixes(dir / "lutris", "/home/me");
    REQUIRE(lutris.size() == 3);
    CHECK(lutris[0].name == "FINAL FANTASY XI");
    CHECK(lutris[0].prefix == "/home/me/Games/ffxi/drive_c/PlayOnline");
    CHECK(lutris[1].prefix == "/home/me/Games/ffxi");
    CHECK(lutris[2].prefix == "/opt/prefixes/ffxi");

    touch(dir / "games.json", R"([{"title": "HorizonXI", "prefix": "/home/me/Games/faugus/horizonxi",
                                   "path": "/home/me/Games/faugus/horizonxi/drive_c/HorizonXI/launcher.exe"},
                                  {"title": "No prefix"}, 7])");
    const auto faugus = xi::install::faugusPrefixes(dir / "games.json");
    REQUIRE(faugus.size() == 2);
    CHECK(faugus[0].name == "HorizonXI");
    CHECK(faugus[0].prefix == "/home/me/Games/faugus/horizonxi");
    CHECK(faugus[1].prefix == "/home/me/Games/faugus/horizonxi/drive_c/HorizonXI");
    touch(dir / "broken.json", "{ not json");
    CHECK(xi::install::faugusPrefixes(dir / "broken.json").empty());

    fs::create_directories(dir / "bottles" / "Gaming");
    fs::create_directories(dir / "bottles" / "Apps");
    const auto bottles = xi::install::bottlesPrefixes(dir / "bottles");
    REQUIRE(bottles.size() == 2);
    CHECK(bottles[0].name == "Apps");
    CHECK(bottles[1].prefix == dir / "bottles" / "Gaming");
    fs::remove_all(dir);
}

TEST_CASE("installs are found in Steam libraries, launcher prefixes, ~/Games and mounted drives")
{
    const auto                root = scratchInstall("detect");
    const auto                home = root / "home";
    xi::install::DetectPlaces places;
    places.home       = home;
    places.dataHome   = home / ".local" / "share";
    places.configHome = home / ".config";
    places.mountBases = { root / "mnt", root / "run-media" };

    // Steam: a second library listed in libraryfolders.vdf, holding the Proton prefix of app 230330.
    const auto steam = places.dataHome / "Steam";
    fs::create_directories(steam / "steamapps");
    const auto lib = root / "games" / "SteamLibrary";
    touch(steam / "steamapps" / "libraryfolders.vdf",
          ("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + steam.string() +
           "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + lib.string() + "\"\n\t}\n}\n")
              .c_str());
    fakeGame(lib / "steamapps" / "compatdata" / "230330" / "pfx" / kPlayOnline, "30260703_1");

    // Lutris (prefix outside ~/Games), Faugus, Bottles (Flatpak).
    fs::create_directories(places.dataHome / "lutris" / "games");
    touch(places.dataHome / "lutris" / "games" / "ffxi.yml",
          ("name: FFXI\ngame:\n  prefix: " + (root / "lutris-pfx").string() + "\n").c_str());
    fakeGame(root / "lutris-pfx" / kPlayOnline);
    fs::create_directories(places.configHome / "faugus-launcher");
    touch(places.configHome / "faugus-launcher" / "games.json",
          (R"([{"title": "HorizonXI", "prefix": ")" + (root / "faugus-pfx").string() + R"("}])").c_str());
    // A private server's launcher layout: Program Files/HorizonXI/Game/SquareEnix/FINAL FANTASY XI.
    fakeGame(root / "faugus-pfx" / "drive_c" / "Program Files" / "HorizonXI" / "Game" / "SquareEnix" /
             "FINAL FANTASY XI");
    fakeGame(home / ".var/app/com.usebottles.bottles/data/bottles/bottles/Games" / kPlayOnline);

    // ~/.wine, a prefix two levels under ~/Games, and a mounted Windows drive.
    fakeGame(home / ".wine" / kPlayOnline);
    fakeGame(home / "Games" / "other" / "ffxi" / kPlayOnline);
    fakeGame(root / "run-media" / "WINDOWS" / "Program Files (x86)" / "PlayOnline" / "SquareEnix" / "FINAL FANTASY XI");
    // Our own copy is never listed, and a prefix reached twice (Lutris and ~/Games) is listed once.
    fakeGame(places.dataHome / "ffxi-linux" / "game");
    fs::create_directories(home / "Games");
    fs::create_directory_symlink(root / "lutris-pfx", home / "Games" / "lutris-link");

    const auto found = xi::install::detectInstalls(places);
    CHECK(found.size() == 7);
    const auto* s = findBy(found, "Steam (Proton prefix)");
    REQUIRE(s);
    CHECK(s->version == "30260703_1");
    CHECK(s->problem.empty());
    CHECK(findBy(found, "Lutris: FFXI"));
    const auto* h = findBy(found, "Faugus: HorizonXI");
    REQUIRE(h);
    CHECK(h->dir.filename() == "FINAL FANTASY XI");
    CHECK(h->dir.parent_path().parent_path().filename() == "Game");
    CHECK(findBy(found, "Bottles: Games"));
    CHECK(findBy(found, "Wine (~/.wine)"));
    CHECK(findBy(found, "Wine prefix under ~/Games"));
    CHECK(findBy(found, "mounted drive"));
    for (const auto& f : found)
    {
        CHECK(f.dir.string().find("ffxi-linux") == std::string::npos);
    }
    fs::remove_all(root);
}

TEST_CASE("the required-file table is the DAT indexes and FFXiMain.dll")
{
    using xi::install::requiredGameFile;
    for (const char* yes :
         { "FFXiMain.dll", "VTABLE.DAT", "ftable.dat", "ROM2/VTABLE2.DAT", "ROM9/FTABLE9.DAT", "rom3/vtable3.dat" })
    {
        CHECK(requiredGameFile(yes));
    }
    for (const char* no :
         { "patch.cfg", "ROM/1/2.DAT", "ROM2/VTABLE3.DAT", "ROM/VTABLE.DAT", "sound/x.spw", "USER/4/mcr.dat" })
    {
        CHECK_FALSE(requiredGameFile(no));
    }
}

TEST_CASE("checking an import reports missing, damaged and out-of-date files")
{
    using xi::install::FileState;
    const auto root = scratchInstall("verify");
    const auto src  = root / "install";
    const auto dst  = root / "copy";
    fakeGame(src);
    xi::install::importGame(src, dst);

    xi::install::VerifyOptions opt;
    opt.install = src;
    auto rep    = xi::install::verifyGameData(dst, opt);
    CHECK(rep.issues.empty());
    CHECK_FALSE(rep.broken());
    CHECK(rep.checked >= 7);
    CHECK(xi::install::reportSummary(rep).ends_with("all ok"));
    // The install recorded in import.version is used when none is given.
    rep = xi::install::verifyGameData(dst);
    REQUIRE(rep.install);
    CHECK(*rep.install == src);

    // Damage the copy: a required table gone, a DAT truncated, an index file changed in place (same size).
    fs::remove(dst / "ROM2" / "VTABLE2.DAT");
    touch(dst / "ROM" / "1" / "2.DAT", "da");
    touch(dst / "VTABLE.DAT", "X");
    // The install moves on: a patched DAT, a new one, one removed.
    touch(src / "ROM2" / "1" / "3.DAT", "patched data2");
    touch(src / "ROM2" / "1" / "4.DAT", "new");
    fs::remove(src / "patch.cfg");

    rep = xi::install::verifyGameData(dst, opt);
    CHECK(rep.broken());
    CHECK(rep.requiredMissing() == 1);
    CHECK(rep.count(FileState::Missing) == 1);
    CHECK(rep.count(FileState::SizeDiffers) == 1);
    CHECK(rep.count(FileState::ContentDiffers) == 1);
    CHECK(rep.count(FileState::Outdated) == 2);
    CHECK(rep.count(FileState::Stale) == 1);
    REQUIRE_FALSE(rep.issues.empty());
    CHECK(rep.issues.front().rel == "ROM2/VTABLE2.DAT"); // required missing files come first
    CHECK(rep.issues.front().required);
    const auto summary = xi::install::reportSummary(rep);
    CHECK(summary.find("1 missing (1 required)") != std::string::npos);
    CHECK(summary.find("2 out of date") != std::string::npos);
    const auto lines = xi::install::formatReport(rep, 1);
    CHECK(lines.front().find(src.string()) != std::string::npos);
    CHECK(lines.back().find("xi-setup import") != std::string::npos);

    // Importing again fixes everything the install still has.
    touch(src / "patch.cfg", "file x {\n30260604_0 1 2 3 x 4\n}\n");
    fs::remove(dst / "import.manifest"); // forget the import, so the damaged copies are copied again
    xi::install::importGame(src, dst);
    opt.compareContents = true;
    rep                 = xi::install::verifyGameData(dst, opt);
    CHECK(rep.contents);
    CHECK(rep.issues.empty());

    // A folder that is not an import (FFXI_ROOT at an install): only the required files are checked.
    fs::remove(src / "FFXiMain.dll");
    rep = xi::install::verifyGameData(src);
    CHECK_FALSE(rep.manifest);
    CHECK_FALSE(rep.install);
    CHECK(rep.requiredMissing() == 1);
    fs::remove_all(root);
}

namespace
{

// A valid compress table: every byte value an 8-bit code (complete and prefix-free), as LSB's file is laid out
// (256 codes, then 256 lengths, LE uint32). Not LSB's real codes.
void writeCompressDat(const fs::path& p, std::size_t bytes = 2048, uint32_t len = 8)
{
    std::vector<uint32_t> t(512);
    for (uint32_t i = 0; i < 256; ++i)
    {
        t[i]       = i;
        t[i + 256] = len;
    }
    std::ofstream     out(p, std::ios::binary);
    std::vector<char> raw(bytes, 0);
    std::memcpy(raw.data(), t.data(), std::min(bytes, t.size() * sizeof(uint32_t)));
    out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
}

// Sets an environment variable for one test case and puts the old value back.
struct ScopedEnv
{
    ScopedEnv(const char* name, const std::string& value)
    : name_(name)
    {
        if (const char* old = std::getenv(name))
        {
            old_ = old;
        }
        if (value.empty())
        {
            ::unsetenv(name);
        }
        else
        {
            ::setenv(name, value.c_str(), 1);
        }
    }
    ~ScopedEnv()
    {
        if (old_)
        {
            ::setenv(name_, old_->c_str(), 1);
        }
        else
        {
            ::unsetenv(name_);
        }
    }
    ScopedEnv(const ScopedEnv&)            = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char*                name_;
    std::optional<std::string> old_;
};

} // namespace

TEST_CASE("compress.dat is checked the way the client loads it (#136)")
{
    const auto dir = scratchInstall("cdat_check");
    CHECK(xi::install::compressDatProblem(dir / "none.dat") == "missing");
    CHECK(xi::install::compressDatProblem(dir) == "not a file");
    writeCompressDat(dir / "good.dat");
    CHECK(xi::install::compressDatProblem(dir / "good.dat").empty());
    writeCompressDat(dir / "short.dat", 1000);
    CHECK(xi::install::compressDatProblem(dir / "short.dat").find("1000 bytes") != std::string::npos);
    writeCompressDat(dir / "zero.dat", 2048, 0); // every length 0: not a code table
    CHECK(xi::install::compressDatProblem(dir / "zero.dat").starts_with("not a compress table"));
    fs::remove_all(dir);
}

TEST_CASE("compress.dat is found in order, and a saved copy is used without LSB_DIR (#136)")
{
    const auto dir  = scratchInstall("cdat_find");
    const auto home = dir / "home";
    fs::create_directories(home);
    const ScopedEnv h("HOME", home.string());
    const ScopedEnv x("XDG_DATA_HOME", (dir / "data").string());
    const ScopedEnv c("XI_COMPRESS_DAT", "");
    const ScopedEnv l("LSB_DIR", "");

    const auto own = xi::install::dataCompressDat();
    CHECK(own == dir / "data" / "ffxi-linux" / "compress.dat");

    // Nothing anywhere: not found, and the description says how to fix it.
    auto st = xi::install::compressDatStatus();
    CHECK_FALSE(st.found);
    CHECK(xi::install::describeCompressDat(st).find("xi-setup compress-dat") != std::string::npos);
    CHECK(std::find(st.tried.begin(), st.tried.end(), own) != st.tried.end());

    // A server checkout in a default place is found.
    fs::create_directories(home / "git" / "server" / "res");
    writeCompressDat(home / "git" / "server" / "res" / "compress.dat");
    st = xi::install::compressDatStatus();
    REQUIRE(st.found);
    CHECK(*st.found == home / "git" / "server" / "res" / "compress.dat");

    // Saving a file makes our copy, which comes before the default checkouts.
    fs::create_directories(dir / "picked");
    writeCompressDat(dir / "picked" / "compress.dat");
    CHECK(xi::install::saveCompressDat(dir / "picked" / "compress.dat").empty());
    CHECK(fs::file_size(own) == 2048);
    CHECK_FALSE(fs::exists(fs::path(own.string() + ".part")));
    st = xi::install::compressDatStatus();
    REQUIRE(st.found);
    CHECK(*st.found == own);

    // A broken file is never saved, and the good copy stays.
    writeCompressDat(dir / "picked" / "bad.dat", 100);
    CHECK_FALSE(xi::install::saveCompressDat(dir / "picked" / "bad.dat").empty());
    CHECK(xi::install::compressDatProblem(own).empty());

    {
        // LSB_DIR wins over our copy; XI_COMPRESS_DAT wins over both. An unusable file is skipped and reported.
        fs::create_directories(dir / "lsb" / "res");
        writeCompressDat(dir / "lsb" / "res" / "compress.dat");
        const ScopedEnv l2("LSB_DIR", (dir / "lsb").string());
        st = xi::install::compressDatStatus();
        REQUIRE(st.found);
        CHECK(*st.found == dir / "lsb" / "res" / "compress.dat");
        const ScopedEnv c2("XI_COMPRESS_DAT", (dir / "picked" / "bad.dat").string());
        st = xi::install::compressDatStatus();
        REQUIRE(st.found);
        CHECK(*st.found == dir / "lsb" / "res" / "compress.dat");
        REQUIRE(st.skipped.size() == 1);
        CHECK(st.skipped[0].find("bad.dat") != std::string::npos);
        CHECK(xi::install::findCompressDat() == st.found);
    }
    fs::remove_all(dir);
}
