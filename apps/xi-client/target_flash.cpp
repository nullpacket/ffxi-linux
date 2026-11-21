// SPDX-License-Identifier: GPL-3.0-or-later
// The Config "target visual effect" pulse (cfg 153 bit 7) and the AoE range graphics (cfg 192) that reuse it.
#include "client_app.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace xi::app
{

namespace
{
std::string lowered(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

// The spell a sub-target line casts: `/ma "Stonega" <st>` (or /magic, /ninjutsu, /nin, /song, /so; quotes optional).
std::string spellOfLine(std::string_view line)
{
    const auto sp = line.find(' ');
    if (sp == std::string_view::npos)
    {
        return {};
    }
    const std::string cmd = lowered(line.substr(0, sp));
    if (cmd != "/ma" && cmd != "/magic" && cmd != "/ninjutsu" && cmd != "/nin" && cmd != "/song" && cmd != "/so")
    {
        return {};
    }
    std::string_view rest = line.substr(sp + 1);
    rest                  = rest.substr(0, rest.find('<'));
    std::string name;
    for (const char c : rest)
    {
        if (c != '"')
        {
            name += c;
        }
    }
    while (!name.empty() && name.back() == ' ')
    {
        name.pop_back();
    }
    return lowered(name.substr(std::min(name.find_first_not_of(' '), name.size())));
}
} // namespace

float ClientApp::targetFlash(uint32_t id)
{
    // CODE 0x100cb8a8: three 40-tick pulses, t = a 0..1..0 triangle; a new target (or an actor newly inside the area)
    // starts them again.
    if (((cfg(153, 0) >> 7) & 1) != 0)
    {
        return 0.0f;
    }
    const auto pulse = [&](std::chrono::steady_clock::time_point at) {
        const float ticks = std::chrono::duration<float>(now - at).count() * 60.0f;
        if (ticks >= 120.0f)
        {
            return 0.0f;
        }
        const float k = std::fmod(ticks, 40.0f);
        return (k >= 20.0f ? 40.0f - k : k) * 0.05f;
    };
    // AoE range graphics (CODE 0x101582b5 -> 0x1009ad10, 0x1008f290): while a sub-target cursor is up for an area spell
    // and cfg 192 = 1, every actor within its radius of the cursor's actor flashes too. Once per frame.
    if (flashState_.aoeFlashTick != frameTick)
    {
        flashState_.aoeFlashTick = frameTick;
        const uint32_t centreId = gameUi.subTarget();
        const auto*    spell    = [&]() -> const dat::SpellInfo* {
            if (cfg(192, 0) != 1 || centreId == 0 || !battleText || !spellData)
            {
                return nullptr;
            }
            // By name over the whole spell table (the range shows whether or not the spell is learned); the last
            // line's answer is kept.
            static std::string             lastName;
            static const dat::SpellInfo*   lastSpell = nullptr;
            const std::string              name      = spellOfLine(gameUi.subTargetLine());
            if (name != lastName)
            {
                lastName  = name;
                lastSpell = nullptr;
                for (uint16_t sid = 1; sid < 1024 && !name.empty() && !lastSpell; ++sid)
                {
                    if (lowered(battleText->spellName(sid)) == name)
                    {
                        lastSpell = spellData->find(sid);
                    }
                }
            }
            return lastSpell;
        }();
        const auto posOf = [&](uint32_t eid) -> std::optional<glm::vec3> {
            if (eid == world.charId)
            {
                return feet;
            }
            const auto e = std::ranges::find(world.entities, eid, &client::Entity::id);
            return e != world.entities.end() ? std::optional(gfx::ZoneScene::gameToRender(e->pos)) : std::nullopt;
        };
        const auto centre = spell && spell->aoeRadius ? posOf(centreId) : std::nullopt;
        std::vector<uint32_t> inside;
        if (centre)
        {
            const float r = spell->aoeRadius;
            if (glm::distance(feet, *centre) <= r)
            {
                inside.push_back(world.charId);
            }
            for (const auto& e : world.entities)
            {
                if (e.id != world.charId && glm::distance(gfx::ZoneScene::gameToRender(e.pos), *centre) <= r)
                {
                    inside.push_back(e.id);
                }
            }
        }
        // XI_AOE_TRACE=1 logs who is inside whenever that changes.
        static const bool trace = std::getenv("XI_AOE_TRACE") != nullptr;
        if (trace && inside.size() != flashState_.aoeFlash.size())
        {
            spdlog::info("aoe: {} actors within {} of {:x}", inside.size(), spell ? spell->aoeRadius : 0, centreId);
        }
        std::erase_if(flashState_.aoeFlash, [&](const auto& kv) { return std::ranges::find(inside, kv.first) == inside.end(); });
        for (const auto a : inside)
        {
            flashState_.aoeFlash.try_emplace(a, now);
        }
    }
    float t = 0.0f;
    if (id == targetId)
    {
        if (flashTarget != id)
        {
            flashTarget = id;
            flashState_.flashAt     = now;
        }
        t = pulse(flashState_.flashAt);
    }
    if (const auto it = flashState_.aoeFlash.find(id); it != flashState_.aoeFlash.end())
    {
        t = std::max(t, pulse(it->second));
    }
    return t;
}

} // namespace xi::app
