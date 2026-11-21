// SPDX-License-Identifier: GPL-3.0-or-later
// Natively built Ashita plugins (issue #34, docs/ashita-compat.md "Plugins"): the plugin host driven headless with the
// test plugin tests/ashita_plugin_xitest.cpp, built against the Ashita SDK like a real one. Only built when the SDK is
// (cmake/ashita_plugins.cmake).
#include "ashita/ashita.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using xi::ashita::AddonManager;
using xi::ashita::Frame;
using xi::ashita::PacketEvent;

namespace
{

struct Harness
{
    fs::path                                                 dir;
    std::vector<std::string>                                 chat;
    std::vector<std::pair<std::string, int>>                 commands;
    std::vector<std::pair<uint16_t, std::vector<std::byte>>> sent;
    std::unique_ptr<AddonManager>                            m;

    Harness()
    {
        dir = fs::temp_directory_path() /
              ("xi_ashita_plugins_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir / "plugins");
        fs::create_directories(dir / "scripts");
        fs::copy_file(XI_TEST_PLUGIN, dir / "plugins" / "XiTest.so"); // any case: /load finds it
        xi::ashita::Host h;
        h.command = [this](const std::string& line, int mode) { commands.emplace_back(line, mode); };
        h.chat    = [this](const std::string& text, int) { chat.push_back(text); };
        h.send    = [this](uint16_t id, std::vector<std::byte> payload) { sent.emplace_back(id, std::move(payload)); };
        m = std::make_unique<AddonManager>(dir, xi::ashita::Profile::Latest, xi::ashita::Resources{}, std::move(h));
        auto w      = std::make_shared<xi::client::WorldSnapshot>();
        w->inZone   = true;
        w->charId   = 0x1001;
        w->actIndex = 0x400;
        w->charName = "Tester";
        Frame f;
        f.world = std::move(w);
        m->update(std::move(f));
    }
    ~Harness()
    {
        m.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Harness(const Harness&)            = delete;
    Harness& operator=(const Harness&) = delete;

    [[nodiscard]] bool saidPart(const std::string& s) const
    {
        return std::any_of(chat.begin(), chat.end(),
                           [&](const std::string& l) { return l.find(s) != std::string::npos; });
    }
};

} // namespace

TEST_CASE("ashita plugins: /load, commands through IAshitaCore, /unload")
{
    Harness h;
    CHECK(h.m->command("/load xitest", 1));
    REQUIRE(h.m->loadedPlugins() == std::vector<std::string>{ "XiTest" });
    CHECK(h.saidPart("Loaded plugin: xitest"));

    // The plugin takes its command, reads the party / resources and queues a line, which comes back as injected.
    CHECK(h.m->command("/xitest", 1));
    CHECK(h.saidPart("xitest: party0=Tester item1=- spell1=- mode=1 injected=0"));
    REQUIRE(h.commands.size() == 1);
    CHECK(h.commands[0] == std::pair<std::string, int>{ "/echo from xitest", 1 });
    CHECK_FALSE(h.m->command("/echo from xitest", 1));

    // A plugin may rewrite the line in place; the rest of the chain sees the new text.
    std::string line = "/raw /ma \"Cure\" <t>";
    CHECK_FALSE(h.m->commandEdit(line, 1));
    CHECK(line == "/ma \"Cure\" <t>");

    CHECK(h.m->command("/unload xitest", 1));
    CHECK(h.m->loadedPlugins().empty());
    CHECK_FALSE(h.m->command("/xitest", 1)); // nobody takes it now
}

TEST_CASE("ashita plugins: packets in and out")
{
    Harness h;
    REQUIRE(h.m->loadPlugin("XITEST"));
    PacketEvent in;
    in.id      = 0x051;
    in.data    = std::vector<std::byte>(24, std::byte{ 0 });
    in.data[0] = std::byte{ 0x51 };
    CHECK(h.m->packetIn(in));
    CHECK(in.data[4] == std::byte{ 0x42 });
    PacketEvent out;
    out.id   = 0x0B6;
    out.data = std::vector<std::byte>(8, std::byte{ 0 });
    CHECK_FALSE(h.m->packetOut(out)); // blocked
    PacketEvent other;
    other.id   = 0x015;
    other.data = std::vector<std::byte>(8, std::byte{ 0 });
    CHECK(h.m->packetOut(other));
}

TEST_CASE("ashita plugins: the boot script's /load lines, and plugins that are not there")
{
    Harness h;
    std::ofstream(h.dir / "scripts" / "default.txt") << "/load addons\r\n/load xitest\r\n/load nosuchplugin\r\n";
    h.m->runBootScript();
    CHECK(h.m->loadedPlugins() == std::vector<std::string>{ "XiTest" });
    CHECK(h.saidPart("Failed to load plugin 'nosuchplugin': no native build of plugin 'nosuchplugin'"));
    CHECK_FALSE(h.saidPart("'addons'"));    // the Lua layer is the addons plugin
    CHECK_FALSE(h.m->loadPlugin("xitest")); // already loaded
}
