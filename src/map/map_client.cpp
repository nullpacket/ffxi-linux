// SPDX-License-Identifier: GPL-3.0-or-later
#include "map/map_client.h"

#include "xi/bytes.h"
#include "xi/md5.h"
#include "xi/packet_log.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <numeric>
#include <thread>

namespace xi::map
{

namespace
{

constexpr std::size_t kHeaderSize = 0x1C;
constexpr std::size_t kMd5Size    = 16;
constexpr std::size_t kMaxDatagram = 2500;

std::vector<std::byte> makeSubPacket(uint16_t id, std::span<const std::byte> payload)
{
    const std::size_t size = (4 + payload.size() + 3) & ~std::size_t{ 3 };
    std::vector<std::byte> p(size);
    store<uint16_t>(p, 0, static_cast<uint16_t>((id & 0x1FF) | ((size / 4) << 9)));
    storeBytes(p, 4, payload);
    return p;
}

void appendMd5(std::vector<std::byte>& dgram)
{
    const auto digest = md5(std::span(dgram).subspan(kHeaderSize));
    dgram.insert(dgram.end(), digest.begin(), digest.end());
}

bool checkMd5(std::span<const std::byte> dgram)
{
    const auto body   = dgram.subspan(kHeaderSize, dgram.size() - kHeaderSize - kMd5Size);
    const auto digest = md5(body);
    return std::equal(digest.begin(), digest.end(), dgram.end() - kMd5Size);
}

std::span<std::byte> cipherRegion(std::span<std::byte> dgram)
{
    const auto n = ((dgram.size() - kHeaderSize) / 8) * 8;
    return dgram.subspan(kHeaderSize, n);
}

} // namespace

std::chrono::milliseconds nextSendInterval(std::chrono::milliseconds interval, std::chrono::milliseconds base,
                                           int16_t unacked, bool inZone)
{
    if (!inZone)
    {
        return interval;
    }
    if (unacked < -1 || unacked > 1)
    {
        return std::min(interval + std::chrono::milliseconds(50), base + std::chrono::milliseconds(3000));
    }
    if (interval > base)
    {
        return std::max(interval - base, base);
    }
    return interval;
}

SendKind sendDue(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point lastPaced,
                 std::chrono::steady_clock::time_point lastSend, std::chrono::milliseconds interval, bool urgent,
                 std::chrono::steady_clock::time_point quickUntil, bool quickSends, bool replyPending)
{
    if (now - lastPaced >= interval)
    {
        return SendKind::Paced;
    }
    if (quickSends && now - lastSend >= kQuickSendGap && (urgent || (now < quickUntil && !replyPending)))
    {
        return SendKind::Quick;
    }
    return SendKind::None;
}

std::chrono::steady_clock::time_point nextSendAt(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point lastPaced,
                                                 std::chrono::steady_clock::time_point lastSend, std::chrono::milliseconds interval,
                                                 bool urgent, std::chrono::steady_clock::time_point quickUntil, bool quickSends)
{
    auto at = lastPaced + interval;
    if (quickSends && (urgent || now < quickUntil))
    {
        at = std::min(at, std::max(now, lastSend + kQuickSendGap));
    }
    return at;
}

MapClient::MapClient(const lobby::MapHandoff& handoff, const Huffman& huffman)
: handoff_(handoff)
, huffman_(huffman)
, blowfish_(handoff.key)
, server_(handoff.map)
{
    const char* t = std::getenv("XI_NET_TRACE");
    trace_.on     = t && *t && *t != '0';
    const char* pacing = std::getenv("XI_NET_PACING");
    replyPaced_        = pacing && std::string_view(pacing) == "reply";
    const char* iv     = std::getenv("XI_NET_INTERVAL");
    baseInterval_      = std::chrono::milliseconds(iv && *iv ? std::clamp(std::atoi(iv), 16, 2000) : static_cast<int>(kDefaultSendInterval.count()));
    interval_          = baseInterval_;
    const char* urgent = std::getenv("XI_NET_URGENT");
    quickSends_        = !(urgent && std::string_view(urgent) == "0");
    // Test hook XI_NET_DEAF_AFTER=s: ignore everything the server sends from s seconds on, as if its watchdog had killed
    // it (#43), to see the client give up and go back to the login form.
    if (const char* deaf = std::getenv("XI_NET_DEAF_AFTER"); deaf && *deaf)
    {
        int        s = 0;
        const auto r = std::from_chars(deaf, deaf + std::strlen(deaf), s);
        if (r.ec == std::errc{} && s > 0)
        {
            deafAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(s);
        }
    }
}

void MapClient::traceDatagram(std::size_t bytes)
{
    ++trace_.datagrams;
    trace_.bytes += static_cast<uint32_t>(bytes);
    const auto now = std::chrono::steady_clock::now();
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now - trace_.since).count();
    if (ms < 5000)
    {
        return;
    }
    std::string ids;
    for (std::size_t id = 0; id < trace_.perId.size(); ++id)
    {
        if (trace_.perId[id] != 0)
        {
            ids += fmt::format(" {:03X}:{:.1f}/s", id, trace_.perId[id] * 1000.0 / static_cast<double>(ms));
        }
    }
    spdlog::info("net: {:.1f} datagrams/s, {:.0f} B/s, {:.2f} sub-packets each, answered in {} ms avg / {} ms max;{}",
                 trace_.datagrams * 1000.0 / static_cast<double>(ms), trace_.bytes * 1000.0 / static_cast<double>(ms),
                 trace_.datagrams ? trace_.subPackets / static_cast<double>(trace_.datagrams) : 0.0,
                 trace_.replies ? trace_.replyMsSum / trace_.replies : 0, trace_.replyMsMax, ids);
    trace_ = NetTrace{ true, now };
}

void MapClient::queue(uint16_t id, std::span<const std::byte> payload, bool injected)
{
    outgoing_.push_back({ id, 0, makeSubPacket(id, payload), injected, false, std::chrono::steady_clock::now() });
    urgent_ = true; // a player command (or a handshake packet): send it at once (#97)
}

void MapClient::say(const std::string& text, uint8_t kind)
{
    // 0x0B5: kind u8 @+4, unused u8 @+5, NUL-terminated message @+6 (docs/protocol-map.md §5.2).
    const auto            len = std::min<std::size_t>(text.size(), 127);
    std::vector<std::byte> payload(2 + len + 1);
    payload[0] = std::byte{ kind };
    storeBytes(payload, 2, asBytes(std::string_view(text).substr(0, len)));
    queue(0x0B5, payload);
}

// docs/protocol-map.md §4.2: plaintext 0x00A, 92 bytes, byte-sum check over 0x08..0x5B.
void MapClient::sendLogin()
{
    std::array<std::byte, 88> payload{}; // sub-packet offsets 0x04..0x5B
    auto at = [&](std::size_t subOff) { return subOff - 4; };
    store<uint32_t>(payload, at(0x0C), handoff_.charId);
    storeString(payload, at(0x22), 15, handoff_.charName);
    storeString(payload, at(0x54), 4, "WIN");
    store<uint16_t>(payload, at(0x58), 1); // English

    auto sub = makeSubPacket(0x00A, payload);
    store<uint16_t>(sub, 2, 1); // sync
    sub[4] = std::byte{ static_cast<uint8_t>(std::accumulate(sub.begin() + 8, sub.begin() + 0x5C, 0u,
                                                              [](unsigned a, std::byte b) { return a + std::to_integer<unsigned>(b); })) };

    clientSeq_ = 1;
    serverSeq_ = 0;
    std::vector<std::byte> dgram(kHeaderSize);
    store<uint16_t>(dgram, 0, clientSeq_);
    store<uint16_t>(dgram, 2, 0);
    store<uint32_t>(dgram, 8, static_cast<uint32_t>(std::time(nullptr)));
    dgram.insert(dgram.end(), sub.begin(), sub.end());
    dgram.insert(dgram.end(), 4, std::byte{ 0 }); // terminator so the parser never reads the MD5 as a sub-packet
    appendMd5(dgram);
    udp_.sendTo(server_, dgram);
}

void MapClient::zoneIn(std::chrono::milliseconds timeout)
{
    spdlog::info("map: zoning in to {} as {} (charid {})", server_.toString(), handoff_.charName, handoff_.charId);
    inZone_        = false;
    zoning_        = false;
    awaitingReply_ = false;
    serverAckOfUs_ = 0;
    outgoing_.clear();

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        sendLogin();
        if (receive(std::chrono::milliseconds(1000)) && inZone_)
        {
            break;
        }
    }
    if (!inZone_)
    {
        throw std::runtime_error("map: no reply to 0x00A login");
    }

    // docs/protocol-map.md §4.4: GAMEOK, ZONE_TRANSITION, CLISTATUS.
    queue(0x00C, std::array<std::byte, 8>{});
    std::array<std::byte, 4> zt{ std::byte{ 2 } };
    queue(0x011, zt);
    queue(0x061, std::array<std::byte, 4>{});
}

void MapClient::sendDatagram(bool withPosition)
{
    // Carry a position update (drives entity spawns) unless we're logging out, where the server rejects it.
    if (withPosition && !logoutRequested_ && sendPositions_)
    {
        std::array<std::byte, 28> posPayload{};
        store<float>(posPayload, 0x04 - 4, pos_.x);
        store<float>(posPayload, 0x08 - 4, pos_.y);
        store<float>(posPayload, 0x0C - 4, pos_.z);
        store<uint8_t>(posPayload, 0x14 - 4, pos_.dir);
        store<uint16_t>(posPayload, 0x16 - 4, pos_.target);
        outgoing_.push_back({ 0x015, 0, makeSubPacket(0x015, posPayload) });
    }

    // Ashita's packet_out sees each new sub-packet once; a changed one gets its size rebuilt, a dropped one goes.
    if (filter_)
    {
        // The chunk is this datagram's new sub-packets together, as Ashita passes it (LuAshitacast spots the start of a
        // new chunk by comparing a packet with the chunk's first bytes).
        std::vector<std::byte> chunk;
        for (const auto& o : outgoing_)
        {
            if (!o.filtered)
            {
                chunk.insert(chunk.end(), o.bytes.begin(), o.bytes.end());
            }
        }
        std::erase_if(outgoing_, [&](Outgoing& o) {
            if (o.filtered)
            {
                return false;
            }
            o.filtered = true;
            if (!filter_(false, o.injected, o.id, o.bytes, chunk))
            {
                return true;
            }
            o.bytes.resize(std::max<std::size_t>((o.bytes.size() + 3) & ~std::size_t{ 3 }, 4));
            store<uint16_t>(o.bytes, 0, static_cast<uint16_t>((o.id & 0x1FF) | ((o.bytes.size() / 4) << 9)));
            return false;
        });
    }

    ++clientSeq_;
    std::vector<std::byte> dgram(kHeaderSize);
    store<uint16_t>(dgram, 0, clientSeq_);
    store<uint16_t>(dgram, 2, serverSeq_);
    store<uint32_t>(dgram, 8, static_cast<uint32_t>(std::time(nullptr)));

    // Body = sub-packets + a zero terminator (so the server's parse loop stops cleanly), compressed with the
    // same Huffman code the server uses, so it takes the normal path rather than the raw fallback (§3.4).
    std::vector<std::byte> body;
    for (auto& o : outgoing_)
    {
        if (o.sync == 0)
        {
            o.sync = clientSeq_;
        }
        store<uint16_t>(o.bytes, 2, o.sync);
        if (body.size() + o.bytes.size() > 1000)
        {
            break;
        }
        body.insert(body.end(), o.bytes.begin(), o.bytes.end());
        PacketLog::instance().record(false, o.id, o.bytes.size());
        if (trace_.on && o.sync == clientSeq_ && o.id != 0x015)
        {
            const auto now = std::chrono::steady_clock::now();
            spdlog::info("net: {:03X} queued -> sent in {} ms (datagram {})", o.id,
                         std::chrono::duration_cast<std::chrono::milliseconds>(now - o.queuedAt).count(), clientSeq_);
            actionSentAt_ = now;
            actionId_     = o.id;
        }
        if (trace_.on)
        {
            ++trace_.subPackets;
            ++trace_.perId[o.id & 0x1FF];
        }
    }
    body.insert(body.end(), 4, std::byte{ 0 });

    uint32_t   bitCount = 0;
    const auto packed   = huffman_.encode(body, bitCount);
    dgram.insert(dgram.end(), packed.begin(), packed.end());
    dgram.resize(dgram.size() + 4);
    store<uint32_t>(dgram, dgram.size() - 4, bitCount);
    appendMd5(dgram);
    blowfish_.encryptBlocks(cipherRegion(dgram));
    udp_.sendTo(server_, dgram);
    if (trace_.on && actionSentAt_ && std::chrono::steady_clock::now() - *actionSentAt_ < std::chrono::seconds(2))
    {
        spdlog::info(
            "net: sent datagram {} (acks {}, {} sub-packets) {} ms after the {:03X}", clientSeq_, serverSeq_,
            outgoing_.size(),
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - *actionSentAt_)
                .count(),
            actionId_);
    }
    if (trace_.on)
    {
        traceDatagram(dgram.size());
    }
    awaitingReply_ = true;
    lastSend_      = std::chrono::steady_clock::now();

    // Position updates are never worth resending; everything else stays until the server acks its datagram.
    std::erase_if(outgoing_, [](const Outgoing& o) { return o.id == 0x015; });
}

bool MapClient::receive(std::chrono::milliseconds wait)
{
    std::array<std::byte, kMaxDatagram> buf{};
    net::Endpoint                       from;
    const auto                          n = udp_.recvFrom(buf, from, wait);
    if (n <= kHeaderSize + kMd5Size)
    {
        return false;
    }
    if (deafAt_ && std::chrono::steady_clock::now() >= *deafAt_)
    {
        return false;
    }
    auto dgram = std::span(buf).first(n);

    // Server only ever encrypts with the current key; anything that fails decrypt+MD5 is dropped (§2.7).
    blowfish_.decryptBlocks(cipherRegion(dgram));
    if (!checkMd5(dgram))
    {
        spdlog::debug("map: dropped datagram ({} bytes) failing MD5", n);
        return false;
    }

    const auto seq = load<uint16_t>(dgram, 0);
    const auto ack = load<uint16_t>(dgram, 2);
    if (inZone_ && seq != 0 && static_cast<int16_t>(seq - serverSeq_) <= 0)
    {
        if (trace_.on && actionSentAt_ && std::chrono::steady_clock::now() - *actionSentAt_ < std::chrono::seconds(2))
        {
            spdlog::info(
                "net: re-sent datagram {} (acks {}) {} ms after the {:03X} went out", seq, ack,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - *actionSentAt_)
                    .count(),
                actionId_);
        }
        return false; // re-send of something we've already processed
    }
    serverSeq_     = seq;
    serverAckOfUs_ = ack;
    lastReceive_   = std::chrono::steady_clock::now();
    if (trace_.on && actionSentAt_ && lastReceive_ - *actionSentAt_ < std::chrono::seconds(2))
    {
        spdlog::info("net: datagram {} (acks {}) {} ms after the {:03X} went out", seq, ack,
                     std::chrono::duration_cast<std::chrono::milliseconds>(lastReceive_ - *actionSentAt_).count(),
                     actionId_);
    }
    if (trace_.on && awaitingReply_)
    {
        const auto rtt = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(lastReceive_ - lastSend_).count());
        ++trace_.replies;
        trace_.replyMsSum += rtt;
        trace_.replyMsMax = std::max(trace_.replyMsMax, rtt);
    }
    awaitingReply_ = false;
    std::erase_if(outgoing_, [&](const Outgoing& o) { return o.sync != 0 && static_cast<int16_t>(o.sync - ack) <= 0; });

    const auto body     = dgram.subspan(kHeaderSize, n - kHeaderSize - kMd5Size);
    const auto bitCount = load<uint32_t>(body, body.size() - 4);
    const auto plain    = huffman_.decode(body.first(body.size() - 4), bitCount);

    for (std::size_t off = 0; off + 4 <= plain.size();)
    {
        const auto hdr  = load<uint16_t>(plain, off);
        const auto size = static_cast<std::size_t>(hdr >> 9) * 4;
        if (size == 0 || off + size > plain.size())
        {
            break;
        }
        SubPacket sp{ static_cast<uint16_t>(hdr & 0x1FF), { plain.begin() + off, plain.begin() + off + size } };
        off += size;
        if (filter_ && !filter_(true, false, sp.id, sp.data, plain))
        {
            continue;
        }
        PacketLog::instance().record(true, sp.id, sp.data.size());
        handle(sp);
    }
    return true;
}

void MapClient::handle(const SubPacket& sp)
{
    if ((sp.id == 0x028 || sp.id == 0x029) && actionSentAt_)
    {
        spdlog::info(
            "net: first {:03X} {} ms after the {:03X} went out", sp.id,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - *actionSentAt_)
                .count(),
            actionId_);
        actionSentAt_.reset();
    }
    switch (sp.id)
    {
        case 0x005: // GP_SERV_COMMAND_PACKETCONTROL: PacketCnt @+4 becomes the flush interval's base (CODE 0x100fabd0)
            if (sp.data.size() >= 8 && load<uint32_t>(sp.data, 4) != 0)
            {
                baseInterval_ = std::chrono::milliseconds(std::clamp<uint32_t>(load<uint32_t>(sp.data, 4), 16, 2000));
                interval_     = baseInterval_;
            }
            break;
        case 0x00A: // zone-in: PosHead @+4, ZoneNo @+0x30
        {
            actIndex_ = load<uint16_t>(sp.data, 0x08);
            pos_.dir  = load<uint8_t>(sp.data, 0x0B);
            pos_.x    = load<float>(sp.data, 0x0C);
            pos_.y    = load<float>(sp.data, 0x10);
            pos_.z    = load<float>(sp.data, 0x14);
            zone_     = static_cast<uint16_t>(load<uint32_t>(sp.data, 0x30));
            inZone_   = true;
            sendPositions_ = true;
            break;
        }
        case 0x00B:
            onLogout(sp);
            break;
        default:
            break;
    }
    if (handler_)
    {
        handler_(sp);
    }
}

// docs/protocol-map.md §6: step the key, reconnect from the same socket to the new address.
void MapClient::onLogout(const SubPacket& sp)
{
    const auto state = load<uint8_t>(sp.data, 0x04);
    const auto ip    = load<uint32_t>(sp.data, 0x08);
    const auto port  = static_cast<uint16_t>(load<uint32_t>(sp.data, 0x0C));
    blowfish_.incrementForZone();
    handoff_.key = blowfish_.sessionKey();
    inZone_      = false;
    if (state == 2 && ip != 0)
    {
        nextServer_ = net::Endpoint{ ip, port };
    }
    else
    {
        loggedOut_ = true;
    }
}

bool MapClient::logout(std::chrono::milliseconds timeout)
{
    // Mode 3 = ShutdownOn, Kind 3 = Shutdown (src/map/packets/c2s/0x0e7_reqlogout.h).
    std::array<std::byte, 4> payload{};
    store<uint16_t>(payload, 0, 3);
    store<uint16_t>(payload, 2, 3);
    queue(0x0E7, payload);
    logoutRequested_ = true;

    // Logging out rests the character (LEAVEGAME applies HEALING); taking damage cancels it, so ask again
    // every 10 s until the server lets us go.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto       resend   = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!loggedOut_ && std::chrono::steady_clock::now() < deadline)
    {
        const auto start = std::chrono::steady_clock::now();
        if (start >= resend)
        {
            queue(0x0E7, payload);
            resend = start + std::chrono::seconds(10);
        }
        tick();
        std::this_thread::sleep_until(start + std::chrono::milliseconds(250));
    }
    return loggedOut_;
}

void MapClient::tick(std::chrono::milliseconds wait)
{
    if (nextServer_)
    {
        server_ = *nextServer_;
        nextServer_.reset();
        zoneIn();
        return;
    }
    while (!injected_.empty())
    {
        auto bytes = std::move(injected_.front());
        injected_.pop_front();
        if (bytes.size() < 4)
        {
            continue;
        }
        SubPacket sp{ static_cast<uint16_t>(load<uint16_t>(bytes, 0) & 0x1FF), std::move(bytes) };
        if (filter_ && !filter_(true, true, sp.id, sp.data, sp.data))
        {
            continue;
        }
        handle(sp);
    }
    const auto now = std::chrono::steady_clock::now();
    if (replyPaced_)
    {
        // Resend only if the reply looks lost; otherwise just keep listening.
        if (!awaitingReply_ || now - lastSend_ > std::chrono::milliseconds(1500))
        {
            sendDatagram();
        }
        receive(wait);
        return;
    }
    const bool quick = quickSends_ && inZone_;
    const auto kind  = sendDue(now, lastPaced_, lastSend_, interval_, urgent_, quickUntil_, quick,
                               awaitingReply_ && now - lastSend_ < std::chrono::milliseconds(1000));
    if (kind == SendKind::Paced)
    {
        interval_  = nextSendInterval(interval_, baseInterval_, static_cast<int16_t>(clientSeq_ - serverAckOfUs_), inZone_);
        lastPaced_ = now;
        sendDatagram();
    }
    else if (kind == SendKind::Quick)
    {
        sendDatagram(false);
    }
    if (kind != SendKind::None)
    {
        if (urgent_ && quick)
        {
            quickUntil_ = now + kQuickWindow;
        }
        urgent_ = false; // whatever was queued went out with this datagram
    }
    // Listen until the next send is due (or `wait`, whichever is sooner).
    const auto at  = nextSendAt(std::chrono::steady_clock::now(), lastPaced_, lastSend_, interval_, urgent_, quickUntil_, quick);
    const auto due = std::chrono::duration_cast<std::chrono::milliseconds>(at - std::chrono::steady_clock::now());
    receive(std::clamp(due, std::chrono::milliseconds(1), wait));
    while (receive(std::chrono::milliseconds(0)))
    {
    }
}

} // namespace xi::map
