// SPDX-License-Identifier: GPL-3.0-or-later
// The session's world each frame: the snapshot, events and cutscenes (client::Cutscene), synthesis results.
#include "client_app.h"

namespace xi::app
{

// Other actors on the zone's floor, as the game's actor update stands them (client::actorGround): NPCs, monsters and
// other players, not those with GroundFlag / HackMove (moogles, markers and wells hover), doors, lifts and ships (look
// kinds 2..4, which move on their own), or the Mog House's own actors (fitHeight). A cutscene's actors are placed by
// their event (Cutscene::place) where it moves them. Game space: y down.
void ClientApp::standOnGround(std::vector<client::Entity>& entities)
{
    if (scene.get() != groundedScene)
    {
        groundedAt.clear();
        groundedScene = scene.get();
    }
    if (!scene)
    {
        return;
    }
    for (auto& e : entities)
    {
        if (e.keepsHeight() || (e.look.kind >= 2 && e.look.kind <= 4) || e.fitHeight > 0.0f)
        {
            continue;
        }
        auto it = groundedAt.find(e.id);
        if (it == groundedAt.end() || it->second.first != e.pos)
        {
            const glm::vec3 r = gfx::ZoneScene::gameToRender(e.pos);
            const auto      g = client::actorGround(r.y, [&](float maxY) { return scene->groundHeight(r.x, r.z, maxY); });
            it                = groundedAt.insert_or_assign(e.id, std::pair{ e.pos, g ? -*g : e.pos.y }).first;
        }
        e.pos.y = it->second.second;
    }
}

void ClientApp::updateWorld()
{
    world = session ? session->snapshot() : client::WorldSnapshot{};
    standOnGround(world.entities);
    // Test hook XI_MOG_UNLOCK=1: act as if the Mog House 2F and San d'Oria's exit were unlocked (Remodel, the area list
    // and its Mog Garden row) for a character without those quests; the server still decides.
    static const bool mogUnlock = std::getenv("XI_MOG_UNLOCK") != nullptr;
    if (mogUnlock && world.inMogHouse)
    {
        world.mhExpansion = true;
        world.mhExitBit   = world.mhExitBit ? world.mhExitBit : uint8_t{ 1 };
    }
    // Test hook XI_FAKE_PET=NAME: show a pet (HP 75%, TP 1200) without summoning one, for the pet line's layout.
    if (static const char* fakePet = std::getenv("XI_FAKE_PET"); fakePet && world.inZone && !world.petPresent)
    {
        world.petPresent = true;
        world.petName    = fakePet;
        world.petHpp     = 75;
        world.petTp      = 1200;
    }
    mogView.update(world, session.get());
    mhf = mogView.furniture(world);
    mogView.addTargets(world, scene.get(), mhf);
    uiActive   = !session || world.inZone || world.choosingCharacter; // login form, lobby, or in game
    inGame     = session && world.inZone && !world.choosingCharacter;
    lastTellFrom = world.lastTellFrom;
    curWorld = &world;
    ++frameTick;
    if (targetId == 0)
    {
        flashTarget = 0; // a target picked again flashes again
    }
    if (session && !cutscene)
    {
        client::Cutscene::Hooks h;
        h.ground = [&](const glm::vec3& g) {
            const glm::vec3 r = gfx::ZoneScene::gameToRender(g);
            // The event's sub-map floor where it has one (a room built away from the zone), else the zone's.
            auto y = subScene ? subScene->groundHeight(r.x, r.z, r.y + 2.0f) : std::nullopt;
            if (!y && scene)
            {
                y = scene->groundHeight(r.x, r.z, r.y + 2.0f);
            }
            return y ? -*y : g.y;
        };
        h.playerPos = [&] { return gfx::ZoneScene::renderToGame(feet); };
        h.loaded    = [&](uint32_t id) {
            for (const auto& e : curWorld->entities)
            {
                if (e.id == id)
                {
                    return !app::lookKey(e.look).empty() || !e.look.valid || e.look.kind > 1;
                }
            }
            return false;
        };
        h.motion = [&](uint32_t id, uint32_t aim, uint32_t fcc, int file, int raceSet) {
            // Op 0x66 (event-format §6.3): n -> file P = {0x7FC8, 0xEF39, 0x15711, 0x18F5F}[n / 70] + n % 70
            // (pieces 0 / 1) and, by the body variant v, Q = {0x800E ...}[..] (v 1) or {0x8054 ...} (v >= 2)
            // for piece 2 (CODE 0x100d2150). Op 0x5B: one DAT. The fcc is a scheduler there whose op 0x05
            // motions (`ta1?` ...) come from those files.
            std::vector<uint32_t> files;
            if (raceSet >= 0 && raceSet < 0x118)
            {
                static constexpr uint32_t kP[] = { 0x7FC8, 0xEF39, 0x15711, 0x18F5F }, kQ1[] = { 0x800E, 0xEF7F, 0x15757, 0x18FA5 },
                                          kQ2[] = { 0x8054, 0xEFC5, 0x1579D, 0x18FEB };
                const auto b = static_cast<std::size_t>(raceSet / 70);
                const auto m = static_cast<uint32_t>(raceSet % 70);
                uint8_t    v = 0;
                const client::Look* look = id == curWorld->charId ? &curWorld->look : nullptr;
                for (const auto& e : curWorld->entities)
                {
                    look = e.id == id ? &e.look : look;
                }
                if (const auto* ce = look ? chars.find(cutscene ? cutscene->look(id, *look) : *look) : nullptr)
                {
                    v = ce->bodyVariant;
                }
                files.push_back(kP[b] + m);
                if (v == 1 || v >= 2)
                {
                    files.push_back((v == 1 ? kQ1[b] : kQ2[b]) + m);
                }
            }
            else if (file >= 0)
            {
                files.push_back(static_cast<uint32_t>(file));
            }
            eventMotionFiles[id] = files;
            if (!files.empty() && index.path(files[0]) && effects.playFile(files[0], fcc, id, aim))
            {
                eventMotionSched[id] = { files[0], fcc };
                return;
            }
            client::FxMotion mo;
            mo.actor = id;
            mo.stem  = dat::fxNameString(fcc);
            mo.event = true;
            eventMotions.push_back(std::move(mo));
        };
        h.motionBusy = [&](uint32_t id) {
            if (const auto sc = eventMotionSched.find(id); sc != eventMotionSched.end() && effects.runningFile(sc->second.first, sc->second.second, id))
            {
                return true;
            }
            const auto it = animTracks.find(id == curWorld->charId ? 0xFFFFFFFFu : id);
            if (it != animTracks.end() && it->second.drawnTick + 2 < frameTick)
            {
                // Not posed lately (hidden, or no model): its motion never advances, so op 0x53 / 0x5F 07 would wait
                // for ever (the Windurst intro, event 367, stalled on 0x10f100b). Drop it; INFERRED that the game's
                // wait ends for such an actor.
                it->second.oneShot  = nullptr;
                it->second.fxMotion = nullptr;
                return false;
            }
            const bool busy = it != animTracks.end() && (it->second.oneShot || it->second.fxMotion);
            if (busy && std::getenv("XI_EVENT_TRACE"))
            {
                static auto lastLog = std::chrono::steady_clock::time_point{};
                if (const auto t = std::chrono::steady_clock::now(); t - lastLog > std::chrono::seconds(2))
                {
                    lastLog = t;
                    const auto& tr = it->second;
                    spdlog::info("event: motion busy on {:#x}: oneShot {} ({} frames, frame {:.1f}, speed {:.2f}), fxMotion {}, hold {:.1f}",
                                 id, static_cast<const void*>(tr.oneShot), tr.oneShot ? tr.oneShot->frames() : 0, tr.frame,
                                 tr.oneShot ? tr.oneShot->speed() : 0.0f, static_cast<const void*>(tr.fxMotion), tr.fxHold);
                }
            }
            return busy;
        };
        h.music   = [&](int slot, uint16_t id) { sound.setMusicOverride(slot, id); }; // op 0x5C, until the event ends
        h.sound   = [&](uint32_t id) { sound.playUi(id); };
        h.volume  = [&](float fx, float se, float amb) { sound.setEventLevels(fx, se, amb); }; // ops 0x69 / 0x6A
        h.eventMap       = [&](int zone, int map) { gameUi.eventMap(zone, map); }; // ops 0xC8 / 0x8A
        h.eventMapMarker = [&](int slot, float x, float z, const std::string& label) { gameUi.eventMapMarker(slot, x, z, label); }; // op 0x8B
        h.eventTarget    = [&](const std::string& name, int gauge) { gameUi.eventTarget(name, gauge); };
        h.eventTimer     = [&](int which, int ticks) { gameUi.eventTimer(which, ticks); };
        h.eventTimerLeft = [&](int which) { return gameUi.eventTimerLeft(which); };
        h.mapView        = [&](float x, float y, float zoom, float seconds, int flags) { gameUi.eventMapView(x, y, zoom, seconds, flags); };
        h.subMap         = [&](uint16_t model) {
            // Op 0x75 00: load the sub-map now (the game's thread waits while it loads), replacing any earlier one.
            if (subScene && subScene->model() == model)
            {
                return;
            }
            ctx.waitIdle();
            subScene.reset();
            try
            {
                subScene = std::make_unique<gfx::ZoneScene>(ctx, index, keys, curWorld->zone, model);
                spdlog::info("event: sub-map {} loaded (file {})", model, dat::zoneModelFileId(model));
            }
            catch (const std::exception& ex)
            {
                spdlog::error("event: sub-map {} failed: {}", model, ex.what());
            }
        };
        h.message = [&](uint32_t message, uint32_t speaker) {
            // The speaker's name as shown over their head (the zone's name table fills NPC names here).
            const auto& w = *curWorld;
            std::string from;
            if (speaker == w.charId)
            {
                from = w.charName;
            }
            for (const auto& e : w.entities)
            {
                if (speaker != 0 && e.id == speaker)
                {
                    from = e.name;
                }
            }
            if (const auto zn = zoneNames.find(speaker); from.empty() && zn != zoneNames.end())
            {
                from = zn->second;
            }
            std::array<int32_t, 8> nums{};
            for (std::size_t k = 0; k < 8; ++k)
            {
                nums[k] = static_cast<int32_t>(w.event.params[k]);
            }
            session->eventSay(from, static_cast<uint16_t>(message), nums);
            gameUi.setEventWaiting(true);
        };
        h.menu = [&](uint32_t message, uint32_t hideMask, uint32_t defaultOption) {
            // Op 0x24 (CODE 0x100b2520) into "menu query": each prompt line (before the 0x0B marker) replaces the one
            // before (0x1014de80 clears the window first), then one option per line; hidden ones keep their index. The
            // prompt is the window's own line, not a log line. The cursor starts on option `defaultOption`.
            std::array<int32_t, 8> nums{};
            for (std::size_t k = 0; k < 8; ++k)
            {
                nums[k] = static_cast<int32_t>(curWorld->event.params[k]);
            }
            const auto  lines = session->zoneDialog(static_cast<uint16_t>(message), nums);
            std::string prompt;
            std::vector<std::pair<int, std::string>> options;
            bool inOptions = false;
            int  n         = 0;
            int  cursor    = 0;
            for (const auto& l : lines)
            {
                if (l == "\x0B")
                {
                    inOptions = true;
                }
                else if (!inOptions)
                {
                    prompt = l;
                }
                else
                {
                    if (!(hideMask & (1u << n)))
                    {
                        if (static_cast<uint32_t>(n) == defaultOption)
                        {
                            cursor = static_cast<int>(options.size());
                        }
                        options.emplace_back(n, l);
                    }
                    ++n;
                }
            }
            gameUi.openEventMenu(prompt, std::move(options), cursor);
        };
        cutscene.emplace(*session, effects, index, std::move(h));
    }
    if (cutscene)
    {
        // Test hook XI_EVENT_AUTOCLOSE=SECONDS: a message closes by itself after that long.
        static const float autoClose = std::getenv("XI_EVENT_AUTOCLOSE") ? std::strtof(std::getenv("XI_EVENT_AUTOCLOSE"), nullptr) : -1.0f;
        static float       msgUp     = 0.0f;
        msgUp                        = cutscene->waitingMessage() ? msgUp + dt : 0.0f;
        if (autoClose >= 0.0f && msgUp > autoClose)
        {
            eventKey = true;
        }
        if (eventKey && cutscene->waitingMessage())
        {
            cutscene->closeMessage();
            gameUi.setEventWaiting(false);
        }
        eventKey = false;
        if (cutscene->active())
        {
            if (const auto c = gameUi.takeEventChoice())
            {
                cutscene->choose(*c);
            }
        }
        const bool wasActive = cutscene->active();
        if (!wasActive)
        {
            eventMotionFiles.clear();
            eventMotionSched.clear();
        }
        cutscene->frame(world, dt);
        if (wasActive && !cutscene->active())
        {
            gameUi.clearEventWindows(); // op 0xB4's windows close with the event
            if (subScene) // the event's sub-map goes with it
            {
                ctx.waitIdle();
                subScene.reset();
            }
            sound.clearMusicOverrides();
            gameUi.setEventWaiting(false);
        }
        cutscene->apply(world); // door animations, weather (op 0x77)
    }
    // Synthesis result (S2C 0x06F) in the log. The texts are those of LSB's SynthesisResult notes
    // (atom0s XiPackets); success names the item made.
    static uint32_t synthSeen = 0;
    if (session && world.synthSerial != synthSeen)
    {
        synthSeen = world.synthSerial;
        const auto& r    = world.synth;
        const auto  info = items.find(r.item);
        std::string text;
        switch (r.result)
        {
            case 0x00:
            case 0x0C:
            {
                // The log names ("arrowwood lumber" / "arrowwood lumbers"), as the game's messages use them.
                const std::string what = !info ? fmt::format("item #{}", r.item)
                                       : r.count > 1 && !info->logPlural.empty() ? fmt::format("{} {}", r.count, info->logPlural)
                                       : !info->logName.empty()                ? info->logName
                                                                               : info->name;
                text = fmt::format("Synthesis successful: {}{}.", what, r.grade > 0 ? fmt::format(" (HQ{})", r.grade) : "");
            }
                break;
            case 0x01: text = "Synthesis failed. You lost the crystal you were using."; break;
            case 0x02:
            case 0x0E: text = "Synthesis interrupted. You lost the crystal and materials you were using."; break;
            case 0x03: text = "Synthesis canceled. That combination of materials cannot be synthesized."; break;
            case 0x04: text = "Synthesis canceled."; break;
            case 0x06: text = "Synthesis canceled. That formula is beyond your current craft skill level."; break;
            case 0x07: text = "Synthesis canceled. You cannot hold more than one item of that type."; break;
            case 0x0D: text = "You must wait longer before repeating that action."; break;
            default: text = fmt::format("Synthesis result {}.", r.result); break;
        }
        session->note(text);
    }
}

} // namespace xi::app
