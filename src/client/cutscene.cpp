// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/cutscene.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace xi::client
{

Cutscene::Cutscene(Session& session, EffectSystem& effects, const dat::DatIndex& index, Hooks hooks)
    : session_(session)
    , effects_(effects)
    , index_(index)
    , hooks_(std::move(hooks))
    , trace_(std::getenv("XI_EVENT_TRACE") != nullptr)
{
}

void Cutscene::reset()
{
    mapClose();
    runner_.reset();
    puppets_.clear();
    costumes_.clear();
    fades_.clear();
    cameraMode_ = false;
    effects_.clearCamera();
    restoreEnvironment();
    volume(8, 1.0f, 0.0f);
}

void Cutscene::finish()
{
    if (!local_)
    {
        session_.finishEvent(runner_->result());
    }
    local_ = false;
    if (trace_)
    {
        spdlog::info("event: finished, result {:#x}", runner_->result());
    }
    std::string unknown;
    for (const auto& [op, n] : runner_->skipped())
    {
        unknown += fmt::format(" {:02x} x{}", op, n);
    }
    spdlog::info("event: unhandled ops:{}", unknown.empty() ? " none" : unknown);
    mapClose();
    runner_.reset();
    // Actors go back to what the server says (the player stays where the event put them: main keeps its feet).
    puppets_.clear();
    fades_.clear();
    cameraMode_ = false;
    effects_.clearCamera();
    restoreEnvironment();
    volume(8, 1.0f, 0.0f);
}

std::shared_ptr<const dat::EventFile> Cutscene::file(uint16_t zone)
{
    auto& f = files_[zone];
    if (!f)
    {
        try
        {
            f = std::make_shared<const dat::EventFile>(dat::EventFile::load(index_, zone));
        }
        catch (const std::exception& e)
        {
            spdlog::warn("event file for zone {}: {}", zone, e.what());
            f = std::make_shared<const dat::EventFile>();
        }
    }
    return f;
}

void Cutscene::startLocal(uint16_t zone, uint32_t owner, uint16_t event, const std::array<uint32_t, 8>& params,
                          const std::array<std::string, 4>& strings)
{
    runner_.reset();
    puppets_.clear();
    cameraMode_ = false;
    local_      = true;
    localParams_ = params;
    runner_     = std::make_unique<dat::EventRunner>(file(zone), owner, event, params, *this);
    runner_->setTrace(trace_);
    runner_->setSavedNames(strings);
    if (!runner_->valid())
    {
        spdlog::warn("event {} (zone {}, actor {:#x}): no script to run locally", event, zone, owner);
        runner_.reset();
        local_ = false;
        return;
    }
    spdlog::info("event {} (zone {}, actor {:#x}): running locally", event, zone, owner);
}

void Cutscene::frame(const WorldSnapshot& world, float dt)
{
    world_ = &world;
    // A costume ends when the server sends the actor a new look (or it leaves).
    std::erase_if(costumes_, [&](const auto& c) {
        const Look now = serverLook(c.first);
        return !now.valid || now.kind != c.second.base.kind || now.ids != c.second.base.ids;
    });
    if (world.eventSerial != eventSeen_)
    {
        eventSeen_   = world.eventSerial;
        updateSeen_  = world.eventUpdateSerial;
        releaseSeen_ = world.eventReleaseSerial;
        stringsSeen_ = world.eventStringsSerial;
        const auto& ev = world.event;
        // The player's own events (zone-in, cutscenes) run block 0x7FFFFFF0.
        const uint32_t owner = ev.uniqueNo == world.charId ? 0x7FFFFFF0u : ev.uniqueNo;
        runner_.reset();
        puppets_.clear();
        cameraMode_ = false;
        local_      = false;
        runner_     = std::make_unique<dat::EventRunner>(file(ev.eventNum), owner, ev.eventPara, ev.params, *this);
        runner_->setTrace(trace_);
        runner_->setSavedNames(ev.strings);
        if (!runner_->valid())
        {
            spdlog::info("event {} (zone {}, actor {:#x}): no script; finished", ev.eventPara, ev.eventNum, owner);
            session_.finishEvent(0);
            runner_.reset();
        }
        else
        {
            spdlog::info("event {} (zone {}, actor {:#x}): running its script", ev.eventPara, ev.eventNum, owner);
        }
    }
    if (!runner_)
    {
        return;
    }
    if (world.eventStringsSerial != stringsSeen_)
    {
        stringsSeen_ = world.eventStringsSerial; // S2C 0x05D: new strings (its update wait ends with 0x052)
        runner_->setSavedNames(world.event.strings);
    }
    if (world.eventUpdateSerial != updateSeen_)
    {
        updateSeen_ = world.eventUpdateSerial;
        runner_->updated(world.eventUpdate);
    }
    if (world.eventReleaseSerial != releaseSeen_)
    {
        // After the parameters (0x05C comes first when there are any): the update wait (op 0x43 01) ends.
        releaseSeen_ = world.eventReleaseSerial;
        runner_->released();
    }
    for (auto& [id, p] : puppets_)
    {
        p.walking = false;
    }
    const float ticks = std::min(dt, 0.25f) * 60.0f;
    runner_->tick(ticks);
    for (auto it = fades_.begin(); it != fades_.end();)
    {
        auto& f  = it->second;
        auto& pu = puppets_[it->first];
        f.left -= ticks;
        if (f.left > 0)
        {
            pu.alpha = f.to + (f.from - f.to) * (f.left / f.total);
            ++it;
            continue;
        }
        pu.alpha  = f.to;
        pu.hidden = f.to <= 0.0f;
        it        = fades_.erase(it);
    }
    if (std::any_of(levels_.begin(), levels_.end(), [](const Level& l) { return l.now != l.to; }))
    {
        tickLevels(ticks);
    }
    if (runner_ && runner_->finished())
    {
        finish();
    }
}

void Cutscene::closeMessage()
{
    if (runner_)
    {
        runner_->closeMessage();
    }
}

void Cutscene::choose(int option)
{
    if (runner_)
    {
        runner_->choose(option);
    }
}

uint32_t Cutscene::player() const
{
    return world_ ? world_->charId : 0;
}

bool Cutscene::exists(uint32_t id) const
{
    if (!world_)
    {
        return false;
    }
    if (id == world_->charId)
    {
        return true;
    }
    for (const auto& e : world_->entities)
    {
        if (e.id == id)
        {
            return !hooks_.loaded || hooks_.loaded(id);
        }
    }
    return false;
}

void Cutscene::request(uint32_t id)
{
    // C2S 0x016 CHARREQ {ActIndex, pad}: LSB answers with the entity, cutscene-only NPCs included.
    std::vector<std::byte> p(4);
    const uint16_t         index = static_cast<uint16_t>(id & 0xFFF);
    p[0]                         = static_cast<std::byte>(index & 0xFF);
    p[1]                         = static_cast<std::byte>(index >> 8);
    session_.send(0x016, std::move(p));
    if (trace_)
    {
        spdlog::info("event: request entity {:#x}", id);
    }
}

dat::EventVec Cutscene::position(uint32_t id) const
{
    if (const auto it = puppets_.find(id); it != puppets_.end() && it->second.pos)
    {
        return { it->second.pos->x, it->second.pos->y, it->second.pos->z };
    }
    if (world_ && id == world_->charId && hooks_.playerPos)
    {
        const auto p = hooks_.playerPos();
        return { p.x, p.y, p.z };
    }
    for (const auto& e : world_ ? world_->entities : std::vector<Entity>{})
    {
        if (e.id == id)
        {
            return { e.pos.x, e.pos.y, e.pos.z };
        }
    }
    return {};
}

void Cutscene::place(uint32_t id, const dat::EventVec& pos, std::optional<float> yaw, bool snapToGround)
{
    auto&     p = puppets_[id];
    glm::vec3 g(pos.x, pos.y, pos.z);
    if (snapToGround && hooks_.ground)
    {
        g.y = hooks_.ground(g);
    }
    p.pos = g;
    if (yaw)
    {
        p.yaw = *yaw;
    }
}

void Cutscene::face(uint32_t id, float yaw)
{
    puppets_[id].yaw = yaw;
}

void Cutscene::walking(uint32_t id, float speed)
{
    puppets_[id].walking = true;
    puppets_[id].speed   = speed;
}

void Cutscene::hide(uint32_t id, bool hidden)
{
    puppets_[id].hidden = hidden;
}

void Cutscene::motion(uint32_t id, uint32_t target, uint32_t fcc, int file, int raceSet)
{
    if (hooks_.motion)
    {
        hooks_.motion(id, target, fcc, file, raceSet);
    }
}

bool Cutscene::motionBusy(uint32_t id) const
{
    return hooks_.motionBusy && hooks_.motionBusy(id);
}

void Cutscene::idle(uint32_t id, uint32_t fcc)
{
    puppets_[id].idle = fcc;
}

void Cutscene::scheduler(uint32_t file, uint32_t caster, uint32_t target, uint32_t fcc)
{
    const bool ok = effects_.playFile(file, fcc, caster, target);
    if (trace_)
    {
        spdlog::info("event: scheduler {} from {}: {}", dat::fxNameString(fcc), file, ok);
    }
}

void Cutscene::stopScheduler(uint32_t file, uint32_t caster, uint32_t fcc)
{
    effects_.stopFile(file, fcc, caster);
}

bool Cutscene::schedulerRunning(uint32_t file, uint32_t caster, uint32_t fcc) const
{
    return effects_.runningFile(file, fcc, caster);
}

void Cutscene::cameraMode(int mode)
{
    cameraMode_ = mode != 0;
    if (!cameraMode_)
    {
        effects_.clearCamera();
    }
}

void Cutscene::music(int slot, uint16_t id)
{
    if (hooks_.music)
    {
        hooks_.music(slot, id);
    }
}

void Cutscene::sound(uint32_t id)
{
    if (hooks_.sound)
    {
        hooks_.sound(id);
    }
}

void Cutscene::message(uint32_t message, uint32_t speaker)
{
    if (hooks_.message)
    {
        hooks_.message(message, speaker);
    }
}

void Cutscene::nameRecord(std::size_t slot, const std::string& name, uint32_t value)
{
    session_.setEventName(slot, name, value);
}

void Cutscene::menu(uint32_t message, uint32_t hideMask, uint32_t defaultOption, uint32_t)
{
    if (hooks_.menu)
    {
        hooks_.menu(message, hideMask, defaultOption);
    }
}

void Cutscene::update(uint32_t result)
{
    if (local_)
    {
        runner_->updated(localParams_); // startLocal: answered at once with the event's parameters
        return;
    }
    session_.updateEvent(result);
}

void Cutscene::chocobo(uint32_t id, const std::array<uint16_t, 6>& look)
{
    const Look server = serverLook(id);
    if (!server.valid)
    {
        return;
    }
    auto [it, fresh] = costumes_.try_emplace(id);
    if (fresh)
    {
        it->second.base = server;
    }
    // As a ridden chocobo (app::Characters::mountLook): kind 7, {race, head, body, hands, legs, feet}.
    Look l;
    l.kind  = 7;
    l.valid = true;
    for (std::size_t k = 0; k < look.size(); ++k)
    {
        l.ids[k] = look[k];
    }
    it->second.look = l;
    if (trace_)
    {
        spdlog::info("event: chocobo {:x} race {} head {} body {} tail {} legs {} feet {}", id, look[0], look[1], look[2], look[3], look[4], look[5]);
    }
}

float Cutscene::yaw(uint32_t id) const
{
    if (const auto it = puppets_.find(id); it != puppets_.end() && it->second.yaw)
    {
        return *it->second.yaw;
    }
    uint8_t dir = world_ && id == world_->charId ? world_->serverDir : 0;
    for (const auto& e : world_ ? world_->entities : std::vector<Entity>{})
    {
        dir = e.id == id ? e.dir : dir;
    }
    return static_cast<float>(dir) * 3.14159265f / 128.0f;
}

void Cutscene::animation(uint32_t id, uint8_t value)
{
    puppets_[id].animation = value;
}

void Cutscene::fade(uint32_t id, int alpha, float ticks)
{
    // Op 0x6C (CODE 0x100b7560): model alpha to alpha / 0x80 over `ticks`, linearly (INFERRED: the curve).
    auto&       pu = puppets_[id];
    const float to = std::clamp(static_cast<float>(alpha) / 128.0f, 0.0f, 1.0f);
    const float at = pu.hidden ? 0.0f : pu.alpha;
    pu.hidden      = false;
    if (ticks <= 0.0f)
    {
        pu.alpha  = to;
        pu.hidden = to <= 0.0f;
        fades_.erase(id);
        return;
    }
    pu.alpha   = at;
    fades_[id] = { at, to, ticks, ticks };
}

void Cutscene::emote(uint32_t id, uint32_t, int emote, int)
{
    auto& e = emotes_[id];
    e.id    = static_cast<uint8_t>(emote);
    e.count += 1;
    e.serverAt = world_ && id == world_->charId ? world_->emotes : 0;
    for (const auto& w : world_ ? world_->entities : std::vector<Entity>{})
    {
        e.serverAt = w.id == id ? w.emotes : e.serverAt;
    }
    if (trace_)
    {
        spdlog::info("event: emote {:x} {}", id, emote);
    }
}

bool Cutscene::emoteBusy(uint32_t id) const
{
    return motionBusy(id);
}

void Cutscene::environment(int hour, int weather)
{
    if (hour >= 0)
    {
        hour_ = hour;
    }
    if (weather >= 0)
    {
        weather_ = weather;
    }
}

void Cutscene::restoreEnvironment()
{
    hour_.reset();
    weather_.reset();
}

void Cutscene::volume(uint32_t mask, float level, float ticks)
{
    // Bits 1 zone effect sounds, 2 sound effects, 4 ambience; 8 all three.
    for (std::size_t k = 0; k < levels_.size(); ++k)
    {
        if (mask & (8u | (1u << k)))
        {
            auto& l = levels_[k];
            l.to    = std::clamp(level, 0.0f, 1.0f);
            l.step  = ticks > 0 ? std::abs(l.to - l.now) / ticks : 0.0f;
            l.now   = ticks > 0 ? l.now : l.to;
        }
    }
    tickLevels(0.0f);
}

void Cutscene::tickLevels(float ticks)
{
    for (auto& l : levels_)
    {
        l.now = l.now < l.to ? std::min(l.to, l.now + l.step * ticks) : std::max(l.to, l.now - l.step * ticks);
    }
    if (hooks_.volume)
    {
        hooks_.volume(levels_[0].now, levels_[1].now, levels_[2].now);
    }
}

void Cutscene::subMap(uint16_t n)
{
    // Op 0x75 00 (CODE 0x10178810): a zone-format model, file n + 100 (n + 83191 from 600; dat::zoneModelFileId),
    // shown for the rest of the event in place of the zone (the client's draw: INFERRED); n <= 0 does nothing.
    if (trace_)
    {
        spdlog::info("event: sub-map {}", n);
    }
    if (n > 0 && hooks_.subMap)
    {
        hooks_.subMap(n);
    }
}

void Cutscene::positionUpdate(const dat::EventVec& pos, float yaw)
{
    const int dir = static_cast<int>(std::lround(yaw * 128.0f / 3.14159265f)) & 0xFF;
    if (local_)
    {
        runner_->updated(localParams_);
        return;
    }
    session_.updateEventPosition(runner_ ? runner_->result() : 0, { pos.x, pos.y, pos.z }, static_cast<uint8_t>(dir));
}

void Cutscene::mapShow(int zone, int map)
{
    // Op 0xC8 (CODE 0x100b92a0): the map window ("menu mapframe") with map `map` of `zone`.
    if (trace_)
    {
        spdlog::info("event: map {} / {}", zone, map);
    }
    mapOpen_ = true;
    if (hooks_.eventMap)
    {
        hooks_.eventMap(zone, map);
    }
}

void Cutscene::mapMarker(int zone, int slot, float x, float z, const std::string& label)
{
    // Op 0x8B (CODE 0x100b93c0): event marker `slot` (the map window's page 1, items 10..19: 0x101f8900 / 0x101f89e0
    // with page 1) named `label` at (x, z) on the map under the player of `zone` (0x100b24a0(4, zone, -1)).
    if (trace_)
    {
        spdlog::info("event: map {} marker {} ({:.1f}, {:.1f}) {}", zone, slot, x, z, label);
    }
    if (!mapOpen_)
    {
        mapShow(zone, -1);
    }
    if (hooks_.eventMapMarker)
    {
        hooks_.eventMapMarker(slot, x, z, label);
    }
}

void Cutscene::eventTarget(const std::string& name, int gauge)
{
    if (trace_)
    {
        spdlog::info("event: target window '{}' gauge {}", name, gauge);
    }
    if (hooks_.eventTarget)
    {
        hooks_.eventTarget(name, gauge);
    }
}

void Cutscene::eventTimer(int which, int ticks)
{
    if (trace_)
    {
        spdlog::info("event: {} timer {}", which ? "event" : "cast", ticks);
    }
    if (hooks_.eventTimer)
    {
        hooks_.eventTimer(which, ticks);
    }
}

int Cutscene::eventTimerLeft(int which) const
{
    return hooks_.eventTimerLeft ? hooks_.eventTimerLeft(which) : 0;
}

void Cutscene::mapView(float x, float y, float zoom, float seconds, int flags)
{
    if (trace_)
    {
        spdlog::info("event: map view ({:.3f}, {:.3f}) x{:.3f} over {:.3f} s, flags {:#x}", x, y, zoom, seconds, flags);
    }
    if (hooks_.mapView)
    {
        hooks_.mapView(x, y, zoom, seconds, flags);
    }
}

void Cutscene::mapClose()
{
    // Op 0x8A (CODE 0x100b92f0); the event's end closes it too.
    if (mapOpen_ && hooks_.eventMap)
    {
        hooks_.eventMap(0, -1);
    }
    mapOpen_ = false;
}

void Cutscene::apply(WorldSnapshot& world) const
{
    // Event emotes count on top of the server's (kept after the event: the renderer plays one per change).
    for (auto& e : world.entities)
    {
        if (const auto it = emotes_.find(e.id); it != emotes_.end())
        {
            e.emote = e.emotes > it->second.serverAt ? e.emote : it->second.id; // the newer one
            e.emotes += it->second.count;
        }
    }
    if (const auto it = emotes_.find(world.charId); it != emotes_.end())
    {
        world.emote = world.emotes > it->second.serverAt ? world.emote : it->second.id;
        world.emotes += it->second.count;
    }
    if (!active())
    {
        return;
    }
    for (auto& e : world.entities)
    {
        if (const auto it = puppets_.find(e.id); it != puppets_.end() && it->second.animation)
        {
            e.animation = *it->second.animation;
        }
    }
    if (weather_)
    {
        world.weather = static_cast<uint16_t>(*weather_);
    }
}

} // namespace xi::client

namespace xi::client
{

Look Cutscene::serverLook(uint32_t id) const
{
    if (!world_)
    {
        return {};
    }
    if (id == world_->charId)
    {
        return world_->look;
    }
    for (const auto& e : world_->entities)
    {
        if (e.id == id)
        {
            return e.look;
        }
    }
    return {};
}

Look Cutscene::look(uint32_t id, const Look& server) const
{
    const auto it = costumes_.find(id);
    return it == costumes_.end() ? server : it->second.look;
}

float Cutscene::scaleOf(uint32_t id) const
{
    const auto it = costumes_.find(id);
    return it == costumes_.end() ? 1.0f : it->second.scale;
}

void Cutscene::lookAt(uint32_t id, uint32_t target, int point)
{
    auto& p      = puppets_[id];
    p.lookMode   = 1;
    p.lookTarget = target;
    p.lookPoint  = point;
}

void Cutscene::lookAngles(uint32_t id, int pitch, int yaw)
{
    auto& p     = puppets_[id];
    p.lookMode  = 2;
    p.lookPitch = static_cast<float>(pitch) * 6.2831853f / 4096.0f;
    p.lookYaw   = static_cast<float>(yaw) * 6.2831853f / 4096.0f;
}

void Cutscene::lookStop(uint32_t id)
{
    puppets_[id].lookMode = 0;
}

void Cutscene::costume(uint32_t id, int slot, uint16_t value)
{
    const Look server = serverLook(id);
    if (!server.valid)
    {
        return;
    }
    auto [it, fresh] = costumes_.try_emplace(id);
    if (fresh)
    {
        it->second.base = server;
        it->second.look = server;
    }
    Look&      l  = it->second.look;
    const bool pc = l.kind == 0xFFFF || l.kind == 1;
    if (slot == 9)
    {
        l.kind = 0;
        l.ids  = {};
        l.ids[0] = value;
    }
    else if (slot == -1)
    {
        if (!pc)
        {
            l.kind = 1; // a single-model NPC dressed as a race
            l.ids  = {};
        }
        l.ids[0] = static_cast<uint16_t>((value & 0xFF) << 8 | (l.ids[0] & 0xFF));
    }
    else if (slot == 0)
    {
        l.ids[0] = pc ? static_cast<uint16_t>((l.ids[0] & 0xFF00) | (value & 0xFF)) : value;
    }
    else if (slot >= 1 && slot <= 8)
    {
        l.ids[static_cast<std::size_t>(slot)] = value;
    }
    if (trace_)
    {
        spdlog::info("event: costume {:x} slot {} = {:#x}", id, slot, value);
    }
}

void Cutscene::scale(uint32_t id, float scale)
{
    const Look server = serverLook(id);
    if (!server.valid)
    {
        return;
    }
    auto [it, fresh] = costumes_.try_emplace(id);
    if (fresh)
    {
        it->second.base = server;
        it->second.look = server;
    }
    it->second.scale = scale;
}

} // namespace xi::client
