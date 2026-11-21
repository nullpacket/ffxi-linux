// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/event.h"

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

// Fixed instruction lengths (docs/event-format.md §3; 0 = depends on the next byte or unknown).
constexpr std::array<uint8_t, 256> kLength = [] {
    std::array<uint8_t, 256> t{};
    auto set = [&](std::initializer_list<int> ops, uint8_t len) {
        for (const int op : ops)
        {
            t[static_cast<std::size_t>(op)] = len;
        }
    };
    set({ 0x00, 0x1B, 0x21, 0x23, 0x25, 0x26, 0x6F, 0x70 }, 1);
    set({ 0x01, 0x05, 0x06, 0x0B, 0x0C, 0x12, 0x1A, 0x1C, 0x1D, 0x48 }, 3);
    set({ 0x03, 0x07, 0x08, 0x09, 0x0A, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x13, 0x14, 0x15, 0x19, 0x1E, 0x44, 0x5E }, 5);
    set({ 0x24, 0x2B, 0x3E, 0x49, 0x82 }, 7);
    set({ 0x02 }, 8);
    set({ 0xB0 }, 12);
    set({ 0x5B, 0x66 }, 15);
    set({ 0x2E, 0x30, 0x42, 0x4C, 0x4D, 0x58, 0x68, 0x78, 0x7F, 0x84, 0x85, 0x8A, 0x8E, 0x8F, 0x90, 0x96, 0x98, 0x9A, 0x9B, 0xC9, 0xCA, 0xCB }, 1);
    set({ 0x20, 0x22, 0x33, 0x61, 0x74, 0x9E, 0xA4, 0xA5, 0xD9 }, 2);
    set({ 0x04, 0x32, 0x34, 0x35, 0x38, 0x39, 0x4F, 0x57, 0x63, 0x7D, 0x83, 0x89, 0x91, 0x93, 0x95, 0x9C, 0xA9, 0xBE, 0xC0, 0xDA }, 3);
    set({ 0x69, 0xB1, 0xB5 }, 4);
    set({ 0x56, 0x5D, 0x67, 0x76, 0x77, 0x7B, 0x80, 0x8D, 0x97, 0x99, 0xC1 }, 5);
    set({ 0x2A, 0x2F, 0x4E, 0x7C, 0x81, 0x86, 0x92, 0x94, 0xA8, 0xD3 }, 6);
    set({ 0x16, 0x17, 0x18, 0x27, 0x28, 0x29, 0x36, 0x3A, 0x3C, 0x3D, 0x3F, 0x4B, 0x6A, 0x6D, 0x6E, 0xC3, 0xC8 }, 7);
    set({ 0xAF, 0xB9 }, 8);
    set({ 0x37, 0x40, 0x41, 0x4A, 0x6B, 0x6C }, 9);
    set({ 0x3B, 0x64, 0x65, 0x73 }, 11);
    set({ 0x2C, 0x2D, 0x50, 0x51, 0x53, 0x54, 0xBA }, 13);
    set({ 0x52, 0x55, 0xA0, 0xA1, 0xA2, 0xA3, 0xBC, 0xBD, 0xC6, 0xC7, 0xCE, 0xCF, 0xD1, 0xD2, 0xD6, 0xD7 }, 15);
    set({ 0x45, 0x62, 0x9F, 0xAA, 0xBB, 0xC5, 0xCD, 0xD0, 0xD5 }, 17);
    set({ 0x8B }, 25);
    set({ 0xB8 }, 27);
    return t;
}();

} // namespace

uint32_t EventScript::fileFor(uint16_t zone)
{
    return zone < 256 ? 5820u + zone : zone < 1000 ? 84735u + zone : zone < 2000 ? 56881u + zone : 65611u + zone;
}

std::optional<EventScript> EventScript::find(const DatIndex& index, uint16_t zone, uint32_t actor, uint16_t eventId)
{
    const auto d = index.read(fileFor(zone));
    // u32 N, u32 size[N], then the blocks back to back.
    const uint32_t n   = rd32(d, 0);
    std::size_t    off = 4 + 4u * n;
    for (uint32_t b = 0; b < n && off < d.size(); ++b)
    {
        const uint32_t size = rd32(d, 4 + 4u * b);
        // Block: u32 id, u16 n (4), u16 entryOff[n], u16 entryEventId[n], u16 m (4), u32 const[m], u32 codeLen, code.
        if (rd32(d, off) == actor)
        {
            const uint16_t entries = rd16(d, off + 4);
            const std::size_t ids  = off + 8 + 2u * entries;
            for (uint16_t i = 0; i < entries; ++i)
            {
                const uint16_t id = rd16(d, ids + 2u * i);
                if (id == eventId || id == 0xFFFE)
                {
                    EventScript s;
                    s.entry              = rd16(d, off + 8 + 2u * i);
                    const std::size_t mo = off + 8 + 4u * entries;
                    const uint16_t    m  = rd16(d, mo);
                    for (uint16_t k = 0; k < m; ++k)
                    {
                        s.consts.push_back(rd32(d, mo + 4 + 4u * k));
                    }
                    const std::size_t co  = mo + 4 + 4u * m;
                    const uint32_t    len = rd32(d, co);
                    for (uint32_t k = 0; k < len && co + 4 + k < d.size(); ++k)
                    {
                        s.code.push_back(static_cast<uint8_t>(d[co + 4 + k]));
                    }
                    return s;
                }
            }
        }
        off += size;
    }
    return std::nullopt;
}

std::vector<std::pair<uint32_t, uint16_t>> EventScript::entries(const DatIndex& index, uint16_t zone)
{
    std::vector<std::pair<uint32_t, uint16_t>> out;
    const auto     d   = index.read(fileFor(zone));
    const uint32_t n   = rd32(d, 0);
    std::size_t    off = 4 + 4u * n;
    for (uint32_t b = 0; b < n && off < d.size(); ++b)
    {
        const uint32_t id      = rd32(d, off);
        const uint16_t entries = rd16(d, off + 4);
        for (uint16_t i = 0; i < entries; ++i)
        {
            const uint16_t ev = rd16(d, off + 8 + 2u * entries + 2u * i);
            if (ev < 0xFFFE)
            {
                out.emplace_back(id, ev);
            }
        }
        off += rd32(d, 4 + 4u * b);
    }
    return out;
}

EventVm::EventVm(EventScript script, const std::array<uint32_t, 8>& params) : s_(std::move(script)), pc_(s_.entry)
{
    setParams(params);
}

void EventVm::setParams(const std::array<uint32_t, 8>& params)
{
    for (std::size_t i = 0; i < 8; ++i)
    {
        globals_[2 + i] = params[i];
    }
}

void EventVm::choose(int option)
{
    if (option < 0)
    {
        cancelled_  = true;
        globals_[0] = 0xFE;
        return;
    }
    globals_[0] = static_cast<uint32_t>(option);
}

uint8_t EventVm::u8(std::size_t at) const
{
    return at < s_.code.size() ? s_.code[at] : 0;
}

uint16_t EventVm::u16(std::size_t at) const
{
    return static_cast<uint16_t>(u8(at) | u8(at + 1) << 8);
}

uint32_t EventVm::u32(std::size_t at) const
{
    return static_cast<uint32_t>(u16(at)) | static_cast<uint32_t>(u16(at + 2)) << 16;
}

uint32_t EventVm::read(uint16_t ref) const
{
    if (ref & 0x8000)
    {
        const std::size_t k = ref & 0x7FFF;
        return k < s_.consts.size() ? s_.consts[k] : 0;
    }
    if (ref < locals_.size())
    {
        return locals_[ref];
    }
    if (ref >= 0x1000 && ref < 0x1000 + globals_.size())
    {
        return globals_[ref - 0x1000];
    }
    return 0; // other banks and actor fields (positions, directions) are not modelled
}

void EventVm::write(uint16_t ref, uint32_t value)
{
    if (ref < locals_.size())
    {
        locals_[ref] = value;
    }
    else if (ref >= 0x1000 && ref < 0x1000 + globals_.size())
    {
        globals_[ref - 0x1000] = value;
    }
}

int EventVm::length(std::size_t at) const
{
    return eventOpLength(s_.code, at);
}

int eventOpLength(const std::vector<uint8_t>& code, std::size_t at)
{
    auto u8 = [&](std::size_t i) -> uint8_t { return i < code.size() ? code[i] : 0; };
    const uint8_t op = u8(at);
    if (kLength[op] != 0)
    {
        return kLength[op];
    }
    const uint8_t sub = u8(at + 1);
    auto pick         = [&](std::initializer_list<std::pair<int, int>> t, int other) {
        for (const auto& [k, v] : t)
        {
            if (k == sub)
            {
                return v;
            }
        }
        return other;
    };
    switch (op)
    {
        case 0x1F:
        case 0x5A: return sub == 0 ? 8 : 2;
        case 0x31: return sub == 0 ? 10 : 2;
        case 0x43: return 2;
        case 0x47: return sub == 0 ? 10 : 2;
        case 0xA7: return sub == 0 ? 2 : 4;
        case 0xD4: return sub <= 2 ? 8 : sub == 3 ? 6 : 12;
        case 0x46: return pick({ { 0, 2 }, { 1, 2 }, { 2, 4 }, { 3, 8 } }, 0);
        case 0x59: return pick({ { 0, 4 }, { 1, 8 }, { 2, 4 }, { 3, 8 }, { 4, 8 }, { 5, 7 }, { 6, 6 }, { 7, 4 }, { 8, 8 } }, 0);
        case 0x5C: return sub <= 7 ? 4 : (sub >= 128 && sub <= 135) || sub == 160 || sub == 161 ? 6 : sub == 176 ? 4 : sub == 177 ? 2 : 0;
        case 0x5F: return pick({ { 0, 2 }, { 1, 2 }, { 2, 6 }, { 3, 16 }, { 4, 16 }, { 5, 18 }, { 6, 18 }, { 7, 14 } }, 0);
        case 0x60: return sub <= 1 ? 4 : sub == 2 ? 6 : 2;
        case 0x71:
            return pick({ { 0, 2 }, { 1, 2 }, { 2, 2 }, { 33, 2 }, { 81, 2 }, { 83, 2 }, { 18, 6 }, { 50, 6 }, { 32, 16 }, { 65, 8 }, { 84, 10 }, { 3, 4 },
                          { 16, 4 }, { 17, 4 }, { 19, 4 }, { 48, 4 }, { 49, 4 }, { 64, 4 }, { 80, 4 }, { 82, 4 }, { 85, 4 } },
                        0);
        case 0x72: return sub == 0 ? 4 : sub == 1 ? 6 : 0;
        case 0x75: return sub == 0 ? 4 : sub <= 2 ? 2 : 0;
        case 0x79: return sub == 1 ? 12 : sub <= 2 ? 10 : 0;
        case 0x7A: return pick({ { 0, 6 }, { 1, 7 }, { 2, 6 }, { 3, 2 }, { 4, 8 }, { 5, 6 } }, 0);
        case 0x7E: return sub == 3 ? 16 : sub == 6 ? 18 : sub == 7 ? 8 : 6;
        case 0x87:
        case 0x88: return sub <= 2 ? 2 : 0;
        case 0x8C: return pick({ { 0, 8 }, { 1, 2 }, { 2, 12 }, { 3, 10 }, { 4, 10 }, { 5, 14 } }, 0);
        case 0x9D: return sub == 2 || sub == 7 ? 6 : sub == 8 ? 23 : sub == 9 ? 9 : (sub >= 10 && sub <= 16 && sub != 12) ? 10 : sub <= 16 ? 8 : 0;
        case 0xA6: return sub <= 1 ? 2 : sub == 2 ? 4 : 0;
        case 0xAB: return (sub == 17 || (sub >= 20 && sub <= 24)) ? 4 : (sub == 27 || sub == 28) ? 6 : sub <= 28 ? 2 : 0;
        case 0xAC: return pick({ { 0, 4 }, { 1, 4 }, { 2, 6 }, { 3, 6 }, { 4, 8 } }, 0);
        case 0xAD: return sub <= 9 ? 12 : 0;
        case 0xAE: return pick({ { 0, 6 }, { 1, 8 }, { 2, 8 }, { 3, 8 }, { 4, 8 }, { 5, 10 }, { 6, 6 }, { 7, 10 }, { 8, 6 } }, 0);
        case 0xB3: return sub == 1 ? 14 : sub == 5 ? 18 : (sub == 2 || sub == 8) ? 2 : sub <= 9 ? 4 : 2;
        case 0xB4:
            return (sub == 0 || sub == 19) ? 20 : sub == 20 ? 12 : (sub == 5 || sub == 6) ? 3
                 : (sub == 3 || sub == 8 || sub == 11 || sub == 13 || sub == 14 || sub == 21) ? 2
                 : (sub == 7 || sub == 9 || sub == 10 || sub == 12) ? 4 : 6;
        case 0xB6:
            return sub == 11 ? 20 : sub == 13 ? 14 : sub == 14 ? 16 : (sub == 16 || sub == 18 || sub == 19) ? 2 : (sub == 20 || sub == 21) ? 6
                 : sub == 22 ? 22 : sub <= 23 ? 4 : 0;
        case 0xB7: return sub == 0 ? 10 : sub <= 4 ? 8 : 0;
        case 0xBF: return pick({ { 0, 8 }, { 32, 10 }, { 64, 10 }, { 96, 8 } }, 0);
        case 0xC2: return pick({ { 0, 2 }, { 1, 4 }, { 2, 6 } }, 0);
        case 0xC4: return sub <= 2 ? 12 : 0;
        case 0xCC: return pick({ { 0, 10 }, { 1, 10 }, { 3, 10 }, { 2, 14 }, { 16, 6 }, { 17, 4 }, { 32, 4 } }, 0);
        case 0xD8: return sub == 0 ? 6 : sub <= 3 ? 8 : sub == 4 ? 12 : 0;
        default: return 0;
    }
}

EventVm::Step EventVm::run()
{
    for (int budget = 0; budget < 20000; ++budget)
    {
        if (pc_ >= s_.code.size())
        {
            return { Stop::End };
        }
        const std::size_t at = pc_;
        const uint8_t     op = u8(at);
        auto ref             = [&](std::size_t o) { return u16(at + o); };
        auto arith           = [&](auto f) {
            write(ref(1), f(read(ref(1)), read(ref(3))));
            pc_ = at + 5;
        };
        switch (op)
        {
            case 0x00: return { Stop::End }; // the thread ends: the dialog actor's script is the event here
            case 0x01: pc_ = u16(at + 1); continue;
            case 0x02:
            {
                const uint32_t a = read(ref(1)), b = read(ref(3));
                bool           c = false;
                switch (u8(at + 5) & 0x0F)
                {
                    case 0: c = a == b; break;
                    case 1: c = a != b; break;
                    case 2: c = a > b; break;
                    case 3: c = a < b; break;
                    case 4: c = a >= b; break;
                    case 5: c = a <= b; break;
                    case 6:
                    case 9: c = (a & b) != 0; break;
                    case 7: c = a != b; break;
                    case 8: c = (a | b) != 0; break;
                    case 10: c = (b & ~a) != 0; break;
                    default: c = false; break;
                }
                pc_ = c ? at + 8 : u16(at + 6);
                continue;
            }
            case 0x03: arith([](uint32_t, uint32_t b) { return b; }); continue;
            case 0x05: write(ref(1), 1); pc_ = at + 3; continue;
            case 0x06: write(ref(1), 0); pc_ = at + 3; continue;
            case 0x07: arith([](uint32_t a, uint32_t b) { return a + b; }); continue;
            case 0x08: arith([](uint32_t a, uint32_t b) { return a - b; }); continue;
            case 0x09: arith([](uint32_t a, uint32_t b) { return a | (1u << (b & 31)); }); continue;
            case 0x0A: arith([](uint32_t a, uint32_t b) { return a & ~(1u << (b & 31)); }); continue;
            case 0x0B: write(ref(1), read(ref(1)) + 1); pc_ = at + 3; continue;
            case 0x0C: write(ref(1), read(ref(1)) - 1); pc_ = at + 3; continue;
            case 0x0D: arith([](uint32_t a, uint32_t b) { return a & b; }); continue;
            case 0x0E: arith([](uint32_t a, uint32_t b) { return a | b; }); continue;
            case 0x0F: arith([](uint32_t a, uint32_t b) { return a ^ b; }); continue;
            case 0x10: arith([](uint32_t a, uint32_t b) { return a << (b & 31); }); continue;
            case 0x11: arith([](uint32_t a, uint32_t b) { return a >> (b & 31); }); continue;
            case 0x12:
                rng_ = rng_ * 1103515245u + 12345u;
                write(ref(1), rng_ >> 8);
                pc_ = at + 3;
                continue;
            case 0x13:
                rng_ = rng_ * 1103515245u + 12345u;
                write(ref(1), (rng_ >> 8) % (read(ref(3)) + 1));
                pc_ = at + 5;
                continue;
            case 0x14: arith([](uint32_t a, uint32_t b) { return a * b; }); continue;
            case 0x15: arith([](uint32_t a, uint32_t b) { return b ? a / b : 0u; }); continue;
            case 0x19:
            {
                const uint32_t a = read(ref(1)), b = read(ref(3));
                write(ref(1), b);
                write(ref(3), a);
                pc_ = at + 5;
                continue;
            }
            case 0x1A:
                if (stack_.size() >= 8)
                {
                    return { Stop::Error };
                }
                stack_.push_back(at + 3);
                pc_ = u16(at + 1);
                continue;
            case 0x1B:
                if (stack_.empty())
                {
                    return { Stop::End };
                }
                pc_ = stack_.back();
                stack_.pop_back();
                continue;
            case 0x1D: pc_ = at + 3; return { Stop::Message, read(ref(1)), 0xFFFFFFFFu };
            case 0x48: pc_ = at + 3; return { Stop::Message, read(ref(1)), 0 };
            case 0x2B: pc_ = at + 7; return { Stop::Message, read(ref(5)), u32(at + 1) };
            case 0x49: pc_ = at + 7; return { Stop::Message, read(ref(5)), 0 };
            case 0xB0: pc_ = at + 12; return { Stop::Message, read(ref(10)), 0 };
            case 0x1E: pc_ = at + 5; return { Stop::Face, 0, u32(at + 1) };
            case 0x21: pc_ = at + 1; return { Stop::End };
            case 0x26: return { Stop::End };
            case 0x23: pc_ = at + 1; return { Stop::WaitClose };
            case 0x24:
            {
                Step s{ Stop::Menu, read(ref(1)), 0, read(ref(5)) };
                pc_ = at + 7;
                return s;
            }
            case 0x25:
                pc_ = at + 1;
                if (cancelled_)
                {
                    return { Stop::End };
                }
                continue; // g0 was set by choose()
            case 0x3E:
            {
                // Bit test in a bitfield array: b = ref@+3; word = ref@+1 offset by b >> 5; bit b & 31 set → continue,
                // clear → goto u16@+5 (CODE 0x100b0180).
                const uint32_t b    = read(ref(3));
                const uint32_t word = read(static_cast<uint16_t>(ref(1) + (b >> 5)));
                pc_                 = (word >> (b & 31)) & 1 ? at + 7 : u16(at + 5);
                continue;
            }
            case 0x43:
                pc_ = at + 2;
                if (u8(at + 1) == 0)
                {
                    return { Stop::Update };
                }
                continue; // sub 1: wait for the server, already done when run() resumes
            default:
            {
                const int len = length(at);
                if (len <= 0)
                {
                    return { Stop::Error, op, static_cast<uint32_t>(at) }; // message = opcode, speaker = offset
                }
                pc_ = at + static_cast<std::size_t>(len); // camera, motion, effect, wait and sync opcodes: skipped
                continue;
            }
        }
    }
    return { Stop::Error };
}

} // namespace xi::dat
