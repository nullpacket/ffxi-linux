// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// xi-client's state and frame loop (ClientApp). The loop's stages live in their own files.

#include "audio/footsteps.h"
#include "audio/sound_system.h"
#include "client/action_effects.h"
#include "client/actor_ground.h"
#include "client/battle_colours.h"
#include "addons_page.h"
#include <deque>
#include "actor_poses.h"
#include "characters.h"
#include "ground_shadows.h"
#include "mog_house.h"
#include "chat_commands.h"
#include "client_args.h"
#include "test_script.h"
#include "client/cutscene.h"
#include "client/game_camera.h"
#include "client/doors.h"
#include "client/ships.h"
#include "gfx/character_anim.h"
#include "client/search.h"
#include "client/session.h"
#include "ashita/ashita.h"
#include "ashita/imgui_ini.h"
#include "dat/items.h"
#include "dat/spells.h"
#include "dat/sound.h"
#include "xi/bytes.h"
#include "dat/dat_index.h"
#include "dat/event.h"
#include "dat/texture.h"
#include "dat/weather.h"
#include "dat/zone.h"
#include "gfx/camera.h"
#include "gfx/context.h"
#include "gfx/dlss.h"
#include "gfx/fsr.h"
#include "gfx/pathtrace.h"
#include "gfx/post.h"
#include "gfx/raytracing.h"
#include "gfx/motion_vectors.h"
#include "gfx/frame_trace.h"
#include "gfx/window.h"
#include "gfx/effect_renderer.h"
#include "gfx/marker_renderer.h"
#include "dat/character.h"
#include "dat/motion.h"
#include "gfx/character_model.h"
#include "gfx/look_at.h"
#include "gfx/environment.h"
#include "gfx/skinned_renderer.h"
#include "gfx/sky_renderer.h"
#include "gfx/prop_layer.h"
#include "gfx/zone_scene.h"

#include <SDL3/SDL.h>
#include "dat/dll_data.h"
#include "dat/ui.h"
#include "client/emotes.h"
#include "gfx/scene_target.h"
#include "gfx/gamma_pass.h"
#include "ui/game_ui.h"
#include "ui/keymap.h"
#include "xi/settings.h"
#include "xi/install.h"
#include "ui/layout_view.h"
#include "ui/ui_draw.h"

#include <imgui.h>

#include <cstring>
#include <thread>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <spdlog/spdlog.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <map>
#include <unordered_set>
#include <future>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>
#include <array>
#include <filesystem>

namespace xi::bugreport
{
class Reporter; // the private build's bug reporter (XI_PRIVATE_FEATURES)
}

namespace xi::app
{

// FFXI heading byte from a movement vector in game space (LSB: 0..255, counter-clockwise from +X, Y down).
uint8_t headingByte(float dx, float dz);
// The same heading unrounded (0..256).
float headingFloat(float dx, float dz);
// A drawn heading turning toward `target` (heading units, 0..256) the short way round: most of the way in ~0.15 s
// (the game turns models smoothly; its rate is INFERRED). `shown` < 0 starts at the target.
float approachHeading(float shown, float target, float dt);
// Take Screenshot: $XDG_DATA_HOME/ffxi-linux/screenshots/ffxi_YYYYMMDD_HHMMSS.png (~/.local/share by default).
std::string userScreenshotPath();
// Adds /bugreport's in-memory log ring to spdlog's default logger (main.cpp, at start-up); a no-op in
// bugreport_none.cpp without XI_PRIVATE_FEATURES.
void installBugReportLog();

class ClientApp
{
public:
    explicit ClientApp(Args a);
    ClientApp(const ClientApp&)            = delete;
    ClientApp& operator=(const ClientApp&) = delete;
    // The frame loop until the character has logged out (or the window closed); the process exit code.
    int run();

private:
    // Owns the window: destroyed (with SDL) after every member declared below it.
    struct WindowOwner
    {
        SDL_Window* w = nullptr;
        explicit WindowOwner(SDL_Window* win) : w(win) {}
        WindowOwner(const WindowOwner&)            = delete;
        WindowOwner& operator=(const WindowOwner&) = delete;
        ~WindowOwner();
    };
    static SDL_Window* openWindow(const Args& args);
    // Construction steps that must run between members (in declaration order): game text tables, Dear ImGui, the font
    // widths, the login form.
    bool loadTables();
    bool initImGui();
    bool initMoji();
    bool initLogin();

    // Helpers (client_app.cpp, input.cpp).
    int        castSchool(uint16_t spell);
    void       connect();
    void       returnToLoginIfEnded();
    ui::Keymap makeKeymap();
    float      camInvert(bool x);
    void       saveCamZoom(); // camZoom to config 0x49 (cfg_73)
    [[nodiscard]] float clothWindAt(const glm::vec3& at) const;
    static bool isTrust(const client::Entity& e, const client::WorldSnapshot& w);
    int        cfg(int id, int def);
    bool       eventBusy();
    bool       playerLocked(); // an event, or our line out (fishing): no walking (CODE 0x10086fa0 with flags 0)
    double     eventTimeOfDay();
    bool       onKey(SDL_Scancode sc, SDL_Keymod mods, bool repeat);

    // The frame loop's stages, in order.
    void handleEvents();   // input.cpp: SDL events, keys through the game's bindings
    bool addonInput(const SDL_Event& ev, bool uiMouse); // input.cpp: an event to the addons' key / mouse events; true when blocked
    void updateWorld();    // world_frame.cpp: the session's snapshot, events / cutscenes, synthesis results
    void runUiKeys();      // test_hooks.cpp: macros, --ui-keys
    void updateTargets();  // input.cpp: Tab / F-key targeting, --auto-fight / --tab picks
    // Engaged with a living monster: the target is locked on it (Tab, F1..F6 and Esc do nothing; input.cpp).
    [[nodiscard]] bool targetLocked() const;
    // Tab's filter: an entity the target keys may pick (named, not a dead monster, not hidden; trusts / pet by option).
    [[nodiscard]] bool targetable(const client::Entity& e) const;
    // Enter with nothing targeted: the nearest targetable entity in front of us within 50 yalms (input.cpp).
    [[nodiscard]] const client::Entity* frontTarget() const;
    void startAddons();   // addons.cpp: Ashita addons in ashitaRoot()
    std::filesystem::path ashitaRoot() const; // XI_ASHITA_DIR, client.ini ashita_dir, else the game folder (#73)
    void restartAddons(const std::string& release); // addons.cpp: Config -> Add-ons changed the Ashita release
    void updateAddons();   // addons.cpp: their state for this frame
    void startAddonNatives(); // addon_native.cpp: the client settings addons' memory patches stand for
    void runScript();      // script_steps.cpp: --script steps
    void runTestHooks();   // test_hooks.cpp: --attack, --cast-at, --ws-at ... and --equip-item / --use-item
    void handleActions();  // input.cpp: Confirm, talk, the Attack shortcut, auto-target
    void updateZone();     // zone_frame.cpp: zone (re)load, Mog House props, ships, timed captures, teleports
    void updateMovement(); // movement.cpp: walking, gravity-free moves, facing the target, follow / chase
    void updateCamera();   // movement.cpp: camera keys, lock-on, lifts, ships, cutscene puppets, gravity, the camera
    void drawInterface();  // ui_frame.cpp: login form / lobby / the game interface and debug overlay
    void updateFishing();  // fishing.cpp: fishing schedulers on status changes, the minigame's arrows and motions
    void drawActors();     // actors_draw.cpp: character models, motions, mounts, corpses, names, shadows
    void updateEffects();  // effects_frame.cpp: battle effects, doors, weather, sounds
    bool renderFrame();    // render.cpp: record and submit the frame; false when no swapchain image was ready


    Args                                   args;
    const dat::DatIndex                    index{ std::filesystem::path(args.root) };
    const dat::ZoneKeys                    keys = dat::ZoneKeys::fromDll(index.installDir() / "FFXiMain.dll");
    std::shared_ptr<const dat::BattleText> battleText;
    const std::shared_ptr<const dat::ItemTable> itemTable = std::make_shared<const dat::ItemTable>(index);
    const dat::ItemTable&                  items     = *itemTable;
    std::optional<dat::SpellTable>         spellData;
    std::optional<dat::AbilityTable>       abilityData;
    std::optional<dat::StringTable>        helpText;  // menu help lines (55651, dat-format §14)
    std::optional<dat::StringTable>        areaShort; // short area names (55661)
    std::optional<dat::KeyItemTable>       keyItemNames; // key item names (55695)
    std::optional<dat::StringTable>        windowTitles; // window titles (55652)
    std::optional<dat::StringTable>        filterText;   // Config filter row labels (55650)
    std::optional<dat::XiStringTable>      formatText; // string table 10 (file 50, XISTRING): window title formats
    std::optional<dat::StringTable>        clientText;   // the client's own messages (55648: command errors, ...)
    std::optional<dat::StringTable>        areaNames;    // zone names (55465)
    std::optional<dat::StringTable>        regionNames;  // nation and region names (55654)
    std::optional<dat::StringTable>        unityText;    // Unity leader names (55653)
    std::optional<dat::XiStringTable>           dropText;     // item prompts (55649: #48 "Really dispose of the %s?")
    std::optional<dat::StringTable>        charTitles;   // character titles by title id (55704)
    std::optional<dat::EmoteList>          emoteList;    // the Emote List (file 96)
    std::optional<dat::StringTable>        emoteText;    // its labels / help (55675)
    std::optional<dat::StringTable>        usageText;    // command usage texts (55687: "/say, /sUSAGE: /say [mes.]..."), /help
    std::optional<std::array<dat::LogNameTable, 8>> questNames;
    std::optional<std::array<dat::LogNameTable, 4>> missionNames;
    const dat::ZoneMaps                    zoneMaps{ index, std::filesystem::path(args.root) / "FFXiMain.dll" };
    bool                                   tablesLoaded_ = loadTables();

    WindowOwner                            windowOwner_{ openWindow(args) };
    SDL_Window*                            window = windowOwner_.w;
    // --dlss (goal 15): NGX's extensions go into the context; without the flag it is made exactly as before.
    // --rt (goal 38): ray queries likewise, only with the flag.
    gfx::ContextExtensions                 dlssExtensions_ = prepareExtensions(args);
    gfx::Context                           ctx{ window, args.validation, args.dlss.empty() && !args.rt && !args.pathtrace ? nullptr : &dlssExtensions_ };
    gfx::MarkerRenderer                    markers{ ctx };
    // Visual effects (docs/effect-format.md): 0x028 actions start schedulers whose generators draw here.
    gfx::EffectRenderer                    effectRenderer{ ctx };
    client::EffectSystem                   effects{ index };
    client::ActionEffects                  actionFx; // 0x028 actions, appearing and dying characters -> effects
    client::Doors                          doors;    // door entities -> the zone's door parts
    client::Ships                          ships;    // ship entities -> their hulls' poses
    // Sound effects and zone music (docs/sound-format.md); XI_NO_AUDIO=1 runs silent.
    audio::SoundSystem                     sound{ index.installDir(), std::getenv("XI_NO_AUDIO") == nullptr };
    std::vector<client::EffectSystem::Batch> fxBatches;
    const bool                             fxTrace = std::getenv("XI_FX_TRACE") != nullptr;
    gfx::SkyRenderer                       skyRenderer{ ctx };
    gfx::GammaPass                         gammaPass{ ctx };   // Config gamma R / G / B (cfg 55..57)
    gfx::SceneTarget                       sceneTarget{ ctx }; // background resolution (registry 0003/0004), when it differs from the window
    gfx::TextureSet                        charTextures{ ctx };
    gfx::SkinnedRenderer                   skinned{ ctx, charTextures };
    bool                                   skinnedWarm_ = (skinned.prewarm(VK_FORMAT_R8G8B8A8_UNORM, effectRenderer.screenLayout()), true);
    audio::Footsteps                       steps;
    app::Characters                        chars{ index, charTextures, skinned };
    app::ActorPoses                        poses{ chars }; // per actor this frame: bones and resource files (effects, look-at)
    app::GroundShadows                     shadows{ index }; // XI_NO_SHADOWS=1 turns them off
    // Config Misc. 4 chat filter (cfg 206): prohibited.txt / allowed.txt beside client.ini.
    std::shared_ptr<client::ChatFilter>    chatFilter = std::make_shared<client::ChatFilter>();
    int                                    applyChatFilter(int mode); // reloads the lists; the mode it could set
    std::unordered_set<uint64_t>                 shownResults;   // ChatLine::hold values released by op 0x2B
    std::vector<gfx::SkinnedRenderer::Instance> charInstances;
    std::unordered_map<uint32_t, gfx::AnimTrack> animTracks;
    bool restCancelSent = false; // a movement key already asked the server to end our rest / logout countdown
    // drawActors' own state (actors_draw.cpp), kept between frames.
    struct ActorsDrawState
    {
        std::map<uint32_t, std::size_t>              charIndex;   // actor id -> its instance this frame (dolls copy it)
        gfx::ClothWind                               clothWind;   // the scene wind on soft bodies (gfx/cloth.h)
        std::unordered_map<uint32_t, gfx::AnimTrack> mountTracks; // the chocobo / mount under a rider, by rider
        std::unordered_map<uint32_t, bool>           riding;      // riding last frame, by rider (the mount hop on a change)
        // What each rider last sat on (status 5 / 85, the mount's look), and the mounts leaving after a dismount.
        std::unordered_map<uint32_t, std::pair<uint8_t, client::Look>> lastRide;
        struct Dismount
        {
            client::Look mount;
            glm::vec3    at{ 0.0f }; // render space
            uint8_t      dir = 0;
            float        t   = 0.0f;
        };
        std::unordered_map<uint32_t, Dismount> dismounts;
        float                                  ownMountHeight = 0.0f;  // what our mount adds to the camera height (dat::mountCameraHeight)
        float                                  ownShownDir    = -1.0f; // our model's drawn heading (smoothed)
        std::unordered_map<uint32_t, float>    shownDir;               // other characters' drawn headings (smoothed)
    } drawState_;
    bool                                    ownMoving = false;
    // The weather group shown (the server's, or an effect's), for the sky effects.
    std::string                             weatherGroup() const;
    std::unique_ptr<gfx::ZoneScene> scene;
    // An event's sub-map (op 0x75 00), drawn with `scene` until the event ends: it adds to the zone (zone 233's 374 is
    // Prince Trion's furniture inside the chateau; N. San d'Oria's 361 a whole room away from the town).
    std::unique_ptr<gfx::ZoneScene> subScene;
    // Ships and airships (kind 4): their hulls, which come and go without rebuilding `scene` (gfx::PropLayer).
    std::unique_ptr<gfx::PropLayer> propLayer;
    std::unordered_map<uint32_t, std::string> zoneNames;
    std::string                               zoneError;

    ashita::ImGuiIniFile          imguiIni_; // addon window placement, <ashita root>/config/imgui.ini (#111)
    bool                          imguiReady_ = initImGui(); // Dear ImGui, before the interface renderer
    const uint32_t                uiFiles[4] = { dat::kUiLobbyFile, dat::kUiMenuFile, dat::kUiIconFile, dat::uiSkinFile(static_cast<unsigned>(args.display_.windowSkin - 1)) };
    dat::UiResources              uiRes      = dat::UiResources::load(index, uiFiles);
    std::optional<ui::UiRenderer> uiHolder{ std::in_place, ctx, uiRes };
    ui::UiRenderer&               uiR = *uiHolder; // stays valid when Config swaps the window skin (same storage)
    std::optional<std::vector<uint8_t>> mojiWidths;
    int                                 pendingSkin = 0; // Config → Window Type: swapped in at the next frame's start
    bool                                mojiReady_  = initMoji();

    // Login form state (pre-filled from flags).
    char            host[64]{}, user[32]{}, pass[64]{}, chr[32]{};
    ui::LoginFields loginFields{ args.host, args.user, args.pass, args.character, args.user.empty() ? 1 : args.pass.empty() ? 2 : 1 };
    std::unique_ptr<client::Session> session;
    std::unique_ptr<ashita::AddonManager> addons; // Ashita addons (null when off); outlives the session
    std::atomic<ashita::AddonManager*>    addonsLive{ nullptr }; // the same, for the session thread's packet events
    std::unique_ptr<AddonsPage>           addonsPage; // Config -> Add-ons (goal 47)
    std::optional<std::chrono::steady_clock::time_point> connectedAt; // when the session was started
    bool                                                loginReady_ = initLogin(); // the form's fields; --autoconnect

    // Player + camera (render space, Y up).
    glm::vec3 feet{ 0.0f };
    // Where our model is drawn: the game's drawn position (actor +0x5fc) closing 1/8 of its gap to the feet (the
    // physics position, +0x5c4) each 1/60 s tick (CODE 0x100c651a), so a step up or down a stair eases instead of
    // jumping. movement.cpp sets it; XI_EASE_OLD=1 draws at the feet.
    glm::vec3 drawFeet{ 0.0f };
    bool      drawFeetSeen = false;
    bool      snapPending = false; // put the feet on the floor once the zone is loaded (after a teleport)
    float     camYaw = args.camYaw, camPitch = args.camPitch * 3.14159265f / 180.0f, camDist = args.camDist;
    // The projection zoom (config 0x49): Zoom In / Out move it 6 a tick between 242 and 900 (CODE 0x1001f742).
    float     camZoom = static_cast<float>(cfg(0x49, 350));
    bool      camZoomDirty = false;
    bool      camReset    = true; // place the eye behind the character (zone-in, a warp, an event camera ending)
    float     camMouseYaw = 0.0f, camMouseUp = 0.0f; // orbit drag this frame (radians)
    bool      camBehind   = false; // Home (action 0x51)
    float     camWheelTicks = 0.0f; // the wheel's zoom still to apply, in ticks (+ in, - out)
    float     camHoldYaw = 0.0f, camHoldUp = 0.0f; // test key "cam:": camera axes held until camHoldUntil
    std::chrono::steady_clock::time_point camHoldUntil{};
    bool      camAtPending = false; // test key "camat:": place the camera at this view once (goal 41's comparisons)
    float     camAtYaw = 0.0f, camAtPitch = 0.0f, camAtDist = 5.5f;
    float     ownCamHeight  = 1.8f;  // our model's height for the camera (actors_draw: riding, the head top)
    float     ownHeadHeight = 1.6f;  // our head above the feet (first person)
    std::optional<std::chrono::steady_clock::time_point> zonedAt;
    bool      captureNext = false;
    bool      orbiting = false;
    bool      zoneLineSent = false;
    uint32_t  targetId     = 0; // selected entity (UniqueNo), 0 = none
    // Other entities' movement: the server sends a walker's position about once per yalm (a roaming hare: every
    // ~0.73 s), so the drawn position walks toward the latest one at the pace those updates show (distance over
    // time between them), and only hurries when it falls behind; spawns and warps snap.
    struct MoveTrack
    {
        glm::vec3 shown{ 0.0f }, server{ 0.0f }; // render space
        double    at       = 0.0;                // when `server` arrived
        double    interval = 0.75;               // typical time between moving updates
        float     speed    = 1.0f;               // yalms per second
        bool      init     = false;
    };
    std::unordered_map<uint32_t, MoveTrack> smoothPos;
    // Dead monsters as last seen, and those the server has despawned: they dissolve over kCorpseFade seconds.
    struct Corpse
    {
        client::Entity e;
        float          age = 0.0f;
    };
    static constexpr float                          kCorpseFade = 1.2f;
    std::unordered_map<uint32_t, client::Entity> deadSeen;
    std::unordered_map<uint32_t, Corpse>         corpses;
    bool      cycleTarget  = false;
    bool      toggleEngage = false; // Attack shortcut (Ctrl/Alt + A): attack the target / disengage
    bool      talk         = false; // talk to / interact with the target
    // Config → Keyboard Size and the key-assignment lists (cfg 74.. / 106..) change it.
    ui::Keymap keymap = makeKeymap();
    bool      autorun      = false; // Autorun (Num 7 / Compact R): run forward until a movement key
    bool      walking      = false; // Toggle Walk/Run (Num / / Compact Z)
    bool      firstPerson  = false; // Change View (Num 5 / Compact V)
    // /bugreport (goal 48; private builds): the request from the chat line, the capture in progress, the uploader and
    // the recent frame times.
    std::optional<std::string>             bugReportRequest;
    struct BugCapture
    {
        int                   stage = 0; // 0 idle, 1 full frame shot pending, 2 scene shot pending
        std::filesystem::path dir;
        std::string           description;
        bool                  uiWasHidden = false;
    } bugCapture;
    std::shared_ptr<bugreport::Reporter> bugReporter; // shared_ptr: Reporter is only declared here
    std::deque<float>                    recentFrameMs;
    void stepBugReport();
    void writeBugReport();
    std::string shotPath;          // Take Screenshot (Print Screen / Compact [): the file of the pending capture
    bool      inGame       = false; // in a zone (not login / lobby): keys go through the keymap
    std::string lastTellFrom;       // /reply target (from the session's world, each frame)
    bool      showDebug = false; // F11: debug overlay (status line, nearby entities)
    uint8_t   namesHidden = 0;   // /names: 0 names shown, 1 none (/names off), 2 none over other players (/names off otherpc)
    bool      uiActive    = false; // in a zone: the game interface takes input
    int       uiKeyStep   = 0;
    std::string macrosFor;
    client::Entity selfEntity; // stand-in when we target ourselves
    std::optional<std::chrono::steady_clock::time_point> firstPopupAt; // --shot-after-popup
    std::future<std::vector<std::string>> searchJob; // /search: the search server's answer, as log lines
    int      blacklistAsked = 0; // the last /blacklist edit sent: 0 add, 1 delete
    // /hidefaith (/hidetrust): trusts are not drawn; /ignorefaith (/ignoretrust), /ignorepet: Tab skips them.
    bool hideTrusts = false, ignoreTrusts = false, ignorePet = false;
    ui::GameUi gameUi{ uiR };
    // Event scripts (docs/event-format.md, dat::EventVm): the script the server starts runs here; its messages go
    // to the log, Enter continues past a message, menus open as lists, and the end / updates go back as 0x05B.
    // Events and cutscenes (client::Cutscene over dat::EventRunner): created with the session.
    std::optional<client::Cutscene>     cutscene;
    const client::WorldSnapshot*        curWorld = nullptr;      // this frame's snapshot, for the cutscene hooks
    uint64_t                            frameTick = 0;           // frames drawn (Config animation rate)
    std::chrono::steady_clock::time_point lastInput    = std::chrono::steady_clock::now(); // Config auto-disconnect
    std::chrono::steady_clock::time_point lastCamInput = std::chrono::steady_clock::now(); // camera turned by the player
    std::unordered_map<uint32_t, bool>  weaponFx;         // weapon effects: engaged when last seen, by character
    uint32_t                            flashTarget = 0;  // Config target visual effect: who pulses, since when
    // target_flash.cpp: the target visual effect's brightness for an actor now (0 none .. 1 peak).
    float targetFlash(uint32_t id);
    std::vector<client::FxMotion>       eventMotions;            // motions the event scripts asked for
    std::unordered_map<uint32_t, std::vector<uint32_t>>               eventMotionFiles; // actor -> its event motion DATs
    std::unordered_map<uint32_t, std::pair<uint32_t, dat::FxName>>    eventMotionSched; // actor -> (DAT, scheduler)
    bool                                eventKey = false;        // Confirm / Cancel while a message is up
    // The Mog House: furniture, the exit door and layout mode (mog_house.h).
    app::MogHouseView mogView{ index, items, { clientText ? &*clientText : nullptr, unityText ? &*unityText : nullptr,
                                               areaNames ? &*areaNames : nullptr, helpText ? &*helpText : nullptr } };
    std::chrono::steady_clock::time_point nextAuto = std::chrono::steady_clock::now(); // --auto-fight pacing
    uint8_t   heading = 0;
    float     headingExact = -1.0f; // the unrounded heading from movement (the drawn model turns toward it)
    int       ownMoveDir = 0; // engaged movement relative to the facing: 0 forward, 1 back, 2 left, 3 right
    std::unordered_map<uint32_t, uint8_t> faceOverride; // NPCs an event turned (op 0x1E): id -> heading byte
    // Other actors stood on the floor (client::actorGround, updateWorld): id -> the server position it was worked out for
    // and the height (game space) it stands at; cleared with the scene.
    std::unordered_map<uint32_t, std::pair<glm::vec3, float>> groundedAt;
    const gfx::ZoneScene*                                     groundedScene = nullptr;
    void                                                      standOnGround(std::vector<client::Entity>& entities);
    uint32_t  followId = 0;     // /follow: the entity to walk after (0 = none)
    // --script (apps/xi-client/test_script.h): the steps, and what a step asks of this frame.
    app::TestScript script;
    std::optional<glm::vec2> scriptGoto;    // walk toward this game x / z at run speed
    std::string     scriptCmd;              // a chat-line command to run
    uint32_t        scriptTradeSeen = 0;    // `trade offer`: the partner's offer counter when the step began
    std::optional<std::chrono::steady_clock::time_point> scriptKeysFrom; // interface keys start from here
    const std::filesystem::path scriptDir = args.script.empty() ? std::filesystem::path() : std::filesystem::path(args.script).parent_path();
    bool lockOn = false; // the target lock (Num *, /lockon, engaging): target, camera and facing stay on it
    // Config flags (/anon, /away, /autotarget, /displayhead, /invite): what this client last asked for.
    std::unordered_map<std::string, bool> configFlags;
    bool      quitting  = false;

    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    float                                 fps  = 0.0f;
    gfx::FrameTrace                       frameTrace; // XI_FRAME_TRACE
    int                                   exitCode_ = 0; // --script that did not load: 1

    // Each stage's own state, kept between frames (only that stage's file uses it).
    struct InputState // input.cpp
    {
        uint32_t soundTargetSeen = 0;     // the target the last target sound was for
        bool     targetEscaped   = false; // Escape cleared the target this frame (its sound)
        uint32_t engagedId       = 0;     // the monster we engaged (for the Attack key's switch-target)
        bool     lockWasEngaged  = false; // engaged last frame (the target lock's engage / disengage edges)
        uint32_t autoTargetSeen  = 0;
        bool     confirmKey      = false; // Confirm with no menu open: talk to an NPC target, else the chat line
    } inputState_;
    struct MovementState // movement.cpp
    {
        // The game's camera: an eye trailing a look-at point (client::GameCamera; XI_CAM_ORBIT=1 gives the earlier
        // fixed-distance orbit, for comparison).
        client::GameCamera gameCam;
        bool      camZoomReset = false;  // Zoom In + Out held: easing back to 350 (DAT_10456d84)
        bool      camGivenUsed = false;  // --yaw / --cam-pitch / --cam-dist placed the first view
        glm::vec3 camLastFeet{ 0.0f };   // the feet the game camera saw last frame (a jump places it again)
        bool      camFeetSeen = false;
        float     fallSpeed   = 0.0f;    // game y per 1/60 s tick while falling
        std::optional<std::chrono::steady_clock::time_point> zoneLineAt; // when the last 0x05E went out
        uint32_t  blacklistSeen = 0;     // WorldSnapshot::blacklistSerial answered in the log
    } moveState_;
    struct ZoneFrameState // zone_frame.cpp
    {
        int                                                  failedZone   = -1;
        uint32_t                                             seenTeleport = 0;
        std::optional<std::chrono::steady_clock::time_point> firstWsAt;
        std::optional<std::chrono::steady_clock::time_point> layerSeenAt; // --shot-on-layer
        bool                                                 layerShotDone = false;
        // The zone change fade (the game's `fdo0` / `fdi0` in file 30904, CODE 0x1008ab10): black over the scene, 0 clear
        // .. 1 black. Phase 0 none, 1 fading out, 2 black (waiting for the new zone), 3 fading in.
        float fade       = 0.0f;
        float fadeFrom   = 0.0f;
        float fadeTime   = 0.0f;
        int   fadePhase  = 0;
        int   fadeFrames = 0;     // frames drawn with the new zone before the fade in starts
        bool  wasInZone  = false;
    } zoneState_;
    // The zone change fade is still darkening the old zone while the server has already put us in the new one: the
    // old scene stays on screen, so nothing that reads its collision (movement, camera) runs (zone_frame.cpp).
    [[nodiscard]] bool zoneFadeHold() const
    {
        return zoneState_.fadePhase == 1 && scene && world.inZone && (scene->zone() != world.zone || scene->model() != world.zoneModel);
    }
    struct UiFrameState // ui_frame.cpp
    {
        int  uiTick      = 0;
        bool textInputOn = false;
        // Auction house lists from the search server (GameUiActions::auctionList / auctionHistory).
        std::future<std::pair<int, std::vector<client::AuctionItem>>> auctionListJob;
        std::future<std::vector<client::AuctionSale>>                 auctionHistoryJob;
        std::vector<client::AuctionItem>                              auctionItems;
        std::vector<client::AuctionSale>                              auctionSales;
        int                                                           auctionCategory   = -1;
        bool                                                          auctionSalesReady = false;
        std::future<std::optional<client::SearchResult>>              searchWindowJob; // the Search window's query
        std::optional<client::SearchResult>                           searchResult;
        bool                                                          searchAllAreas = false;
    } uiState_;
    struct RenderState // render.cpp
    {
        uint16_t                              shownWeather = 0xFFFF, fromWeather = 0xFFFF;
        std::chrono::steady_clock::time_point weatherChanged = std::chrono::steady_clock::now();
        // An effect's environment (EffectSystem::environment, ops 0x7C / 0xAE / 0x7E): the record shown last frame
        // and the cross-fade from it when a request arrives.
        uint32_t                              envSerial = 0;
        float                                 envBlend  = 0.0f;
        dat::WeatherRecord                    envFrom, lastWeatherRec;
        std::chrono::steady_clock::time_point envChanged = std::chrono::steady_clock::now();
    } renderState_;
    struct DlssState // render.cpp, only with --dlss or --fsr (issue #139): the upscaler, DLSS or FSR
    {
        std::unique_ptr<gfx::Upscaler>      dlss;
        std::unique_ptr<gfx::MotionVectors> motion;
        std::shared_ptr<gfx::Image>         output; // window size, RGBA16F, DLSS's storage target (freed with the ctx)
        glm::mat4                           prevViewProj{ 1.0f };
        glm::vec3                           prevEye{ 0.0f };
        const void*                         prevScene = nullptr;
        uint16_t                            prevZone  = 0;
        uint64_t                            frame     = 0;
        bool                                history   = false; // a previous frame to reuse (else reset)
    } dlss_;
    void startDlss();
    // --fsr (issue #139), or --dlss where DLSS didn't start: FSR 3.1 into the same state.
    void startFsr();
    // The time since the last frame, for FSR (render.cpp).
    std::chrono::steady_clock::time_point upscaleLast_{};
    // --rt (goal 38): the ray tracer, made before any zone or character buffer (they get its usage flags).
    std::unique_ptr<gfx::RayTracing> rt_;
    // --pathtrace (goal 41 option 3): path-traced lighting on RayTracing's geometry (rt_ is made for it too).
    std::unique_ptr<gfx::PathTracer> pt_;
    struct RtState
    {
        glm::mat4   prevViewProj{ 1.0f };
        glm::vec3   prevEye{ 0.0f };
        const void* prevScene = nullptr;
        uint16_t    prevZone  = 0;
        float       sunShadow = 1.0f; // eased toward 1 outdoors, kIndoor under a roof
        std::vector<glm::vec4> water;  // this frame's water planes from the sea effects (RayTracing::View::water)
        std::size_t            waterLogged = 0;
    } rtState_;
    void startRt();
    // --post (goal 41): bloom, light shafts, fog and the tone map over the finished scene, before the interface;
    // nothing of it exists without the flag. postView_: this frame's light, fog and camera for it.
    std::unique_ptr<gfx::PostProcess> post_;
    gfx::PostProcess::View            postView_;
    void startPost();
    static gfx::ContextExtensions prepareExtensions(const Args& a);
    // Between the characters and the effects: TLAS, trace, denoise, re-light the scene colour (offscreen target).
    void rtLighting(VkCommandBuffer cmd, uint32_t slot, VkExtent2D render, const glm::mat4& vpScene, const gfx::SceneParams& sp,
                    const gfx::SkyRenderer::Gradient& sky);
    // Before the context: NGX's extensions, and the texture LOD bias for the texture sets made after it.
    static gfx::ContextExtensions prepareDlss(const Args& a);
    // After the scene pass: motion vectors, DLSS into the window-size output, copied to `dst` (left as a colour
    // attachment for the interface). False = DLSS failed; the caller scales the scene as usual.
    // `fxAfter` (Ray Reconstruction, issue #101): the effects were left out of the scene; they draw over the copy in
    // `dst` (view `dstView`) before --post.
    bool dlssResolve(VkCommandBuffer cmd, uint32_t slot, VkImage dst, VkImageView dstView, VkExtent2D ext,
                     VkExtent2D render, const glm::mat4& vpScene, glm::vec2 jitterPx, glm::vec2 jitterNdc,
                     bool fxAfter);
    // The effect batches (not the screen-sampling ones) over the window-size image `dst` (a colour attachment), depth-
    // tested against the scene's depth scaled up to it (MotionVectors::drawDepth).
    void drawEffectsOver(VkCommandBuffer cmd, uint32_t slot, VkImageView dst, VkExtent2D ext, glm::vec2 jitterPx);
    // What Ashita addons set through the game's memory (addon_native.cpp): the client's own behaviour unless one does.
    struct AddonOverrides
    {
        float                   drawWorld  = 1.0f; // drawdistance: factor on the zone's fog distances
        float                   drawEntity = 1.0f; // factor on the actors' fog distances
        std::optional<uint32_t> clearColour;       // clearcolor: D3DCOLOR behind the scene
        int                     fpsDivisor = 0;    // fps: frames at 60 / n a second (0 = no cap, as today)
    } addonOverrides_;
    struct TargetFlashState // target_flash.cpp
    {
        std::chrono::steady_clock::time_point flashAt = std::chrono::steady_clock::now(); // flashTarget's pulse start
        // Config Misc. 3 AoE range graphics (cfg 192): actors inside the sub-targeted area spell's radius, since when.
        std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> aoeFlash;
        uint64_t                                                            aoeFlashTick = ~0ull;
    } flashState_;
    struct ScriptState // script_steps.cpp
    {
        bool started = false;
        int  shots   = 0;
    } scriptState_;
    struct FishState // fishing.cpp
    {
        std::unordered_map<uint32_t, uint8_t>               status; // each player's last status seen
        std::unordered_map<uint32_t, std::vector<uint32_t>> files;  // players fishing: their race's fishing files
        std::unordered_map<uint32_t, uint32_t>              rods;   // players fishing: their rod sub-actor's file
        const void*                                          waterFor = nullptr; // the session XI_FAKE_FISH's water test is on
    } fishState_;
    static bool                         fishingStatus(uint8_t status);
    const std::vector<uint32_t>*        fishingFilesOf(uint32_t id) const;

    // This frame (set by the stages in order).
    std::chrono::steady_clock::time_point now{};
    float                                 dt      = 0.0f;
    bool                                  testRun = false; // --screenshot / --script runs ignore real input
    client::WorldSnapshot                 world;           // the session's snapshot (updateWorld)
    app::MogHouseView::Furniture          mhf;             // Mog House furniture in it
    const client::Entity*                 target = nullptr; // the selected entity (updateTargets)
    uint64_t                              propsKey  = 0;    // this frame's scene props (updateZone)
    float                                 sinceZone = 0.0f; // seconds since zone-in
    bool                                  dead      = false;
    gfx::FlyCamera                        cam;              // this frame's camera (updateCamera)
    bool                                  selfHidden         = false; // an event hides the player
    bool                                  ownMovingThisFrame = false;
    float                                 ownEventSpeed      = 0.0f; // an event moving us: its speed (walk / run motion)
    int                                   ww = 0, wh = 0;   // window size (drawInterface)
    ui::UiCanvas                          canvas{};         // the interface canvas
    std::vector<gfx::MarkerRenderer::Marker> mk;            // markers (drawActors)
    std::unordered_map<uint32_t, float>   modelHeight;      // entity id -> label height
    float                                 aspect = 1.0f;
    glm::mat4                             viewProj{ 1.0f };

};

} // namespace xi::app
