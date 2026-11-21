// SPDX-License-Identifier: GPL-3.0-or-later
// Native stand-ins for the memory signatures Ashita addons scan for (scan_registry.h; docs/ashita-compat.md).
#include "ashita/scan_registry.h"
#include "ashita/game_time.h"
#include "xi/install.h"

#include "ashita/ashita.h"
#include "ashita/lua_util.h"

#include <spdlog/spdlog.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xi::ashita
{
namespace
{

struct Chunk
{
    uintptr_t   base = 0;
    std::size_t size = 0, used = 0;
};

struct State
{
    std::mutex                                 m;
    ClientUi                                   ui;
    ConfigHooks                                config;
    NativeHooks                                native;
    MacroHooks                                 macros;
    TimeSource                                 time;
    TargetHooks                                targets;
    uintptr_t                                  macroObject = 0;
    std::vector<std::array<int32_t, 4>>        equipEx; // queued EquipEx calls, sent by prelude_scan.lua
    std::filesystem::path                      dll;
    bool                                       imageTried = false;
    ImageInfo                                  image;
    std::vector<Chunk>                         chunks;
    std::unordered_map<std::string, uintptr_t> keyed;
    std::map<std::pair<std::string, int>, uintptr_t> found;
};

State& state()
{
    static State s;
    return s;
}

uint32_t le32(const std::vector<uint8_t>& d, std::size_t o)
{
    if (o + 4 > d.size())
    {
        return 0;
    }
    return static_cast<uint32_t>(d[o]) | static_cast<uint32_t>(d[o + 1]) << 8 | static_cast<uint32_t>(d[o + 2]) << 16 |
           static_cast<uint32_t>(d[o + 3]) << 24;
}

uint16_t le16(const std::vector<uint8_t>& d, std::size_t o)
{
    if (o + 2 > d.size())
    {
        return 0;
    }
    return static_cast<uint16_t>(d[o] | d[o + 1] << 8);
}

// The packer's LZSS: a flag byte, MSB first; 1 = a literal, 0 = two bytes b0 b1 giving
// offset ((b0 << 8) | b1) & 0xFFF (0 ends) and length (b0 >> 4) + 3.
std::vector<uint8_t> lzss(const uint8_t* src, std::size_t n, std::size_t expected)
{
    std::vector<uint8_t> out;
    out.reserve(expected);
    std::size_t i = 0;
    while (i < n)
    {
        uint8_t flags = src[i++];
        for (int k = 0; k < 8; ++k)
        {
            if (flags & 0x80)
            {
                if (i >= n)
                {
                    return out;
                }
                out.push_back(src[i++]);
            }
            else
            {
                if (i + 1 >= n)
                {
                    return out;
                }
                const uint8_t b0 = src[i], b1 = src[i + 1];
                const std::size_t off = static_cast<std::size_t>((b0 << 8 | b1) & 0xFFF);
                if (off == 0)
                {
                    return out;
                }
                i += 2;
                const std::size_t len = static_cast<std::size_t>((b0 >> 4) + 3);
                if (off > out.size())
                {
                    return {};
                }
                const std::size_t start = out.size() - off;
                for (std::size_t c = 0; c < len; ++c)
                {
                    out.push_back(out[start + c]);
                }
            }
            flags = static_cast<uint8_t>(flags << 1);
            if (out.size() > expected + 0x100000)
            {
                return {};
            }
        }
    }
    return out;
}

// Maps the DLL as data at its preferred base, its packed .text unpacked. Caller holds the lock.
void mapImage(State& s)
{
    s.imageTried = true;
    auto path = s.dll;
    if (path.empty())
    {
        const char* root = xi::install::gameRootOrNull();
        if (!root || !*root)
        {
            return;
        }
        path = std::filesystem::path(root) / "FFXiMain.dll";
    }
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        spdlog::info("ashita: no game image ({}); memory scans answer only the emulated signatures", path.string());
        return;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const uint32_t pe = le32(d, 0x3C);
    if (d.size() < 0x200 || pe + 0xF8 > d.size() || le32(d, pe) != 0x4550)
    {
        return;
    }
    const uint16_t nsec    = le16(d, pe + 6);
    const uint16_t optSize = le16(d, pe + 20);
    const std::size_t opt  = pe + 24;
    const uint32_t entry   = le32(d, opt + 16);
    const uint32_t base    = le32(d, opt + 28);
    const uint32_t size    = le32(d, opt + 56);
    const uint32_t hdrSize = le32(d, opt + 60);
    struct Sec
    {
        std::string name;
        uint32_t    vsize, va, rsize, rptr;
    };
    std::vector<Sec> secs;
    for (uint16_t k = 0; k < nsec; ++k)
    {
        const std::size_t o = opt + optSize + 40u * k;
        if (o + 40 > d.size())
        {
            return;
        }
        Sec sec;
        sec.name.assign(reinterpret_cast<const char*>(&d[o]), strnlen(reinterpret_cast<const char*>(&d[o]), 8));
        sec.vsize = le32(d, o + 8);
        sec.va    = le32(d, o + 12);
        sec.rsize = le32(d, o + 16);
        sec.rptr  = le32(d, o + 20);
        secs.push_back(sec);
    }
    auto rvaToOff = [&](uint32_t rva) -> std::optional<std::size_t> {
        for (const auto& sec : secs)
        {
            if (rva >= sec.va && rva < sec.va + std::max(sec.vsize, sec.rsize))
            {
                return sec.rptr + (rva - sec.va);
            }
        }
        return std::nullopt;
    };
    if (base < 0x10000 || size == 0 || size > 0x4000000)
    {
        return;
    }
    const long page = sysconf(_SC_PAGESIZE);
    const std::size_t mapSize = (size + static_cast<std::size_t>(page) - 1) & ~(static_cast<std::size_t>(page) - 1);
    void* want = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
    void* p    = mmap(want, mapSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || p != want)
    {
        if (p != MAP_FAILED)
        {
            munmap(p, mapSize);
        }
        spdlog::warn("ashita: the game image's range {:#x}+{:#x} is taken; memory scans answer only the emulated signatures", base, size);
        return;
    }
    auto* img = static_cast<uint8_t*>(p);
    std::memcpy(img, d.data(), std::min<std::size_t>({ hdrSize, d.size(), mapSize }));
    for (const auto& sec : secs)
    {
        const std::size_t n = std::min<std::size_t>(sec.rsize, sec.vsize ? sec.vsize : sec.rsize);
        if (sec.va < mapSize && sec.rptr + n <= d.size())
        {
            std::memcpy(img + sec.va, d.data() + sec.rptr, std::min<std::size_t>(n, mapSize - sec.va));
        }
    }
    // The packed .text: the entry stub's immediates give the payload.
    const auto text = std::find_if(secs.begin(), secs.end(), [](const Sec& sec) { return sec.name == ".text"; });
    if (const auto eo = rvaToOff(entry); text != secs.end() && eo && *eo + 0x100 <= d.size())
    {
        const uint8_t* stub = d.data() + *eo;
        auto           find = [&](std::size_t from, std::initializer_list<uint8_t> bytes) -> std::optional<std::size_t> {
            for (std::size_t i = from; i + bytes.size() <= 0x100; ++i)
            {
                if (std::equal(bytes.begin(), bytes.end(), stub + i))
                {
                    return i;
                }
            }
            return std::nullopt;
        };
        const auto a = find(0, { 0xB8 });
        const auto b = a ? find(*a, { 0x68 }) : std::nullopt;
        const auto c = b ? find(*b, { 0x8D, 0x86 }) : std::nullopt;
        if (a && b && c)
        {
            const uint32_t dstLen = static_cast<uint32_t>(stub[*a + 1] | stub[*a + 2] << 8 | stub[*a + 3] << 16 | stub[*a + 4] << 24);
            const uint32_t srcLen = static_cast<uint32_t>(stub[*b + 1] | stub[*b + 2] << 8 | stub[*b + 3] << 16 | stub[*b + 4] << 24);
            const uint32_t srcRva = static_cast<uint32_t>(stub[*c + 2] | stub[*c + 3] << 8 | stub[*c + 4] << 16 | stub[*c + 5] << 24);
            const auto     so     = rvaToOff(srcRva);
            if (so && *so + srcLen <= d.size() && dstLen <= text->vsize + 0x1000)
            {
                const auto code = lzss(d.data() + *so, srcLen, dstLen);
                if (code.size() == dstLen && text->va + code.size() <= mapSize)
                {
                    std::memcpy(img + text->va, code.data(), code.size());
                }
                else
                {
                    spdlog::warn("ashita: the game image's code did not unpack ({} of {} bytes)", code.size(), dstLen);
                }
            }
        }
    }
    s.image = { reinterpret_cast<uintptr_t>(img), mapSize };
    spdlog::info("ashita: game image {} mapped at {:#x} ({:#x} bytes) for memory scans", path.filename().string(), s.image.base, s.image.size);
}

ImageInfo imageLocked(State& s)
{
    if (!s.imageTried)
    {
        mapImage(s);
    }
    return s.image;
}

uintptr_t allocLocked(State& s, std::size_t size, const std::string& key)
{
    if (!key.empty())
    {
        if (const auto it = s.keyed.find(key); it != s.keyed.end())
        {
            return it->second;
        }
    }
    size = (std::max<std::size_t>(size, 16) + 15) & ~std::size_t{ 15 };
    Chunk* c = s.chunks.empty() ? nullptr : &s.chunks.back();
    if (!c || c->used + size > c->size)
    {
        const std::size_t n = std::max<std::size_t>(size, 1u << 20);
        void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED)
        {
            spdlog::error("ashita: no low memory for the memory stand-ins");
            return 0;
        }
        s.chunks.push_back({ reinterpret_cast<uintptr_t>(p), n, 0 });
        c = &s.chunks.back();
    }
    const uintptr_t a = c->base + c->used;
    c->used += size;
    if (!key.empty())
    {
        s.keyed[key] = a;
    }
    return a;
}

bool validLocked(const State& s, uintptr_t a, std::size_t n)
{
    if (a == 0 || a + n < a)
    {
        return false;
    }
    if (s.image.base && a >= s.image.base && a + n <= s.image.base + s.image.size)
    {
        return true;
    }
    for (const auto& c : s.chunks)
    {
        if (a >= c.base && a + n <= c.base + c.size)
        {
            return true;
        }
    }
    return false;
}

// The game functions addons call (System V ABI on x64: LuaJIT calls through the addon's own typedef, whose
// __cdecl / __fastcall it ignores here, so arguments arrive in order).
int32_t stubConfigGet(int32_t id)
{
    std::function<std::optional<int32_t>(int32_t)> get;
    {
        std::lock_guard lock(state().m);
        get = state().config.get;
    }
    const auto v = get ? get(id) : std::nullopt;
    return v ? *v : -1;
}

int32_t stubConfigSet(int32_t id, int32_t value)
{
    std::function<bool(int32_t, int32_t)> set;
    {
        std::lock_guard lock(state().m);
        set = state().config.set;
    }
    return set && set(id, value) ? 0 : -1;
}

// FsConfigSubject as the config addon declares it, laid out the way x64 LuaJIT reads that declaration: its uintptr_t
// members (vtable, callback list) are 8 bytes here, so key +8, value +12, type +16, PlayOnline name +20, min +28,
// max +32, default +36, callback list +40, flags +48 (the 32-bit game has them at +4 .. +0x28).
int32_t stubConfigEntry(int32_t /*self*/, int32_t /*unused*/, int32_t id)
{
    std::function<std::optional<ConfigEntry>(int32_t)> entry;
    {
        std::lock_guard lock(state().m);
        entry = state().config.entry;
    }
    const auto e = entry ? entry(id) : std::nullopt;
    if (!e)
    {
        return 0;
    }
    uintptr_t a = 0;
    {
        std::lock_guard lock(state().m);
        a = allocLocked(state(), 64, "config.entry." + std::to_string(id));
    }
    if (a == 0)
    {
        return 0;
    }
    auto* p = reinterpret_cast<uint8_t*>(a);
    std::memset(p, 0, 64);
    auto put = [p](std::size_t off, int32_t v) { std::memcpy(p + off, &v, 4); };
    put(8, id);
    put(12, e->value);
    put(16, e->type);
    put(28, e->min);
    put(32, e->max);
    put(36, e->def);
    return static_cast<int32_t>(a);
}

int32_t stubAutoOffline(int32_t value)
{
    std::function<void(int32_t)> f;
    {
        std::lock_guard lock(state().m);
        f = state().config.autoOffline;
    }
    if (f)
    {
        f(value);
    }
    return 0;
}

// The macro functions (MacroHooks). Strings handed back stay valid until a few calls later (ffi.string copies them).
template <class F>
F macroHook(F MacroHooks::*m)
{
    std::lock_guard lock(state().m);
    return state().macros.*m;
}

// Whether [p, p + n) is mapped memory (mincore fails with ENOMEM on a page that is not).
bool mapped(const void* p, std::size_t n)
{
    const auto page  = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const auto first = reinterpret_cast<uintptr_t>(p) & ~(page - 1);
    const auto last  = (reinterpret_cast<uintptr_t>(p) + n + page - 1) & ~(page - 1);
    std::vector<unsigned char> v((last - first) / page);
    return mincore(reinterpret_cast<void*>(first), last - first, v.data()) == 0;
}

const char* keepString(std::string s)
{
    static thread_local std::array<std::string, 4> ring;
    static thread_local std::size_t                next = 0;
    auto&                                          slot = ring[next++ % ring.size()];
    slot                                                = std::move(s);
    return slot.c_str();
}

bool stubMacroCanUse(uint32_t /*self*/)
{
    const auto f = macroHook(&MacroHooks::canUse);
    return f ? f() : true;
}

bool stubMacroClear(uint32_t /*self*/, uint32_t index)
{
    const auto f = macroHook(&MacroHooks::set);
    if (!f || index >= 20)
    {
        return false;
    }
    f(static_cast<int>(index), {}, {});
    return true;
}

const char* stubMacroName(uint32_t /*self*/, uint32_t index)
{
    const auto f = macroHook(&MacroHooks::name);
    return keepString(f && index < 20 ? f(static_cast<int>(index)) : std::string());
}

const char* stubMacroLine(uint32_t /*self*/, uint32_t index, uint32_t line)
{
    const auto f = macroHook(&MacroHooks::line);
    return keepString(f && index < 20 && line < 6 ? f(static_cast<int>(index), static_cast<int>(line)) : std::string());
}

void stubMacroRun(uint32_t /*self*/, uint8_t mod, uint8_t index)
{
    const auto f = macroHook(&MacroHooks::run);
    if (f && (mod == 1 || mod == 2) && index < 10)
    {
        f(mod - 1, index);
    }
}

int32_t stubMacroSetBook(uint32_t /*self*/, uint32_t book)
{
    const auto f = macroHook(&MacroHooks::setBook);
    return f && book < 40 && f(static_cast<int>(book)) ? 1 : 0;
}

int32_t stubMacroSetPage(uint32_t /*self*/, uint32_t raw)
{
    const auto f = macroHook(&MacroHooks::setPage);
    return f && raw / 2 < 10 && f(static_cast<int>(raw / 2)) ? 1 : 0;
}

// The game's macro record: this + 4 + 380 x index; the lines are 6 x 61 bytes, NUL-padded.
void stubMacroSet(uint32_t record, const char* title, const char* lines)
{
    const auto f   = macroHook(&MacroHooks::set);
    uintptr_t  obj = 0;
    {
        std::lock_guard lock(state().m);
        obj = state().macroObject;
    }
    if (!f || obj == 0 || record < obj + 4 || (record - obj - 4) % 380 != 0 || (record - obj - 4) / 380 >= 20)
    {
        return;
    }
    // The block is as long as the lines the addon gave (XIUI pads each to 61 bytes but sends fewer than six); a block
    // past its end is whatever follows the string, so reading stops at the first block that is not text followed by
    // NUL padding.
    std::vector<std::string> out;
    for (int k = 0; k < 6 && lines; ++k)
    {
        const char* l = lines + 61 * k;
        if (!mapped(l, 61)) // past the string there may be no memory at all
        {
            break;
        }
        const std::size_t len = strnlen(l, 61);
        bool              ok  = len < 61;
        for (std::size_t c = 0; c < len && ok; ++c)
        {
            const auto b = static_cast<unsigned char>(l[c]);
            ok           = b >= 0x20 || b == 0x1E || b == 0x1F || b == 0x07;
        }
        for (std::size_t c = len; c < 61 && ok; ++c)
        {
            ok = l[c] == 0;
        }
        if (!ok)
        {
            break;
        }
        out.emplace_back(l, len);
    }
    while (!out.empty() && out.back().empty())
    {
        out.pop_back();
    }
    f(static_cast<int>((record - obj - 4) / 380), title ? std::string(title, strnlen(title, 8)) : std::string(), out);
}

void nativeSet(std::string_view name, double value)
{
    std::function<void(std::string_view, double)> f;
    {
        std::lock_guard lock(state().m);
        f = state().native.set;
    }
    if (f)
    {
        f(name, value);
    }
}

void stubEventMessageKey(uint32_t /*self*/, int16_t key, int16_t /*unused*/)
{
    nativeSet("event.message.key", key);
}

void stubChime(int32_t id, float /*power*/, bool /*force*/)
{
    nativeSet("sound.chime", id);
}

// Queued for the next frame: prelude_scan.lua builds the packet with the current set and sends it as an addon's
// outgoing packet would go (the game queues it the same way).
uint8_t stubEquipEx(uint8_t isSubJob, uint16_t jobType, uint16_t index, uint8_t id)
{
    if (jobType != 0x1000 || index >= 20)
    {
        return 0; // only BLU's spell slots are known
    }
    std::lock_guard lock(state().m);
    state().equipEx.push_back({ isSubJob, jobType, index, id });
    return 1;
}

// The game's clock (game_time.h). The text a string stub returns lives until the next call, as the game's own static
// buffer (0x10630378) and the CRT's localtime one do.
uint32_t gameTimeNow()
{
    std::function<std::optional<uint32_t>()> get;
    {
        std::lock_guard lock(state().m);
        get = state().time.gameTime;
    }
    if (const auto t = get ? get() : std::nullopt)
    {
        return *t;
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return static_cast<uint32_t>(now - 1009810800);
}
uint32_t stubGameTimeGet()
{
    return gameTimeNow();
}
uint32_t stubGameTimeDiff(uint32_t t)
{
    return gametime::timeDiff(gameTimeNow(), t);
}
uint32_t stubGameWeekday(uint32_t t)
{
    return gametime::weekday(t);
}
uint32_t stubGameDay(uint32_t t)
{
    return gametime::day(t);
}
uint32_t stubGameMonth(uint32_t t)
{
    return gametime::month(t);
}
uint32_t stubGameYear(uint32_t t)
{
    return gametime::year(t);
}
uint32_t stubGameMinutes(uint32_t t)
{
    return gametime::minutes(t);
}
uint32_t stubGameHours(uint32_t t)
{
    return gametime::hours(t);
}
uint32_t stubGameMoon(uint32_t t)
{
    return gametime::moonPhase(t);
}
uint32_t stubGameMoonPercent(uint32_t t)
{
    return gametime::moonPercent(t);
}
uint32_t stubUnixTime()
{
    return static_cast<uint32_t>(std::time(nullptr));
}
const char* stubTimeString(uint32_t unixTime, uint8_t flags)
{
    static std::string              out;
    std::function<std::string(int)> text;
    {
        std::lock_guard lock(state().m);
        text = state().time.text;
    }
    out = gametime::timeString(unixTime, flags, gametime::Locale{}, text);
    return out.c_str();
}
gametime::Tm* stubLocalTime(uint32_t unixTime)
{
    static gametime::Tm tm;
    return gametime::localTime(unixTime, tm) ? &tm : nullptr;
}

// The game's target helpers (fancychat's targets.lua). Each returns a static block, as the game returns its own
// objects; null when there is nothing, as the game does.
void* stubBattleActor()
{
    // An XiAtelBuff (the game's actor): fancychat reads only its CHAR_ID at +0x74 (index, then server id).
    static std::array<uint8_t, 0x200> actor{};
    std::function<std::optional<std::pair<uint16_t, uint32_t>>()> get;
    {
        std::lock_guard lock(state().m);
        get = state().targets.battleTarget;
    }
    const auto bt = get ? get() : std::nullopt;
    if (!bt)
    {
        return nullptr;
    }
    actor.fill(0);
    const uint32_t index = bt->first;
    std::memcpy(actor.data() + 0x74, &index, 4);
    std::memcpy(actor.data() + 0x78, &bt->second, 4);
    return actor.data();
}
const char* stubMonsterName(uint16_t index)
{
    static std::array<char, 32> name{}; // the record: char name[28] + u32 id
    std::function<std::string(uint16_t)> get;
    {
        std::lock_guard lock(state().m);
        get = state().targets.zoneName;
    }
    const auto n = get ? get(index) : std::string();
    if (n.empty())
    {
        return nullptr;
    }
    name.fill(0);
    std::memcpy(name.data(), n.data(), std::min<std::size_t>(n.size(), 27));
    return name.data();
}
const char* stubLastTeller()
{
    static std::array<char, 16> name{}; // the game's teller list entries are 16 bytes
    std::function<std::string()> get;
    {
        std::lock_guard lock(state().m);
        get = state().targets.lastTeller;
    }
    const auto n = get ? get() : std::string();
    if (n.empty())
    {
        return nullptr;
    }
    name.fill(0);
    std::memcpy(name.data(), n.data(), std::min<std::size_t>(n.size(), 15));
    return name.data();
}
void* stubTrackingPos()
{
    return nullptr; // the client has no wide scan tracking yet: the game's answer while nothing is tracked
}

int luaTakeEquipEx(lua_State* L)
{
    std::vector<std::array<int32_t, 4>> q;
    {
        std::lock_guard lock(state().m);
        q.swap(state().equipEx);
    }
    lua_createtable(L, static_cast<int>(q.size()), 0);
    for (std::size_t i = 0; i < q.size(); ++i)
    {
        lua_createtable(L, 4, 0);
        for (int k = 0; k < 4; ++k)
        {
            lua_pushinteger(L, q[i][static_cast<std::size_t>(k)]);
            lua_rawseti(L, -2, k + 1);
        }
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// ---- Lua natives (__xi.scan_*) ----

int luaImage(lua_State* L)
{
    const auto i = gameImage();
    lua_pushnumber(L, static_cast<lua_Number>(i.base));
    lua_pushnumber(L, static_cast<lua_Number>(i.size));
    return 2;
}

int luaFind(lua_State* L)
{
    const char* pat   = luaL_checkstring(L, 1);
    const int   count = static_cast<int>(luaL_optinteger(L, 2, 0));
    lua_pushnumber(L, static_cast<lua_Number>(findPattern(pat, count)));
    return 1;
}

int luaAlloc(lua_State* L)
{
    const auto  n   = static_cast<std::size_t>(luaL_checknumber(L, 1));
    const char* key = luaL_optstring(L, 2, "");
    lua_pushnumber(L, static_cast<lua_Number>(lowAlloc(n, key)));
    return 1;
}

int luaValid(lua_State* L)
{
    const auto a = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    const auto n = static_cast<std::size_t>(luaL_optnumber(L, 2, 1));
    lua_pushboolean(L, validRange(a, n));
    return 1;
}

int luaStub(lua_State* L)
{
    const auto        a    = static_cast<uintptr_t>(luaL_checknumber(L, 1));
    const std::string name = luaL_checkstring(L, 2);
    const int         keep = static_cast<int>(luaL_optinteger(L, 3, 0));
    static const std::unordered_map<std::string, Stub> names = {
        { "config_get", Stub::ConfigGet },
        { "config_set", Stub::ConfigSet },
        { "config_entry", Stub::ConfigEntry },
        { "auto_offline", Stub::AutoOffline },
        { "macro_can_use", Stub::MacroCanUse },
        { "macro_clear", Stub::MacroClear },
        { "macro_name", Stub::MacroName },
        { "macro_line", Stub::MacroLine },
        { "macro_run", Stub::MacroRun },
        { "macro_set_book", Stub::MacroSetBook },
        { "macro_set_page", Stub::MacroSetPage },
        { "macro_set", Stub::MacroSet },
        { "event_message_key", Stub::EventMessageKey },
        { "chime", Stub::Chime },
        { "equipex", Stub::EquipEx },
        { "game_time", Stub::GameTimeGet },
        { "game_time_diff", Stub::GameTimeDiff },
        { "game_weekday", Stub::GameWeekday },
        { "game_day", Stub::GameDay },
        { "game_month", Stub::GameMonth },
        { "game_year", Stub::GameYear },
        { "game_minutes", Stub::GameMinutes },
        { "game_hours", Stub::GameHours },
        { "game_moon", Stub::GameMoon },
        { "game_moon_percent", Stub::GameMoonPercent },
        { "unix_time", Stub::UnixTime },
        { "time_string", Stub::TimeString },
        { "local_time", Stub::LocalTime },
        { "battle_actor", Stub::BattleActor },
        { "monster_name", Stub::MonsterName },
        { "last_teller", Stub::LastTeller },
        { "tracking_pos", Stub::TrackingPos },
    };
    const auto it = names.find(name);
    lua_pushboolean(L, it != names.end() && writeStub(a, it->second, keep));
    return 1;
}

int luaClientUi(lua_State* L)
{
    const auto ui = clientUi();
    lua_createtable(L, 0, 4);
    lua_pushstring(L, ui.topMenu.c_str());
    lua_setfield(L, -2, "top_menu");
    lua_pushboolean(L, ui.eventActive);
    lua_setfield(L, -2, "event");
    lua_pushboolean(L, ui.interfaceHidden);
    lua_setfield(L, -2, "hidden");
    lua_pushboolean(L, ui.chatExpanded);
    lua_setfield(L, -2, "chat_expanded");
    lua_pushboolean(L, ui.eventMessage);
    lua_setfield(L, -2, "event_message");
    // The network window's rates (bytes a second sent / received over the last 3 s; session.h netSendRate).
    const auto& w = managerOf(L).frame().world;
    lua_pushnumber(L, w ? w->netSendRate : 0);
    lua_setfield(L, -2, "send_rate");
    lua_pushnumber(L, w ? w->netRecvRate : 0);
    lua_setfield(L, -2, "recv_rate");
    return 1;
}

// The world's S2C 0x044 copies (session.h extendedJob), [0] main job / [1] support job, as strings.
int luaExtendedJob(lua_State* L)
{
    const auto  i = static_cast<std::size_t>(luaL_checkinteger(L, 1));
    const auto& w = managerOf(L).frame().world;
    if (!w || i > 1)
    {
        lua_pushnil(L);
        return 1;
    }
    const auto& b = w->extendedJob[i];
    lua_pushlstring(L, reinterpret_cast<const char*>(b.data()), b.size());
    return 1;
}

int luaNativeSupports(lua_State* L)
{
    const std::string name = luaL_checkstring(L, 1);
    std::function<bool(std::string_view)> f;
    {
        std::lock_guard lock(state().m);
        f = state().native.supports;
    }
    // XI_ASHITA_NATIVE=all (experiments, the smoke test): every setting counts as supported when the client gives none.
    static const bool all = std::getenv("XI_ASHITA_NATIVE") && std::string_view(std::getenv("XI_ASHITA_NATIVE")) == "all";
    lua_pushboolean(L, f ? f(name) : all);
    return 1;
}

int luaNativeGet(lua_State* L)
{
    const std::string name = luaL_checkstring(L, 1);
    std::function<std::optional<double>(std::string_view)> f;
    {
        std::lock_guard lock(state().m);
        f = state().native.get;
    }
    const auto v = f ? f(name) : std::nullopt;
    if (v)
    {
        lua_pushnumber(L, *v);
    }
    else
    {
        lua_pushnil(L);
    }
    return 1;
}

int luaNativeSet(lua_State* L)
{
    const std::string name  = luaL_checkstring(L, 1);
    const double      value = luaL_checknumber(L, 2);
    std::function<void(std::string_view, double)> f;
    {
        std::lock_guard lock(state().m);
        f = state().native.set;
    }
    if (f)
    {
        f(name, value);
    }
    return 0;
}

int luaMacroObject(lua_State* L)
{
    setMacroObject(static_cast<uintptr_t>(luaL_checknumber(L, 1)));
    return 0;
}

// Whether the client gives the macro palette (MacroHooks::name set), and whether a macro runs.
int luaMacros(lua_State* L)
{
    MacroHooks h;
    {
        std::lock_guard lock(state().m);
        h = state().macros;
    }
    static const bool all = std::getenv("XI_ASHITA_NATIVE") && std::string_view(std::getenv("XI_ASHITA_NATIVE")) == "all";
    lua_pushboolean(L, h.name ? true : all);
    lua_pushboolean(L, h.running && h.running());
    return 2;
}

int luaMacroStop(lua_State* /*L*/)
{
    if (const auto f = macroHook(&MacroHooks::stop))
    {
        f();
    }
    return 0;
}

int luaConfigGet(lua_State* L)
{
    lua_pushinteger(L, stubConfigGet(static_cast<int32_t>(luaL_checkinteger(L, 1))));
    return 1;
}

int luaConfigSet(lua_State* L)
{
    lua_pushinteger(L, stubConfigSet(static_cast<int32_t>(luaL_checkinteger(L, 1)), static_cast<int32_t>(luaL_checkinteger(L, 2))));
    return 1;
}

} // namespace

void setClientUi(ClientUi ui)
{
    std::lock_guard lock(state().m);
    state().ui = std::move(ui);
}

ClientUi clientUi()
{
    std::lock_guard lock(state().m);
    return state().ui;
}

void setConfigHooks(ConfigHooks hooks)
{
    std::lock_guard lock(state().m);
    state().config = std::move(hooks);
}

void setNativeHooks(NativeHooks hooks)
{
    std::lock_guard lock(state().m);
    state().native = std::move(hooks);
}

void setTargetHooks(TargetHooks hooks)
{
    std::lock_guard lock(state().m);
    state().targets = std::move(hooks);
}

void setTimeSource(TimeSource source)
{
    std::lock_guard lock(state().m);
    state().time = std::move(source);
}

void setMacroHooks(MacroHooks hooks)
{
    std::lock_guard lock(state().m);
    state().macros = std::move(hooks);
}

void setMacroObject(uintptr_t addr)
{
    std::lock_guard lock(state().m);
    state().macroObject = addr;
}

void setGameImage(const std::filesystem::path& ffxiMainDll)
{
    std::lock_guard lock(state().m);
    if (!state().imageTried)
    {
        state().dll = ffxiMainDll;
    }
}

ImageInfo gameImage()
{
    std::lock_guard lock(state().m);
    return imageLocked(state());
}

uintptr_t findPattern(std::string_view pattern, int count)
{
    std::lock_guard lock(state().m);
    auto& s = state();
    std::string key;
    for (const char c : pattern)
    {
        if (c != ' ')
        {
            key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    if (const auto it = s.found.find({ key, count }); it != s.found.end())
    {
        return it->second;
    }
    const auto img = imageLocked(s);
    uintptr_t  hit = 0;
    if (img.base && key.size() >= 2 && key.size() % 2 == 0)
    {
        std::vector<int> bytes; // -1 = any
        for (std::size_t i = 0; i < key.size(); i += 2)
        {
            if (key[i] == '?')
            {
                bytes.push_back(-1);
            }
            else
            {
                bytes.push_back(static_cast<int>(std::strtoul(key.substr(i, 2).c_str(), nullptr, 16)));
            }
        }
        const auto* p    = reinterpret_cast<const uint8_t*>(img.base);
        int         seen = 0;
        // The first byte that is not a wildcard anchors the search (memchr).
        const auto anchor = std::find_if(bytes.begin(), bytes.end(), [](int b) { return b >= 0; });
        const std::size_t ai = static_cast<std::size_t>(anchor - bytes.begin());
        if (anchor != bytes.end())
        {
            std::size_t i = 0;
            while (i + bytes.size() <= img.size)
            {
                const void* q = std::memchr(p + i + ai, *anchor, img.size - bytes.size() - i + 1);
                if (!q)
                {
                    break;
                }
                i = static_cast<std::size_t>(static_cast<const uint8_t*>(q) - p) - ai;
                bool ok = true;
                for (std::size_t k = 0; k < bytes.size() && ok; ++k)
                {
                    ok = bytes[k] < 0 || p[i + k] == bytes[k];
                }
                if (ok && seen++ == count)
                {
                    hit = img.base + i;
                    break;
                }
                ++i;
            }
        }
    }
    s.found[{ key, count }] = hit;
    return hit;
}

uintptr_t lowAlloc(std::size_t size, const std::string& key)
{
    std::lock_guard lock(state().m);
    return allocLocked(state(), size, key);
}

bool validRange(uintptr_t addr, std::size_t size)
{
    std::lock_guard lock(state().m);
    return validLocked(state(), addr, size);
}

bool writeStub(uintptr_t addr, Stub stub, int keep)
{
    uintptr_t target = 0;
    switch (stub)
    {
        case Stub::ConfigGet: target = reinterpret_cast<uintptr_t>(&stubConfigGet); break;
        case Stub::ConfigSet: target = reinterpret_cast<uintptr_t>(&stubConfigSet); break;
        case Stub::ConfigEntry: target = reinterpret_cast<uintptr_t>(&stubConfigEntry); break;
        case Stub::AutoOffline: target = reinterpret_cast<uintptr_t>(&stubAutoOffline); break;
        case Stub::MacroCanUse: target = reinterpret_cast<uintptr_t>(&stubMacroCanUse); break;
        case Stub::MacroClear: target = reinterpret_cast<uintptr_t>(&stubMacroClear); break;
        case Stub::MacroName: target = reinterpret_cast<uintptr_t>(&stubMacroName); break;
        case Stub::MacroLine: target = reinterpret_cast<uintptr_t>(&stubMacroLine); break;
        case Stub::MacroRun: target = reinterpret_cast<uintptr_t>(&stubMacroRun); break;
        case Stub::MacroSetBook: target = reinterpret_cast<uintptr_t>(&stubMacroSetBook); break;
        case Stub::MacroSetPage: target = reinterpret_cast<uintptr_t>(&stubMacroSetPage); break;
        case Stub::MacroSet: target = reinterpret_cast<uintptr_t>(&stubMacroSet); break;
        case Stub::EventMessageKey: target = reinterpret_cast<uintptr_t>(&stubEventMessageKey); break;
        case Stub::Chime: target = reinterpret_cast<uintptr_t>(&stubChime); break;
        case Stub::EquipEx: target = reinterpret_cast<uintptr_t>(&stubEquipEx); break;
        case Stub::GameTimeGet: target = reinterpret_cast<uintptr_t>(&stubGameTimeGet); break;
        case Stub::GameTimeDiff: target = reinterpret_cast<uintptr_t>(&stubGameTimeDiff); break;
        case Stub::GameWeekday: target = reinterpret_cast<uintptr_t>(&stubGameWeekday); break;
        case Stub::GameDay: target = reinterpret_cast<uintptr_t>(&stubGameDay); break;
        case Stub::GameMonth: target = reinterpret_cast<uintptr_t>(&stubGameMonth); break;
        case Stub::GameYear: target = reinterpret_cast<uintptr_t>(&stubGameYear); break;
        case Stub::GameMinutes: target = reinterpret_cast<uintptr_t>(&stubGameMinutes); break;
        case Stub::GameHours: target = reinterpret_cast<uintptr_t>(&stubGameHours); break;
        case Stub::GameMoon: target = reinterpret_cast<uintptr_t>(&stubGameMoon); break;
        case Stub::GameMoonPercent: target = reinterpret_cast<uintptr_t>(&stubGameMoonPercent); break;
        case Stub::UnixTime: target = reinterpret_cast<uintptr_t>(&stubUnixTime); break;
        case Stub::TimeString: target = reinterpret_cast<uintptr_t>(&stubTimeString); break;
        case Stub::LocalTime: target = reinterpret_cast<uintptr_t>(&stubLocalTime); break;
        case Stub::BattleActor: target = reinterpret_cast<uintptr_t>(&stubBattleActor); break;
        case Stub::MonsterName: target = reinterpret_cast<uintptr_t>(&stubMonsterName); break;
        case Stub::LastTeller: target = reinterpret_cast<uintptr_t>(&stubLastTeller); break;
        case Stub::TrackingPos: target = reinterpret_cast<uintptr_t>(&stubTrackingPos); break;
    }
    // Kept bytes are followed by 4 zero bytes: on x64 an addon's `uint32_t**` cast reads a kept address as 8 bytes.
    keep = std::clamp(keep, 0, 100);
    const std::size_t lead = keep > 0 ? 2u + static_cast<std::size_t>(keep) + 4u : 0u;
    const std::size_t len  = lead + 12; // movabs rax, imm64; jmp rax
    std::lock_guard lock(state().m);
    auto& s = state();
    if (!validLocked(s, addr, len))
    {
        return false;
    }
    if (s.image.base && addr >= s.image.base && addr < s.image.base + s.image.size)
    {
        const auto page  = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
        const auto first = addr & ~(page - 1);
        const auto last  = (addr + len + page - 1) & ~(page - 1);
        if (mprotect(reinterpret_cast<void*>(first), last - first, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        {
            return false;
        }
    }
    auto* p = reinterpret_cast<uint8_t*>(addr);
    if (keep > 0)
    {
        p[0] = 0xEB; // jmp short over the kept bytes and the zero pad
        p[1] = static_cast<uint8_t>(keep + 4);
        std::memset(p + 2 + keep, 0, 4);
    }
    uint8_t* q = p + lead;
    q[0] = 0x48;
    q[1] = 0xB8; // movabs rax, target
    std::memcpy(q + 2, &target, 8);
    q[10] = 0xFF;
    q[11] = 0xE0; // jmp rax
    __builtin___clear_cache(reinterpret_cast<char*>(p), reinterpret_cast<char*>(p + len));
    return true;
}

void openScan(lua_State* L)
{
    const luaL_Reg fns[] = {
        { "scan_image", luaImage },
        { "scan_find", luaFind },
        { "scan_alloc", luaAlloc },
        { "scan_valid", luaValid },
        { "scan_stub", luaStub },
        { "client_ui", luaClientUi },
        { "config_get", luaConfigGet },
        { "config_set", luaConfigSet },
        { "extended_job", luaExtendedJob },
        { "native_supports", luaNativeSupports },
        { "native_get", luaNativeGet },
        { "native_set", luaNativeSet },
        { "macro_object", luaMacroObject },
        { "macros", luaMacros },
        { "macro_stop", luaMacroStop },
        { "take_equipex", luaTakeEquipEx },
        { nullptr, nullptr },
    };
    for (const auto* f = fns; f->name; ++f)
    {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
}

} // namespace xi::ashita
