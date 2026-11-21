// SPDX-License-Identifier: GPL-3.0-or-later
// The player's movement and the camera.
#include "client_app.h"

#include "client/target_lock.h"

namespace xi::app
{

void ClientApp::updateMovement()
{
    // Movement (only while in zone, alive and not typing).
    dead = world.animation == 2 || world.animation == 3;
    if (zoneFadeHold())
    {
        return; // the new zone is in but the old one is still fading out: nothing moves until it is built
    }
    static std::optional<std::chrono::steady_clock::time_point> diedAt;
    if (!dead)
    {
        diedAt.reset();
    }
    else if (!diedAt)
    {
        diedAt = now;
        spdlog::info("knocked out");
    }
    if (args.homepoint && diedAt && now - *diedAt > std::chrono::seconds(5) && session)
    {
        spdlog::info("test: return to home point");
        session->action(world.charId, world.actIndex, 0x0B, 0);
        diedAt = now + std::chrono::seconds(30); // once
    }
    if (scene && world.inZone && !dead && !ImGui::GetIO().WantTextInput)
    {
        static const bool kNoKeys[SDL_SCANCODE_COUNT] = {};
        // During an event or with a line out the character stays put (the game's player lock).
        const bool*       k = testRun || gameUi.typing() || playerLocked() ? kNoKeys : SDL_GetKeyboardState(nullptr);
        const bool  autoWalk = zonedAt && sinceZone < args.walk;
        glm::vec2   in = keymap.move(k, focusedModState()); // Normal: keypad 8/2/4/6; Compact also WASD
        // Kneeling (/heal, or the /logout / /shutdown countdown: status 33) the character does not walk. A movement key
        // ends the rest (Session::standUp, once per kneel) and the character only walks once the server has stood it up
        // and the stand-up motion (rest up) has played. The game keeps the kneel while status 33 holds: its actor state
        // plays res0 / loops / res2 by status (CODE 0x1008bcca), and its own "/heal" key toggles by status 33
        // (0x10123de0, 0x10157ed0). That a movement key sends the cancel is INFERRED (LSB does not end the rest on
        // movement; the game client must).
        bool restHold = false;
        {
            const auto own       = animTracks.find(0xFFFFFFFFu);
            const bool kneeling  = world.animation == 33;
            // Also the frame the status changed in, before the animation has started the stand-up (wasResting still set):
            // a step there would cancel it (a move cancels one-shots).
            const bool standing  = own != animTracks.end() && (own->second.standingUp || own->second.wasResting);
            const bool wantsMove = in != glm::vec2(0.0f) || autorun || followId != 0 || ((args.gotoXZ || scriptGoto) && zonedAt);
            if (kneeling && wantsMove && session && !restCancelSent)
            {
                session->standUp();
                restCancelSent = true;
                spdlog::info("move: movement key while kneeling: rest / logout cancelled, waiting to stand");
            }
            if (!kneeling)
            {
                restCancelSent = false;
            }
            if (kneeling || standing)
            {
                static const bool restTrace = std::getenv("XI_REST_TRACE") != nullptr;
                if (restTrace && wantsMove)
                {
                    spdlog::info("rest: move held (kneeling {}, standing up {}) at {:.2f},{:.2f}", kneeling, standing, feet.x, feet.z);
                }
                in       = glm::vec2(0.0f);
                autorun  = false;
                followId = 0;
                restHold = true;
            }
        }
        if (in != glm::vec2(0.0f))
        {
            autorun  = false; // a movement key ends autorun and /follow
            followId = 0;
        }
        // /follow: walk after the entity until close (2.5 yalms), at the normal speed.
        glm::vec3 followDir(0.0f);
        if (followId != 0 && !playerLocked())
        {
            const client::Entity* f = nullptr;
            for (const auto& e : world.entities)
            {
                if (e.id == followId)
                {
                    f = &e;
                }
            }
            if (!f)
            {
                followId = 0;
            }
            else
            {
                const glm::vec3 fg = gfx::ZoneScene::renderToGame(feet);
                const glm::vec2 d(f->pos.x - fg.x, f->pos.z - fg.z);
                if (glm::length(d) > 2.5f)
                {
                    followDir = glm::normalize(glm::vec3(d.x, 0.0f, -d.y));
                }
            }
        }
        if ((autorun || autoWalk) && !playerLocked() && !restHold)
        {
            in.y = 1.0f;
        }
        // First person (the game camera looks along the facing): left / right turn the character and forward / back
        // walk along its facing; the turn is 2 degrees a tick at full stick (CODE 0x100a72b1: x * ticks * 0.01745 * 2).
        const bool fpMove = firstPerson && !std::getenv("XI_CAM_ORBIT");
        if (fpMove && in.x != 0.0f && !playerLocked())
        {
            float f = headingExact >= 0.0f ? headingExact : static_cast<float>(heading);
            f       = std::fmod(f + in.x * 2.0f * dt * 60.0f * 256.0f / 360.0f + 256.0f, 256.0f);
            headingExact = f;
            heading      = static_cast<uint8_t>(static_cast<int>(std::lround(f)) & 0xFF);
            if (in.y == 0.0f && session)
            {
                session->setPlayer(gfx::ZoneScene::renderToGame(feet), heading, target ? target->index : 0);
            }
        }
        if (fpMove)
        {
            in.x = 0.0f;
        }
        glm::vec3 gotoDir(0.0f);
        if ((args.gotoXZ || scriptGoto) && zonedAt && !playerLocked() && !restHold)
        {
            const glm::vec2 to = scriptGoto ? *scriptGoto : *args.gotoXZ;
            const glm::vec3 fg = gfx::ZoneScene::renderToGame(feet);
            const glm::vec2 d(to.x - fg.x, to.y - fg.z); // game-space x/z
            if (glm::length(d) > 0.3f)
            {
                gotoDir = glm::normalize(glm::vec3(d.x, 0.0f, -d.y)); // back to render space
            }
        }
        if (glm::length(in) > 0.0f || glm::length(gotoDir) > 0.0f || glm::length(followDir) > 0.0f)
        {
            if (glm::length(in) > 0.0f)
            {
                in = glm::normalize(in);
            }
            // Relative to the view; in first person to the facing (the view's look-aside does not steer).
            const float     moveYaw = fpMove ? 3.14159265f * 0.5f + static_cast<float>(heading) * 3.14159265f / 128.0f : camYaw;
            const glm::vec3 fwd(std::sin(moveYaw), 0.0f, -std::cos(moveYaw));
            const glm::vec3 right(std::cos(moveYaw), 0.0f, std::sin(moveYaw));
            const glm::vec3 dir   = glm::length(gotoDir) > 0.0f ? gotoDir : glm::length(in) > 0.0f ? fwd * in.y + right * in.x : followDir;
            // The server's Speed x 0.1 yalms a second (CODE 0x10094aaa; LSB's normal 50 = 5), doubled on a chocobo
            // or mount (LSB sends half the mount's), at most 30 (CODE 0x100a6413..0x100a643a); walking a third
            // (0x10081fe0, used when the actor's +0x598 is 3, 0x100a7f7c). Only the --goto test hook moves faster.
            const bool  mounted = world.animation == 5 || world.animation == 85;
            const float run     = std::min(static_cast<float>(world.speed) * 0.1f * (mounted ? 2.0f : 1.0f), 30.0f);
            const float speed   = run * (glm::length(gotoDir) > 0.0f && !scriptGoto ? 4.0f : 1.0f) * (walking && !mounted ? 1.0f / 3.0f : 1.0f);
            const glm::vec3 before = feet;
            feet      = scene->move(feet, dir * speed * dt);
            if (feet.y < before.y - gfx::ZoneCollision::kStepUp)
            {
                feet.y = before.y; // walked off a ledge: fall to the floor below (gravity, below)
            }
            ownMoving = true;
            // Zone lines (CODE 0x10175820): the first one this step enters or crosses sends C2S 0x05E once; the
            // latch clears on the zone change, or after 6 s without one (the server refused; CODE 0x1009f070).
            if (zoneLineSent && moveState_.zoneLineAt && now - *moveState_.zoneLineAt > std::chrono::seconds(6))
            {
                zoneLineSent = false;
            }
            const auto g0 = gfx::ZoneScene::renderToGame(before), g1 = gfx::ZoneScene::renderToGame(feet);
            const float from[3] = { g0.x, g0.y, g0.z }, to[3] = { g1.x, g1.y, g1.z };
            for (const auto& zl : scene->zoneLines())
            {
                if (!zoneLineSent && zl.crossedBy(from, to))
                {
                    spdlog::info("entering zone line {}", zl.name());
                    session->enterZoneLine(zl.id);
                    zoneLineSent = true;
                    moveState_.zoneLineAt   = now;
                }
            }
            // Heading from the game-space delta (render z = -game z). Locked on (client/target_lock.h), the
            // character keeps facing its target and steps back / sideways relative to it, as in the game; unlocked
            // it runs where it is steered, engaged or not.
            if (!fpMove)
            {
                heading      = headingByte(dir.x, -dir.z);
                headingExact = headingFloat(dir.x, -dir.z);
            }
            ownMoveDir   = 0;
            if (targetLocked() && glm::length(gotoDir) == 0.0f)
            {
                const glm::vec2 td(target->pos.x - g1.x, target->pos.z - g1.z);
                if (glm::length(td) > 0.1f)
                {
                    heading      = headingByte(td.x, td.y);
                    headingExact = headingFloat(td.x, td.y);
                    ownMoveDir   = client::lockedMoveDir(glm::vec2(td.x, -td.y), glm::vec2(dir.x, dir.z)); // render x/z
                }
            }
            session->setPlayer(gfx::ZoneScene::renderToGame(feet), heading, target ? target->index : 0);
        }
    }

    // Locked on, face the target (the server refuses melee rounds we are not facing; unlocked, turning is up to the
    // player, INFERRED) and, as a test hook, run into melee range.
    if (scene && target && !ownMoving && (targetLocked() || (args.chase && !target->isPc && target->hpp > 0)))
    {
        const glm::vec3 g = gfx::ZoneScene::renderToGame(feet);
        const glm::vec2 d(target->pos.x - g.x, target->pos.z - g.z);
        if (glm::length(d) > 0.1f)
        {
            const float dist = glm::length(d);
            // The server measures melee range in 3D (hitbox sizes), so close in further on slopes.
            const float dist3 = glm::distance(target->pos, g);
            if (args.chase && dist3 > 2.2f && dist > 1.2f)
            {
                const glm::vec3 dir = glm::normalize(glm::vec3(d.x, 0.0f, -d.y));
                feet      = scene->move(feet, dir * 5.0f * std::min(dt, (dist - 1.0f) / 5.0f));
                ownMoving = true;
            }
            const uint8_t face = headingByte(d.x, d.y);
            if ((face != heading || ownMoving) && session)
            {
                heading = face;
                session->setPlayer(gfx::ZoneScene::renderToGame(feet), heading, target->index);
            }
        }
    }

    if (session && world.blacklistSerial != moveState_.blacklistSeen)
    {
        // 55648: 116 "Added to blacklist.", 117 failed to add, 118 "Removed from blacklist.", 119 failed.
        moveState_.blacklistSeen = world.blacklistSerial;
        const int  id = world.blacklistEdit == 0 ? 116 : world.blacklistEdit == 1 ? 118 : blacklistAsked == 0 ? 117 : 119;
        const auto t  = clientText ? clientText->get(static_cast<std::size_t>(id)) : std::string_view();
        session->note(t.empty() ? std::string("Blacklist updated.") : std::string(t));
    }
    if (searchJob.valid() && searchJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready && session)
    {
        for (auto& l : searchJob.get())
        {
            session->note(std::move(l));
        }
    }
}

void ClientApp::updateCamera()
{
    if (zoneFadeHold())
    {
        return; // keep the old zone's view while it fades out (its collision is not the new zone's)
    }
    // XI_CAM_ORBIT=1: the camera as before (a yaw / pitch orbit at a fixed distance), for comparisons.
    static const bool orbitCam = std::getenv("XI_CAM_ORBIT") != nullptr;
    const float       ticks    = dt * 60.0f;
    // Camera keys: arrows (no menu open) and Compact IJKL, Compact 2's Q / E turn, the window edge (cfg 143). The
    // game camera turns at the game's rates (client::GameCamera); the orbit at the earlier placeholder ones.
    float inYaw = 0.0f, inUp = 0.0f;
    if (inGame && !testRun && !gameUi.typing() && !ImGui::GetIO().WantTextInput)
    {
        const bool*     held = SDL_GetKeyboardState(nullptr);
        const glm::vec2 cv   = keymap.camera(held, focusedModState(), gameUi.menuOpen());
        // The keys pan the camera the way the game does: Right / Up move the view the opposite way to the mouse
        // orbit (the camera swings around the character, so the view turns left / looks down).
        inYaw = -cv.x * camInvert(true) + keymap.turn(held, focusedModState());
        inUp  = cv.y * camInvert(false);
        // Config Mouse/Cam.: edge-of-screen rotation (cfg 143 = 0, the default; CODE 0x10124bf0): the cursor
        // within a pixel of the window's edge turns the camera like the camera keys (not while orbiting).
        float mx = 0, my = 0;
        if (cfg(143, 0) == 0 && !orbiting && (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_FOCUS))
        {
            int winW = 0, winH = 0;
            SDL_GetWindowSize(window, &winW, &winH);
            SDL_GetMouseState(&mx, &my);
            const float ex = mx <= 1.0f ? -1.0f : mx >= static_cast<float>(winW) - 2.0f ? 1.0f : 0.0f;
            const float ey = my <= 1.0f ? -1.0f : my >= static_cast<float>(winH) - 2.0f ? 1.0f : 0.0f;
            inYaw += ex * camInvert(true);
            inUp += ey * camInvert(false);
        }
        inYaw = std::clamp(inYaw, -1.0f, 1.0f);
        inUp  = std::clamp(inUp, -1.0f, 1.0f);
        if (inYaw != 0.0f || inUp != 0.0f)
        {
            lastCamInput = now;
        }
        if (orbitCam)
        {
            camYaw += inYaw * 2.0f * dt;
            camPitch = std::clamp(camPitch - inUp * 1.5f * dt, -1.55f, 0.6f);
        }
        // Zoom In / Out held: the game's projection zoom, 6 a tick (1/60 s), 242..900 (CODE 0x1001f742..0x1001f875;
        // saved as config 0x49 by 0x10193740); both held ease it back to 350, a quarter of the gap a frame (30 a
        // second, INFERRED). Not in first person (there it is 280). The view distance does not change.
        if (!firstPerson && !gameUi.menuOpen())
        {
            if (keymap.zoomReset(held, focusedModState()))
            {
                moveState_.camZoomReset  = true;
                camWheelTicks = 0.0f;
            }
            if (moveState_.camZoomReset)
            {
                const float gap = 350.0f - camZoom;
                camZoom         = std::abs(gap) * 0.25f < 1.0f ? 350.0f : camZoom + gap * (1.0f - std::pow(0.75f, ticks * 0.5f));
                moveState_.camZoomReset    = camZoom != 350.0f;
                camZoomDirty    = true;
            }
            else if (const float z = keymap.zoom(held, focusedModState()); z != 0.0f)
            {
                camZoom       = std::clamp(camZoom + z * 6.0f * ticks, 242.0f, 900.0f);
                camZoomDirty  = true;
                camWheelTicks = 0.0f;
            }
        }
    }
    if (testRun && now < camHoldUntil) // test key "cam:" (test_hooks.cpp)
    {
        inYaw = camHoldYaw;
        inUp  = camHoldUp;
    }
    // The wheel's notches (input.cpp): 6 a tick until spent; reaching a limit drops the rest (0x1025dd90).
    if (camWheelTicks != 0.0f && !firstPerson)
    {
        const float step = std::copysign(std::min(std::abs(camWheelTicks), ticks), camWheelTicks);
        camWheelTicks -= step;
        camZoom += step * 6.0f;
        if (camZoom <= 242.0f || camZoom >= 900.0f)
        {
            camZoom       = std::clamp(camZoom, 242.0f, 900.0f);
            camWheelTicks = 0.0f;
        }
        camZoomDirty = true;
    }
    else if (camZoomDirty && !moveState_.camZoomReset && keymap.zoom(SDL_GetKeyboardState(nullptr), focusedModState()) == 0.0f)
    {
        saveCamZoom();
    }
    if (orbitCam)
    {
        // Config Mouse/Cam.: third-person camera locked behind (cfg 145 = 1; CODE 0x1001f9d0): with no camera
        // input for 4 ticks it swings 2.5 % a tick toward behind the character, unless it already looks along the
        // character's facing or looks at its front (-0.5 < dot < 0.99).
        if (inGame && cfg(145, 0) == 1 && !orbiting && now - lastCamInput > std::chrono::milliseconds(67) && !lockOn)
        {
            const float     ang  = -static_cast<float>(heading) * 3.14159265f / 128.0f;
            const glm::vec3 face = glm::vec3(std::cos(ang), 0.0f, -std::sin(ang));
            const glm::vec3 fwd(std::sin(camYaw), 0.0f, -std::cos(camYaw));
            if (const float dot = glm::dot(fwd, face); dot > -0.5f && dot < 0.99f)
            {
                float delta = std::atan2(face.x, -face.z) - camYaw;
                delta       = std::remainder(delta, 6.2831853f);
                camYaw += delta * (1.0f - std::pow(0.975f, dt * 60.0f));
            }
        }
        // /lockon: the camera turns to keep the target ahead.
        if (lockOn && target)
        {
            const glm::vec3 fg = gfx::ZoneScene::renderToGame(feet);
            const glm::vec2 d(target->pos.x - fg.x, target->pos.z - fg.z); // game x/z; render z = -game z
            if (glm::length(d) > 0.1f)
            {
                camYaw = std::atan2(d.x, d.y); // forward() = (sin yaw, ., -cos yaw) in render space
            }
        }
    }
    if (lockOn && !target)
    {
        lockOn = false;
    }

    cam      = gfx::FlyCamera{};
    cam.fovY = gfx::gameFovY(firstPerson ? 280.0f : camZoom);
    // Lifts: inside a lift's region the player stands on its platform (a height override; dat-format §5.8). The
    // game camera's eye rides along (actor +0x5e2, CODE 0x1001f1ab).
    bool onLift = false;
    if (scene && world.inZone)
    {
        if (const auto ly = scene->liftFloor(gfx::ZoneScene::renderToGame(feet)))
        {
            onLift = true;
            feet.y     = -*ly;
        }
    }
    if (scene && scene->propsKey() == propsKey)
    {
        mogView.poseProps(*scene, mhf);
        scene->setSolidBoxes(mhf.boxes);
        scene->setCameraSolidBoxes(mhf.cameraBoxes);
        scene->setCameraBoxes(((cfg(153, 0) >> 6) & 1) == 0); // Config Misc. 3 furniture camera collision
    }
    // An event moving the player (client::Cutscene puppet): its position, facing and walking win.
    selfHidden    = false;
    ownEventSpeed = 0.0f;
    if (cutscene && cutscene->active())
    {
        if (const auto* pu = cutscene->puppet(world.charId))
        {
            if (pu->pos)
            {
                feet = gfx::ZoneScene::gameToRender(*pu->pos);
            }
            if (pu->yaw)
            {
                heading = static_cast<uint8_t>(static_cast<int>(std::lround(*pu->yaw * 128.0f / 3.14159265f)) & 0xFF);
            }
            ownMoving |= pu->walking;
            ownEventSpeed = pu->walking ? pu->speed : 0.0f;
            selfHidden = pu->hidden;
        }
    }
    // Gravity (CODE 0x100a4e00): over a floor lower than the feet, the fall speed grows 0.0408 per 1/60 s tick, at
    // most 1 per tick, until the feet reach it.
    if (scene && world.inZone && !(cutscene && cutscene->active()))
    {
        auto ground = scene->groundHeight(feet.x, feet.z, feet.y + 0.05f);
        if (ground && *ground < feet.y - 0.5f)
        {
            // Standing on a stair riser's sliver over a lower floor (Port Bastok's harbour stairs): the game's probe
            // stands on the riser, so no fall into the sea.
            if (const auto s = scene->collision().sliverHeight(feet.x, feet.z, feet.y - 0.5f, feet.y + 0.05f))
            {
                ground = s;
            }
        }
        if (ground && *ground < feet.y - 0.01f)
        {
            moveState_.fallSpeed = std::min(moveState_.fallSpeed + gfx::ZoneCollision::kGravityPerTick * ticks, gfx::ZoneCollision::kMaxFallPerTick);
            feet.y    = std::max(*ground, feet.y - moveState_.fallSpeed * ticks);
        }
        else
        {
            moveState_.fallSpeed = 0.0f;
        }
    }
    // The drawn position (actor +0x5fc) closes 1/8 of its gap to the physics position (+0x5c4) each tick (actor
    // update CODE 0x100c6506..0x100c6586, all three axes), so the floor snap's step up or down a riser is eased over
    // ~10 ticks. A placement (a jump of more than 3: warp, zone-in) and a lift ride draw at the feet (INFERRED: the
    // update's snap branch 0x100c659c, taken under the actor flags +0x101..+0x104, is not tied to these).
    {
        static const bool easeOld = std::getenv("XI_EASE_OLD") != nullptr;
        if (easeOld || !drawFeetSeen || onLift || snapPending || glm::distance(drawFeet, feet) > 3.0f)
        {
            drawFeet = feet;
        }
        else
        {
            drawFeet += (feet - drawFeet) * (1.0f - std::pow(0.875f, ticks));
        }
        drawFeetSeen = true;
        static const bool stairsTrace = std::getenv("XI_STAIRS_TRACE") != nullptr;
        if (stairsTrace && world.inZone)
        {
            spdlog::info("stairs: {:.4f} feet {:.3f} {:.3f} {:.3f} drawn {:.3f}", dt, feet.x, feet.y, feet.z, drawFeet.y);
        }
    }
    ownMovingThisFrame = ownMoving;
    ownMoving          = false;
    // The cutscene camera drives the view between event ops 0x46 01 and 00 (below); the game camera is placed
    // behind the character again when it ends (INFERRED: the game restores its own saved view).
    const bool eventCamera = cutscene && cutscene->cameraMode();
    if (orbitCam)
    {
        const glm::vec3 lookAt = feet + glm::vec3(0.0f, 1.5f, 0.0f);
        cam.yaw                = camYaw;
        cam.pitch              = camPitch;
        cam.position           = firstPerson ? lookAt : lookAt - cam.forward() * camDist; // Change View: first person
        // XI_CAM_OLD=1: the camera as before goal 22 (ray from the view target, no clearance), for comparisons.
        static const bool oldCamera = std::getenv("XI_CAM_OLD") != nullptr;
        // Camera collision: the camera stays in front of the first zone triangle between the head and where it
        // would be (0.2 short of it), pulling in at once and easing back out at 8 yalms a second.
        static float shown = -1.0f;
        float        want  = camDist;
        if (!firstPerson && scene)
        {
            want = scene->cameraDistance(lookAt, cam.forward(), camDist, oldCamera ? 0.0f : 0.5f); // from 1 above the feet
        }
        shown = shown < 0.0f || want < shown ? want : std::min(want, shown + 8.0f * dt);
        if (!firstPerson)
        {
            cam.position = lookAt - cam.forward() * shown;
            // The eye keeps 0.17 from the ground, ceilings and walls beside it (CODE 0x1001ed90 tail).
            if (scene && !oldCamera)
            {
                cam.position = scene->cameraClearance(cam.position, cam.forward());
            }
        }
    }
    else if (world.inZone && scene)
    {
        // The game's camera (client::GameCamera): an eye trailing a look-at point.
        client::GameCamera::Subject subj;
        subj.feet       = drawFeet; // the look-at follows the drawn model, so a stair step doesn't bob the view
        const float ang = -static_cast<float>(heading) * 3.14159265f / 128.0f;
        subj.facing     = glm::vec3(std::cos(ang), 0.0f, -std::sin(ang));
        subj.height     = ownCamHeight;
        subj.headHeight = ownHeadHeight;
        subj.moving     = ownMovingThisFrame;
        // On a lift the eye rises and sinks with the feet: their change since the camera last saw them (not the
        // override this frame: something else can reset the feet below the platform first; ours).
        subj.platformDy = !camReset && onLift && moveState_.camFeetSeen && std::abs(feet.y - moveState_.camLastFeet.y) <= 3.0f ? feet.y - moveState_.camLastFeet.y : 0.0f;
        // A jump of more than 3 in one frame is a placement, not a walk or a ride (a lift moves 1.5 a second): the
        // eye is placed again, as at a warp (a lift's first state arriving after zone-in moved the feet 12 at once
        // and left the eye 10 over the look-at; ours, INFERRED).
        if (moveState_.camFeetSeen && glm::distance(feet, moveState_.camLastFeet) > 3.0f)
        {
            camReset        = true;
            subj.platformDy = 0.0f;
        }
        moveState_.camLastFeet = feet;
        moveState_.camFeetSeen = true;
        if (snapPending || eventCamera)
        {
            camReset = true; // wait for the floor; an event shot owns the view
        }
        else if (camReset)
        {
            camReset = false;
            if (args.camGiven && !moveState_.camGivenUsed)
            {
                moveState_.camGivenUsed = true; // the test hooks' view, once
                moveState_.gameCam.resetAt(subj, camYaw, camPitch, camDist);
            }
            else
            {
                moveState_.gameCam.reset(subj);
            }
        }
        else if (camAtPending) // test key "camat:"
        {
            camAtPending = false;
            moveState_.gameCam.resetAt(subj, camAtYaw, camAtPitch, camAtDist);
            spdlog::info("test: camera at yaw {:.0f} pitch {:.0f} dist {:.1f}", camAtYaw * 57.2958f, camAtPitch * 57.2958f, camAtDist);
        }
        client::GameCamera::Input gin;
        gin.yaw          = inYaw;
        gin.vertical     = inUp;
        gin.mouseYaw     = camMouseYaw;
        gin.mouseUp      = camMouseUp;
        gin.recentre     = camBehind;
        gin.lockedBehind = cfg(145, 0) == 1;
        gin.zoom         = camZoom;
        std::optional<client::GameCamera::Lock> lock;
        if (lockOn && target)
        {
            client::GameCamera::Lock l;
            l.pos = gfx::ZoneScene::gameToRender(target->pos);
            // The aim points: the game's point 7 on the target and 13 on us (CODE 0x100d65a0, 0x1001f428), else
            // 3/4 of the model's height (INFERRED).
            l.aimHeight = 0.75f * (modelHeight.contains(target->id) ? modelHeight[target->id] : 1.8f);
            if (const auto p = poses.point(target->id, 7); p && p->y > l.pos.y)
            {
                l.aimHeight = p->y - l.pos.y;
            }
            l.selfHeight = 0.75f * ownCamHeight;
            if (const auto p = poses.point(world.charId, 13); p && p->y > feet.y)
            {
                l.selfHeight = p->y - feet.y;
            }
            l.engaged = world.engaged();
            l.size    = 1.5f * target->hitbox;
            lock      = l;
        }
        if (!eventCamera && !camReset)
        {
            moveState_.gameCam.update(
                subj, gin, lock, firstPerson, ticks, [this](const glm::vec3& a, const glm::vec3& b) { return scene->cameraRay(a, b); },
                [this](const glm::vec3& e, const glm::vec3& f) { return scene->cameraClearance(e, f); });
        }
        if (moveState_.gameCam.placed())
        {
            cam.position = moveState_.gameCam.eye();
            cam.yaw      = moveState_.gameCam.yaw();
            cam.pitch    = moveState_.gameCam.pitch();
            camYaw       = cam.yaw; // movement and furniture placing go by the view's heading
        }
        // XI_CAM_TRACE=SECONDS (1: twice a second): the eye and look-at (game space), their distance and the eye's rise.
        static const float camTrace = std::getenv("XI_CAM_TRACE") ? std::max(0.0f, std::strtof(std::getenv("XI_CAM_TRACE"), nullptr)) : -1.0f;
        static float      traceAt  = 0.0f;
        if (camTrace >= 0.0f && (traceAt += dt) >= (camTrace == 1.0f ? 0.5f : camTrace))
        {
            traceAt           = 0.0f;
            const glm::vec3 e = gfx::ZoneScene::renderToGame(moveState_.gameCam.eye()), a = gfx::ZoneScene::renderToGame(moveState_.gameCam.at());
            spdlog::info("camera: eye ({:.2f}, {:.2f}, {:.2f}) at ({:.2f}, {:.2f}, {:.2f}) dist {:.2f} rise {:.2f} yaw {:.0f} pitch {:.0f} h {:.2f}{}{}", e.x,
                         e.y, e.z, a.x, a.y, a.z, moveState_.gameCam.distance(), moveState_.gameCam.eye().y - moveState_.gameCam.at().y, glm::degrees(cam.yaw),
                         glm::degrees(cam.pitch), ownCamHeight, firstPerson ? " first-person" : "", lock ? " lock-on" : "");
        }
    }
    camMouseYaw = camMouseUp = 0.0f;
    camBehind                = false;
    // Cutscene shots (scheduler op 0x04 Camera chunks) drive the camera between event ops 0x46 01 and 00.
    if (eventCamera)
    {
        if (const auto shot = effects.camera())
        {
            const glm::vec3 eye = gfx::ZoneScene::gameToRender(shot->eye), at = gfx::ZoneScene::gameToRender(shot->at);
            const glm::vec3 d   = at - eye;
            if (glm::length(d) > 1e-3f)
            {
                const glm::vec3 n = glm::normalize(d);
                cam.position      = eye;
                cam.yaw           = std::atan2(n.x, -n.z);
                cam.pitch         = std::asin(std::clamp(n.y, -1.0f, 1.0f));
                // The shot's zoom is the game's projection scale (350 = the normal view).
                cam.fovY = gfx::gameFovY(std::max(shot->zoom, 50.0f));
            }
        }
    }

    frameTrace.mark(gfx::FrameTrace::Game);
}

} // namespace xi::app
