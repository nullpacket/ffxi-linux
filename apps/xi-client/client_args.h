// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "xi/settings.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// xi-client's command line (parseArgs in main.cpp) and small helpers shared by its translation units.
namespace xi::app
{

struct Args
{
    std::vector<std::string> argv; // the command line as given (scrubbed of passwords before any report)
    std::string host = "127.0.0.1";
    // The server's ports (--auth-port / --data-port / --view-port / --search-port; LSB's defaults).
    uint16_t    authPort = 54231, dataPort = 54230, viewPort = 54001, searchPort = 54002;
    std::string user;
    std::string pass;
    std::string character;
    std::string root;
    std::string clientVer = "30260604_0";
    std::string compressDat;
    bool        validation  = false;
    bool        autoconnect = false;
    bool entityRenames = false; // --entity-renames / XI_ENTITY_RENAMES=1: ask the server for its rename list (#138)
    std::string dlss;        // --dlss[=mode]: "" = off; quality / balanced / performance / ultra-performance / dlaa (goal 15)
    std::string fsr; // --fsr[=mode]: "" = off; quality / balanced / performance / ultra-performance / native (#139)
    float       fsrSharpness = 0.2f; // --fsr-sharpness=0..1: FSR's sharpening (0 = off)
    std::string upscaler; // --upscaler=dlss|fsr|auto|off; "" = what --dlss / --fsr say (auto: DLSS if it can, else FSR)
    bool        rt = false;  // --rt[=ao,shadows,gi,reflections|all]: ray-traced lighting (goal 38); off = exactly as without it
    std::string rtEffects;   // the list after --rt= ("" = ao,shadows)
    std::string hdTextures;  // --hd-textures[=DIR]: "" = off; the HD texture cache (goal 41, dat/hd_textures.h)
    float       rtStrength = 0.7f; // --rt-strength=0..2 (default 0.7: subtle)
    bool        rtStrengthGiven = false;
    bool        rtEffectsGiven  = false;   // --rt / --rt= on the line (else --rt-boost turns on all)
    std::string rtBoost;                   // --rt-boost[=low|high|max] (goal 41): "" = off
    std::optional<float> rtShadowCap;      // --rt-shadow-cap=0..1: the most light 0's shadow removes (0.4)
    std::optional<float> rtReflect;        // --rt-reflect=0..3: the water reflection's weight scale (1)
    std::string post;                      // --post[=hdr,bloom,godrays,fog,all] (goal 41): "" = off
    float       postBloom = 1.0f, postShafts = 1.0f, postFog = 1.0f; // --post-bloom= / --post-shafts= / --post-fog=: x the defaults
    bool        pathtrace = false;   // --pathtrace[=bounces 1..3]: path-traced lighting (goal 41 option 3); off = as without it
    std::string ptBounces;           // the value after --pathtrace= ("" = 2)
    float       ptBlend    = 1.0f;   // --pathtrace-blend=0..1: 0 the game's baked look, 1 traced
    float       ptExposure = 0.0f;   // --pathtrace-exposure=EV
    float       ptBaked    = 0.3f;   // --pathtrace-baked=0..1: the vertex colours' baked shading kept
    // Test hooks: hold W for `walk` seconds after zoning in; screenshot `after` seconds after zone-in, then quit.
    std::string screenshot;
    float       after = 5.0f;
    float       walk  = 0.0f;
    float       camYaw = 0.0f;
    std::string say; // test hook: shouted once, 1 s after zone-in
    std::optional<glm::vec2> gotoXZ; // test hook: walk (fast) toward this game-space x/z after zone-in
    std::string display;             // "", "primary", "secondary" or an index (env XI_DISPLAY)
    float       labelRange = 40.0f;
    bool        tab        = false; // test hook: one Tab press 4 s after zone-in
    std::string targetName;          // test hook: with --tab, target the nearest entity whose name contains this ('|' = alternatives)
    float       attackAt   = -1.0f;  // test hook: press F this many seconds after zone-in
    bool        autoFight  = false;  // test hook: keep engaging --target-name mobs; weapon skill at 1000 TP
    bool        autoWs     = true;   // test hook: --auto-fight weapon skills at 1000 TP (script `autows off` keeps the TP)
    bool        autoRanged = false;  // test hook: with --auto-fight, a ranged attack every 6 s while engaged
    bool        autoFish   = false;  // test hook: the fishing minigame presses each arrow and reels in
    std::string fx;                   // test hook: "FILE:NAME" plays that effect scheduler on us every 2 s
    float       wsAt       = -1.0f;
    float       castAt     = -1.0f;  // test hook: cast the first usable enemy spell on the target at this time
    bool        homepoint  = false;
    float       talkAt     = -1.0f;
    float       jaAt       = -1.0f;  // test hook: use a job ability (on yourself) at this time
    int         jaId       = -1;     //   ... this one (default: the first known)
    int         useItemId  = -1;     // test hook: use this item 8 s after zone-in  // test hook: press E this many seconds after zone-in  // test hook: return to the home point 5 s after being knocked out
    std::string script;                 // test hook: play this step file (apps/xi-client/test_script.h)
    std::string uiKeys;                 // test hook: keys fed to the interface from 5 s after zone-in, one per 0.4 s
                                        //   ("down,down,enter,t:hello,enter"; t: types text)
    float       shotAfterLogout = -1.0f; // test hook: with --screenshot, a second capture (<name>_logout.png) this long into the logout
    int         shotOnLayerOp  = -1;    // test hook "--shot-on-layer OP:SEC": capture SEC s after camera op OP (14 / 16) starts
    float       shotOnLayerSec = 0.0f;
    float       shotAfterPopup = -1.0f; // test hook: with --screenshot, capture this many seconds after the first damage pop-up
    float       shotAfterWs = -1.0f;    // test hook: with --screenshot, capture this many seconds after our first weapon skill
    float       shotAfterDespawn = -1.0f; // test hook: with --screenshot, capture this long after a corpse starts to dissolve
    std::string uiOpen;                 // test hook: start with this menu open (main, status, equip, items, magic, abilities)
    int         equipItem  = -1;     // test hook: equip this item id (first fitting slot) 6 s after zone-in  // test hook: use the first weapon skill this many seconds after zone-in
    bool        chase      = false;  // test hook: run to melee range of the (mob) target, engaged or not
    double      timeOverride   = -1.0; // test hook: fixed Vana'diel time of day (seconds)
    int         weatherOverride = -1;  // test hook: fixed weather id
    float       camDist    = 8.0f;
    float       camPitch   = -20.0f; // degrees
    bool        camGiven   = false;  // --yaw / --cam-pitch / --cam-dist: the camera starts at that view
    DisplaySettings display_;            // window / background / menu resolution (Ashita-style [ffxi.registry] keys)
    std::string uiSheet;              // debug: draw this interface sprite sheet as a grid ("menu windowps")
    std::vector<std::string> uiLayout; // debug: draw these window layouts ("menu menuwind"), repeatable
    int         uiFirst    = 0;       //   ... starting at this sprite
};

// Job abbreviation by job id (WAR, MNK ...; "---" for 0).
const char* jobName(uint8_t j);

} // namespace xi::app
