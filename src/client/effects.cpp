// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/effects.h"
#include "client/effect_ops.h"

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xi::client
{

namespace
{
// Draw distance used by L0 op 0x0a when its record says 0 (the client's global setting; INFERRED value).
constexpr float kZoneDrawDistance = 200.0f;


constexpr float kTicksPerSecond = 60.0f;
constexpr float kPi             = 3.14159265f;
constexpr int   kMaxDepth       = 8;      // nested schedulers
constexpr std::size_t kMaxParticles = 4000; // per generator, a guard

template <typename T>
T rd(std::span<const std::byte> s, std::size_t at)
{
    T v{};
    if (at + sizeof(T) <= s.size())
    {
        std::memcpy(&v, s.data() + at, sizeof(T));
    }
    return v;
}

glm::vec3 rdVec(std::span<const std::byte> s, std::size_t at)
{
    return { rd<float>(s, at), rd<float>(s, at + 4), rd<float>(s, at + 8) };
}

float wrapPi(float a)
{
    while (a > kPi)
    {
        a -= 2 * kPi;
    }
    while (a < -kPi)
    {
        a += 2 * kPi;
    }
    return a;
}

// Facing rotation as the character models use it: +x forward, a = -dir * pi / 128.
glm::mat4 yawMatrix(float a)
{
    const float c = std::cos(a), s = std::sin(a);
    glm::mat4   r(1.0f);
    r[0] = glm::vec4(c, 0, s, 0);
    r[2] = glm::vec4(-s, 0, c, 0);
    return r;
}

glm::mat4 eulerMatrix(const glm::vec3& r)
{
    // INFERRED D3DX yaw (Y) - pitch (X) - roll (Z) order (§G.12).
    glm::mat4 m(1.0f);
    m = glm::rotate(m, r.y, glm::vec3(0, 1, 0));
    m = glm::rotate(m, r.x, glm::vec3(1, 0, 0));
    m = glm::rotate(m, r.z, glm::vec3(0, 0, 1));
    return m;
}

float asFloat(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
uint32_t asBits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// The first record of op `op` in the list starting at `at` (the game's 0x10049220 / 0x10049260 for L1 / L2).
std::optional<std::size_t> findRecord(std::span<const std::byte> p, std::size_t at, uint32_t op)
{
    for (int guard = 0; at != 0 && at + 4 <= p.size() && guard < 4096; ++guard)
    {
        const auto w = rd<uint32_t>(p, at);
        if ((w & 0xFF) == 0 || ((w >> 8) & 0x1F) == 0)
        {
            break;
        }
        if ((w & 0xFF) == op)
        {
            return at;
        }
        at += 4 * ((w >> 8) & 0x1F);
    }
    return std::nullopt;
}

// A float to a colour byte as the game stores it (CODE 0x1004e150): 0 at or below 0, else truncated, low byte kept.
uint8_t fxByte(float x)
{
    return x > 0.0f ? static_cast<uint8_t>(static_cast<int>(x) & 0xFF) : 0;
}

// A keyframe value in half turns as an angle wrapped to +-pi (CODE 0x10047ae0 / 0x1003de40: fmod(v, 2) x pi).
float halfTurns(float v)
{
    return wrapPi(std::fmod(v, 2.0f) * 3.1415920258f);
}

} // namespace

EffectSystem::EffectSystem(const dat::DatIndex& index)
: index_(index)
{
    file(0); // the common effect file (hit sparks, eis1, mdam ...)
}

const dat::EffectFile* EffectSystem::file(uint32_t id)
{
    auto it = files_.find(id);
    if (it == files_.end())
    {
        std::unique_ptr<dat::EffectFile> f;
        try
        {
            f = std::make_unique<dat::EffectFile>(index_, id);
        }
        catch (const std::exception& e)
        {
            spdlog::warn("effects: file {}: {}", id, e.what());
        }
        it = files_.emplace(id, std::move(f)).first;
    }
    return it->second.get();
}

std::pair<const dat::EffectFile*, const dat::FxResource*> EffectSystem::lookup(const dat::EffectFile* f, int block, dat::FxType type, dat::FxName name)
{
    if (f)
    {
        if (const auto* r = f->find(type, name, block))
        {
            return { f, r };
        }
    }
    const auto* common = file(0);
    if (common && common != f)
    {
        if (const auto* r = common->find(type, name))
        {
            return { common, r };
        }
    }
    return { nullptr, nullptr };
}

void EffectSystem::footstepEffect(uint32_t actor, uint8_t material, bool walking, bool rain)
{
    if (zoneFile_ && material <= 9)
    {
        const char name[5] = { walking ? 'w' : '0', static_cast<char>('0' + material), '0', '0', 0 };
        const auto set     = dat::fxName(rain ? "fser" : "fses");
        for (const auto& r : zoneFile_->resources())
        {
            if (r.type == dat::FxType::Generator && r.name == dat::fxName(name) && zoneFile_->underBlock(r.block, set))
            {
                startGenerator(zoneFile_, &r, actor, actor, 1.0f);
                break;
            }
        }
    }
    if (material == 3 || material == 4 || material == 8)
    {
        if (const auto [f, r] = lookup(nullptr, -1, dat::FxType::Generator, dat::fxName("fmrk")); r)
        {
            startGenerator(f, r, actor, actor, 1.0f);
        }
    }
}

void EffectSystem::wadeEffect(uint32_t actor, bool deep, bool rain)
{
    // CODE 0x100d6400 / 0x100d6480: while the floor is deep water (material 9), the zone set's `i900` runs once,
    // attached to the actor; off deep water it stops. Kept alive here a few ticks at a time, so it ends on its own
    // when the actor leaves the water or stops being drawn.
    if (!zoneFile_ || !deep)
    {
        return;
    }
    const auto set = dat::fxName(rain ? "fser" : "fses");
    for (const auto& r : zoneFile_->resources())
    {
        if (r.type != dat::FxType::Generator || r.name != dat::fxName("i900") || !zoneFile_->underBlock(r.block, set))
        {
            continue;
        }
        for (auto& g : generators_)
        {
            if (g->res == &r && g->caster == actor && !g->stopped && !g->child)
            {
                g->duration = g->elapsed + 10.0f;
                return;
            }
        }
        startGenerator(zoneFile_, &r, actor, actor, 10.0f, false);
        return;
    }
}

std::span<const std::byte> EffectSystem::footstepSep(dat::FxName name, bool rain)
{
    // CODE 0x100d4fa7..0x100d506e: the actor's set, the alternate name, then 0.DAT `foot` (se000012).
    if (zoneFile_)
    {
        const auto set   = dat::fxName(rain ? "fser" : "fses");
        const auto inSet = [&](dat::FxName n) -> const dat::FxResource* {
            for (const auto& r : zoneFile_->resources())
            {
                if (r.type == dat::FxType::Sep && r.name == n && zoneFile_->underBlock(r.block, set))
                {
                    return &r;
                }
            }
            return nullptr;
        };
        const auto* r = inSet(name);
        if (!r)
        {
            // Material in the name's second byte (as stored by dat::fxName: first char lowest).
            r = inSet((name & ~0xFF00u) | (static_cast<dat::FxName>('2') << 8));
        }
        if (r)
        {
            return r->payload;
        }
    }
    if (const auto* common = file(0))
    {
        if (const auto* r = common->find(dat::FxType::Sep, dat::fxName("foot")))
        {
            return r->payload;
        }
    }
    return {};
}

std::pair<const dat::EffectFile*, const dat::FxResource*> EffectSystem::lookupActor(uint32_t actor, dat::FxType type, dat::FxName name)
{
    if (actorFiles_)
    {
        for (const auto id : actorFiles_(actor))
        {
            if (const auto* f = file(id))
            {
                if (const auto* r = f->find(type, name))
                {
                    return { f, r };
                }
            }
        }
    }
    return { nullptr, nullptr };
}

glm::vec3 EffectSystem::pathPoint(const std::vector<glm::vec3>& v, float u)
{
    // An open chord-length Catmull-Rom (CODE 0x10063ab0 / 0x100634e0 / 0x100637c0): P[1..n] = the points, the ends
    // reflected; segments weighted by length for the u lookup; tangents scaled by neighbouring lengths (a, b).
    const std::size_t n = v.size();
    if (n < 2)
    {
        return n ? v[0] : glm::vec3(0.0f);
    }
    std::vector<glm::vec3> P(n + 2);
    for (std::size_t i = 0; i < n; ++i)
    {
        P[i + 1] = v[i];
    }
    P[0]     = 2.0f * P[1] - P[2];
    P[n + 1] = 2.0f * P[n] - P[n - 1];
    std::vector<float> len(n + 1);
    for (std::size_t i = 1; i + 1 <= n; ++i)
    {
        len[i] = std::max(glm::distance(P[i + 1], P[i]), 0.01f);
    }
    len[0] = len[1];
    len[n] = len[n - 1];
    float total = 0;
    for (std::size_t i = 1; i < n; ++i)
    {
        total += len[i];
    }
    // Segment k (0-based) runs P[k + 1] -> P[k + 2] with length len[k + 1].
    std::size_t k   = 0;
    float       acc = 0, s = 1.0f;
    for (; k + 1 < n; ++k)
    {
        const float l = len[k + 1] / total;
        if (u <= acc + l || k + 2 == n)
        {
            s = l > 0 ? std::clamp((u - acc) / l, 0.0f, 1.0f) : 1.0f;
            break;
        }
        acc += l;
    }
    const float a = len[k] / (len[k] + len[k + 1]), b = len[k + 1] / (len[k + 1] + len[k + 2]);
    const float ia = 1.0f - a;
    const float M[4][4] = {
        { -ia * ia / a, (ia + a * b) / a, (-ia - a * b) / (1.0f - b), b * b / (1.0f - b) },
        { 2.0f * ia * ia / a, (-2.0f * ia - a * b) / a, (2.0f * ia - b + 2.0f * a * b) / (1.0f - b), -b * b / (1.0f - b) },
        { -ia * ia / a, (1.0f - 2.0f * a) / a, a, 0.0f },
        { 0.0f, 1.0f, 0.0f, 0.0f },
    };
    const float pw[4] = { s * s * s, s * s, s, 1.0f };
    glm::vec3   out(0.0f);
    for (int j = 0; j < 4; ++j)
    {
        const float w = pw[0] * M[0][j] + pw[1] * M[1][j] + pw[2] * M[2][j] + pw[3] * M[3][j];
        out += w * P[k + static_cast<std::size_t>(j)];
    }
    return out;
}

std::optional<FxCameraPose> EffectSystem::camera() const
{
    if (!shot_)
    {
        return std::nullopt;
    }
    // One key: static. Two: linear by the keys' t. More: Catmull-Rom through the keys (CODE 0x100556b0; INFERRED kind).
    const auto& k = shot_->keys;
    const float u = shot_->duration > 0 ? shot_->elapsed / shot_->duration : 1.0f;
    auto        pose = [](const CameraKey& c) { return FxCameraPose{ c.eye, c.at, c.zoom, c.roll }; };
    if (k.size() == 1 || u <= k.front().t)
    {
        return pose(k.front());
    }
    if (u >= k.back().t)
    {
        return pose(k.back());
    }
    std::size_t i = 0;
    while (i + 2 < k.size() && u > k[i + 1].t)
    {
        ++i;
    }
    const float s = k[i + 1].t > k[i].t ? (u - k[i].t) / (k[i + 1].t - k[i].t) : 1.0f;
    if (k.size() == 2)
    {
        return FxCameraPose{ glm::mix(k[0].eye, k[1].eye, s), glm::mix(k[0].at, k[1].at, s), glm::mix(k[0].zoom, k[1].zoom, s), glm::mix(k[0].roll, k[1].roll, s) };
    }
    auto cr = [&](auto get) {
        const auto p0 = get(k[i > 0 ? i - 1 : i]), p1 = get(k[i]), p2 = get(k[i + 1]), p3 = get(k[std::min(i + 2, k.size() - 1)]);
        const float s2 = s * s, s3 = s2 * s;
        return 0.5f * ((2.0f * p1) + (-p0 + p2) * s + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * s2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * s3);
    };
    return FxCameraPose{ cr([](const CameraKey& c) { return c.eye; }), cr([](const CameraKey& c) { return c.at; }), cr([](const CameraKey& c) { return c.zoom; }),
                         cr([](const CameraKey& c) { return c.roll; }) };
}

std::span<const std::byte> EffectSystem::ambienceSep(bool indoor) const
{
    const auto& list = indoor && !ambienceIndoor_.empty() ? ambienceIndoor_ : ambience_;
    if (list.empty())
    {
        return {};
    }
    const float                now  = dayFraction_ * 5184000.0f; // 1/60 Vana'diel seconds of the day
    const dat::FxResource*     best = nullptr;
    float                      bestStart = -1;
    for (const auto& [start, r] : list)
    {
        if (start <= now && start >= bestStart)
        {
            best      = r;
            bestStart = start;
        }
    }
    return (best ? best : list.back().second)->payload; // none started yet today: the last in file order
}

glm::vec4 EffectSystem::screenColour() const
{
    const float u = screen_.duration > 0 ? screen_.elapsed / screen_.duration : 1.0f;
    return glm::mix(screen_.from, screen_.to, u);
}

bool EffectSystem::layerActive(int op) const
{
    return std::ranges::any_of(layers_, [&](const Layer& l) { return l.op == op; });
}

std::vector<EffectSystem::ScreenLayer> EffectSystem::screenLayers() const
{
    std::vector<ScreenLayer> out;
    for (const auto& l : layers_)
    {
        const float u = l.duration > 0 ? std::clamp(l.elapsed / l.duration, 0.0f, 1.0f) : 1.0f;
        ScreenLayer s;
        if (l.op == 0x10)
        {
            // Freeze frame (CODE 0x10012f00): the captured image, alpha = remaining / duration, normal blending.
            s.frozen = true;
            s.colour = glm::vec4(1, 1, 1, 1.0f - u);
        }
        else
        {
            continue; // 0x0E draws from its persistent state below
        }
        out.push_back(s);
    }
    if (const auto& sc = screenColour_; sc.colour.a > 0.002f)
    {
        // 0x0E (CODE 0x10013806..0x1001385d): this frame's image drawn over the scene, source rect u and v from 1 - z to z
        // (z just below 1: a slight zoom, the `blon` glow), colour rgb, alpha byte x 2 (max 255), normal blending.
        ScreenLayer s;
        s.frozen = false;
        s.uvRect = glm::vec4(1.0f - sc.z, 1.0f - sc.z, sc.z, sc.z);
        s.colour = glm::vec4(glm::vec3(sc.colour), std::min(1.0f, sc.colour.a * 2.0f));
        out.push_back(s);
    }
    screenLayers7b(out);
    const glm::vec3 flash = glm::min(flash_ + glare_, glm::vec3(1.0f));
    glare_                = glm::vec3(0.0f);
    if (flash.r + flash.g + flash.b > 0.002f)
    {
        ScreenLayer f;
        f.image    = false;
        f.additive = true;
        f.colour   = glm::vec4(flash, 1.0f);
        out.push_back(f);
    }
    return out;
}

bool EffectSystem::playFile(uint32_t fileId, dat::FxName name, uint32_t caster, uint32_t target)
{
    const auto* f = file(fileId);
    const auto* r = f ? f->find(dat::FxType::Scheduler, name) : nullptr;
    if (!r)
    {
        return false;
    }
    startScheduler(f, r, caster, target, 0);
    return true;
}

void EffectSystem::stopFile(uint32_t fileId, dat::FxName name, uint32_t caster)
{
    const auto* f = file(fileId);
    for (auto* list : { &schedulers_, &started_ })
    {
        std::erase_if(*list, [&](const Scheduler& s) { return s.file == f && s.res && s.res->name == name && (caster == 0 || s.caster == caster); });
    }
}

bool EffectSystem::runningFile(uint32_t fileId, dat::FxName name, uint32_t caster) const
{
    const auto it = files_.find(fileId);
    if (it == files_.end())
    {
        return false;
    }
    for (const auto* list : { &schedulers_, &started_ })
    {
        for (const auto& s : *list)
        {
            if (s.file == it->second.get() && s.res && s.res->name == name && !s.dead && (caster == 0 || s.caster == caster))
            {
                return true;
            }
        }
    }
    return false;
}

bool EffectSystem::playDoor(uint32_t door, dat::FxName name, uint32_t actor)
{
    if (!zoneFile_)
    {
        return false;
    }
    for (const auto& r : zoneFile_->resources())
    {
        if (r.type == dat::FxType::Scheduler && r.name == name && zoneFile_->underBlock(r.block, door))
        {
            startScheduler(zoneFile_, &r, actor, actor, 0);
            return true;
        }
    }
    return false;
}

bool EffectSystem::playFromActor(uint32_t actor, dat::FxName name, uint32_t caster, uint32_t target, const FxAction* action)
{
    action_ = action ? std::make_shared<const FxAction>(*action) : nullptr;
    result_ = 0;
    auto [f, r] = lookupActor(actor, dat::FxType::Scheduler, name);
    if (!r)
    {
        std::tie(f, r) = lookup(nullptr, -1, dat::FxType::Scheduler, name);
    }
    if (!r)
    {
        return false;
    }
    startScheduler(f, r, caster, target, 0);
    return true;
}

void EffectSystem::setWeather(std::string_view fourcc)
{
    if (!zoneFile_ || fourcc == weather_)
    {
        return;
    }
    weather_ = std::string(fourcc);
    std::erase_if(generators_, [](const std::unique_ptr<Generator>& g) { return g->weather; });
    flares_.clear();
    ambience_.clear();
    ambienceIndoor_.clear();
    const auto weat = dat::fxName("weat");
    auto       has  = [&](dat::FxName w) {
        return std::any_of(zoneFile_->resources().begin(), zoneFile_->resources().end(),
                           [&](const dat::FxResource& r) { return zoneFile_->underBlock(r.block, weat) && zoneFile_->underBlock(r.block, w); });
    };
    dat::FxName w = dat::fxName(weather_);
    if (!has(w))
    {
        w = dat::fxName("suny");
    }
    // Hour loops: HHMM Seps of this weather, not under `indo` (ambience_re.md).
    for (const auto& r : zoneFile_->resources())
    {
        if (r.type != dat::FxType::Sep || !zoneFile_->underBlock(r.block, weat) || !zoneFile_->underBlock(r.block, w))
        {
            continue;
        }
        auto& list = zoneFile_->underBlock(r.block, dat::fxName("indo")) ? ambienceIndoor_ : ambience_;
        const std::string n = dat::fxNameString(r.name);
        if (n.size() == 4 && std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; }) && (n[0] == '0' || n[0] == '1') && n[2] <= '5')
        {
            const float start = static_cast<float>((n[0] - '0') * 10 + (n[1] - '0')) * 216000.0f + static_cast<float>((n[2] - '0') * 10 + (n[3] - '0')) * 3600.0f;
            list.emplace_back(start, &r);
        }
    }
    startingZone_ = true;
    for (const auto& r : zoneFile_->resources())
    {
        if (r.type != dat::FxType::Generator || r.payload.size() < 0x80 || !zoneFile_->underBlock(r.block, weat) || !zoneFile_->underBlock(r.block, w))
        {
            continue;
        }
        if (!(rd<uint32_t>(r.payload, 0x68) & 0x1000))
        {
            continue;
        }
        // Lens flares (resource type 0x39) have their own screen-space renderer (CODE 0x10054800): kept for flares().
        if (const auto h = rd<uint32_t>(r.payload, 0x74); h >= 0x10 && h - 0x10 + 0x22 <= r.payload.size() && rd<uint8_t>(r.payload, h - 0x10 + 0x21) == 0x39)
        {
            const auto [mf, mr] = lookup(zoneFile_, r.block, dat::FxType::D3a, rd<uint32_t>(r.payload, h - 0x10 + 0x0C));
            const auto origin   = rd<uint32_t>(r.payload, 0x00);
            if (mr)
            {
                Flare fl{ dat::FxFlare::parse(mr->payload), nullptr, (((origin >> 16) & 1) << 4 | (origin & 0xF)) == 0xf };
                fl.texture = mf->texture(fl.flare.texName);
                if (!fl.flare.elements.empty())
                {
                    flares_.push_back(std::move(fl));
                }
            }
            continue;
        }
        const auto before = generators_.size();
        startGenerator(zoneFile_, &r, 0, 0, 0.0f);
        if (generators_.size() > before)
        {
            auto& g   = *generators_.back();
            g.weather = true;
            g.follow  = true; // the sun and moon move with the camera and the clock
        }
    }
    startingZone_ = false;
    spdlog::debug("fx: weather {} ({}): {} lens flares", weather_, dat::fxNameString(w), flares_.size());
    for (const auto& fl : flares_)
    {
        spdlog::debug("fx: flare {} {} elements, texture {}", fl.moon ? "moon" : "sun", fl.flare.elements.size(), fl.texture ? "found" : "missing");
    }
}

std::size_t EffectSystem::startZone(uint32_t fileId, std::function<const dat::FxMesh*(dat::FxName chunk)> zoneMesh)
{
    zoneMesh_ = std::move(zoneMesh);
    if (zoneFileId_ != 0 && zoneFileId_ != fileId)
    {
        // The previous zone's file (tens of MB): nothing may still point into it.
        const auto* old = files_.contains(zoneFileId_) ? files_[zoneFileId_].get() : nullptr;
        std::erase_if(generators_, [&](const std::unique_ptr<Generator>& g) { return g->file == old || g->meshFile == old; });
        std::erase_if(schedulers_, [&](const Scheduler& s) { return s.file == old; });
        std::erase_if(started_, [&](const Scheduler& s) { return s.file == old; });
        files_.erase(zoneFileId_);
    }
    zoneFileId_ = fileId;
    zoneFile_   = file(fileId);
    if (!zoneFile_)
    {
        return 0;
    }
    std::size_t started = 0;
    startingZone_       = true;
    for (const auto& r : zoneFile_->resources())
    {
        if (r.type != dat::FxType::Generator || r.payload.size() < 0x80)
        {
            continue;
        }
        const auto     origin = rd<uint32_t>(r.payload, 0x00);
        const auto     flags  = rd<uint32_t>(r.payload, 0x68);
        const uint32_t mode   = (((origin >> 16) & 1) << 4) | (origin & 0xF);
        if (!(flags & 0x1000) || zoneFile_->underBlock(r.block, dat::fxName("weat")) || !(mode == 0 || mode == 0xe || mode == 0xf || mode == 0x14))
        {
            continue;
        }
        startGenerator(zoneFile_, &r, 0, 0, 0.0f);
        ++started;
    }
    // Schedulers that start themselves (CODE 0x10056980: ones with op 0x52 and no op 0xa2; also op 0x01, not done):
    // the seagulls `kamo` s001..s003, which wait for their clock window.
    for (const auto& r : zoneFile_->resources())
    {
        if (r.type != dat::FxType::Scheduler || r.payload.size() < 0x20 || zoneFile_->underBlock(r.block, dat::fxName("weat")))
        {
            continue;
        }
        const auto  start = rd<uint32_t>(r.payload, 0x14);
        bool        clock = false, stop = false;
        for (std::size_t at = start >= 0x10 ? start - 0x10 : 0x40; at + 4 <= r.payload.size();)
        {
            const auto w = rd<uint32_t>(r.payload, at);
            const auto op = w & 0xFF, len = (w >> 8) & 0x1F;
            if (op == 0 || len == 0)
            {
                break;
            }
            clock |= op == 0x52;
            stop |= op == 0xa2;
            at += 4 * len;
        }
        if (clock && !stop)
        {
            startScheduler(zoneFile_, &r, 0, 0, 0);
            ++started;
        }
    }
    startingZone_ = false;
    return started;
}

bool EffectSystem::play(uint32_t fileId, std::string_view name, uint32_t caster, uint32_t target, float delaySeconds, const FxAction* action)
{
    action_ = action ? std::make_shared<const FxAction>(*action) : nullptr;
    result_ = 0;
    const auto* f = file(fileId);
    if (!f)
    {
        return false;
    }
    const auto [ff, r] = lookup(f, -1, dat::FxType::Scheduler, dat::fxName(name));
    if (!r)
    {
        return false;
    }
    startScheduler(ff, r, caster, target, 0, delaySeconds * kTicksPerSecond);
    return true;
}

bool EffectSystem::playNumber(uint32_t fileId, std::string_view name, uint32_t caster, uint32_t target, int32_t value, uint32_t colour)
{
    const auto before = started_.size();
    if (!play(fileId, name, caster, target))
    {
        return false;
    }
    if (started_.size() > before)
    {
        started_.back().number       = name == "misv" ? std::numeric_limits<int32_t>::min() : value;
        started_.back().numberColour = colour & 0xFFFFFF;
    }
    return true;
}

std::vector<EffectSystem::TextParticle> EffectSystem::textParticles() const
{
    std::vector<TextParticle> out;
    for (const auto& gp : generators_)
    {
        const auto& g = *gp;
        if (!g.text)
        {
            continue;
        }
        for (const auto& q : g.particles)
        {
            if (q.rgba[3] == 0)
            {
                continue;
            }
            TextParticle t;
            t.pos  = glm::vec3(g.matrix * glm::vec4(q.pos, 1.0f));
            t.text = g.number == std::numeric_limits<int32_t>::min() ? std::string(":") : std::to_string(g.number);
            t.rgba = q.rgba;
            // The scheduler's colour applies while the particle's own is the neutral 0x808080 (CODE 0x10041e70).
            if (q.rgba[0] == 0x80 && q.rgba[1] == 0x80 && q.rgba[2] == 0x80)
            {
                t.rgba = { static_cast<uint8_t>(g.numberColour >> 16), static_cast<uint8_t>(g.numberColour >> 8), static_cast<uint8_t>(g.numberColour), q.rgba[3] };
            }
            t.scale    = q.scale.x;
            t.scaleY   = q.scale.y;
            t.subtract = fxBlendClass(q.blend) == Batch::Blend::Subtract;
            t.additive = fxBlendClass(q.blend) == Batch::Blend::Additive;
            out.push_back(std::move(t));
        }
    }
    return out;
}

void EffectSystem::startScheduler(const dat::EffectFile* f, const dat::FxResource* r, uint32_t caster, uint32_t target, int depth, float waitTicks, float rate)
{
    if (std::getenv("XI_FX_TRACE"))
    {
        spdlog::info("fx: sched {} (file {}) caster {:x} target {:x}", dat::fxNameString(r->name), f->fileId(), caster, target);
    }
    if (depth > kMaxDepth || schedulers_.size() + started_.size() > 256)
    {
        return;
    }
    Scheduler s;
    s.file   = f;
    s.res    = r;
    s.caster = caster;
    s.target = target;
    s.depth  = depth;
    s.wait   = waitTicks;
    s.rate   = rate;
    s.action = action_;
    s.result = result_;
    const auto start = rd<uint32_t>(r->payload, 0x14);
    s.pc             = start >= 0x10 ? start - 0x10 : 0x40; // §S.1: records start at payload + (u32 +0x14) - 0x10
    started_.push_back(s);
}

EffectSystem::Generator* EffectSystem::startGenerator(const dat::EffectFile* f, const dat::FxResource* r, uint32_t caster, uint32_t target, float duration, bool reuse)
{
    // Re-trigger an instance already running for the same caster/target (§S.3 op 2).
    for (auto& g : generators_)
    {
        if (reuse && g->res == r && g->caster == caster && g->target == target && !g->stopped && !g->child)
        {
            g->duration = duration;
            g->elapsed  = 0;
            g->spawned  = 0;
            return g.get();
        }
    }
    // A cap on effect generators; the zone's own ambient ones do not count (West Ronfaure alone runs 519).
    if (!startingZone_ && std::count_if(generators_.begin(), generators_.end(), [](const std::unique_ptr<Generator>& g) { return !g->zone; }) > 512)
    {
        return nullptr;
    }
    auto g    = std::make_unique<Generator>();
    g->file   = f;
    g->res    = r;
    g->caster = caster;
    g->target = target;
    // Chunk offsets in docs/effect-format.md §G.2 count the 16-byte header; payload = chunk - 0x10.
    const auto p   = r->payload;
    g->origin      = rd<uint32_t>(p, 0x00);
    g->intervalRand = rd<int16_t>(p, 0x64);
    g->interval    = rd<int16_t>(p, 0x66);
    g->flags       = rd<uint32_t>(p, 0x68);
    for (std::size_t k = 0; k < 4; ++k)
    {
        const auto off = rd<uint32_t>(p, 0x70 + 4 * k);
        g->lists[k]    = off >= 0x10 ? off - 0x10 : 0;
    }
    g->duration = duration;
    g->zone     = startingZone_ || (f == zoneFile_ && caster == 0 && target == 0); // also what zone schedulers start (the gulls)
    // L1's first record is the particle header: resource type and name, life (§G.8).
    const std::size_t h       = g->lists[1];
    const auto        resType = rd<int8_t>(p, h + 0x21);
    const auto        resName = rd<uint32_t>(p, h + 0x0C);
    const auto        life    = rd<int16_t>(p, h + 0x22);
    if (std::getenv("XI_FX_TRACE"))
    {
        spdlog::info("fx: gen {} (file {}) origin {:x} flags {:x} pflags {:x} res {:x} {} life {}", dat::fxNameString(r->name), f->fileId(), g->origin, g->flags,
                     rd<uint32_t>(p, h + 4), static_cast<uint8_t>(resType), dat::fxNameString(resName), life);
    }
    g->singleShot             = (g->flags & 0x400) || life == 0;
    g->resType                = static_cast<uint8_t>(resType);
    if (resType == 0x0b || resType == 0x0c || resType == 0x1f)
    {
        const auto [mf, mr] = lookup(f, r->block, dat::FxType::D3m, resName);
        g->mesh             = mr ? mf->mesh(*mr) : nullptr;
        g->meshFile         = mf;
        if (!g->mesh && f == zoneFile_ && zoneMesh_)
        {
            g->mesh     = zoneMesh_(resName); // no D3m: the zone's Mmb of that chunk name (the sea, windmills ...)
            g->meshFile = f;                  // its textures are the zone file's
        }
    }
    else if (resType == 0x47)
    {
        g->light = true; // a point light (CODE ctor 0x10056220)
    }
    else if (resType == 0x5c)
    {
        g->text = true; // a damage pop-up's number (CODE draw 0x10041e70)
    }
    else if (resType == 0x3d)
    {
        // Sound particles (zone ambience: birds, wind, water); the Sep from the generator's file up, then global.
        g->sound = lookup(f, r->block, dat::FxType::Sep, resName).second;
    }
    else if (resType == 0x1d)
    {
        // A D3b morphing mesh (§M.4; CODE particle class 0x1003faa0): the seagull, Protect's shell, wave fans.
        const auto [mf, mr] = lookup(f, r->block, dat::FxType::D3b, resName);
        g->mesh             = mr ? mf->mesh(*mr) : nullptr;
        g->meshFile         = mf;
    }
    else if (resType == 0x0e || resType == 0x21 || resType == 0x39)
    {
        const auto [mf, mr] = lookup(f, r->block, dat::FxType::D3a, resName);
        g->mesh             = mr ? mf->mesh(*mr) : nullptr;
        g->meshFile         = mf;
    }
    // Coverage: every op of the four lists runs for this generator or its particles (counted once per start).
    for (std::size_t k = 0; k < 4; ++k)
    {
        for (std::size_t at = g->lists[k]; at + 4 <= p.size();)
        {
            const auto w = rd<uint32_t>(p, at);
            if ((w & 0xFF) == 0 || ((w >> 8) & 0x1F) == 0)
            {
                break;
            }
            noteOp(static_cast<uint8_t>(k + 1), static_cast<uint8_t>(w & 0xFF), f, r->name);
            at += 4 * ((w >> 8) & 0x1F);
        }
    }
    // L0 op 0x11: follow the attach point every frame (§G.11).
    for (std::size_t at = g->lists[0]; at + 4 <= p.size();)
    {
        const auto w   = rd<uint32_t>(p, at);
        const auto op  = w & 0xFF;
        const auto len = (w >> 8) & 0x1F;
        if (op == 0 || len == 0)
        {
            break;
        }
        if (op == 0x11)
        {
            g->follow = true;
        }
        at += 4 * len;
    }
    generators_.push_back(std::move(g));
    return generators_.back().get();
}

const dat::FxCurve* EffectSystem::curve(Generator& g, std::size_t recordOffset, dat::FxName name)
{
    if (auto it = g.curves.find(static_cast<uint32_t>(recordOffset)); it != g.curves.end())
    {
        return it->second.keys.empty() ? nullptr : &it->second;
    }
    const auto [f, r] = lookup(g.file, g.res->block, dat::FxType::KeyFrame, name);
    auto& c           = g.curves[static_cast<uint32_t>(recordOffset)];
    if (r)
    {
        c = dat::FxCurve::parse(r->payload);
    }
    return c.keys.empty() ? nullptr : &c;
}

bool EffectSystem::stepScheduler(Scheduler& s, float ticks)
{
    // §S.1 timing: wait -= dt; while wait <= 0, run the record and add its delay (post-delay).
    const auto p = s.res->payload;
    action_      = s.action; // sub-schedulers share the action and start on the current result
    result_      = s.result;
    s.wait -= ticks;
    int guard = 0;
    while (s.wait <= 0 && guard++ < 256)
    {
        if (s.pc + 8 > p.size())
        {
            return false;
        }
        const auto w     = rd<uint32_t>(p, s.pc);
        const auto op    = w & 0xFF;
        const auto len   = (w >> 8) & 0x1F;
        const auto delay = rd<int16_t>(p, s.pc + 4);
        const auto dur   = rd<int16_t>(p, s.pc + 6);
        if (op == 0 || len == 0)
        {
            return false;
        }
        const auto name = rd<uint32_t>(p, s.pc + 8);
        noteOp(static_cast<uint8_t>(FxList::Scheduler), static_cast<uint8_t>(op), s.file, s.res->name);
        switch (op)
        {
            case 0x02: // start generator
                if (s.action && hideBattleEffect && hideBattleEffect(s.caster, s.target))
                {
                    break;
                }
                if (const auto [f, r] = lookup(s.file, s.res->block, dat::FxType::Generator, name); r)
                {
                    // §G.2: the record's duration, +1 when it is 0 (so a 0 is a one-tick burst).
                    const auto before = generators_.size();
                    startGenerator(f, r, s.caster, s.target, static_cast<float>(dur == 0 ? 1 : dur) * s.rate);
                    if (s.number && generators_.size() > before)
                    {
                        generators_.back()->number       = *s.number;
                        generators_.back()->numberColour = s.numberColour;
                    }
                }
                break;
            case 0x1E: // kill a generator
            case 0x2D: // stop emitting
                // By name and caster, whatever the target (§C.3): the cast glow started by `ca..` (caster = target =
                // the caster) is stopped by `st..`, which a spell's `sh..` runs toward the spell's target (#113).
                for (auto& g : generators_)
                {
                    if (g->res && g->res->name == name && g->caster == s.caster)
                    {
                        g->stopped = true;
                        if (op == 0x1E)
                        {
                            g->particles.clear();
                        }
                    }
                }
                break;
            case 0x03: // sub-scheduler, same roles
            case 0x73: // the same, at rate 0.7 when the action has more than one target (CODE 0x10056ba0)
                if (const auto [f, r] = lookup(s.file, s.res->block, dat::FxType::Scheduler, name); r)
                {
                    bool several = false;
                    if (op == 0x73 && s.action)
                    {
                        for (const auto& res : s.action->results)
                        {
                            several = several || res.target != s.action->results.front().target;
                        }
                    }
                    startScheduler(f, r, s.caster, s.target, s.depth + 1, 0.0f, several ? 0.7f : 1.0f);
                }
                break;
            case 0x30: // once per action target, on that target's first result
                if (const auto [f, r] = lookup(s.file, s.res->block, dat::FxType::Scheduler, name); r)
                {
                    if (!s.action || s.action->results.empty())
                    {
                        startScheduler(f, r, s.caster, s.target, s.depth + 1);
                        break;
                    }
                    for (std::size_t i = 0; i < s.action->results.size(); ++i)
                    {
                        const auto t = s.action->results[i].target;
                        if (i == 0 || s.action->results[i - 1].target != t)
                        {
                            result_ = i;
                            startScheduler(f, r, s.caster, t, s.depth + 1);
                        }
                    }
                    result_ = s.result;
                }
                break;
            case 0x3D: // random branch (§S.3): run one random record up to the next 0x3E, then continue after it
            case 0xAF:
            {
                std::vector<std::size_t> picks;
                std::size_t              at = s.pc + 4 * len;
                while (at + 8 <= p.size())
                {
                    const auto w2 = rd<uint32_t>(p, at);
                    const auto l2 = (w2 >> 8) & 0x1F;
                    if ((w2 & 0xFF) == 0 || l2 == 0)
                    {
                        break;
                    }
                    if ((w2 & 0xFF) == 0x3E)
                    {
                        s.after = at + 4 * l2;
                        break;
                    }
                    picks.push_back(at);
                    at += 4 * l2;
                }
                if (picks.empty() || s.after == SIZE_MAX)
                {
                    s.after = SIZE_MAX;
                    break;
                }
                s.wait += static_cast<float>(delay) * s.rate;
                s.pc = picks[rng_() % picks.size()];
                continue;
            }
            case 0x24: // the attacker's swing for the current result (CODE 0x100d6780 / 0x100d6870; §Melee)
            case 0x5D:
            {
                const auto* v    = s.action && s.result < s.action->results.size() ? &s.action->results[s.result].vars : nullptr;
                const int   kind = v ? (*v)[0x29] : 0, anim = v ? (*v)[0x2A] : 0;
                std::string stem = "ati";
                if (kind == 1 && anim >= 1 && anim <= 3)
                {
                    stem[0] = static_cast<char>('b' + anim - 1); // off-hand / kicks (INFERRED)
                }
                std::vector<std::pair<const dat::EffectFile*, const dat::FxResource*>> found;
                if (kind == 1 && anim == 4)
                {
                    if (auto fr = lookupActor(s.caster, dat::FxType::Scheduler, dat::fxName("ls05")); fr.second)
                    {
                        found.push_back(fr);
                    }
                }
                // Engaged actors use the directional swings (`atf?` front) when they have them, else `ati?`. An
                // off-hand / kick stem the actor's files lack falls back to the main swings (a Tarutaru's battle file
                // has only `at??`; INFERRED, so the swing and its contact sounds still play).
                for (const char first : { stem[0], 'a' })
                {
                    if (!found.empty())
                    {
                        break;
                    }
                    stem[0] = first;
                    for (const char dirc : { 'f', 'i' })
                    {
                        if (!found.empty())
                        {
                            break; // ls05
                        }
                        stem[2] = dirc;
                        for (char d = '0'; d <= '9'; ++d)
                        {
                            if (auto fr = lookupActor(s.caster, dat::FxType::Scheduler, dat::fxName(stem + d)); fr.second)
                            {
                                found.push_back(fr);
                            }
                        }
                        if (!found.empty())
                        {
                            break;
                        }
                    }
                }
                if (!found.empty())
                {
                    const auto [f, r] = found[rng_() % found.size()]; // a random existing variant
                    startScheduler(f, r, s.caster, s.target, s.depth + 1);
                }
                if (std::getenv("XI_FX_TRACE"))
                {
                    spdlog::info("fx: {} swing for result {}: {} variants", dat::fxNameString(s.res->name), s.result, found.size());
                }
                break;
            }
            case 0x31: // label for 0x32
                s.loop = s.pc;
                break;
            case 0x32: // next result (CODE 0x1005d80d): loop back past the 0x31 label with its delay, or fall through
                       // with no delay after the last one
                if (s.action && s.result + 1 < s.action->results.size() && s.loop != SIZE_MAX)
                {
                    s.result += 1;
                    s.target = s.action->results[s.result].target;
                    result_  = s.result;
                    s.pc     = s.loop;
                    s.wait += static_cast<float>(rd<int16_t>(p, s.pc + 4)) * s.rate;
                    s.pc += 4 * ((rd<uint32_t>(p, s.pc) >> 8) & 0x1F);
                }
                else
                {
                    s.pc += 4 * len;
                }
                continue;
            case 0x57: // from the caster's files, then own / global (§S.3)
            case 0x85:
            case 0x3C: // the caster's `sh..` release (spell finish, §C.3)
            {
                auto [f, r] = lookupActor(s.caster, dat::FxType::Scheduler, name);
                if (!r)
                {
                    std::tie(f, r) = lookup(s.file, s.res->block, dat::FxType::Scheduler, name);
                }
                // `ef h` (`efhl` / `efhr` by hand) is the weapon model's hit spark (`chit` / `chih` ask for it; the spark, file 0 `hit1` for every weapon,
                // DATA); `sehl` / `sehr` its `se h` impact sound (INFERRED mapping, effect-format "Open items"). The weapon
                // model file is among the actor's files; without one the spark falls back to file 0 `hit1`.
                if (!r && (name == dat::fxName("efhl") || name == dat::fxName("efhr") || name == dat::fxName("ef h")))
                {
                    std::tie(f, r) = lookupActor(s.caster, dat::FxType::Scheduler, dat::fxName("ef h"));
                    if (!r)
                    {
                        std::tie(f, r) = lookup(nullptr, -1, dat::FxType::Scheduler, dat::fxName("hit1"));
                    }
                }
                if (r)
                {
                    startScheduler(f, r, s.caster, s.target, s.depth + 1);
                }
                break;
            }
            case 0x76: // `lcNN` / `lsNN` by the caster's ranged class (§C.5 type 12 / type 2); +8 = mode
            case 0x77:
            {
                int            n    = rangedClass_ ? rangedClass_(s.caster) : -1;
                const uint32_t mode = rd<uint32_t>(p, s.pc + 8);
                if (mode == 1 && n > 2)
                {
                    n = 0; // songs
                }
                else if (mode == 2 && n != 11)
                {
                    n = 10; // geomancy
                }
                if (n >= 0 && n < 100)
                {
                    const char      pre[2] = { 'l', op == 0x76 ? 'c' : 's' };
                    const std::string nm   = { pre[0], pre[1], static_cast<char>('0' + n / 10), static_cast<char>('0' + n % 10) };
                    auto [f, r]            = lookupActor(s.caster, dat::FxType::Scheduler, dat::fxName(nm));
                    if (r)
                    {
                        startScheduler(f, r, s.caster, s.target, s.depth + 1);
                    }
                }
                break;
            }
            case 0x05: // caster / target plays a motion (§S.3); the character animation picks it up
            case 0x06:
                if (motions_.size() < 64)
                {
                    const auto nm = dat::fxNameString(name);
                    motions_.push_back({ op == 0x05 ? s.caster : s.target, nm.substr(0, nm.size() == 4 && nm[3] == '?' ? 3 : nm.size()),
                                         static_cast<float>(std::max<int16_t>(dur, 0)) / kTicksPerSecond });
                    if (std::getenv("XI_FX_TRACE"))
                    {
                        spdlog::info("fx: {} motion {} on {:x} ({} ticks)", dat::fxNameString(s.res->name), nm, motions_.back().actor, dur);
                    }
                }
                break;
            case 0x0A: // sound at the caster / target (§S.3): the Sep chunk, own file first, then global
            case 0x0B:
            case 0x60: // a Sep named rec+8, this file first then the others (type 0x3d), volume = a global level x 127,
                       // flag 0x40 (CODE 0x1005b720 -> 0x10036e30; cutscene sound effects, e.g. 30812 `bash` '8158').
                       // INFERRED: played at the caster like 0x0A.
                // The weapon's swing whoosh `skaz` only on a swing that does not connect: on a hit the contact sound
                // (`shit`) is heard alone (as the user hears retail; the game's own test was not found, INFERRED).
                if (sounds_.size() < 32 && !(name == dat::fxName("skaz") && s.action && s.result < s.action->results.size() &&
                                             s.action->results[s.result].vars[0x28] == 0))
                {
                    const uint32_t actor = op == 0x0B ? s.target : s.caster;
                    auto [f, r]          = lookup(s.file, s.res->block, dat::FxType::Sep, name);
                    if (!r)
                    {
                        std::tie(f, r) = lookupActor(actor, dat::FxType::Sep, name);
                    }
                    if (r)
                    {
                        const float far = rd<float>(p, s.pc + 0x14), near = rd<float>(p, s.pc + 0x18);
                        sounds_.push_back({ .actor = actor, .name = name, .sep = r->payload, .near = near > 0 && near < 500 ? near : 3.0f, .far = far > 0 && far < 500 ? far : 30.0f, .at = {}, .gain = 1.0f });
                    }
                    if (std::getenv("XI_FX_TRACE"))
                    {
                        spdlog::info("fx: {} sound {} on {:x}: {}", dat::fxNameString(s.res->name), dat::fxNameString(name), actor, r != nullptr);
                    }
                }
                break;
            case 0x0C: // door part translation / rotation (doors_re: ops 0x0C / 0x0D; +8 x, y, z, +0x14 part)
            case 0x0D:
                if (partMoves_.size() < 64 && s.pc + 0x18 <= p.size())
                {
                    partMoves_.push_back({ s.caster, rd<uint32_t>(p, s.pc + 0x14), op == 0x0D,
                                           glm::vec3(rd<float>(p, s.pc + 8), rd<float>(p, s.pc + 12), rd<float>(p, s.pc + 16)),
                                           static_cast<float>(std::max<int16_t>(dur, 0)) / kTicksPerSecond });
                    if (std::getenv("XI_FX_TRACE"))
                    {
                        spdlog::info("fx: {} part {} {} -> ({:.3f}, {:.3f}, {:.3f}) over {} ticks", dat::fxNameString(s.res->name), partMoves_.back().part,
                                     op == 0x0D ? "rot" : "off", partMoves_.back().target.x, partMoves_.back().target.y, partMoves_.back().target.z, dur);
                    }
                }
                break;
            case 0x04: // camera: run Camera chunk `name` over `dur` (CODE 0x10059f6c -> 0x1003c7d0 / 0x1003cbb0)
                if (const auto [cf, cr] = lookup(s.file, s.res->block, dat::FxType::Camera, name); cr && cr->payload.size() >= 0x20)
                {
                    // Payload: 16 bytes, u32 flags at +0x10 (low byte = key count; bits 19 / 20 add the current / follow
                    // camera: not done), 12 pad, then 0x30-byte keys from +0x20 {eye xyz, zoom, at xyz, roll, t}.
                    // DATA: 30834 `c043` (2 keys, t 0 and 1).
                    CameraTask task;
                    const auto n = rd<uint8_t>(cr->payload, 0x10);
                    for (std::size_t k = 0; k < n && 0x20 + 0x30 * (k + 1) <= cr->payload.size(); ++k)
                    {
                        const std::size_t o = 0x20 + 0x30 * k;
                        task.keys.push_back({ { rd<float>(cr->payload, o), rd<float>(cr->payload, o + 4), rd<float>(cr->payload, o + 8) },
                                              { rd<float>(cr->payload, o + 0x10), rd<float>(cr->payload, o + 0x14), rd<float>(cr->payload, o + 0x18) },
                                              rd<float>(cr->payload, o + 0x0C), rd<float>(cr->payload, o + 0x1C), rd<float>(cr->payload, o + 0x20) });
                    }
                    task.duration = std::max<float>(dur, 0.0f);
                    task.name     = name;
                    if (!task.keys.empty())
                    {
                        shot_ = std::move(task);
                    }
                    if (std::getenv("XI_FX_TRACE"))
                    {
                        spdlog::info("fx: {} camera {} ({} keys, {} ticks)", dat::fxNameString(s.res->name), dat::fxNameString(name), n, dur);
                    }
                }
                break;
            case 0x3B: // sub-scheduler by name, from this file up, else from the caster's own files (CODE 0x1005da3b:
            {          // 0x10056ae0 / 0x1005e420 -> 0x10056ba0)
                auto [f, r] = lookup(s.file, s.res->block, dat::FxType::Scheduler, name);
                if (!r)
                {
                    std::tie(f, r) = lookupActor(s.caster, dat::FxType::Scheduler, name);
                }
                if (r)
                {
                    startScheduler(f, r, s.caster, s.target, s.depth + 1, 0.0f, s.rate);
                }
                break;
            }
            case 0x78: // a sound from the caster's own files named "0a" + its sound set (CODE 0x1005bea2: actor +0x879 as a
            {          // hex digit, '1' + actor +0x87a), at the caster. The actor bytes are not known here: the first
                       // `0a??` Sep of the caster's files plays (INFERRED; DATA: monsters' weapon swing sets).
                if (sounds_.size() < 32 && actorFiles_)
                {
                    for (const auto id : actorFiles_(s.caster))
                    {
                        const auto* af = file(id);
                        if (!af)
                        {
                            continue;
                        }
                        const auto it = std::ranges::find_if(af->resources(), [](const dat::FxResource& r) {
                            return r.type == dat::FxType::Sep && (r.name & 0xFFFF) == 0x6130; // "0a.."
                        });
                        if (it != af->resources().end())
                        {
                            sounds_.push_back({ .actor = s.caster, .name = it->name, .sep = it->payload, .near = 3.0f, .far = 30.0f, .at = {}, .gain = 1.0f });
                            break;
                        }
                    }
                }
                break;
            }
            case 0x2E: // motion hold (CODE 0x10062240 / 0x100621f0): the caster's (0x2E) / target's (0x2F) motion stays
            case 0x2F: // for the duration, then the stance motion returns
                if (motions_.size() < 64)
                {
                    FxMotion m;
                    m.actor = op == 0x2E ? s.caster : s.target;
                    m.hold  = static_cast<float>(std::max<int16_t>(dur, 0)) * s.rate / kTicksPerSecond;
                    motions_.push_back(m);
                }
                break;
            case 0x0E: // screen colour over this frame's image, zoomed (CODE 0x10060260, draw 0x1001385d): +8 colour
            case 0x10: // (alpha x 2), +0xC zoom, over `dur`; 0x10 = freeze frame: the image when it started, faded out
            {          // over `dur` (CODE 0x1005ff00 / 0x10012f00)
                Layer l;
                l.op       = static_cast<int>(op);
                l.duration = std::max<float>(static_cast<float>(dur) * s.rate, 1.0f);
                if (op == 0x0E && s.pc + 0x10 <= p.size())
                {
                    l.colour = glm::vec4(rd<uint8_t>(p, s.pc + 8), rd<uint8_t>(p, s.pc + 9), rd<uint8_t>(p, s.pc + 10), rd<uint8_t>(p, s.pc + 11)) / 255.0f;
                    l.zoom   = rd<float>(p, s.pc + 0xC);
                    auto& sc     = screenColour_;
                    sc.from      = sc.colour;
                    sc.to        = l.colour;
                    sc.zStart    = sc.z;
                    sc.zTarget   = l.zoom;
                    sc.duration  = l.duration;
                    sc.remaining = l.duration;
                    sc.z         = sc.zTarget; // the tick's first frame (frac 1) gives the target (CODE 0x1006035c)
                }
                if (op == 0x10)
                {
                    ++freezeSerial_;
                }
                if (static int logged = 0; logged < 12)
                {
                    ++logged;
                    spdlog::info("fx: camera op 0x{:02X} ({}) for {:.2f} s, colour {:.2f} {:.2f} {:.2f} {:.2f}, zoom {:.3f}", op,
                                 op == 0x10 ? "freeze frame" : "screen colour", l.duration / kTicksPerSecond, l.colour.r, l.colour.g,
                                 l.colour.b, l.colour.a, l.zoom);
                }
                std::erase_if(layers_, [&](const Layer& o) { return o.op == l.op; }); // singletons in the game
                layers_.push_back(l);
                break;
            }
            case 0x0F: // screen colour (fades `fdi1` / `fdo1`): +8 R, G, B, A bytes (0x80 = 1) over `dur`
                if (s.pc + 12 <= p.size())
                {
                    screen_.from     = screenColour();
                    screen_.to       = glm::vec4(rd<uint8_t>(p, s.pc + 8), rd<uint8_t>(p, s.pc + 9), rd<uint8_t>(p, s.pc + 10), rd<uint8_t>(p, s.pc + 11)) / 128.0f;
                    screen_.duration = std::max<float>(dur, 0.0f);
                    screen_.elapsed  = 0;
                }
                break;
            case 0x52: // wait for a Vana'diel clock window (CODE 0x100583e0): {start, end, period, accept} in 1/60 Vana'diel
            {          // seconds of the day (5,184,000); fires when (now - start) mod period < accept inside the window
                constexpr float kDay = 5184000.0f;
                const float     now  = dayFraction_ * kDay;
                const float     st   = static_cast<float>(rd<uint32_t>(p, s.pc + 8));
                float           en = static_cast<float>(rd<uint32_t>(p, s.pc + 0xC));
                const float     period = std::max(1.0f, static_cast<float>(rd<uint32_t>(p, s.pc + 0x10)));
                const float     accept = static_cast<float>(rd<uint32_t>(p, s.pc + 0x14));
                if (en == st)
                {
                    en += kDay;
                }
                auto wrap = [&](float x) { return std::fmod(std::fmod(x, kDay) + kDay, kDay); };
                const float d = wrap(now - st), span = wrap(en - st);
                if (!(d < span && std::fmod(d, period) < accept))
                {
                    s.wait = 30.0f; // look again in half a second (the game waits the exact time)
                    return true;
                }
                break;
            }
            case 0x2B: // the damage shows (file 0 `mdam`, `ldad`): release the result's messages
            case 0x71: // the same with a kind at +8 (1 after an added effect, 2 after spikes; CODE 0x1018c780)
                if (s.action && s.action->tag && commits_.size() < 256)
                {
                    commits_.push_back(s.action->tag | (s.result + 1));
                }
                break;
            case 0x75: // hide (+8 != 0) or show the caster's model part wepN, N = s16 +0xC (CODE 0x1005bd44: +8 != 0 ->
            case 0xA3: // 0x100d6010, else 0x100d5e40; 0xA3 the same through 0x1005be24). Both walk the Os2 parts named
            {          // 'wep0' + N and set a per-part byte through 0x1002c660 (0x75 byte 0, 0xA3 byte 1): 0x100d6010 writes 1,
                       // 0x100d5e40 writes 0. Any byte set reads as hidden (0x1002b3a0 tests the dword, 0x100cf05e then
                       // runs `!w02` on the actor instead of `!w01`). DATA agrees: treasure chests' `init` (model 258) sets
                       // the box parts 3 / 4 to 0 and the variants 5..7 to 1; Aern `init` (model 1373) hides the body
                       // (`hiof`, 1) and shows it again 60 frames later (`hion`, 0).
                const int n = rd<int16_t>(p, s.pc + 0xC);
                if (n >= 0 && n <= 9)
                {
                    auto& m = hiddenParts_[s.caster];
                    m       = rd<uint32_t>(p, s.pc + 8) ? static_cast<uint16_t>(m | (1u << n)) : static_cast<uint16_t>(m & ~(1u << n));
                    if (std::getenv("XI_FX_TRACE"))
                    {
                        spdlog::info("fx: op {:#x} {} wep{} of {:#x} (res {:#x})", op, rd<uint32_t>(p, s.pc + 8) ? "hides" : "shows", n, s.caster, s.res ? s.res->name : 0u);
                    }
                }
                break;
            }
            case 0x3F: // stop generator +8 emitting, start generator +0x10 (CODE 0x10059dbe)
                for (auto& g : generators_)
                {
                    if (g->res && g->res->name == name && g->caster == s.caster)
                    {
                        g->stopped = true;
                    }
                }
                if (const auto [f, r] = lookup(s.file, s.res->block, dat::FxType::Generator, rd<uint32_t>(p, s.pc + 0x10)); r)
                {
                    startGenerator(f, r, s.caster, s.target, static_cast<float>(dur == 0 ? 1 : dur) * s.rate);
                }
                break;
            case 0x53: // sound at the action target nearest the caster (CODE 0x1005b3e9)
                if (sounds_.size() < 32)
                {
                    uint32_t actor = s.target;
                    if (s.action && actors_ && *actors_)
                    {
                        const auto c    = (*actors_)(s.caster);
                        float      best = std::numeric_limits<float>::max();
                        for (const auto& res : s.action->results)
                        {
                            const auto t = (*actors_)(res.target);
                            if (c && t && glm::distance(c->pos, t->pos) < best)
                            {
                                best  = glm::distance(c->pos, t->pos);
                                actor = res.target;
                            }
                        }
                    }
                    auto [f, r] = lookup(s.file, s.res->block, dat::FxType::Sep, name);
                    if (!r)
                    {
                        std::tie(f, r) = lookupActor(actor, dat::FxType::Sep, name);
                    }
                    if (r)
                    {
                        const float far = rd<float>(p, s.pc + 0x14), near = rd<float>(p, s.pc + 0x18);
                        sounds_.push_back({ .actor = actor, .name = name, .sep = r->payload, .near = near > 0 && near < 500 ? near : 3.0f, .far = far > 0 && far < 500 ? far : 30.0f, .at = {}, .gain = 1.0f });
                    }
                }
                break;
            // Dolls (CODE 0x100589fd, 0x10058ace / 0x10058b38): the caster (0x15 / 0x22) or target (0x16 / 0x23) is
            // replaced by a copy of where it is now, so what starts next stays there; 0x17 / 0x18 put the actor back.
            // 0x22 / 0x23 dolls follow the actor's position (CXiDollActor flags +0x424..+0x426, update 0x100ac020); none
            // is drawn (the doll has no model).
            case 0x15:
            case 0x22:
                s.realCaster = s.realCaster ? s.realCaster : s.caster;
                s.caster     = makeDoll(s.caster, op == 0x22);
                break;
            case 0x16:
            case 0x23:
                s.realTarget = s.realTarget ? s.realTarget : s.target;
                s.target     = makeDoll(s.target, op == 0x23);
                break;
            case 0x17:
                if (s.realCaster)
                {
                    s.caster = std::exchange(s.realCaster, 0);
                }
                break;
            case 0x18:
                if (s.realTarget)
                {
                    s.target = std::exchange(s.realTarget, 0);
                }
                break;
            case 0x21: // damage flinch on the caster / target (§S.3)
            case 0x25:
                if (motions_.size() < 64)
                {
                    motions_.push_back({ op == 0x21 ? s.caster : s.target, {}, 0.0f, true });
                    if (std::getenv("XI_FX_TRACE"))
                    {
                        spdlog::info("fx: {} flinch on {:x}", dat::fxNameString(s.res->name), motions_.back().actor);
                    }
                }
                break;
            case 0x5A: // the caster's guard motion (parry / guard reactions run from the defender's files, roles swapped)
            case 0x5B:
                if (motions_.size() < 64)
                {
                    motions_.push_back({ s.caster, {}, 0.0f, false, true });
                    if (std::getenv("XI_FX_TRACE"))
                    {
                        spdlog::info("fx: {} guard motion on {:x}", dat::fxNameString(s.res->name), s.caster);
                    }
                }
                break;
            case 0x29: // tint the caster / target: u8 R, G, B, A at +8 (0x80 = 1.0) over the record's duration. Order from
            case 0x2A: // the added-effect flashes (atpr): water hi10 60 80 c0 80 blue, earth hit8 ca ab 61 80 yellow
            case 0x46: // colour slot 1 of the caster (0x46) / target (0x47): the texture overlay's colour, not the body's
            case 0x47: // (CODE 0x1005b15b / 0x1005b1d4 -> 0x1005f5f0 slot 1: read vfunc +0x40c, written +0x60 = actor +0x74c,
                       // beside the overlay texture +0x750; slot 0 is +0x660 via +0x2c / +0x30). Skeleton actors only.
            {
                const uint32_t id = op == 0x29 || op == 0x46 ? s.caster : s.target;
                const glm::vec4 to(rd<uint8_t>(p, s.pc + 8) / 128.0f, rd<uint8_t>(p, s.pc + 9) / 128.0f, rd<uint8_t>(p, s.pc + 10) / 128.0f,
                                   rd<uint8_t>(p, s.pc + 11) / 128.0f);
                auto& t   = (op == 0x46 || op == 0x47 ? tints2_ : tints_)[id];
                t.from    = t.now();
                t.to      = to;
                t.elapsed = 0;
                t.duration = static_cast<float>(std::max<int16_t>(dur, 0)) * s.rate;
                if (std::getenv("XI_FX_TRACE"))
                {
                    spdlog::info("fx: {} colour {:02x} on {:x} -> ({:.2f} {:.2f} {:.2f} {:.2f}) over {} ticks", dat::fxNameString(s.res->name), op, id,
                                 to.r, to.g, to.b, to.a, t.duration);
                }
                break;
            }
            case 0x5F: // kill every running copy of the named scheduler (§C.3)
                for (auto& o : schedulers_)
                {
                    if (&o != &s && o.res && o.res->name == name && o.caster == s.caster)
                    {
                        o.dead = true;
                    }
                }
                break;
            case 0x2C: // weapon trail: the Afb whose first 3 characters match, in the scheduler's file (§S.4, §M.5)
                if (const auto* afb = s.file ? s.file->find(dat::FxType::Afb, name, -1, 0x00FFFFFF) : nullptr; afb)
                {
                    const auto a  = afb->payload;
                    const auto rp = p.subspan(s.pc);
                    Trail      t;
                    t.caster       = s.caster;
                    const int n    = rd<uint8_t>(a, 0);
                    const float sx = rd<float>(a, 4), sy = rd<float>(a, 8), sz = rd<float>(a, 0xC);
                    const auto offA = rd<uint32_t>(a, 0x14), offB = rd<uint32_t>(a, 0x18);
                    const float rate = std::max<float>(1.0f, rd<uint8_t>(a, 1)); // samples per second of motion (30 = per frame)
                    const float first = rd<uint8_t>(a, 0x10), frames = std::max<float>(1.0f, rd<uint8_t>(a, 0x12));
                    t.duration = std::max<float>(1.0f, static_cast<float>(dur));
                    for (int k = 0; k < n; ++k)
                    {
                        auto edge = [&](uint32_t off) {
                            return glm::vec3(rd<int16_t>(a, off + 8 * static_cast<std::size_t>(k)) * sx, rd<int16_t>(a, off + 8 * static_cast<std::size_t>(k) + 2) * sy,
                                             rd<int16_t>(a, off + 8 * static_cast<std::size_t>(k) + 4) * sz);
                        };
                        t.edgeA.push_back(edge(offA));
                        t.edgeB.push_back(edge(offB));
                        // Sample k sits at motion frame first + k * 30 / rate; the motion's frames span the duration.
                        const float frame = first + static_cast<float>(k) * 30.0f / rate;
                        t.sampleTick.push_back(frame / frames * t.duration);
                    }
                    auto col = [&](std::size_t at) {
                        return glm::vec4(rd<uint8_t>(rp, at) / 128.0f, rd<uint8_t>(rp, at + 1) / 128.0f, rd<uint8_t>(rp, at + 2) / 128.0f, rd<uint8_t>(rp, at + 3) / 128.0f);
                    };
                    t.colA  = col(0x10);
                    t.colB  = col(0x14);
                    t.fadeA = rd<float>(rp, 0x24) / 128.0f;
                    t.fadeB = rd<float>(rp, 0x34) / 128.0f;
                    if (n >= 2 && trails_.size() < 64)
                    {
                        trails_.push_back(std::move(t));
                    }
                }
                break;
            case 0x09: // from the target's files, roles swapped
            {
                auto [f, r] = lookupActor(s.target, dat::FxType::Scheduler, name);
                if (!r)
                {
                    std::tie(f, r) = lookup(s.file, s.res->block, dat::FxType::Scheduler, name);
                }
                if (r)
                {
                    startScheduler(f, r, s.target, s.caster, s.depth + 1);
                }
                break;
            }
            // Expression VM (§S.5; operators CODE 0x1005d18e, jump table 0x1005deb0): 0x6B pushes / combines tokens,
            // 0x64/0x65 pop a condition and skip to the matching else (0x67) or block end (0x6A) when it is 0.
            case 0x6B:
            {
                const auto sub = rd<uint16_t>(p, s.pc + 8);
                if (sub == 0x1C)
                {
                    const auto kind = rd<uint16_t>(p, s.pc + 0xA);
                    int32_t    v    = 0;
                    if (kind == 1)
                    {
                        v = rd<int32_t>(p, s.pc + 0xC);
                    }
                    else if (kind == 2)
                    {
                        v = static_cast<int32_t>(rd<float>(p, s.pc + 0xC));
                    }
                    else if (kind == 3)
                    {
                        const auto idx = rd<uint32_t>(p, s.pc + 0xC);
                        if (idx == 4)
                        {
                            v = static_cast<int32_t>(rng_() % 10) - 5;
                        }
                        else if (idx == 0x38) // the action's actor is a PC: server id top byte 0 (CODE 0x10062995)
                        {
                            v = s.action && s.action->actor != 0 && (s.action->actor >> 24) == 0;
                        }
                        else if (idx == 0x39) // the caster is a zone NPC / monster: id top byte 1 (CODE 0x100629c6)
                        {
                            v = (s.caster >> 24) == 1;
                        }
                        else if (s.action && s.result < s.action->results.size() && idx < 0x40)
                        {
                            v = s.action->results[s.result].vars[idx];
                        }
                    }
                    s.stack.push_back(v);
                }
                else if (sub >= 5 && sub <= 0x12 && s.stack.size() >= 2)
                {
                    const int32_t b = s.stack.back();
                    s.stack.pop_back();
                    const int32_t a = s.stack.back();
                    int32_t       r = 0;
                    switch (sub)
                    {
                        case 5: r = a + b; break;
                        case 6: r = a - b; break;
                        case 7: r = a * b; break;
                        case 8: r = b ? a / b : 0; break;
                        case 9: r = a << (b & 31); break;
                        case 10: r = a >> (b & 31); break;
                        case 0x0B: r = a != b; break;
                        case 0x0C: r = a == b; break;
                        case 0x0D: r = a >= b; break;
                        case 0x0E: r = a <= b; break;
                        case 0x0F: r = a > b; break;
                        case 0x10: r = a < b; break;
                        case 0x11: r = a & b; break;
                        case 0x12: r = a | b; break;
                        default: break;
                    }
                    s.stack.back() = r;
                }
                break;
            }
            case 0x64:
            case 0x65:
            {
                const int32_t c = s.stack.empty() ? 0 : s.stack.back();
                if (!s.stack.empty())
                {
                    s.stack.pop_back();
                }
                if (c == 0)
                {
                    skipBlock(s, true);
                    continue;
                }
                break;
            }
            case 0x67: // reached the else from the taken branch: skip it
                skipBlock(s, false);
                continue;
            // Actor ops (effect-format "Coverage, part 7A"; effects_actor.cpp).
            case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x48: case 0x49: case 0x4A:
            case 0x54: case 0x55: case 0x5C: case 0x5E: case 0x79: case 0x84: case 0x86: case 0x88: case 0x89:
            case 0x8C: case 0x8D: case 0x8E: case 0x8F: case 0x90: case 0x91: case 0x92: case 0x93: case 0x94:
            case 0x95: case 0x96: case 0x98: case 0x99: case 0x9A: case 0x9E: case 0x9F: case 0xBE: case 0x6E:
                actorOp(s, static_cast<uint8_t>(op), dur);
                break;
            // --- Coverage, part 7B (effect-format): screen, environment, sound and actor-state ops. ---
            case 0x19: // `main` of effect file 2800 + rec+8 from the caster to the target, once the file is loaded (CODE
            {          // 0x1005834c -> vfunc 0x2c4 = 0x100a9630 -> 0x100a9800: id + 0xaf0; DATA 0 `spop` 0x268 -> 3416)
                if (const auto* f = file(2800 + rd<uint32_t>(p, s.pc + 8)))
                {
                    if (const auto [ff, r] = lookup(f, -1, dat::FxType::Scheduler, dat::fxName("main")); r)
                    {
                        startScheduler(ff, r, s.caster, s.target, s.depth + 1);
                    }
                }
                break;
            }
            case 0x51: // screen colours the game keeps (CODE 0x10058ebf / 0x10058fb8, tick 0x10060510, end 0x10060680): bytes
            case 0x72: // R, G, B, A at +8 from the current colour over `dur` - 1 ticks; 0x51 -> 0x104568fc +0x334 (+0x31c, the
            {          // dimming pass 0x1000a820), 0x72 -> +0x328 (+0x324, the additive pass 0x10013270)
                auto& d    = op == 0x51 ? screenDim_ : screenAdd_;
                d.from     = d.colour;
                d.to       = glm::vec4(rd<uint8_t>(p, s.pc + 8), rd<uint8_t>(p, s.pc + 9), rd<uint8_t>(p, s.pc + 10), rd<uint8_t>(p, s.pc + 11)) / 255.0f;
                d.duration = std::max(static_cast<float>(dur) * s.rate - 1.0f, 0.0f);
                d.elapsed  = 0;
                break;
            }
            case 0x6F: // a light generator's intensity (0x6F, particle +0x1a4) or range (0x70, +0x1a8) scale to the float at
            case 0x70: // +0x10 over `dur` (CODE 0x10059cd7 -> 0x100608c0, tick 0x10060920); DATA zone 104 `on01` lamps
                for (auto& g : generators_)
                {
                    if (g->light && g->res && g->res->name == name && g->file == s.file)
                    {
                        lightDrives_.push_back({ g.get(), op == 0x6F, 1.0f, rd<float>(p, s.pc + 0x10), std::max<float>(static_cast<float>(dur) * s.rate, 1.0f), 0.0f, false });
                    }
                }
                break;
            case 0x7A: // the caster held at the target's attach point +0x20 plus the offset +0x10 turned by the target's
            {          // yaw, for `dur` (CODE 0x1005c6f9 -> 0x10061f00; actor +0x7a4 / +0x7b4, applied by 0x100c6e7a)
                Attach a;
                a.actor     = s.caster;
                a.target    = s.target;
                a.point     = rd<uint32_t>(p, s.pc + 0x20);
                a.offset    = glm::vec3(rd<float>(p, s.pc + 0x10), rd<float>(p, s.pc + 0x14), rd<float>(p, s.pc + 0x18));
                a.remaining = std::max<float>(static_cast<float>(dur) * s.rate, 1.0f);
                std::erase_if(attaches_, [&](const Attach& o) { return o.actor == a.actor; });
                if (a.actor != a.target)
                {
                    attaches_.push_back(a);
                }
                break;
            }
            case 0x7C: // weather: the group named +8 now (CODE 0x10057f4e -> Env_ChangeWeather 0x10187400, offset 0)
                environment_.fourcc       = dat::fxNameString(name);
                environment_.blendSeconds = 0;
                environment_.record       = {};
                ++environment_.serial;
                break;
            case 0xAE: // the alternate fourcc table (+8: 1 on, 0 off) for the weather shown now, cross-fading over +0xC
            {          // Vana'diel minutes (x 3600 ticks; CODE 0x1005cf4f -> 0x1018b9f0, 0x10187400). DATA 158 `ints` / `inte`
                environment_.altTable     = rd<uint32_t>(p, s.pc + 8) != 0;
                environment_.blendSeconds = std::max(rd<float>(p, s.pc + 0xC), 0.0f) * 2.4f; // a Vana'diel minute = 2.4 s
                environment_.record       = {};
                ++environment_.serial;
                break;
            }
            case 0x7E: // one Weather record (type 0x2f) named +8 applied as it is (CODE 0x10058399 -> 0x1018af20)
                if (const auto [wf, wr] = lookup(s.file, s.res->block, dat::FxType::Weather, name); wr && wr->payload.size() >= 0xAC)
                {
                    environment_.record       = wr->payload;
                    environment_.blendSeconds = 0;
                    ++environment_.serial;
                }
                break;
            case 0x8A: // a sound held in slot +0x20 (< 8) until op 0x8B stops it (CODE 0x1005b7d0: the Sep named +8, type 0x3d,
            case 0x8B: // -> 0x10036e30, handle in 0x1047bcc4[slot]; 0x1005b8ae -> 0x10036ac0 stops it)
            {
                const uint32_t slot = rd<uint32_t>(p, s.pc + 0x20);
                if (slot >= 8)
                {
                    break;
                }
                if (op == 0x8B)
                {
                    soundStops_.push_back(slot + 1);
                }
                else if (const auto [sf, sr] = lookup(s.file, s.res->block, dat::FxType::Sep, name); sr)
                {
                    sounds_.push_back({ .actor = s.caster, .name = name, .sep = sr->payload, .near = 3.0f, .far = 30.0f, .at = {}, .gain = 1.0f, .slot = slot + 1 });
                }
                break;
            }
            case 0x9B: // the caster holds weapon +8 (0 main, 1 sub; > 1 = 0) in the grip for `dur` (CODE 0x1005c577 ->
            case 0xA7: // 0x10061d30, end 0x10061ce0: vfunc 0x324 -> actor +0x83e[slot]); 0xA7 on, 0xA8 off with no time
            case 0xA8: // (CODE 0x1005c5c7 / 0x1005c600)
            {
                const int slot = rd<uint32_t>(p, s.pc + 8) > 1 ? 0 : static_cast<int>(rd<uint32_t>(p, s.pc + 8));
                std::erase_if(holds_, [&](const ActorHold& h) { return h.actor == s.caster && h.kind == 0 && h.value == slot; });
                if (op != 0xA8)
                {
                    holds_.push_back({ s.caster, 0, slot, op == 0x9B ? std::max<float>(static_cast<float>(dur) * s.rate, 1.0f) : -1.0f });
                }
                break;
            }
            case 0xBC: // footsteps while standing for `dur` (CODE 0x1005c531 -> 0x10061ba0: actor +0x8ab, read by 0x100d4d91)
                holds_.push_back({ s.caster, 1, 0, std::max<float>(static_cast<float>(dur) * s.rate, 1.0f) });
                break;
            case 0x9D: // a following doll of the target that takes its place, as 0x23 (CODE 0x10058912: CXiDollActor 0x100abdd0
                       // (target, 1), then 0x100ac1c0(+8) writes +8 into its 48 point slots and +0x408; DATA only 0)
                s.realTarget = s.realTarget ? s.realTarget : s.target;
                s.target     = makeDoll(s.target, true);
                break;
            case 0xA1: // the target's colour as 0x2A, and a task it replaces ends at its own colour (CODE 0x1005b27c ->
            {          // 0x1005f5f0 with +0x86 = 1; end 0x1005f8c0 sets the target colour)
                const glm::vec4 to(rd<uint8_t>(p, s.pc + 8) / 128.0f, rd<uint8_t>(p, s.pc + 9) / 128.0f, rd<uint8_t>(p, s.pc + 10) / 128.0f,
                                   rd<uint8_t>(p, s.pc + 11) / 128.0f);
                auto& t    = tints_[s.target];
                t.from     = t.now();
                t.to       = to;
                t.elapsed  = 0;
                t.duration = static_cast<float>(std::max<int16_t>(dur, 0)) * s.rate;
                break;
            }
            case 0xB2: // the target's colour back to 0x80808080 (CODE 0x1005bd26 -> 0x100617d0: vfunc 0x30, entity kinds 2 / 3)
                tints_.erase(s.target);
                break;
            case 0xA4: // the caster's run (0xA4) / walk (0xA5) motion: `run` `1un` `2un` `3un` / `wlk` `1lk` .. by +8 (CODE
            case 0xA5: // 0x1005c011 / 0x1005c068, tables 0x1032b824 / 0x1032b834 -> actor +0x7d0 / +0x7cc, entity +0x1b4 ..)
                motionVariants_[{ s.caster, op == 0xA4 ? 2 : 3 }] = static_cast<int>(std::min<uint32_t>(rd<uint32_t>(p, s.pc + 8), 3));
                break;
            case 0xAC: // which scheduler the caster's death runs: `dead` `1ead` `2ead` `3ead` by +8 & 3 (CODE 0x1005c0fb ->
                       // vfunc 0x130: entity +0x130 bits 5-6, read at 0x1008ca55 from table 0x1032d5f0)
                motionVariants_[{ s.caster, 4 }] = static_cast<int>(rd<uint32_t>(p, s.pc + 8) & 3);
                break;
            case 0xA6: // music level +8 (float) over +0xC ticks; 0 stops it (CODE 0x1005b95b -> 0x10035d60 -> 0x10035930
                       // type 8, tick 0x10035870 -> 0x10036020)
                musicFade_ = std::pair{ std::clamp(rd<float>(p, s.pc + 8), 0.0f, 1.0f), static_cast<float>(rd<int32_t>(p, s.pc + 0xC)) / kTicksPerSecond };
                break;
            case 0xB0: // stop the camera task running Camera chunk +8 (CODE 0x1005a05c -> 0x1003cae0: CMoCameraTask)
                if (shot_ && shot_->name == name)
                {
                    shot_.reset();
                }
                break;
            case 0xB3: // a sound at the caster as 0x0A, only with config 153 bit 1 set (CODE 0x1005b531 -> 0x101937a0(1))
                if ((config153_ & 2) != 0)
                {
                    if (auto [sf, sr] = lookup(s.file, s.res->block, dat::FxType::Sep, name); sr || (std::tie(sf, sr) = lookupActor(s.caster, dat::FxType::Sep, name), sr))
                    {
                        sounds_.push_back({ .actor = s.caster, .name = name, .sep = sr->payload, .near = 3.0f, .far = 30.0f, .at = {}, .gain = 1.0f, .slot = 0 });
                    }
                }
                break;
            case 0xB6: // +8 != 0: the caster's weapon parts wep0 / wep1 shown and wep2 hidden (CODE 0x1005be66 -> 0x1005e710:
                       // actor +0x840 bit 22, read by the stance picker 0x100c86df -> 0x100d5e40(0), (1), 0x100d6010(2))
                if (rd<uint32_t>(p, s.pc + 8) != 0)
                {
                    auto& m = hiddenParts_[s.caster];
                    m       = static_cast<uint16_t>((m & ~3u) | 4u);
                }
                break;
            case 0xBA: // +8 != 0: the caster's stance motion is not changed until cleared (CODE 0x1005b0cc -> 0x100d5360:
            case 0xB9: // actor +0x840 bit 23 / 0xB9 vfunc 0x150 +0x8ac bit 1; read at 0x100c8598 / 0x100c7171 before the stance
            {          // picker); as a motion hold. 0xB9 also places the actor between points 0x17 / 0x18 (0x100ccd90): not done
                FxMotion m;
                m.actor = s.caster;
                m.hold  = rd<uint32_t>(p, s.pc + 8) != 0 ? 1.0e6f : 0.001f;
                motions_.push_back(m);
                break;
            }
            case 0x62: // the caster turns toward the target at +8 degrees a tick (CODE 0x1005ae5c: actor +0x870 angle left,
                       // +0x874 signed rate; with a 0x78 task 0x10060eb0 holding it for `dur`)
                turns_.push_back({ s.caster, s.target, 0.0f, rd<float>(p, s.pc + 8) * 0.017452778f, 0.0f,
                                   std::max<float>(static_cast<float>(dur) * s.rate, 60.0f), false });
                break;
            case 0xBF: // knock the target back (CODE 0x1005abd3 -> 0x10060cd0): the `bf0` / `bb0` motion by the side it was hit
            {          // from (0x100ce0b0 picks; +0xA != 0 swaps); the push itself (+0xC speed / 60, +0x10, +0x14) is not done
                const auto ac = actors_ && *actors_ ? (*actors_)(s.caster) : std::nullopt;
                const auto at = actors_ && *actors_ ? (*actors_)(s.target) : std::nullopt;
                bool       front = true;
                if (ac && at)
                {
                    const glm::vec2 facing(std::cos(at->yaw), std::sin(at->yaw)), toCaster(ac->pos.x - at->pos.x, ac->pos.z - at->pos.z);
                    front = glm::dot(facing, toCaster) >= 0.0f;
                }
                if (rd<int16_t>(p, s.pc + 0xA) != 0)
                {
                    front = !front;
                }
                FxMotion m;
                m.actor   = s.target;
                m.stem    = front ? "bf0" : "bb0";
                m.seconds = std::max<float>(static_cast<float>(dur) * s.rate, 1.0f) / kTicksPerSecond;
                motions_.push_back(m);
                break;
            }
            case 0x1D: // lift platform move (lift_re.md): client::Doors::updateLifts moves lifts from the server's state
            case 0x27: // path drive (ships: client::Ships runs the `seq` legs; ship_re.md)
            case 0xA9: // actor rotation drive (ships: client::Ships)
                break;
            default: break; // motions, sounds, camera, results ... are not effects
        }
        s.wait += static_cast<float>(delay) * s.rate;
        s.pc += 4 * len;
        if (s.after != SIZE_MAX)
        {
            s.pc    = std::exchange(s.after, SIZE_MAX); // the random branch's record ran: skip the rest of its group
        }
    }
    return true;
}

void EffectSystem::skipBlock(Scheduler& s, bool toElse)
{
    // From the record after s.pc: nested 0x69 / 0x6A pairs are skipped whole; stop after the 0x6A that closes the
    // enclosing block, or (toElse) after a 0x67 at this level.
    const auto p     = s.res->payload;
    int        depth = 0;
    s.pc += 4 * ((rd<uint32_t>(p, s.pc) >> 8) & 0x1F);
    while (s.pc + 8 <= p.size())
    {
        const auto w   = rd<uint32_t>(p, s.pc);
        const auto op  = w & 0xFF;
        const auto len = (w >> 8) & 0x1F;
        if (op == 0 || len == 0)
        {
            return;
        }
        s.pc += 4 * len;
        if (op == 0x69)
        {
            ++depth;
        }
        else if (op == 0x6A)
        {
            if (depth-- == 0)
            {
                return;
            }
        }
        else if (op == 0x67 && depth == 0 && toElse)
        {
            return;
        }
    }
}

void EffectSystem::place(Generator& g, const FxActorLookup& actors)
{
    if (g.zone)
    {
        const uint32_t m = (((g.origin >> 16) & 1) << 4) | (g.origin & 0xF);
        if (m == 0xe || m == 0xf)
        {
            // Sun (0xe) 1000 / moon (0xf) 900 from the camera toward the body; translation only (CODE 0x1003c485..,
            // position 0x10187630).
            g.matrix = glm::translate(glm::mat4(1.0f), camera_ + (m == 0xe ? 1000.0f * glm::normalize(towardSun_) : 900.0f * glm::normalize(towardMoon_)));
        }
        else if (g.weather)
        {
            g.matrix = glm::translate(glm::mat4(1.0f), camera_); // the sky (cloud dome, stars, rain) moves with the camera
        }
        else
        {
            g.matrix = glm::mat4(1.0f); // world placed: the particle's own position / rotation / scale (§G.4 mode 0)
        }
        g.placed = true;
        return;
    }
    // §G.4: mode, point ids on actor A (caster) and B (target).
    const uint32_t w      = g.origin;
    const uint32_t mode   = (((w >> 16) & 1) << 4) | (w & 0xF);
    const uint32_t pointA = (((w >> 18) & 1) << 6) | ((w >> 4) & 0x3F);
    const uint32_t pointB = (w >> 10) & 0x3F;
    const auto     a      = actors ? actors(g.caster) : std::nullopt;
    const auto     b      = actors ? actors(g.target) : std::nullopt;
    if (!a && !b)
    {
        return;
    }
    const FxActor A = a.value_or(*b), B = b.value_or(*a);
    // Points (§G.4, §C.6/C.7): 0x30 the ground; 0x31-0x33 the ring point (0x0D-0x14) nearest the other actor; 0x36-0x3C
    // remapped; others from the posed skeleton. Without a skeleton: estimates by height.
    auto point = [](const FxActor& self, const FxActor& other, uint32_t id) {
        const glm::vec3 up(0, -1, 0); // game space is Y down
        if (id == 0x30)
        {
            return self.pos;
        }
        static constexpr uint32_t kRemap[7] = { 0x7e, 0x7f, 0x66, 0x67, 0x68, 0x69, 0x6a };
        if (id >= 0x36 && id <= 0x3c)
        {
            id = kRemap[id - 0x36];
        }
        if (self.point)
        {
            if (id >= 0x31 && id <= 0x33)
            {
                std::optional<glm::vec3> best;
                float                    bestD = 1e9f;
                for (uint32_t k = 0x0d; k <= 0x14; ++k)
                {
                    if (const auto p = self.point(k))
                    {
                        const float d = glm::distance(*p, other.pos);
                        if (d < bestD)
                        {
                            best  = p;
                            bestD = d;
                        }
                    }
                }
                if (best)
                {
                    return *best;
                }
            }
            else if (const auto p = self.point(id))
            {
                return *p;
            }
        }
        if (id >= 0x31 && id <= 0x33)
        {
            glm::vec3 toward = other.pos - self.pos;
            toward.y         = 0;
            const float d    = glm::length(toward);
            return self.pos + up * (self.height * 0.6f) + (d > 0.01f ? toward / d * 0.35f : glm::vec3(0));
        }
        return id == 0 ? self.pos : self.pos + up * (self.height * 0.5f);
    };
    auto toward = [](const glm::vec3& from, const glm::vec3& to) {
        const glm::vec3 d = to - from;
        return std::atan2(d.z, d.x); // facing (cos a, 0, sin a)
    };
    glm::vec3 at{ 0.0f };
    float     yaw = 0.0f;
    switch (mode)
    {
        case 1: at = point(A, B, pointA), yaw = A.yaw; break;
        case 3:
        case 0x16:
        case 13:
        case 0x1a: at = point(A, B, pointA), yaw = toward(A.pos, B.pos); break;
        case 4:
        case 0x17: at = point(B, A, pointB), yaw = A.yaw; break;
        case 5:
        case 0x18: at = point(A, B, pointA), yaw = B.yaw; break;
        case 6:
        case 0x19: at = point(B, A, pointB), yaw = toward(B.pos, A.pos); break;
        case 7:
        case 9:
        case 11:
        case 0x10: at = point(A, B, pointA), yaw = A.yaw; break;
        case 0:
        case 14:
        case 15:
        case 0x14:
        case 0x15: at = A.pos, yaw = A.yaw; break;
        default: at = point(B, A, pointB), yaw = B.yaw; break; // 2, 8, 10, 12, 0x11, 0x13 ...
    }
    if (std::getenv("XI_FX_TRACE"))
    {
        spdlog::info("fx: place {} mode {:x} pA {:x} pB {:x} at ({:.2f},{:.2f},{:.2f}) A ({:.2f},{:.2f},{:.2f}) skel {}", dat::fxNameString(g.res->name), mode, pointA,
                     pointB, at.x, at.y, at.z, A.pos.x, A.pos.y, A.pos.z, static_cast<bool>(A.point));
    }
    g.matrix = glm::translate(glm::mat4(1.0f), at) * yawMatrix(yaw);
    // L1 0x7b (CODE 0x10053d90 -> Gen_AttachMatrix 0x1003bd40): a position offset (rec+4) and a rotation (rec+0x10) for
    // the attach matrix. INFERRED: both in the attach frame (the data's offsets are almost all along Y).
    const auto p = g.res->payload;
    for (std::size_t r = g.lists[1]; r + 4 <= p.size();)
    {
        const auto rw = rd<uint32_t>(p, r);
        const auto rl = (rw >> 8) & 0x1F;
        if ((rw & 0xFF) == 0 || rl == 0)
        {
            break;
        }
        if ((rw & 0xFF) == 0x7b && rl >= 7)
        {
            g.matrix = g.matrix * glm::translate(glm::mat4(1.0f), rdVec(p, r + 4)) * eulerMatrix(rdVec(p, r + 0x10));
            break;
        }
        r += 4 * rl;
    }
    g.matrix = g.matrix * glm::scale(glm::mat4(1.0f), sizeScale(w, rd<float>(p, 0x10), A, B));
    g.placed = true;
}

glm::vec3 EffectSystem::sizeScale(uint32_t origin, float weight, const FxActor& a, const FxActor& b)
{
    // Size scaling (effect-format §G.4, CODE 0x10053d90): origin bits 20-23 pick the actor (1-4 A, 5-8 B, else none)
    // and how: & 3 == 0 uniform by the larger of width and height; else bit 1 = Y by the height, bit 0 = X and Z by the
    // width. Each is (size - 1) x weight + 1, weight = chunk+0x20 (1 if 0). Stored at gen+0x78..0x80; INFERRED: applied
    // as the generator matrix's scale (its consumer is not traced). The second set (origin byte 3, chunk+0x24, gen+0x84)
    // is not applied.
    const uint32_t mode = (origin >> 20) & 0xF;
    if (mode < 1 || mode > 8)
    {
        return glm::vec3(1.0f);
    }
    const glm::vec2 s = mode <= 4 ? a.size : b.size;
    const float     k = weight == 0.0f ? 1.0f : weight;
    auto            f = [k](float v) { return (v - 1.0f) * k + 1.0f; };
    if ((mode & 3) == 0)
    {
        return glm::vec3(f(std::max(s.x, s.y)));
    }
    const float y  = (mode & 2) ? f(s.y) : 1.0f;
    const float xz = (mode & 1) ? f(s.x) : 1.0f;
    return { xz, y, xz };
}

void EffectSystem::spawn(Generator& g, uint32_t index)
{
    if (g.particles.size() >= kMaxParticles)
    {
        return;
    }
    const auto p = g.patched.empty() ? g.res->payload : std::span<const std::byte>(g.patched);
    Particle   q;
    q.spawnMatrix = g.matrix;
    q.serial      = ++particleSerial_;
    auto randF    = [&](float r) { return static_cast<float>(rng_() % 100000) * 1e-5f * r; };
    auto randSym  = [&](float r) { return (static_cast<float>(rng_() % 200000) * 1e-5f - 1.0f) * r; };
    auto randU    = [&](uint32_t n) { return n ? static_cast<uint32_t>(rng_() % n) : 0u; };
    glm::vec3 headerPos{ 0.0f };
    // The parent particle of a child generator (attached: the live one; a burst: as it was), and the first record with
    // op `op` in the parent generator's L1 list (CODE 0x10049220 on the parent's generator).
    auto parent = [&]() -> const Particle* {
        if (!g.child)
        {
            return nullptr;
        }
        if (g.parent)
        {
            const auto it = std::ranges::find_if(g.parent->particles, [&](const Particle& o) { return o.serial == g.parentSerial; });
            return it != g.parent->particles.end() ? &*it : nullptr;
        }
        return g.parentSnap.get();
    };
    auto parentRecord = [&](uint8_t want) -> std::optional<std::pair<std::span<const std::byte>, std::size_t>> {
        const dat::FxResource* pr = g.parent ? g.parent->res : g.parentRes;
        std::size_t            at = g.parent ? g.parent->lists[1] : g.parentL1;
        if (!g.child || !pr)
        {
            return std::nullopt;
        }
        const auto pp = pr->payload;
        while (at + 4 <= pp.size())
        {
            const auto w = rd<uint32_t>(pp, at);
            if ((w & 0xFF) == 0 || ((w >> 8) & 0x1F) == 0)
            {
                break;
            }
            if ((w & 0xFF) == want)
            {
                return std::pair{ pp, at };
            }
            at += 4 * ((w >> 8) & 0x1F);
        }
        return std::nullopt;
    };
    // Bit 29 is a "particle cloud" (CODE): one particle drawing (flags & 0x1ff) + 1 copies, with the velocity ops kept.
    // Weather generators (rain) get that reading as separate particles; elsewhere the older "no velocity" one stays.
    const bool cloud      = (g.flags & 0x20000000) && g.weather;
    const bool noVelocity = (g.flags & 0x20000000) != 0 && !cloud;
    auto slotVec = [&](uint32_t slot) -> glm::vec3 {
        if (slot + 3 > q.vars.size())
        {
            return glm::vec3(0);
        }
        return { asFloat(q.vars[slot]), asFloat(q.vars[slot + 1]), asFloat(q.vars[slot + 2]) };
    };
    auto setSlotVec = [&](uint32_t slot, const glm::vec3& v) {
        if (slot + 3 <= q.vars.size())
        {
            q.vars[slot] = asBits(v.x), q.vars[slot + 1] = asBits(v.y), q.vars[slot + 2] = asBits(v.z);
        }
    };
    auto channel = [&](std::size_t at, uint32_t slot, float base) {
        // Keyframe channel slot: {curve key (record offset), mode, base} (§G.3).
        if (slot + 3 <= q.vars.size())
        {
            curve(g, at, rd<uint32_t>(p, at + 8));
            q.vars[slot]     = static_cast<uint32_t>(at);
            q.vars[slot + 1] = rd<uint32_t>(p, at + 0xC);
            q.vars[slot + 2] = asBits(base);
        }
    };
    auto curveSlot = [&](std::size_t at, uint32_t slot) {
        // Keyframe slot without a base: {curve key (record offset), mode} (CODE 0x1005032a).
        if (slot + 2 <= q.vars.size())
        {
            curve(g, at, rd<uint32_t>(p, at + 8));
            q.vars[slot]     = static_cast<uint32_t>(at);
            q.vars[slot + 1] = rd<uint32_t>(p, at + 0xC);
        }
    };
    for (std::size_t at = g.lists[1]; at + 4 <= p.size();)
    {
        const auto w    = rd<uint32_t>(p, at);
        const auto op   = w & 0xFF;
        const auto len  = (w >> 8) & 0x1F;
        const auto slot = (w >> 13) & 0x3F;
        if (op == 0 || len == 0)
        {
            break;
        }
        const glm::vec3 v = rdVec(p, at + 4);
        switch (op)
        {
            case 0x01: // header (§G.8)
                q.flags  = rd<uint32_t>(p, at + 4);
                headerPos = rdVec(p, at + 0x14);
                q.pos    = headerPos;
                if (cloud)
                {
                    q.pos += glm::vec3(randSym(20.0f), randSym(20.0f), randSym(20.0f)); // the cloud's ±20 cube (rain)
                }
                q.vars.assign(rd<uint8_t>(p, at + 0x20), 0);
                q.total  = static_cast<float>(rd<int16_t>(p, at + 0x22)) + static_cast<float>(randU(static_cast<uint16_t>(rd<int16_t>(p, at + 0x24))));
                q.remaining = q.total;
                q.mesh   = g.mesh;
                break;
            case 0x02: setSlotVec(slot, noVelocity ? glm::vec3(0) : v); break;
            case 0x03: setSlotVec(slot, slotVec(slot) + glm::vec3(randSym(v.x), randSym(v.y), randSym(v.z))); break;
            case 0x06: // on a sphere
                if (!noVelocity)
                {
                    const float     r   = randF(rd<float>(p, at + 4)) + rd<float>(p, at + 8);
                    const glm::mat4 rot = eulerMatrix(glm::vec3(randSym(kPi), randSym(kPi), 0));
                    q.pos += glm::vec3(rot * glm::vec4(0, 0, r, 0));
                }
                break;
            case 0x08: // radial velocity
                if (!noVelocity)
                {
                    const glm::vec3 d = q.pos - headerPos;
                    const float     l = glm::length(d);
                    if (l > 1e-5f)
                    {
                        setSlotVec(slot, slotVec(slot) + d / l * rd<float>(p, at + 4));
                    }
                }
                break;
            case 0x09: q.rot = v; break;
            case 0x0a: q.rot = glm::vec3(wrapPi(q.rot.x + randSym(v.x)), wrapPi(q.rot.y + randSym(v.y)), wrapPi(q.rot.z + randSym(v.z))); break;
            case 0x0b: setSlotVec(slot, v); break;
            case 0x0c: setSlotVec(slot, slotVec(slot) + glm::vec3(randSym(v.x), randSym(v.y), randSym(v.z))); break;
            case 0x0f: q.scale = v; break;
            case 0x10: q.scale += glm::vec3(randF(v.x), randF(v.y), randF(v.z)); break;
            case 0x11: q.scale += glm::vec3(randF(v.x)); break;
            case 0x12: setSlotVec(slot, v); break;
            case 0x13: setSlotVec(slot, slotVec(slot) + glm::vec3(randSym(v.x), randSym(v.y), randSym(v.z))); break;
            case 0x16: // colour: file bytes R, G, B, A
                for (std::size_t c = 0; c < 4; ++c)
                {
                    q.rgba[c] = rd<uint8_t>(p, at + 4 + c);
                }
                break;
            case 0x17:
                for (std::size_t c = 0; c < 3; ++c)
                {
                    q.rgba[c] = static_cast<uint8_t>(q.rgba[c] + randU(rd<uint8_t>(p, at + 4 + c)));
                }
                break;
            case 0x18:
            {
                const auto add = static_cast<uint8_t>(randU(rd<uint8_t>(p, at + 4)));
                for (std::size_t c = 0; c < 3; ++c)
                {
                    q.rgba[c] = static_cast<uint8_t>(q.rgba[c] + add);
                }
                break;
            }
            case 0x19: // colour velocity, 4 x s16 in 1/256 per tick
                if (slot + 2 <= q.vars.size())
                {
                    q.vars[slot]     = rd<uint32_t>(p, at + 4);
                    q.vars[slot + 1] = rd<uint32_t>(p, at + 8);
                }
                break;
            case 0x1d: // flipbook over the mesh's frames
                if (q.mesh && !q.mesh->groups.empty() && q.total > 0)
                {
                    q.frameLength = q.total / static_cast<float>(q.mesh->groups.size());
                    q.frameTimer  = q.frameLength;
                }
                break;
            case 0x1e: q.blend = rd<uint16_t>(p, at + 4); break;
            case 0x1f: // ellipsoid shell / ring
            {
                const int   n      = rd<int16_t>(p, at + 0x2c);
                const float yaw    = rd<float>(p, at + 0x20) + randSym(rd<float>(p, at + 0x24));
                const float second = n != 0 ? 2 * kPi * static_cast<float>(index % static_cast<uint32_t>(n + 1)) / static_cast<float>(n + 1) - kPi : randSym(kPi);
                const float radius = randF(rd<float>(p, at + 4)) + rd<float>(p, at + 8);
                const glm::vec3 shape(rd<float>(p, at + 0xc), rd<float>(p, at + 0x10), rd<float>(p, at + 0x14));
                const glm::mat4 rot = eulerMatrix(glm::vec3(rd<float>(p, at + 0x18), rd<float>(p, at + 0x1c), 0)) * eulerMatrix(glm::vec3(second, yaw, 0));
                q.pos += glm::vec3(rot * glm::vec4(0, 0, radius, 0)) * shape;
                break;
            }
            case 0x21:
            case 0x22:
            case 0x23: channel(at, slot, q.pos[op - 0x21]); break;
            case 0x24:
            case 0x25:
            case 0x26: channel(at, slot, q.rot[op - 0x24] / kPi); break;
            case 0x27:
            case 0x28:
            case 0x29: channel(at, slot, q.scale[op - 0x27]); break;
            case 0x2a:
            case 0x2b:
            case 0x2c: channel(at, slot, static_cast<float>(q.rgba[op - 0x2a]) / 255.0f); break;
            case 0x2d: channel(at, slot, static_cast<float>(q.rgba[3]) / 255.0f); break;
            case 0x2e: channel(at, slot, q.uv.x); break;
            case 0x2f: channel(at, slot, q.uv.y); break;
            case 0x30: q.depthKey = rd<float>(p, at + 4); break; // draw-order key (CODE: p+0x12c sort key)
            // The same field, scaled by actor B's size when B is a PC model (CODE 0x10051c53: ((size - 1) * (rec+8 or 1)
            // + 1) * rec+4); xi-client has no per-race size here, so size 1 (INFERRED: Hume / Elvaan-sized).
            case 0x72: q.depthKey = rd<float>(p, at + 4); break;
            case 0x07: // on an ellipsoid shell (CODE 0x10051e83): the 0x06 direction, scaled per axis, then turned
                if (!noVelocity)
                {
                    const float r   = randF(rd<float>(p, at + 4)) + rd<float>(p, at + 8);
                    glm::vec3   dir = glm::vec3(eulerMatrix(glm::vec3(0, randSym(kPi), randSym(kPi))) * glm::vec4(0, 0, r, 0));
                    dir *= glm::vec3(rd<float>(p, at + 0xC), rd<float>(p, at + 0x10), rd<float>(p, at + 0x14));
                    q.pos += glm::vec3(eulerMatrix(glm::vec3(rd<float>(p, at + 0x18), rd<float>(p, at + 0x1C), 0)) * glm::vec4(dir, 0));
                }
                break;
            case 0x1a: // colour velocity R, G, B += rand_sym(rec s16) (§G.9)
                if (slot + 2 <= q.vars.size())
                {
                    for (std::size_t c = 0; c < 3; ++c)
                    {
                        const auto cur = static_cast<int16_t>(q.vars[slot + c / 2] >> (16 * (c % 2)));
                        const auto add = static_cast<int16_t>(std::lround(randSym(static_cast<float>(rd<int16_t>(p, at + 4 + 2 * c)))));
                        const auto nv  = static_cast<uint16_t>(cur + add);
                        q.vars[slot + c / 2] = (q.vars[slot + c / 2] & ~(0xFFFFu << (16 * (c % 2)))) | (static_cast<uint32_t>(nv) << (16 * (c % 2)));
                    }
                }
                break;
            case 0x3c: // a child generator's one emission tick at this particle now (CODE 0x1004ed3e: Gen_Tick with the particle as parent)
                startChild(g, q, rd<uint32_t>(p, at + 8), false);
                break;
            case 0x44: // a child generator riding this particle (CODE 0x1004ed7a; ticked by L2 0x25 / 0x33 / 0x46)
            case 0x53:
            case 0x6a:
                startChild(g, q, rd<uint32_t>(p, at + 8), true);
                break;
            case 0x45: break; // the child takes the parent's actors A / B: children here always do
            case 0x3d: // the wobble's heading: the angle of the spawn offset in X / Z (CODE 0x1004f01b), in slot + 3
                if (slot + 4 <= q.vars.size())
                {
                    const glm::vec3 d = q.pos - headerPos;
                    q.vars[slot + 3]  = asBits(-std::atan2(d.z, d.x));
                }
                break;
            case 0x3e: // wobble amplitude X / Y / Z: rec+4 + rand_sym(rec+8) (CODE 0x1004f20b..0x1004f269)
            case 0x3f:
            case 0x40:
                if (slot + (op - 0x3e) < q.vars.size())
                {
                    q.vars[slot + (op - 0x3e)] = asBits(rd<float>(p, at + 4) + randSym(rd<float>(p, at + 8)));
                }
                break;
            case 0x58: // point light: range, intensity, range scale - 1, intensity scale - 1 (CODE 0x10051449)
                q.lightRange          = rd<float>(p, at + 4);
                q.lightIntensity      = rd<float>(p, at + 8);
                q.lightRangeScale     = 1.0f + rd<float>(p, at + 0xC);
                q.lightIntensityScale = 1.0f + rd<float>(p, at + 0x10);
                break;
            case 0x4e: // spawn on a point of a Vtx list (CODE 0x1004e9ea): +4 the Vtx name; 0x4e a random point, 0x4f the next
            case 0x4f: // in turn (the generator's spawn count % n): pos += point. rec+0xc bit 0 first turns the point by the
            {          // attach matrix (not done here); under generator flag bit 29 the game takes another path (not done).
                if (noVelocity || cloud)
                {
                    break;
                }
                const auto [vf, vr] = lookup(g.file, g.res->block, dat::FxType::Vtx, g.vtxOverride ? g.vtxOverride : rd<uint32_t>(p, at + 4));
                if (vr)
                {
                    auto& pts = paths_[vr];
                    if (pts.empty())
                    {
                        const auto n = rd<uint32_t>(vr->payload, 0); // u32 n, 12 pad, n x {x, y, z, w} (CODE 0x10063a80)
                        for (std::size_t k = 0; k < n && 16 + 16 * (k + 1) <= vr->payload.size(); ++k)
                        {
                            pts.push_back(rdVec(vr->payload, 16 + 16 * k));
                        }
                    }
                    if (!pts.empty())
                    {
                        const auto n = static_cast<uint32_t>(pts.size());
                        q.pos += pts[op == 0x4e ? randU(n) : g.spawned % n];
                    }
                }
                break;
            }
            case 0x67: // converge (CODE 0x1004ef92): pos += velocity slot x remaining life, then the velocity reverses, so
            {          // the particle starts where it would have ended and travels back to the spawn point
                const glm::vec3 vel = slotVec(slot);
                q.pos += vel * q.remaining;
                setSlotVec(slot, -vel);
                break;
            }
            case 0x54: // follow a Vtx path (CODE 0x1004ebc9): +4 keyframe (u along the path; none = the age), +0x14 Vtx
            {          // name (DATA: Selbina `kamo` -> `line`); at spawn pos += path(0) (zonefx2 §4: the seagulls)
                const auto [vf, vr] = lookup(g.file, g.res->block, dat::FxType::Vtx, rd<uint32_t>(p, at + 0x14));
                if (vr)
                {
                    auto& path = paths_[vr];
                    if (path.empty())
                    {
                        // u32 n, 12 pad, n x {x, y, z, w} (CODE 0x10063a80).
                        const auto n = rd<uint32_t>(vr->payload, 0);
                        for (std::size_t k = 0; k < n && 16 + 16 * (k + 1) <= vr->payload.size(); ++k)
                        {
                            path.push_back(rdVec(vr->payload, 16 + 16 * k));
                        }
                    }
                    if (!path.empty())
                    {
                        q.path      = &path;
                        q.pathCurve = rd<uint32_t>(p, at + 4) && curve(g, at, rd<uint32_t>(p, at + 4)) ? static_cast<uint32_t>(at) : 0u;
                        q.pathPrev  = pathPoint(path, 0.0f);
                        q.pos += q.pathPrev;
                    }
                }
                break;
            }
            case 0x33: // D3b morph-weight channels 0..4 for L2 0x1e-0x22 (CODE: §G, pairs 0x33-0x37 / 0x1e-0x22)
            case 0x34:
            case 0x35:
            case 0x36:
            case 0x37:
            case 0x6c: // the point light's intensity curve slot (`ktpl`, for L2 0x49)
            case 0x60: // time-of-day keyframe slots for L2 0x3c-0x3f (CODE 0x1005039a / 0x100503d4): {curve, mode}
            case 0x61: // only (DATA: lamps put one at slot 0 next to an alpha channel at slot 2)
            case 0x62:
            case 0x63:
                if (slot + 2 <= q.vars.size())
                {
                    curve(g, at, rd<uint32_t>(p, at + 8));
                    q.vars[slot]     = static_cast<uint32_t>(at);
                    q.vars[slot + 1] = rd<uint32_t>(p, at + 0xC);
                }
                break;
            case 0x31: // slot.xyz = one rand_sym(rec+4) (CODE 0x10051724)
            {
                const float r = randSym(rd<float>(p, at + 4));
                setSlotVec(slot, glm::vec3(r));
                break;
            }
            case 0x32: q.distortion = rd<float>(p, at + 8); break; // class 0x22 `dist`: vfunc +0x34 (CODE 0x10051a7b)
            case 0x3a: // class 0x24 `ring` (CODE 0x10051a8b): 4 radii, 4 D3DCOLOR colours, segments, ring count - 2 (max 4)
                q.ringRadius   = { rd<float>(p, at + 4), rd<float>(p, at + 8), rd<float>(p, at + 0xC), rd<float>(p, at + 0x10) };
                q.ringColour   = { rd<uint32_t>(p, at + 0x14), rd<uint32_t>(p, at + 0x18), rd<uint32_t>(p, at + 0x1C), rd<uint32_t>(p, at + 0x20) };
                q.ringSegments = rd<uint8_t>(p, at + 0x24);
                q.ringCount    = static_cast<uint8_t>(std::min(rd<uint8_t>(p, at + 0x25) + 2, 4));
                break;
            case 0x43: // the blend of the ground-projected pass of class 0x200000 meshes (vfunc +0x38 -> p+0x198, CODE
                if (q.flags & 0x200000) // 0x10040da0 draws it with 0x10046710); xi-client draws one pass, with this blend
                {
                    q.blend = rd<uint16_t>(p, at + 4);
                }
                break;
            case 0x47: // a child particle takes its parent particle's rotation (CODE 0x1004fc92), adds it (0x48), or takes
                if (g.child) // its scale (0x49)
                {
                    q.rot = g.parentRot;
                }
                break;
            case 0x48: // the parent particle's colour (CODE 0x1004fd87: p+0xf8; adding the rotation is L1 0x79)
                if (const auto* pq = parent())
                {
                    q.rgba = pq->rgba;
                }
                break;
            case 0x49:
                if (g.child)
                {
                    q.scale = g.parentScale;
                }
                break;
            case 0x6b: // sound particle (class 0xe, CODE 0x100500f9): rec+8 the handle of a resource (name rec+4) whose
                       // position (+ the camera offset) becomes the particle's: an ambience sound on a moving thing (DATA:
                       // `se04` on `kiji`, the pheasants). INFERRED: the resource is a generator of the same file.
                g.soundAt = rd<uint32_t>(p, at + 4);
                break;
            case 0x4c: // sound particle (class 0xe, CODE 0x10050201): rec+8 the full-volume distance (p+0x1b0), rec+4 the
                       // silent one (p+0x1b4; CODE 0x10036ba0 compares far first), volume 0x7f; a world-placed generator
                       // (origin mode 0) measures plain distance (p+0x1e9 = 1, CODE 0x1005023d)
                g.soundNear = rd<float>(p, at + 8);
                g.soundFar  = rd<float>(p, at + 4);
                if (((((g.origin >> 16) & 1) << 4) | (g.origin & 0xF)) == 0)
                {
                    q.soundMode = 1;
                }
                break;
            case 0x92: // sound particle distance: plain (p+0x1e9 = 1, CODE 0x100501c1) or across the ground only (0x9e: 2,
            case 0x9e: // CODE 0x100501e1); read by 0x10036ba0 (mode 0: the vertical part x3)
                if (g.sound)
                {
                    q.soundMode = op == 0x92 ? 1 : 2;
                }
                break;
            case 0x8a: // sound particle: out of range it keeps playing at volume 1 (p+0x1e8, CODE 0x100501a4 / 0x10036cc9)
                q.soundKeepFar = g.sound != nullptr;
                break;
            case 0x55: // mesh class 0x1c0 (header flags bit 24): a second texture (TexInfo rec+0x10) environment-mapped over
                if (q.flags & 0x01000000) // the mesh, colour rec+0x20 (CODE 0x10051765, draw 0x100411e0)
                {
                    const dat::FxName tex = rd<uint32_t>(p, at + 0x10);
                    q.overlay             = g.file ? g.file->textureByChunk(tex) : nullptr;
                    if (const auto* f0 = q.overlay ? nullptr : file(0))
                    {
                        q.overlay = f0->textureByChunk(tex);
                    }
                    for (std::size_t c = 0; c < 4; ++c)
                    {
                        q.overlayRgba[c] = rd<uint8_t>(p, at + 0x20 + c);
                    }
                    q.overlayAngles = rdVec(p, at + 4);
                }
                break;
            case 0x59: // keyframe slots for the overlay's texture-matrix angles (L2 0x35-0x37)
            case 0x5a:
            case 0x5b:
            case 0x68: // keyframe slot for a sound particle's volume (L2 0x43)
            case 0x82: // keyframe slot for the vibration strength (L2 0x5f): {curve, mode, strength}
                if (slot + 3 <= q.vars.size())
                {
                    curve(g, at, rd<uint32_t>(p, at + 8));
                    q.vars[slot]     = static_cast<uint32_t>(at);
                    q.vars[slot + 1] = rd<uint32_t>(p, at + 0xC);
                    q.vars[slot + 2] = op == 0x82 ? rd<uint32_t>(p, at + 0x10) : asBits(op >= 0x59 && op <= 0x5b ? q.overlayAngles[op - 0x59] / kPi : 1.0f);
                    if (const auto* c = op == 0x68 ? curve(g, at, rd<uint32_t>(p, at + 8)) : nullptr)
                    {
                        q.volume = std::clamp(c->eval(0.0f, c->keys.front().second, false), 0.0f, 2.0f); // the sound starts at this level
                    }
                }
                break;
            case 0x4b: // the lens flare's glare colour (class 0xc only; CODE 0x1004e86a: rec+0xc R G B A -> +0x19c, read by
            {          // the flare pass 0x10054b90; rec+4 / +8 -> +0x194 / +0x198, not traced)
                const uint32_t c = rd<uint32_t>(p, at + 0xc);
                for (int k = 0; k < 4; ++k)
                {
                    glareRgba_[k] = c ? rd<uint8_t>(p, at + 0xc + k) : 0x80;
                }
                break;
            }
            case 0x8f: // colour x the light at the generator's actor (its area's light set; CODE 0x1004f33e: R, G, B bytes x
            case 0x90: // light.xyz, truncated, below 0 -> 0, low byte kept) or of the scene (0x90: 0x10629fe4, 0x1004f4dc)
            {
                const glm::vec3 l = op == 0x8f ? actorLight_ : sceneLight_;
                for (int c = 0; c < 3; ++c)
                {
                    const float x = static_cast<float>(q.rgba[c]) * l[c];
                    q.rgba[c]     = x > 0.0f ? static_cast<uint8_t>(static_cast<int>(x) & 0xff) : 0;
                }
                break;
            }
            case 0x87: // remember the position (slot.xyz) for L2 0x64's heading (CODE 0x1004fda9)
                if (slot + 4 <= q.vars.size())
                {
                    setSlotVec(slot, q.pos);
                    q.vars[slot + 3] = 0;
                }
                break;
            case 0x3b:
            {
                const float k = static_cast<float>(index);
                q.rot         = glm::vec3(wrapPi(q.rot.x + k * v.x), wrapPi(q.rot.y + k * v.y), wrapPi(q.rot.z + k * v.z));
                break;
            }
            case 0x41:
            {
                const glm::vec3 d = q.pos - headerPos;
                const float     l = glm::length(d);
                if (l > 1e-5f)
                {
                    setSlotVec(slot, slotVec(slot) + d / l * randSym(rd<float>(p, at + 4)));
                }
                break;
            }
            // Coverage part 7C: keyframe slots read by their paired L2 op (effect-format "Coverage, part 7C").
            // {curve, mode} only (CODE 0x1005032a / 0x1005034b / 0x1005036c / 0x10050381..0x100503d4): texture-scroll and
            // spin-rate curves at the age (0x74-0x78 -> L2 0x54-0x58), scale / colour / spin curves at the time of day
            // (0x64-0x66 -> L2 0x40-0x42, 0x6d-0x70 -> 0x4a-0x4d, 0x83-0x85 -> 0x61-0x63, 0x8b-0x8d -> 0x66-0x68), the
            // point light's scale curves (0x80 / 0x81 -> 0x5d / 0x5e).
            case 0x74: case 0x75: case 0x76: case 0x77: case 0x78:
            case 0x64: case 0x65: case 0x66:
            case 0x6d: case 0x6e: case 0x6f: case 0x70:
            case 0x80: case 0x81: case 0x83: case 0x84: case 0x85:
            case 0x8b: case 0x8c: case 0x8d:
                curveSlot(at, slot);
                break;
            // {curve, mode, base}: the base replaces the curve's first key (L2 reads it back).
            case 0x95: // position X / Y / Z at the time of day (CODE 0x100503f1.., base = the position; L2 0x6b-0x6d)
            case 0x96:
            case 0x97: channel(at, slot, q.pos[op - 0x95]); break;
            case 0x39: // `dist` strength curve (CODE 0x10050af6: base = the current strength p+0x194; L2 0x24)
                channel(at, slot, q.distortion);
                break;
            case 0x5c: // overlay colour R / G / B / A curves (CODE 0x100508c3..: base = the L1 0x55 colour byte / 255;
            case 0x5d: // L2 0x38-0x3b)
            case 0x5e:
            case 0x5f: channel(at, slot, static_cast<float>(q.overlayRgba[op - 0x5c]) / 255.0f); break;
            case 0x7c: channel(at, slot, q.lightIntensity); break; // point light intensity curve, base p+0x1a0 (L2 0x5b)
            case 0x7d: channel(at, slot, q.lightRange); break;     // point light range curve, base p+0x19c (L2 0x5c)
            case 0x91: // the colour now, kept for L2 0x69's scene-light tint (CODE 0x1004f687: slot = p+0xf8)
                if (slot < q.vars.size())
                {
                    std::memcpy(&q.vars[slot], q.rgba.data(), 4);
                }
                break;
            case 0x50: // velocity X / Y / Z curves (L2 0x30-0x32) of the L2 op 2 velocity slot (CODE 0x1004e8b6..: slot
            case 0x51: // {curve, mode}; then, as the game does, that slot's +2 word = its +1 word)
            case 0x52:
                curveSlot(at, slot);
                if (const auto r2 = findRecord(g.res->payload, g.lists[2], 0x02))
                {
                    if (const auto s2 = (rd<uint32_t>(g.res->payload, *r2) >> 13) & 0x3F; s2 + 3 <= q.vars.size())
                    {
                        q.vars[s2 + 2] = q.vars[s2 + 1];
                    }
                }
                break;
            case 0x69: // {curve, mode} for L2 0x44: the drag factor of the L2 0x2c record (CODE 0x10050a57)
                curveSlot(at, slot);
                break;
            case 0x93: // {curve, mode} for L2 0x6a, which writes into the L2 0x65 record (CODE 0x10050aa0); see effect_ops.h
                curveSlot(at, slot);
                break;
            case 0x42: // onto the ground (CODE 0x1004f706): the particle's world point, a collision ray from 1 above to 6
            {          // below (0x101814a0); on a hit the point moves there (back through the inverse matrix)
                if (!groundRay_)
                {
                    break;
                }
                const glm::vec3 wp = glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f)); // game space, Y down
                if (const auto hit = groundRay_(wp - glm::vec3(0, 1.0f, 0), wp + glm::vec3(0, 6.0f, 0)))
                {
                    q.pos = glm::vec3(glm::inverse(q.spawnMatrix) * glm::vec4(*hit, 1.0f));
                }
                break;
            }
            case 0x46: // the parent's velocity (CODE 0x1004fbb5): the slot of the parent generator's first L1 0x02, x rec+4,
            {          // and slot+3 = 1
                const auto* pq = parent();
                const auto  pr = parentRecord(0x02);
                if (pq && pr && slot + 4 <= q.vars.size())
                {
                    const auto ps = (rd<uint32_t>(pr->first, pr->second) >> 13) & 0x3F;
                    const float k = rd<float>(p, at + 4);
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        q.vars[slot + c] = asBits(ps + c < pq->vars.size() ? asFloat(pq->vars[ps + c]) * k : 0.0f);
                    }
                    q.vars[slot + 3] = asBits(1.0f);
                }
                break;
            }
            case 0x4a: // the parent's texture offset (CODE 0x1004fe58: p+0x120 / +0x124)
                if (const auto* pq = parent())
                {
                    q.uv = pq->uv;
                }
                break;
            case 0x79: // rotation += the parent's (CODE 0x1004fcf3), scale += the parent's (0x7a, CODE 0x1004fd3d)
                if (const auto* pq = parent())
                {
                    q.rot += pq->rot;
                }
                break;
            case 0x7a:
                if (const auto* pq = parent())
                {
                    q.scale += pq->scale;
                }
                break;
            case 0x7e: // point lights (class 0xb): the parent's intensity (p+0x1a0, CODE 0x1004fe84) or range (0x7f: p+0x19c,
            case 0x7f: // CODE 0x1004febc); L1 0x58 sets them
                if (const auto* pq = parent(); pq && g.light)
                {
                    (op == 0x7e ? q.lightIntensity : q.lightRange) = op == 0x7e ? pq->lightIntensity : pq->lightRange;
                }
                break;
            case 0x9d: // the parent's draw-order key (CODE 0x1004fef4: p+0x128; p+0x13c, the last view depth, too, which the
                if (const auto* pq = parent()) // draw rewrites anyway, CODE 0x100450a0)
                {
                    q.depthKey = pq->depthKey;
                }
                break;
            case 0x98: // the parent's life (CODE 0x1004f056: remaining / total while its remaining != 0) and its wobble slot
            {          // (the parent generator's first L1 0x3e: amplitude X and the heading, slot +0 / +3)
                const auto* pq = parent();
                if (!pq || pq->remaining == 0.0f)
                {
                    break;
                }
                q.remaining = pq->remaining;
                q.total     = pq->total;
                if (const auto pr = parentRecord(0x3e); pr && slot + 4 <= q.vars.size())
                {
                    const auto ps = (rd<uint32_t>(pr->first, pr->second) >> 13) & 0x3F;
                    for (const uint32_t c : { 0u, 3u })
                    {
                        q.vars[slot + c] = ps + c < pq->vars.size() ? pq->vars[ps + c] : 0u;
                    }
                }
                break;
            }
            case 0x94: // use the parent generator's Vtx (CODE 0x1004fe03: its first L1 0x4e's name into this generator's)
                if (const auto pr = parentRecord(0x4e))
                {
                    g.vtxOverride = rd<uint32_t>(pr->first, pr->second + 4);
                }
                break;
            case 0x89: // rec+4 a resource handle (CODE 0x1004e97d): 0 clears the slot; a handle would also redirect the next
                if (rd<uint32_t>(p, at + 4) == 0 && slot < q.vars.size()) // L1 0x4e / 0x4f to its points. The data's
                {                                                          // non-zero values are stale editor pointers
                    q.vars[slot] = 0;                                      // (not linked, CODE 0x10048325): ignored.
                }
                break;
            case 0x8e: // footprints (`fmrk`, CODE 0x1004f69b): the particle's matrix x the actor's ground-aligned matrix
            {          // (0x100d58d0) and its position = actor +0x63c; here the actor's position and facing (the slope tilt
                       // and which stored position +0x63c is are INFERRED / not done)
                const auto a = actors_ && *actors_ ? (*actors_)(g.caster) : std::nullopt;
                if (a)
                {
                    q.spawnMatrix = glm::translate(glm::mat4(1.0f), a->pos) * yawMatrix(a->yaw);
                    q.pos         = glm::vec3(0.0f);
                }
                break;
            }
            // Read elsewhere or by nothing: L1 0x7b the attach offsets (place()); 0x9b places a child at its parent
            // particle (CODE 0x1004ff20), which every child here already is; 0x88 / 0x9c (p+0x160 a light generator's
            // name / the parent's) and 0x57 (class 0x23 `vlin`, whose draw is empty, CODE 0x100640b0) have no reader.
            default: break;
        }
        at += 4 * len;
    }
    if (g.sound && sounds_.size() < 32)
    {
        // Ambience at the particle's spawn point; the audible range is INFERRED (no record sets it here).
        glm::vec3 where = glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f));
        if (g.soundAt != 0)
        {
            for (const auto& o : generators_)
            {
                if (o->res && o->res->name == g.soundAt && o->file == g.file && !o->particles.empty())
                {
                    const auto& op = o->particles.front();
                    where          = glm::vec3(op.spawnMatrix * glm::vec4(op.pos, 1.0f));
                    break;
                }
            }
        }
        // 0 -> the defaults 3 / 30 (CODE 0x10036bcc: 0x103516fc / 0x103516f8).
        sounds_.push_back({ 0, g.sound->name, g.sound->payload, g.soundNear > 0 ? g.soundNear : 3.0f, g.soundFar > 0 ? g.soundFar : 30.0f,
                            where, q.volume, q.soundMode, q.soundKeepFar });
    }
    g.particles.push_back(std::move(q));
    ++g.spawned;
}

void EffectSystem::tickParticle(Generator& g, Particle& q, float dt)
{
    const auto p = g.res->payload;
    auto slotVec = [&](uint32_t slot) -> glm::vec3 {
        if (slot + 3 > q.vars.size())
        {
            return glm::vec3(0);
        }
        return { asFloat(q.vars[slot]), asFloat(q.vars[slot + 1]), asFloat(q.vars[slot + 2]) };
    };
    auto setSlotVec = [&](uint32_t slot, const glm::vec3& v) {
        if (slot + 3 <= q.vars.size())
        {
            q.vars[slot] = asBits(v.x), q.vars[slot + 1] = asBits(v.y), q.vars[slot + 2] = asBits(v.z);
        }
    };
    auto channel = [&](uint32_t slot, float t, bool zeroKey0) -> std::optional<float> {
        if (slot + 3 > q.vars.size())
        {
            return std::nullopt;
        }
        const auto it = g.curves.find(q.vars[slot]);
        if (it == g.curves.end() || it->second.keys.empty())
        {
            return std::nullopt;
        }
        const bool  smooth = (q.vars[slot + 1] & 0xF) != 0;
        const float base   = asFloat(q.vars[slot + 2]);
        return zeroKey0 ? base + it->second.eval(t, 0.0f, smooth) : it->second.eval(t, base, smooth);
    };
    q.dayFactor = 1.0f; // reset every particle frame (CODE 0x1004a8e3)
    // A time-of-day slot (L1 0x62 / 0x63): the curve as stored, at day fraction t.
    auto dayCurve = [&](uint32_t slot, float t) -> std::optional<float> {
        if (slot + 2 > q.vars.size())
        {
            return std::nullopt;
        }
        const auto it = g.curves.find(q.vars[slot]);
        if (it == g.curves.end() || it->second.keys.empty())
        {
            return std::nullopt;
        }
        return it->second.eval(t, it->second.keys.front().second, (q.vars[slot + 1] & 0xF) != 0);
    };
    // A {curve, mode} slot evaluated with the curve's own first key (CODE 0x10054430 / 0x100546d0), at any t.
    const auto& storedCurve = dayCurve;
    for (std::size_t at = g.lists[2]; at + 4 <= p.size();)
    {
        const auto w    = rd<uint32_t>(p, at);
        const auto op   = w & 0xFF;
        const auto len  = (w >> 8) & 0x1F;
        const auto slot = (w >> 13) & 0x3F;
        if (op == 0 || len == 0)
        {
            break;
        }
        const glm::vec3 v = rdVec(p, at + 4);
        switch (op)
        {
            case 0x45: // the moon: its frame from the phase, rolled a quarter turn (CODE 0x1018bcf0)
                q.frame = moonFrame_;
                q.rot.z = kPi / 2;
                break;
            case 0x4e: // colour tables by the day (CODE 0x1004dce6 / 0x1004db1b; record + 8 = n RGBA entries): 0x4e sets the
            case 0x4f: // colour to entry[weekday] (8), 0x4f multiplies it by entry[moon phase] (12) / 128. In the day's first
            {          // Vana'diel hour the entry fades in from the previous one (t = seconds of the day / 3600).
                const uint32_t n   = op == 0x4e ? 8u : 12u;
                const uint32_t cur = op == 0x4e ? weekday_ : moonFrame_;
                if (at + 8 + 4 * n > p.size())
                {
                    break;
                }
                const uint32_t prev = (cur + n - 1) % n;
                const float    secs = dayFraction_ * 86400.0f;
                const float    t    = secs < 3600.0f ? secs / 3600.0f : 1.0f;
                for (std::size_t c = 0; c < 4; ++c)
                {
                    const float a0 = rd<uint8_t>(p, at + 8 + 4 * prev + c), a1 = rd<uint8_t>(p, at + 8 + 4 * cur + c);
                    const float mix = a0 + (a1 - a0) * t;
                    q.rgba[c] = op == 0x4e ? static_cast<uint8_t>(std::clamp(mix, 0.0f, 255.0f))
                                           : static_cast<uint8_t>(std::clamp(q.rgba[c] * mix / 128.0f, 0.0f, 255.0f));
                }
                break;
            }
            case 0x34: // along the Vtx path (CODE 0x1004b389): u = the keyframe at the age (wrapped) or the age; pos moves by
                if (q.path) // path(u) - the previous point
                {
                    float u = q.total > 0 ? 1.0f - q.remaining / q.total : 0.0f;
                    if (const auto it = g.curves.find(q.pathCurve); q.pathCurve && it != g.curves.end() && !it->second.keys.empty())
                    {
                        u = std::fmod(it->second.eval(u, it->second.keys.front().second, false), 1.0f);
                    }
                    const glm::vec3 pt = pathPoint(*q.path, std::clamp(u, 0.0f, 1.0f));
                    // Position only: the game writes no rotation here, and nothing else turns a path particle toward
                    // its path (every write of rot.y, p+0xe4, is L2 0x13 / 0x57 / 0x62 / 0x64 / 0x67 or L1 0x0a / 0x3b /
                    // 0x79). Selbina's gulls keep their spawn heading and only bank (L2 0x64 on X).
                    q.pos += pt - q.pathPrev;
                    q.pathPrev = pt;
                }
                break;
            case 0x1e: // D3b morph weight k = op - 0x1e from its channel's curve (CODE 0x1003faa0 builder): t = fmod(n x age,
            case 0x1f: // 1) with n = mode bits 5..13 (the gull: 50 flaps a life), else the particle's age (INFERRED)
            case 0x20:
            case 0x21:
            case 0x22:
            {
                const float age = q.total > 0 ? 1.0f - q.remaining / q.total : 0.0f;
                const auto  n   = slot + 1 < q.vars.size() ? (q.vars[slot + 1] >> 5) & 0x1FF : 0u;
                const float t   = n != 0 ? std::fmod(age * static_cast<float>(n), 1.0f) : age;
                if (const auto c = dayCurve(slot, t))
                {
                    q.morph[op - 0x1e] = *c;
                }
                break;
            }
            case 0x49: // point-light intensity from a time-of-day curve (CODE 0x1004d426; `ktpl`: 8 at night, 0 by day)
                if (const auto c = dayCurve(slot, dayFraction_))
                {
                    q.lightIntensity = *c;
                }
                break;
            case 0x2e: // fade by distance (CODE 0x1004af82, the distance from vfunc +0x44 = 0x10041970; INFERRED the camera
            case 0x48: // distance): 0x2e: full within rec+4, fading to nothing at rec+8; 0x48 (same handler): a band, nothing
            case 0x6e: // below rec+4, fading in to rec+8, full to rec+0xC, fading out to rec+0x10. Gone: with the kill bit
            {          // (0x2e rec+0xC, 0x48 rec+0x14 bit 0) the particle ends (remaining = 1). 0x6e (CODE 0x1004b0b1): the
                       // 0x48 band's weight w into D3b morph weights 0 / 1 = w / 1 - w (class 0xf only: lods by distance)
                const float d = glm::distance(camera_, glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f)));
                const auto  r = [&](std::size_t off) { return rd<float>(p, at + off); };
                if (d == 0.0f)
                {
                    q.dayFactor = op == 0x6e ? q.dayFactor : 0.0f;
                    break;
                }
                float k    = 1.0f;
                bool  gone = false;
                if (op == 0x2e)
                {
                    gone = d <= 0.0f || d > r(8);
                    k    = gone ? 0.0f : d >= r(4) ? 1.0f - (d - r(4)) / (r(8) - r(4)) : 1.0f;
                }
                else
                {
                    gone = d < r(4) || d >= r(0x10);
                    k    = gone ? 0.0f : d < r(8) ? 1.0f - (d - r(8)) / (r(4) - r(8)) : d < r(0xC) ? 1.0f : 1.0f - (d - r(0xC)) / (r(0x10) - r(0xC));
                }
                if (op == 0x6e)
                {
                    if (g.resType == 0x1d && !(q.flags & 0x100000)) // the D3b morph class 0x1003faa0
                    {
                        q.morph[0] = k, q.morph[1] = 1.0f - k;
                    }
                    break;
                }
                q.dayFactor *= k;
                if (gone && (rd<uint8_t>(p, at + (op == 0x2e ? 0xC : 0x14)) & 1) && q.total > 0)
                {
                    q.remaining = std::min(q.remaining, 1.0f);
                }
                break;
            }
            case 0x2f: // head the velocity along the particle's rotation (CODE 0x1004c3bf): slot = R(rotation) (|slot|, 0, 0)
            {
                const float speed = glm::length(slotVec(slot));
                setSlotVec(slot, glm::vec3(eulerMatrix(q.rot) * glm::vec4(speed, 0.0f, 0.0f, 0.0f)));
                break;
            }
            case 0x3c: // red / green / blue byte from a time-of-day curve (CODE 0x1004d3f2 and siblings; the water's
            case 0x3d: // colour through the day: Port Bastok `col1` rtms / gtms / btms)
            case 0x3e:
                if (const auto c = dayCurve(slot, dayFraction_))
                {
                    q.rgba[op - 0x3c] = static_cast<uint8_t>(std::clamp(*c * 255.0f, 0.0f, 255.0f));
                }
                break;
            case 0x3f: // alpha factor from a time-of-day curve (CODE 0x1004d51f): lamps lit at night, the sea's shading
            {
                float      t = dayFraction_;
                const auto n = slot + 1 < q.vars.size() ? (q.vars[slot + 1] >> 5) & 0x1FF : 0u;
                if (n != 0)
                {
                    t = std::fmod(t * static_cast<float>(n), 1.0f);
                }
                if (const auto c = dayCurve(slot, t))
                {
                    q.dayFactor *= *c;
                }
                break;
            }
            case 0x02: q.pos += slotVec(slot) * dt; break;
            case 0x03:
                if (!(g.flags & 0x20000000))
                {
                    setSlotVec(slot, slotVec(slot) + v * dt);
                }
                break;
            case 0x05: q.rot += slotVec(slot) * dt, q.rot = glm::vec3(wrapPi(q.rot.x), wrapPi(q.rot.y), wrapPi(q.rot.z)); break;
            case 0x06:
            case 0x09: setSlotVec(slot, slotVec(slot) + v * dt); break;
            case 0x08: q.scale += slotVec(slot) * dt; break;
            case 0x0b: // colour velocity
                if (slot + 2 <= q.vars.size())
                {
                    for (std::size_t c = 0; c < 4; ++c)
                    {
                        int16_t vel;
                        std::memcpy(&vel, reinterpret_cast<const uint8_t*>(&q.vars[slot]) + 2 * c, 2);
                        const int v16 = std::clamp(static_cast<int>(q.rgba[c]) * 256 + static_cast<int>(vel * dt), 0, 0xFFFF);
                        q.rgba[c]     = static_cast<uint8_t>(v16 >> 8);
                    }
                }
                break;
            case 0x0d: // flipbook
                if (q.frameLength > 0)
                {
                    q.frameTimer -= dt;
                    while (q.frameTimer < 0)
                    {
                        q.frameTimer += q.frameLength;
                        ++q.frame;
                    }
                }
                break;
            case 0x0e: q.age = q.total > 0 ? 1.0f - q.remaining / q.total : 0.0f; break;
            case 0x0f:
            case 0x10:
            case 0x11:
                if (const auto c = channel(slot, q.age, true))
                {
                    q.pos[op - 0x0f] = *c;
                }
                break;
            case 0x12:
            case 0x13:
            case 0x14:
                if (const auto c = channel(slot, q.age, false))
                {
                    q.rot[op - 0x12] = wrapPi(std::fmod(*c, 2.0f) * kPi);
                }
                break;
            case 0x15:
            case 0x16:
            case 0x17:
                if (const auto c = channel(slot, q.age, false))
                {
                    q.scale[op - 0x15] = *c;
                }
                break;
            case 0x18:
            case 0x19:
            case 0x1a:
            case 0x1b:
                if (const auto c = channel(slot, q.age, false))
                {
                    q.rgba[op - 0x18] = static_cast<uint8_t>(std::clamp(*c * 255.0f, 0.0f, 255.0f));
                }
                break;
            case 0x1c:
            case 0x1d:
                if (const auto c = channel(slot, q.age, false))
                {
                    q.uv[op - 0x1c] = *c;
                }
                break;
            case 0x27: q.uv.x += rd<float>(p, at + 4) * dt; break;
            case 0x28: q.uv.y += rd<float>(p, at + 4) * dt; break;
            case 0x2c: setSlotVec(slot, slotVec(slot) * std::pow(q.drag ? *q.drag : rd<float>(p, at + 4), dt)); break;
            case 0x26: // turn the slot's vector by Euler(rec.xyz) every frame (CODE 0x1004c485; not scaled by dt)
                setSlotVec(slot, glm::vec3(eulerMatrix(v) * glm::vec4(slotVec(slot), 0)));
                break;
            case 0x29: // sine wobble (CODE 0x1004c507..0x1004c5f6): w = sin(ticks lived * rec+4 / 60 + rec+8) * amplitude * dt,
            case 0x2a: // along the L1 0x3d heading a in X / Z (0x29: x += cos a w, z += sin a w; 0x2b: z += cos a w,
            case 0x2b: // x += sin a w) or straight in Y (0x2a)
                if (slot + 4 <= q.vars.size())
                {
                    const float wob = std::sin((q.total - q.remaining) * rd<float>(p, at + 4) / 60.0f + rd<float>(p, at + 8)) * asFloat(q.vars[slot + (op - 0x29)]) * dt;
                    const float a = asFloat(q.vars[slot + 3]);
                    if (op == 0x29)
                    {
                        q.pos += glm::vec3(std::cos(a) * wob, 0, std::sin(a) * wob);
                    }
                    else if (op == 0x2a)
                    {
                        q.pos.y += wob;
                    }
                    else
                    {
                        q.pos += glm::vec3(std::sin(a) * wob, 0, std::cos(a) * wob);
                    }
                }
                break;
            case 0x35: // the overlay's texture-matrix angles from their keyframes (CODE 0x1004bccd: p+0x19c..0x1a4)
            case 0x36:
            case 0x37:
                if (const auto c = channel(slot, q.age, false))
                {
                    q.overlayAngles[op - 0x35] = wrapPi(std::fmod(*c, 2.0f) * kPi);
                }
                break;
            case 0x43: // a sound particle's volume from the L1 0x68 curve (CODE 0x1004d5ab -> p+0x1be)
                if (const auto it = slot < q.vars.size() ? g.curves.find(q.vars[slot]) : g.curves.end(); it != g.curves.end() && !it->second.keys.empty())
                {
                    q.volume = std::clamp(it->second.eval(q.age, it->second.keys.front().second, false), 0.0f, 2.0f);
                }
                break;
            case 0x53: // visibility (CODE 0x1004aee4): a sphere of size rec+4 at the particle through the view; rec+8 == 0:
            {          // hidden (alpha 0) when the zone's visibility query (0x1004a7a0) says unseen; else alpha x the seen
                       // fraction (0x1004e560) x rec+8. Here: the frustum plus the collision ray from the camera.
                const glm::vec3 wp   = glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f));
                const glm::vec4 c    = viewProj_ * glm::vec4(wp, 1.0f);
                const float     r    = rd<float>(p, at + 4);
                const bool      seen = !haveViewProj_ || (c.w > -r && std::abs(c.x) <= c.w + r && std::abs(c.y) <= c.w + r &&
                                                     (!sightTest_ || sightTest_(wp)));
                const float     k    = len >= 3 ? rd<float>(p, at + 8) : 0.0f;
                q.dayFactor *= !seen ? 0.0f : k != 0.0f ? k : 1.0f;
                break;
            }
            case 0x59: // spin by view depth (CODE 0x1004aeb5): rotation z = fmod(key x rec+4 + rec+8, 2 pi) - pi, key = the
            case 0x5a: // draw bias - view depth (p+0x12c, CODE 0x10045068); 0x5a (CODE 0x1004ae46): scale = rec+4..0xC, plus
            {          // (key - rec+0x10) x rec+0x14 on each axis when key > rec+0x10 (lamp glows' size by distance)
                const glm::vec4 c   = viewProj_ * glm::vec4(glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f)), 1.0f);
                const float     key = q.depthKey - (haveViewProj_ ? c.w : 0.0f);
                if (op == 0x59)
                {
                    q.rot.z = std::fmod(key * rd<float>(p, at + 4) + rd<float>(p, at + 8), 6.2831840515f) - 3.1415920258f;
                }
                else if (len >= 6)
                {
                    const float past = key - rd<float>(p, at + 0x10);
                    q.scale          = rdVec(p, at + 4) + glm::vec3(past > 0.0f ? past * rd<float>(p, at + 0x14) : 0.0f);
                }
                break;
            }
            case 0x50: // alpha x the weather cross-fade weight of the YmCombineWeather owner (CODE 0x1004da69 -> p+0x138):
                break; // xi-client swaps weather effects at once, so the weight is 1
            case 0x5f: // vibration (CODE 0x1004c009): strength = curve x slot+2 x falloff (full within rec+4 of the camera,
            {          // none beyond rec+8) x 255 into the pad's rumble; with rec+0xC the camera also shakes by it x rec+0xC
                const float d   = glm::distance(camera_, glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f)));
                const float n   = rd<float>(p, at + 4), f = rd<float>(p, at + 8);
                const float k   = d <= n ? 1.0f : d >= f || f <= n ? 0.0f : 1.0f - (d - n) / (f - n);
                const float amp = len >= 4 ? rd<float>(p, at + 0xC) : 0.0f;
                if (k > 0 && amp != 0 && slot + 3 <= q.vars.size())
                {
                    if (const auto it = g.curves.find(q.vars[slot]); it != g.curves.end() && !it->second.keys.empty())
                    {
                        const float c = it->second.eval(q.age, it->second.keys.front().second, (q.vars[slot + 1] & 0xF) != 0);
                        shake_ += std::max(0.0f, c * asFloat(q.vars[slot + 2]) * k) * amp;
                    }
                }
                break;
            }
            case 0x60: // screen flash (CODE 0x1004ab03 -> 0x10014f50): the particle colour x a camera-distance falloff (full
            {          // within rec+4, none beyond rec+8) added into one colour for the frame
                const float d = glm::distance(camera_, glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f)));
                const float n = rd<float>(p, at + 4), f = rd<float>(p, at + 8);
                const float k = d <= n ? 1.0f : d >= f || f <= n ? 0.0f : 1.0f - (d - n) / (f - n);
                if (k > 0)
                {
                    flash_ = glm::min(flash_ + glm::vec3(q.rgba[0], q.rgba[1], q.rgba[2]) / 255.0f * (static_cast<float>(q.rgba[3]) / 128.0f) * k, glm::vec3(1.0f));
                }
                break;
            }
            // bank into turns (CODE 0x1004b179): turn = atan2(d.z, d.x) - atan2(last.z, last.x) in game space, d = pos -
            // slot.xyz (the last position), last = slot+3..5 (the last d); rotation X / Y / Z = wrap(turn x rec+4 / +8 /
            // +0xC) for the non-zero factors, then slot = pos, last = d. The game runs this once a frame of 2 ticks (its
            // 30 fps), so the turn is scaled to a 2-tick step (INFERRED). A frame without movement keeps the last
            // direction (the game would store a zero and spike next frame; its gulls never stop).
            case 0x64:
                if (slot + 6 <= q.vars.size())
                {
                    const glm::vec3 prev = slotVec(slot);
                    const glm::vec3 d    = q.pos - prev;
                    const glm::vec3 last(asFloat(q.vars[slot + 3]), asFloat(q.vars[slot + 4]), asFloat(q.vars[slot + 5]));
                    if (glm::length(glm::vec2(d.x, d.z)) > 1e-5f)
                    {
                        float turn = 0;
                        if (glm::length(glm::vec2(last.x, last.z)) > 1e-5f)
                        {
                            turn = wrapPi(std::atan2(d.z, d.x) - std::atan2(last.z, last.x));
                            if (dt > 0)
                            {
                                turn *= 2.0f / dt;
                            }
                        }
                        for (int k = 0; k < 3; ++k)
                        {
                            if (const float m = rd<float>(p, at + 4 + 4 * static_cast<std::size_t>(k)); m != 0)
                            {
                                q.rot[k] = wrapPi(turn * m);
                            }
                        }
                        q.vars[slot + 3] = asBits(d.x), q.vars[slot + 4] = asBits(d.y), q.vars[slot + 5] = asBits(d.z);
                    }
                    setSlotVec(slot, q.pos);
                }
                break;
            case 0x0c: // colour-velocity acceleration: slot s16 x 4 += rec s16 * dt (§G.10; the game's dt2 taken as dt)
                if (slot + 2 <= q.vars.size())
                {
                    for (std::size_t c = 0; c < 4; ++c)
                    {
                        auto&      word = q.vars[slot + c / 2];
                        const auto cur  = static_cast<int16_t>(word >> (16 * (c % 2)));
                        const auto nv   = static_cast<uint16_t>(cur + static_cast<int16_t>(std::lround(rd<int16_t>(p, at + 4 + 2 * c) * dt)));
                        word            = (word & ~(0xFFFFu << (16 * (c % 2)))) | (static_cast<uint32_t>(nv) << (16 * (c % 2)));
                    }
                }
                break;
            // Coverage part 7C (effect-format "Coverage, part 7C"): the L2 halves of the paired keyframe slots.
            case 0x69: // colour = the colour L1 0x91 kept x the scene light, every frame (CODE 0x1004a958: R, G, B bytes x
                if (slot < q.vars.size()) // light.xyz, truncated, below 0 -> 0; alpha kept)
                {
                    std::array<uint8_t, 4> kept{};
                    std::memcpy(kept.data(), &q.vars[slot], 4);
                    for (int c = 0; c < 3; ++c)
                    {
                        q.rgba[c] = fxByte(static_cast<float>(kept[c]) * sceneLight_[c]);
                    }
                }
                break;
            case 0x54: // texture scroll U / V by a rate curve at the age: uv = fmod(uv + dt x curve, 2) (CODE 0x1004c88a /
            case 0x55: // 0x1004c90d)
                if (const auto c = storedCurve(slot, q.age))
                {
                    q.uv[op - 0x54] = std::fmod(q.uv[op - 0x54] + dt * *c, 2.0f);
                }
                break;
            case 0x56: // spin X / Y / Z by a rate curve (half turns per tick) at the age (CODE 0x1004c99b..0x1004cab5)
            case 0x57:
            case 0x58:
                if (const auto c = storedCurve(slot, q.age))
                {
                    q.rot[op - 0x56] = wrapPi(q.rot[op - 0x56] + dt * halfTurns(*c));
                }
                break;
            case 0x61: // the same with the curve at the time of day (CODE 0x1004cb42..0x1004cc60: DAT_10629ff4)
            case 0x62:
            case 0x63:
                if (const auto c = storedCurve(slot, dayFraction_))
                {
                    q.rot[op - 0x61] = wrapPi(q.rot[op - 0x61] + dt * halfTurns(*c));
                }
                break;
            case 0x66: // rotation X / Y / Z = the curve at the time of day, in half turns (CODE 0x1004cca4..0x1004cd83)
            case 0x67:
            case 0x68:
                if (const auto c = storedCurve(slot, dayFraction_))
                {
                    q.rot[op - 0x66] = halfTurns(*c);
                }
                break;
            case 0x40: // scale X / Y / Z = the curve at the time of day (CODE 0x1004d1af..0x1004d2b6)
            case 0x41:
            case 0x42:
                if (const auto c = storedCurve(slot, dayFraction_))
                {
                    q.scale[op - 0x40] = *c;
                }
                break;
            case 0x4a: // R / G / B / A byte = the curve at the time of day x 255 (CODE 0x1004d44f..0x1004d4eb)
            case 0x4b:
            case 0x4c:
            case 0x4d:
                if (const auto c = storedCurve(slot, dayFraction_))
                {
                    q.rgba[op - 0x4a] = fxByte(*c * 255.0f);
                }
                break;
            case 0x6b: // position X / Y / Z = base + the curve at the time of day, key 0 as 0 (CODE 0x1004b746..0x1004b83a)
            case 0x6c:
            case 0x6d:
                if (const auto c = channel(slot, dayFraction_, true))
                {
                    q.pos[op - 0x6b] = *c;
                }
                break;
            case 0x24: // `dist` strength = the curve at the age, key 0 = the L1 0x39 base (CODE 0x1004c635 -> vfunc +0x34)
                if (const auto c = channel(slot, q.age, false))
                {
                    q.distortion = *c;
                }
                break;
            case 0x38: // overlay colour R / G / B / A byte (p+0x1ac..) from its curve at the age (CODE 0x1004bd30..0x1004be17)
            case 0x39:
            case 0x3a:
            case 0x3b:
                if (const auto c = channel(slot, q.age, false))
                {
                    q.overlayRgba[op - 0x38] = fxByte(*c * 255.0f);
                }
                break;
            case 0x5b: // point light intensity (p+0x1a0) / range (p+0x19c) from the curve at the age (CODE 0x1004c6c3 /
            case 0x5c: // 0x1004c728)
                if (const auto c = channel(slot, q.age, false))
                {
                    (op == 0x5b ? q.lightIntensity : q.lightRange) = *c;
                }
                break;
            case 0x5d: // point light intensity scale (p+0x1a4) / range scale (p+0x1a8) from the curve (CODE 0x1004be64 /
            case 0x5e: // 0x1004beb3)
                if (const auto c = storedCurve(slot, q.age))
                {
                    (op == 0x5d ? q.lightIntensityScale : q.lightRangeScale) = *c;
                }
                break;
            case 0x30: // velocity X / Y / Z (the L2 op 2 slot) = base + the curve at the age, key 0 = base (CODE 0x1004c24c..
            case 0x31: // 0x1004c336); nothing without an L2 op 2
            case 0x32:
                if (const auto r2 = findRecord(p, g.lists[2], 0x02))
                {
                    const auto s2 = (rd<uint32_t>(p, *r2) >> 13) & 0x3F;
                    if (const auto c = channel(slot, q.age, false); c && s2 + 3 <= q.vars.size())
                    {
                        q.vars[s2 + (op - 0x30)] = asBits(asFloat(q.vars[slot + 2]) + *c);
                    }
                }
                break;
            case 0x44: // the drag factor of the L2 0x2c record = the curve at the age (CODE 0x1004bf0f: record + 4)
                if (findRecord(p, g.lists[2], 0x2c))
                {
                    if (const auto c = storedCurve(slot, q.age))
                    {
                        q.drag = *c;
                    }
                }
                break;
            default: break;
        }
        at += 4 * len;
    }
}

std::vector<FxPointLight> EffectSystem::pointLights() const
{
    std::vector<FxPointLight> out;
    for (const auto& g : generators_)
    {
        if (!g->light)
        {
            continue;
        }
        for (const auto& q : g->particles)
        {
            // intensity = max(I, 0) x scale x the frame factor; the light table keeps colour / 128 (CODE 0x10178440).
            const float     i   = std::max(q.lightIntensity, 0.0f) * q.lightIntensityScale * std::max(q.dayFactor, 0.0f);
            const glm::vec3 rgb = glm::vec3(q.rgba[0], q.rgba[1], q.rgba[2]) / 128.0f;
            out.push_back({ g->res->name, glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f)), q.lightRange * q.lightRangeScale, rgb * i });
        }
    }
    return out;
}

std::string EffectSystem::describeGenerators() const
{
    std::string out;
    for (const auto& g : generators_)
    {
        out += fmt::format("{}{} ({}{}{}, {} particles, {:.0f} / {:.0f} ticks)", out.empty() ? "" : "; ", g->res ? dat::fxNameString(g->res->name) : "?",
                           g->stopped ? "stopped" : "running", g->child ? ", child" : "", g->model ? ", model" : "", g->particles.size(), g->elapsed,
                           g->duration);
    }
    return out;
}

glm::vec4 EffectSystem::actorTint(uint32_t id) const
{
    const auto a = tints_.find(id);
    return a == tints_.end() ? glm::vec4(1.0f) : a->second.now();
}

void EffectSystem::update(float seconds, const FxActorLookup& actorsIn)
{
    const float ticks = std::min(seconds, 0.25f) * kTicksPerSecond;
    if (ticks <= 0)
    {
        return;
    }
    // Dolls (ops 0x15 / 0x16 / 0x22 / 0x23) resolve to their snapshot; everything else to the caller's actors. A
    // following doll (0x22 / 0x23 / 0x9D) takes the source's current position and moves its attach points by as much
    // (CODE 0x100ac020: x, y, z each by flag; the pose and rotation stay as copied).
    const FxActorLookup actors = [this, &actorsIn](uint32_t id) -> std::optional<FxActor> {
        if (const auto it = dolls_.find(id); it != dolls_.end())
        {
            FxActor   a = it->second.snapshot;
            glm::vec3 shift(0.0f);
            if (it->second.follows && actorsIn)
            {
                if (const auto src = actorsIn(it->second.source))
                {
                    shift = src->pos - a.pos;
                    a.pos = src->pos;
                }
            }
            a.point = [pts = it->second.points, shift](uint32_t i) -> std::optional<glm::vec3> {
                if (i >= pts.size() || !pts[i])
                {
                    return std::nullopt;
                }
                return *pts[i] + shift;
            };
            return a;
        }
        return actorsIn ? actorsIn(id) : std::nullopt;
    };
    for (auto it = dolls_.begin(); it != dolls_.end();) // a doll outlives its removal op while effects still use it
    {
        it->second.age += ticks;
        it = it->second.age > 600 ? dolls_.erase(it) : std::next(it);
    }
    if (shot_)
    {
        shot_->elapsed = std::min(shot_->elapsed + ticks, shot_->duration);
    }
    screen_.elapsed = std::min(screen_.elapsed + ticks, screen_.duration);
    for (auto& l : layers_)
    {
        l.elapsed += ticks;
    }
    std::erase_if(layers_, [](const Layer& l) { return l.elapsed >= l.duration; });
    if (auto& sc = screenColour_; sc.remaining > 0)
    {
        // CODE 0x10060260: remaining -= dt; colour = target + (start - target) x frac (start -> target) and z = start +
        // (target - start) x frac (the game's order: target first, easing back toward the start), frac = remaining /
        // duration; at the end colour and z become the target.
        sc.remaining -= ticks;
        const float f = sc.remaining > 0 ? sc.remaining / sc.duration : 0.0f;
        sc.colour     = sc.remaining > 0 ? sc.to + (sc.from - sc.to) * f : sc.to;
        sc.z          = sc.remaining > 0 ? sc.zStart + (sc.zTarget - sc.zStart) * f : sc.zTarget;
    }
    flash_ = glm::vec3(0.0f); // L2 0x60 adds into these during the particle pass
    shake_ = 0.0f;
    for (auto* m : { &tints_, &tints2_ })
    {
        for (auto it = m->begin(); it != m->end();)
        {
            it->second.elapsed += ticks;
            const bool done = it->second.elapsed >= it->second.duration && it->second.to == glm::vec4(1.0f);
            it = done ? m->erase(it) : std::next(it);
        }
    }
    // Schedulers (new ones started while stepping are stepped from the next frame).
    const std::size_t n = schedulers_.size();
    std::vector<bool> keep(n, true);
    actors_ = &actors;
    update7b(ticks);
    for (std::size_t i = 0; i < n; ++i)
    {
        keep[i] = !schedulers_[i].dead && stepScheduler(schedulers_[i], ticks);
    }
    std::vector<Scheduler> next;
    for (std::size_t i = 0; i < schedulers_.size(); ++i)
    {
        if ((i >= n || keep[i]) && !schedulers_[i].dead)
        {
            next.push_back(schedulers_[i]);
        }
    }
    schedulers_ = std::move(next);
    // Started this frame (by play() or by the records above): they run from the next frame.
    schedulers_.insert(schedulers_.end(), started_.begin(), started_.end());
    started_.clear();
    tickActorState(ticks); // knockbacks, overlays, scrolls ... (part 7A); model generators after `init`

    // Generators (§G.5).
    for (auto& gp : generators_)
    {
        auto& g = *gp;
        if (g.child)
        {
            if (g.parent)
            {
                const auto it = std::ranges::find_if(g.parent->particles, [&](const Particle& q) { return q.serial == g.parentSerial; });
                if (it != g.parent->particles.end() && it->total >= 0)
                {
                    g.matrix      = it->spawnMatrix * glm::translate(glm::mat4(1.0f), it->pos);
                    g.parentRot   = it->rot;
                    g.parentScale = it->scale;
                }
                else
                {
                    g.stopped = true; // its particle died (the game deletes it with the particle)
                }
            }
        }
        else if (!g.placed || g.follow)
        {
            place(g, actors);
        }
        const float dt = ticks;
        // L0 op 0x0a camera-distance cull (CODE): the L1 header position through the generator matrix against the camera;
        // emission pauses beyond rec+4 (0 = the global draw distance) or nearer than rec+8 (rec+0xc bit 0: a soft stop,
        // here the same pause so it resumes in range). Live particles stay. Only zone generators have a camera here.
        bool culled = false;
        if (g.zone && g.placed)
        {
            const auto p = g.res->payload;
            for (std::size_t at = g.lists[0]; at + 4 <= p.size();)
            {
                const auto w   = rd<uint32_t>(p, at);
                const auto len = (w >> 8) & 0x1F;
                if ((w & 0xFF) == 0 || len == 0)
                {
                    break;
                }
                if ((w & 0xFF) == 0x15 && at + 0x18 <= p.size() && haveViewProj_)
                {
                    // L0 op 0x15 (CODE 0x100497ec -> 0x1004a7a0): a box, scale rec+0xC / +0x10 / +0x14 on the diagonal,
                    // translated to the generator's position (gen+0xe4 +0x14 plus its actor's), through the generator
                    // matrix (gen+0xa4) and the view; the zone object's visibility query (vtable +0x8c, 8 corners) says
                    // whether it is seen. Not seen: the generator's tick is skipped (its clock still advances, CODE
                    // 0x1004a4b2). Here: the unit cube's 8 corners against the frustum (the game's occlusion part not
                    // modelled; rec+0x18 bit 1 adds a screen-area test there, INFERRED).
                    const glm::vec3 half(rd<float>(p, at + 0xC), rd<float>(p, at + 0x10), rd<float>(p, at + 0x14));
                    const glm::vec3 c = glm::vec3(g.matrix * glm::vec4(rdVec(p, g.lists[1] + 0x14), 1.0f));
                    int outside[6] = {};
                    for (int k = 0; k < 8; ++k)
                    {
                        const glm::vec3 o((k & 1) ? 0.5f : -0.5f, (k & 2) ? 0.5f : -0.5f, (k & 4) ? 0.5f : -0.5f);
                        const glm::vec4 q = viewProj_ * glm::vec4(c + o * half, 1.0f);
                        outside[0] += q.x < -q.w, outside[1] += q.x > q.w, outside[2] += q.y < -q.w;
                        outside[3] += q.y > q.w, outside[4] += q.z < 0.0f, outside[5] += q.z > q.w;
                    }
                    culled = culled || std::ranges::any_of(outside, [](int hits) { return hits == 8; });
                }
                if ((w & 0xFF) == 0x0a && at + 0x10 <= p.size())
                {
                    const glm::vec3 pos  = glm::vec3(g.matrix * glm::vec4(rdVec(p, g.lists[1] + 0x14), 1.0f));
                    const float     d    = glm::distance(pos, camera_);
                    const float     far  = rd<float>(p, at + 4) > 0 ? rd<float>(p, at + 4) : kZoneDrawDistance;
                    culled               = d > far || d < rd<float>(p, at + 8); // (bit 0's soft stop treated the same)
                }
                at += 4 * len;
            }
        }
        // L0 op 0x04 (CODE 0x10049940): emission rate from a keyframe at the generator's age t = elapsed / duration
        // (rec+0x10 the curve's base): v particles per second -> one emission every 60 / v ticks with v / 60 extra
        // each (flags bits 0-8), the countdown cut to that; v <= 0 stops emission (countdown 32767).
        std::optional<float> rate;
        {
            const auto p = g.res->payload;
            for (std::size_t at = g.lists[0]; at + 4 <= p.size();)
            {
                const auto w   = rd<uint32_t>(p, at);
                const auto len = (w >> 8) & 0x1F;
                if ((w & 0xFF) == 0 || len == 0)
                {
                    break;
                }
                if ((w & 0xFF) == 0x04 && len >= 5)
                {
                    if (const auto* c = curve(g, at, rd<uint32_t>(p, at + 8)))
                    {
                        const float t = g.duration > 0 ? std::clamp(g.elapsed / g.duration, 0.0f, 1.0f) : 0.0f;
                        rate          = c->eval(t, rd<float>(p, at + 0x10), (rd<uint32_t>(p, at + 0xC) & 0xF) != 0);
                    }
                }
                // L0 0x17 (CODE 0x10049a04): the same emission rate from a keyframe at the time of day (0x10629ff4), the
                // curve's own first key (lamps' and birds' daily rhythm: `time`, `tims`, `wabi`).
                if ((w & 0xFF) == 0x17 && len >= 4)
                {
                    if (const auto* c = curve(g, at, rd<uint32_t>(p, at + 8)); c && !c->keys.empty())
                    {
                        rate = c->eval(dayFraction_, c->keys.front().second, (rd<uint32_t>(p, at + 0xC) & 0xF) != 0);
                    }
                }
                // L0 0x0e-0x10 (CODE 0x10049fe6 / 0x1004a166 / 0x1004a2e6): the attach matrix again (0x10053d90), then
                // turned about X / Y / Z by a = fmod(kf(t), 2) x pi (wrapped to +-pi) around its own position, t = elapsed
                // / (L1 header life + duration) up to 1. Absolute each frame, so the last such op wins.
                if ((w & 0xFF) >= 0x0e && (w & 0xFF) <= 0x10 && len >= 4)
                {
                    if (const auto* c = curve(g, at, rd<uint32_t>(p, at + 8)); c && !c->keys.empty())
                    {
                        if (!g.child)
                        {
                            place(g, actors);
                        }
                        else if (!g.parent) // a burst child: its first placement
                        {
                            if (!g.spinBase)
                            {
                                g.spinBase = g.matrix;
                            }
                            g.matrix = *g.spinBase;
                        }
                        const float life = static_cast<float>(rd<int16_t>(p, g.lists[1] + 0x22));
                        const float t    = life + g.duration > 0 ? std::min(g.elapsed / (life + g.duration), 1.0f) : 0.0f;
                        const float v    = c->eval(t, c->keys.front().second, (rd<uint32_t>(p, at + 0xC) & 0xF) != 0);
                        float       a    = static_cast<float>(std::fmod(static_cast<double>(v), 2.0)) * kPi;
                        a                = a > kPi ? a - 2 * kPi : a < -kPi ? a + 2 * kPi : a;
                        const glm::vec3 axis = (w & 0xFF) == 0x0e ? glm::vec3(1, 0, 0) : (w & 0xFF) == 0x0f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
                        glm::mat4       rot  = g.matrix;
                        rot[3]               = glm::vec4(0, 0, 0, 1);
                        g.matrix             = glm::translate(glm::mat4(1.0f), glm::vec3(g.matrix[3])) * glm::rotate(glm::mat4(1.0f), a, axis) * rot;
                    }
                }
                // L0 keyframe ops that rewrite an L1 record (§G.11; CODE 0x10049aea..0x1004a463, the record found by
                // 0x10049220 = the first L1 record with that op): 0x05 L1 0x08 +4 (radial speed); 0x06 / 0x07 L1 0x1f
                // +4 / +8 (radius random / base); 0x08 / 0x09 L1 0x1f +0x18 / +0x1c (x pi, wrapped); 0x0b-0x0d the
                // header's spawn position +0x14..+0x1c; 0x12-0x14 L1 0x02 +4..+0xc (initial velocity).
                const auto l0 = w & 0xFF;
                struct Patch
                {
                    uint32_t op, l1op, off;
                    bool     angle;
                };
                static constexpr Patch kPatches[] = { { 0x05, 0x08, 4, false },   { 0x06, 0x1f, 4, false },   { 0x07, 0x1f, 8, false },
                                                      { 0x08, 0x1f, 0x18, true }, { 0x09, 0x1f, 0x1c, true }, { 0x0b, 0x01, 0x14, false },
                                                      { 0x0c, 0x01, 0x18, false }, { 0x0d, 0x01, 0x1c, false }, { 0x12, 0x02, 4, false },
                                                      { 0x13, 0x02, 8, false },   { 0x14, 0x02, 0xc, false } };
                for (const auto& pt : kPatches)
                {
                    if (pt.op != l0 || len < 4)
                    {
                        continue;
                    }
                    const auto* c = curve(g, at, rd<uint32_t>(p, at + 8));
                    if (!c)
                    {
                        break;
                    }
                    const float t = g.duration > 0 ? std::clamp(g.elapsed / g.duration, 0.0f, 1.0f) : 0.0f;
                    float       v = c->eval(t, len >= 5 ? rd<float>(p, at + 0x10) : 0.0f, (rd<uint32_t>(p, at + 0xC) & 0xF) != 0);
                    if (pt.angle)
                    {
                        v = wrapPi(v * kPi);
                    }
                    // The first L1 record with that op.
                    for (std::size_t r = g.lists[1]; r + 4 <= p.size();)
                    {
                        const auto rw = rd<uint32_t>(p, r);
                        const auto rl = (rw >> 8) & 0x1F;
                        if ((rw & 0xFF) == 0 || rl == 0)
                        {
                            break;
                        }
                        if ((rw & 0xFF) == pt.l1op)
                        {
                            if (pt.off + 4 <= 4 * rl)
                            {
                                if (g.patched.empty())
                                {
                                    g.patched.assign(p.begin(), p.end());
                                }
                                std::memcpy(g.patched.data() + r + pt.off, &v, 4);
                            }
                            break;
                        }
                        r += 4 * rl;
                    }
                    break;
                }
                at += 4 * len;
            }
        }
        if (rate)
        {
            if (*rate <= 0.0f)
            {
                g.countdown = 32767.0f;
            }
            else
            {
                g.interval = static_cast<int16_t>(std::min(60.0f / *rate, 32767.0f));
                g.intervalRand = 0;
                g.flags        = (g.flags & ~0x1FFu) | (static_cast<uint32_t>(*rate / 60.0f) & 0x1FFu);
                g.countdown    = std::min(g.countdown, static_cast<float>(g.interval));
            }
        }
        if (g.weather && !weatherParticles_)
        {
            g.particles.clear();
            continue;
        }
        if (!g.stopped && g.placed && !culled)
        {
            if (g.singleShot)
            {
                if (g.spawned == 0)
                {
                    spawn(g, 0);
                }
            }
            else
            {
                g.countdown -= std::min(dt, 10.0f);
                while (g.countdown < 0 && (g.duration == 0 || g.elapsed < g.duration))
                {
                    const uint32_t burst = (g.flags & 0x20000000) && !g.weather ? 1 : (g.flags & 0x1FF) + 1;
                    for (uint32_t k = 0; k < burst; ++k)
                    {
                        spawn(g, g.spawned);
                    }
                    g.countdown += static_cast<float>(std::uniform_int_distribution<int>(0, std::max<int>(0, g.intervalRand - 1))(rng_) + g.interval + 1);
                }
            }
        }
        g.elapsed += dt;
        if (g.duration != 0 && g.elapsed >= g.duration)
        {
            g.stopped = true;
        }
        // Duration 0 runs until its scheduler stops it; without one, end it after 10 seconds.
        if (g.duration == 0 && g.elapsed > 600 && !g.zone && !g.model)
        {
            g.stopped = true;
        }
        for (auto& q : g.particles)
        {
            if (q.total > 0)
            {
                q.remaining -= dt;
            }
            if (q.total > 0 && q.remaining < 1)
            {
                // L3 op 5 (rebirth) keeps it; anything else lets it die (§G.11).
                bool reborn = false;
                const auto p = g.res->payload;
                for (std::size_t at = g.lists[3]; at + 4 <= p.size();)
                {
                    const auto w = rd<uint32_t>(p, at);
                    if ((w & 0xFF) == 0 || ((w >> 8) & 0x1F) == 0)
                    {
                        break;
                    }
                    if ((w & 0xFF) == 0x05 && !g.stopped)
                    {
                        q.remaining = q.total;
                        reborn      = true;
                    }
                    if ((w & 0xFF) == 0x01 && ((w >> 8) & 0x1F) >= 3) // a burst of generator rec+8 where it died (§G.11)
                    {
                        startChild(g, q, rd<uint32_t>(p, at + 8), false);
                    }
                    at += 4 * ((w >> 8) & 0x1F);
                }
                // Particle header flag bit 29 keeps it at life 0, but only while its generator lives: once that is gone
                // the particle's generator link (p+0xfc) is null and it dies at the end of its life (CODE 0x10044a10:
                // p+0x10c bit 29, else / without a generator vfunc +0x18(1)). Weather clouds keep the generator flag
                // as before. Not done: a kept particle also dies once its attached child (p+0x164) count drops below 2.
                const bool kept = !g.stopped && ((q.flags & 0x20000000) || (g.weather && (g.flags & 0x20000000)));
                if (!reborn && !kept)
                {
                    q.total = -1; // dead
                    continue;
                }
                if (!reborn)
                {
                    q.remaining = 0; // kept at life 0
                }
            }
            if (g.follow && (!(q.flags & 0x800000) || g.weather))
            {
                q.spawnMatrix = g.matrix; // sky particles (sun / moon set bit 23) always ride their camera-placed generator
            }
            tickParticle(g, q, dt);
        }
        std::erase_if(g.particles, [](const Particle& q) { return q.total < 0; });
        if (g.stopped)
        {
            // Immortal particles (life 0) go with their generator.
            std::erase_if(g.particles, [](const Particle& q) { return q.total == 0; });
        }
    }
    // Children start now (inside the pass they would move the list), placed where their particle was this frame.
    for (const auto& c : childStarts_)
    {
        if (auto* ng = startGenerator(c.file, c.res, c.caster, c.target, c.parent ? 0.0f : 1.0f, false))
        {
            ng->child        = true;
            ng->parent       = c.parent;
            ng->parentSerial = c.parentSerial;
            ng->matrix       = c.matrix;
            ng->parentRot    = c.rot;
            ng->parentScale  = c.scale;
            ng->parentSnap   = c.snap;
            ng->parentRes    = c.parentRes;
            ng->parentL1     = c.parentL1;
            ng->placed       = true;
        }
    }
    childStarts_.clear();
    for (auto& g : generators_) // children of generators about to go lose their parent (and stop next frame)
    {
        if (g->parent && g->parent->stopped && g->parent->particles.empty())
        {
            g->parent = nullptr;
            g->stopped = true;
        }
    }
    std::erase_if(generators_, [](const std::unique_ptr<Generator>& g) { return g->stopped && g->particles.empty(); });
    for (auto& t : trails_)
    {
        if (!t.placed)
        {
            // The blade samples are in the caster's model space at the moment of the swing.
            if (const auto a = actors ? actors(t.caster) : std::nullopt)
            {
                t.matrix = glm::translate(glm::mat4(1.0f), a->pos) * yawMatrix(a->yaw);
            }
            t.placed = true;
        }
        t.age += ticks;
    }
    // A trail lives until its last sample has faded out.
    std::erase_if(trails_, [](const Trail& t) {
        const float fade = std::max(std::min(t.fadeA, t.fadeB), 1e-3f);
        return t.age > t.sampleTick.back() + std::max(t.colA.a, t.colB.a) / fade + 1.0f;
    });
    actors_ = nullptr; // `actors` is local to this call
}

std::size_t EffectSystem::particleCount() const
{
    std::size_t n = 0;
    for (const auto& g : generators_)
    {
        n += g->particles.size();
    }
    return n;
}

std::vector<EffectSystem::ParticleView> EffectSystem::particles(dat::FxName generator) const
{
    std::vector<ParticleView> out;
    for (const auto& g : generators_)
    {
        if (g->res && g->res->name == generator)
        {
            for (const auto& q : g->particles)
            {
                out.push_back({ q.pos, q.rot });
            }
        }
    }
    return out;
}

void EffectSystem::startChild(Generator& g, const Particle& q, dat::FxName name, bool attached)
{
    if (childStarts_.size() >= 256)
    {
        return;
    }
    if (const auto [f, r] = lookup(g.file, g.res->block, dat::FxType::Generator, name); r && r != g.res)
    {
        childStarts_.push_back({ f, r, g.caster, g.target, attached ? &g : nullptr, q.serial, q.spawnMatrix * glm::translate(glm::mat4(1.0f), q.pos), q.rot, q.scale,
                                 attached ? nullptr : std::make_shared<const Particle>(q), g.res, g.lists[1] });
    }
}

uint32_t EffectSystem::makeDoll(uint32_t source, bool follows)
{
    const auto a = actors_ && *actors_ ? (*actors_)(source) : std::nullopt;
    if (!a)
    {
        return source; // nothing to copy: effects stay on the actor
    }
    Doll d;
    d.source   = dolls_.contains(source) ? dolls_.at(source).source : source;
    d.snapshot = *a;
    d.snapshot.point = nullptr;
    for (uint32_t i = 0; i < d.points.size(); ++i)
    {
        d.points[i] = a->point ? a->point(i) : std::nullopt;
    }
    d.follows        = follows;
    const uint32_t id = kDollIdBase | (++dollSerial_ & 0xFFFFFF);
    if (std::getenv("XI_FX_TRACE"))
    {
        spdlog::info("fx: doll {:x} of {:x} at ({:.1f}, {:.1f}, {:.1f}){}", id, d.source, a->pos.x, a->pos.y, a->pos.z, follows ? " following" : "");
    }
    dolls_[id]        = std::move(d);
    return id;
}

void EffectSystem::noteOp(uint8_t list, uint8_t op, const dat::EffectFile* f, dat::FxName name)
{
    if (fxOpStatus(static_cast<FxList>(list), op) != FxOpStatus::Unhandled)
    {
        return;
    }
    auto& u = unhandled_[{ list, op }];
    if (u.count++ == 0)
    {
        u.where = fmt::format("{}:{}", f ? f->fileId() : 0, dat::fxNameString(name));
        if (std::getenv("XI_FX_TRACE"))
        {
            spdlog::info("fx: unhandled {} op {:#04x} in {}", fxListName(static_cast<FxList>(list)), op, u.where);
        }
    }
}

std::string EffectSystem::unhandledSummary() const
{
    if (unhandled_.empty())
    {
        return "unhandled ops: none";
    }
    std::string s = "unhandled ops:";
    for (const auto& [k, u] : unhandled_)
    {
        s += fmt::format(" {} {:#04x} x{} ({})", fxListName(static_cast<FxList>(k.first)), k.second, u.count, u.where);
    }
    return s;
}

namespace
{
// XI_FX_TRACE: one flare line per body about every 5 s (300 frames).
bool flareTrace(bool moon)
{
    static const bool trace     = std::getenv("XI_FX_TRACE") != nullptr;
    static int        calls[2] = { 0, 0 };
    return trace && ++calls[moon ? 1 : 0] % 300 == 1;
}
} // namespace

void EffectSystem::buildFlares(std::vector<Batch>& out, const glm::vec3& camPos, const glm::vec3& camForward, const glm::vec3& camRight,
                               const glm::vec3& camDown, float tanHalfY, float aspect, float sunVisible, float moonVisible) const
{
    // CODE 0x10054800: the body's screen position s (not drawn behind the camera); element centre s + t * (-2 s), so
    // t = 0.5 is the screen centre; vertex x / y in 1/16 of the screen width (0x1032a9f0). INFERRED: fading by the
    // body's height over the horizon, and the screen-edge cut-off.
    const float tanHalfX = tanHalfY * aspect;
    for (const auto& fl : flares_)
    {
        const glm::vec3 dir = fl.moon ? towardMoon_ : towardSun_;
        const float     vis = (fl.moon ? moonVisible : sunVisible) * std::clamp(-dir.y * 8.0f, 0.0f, 1.0f); // game Y down
        const float     z   = glm::dot(dir, camForward);
        if (vis <= 0.0f || z <= 0.05f)
        {
            if (flareTrace(fl.moon))
            {
                spdlog::info("fx: {} flare not in view: toward ({:.2f}, {:.2f}, {:.2f}), camera forward ({:.2f}, {:.2f}, {:.2f}), visible {:.2f}",
                             fl.moon ? "moon" : "sun", dir.x, dir.y, dir.z, camForward.x, camForward.y, camForward.z, vis);
            }
        }
        if (vis <= 0.0f || z <= 0.05f)
        {
            continue;
        }
        const glm::vec2 s(glm::dot(dir, camRight) / (z * tanHalfX), glm::dot(dir, camDown) / (z * tanHalfY));
        if (std::abs(s.x) > 1.3f || std::abs(s.y) > 1.3f)
        {
            continue;
        }
        Batch b{ fl.texture, Batch::Blend::Additive, {}, -10000.0f };
        for (const auto& e : fl.flare.elements)
        {
            const glm::vec2 c = s * (1.0f - 2.0f * e.t);
            for (const auto& v : e.vertices)
            {
                const glm::vec2 n = c + glm::vec2(v.x / 8.0f, v.y * aspect / 8.0f); // 1/16 width in pixels -> NDC
                const glm::vec3 p = camPos + camForward + camRight * (n.x * tanHalfX) + camDown * (n.y * tanHalfY);
                // v as stored: the flare texture (lf01, 8 bpp) is turned top-down at decode like every non-DXT
                // texture (#79), which is what the old "v from the bottom" reading here made up for.
                b.vertices.push_back({ p, { v.u, v.v },
                                       glm::vec4(v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3] * vis) / 128.0f });
            }
        }
        out.push_back(std::move(b));
        // The glare (CODE 0x10054ab0..0x10054c86): the glare colour x k, added into the frame's screen flash, with
        // k = max(0, 1 - 2/3 |s|) x 0.5 x the visible part (the game's visible-pixel count >> 3 / 128; INFERRED: 0..1).
        const float k = std::max(0.0f, 1.0f - glm::length(s) * (2.0f / 3.0f)) * 0.5f * vis;
        if (flareTrace(fl.moon))
        {
            spdlog::info("fx: {} flare at ({:.2f}, {:.2f}), visible {:.2f}, glare {:.3f}", fl.moon ? "moon" : "sun", s.x, s.y, vis, k);
        }
        if (k > 0.0f)
        {
            const glm::vec3 rgb = glm::vec3(glareRgba_[0], glareRgba_[1], glareRgba_[2]) * k / 255.0f;
            glare_              = glm::min(glare_ + rgb * (static_cast<float>(glareRgba_[3]) * k / 128.0f), glm::vec3(1.0f));
        }
    }
}

void EffectSystem::build(std::vector<Batch>& out, const glm::vec3& camRight, const glm::vec3& camDown) const
{
    out.clear();
    auto batchFor = [&](const dat::Texture* tex, Batch::Blend blend, float key = 0.0f, bool screen = false) -> Batch& {
        for (auto& b : out)
        {
            if (b.texture == tex && b.blend == blend && b.depthKey == key && b.screen == screen)
            {
                return b;
            }
        }
        out.push_back({ tex, blend, {}, key, screen });
        return out.back();
    };
    auto blendOf = [](uint16_t mode) { return fxBlendClass(mode); };
    const glm::vec3 camBack = glm::normalize(glm::cross(camRight, camDown)); // toward the viewer (right-handed in game space)
    // Camera space (x right, y down, z ahead) to the scene's space: what the game's identity view does for
    // screen-space particles.
    glm::mat4 camSpace(1.0f);
    camSpace[0] = glm::vec4(camRight, 0);
    camSpace[1] = glm::vec4(camDown, 0);
    camSpace[2] = glm::vec4(glm::normalize(glm::cross(camRight, camDown)), 0); // ahead: right x down in game space (Y down)
    camSpace[3] = glm::vec4(camera_, 1);
    auto screenSpace = [&](const glm::mat4& local) { return camSpace * local; };
    // Weapon trails: an additive, untextured ribbon between edge A and edge B over the samples revealed so far; each
    // sample's alpha falls by the per-tick fade from when it appeared (§M.5).
    for (const auto& t : trails_)
    {
        auto& b = batchFor(nullptr, Batch::Blend::Additive);
        for (std::size_t k = 0; k + 1 < t.edgeA.size(); ++k)
        {
            if (t.age < t.sampleTick[k + 1])
            {
                break;
            }
            auto vert = [&](std::size_t i, bool edgeA) {
                const float     since = t.age - t.sampleTick[i];
                glm::vec4       c     = edgeA ? t.colA : t.colB;
                c.a                   = std::max(0.0f, c.a - (edgeA ? t.fadeA : t.fadeB) * since);
                const glm::vec3 p     = glm::vec3(t.matrix * glm::vec4(edgeA ? t.edgeA[i] : t.edgeB[i], 1.0f));
                return Vertex{ p, glm::vec2(0), glm::vec4(glm::vec3(c) * 2.0f, c.a * 2.0f) };
            };
            const Vertex a0 = vert(k, true), b0 = vert(k, false), a1 = vert(k + 1, true), b1 = vert(k + 1, false);
            b.vertices.insert(b.vertices.end(), { a0, b0, a1, a1, b0, b1 });
        }
    }
    // §G.12: local = position · rotation · scale; world = generator matrix · local, or a camera-facing matrix.
    auto worldOf = [&](const Particle& q) {
        const glm::mat4 local   = glm::translate(glm::mat4(1.0f), q.pos) * eulerMatrix(q.rot) * glm::scale(glm::mat4(1.0f), q.scale);
        glm::mat4       world   = q.spawnMatrix * local;
        // The camera-facing paths below drop the generator matrix's axes; they keep its size scale (EffectSystem::sizeScale,
        // the only scale a generator matrix carries) on the particle's own (INFERRED).
        const glm::vec3 qs = q.scale * glm::vec3(glm::length(glm::vec3(q.spawnMatrix[0])), glm::length(glm::vec3(q.spawnMatrix[1])),
                                                 glm::length(glm::vec3(q.spawnMatrix[2])));
        if ((q.flags & 0xC) == 0xC)
        {
            // Particle flags bits 2 + 3: screen space (CODE 0x10045cd7..0x10045d24). The world matrix is the local
            // matrix alone (no generator matrix) times diag(1, -1, 1), and the view is the identity (0x10045b80
            // returns [0x1047cf7c]+0x810, set to the identity at 0x1006ce6c): the particle sits in camera space,
            // x right, y down (flipped up for D3D), z ahead, through the scene's projection. The fishing arrows
            // (3727 `ar*` / `al*`) are drawn this way.
            return screenSpace(local);
        }
        if ((q.flags & 0x1) && !(q.flags & 0x1C0))
        {
            const glm::vec3 at = glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f));
            glm::mat4       bb(1.0f);
            bb[0] = glm::vec4(camRight, 0);
            bb[1] = glm::vec4(camDown, 0);
            bb[2] = glm::vec4(camBack, 0);
            bb[3] = glm::vec4(at, 1);
            world = bb * glm::rotate(glm::mat4(1.0f), q.rot.z, glm::vec3(0, 0, 1)) * glm::scale(glm::mat4(1.0f), qs);
        }
        else if (((q.flags >> 6) & 7) == 3)
        {
            // Particle flags bits 6-8 = 3: the mesh's +X turns toward the camera (the sun's glow bowl; CODE):
            // d = camera - position, local x RotZ(pitch) x RotY(yaw) in D3D row order.
            const glm::vec3 at    = glm::vec3(world[3]);
            const glm::vec3 d     = camera_ - at;
            const float     pitch = std::atan2(d.y, std::hypot(d.x, d.z));
            const float     yaw   = -std::atan2(d.z, d.x);
            world = glm::translate(glm::mat4(1.0f), at) * glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0)) *
                    glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(0, 0, 1)) * eulerMatrix(q.rot) * glm::scale(glm::mat4(1.0f), qs);
        }
        else if (const uint32_t align = (q.flags >> 13) & 3; align != 0 && !(q.flags & 0x1))
        {
            // Bits 13-14 pick a view-alignment matrix and drop the generator's rotation (§G.11 / §G.12). INFERRED:
            // 2 = Y-locked (upright, turned to the camera), 1 and 3 = full camera alignment.
            const glm::vec3 at = glm::vec3(q.spawnMatrix * glm::vec4(q.pos, 1.0f));
            glm::vec3       r  = camRight, d = camDown;
            if (align == 2)
            {
                r = glm::length(glm::vec2(camRight.x, camRight.z)) > 1e-4f ? glm::normalize(glm::vec3(camRight.x, 0, camRight.z)) : glm::vec3(1, 0, 0);
                d = glm::vec3(0, 1, 0);
            }
            glm::mat4 va(1.0f);
            va[0] = glm::vec4(r, 0);
            va[1] = glm::vec4(d, 0);
            va[2] = glm::vec4(glm::normalize(glm::cross(r, d)), 0);
            va[3] = glm::vec4(at, 1);
            world = va * eulerMatrix(q.rot) * glm::scale(glm::mat4(1.0f), qs);
        }
        return world;
    };
    for (const auto& gp : generators_)
    {
        const auto& g = *gp;
        for (const auto& q : g.particles)
        {
            const glm::vec4 pc(q.rgba[0] / 128.0f, q.rgba[1] / 128.0f, q.rgba[2] / 128.0f, q.rgba[3] / 128.0f * std::min(q.dayFactor, 1.0f));
            if (q.ringCount >= 2 && q.ringSegments > 0 && q.rgba[3] != 0 && q.dayFactor > 0.01f)
            {
                // Class 0x24 `ring` (CODE 0x10043650): ringCount - 1 bands, each `segments` quads between radius r[b] and
                // r[b + 1] in the particle's XY plane; corner colour = the ring's D3DCOLOR x the particle colour >> 7. The
                // texture is the screen as drawn so far at each vertex's own projection (0x10042c30), so the ring
                // brightens what is behind it by its colours.
                const glm::mat4 world = worldOf(q);
                auto&           b     = batchFor(nullptr, blendOf(q.blend), q.depthKey, true);
                auto colourOf = [&](uint32_t d) {
                    return glm::vec4(static_cast<float>((d >> 16) & 0xFF), static_cast<float>((d >> 8) & 0xFF), static_cast<float>(d & 0xFF),
                                     static_cast<float>(d >> 24)) / 255.0f * pc;
                };
                const float step = 2.0f * kPi / static_cast<float>(q.ringSegments);
                for (int r = 0; r + 1 < q.ringCount; ++r)
                {
                    const glm::vec4 c0 = colourOf(q.ringColour[static_cast<std::size_t>(r)]), c1 = colourOf(q.ringColour[static_cast<std::size_t>(r) + 1]);
                    const float     r0 = q.ringRadius[static_cast<std::size_t>(r)], r1 = q.ringRadius[static_cast<std::size_t>(r) + 1];
                    for (int k = 0; k < q.ringSegments; ++k)
                    {
                        const float a0 = step * static_cast<float>(k), a1 = step * static_cast<float>(k + 1);
                        auto        v  = [&](float a, float rad, const glm::vec4& c) {
                            return Vertex{ glm::vec3(world * glm::vec4(std::sin(a) * rad, std::cos(a) * rad, 0, 1)), glm::vec2(0), c };
                        };
                        const Vertex i0 = v(a0, r0, c0), o0 = v(a0, r1, c1), i1 = v(a1, r0, c0), o1 = v(a1, r1, c1);
                        b.vertices.insert(b.vertices.end(), { i0, o0, i1, i1, o0, o1 });
                    }
                }
                continue;
            }
            if (g.resType == 0x22 && q.rgba[3] != 0 && q.dayFactor > 0.01f)
            {
                // Class 0x22 `dist` (heat haze; CODE draw 0x10043040 -> 0x10042c30): the screen behind it, displaced. The
                // game draws the `dist` resource's mesh with projected UVs moved by the L1 0x32 strength; xi-client draws a
                // camera-facing quad whose corners sample the screen pushed out and swirled by that strength (INFERRED).
                const glm::mat4 world = worldOf(q);
                auto&           b     = batchFor(nullptr, Batch::Blend::Alpha, q.depthKey, true);
                const float     k     = q.distortion * 4.0f;
                const float     ph    = (q.total - q.remaining) * 0.15f;
                const glm::vec4 c(1, 1, 1, pc.a);
                auto            v = [&](float x, float y, float o) {
                    return Vertex{ glm::vec3(world * glm::vec4(x, y, 0, 1)), glm::vec2(std::cos(ph + o) * k, std::sin(ph + o) * k), c };
                };
                const Vertex a = v(-1, -1, 0), bb = v(1, -1, 1.6f), cc = v(-1, 1, 3.1f), d = v(1, 1, 4.7f);
                b.vertices.insert(b.vertices.end(), { a, bb, cc, cc, bb, d });
                continue;
            }
            const auto* m = q.mesh;
            if (!m || m->vertices.empty() || q.rgba[3] == 0 || q.dayFactor <= 0.01f)
            {
                continue; // hidden (a lamp by day)
            }
            const glm::mat4 world = worldOf(q);
            const auto blend = fxBlendClass(q.blend);
            // Which groups: D3a = one frame; D3m = all.
            std::size_t g0 = 0, g1 = m->groups.size();
            if (m->frames && !m->groups.empty())
            {
                g0 = q.frame % m->groups.size();
                g1 = g0 + 1;
            }
            // The element colour is TFACTOR (CODE 0x10045fa0: alpha x the particle's fades, truncated to a byte). A D3m
            // drawn with blend 0x44 forces a TFACTOR alpha of 0x7F or more to 0xFF (CODE 0x10040170..0x1004018c), so a
            // fully faded-in 0x44 particle is twice as opaque; untextured, its alpha stage is MODULATE2X instead of 4X
            // (0x103523c4).
            const bool      d3mBlendD = !m->frames && m->morph.empty() && (q.blend & 0xFF) == 0x44;
            // Blend 0x64 / 0x68 / 0x62 then replace TFACTOR's alpha byte outright (fxBlendElementAlpha).
            const auto      forced = fxBlendElementAlpha(q.blend);
            const glm::vec4 elem(q.rgba[0] / 255.0f, q.rgba[1] / 255.0f, q.rgba[2] / 255.0f,
                                 forced ? *forced : fxElementAlpha(static_cast<float>(q.rgba[3]) * std::min(q.dayFactor, 1.0f), d3mBlendD));
            for (std::size_t gi = g0; gi < g1; ++gi)
            {
                const auto&         grp = m->groups[gi];
                const dat::Texture* t = nullptr;
                if (!grp.texName.empty() && g.meshFile)
                {
                    t = g.meshFile->texture(grp.texName); // the texture sits beside the mesh
                }
                auto& b = batchFor(t, blend, q.depthKey);
                // D3b: blend the morph frames by the particle's weights, normalised (all 0: frame 0) (§M.4).
                float wsum = 0;
                for (std::size_t k = 0; k < m->morph.size() && k < q.morph.size(); ++k)
                {
                    wsum += q.morph[k];
                }
                for (uint32_t i = grp.first; i < grp.first + grp.count && i < m->vertices.size(); ++i)
                {
                    auto v = m->vertices[i];
                    if (wsum > 1e-4f)
                    {
                        v.x = v.y = v.z = 0;
                        for (std::size_t k = 0; k < m->morph.size() && k < q.morph.size(); ++k)
                        {
                            const float w = q.morph[k] / wsum;
                            v.x += w * m->morph[k][3 * i], v.y += w * m->morph[k][3 * i + 1], v.z += w * m->morph[k][3 * i + 2];
                        }
                    }
                    // §M.2: the two texture stages, each clamped by Direct3D (a vertex colour above 0x80 saturates in
                    // stage 0 before the element colour tints it).
                    const auto [c, st] = fxStages(glm::vec4(v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3]) / 255.0f, elem, t != nullptr, d3mBlendD);
                    glm::vec2 uv(v.u, v.v);
                    if (m->uvPixels && t && t->width > 0 && t->height > 0)
                    {
                        uv /= glm::vec2(static_cast<float>(t->width), static_cast<float>(t->height));
                    }
                    b.vertices.push_back({ glm::vec3(world * glm::vec4(v.x, v.y, v.z, 1.0f)), uv + q.uv, c, st });
                }
            }
            if (q.overlay)
            {
                // L1 0x55 (CODE 0x100411e0): the second texture over the whole mesh in texture stage 1, its coordinates from
                // the camera through a matrix of the L1 0x59-0x5b / L2 0x35-0x37 angles. Drawn as an additive pass with
                // view-space UVs (0.5 + the vertex's camera-plane offset / 2), turned by the angles (INFERRED: the exact
                // texture-coordinate source is not traced).
                auto&           ob = batchFor(q.overlay, Batch::Blend::Additive, q.depthKey);
                const glm::vec4 oc = glm::vec4(q.overlayRgba[0], q.overlayRgba[1], q.overlayRgba[2], q.overlayRgba[3]) / 128.0f * glm::vec4(1, 1, 1, pc.a);
                const float     ca = std::cos(q.overlayAngles.y + q.overlayAngles.z), sa = std::sin(q.overlayAngles.y + q.overlayAngles.z);
                const glm::vec3 centre(world[3]);
                for (uint32_t i = 0; i < m->vertices.size(); ++i)
                {
                    const auto&     v  = m->vertices[i];
                    const glm::vec3 wp = glm::vec3(world * glm::vec4(v.x, v.y, v.z, 1.0f));
                    const glm::vec3 d  = wp - centre;
                    const float     sx = glm::dot(d, camRight) * 0.5f, sy = glm::dot(d, camDown) * 0.5f;
                    const glm::vec2 uv(0.5f + ca * sx - sa * sy + q.overlayAngles.x / kPi, 0.5f + sa * sx + ca * sy);
                    ob.vertices.push_back({ wp, uv, glm::vec4(v.rgba[0], v.rgba[1], v.rgba[2], v.rgba[3]) / 128.0f * oc });
                }
            }
        }
    }
    // Draw order by the particles' depth keys, highest first (Port Bastok: `down` 5000 black base, `col1` 4500 water
    // colour, `1sea` 4000 surface; INFERRED direction). Stable, so ordinary effects (key 0) keep their order.
    std::stable_sort(out.begin(), out.end(), [](const Batch& a, const Batch& b) { return a.depthKey > b.depthKey; });
}

// --- Coverage, part 7B (effect-format): the state behind the screen / environment / sound / actor ops. ---

bool EffectSystem::weaponHeld(uint32_t actor, int slot) const
{
    return std::ranges::any_of(holds_, [&](const ActorHold& h) { return h.actor == actor && h.kind == 0 && h.value == slot; });
}

bool EffectSystem::stepsForced(uint32_t actor) const
{
    return std::ranges::any_of(holds_, [&](const ActorHold& h) { return h.actor == actor && h.kind == 1; });
}

int EffectSystem::motionVariant(uint32_t actor, int kind) const
{
    const auto it = motionVariants_.find({ actor, kind });
    return it == motionVariants_.end() ? 0 : it->second;
}

std::optional<glm::vec3> EffectSystem::attachedPosition(uint32_t actor) const
{
    for (const auto& a : attaches_)
    {
        if (a.actor == actor && a.at)
        {
            return a.at;
        }
    }
    return std::nullopt;
}

std::optional<float> EffectSystem::yawOverride(uint32_t actor) const
{
    for (const auto& t : turns_)
    {
        if (t.actor == actor && t.started)
        {
            return t.yaw;
        }
    }
    return std::nullopt;
}

void EffectSystem::update7b(float ticks)
{
    for (auto& h : holds_)
    {
        h.remaining = h.remaining < 0 ? h.remaining : std::max(h.remaining - ticks, 0.0f);
    }
    std::erase_if(holds_, [](const ActorHold& h) { return h.remaining == 0.0f; });
    const auto actorNow = [this](uint32_t id) { return actors_ && *actors_ ? (*actors_)(id) : std::nullopt; };
    // Op 0x7A (CODE 0x100c6e7a): the target's point (vfunc 0x1c4) + RotY(target yaw) x offset.
    for (auto& a : attaches_)
    {
        a.remaining -= ticks;
        a.at.reset();
        if (const auto t = actorNow(a.target))
        {
            const auto pt = t->point && a.point < 0x30 ? t->point(a.point) : std::nullopt;
            a.at          = pt.value_or(t->pos) + glm::vec3(yawMatrix(t->yaw) * glm::vec4(a.offset, 0.0f));
        }
    }
    std::erase_if(attaches_, [](const Attach& a) { return a.remaining <= 0; });
    // Op 0x62: from the actor's facing toward the target at `rate` per tick; the facing stays for `hold` ticks.
    for (auto& t : turns_)
    {
        const auto a = actorNow(t.actor);
        const auto b = actorNow(t.target);
        if (!t.started)
        {
            if (!a || !b)
            {
                t.hold = 0;
                continue;
            }
            const glm::vec2 d(b->pos.x - a->pos.x, b->pos.z - a->pos.z);
            const float     want = glm::length(d) < 0.1f ? a->yaw : std::atan2(d.y, d.x);
            float           diff = std::remainder(want - a->yaw, 6.2831853f);
            t.yaw     = a->yaw;
            t.left    = std::abs(diff);
            t.rate    = diff < 0 ? -std::abs(t.rate) : std::abs(t.rate);
            t.started = true;
        }
        const float step = std::min(t.left, std::abs(t.rate) * ticks);
        t.yaw += t.rate < 0 ? -step : step;
        t.left -= step;
        t.hold -= ticks;
    }
    std::erase_if(turns_, [](const Turn& t) { return t.hold <= 0; });
    // Ops 0x6F / 0x70 (CODE tick 0x10060920): value = start + (target - start) x (1 - remaining / duration).
    for (auto& d : lightDrives_)
    {
        const bool alive = std::ranges::any_of(generators_, [&](const auto& g) { return g.get() == d.g; });
        if (!alive)
        {
            d.g = nullptr;
            continue;
        }
        if (!d.started)
        {
            if (!d.g->particles.empty())
            {
                const auto& q = d.g->particles.front();
                d.from        = d.intensity ? q.lightIntensityScale : q.lightRangeScale;
            }
            d.started = true;
        }
        d.elapsed       = std::min(d.elapsed + ticks, d.duration);
        const float v   = d.from + (d.to - d.from) * (d.elapsed / d.duration);
        for (auto& q : d.g->particles)
        {
            (d.intensity ? q.lightIntensityScale : q.lightRangeScale) = v;
        }
    }
    std::erase_if(lightDrives_, [](const LightDrive& d) { return !d.g || d.elapsed >= d.duration; });
    // Ops 0x51 / 0x72 (CODE tick 0x10060510): from the colour at the start to the target, held afterwards.
    for (auto* d : { &screenDim_, &screenAdd_ })
    {
        d->elapsed = std::min(d->elapsed + ticks, d->duration);
        d->colour  = d->duration > 0 ? glm::mix(d->from, d->to, d->elapsed / d->duration) : d->to;
    }
}

void EffectSystem::reset7b()
{
    const uint32_t serial = environment_.serial + 1;
    environment_          = {};
    environment_.serial   = serial;
    soundStops_.clear(), musicFade_.reset(), holds_.clear(), motionVariants_.clear(), attaches_.clear(), turns_.clear();
    lightDrives_.clear();
    screenDim_ = {};
    screenAdd_ = { glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f) };
}

void EffectSystem::screenLayers7b(std::vector<ScreenLayer>& out) const
{
    const auto bytes = [](const glm::vec4& c) { return glm::round(glm::clamp(c, 0.0f, 1.0f) * 255.0f); };
    // Op 0x51's colour through 0x1000a820: nothing at 0x80808080; alpha 0x80 means alpha = A x B / 128 and no colour;
    // above 0x80 it is added (SRCALPHA / ONE), else the scene is scaled by alpha (black, INVSRCALPHA / SRCALPHA).
    if (const glm::vec4 c = bytes(screenDim_.colour); c != glm::vec4(128.0f))
    {
        glm::vec3 rgb = glm::vec3(c) / 255.0f;
        float     a   = c.a;
        if (c.a == 128.0f)
        {
            a   = std::floor(c.a * c.b / 128.0f);
            rgb = glm::vec3(0.0f);
        }
        ScreenLayer l;
        l.image = false;
        if (a > 128.0f)
        {
            l.additive = true;
            l.colour   = glm::vec4(rgb, a / 255.0f);
        }
        else
        {
            l.colour = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f - a / 255.0f);
        }
        if (l.colour.a > 0.002f && (!l.additive || rgb.r + rgb.g + rgb.b > 0.0f))
        {
            out.push_back(l);
        }
    }
    // Op 0x72's colour through 0x10013270: when R, G or B is set, added over the frame (SRCALPHA / ONE), alpha x 2.
    if (const glm::vec4 c = bytes(screenAdd_.colour); c.r + c.g + c.b > 0.0f)
    {
        ScreenLayer l;
        l.image    = false;
        l.additive = true;
        l.colour   = glm::vec4(glm::vec3(c) / 255.0f, std::min(c.a * 2.0f, 255.0f) / 255.0f);
        out.push_back(l);
    }
}

} // namespace xi::client
