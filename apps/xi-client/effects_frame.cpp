// SPDX-License-Identifier: GPL-3.0-or-later
// Visual effects each frame: battle actions, doors, lifts, weather particles and sounds.
#include "client_app.h"

namespace xi::app
{

void ClientApp::updateEffects()
{
    // Effects: new battle actions start their schedulers; then advance, and build this frame's geometry.
    if (session && world.inZone)
    {
        const bool settled = zonedAt && std::chrono::steady_clock::now() - *zonedAt > std::chrono::seconds(3);
        actionFx.update(effects, world, settled, [&](uint32_t id) { return poses.find(id) != nullptr; }, shownResults, fxTrace, [&](uint32_t id) {
            const auto* fp = poses.find(id);
            return fp ? fmt::format("{}", fmt::join(fp->files, ",")) : std::string("none");
        });
        // Skeleton attach point (effect-format §C.6): the posed bone's matrix applied to the point's offset.
        auto pointOf = [&](uint32_t id) -> std::function<std::optional<glm::vec3>(uint32_t)> {
            if (!poses.find(id))
            {
                return {};
            }
            return [this, id](uint32_t pt) -> std::optional<glm::vec3> {
                const auto r = poses.point(id, static_cast<int>(pt));
                return r ? std::optional<glm::vec3>(gfx::ZoneScene::renderToGame(*r)) : std::nullopt;
            };
        };
        // The generators' size scaling (effect-format §G.4): the posed model's skeleton bounds and scale.
        auto sizeOf = [this](uint32_t id) {
            const auto* p = poses.find(id);
            return p && p->model ? client::fxActorSize(p->model->skeleton.bounds[0], p->model->scale) : glm::vec2(1.0f);
        };
        const client::FxActorLookup lookup = [&](uint32_t id) -> std::optional<client::FxActor> {
            if (id == world.charId)
            {
                return client::FxActor{ gfx::ZoneScene::renderToGame(feet), -static_cast<float>(heading) * 3.14159265f / 128.0f,
                                        modelHeight.contains(0xFFFFFFFFu) ? modelHeight[0xFFFFFFFFu] : 1.8f, pointOf(id), sizeOf(id) };
            }
            for (const auto& e : world.entities)
            {
                if (e.id == id)
                {
                    return client::FxActor{ e.pos, -static_cast<float>(e.dir) * 3.14159265f / 128.0f,
                                            modelHeight.contains(id) ? modelHeight[id] : 1.8f, pointOf(id), sizeOf(id) };
                }
            }
            return std::nullopt;
        };
        if (!args.fx.empty() && zonedAt)
        {
            static float fxTimer = 3.0f;
            fxTimer -= dt;
            if (fxTimer <= 0)
            {
                fxTimer          = 2.0f;
                const auto colon = args.fx.find(':');
                const bool ok    = effects.play(static_cast<uint32_t>(std::stoul(args.fx.substr(0, colon))), args.fx.substr(colon + 1), world.charId,
                                                target ? target->id : world.charId);
                spdlog::info("test: fx {} -> {}", args.fx, ok);
            }
        }
        const double tod = args.timeOverride >= 0 ? args.timeOverride : eventTimeOfDay();
        effects.setDayFraction(static_cast<float>(tod / 86400.0));
        {
            // Moon phase frame: (day / 7) % 12 over the 84-day cycle, aligned like LSB's moon ((day + 26) % 84;
            // INFERRED alignment).
            const double vanaSec = (static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                             std::chrono::system_clock::now().time_since_epoch()).count()) / 1000.0 - 1009810800.0) * 25.0;
            const auto   day     = static_cast<int64_t>(vanaSec / 86400.0);
            effects.setMoonFrame(static_cast<uint32_t>(((day + 26) % 84) / 7));
            effects.setWeekday(static_cast<uint32_t>(day % 8)); // LSB's weekday (vana_time.h get_weekday)
        }
        // The weather's sky effects (sun, moon, clouds, stars), placed from the camera; the sun and moon
        // travel as the lighting's clock (gfx::applyEnvironment): toward = -travel.
        effects.setWeather(weatherGroup());
        const float     skyA       = static_cast<float>(tod / 86400.0) * 6.283185f;
        const glm::vec3 towardSun  = -glm::vec3(std::sin(skyA + 3.141593f), std::cos(skyA + 3.141593f), 0.0f); // game space
        const glm::vec3 towardMoon = -glm::vec3(std::sin(skyA), std::cos(skyA), 0.0f);
        effects.setSky(gfx::ZoneScene::renderToGame(cam.position), towardSun, towardMoon);
        effects.setViewProj(viewProj * gfx::ZoneScene::gameToRenderMatrix());
        effects.setGroundRay([this](const glm::vec3& from, const glm::vec3& to) -> std::optional<glm::vec3> {
            const glm::vec3 a = gfx::ZoneScene::gameToRender(from), b = gfx::ZoneScene::gameToRender(to);
            const float     len = glm::length(b - a);
            if (!scene || len < 1e-4f)
            {
                return std::nullopt;
            }
            if (const auto t = scene->rayHit(a, (b - a) / len, len))
            {
                return gfx::ZoneScene::renderToGame(a + (b - a) / len * *t);
            }
            return std::nullopt;
        });
        effects.setSightTest([this](const glm::vec3& at) {
            const glm::vec3 to   = gfx::ZoneScene::gameToRender(at) - cam.position;
            const float     dist = glm::length(to);
            return !scene || dist < 0.5f || !scene->rayBlocked(cam.position, to / dist, dist - 0.3f);
        });
        const auto fxT0 = std::chrono::steady_clock::now();
        if (scene)
        {
            doors.update(world, effects, dt, [&](uint32_t d) { return scene->doorParts(d); },
                         [&](uint32_t d, std::size_t part, const glm::vec3& rot, const glm::vec3& off) { scene->setDoorPart(d, part, rot, off); },
                         [&](uint32_t d, bool solid) { scene->setDoorSolid(d, solid); },
                         client::Doors::LiftHooks{ [&](uint32_t l) { return scene->liftHeights(l); },
                                                   [&](uint32_t l, float y) { scene->setLiftY(l, y); } });
        }
        // Weapon effects (config_fx2_re.md §1; CODE 0x100cf030): a weapon model's `!w01` (main) / `!w11` (sub)
        // start its aura when the character engages, `!w02` / `!w12` stop it on disengaging or dying; each once
        // per change. Config Misc. 3 "weapon effects off" (cfg 190 = 1) skips the starting ones.
        {
            // Drawing and sheathing (CODE 0x1008b681 / 0x1008b7b5, names 0x100d2e00 / 0x100d2e20): the status change
            // starts the actor's `in N` on engaging and `out N` on disengaging, N = '0' + the main weapon's sheath style
            // (max(0, actor +0x87c)); in the battle files they play the draw / sheathe motion and op 0x57 `sinr` /
            // `sotr`, the weapon model's draw / sheathe sound. Not on dying (another state). Its motion op 0x05 does not
            // drive the actor here (the animator plays the draw / sheathe motion itself), so only its sound shows.
            auto weapons = [&](uint32_t id, bool engaged, bool alive) {
                const auto* pose = poses.find(id);
                if (!pose)
                {
                    return;
                }
                auto [it, fresh] = weaponFx.try_emplace(id, false);
                if (!fresh && it->second == engaged)
                {
                    return;
                }
                it->second = engaged;
                const char digit = static_cast<char>('0' + std::clamp(pose->sheathStyle, 0, 9));
                if (engaged && !fresh)
                {
                    effects.playFromActor(id, dat::fxName(std::string("in ") + digit), id, id);
                }
                else if (!engaged && !fresh && alive)
                {
                    effects.playFromActor(id, dat::fxName(std::string("out") + digit), id, id);
                }
                if (engaged && cfg(190, 0) != 1)
                {
                    effects.playFromActor(id, dat::fxName("!w01"), id, id);
                    effects.playFromActor(id, dat::fxName("!w11"), id, id);
                }
                else if (!engaged && !fresh)
                {
                    effects.playFromActor(id, dat::fxName("!w02"), id, id);
                    effects.playFromActor(id, dat::fxName("!w12"), id, id);
                }
            };
            weapons(world.charId, world.animation == 1, world.animation != 2 && world.animation != 3);
            for (const auto& e : world.entities)
            {
                if (e.isPc)
                {
                    weapons(e.id, e.animation == 1 && e.hpp > 0, e.hpp > 0);
                }
            }
        }
        effects.setLocalPlayer(world.charId); // op 0x4A
        effects.update(dt, lookup);
        actionFx.tick(dt);
        // Damage pop-ups: each text particle's number in "menu dmgnum" digits (0-9; 10 = "Miss!"), centred on
        // a camera-facing quad of 0.1 x scale per sprite unit (CODE 0x10041a20 / 0x10041b50).
        {
            ui::UiPainter pops(uiR, *ImGui::GetBackgroundDrawList(), canvas.scale);
            for (const auto& t : effects.textParticles())
            {
                const glm::vec3 p  = gfx::ZoneScene::gameToRender(t.pos);
                const glm::vec4 c0 = viewProj * glm::vec4(p, 1.0f);
                const glm::vec4 c1 = viewProj * glm::vec4(p + glm::vec3(0.0f, 1.0f, 0.0f), 1.0f);
                if (c0.w < 0.1f || c1.w < 0.1f)
                {
                    continue;
                }
                const float sx = (c0.x / c0.w * 0.5f + 0.5f) * static_cast<float>(ww);
                const float sy = (c0.y / c0.w * 0.5f + 0.5f) * static_cast<float>(wh);
                const float perWorld = std::abs((c1.y / c1.w - c0.y / c0.w) * 0.5f * static_cast<float>(wh));
                const float perUnit  = 0.1f * t.scale * perWorld; // pixels per sprite unit
                const float perUnitY = 0.1f * t.scaleY * perWorld;
                pops.overrideBlend(t.subtract ? std::optional(ui::Blend::Subtract) : t.additive ? std::optional(ui::Blend::Add) : std::nullopt);
                float       total    = 0;
                for (const char ch : t.text)
                {
                    total += ch == ':' ? 34.0f : 10.0f;
                }
                float          x   = sx - total * 0.5f * perUnit;
                const ui::UiColor col{ t.rgba[0], t.rgba[1], t.rgba[2], t.rgba[3] };
                for (const char ch : t.text)
                {
                    const std::size_t digit = ch == ':' ? 10 : static_cast<std::size_t>(ch - '0');
                    const float       h     = ch == ':' ? 15.0f : 18.0f;
                    if (ch == ':' || (ch >= '0' && ch <= '9'))
                    {
                        pops.sprite("menu dmgnum", digit, ImVec2(x, sy - h * 0.5f * perUnitY), col, 0, perUnit / canvas.scale, perUnitY / canvas.scale);
                    }
                    x += (ch == ':' ? 34.0f : 10.0f) * perUnit;
                }
            }
            pops.overrideBlend(std::nullopt);
        }
        sound.updateMusic(world, client::vanaHourOfDay());
        // Indoors (the roofed bit of the floor under us) the weather's `indo` loop plays.
        sound.updateAmbience(effects.ambienceSep(scene && scene->roofedAt(feet.x, feet.z, feet.y + 0.51f)));
        // <callN> in a party line (CODE 0x101278c0): the first tag plays file 0 Sep 17 + N (N 0..21; also
        // <ncallN> / <scallN>), centred; one call at a time (the call object lasts 120 ticks).
        {
            static auto lastLine = std::chrono::system_clock::time_point{};
            static auto callEnd  = std::chrono::steady_clock::time_point{};
            const auto  newest   = world.chat.empty() ? lastLine : world.chat.back().at;
            for (const auto& l : world.chat)
            {
                if (l.at <= lastLine || l.kind != 4)
                {
                    continue;
                }
                std::string t = l.text;
                std::ranges::transform(t, t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                for (std::size_t at = t.find('<'); at != std::string::npos; at = t.find('<', at + 1))
                {
                    const auto end = t.find('>', at);
                    if (end == std::string::npos)
                    {
                        break;
                    }
                    std::string tag = t.substr(at + 1, end - at - 1);
                    for (const char* pre : { "ncall", "scall", "call" })
                    {
                        if (tag.rfind(pre, 0) == 0)
                        {
                            tag = tag.substr(std::strlen(pre));
                            int n = tag.empty() ? 0 : -1;
                            if (!tag.empty() && tag.size() <= 2 && std::all_of(tag.begin(), tag.end(), [](char c) { return c >= '0' && c <= '9'; }))
                            {
                                n = std::stoi(tag);
                            }
                            if (n >= 0 && n <= 21 && std::chrono::steady_clock::now() >= callEnd)
                            {
                                sound.playUi(17u + static_cast<uint32_t>(n));
                                callEnd = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                            }
                            at = std::string::npos - 1; // first tag only
                            break;
                        }
                    }
                    if (at == std::string::npos - 1)
                    {
                        break;
                    }
                }
            }
            lastLine = std::max(lastLine, newest);
        }
        // Talking to the Moogle (S2C 0x02E) opens the Mog House menu.
        if (static uint32_t mogSeen = world.mogMenuSerial; world.mogMenuSerial != mogSeen)
        {
            mogSeen = world.mogMenuSerial;
            gameUi.openMogHouse();
        }
        // The Mog House's menus (exit door, layout, mannequins) answer here.
        while (const auto c = gameUi.takeAppChoice())
        {
            if (!addonsPage || !addonsPage->onChoice(gameUi, c->first, c->second))
            {
                mogView.onChoice(c->first, c->second, world, gameUi, session.get());
            }
        }
        while (const auto a = gameUi.takeAppAdjust())
        {
            if (addonsPage)
            {
                addonsPage->onAdjust(gameUi, std::get<0>(*a), std::get<1>(*a), std::get<2>(*a));
            }
        }
        if (addonsPage)
        {
            addonsPage->frame(gameUi);
        }
        if (const auto t = mogView.takeTarget())
        {
            targetId = *t; // the gardening cursor's pot
        }
        if (static uint32_t tellsHeard = world.tellsReceived; world.tellsReceived != tellsHeard)
        {
            tellsHeard = world.tellsReceived;
            sound.playUi(40); // a tell arrived (CODE 0x101278c0)
        }
        // Effect sounds (ops 0x0A / 0x0B) at their actor, heard from the camera.
        const audio::SoundSystem::Listener ear{ cam.position, cam.right(), cam.forward() };
        for (const auto& snd : effects.takeSounds())
        {
            const auto who = snd.at ? std::optional<client::FxActor>{} : lookup(snd.actor);
            if (!snd.at && !who)
            {
                continue;
            }
            const auto p = sound.playSep(snd.sep, gfx::ZoneScene::gameToRender(snd.at ? *snd.at : who->pos), snd.near, snd.far, ear, !snd.at, snd.gain,
                                            snd.distanceMode, snd.keepFar,
                                            snd.slot ? 0x8A000000u | snd.slot : 0u);
            if (fxTrace && p)
            {
                spdlog::info("fx: sound {} -> se{:06} vol {:.2f} pan {:.2f}: {}", dat::fxNameString(snd.name), p->id, p->volume, p->pan, p->ok);
            }
        }
        // Slot sounds an effect stops (op 0x8B) and its music fades (op 0xA6).
        for (const auto slot : effects.takeSoundStops())
        {
            sound.stopTagged(0x8A000000u | slot);
        }
        if (const auto fade = effects.takeMusicFade())
        {
            sound.fadeMusic(fade->first, fade->second);
        }
        // Config Misc. damage display (cfg 19: 0 log only, 1 screen and log, 2 screen only): when a result's damage
        // shows, a pop-up from effect file 27 (`damv` / `curv` / `misv`) with the number in the result's log
        // colour (CODE 0x1009e1b3; config_misc_re.md §19).
        auto popNumber = [&](uint64_t tag) {
            if (cfg(19, 0) == 0 || world.actions.empty())
            {
                return;
            }
            const uint64_t serial = tag >> 16;
            const auto     newest = static_cast<uint64_t>(world.actionSerial);
            if (serial > newest || newest - serial >= world.actions.size())
            {
                return;
            }
            const auto& a = world.actions[world.actions.size() - 1 - static_cast<std::size_t>(newest - serial)];
            std::size_t k = (tag & 0xFFFF) - 1;
            for (const auto& t : a.targets)
            {
                for (const auto& r : t.results)
                {
                    if (k-- != 0)
                    {
                        continue;
                    }
                    const auto category = client::damageCategory(r.message);
                    if (category == 0)
                    {
                        return;
                    }
                    const int  colourId = client::battleColour(r.message, t.id == world.charId, a.actor == world.charId);
                    const auto colour   = static_cast<uint32_t>(colourId ? cfg(colourId, ui::GameUi::cfgDefault(colourId)) : 0x808080);
                    if (effects.playNumber(27, category == 1 ? "damv" : category == 2 ? "curv" : "misv", a.actor, t.id, static_cast<int32_t>(r.param), colour) && !firstPopupAt)
                    {
                        firstPopupAt = std::chrono::steady_clock::now();
                    }
                    return;
                }
            }
        };
        for (const auto c : actionFx.takeReleased())
        {
            popNumber(c);
        }
        for (const auto c : effects.takeCommits())
        {
            popNumber(c);
            if (shownResults.size() > 4096)
            {
                shownResults.clear();
            }
            shownResults.insert(c);
            if (fxTrace)
            {
                spdlog::info("fx: result {:x} shown", c);
            }
        }
        auto motions = effects.takeMotions();
        motions.insert(motions.end(), eventMotions.begin(), eventMotions.end());
        eventMotions.clear();
        for (const auto& fm : motions)
        {
            if (fm.hold > 0.0f)
            {
                animTracks[fm.actor == world.charId ? 0xFFFFFFFFu : fm.actor].fxHold = fm.hold; // ops 0x2E / 0x2F
                continue;
            }
            if (fm.flinch)
            {
                // At the effect's contact (melee hit1, a spell's tgt0 ...) rather than when the packet arrived.
                animTracks[fm.actor == world.charId ? 0xFFFFFFFFu : fm.actor].fxFlinch = true;
                continue;
            }
            const auto  evFiles   = eventMotionFiles.find(fm.actor);
            const auto* fishFiles = fishingFilesOf(fm.actor); // a fishing scheduler's motions (fishing.cpp)
            if (!fm.guard && !fm.event && !fm.knockback && evFiles == eventMotionFiles.end() && !fishFiles && !actionFx.motionsAllowed(fm.actor))
            {
                continue;
            }
            const client::Look* look    = fm.actor == world.charId ? &world.look : nullptr;
            bool                engaged = fm.actor == world.charId && world.engaged();
            for (const auto& e : world.entities)
            {
                look    = e.id == fm.actor ? &e.look : look;
                engaged = e.id == fm.actor ? e.animation == 1 : engaged;
            }
            const std::string stem = fm.guard ? (engaged ? "gdm" : "gdi") : fm.stem;
            if (!look)
            {
                continue;
            }
            // Event motions name their DAT first (op 0x5B; dat::EventRunner), then the actor's own files.
            std::vector<uint32_t> extra;
            if (evFiles != eventMotionFiles.end())
            {
                extra = evFiles->second;
            }
            if (fishFiles)
            {
                extra.insert(extra.end(), fishFiles->begin(), fishFiles->end());
            }
            if (fm.file >= 0)
            {
                extra.push_back(static_cast<uint32_t>(fm.file));
            }
            const client::Look drawnLook = cutscene ? cutscene->look(fm.actor, *look) : *look;
            if (const auto* ms = chars.motion(drawnLook, stem, extra))
            {
                animTracks[fm.actor == world.charId ? 0xFFFFFFFFu : fm.actor].fxMotion = ms;
            }
        }
        const glm::vec3 up = glm::normalize(glm::cross(cam.right(), cam.forward()));
        effects.build(fxBatches, gfx::ZoneScene::renderToGame(cam.right()), gfx::ZoneScene::renderToGame(-up));
        // --rt: outdoors the characters cast traced shadows instead of the decals; indoors (light 0's shadow faded) the
        // game's own decals stay.
        if (rt_ && (rt_->options().shadows || pt_) && rtState_.sunShadow > 0.6f)
        {
            shadows.clear();
        }
        else
        {
            shadows.build(scene.get(), cam.position, fxBatches);
        }
        {
            // Lens flares: the game counts visible pixels around the sun (0x1006c1b0); this asks the
            // collision mesh instead (trees and roofs without collision do not hide it), fading 0.25 s.
            static float sunVis = 0.0f, moonVis = 0.0f;
            auto         sight  = [&](float& vis, const glm::vec3& toward) {
                const bool clear = !scene || !scene->rayBlocked(cam.position, gfx::ZoneScene::gameToRender(toward), 600.0f);
                vis              = std::clamp(vis + (clear ? dt : -dt) * 4.0f, 0.0f, 1.0f);
            };
            if (!effects.flares().empty())
            {
                sight(sunVis, towardSun);
                sight(moonVis, towardMoon);
            }
            effects.buildFlares(fxBatches, gfx::ZoneScene::renderToGame(cam.position), gfx::ZoneScene::renderToGame(cam.forward()),
                                gfx::ZoneScene::renderToGame(cam.right()), gfx::ZoneScene::renderToGame(-up), std::tan(cam.fovY * 0.5f), aspect,
                                sunVis, moonVis);
        }
        if (static const bool perf = std::getenv("XI_PERF") != nullptr; perf)
        {
            // XI_PERF=1: average CPU time of the effect update + build, every 5 s.
            static double   acc = 0;
            static int      n   = 0;
            static auto     at  = std::chrono::steady_clock::now();
            acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fxT0).count();
            ++n;
            if (std::chrono::steady_clock::now() - at > std::chrono::seconds(5))
            {
                std::size_t verts = 0;
                for (const auto& fb : fxBatches)
                {
                    verts += fb.vertices.size();
                }
                spdlog::info("perf: effects {:.2f} ms/frame ({} particles, {} vertices, {:.0f} fps)", acc / n, effects.particleCount(), verts, fps);
                acc = 0, n = 0, at = std::chrono::steady_clock::now();
            }
        }
        effectRenderer.prepare(fxBatches);
    }
    else
    {
        fxBatches.clear();
    }
    // Event fades (scheduler op 0x0F `fdo1` / `fdi1`): over the scene and the names, under the windows.
    if (const glm::vec4 sc = effects.screenColour(); sc.a > 0.002f)
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), size,
                                                      ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(sc.r, 1.0f), std::min(sc.g, 1.0f), std::min(sc.b, 1.0f), std::min(sc.a, 1.0f))));
    }
    // The zone change fade (zone_frame.cpp): black over the scene and the names, under the windows.
    if (zoneState_.fade > 0.002f)
    {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize,
                                                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, std::min(zoneState_.fade, 1.0f))));
    }
    ImGui::Render();
    frameTrace.mark(gfx::FrameTrace::Effects);
}

} // namespace xi::app
