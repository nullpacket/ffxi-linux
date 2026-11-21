// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace xi::app
{

// Test hook --script FILE: a list of steps the client plays through as a player would (walk, target, talk, menus, fight),
// one per line, '#' starts a comment. The steps (run by main.cpp, one at a time):
//   wait SEC                 goto X Z [X Z ...]    (game x / z waypoints, run speed; `xi-dat route` prints them)
//   zone ID                  (wait until in that zone)
//   target NAME              (nearest entity whose name contains NAME, '|' alternatives)
//   talk                     event [OPTION ...]    (play the event out: messages closed, menus answered by option value;
//                                                   flags: shot = a screenshot per message and menu, ifany = none in 15 s is fine,
//                                                   last = menus without an OPTION take their bottom row: `event ifany
//                                                   last` declines the zone-in "Start Seekers of Adoulin?" prompt)
//   keys K1,K2,...           (as --ui-keys)        cmd TEXT              (a chat-line command, "/heal")
//   fight NAME item ID | level N | kills N         heal                  (rest until HP / MP are full)
//   shot NAME                (screenshot NAME.png next to --screenshot, or in the working directory)
//   roam X Z [X Z ...]       (where the next fights walk when nothing to fight is in sight)
//   log TEXT                 logout
//   autows on|off            (whether fights weapon skill at 1000 TP; on by default)
//   pick ITEMID              (in the open item list, move to that item's row and choose it: trading, using items)
//   approach [DIST]          (walk toward the target until within DIST, default 12 = spell range; then stand still)
//   localevent OWNER EVENT [P0..P7] [s:TEXT ...]  (run that block's event of this zone without the server, with those
//                            parameters and strings, '_' = space; then play it with `event`)
// goto logs each waypoint; under half a yalm of progress in 3 s is stuck: three sidesteps, then abort. An event that holds
// a goto for 30 s aborts it (answer zone-in events with an event step). A roam leg not reached in 60 s moves on.
// A step that cannot finish (stuck, not found, knocked out) logs "script: abort" and logs out.
struct ScriptStep
{
    std::string              op;
    std::vector<std::string> args;
    std::string              rest; // the line after the op (for cmd / log)
    int                      line = 0;
};

class TestScript
{
public:
    using Clock = std::chrono::steady_clock;

    bool load(const std::string& path);
    [[nodiscard]] bool loaded() const
    {
        return !steps_.empty();
    }
    [[nodiscard]] bool done() const
    {
        return at_ >= steps_.size();
    }
    [[nodiscard]] const ScriptStep& step() const
    {
        return steps_[at_];
    }
    // Seconds since the current step began.
    [[nodiscard]] float elapsed(Clock::time_point now) const
    {
        return std::chrono::duration<float>(now - began_).count();
    }
    void begin(Clock::time_point now); // the first step starts now
    void next(Clock::time_point now);
    void abort(const std::string& why);

    // Per-step scratch for main.cpp (reset by next()).
    int               stage    = 0;
    std::size_t       waypoint = 0;
    int               counter  = 0;
    int               base     = 0;     // fight: the item count / kills at the start
    unsigned          foe      = 0;     // fight: the monster we are engaged with (counted when it dies)
    unsigned          zone     = 0;     // goto: the zone it started in (a zone line ends the step)
    float             best     = 1e9f;  // goto: closest approach to the waypoint so far
    Clock::time_point progress = {};   // goto: when `best` last improved by a yalm
    Clock::time_point mark     = {};
    Clock::time_point menuSeen = {};    // event: when the current menu opened (zero: none open)
    Clock::time_point menuShot = {};    // event ... shot: the menu already captured (its menuSeen)
    // roam X Z ...: where a later fight walks (in turn) when no monster to fight is in sight for 15 s.
    std::vector<std::pair<float, float>> roam;
    std::size_t                          roamAt = 0;
    // fight: stuck check for roam legs and chases (as goto's): under half a yalm moved in 3 s while walking somewhere
    // -> sidestep across the line for 1.5 s, alternately left and right, farther each time.
    std::pair<float, float> stuckPos{ 1e9f, 1e9f };
    Clock::time_point       stuckSince = {};
    Clock::time_point       sideUntil  = {};
    std::pair<float, float> side{};
    int                     stuckTries = 0;

private:
    std::vector<ScriptStep> steps_;
    std::size_t             at_ = 0;
    Clock::time_point       began_{};
};

} // namespace xi::app
