// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_script.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace xi::app
{

bool TestScript::load(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        spdlog::error("script: cannot open {}", path);
        return false;
    }
    std::string line;
    int         n = 0;
    while (std::getline(in, line))
    {
        ++n;
        if (const auto hash = line.find('#'); hash != std::string::npos)
        {
            line.erase(hash);
        }
        std::istringstream ss(line);
        ScriptStep         s;
        if (!(ss >> s.op))
        {
            continue;
        }
        s.line = n;
        std::getline(ss >> std::ws, s.rest);
        while (!s.rest.empty() && (s.rest.back() == ' ' || s.rest.back() == '\r'))
        {
            s.rest.pop_back();
        }
        std::istringstream as(s.rest);
        for (std::string a; as >> a;)
        {
            s.args.push_back(a);
        }
        steps_.push_back(std::move(s));
    }
    spdlog::info("script: {} steps from {}", steps_.size(), path);
    return !steps_.empty();
}

void TestScript::begin(Clock::time_point now)
{
    began_ = now;
    if (!done())
    {
        spdlog::info("script: line {}: {} {}", step().line, step().op, step().rest);
    }
}

void TestScript::next(Clock::time_point now)
{
    ++at_;
    stage    = 0;
    waypoint = 0;
    counter  = 0;
    base     = 0;
    foe      = 0;
    zone     = 0;
    best     = 1e9f;
    progress = now;
    mark     = now;
    menuSeen = {};
    stuckPos   = { 1e9f, 1e9f };
    stuckSince = now;
    sideUntil  = {};
    stuckTries = 0;
    begin(now);
    if (done())
    {
        spdlog::info("script: done");
    }
}

void TestScript::abort(const std::string& why)
{
    spdlog::error("script: abort at line {} ({}): {}", done() ? 0 : step().line, done() ? "" : step().op, why);
    at_ = steps_.size();
}

} // namespace xi::app
