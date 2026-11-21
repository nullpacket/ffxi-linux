// SPDX-License-Identifier: GPL-3.0-or-later
// The interface: login form, lobby, the game interface (ui::GameUi) and the debug overlay.
#include "client_app.h"

namespace xi::app
{

void ClientApp::drawInterface()
{
    // --- UI ---
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ww = 0;
    wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    ++uiState_.uiTick;
    // Interface canvas: the menu resolution (registry 0037/0038) scaled to the window.
    const auto         mc = args.display_.canvasFor(static_cast<float>(ww), static_cast<float>(wh));
    canvas = ui::UiCanvas{ mc.width, mc.height, mc.scale };
    if (!args.screenshot.empty() && !session && !args.autoconnect && uiState_.uiTick == 30)
    {
        captureNext = true; // debug views are captured before any login
    }
    if (const auto sh = uiRes.sheets.find(args.uiSheet); sh != uiRes.sheets.end())
    {
        // Debug grid: sprite n of the sheet in a 128-unit cell, numbered.
        auto&          dl = *ImGui::GetBackgroundDrawList();
        ui::UiPainter  paint(uiR, dl, 1.0f);
        const int      cols = std::max(1, ww / 136);
        for (int i = 0; i + args.uiFirst < static_cast<int>(sh->second.sprites.size()) && i < cols * std::max(1, wh / 136); ++i)
        {
            const ImVec2 cell(static_cast<float>(8 + (i % cols) * 136), static_cast<float>(8 + (i / cols) * 136));
            paint.sprite(sh->second.sprites[static_cast<std::size_t>(i + args.uiFirst)], ImVec2(cell.x + 16, cell.y + 16));
            paint.text(std::to_string(i + args.uiFirst), cell, { 0x80, 0x80, 0x20, 0x80 });
        }
    }
    for (const auto& name : args.uiLayout)
    {
        if (const auto it = uiRes.layouts.find(name); it != uiRes.layouts.end())
        {
            ui::UiPainter     paint(uiR, *ImGui::GetBackgroundDrawList(), canvas.scale);
            ui::LayoutState   st;
            st.cursor = 1;
            st.tick   = uiState_.uiTick;
            ui::drawLayout(paint, canvas, it->second, st);
        }
    }

    if (!session)
    {
        // The login form in the game's frame and font (ui/game_ui.h).
        if (args.uiSheet.empty() && args.uiLayout.empty() && gameUi.drawLogin(*ImGui::GetForegroundDrawList(), canvas, loginFields))
        {
            std::snprintf(host, sizeof(host), "%s", loginFields.host.c_str());
            std::snprintf(user, sizeof(user), "%s", loginFields.user.c_str());
            std::snprintf(pass, sizeof(pass), "%s", loginFields.pass.c_str());
            std::snprintf(chr, sizeof(chr), "%s", loginFields.character.c_str());
            loginFields.notice.clear();
            connect();
        }
        if (gameUi.typing() != uiState_.textInputOn)
        {
            uiState_.textInputOn = gameUi.typing();
            uiState_.textInputOn ? SDL_StartTextInput(window) : SDL_StopTextInput(window);
        }
    }
    else
    {
        // Debug overlay (F11): status line and nearby entities.
        const auto g = gfx::ZoneScene::renderToGame(feet);
        if (showDebug)
        {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("Status", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::Text("%s  -  %s", world.charName.c_str(), world.status.c_str());
        if (world.mainLevel)
        {
            ImGui::Text("%s%u/%s%u", jobName(world.mainJob), world.mainLevel, jobName(world.subJob), world.subLevel);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.3f, 0.3f, 1));
            ImGui::ProgressBar(world.hpp / 100.0f, ImVec2(220, 0), fmt::format("HP {}", world.hp).c_str());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.5f, 0.9f, 1));
            ImGui::ProgressBar(world.mpp / 100.0f, ImVec2(220, 0), fmt::format("MP {}", world.mp).c_str());
            ImGui::PopStyleColor();
            ImGui::Text("TP %u", world.tp);
        }
        ImGui::Text("zone %u   pos %.1f, %.1f, %.1f", world.zone, g.x, g.y, g.z);
        {
            const double vh = client::vanaHourOfDay();
            static const char* kWeatherNames[] = { "clear", "sunshine", "clouds", "fog", "hot spell", "heat wave", "rain", "squall", "dust storm", "sand storm",
                                                   "wind", "gales", "snow", "blizzards", "thunder", "thunderstorms", "auroras", "stellar glare", "gloom", "darkness" };
            ImGui::Text("Vana'diel %02d:%02d   %s", static_cast<int>(vh), static_cast<int>((vh - std::floor(vh)) * 60.0),
                        world.weather < std::size(kWeatherNames) ? kWeatherNames[world.weather] : "?");
        }
        if (scene)
        {
            ImGui::Text("%u draws, %llu tris, %.0f fps", scene->stats().draws, static_cast<unsigned long long>(scene->stats().triangles), fps);
        }
        if (!zoneError.empty())
        {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", zoneError.c_str());
        }
        if (quitting)
        {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Logging out...");
        }
        ImGui::End();

        // Nearby entities
        std::vector<std::pair<float, const client::Entity*>> near;
        for (const auto& e : world.entities)
        {
            near.emplace_back(glm::distance(e.pos, g), &e);
        }
        std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(ww) - 290.0f, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(280, 300), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("Nearby");
        if (ImGui::BeginTable("near", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            for (std::size_t i = 0; i < near.size() && i < 60; ++i)
            {
                const auto& e = *near[i].second;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(e.isPc ? ImVec4(0.5f, 0.8f, 1, 1) : ImVec4(1, 0.8f, 0.5f, 1), "%s",
                                   e.name.empty() ? fmt::format("#{:x}", e.id).c_str() : e.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", near[i].first);
                ImGui::TableNextColumn();
                ImGui::Text("%u%%", e.hpp);
            }
            ImGui::EndTable();
        }
        ImGui::End();
        }

        // The game's own interface (layouts from the menu DATs): chat log, target, party, menus.
        {
            ui::GameUiData uiData;
            uiData.world     = &world;
            uiData.shownResults = &shownResults;
            uiData.target    = target;
            uiData.battleTarget = world.engaged() ? inputState_.engagedId : 0;
            uiData.targetLocked = targetLocked();
            uiData.items     = &items;
            uiData.text      = battleText.get();
            uiData.spells    = spellData ? &*spellData : nullptr;
            uiData.abilities = abilityData ? &*abilityData : nullptr;
            uiData.help      = helpText ? &*helpText : nullptr;
            uiData.filterText = filterText ? &*filterText : nullptr;
            uiData.keyItems  = keyItemNames ? &*keyItemNames : nullptr;
            uiData.titles    = windowTitles ? &*windowTitles : nullptr;
            uiData.clientText = clientText ? &*clientText : nullptr;
            uiData.formats    = formatText ? &*formatText : nullptr;
            uiData.regions   = regionNames ? &*regionNames : nullptr;
            uiData.unityText = unityText ? &*unityText : nullptr;
            uiData.dropText     = dropText ? &*dropText : nullptr;
            uiData.charTitles = charTitles ? &*charTitles : nullptr;
            uiData.emotes    = emoteList ? &*emoteList : nullptr;
            uiData.emoteText = emoteText ? &*emoteText : nullptr;
            uiData.questNames   = questNames ? &*questNames : nullptr;
            uiData.missionNames = missionNames ? &*missionNames : nullptr;
            uiData.index     = &index;
            uiData.areaShort = areaShort ? &*areaShort : nullptr;
            uiData.maps      = &zoneMaps;
            {
                const auto own = gfx::ZoneScene::renderToGame(feet);
                uiData.playerX = own.x;
                uiData.playerZ = own.z;
                uiData.playerY = own.y;
                uiData.heading = heading;
                // render forward (sin yaw, ., -cos yaw) is game (sin yaw, ., cos yaw): render z = -game z
                uiData.camForwardX = std::sin(camYaw);
                uiData.camForwardZ = std::cos(camYaw);
                // The client casts a short vertical ray at the player; the highest floor within ~0.5 above the feet.
                uiData.mapId   = scene ? static_cast<int8_t>(scene->mapIdAt(feet.x, feet.z, feet.y + 0.5f)) : 0;
                uiData.mapIdAt = [&scene = scene](float x, float y, float z) {
                    const auto r = gfx::ZoneScene::gameToRender({ x, y, z });
                    return scene ? static_cast<int>(static_cast<int8_t>(scene->mapIdAt(r.x, r.z, r.y + 0.5f))) : 0;
                };
            }
            uiData.now       = now;
            uiData.dead      = dead && world.inZone;
            uiData.windowSkin = args.display_.windowSkin;
            uiData.keyboard   = args.display_.keyboard;
            args.display_.gameConfig[9]  = args.display_.soundVolume; // Config's SE / BGM volume sliders
            args.display_.gameConfig[10] = args.display_.musicVolume;
            uiData.gameConfig            = &args.display_.gameConfig;
            uiData.chatFilterLists       = chatFilter->size() > 0;
            uiData.fishingRunning        = [&](uint32_t name) {
                const auto* f = fishingFilesOf(world.charId);
                return f && effects.runningFile((*f)[0], name, world.charId);
            };
            ui::GameUiActions uiActions;
            uiActions.action  = [&](uint32_t id, uint16_t idx, uint16_t a, uint32_t prm) {
                session->action(id, idx, a, prm);
                if (a == 2 || a == 0x0F)
                {
                    inputState_.engagedId = id; // engage / change target: the monster the lock follows
                }
            };
            uiActions.equip   = [&](uint8_t c, uint8_t sl, uint8_t eq) { session->equip(c, sl, eq); };
            uiActions.useItem = [&](uint8_t c, uint8_t sl, uint32_t tid, uint16_t tidx) { session->useItem(c, sl, tid, tidx); };
            uiActions.send    = [&](uint16_t id, std::vector<std::byte> payload) { session->send(id, std::move(payload)); };
            uiActions.answerInvite = [&](bool accept) { session->answerPartyInvite(accept); };
            uiActions.note         = [&](const std::string& t) { session->note(t); };
            uiActions.stdNote      = [&](uint16_t m, const std::string& s0) { session->stdNote(m, s0); };
            uiActions.logout       = [&](bool shutdown) { session->requestLogout(shutdown); }; // the server counts down
            // Config changes apply at once and are saved to client.ini's [ffxi-linux] section.
            auto saveSetting = [&](const char* key, int value) {
                if (const auto file = DisplaySettings::userFile(); file && !DisplaySettings::saveOwn(*file, key, value))
                {
                    spdlog::warn("config: could not save {} to {}", key, file->string());
                }
            };
            uiActions.setConfig = [&](int id, int value) {
                // Config pages (config_re.md): stored as cfg_<id>; a few act here. Character data public / hidden
                // (pseudo id -1) and auto-target (7) go to the server as the /anon and /autotarget commands do.
                if (id == -1)
                {
                    uiActions.say(value ? "/anon on" : "/anon off", 0);
                    return;
                }
                args.display_.gameConfig[id] = value;
                if (id == 9 || id == 10)
                {
                    (id == 9 ? args.display_.soundVolume : args.display_.musicVolume) = value;
                    sound.setVolumes(args.display_.soundVolume, args.display_.musicVolume);
                    saveSetting(id == 9 ? "sound_volume" : "music_volume", value);
                    return;
                }
                if (id == 7)
                {
                    uiActions.say(value == 0 ? "/autotarget on" : "/autotarget off", 0);
                }
                if (id == 191)
                {
                    uiActions.say(value == 1 ? "/lockstyle on" : "/lockstyle off", 0); // C2S 0x053 Set / Disable
                    return;
                }
                if (id == 59)
                {
                    effects.setWeatherParticles(value != 0);
                }
                if (id == 58)
                {
                    shadows.setMode(value); // shadows: 2 detailed, 0 simple, 1 none
                }
                if (id == 67)
                {
                    gfx::configuredAspect() = gfx::aspectOption(value); // aspect ratio: the projection's shape
                }
                if (id >= 55 && id <= 57)
                {
                    gammaPass.setValues(cfg(55, 50), cfg(56, 50), cfg(57, 50)); // gamma R / G / B
                }
                if (id == 206)
                {
                    value = args.display_.gameConfig[id] = applyChatFilter(value); // chat filter: re-reads the lists
                }
                if (id == 24)
                {
                    applyChatFilter(args.display_.gameConfig.contains(206) ? args.display_.gameConfig.at(206) : 0); // re-reads profanity.txt
                    chatFilter->setProfanityOn(value == 0);                                                          // profanity filter on 0 / off 1
                }
                if (id >= ui::KeyAssign::kCompactCfg && id < ui::KeyAssign::kShortcutCfg + ui::KeyAssign::kShortcutRows)
                {
                    keymap = makeKeymap(); // a key re-assigned
                }
                saveSetting(fmt::format("cfg_{}", id).c_str(), value);
            };
            uiActions.setChatFilters = [&](uint32_t f1, uint32_t f2, int level, uint32_t local) {
                // C2S 0x0DB Kind 0 (CODE 0x100e4560): ConfigSys = the server's SAVE_CONF head (0x0B4) with the
                // filters and the system message level replaced; LSB stores them and answers with 0x0B4.
                if (!session || !world.saveConfValid)
                {
                    return;
                }
                std::vector<std::byte> p(0x24);
                auto conf = world.saveConf;
                conf[1]   = static_cast<uint8_t>((conf[1] & ~0x18) | (level & 3) << 3);
                for (int k = 0; k < 4; ++k)
                {
                    conf[static_cast<std::size_t>(4 + k)] = static_cast<uint8_t>(f1 >> (8 * k));
                    conf[static_cast<std::size_t>(8 + k)] = static_cast<uint8_t>(f2 >> (8 * k));
                }
                std::memcpy(p.data() + 4, conf.data(), conf.size());
                session->send(0x0DB, std::move(p));
                args.display_.gameConfig[52] = static_cast<int>(local);
                saveSetting("cfg_52", static_cast<int>(local));
            };
            uiActions.setWindowSkin = [&](int type) {
                if (type != args.display_.windowSkin)
                {
                    pendingSkin = type;
                    saveSetting("window_skin", type);
                }
            };
            uiActions.setKeyboard = [&](int type) {
                args.display_.keyboard = type;
                keymap                 = makeKeymap();
                saveSetting("keyboard", type);
                spdlog::info("keyboard: {}", type == 2 ? "Compact 2" : type == 1 ? "Compact" : "Normal");
            };
            uiActions.chooseCharacter = [&](std::size_t slot) { session->chooseCharacter(slot); };
            uiActions.auctionList     = [&](uint8_t category, std::vector<client::AuctionTerm> terms) {
                uiState_.auctionCategory = -1;
                uiState_.auctionListJob  = std::async(std::launch::async, [category, terms = std::move(terms), host = args.host, port = args.searchPort]() -> std::pair<int, std::vector<client::AuctionItem>> {
                    try
                    {
                        return { category, client::auctionList(net::Endpoint::resolve(host, port), category, terms) };
                    }
                    catch (const std::exception& e)
                    {
                        spdlog::warn("auction list {}: {}", category, e.what());
                        return { category, {} };
                    }
                });
            };
            uiActions.auctionHistory = [&](uint16_t item, bool stack) {
                uiState_.auctionSalesReady = false;
                uiState_.auctionHistoryJob = std::async(std::launch::async, [item, stack, host = args.host, port = args.searchPort]() -> std::vector<client::AuctionSale> {
                    try
                    {
                        return client::auctionHistory(net::Endpoint::resolve(host, port), item, stack);
                    }
                    catch (const std::exception& e)
                    {
                        spdlog::warn("auction history {}: {}", item, e.what());
                        return {};
                    }
                });
            };
            if (uiState_.auctionListJob.valid() && uiState_.auctionListJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                auto [category, listed] = uiState_.auctionListJob.get();
                uiState_.auctionItems            = std::move(listed);
                uiState_.auctionCategory         = category;
            }
            if (uiState_.auctionHistoryJob.valid() && uiState_.auctionHistoryJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                uiState_.auctionSales      = uiState_.auctionHistoryJob.get();
                uiState_.auctionSalesReady = true;
            }
            // The Search window: one search at a time; the count goes to the log as the game's does (file
            // 50 #5 / #6, not readable yet: the English text is written here).
            uiActions.search = [&](const client::SearchQuery& q) {
                uiState_.searchResult.reset();
                uiState_.searchWindowJob = std::async(std::launch::async, [q, host = args.host, port = args.searchPort]() -> std::optional<client::SearchResult> {
                    try
                    {
                        return client::searchPlayers(net::Endpoint::resolve(host, port), q);
                    }
                    catch (const std::exception& e)
                    {
                        spdlog::warn("search: {}", e.what());
                        return std::nullopt;
                    }
                });
                uiState_.searchAllAreas = !q.zone.has_value();
            };
            if (uiState_.searchWindowJob.valid() && uiState_.searchWindowJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                const auto r = uiState_.searchWindowJob.get();
                uiState_.searchResult = r ? *r : client::SearchResult{};
                if (!r)
                {
                    session->note("Search failed."); // 55646 #101
                }
                else if (r->hits.empty())
                {
                    session->note("No applicable entries."); // 55646 #107
                }
                else
                {
                    session->note(fmt::format("Search result: {} {} found in {}.", r->total, r->total == 1 ? "person" : "people",
                                              uiState_.searchAllAreas ? "all known areas" : "this area"));
                }
            }
            uiData.searchResult = uiState_.searchResult ? &*uiState_.searchResult : nullptr;
            uiData.auctionItems      = &uiState_.auctionItems;
            uiData.auctionCategory   = uiState_.auctionCategory;
            uiData.auctionSales      = &uiState_.auctionSales;
            uiData.auctionSalesReady = uiState_.auctionSalesReady;
            uiActions.setTarget       = [&](uint32_t id) {
                targetId = id;
                for (const auto& e : world.entities)
                {
                    if (e.id == id)
                    {
                        session->setPlayer(gfx::ZoneScene::renderToGame(feet), heading, e.index);
                    }
                }
            };
            {
                const auto me = gfx::ZoneScene::renderToGame(feet);
                for (const auto& e : world.entities)
                {
                    if (!e.name.empty() && e.hpp > 0 && !e.targetOff)
                    {
                        uiData.nearby.emplace_back(glm::distance(e.pos, me), &e);
                    }
                }
                std::sort(uiData.nearby.begin(), uiData.nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            }
            const client::Entity*& outerTarget = target;
            uiActions.say     = [&](const std::string& line, uint8_t kind) {
                // Commands and chat: apps/xi-client/chat_commands.cpp.
                app::ChatContext c{ session, world, uiActions, searchJob, clientText, args, zoneMaps, usageText, sound, namesHidden, selfEntity, scene, quitting, outerTarget, lockOn, keyItemNames, hideTrusts, ignoreTrusts, ignorePet, followId, feet, configFlags, blacklistAsked, battleText, autorun, areaNames, bugReportRequest };
                app::runChatLine(c, line, kind);
            };
            if (!scriptCmd.empty())
            {
                gameUi.runLine(scriptCmd); // as typed on the chat line (/ma, /ja, /attack ... live in GameUi)
                scriptCmd.clear();
            }
            // Config -> Add-ons' own commands (/addons, /addons set ...), with or without the addons running.
            uiActions.intercept = [this](std::string& line, int mode) {
                if (addonsPage && !line.empty() && line[0] == '/' && addonsPage->command(gameUi, line))
                {
                    return true;
                }
                if (!addons)
                {
                    return false;
                }
                if (!line.empty() && line[0] == '/' && addons->commandEdit(line, mode))
                {
                    return true;
                }
                std::string text = line;
                return addons->textOut(text, mode);
            };
            if (addons)
            {
                // Ashita: every chat line goes through the addons first (command / text_out); log lines they changed.
                uiActions.intercept = [this](std::string& line, int mode) {
                    if (addonsPage && !line.empty() && line[0] == '/' && addonsPage->command(gameUi, line))
                    {
                        return true;
                    }
                    if (!line.empty() && line[0] == '/' && addons->commandEdit(line, mode))
                    {
                        return true;
                    }
                    std::string text = line;
                    return addons->textOut(text, mode);
                };
                uiData.lineEdit = [this](uint64_t serial) -> std::optional<std::pair<bool, std::string>> {
                    if (const auto ed = addons->lineEdit(serial))
                    {
                        return std::pair{ ed->hidden, ed->text };
                    }
                    return std::nullopt;
                };
            }
            gameUi.draw(*ImGui::GetForegroundDrawList(), canvas, uiData, uiActions);
            if (addons)
            {
                addons->present(); // d3d_present, then the addons' fonts and primitives
            }
            if (gameUi.typing() != uiState_.textInputOn)
            {
                uiState_.textInputOn = gameUi.typing();
                uiState_.textInputOn ? SDL_StartTextInput(window) : SDL_StopTextInput(window);
            }
        }
    }

    frameTrace.mark(gfx::FrameTrace::Ui);
}

} // namespace xi::app
