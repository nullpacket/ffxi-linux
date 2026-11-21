// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/ashita_dir.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{

fs::path scratch(const char* name)
{
    const auto dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

} // namespace

TEST_CASE("ashita dir: the game folder is the default root, ashita_dir and XI_ASHITA_DIR override it")
{
    unsetenv("XI_ASHITA_DIR");
    CHECK(xi::ashitadir::effectiveRoot("", "/data/game") == fs::path("/data/game"));
    CHECK(xi::ashitadir::effectiveRoot("/opt/ashita", "/data/game") == fs::path("/opt/ashita"));
    setenv("XI_ASHITA_DIR", "/env/ashita", 1);
    CHECK(xi::ashitadir::effectiveRoot("/opt/ashita", "/data/game") == fs::path("/env/ashita"));
    unsetenv("XI_ASHITA_DIR");
    CHECK(xi::ashitadir::effectiveRoot("off", "/data/game").empty());
    setenv("XI_ASHITA_DIR", "off", 1);
    CHECK(xi::ashitadir::effectiveRoot("", "/data/game").empty());
    unsetenv("XI_ASHITA_DIR");
}

TEST_CASE("ashita dir: the layout is created and the libraries are detected")
{
    const auto root = scratch("xi_test_ashita_layout");
    CHECK(xi::ashitadir::ensureLayout(root).empty());
    for (const auto* sub : { "addons", "addons/libs", "config/addons", "scripts" })
    {
        CHECK(fs::is_directory(root / sub));
    }
    CHECK_FALSE(xi::ashitadir::libsPresent(root));
    CHECK(xi::ashitadir::libsMissingHint(root).find("xi-setup addons") != std::string::npos);
    write(root / "addons/libs/settings.lua", "return {}");
    write(root / "addons/libs/common.lua", "return {}");
    CHECK(xi::ashitadir::libsPresent(root));
    CHECK(xi::ashitadir::ensureLayout(root).empty()); // again: existing folders are kept
    CHECK_FALSE(xi::ashitadir::ensureLayout("").empty());
    fs::remove_all(root);
}

TEST_CASE("ashita dir: copying libs and addons from an install never overwrites the user's addons")
{
    const auto from = scratch("xi_test_ashita_from");
    write(from / "addons/libs/settings.lua", "return 1");
    write(from / "addons/libs/common.lua", "return 2");
    write(from / "addons/libs/ffxi/enums.lua", "return 3");
    write(from / "addons/fps/fps.lua", "-- fps");
    write(from / "addons/distance/distance.lua", "-- distance");
    const auto root = scratch("xi_test_ashita_root");
    write(root / "addons/distance/distance.lua", "-- mine");

    CHECK(xi::ashitadir::ashitaFolderIn(from) == from);
    const auto r = xi::ashitadir::copyFromInstall(from, root, { "fps", "distance", "missing" });
    CHECK(r.libFiles == 3);
    CHECK(xi::ashitadir::libsPresent(root));
    CHECK(fs::exists(root / "addons/libs/ffxi/enums.lua"));
    CHECK(r.addons == std::vector<std::string>{ "fps" });
    CHECK(r.skipped == std::vector<std::string>{ "distance", "missing" });
    std::ifstream mine(root / "addons/distance/distance.lua");
    std::string   line;
    std::getline(mine, line);
    CHECK(line == "-- mine");

    const auto again = xi::ashitadir::copyFromInstall(from, root, {});
    CHECK(again.libFiles == 0); // unchanged files are left alone
    CHECK_THROWS(xi::ashitadir::copyFromInstall(root / "nowhere", root, {}));
    fs::remove_all(from);
    fs::remove_all(root);
}

TEST_CASE("ashita dir: the bundled addons fill in what is missing and never overwrite or re-add")
{
    const auto bundle = scratch("xi_test_ashita_bundle");
    write(bundle / "addons/libs/settings.lua", "return 1");
    write(bundle / "addons/libs/common.lua", "return 2");
    write(bundle / "addons/libs/ffxi/enums.lua", "return 3");
    write(bundle / "addons/fps/fps.lua", "-- fps");
    write(bundle / "addons/distance/distance.lua", "-- distance");
    write(bundle / "addons/dynamic_entity_renamer/dynamic_entity_renamer.lua", "-- renamer");
    const auto root = scratch("xi_test_ashita_bundle_root");
    write(root / "addons/libs/common.lua", "-- my common");
    write(root / "addons/distance/distance.lua", "-- mine");

    const auto r = xi::ashitadir::installBundled(bundle, root);
    CHECK(r.libFiles == 2); // common.lua is the user's: left alone
    CHECK(r.addons == std::vector<std::string>{ "dynamic_entity_renamer", "fps" });
    CHECK(r.skipped == std::vector<std::string>{ "distance" });
    CHECK(xi::ashitadir::libsPresent(root));
    for (const auto& [file, text] :
         { std::pair{ "addons/libs/common.lua", "-- my common" },
           std::pair{ "addons/distance/distance.lua", "-- mine" }, std::pair{ "addons/fps/fps.lua", "-- fps" } })
    {
        std::ifstream in(root / file);
        std::string   line;
        std::getline(in, line);
        CHECK(line == text);
    }

    // An addon the user removed is not added back; one new in the bundle is.
    fs::remove_all(root / "addons/fps");
    write(bundle / "addons/clock/clock.lua", "-- clock");
    const auto again = xi::ashitadir::installBundled(bundle, root);
    CHECK(again.libFiles == 0);
    CHECK(again.addons == std::vector<std::string>{ "clock" });
    CHECK_FALSE(fs::exists(root / "addons/fps"));
    CHECK_THROWS(xi::ashitadir::installBundled(root / "nowhere", root));
    fs::remove_all(bundle);
    fs::remove_all(root);
}

TEST_CASE("ashita dir: XI_ASHITA_BUNDLE names the bundle; the source tree's is found otherwise")
{
    const auto bundle = scratch("xi_test_ashita_bundle_env");
    write(bundle / "addons/libs/settings.lua", "return 1");
    setenv("XI_ASHITA_BUNDLE", bundle.c_str(), 1);
    CHECK(xi::ashitadir::bundledDir() == bundle.lexically_normal());
    unsetenv("XI_ASHITA_BUNDLE");
    const auto found = xi::ashitadir::bundledDir();
    REQUIRE_FALSE(found.empty());
    CHECK(fs::exists(found / "addons/distance/distance.lua"));
    CHECK_FALSE(fs::exists(found / "addons/dynamic_entity_renamer")); // built into the client (#138)
    CHECK(fs::exists(found / "MANIFEST.sha256"));
    fs::remove_all(bundle);
}
