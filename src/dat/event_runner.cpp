// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/event_runner.h"

#include "dat/event.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace xi::dat
{

namespace
{

uint32_t rd32(const std::vector<std::byte>& d, std::size_t at)
{
    uint32_t v = 0;
    if (at + 4 <= d.size())
    {
        std::memcpy(&v, d.data() + at, 4);
    }
    return v;
}

uint16_t rd16(const std::vector<std::byte>& d, std::size_t at)
{
    uint16_t v = 0;
    if (at + 2 <= d.size())
    {
        std::memcpy(&v, d.data() + at, 2);
    }
    return v;
}

constexpr float kTwoPi = 6.2831853f;

// Op 0x45's DAT: 0x77F0 + f(n) (CODE 0x100b4280). The sibling ops use fixed bases without f.
uint32_t shotFile(uint32_t n)
{
    const uint32_t f = n < 300 ? n : n < 600 ? n + 0x6551 : n < 900 ? n + 0x9ADB : n + 0x12FE5;
    return 0x77F0 + f;
}

// Start / stop / wait scheduler ops and their file bases (CODE 0x100b4400.., 0x100b4810.., 0x100b4890..).
struct ShotOp
{
    uint8_t  start, stop, wait;
    uint32_t base; // 0 = op 0x45's f(n)
};
constexpr ShotOp kShotOps[] = {
    { 0x45, 0x52, 0x55, 0 },       { 0x62, 0xA1, 0xA0, 0x1394 },  { 0x9F, 0xA3, 0xA2, 0xC7EF },  { 0xBB, 0xBD, 0xBC, 0xDD6D },
    { 0xC5, 0xC7, 0xC6, 0x1071B }, { 0xCD, 0xCF, 0xCE, 0x11323 }, { 0xD0, 0xD2, 0xD1, 0x11423 }, { 0xD5, 0xD7, 0xD6, 0x19031 },
};

} // namespace

int EventBlock::entryFor(uint16_t eventId) const
{
    for (std::size_t i = 0; i < entryId.size(); ++i)
    {
        if (entryId[i] == eventId)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

EventFile EventFile::load(const DatIndex& index, uint16_t zone)
{
    EventFile  f;
    const auto d = index.read(EventScript::fileFor(zone));
    // u32 N, u32 size[N], then blocks: u32 id, u16 n (4), u16 entryOff[n], u16 entryEventId[n], u16 m (4),
    // u32 const[m], u32 codeLen, code (docs/event-format.md §1).
    const uint32_t n   = rd32(d, 0);
    std::size_t    off = 4 + 4u * n;
    for (uint32_t b = 0; b < n && off < d.size(); ++b)
    {
        EventBlock blk;
        blk.id                 = rd32(d, off);
        const uint16_t entries = rd16(d, off + 4);
        for (uint16_t i = 0; i < entries; ++i)
        {
            blk.entryOff.push_back(rd16(d, off + 8 + 2u * i));
            blk.entryId.push_back(rd16(d, off + 8 + 2u * entries + 2u * i));
        }
        const std::size_t mo = off + 8 + 4u * entries;
        const uint16_t    m  = rd16(d, mo);
        for (uint16_t k = 0; k < m; ++k)
        {
            blk.consts.push_back(rd32(d, mo + 4 + 4u * k));
        }
        const std::size_t co  = mo + 4 + 4u * m;
        const uint32_t    len = rd32(d, co);
        for (uint32_t k = 0; k < len && co + 4 + k < d.size(); ++k)
        {
            blk.code.push_back(static_cast<uint8_t>(d[co + 4 + k]));
        }
        f.blocks.push_back(std::move(blk));
        off += rd32(d, 4 + 4u * b);
    }
    return f;
}

const EventBlock* EventFile::block(uint32_t id) const
{
    for (const auto& b : blocks)
    {
        if (b.id == id)
        {
            return &b;
        }
    }
    return nullptr;
}

EventRunner::EventRunner(std::shared_ptr<const EventFile> file, uint32_t owner, uint16_t eventId, const std::array<uint32_t, 8>& params, EventHost& host)
    : file_(std::move(file))
    , host_(host)
    , eventId_(eventId)
{
    for (std::size_t i = 0; i < 8; ++i)
    {
        globals_[2 + i] = params[i];
    }
    // Participants (CODE 0x100aedd0): every block with an entry for the event. The owner may answer through its
    // 0xFFFE wildcard. INFERRED deviation: other blocks join only on an exact match (wildcards would pull every NPC
    // with a generic entry into each dialog).
    for (const auto& b : file_->blocks)
    {
        int entry = b.entryFor(eventId);
        if (entry < 0 && b.id == owner)
        {
            entry = b.entryFor(0xFFFE);
        }
        if (entry < 0 || (b.id >= 0x7FFFFFF1 && b.id <= 0x7FFFFFF5) || (b.id >= 0x7FFFFFC1 && b.id <= 0x7FFFFFD1))
        {
            continue; // party / alliance members' blocks: not run
        }
        Actor a;
        a.block     = &b;
        a.entity    = b.id == 0x7FFFFFF0 ? host_.player() : b.id;
        a.mainEntry = entry;
        valid_ |= b.id == owner;
        actors_.push_back(a);
    }
    for (auto& a : actors_)
    {
        if (!host_.exists(a.entity))
        {
            host_.request(a.entity);
        }
    }
    if (!valid_)
    {
        finished_ = true;
    }
}

void EventRunner::choose(int option)
{
    menuOpen_ = false;
    if (option < 0)
    {
        cancelled_  = true;
        globals_[0] = 0xFE;
        return;
    }
    globals_[0] = static_cast<uint32_t>(option);
}

void EventRunner::updated(const std::array<uint32_t, 8>& params)
{
    updatePending_ = false;
    for (std::size_t i = 0; i < 8; ++i)
    {
        globals_[2 + i] = params[i];
    }
}

EventRunner::Actor* EventRunner::actorFor(uint32_t entity)
{
    for (auto& a : actors_)
    {
        if (a.entity == entity && a.started)
        {
            return &a;
        }
    }
    return nullptr;
}

uint32_t EventRunner::entityOperand(const Actor& a, uint32_t v) const
{
    // CODE 0x100afa30: the player; party members (not run); this block's own entity; else a UniqueNo.
    if (v == 0x7FFFFFF0 || v == 0x7FFFFFC0 || v == 0x7FFFFFF9)
    {
        return host_.player();
    }
    if (v == 0x7FFFFFF8 || (v >> 24) == 0)
    {
        return a.entity;
    }
    if ((v >= 0x7FFFFFF1 && v <= 0x7FFFFFF5) || (v >= 0x7FFFFFC1 && v <= 0x7FFFFFD1))
    {
        return 0;
    }
    return v;
}

uint32_t EventRunner::read(const Actor& a, uint16_t ref) const
{
    if (ref & 0x8000)
    {
        const std::size_t k = ref & 0x7FFF;
        return k < a.block->consts.size() ? a.block->consts[k] : 0;
    }
    if (ref < a.locals.size())
    {
        return a.locals[ref];
    }
    if (ref >= 0x1000 && ref < 0x1000 + globals_.size())
    {
        return globals_[ref - 0x1000];
    }
    return 0;
}

void EventRunner::write(Actor& a, uint16_t ref, uint32_t value)
{
    if (ref < a.locals.size())
    {
        a.locals[ref] = value;
    }
    else if (ref >= 0x1000 && ref < 0x1000 + globals_.size())
    {
        globals_[ref - 0x1000] = value;
    }
}

int EventRunner::length(const EventBlock& b, std::size_t at) const
{
    return eventOpLength(b.code, at);
}

bool EventRunner::startThread(Actor& from, uint32_t entity, int prio, int entry)
{
    // CODE 0x100b3650: already running that entry -> done; no free slot -> retry next frame.
    Actor* t = actorFor(entity);
    if (!t || entry < 0 || static_cast<std::size_t>(entry) >= t->block->entryOff.size())
    {
        return true; // nobody to run it: carry on
    }
    for (const auto& s : t->slots)
    {
        if (s.prio != 0xFF && s.entry == entry)
        {
            return true;
        }
    }
    for (auto& s : t->slots)
    {
        if (s.prio == 0xFF)
        {
            s       = Slot{};
            s.prio  = prio;
            s.pc    = t->block->entryOff[static_cast<std::size_t>(entry)];
            s.entry = entry;
            if (trace_)
            {
                spdlog::info("event: {:#x} starts entry {} on {:#x} (prio {:#x})", from.entity, entry, entity, prio);
            }
            return true;
        }
    }
    return false;
}

void EventRunner::startActor(Actor& a)
{
    a.started = true;
    // CODE 0x100bcd00: entry 0 first at priority 0x80 (an init thread) until it yields, then the event's entry at 0x10.
    if (a.mainEntry != 0 && !a.block->entryOff.empty())
    {
        Slot& s = a.slots[0];
        s       = Slot{};
        s.prio  = 0x80;
        s.pc    = a.block->entryOff[0];
        s.entry = 0;
        run(a, s, 0.0f);
    }
    Slot& s = a.slots[0];
    s       = Slot{};
    s.prio  = 0x10;
    s.pc    = a.block->entryOff[static_cast<std::size_t>(a.mainEntry)];
    s.entry = a.mainEntry;
}

void EventRunner::tick(float ticks)
{
    if (finished_)
    {
        return;
    }
    // Wait for the actors (up to 2 s; the game waits as long as it takes, CODE 0x100aedd0 states 6-8).
    bool ready = true;
    for (const auto& a : actors_)
    {
        ready &= a.started || host_.exists(a.entity);
    }
    if (!ready && waitStart_ < 120.0f)
    {
        waitStart_ += ticks;
        return;
    }
    for (auto& a : actors_)
    {
        if (!a.started && host_.exists(a.entity))
        {
            startActor(a);
            if (finished_)
            {
                return;
            }
        }
    }
    for (auto& a : actors_)
    {
        if (!a.started || a.suspended)
        {
            continue;
        }
        Slot* best = nullptr;
        for (auto& s : a.slots)
        {
            if (s.prio != 0xFF && (!best || s.prio < best->prio))
            {
                best = &s;
            }
        }
        if (best)
        {
            run(a, *best, ticks);
        }
        if (finished_)
        {
            return;
        }
    }
}

void EventRunner::run(Actor& a, Slot& s, float ticks)
{
    const auto& code = a.block->code;
    auto u8          = [&](std::size_t i) -> uint8_t { return i < code.size() ? code[i] : 0; };
    auto u16         = [&](std::size_t i) -> uint16_t { return static_cast<uint16_t>(u8(i) | u8(i + 1) << 8); };
    auto u32         = [&](std::size_t i) -> uint32_t { return static_cast<uint32_t>(u16(i)) | static_cast<uint32_t>(u16(i + 2)) << 16; };
    auto endThread   = [&] { s = Slot{}; };
    // A wait of `n` ticks: set on the first visit, counted down on later frames.
    auto waitTicks = [&](float n, std::size_t next) -> bool {
        if (s.wait < 0)
        {
            s.wait = n;
            return false;
        }
        s.wait -= ticks;
        if (s.wait > 0)
        {
            return false;
        }
        s.wait = -1;
        s.pc   = next;
        return true;
    };
    for (int budget = 0; budget < 4096; ++budget)
    {
        if (s.pc >= code.size())
        {
            endThread();
            return;
        }
        const std::size_t at  = s.pc;
        const uint8_t     op  = u8(at);
        auto              ref = [&](std::size_t o) { return u16(at + o); };
        auto              val = [&](std::size_t o) { return read(a, ref(o)); };
        auto              pos = [&](std::size_t o) {
            // x, z, y operand order; int / 1000 (CODE 0x100b5d00).
            return EventVec{ static_cast<float>(static_cast<int32_t>(val(o))) * 0.001f, static_cast<float>(static_cast<int32_t>(val(o + 4))) * 0.001f,
                             static_cast<float>(static_cast<int32_t>(val(o + 2))) * 0.001f };
        };
        auto arith = [&](auto f) {
            write(a, ref(1), f(val(1), val(3)));
            s.pc = at + 5;
        };
        // XI_EVENT_TRACE=1 XI_EVENT_OPS=1 XI_LOG=debug: every op with its first bytes.
        static const bool opTrace = std::getenv("XI_EVENT_OPS") != nullptr;
        if (trace_ && opTrace)
        {
            spdlog::debug("event: {:#x} entry {} pc {:#x} op {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}", a.entity, s.entry, at, op, u8(at + 1), u8(at + 2), u8(at + 3), u8(at + 4), u8(at + 5), u8(at + 6), u8(at + 7));
        }
        switch (op)
        {
            // --- control and arithmetic (event-format §3) ---
            case 0x00: endThread(); return;
            case 0x26: endThread(); return;
            case 0x01: s.pc = u16(at + 1); continue;
            case 0x02:
            {
                const uint32_t x = val(1), y = val(3);
                bool           c = false;
                switch (u8(at + 5) & 0x0F)
                {
                    case 0: c = x == y; break;
                    case 1: c = x != y; break;
                    case 2: c = x > y; break;
                    case 3: c = x < y; break;
                    case 4: c = x >= y; break;
                    case 5: c = x <= y; break;
                    case 6:
                    case 9: c = (x & y) != 0; break;
                    case 7: c = x != y; break;
                    case 8: c = (x | y) != 0; break;
                    case 10: c = (y & ~x) != 0; break;
                    default: c = false; break;
                }
                s.pc = c ? at + 8 : u16(at + 6);
                continue;
            }
            case 0x03: arith([](uint32_t, uint32_t y) { return y; }); continue;
            case 0x05: write(a, ref(1), 1); s.pc = at + 3; continue;
            case 0x06: write(a, ref(1), 0); s.pc = at + 3; continue;
            case 0x07: arith([](uint32_t x, uint32_t y) { return x + y; }); continue;
            case 0x08: arith([](uint32_t x, uint32_t y) { return x - y; }); continue;
            case 0x09: arith([](uint32_t x, uint32_t y) { return x | (1u << (y & 31)); }); continue;
            case 0x0A: arith([](uint32_t x, uint32_t y) { return x & ~(1u << (y & 31)); }); continue;
            case 0x0B: write(a, ref(1), val(1) + 1); s.pc = at + 3; continue;
            case 0x0C: write(a, ref(1), val(1) - 1); s.pc = at + 3; continue;
            case 0x0D: arith([](uint32_t x, uint32_t y) { return x & y; }); continue;
            case 0x0E: arith([](uint32_t x, uint32_t y) { return x | y; }); continue;
            case 0x0F: arith([](uint32_t x, uint32_t y) { return x ^ y; }); continue;
            case 0x10: arith([](uint32_t x, uint32_t y) { return x << (y & 31); }); continue;
            case 0x11: arith([](uint32_t x, uint32_t y) { return x >> (y & 31); }); continue;
            case 0x12:
                rng_ = rng_ * 1103515245u + 12345u;
                write(a, ref(1), rng_ >> 8);
                s.pc = at + 3;
                continue;
            case 0x13:
                rng_ = rng_ * 1103515245u + 12345u;
                write(a, ref(1), (rng_ >> 8) % (val(3) + 1));
                s.pc = at + 5;
                continue;
            case 0x14: arith([](uint32_t x, uint32_t y) { return x * y; }); continue;
            case 0x15: arith([](uint32_t x, uint32_t y) { return y ? x / y : 0u; }); continue;
            case 0x19:
            {
                const uint32_t x = val(1), y = val(3);
                write(a, ref(1), y);
                write(a, ref(3), x);
                s.pc = at + 5;
                continue;
            }
            case 0x1A:
                if (s.stack.size() >= 8)
                {
                    endThread();
                    return;
                }
                s.stack.push_back(at + 3);
                s.pc = u16(at + 1);
                continue;
            case 0x1B:
                if (s.stack.empty())
                {
                    endThread();
                    return;
                }
                s.pc = s.stack.back();
                s.stack.pop_back();
                continue;
            case 0x3E:
            {
                const uint32_t b    = val(3);
                const uint32_t word = read(a, static_cast<uint16_t>(ref(1) + (b >> 5)));
                s.pc                = (word >> (b & 31)) & 1 ? at + 7 : u16(at + 5);
                continue;
            }

            // --- dialog ---
            case 0x1D:
                host_.message(val(1), a.entity);
                messageOpen_ = true;
                s.pc         = at + 3;
                continue;
            case 0x48:
                host_.message(val(1), 0);
                messageOpen_ = true;
                s.pc         = at + 3;
                continue;
            case 0x2B:
                host_.message(val(5), entityOperand(a, u32(at + 1)));
                messageOpen_ = true;
                s.pc         = at + 7;
                continue;
            case 0x49:
                host_.message(val(5), 0);
                messageOpen_ = true;
                s.pc         = at + 7;
                continue;
            case 0xB0:
                host_.message(val(10), 0);
                messageOpen_ = true;
                s.pc         = at + 12;
                continue;
            case 0x23:
                if (messageOpen_)
                {
                    return; // the window is still open
                }
                s.pc = at + 1;
                continue;
            case 0x24:
                host_.menu(val(1), val(5), val(3), a.entity);
                menuOpen_ = true;
                s.pc      = at + 7;
                continue;
            case 0x25:
                if (menuOpen_)
                {
                    return;
                }
                s.pc = at + 1;
                if (cancelled_)
                {
                    finished_ = true;
                    return;
                }
                continue;
            case 0x43:
                if (u8(at + 1) == 0)
                {
                    updatePending_ = true; // before: a host may answer at once
                    host_.update(globals_[1]);
                }
                else if (updatePending_)
                {
                    return;
                }
                s.pc = at + 2;
                continue;
            case 0x21:
                s.pc      = at + 1;
                finished_ = true;
                return;

            // --- threads (CODE 0x100b3860 / 0x100b3f20 / 0x100b41b0 / 0x100b3510) ---
            case 0x27:
                if (!startThread(a, entityOperand(a, u32(at + 2)), u8(at + 1), u8(at + 6)))
                {
                    return; // no free slot: retry
                }
                s.pc = at + 7;
                continue;
            case 0x29:
            {
                const uint32_t ent   = entityOperand(a, u32(at + 2));
                const int      entry = u8(at + 6);
                if (s.sub == 0)
                {
                    // CODE 0x100b3e87: start result 0 (that entry already runs there) carries on at once, without
                    // waiting; only a thread this op started is waited for. (The Moogle's event 30000: entry 30 waits
                    // on 40, which then waits on the running 30 - waiting there never ends.)
                    bool already = false;
                    if (const Actor* t = actorFor(ent))
                    {
                        for (const auto& o : t->slots)
                        {
                            already |= o.prio != 0xFF && o.entry == entry;
                        }
                    }
                    if (already)
                    {
                        s.pc = at + 7;
                        continue;
                    }
                    if (!startThread(a, ent, u8(at + 1), entry))
                    {
                        return;
                    }
                    s.sub = 1;
                    return; // the thread runs from the next frame
                }
                const Actor* t       = actorFor(ent);
                bool         running = false;
                for (const auto& o : t ? t->slots : std::array<Slot, 16>{})
                {
                    running |= o.prio != 0xFF && o.entry == entry;
                }
                if (running)
                {
                    return;
                }
                s.sub = 0;
                s.pc  = at + 7;
                continue;
            }
            case 0x2A:
            {
                const Actor* t    = actorFor(entityOperand(a, u32(at + 2)));
                const int    prio = u8(at + 1);
                bool         busy = false;
                if (t && t != &a)
                {
                    for (const auto& o : t->slots)
                    {
                        busy |= o.prio != 0xFF && o.prio <= prio;
                    }
                }
                if (busy)
                {
                    return;
                }
                s.pc = at + 6;
                continue;
            }
            case 0x2F:
                if (Actor* t = actorFor(entityOperand(a, u32(at + 2))))
                {
                    t->suspended = u8(at + 1) != 0;
                }
                s.pc = at + 6;
                if (a.suspended)
                {
                    return; // suspended itself
                }
                continue;

            // --- waits (CODE 0x100b07a0, 0x100b0720, 0x100b7c80, 0x100b8dc0) ---
            case 0x1C:
                if (!waitTicks(static_cast<float>(val(1)), at + 3))
                {
                    return;
                }
                continue;
            case 0x6F:
                if (!waitTicks(16.0f, at + 1))
                {
                    return;
                }
                continue;
            case 0x70:
                if (host_.motionBusy(a.entity))
                {
                    return;
                }
                s.pc = at + 1;
                continue;
            case 0x76:
                if (host_.motionBusy(entityOperand(a, u32(at + 1))))
                {
                    return;
                }
                s.pc = at + 5;
                continue;
            case 0x80:
            {
                const uint32_t ent = entityOperand(a, u32(at + 1));
                if (!host_.exists(ent))
                {
                    if (!waitTicks(120.0f, at + 5))
                    {
                        return; // up to 2 s, then on
                    }
                    continue;
                }
                s.wait = -1;
                s.pc   = at + 5;
                continue;
            }

            // --- placement and movement (CODE 0x100b5d00, 0x100b5b80, 0x100b5ea0, 0x100b67a0, 0x100b6630) ---
            case 0x37:
                host_.place(a.entity, pos(1), static_cast<float>(val(7) & 0xFFF) * kTwoPi / 4096.0f, !a.noSnap);
                s.pc = at + 9;
                continue;
            case 0x36:
                host_.place(a.entity, pos(1), std::nullopt, !a.noSnap);
                s.pc = at + 7;
                continue;
            case 0xBA:
            {
                const uint32_t ent = entityOperand(a, u32(at + 1));
                const Actor*   t   = actorFor(ent);
                host_.place(ent, pos(5), static_cast<float>(val(11) & 0xFFF) * kTwoPi / 4096.0f, !(t && t->noSnap));
            }
                s.pc = at + 13;
                continue;
            case 0x4B:
                host_.face(entityOperand(a, u32(at + 1)), static_cast<float>(val(5) & 0xFFF) * kTwoPi / 4096.0f);
                s.pc = at + 7;
                continue;
            case 0x4A:
            case 0x1E:
            {
                // 0x4A: ent1 turns to ent2; 0x1E: this actor turns to ent.
                const uint32_t who = op == 0x4A ? entityOperand(a, u32(at + 1)) : a.entity;
                const uint32_t to  = entityOperand(a, u32(at + (op == 0x4A ? 5 : 1)));
                const auto     p   = host_.position(who), q = host_.position(to);
                if (who != to && (q.x != p.x || q.z != p.z))
                {
                    host_.face(who, std::atan2(-(q.z - p.z), q.x - p.x));
                }
                host_.lookAt(who, to, 6); // CODE 0x100b2cee / 0x100b677d
                s.pc = at + (op == 0x4A ? 9 : 5);
                continue;
            }
            case 0x32:
                a.speed = static_cast<float>(val(1)) * 0.1f;
                s.pc    = at + 3;
                continue;
            case 0x1F:
            case 0x5A:
            case 0x31:
            {
                // Sub 0: set the target (0x31 also a timeout in ticks); sub 1: walk there, a step a frame, facing the
                // way (CODE 0x100b2dd0 / 0x100b3120 / 0x100b5560). 0x1F stays on the ground; 0x5A / 0x31 move in 3-D.
                if (u8(at + 1) == 0)
                {
                    s.target  = pos(2);
                    s.timeout = op == 0x31 ? static_cast<float>(val(8)) : 0.0f;
                    s.pc      = at + (op == 0x31 ? 10 : 8);
                    continue;
                }
                const auto  p    = host_.position(a.entity);
                const float dx   = s.target.x - p.x, dz = s.target.z - p.z, dy = s.target.y - p.y;
                const float dist = op == 0x1F ? std::hypot(dx, dz) : std::sqrt(dx * dx + dy * dy + dz * dz);
                const float step = a.speed * ticks / 60.0f;
                if (op == 0x31 && s.timeout > 0)
                {
                    s.timeout -= ticks;
                }
                if (dist <= step || (op == 0x31 && s.timeout <= 0 && ticks > 0))
                {
                    host_.place(a.entity, s.target, std::nullopt, op == 0x1F && !a.noSnap);
                    s.pc = at + 2;
                    continue;
                }
                const float k = dist > 0 ? step / dist : 0.0f;
                host_.place(a.entity, { p.x + dx * k, op == 0x1F ? s.target.y : p.y + dy * k, p.z + dz * k }, std::atan2(-dz, dx), op == 0x1F && !a.noSnap);
                host_.walking(a.entity, a.speed);
                return;
            }
            case 0x4E:
                host_.hide(entityOperand(a, u32(at + 2)), u8(at + 1) != 0);
                s.pc = at + 6;
                continue;
            case 0x22:
                host_.hide(a.entity, u8(at + 1) != 0);
                s.pc = at + 2;
                continue;

            // --- motions (CODE 0x100b5140, 0x100b71f0, 0x100b4b50, 0x100b4d40, 0x100b7110) ---
            case 0x5B:
            case 0x66:
            {
                // File n: 0x5B a fixed DAT (n < 0x200 -> 0x7D68 + n ...), 0x66 per actor race (not resolved here: -1).
                const uint32_t n    = val(1);
                const int      file = op == 0x66 ? -1 : static_cast<int>(n < 0x200 ? 0x7D68 + n : n < 0x400 ? 0xBFEF + n : n < 0x800 ? 0xDC19 + n : n < 0xC00 ? 0xE95B + n : 0x10323 + n);
                host_.motion(entityOperand(a, u32(at + 3)), entityOperand(a, u32(at + 7)), u32(at + 11), file, op == 0x66 ? static_cast<int>(n) : -1);
                s.pc = at + 15;
                return; // the game yields once while the file loads
            }
            case 0x5F:
            {
                const uint8_t sub = u8(at + 1);
                const int     len = length(*a.block, at);
                if (sub >= 3 && sub <= 6)
                {
                    const uint32_t n    = val(2);
                    const bool     race = (sub & 1) == 0; // 0x5F 04 / 06 as 0x66, 03 / 05 as 0x5B
                    const int      file = race ? -1 : static_cast<int>(n < 0x200 ? 0x7D68 + n : n < 0x400 ? 0xBFEF + n : n < 0x800 ? 0xDC19 + n : n < 0xC00 ? 0xE95B + n : 0x10323 + n);
                    host_.motion(entityOperand(a, u32(at + 4)), entityOperand(a, u32(at + 8)), u32(at + 12), file, race ? static_cast<int>(n) : -1);
                }
                else if (sub == 7 && host_.motionBusy(entityOperand(a, u32(at + 2))))
                {
                    return;
                }
                s.pc = at + static_cast<std::size_t>(std::max(len, 2));
                continue;
            }
            case 0x2C:
                host_.motion(entityOperand(a, u32(at + 1)), entityOperand(a, u32(at + 5)), u32(at + 9), -1, -1);
                s.pc = at + 13;
                continue;
            case 0x53:
            {
                const uint32_t ent = entityOperand(a, u32(at + 1));
                if (s.sub == 0)
                {
                    host_.motion(ent, entityOperand(a, u32(at + 5)), u32(at + 9), -1, -1);
                    s.sub = 1;
                    return;
                }
                if (host_.motionBusy(ent))
                {
                    return;
                }
                s.sub = 0;
                s.pc  = at + 13;
                continue;
            }
            case 0x5E:
                host_.idle(a.entity, u32(at + 1));
                s.pc = at + 5;
                continue;

            // --- camera mode, sound, music (CODE 0x100b6370, 0x100b6d60, 0x100b6f70) ---
            case 0x46:
                host_.cameraMode(u8(at + 1));
                s.pc = at + static_cast<std::size_t>(std::max(length(*a.block, at), 2));
                continue;
            case 0x5C:
            {
                const uint8_t sub = u8(at + 1);
                if (sub <= 7 || (sub >= 0x80 && sub <= 0x87))
                {
                    host_.music(sub & 7, static_cast<uint16_t>(val(2)));
                }
                else if (sub == 0xA0 || sub == 0xA1)
                {
                    host_.sound(val(2));
                }
                const int len = length(*a.block, at);
                s.pc          = at + static_cast<std::size_t>(len > 0 ? len : 2);
                continue;
            }
            case 0x5D:
                host_.sound(val(1));
                s.pc = at + 5;
                continue;

            // --- head look-at (CODE 0x100b8740 / 0x100b8810 / 0x100b8930) ---
            case 0x79:
            {
                const uint8_t  sub = u8(at + 1);
                const uint32_t who = entityOperand(a, u32(at + 2));
                if (sub == 0 || sub == 1)
                {
                    host_.lookAt(who, entityOperand(a, u32(at + 6)), sub == 0 ? 6 : static_cast<int>(val(10)));
                    s.pc = at + (sub == 0 ? 10 : 12);
                }
                else
                {
                    host_.lookAngles(who, static_cast<int16_t>(val(6)), static_cast<int16_t>(val(8)));
                    s.pc = at + 10;
                }
                continue;
            }
            case 0x7B:
                host_.lookStop(entityOperand(a, u32(at + 1)));
                s.pc = at + 5;
                continue;

            // --- trigonometry, positions, bit fields (§6.8; CODE 0x100b0530 / 0x100b0590 / 0x100b2bb0 / 0x100b2ac0 /
            // 0x100b01d0 / 0x100b0220 / 0x100b6110 / 0x100b6180) ---
            case 0x16:
            case 0x17:
            {
                // dst = -r sin(a) / r cos(a), a in 4096ths of a turn, truncated.
                const float ang = static_cast<float>(static_cast<int32_t>(val(3))) * kTwoPi / 4096.0f;
                const float r   = static_cast<float>(static_cast<int32_t>(val(5)));
                write(a, ref(1), static_cast<uint32_t>(static_cast<int32_t>(op == 0x16 ? -r * std::sin(ang) : r * std::cos(ang))));
                s.pc = at + 7;
                continue;
            }
            case 0x3A:
            {
                // dst = the entity's facing, 0..4095.
                float y = std::fmod(host_.yaw(entityOperand(a, u32(at + 1))), kTwoPi);
                y       = y < 0 ? y + kTwoPi : y;
                write(a, ref(5), static_cast<uint32_t>(static_cast<int32_t>(y * 4096.0f / kTwoPi)));
                s.pc = at + 7;
                continue;
            }
            case 0x3B:
            {
                // x, z, y (vertical) = the entity's position x 1000.
                const auto p = host_.position(entityOperand(a, u32(at + 1)));
                write(a, ref(5), static_cast<uint32_t>(static_cast<int32_t>(p.x * 1000.0f)));
                write(a, ref(7), static_cast<uint32_t>(static_cast<int32_t>(p.z * 1000.0f)));
                write(a, ref(9), static_cast<uint32_t>(static_cast<int32_t>(p.y * 1000.0f)));
                s.pc = at + 11;
                continue;
            }
            case 0x3C:
            case 0x3D:
            {
                // Set / clear bit b of the word array at dst (b >> 5 below the word count @5); 0x3E tests.
                const uint32_t b = val(3);
                if ((b >> 5) < val(5))
                {
                    const auto     w   = static_cast<uint16_t>(ref(1) + (b >> 5));
                    const uint32_t bit = 1u << (b & 31);
                    write(a, w, op == 0x3C ? read(a, w) | bit : read(a, w) & ~bit);
                }
                s.pc = at + 7;
                continue;
            }
            case 0x40:
            case 0x41:
            {
                // Bits lo..hi: 0x40 inserts src (@7) into dst (@5), 0x41 extracts src (@5) into dst (@7).
                const uint32_t lo = val(1), hi = val(3);
                uint32_t       mask = 0;
                for (uint32_t i = 0; i < 32; ++i)
                {
                    mask |= (lo <= i && hi >= i) ? 1u << i : 0u;
                }
                if (op == 0x40)
                {
                    write(a, ref(5), (val(5) & ~mask) | ((lo < 32 ? val(7) << lo : 0u) & mask));
                }
                else
                {
                    write(a, ref(7), lo < 32 ? (val(5) & mask) >> lo : 0u);
                }
                s.pc = at + 9;
                continue;
            }

            // --- actors (§6.8; CODE 0x100b5970, 0x100b6070, 0x100b6fa0, 0x100b6880.., 0x100b7560, 0x100b7b70,
            // 0x100b7c10, 0x100b65f0, 0x100b3d80) ---
            case 0x33:
                a.noSnap = u8(at + 1) != 0;
                s.pc     = at + 2;
                continue;
            case 0x39:
                host_.face(a.entity, static_cast<float>(static_cast<int32_t>(val(1))) * kTwoPi / 4096.0f);
                s.pc = at + 3;
                continue;
            case 0x6B:
                host_.idle(entityOperand(a, u32(at + 1)), u32(at + 5));
                s.pc = at + 9;
                continue;
            case 0x4C:
            case 0x4D:
            case 0x8E:
            case 0x8F:
                host_.animation(a.entity, op == 0x4C ? 8 : op == 0x4D ? 9 : op == 0x8E ? 45 : 46);
                s.pc = at + 1;
                continue;
            case 0x4F:
                host_.animation(a.entity, static_cast<uint8_t>(val(1) + 0x12));
                s.pc = at + 3;
                continue;
            case 0x6C:
            {
                // Fade ent's model alpha to @5 (0x80 opaque) over @7 ticks, waiting.
                const float n = static_cast<float>(val(7));
                if (s.wait < 0)
                {
                    host_.fade(entityOperand(a, u32(at + 1)), static_cast<int>(val(5)), n);
                }
                if (!waitTicks(n, at + 9))
                {
                    return;
                }
                continue;
            }
            case 0x6E:
            {
                // Emote @5 (low byte; mode = next byte) on ent, once it has finished the last one.
                const uint32_t ent = entityOperand(a, u32(at + 1));
                if (host_.emoteBusy(ent))
                {
                    return;
                }
                host_.emote(ent, ent, static_cast<int>(val(5) & 0xFF), static_cast<int>((val(5) >> 8) & 0xFF));
                s.pc = at + 7;
                continue;
            }
            case 0x99:
                if (host_.emoteBusy(entityOperand(a, u32(at + 1))))
                {
                    return;
                }
                s.pc = at + 5;
                continue;
            case 0x56:
                s.pc = at + 5; // resolves ent, then yields a frame
                return;
            case 0x28:
                // As 0x27 for another player's own event block (+0x120 bit 7); NPCs: nothing. INFERRED: only the player
                // here (party members' blocks are not run).
                if (entityOperand(a, u32(at + 2)) == host_.player() && !startThread(a, host_.player(), u8(at + 1), u8(at + 6)))
                {
                    return;
                }
                s.pc = at + 7;
                continue;

            // --- screen, sound, world (§6.8; CODE 0x100b7990, 0x100b7a20, 0x100b8640, 0x100b86e0, 0x100b8710,
            // 0x100b8590, 0x100b61e0, 0x100b8a50, 0x100b92a0, 0x100b92f0, 0x100b93c0) ---
            case 0x69:
                // u8 b, mask: channels on (b = 0) / off at once; mask bit 4 a flag (0x10351720) the client has no use for.
                host_.volume(val(2) & 0xF, u8(at + 1) ? 0.0f : 1.0f, 0.0f);
                s.pc = at + 4;
                continue;
            case 0x6A:
                host_.volume(val(5) & 0xF, static_cast<float>(static_cast<int32_t>(val(1))) * 0.001f, static_cast<float>(val(3)));
                s.pc = at + 7;
                continue;
            case 0x77:
            {
                const uint32_t h = val(1), w = val(3);
                host_.environment(h == 0xFF ? -1 : static_cast<int>(h), w == 0xFF ? -1 : static_cast<int>(w));
                s.pc = at + 5;
                continue;
            }
            case 0x78:
            case 0xC9:
                host_.restoreEnvironment(); // C9: the clock only (INFERRED: nothing sets the weather alone)
                s.pc = at + 1;
                continue;
            case 0x75:
            {
                const uint8_t sub = u8(at + 1);
                if (sub == 0)
                {
                    host_.subMap(static_cast<uint16_t>(val(2)));
                }
                else if (sub == 2)
                {
                    ++stateOnly_[op]; // C2S 0x0F2 {State 2, the sub-map}; the server does not answer
                }
                s.pc = at + (sub == 0 ? 4 : 2);
                continue;
            }
            case 0x47:
                if (u8(at + 1) == 0)
                {
                    // {x, y (vertical), z, dir} as C2S 0x05C sends them; the server answers as to 0x43 (S2C 0x05C / 0x052).
                    updatePending_ = true;
                    host_.positionUpdate({ static_cast<float>(static_cast<int32_t>(val(2))) * 0.001f, static_cast<float>(static_cast<int32_t>(val(4))) * 0.001f,
                                           static_cast<float>(static_cast<int32_t>(val(6))) * 0.001f },
                                         static_cast<float>(static_cast<int32_t>(val(8))) * kTwoPi / 4096.0f);
                    s.pc = at + 10;
                    continue;
                }
                if (updatePending_)
                {
                    return;
                }
                s.pc = at + 2;
                continue;
            case 0x7D:
                // Scheduler `main` of DAT 0x13F8 + n on the player.
                host_.scheduler(0x13F8 + val(1), host_.player(), host_.player(), 0x6E69616D);
                s.pc = at + 3;
                continue;
            case 0xC8:
                host_.mapShow(static_cast<int16_t>(val(1)), static_cast<int16_t>(val(3)));
                s.pc = at + 7;
                continue;
            case 0x89:
                // The map window with zone @1's map under the player (CODE 0x100b9230 -> 0x100b24a0(1, @1, -1):
                // map -1 picks it from the player's position, 0x101f74f3). Zone 233 event 597 opens it this way.
                host_.mapShow(static_cast<int16_t>(val(1)), -1);
                s.pc = at + 3;
                continue;
            case 0x8A:
                host_.mapClose();
                s.pc = at + 1;
                continue;
            case 0x8B:
            {
                std::string label;
                for (std::size_t i = 0; i < 16 && u8(at + 9 + i); ++i)
                {
                    const char c = static_cast<char>(u8(at + 9 + i));
                    label += c == '_' ? ' ' : c;
                }
                host_.mapMarker(static_cast<int16_t>(val(1)), static_cast<int16_t>(val(3)), static_cast<float>(static_cast<int32_t>(val(5))) * 0.001f,
                                static_cast<float>(static_cast<int32_t>(val(7))) * 0.001f, label);
                s.pc = at + 25;
                continue;
            }

            // --- op 0x9D (CODE 0x100ba0c0): tables and 16-byte strings in the script's code and work ---
            case 0x9D:
            {
                const uint8_t sub  = u8(at + 1);
                const bool    wide = sub >= 10; // 0A..10: as 00..06 with a bound @8 (@6 for 0C)
                const int     len  = wide ? 10 : 8;
                auto bounded = [&](std::size_t o, uint32_t bound) {
                    const auto i = static_cast<int16_t>(val(o));
                    return bound != 0 && static_cast<int16_t>(bound) <= i ? 0 : static_cast<int>(i);
                };
                // A string: 4 work slots from a ref (constants read as empty), the last byte cleared (CODE 0x100af8a0).
                auto str = [&](uint16_t r) {
                    std::array<uint32_t, 4> v{};
                    for (uint16_t i = 0; i < 4 && !(r & 0x8000); ++i)
                    {
                        v[i] = read(a, static_cast<uint16_t>(r + i));
                    }
                    v[3] &= 0xFFFFFF;
                    return v;
                };
                auto inlineStr = [&](const std::vector<uint8_t>& c, std::size_t o) {
                    std::array<uint32_t, 4> v{};
                    for (std::size_t i = 0; i < 16; ++i)
                    {
                        v[i / 4] |= static_cast<uint32_t>(o + i < c.size() ? c[o + i] : 0) << (8 * (i % 4));
                    }
                    v[3] &= 0xFFFFFF;
                    return v;
                };
                auto put = [&](uint16_t r, const std::array<uint32_t, 4>& v) {
                    for (uint16_t i = 0; i < 4; ++i)
                    {
                        write(a, static_cast<uint16_t>(r + i), v[i]);
                    }
                };
                auto same = [](const std::array<uint32_t, 4>& x, const std::array<uint32_t, 4>& y) {
                    for (std::size_t i = 0; i < 16; ++i)
                    {
                        const auto cx = static_cast<uint8_t>(x[i / 4] >> (8 * (i % 4))), cy = static_cast<uint8_t>(y[i / 4] >> (8 * (i % 4)));
                        if (cx != cy)
                        {
                            return false;
                        }
                        if (cx == 0)
                        {
                            break;
                        }
                    }
                    return true;
                };
                const std::size_t table = ref(2); // a code offset in this block
                switch (sub)
                {
                    case 0x00:
                    case 0x0A: // dst@4 = the value of the ref table[idx@6]
                        write(a, ref(4), read(a, u16(table + static_cast<std::size_t>(bounded(6, wide ? val(8) : 0)) * 2)));
                        s.pc = at + static_cast<std::size_t>(len);
                        continue;
                    case 0x01:
                    case 0x0B: // string @4 = the 16 bytes at table + idx * 16
                        put(ref(4), inlineStr(code, table + static_cast<std::size_t>(bounded(6, wide ? val(8) : 0)) * 16));
                        s.pc = at + static_cast<std::size_t>(len);
                        continue;
                    case 0x02:
                    case 0x0C: // pointer slot idx@4 = table
                    {
                        const uint32_t bound = sub == 0x0C ? val(6) : 0;
                        auto           i     = static_cast<int16_t>(val(4));
                        if (bound == 0 || i < static_cast<int16_t>(bound))
                        {
                            if (i < 0 || i > 0x3F)
                            {
                                s.pc = at + 6;
                                continue;
                            }
                        }
                        else
                        {
                            i = 0;
                        }
                        workPtrs_[static_cast<std::size_t>(i)] = { a.block, table };
                        s.pc = at + (sub == 0x0C ? 8 : 6);
                        continue;
                    }
                    case 0x03:
                    case 0x0D:
                    case 0x04:
                    case 0x0E: // through pointer slot @2: a ref's value (03) or a string (04) at index @6 -> @4
                    {
                        const uint32_t k = val(2);
                        if (k >= workPtrs_.size() || !workPtrs_[k].first)
                        {
                            s.pc = at + 8;
                            continue;
                        }
                        const auto& [blk, off] = workPtrs_[k];
                        const auto  i          = static_cast<std::size_t>(bounded(6, wide ? val(8) : 0));
                        const auto& c          = blk->code;
                        if (sub == 0x03 || sub == 0x0D)
                        {
                            const std::size_t o = off + i * 2;
                            const auto        r = static_cast<uint16_t>((o < c.size() ? c[o] : 0) | (o + 1 < c.size() ? c[o + 1] : 0) << 8);
                            Actor             other = a;
                            other.block             = blk;
                            write(a, ref(4), read(other, r));
                        }
                        else
                        {
                            put(ref(4), inlineStr(c, off + i * 16));
                        }
                        s.pc = at + static_cast<std::size_t>(len);
                        continue;
                    }
                    case 0x05:
                    case 0x0F: // the ref table[idx@6] = @4
                        write(a, u16(table + static_cast<std::size_t>(bounded(6, wide ? val(8) : 0)) * 2), val(4));
                        s.pc = at + static_cast<std::size_t>(len);
                        continue;
                    case 0x06:
                    case 0x10: // the string at the ref table[idx@6 * 8] = string @4
                    {
                        const auto v = str(ref(4));
                        put(u16(table + static_cast<std::size_t>(bounded(6, wide ? val(8) : 0)) * 16), v);
                        s.pc = at + static_cast<std::size_t>(len);
                        continue;
                    }
                    case 0x07: // gosub table[n@4]
                        if (s.stack.size() >= 8)
                        {
                            endThread();
                            return;
                        }
                        s.stack.push_back(at + 6);
                        s.pc = u16(table + static_cast<std::size_t>(val(4)) * 2);
                        continue;
                    case 0x08: // string @2 against the inline 16 bytes @5: mode @4 0 equal / 1 different -> next, else jump @0x15
                    {
                        const bool eq = same(str(ref(2)), inlineStr(code, at + 5));
                        const int  m  = u8(at + 4) & 0x0F;
                        s.pc          = (m == 0 && eq) || (m == 1 && !eq) ? at + 0x17 : u16(at + 0x15);
                        continue;
                    }
                    case 0x09: // strings @2 and @4: mode @6 as 08; jump @7
                    {
                        const bool eq = same(str(ref(2)), str(ref(4)));
                        const int  m  = u8(at + 6) & 0x0F;
                        s.pc          = (m == 0 && eq) || (m == 1 && !eq) ? at + 9 : u16(at + 7);
                        continue;
                    }
                    default: break;
                }
                const int n = length(*a.block, at);
                ++skipped_[op];
                s.pc = at + static_cast<std::size_t>(n > 0 ? n : 2);
                continue;
            }

            // --- known, game state only (§6.8): UI and event flags, entity display bits whose meaning is open ---
            // --- op 0xC3 (CODE 0x100bbf30, 7 bytes): name record val@1 (0..7, else nothing) = the 16-byte string at ref @3
            // and the value @5 (0x10488210 + n x 20). Dialog 01 05 84 prints the name, 01 05 85 picks by the value. ---
            case 0xC3:
            {
                const auto n = static_cast<int32_t>(val(1));
                if (n >= 0 && n < 8)
                {
                    std::string name;
                    for (uint16_t k = 0; k < 4 && !(ref(3) & 0x8000); ++k)
                    {
                        const uint32_t w = read(a, static_cast<uint16_t>(ref(3) + k));
                        for (int b = 0; b < 4; ++b)
                        {
                            name += static_cast<char>(w >> (8 * b));
                        }
                    }
                    name = name.substr(0, std::min<std::size_t>(name.find('\0'), 15));
                    host_.nameRecord(static_cast<std::size_t>(n), name, val(5));
                }
                s.pc = at + 7;
                continue;
            }

            // --- op 0xB4 (CODE 0x100b0930): interface. Operands: @2 a ref (string or value), @4.. values. ---
            case 0xB4:
            {
                const uint8_t sub = u8(at + 1);
                auto strAt = [&](uint16_t r) { // a 16-byte string in four work slots (constants read as empty)
                    std::array<uint32_t, 4> v{};
                    for (uint16_t i = 0; i < 4 && !(r & 0x8000); ++i)
                    {
                        v[i] = read(a, static_cast<uint16_t>(r + i));
                    }
                    v[3] &= 0xFFFFFF;
                    return v;
                };
                auto putAt = [&](uint16_t r, const std::array<uint32_t, 4>& v) {
                    for (uint16_t i = 0; i < 4; ++i)
                    {
                        write(a, static_cast<uint16_t>(r + i), v[i]);
                    }
                };
                auto text = [](const std::array<uint32_t, 4>& v) {
                    std::string t;
                    for (std::size_t i = 0; i < 16; ++i)
                    {
                        const auto c = static_cast<char>(v[i / 4] >> (8 * (i % 4)));
                        if (c == 0)
                        {
                            break;
                        }
                        t += c;
                    }
                    return t;
                };
                const auto slot = [&](std::size_t o) {
                    const auto i = static_cast<int16_t>(val(o));
                    return i >= 0 && i < 4 ? static_cast<std::size_t>(i) : 0u;
                };
                int len = length(*a.block, at);
                switch (sub)
                {
                    case 0x00:
                    case 0x13: // the string @2 = the 16 bytes @4 (13: '@' -> space)
                    {
                        std::array<uint32_t, 4> v{};
                        for (std::size_t i = 0; i < 15; ++i)
                        {
                            uint8_t c = u8(at + 4 + i);
                            c         = sub == 0x13 && c == '@' ? static_cast<uint8_t>(' ') : c;
                            v[i / 4] |= static_cast<uint32_t>(c) << (8 * (i % 4));
                        }
                        putAt(ref(2), v);
                        len = 20;
                        break;
                    }
                    case 0x01: putAt(ref(2), savedNames_[slot(4)]); len = 6; break; // a saved name
                    case 0x04: savedNames_[slot(4)] = strAt(ref(2)); len = 6; break; // save one
                    case 0x02: // the target window: the name @2, its gauge @4 (0..100)
                    {
                        const auto g = static_cast<int16_t>(val(4));
                        if (g >= 0 && g <= 100)
                        {
                            host_.eventTarget(text(strAt(ref(2))), g);
                        }
                        len = 6;
                        break;
                    }
                    case 0x03: host_.eventTarget({}, -1); len = 2; break;
                    case 0x07:
                    case 0x0A: host_.eventTimer(sub == 0x0A, std::max(0, static_cast<int>(static_cast<int16_t>(val(2))))); len = 4; break;
                    case 0x08:
                    case 0x0B: host_.eventTimer(sub == 0x0B, -1); len = 2; break;
                    case 0x09:
                    case 0x0C: write(a, ref(2), static_cast<uint32_t>(host_.eventTimerLeft(sub == 0x0C))); len = 4; break;
                    case 0x14: // the map window's view: @2, @4, @6, @8 in thousandths, flags @10
                        host_.mapView(static_cast<float>(static_cast<int32_t>(val(2))) / 1000.0f, static_cast<float>(static_cast<int32_t>(val(4))) / 1000.0f,
                                      static_cast<float>(static_cast<int32_t>(val(6))) / 1000.0f, static_cast<float>(static_cast<int32_t>(val(8))) / 1000.0f,
                                      static_cast<int>(val(10) & 0xFF));
                        len = 12;
                        break;
                    case 0x15: // waits while the map window is open
                        if (host_.mapOpen())
                        {
                            return;
                        }
                        len = 2;
                        break;
                    default: ++stateOnly_[op]; break; // 05 / 06 / 0D / 0E flags, 0F..12 two more windows
                }
                s.pc = at + static_cast<std::size_t>(len > 0 ? len : 1);
                continue;
            }

            case 0xAB: // self flag bits (+0x120 bits 1 / 2 / 3 / 6, +0x128 bits 1 / 24, +0x130 bit 1, +0x13C bit 19), 0x10489900,
                       // 0x1035af4c, entity setters 0x1008a9b0.. (CODE 0x100babc0); 04 waits while self's emote plays
                if (u8(at + 1) == 0x04 && host_.emoteBusy(a.entity))
                {
                    return;
                }
                [[fallthrough]];
            case 0x20: // u8 -> 0x10482f18
            case 0x2E: // event message mode on (0x104886b8 / 0x10485a70)
            case 0x30: // 0x10488100 = 0
            case 0x42: // event message mode off
            case 0x38: // ref | 0x2000 -> 0x104808fc (event flags)
            case 0x34: // ref: the area's environment set (0x1018b2b0), over three frames
            case 0x35:
            case 0x67: // refs x, y: move the log windows ("menu rem4line" / "rem4li2") during the event
            case 0x68: // put them back
            case 0x61: // u8 -> self +0x128 bit 0
            case 0x7C: // u8, ent -> +0x128 bit 17
            case 0x81: // u8, ent -> model +0x8B4
            case 0x84: // self +0x12C |= 1
            case 0x86: // u8, ent -> +0x12C bit 3
            case 0x92: // u8, ent -> +0x12C bit 16
            case 0x94: // u8, ent -> +0x12C bit 17
            case 0x95: // ref -> self (0x10087cb0), model refresh
            case 0x96: // self (0x10087ce0), model refresh
            case 0x97: // refs -> world +0x138 / +0x13C (a random interval, default 4 + rand % 10), restored at the end
            case 0x9A: // waits for the music to be loaded
            case 0x9E: // u8 == 0 -> 0x104898fc
            case 0xA4: // u8 -> self +0x12C bit 26
            case 0xA5: // u8 -> self +0x12C bit 11
            {
                const int len = length(*a.block, at);
                ++stateOnly_[op];
                s.pc = at + static_cast<std::size_t>(len > 0 ? len : 1);
                continue;
            }

            // --- costume (CODE 0x100b1080): every form ends the thread's frame ---
            case 0xB6:
            {
                const uint8_t  sub  = u8(at + 1);
                const uint32_t self = a.entity, pl = host_.player();
                int            len  = 4;
                switch (sub)
                {
                    case 0x00: host_.costume(self, 0, static_cast<uint16_t>(val(2))); break;
                    case 0x01: host_.costume(self, -1, static_cast<uint16_t>(val(2))); break;
                    case 0x0B:
                        host_.costume(self, -1, static_cast<uint16_t>(val(2)));
                        for (int k = 0; k < 8; ++k)
                        {
                            host_.costume(self, k, static_cast<uint16_t>(val(4 + 2 * static_cast<std::size_t>(k))));
                        }
                        len = 20;
                        break;
                    case 0x0C: host_.costume(self, 9, static_cast<uint16_t>(val(2))); break;
                    case 0x0D:
                    case 0x0E:
                        // @4 colour (race 32 + v), body @6, head @8, feet @10, hands @12, legs @14 (0E only).
                        host_.chocobo(self, { static_cast<uint16_t>(32 + val(4)), static_cast<uint16_t>(val(8)), static_cast<uint16_t>(val(6)),
                                              static_cast<uint16_t>(val(12)), static_cast<uint16_t>(sub == 0x0E ? val(14) : 0),
                                              static_cast<uint16_t>(val(10)) });
                        len = sub == 0x0E ? 16 : 14;
                        break;
                    case 0x0F: host_.scale(self, static_cast<float>(val(2)) / 100.0f); break;
                    case 0x10:
                        if (!host_.exists(pl))
                        {
                            return;
                        }
                        len = 2;
                        break;
                    case 0x11: break;
                    case 0x12:
                    case 0x13: len = 2; break;
                    case 0x14:
                    case 0x15: len = 6; break;
                    case 0x16:
                        host_.costume(pl, -1, static_cast<uint16_t>(val(2)));
                        for (int k = 0; k < 9; ++k)
                        {
                            host_.costume(pl, k, static_cast<uint16_t>(val(4 + 2 * static_cast<std::size_t>(k))));
                        }
                        len = 22;
                        break;
                    case 0x17: host_.scale(pl, static_cast<float>(val(2)) / 100.0f); break;
                    default:
                        if (sub >= 0x02 && sub <= 0x0A)
                        {
                            host_.costume(self, sub - 2, static_cast<uint16_t>(val(2)));
                            break;
                        }
                        spdlog::warn("event {}: costume op b6 {:02x} at {:#x}; thread ended", eventId_, sub, at);
                        endThread();
                        return;
                }
                s.pc = at + static_cast<std::size_t>(len);
                return;
            }

            default:
            {
                // Effect schedulers: shots (camera), fades (CODE 0x100b42c0 / 0x100b4900 / 0x100b4a20 and siblings).
                bool handled = false;
                for (const auto& so : kShotOps)
                {
                    if (op != so.start && op != so.stop && op != so.wait)
                    {
                        continue;
                    }
                    const uint32_t n      = val(1);
                    const uint32_t file   = so.base ? so.base + n : shotFile(n);
                    const uint32_t caster = entityOperand(a, u32(at + 3)), target = entityOperand(a, u32(at + 7));
                    const uint32_t name   = u32(at + 11);
                    if (op == so.start)
                    {
                        host_.scheduler(file, caster, target, name);
                        s.pc = at + 17;
                    }
                    else if (op == so.stop)
                    {
                        host_.stopScheduler(file, caster, name);
                        s.pc = at + 15;
                    }
                    else
                    {
                        if (host_.schedulerRunning(file, caster, name))
                        {
                            return;
                        }
                        s.pc = at + 15;
                    }
                    handled = true;
                    break;
                }
                if (handled)
                {
                    continue;
                }
                const int len = length(*a.block, at);
                if (len <= 0)
                {
                    spdlog::warn("event {}: unknown opcode {:02x} at {:#x} (actor {:#x}); thread ended", eventId_, op, at, a.entity);
                    endThread();
                    return;
                }
                ++skipped_[op];
                if (trace_)
                {
                    std::string bytes;
                    for (int i = 0; i < len; ++i)
                    {
                        bytes += fmt::format(" {:02x}", u8(at + static_cast<std::size_t>(i)));
                    }
                    spdlog::debug("event: {:#x} skipped{}", a.entity, bytes);
                }
                s.pc = at + static_cast<std::size_t>(len);
                continue;
            }
        }
    }
    spdlog::warn("event {}: actor {:#x} runs away (budget); thread ended", eventId_, a.entity);
    endThread();
}

} // namespace xi::dat
