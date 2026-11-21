// SPDX-License-Identifier: GPL-3.0-or-later
// Zone (re)load, Mog House props and ships as scene props, timed captures, teleports.
#include "client_app.h"

namespace xi::app
{

void ClientApp::updateZone()
{
    // Zone (re)load and server-forced positions.
    // Mog House furniture (mog_house.h): static models are scene props (posed every frame), statues and mannequins
    // are in the entity list. The inventory arrives after zone-in, so the scene is rebuilt once the set of models
    // has been the same for a second.
    std::vector<dat::ZoneProp> props    = mhf.props;
    propsKey                            = mhf.key;
    static uint64_t propsSeen = 0;
    static auto     propsSince = std::chrono::steady_clock::now();
    if (propsKey != propsSeen)
    {
        spdlog::debug("mog house: {} furniture props (safe items: {})", props.size(),
                      std::count_if(world.inventory.begin(), world.inventory.end(), [](const client::InventoryItem& i) { return i.container == 1 || i.container == 9; }));
        propsSeen  = propsKey;
        propsSince = std::chrono::steady_clock::now();
    }
    const bool propsChanged = scene && scene->propsKey() != propsKey &&
                              std::chrono::steady_clock::now() - propsSince > std::chrono::seconds(1);
    // The zone change fade (CODE 0x1008ab10, the game's own fade task): when the zone is left (S2C 0x00B, flag set at
    // 0x100daeed) the game plays scheduler `fdo0` of file 30904 (op 0x0F: to R G B A 00 00 00 80, black, over 30 ticks)
    // and fades the music and sounds to 0 over 30 ticks (0x10035c70 / 0x10035cc0); after the new zone is loaded and
    // C2S 0x00C sent (0x10093d32) it plays `fdi0` (op 0x0F: to 80 80 80 00, clear, over 30 ticks) and brings the sound
    // back over 30 ticks. The black is drawn over the scene and the names, under the windows. Here the old zone keeps
    // drawing under the fade out and the new one is built once it is black. XI_ZONE_FADE=0 turns it off;
    // XI_FADE_TRACE=1 logs the level each frame.
    {
        static const bool fadeOn    = !(std::getenv("XI_ZONE_FADE") && std::string_view(std::getenv("XI_ZONE_FADE")) == "0");
        static const bool fadeTrace = std::getenv("XI_FADE_TRACE") != nullptr;
        constexpr float   kFade     = 30.0f / 60.0f; // 30 ticks
        constexpr float   kBlackMax = 20.0f;         // never stay black longer than this waiting for a zone-in
        auto&             zs        = zoneState_;
        const bool        zoneChange = scene && world.inZone && (scene->zone() != world.zone || scene->model() != world.zoneModel);
        const bool        leaving    = zs.wasInZone && !world.inZone && session && !world.choosingCharacter;
        if (!fadeOn || !session || world.choosingCharacter)
        {
            zs.fade = 0.0f, zs.fadePhase = 0;
        }
        else if ((leaving || zoneChange) && (zs.fadePhase == 0 || zs.fadePhase == 3))
        {
            zs.fadePhase = 1, zs.fadeTime = 0.0f, zs.fadeFrom = zs.fade;
            sound.fadeMusic(0.0f, kFade);
        }
        zs.fadeTime += dt;
        if (zs.fadePhase == 1)
        {
            zs.fade = std::min(1.0f, zs.fadeFrom + (1.0f - zs.fadeFrom) * zs.fadeTime / kFade);
            if (zs.fade >= 1.0f)
            {
                zs.fadePhase = 2, zs.fadeTime = 0.0f, zs.fadeFrames = 0;
            }
        }
        else if (zs.fadePhase == 2)
        {
            zs.fade = 1.0f;
            const bool loaded = scene && world.inZone && scene->zone() == world.zone && scene->model() == world.zoneModel;
            zs.fadeFrames     = loaded ? zs.fadeFrames + 1 : 0;
            if (zs.fadeFrames > 2 || zs.fadeTime > kBlackMax || zoneState_.failedZone == world.zone)
            {
                zs.fadePhase = 3, zs.fadeTime = 0.0f;
                sound.fadeMusic(1.0f, kFade);
            }
        }
        else if (zs.fadePhase == 3)
        {
            zs.fade = std::max(0.0f, 1.0f - zs.fadeTime / kFade);
            if (zs.fade <= 0.0f)
            {
                zs.fadePhase = 0;
            }
        }
        zs.wasInZone = world.inZone;
        if (fadeTrace && zs.fadePhase != 0)
        {
            spdlog::info("fade: phase {} level {:.3f}", zs.fadePhase, zs.fade);
        }
    }
    if (world.inZone && (!scene || scene->zone() != world.zone || scene->model() != world.zoneModel || propsChanged) && zoneState_.failedZone != world.zone &&
        zoneState_.fadePhase != 1)
    {
        if (session && (!scene || scene->zone() != world.zone || scene->model() != world.zoneModel) && zoneState_.fadePhase == 0 &&
            !(std::getenv("XI_ZONE_FADE") && std::string_view(std::getenv("XI_ZONE_FADE")) == "0"))
        {
            // A zone-in with no fade out before it (the first one after login): start black and fade in.
            zoneState_.fadePhase = 2, zoneState_.fadeTime = 0.0f, zoneState_.fadeFrames = 0, zoneState_.fade = 1.0f;
        }
        ctx.waitIdle();
        scene.reset();
        subScene.reset(); // an event's sub-map does not outlive its zone
        propLayer.reset(); // the ships' hulls go with their zone
        try
        {
            smoothPos.clear();
            deadSeen.clear();
            corpses.clear();
            animTracks.clear();
            effects.reset();
            actionFx.reset();
            doors.reset();
            steps.reset();
            scene     = std::make_unique<gfx::ZoneScene>(ctx, index, keys, world.zone, world.zoneModel, props, propsKey);
            propLayer = std::make_unique<gfx::PropLayer>(ctx, index, keys);
            zoneNames = dat::loadEntityNames(index, world.zone);
            // "Resources ready" (C2S 0x01A ActionID 0x14, CODE 0x10088200, once per zone): LSB spawns the
            // Mog House Moogle only after it.
            if (session)
            {
                session->action(world.charId, world.actIndex, 0x14);
            }
            // The zone's own ambient effects: sea, smoke, windmills, lamps (zone file generators).
            const auto zoneFx = std::getenv("XI_NO_ZONE_FX") ? 0 : effects.startZone(dat::zoneModelFileId(world.zoneModel), [sc = scene.get()](dat::FxName n) {
                std::string chunk;
                for (int i = 0; i < 4 && ((n >> (8 * i)) & 0xFF) != 0; ++i)
                {
                    chunk += static_cast<char>((n >> (8 * i)) & 0xFF);
                }
                return sc->effectMesh(chunk);
            });
            spdlog::info("zone {}: {} ambient effect generators", world.zone, zoneFx);
            faceOverride.clear();
            zoneError.clear();
        }
        catch (const std::exception& e)
        {
            zoneState_.failedZone = world.zone;
            zoneError  = fmt::format("could not load zone {}: {}", world.zone, e.what());
            spdlog::error("{}", zoneError);
        }
    }
    // Ships (kind 4; ship_re.md): each hull (DAT 31004 + id) is posed every frame by client::Ships, and hidden once the
    // server no longer sends its entity or hides it. Taking or freeing a hull's slot leaves the zone as it is.
    if (scene && propLayer)
    {
        std::vector<uint32_t> present;
        for (const auto& e : world.entities)
        {
            // A ship away on its voyage keeps its entity with the HideFlag (LSB ship.cpp: status Disappear).
            if (const auto ship = client::Ships::shipId(e); ship && !e.hiddenByStatus(cutscene.has_value()))
            {
                const auto pose = ships.pose(index, e);
                if (propLayer->pose(e.id, client::Ships::fileFor(*ship), pose.pos, pose.yaw))
                {
                    present.push_back(e.id);
                }
            }
        }
        propLayer->keepOnly(present);
    }
    // Config Global: auto-disconnect after 10..60 idle minutes (cfg 65, -1 off). The game leaves this to
    // PlayOnline (INFERRED); here it logs out like /logout.
    if (const int idle = cfg(65, -1); idle > 0 && inGame && !quitting && now - lastInput > std::chrono::minutes(idle))
    {
        spdlog::info("config: no input for {} minutes: logging out", idle);
        quitting = true;
    }
    if (world.inZone && scene && !zonedAt)
    {
        zonedAt = now;
        if (session)
        {
            // Own equipment look (cfg 191): after each zone-in the game re-sends C2S 0x053 Mode 1 (continue)
            // when it is fixed, else Mode 0 (CODE 0x101a7c80).
            std::vector<std::byte> pkt(4 + 16 * 8);
            pkt[1] = static_cast<std::byte>(cfg(191, 0) == 1 ? 1 : 0);
            session->send(0x053, std::move(pkt));
        }
    }
    sinceZone = zonedAt ? std::chrono::duration<float>(now - *zonedAt).count() : 0.0f;
    if (args.shotAfterPopup >= 0.0f && firstPopupAt && !args.screenshot.empty() && !captureNext && !quitting &&
        std::chrono::duration<float>(now - *firstPopupAt).count() >= args.shotAfterPopup)
    {
        captureNext = true;
    }
    if (args.shotOnLayerOp >= 0 && !args.screenshot.empty() && !captureNext && !quitting)
    {
        if (!zoneState_.layerSeenAt && effects.layerActive(args.shotOnLayerOp))
        {
            zoneState_.layerSeenAt = now;
        }
        if (zoneState_.layerSeenAt && !zoneState_.layerShotDone && std::chrono::duration<float>(now - *zoneState_.layerSeenAt).count() >= args.shotOnLayerSec)
        {
            // One capture, without quitting (a --script run carries on and logs out itself).
            captureNext   = true;
            shotPath      = args.screenshot;
            zoneState_.layerShotDone = true;
        }
    }
    if (!args.screenshot.empty() && args.shotAfterWs < 0.0f && args.shotAfterDespawn < 0.0f && args.shotAfterPopup < 0.0f && args.shotOnLayerOp < 0 && zonedAt && sinceZone >= args.after && !quitting && !captureNext)
    {
        captureNext = true;
    }
    // --shot-after-ws: the capture follows our first weapon skill instead of the zone-in timer.
    if (args.shotAfterWs >= 0.0f && !args.screenshot.empty())
    {
        if (world.weaponSkillsDone > 0 && !zoneState_.firstWsAt)
        {
            zoneState_.firstWsAt = now;
        }
        if (zoneState_.firstWsAt && std::chrono::duration<float>(now - *zoneState_.firstWsAt).count() >= args.shotAfterWs && !captureNext && !quitting)
        {
            captureNext = true;
        }
    }
    // In the lobby (choosing a character), or back at the login form after a session ended (#43), time counts from the
    // connect.
    if (!args.screenshot.empty() && !zonedAt && (world.choosingCharacter || !session) && connectedAt &&
        std::chrono::duration<float>(now - *connectedAt).count() >= args.after && !quitting && !captureNext)
    {
        captureNext = true;
    }
    if (!args.say.empty() && zonedAt && sinceZone > 1.0f)
    {
        session->say(args.say, 1);
        args.say.clear();
    }
    if (world.teleportGeneration != zoneState_.seenTeleport)
    {
        zoneState_.seenTeleport = world.teleportGeneration;
        zoneLineSent = false;
        feet         = gfx::ZoneScene::gameToRender(world.serverPos);
        heading      = world.serverDir;
        snapPending  = true;
        camReset     = true; // the camera starts behind the character again (CODE 0x100215b0)
    }
    // Arrivals stand on the floor (the game ray-casts down 30 from the spawn point; Mog House CODE
    // 0x10094773): once the zone's collision is loaded.
    if (snapPending && scene && scene->zone() == world.zone && scene->model() == world.zoneModel)
    {
        snapPending = false;
        auto h = scene->groundHeight(feet.x, feet.z, feet.y + 0.5f);
        if (h && *h < feet.y - 0.5f)
        {
            // A stair riser's sliver over a lower floor is the first thing the game's ray meets (Port Bastok's harbour
            // stairs over the sea).
            if (const auto s = scene->collision().sliverHeight(feet.x, feet.z, *h, feet.y + 0.5f))
            {
                h = s;
            }
        }
        if (h && *h > feet.y - 30.0f)
        {
            feet.y = *h;
        }
    }
}

} // namespace xi::app
