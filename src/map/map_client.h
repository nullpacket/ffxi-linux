// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lobby/lobby.h"
#include "net/socket.h"
#include "xi/blowfish.h"
#include "xi/huffman.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xi::map
{

// One decoded server->client sub-packet: id, and the full bytes including its 4-byte header.
struct SubPacket
{
    uint16_t               id = 0;
    std::vector<std::byte> data;
};

struct Position
{
    float    x = 0, y = 0, z = 0; // y is height (LSB convention)
    uint8_t  dir    = 0;
    uint16_t target = 0; // target's ActIndex (0x015 facetarget), 0 = none
};

// The game's C2S send cadence (docs/lsb-packet-timing.md §3). The base is the `/pcnt` launch argument, 400 ms by
// default (CODE 0x10103260 stores 400; 0x100f9870 copies it to the base at +0x44bac); S2C 0x005 replaces it.
inline constexpr std::chrono::milliseconds kDefaultSendInterval{ 400 };

// One step of the flush interval before each send (CODE 0x100fa920, only while in zone): more than one datagram
// unacknowledged grows it by 50 ms (at most base + 3000 ms); otherwise it falls back toward the base by one base step.
std::chrono::milliseconds nextSendInterval(std::chrono::milliseconds interval, std::chrono::milliseconds base,
                                           int16_t unacked, bool inZone);

// Player commands go out at once instead of waiting for the timer (#97). The game itself only sends on its flush
// timer (CODE 0x100fa3d0 / 0x100fa0b0 are the only callers of the send 0x100fa920, both gated by timer 0, and nothing
// else touches timer 0 or the interval at +0x417c), so an action waits up to one interval (400 ms) there. On LSB the
// server also only answers datagrams, so its result waits for our next send too: after any queued command the client
// sends every kQuickSendGap for kQuickWindow so the result comes back within one gap of the server producing it.
// This is a deliberate improvement over retail, not a traced behaviour; XI_NET_URGENT=0 restores timer-only sends.
inline constexpr std::chrono::milliseconds kQuickSendGap{ 100 };
inline constexpr std::chrono::milliseconds kQuickWindow{ 1200 };

enum class SendKind
{
    None,
    Paced, // the timer's datagram, with a position update
    Quick, // a command or the quick window after one: no position update (positions stay on the timer)
};

// What to send now. `lastPaced` is the timer's last datagram, `lastSend` any datagram; `urgent` = commands queued and
// not yet sent; `quickUntil` = the end of the window after the last command; `replyPending` = our last datagram has
// not been answered yet. The window's polls wait for that reply: the server treats a datagram carrying a stale ack as a
// lost reply and re-sends its cached datagram instead of new data (docs/protocol-map.md §1.4). A command does not wait:
// LSB still dispatches the sub-packets of such a datagram before re-sending (map_networking.cpp parse() then the ack
// check), so the command reaches the server at once and its result comes with the next real reply.
SendKind sendDue(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point lastPaced,
                 std::chrono::steady_clock::time_point lastSend, std::chrono::milliseconds interval, bool urgent,
                 std::chrono::steady_clock::time_point quickUntil, bool quickSends, bool replyPending = false);

// When sendDue() next has something to send (for the receive wait).
std::chrono::steady_clock::time_point nextSendAt(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point lastPaced,
                                                 std::chrono::steady_clock::time_point lastSend, std::chrono::milliseconds interval,
                                                 bool urgent, std::chrono::steady_clock::time_point quickUntil, bool quickSends);

// Headless map-server session: zone-in handshake, sequencing, encryption, compression, keepalive.
// Protocol reference: docs/protocol-map.md.
class MapClient
{
public:
    using Handler = std::function<void(const SubPacket&)>;
    // Sees every sub-packet before it is handled (incoming) or sent (outgoing), with its full bytes (header included),
    // which it may change; false drops it. `chunk` is the datagram body it came in (incoming only). Ashita's
    // packet_in / packet_out (src/ashita).
    using Filter = std::function<bool(bool incoming, bool injected, uint16_t id, std::vector<std::byte>& bytes, std::span<const std::byte> chunk)>;

    MapClient(const lobby::MapHandoff& handoff, const Huffman& huffman);

    // Sends the plaintext 0x00A until an encrypted reply arrives (or the timeout lapses).
    void zoneIn(std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // One network tick: flush queued sub-packets (plus a position update), wait up to `wait` for the
    // reply and dispatch its sub-packets to the handler. Call this every ~100-400 ms.
    void tick(std::chrono::milliseconds wait = std::chrono::milliseconds(250));

    // When the last valid server datagram arrived (zoneIn() sets it too): a map server that stops answering (killed
    // or restarting) shows as a growing gap.
    [[nodiscard]] std::chrono::steady_clock::time_point lastReceive() const
    {
        return lastReceive_;
    }

    void onPacket(Handler h)
    {
        handler_ = std::move(h);
    }
    void setFilter(Filter f)
    {
        filter_ = std::move(f);
    }
    // A server->client sub-packet made up by the client (Ashita's AddIncomingPacket), handled at the next tick.
    void inject(std::vector<std::byte> full)
    {
        injected_.push_back(std::move(full));
    }

    // Queue a client->server sub-packet. `payload` excludes the 4-byte header.
    void queue(uint16_t id, std::span<const std::byte> payload, bool injected = false);

    // kind: 0 Say, 1 Shout, 0x1A Yell, ... (docs/protocol-map.md §5.2)
    void say(const std::string& text, uint8_t kind = 0);

    // Requests /shutdown (C2S 0x0E7) and ticks until the server's 0x00B arrives. Returns false on timeout.
    bool logout(std::chrono::milliseconds timeout = std::chrono::seconds(45));

    [[nodiscard]] const Position& position() const
    {
        return pos_;
    }
    void setPosition(const Position& p)
    {
        pos_ = p;
    }
    [[nodiscard]] uint16_t zone() const
    {
        return zone_;
    }
    // Position updates are rejected while a zone change is pending; the session pauses them after 0x05E.
    void setPositionUpdates(bool on)
    {
        sendPositions_ = on;
    }
    [[nodiscard]] uint16_t actIndex() const
    {
        return actIndex_;
    }
    [[nodiscard]] bool inZone() const
    {
        return inZone_;
    }
    // The server's S2C 0x00B ended the session (a /logout or /shutdown countdown ran out).
    [[nodiscard]] bool loggedOut() const
    {
        return loggedOut_;
    }
    [[nodiscard]] uint64_t bytesSent() const
    {
        return udp_.bytesSent();
    }
    [[nodiscard]] uint64_t bytesReceived() const
    {
        return udp_.bytesReceived();
    }

private:
    struct Outgoing
    {
        uint16_t               id   = 0;
        uint16_t               sync = 0; // 0 = not yet sent
        std::vector<std::byte> bytes;    // full sub-packet incl. header
        bool                   injected = false;
        bool                   filtered = false; // the filter has seen it
        std::chrono::steady_clock::time_point queuedAt{};
    };

    void sendLogin();
    void sendDatagram(bool withPosition = true);
    bool receive(std::chrono::milliseconds wait);
    void handle(const SubPacket& sp);
    void onLogout(const SubPacket& sp);

    lobby::MapHandoff handoff_;
    const Huffman&    huffman_;
    Blowfish          blowfish_;
    net::UdpSocket    udp_;
    net::Endpoint     server_;

    uint16_t clientSeq_    = 0; // header[0] of our last datagram
    uint16_t serverSeq_    = 0; // header[0] of the last valid server datagram (our ack)
    uint16_t serverAckOfUs_ = 0;

    // Send pacing (goal 43, docs/protocol-map.md "C2S cadence"). The game flushes its queue on a timer (CODE
    // 0x100fa3d0: timer 0 against the interval at session +0x417c), not per reply: before each send, if more than one
    // datagram is unacknowledged the interval grows by 50 ms (at most base + 3000), else it falls back toward the base
    // (0x100fa920). S2C 0x005 sets the base (handler 0x100fabd0; LSB never sends it). XI_NET_PACING=reply restores the
    // old pacing (one datagram in flight, the next as soon as the reply arrives).
    bool                                  replyPaced_    = false;
    std::chrono::milliseconds             baseInterval_{ 0 };
    std::chrono::milliseconds             interval_{ 0 };
    // The server answers each datagram exactly once and treats a stale ack as a lost reply (re-sending
    // its cached datagram instead of new data); with reply pacing we keep exactly one datagram in flight.
    bool                                  awaitingReply_ = false;
    std::chrono::steady_clock::time_point lastSend_{};
    // #97: commands go out at once, then a short quick window (see kQuickWindow). lastPaced_ is the timer's last send.
    bool                                  quickSends_ = true;
    bool                                  urgent_     = false;
    std::chrono::steady_clock::time_point lastPaced_{};
    std::chrono::steady_clock::time_point quickUntil_{};
    // XI_NET_TRACE: when the last 0x01A went out, to time the first 0x028 after it.
    std::optional<std::chrono::steady_clock::time_point> actionSentAt_;
    uint16_t                                             actionId_ = 0;
    std::chrono::steady_clock::time_point lastReceive_{ std::chrono::steady_clock::now() };
    std::optional<std::chrono::steady_clock::time_point> deafAt_; // XI_NET_DEAF_AFTER test hook

    std::deque<Outgoing> outgoing_;
    Handler              handler_;
    Filter               filter_;
    std::deque<std::vector<std::byte>> injected_;
    Position             pos_;
    uint16_t             zone_      = 0;
    uint16_t             actIndex_  = 0;
    bool                 inZone_    = false;
    bool                 zoning_    = false;
    bool                 loggedOut_ = false;
    bool                 logoutRequested_ = false;
    bool                 sendPositions_   = true;
    std::optional<net::Endpoint> nextServer_;

    // XI_NET_TRACE=1: every 5 s, the datagrams sent per second, sub-packets per datagram and the count of each
    // C2S id in that window (goal 43, docs/protocol-map.md "C2S cadence").
    struct NetTrace
    {
        bool                                  on = false;
        std::chrono::steady_clock::time_point since{ std::chrono::steady_clock::now() };
        uint32_t                              datagrams = 0, subPackets = 0, bytes = 0;
        uint32_t                              replies = 0, replyMsSum = 0, replyMsMax = 0; // send -> answer times
        std::array<uint32_t, 0x200>           perId{};
    } trace_;
    void traceDatagram(std::size_t bytes);
};

} // namespace xi::map
