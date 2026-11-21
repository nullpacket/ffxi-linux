// SPDX-License-Identifier: GPL-3.0-or-later
// xi-client's construction, small helpers and the frame loop (the stages are in their own files; client_app.h).
#include "client_app.h"
#include "client/session_end.h"

#include <thread>

namespace xi::app
{

namespace
{

void checkVk(VkResult r)
{
    if (r != VK_SUCCESS)
    {
        spdlog::error("imgui vulkan: VkResult {}", static_cast<int>(r));
    }
}

} // namespace

// Take Screenshot: $XDG_DATA_HOME/ffxi-linux/screenshots/ffxi_YYYYMMDD_HHMMSS.png (~/.local/share by default).
std::string userScreenshotPath()
{
    const char*           xdg  = std::getenv("XDG_DATA_HOME");
    const char*           home = std::getenv("HOME");
    std::filesystem::path dir  = xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(home ? home : ".") / ".local" / "share";
    dir /= "ffxi-linux";
    dir /= "screenshots";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::time_t t = std::time(nullptr);
    std::tm           tm{};
    localtime_r(&t, &tm);
    char name[64];
    std::strftime(name, sizeof(name), "ffxi_%Y%m%d_%H%M%S", &tm);
    // Several in one second get _2, _3 ... instead of overwriting each other.
    auto path = dir / (std::string(name) + ".png");
    for (int n = 2; std::filesystem::exists(path, ec) && n < 100; ++n)
    {
        path = dir / fmt::format("{}_{}.png", name, n);
    }
    return path.string();
}

// FFXI heading byte from a movement vector in game space (LSB: 0..255, counter-clockwise from +X, Y down).
float headingFloat(float dx, float dz)
{
    const float a = -std::atan2(dz, dx) * 128.0f / 3.14159265f;
    return a < 0 ? a + 256.0f : a;
}

float approachHeading(float shown, float target, float dt)
{
    if (shown < 0.0f)
    {
        return target;
    }
    const float d = std::fmod(target - shown + 384.0f, 256.0f) - 128.0f; // -128..128, the short way
    shown += d * (1.0f - std::exp(-dt * 20.0f));
    return std::fmod(shown + 256.0f, 256.0f);
}

uint8_t headingByte(float dx, float dz)
{
    float a = -std::atan2(dz, dx) * 128.0f / 3.14159265f;
    if (a < 0)
    {
        a += 256.0f;
    }
    return static_cast<uint8_t>(static_cast<int>(a + 0.5f) & 0xFF);
}

ClientApp::WindowOwner::~WindowOwner()
{
    SDL_DestroyWindow(w);
    SDL_Quit();
}

SDL_Window* ClientApp::openWindow(const Args& args)
{
    gfx::prepareVideoDriver(args.display);
    if (!std::getenv("XI_WINDOW_SLOT"))
    {
        // XI_WINDOW_SLOTS="Alice=tl,Bob=tr,...": each test character's window gets its own quarter of the display.
        if (const std::string slot = gfx::windowSlotFor(std::getenv("XI_WINDOW_SLOTS"), args.character); !slot.empty())
        {
            setenv("XI_WINDOW_SLOT", slot.c_str(), 1);
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        throw std::runtime_error(fmt::format("SDL_Init: {}", SDL_GetError()));
    }
    SDL_Window* window = gfx::createWindow("ffxi-linux", args.display_.windowWidth, args.display_.windowHeight, args.display, /*takeFocus=*/args.screenshot.empty() && args.script.empty(),
                                           args.display_.windowMode);
    return window;
}

bool ClientApp::loadTables()
{
    try
    {
        spellData.emplace(index);
        abilityData.emplace(index);
        helpText  = dat::StringTable::parse(index.read(55651));
        areaShort = dat::StringTable::parse(index.read(55661));
        keyItemNames = dat::KeyItemTable::parse(index.read(dat::KeyItemTable::kFile));
        windowTitles = dat::StringTable::parse(index.read(55652));
        filterText   = dat::StringTable::parse(index.read(55650));
        clientText   = dat::StringTable::parse(index.read(55648));
        formatText   = dat::XiStringTable::parse(index.read(50));
        areaNames    = dat::StringTable::parse(index.read(55465));
        regionNames  = dat::StringTable::parse(index.read(55654));
        unityText    = dat::StringTable::parse(index.read(55653));
        dropText     = dat::XiStringTable::parse(index.read(55649));
        charTitles   = dat::StringTable::parse(index.read(55704));
        usageText    = dat::StringTable::parse(index.read(55687));
        emoteList    = dat::EmoteList::parse(index.read(dat::EmoteList::kFile));
        emoteText    = dat::StringTable::parse(index.read(dat::EmoteList::kTextFile));
        questNames.emplace();
        for (std::size_t i = 0; i < 8; ++i)
        {
            (*questNames)[i] = dat::LogNameTable::parse(index.read(dat::LogNameTable::kQuestFiles[i]));
        }
        missionNames.emplace();
        for (std::size_t i = 0; i < 4; ++i)
        {
            (*missionNames)[i] = dat::LogNameTable::parse(index.read(dat::LogNameTable::kMissionFiles[i]));
        }
    }
    catch (const std::exception& e)
    {
        spdlog::warn("spell data unavailable ({})", e.what());
    }
    try
    {
        auto text = std::make_shared<dat::BattleText>(index);
        text->setItems(itemTable);
        battleText = std::move(text);
    }
    catch (const std::exception& e)
    {
        spdlog::warn("game message text unavailable ({}); using built-in strings", e.what());
    }
    return true;
}

bool ClientApp::initImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    ImGui_ImplSDL3_InitForVulkan(window);
    const VkFormat            colorFormat = ctx.colorFormat();
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion         = VK_API_VERSION_1_3;
    ii.Instance           = ctx.instance();
    ii.PhysicalDevice     = ctx.physicalDevice();
    ii.Device             = ctx.device();
    ii.QueueFamily        = ctx.queueFamily();
    ii.Queue              = ctx.queue();
    ii.DescriptorPoolSize = 1024; // ImGui font + every menu texture (~315, uploaded at start-up: ui/ui_draw.h) + item icons
    ii.MinImageCount      = 2;
    ii.ImageCount         = std::max(2u, ctx.imageCount());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat   = VK_FORMAT_D32_SFLOAT;
    ii.CheckVkResultFn    = checkVk;
    ImGui_ImplVulkan_Init(&ii);
    return true;
}

bool ClientApp::initMoji()
{
    try
    {
        if (const auto widths = dat::mojiWidthsFromDll(std::filesystem::path(args.root) / "FFXiMain.dll"))
        {
            uiR.setMojiWidths(*widths);
            mojiWidths = *widths;
        }
        else
        {
            spdlog::warn("ui: font width table not found in FFXiMain.dll; using measured widths");
        }
    }
    catch (const std::exception& e)
    {
        spdlog::warn("ui: {}", e.what());
    }
    return true;
}

bool ClientApp::initLogin()
{
    std::snprintf(host, sizeof(host), "%s", args.host.c_str());
    std::snprintf(user, sizeof(user), "%s", args.user.c_str());
    std::snprintf(pass, sizeof(pass), "%s", args.pass.c_str());
    std::snprintf(chr, sizeof(chr), "%s", args.character.c_str());
    if (args.autoconnect && !args.user.empty())
    {
        connect();
    }
    return true;
}

ClientApp::ClientApp(Args a) : args(std::move(a))
{
    sound.setVolumes(args.display_.soundVolume, args.display_.musicVolume);
    if (const auto it = args.display_.gameConfig.find(58); it != args.display_.gameConfig.end())
    {
        shadows.setMode(it->second); // Config Misc.: shadows detailed 2 / simple 0 / none 1
    }
    if (const auto it = args.display_.gameConfig.find(67); it != args.display_.gameConfig.end())
    {
        gfx::configuredAspect() = gfx::aspectOption(it->second); // Config Misc. 2: aspect ratio, once chosen
    }
    if (const int mode = applyChatFilter(args.display_.gameConfig.contains(206) ? args.display_.gameConfig.at(206) : 0); mode != 0)
    {
        args.display_.gameConfig[206] = mode; // Config Misc. 4: chat filter, if its lists are there
    }
    else
    {
        args.display_.gameConfig.erase(206);
    }
    gammaPass.setValues(cfg(55, 50), cfg(56, 50), cfg(57, 50)); // Config gamma R / G / B
    if (!args.dlss.empty())
    {
        startDlss(); // --dlss (goal 15); nothing of it exists without the flag
    }
    if (!args.fsr.empty() || (!args.dlss.empty() && !dlss_.dlss))
    {
        startFsr(); // --fsr (issue #139), or DLSS didn't start
    }
    if (args.rt || args.pathtrace)
    {
        startRt(); // --rt (goal 38), --pathtrace (goal 41); likewise
    }
    if (!args.post.empty())
    {
        startPost(); // --post (goal 41); likewise
    }
    // The detailed shadows' silhouettes: made before any batch names it (EffectRenderer::prepare would upload the key).
    (void)effectRenderer.renderTexture(shadows.silhouetteKey(), { GroundShadows::kAtlas, GroundShadows::kAtlas });
    chatFilter->setProfanityOn(!args.display_.gameConfig.contains(24) || args.display_.gameConfig.at(24) == 0); // Global: on 0 / off 1
    effects.setActorFiles([&](uint32_t id) {
        const auto* p = poses.find(id);
        return p ? p->files : std::vector<uint32_t>{};
    });
    effects.setActorRangedClass([&](uint32_t id) {
        const auto* p = poses.find(id);
        return p ? p->rangedClass : -1;
    });
    effects.setActorSheathStyles([&](uint32_t id) {
        // Op 0x84: main and sub styles (actor +0x87c / +0x87f); the sub weapon's is not loaded: the main's (INFERRED).
        const auto* p = poses.find(id);
        const int   s = p ? p->sheathStyle : 0;
        return std::array<int, 2>{ s, s };
    });
    gameUi.setSoundHook([this](uint32_t id) { sound.playUi(id); }); // menu cursor / confirm / cancel / open
    gameUi.setAutoFish(args.autoFish);
    if (!args.uiOpen.empty())
    {
        gameUi.openByName(args.uiOpen);
    }
    effects.setWeatherParticles(cfg(59, 1) != 0);
    effects.setConfig153(static_cast<uint32_t>(cfg(153, 0))); // op 0xB3's sound filter (bit 1)
    effects.hideBattleEffect = [&](uint32_t caster, uint32_t victim) {
        // Config Effects (cfg 146, CODE 0x100ade10): bits 1..16 hide source -> target pairs, source / target each
        // you (or your pet), a monster, a party member, anyone else (0x100adec0); bit 0 limits that to while
        // engaged.
        const int bits = cfg(146, 0);
        if (!curWorld || (bits & ~1) == 0 || ((bits & 1) && curWorld->animation != 1 && curWorld->animation != 3))
        {
            return false;
        }
        const auto& w     = *curWorld;
        auto        group = [&](uint32_t id) {
            if (id == w.charId)
            {
                return 0;
            }
            for (const auto& e : w.entities)
            {
                if (e.id == id)
                {
                    if (w.petIndex != 0 && e.index == w.petIndex)
                    {
                        return 0;
                    }
                    if (e.isMob)
                    {
                        return 1;
                    }
                    break;
                }
            }
            const bool party = std::any_of(w.party.begin(), w.party.end(), [&](const client::PartyMember& m) { return m.id == id; });
            return party ? 2 : 3;
        };
        return ((bits >> (1 + 4 * group(caster) + group(victim))) & 1) != 0;
    };
    if (!args.script.empty() && !script.load(args.script))
    {
        exitCode_ = 1;
    }
    startAddons();
    AddonsPage::Host ah;
    ah.manager   = [this] { return addons.get(); };
    ah.root      = [this] { return ashitaRoot(); };
    ah.character = [this] { return world.inZone ? world.charName : std::string(); };
    ah.charId    = [this] { return world.charId; };
    ah.release   = [this] {
        const char* e = std::getenv("XI_ASHITA_VERSION");
        const std::string v = e && *e ? std::string(e) : args.display_.ashitaVersion;
        return v.empty() ? std::string("auto") : v;
    };
    ah.setRelease = [this](const std::string& r) { restartAddons(r); };
    ah.note       = [this](const std::string& t) {
        if (session)
        {
            session->note(t);
        }
    };
    ah.command    = [this](const std::string& line) { gameUi.runLine(line); };
    addonsPage    = std::make_unique<AddonsPage>(std::move(ah));
}

// Casting motion family for a spell: 0 white, 1 black, 2 other (summoning, ninjutsu, songs...); -1 = none.
int ClientApp::castSchool(uint16_t spell)
{
    if (spell == 0)
    {
        return -1;
    }
    const auto* sp = spellData ? spellData->find(spell) : nullptr;
    return !sp ? 1 : sp->type == 1 ? 0 : sp->type == 2 ? 1 : 2;
}

void ClientApp::connect()
{
    client::Session::Config cfg;
    cfg.lobby.host          = host;
    cfg.lobby.authPort      = args.authPort;
    cfg.lobby.dataPort      = args.dataPort;
    cfg.lobby.viewPort      = args.viewPort;
    cfg.lobby.username      = user;
    cfg.lobby.password      = pass;
    cfg.lobby.clientVersion = args.clientVer;
    // --login-protocol / client.ini login_protocol, else XI_LOGIN_PROTOCOL, else auto (docs/protocol-login.md §1.4).
    if (!args.display_.loginProtocol.empty())
    {
        if (const auto p = lobby::parseLoginProtocol(args.display_.loginProtocol))
        {
            cfg.lobby.protocol = *p;
        }
        else
        {
            spdlog::warn("login protocol '{}' is not auto / 2.1 / 2.2; using the default", args.display_.loginProtocol);
        }
    }
    cfg.character           = chr;
    cfg.chooseInUi          = true; // no character named: pick it in the lobby
    connectedAt             = std::chrono::steady_clock::now();
    cfg.compressDat         = args.compressDat;
    cfg.entityRenames       = args.entityRenames;
    cfg.text                = battleText;
    cfg.index               = &index;
    cfg.chatFilter          = chatFilter;
    {
        // Ashita's packet_in / packet_out, on the session thread (the session can start before the addons do).
        cfg.packetFilter = [this](bool incoming, bool injected, uint16_t id, std::vector<std::byte>& bytes, std::span<const std::byte> chunk) {
            auto* a = addonsLive.load();
            if (!a)
            {
                return true;
            }
            ashita::PacketEvent e;
            e.id       = id;
            e.data     = std::move(bytes);
            e.chunk    = chunk;
            e.injected = injected;
            const bool keep = incoming ? a->packetIn(e) : a->packetOut(e);
            bytes           = std::move(e.data);
            return keep;
        };
    }
    if (clientText && !clientText->get(345).empty())
    {
        cfg.deletedText = std::string(clientText->get(345)); // " ***This message has been deleted.***"
    }
    session                 = std::make_unique<client::Session>(cfg);
    // Data older than the LSB checkout's target version shows the server's zone messages off by a line (#58). The log
    // and `xi-setup check` say so; the chat log stays the game's own, as in retail (#78).
    if (const auto lsb = install::findLsbDir())
    {
        const auto lsbVer = install::lsbTargetClientVersion(*lsb);
        if (const auto warn = lsbVer ? install::dataVersionWarning(args.clientVer, *lsbVer) : std::string(); !warn.empty())
        {
            spdlog::warn("{}", warn);
        }
    }
    session->start();
}

// #43: a session that ended on its own (the map server stopped answering, which LSB's watchdog restarts cause, or a
// failed login) goes back to the login form with the reason, instead of leaving a frozen game window. Enter logs in
// again once the server is back. Test scripts quit instead (runScript).
void ClientApp::returnToLoginIfEnded()
{
    if (!session || script.loaded())
    {
        return;
    }
    const auto notice = client::sessionEndNotice({ session->finished(), quitting, session->loggedOutByServer(), session->connectionLost(), world.status });
    if (!notice)
    {
        return;
    }
    spdlog::warn("session ended ({}): back to the login form", world.status);
    loginFields.notice = *notice;
    session.reset();
    world = client::WorldSnapshot{}; // this frame's stages see no zone either
    zonedAt.reset();
}

// Config Misc. 4 chat filter (cfg 206): selecting a mode re-reads prohibited.txt / allowed.txt ("Pressing this button
// will reflect your edits", 55651 #1088 / #1090; so does "off" here, which un-dims the modes after the files are
// made). Like the game (0x101bca80), the term / player modes fall back to off while no prohibited entry is loaded.
int ClientApp::applyChatFilter(int mode)
{
    const auto file = DisplaySettings::userFile();
    if ((file ? chatFilter->load(file->parent_path()) : 0) == 0)
    {
        mode = 0;
    }
    chatFilter->setMode(mode);
    return mode;
}

// Config → Keyboard Size and the key-assignment lists (cfg 74.. / 106..) change it.
ui::Keymap ClientApp::makeKeymap()
{
    ui::KeyAssign assign;
    assign.load(args.display_.gameConfig);
    return ui::Keymap(static_cast<ui::KeyLayout>(args.display_.keyboard), assign);
}

// Config Mouse/Cam. (config_re.md): X / Y inverted for the third-person (139 / 140) and first-person (141 / 142)
// camera: -1 flips the input.
float ClientApp::camInvert(bool x)
{
    const int  id = (firstPerson ? 141 : 139) + (x ? 0 : 1);
    const auto it = args.display_.gameConfig.find(id);
    return it != args.display_.gameConfig.end() && it->second == 1 ? -1.0f : 1.0f;
}

bool ClientApp::isTrust(const client::Entity& e, const client::WorldSnapshot& w)
{
    return (e.id >> 24) == 1 && std::ranges::any_of(w.party, [&](const client::PartyMember& m) { return m.id == e.id; });
}

// A game config id (Config pages, cfg_<id>), or `def` when not set.
int ClientApp::cfg(int id, int def)
{
    const auto it = args.display_.gameConfig.find(id);
    return it != args.display_.gameConfig.end() ? it->second : def;
}

void ClientApp::saveCamZoom()
{
    camZoomDirty                       = false;
    const int z                        = static_cast<int>(std::lround(camZoom));
    args.display_.gameConfig[0x49]     = z;
    if (const auto file = DisplaySettings::userFile())
    {
        DisplaySettings::saveOwn(*file, "cfg_73", z);
    }
}

// The time of day in seconds: the event's hour (op 0x77; minutes run on) or Vana'diel's.
double ClientApp::eventTimeOfDay()
{
    const double secs = client::vanaHourOfDay() * 3600.0;
    const auto   h    = cutscene ? cutscene->hour() : std::nullopt;
    return h ? *h * 3600.0 + std::fmod(secs, 3600.0) : secs;
}

std::string ClientApp::weatherGroup() const
{
    const uint16_t w   = args.weatherOverride >= 0 ? static_cast<uint16_t>(args.weatherOverride) : world.weather;
    const auto&    env = effects.environment();
    return !env.fourcc.empty() ? env.fourcc : env.altTable ? dat::altWeatherFourcc(w) : dat::weatherFourcc(w);
}

bool ClientApp::eventBusy()
{
    return cutscene && cutscene->active();
}

bool ClientApp::playerLocked()
{
    // The game's "player locked" test (CODE 0x10086fa0, flags 0): an event running, or one of the fishing statuses
    // (0x100955f0: 6, 38..43, 50..53, 56..62). Walking keys, autorun and /follow wait while it holds.
    return eventBusy() || fishingStatus(world.animation);
}

int ClientApp::run()
{
    if (exitCode_ != 0)
    {
        return exitCode_;
    }
    while (true)
    {
        // Ashita's fps addon (addon_native.cpp): a frame every n / 60 s; no cap unless it sets one.
        if (addonOverrides_.fpsDivisor > 0)
        {
            std::this_thread::sleep_until(last + std::chrono::microseconds(addonOverrides_.fpsDivisor * 1000000 / 60));
        }
        frameTrace.begin(ctx.gpuFrameMs(), static_cast<int>(charInstances.size()));
        now = std::chrono::steady_clock::now();
        dt  = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        // Config → Window Type: reload the interface with the new skin file before anything is drawn this frame.
        // The renderer is rebuilt in the same optional, so references to it stay valid.
        if (pendingSkin != 0)
        {
            args.display_.windowSkin = pendingSkin;
            pendingSkin              = 0;
            uiHolder.reset();
            const uint32_t files[] = { dat::kUiLobbyFile, dat::kUiMenuFile, dat::kUiIconFile, dat::uiSkinFile(static_cast<unsigned>(args.display_.windowSkin - 1)) };
            uiRes                  = dat::UiResources::load(index, files);
            uiHolder.emplace(ctx, uiRes);
            if (mojiWidths)
            {
                uiR.setMojiWidths(*mojiWidths);
            }
            spdlog::info("window type {}", args.display_.windowSkin);
        }
        last            = now;
        fps             = fps * 0.95f + (dt > 0 ? 1.0f / dt : 0.0f) * 0.05f;


        handleEvents();

        // Quit: log out cleanly while still drawing frames.
        if (quitting && session && !session->finished())
        {
            session->requestStop();
            // --shot-after-logout: one more capture while the logout runs (the character should be resting).
            static std::optional<std::chrono::steady_clock::time_point> quitAt;
            static bool                                                  logoutShot = false;
            if (!quitAt)
            {
                quitAt = std::chrono::steady_clock::now();
            }
            if (args.shotAfterLogout >= 0.0f && !logoutShot && !args.screenshot.empty() &&
                std::chrono::steady_clock::now() - *quitAt > std::chrono::duration<float>(args.shotAfterLogout))
            {
                logoutShot  = true;
                shotPath    = std::filesystem::path(args.screenshot).replace_extension().string() + "_logout.png";
                captureNext = true;
            }
        }
        if (session && session->loggedOutByServer())
        {
            quitting = true; // the logout countdown ran out
        }
        if (quitting && (!session || session->finished()))
        {
            break;
        }

        updateWorld();
        stepBugReport(); // /bugreport: the capture steps and the uploader's messages (a no-op in the public build)
        runUiKeys();
        updateTargets();
        updateAddons();
        runScript();
        returnToLoginIfEnded();
        runTestHooks();
        handleActions();
        updateZone();
        updateMovement();
        updateCamera();
        drawInterface();
        updateFishing();
        drawActors();
        updateEffects();
        if (!renderFrame())
        {
            continue;
        }
    }

    spdlog::info("fx: {}", effects.unhandledSummary()); // goal 7: effect ops met that xi-client does not carry out
    if (addons)
    {
        addons->unloadAll(); // their unload events (settings saves) while the session can still send
    }
    ctx.waitIdle();
    scene.reset();
    propLayer.reset();
    uiHolder.reset(); // before the ImGui backend that owns its descriptor sets
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    cutscene.reset();
    session.reset();
    addonsLive = nullptr;
    addons.reset(); // after the session thread, which calls its packet events
    return 0;
}

} // namespace xi::app
