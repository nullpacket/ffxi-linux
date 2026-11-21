// SPDX-License-Identifier: GPL-3.0-or-later
// xi-headless: logs a character into an LSB server and stays in zone, printing what it sees.
//
//   xi-headless --user USER --pass PASS [--char Name] [--host 127.0.0.1] [--seconds 60]
//               [--compress-dat ~/git/server/res/compress.dat] [--say "hello"] [--wander R] [--quiet] [--verbose]
//
// --wander R walks between random points within R yalms of the zone-in spot (about 4 yalms a second, short pauses),
// for crowds in performance tests (docs/performance.md). --quiet logs only warnings and errors.

#include "lobby/lobby.h"
#include "map/map_client.h"
#include "xi/bytes.h"
#include "xi/install.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <map>
#include <numbers>
#include <random>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace
{

// SIGINT / SIGTERM end the stay early with a clean logout.
volatile std::sig_atomic_t stopRequested = 0;

struct Args
{
    xi::lobby::Config lobby;
    std::string       character;
    std::string       compressDat;
    std::string       say;
    int               seconds = 60;
    uint8_t           chatKind = 0;
    float             wander   = 0.0f;
    bool              verbose = false;
    bool              quiet   = false;
};

[[noreturn]] void usage()
{
    fmt::print(stderr,
               "usage: xi-headless --user U --pass P [--char NAME] [--host H] [--seconds N]\n"
               "                   [--client-ver 30260604_0] [--compress-dat PATH] [--say TEXT | --shout TEXT] [--wander R]\n"
               "                   [--quiet | --verbose] [--login-protocol auto|2.1|2.2]\n");
    std::exit(2);
}

Args parseArgs(int argc, char** argv)
{
    Args a;
    if (const auto c = xi::install::findCompressDat())
    {
        a.compressDat = c->string();
    }
    if (const auto game = xi::install::findGameDir())
    {
        a.lobby.clientVersion = xi::install::installedClientVersion(*game).value_or(a.lobby.clientVersion);
    }
    for (int i = 1; i < argc; ++i)
    {
        const std::string k    = argv[i];
        auto              next = [&]() -> std::string {
            if (i + 1 >= argc)
            {
                usage();
            }
            return argv[++i];
        };
        if (k == "--user") a.lobby.username = next();
        else if (k == "--pass") a.lobby.password = next();
        else if (k == "--otp") a.lobby.otp = next();
        else if (k == "--host") a.lobby.host = next();
        else if (k == "--char") a.character = next();
        else if (k == "--client-ver") a.lobby.clientVersion = next();
        else if (k == "--login-protocol")
        {
            const auto v = next();
            if (const auto p = xi::lobby::parseLoginProtocol(v))
            {
                a.lobby.protocol = *p;
            }
            else
            {
                fmt::print(stderr, "--login-protocol: '{}' is not auto, 2.1 or 2.2\n", v);
                usage();
            }
        }
        else if (k == "--compress-dat") a.compressDat = next();
        else if (k == "--seconds") a.seconds = std::stoi(next());
        else if (k == "--say") a.say = next();
        else if (k == "--shout") { a.say = next(); a.chatKind = 1; }
        else if (k == "--wander") a.wander = std::stof(next());
        else if (k == "--verbose") a.verbose = true;
        else if (k == "--quiet") a.quiet = true;
        else usage();
    }
    if (a.lobby.username.empty() || a.lobby.password.empty())
    {
        usage();
    }
    return a;
}

} // namespace

int main(int argc, char** argv)
{
    const auto args = parseArgs(argc, argv);
    spdlog::set_level(args.verbose ? spdlog::level::debug : args.quiet ? spdlog::level::warn : spdlog::level::info);

    try
    {
        const auto huffman = xi::Huffman::fromCompressDat(args.compressDat);

        const auto auth = xi::lobby::authenticate(args.lobby);
        spdlog::info("auth: account {} ok", auth.accountId);

        const auto handoff = xi::lobby::enterWorld(args.lobby, auth, [&](const std::vector<xi::lobby::CharacterSlot>& slots) -> std::optional<std::size_t> {
            std::optional<std::size_t> pick;
            for (std::size_t i = 0; i < slots.size(); ++i)
            {
                const auto& s = slots[i];
                if (!s.occupied)
                {
                    continue;
                }
                spdlog::info("lobby: slot {} {} (charid {}) on {} - job {} lv{}/{} zone {}", i, s.name, s.charId, s.world, s.mainJob, s.mainLvl, s.subJob, s.zone);
                if (!pick && (args.character.empty() || s.name == args.character))
                {
                    pick = i;
                }
            }
            return pick;
        });
        spdlog::info("lobby: map server {} search {}", handoff.map.toString(), handoff.search.toString());

        xi::map::MapClient map(handoff, huffman);

        std::map<uint16_t, std::size_t> seen;
        std::map<uint32_t, std::string> names;
        map.onPacket([&](const xi::map::SubPacket& sp) {
            ++seen[sp.id];
            switch (sp.id)
            {
                case 0x00A:
                    spdlog::info("zone-in: zone {} pos ({:.2f}, {:.2f}, {:.2f}) dir {}", map.zone(), map.position().x, map.position().y, map.position().z, map.position().dir);
                    break;
                case 0x00E: // entity update: UniqueNo @+4, update mask @+0x0A, name @+0x34 when mask has 0x08
                {
                    const auto id   = xi::load<uint32_t>(sp.data, 4);
                    const auto mask = xi::load<uint8_t>(sp.data, 0x0A);
                    if ((mask & 0x08) && sp.data.size() > 0x34 && !names.contains(id))
                    {
                        auto name = xi::loadString(sp.data, 0x34, std::min<std::size_t>(sp.data.size() - 0x34, 24));
                        if (!name.empty())
                        {
                            names[id] = name;
                            spdlog::info("entity: {:#010x} {}", id, name);
                        }
                    }
                    break;
                }
                case 0x017: // chat: kind @+4, sender @+8 (15), message @+0x17
                    spdlog::info("chat[{}] {}: {}", xi::load<uint8_t>(sp.data, 4), xi::loadString(sp.data, 8, 15), xi::loadString(sp.data, 0x17, sp.data.size() - 0x17));
                    break;
                case 0x00B:
                    spdlog::info("server sent 0x00B (logout/zone, state {})", xi::load<uint8_t>(sp.data, 4));
                    break;
                default:
                    spdlog::debug("rx 0x{:03X} ({} bytes)", sp.id, sp.data.size());
                    break;
            }
        });

        map.zoneIn();
        if (!args.say.empty())
        {
            map.say(args.say, args.chatKind);
        }

        // --wander: the zone-in spot is the centre; one step per tick toward a random waypoint, then a pause.
        const xi::map::Position home = map.position();
        std::mt19937            rng(std::random_device{}());
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        float                   goalX = home.x, goalZ = home.z;
        int                     pause = 0;
        if (args.wander > 0.0f)
        {
            map.setPositionUpdates(true);
        }

        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(args.seconds);
        std::signal(SIGINT, [](int) { stopRequested = 1; });
        std::signal(SIGTERM, [](int) { stopRequested = 1; });
        while (std::chrono::steady_clock::now() < end && !stopRequested)
        {
            const auto start = std::chrono::steady_clock::now();
            if (args.wander > 0.0f)
            {
                auto        p    = map.position();
                const float dx   = goalX - p.x;
                const float dz   = goalZ - p.z;
                const float dist = std::hypot(dx, dz);
                if (pause > 0)
                {
                    --pause;
                }
                else if (dist < 0.5f)
                {
                    // Next waypoint: uniform in the disc; stand 0..3 s first.
                    const float r = args.wander * std::sqrt(unit(rng));
                    const float a = 2.0f * std::numbers::pi_v<float> * unit(rng);
                    goalX         = home.x + r * std::cos(a);
                    goalZ         = home.z + r * std::sin(a);
                    pause         = static_cast<int>(unit(rng) * 12.0f);
                }
                else
                {
                    const float step = std::min(dist, 1.0f); // 4 yalms/s at 4 ticks a second
                    p.x += dx / dist * step;
                    p.z += dz / dist * step;
                    // Heading byte: facing (cos a, 0, sin a) with a = -dir * pi / 128.
                    const float h = -std::atan2(dz, dx) * 128.0f / std::numbers::pi_v<float>;
                    p.dir         = static_cast<uint8_t>(static_cast<int>(std::lround(h)) & 0xFF);
                    map.setPosition(p);
                }
            }
            map.tick(250ms);
            std::this_thread::sleep_until(start + 250ms);
        }

        spdlog::info("logging out...");
        if (!map.logout())
        {
            spdlog::warn("logout: no 0x00B from server; session will time out server-side");
        }

        std::string summary;
        for (const auto& [id, count] : seen)
        {
            summary += fmt::format(" {:03X}x{}", id, count);
        }
        spdlog::info("done: {} named entities; packets:{}", names.size(), summary);
    }
    catch (const std::exception& e)
    {
        spdlog::error("{}", e.what());
        return 1;
    }
    return 0;
}
