// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/first_run.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using xi::launcher::FirstRunImport;

namespace
{

// A folder of its own per case: ctest runs the cases of this file in parallel processes.
fs::path scratch(const char* tag)
{
    const auto dir = fs::temp_directory_path() / (std::string("xi_test_first_run_") + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void touch(const fs::path& p, const std::string& text = "")
{
    std::ofstream(p) << text;
}

// A minimal FINAL FANTASY XI folder (no real game data: a few bytes per file).
void fakeGame(const fs::path& dir, const std::string& version = "30260604_0")
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
    touch(dir / "patch.cfg", "file x {\n" + version + " 1 2 3 x 4\n}\n");
}

xi::install::FoundInstall found(const fs::path& dir, std::string version, std::string problem = {})
{
    xi::install::FoundInstall f;
    f.dir     = dir;
    f.version = std::move(version);
    f.problem = std::move(problem);
    f.source  = "test";
    return f;
}

} // namespace

TEST_CASE("the first-run import is offered only when there is no usable game data")
{
    const auto root = scratch("have");
    const auto game = root / "game";
    fakeGame(game);
    const auto empty = root / "empty";
    fs::create_directories(empty);

    CHECK(xi::launcher::haveGameData("", game));
    CHECK_FALSE(xi::launcher::haveGameData("", std::nullopt));
    CHECK_FALSE(xi::launcher::haveGameData("", empty));
    // A profile's own game folder decides on its own: the client gets it as --root.
    CHECK(xi::launcher::haveGameData(game.string(), std::nullopt));
    CHECK_FALSE(xi::launcher::haveGameData(empty.string(), game));
}

TEST_CASE("first-run choices list usable installs first, newest version first")
{
    auto choices = xi::launcher::firstRunChoices({
        found("/a", "30250101_0", "has no ROM folder"),
        found("/b", "30240101_0"),
        found("/c", "30260604_0"),
        found("/d", ""),
        found("/e", "", "no such directory"),
    });
    REQUIRE(choices.size() == 5);
    CHECK(choices[0].dir == "/c");
    CHECK(choices[1].dir == "/b");
    CHECK(choices[2].dir == "/d");
    // Unusable ones keep their order at the end.
    CHECK(choices[3].dir == "/a");
    CHECK(choices[4].dir == "/e");
}

TEST_CASE("a picked folder resolves to the FINAL FANTASY XI folder inside it")
{
    const auto root   = scratch("pick");
    const auto prefix = root / "prefix";
    const auto game   = prefix / "drive_c" / "Program Files (x86)" / "PlayOnline" / "SquareEnix" / "FINAL FANTASY XI";
    fakeGame(game);

    auto r = xi::launcher::resolvePickedFolder(game.string(), root);
    REQUIRE(r.dir);
    CHECK(*r.dir == game);
    // A Wine prefix holding it, and "~" against the home folder.
    r = xi::launcher::resolvePickedFolder("~/prefix", root);
    REQUIRE(r.dir);
    CHECK(*r.dir == game);

    r = xi::launcher::resolvePickedFolder("", root);
    CHECK_FALSE(r.dir);
    CHECK_FALSE(r.problem.empty());
    r = xi::launcher::resolvePickedFolder((root / "nowhere").string(), root);
    CHECK_FALSE(r.dir);
    CHECK(r.problem.starts_with("no such folder"));
    fs::create_directories(root / "other");
    r = xi::launcher::resolvePickedFolder((root / "other").string(), root);
    CHECK_FALSE(r.dir);
    CHECK(r.problem.find("no FINAL FANTASY XI install") != std::string::npos);
}

TEST_CASE("the first-run import copies, checks and reports")
{
    const auto root = scratch("import");
    const auto src  = root / "install";
    const auto dst  = root / "copy";
    fakeGame(src);

    FirstRunImport imp;
    CHECK(imp.state() == FirstRunImport::State::Idle);
    CHECK(xi::launcher::firstRunOutcome(imp).empty());
    REQUIRE(imp.start(src, dst));
    imp.wait();
    REQUIRE(imp.state() == FirstRunImport::State::Done);
    CHECK(imp.succeeded());
    CHECK(imp.from() == src);
    const auto prog = imp.progress();
    CHECK(prog.filesDone == prog.filesTotal);
    CHECK(prog.filesTotal >= 7);
    REQUIRE(imp.result());
    CHECK(imp.result()->version == "30260604_0");
    REQUIRE(imp.report());
    CHECK(imp.report()->issues.empty());
    CHECK(xi::launcher::firstRunOutcome(imp).starts_with("Game data imported"));
    // The copy is now game data the launcher accepts.
    CHECK(xi::launcher::haveGameData("", dst));

    // Importing again is allowed once the first is finished, and copies nothing new.
    REQUIRE(imp.start(src, dst));
    imp.wait();
    CHECK(imp.succeeded());
    CHECK(imp.result()->copied == 0);
}

TEST_CASE("a first-run import from a broken install says what went wrong")
{
    const auto root = scratch("broken");
    const auto src  = root / "install";
    const auto dst  = root / "copy";

    // No install at all: importGame throws.
    FirstRunImport imp;
    REQUIRE(imp.start(root / "nowhere", dst));
    imp.wait();
    CHECK(imp.state() == FirstRunImport::State::Failed);
    CHECK_FALSE(imp.succeeded());
    CHECK(xi::launcher::firstRunOutcome(imp).starts_with("The import failed: "));

    // An install that lacks a ROM folder's tables: copied, but the check reports the required files missing.
    fakeGame(src);
    fs::remove(src / "ROM2" / "VTABLE2.DAT");
    REQUIRE(imp.start(src, dst));
    imp.wait();
    REQUIRE(imp.state() == FirstRunImport::State::Done);
    CHECK_FALSE(imp.succeeded());
    REQUIRE(imp.report());
    CHECK(imp.report()->requiredMissing() >= 1);
    CHECK(xi::launcher::firstRunOutcome(imp).starts_with("The copy is incomplete"));
}

TEST_CASE("a picked compress.dat is checked and copied, with ~ expanded (#136)")
{
    const auto            dir = scratch("cdat");
    std::vector<uint32_t> t(512);
    for (uint32_t i = 0; i < 256; ++i)
    {
        t[i]       = i;
        t[i + 256] = 8;
    }
    fs::create_directories(dir / "home" / "lsb");
    {
        std::ofstream out(dir / "home" / "lsb" / "compress.dat", std::ios::binary);
        out.write(reinterpret_cast<const char*>(t.data()), static_cast<std::streamsize>(t.size() * sizeof(uint32_t)));
    }
    touch(dir / "home" / "junk.dat", "not a table");
    const auto to = dir / "data" / "compress.dat";

    CHECK(xi::launcher::useCompressDat("", dir / "home", to) == "no file given");
    CHECK(xi::launcher::useCompressDat("~/junk.dat", dir / "home", to).find("bytes") != std::string::npos);
    CHECK_FALSE(fs::exists(to));
    CHECK(xi::launcher::useCompressDat("~/lsb/compress.dat", dir / "home", to).empty());
    CHECK(fs::file_size(to) == 2048);
    fs::remove_all(dir);
}
