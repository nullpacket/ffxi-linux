// SPDX-License-Identifier: GPL-3.0-or-later
// Character drawing: models, motions, mounts, corpses, look-at, footsteps, name labels, shadows.
#include "client_app.h"

namespace xi::app
{

// The scene wind on a soft body at render-space feet `at`: none on a roofed floor (CODE 0x100a5303).
float ClientApp::clothWindAt(const glm::vec3& at) const
{
    return scene && scene->roofedAt(at.x, at.z, at.y + 0.51f) ? 0.0f : drawState_.clothWind.strength;
}

void ClientApp::drawActors()
{
    gfx::stepClothWind(drawState_.clothWind, dt); // once a frame, for every actor's soft bodies
    // Markers + name labels
    mk.clear();

    // Character models: load unseen appearances (at most two per frame) and pose everyone in bind pose.
    charInstances.clear();
    drawState_.charIndex.clear();
    chars.newFrame();
    modelHeight.clear(); // entity id -> label height
    poses.newFrame();
    shadows.clear();
    // Head look-at target (render space): an event's (ops 0x79 / 0x7B), else the head (point 3) of what the
    // actor faces (0x00D / 0x00E facetarget; ours: our current target) while standing or sitting (CODE
    // 0x100d5a40); nullopt = look ahead.
    auto lookTarget = [&](uint32_t id, const glm::vec3& at, uint8_t dir, uint16_t faceIndex, uint8_t animation) -> std::optional<glm::vec3> {
        const auto* pu = cutscene && cutscene->active() ? cutscene->puppet(id) : nullptr;
        auto pointOn = [&](uint32_t who, int pt) { return poses.lastPoint(who, pt); };
        if (pu && pu->lookMode == 1)
        {
            return pointOn(pu->lookTarget, pu->lookPoint);
        }
        if (pu && pu->lookMode == 2)
        {
            // 10 units out from the feet, turned by the yaw offset and pitched (INFERRED: positive = up).
            const float     yaw = static_cast<float>(dir) * 3.14159265f / 128.0f + pu->lookYaw;
            const glm::vec3 g   = gfx::ZoneScene::renderToGame(at) +
                                10.0f * glm::vec3(std::cos(pu->lookPitch) * std::cos(yaw), -std::sin(pu->lookPitch), -std::cos(pu->lookPitch) * std::sin(yaw));
            return gfx::ZoneScene::gameToRender(g);
        }
        const bool calm = animation == 0 || animation == 47 || animation == 48 || (animation >= 63 && animation <= 83);
        if (!calm || (cutscene && cutscene->active()) || effects.lookAtSuppressed(id)) // effect op 0x89: ranged shots
        {
            return std::nullopt;
        }
        if (id == world.charId)
        {
            return targetId != 0 && targetId != id ? pointOn(targetId, 3) : std::nullopt;
        }
        if (faceIndex == 0)
        {
            return std::nullopt;
        }
        if (faceIndex == world.actIndex)
        {
            return pointOn(world.charId, 3);
        }
        for (const auto& o : world.entities)
        {
            if (o.index == faceIndex && o.id != id)
            {
                return pointOn(o.id, 3);
            }
        }
        return std::nullopt;
    };
    auto applyLook = [&](uint32_t id, const gfx::CharacterModel& m, std::vector<glm::mat4>& bones, const glm::vec3& at, uint8_t dir,
                         uint16_t faceIndex, uint8_t animation) {
        const bool  seated = animation == 5 || animation == 85 || animation == 47 || animation == 48 || (animation >= 63 && animation <= 83);
        poses.look(id, m, bones, at, dir, lookTarget(id, at, dir, faceIndex, animation), seated, dt);
    };
    // An event's scale (op 0xB6 0F / 17), about the feet.
    auto applyScale = [&](uint32_t id, std::vector<glm::mat4>& bones, const glm::vec3& at) {
        const float k = cutscene ? cutscene->scaleOf(id) : 1.0f;
        if (k != 1.0f && k > 0.0f)
        {
            app::scaleAbout(bones, at, k);
        }
    };
    // Footsteps (audio::Footsteps) for an actor posed this frame.
    auto footsteps = [&](uint32_t id, const client::Look& look, const std::vector<glm::mat4>& bones, const glm::vec3& at, bool moving,
                         bool walkPace) {
        const auto* ce = chars.find(look);
        const auto* fp = poses.find(id);
        if (!scene || !ce || !fp || !fp->model)
        {
            return;
        }
        const char*            wf = dat::weatherFourcc(args.weatherOverride >= 0 ? static_cast<uint16_t>(args.weatherOverride) : world.weather);
        const std::string_view w  = wf ? wf : "";
        const auto step = steps.update(id, ce->feet, fp->model->skeleton.points, bones, at, moving, walkPace, dt,
                                       [&] { return scene->materialAt(at.x, at.z, at.y + 0.5f); }, w == "rain" || w == "squl" || w == "bolt",
                                       effects, sound, { cam.position, cam.right(), cam.forward() });
        if (fxTrace && step)
        {
            spdlog::info("fx: step {:x} {} mat {} -> se{:06} vol {:.2f}", id, step->name, step->material, step->played.id, step->played.volume);
        }
        // Config Misc. 2 footstep effects (cfg 63 = 0, the default): dust, splashes and footprints per step.
        if (step && cfg(63, 0) == 0)
        {
            effects.footstepEffect(id, step->material, walkPace, w == "rain" || w == "squl" || w == "bolt");
        }
        if (cfg(63, 0) == 0) // ... and the deep-water wake, every frame (not only per step)
        {
            effects.wadeEffect(id, scene->materialAt(at.x, at.z, at.y + 0.5f) == 9, w == "rain" || w == "squl" || w == "bolt");
        }
    };
    // Was the last hit on this actor from behind? Facing (cos a, 0, sin a) with a = -dir * pi / 128.
    auto hitFromBehind = [&](const glm::vec3& pos, uint8_t dir, uint32_t attacker) {
        glm::vec3 from(0.0f);
        bool      found = false;
        if (attacker == world.charId)
        {
            from  = gfx::ZoneScene::renderToGame(feet);
            found = true;
        }
        for (const auto& e : world.entities)
        {
            if (e.id == attacker)
            {
                from  = e.pos;
                found = true;
            }
        }
        const float a = -static_cast<float>(dir) * 3.14159265f / 128.0f;
        return found && (std::cos(a) * (from.x - pos.x) + std::sin(a) * (from.z - pos.z)) < 0.0f;
    };
    aspect = gfx::gameAspect(static_cast<float>(ctx.extent().width) / static_cast<float>(std::max(1u, ctx.extent().height)));
    glm::mat4 viewMat = cam.view();
    if (const float shake = std::min(effects.cameraShake(), 0.5f); shake > 0.0f)
    {
        // Effect camera shake (L2 0x5f with rec+0xC; amplitude scale INFERRED).
        static std::mt19937                   shakeRng{ 7 };
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        viewMat = glm::translate(glm::mat4(1.0f), glm::vec3(u(shakeRng), u(shakeRng), 0.0f) * shake) * viewMat;
    }
    viewProj = cam.projection(aspect) * viewMat;
    if (session && world.inZone)
    {
        // An event's idle motion (op 0x5E; e.g. `idl0` back, or a kneel / salute held), from the actor's files.
        auto eventIdle = [&](uint32_t id, const client::Look& look) -> const dat::MotionSet* {
            const auto* pu = cutscene && cutscene->active() ? cutscene->puppet(id) : nullptr;
            if (!pu || pu->idle == 0 || pu->idle == dat::fxName("idl0"))
            {
                return nullptr;
            }
            return chars.motion(look, dat::fxNameString(pu->idle));
        };
        // Riding (status 5 chocobo, 85 mount; chocobo_re.md §3): the chocobo is its own actor at the rider's position and
        // facing, with no offset; the rider plays chi / wlk / run from the race's riding files, in step with it.
        auto drawMount = [&](uint32_t key, const client::Look& rider, uint8_t animation, uint8_t chocoboIndex,
                             const std::array<uint32_t, 2>& looks, uint8_t mountIndex, const glm::vec3& at, float dir, float speed,
                             gfx::AnimInput& riderIn) -> bool {
            if (animation != 5 && animation != 85)
            {
                // Just got down (scheduler `coff`, chocobo_re.md §3e): the rider plays ch1 then ch2; the
                // chocobo plays ch1, then runs off (drawn below until it is gone).
                if (const auto was = drawState_.riding.find(key); was != drawState_.riding.end() && was->second)
                {
                    if (const auto lr = drawState_.lastRide.find(key); lr != drawState_.lastRide.end())
                    {
                        drawState_.dismounts[key] = { lr->second.second, at, static_cast<uint8_t>(static_cast<int>(std::lround(dir)) & 0xFF), 0.0f };
                        if (const auto* rd = chars.ride(rider, world.zone, lr->second.first == 85); rd && !rd->dismount1.empty())
                        {
                            auto& tr   = animTracks[key];
                            tr.oneShot = &rd->dismount1;
                            tr.chained = rd->dismount2.empty() ? nullptr : &rd->dismount2;
                        }
                        drawState_.mountTracks[key].oneShot = chars.motion(lr->second.second, "ch1");
                        drawState_.mountTracks[key].chained = nullptr;
                    }
                }
                drawState_.riding[key] = false;
                return false;
            }
            const auto mountLook = app::Characters::mountLook(animation, chocoboIndex, looks, mountIndex, world.zone);
            drawState_.lastRide[key]        = { animation, mountLook };
            drawState_.dismounts.erase(key);
            const auto* mm       = chars.model(mountLook);
            if (!mm)
            {
                return false;
            }
            // Just mounted (seen on foot before): both play `chh` once (scheduler `chor`).
            const auto was = drawState_.riding.find(key);
            const bool hop = was != drawState_.riding.end() && !was->second;
            drawState_.riding[key]    = true;
            if (const auto* rd = chars.ride(rider, world.zone, animation == 85))
            {
                riderIn.rideIdle = &rd->idle;
                riderIn.rideWalk = &rd->walk;
                riderIn.rideRun  = &rd->run;
                if (hop && !rd->mount.empty())
                {
                    animTracks[key].fxMotion = &rd->mount;
                }
            }
            if (hop)
            {
                drawState_.mountTracks[key].fxMotion = chars.motion(mountLook, "chh");
            }
            if (key == 0xFFFFFFFFu)
            {
                // The camera's look-at rides higher by the mount's own height byte (dat::mountCameraHeight; movement.cpp).
                static std::unordered_map<uint32_t, float> mountHeights; // by mount file
                const uint32_t file = mountLook.kind == kLookFile ? static_cast<uint32_t>(mountLook.ids[0]) | static_cast<uint32_t>(mountLook.ids[1]) << 16 : 0u;
                auto           mh   = mountHeights.find(file);
                if (mh == mountHeights.end())
                {
                    mh = mountHeights.emplace(file, file ? dat::mountCameraHeight(index, file) : 0.0f).first;
                }
                drawState_.ownMountHeight = mh->second;
            }
            gfx::SkinnedRenderer::Instance mi{ mm->model, {} };
            gfx::AnimInput                 in{};
            in.speed = speed;
            gfx::characterPalette(*mm, gfx::ZoneScene::renderToGame(at), dir, mi.bones, animate(drawState_.mountTracks[key], *mm, in, dt));
            mi.origin = at;
            shadows.cast(at, *mm, 1.0f, charInstances.size());
            charInstances.push_back(std::move(mi));
            return true;
        };
        // The fishing rod (Characters::rodLook): a sub-actor of its own while the owner is in a fishing status, at the
        // owner's position and facing. Its motions have the owner's names (fh40 rod / fh41 body ...) and are authored
        // in the owner's model space, so it plays the same stem at the same frame (and cross-fade) as the owner.
        auto drawRod = [&](const client::Look& owner, uint8_t status, const gfx::AnimState& body, const glm::vec3& at, float dir) {
            if (!fishingStatus(status))
            {
                return;
            }
            const auto rl = Characters::rodLook(owner);
            const auto* rm = rl ? chars.model(*rl) : nullptr;
            if (!rm)
            {
                return;
            }
            auto rodSet = [&](const dat::MotionSet* s) -> const dat::MotionSet* {
                const auto stem = chars.stemOf(s);
                const auto* m   = stem.empty() ? nullptr : chars.motion(*rl, stem);
                return m ? m : chars.motion(*rl, "idl");
            };
            gfx::AnimState ra{ rodSet(body.set), body.frame, body.from ? rodSet(body.from) : nullptr, body.fromFrame, body.blend };
            gfx::SkinnedRenderer::Instance ri{ rm->model, {} };
            gfx::characterPalette(*rm, gfx::ZoneScene::renderToGame(at), dir, ri.bones, ra);
            ri.origin = at;
            charInstances.push_back(std::move(ri));
        };
        // Mounts leaving after a dismount, paced by the mount's own `coff` scheduler (dat::leavePlan; CODE op 0x6E /
        // 0x29): a chocobo finishes ch1 where the rider got down, then runs off along its facing and fades out; a mount
        // has no ch1 and runs at once. The game's actor is removed when its scheduler ends.
        static std::unordered_map<std::string, dat::LeavePlan> leavePlans;
        auto planOf = [&](const client::Look& look) -> const dat::LeavePlan& {
            const auto key = app::lookKey(look);
            if (const auto it = leavePlans.find(key); it != leavePlans.end())
            {
                return it->second;
            }
            dat::LeavePlan plan{ true, 1.67f, 1.7f, 1.2f, 12.0f, 2.5f, 0.4f }; // 39275's, when no file has `coff`
            if (const auto* ce = chars.find(look))
            {
                for (const auto& files : { ce->motionFiles, ce->battleMotionFiles })
                {
                    for (const auto f : files)
                    {
                        try
                        {
                            if (const auto p = dat::leavePlan(dat::EffectFile(index, f)))
                            {
                                return leavePlans[key] = *p;
                            }
                        }
                        catch (const std::exception&)
                        {
                        }
                    }
                }
            }
            return leavePlans[key] = plan;
        };
        for (auto it = drawState_.dismounts.begin(); it != drawState_.dismounts.end();)
        {
            auto&       d    = it->second;
            const auto* mm   = chars.model(d.mount);
            const auto& plan = planOf(d.mount);
            d.t += dt;
            if (!mm || d.t > plan.end())
            {
                drawState_.mountTracks.erase(it->first);
                it = drawState_.dismounts.erase(it);
                continue;
            }
            gfx::AnimInput in{};
            if (d.t >= plan.runAt)
            {
                in.speed = plan.speed > 0.0f ? plan.speed : 8.0f; // run
            }
            if (d.t >= plan.moveAt && d.t < plan.moveAt + plan.moveFor)
            {
                const float ang = -static_cast<float>(d.dir) * 3.14159265f / 128.0f;
                d.at            = scene ? scene->move(d.at, glm::vec3(std::cos(ang), 0.0f, -std::sin(ang)) * plan.speed * dt) : d.at;
            }
            gfx::SkinnedRenderer::Instance mi{ mm->model, {} };
            mi.fade = plan.fadeFor > 0.0f ? std::clamp((d.t - plan.fadeAt) / plan.fadeFor, 0.0f, 1.0f) : 0.0f;
            gfx::characterPalette(*mm, gfx::ZoneScene::renderToGame(d.at), d.dir, mi.bones, animate(drawState_.mountTracks[it->first], *mm, in, dt));
            mi.origin = d.at;
            shadows.cast(d.at, *mm, 1.0f - mi.fade, charInstances.size());
            charInstances.push_back(std::move(mi));
            ++it;
        }
        client::Look ownLook = cutscene ? cutscene->look(world.charId, world.look) : world.look; // op 0xB6 costume
        // Riding (status 5 / 85): the main and sub weapons stay, the ranged weapon is hidden (CODE 0x1008c271 /
        // 0x1008c7a5: show all slots, then hide slot 2).
        auto riderLook = [](client::Look look, uint8_t animation) {
            if ((animation == 5 || animation == 85) && look.kind == 0xFFFF)
            {
                look.ids[8] = static_cast<uint16_t>(look.ids[8] & 0xF000);
            }
            return look;
        };
        ownLook = riderLook(ownLook, world.animation);
        const auto* ownModel = chars.model(ownLook);
        if (ownModel)
        {
            gfx::SkinnedRenderer::Instance inst{ ownModel->model, {} };
            inst.tint = effects.actorTint(world.charId);
            if (const float t = targetFlash(world.charId); t > 0.0f) // target visual effect / AoE range graphics
            {
                inst.tint = glm::vec4(glm::vec3(inst.tint) * (1.0f + 0.86f * t), inst.tint.a);
            }
            // The camera pulled in to our own head (a wall or pillar at our back, ZoneCollision::cameraDistance):
            // the model fades out between 1.6 and 0.6 from the view target (a Galka fills the view at 1.3) (INFERRED: ours; no such fade found in the
            // game, which has no pull-in either).
            if (!(cutscene && cutscene->cameraMode()))
            {
                const float d = glm::length(cam.position - (drawFeet + glm::vec3(0.0f, 1.5f, 0.0f)));
                inst.tint.a *= std::clamp((d - 0.6f) / 1.0f, 0.0f, 1.0f);
            }
            if (const auto ov = effects.actorOverlay(world.charId)) // effect ops 0x40 / 0x41 / 0x54 / 0x55
            {
                inst.overlay = ov->texture, inst.overlayScroll = ov->scroll, inst.overlayAdditive = ov->additive, inst.overlayTint = ov->colour;
            }
            inst.refraction = effects.actorShimmer(world.charId); // effect ops 0x48 / 0x49
            gfx::AnimInput ownIn{ ownMovingThisFrame ? (ownEventSpeed > 0.0f ? ownEventSpeed : 5.0f) : 0.0f, world.engaged(), world.animation == 2 || world.animation == 3, world.swings,
                                  castSchool(world.castSpell), world.castsDone, world.animation == 33, world.weaponSkillsDone,
                                  chars.weaponSkill(ownLook, world.wsAnimation), world.emotes,
                                  chars.emote(ownLook, world.emote), ownMoveDir,
                                  world.animation == 47 || (world.animation >= 63 && world.animation <= 83),
                                  hitFromBehind(gfx::ZoneScene::renderToGame(feet), heading, world.lastHitBy), eventIdle(world.charId, ownLook) };
            static const bool animTrace = std::getenv("XI_ANIM_TRACE") != nullptr;
            ownIn.trace = animTrace;
            if (const auto* ff = fishingFilesOf(world.charId); ff && !ownIn.idle && fishingStatus(world.animation))
            {
                ownIn.idle = chars.motion(ownLook, "fh1", *ff); // fishing statuses stand in `fh1` (CODE 0x100c8ba8)
            }
            // The drawn heading turns toward the movement's exact direction (else the heading byte) instead of snapping.
            const float ownTarget = headingExact >= 0.0f && headingByte(std::cos(-headingExact * 3.14159265f / 128.0f), std::sin(-headingExact * 3.14159265f / 128.0f)) == heading
                                        ? headingExact
                                        : static_cast<float>(heading);
            drawState_.ownShownDir           = approachHeading(drawState_.ownShownDir, ownTarget, dt);
            const bool ownRiding = !firstPerson && !selfHidden &&
                                   drawMount(0xFFFFFFFFu, ownLook, world.animation, world.chocoboIndex, world.chocoboLook, world.mountIndex, drawFeet, drawState_.ownShownDir, ownIn.speed, ownIn);
            animTracks[0xFFFFFFFFu].drawnTick = frameTick;
            const auto anim = animate(animTracks[0xFFFFFFFFu], *ownModel, ownIn, dt);
            gfx::characterPalette(*ownModel, gfx::ZoneScene::renderToGame(drawFeet), drawState_.ownShownDir, inst.bones, anim);
            inst.origin = drawFeet;
            {
                const auto& ot = animTracks[0xFFFFFFFFu];
                effects.stancePick(world.charId, ot.oneShot || ot.fxMotion || ownIn.castSchool >= 0); // weapons back after an action
                inst.hiddenTags = effects.hiddenParts(world.charId);
            }
            if (gfx::weaponDrawn(animTracks[0xFFFFFFFFu], ownModel->motions) || effects.weaponHeld(world.charId, 0))
            {
                gfx::holdWeapon(*ownModel, inst.bones);
            }
            applyLook(world.charId, *ownModel, inst.bones, drawFeet, heading, 0, world.animation);
            applyScale(world.charId, inst.bones, drawFeet);
            gfx::simulateCloth(ownModel->cloth, animTracks[0xFFFFFFFFu].cloth, inst.bones, drawFeet, dt, clothWindAt(drawFeet));
            poses.keep(world.charId, ownLook, ownModel, inst.bones);
            // The camera's model height (client::GameCamera, CODE 0x1001f2c2).
            // Mounted the game adds the mount's height byte / 100 (CODE 0x100222e0: mount +0x89d, from its `info` chunk
            // byte 14, 0x100cf281; none on chocobos and most mounts).
            {
                // The skeleton's bounds set 0, or set 1 while riding (dat::Skeleton::bounds); our head-bone height when
                // a skeleton has none.
                const float b = ownModel->skeleton.boundsHeight(ownRiding ? 1 : 0);
                ownCamHeight  = (b > 0.1f ? b : ownModel->height) * ownModel->scale + (ownRiding ? drawState_.ownMountHeight : 0.0f);
            }
            ownHeadHeight = std::max(0.5f, ownCamHeight - 0.2f);
            footsteps(world.charId, ownLook, inst.bones, drawFeet, ownMovingThisFrame || effects.stepsForced(world.charId), false);
            if (!firstPerson && !selfHidden)
            {
                if (!ownRiding)
                {
                    shadows.cast(drawFeet, *ownModel, 1.0f, charInstances.size(), true);
                }
                drawState_.charIndex[world.charId] = charInstances.size();
                charInstances.push_back(std::move(inst));
                drawRod(ownLook, world.animation, anim, drawFeet, drawState_.ownShownDir);
            }
        }
        else if (!firstPerson && !selfHidden)
        {
            mk.push_back({ drawFeet, 1.7f, glm::vec4(0.3f, 0.9f, 0.4f, 1.0f), 0.6f });
        }
        // The game's target arrows (CODE 0x1014ffc0 / 0x101500d0): anchors over the target's and the sub-target
        // cursor's heads, drawn after the actors as "anc anc_l" frames.
        std::optional<glm::vec3> targetAnchor, subAnchor;
        // Us: our arrow point (2, as for everyone; see below), else over our model's height.
        const auto ownArrow = [&]() -> glm::vec3 {
            const int pt = effects.actorPoint(world.charId, 0);
            if (const auto at = poses.point(world.charId, pt >= 0 ? pt : 2))
            {
                return *at;
            }
            return drawFeet + glm::vec3(0, ownModel->height + 0.25f, 0);
        };
        if (gameUi.subTarget() == world.charId && world.charId != 0 && ownModel)
        {
            subAnchor = ownArrow(); // sub-target: us
        }
        if (targetId == world.charId && world.charId != 0 && ownModel)
        {
            targetAnchor = ownArrow();
        }
        if (!ownModel && !firstPerson)
        {
            const float     ang  = -static_cast<float>(heading) * 3.14159265f / 128.0f;
            const glm::vec3 face = glm::vec3(std::cos(ang), 0.0f, -std::sin(ang));
            mk.push_back({ feet + face * 0.45f + glm::vec3(0, 1.25f, 0), 0.25f, glm::vec4(0.15f, 0.15f, 0.15f, 1.0f), 0.25f });
        }
        // Name labels in the game's font, under the interface windows (background list).
        ui::UiPainter labels(uiR, *ImGui::GetBackgroundDrawList(), canvas.scale);
        // The names themselves (CODE 0x100d0440 -> 0x100d07b0 -> 0x10086140): "font fontshp" glyphs (0x10120440 /
        // 0x1011f9b0: quad w x h, advance w = x1 - x0 - 1) in screen space at the projected anchor, centred, sized
        // screen width x 0.00234375 x s across and screen height x 0.00234375 x s down (so wider than tall on a 4:3
        // screen). s (0x10082ff0): the anchor's depth z x 4096 against 4020 / 4100: 1 nearer than 4020, else
        // (4100 - z) / 80; with the game's projection (near 0.1) that is min(1, 0.05 + 5.12 / view depth).
        ui::UiPainter names(uiR, *ImGui::GetBackgroundDrawList(), 1.0f);
        // Icon codes (0x8E.., "menu ustatshd" shapes) draw in colour2: the linkshell's colour behind the pearl (0x92), else
        // 0x80808080 (CODE 0x10097888, 0x101204c0).
        auto          nameText = [&](const std::string& text, float cx, float bottom, float depth, ui::UiColor color, ui::UiColor color2) {
            const float s  = std::min(1.0f, 0.05f + 5.12f / std::max(depth, 0.1f));
            const float fx = static_cast<float>(ww) * 0.00234375f * s, fy = static_cast<float>(wh) * 0.00234375f * s;
            float       w  = 0;
            for (const char ch : text)
            {
                w += static_cast<unsigned char>(ch) == 0xAD ? 0.0f : uiR.shpAdvance(static_cast<unsigned char>(ch));
            }
            // Glyph tops at the line's top; the anchor is the cap line's foot (INFERRED from captures).
            float       x   = cx - w * fx * 0.5f;
            const float top = bottom - 11.0f * fy;
            for (const char ch : text)
            {
                const auto c = static_cast<unsigned char>(ch);
                // Each glyph drawn w x h, one unit inside its quad (0x1011f9b0: x1 - x0 - 1, y2 - y0 - 1).
                const auto* g = c > 0x20 && c < 0xB2 ? uiR.resources().sprite("font fontshp", c - 0x20u) : nullptr;
                if (g && !g->empty())
                {
                    const auto& q  = (*g)[0];
                    const float qw = static_cast<float>(std::max(2, q.xy[2] - q.xy[0])), qh = static_cast<float>(std::max(2, q.xy[5] - q.xy[1]));
                    if (c == 0xAD)
                    {
                        // 0xAD (after 0xAC): half size, back over the previous icon at (-2 - w, -2), no advance (0x100865a3).
                        names.sprite(*g, ImVec2(x - (2.0f + qw) * fx, top - 2.0f * fy), color2, 0, 0.5f * fx, 0.5f * fy);
                    }
                    else
                    {
                        names.sprite(*g, ImVec2(x, top), c >= 0x8E ? color2 : color, 0, fx * (qw - 1) / qw, fy * (qh - 1) / qh);
                    }
                }
                x += c == 0xAD ? 0.0f : uiR.shpAdvance(c) * fx; // 0xAD rides on 0xAC (0x10086140)
            }
            return std::pair<float, float>{ cx - w * fx * 0.5f, fy };
        };
        // `icons`: the bytes before the name (client::nameIcons); `ls`: the linkshell colour for the pearl (0x00D +0x24..26 /
        // 0x037 +0x31..33, packed as a D3DCOLOR with the first byte as blue: CODE 0x1009a42e / 0x1009d353), alpha from the
        // name's (INFERRED: the packed colour has none). `stars`: Job Master's second line (0xD2 -> D3 D4 D5, 0x10086830):
        // glyph 0xAD half size, full size, half size (the small ones bottom-aligned), centred 16 units above the name.
        // `pulse`: the current target's name (actor +0x88 bit 2, CODE 0x10157b36): alpha x (96 + 32 sin(16 tick deg)) / 128
        // (0x100830b7; the tick at 60 a second is INFERRED).
        auto          label = [&](const glm::vec3& p, const std::string& text, ui::UiColor color, const std::string& icons = {},
                                  const std::array<uint8_t, 3>* ls = nullptr, bool stars = false, bool pulse = false) {
            const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
            if (c.w <= 0.1f)
            {
                return;
            }
            if (pulse)
            {
                const auto  ms    = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                const float angle = static_cast<float>((ms * 60 / 1000 * 16) % 360) * 3.14159265f / 180.0f;
                color.a           = static_cast<uint8_t>(static_cast<float>(color.a) * (96.0f + 32.0f * std::sin(angle)) / 128.0f);
            }
            ui::UiColor color2{ 0x80, 0x80, 0x80, color.a };
            if (ls && !icons.empty() && static_cast<unsigned char>(icons[0]) == 0x92)
            {
                color2 = { static_cast<uint8_t>((*ls)[2] / 2), static_cast<uint8_t>((*ls)[1] / 2), static_cast<uint8_t>((*ls)[0] / 2), color.a };
            }
            const float sx = (c.x / c.w * 0.5f + 0.5f) * static_cast<float>(ww);
            const float sy = (c.y / c.w * 0.5f + 0.5f) * static_cast<float>(wh);
            const auto [x0, fy] = nameText(icons.empty() ? text : icons + " " + text, sx, sy, c.w, color, color2);
            const auto* star = stars ? uiR.resources().sprite("font fontshp", 0xADu - 0x20u) : nullptr;
            if (star && !star->empty())
            {
                const float s    = std::min(1.0f, 0.05f + 5.12f / std::max(c.w, 0.1f));
                const float fx   = static_cast<float>(ww) * 0.00234375f * s;
                const float aw   = uiR.shpAdvance(0xAD);
                const float base = sy - 16.0f * fy; // the line's anchor
                const auto& q    = (*star)[0];
                const float qh   = static_cast<float>(std::max(2, q.xy[5] - q.xy[1]));
                float       x    = sx - 2.0f * aw * fx * 0.5f;
                for (const float k : { 0.5f, 1.0f, 0.5f })
                {
                    names.sprite(*star, ImVec2(x, base - 11.0f * fy + (1.0f - k) * qh * fy), { 0x80, 0x80, 0x80, color.a }, 0, fx * k, fy * k);
                    x += aw * k * fx;
                }
            }
        };
        // While a cutscene has the camera (op 0x46 01 sets the game's flag 0x10351230, CODE 0x10084140) no names or target
        // arrows are drawn (INFERRED from the flag: the game's name pass was not traced).
        const bool csCamera = cutscene && cutscene->cameraMode();
        // Our own name over our head (as other players see theirs), with our linkshell's marker; none in first person
        // (0x10487f80, the name pass 0x100d0440 skips the player) or after /names off (kept with /names off otherpc).
        if (!csCamera && ownModel && !firstPerson && !selfHidden && namesHidden != 1 && !world.charName.empty())
        {
            ui::UiColor own{ 0x80, 0x80, 0x80, 0x80 };
            if (const auto& o = ui::nameColourOverrides()[0]) // an addon's player colour (Ashita namecolors)
            {
                own = { static_cast<uint8_t>(*o >> 16), static_cast<uint8_t>(*o >> 8), static_cast<uint8_t>(*o), 0x80 };
            }
            label(drawFeet + glm::vec3(0, ownModel->height + 0.25f, 0), world.charName, own,
                  client::nameIcons(world.nameFlags), &world.lsColor, world.nameFlags.jobMaster, targetId == world.charId);
        }
        // Zone lines: purple pillars at the trigger centres, labelled with their id (F11 debug view only).
        if (scene && showDebug)
        {
            for (const auto& zl : scene->zoneLines())
            {
                const glm::vec3 p = gfx::ZoneScene::gameToRender(glm::vec3(zl.pos[0], zl.pos[1], zl.pos[2]));
                mk.push_back({ p, 6.0f, glm::vec4(0.7f, 0.3f, 1.0f, 1.0f), 0.3f });
                if (glm::distance(p, feet) < args.labelRange * 3.0f)
                {
                    label(p + glm::vec3(0, 6.5f, 0), fmt::format("zone line {}", zl.name()), { 0x68, 0x50, 0x80, 0x80 });
                }
            }
        }
        std::erase_if(corpses, [&](auto& c) { return (c.second.age += dt) > kCorpseFade; });
        std::unordered_set<uint32_t> present;
        for (const auto& e : world.entities)
        {
            present.insert(e.id);
            if (e.hiddenByStatus(cutscene.has_value()))
            {
                continue;
            }
            if (!e.isPc && e.hpp == 0 && (e.animation == 2 || e.animation == 3))
            {
                // Dying now (not a corpse already there at zone-in): the model's `dead` (death cry `vded`,
                // `ded?` / `cor0` motions, which the animation plays itself).
                if (!deadSeen.contains(e.id) && zonedAt && std::chrono::steady_clock::now() - *zonedAt > std::chrono::seconds(3))
                {
                    actionFx.died(e.id); // played once posed (its files are known then)
                }
                deadSeen[e.id] = e;
            }
            else
            {
                deadSeen.erase(e.id);
            }
        }
        for (auto it = deadSeen.begin(); it != deadSeen.end();)
        {
            if (!present.contains(it->first))
            {
                corpses[it->first] = { it->second, 0.0f };
                spdlog::debug("corpse: {} dissolves", it->second.name);
                it                 = deadSeen.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (const auto& [id, c] : corpses)
        {
            if (args.shotAfterDespawn >= 0.0f && !args.screenshot.empty() && c.age >= args.shotAfterDespawn && !captureNext && !quitting)
            {
                spdlog::info("test: corpse {} at {:.2f} s", c.e.name, c.age);
                captureNext = true;
            }
        }
        std::vector<client::Entity> drawn = world.entities;
        for (const auto& [id, c] : corpses)
        {
            drawn.push_back(c.e);
        }
        if (cutscene)
        {
            for (auto& e : drawn)
            {
                e.look = cutscene->look(e.id, e.look); // op 0xB6 costumes outlast the event
            }
        }
        if (cutscene && cutscene->active())
        {
            // Event actors: hidden ones are not drawn; moved / turned ones are drawn where the script puts them.
            std::erase_if(drawn, [&](const client::Entity& e) {
                const auto* pu = cutscene->puppet(e.id);
                return pu && pu->hidden;
            });
            for (auto& e : drawn)
            {
                if (const auto* pu = cutscene->puppet(e.id))
                {
                    e.pos = pu->pos ? *pu->pos : e.pos;
                    e.dir = pu->yaw ? static_cast<uint8_t>(static_cast<int>(std::lround(*pu->yaw * 128.0f / 3.14159265f)) & 0xFF) : e.dir;
                }
            }
        }
        for (auto& e : drawn)
        {
            if (e.isPc)
            {
                e.look = riderLook(e.look, e.animation);
            }
        }
        // Config Misc.: other characters' armour "static" (cfg 165, CODE 0x1009b141): other PCs draw without
        // headgear and with model 8 for body .. feet; face and weapons stay.
        if (cfg(165, 0) == 1)
        {
            for (auto& e : drawn)
            {
                if (e.isPc && e.id != world.charId && e.look.kind == 0xFFFF)
                {
                    e.look.ids[1] = static_cast<uint16_t>(e.look.ids[1] & 0xF000);
                    for (std::size_t k = 2; k <= 5; ++k)
                    {
                        e.look.ids[k] = static_cast<uint16_t>((e.look.ids[k] & 0xF000) | 8);
                    }
                }
            }
        }
        // Config Misc.: the character limit (cfg 60, 25..51; CODE 0x10082ca0 / 0x100cbd10): only the nearest
        // characters to the camera are drawn (we are drawn apart; a rider brings its mount).
        {
            // Doors, lifts and ships (look kinds 2..4) are not characters: never culled.
            const auto chars_end = std::stable_partition(drawn.begin(), drawn.end(), [](const client::Entity& e) { return e.look.kind < 2 || e.look.kind > 4; });
            const auto limit     = static_cast<std::ptrdiff_t>(std::clamp(cfg(60, 50), 25, 51));
            if (chars_end - drawn.begin() > limit)
            {
                const glm::vec3 eye = cam.position;
                std::partial_sort(drawn.begin(), drawn.begin() + limit, chars_end, [&](const client::Entity& a, const client::Entity& b) {
                    return glm::distance(gfx::ZoneScene::gameToRender(a.pos), eye) < glm::distance(gfx::ZoneScene::gameToRender(b.pos), eye);
                });
                drawn.erase(drawn.begin() + limit, chars_end);
            }
        }
        for (const auto& e : drawn)
        {
            const auto  corpse = corpses.find(e.id);
            float       fade   = corpse != corpses.end() ? corpse->second.age / kCorpseFade : 0.0f;
            if (cutscene && cutscene->active())
            {
                if (const auto* pu = cutscene->puppet(e.id))
                {
                    fade = std::max(fade, 1.0f - pu->alpha); // op 0x6C: the renderer's dissolve
                }
            }
            if (hideTrusts && isTrust(e, world))
            {
                continue; // /hidefaith
            }
            const glm::vec3 latest = gfx::ZoneScene::gameToRender(e.pos);
            const double    tNow   = std::chrono::duration<double>(now.time_since_epoch()).count();
            auto&           mt     = smoothPos[e.id];
            if (!mt.init)
            {
                mt = { latest, latest, tNow, 0.75, 1.0f, true };
            }
            else if (glm::distance(mt.server, latest) > 1e-3f)
            {
                const double gap = tNow - mt.at;
                const float  seg = glm::distance(mt.server, latest);
                if (seg > 25.0f)
                {
                    mt.shown = latest; // a spawn or warp
                }
                else if (gap <= 3.0 * mt.interval)
                {
                    mt.interval = std::clamp(mt.interval * 0.7 + gap * 0.3, 0.05, 2.0);
                    mt.speed    = mt.speed * 0.5f + static_cast<float>(seg / std::max(gap, 0.016)) * 0.5f;
                }
                else
                {
                    mt.speed = static_cast<float>(seg / mt.interval); // starting off after standing still
                }
                mt.server = latest;
                mt.at     = tNow;
                effects.clearDisplacement(e.id); // a knockback's slide (op 0x5E) ends where the server puts it
            }
            {
                const glm::vec3 to   = mt.server - mt.shown;
                const float     dist = glm::length(to);
                float           v    = std::max(mt.speed, 0.3f);
                if (dist > v * static_cast<float>(mt.interval) * 1.5f)
                {
                    v = dist / static_cast<float>(mt.interval); // behind: catch up within one interval
                }
                mt.shown = dist <= v * dt ? mt.server : mt.shown + to / dist * (v * dt);
            }
            // Effect op 0x5E: the knockback's slide so far (game space, Y down; render z = -game z).
            const glm::vec3 kb = effects.actorDisplacement(e.id);
            glm::vec3       p  = mt.shown + glm::vec3(kb.x, -kb.y, -kb.z);
            if (const auto held = effects.attachedPosition(e.id)) // effect op 0x7A: at another actor's point
            {
                p = gfx::ZoneScene::gameToRender(*held);
            }
            static const bool moveTrace = std::getenv("XI_MOVE_TRACE") != nullptr;
            if (moveTrace && e.id == targetId)
            {
                spdlog::info("move: {:.3f} server {:.3f} {:.3f} {:.3f} shown {:.3f} {:.3f} {:.3f}", tNow, latest.x, latest.y, latest.z, p.x, p.y, p.z);
            }
            // Facing: LSB heading byte, rot = -atan2(dz, dx) * 128 / pi in game space (render z = -game z).
            const gfx::CharacterModel* cm = glm::distance(p, feet) < 80.0f ? chars.model(e.look) : nullptr;
            if (!cm)
            {
                const float     ang  = -static_cast<float>(e.dir) * 3.14159265f / 128.0f;
                const glm::vec3 face = glm::vec3(std::cos(ang), 0.0f, -std::sin(ang));
                if (showDebug)
                {
                    mk.push_back({ p + face * 0.45f + glm::vec3(0, 1.25f, 0), 0.25f, glm::vec4(0.15f, 0.15f, 0.15f, 1.0f), 0.25f });
                }
            }
            if (cm)
            {
                gfx::SkinnedRenderer::Instance inst{ cm->model, {} };
                inst.fade   = fade;
                inst.tint   = effects.actorTint(e.id);
                inst.hiddenTags = effects.hiddenParts(e.id);
                if (const auto ov = effects.actorOverlay(e.id)) // effect ops 0x40 / 0x41 / 0x54 / 0x55
                {
                    inst.overlay = ov->texture, inst.overlayScroll = ov->scroll, inst.overlayAdditive = ov->additive, inst.overlayTint = ov->colour;
                }
                inst.refraction = effects.actorShimmer(e.id); // effect ops 0x48 / 0x49
                // Config Misc. 3 "target visual effect" (cfg 153 bit 7 = 0, the default; CODE 0x100cb8a8): a new
                // target pulses brighter three times, 40 ticks each (the game lerps its fog colour toward the
                // actor light by up to 220 / 255; here the material colour is raised as much). Also everyone inside a
                // sub-targeted area spell (AoE range graphics, target_flash.cpp).
                if (const float t = targetFlash(e.id); t > 0.0f)
                {
                    inst.tint = glm::vec4(glm::vec3(inst.tint) * (1.0f + 0.86f * t), inst.tint.a);
                }
                auto& track = animTracks[e.id];
                track.drawnTick = frameTick;
                const float moved = glm::distance(glm::vec2(p.x, p.z), glm::vec2(track.lastPos.x, track.lastPos.z));
                track.speed       = track.speed + ((dt > 0.0f ? moved / dt : 0.0f) - track.speed) * std::min(1.0f, 8.0f * dt);
                track.lastPos     = p;
                const auto faced = faceOverride.find(e.id); // an event turned this NPC
                gfx::AnimInput ai{ track.speed, e.animation == 1, e.hpp == 0 && (e.animation == 2 || e.animation == 3), e.swings, castSchool(e.castSpell), e.castsDone, e.animation == 33, e.weaponSkills, chars.weaponSkill(e.look, e.wsAnimation), e.emotes, chars.emote(e.look, e.emote), 0, e.animation == 47 || (e.animation >= 63 && e.animation <= 83), hitFromBehind(e.pos, e.dir, e.lastHitBy) };
                ai.idle = eventIdle(e.id, e.look);
                if (const auto* ff = fishingFilesOf(e.id); ff && !ai.idle && fishingStatus(e.animation))
                {
                    ai.idle = chars.motion(e.look, "fh1", *ff);
                }
                // Motion sets an effect chose (ops 0x79 / 0x8C / 0xA4 / 0xA5: a bomb's growth stages, `id1` / `bt1` ...).
                if (const auto* st = effects.stance(e.id))
                {
                    auto set = [&](const std::string& stem) { return stem.empty() ? nullptr : chars.motion(e.look, stem); };
                    ai.idle        = ai.idle ? ai.idle : set(st->idle);
                    ai.stanceBattle = set(st->battle);
                    ai.stanceWalk   = set(st->walk);
                    ai.stanceRun    = set(st->run);
                }
                if (const int v = effects.motionVariant(e.id, 2); v > 0) // effect ops 0xA4 / 0xA5
                {
                    ai.runVariant = chars.motion(e.look, std::to_string(v) + "un");
                }
                if (const int v = effects.motionVariant(e.id, 3); v > 0)
                {
                    ai.walkVariant = chars.motion(e.look, std::to_string(v) + "lk");
                }
                if (const auto mq = mhf.mannequinPoses.find(e.id); !ai.idle && mq != mhf.mannequinPoses.end())
                {
                    const auto [stem, files] = app::MogHouseView::poseMotion(mq->second.first, mq->second.second);
                    ai.idle = chars.motion(e.look, stem, files);
                }
                uint8_t dir = faced != faceOverride.end() ? faced->second : e.dir;
                if (const auto yaw = effects.yawOverride(e.id)) // effect op 0x62 turning toward its target
                {
                    dir = static_cast<uint8_t>(static_cast<int>(std::lround(-*yaw * 128.0f / 3.14159265f)) & 0xFF);
                }
                float& shown       = drawState_.shownDir.try_emplace(e.id, -1.0f).first->second;
                shown              = approachHeading(shown, static_cast<float>(dir), dt);
                const bool mounted = e.isPc && drawMount(e.id, e.look, e.animation, e.chocoboIndex, e.chocoboLook, e.mountIndex, p, shown, track.speed, ai);
                // Config Misc. 2: the animation rate (cfg 64, 3 = every frame .. 0 = one in four; CODE
                // 0x100cc8e4): other characters re-pose on some frames only, staggered by their index.
                const int  rate = 3 - std::clamp(cfg(64, 3), 0, 3);
                const auto c4 = static_cast<int>(frameTick & 3), s4 = static_cast<int>(e.index & 3);
                const bool hold = track.heldValid && (rate == 1 ? c4 == s4 : rate == 2 ? ((c4 ^ s4) & 1) != 0 : rate == 3 ? c4 != s4 : false);
                if (hold)
                {
                    track.heldTime += dt;
                }
                else
                {
                    track.held      = animate(track, *cm, ai, dt + track.heldTime);
                    track.heldValid = true;
                    track.heldTime  = 0.0f;
                }
                effects.stancePick(e.id, track.oneShot || track.fxMotion || ai.castSchool >= 0); // weapons back after an action
                inst.hiddenTags = effects.hiddenParts(e.id);
                gfx::characterPalette(*cm, gfx::ZoneScene::renderToGame(p), shown, inst.bones, track.held);
                inst.origin = p;
                if (gfx::weaponDrawn(track, cm->motions) || effects.weaponHeld(e.id, 0)) // ops 0x9B / 0xA7: in the grip
                {
                    gfx::holdWeapon(*cm, inst.bones);
                }
                applyLook(e.id, *cm, inst.bones, p, dir, e.targetIndex, e.animation);
                applyScale(e.id, inst.bones, p);
                if (e.fitHeight > 0 && cm->height > 0.01f)
                {
                    // Scale about the feet to the wanted height (statues).
                    const float     k  = e.fitHeight / cm->height;
                    const glm::mat4 sc = glm::translate(glm::mat4(1.0f), p) * glm::scale(glm::mat4(1.0f), glm::vec3(k)) * glm::translate(glm::mat4(1.0f), -p);
                    for (auto& b : inst.bones)
                    {
                        b = sc * b;
                    }
                }
                gfx::simulateCloth(cm->cloth, track.cloth, inst.bones, p, dt, clothWindAt(p));
                poses.keep(e.id, e.look, cm, inst.bones);
                footsteps(e.id, e.look, inst.bones, p, track.speed > 0.5f || effects.stepsForced(e.id), track.speed < 3.5f); // the walk motion's speed band; op 0xBC
                if (!mounted)
                {
                    shadows.cast(p, *cm, 1.0f - fade, charInstances.size());
                }
                drawState_.charIndex[e.id] = charInstances.size();
                charInstances.push_back(std::move(inst));
                if (e.isPc)
                {
                    drawRod(e.look, e.animation, track.held, p, shown);
                }
                modelHeight[e.id] = cm->height * cm->scale;
                // The arrow over the actor's arrow point (CODE 0x100d0620: point index actor +0x8a6 through vfunc
                // +0x1c4, plus +0x8c0, which only an effect raises): index 2 from the constructor (0x100c611d),
                // or the one an effect named (op 0x9E). Without that point, over the model's height.
                const int       arrowPt = effects.actorPoint(e.id, 0);
                const auto      arrowAt = poses.point(e.id, arrowPt >= 0 ? arrowPt : 2);
                const glm::vec3 anchor  = arrowAt ? *arrowAt : p + glm::vec3(0, cm->height * cm->scale + 0.25f, 0);
                if (e.id == targetId)
                {
                    targetAnchor = anchor;
                }
                if (e.id == gameUi.subTarget())
                {
                    subAnchor = anchor;
                }
            }
            else if (e.id == targetId || e.id == gameUi.subTarget())
            {
                // A target without a model (a door, or one still loading): over its middle (doors are 2.9 tall).
                (e.id == targetId ? targetAnchor : subAnchor) = p + glm::vec3(0, 3.0f, 0);
            }
            else
            {
                if (showDebug) // entities without a model yet (or doors, ships): boxes in the debug view only
                {
                    mk.push_back({ p, 1.7f, e.isPc ? glm::vec4(0.35f, 0.6f, 1.0f, 1.0f) : glm::vec4(1.0f, 0.55f, 0.2f, 1.0f), 0.6f });
                }
            }
            // Names over what is drawn (or the target); every entity's in the debug view. /names off hides them all, the
            // target's too; /names off otherpc only other players' (CODE 0x100d04b8: modes 0 / 4).
            if ((!csCamera || showDebug) && fade == 0.0f && glm::distance(p, feet) < args.labelRange && !e.name.empty() && (!e.labelHidden() || showDebug) && (namesHidden == 0 || (namesHidden == 2 && !e.isPc) || showDebug) && (cm || e.id == targetId || showDebug))
            {
                const float lh = modelHeight.contains(e.id) ? modelHeight[e.id] + 0.25f : 2.1f;
                // Over the attach point an effect named (op 0x9F, CODE 0x100d03f0; INFERRED the name's anchor).
                const int  namePt = effects.actorPoint(e.id, 1);
                const auto nameAt = namePt >= 0 ? poses.point(e.id, namePt) : std::nullopt;
                label(nameAt ? *nameAt + glm::vec3(0, 0.25f, 0) : p + glm::vec3(0, lh, 0), e.name, ui::nameColour(e, world),
                      e.isPc ? client::nameIcons(e.nameFlags) : std::string(), &e.lsColor, e.isPc && e.nameFlags.jobMaster, e.id == targetId);
            }
        }
        // Target arrows (CODE 0x1014ffc0 update, 0x101500d0 draw): sheet 6 "anc anc_l", six frames, the next every 3
        // ticks (1/20 s); a white arrow (0xE0808080) over the target; while a command waits for its sub-target, a
        // coloured one 10 units higher over the sub-target (0x101504d0: blue 0xE04040D0 in range, red 0xE0D04040 out of
        // range, yellow 0xE0D0D040 at the edge; ranges from file 81, see GameUi::subTargetRange). The arrow's tip sits on the screen
        // point just above the name; the anchor is the actor's arrow point (see the draw loop).
        const auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const auto frame = static_cast<std::size_t>((ms * 60 / 1000 / 3) % 6);
        auto       arrow = [&](const glm::vec3& at, float raise, ui::UiColor colour) {
            const glm::vec4 c = viewProj * glm::vec4(at, 1.0f);
            if (c.w > 0.1f)
            {
                const float sx = (c.x / c.w * 0.5f + 0.5f) * static_cast<float>(ww);
                const float sy = (c.y / c.w * 0.5f + 0.5f) * static_cast<float>(wh);
                // The name label shares the anchor (11 glyph units tall at the same depth scale), so the tip clears
                // its top by 2 units (retail captures; INFERRED).
                const float s     = std::min(1.0f, 0.05f + 5.12f / std::max(c.w, 0.1f));
                const float nameH = 11.0f * static_cast<float>(wh) * 0.00234375f * s;
                labels.sprite("anc anc_l", frame, ImVec2(sx, sy - nameH - (2.0f + raise) * canvas.scale), colour);
            }
        };
        if (subAnchor && !csCamera)
        {
            // The range test (CODE 0x10150540): d = distance (flat for spells and abilities) less both hitbox sizes;
            // red past range + 0.5 or at 25 yalms and more, yellow past range - 0.5, else blue.
            ui::UiColor colour{ 0x40, 0x40, 0xD0, 0xE0 };
            const uint32_t sid = gameUi.subTarget();
            const auto     rng = gameUi.subTargetRange(world, battleText.get(), spellData ? &*spellData : nullptr, abilityData ? &*abilityData : nullptr);
            const client::Entity* st = nullptr;
            for (const auto& e : world.entities)
            {
                st = e.id == sid ? &e : st;
            }
            if (rng && st)
            {
                glm::vec3 a = gfx::ZoneScene::renderToGame(feet), b = st->pos;
                if (rng->flat)
                {
                    a.y = b.y = 0.0f;
                }
                const float dist = glm::length(a - b);
                const float d    = dist - st->hitbox;
                const float r    = static_cast<float>(rng->yalms);
                if (d > r + 0.5f || dist >= 25.0f)
                {
                    colour = { 0xD0, 0x40, 0x40, 0xE0 };
                }
                else if (d > std::max(r - 0.5f, 0.0f))
                {
                    colour = { 0xD0, 0xD0, 0x40, 0xE0 };
                }
                static const bool trace = std::getenv("XI_RANGE_TRACE") != nullptr; // XI_RANGE_TRACE=1: log each change
                static std::tuple<uint32_t, int, uint8_t> lastLogged{};
                if (trace && lastLogged != std::tuple{ sid, rng->yalms, colour.r })
                {
                    lastLogged = { sid, rng->yalms, colour.r };
                    spdlog::info("range: {} '{}' {:.1f} yalms (less hitbox {:.1f}), range {}{}, {}", sid, st->name, dist, st->hitbox, rng->yalms,
                                 rng->flat ? " flat" : "", colour.r == 0x40 ? "blue" : colour.g == 0xD0 ? "yellow" : "red");
                }
            }
            arrow(*subAnchor, 10.0f, colour);
        }
        if (targetAnchor && !csCamera)
        {
            arrow(*targetAnchor, 0.0f, ui::UiColor{ 0x80, 0x80, 0x80, 0xE0 });
        }
    }
    frameTrace.mark(gfx::FrameTrace::Actors);
    skinned.prepare(charInstances); // effect overlay textures (before the frame is recorded)
}

} // namespace xi::app
