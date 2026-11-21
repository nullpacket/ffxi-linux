// SPDX-License-Identifier: GPL-3.0-or-later
// --script: the step-file player's steps (apps/xi-client/test_script.h), through normal player input.
#include "client_app.h"

namespace xi::app
{

void ClientApp::runScript()
{
    if (script.loaded() && session && session->finished() && !quitting)
    {
        spdlog::error("script: the session ended; quitting");
        quitting = true;
    }
    // --script: the steps play out one at a time (apps/xi-client/test_script.h), through the same inputs a
    // player uses: movement, targeting, the Talk action, interface keys and chat-line commands.
    if (script.loaded() && !script.done() && zonedAt && session && !quitting)
    {
        using namespace std::chrono_literals;
        if (!scriptState_.started)
        {
            scriptState_.started = true;
            script.begin(now);
        }
        const auto&     s  = script.step();
        const float     t  = script.elapsed(now);
        const glm::vec3 me = gfx::ZoneScene::renderToGame(feet);
        auto argf = [&](std::size_t i) { return i < s.args.size() ? std::strtof(s.args[i].c_str(), nullptr) : 0.0f; };
        auto invCount = [&](int id) {
            int n = 0;
            for (const auto& it : world.inventory)
            {
                n += it.container == 0 && it.itemId == id ? static_cast<int>(it.count) : 0;
            }
            return n;
        };
        auto sendKeys = [&](std::string list) {
            args.uiKeys    = std::move(list);
            uiKeyStep      = 0;
            scriptKeysFrom = now - std::chrono::milliseconds(5000);
        };
        const bool ready = world.inZone && scene && scene->zone() == world.zone;
        // Knocked out: only the steps a player can still take (waiting, interface keys for the home point
        // prompt, the zone change after it, chat-line commands such as a GM's !raise) go on.
        const bool koSteps = s.op == "wait" || s.op == "keys" || s.op == "zone" || s.op == "shot" || s.op == "log" || s.op == "logout" || s.op == "cmd";
        if ((world.animation == 2 || world.animation == 3) && !koSteps)
        {
            args.autoFight = false;
            args.chase     = false;
            scriptGoto.reset();
            script.abort("knocked out");
        }
        else if (s.op == "wait")
        {
            if (t >= argf(0))
            {
                script.next(now);
            }
        }
        else if (s.op == "log")
        {
            spdlog::info("script: {}", s.rest);
            script.next(now);
        }
        else if (s.op == "fx")
        {
            // fx FILE NAME: run that DAT's scheduler NAME on ourselves (effect tests).
            std::istringstream in(s.rest);
            uint32_t           file = 0;
            std::string        name;
            in >> file >> name;
            name.resize(4, ' ');
            effects.playFile(file, dat::fxName(name), world.charId, world.charId);
            script.next(now);
        }
        else if (s.op == "trade")
        {
            // trade asked|open|closed|offer|ok [SECONDS]: wait for the player-trade state (a request from someone, the
            // window open or closed, the partner's offer changed since this step began, the partner confirmed).
            const auto& tr   = world.trade;
            const auto  what = s.args.empty() ? std::string() : s.args[0];
            if (script.stage == 0)
            {
                script.stage = 1;
                scriptTradeSeen = tr.theirsUpdates;
            }
            const bool met = what == "asked" ? tr.requestFrom != 0 && !tr.active
                           : what == "open"  ? tr.active
                           : what == "closed" ? !tr.active
                           : what == "offer" ? tr.theirsUpdates != scriptTradeSeen
                           : what == "ok"    ? tr.partnerConfirmed
                                             : true;
            if (met)
            {
                script.next(now);
            }
            else if (t > (s.args.size() > 1 ? argf(1) : 90.0f))
            {
                script.abort(fmt::format("trade never '{}'", what));
            }
        }
        else if (s.op == "zone")
        {
            // In the zone (an event starting on arrival is the next step's to answer: `event ifany`).
            if (!ready || world.zone != static_cast<uint16_t>(argf(0)))
            {
                script.mark = now;
            }
            else if (now - script.mark > 3s)
            {
                script.next(now);
            }
            if (t > 180.0f)
            {
                script.abort(fmt::format("still in zone {}", world.zone));
            }
        }
        else if (s.op == "goto")
        {
            if (script.zone == 0)
            {
                script.zone = world.zone;
            }
            const std::size_t n = s.args.size() / 2;
            if (script.waypoint >= n || world.zone != script.zone)
            {
                scriptGoto.reset();
                script.next(now);
            }
            else
            {
                const glm::vec2 w(argf(2 * script.waypoint), argf(2 * script.waypoint + 1));
                const glm::vec2 here(me.x, me.z);
                const float     d = glm::distance(w, here);
                // An event running (a zone-in cutscene) holds the walk; one that does not end needs an event step.
                if (eventBusy() || !ready)
                {
                    if (script.menuSeen == app::TestScript::Clock::time_point{})
                    {
                        script.menuSeen = now;
                    }
                    script.progress = now;
                    script.best     = d;
                    if (now - script.menuSeen > 30s)
                    {
                        scriptGoto.reset();
                        script.abort(fmt::format("an event has held the walk for 30 s at ({:.1f}, {:.1f}); answer it with an event step first", me.x, me.z));
                    }
                }
                else if (d < 0.8f || (d < 3.0f && now - script.progress > 3s))
                {
                    // Reached (or blocked just short of it by a tree or a post: close enough).
                    spdlog::info("script: waypoint {} of {} ({:.1f}, {:.1f}) reached at ({:.1f}, {:.1f})", script.waypoint + 1, n, w.x, w.y, me.x, me.z);
                    ++script.waypoint;
                    script.best     = 1e9f;
                    script.progress = now;
                    script.counter  = 0;
                }
                else
                {
                    script.menuSeen = {};
                    if (d < script.best - 0.5f)
                    {
                        script.best     = d;
                        script.progress = now;
                    }
                    if (now < script.mark)
                    {
                        // Sidestepping: walk across the line to the waypoint, alternately left and right.
                        const glm::vec2 to = glm::normalize(w - here);
                        const float     sgn = script.counter % 2 ? -1.0f : 1.0f;
                        scriptGoto          = here + glm::vec2(-to.y, to.x) * (3.0f * sgn);
                    }
                    else if (now - script.progress > 3s)
                    {
                        // Under half a yalm closer in 3 s: stuck. Sidestep (three tries), then give up.
                        spdlog::warn("script: stuck at ({:.1f}, {:.1f}, y {:.1f}), {:.1f} from waypoint {} ({:.1f}, {:.1f})", me.x, me.z, me.y, d,
                                     script.waypoint + 1, w.x, w.y);
                        if (++script.counter > 3)
                        {
                            scriptGoto.reset();
                            script.abort(fmt::format("stuck at ({:.1f}, {:.1f}) on the way to ({:.1f}, {:.1f})", me.x, me.z, w.x, w.y));
                        }
                        else
                        {
                            script.mark     = now + 1500ms;
                            script.progress = now + 1500ms;
                            script.best     = 1e9f;
                        }
                    }
                    else
                    {
                        scriptGoto = w;
                    }
                }
            }
        }
        else if (s.op == "approach")
        {
            // Walk toward the current target until within DIST (default 12: spell range), then stand still.
            const float dist = s.args.empty() ? 12.0f : argf(0);
            if (script.stage == 0)
            {
                script.stage = 1;
                script.mark  = now;
            }
            if (!target)
            {
                scriptGoto.reset();
                script.abort("approach: no target");
            }
            else if (glm::distance(glm::vec2(target->pos.x, target->pos.z), glm::vec2(me.x, me.z)) <= dist)
            {
                scriptGoto.reset();
                spdlog::info("script: within {:.0f} of {} at ({:.1f}, {:.1f})", dist, target->name, me.x, me.z);
                script.next(now);
            }
            else if (now - script.mark > 30s)
            {
                scriptGoto.reset();
                script.abort(fmt::format("approach: {} not reached in 30 s", target->name));
            }
            else
            {
                scriptGoto = glm::vec2(target->pos.x, target->pos.z);
            }
        }
        else if (s.op == "target")
        {
            auto matches = [&](const std::string& name) {
                std::size_t b = 0;
                while (b <= s.rest.size())
                {
                    const auto e = std::min(s.rest.find('|', b), s.rest.size());
                    if (e > b && name.find(s.rest.substr(b, e - b)) != std::string::npos)
                    {
                        return true;
                    }
                    b = e + 1;
                }
                return false;
            };
            if (script.stage == 0)
            {
                args.targetName = s.rest;
                targetId        = 0;
                cycleTarget     = true;
                script.stage    = 1;
                script.mark     = now;
            }
            else if (target && matches(target->name))
            {
                script.next(now);
            }
            else if (now - script.mark > 2s)
            {
                script.stage = 0;
                if (++script.counter > 5)
                {
                    script.abort(fmt::format("no '{}' to target", s.rest));
                }
            }
        }
        else if (s.op == "talk")
        {
            talk = true;
            script.next(now);
        }
        else if (s.op == "localevent")
        {
            // localevent OWNER EVENT [P0..P7] [s:TEXT ...]: the event runs from this zone's event file without the
            // server (Cutscene::startLocal), for events the server does not script; an `event` step then plays it.
            // s:TEXT are its strings ('_' = space), as S2C 0x033's.
            std::array<uint32_t, 8>    params{};
            std::array<std::string, 4> strings{};
            std::size_t                np = 0, ns = 0;
            for (std::size_t i = 2; i < s.args.size(); ++i)
            {
                if (s.args[i].starts_with("s:") && ns < strings.size())
                {
                    strings[ns] = s.args[i].substr(2);
                    std::replace(strings[ns].begin(), strings[ns].end(), '_', ' ');
                    ++ns;
                }
                else if (np < params.size())
                {
                    params[np++] = static_cast<uint32_t>(std::stoul(s.args[i], nullptr, 0));
                }
            }
            if (cutscene && s.args.size() >= 2)
            {
                cutscene->startLocal(world.zone, static_cast<uint32_t>(std::stoul(s.args[0], nullptr, 0)),
                                     static_cast<uint16_t>(std::stoul(s.args[1], nullptr, 0)), params, strings);
            }
            script.next(now);
        }
        else if (s.op == "event")
        {
            // Messages close after 1.5 s; each menu takes the next OPTION (the server's option value; none
            // left = the first row). Done once no event has run for 2 s.
            if (eventBusy())
            {
                script.stage = 1;
                script.mark  = now;
                if (!cutscene->waitingMessage())
                {
                    script.progress = now;
                    script.counter  = 0;
                }
                else if (script.counter == 0 && now - script.progress > 800ms && std::ranges::find(s.args, "shot") != s.args.end())
                {
                    // event ... shot: a screenshot of each message (NAME_evNN.png after the script).
                    script.counter = 1;
                    shotPath       = (scriptDir / fmt::format("{}_ev{:02d}.png", std::filesystem::path(args.script).stem().string(), ++scriptState_.shots)).string();
                    captureNext    = true;
                }
                else if (now - script.progress > 1500ms && args.uiKeys.empty())
                {
                    eventKey        = true;
                    script.progress = now;
                    spdlog::info("script: close the message");
                }
                const auto* opts = gameUi.eventMenu();
                if (!opts)
                {
                    script.menuSeen = {};
                }
                else if (script.menuSeen == app::TestScript::Clock::time_point{})
                {
                    script.menuSeen = now;
                }
                if (opts && script.menuShot != script.menuSeen && now - script.menuSeen > 600ms && std::ranges::find(s.args, "shot") != s.args.end())
                {
                    // event ... shot: each menu too, before its row is picked.
                    script.menuShot = script.menuSeen;
                    shotPath        = (scriptDir / fmt::format("{}_ev{:02d}.png", std::filesystem::path(args.script).stem().string(), ++scriptState_.shots)).string();
                    captureNext     = true;
                }
                if (opts && args.uiKeys.empty() && now - script.menuSeen > 1000ms)
                {
                    int              row = 0;
                    std::vector<int> wanted; // the OPTION values (other words are flags)
                    for (const auto& a : s.args)
                    {
                        if (!a.empty() && (std::isdigit(static_cast<unsigned char>(a[0])) || a[0] == '-'))
                        {
                            wanted.push_back(std::atoi(a.c_str()));
                        }
                    }
                    if (script.waypoint >= wanted.size() && std::ranges::find(s.args, "last") != s.args.end())
                    {
                        row = static_cast<int>(opts->size()) - 1; // `last`: the bottom row (declines: "Maybe later.")
                    }
                    else if (script.waypoint < wanted.size())
                    {
                        const int want = wanted[script.waypoint];
                        for (std::size_t r = 0; r < opts->size(); ++r)
                        {
                            row = (*opts)[r].first == want ? static_cast<int>(r) : row;
                        }
                    }
                    ++script.waypoint;
                    std::string moves; // from the row the menu opened on (op 0x24's default option)
                    for (int r = gameUi.eventMenuRow(); r < row; ++r)
                    {
                        moves += "down,";
                    }
                    for (int r = gameUi.eventMenuRow(); r > row; --r)
                    {
                        moves += "up,";
                    }
                    spdlog::info("script: menu row {} of {}", row, opts->size());
                    sendKeys(moves + "enter");
                    script.menuSeen = now + 10s; // not again until this menu has closed
                }
            }
            else if (script.stage == 1 && now - script.mark > 2s)
            {
                script.next(now);
            }
            else if (script.stage == 0 && t > 15.0f && std::ranges::find(s.args, "ifany") != s.args.end())
            {
                spdlog::info("script: no event to answer");
                script.next(now);
            }
            else if (script.stage == 0 && t > 15.0f)
            {
                script.abort("no event started");
            }
        }
        else if (s.op == "keys")
        {
            if (script.stage == 0)
            {
                sendKeys(s.rest);
                script.stage = 1;
            }
            else if (args.uiKeys.empty() && t > 0.6f)
            {
                script.next(now);
            }
        }
        else if (s.op == "pick")
        {
            // pick ITEMID: in the open item list, move down to that item's row and choose it.
            if (script.stage == 0 && t > 0.8f)
            {
                const int row = gameUi.itemRow(static_cast<uint16_t>(argf(0)));
                if (row < 0)
                {
                    script.abort(fmt::format("item {} not in the open list", s.rest));
                }
                else
                {
                    std::string moves;
                    for (int i = 0; i < row; ++i)
                    {
                        moves += "down,";
                    }
                    sendKeys(moves + "enter");
                    script.stage = 1;
                }
            }
            else if (script.stage == 1 && args.uiKeys.empty() && t > 1.5f)
            {
                script.next(now);
            }
        }
        else if (s.op == "cmd")
        {
            scriptCmd = s.rest;
            script.next(now);
        }
        else if (s.op == "shot")
        {
            shotPath    = (scriptDir / (s.rest + ".png")).string();
            captureNext = true;
            script.next(now);
        }
        else if (s.op == "roam")
        {
            script.roam.clear();
            for (std::size_t k = 0; k + 1 < s.args.size(); k += 2)
            {
                script.roam.emplace_back(argf(k), argf(k + 1));
            }
            script.roamAt = 0;
            script.next(now);
        }
        else if (s.op == "autows")
        {
            args.autoWs = s.args.empty() || s.args[0] != "off";
            script.next(now);
        }
        else if (s.op == "logout")
        {
            quitting = true;
            script.next(now);
        }
        else if (s.op == "heal" || s.op == "fight")
        {
            // fight NAME item ID | level N | kills N: fight NAME monsters (--auto-fight with --chase) until the
            // goal; rest (/heal) below 40% HP between fights. heal: rest until HP and MP are full.
            const bool        fight = s.op == "fight";
            // NAME may hold spaces: the last two words are the goal.
            const std::size_t na    = s.args.size();
            const std::string cond  = fight && na >= 3 ? s.args[na - 2] : "";
            const int         goal  = fight && na >= 3 ? std::atoi(s.args[na - 1].c_str()) : 0;
            std::string       names;
            for (std::size_t k = 0; k + 2 < na; ++k)
            {
                names += (k ? " " : "") + s.args[k];
            }
            if (fight && world.engaged() && target && target->isMob)
            {
                script.foe = target->id;
            }
            if (script.foe != 0)
            {
                const auto it = std::ranges::find_if(world.entities, [&](const client::Entity& e) { return e.id == script.foe; });
                if (it == world.entities.end() || it->hpp == 0)
                {
                    ++script.counter;
                    script.foe = 0;
                    spdlog::info("script: {} defeated ({} so far)", it == world.entities.end() ? std::string("foe") : it->name, script.counter);
                }
            }
            const bool full    = world.hpp >= 100 && (world.mpp >= 100 || world.mp == 0);
            const bool resting = world.animation == 33;
            bool       reached = false;
            if (fight)
            {
                reached = cond == "item" ? invCount(goal) > script.base : cond == "level" ? world.mainLevel >= goal : script.counter >= goal;
            }
            if (script.stage == 0)
            {
                script.base = cond == "item" ? invCount(goal) : 0;
                script.stage = fight ? 1 : 2;
                script.mark  = now;
                script.best  = static_cast<float>(world.hpp);
                if (!fight && !resting && !full)
                {
                    scriptCmd = "/heal";
                }
                else if (!fight)
                {
                    script.stage = 3;
                }
            }
            if (fight && reached && !world.engaged())
            {
                args.autoFight = false;
                args.chase     = false;
                if (resting)
                {
                    scriptCmd = "/heal";
                }
                scriptGoto.reset();
                spdlog::info("script: fight done ({} {}, {} defeated)", cond, goal, script.counter);
                script.next(now);
            }
            else if (script.stage == 1) // fighting
            {
                args.targetName = names;
                args.autoFight  = true;
                args.chase      = true;
                // Nothing to fight in sight for 15 s: walk the roam points in turn.
                if (world.engaged() || (target && target->isMob && target->hpp > 0))
                {
                    script.progress = now;
                    scriptGoto.reset();
                }
                else if (!script.roam.empty() && now - script.progress > 15s)
                {
                    const auto [rx, rz] = script.roam[script.roamAt % script.roam.size()];
                    if (!scriptGoto)
                    {
                        spdlog::info("script: nothing to fight in sight; roam to ({}, {}) from ({:.1f}, {:.1f})", rx, rz, me.x, me.z);
                        script.menuSeen = now; // this leg's start
                    }
                    scriptGoto = glm::vec2(rx, rz);
                    const bool late = now - script.menuSeen > 60s; // blocked on the way: the next point
                    if (late)
                    {
                        spdlog::warn("script: roam point ({}, {}) not reached in 60 s; stuck at ({:.1f}, {:.1f})", rx, rz, me.x, me.z);
                    }
                    if (glm::distance(*scriptGoto, glm::vec2(me.x, me.z)) < 1.0f || late)
                    {
                        ++script.roamAt;
                        script.progress = now - 10s; // look around for 5 s, then on
                        scriptGoto.reset();
                    }
                }
                // Stuck check for roam legs and chases, as goto's: a trunk met head-on gives no slide (the push-out is
                // straight back), so step across the line and try again.
                const glm::vec2          here(me.x, me.z);
                std::optional<glm::vec2> want;
                const bool               chasing = target && target->isMob && target->hpp > 0 &&
                                     glm::distance(glm::vec2(target->pos.x, target->pos.z), here) > 3.0f;
                if (chasing)
                {
                    want = glm::vec2(target->pos.x, target->pos.z);
                }
                else if (scriptGoto && !world.engaged())
                {
                    want = *scriptGoto;
                }
                if (!want)
                {
                    script.stuckPos   = { here.x, here.y };
                    script.stuckSince = now;
                }
                else if (now < script.sideUntil)
                {
                    scriptGoto = glm::vec2(script.side.first, script.side.second);
                }
                else
                {
                    if (chasing)
                    {
                        scriptGoto.reset(); // the chase walks
                    }
                    if (glm::distance(here, glm::vec2(script.stuckPos.first, script.stuckPos.second)) > 0.5f)
                    {
                        script.stuckPos   = { here.x, here.y };
                        script.stuckSince = now;
                    }
                    else if (now - script.stuckSince > 3s)
                    {
                        ++script.stuckTries;
                        const glm::vec2 to   = glm::length(*want - here) > 0.01f ? glm::normalize(*want - here) : glm::vec2(1, 0);
                        const float     sgn  = script.stuckTries % 2 ? 1.0f : -1.0f;
                        const float     off  = std::min(2.0f + static_cast<float>(script.stuckTries), 6.0f);
                        const glm::vec2 side = here + glm::vec2(-to.y, to.x) * (off * sgn);
                        spdlog::warn("script: stuck at ({:.1f}, {:.1f}) {} ({:.1f}, {:.1f}); sidestep {} to ({:.1f}, {:.1f})", here.x, here.y,
                                     chasing ? "chasing" : "on the way to", want->x, want->y, script.stuckTries, side.x, side.y);
                        script.side       = { side.x, side.y };
                        script.sideUntil  = now + 1500ms;
                        script.stuckSince = now + 1500ms;
                        script.stuckPos   = { here.x, here.y };
                        scriptGoto        = side;
                        if (!chasing && script.stuckTries > 3)
                        {
                            spdlog::warn("script: giving up on roam point ({:.1f}, {:.1f})", want->x, want->y);
                            ++script.roamAt;
                            script.progress   = now - 10s;
                            script.menuSeen   = now; // the next leg's start
                            script.stuckTries = 0;
                        }
                    }
                }
                if (!world.engaged() && world.hpp < 40 && !resting && now - script.mark > 3s)
                {
                    args.autoFight = false;
                    args.chase     = false;
                    scriptCmd      = "/heal";
                    script.stage   = 2;
                    script.mark    = now;
                    script.best    = static_cast<float>(world.hpp);
                }
            }
            else if (script.stage == 2) // resting
            {
                scriptGoto.reset();
                args.autoFight = false;
                args.chase     = false;
                const bool hurt = static_cast<float>(world.hpp) < script.best; // HP falling: attacked
                script.best     = static_cast<float>(world.hpp);
                if (hurt && fight)
                {
                    script.stage = 1; // fight back (--auto-fight picks the nearest NAME)
                    script.mark  = now;
                }
                else if (hurt)
                {
                    script.abort("attacked while resting");
                }
                else if (resting && full)
                {
                    scriptCmd    = "/heal";
                    script.stage = 3;
                    script.mark  = now;
                }
                else if (!resting && now - script.mark > 6s)
                {
                    if (full)
                    {
                        script.stage = 3; // the server ended it
                    }
                    else
                    {
                        scriptCmd   = "/heal"; // not resting yet (moving, or the server refused): again
                        script.mark = now;
                    }
                }
            }
            else if (script.stage == 3 && now - script.mark > 2s)
            {
                if (!fight)
                {
                    script.next(now);
                }
                else
                {
                    script.stage = 1;
                    script.mark  = now;
                }
            }
            if (t > 3600.0f)
            {
                args.autoFight = false;
                script.abort("fight: out of time");
            }
        }
        else
        {
            script.abort(fmt::format("unknown step '{}'", s.op));
        }
        if (script.done())
        {
            quitting = true; // finished or aborted: log out
            if (!captureNext)
            {
                shotPath    = (scriptDir / (std::filesystem::path(args.script).stem().string() + "_end.png")).string();
                captureNext = true;
            }
        }
    }
}

} // namespace xi::app
