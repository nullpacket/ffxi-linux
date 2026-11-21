// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "client/effects.h"
#include "client/session.h"
#include "dat/event_runner.h"

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

// Runs the server's events (S2C 0x032 / 0x034 / zone-in) with dat::EventRunner: dialog, and cutscenes with their actors,
// camera shots and fades (docs/event-format.md). Actor overrides ("puppets") are read by the renderer while it runs.
namespace xi::client
{

class Cutscene : public dat::EventHost
{
public:
    struct Hooks
    {
        std::function<float(const glm::vec3& gamePos)>                       ground;     // floor height (game y) under a point
        std::function<glm::vec3()>                                           playerPos;  // our feet, game space
        std::function<bool(uint32_t id)>                                     loaded;     // the entity's model is known
        std::function<void(uint32_t id, uint32_t target, uint32_t fcc, int file, int raceSet)> motion;
        std::function<bool(uint32_t id)>                                     motionBusy;
        std::function<void(int slot, uint16_t id)>                           music;
        std::function<void(uint32_t id)>                                     sound;
        std::function<void(uint32_t message, uint32_t speaker)>              message;
        std::function<void(uint32_t message, uint32_t hideMask, uint32_t defaultOption)> menu;
        // Ops 0x69 / 0x6A: sound channel levels (0..1): zone effect sounds, sound effects, ambience.
        std::function<void(float effects, float se, float ambience)>         volume;
        // Ops 0xC8 / 0x8B / 0x8A: the event map window: open map `map` of `zone` (map < 0 closes it), a marker.
        std::function<void(int zone, int map)>                               eventMap;
        std::function<void(int slot, float x, float z, const std::string& label)> eventMapMarker;
        std::function<void(uint16_t model)>                                  subMap; // op 0x75 00: show zone model `model`
        std::function<void(const std::string& name, int gauge)>             eventTarget;    // op 0xB4 02 / 03 (gauge -1)
        std::function<void(int which, int ticks)>                            eventTimer;     // op 0xB4 07..0B (ticks -1)
        std::function<int(int which)>                                        eventTimerLeft; // op 0xB4 09 / 0C
        std::function<void(float x, float y, float zoom, float seconds, int flags)> mapView; // op 0xB4 14
    };
    // What the renderer uses instead of the server's state while an event moves an actor.
    struct Puppet
    {
        std::optional<glm::vec3> pos; // game space
        std::optional<float>     yaw; // radians, a = atan2(-dz, dx)
        bool                     hidden  = false;
        float                    alpha   = 1.0f;  // op 0x6C model alpha: 1 solid .. 0 gone
        bool                     walking = false; // moved this frame
        float                    speed   = 0.0f;  // at this many units a second (the script's op 0x32)
        uint32_t                 idle    = 0;     // op 0x5E idle motion (fcc), 0 = the normal one
        // Head look-at (ops 0x79 / 0x7B): 0 none, 1 at lookTarget's skeleton point lookPoint, 2 at fixed angles
        // (radians; pitch up, yaw offset from the body) 10 units out from the feet.
        int                      lookMode   = 0;
        uint32_t                 lookTarget = 0;
        int                      lookPoint  = 0;
        float                    lookPitch = 0, lookYaw = 0;
        std::optional<uint8_t>   animation; // ops 0x4C / 0x4D / 0x8E / 0x8F / 0x4F (doors)
    };

    Cutscene(Session& session, EffectSystem& effects, const dat::DatIndex& index, Hooks hooks);

    // Per frame: starts a new event from the snapshot, feeds server answers, runs the threads.
    void frame(const WorldSnapshot& world, float dt);
    [[nodiscard]] bool active() const
    {
        return runner_ != nullptr;
    }
    // Op 0x46 01 .. 00: the camera follows the shots instead of the player.
    [[nodiscard]] bool cameraMode() const
    {
        return active() && cameraMode_;
    }
    [[nodiscard]] const Puppet* puppet(uint32_t id) const
    {
        const auto it = puppets_.find(id);
        return it == puppets_.end() ? nullptr : &it->second;
    }
    // The look to draw for an entity whose server look is `server` (op 0xB6 costumes last past the event, until the
    // server sends a new look), and its scale (1 = as modelled).
    [[nodiscard]] Look  look(uint32_t id, const Look& server) const;
    [[nodiscard]] float scaleOf(uint32_t id) const;
    // A message is up (Enter closes it).
    [[nodiscard]] bool waitingMessage() const
    {
        return runner_ && runner_->waitingMessage();
    }
    // The message window was closed (Enter) / a menu option chosen (-1 = cancelled).
    void closeMessage();
    void choose(int option);
    // A new zone: everything stops.
    void reset();
    // Test hook: runs `owner`'s event `event` of this zone's event file without the server (its updates are
    // answered at once with `params` again, and its end is not sent), so events the server does not script can be
    // seen. `strings` as S2C 0x033's.
    void startLocal(uint16_t zone, uint32_t owner, uint16_t event, const std::array<uint32_t, 8>& params,
                    const std::array<std::string, 4>& strings = {});
    // After frame(): the event's overrides on the frame's snapshot (actor animations, ops 0x77 weather); main draws
    // and runs the doors from it.
    void apply(WorldSnapshot& world) const;
    // Op 0x77: the event's Vana'diel hour (0..23), while it runs.
    [[nodiscard]] std::optional<int> hour() const
    {
        return active() ? hour_ : std::nullopt;
    }

    // dat::EventHost
    [[nodiscard]] uint32_t     player() const override;
    [[nodiscard]] bool         exists(uint32_t id) const override;
    void                       request(uint32_t id) override;
    [[nodiscard]] dat::EventVec position(uint32_t id) const override;
    void                       place(uint32_t id, const dat::EventVec& pos, std::optional<float> yaw, bool snapToGround) override;
    void                       face(uint32_t id, float yaw) override;
    void                       walking(uint32_t id, float speed) override;
    void                       hide(uint32_t id, bool hidden) override;
    void                       motion(uint32_t id, uint32_t target, uint32_t fcc, int file, int raceSet) override;
    [[nodiscard]] bool         motionBusy(uint32_t id) const override;
    void                       idle(uint32_t id, uint32_t fcc) override;
    void                       lookAt(uint32_t id, uint32_t target, int point) override;
    void                       lookAngles(uint32_t id, int pitch, int yaw) override;
    void                       lookStop(uint32_t id) override;
    void                       costume(uint32_t id, int slot, uint16_t value) override;
    void                       scale(uint32_t id, float scale) override;
    void                       scheduler(uint32_t file, uint32_t caster, uint32_t target, uint32_t fcc) override;
    void                       stopScheduler(uint32_t file, uint32_t caster, uint32_t fcc) override;
    [[nodiscard]] bool         schedulerRunning(uint32_t file, uint32_t caster, uint32_t fcc) const override;
    void                       cameraMode(int mode) override;
    void                       music(int slot, uint16_t id) override;
    void                       sound(uint32_t id) override;
    void                       message(uint32_t message, uint32_t speaker) override;
    void                       menu(uint32_t message, uint32_t hideMask, uint32_t defaultOption, uint32_t speaker) override;
    void                       nameRecord(std::size_t slot, const std::string& name, uint32_t value) override;
    void                       update(uint32_t result) override;
    void                       chocobo(uint32_t id, const std::array<uint16_t, 6>& look) override;
    [[nodiscard]] float        yaw(uint32_t id) const override;
    void                       animation(uint32_t id, uint8_t value) override;
    void                       fade(uint32_t id, int alpha, float ticks) override;
    void                       emote(uint32_t id, uint32_t target, int emote, int mode) override;
    [[nodiscard]] bool         emoteBusy(uint32_t id) const override;
    void                       environment(int hour, int weather) override;
    void                       restoreEnvironment() override;
    void                       volume(uint32_t mask, float level, float ticks) override;
    void                       subMap(uint16_t n) override;
    void                       positionUpdate(const dat::EventVec& pos, float yaw) override;
    void                       mapShow(int zone, int map) override;
    void                       mapMarker(int zone, int slot, float x, float z, const std::string& label) override;
    void                       mapClose() override;
    void                       eventTarget(const std::string& name, int gauge) override;
    void                       eventTimer(int which, int ticks) override;
    [[nodiscard]] int          eventTimerLeft(int which) const override;
    void                       mapView(float x, float y, float zoom, float seconds, int flags) override;
    [[nodiscard]] bool         mapOpen() const override
    {
        return mapOpen_;
    }

private:
    // Op 0x6A: a channel level moving to `to` by `step` a tick.
    struct Level
    {
        float now = 1.0f, to = 1.0f, step = 0.0f;
    };
    // Op 0x6C: an actor's model alpha moving from `from` to `to` over `total` ticks.
    struct Fade
    {
        float from = 1.0f, to = 1.0f, total = 0.0f, left = 0.0f;
    };
    std::optional<int>                        hour_, weather_;
    std::array<Level, 3>                      levels_{};
    std::unordered_map<uint32_t, Fade>        fades_;
    // Op 0x6E: emotes played per entity {last id, count, the server's count then}; never cleared (counters only grow).
    struct EmoteLog
    {
        uint8_t  id = 0;
        uint32_t count = 0, serverAt = 0;
    };
    std::unordered_map<uint32_t, EmoteLog>    emotes_;
    void                                      tickLevels(float ticks);
    struct Costume
    {
        Look  base;  // the server's look when the event changed it
        Look  look;
        float scale = 1.0f;
    };
    void                 finish();
    [[nodiscard]] Look   serverLook(uint32_t id) const;

    Session&                                  session_;
    EffectSystem&                             effects_;
    const dat::DatIndex&                      index_;
    Hooks                                     hooks_;
    bool                                      mapOpen_ = false; // ops 0xC8 .. 0x8A
    std::map<uint16_t, std::shared_ptr<const dat::EventFile>> files_; // by zone (EventNum)
    std::unique_ptr<dat::EventRunner>         runner_;
    std::unordered_map<uint32_t, Puppet>      puppets_;
    std::unordered_map<uint32_t, Costume>     costumes_;
    const WorldSnapshot*                      world_ = nullptr; // valid during frame()
    uint32_t                                  eventSeen_ = 0, updateSeen_ = 0, releaseSeen_ = 0, stringsSeen_ = 0;
    bool                                      cameraMode_ = false;
    bool                                      trace_      = false;
    bool                                      local_      = false; // startLocal's event: no server
    std::array<uint32_t, 8>                   localParams_{}; // what its updates get back
    // The zone's event file, loaded once.
    std::shared_ptr<const dat::EventFile> file(uint16_t zone);
};

} // namespace xi::client
