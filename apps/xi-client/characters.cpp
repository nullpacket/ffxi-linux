// SPDX-License-Identifier: GPL-3.0-or-later
#include "characters.h"

#include "dat/character.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>

namespace xi::app
{
namespace
{

std::optional<dat::CharacterAsset> loadLook(const dat::DatIndex& index, const client::Look& l)
{
    if (l.kind == 0xFFFF || l.kind == 1)
    {
        const auto race = static_cast<uint8_t>(l.ids[0] >> 8);
        const auto face = static_cast<uint8_t>(l.ids[0] & 0xFF);
        return dat::loadPcAsset(index, race, face, { l.ids[1], l.ids[2], l.ids[3], l.ids[4], l.ids[5], l.ids[6], l.ids[7], l.ids[8] });
    }
    if (l.kind == kLookFile)
    {
        return dat::loadModelFile(index, static_cast<uint32_t>(l.ids[0]) | static_cast<uint32_t>(l.ids[1]) << 16);
    }
    if (l.kind == kLookChocobo)
    {
        // S2C 0x00E SubKind 7 (CODE 0x1009cd80): the race is word 0's LOW byte; races outside 32..36 become 32.
        const auto race = static_cast<uint8_t>(l.ids[0] & 0xFF);
        return dat::loadPcAsset(index, race >= 32 && race <= 36 ? race : 32, 0, { l.ids[1], l.ids[2], l.ids[3], l.ids[4], l.ids[5], 0, 0, 0 });
    }
    return dat::loadNpcAsset(index, client::npcModel(l));
}

bool pcLook(const client::Look& l)
{
    return l.kind == 0xFFFF || l.kind == 1;
}

} // namespace

std::string lookKey(const client::Look& l)
{
    if (!l.valid)
    {
        return {};
    }
    if (pcLook(l)) // PCs / equipped NPCs: face|race + armour
    {
        return fmt::format("pc:{:04x}:{:04x}:{:04x}:{:04x}:{:04x}:{:04x}:{:04x}:{:04x}:{:04x}", l.ids[0], l.ids[1], l.ids[2], l.ids[3], l.ids[4], l.ids[5],
                           l.ids[6], l.ids[7], l.ids[8]);
    }
    if (const auto m = client::npcModel(l); m != 0)
    {
        return fmt::format("npc:{}", m);
    }
    if (l.kind == kLookChocobo)
    {
        return fmt::format("choco:{:04x}:{:04x}:{:04x}:{:04x}:{:04x}:{:04x}", l.ids[0], l.ids[1], l.ids[2], l.ids[3], l.ids[4], l.ids[5]);
    }
    if (l.kind == kLookFile)
    {
        return fmt::format("file:{}", static_cast<uint32_t>(l.ids[0]) | static_cast<uint32_t>(l.ids[1]) << 16);
    }
    return {};
}

Characters::Characters(const dat::DatIndex& index, gfx::TextureSet& textures, gfx::SkinnedRenderer& renderer)
    : index_(index)
    , textures_(textures)
    , renderer_(renderer)
{
    const char* sync = std::getenv("XI_SYNC_LOADS");
    sync_            = sync && std::string(sync) != "0";
    if (!sync_)
    {
        worker_ = std::thread([this] { work(); });
    }
}

Characters::~Characters()
{
    {
        const std::scoped_lock lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

const Characters::Entry* Characters::find(const client::Look& look) const
{
    const auto it = cache_.find(lookKey(look));
    return it == cache_.end() ? nullptr : &it->second;
}

Characters::Prepared Characters::prepare(const std::string& key, const client::Look& look)
{
    // Worker thread (or the frame with XI_SYNC_LOADS): DAT reads and parsing only, no GPU work. The DatIndex reads are
    // independent file reads; the motion file cache (dat::fileMotions) has its own lock; characterMotions_ is worker-only.
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    Prepared p;
    p.key = key;
    try
    {
        const auto t0 = Clock::now();
        p.asset       = loadLook(index_, look);
        const auto t1 = Clock::now();
        p.datMs       = ms(t0, t1);
        if (p.asset)
        {
            // Looks of one race and weapon class share their motion files: parse them once.
            const auto& a    = *p.asset;
            const auto  mkey = fmt::format("{}|{}|{}", fmt::join(a.motionFiles, ","), fmt::join(a.battleMotionFiles, ","), a.sheathStyle);
            auto        mit  = characterMotions_.find(mkey);
            if (mit == characterMotions_.end())
            {
                mit = characterMotions_
                          .emplace(mkey, std::make_shared<const dat::CharacterMotions>(
                                             dat::loadCharacterMotions(index_, a.motionFiles, a.battleMotionFiles, a.sheathStyle)))
                          .first;
            }
            p.motions         = *mit->second; // copied here, not in the frame
            p.drawnWeaponBone = dat::drawnWeaponBone(a.skeleton, p.motions.battle, a.mainWeaponBone);
            const auto t2     = Clock::now();
            p.motionMs        = ms(t1, t2);
            p.upload          = gfx::prepareCharacterUpload(a); // texture decode, vertex build
            p.cpuMs           = ms(t2, Clock::now());
        }
    }
    catch (const std::exception& e)
    {
        p.asset.reset();
        p.error = e.what();
    }
    return p;
}

void Characters::work()
{
    for (;;)
    {
        std::pair<std::string, client::Look> req;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !requests_.empty(); });
            if (stop_)
            {
                return;
            }
            req = std::move(requests_.front());
            requests_.pop_front();
        }
        auto prepared = prepare(req.first, req.second);
        const std::scoped_lock lock(mutex_);
        ready_.push_back(std::move(prepared));
    }
}

void Characters::upload(Prepared&& p)
{
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    Uploading  u;
    u.key = p.key;
    if (!p.error.empty())
    {
        spdlog::warn("character {}: {}", p.key, p.error);
    }
    else if (p.asset)
    {
        try
        {
            const auto& asset              = *p.asset;
            u.entry.model                  = gfx::beginCharacterUpload(asset, p.upload, textures_, renderer_, u.ticket);
            u.entry.model->motions         = std::move(p.motions);
            u.entry.model->weaponBone      = asset.mainWeaponBone;
            u.entry.model->drawnWeaponBone = p.drawnWeaponBone;
            u.entry.motionFiles            = asset.motionFiles;
            u.entry.battleMotionFiles      = asset.battleMotionFiles;
            u.entry.weaponFiles            = asset.weaponFiles;
            u.entry.gearFiles              = asset.gearFiles;
            u.entry.effectFiles            = asset.effectFiles;
            u.entry.model->scale           = asset.scale;
            u.entry.feet                   = { asset.shoe, asset.shoeVariant, asset.footLayout };
            u.entry.bodyVariant            = asset.bodyVariant;
            u.entry.rangedClass            = asset.rangedClass;
            u.entry.sheathStyle            = asset.sheathStyle;
            static const bool trace = std::getenv("XI_FRAME_TRACE") != nullptr;
            if (trace)
            {
                spdlog::info("frame trace: load {}: dat {:.2f} motions {:.2f} decode {:.2f} ({}) upload {:.2f} ({} textures) ms", p.key, p.datMs, p.motionMs,
                             p.cpuMs, sync_ ? "in the frame" : "worker", std::chrono::duration<double, std::milli>(Clock::now() - t0).count(),
                             asset.textures.size());
            }
        }
        catch (const std::exception& e)
        {
            u.entry.model.reset();
            u.ticket = 0;
            spdlog::warn("character {}: {}", p.key, e.what());
        }
    }
    // The frame does not wait for the GPU copy: the model draws once its fence signals (verified live with validation:
    // crowds, zoning, the Mog House, cutscenes). XI_ASYNC_UPLOAD=0 waits for each upload, as before.
    static const bool async = [] {
        const char* v = std::getenv("XI_ASYNC_UPLOAD");
        return !v || std::string(v) != "0";
    }();
    if ((sync_ || !async) && u.ticket)
    {
        renderer_.context().waitUpload(u.ticket);
    }
    uploading_.push_back(std::move(u));
}

void Characters::newFrame()
{
    // Looks whose upload has finished on the GPU become drawable.
    for (std::size_t i = 0; i < uploading_.size();)
    {
        auto& u = uploading_[i];
        if (u.ticket == 0 || renderer_.context().uploadDone(u.ticket))
        {
            pending_.erase(u.key);
            cache_.insert_or_assign(u.key, std::move(u.entry));
            uploading_.erase(uploading_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            ++i;
        }
    }
    if (sync_)
    {
        return;
    }
    // Start the uploads of what the worker has read: the decode and vertex build happened there, so an upload here is
    // the GPU allocations and the recording (no queue wait); a few a frame within ~1 ms.
    const auto start = std::chrono::steady_clock::now();
    for (int n = 0; n < 4; ++n)
    {
        std::optional<Prepared> p;
        {
            const std::scoped_lock lock(mutex_);
            if (ready_.empty())
            {
                return;
            }
            p.emplace(std::move(ready_.front()));
            ready_.pop_front();
        }
        upload(std::move(*p));
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > 1.0)
        {
            return;
        }
    }
}

const gfx::CharacterModel* Characters::model(const client::Look& look)
{
    const auto key = lookKey(look);
    if (key.empty())
    {
        return nullptr;
    }
    auto it = cache_.find(key);
    if (it == cache_.end())
    {
        if (sync_)
        {
            upload(prepare(key, look));
            newFrame(); // the upload was waited for: install it now
            it = cache_.find(key);
        }
        else
        {
            if (pending_.insert(key).second)
            {
                {
                    const std::scoped_lock lock(mutex_);
                    requests_.emplace_back(key, look);
                }
                wake_.notify_one();
            }
            return nullptr;
        }
    }
    return it->second.model ? &*it->second.model : nullptr;
}

const std::vector<dat::MotionSet>* Characters::weaponSkill(const client::Look& look, uint16_t anim)
{
    if (!look.valid || anim == 0)
    {
        return nullptr;
    }
    if (!pcLook(look))
    {
        // Monsters / NPC models: TP moves from the effect file, stems from the model's own motions.
        const auto key = lookKey(look) + "#" + std::to_string(anim);
        auto       mit = mobSkills_.find(key);
        if (mit == mobSkills_.end())
        {
            const auto* ce = find(look);
            if (!ce)
            {
                return nullptr;
            }
            mit = mobSkills_.emplace(key, dat::loadMonsterSkillMotions(index_, anim, ce->motionFiles)).first;
            spdlog::info("monster skill motions: {} animation {}: {} motion sets", lookKey(look), anim, mit->second.size());
        }
        return &mit->second;
    }
    const auto race = static_cast<uint8_t>(look.ids[0] >> 8);
    const auto key  = static_cast<uint32_t>(race) << 16 | anim;
    auto       it   = wsMotions_.find(key);
    if (it == wsMotions_.end())
    {
        it = wsMotions_.emplace(key, dat::loadWeaponSkillMotions(index_, race, anim)).first;
        spdlog::info("weapon skill motions: race {} animation {}: {} motion sets", race, anim, it->second.size());
    }
    return &it->second;
}

const dat::MotionSet* Characters::emote(const client::Look& look, uint8_t emote)
{
    if (!look.valid || !pcLook(look))
    {
        return nullptr;
    }
    const auto race = static_cast<uint8_t>(look.ids[0] >> 8);
    const auto key  = static_cast<uint32_t>(race) << 8 | emote;
    auto       it   = emotes_.find(key);
    if (it == emotes_.end())
    {
        it = emotes_.emplace(key, dat::loadEmoteMotion(index_, race, emote)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

const dat::MotionSet* Characters::motion(const client::Look& look, const std::string& stem, const std::vector<uint32_t>& extraFiles)
{
    const auto* ce = find(look);
    if (!ce)
    {
        return nullptr;
    }
    std::string key = lookKey(look) + "#" + stem;
    for (const auto f : extraFiles)
    {
        key += "@" + std::to_string(f);
    }
    auto it = motions_.find(key);
    if (it == motions_.end())
    {
        std::vector<uint32_t> files;
        for (const auto f : extraFiles)
        {
            if (index_.path(f))
            {
                files.push_back(f);
            }
        }
        files.insert(files.end(), ce->battleMotionFiles.begin(), ce->battleMotionFiles.end());
        files.insert(files.end(), ce->motionFiles.begin(), ce->motionFiles.end());
        it = motions_.emplace(key, dat::loadMotionSet(index_, files, stem)).first;
        spdlog::debug("motion {} for {}: {} pieces", stem, lookKey(look), it->second.pieces.size());
    }
    return it->second.empty() ? nullptr : &it->second;
}

namespace
{

// The [S] zones (CODE 0x100fd2d0: zone ids 80..99, 136, 137; INFERRED that env+0x1b4 is the zone).
bool pastZone(uint16_t zone)
{
    return (zone >= 80 && zone <= 99) || zone == 136 || zone == 137;
}

client::Look fileLook(uint32_t file)
{
    client::Look l;
    l.kind   = kLookFile;
    l.valid  = true;
    l.ids[0] = static_cast<uint16_t>(file & 0xFFFF);
    l.ids[1] = static_cast<uint16_t>(file >> 16);
    return l;
}

} // namespace

client::Look Characters::mountLook(uint8_t animation, uint8_t chocoboIndex, const std::array<uint32_t, 2>& looks, uint8_t mountIndex, uint16_t zone)
{
    if (animation == 85)
    {
        return fileLook(102705u + (mountIndex ? mountIndex - 1u : 0u));
    }
    if (chocoboIndex < 2 || chocoboIndex > 4)
    {
        return fileLook(pastZone(zone) ? 39276u : 39275u);
    }
    // CODE 0x1009aa90 / 0x1009ab50: Noble (bit 0 of the second word) or the own chocobo's colour and features.
    client::Look l;
    l.kind  = kLookChocobo;
    l.valid = true;
    const uint32_t p = looks[0];
    if (looks[1] & 1)
    {
        l.ids = { 32, 3, 3, 3, 1, 3, 0, 0, 0 };
    }
    else
    {
        l.ids = { static_cast<uint16_t>(32 + ((p >> 9) & 7)), static_cast<uint16_t>(p & 7), 7, static_cast<uint16_t>((p >> 6) & 7), 0,
                  static_cast<uint16_t>((p >> 3) & 7), 0, 0, 0 };
    }
    return l;
}

namespace
{
// CODE 0x100d2730 / 0x100d2940 / 0x100d2af0: file A = u16 0x1035f5bc[race] + off; piece 2 from the 0x1035f5d0 pair
// by the body variant (offsets: 0 riding, 1 fishing, 3 riding in the [S] zones, 5 on a mount).
std::vector<uint32_t> raceExtraFiles(uint8_t race, uint8_t bodyVariant, uint32_t off)
{
    static constexpr uint32_t kA[9]    = { 38603, 38603, 38699, 38795, 38891, 38987, 38987, 39083, 39179 };
    static constexpr uint32_t kB[9][2] = { { 38635, 38667 }, { 38635, 38667 }, { 38731, 38763 }, { 38827, 38859 }, { 38923, 38955 },
                                           { 39019, 39051 }, { 39019, 39051 }, { 39115, 39147 }, { 39211, 39243 } };
    const std::size_t     r = race < 9 ? race : 1;
    std::vector<uint32_t> files{ kA[r] + off };
    if (bodyVariant > 0)
    {
        files.push_back(kB[r][bodyVariant == 1 ? 0 : 1] + off);
    }
    return files;
}
} // namespace

std::optional<client::Look> Characters::rodLook(const client::Look& look)
{
    if (!pcLook(look))
    {
        return std::nullopt;
    }
    // u16 0x1035f5a8[race] (races 9..0x1d take Hume male's, CODE 0x100d33cf).
    static constexpr uint32_t kRod[9] = { 39307, 39307, 39339, 39371, 39403, 39435, 39435, 39467, 39499 };
    const auto                race     = static_cast<uint8_t>(look.ids[0] >> 8);
    return fileLook(kRod[race < 9 ? race : 1] + (look.ids[8] & 0xFFFu));
}

std::string Characters::stemOf(const dat::MotionSet* set) const
{
    if (!set)
    {
        return {};
    }
    for (const auto& [key, m] : motions_)
    {
        if (&m == set)
        {
            const auto h = key.find('#');
            return h == std::string::npos ? std::string{} : key.substr(h + 1, key.find('@', h) - h - 1);
        }
    }
    return {};
}

std::vector<uint32_t> Characters::fishingFiles(const client::Look& look)
{
    const auto* ce = find(look);
    if (!ce || !pcLook(look))
    {
        return {};
    }
    return raceExtraFiles(static_cast<uint8_t>(look.ids[0] >> 8), ce->bodyVariant, 1);
}

const Characters::Ride* Characters::ride(const client::Look& look, uint16_t zone, bool mount)
{
    const auto* ce = find(look);
    if (!ce || !pcLook(look))
    {
        return nullptr;
    }
    const auto     race = static_cast<uint8_t>(look.ids[0] >> 8);
    const uint32_t off  = mount ? 5u : pastZone(zone) ? 3u : 0u;
    const auto     files = raceExtraFiles(race, ce->bodyVariant, off);
    const auto     key  = lookKey(look) + "#" + std::to_string(off);
    auto           it   = rides_.find(key);
    if (it == rides_.end())
    {
        Ride rd;
        rd.idle = dat::loadMotionSet(index_, files, "chi");
        rd.walk = dat::loadMotionSet(index_, files, "wlk");
        rd.run  = dat::loadMotionSet(index_, files, "run");
        rd.mount = dat::loadMotionSet(index_, files, "chh");
        rd.dismount1 = dat::loadMotionSet(index_, files, "ch1");
        rd.dismount2 = dat::loadMotionSet(index_, files, "ch2");
        spdlog::info("riding motions for {}: files {} (chi {} pieces)", key, fmt::join(files, ","), rd.idle.pieces.size());
        it = rides_.emplace(key, std::move(rd)).first;
    }
    return &it->second;
}

} // namespace xi::app
