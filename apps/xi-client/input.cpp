// SPDX-License-Identifier: GPL-3.0-or-later
// Input: SDL events, keys through the game's bindings (src/ui/keymap.h), targeting and the Confirm / talk / Attack actions.
#include "client_app.h"

#include "ashita/input_map.h"
#include "client/target_lock.h"

#include <cmath>

namespace xi::app
{

// A key press in game, through the game's bindings (src/ui/keymap.h). The menu keys reach the interface as its
// canonical keys (arrows, Enter, Esc, -). Returns false when the key is not a game action here.
bool ClientApp::onKey(SDL_Scancode sc, SDL_Keymod mods, bool repeat)
{
    using KA        = ui::KeyAction;
    const bool menu = gameUi.menuOpen();
    const auto a    = keymap.pressed(sc, mods, menu);
    if (!a)
    {
        return false;
    }
    if (cutscene && cutscene->waitingMessage() && (*a == KA::Confirm || *a == KA::Cancel))
    {
        eventKey = !repeat; // the event's message window closes: the script continues
        return true;
    }
    // Placing a furnishing (layout mode): the keys move / turn / place it; camera heading in game x / z.
    if (!menu && mogView.takesKeys() && curWorld && mogView.placingKey(*a, *curWorld, std::atan2(std::cos(camYaw), std::sin(camYaw)), session.get(), gameUi))
    {
        return true;
    }
    switch (*a)
    {
        case KA::CursorUp:
        case KA::CursorDown:
        case KA::CursorLeft:
        case KA::CursorRight:
            if (menu)
            {
                gameUi.keyDown(*a == KA::CursorUp ? SDLK_UP : *a == KA::CursorDown ? SDLK_DOWN : *a == KA::CursorLeft ? SDLK_LEFT : SDLK_RIGHT, repeat);
            }
            return true; // with no menu open they turn the camera (held keys, below)
        case KA::Confirm:
            if (menu || gameUi.fishingLineOut())
            {
                gameUi.keyDown(SDLK_RETURN, repeat);
            }
            else if (!repeat)
            {
                inputState_.confirmKey = true; // resolved once the target is known
            }
            return true;
        case KA::Cancel:
            if (menu || gameUi.fishingLineOut())
            {
                gameUi.keyDown(SDLK_ESCAPE, repeat);
            }
            else if (!targetLocked()) // locked: Esc keeps the target (see handleEvents)
            {
                inputState_.targetEscaped = targetId != 0;
                targetId      = 0;
            }
            return true;
        case KA::MainMenu: gameUi.keyDown(SDLK_MINUS, repeat); return true;
        case KA::ZoomIn:
        case KA::ZoomOut:
            if (menu && (sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_PAGEDOWN))
            {
                return false; // page through the open list instead
            }
            return true; // held: the projection zoom (updateCamera, Keymap::zoom)
        default: break;
    }
    if (repeat)
    {
        return true;
    }
    switch (*a)
    {
        case KA::ChangeView: firstPerson = !firstPerson; break; // the zoom: 280 in first person (CODE 0x100a5ec0)
        case KA::CameraBehind: camBehind = true; break;
        case KA::Autorun: autorun = !autorun; break;
        case KA::Heal: gameUi.runLine("/heal"); break;
        case KA::WalkRun: walking = !walking; break;
        case KA::Screenshot:
            shotPath    = userScreenshotPath();
            captureNext = true;
            break;
        case KA::HideWindow: gameUi.toggleHidden(); break;
        case KA::SelectWindow: gameUi.selectWindow(); break;
        case KA::Attack: toggleEngage = true; break;
        case KA::Items: gameUi.openShortcut("items"); break;
        case KA::Magic: gameUi.openShortcut("magic"); break;
        case KA::Abilities: gameUi.openShortcut("abilities"); break;
        case KA::WeaponSkills: gameUi.openShortcut("weaponskills"); break;
        case KA::Equipment: gameUi.openShortcut("equip"); break;
        case KA::Check: gameUi.runLine("/check"); break;
        case KA::ChatSay: gameUi.startTyping("/s "); break;
        case KA::ChatParty: gameUi.startTyping("/p "); break;
        case KA::ChatTell: gameUi.startTyping("/t "); break;
        case KA::ChatLinkshell: gameUi.startTyping("/l "); break;
        case KA::ChatLinkshell2: gameUi.startTyping("/l2 "); break;
        case KA::ChatAssist: gameUi.startTyping("/assist "); break;
        case KA::ChatUnity: gameUi.startTyping("/unity "); break;
        case KA::ChatReply: gameUi.startTyping(lastTellFrom.empty() ? "/t " : "/t " + lastTellFrom + " "); break;
        case KA::RangedAttack: gameUi.runLine("/ra"); break;
        case KA::CallForHelp: gameUi.runLine("/callforhelp"); break;
        case KA::PetCommands: gameUi.openShortcut("petcommands"); break;
        case KA::LockTarget: lockOn = client::targetLockToggle(lockOn, client::lockable(target)); break;
        default: spdlog::info("{}: not implemented yet", ui::Keymap::name(*a)); break;
    }
    return true;
}

bool ClientApp::addonInput(const SDL_Event& ev, bool uiMouse)
{
    if (!addons)
    {
        return false;
    }
    switch (ev.type)
    {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            // Every key, also while typing: an addon keeps its own key state (equipmon's Shift) from these.
            const uint32_t vk      = ashita::vkFromSdl(ev.key.key);
            const bool     down    = ev.type == SDL_EVENT_KEY_DOWN;
            const bool     blocked = vk != 0 && addons->key(vk, down, ev.key.repeat);
            return blocked && down;
        }
        case SDL_EVENT_MOUSE_MOTION:
            return addons->mouse(ashita::kWmMouseMove, static_cast<int>(ev.motion.x), static_cast<int>(ev.motion.y), 0);
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            const bool     down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            const uint32_t msg  = ashita::buttonMessage(ev.button.button, down);
            // A press over an ImGui window is that window's; a release always goes (an addon's drag ends with it), and
            // the game always sees releases (the camera orbit ends on the right button's).
            if (msg == 0 || (down && uiMouse))
            {
                return false;
            }
            const bool blocked = addons->mouse(msg, static_cast<int>(ev.button.x), static_cast<int>(ev.button.y), 0);
            return blocked && down;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            if (uiMouse)
            {
                return false;
            }
            return addons->mouse(ashita::kWmMouseWheel, static_cast<int>(ev.wheel.mouse_x), static_cast<int>(ev.wheel.mouse_y),
                                 static_cast<int>(std::lround(ev.wheel.y * static_cast<float>(ashita::kWheelDelta))));
        default: return false;
    }
}

void ClientApp::handleEvents()
{
    // Scripted test runs (--screenshot) ignore real keyboard/mouse input: the window can steal focus on
    // the user's desktop, and stray keystrokes would change the run.
    testRun = !args.screenshot.empty() || script.loaded();
    SDL_Event ev;
    bool      swallowText = false; // the text event of a key that was a game action (Compact letter keys)
    while (SDL_PollEvent(&ev))
    {
        if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_MOTION ||
            ev.type == SDL_EVENT_MOUSE_WHEEL || ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)
        {
            lastInput = std::chrono::steady_clock::now();
        }
        if (testRun && ev.type != SDL_EVENT_QUIT && ev.type != SDL_EVENT_WINDOW_RESIZED && ev.type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
        {
            continue;
        }
        ImGui_ImplSDL3_ProcessEvent(&ev);
        const bool uiMouse = ImGui::GetIO().WantCaptureMouse;
        const bool uiKeys  = ImGui::GetIO().WantTextInput;
        if (ev.type == SDL_EVENT_TEXT_INPUT && swallowText)
        {
            swallowText = false;
            continue;
        }
        // Ashita addons see the window's messages before the game (their WNDPROC events); a blocked one stops here.
        if (addonInput(ev, uiMouse))
        {
            continue;
        }
        if (ev.type == SDL_EVENT_KEY_DOWN)
        {
            // An open key-assignment list takes the key itself (Config → Misc. 2).
            swallowText = inGame && !uiKeys && gameUi.assignKey(ev.key.scancode, ev.key.repeat);
            swallowText = swallowText || (inGame && !uiKeys && !gameUi.typing() && onKey(ev.key.scancode, ev.key.mod, ev.key.repeat));
            if (swallowText)
            {
                continue;
            }
        }
        // In game, the game's interface sees input first (menus, chat line).
        if (uiActive && !uiKeys)
        {
            bool taken = false;
            switch (ev.type)
            {
                case SDL_EVENT_KEY_DOWN:
                {
                    const bool wasTyping = gameUi.typing();
                    taken                = gameUi.keyDown(ev.key.key, ev.key.repeat);
                    // "/" opened the chat line with the slash typed. Text input follows typing() a frame later
                    // (ui_frame.cpp), so it is normally still off here; if it is still on (the line was just sent),
                    // the same key's text event would type a second slash: drop it.
                    swallowText = taken && !wasTyping && gameUi.typing() && uiState_.textInputOn;
                    break;
                }
                case SDL_EVENT_TEXT_INPUT: taken = gameUi.textInput(ev.text.text); break;
                case SDL_EVENT_MOUSE_MOTION: gameUi.mouseMove(ImVec2(ev.motion.x, ev.motion.y)); break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN: taken = gameUi.mouseDown(ev.button.button, ImVec2(ev.button.x, ev.button.y)); break;
                case SDL_EVENT_MOUSE_WHEEL: taken = gameUi.wheel(ev.wheel.y); break;
                default: break;
            }
            if (taken)
            {
                continue;
            }
        }
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && !uiKeys && targetId != 0 && targetLocked())
        {
            // Locked on the monster we fight: Esc keeps the target (target command 17 returns early, CODE 0x10158720).
        }
        else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && !uiKeys && targetId != 0)
        {
            inputState_.targetEscaped = true;
            targetId      = 0; // Esc clears the target first
        }
        else if (ev.type == SDL_EVENT_QUIT || (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && !uiKeys && !uiActive))
        {
            quitting = true; // closing the window; in game, Escape only clears the target (/logout quits)
        }
        else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_TAB && !uiKeys)
        {
            cycleTarget = true;
        }
        else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F11 && !ev.key.repeat)
        {
            showDebug = !showDebug;
        }
        else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT && !uiMouse)
        {
            orbiting = true;
            SDL_SetWindowRelativeMouseMode(window, true);
        }
        else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_RIGHT)
        {
            orbiting = false;
            SDL_SetWindowRelativeMouseMode(window, false);
        }
        else if (ev.type == SDL_EVENT_MOUSE_MOTION && orbiting)
        {
            lastCamInput = std::chrono::steady_clock::now();
            camYaw += ev.motion.xrel * 0.004f * camInvert(true);
            camPitch = std::clamp(camPitch - ev.motion.yrel * 0.004f * camInvert(false), -1.55f, 0.6f);
            // The game camera turns its eye by the same angles (updateCamera; the drag rate is ours, INFERRED).
            camMouseYaw += ev.motion.xrel * 0.004f * camInvert(true);
            camMouseUp += ev.motion.yrel * 0.004f * camInvert(false);
        }
        else if (ev.type == SDL_EVENT_MOUSE_WHEEL && !uiMouse)
        {
            // The wheel moves the same zoom: a notch (WHEEL_DELTA 120, CODE 0x10001dd8) adds 2 to a count the game
            // spends one a frame, each frame moving the zoom 6 a tick (0x1001f7e6, 0x1025dda0): 2 frames, 4 ticks at
            // its 30 frames a second (INFERRED rate), so 24 a notch, here spent over 4 ticks (updateCamera).
            camWheelTicks += ev.wheel.y * 4.0f;
        }
    }
}

void ClientApp::updateTargets()
{
    for (auto& e : world.entities)
    {
        if (e.name.empty())
        {
            if (const auto it = zoneNames.find(e.id); it != zoneNames.end())
            {
                e.name = it->second;
            }
        }
    }

    // --auto-fight: between fights, pick the next mob (the engage follows below, a second later).
    if (args.autoFight && zonedAt && world.inZone && !world.engaged() && now >= nextAuto && world.animation != 2 && world.animation != 3 &&
        std::chrono::duration<float>(now - *zonedAt).count() > 12.0f)
    {
        const auto* cur = [&]() -> const client::Entity* {
            for (const auto& e : world.entities)
            {
                if (e.id == targetId)
                {
                    return &e;
                }
            }
            return nullptr;
        }();
        if (!cur || cur->hpp == 0)
        {
            targetId    = 0;
            cycleTarget = true;
            nextAuto    = now + std::chrono::seconds(2);
        }
    }
    if (args.tab && zonedAt && std::chrono::duration<float>(now - *zonedAt).count() > 4.0f)
    {
        cycleTarget = true;
        args.tab    = false;
        spdlog::info("test: Tab ({} entities in snapshot)", world.entities.size());
    }
    // Targeting: Tab picks the next entity by distance within 30 yalms (wrapping).
    target = nullptr;
    {
        const glm::vec3 me = gfx::ZoneScene::renderToGame(feet);
        std::vector<std::pair<float, const client::Entity*>> cand;
        for (const auto& e : world.entities)
        {
            const float d = glm::distance(e.pos, me);
            if (d < (args.targetName.empty() ? 30.0f : 60.0f) && targetable(e))
            {
                cand.emplace_back(d, &e);
            }
            if (e.id == targetId)
            {
                target = &e;
            }
        }
        if (cycleTarget && !args.targetName.empty())
        {
            // Test hook: nearest living entity whose name contains --target-name.
            // '|' separates alternatives ("Hare|Worm").
            auto matches = [&](const std::string& n) {
                std::size_t b = 0;
                while (b <= args.targetName.size())
                {
                    const auto e = std::min(args.targetName.find('|', b), args.targetName.size());
                    if (e > b && n.find(args.targetName.substr(b, e - b)) != std::string::npos)
                    {
                        return true;
                    }
                    b = e + 1;
                }
                return false;
            };
            std::erase_if(cand, [&](const auto& c) { return !matches(c.second->name) || c.second->hpp == 0; });
            if (cand.empty())
            {
                spdlog::warn("test: no '{}' within 60 yalms", args.targetName);
            }
        }
        // Engaged, the target is locked on the monster we fight: Tab does nothing until it dies or we disengage (the
        // game's target commands but Confirm return early while target state +0x5c & 3 is set, CODE 0x10158720).
        // Picks that start from no target (--auto-fight, a script's "target" step) still run.
        if (cycleTarget && targetId != 0 && targetLocked())
        {
            spdlog::info("target: locked on the battle target (engaged)");
            cycleTarget = false;
        }
        if (cycleTarget && !cand.empty())
        {
            std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            std::size_t next = 0;
            for (std::size_t i = 0; i < cand.size(); ++i)
            {
                if (cand[i].second->id == targetId)
                {
                    next = (i + 1) % cand.size();
                }
            }
            target   = cand[next].second;
            targetId = target->id;
            spdlog::info("target: {} (index {}, look {}, kind {} id {:#x}{})", target->name, target->index, lookKey(target->look), target->look.kind, target->look.ids[0], target->look.valid ? "" : " invalid");
            if (session)
            {
                session->setPlayer(me, heading, target->index);
            }
        }
        cycleTarget = false;
        // Ourselves (F1, <me> commands): not in the entity list, so a stand-in built from our own state.
        if (!target && targetId != 0 && targetId == world.charId)
        {
            selfEntity       = {};
            selfEntity.id    = world.charId;
            selfEntity.index = world.actIndex;
            selfEntity.name  = world.charName;
            selfEntity.pos   = me;
            selfEntity.hpp   = world.hpp;
            selfEntity.isPc  = true;
            target           = &selfEntity;
        }
        // A monster that dies is dropped from the target (its name greys meanwhile; nameColour).
        if (!target || (target->isMob && target->hpp == 0))
        {
            target   = nullptr;
            targetId = 0;
        }
    }
    // Target sounds (CODE 0x10157a70 -> 0x10157b26 / 0x10157b2d: 0010 / 0009); Escape
    // clearing it 0013 (0x10157e44). Dropped for other reasons (death, out of range): silent.
    if (targetId != inputState_.soundTargetSeen)
    {
        if (targetId != 0)
        {
            sound.playUi(10u); // the user hears 0010 on every Tab in retail, the first too (0009's case not identified)
        }
        else if (inputState_.targetEscaped)
        {
            sound.playUi(13u);
        }
        inputState_.soundTargetSeen = targetId;
    }
    inputState_.targetEscaped = false;
}

bool ClientApp::targetable(const client::Entity& e) const
{
    // The game's target filter (0x10083360) also rejects actors with the TargetOffFlag (0x100844b0).
    return !e.name.empty() && !e.targetOff && !(ignoreTrusts && isTrust(e, world)) && !(ignorePet && world.petIndex != 0 && e.index == world.petIndex) &&
           !(e.isMob && e.hpp == 0) && // a dead monster is no longer targetable
           !e.hiddenByStatus(cutscene.has_value());
}

const client::Entity* ClientApp::frontTarget() const
{
    // Target command 1 with no target (CODE 0x10158720 -> 0x10083550): each actor passing the target filter
    // (0x10083360) is moved into our frame (our position and heading, 0x10028160); of those with local x > 0, in front
    // of us, the nearest by squared distance under 2500 (50 yalms) is picked. The filter's flags (target object
    // +0x60 / +0x64) are not decoded: Tab's filter stands in for them (INFERRED).
    const glm::vec3       me   = gfx::ZoneScene::renderToGame(feet);
    const float           a    = static_cast<float>(heading) * 3.14159265f / 128.0f;
    const glm::vec3       face = glm::vec3(std::cos(a), 0.0f, -std::sin(a)); // as fishing.cpp: game x / z
    const client::Entity* best = nullptr;
    float                 bestD2 = 2500.0f;
    for (const auto& e : world.entities)
    {
        const glm::vec3 v  = e.pos - me;
        const float     d2 = glm::dot(v, v);
        if (d2 < bestD2 && glm::dot(glm::vec3(v.x, 0.0f, v.z), face) > 0.0f && targetable(e))
        {
            best   = &e;
            bestD2 = d2;
        }
    }
    return best;
}

bool ClientApp::targetLocked() const
{
    // One lock (target object +0x5c): Num *, /lockon and engaging set it; it goes when the target dies or leaves (CODE
    // 0x10157620) and when we disengage (0x1008b722). See client::targetLockStep.
    return lockOn && client::lockable(target);
}

void ClientApp::handleActions()
{
    if (inputState_.confirmKey)
    {
        // Confirm with no menu open: talk to a targeted NPC, the battle / player menu on a monster / player, with
        // nothing targeted the entity in front of us; knocked out, the home point prompt.
        inputState_.confirmKey = false;
        if (!gameUi.menuOpen() && (mogView.layoutMode() || (target && app::MogHouseView::isLocal(target->id))))
        {
            mogView.interact(target, world, gameUi); // the Mog House door, or a layout menu
        }
        else if (!gameUi.menuOpen() && target && !target->isPc && !target->isMob && world.animation != 2 && world.animation != 3)
        {
            spdlog::info("confirm: talk to {}", target->name);
            talk = true;
        }
        else if (!gameUi.menuOpen() && !gameUi.typing() && target && target->isMob && target->hpp > 0 && world.animation != 2 &&
                 world.animation != 3)
        {
            gameUi.openBattleMenu(world.engaged() ? 6 : 4); // Ready Mode / Battle Mode (CODE 0x1012b3e0)
        }
        else if (!gameUi.menuOpen() && !gameUi.typing() && target && target->isPc && world.animation != 2 && world.animation != 3)
        {
            // A player, ourselves included (mode 2): Chat / Magic / Abilities / Trust / Items / Trade / Invite / Check.
            gameUi.openBattleMenu(world.engaged() ? 6 : 2);
        }
        else if (!gameUi.menuOpen() && !gameUi.typing() && !target && world.animation != 2 && world.animation != 3)
        {
            // Nothing targeted (CODE 0x10158520, target command 1): Enter targets what is in front of us; with nothing
            // there it opens "menu playermo" (0x1015e260; "menu mp_pmode" in the state 0x10097c80 tests, not
            // modelled). It never opens the chat line (#72). The mode with no target, 2 (6 engaged), is INFERRED; its
            // rows then match the user's retail capture (Chat, Magic, Abilities, Trust, Items, Trade, Check).
            if (const auto* e = frontTarget())
            {
                target   = e;
                targetId = e->id;
                spdlog::info("confirm: target {} in front", e->name);
                if (session)
                {
                    session->setPlayer(gfx::ZoneScene::renderToGame(feet), heading, e->index);
                }
            }
            else
            {
                gameUi.openBattleMenu(world.engaged() ? 6 : 2);
            }
        }
        else
        {
            // Knocked out: the home point prompt (GameUi).
            gameUi.keyDown(SDLK_RETURN, false);
        }
    }
    if (talk && target && app::MogHouseView::isLocal(target->id))
    {
        mogView.interact(target, world, gameUi);
    }
    else if (talk && session && target && !target->isPc)
    {
        session->action(target->id, target->index, 0); // Talk
    }
    talk = false;
    if (toggleEngage && session && world.inZone)
    {
        // The Attack key: engage the target; engaged, switch to another targeted monster (ChangeTarget
        // 0x0F) or, on the same one, disengage.
        if (world.engaged() && target && target->isMob && target->hpp > 0 && target->id != inputState_.engagedId)
        {
            session->action(target->id, target->index, 0x0F);
            inputState_.engagedId = target->id;
        }
        else if (world.engaged())
        {
            session->action(target ? target->id : 0, target ? target->index : 0, 4);
            inputState_.engagedId = 0;
        }
        else if (target && !target->isPc)
        {
            session->action(target->id, target->index, 2);
            inputState_.engagedId = target->id;
        }
    }
    toggleEngage = false;
    if (!world.engaged())
    {
        inputState_.engagedId = 0;
    }
    else if (inputState_.engagedId == 0 && target && target->isMob)
    {
        inputState_.engagedId = target->id; // engaged some other way (battle menu, /attack, a macro)
    }
    // The target lock: engaging locks on, disengaging or the target going clears it (client::targetLockStep).
    lockOn = client::targetLockStep(lockOn, world.engaged(), inputState_.lockWasEngaged, client::lockable(target));
    inputState_.lockWasEngaged = world.engaged();
    // Auto-target: the server picks the next monster on us and says so with S2C 0x058 (session.autoTarget).
    if (session && world.autoTargetSerial != inputState_.autoTargetSeen)
    {
        inputState_.autoTargetSeen = world.autoTargetSerial;
        if (world.autoTargetId != 0)
        {
            targetId  = world.autoTargetId;
            inputState_.engagedId = world.autoTargetId;
        }
    }
}

} // namespace xi::app
