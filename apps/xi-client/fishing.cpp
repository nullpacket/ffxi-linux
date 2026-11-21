// SPDX-License-Identifier: GPL-3.0-or-later
// Fishing on screen: the race's fishing schedulers run on status changes (ours and other players'), and the
// minigame's arrows and motions GameUi asks for (docs/commands.md, "Fishing").
#include "client_app.h"

namespace xi::app
{

namespace
{
// The scheduler a status starts (CODE 0x1008c9b2, table 0x1032d498 by status): the old fishing (38..43, 50..53) and
// the current one (56 line out, 57 fish on, 58 caught, 59 rod break, 60 line break, 61 monster, 62 stop).
const char* statusScheduler(uint8_t status)
{
    switch (status)
    {
        case 38: case 51: case 57: return "fsh7";
        case 39: case 58: return "fsh2";
        case 40: case 59: return "fsh3";
        case 41: case 60: return "fsh4";
        case 42: case 61: return "fsh5";
        case 43: case 62: return "fsh6";
        case 50: case 56: return "fsh0";
        case 52: return "fsh8";
        case 53: return "fsh9";
        default: return nullptr;
    }
}
} // namespace

bool ClientApp::fishingStatus(uint8_t status)
{
    // The statuses whose standing motion is `fh1` (CODE 0x100c8b90 by the switch at 0x100c8b0e).
    return (status >= 38 && status <= 43) || (status >= 50 && status <= 53) || (status >= 56 && status <= 62);
}

const std::vector<uint32_t>* ClientApp::fishingFilesOf(uint32_t id) const
{
    const auto it = fishState_.files.find(id);
    return it == fishState_.files.end() ? nullptr : &it->second;
}

void ClientApp::updateFishing()
{
    if (!session || !world.inZone)
    {
        fishState_ = {};
        return;
    }
    if (static const bool fake = std::getenv("XI_FAKE_FISH") != nullptr; fake && fishState_.waterFor != session.get())
    {
        // XI_FAKE_FISH (Session::fakeFishing): a cast needs dry footing and water (shallow / deep floor material 8 / 9)
        // within 1.5..8 yalms ahead, as the server's fishing areas only cover shores and piers.
        fishState_.waterFor = session.get();
        session->setFakeFishWater([this] {
            if (!scene)
            {
                return false;
            }
            if (const uint8_t under = scene->materialAt(feet.x, feet.z, feet.y + 0.5f); under == 8 || under == 9)
            {
                return false; // standing in the water
            }
            const float a = static_cast<float>(heading) * 3.14159265f / 128.0f;
            const glm::vec3 g = gfx::ZoneScene::renderToGame(feet);
            for (float d = 1.5f; d <= 8.0f; d += 0.5f)
            {
                const glm::vec3 r = gfx::ZoneScene::gameToRender(glm::vec3(g.x + std::cos(a) * d, g.y, g.z - std::sin(a) * d));
                if (const uint8_t m = scene->materialAt(r.x, r.z, feet.y + 0.5f); m == 8 || m == 9)
                {
                    return true;
                }
            }
            return false;
        });
    }
    auto onStatus = [&](uint32_t id, const client::Look& look, uint8_t status) {
        auto& seen = fishState_.status[id];
        if (seen == status)
        {
            return;
        }
        seen = status;
        if (!fishingStatus(status))
        {
            if (const auto it = fishState_.files.find(id); it != fishState_.files.end())
            {
                const auto rod = fishState_.rods.find(id);
                for (char n = '0'; n <= '9'; ++n)
                {
                    effects.stopFile(it->second[0], dat::fxName(std::string("fsh") + n), id);
                    if (rod != fishState_.rods.end())
                    {
                        effects.stopFile(rod->second, dat::fxName(std::string("fsh") + n), id);
                    }
                }
                fishState_.files.erase(it);
                fishState_.rods.erase(id);
            }
            return;
        }
        auto& files = fishState_.files[id];
        if (files.empty())
        {
            files = chars.fishingFiles(look);
        }
        if (files.empty())
        {
            fishState_.files.erase(id);
            seen = 0xFF; // the look is not loaded yet: try again next frame
            return;
        }
        if (const char* name = statusScheduler(status))
        {
            effects.playFile(files[0], dat::fxName(name), id, id);
            // The rod sub-actor runs its own scheduler of the same name (its file's fsh0..9): the cast, splash and reel
            // sounds (SeSep 8231..8236, 6095) and the rod's motions, which Characters::rodLook poses from the owner's.
            if (const auto rl = Characters::rodLook(look))
            {
                const uint32_t rod = static_cast<uint32_t>(rl->ids[0]) | static_cast<uint32_t>(rl->ids[1]) << 16;
                fishState_.rods[id] = rod;
                effects.playFile(rod, dat::fxName(name), id, id);
            }
        }
    };
    onStatus(world.charId, world.look, world.animation);
    for (const auto& e : world.entities)
    {
        if (e.isPc)
        {
            onStatus(e.id, e.look, e.animation);
        }
    }
    // The minigame's own effects on us (GameUi::fishingTick).
    for (const auto& fx : gameUi.takeFishingFx())
    {
        const auto* own  = fishingFilesOf(world.charId);
        const uint32_t file = fx.file != 0 ? fx.file : own ? (*own)[0] : 0;
        if (file == 0)
        {
            continue;
        }
        // The race file's fsh schedulers run with the rod's of the same name (its reel / strain sounds).
        const auto     rod     = fishState_.rods.find(world.charId);
        const uint32_t rodFile = fx.file == 0 && rod != fishState_.rods.end() ? rod->second : 0u;
        if (fx.stop)
        {
            effects.stopFile(file, fx.name, world.charId);
            if (rodFile)
            {
                effects.stopFile(rodFile, fx.name, world.charId);
            }
        }
        else
        {
            effects.playFile(file, fx.name, world.charId, world.charId);
            if (rodFile)
            {
                effects.playFile(rodFile, fx.name, world.charId, world.charId);
            }
        }
    }
}

} // namespace xi::app
