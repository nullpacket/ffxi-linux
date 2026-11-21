// SPDX-License-Identifier: GPL-3.0-or-later
// Macros and the timed test hooks (--ui-keys, --attack, --cast-at, --ws-at, --talk-at, --ja-at, --equip-item, --use-item).
#include "client_app.h"

#include "ashita/input_map.h"

namespace xi::app
{

void ClientApp::runUiKeys()
{
    // Macros, once per character: the game's own files (ui-format-notes §7.9) in a USER/<content id, hex> folder
    // chosen by [ffxi-linux] macro_dir: empty = our copy in ~/.config/ffxi-linux/USER/<id> (XDG_CONFIG_HOME honoured),
    // filled once from the install's USER/<id>, or else from our older macros/<character>.txt; "install" = the
    // install's own folder (shared with the Windows client); anything else = <that folder>/USER/<id>.
    if (uiActive && !world.charName.empty() && world.charId != 0 && macrosFor != world.charName)
    {
        macrosFor = world.charName;
        const char*           xdg  = std::getenv("XDG_CONFIG_HOME");
        const char*           home = std::getenv("HOME");
        std::filesystem::path dir  = xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(home ? home : ".") / ".config";
        const auto            id   = fmt::format("{:x}", world.charId); // LSB's content id is the character id
        const auto&           md   = args.display_.macroDir;
        const auto install = std::filesystem::path(args.root) / "USER" / id;
        auto       userDir = md == "install" ? install : !md.empty() ? std::filesystem::path(md) / "USER" / id : dir / "ffxi-linux" / "USER" / id;
        auto hasMacros     = [](const std::filesystem::path& p) {
            std::error_code ec;
            return std::filesystem::exists(p / "mcr.dat", ec) || std::filesystem::exists(p / "mcr.ttl", ec);
        };
        const auto oldText = dir / "ffxi-linux" / "macros" / (world.charName + ".txt");
        bool       migrate = false;
        if (md.empty() && !hasMacros(userDir))
        {
            std::error_code ec;
            std::filesystem::create_directories(userDir, ec);
            if (hasMacros(install))
            {
                // Copy the game's macro files only (mcr*.dat / .ttl / .sys).
                for (const auto& f : std::filesystem::directory_iterator(install, ec))
                {
                    const auto n = f.path().filename().string();
                    if (n.rfind("mcr", 0) == 0 && (n.ends_with(".dat") || n.ends_with(".ttl") || n.ends_with(".sys")))
                    {
                        std::filesystem::copy_file(f.path(), userDir / n, std::filesystem::copy_options::skip_existing, ec);
                    }
                }
                spdlog::info("macros: copied the game's files from {}", install.string());
            }
            else
            {
                migrate = std::filesystem::exists(oldText, ec);
            }
        }
        gameUi.loadMacroDir(userDir);
        if (migrate)
        {
            gameUi.loadMacros(oldText); // our older text format, written out as the game's files
        }
        gameUi.loadEquipSets(dir / "ffxi-linux" / "equipsets" / (world.charName + ".txt"));
    }
    // Before zone-in (the lobby) the keys count from the connect instead.
    const auto keysFrom = scriptKeysFrom ? scriptKeysFrom : zonedAt ? zonedAt : connectedAt;
    if (!args.uiKeys.empty() && keysFrom && std::chrono::duration<float>(now - *keysFrom).count() > 5.0f + 0.4f * static_cast<float>(uiKeyStep))
    {
        const auto comma = args.uiKeys.find(',');
        const auto key   = args.uiKeys.substr(0, comma);
        args.uiKeys      = comma == std::string::npos ? std::string() : args.uiKeys.substr(comma + 1);
        ++uiKeyStep;
        static const std::pair<const char*, SDL_Keycode> kKeys[] = { { "up", SDLK_UP },       { "down", SDLK_DOWN }, { "left", SDLK_LEFT },
                                                                     { "right", SDLK_RIGHT }, { "enter", SDLK_RETURN }, { "esc", SDLK_ESCAPE },
                                                                     { "minus", SDLK_MINUS }, { "space", SDLK_SPACE }, { "pgup", SDLK_PAGEUP },
                                                                     { "f1", SDLK_F1 },       { "f2", SDLK_F2 },     { "f3", SDLK_F3 }, { "tab", SDLK_TAB } };
        if (key.rfind("t:", 0) == 0)
        {
            gameUi.textInput(key.substr(2));
        }
        // "key:[ctrl+|alt+|shift+]NAME": a physical key (SDL scancode name, e.g. "Keypad 7", "A") through the
        // game's bindings.
        if (key.rfind("key:", 0) == 0)
        {
            std::string name = key.substr(4);
            int         mods = SDL_KMOD_NONE;
            for (const auto& [pre, m] : { std::pair{ "ctrl+", SDL_KMOD_LCTRL }, std::pair{ "alt+", SDL_KMOD_LALT }, std::pair{ "shift+", SDL_KMOD_LSHIFT } })
            {
                if (name.rfind(pre, 0) == 0)
                {
                    name = name.substr(std::strlen(pre));
                    mods |= m;
                }
            }
            const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN || !onKey(sc, static_cast<SDL_Keymod>(mods), false))
            {
                spdlog::warn("test: key '{}' is not a game action", name);
            }
        }
        // "cam:YAW;UP;SECONDS": hold the camera keys' axes (-1..1, as client::GameCamera::Input) for that long;
        // "drag:DX;DY": a right-button orbit drag of that many pixels.
        if (key.rfind("cam:", 0) == 0)
        {
            float y = 0, u = 0, sec = 1;
            std::sscanf(key.c_str() + 4, "%f;%f;%f", &y, &u, &sec);
            camHoldYaw   = y;
            camHoldUp    = u;
            camHoldUntil = now + std::chrono::milliseconds(static_cast<int>(sec * 1000.0f));
        }
        // "camat:YAW;PITCH;DIST": the camera placed at that view of the character (degrees, yalms; world yaw), for
        // repeatable comparison shots (goal 41).
        if (key.rfind("camat:", 0) == 0)
        {
            float y = 0, p = -20, d = 5.5f;
            std::sscanf(key.c_str() + 6, "%f;%f;%f", &y, &p, &d);
            camAtYaw     = y * 3.14159265f / 180.0f;
            camAtPitch   = p * 3.14159265f / 180.0f;
            camAtDist    = d;
            camAtPending = true;
        }
        // Addon input as the window messages (#86): "akey:VK;1|0" a virtual key down / up, "amouse:MSG;X;Y[;DELTA]" a
        // mouse message (decimal or 0x hex), "adrag:X1;Y1;X2;Y2" Shift + left drag from one point to the other.
        if (addons && key.rfind("akey:", 0) == 0)
        {
            int vk = 0, down = 1;
            std::sscanf(key.c_str() + 5, "%i;%i", &vk, &down);
            addons->key(static_cast<uint32_t>(vk), down != 0, false);
        }
        if (addons && key.rfind("amouse:", 0) == 0)
        {
            int msg = 0, x = 0, y = 0, delta = 0;
            std::sscanf(key.c_str() + 7, "%i;%i;%i;%i", &msg, &x, &y, &delta);
            spdlog::info("test: addon mouse 0x{:x} at {},{}: {}", msg, x, y, addons->mouse(static_cast<uint32_t>(msg), x, y, delta) ? "blocked" : "passed");
        }
        if (addons && key.rfind("adrag:", 0) == 0)
        {
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            std::sscanf(key.c_str() + 6, "%i;%i;%i;%i", &x1, &y1, &x2, &y2);
            addons->key(ashita::kVkShift, true, false);
            const bool took = addons->mouse(ashita::kWmLButtonDown, x1, y1, 0);
            for (int i = 1; i <= 4; ++i)
            {
                addons->mouse(ashita::kWmMouseMove, x1 + (x2 - x1) * i / 4, y1 + (y2 - y1) * i / 4, 0);
            }
            addons->mouse(ashita::kWmLButtonUp, x2, y2, 0);
            addons->key(ashita::kVkShift, false, false);
            spdlog::info("test: addon shift-drag {},{} -> {},{}: {}", x1, y1, x2, y2, took ? "taken" : "nothing there");
        }
        if (key.rfind("wheel:", 0) == 0) // "wheel:N": N notches of the mouse wheel (+ zooms in)
        {
            camWheelTicks += std::strtof(key.c_str() + 6, nullptr) * 4.0f;
        }
        if (key.rfind("drag:", 0) == 0)
        {
            float dx = 0, dy = 0;
            std::sscanf(key.c_str() + 5, "%f;%f", &dx, &dy);
            camMouseYaw += dx * 0.004f * camInvert(true);
            camMouseUp += dy * 0.004f * camInvert(false);
        }
        // "mods:ctrl+alt+shift" holds those modifiers for the UI's own key checks (the macro keys, the palette) until
        // "mods:" clears them; "k:NAME" presses one key (SDL key name: "1", "Up", "Return") on the UI as typed.
        if (key.rfind("mods:", 0) == 0)
        {
            int m = SDL_KMOD_NONE;
            m |= key.find("ctrl") != std::string::npos ? SDL_KMOD_LCTRL : 0;
            m |= key.find("alt") != std::string::npos ? SDL_KMOD_LALT : 0;
            m |= key.find("shift") != std::string::npos ? SDL_KMOD_LSHIFT : 0;
            gameUi.setTestMods(static_cast<SDL_Keymod>(m));
        }
        if (key.rfind("k:", 0) == 0)
        {
            if (const SDL_Keycode code = SDL_GetKeyFromName(key.c_str() + 2); code != SDLK_UNKNOWN)
            {
                gameUi.keyDown(code, false);
            }
        }
        if (key.rfind("sel", 0) == 0 && key == "select") // Select Window
        {
            gameUi.selectWindow();
        }
        if (key.rfind("assign:", 0) == 0) // a key pressed on an open key-assignment list
        {
            gameUi.assignKey(SDL_GetScancodeFromName(key.substr(7).c_str()));
        }
        if (key.rfind("n:", 0) == 0) // digits for a number window
        {
            for (const char c : key.substr(2))
            {
                if (c >= '0' && c <= '9')
                {
                    gameUi.keyDown(static_cast<SDL_Keycode>(SDLK_0 + (c - '0')), false);
                }
            }
        }
        if ((key.rfind("ctrl", 0) == 0 || key.rfind("alt", 0) == 0) && key.size() > 3)
        {
            const bool ctrl = key[0] == 'c';
            gameUi.runMacro(ctrl ? 0 : 1, std::atoi(key.c_str() + (ctrl ? 4 : 3)));
        }
        if (key == "tab")
        {
            cycleTarget = true; // with --target-name: the nearest match
        }
        bool known = key == "tab" || key == "select" || key.find(':') != std::string::npos || key.rfind("ctrl", 0) == 0 || key.rfind("alt", 0) == 0;
        for (const auto& [name, code] : kKeys)
        {
            if (key == name || (key == "return" && code == SDLK_RETURN))
            {
                gameUi.keyDown(code, false);
                known = true;
            }
        }
        if (known)
        {
            spdlog::info("test: ui key {}", key);
        }
        else
        {
            spdlog::warn("test: ui key '{}' is unknown (up, down, left, right, enter, esc, minus, space, pgup, f1-f3, tab, t:, key:, n:)", key);
        }
    }
}

void ClientApp::runTestHooks()
{
    // Combat: F engages the target (C2S 0x01A Attack) or disengages (AttackOff).
    if (args.attackAt >= 0.0f && zonedAt && std::chrono::duration<float>(now - *zonedAt).count() > args.attackAt)
    {
        toggleEngage  = true;
        args.attackAt = -1.0f;
        spdlog::info("test: F (target {})", target ? target->name : "none");
    }
    // Equip an inventory item into the first slot its mask allows (ear / ring: the first free one).
    auto equipItem = [&](const client::InventoryItem& it) {
        const auto info = items.find(it.itemId);
        if (!info || !info->equipment() || !session)
        {
            return false;
        }
        for (uint8_t slot = 0; slot < 16; ++slot)
        {
            if (!(info->slots & (1u << slot)))
            {
                continue;
            }
            const bool pair = slot == 11 || slot == 13; // ear1 / ring1: take the second when the first is used
            if (pair && world.equipment[slot].second != 0 && (info->slots & (1u << (slot + 1))))
            {
                continue;
            }
            session->equip(it.container, it.slot, slot);
            return true;
        }
        return false;
    };
    if (args.equipItem >= 0 && zonedAt && std::chrono::duration<float>(now - *zonedAt).count() > 6.0f)
    {
        bool done = false;
        for (const auto& it : world.inventory)
        {
            if (it.itemId == args.equipItem && it.container == 0)
            {
                done = equipItem(it);
                break;
            }
        }
        spdlog::info("test: equip item {}: {}", args.equipItem, done ? "sent" : "not in inventory / not equipment");
        args.equipItem = -1;
    }
    if (args.autoFight && session && target && target->hpp > 0 && now >= nextAuto && world.animation != 2 && world.animation != 3)
    {
        if (!world.engaged() && glm::distance(target->pos, gfx::ZoneScene::renderToGame(feet)) < 25.0f)
        {
            toggleEngage = true;
            nextAuto     = now + std::chrono::seconds(3);
        }
        else if (world.engaged() && args.autoRanged)
        {
            spdlog::info("test: ranged attack on {}", target->name);
            session->action(target->id, target->index, 0x10);
            nextAuto = now + std::chrono::seconds(6);
        }
        else if (world.engaged() && world.tp >= 1000 && !world.weaponSkills.empty() && args.autoWs)
        {
            spdlog::info("test: weapon skill {} at TP {}", world.weaponSkills.front(), world.tp);
            session->action(target->id, target->index, 7, world.weaponSkills.front());
            nextAuto = now + std::chrono::seconds(3);
        }
    }
    if (args.castAt >= 0.0f && zonedAt && std::chrono::duration<float>(now - *zonedAt).count() > args.castAt)
    {
        args.castAt = -1.0f;
        bool sent   = false;
        for (const auto id : world.spells)
        {
            const auto* sp = spellData ? spellData->find(id) : nullptr;
            if (sp && target && !target->isPc && (sp->targets & 0x20) && sp->usableBy(world.mainJob, world.mainLevel) && world.mp >= sp->mp)
            {
                spdlog::info("test: cast spell {} on {}", id, target->name);
                session->action(target->id, target->index, 3, id);
                sent = true;
                break;
            }
        }
        for (const auto id : world.spells) // else a spell we can cast on ourselves
        {
            const auto* sp = spellData ? spellData->find(id) : nullptr;
            if (!sent && sp && (sp->targets & 0x01) && sp->usableBy(world.mainJob, world.mainLevel) && world.mp >= sp->mp)
            {
                spdlog::info("test: cast spell {} on self", id);
                session->action(world.charId, world.actIndex, 3, id);
                sent = true;
            }
        }
        if (!sent)
        {
            spdlog::warn("test: nothing to cast ({} known spells, MP {})", world.spells.size(), world.mp);
        }
    }
    if (args.wsAt >= 0.0f && zonedAt && std::chrono::duration<float>(now - *zonedAt).count() > args.wsAt)
    {
        args.wsAt = -1.0f;
        if (session && target && !world.weaponSkills.empty())
        {
            spdlog::info("test: weapon skill {} (TP {})", world.weaponSkills.front(), world.tp);
            session->action(target->id, target->index, 7, world.weaponSkills.front());
        }
        else
        {
            spdlog::warn("test: no weapon skill or target");
        }
    }
    if (args.talkAt >= 0.0f && zonedAt && std::chrono::duration<float>(now - *zonedAt).count() > args.talkAt)
    {
        args.talkAt = -1.0f;
        talk        = true;
        spdlog::info("test: E (target {})", target ? target->name : "none");
    }
    if (args.jaAt >= 0.0f && zonedAt && std::chrono::duration<float>(now - *zonedAt).count() > args.jaAt)
    {
        args.jaAt = -1.0f;
        if (session && !world.jobAbilities.empty())
        {
            const auto id = args.jaId >= 0 ? static_cast<uint16_t>(args.jaId) : world.jobAbilities.front();
            spdlog::info("test: job ability {}", id);
            session->action(world.charId, world.actIndex, 9, id);
        }
    }
    if (args.useItemId >= 0 && zonedAt && std::chrono::duration<float>(now - *zonedAt).count() > 8.0f)
    {
        for (const auto& it : world.inventory)
        {
            if (it.itemId == args.useItemId && it.container == 0)
            {
                const auto info = items.find(it.itemId);
                spdlog::info("test: use item {} '{}' (type {})", it.itemId, info ? info->name : "?", info ? info->type : 0);
                session->useItem(it.container, it.slot, world.charId, world.actIndex);
                break;
            }
        }
        args.useItemId = -1;
    }
}

} // namespace xi::app
