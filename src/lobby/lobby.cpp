// SPDX-License-Identifier: GPL-3.0-or-later
#include "lobby/lobby.h"

#include "net/tls.h"
#include "xi/bytes.h"
#include "xi/md5.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <thread>

using namespace std::chrono_literals;

// Protocol reference: docs/protocol-login.md (section numbers in comments below refer to it).

namespace xi::lobby
{

namespace
{

constexpr uint32_t kTerminator = 0x46465849; // "IXFF"

// xiloader's placeholder key in 0xA2 (§3.2): 16 zero bytes then 58 E0 5D AD. key[11] must stay 0 (§2.3 0xA2). LSB
// since 2026-10 replaces it with mapKey() (data_session.cpp, accounts_profile.session_hash).
constexpr Blowfish::SessionKey kDefaultKey = [] {
    Blowfish::SessionKey k{};
    k[16] = std::byte{ 0x58 };
    k[17] = std::byte{ 0xE0 };
    k[18] = std::byte{ 0x5D };
    k[19] = std::byte{ 0xAD };
    return k;
}();

// Builds an IXFF view packet with the session hash in the identifier slot (§2.2).
std::vector<std::byte> viewPacket(uint32_t command, std::size_t size, const SessionHash& hash)
{
    std::vector<std::byte> p(size);
    store<uint32_t>(p, 0, static_cast<uint32_t>(size));
    store<uint32_t>(p, 4, kTerminator);
    store<uint32_t>(p, 8, command);
    storeBytes(p, 12, hash);
    return p;
}

// Reads one IXFF packet from the view socket, sized by its first u32.
std::vector<std::byte> readView(net::TcpStream& view)
{
    std::vector<std::byte> p(28);
    view.recvExact(p);
    const auto size = load<uint32_t>(p, 0);
    if (load<uint32_t>(p, 4) != kTerminator || size < 28 || size > 0x1000)
    {
        throw LobbyError(fmt::format("bad view packet header (size {})\n{}", size, hexdump(p)));
    }
    p.resize(size);
    view.recvExact(std::span(p).subspan(28));

    const auto cmd = load<uint32_t>(p, 8);
    if (cmd == 0x04)
    {
        const auto code = load<uint16_t>(p, 32);
        throw LobbyError(fmt::format("lobby error {} (FFXI-{})", code, 3000 + code), code);
    }
    return p;
}

std::vector<std::byte> expectView(net::TcpStream& view, uint32_t command)
{
    auto       p   = readView(view);
    const auto cmd = load<uint32_t>(p, 8);
    if (cmd != command)
    {
        throw LobbyError(fmt::format("expected view 0x{:02X}, got 0x{:02X}", command, cmd));
    }
    return p;
}

void expectData(net::TcpStream& data, uint8_t command, std::size_t size)
{
    std::vector<std::byte> p(size);
    data.recvExact(p);
    if (std::to_integer<uint8_t>(p[0]) != command)
    {
        throw LobbyError(fmt::format("expected data 0x{:02X}, got:\n{}", command, hexdump(p)));
    }
}

CharacterSlot parseSlot(std::span<const std::byte> e)
{
    CharacterSlot s;
    s.charId   = load<uint32_t>(e, 0);
    s.name     = loadString(e, 12, 16);
    s.world    = loadString(e, 28, 16);
    s.race     = load<uint16_t>(e, 44);
    s.mainJob  = load<uint8_t>(e, 46);
    s.subJob   = load<uint8_t>(e, 47);
    s.mainLvl  = load<uint8_t>(e, 73);
    s.zone     = static_cast<uint16_t>(load<uint8_t>(e, 72) | (load<uint8_t>(e, 79) << 8));
    s.occupied = !s.name.empty() && s.name != " ";
    return s;
}

// One auth (TLS/JSON) request and its reply (§1.2, §1.3).
nlohmann::json authRequest(const Config& cfg, uint32_t command, LoginProtocol protocol)
{
    auto       tls  = net::TlsStream::connect(net::Endpoint::resolve(cfg.host, cfg.authPort), cfg.pinnedCert);
    const auto body = authRequestJson(cfg, command, protocol);
    tls.write(asBytes(body));

    // A malformed username/password gets no reply at all (§1.3), hence the timeout.
    const auto reply = tls.readSome(10s);
    const auto json  = nlohmann::json::parse(reply, nullptr, false);
    if (json.is_discarded())
    {
        throw LobbyError(fmt::format("auth: non-JSON reply ({} bytes)", reply.size()));
    }
    if (json.contains("error_message"))
    {
        throw LobbyError("auth: " + json["error_message"].get<std::string>());
    }
    return json;
}

// The data and view sockets after the character list and world list (§2.3, §2.4), ready to select or create.
struct OpenLobby
{
    net::TcpStream             data;
    net::TcpStream             view;
    std::vector<CharacterSlot> slots;
    uint32_t                   viewKey = 0; // the 0x05 ResponseKey's key (+0x1C), where the client's key counter starts
};

OpenLobby openLobby(const Config& cfg, const AuthResult& auth)
{
    const auto& H = auth.sessionHash;

    // Data socket: register the session hash (§2.3 0xFE). The server answers with a zero-length write.
    auto data = net::TcpStream::connect(net::Endpoint::resolve(cfg.host, cfg.dataPort));
    {
        std::array<std::byte, 28> p{};
        p[0] = std::byte{ 0xFE };
        storeBytes(p, 12, H);
        data.sendAll(p);
    }

    auto     view    = net::TcpStream::connect(net::Endpoint::resolve(cfg.host, cfg.viewPort));
    uint32_t viewKey = 0;

    // 0x26 RequestLobbyLogin -> 0x05 (§2.4). This round-trip also gives the data session time to register.
    {
        auto p = viewPacket(0x26, 0x98, H);
        storeString(p, 0x74, 16, cfg.clientVersion);
        view.sendAll(p);
        viewKey = load<uint32_t>(expectView(view, 0x05), 0x1C);
    }

    // 0x1F RequestGetChr: the server asks for the account id on the data socket, then answers on view.
    view.sendAll(viewPacket(0x1F, 0x2C, H));
    expectData(data, 0x01, 5);
    {
        std::array<std::byte, 28> p{};
        p[0] = std::byte{ 0xA1 };
        store<uint32_t>(p, 1, auth.accountId);
        store<uint32_t>(p, 5, net::Endpoint::resolve(cfg.host, 0).addr); // search server ip, echoed back in 0x0B
        storeBytes(p, 12, H);
        data.sendAll(p);
    }
    expectData(data, 0x03, 0x148);
    const auto chrInfo = expectView(view, 0x20);

    std::vector<CharacterSlot> slots;
    const auto                 count = load<uint32_t>(chrInfo, 28);
    for (uint32_t i = 0; i < count && 32 + 140 * (i + 1) <= chrInfo.size(); ++i)
    {
        slots.push_back(parseSlot(std::span(chrInfo).subspan(32 + 140 * i, 140)));
    }

    // 0x24 world list (retail does this before selecting).
    view.sendAll(viewPacket(0x24, 0x2C, H));
    expectView(view, 0x23);
    return OpenLobby{ std::move(data), std::move(view), std::move(slots), viewKey };
}

// The auth request, and with Auto the retry the server's version error asks for. Returns the reply and the protocol.
std::pair<nlohmann::json, LoginProtocol> authWithProtocol(const Config& cfg, uint32_t command)
{
    const auto first = cfg.protocol == LoginProtocol::Auto ? LoginProtocol::V22 : cfg.protocol;
    try
    {
        return { authRequest(cfg, command, first), first };
    }
    catch (const LobbyError& e)
    {
        const auto wanted = requiredProtocol(e.what());
        if (cfg.protocol != LoginProtocol::Auto || !wanted || *wanted == first)
        {
            throw;
        }
        const auto v = xiloaderVersion(*wanted);
        spdlog::info("auth: the server wants xiloader {}.{}.x; retrying with that login protocol", v[0], v[1]);
        return { authRequest(cfg, command, *wanted), *wanted };
    }
}

} // namespace

std::string authRequestJson(const Config& cfg, uint32_t command, LoginProtocol protocol)
{
    const auto     version = xiloaderVersion(protocol);
    nlohmann::json req     = {
        { "command", command },
        { "version", { version[0], version[1], version[2] } },
        { "username", cfg.username },
        { "password", cfg.password },
    };
    if (!cfg.otp.empty())
    {
        req["otp"] = cfg.otp;
    }
    // A name or password that is not UTF-8 must not throw out of the login (#106); the server sees U+FFFD instead.
    return req.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::optional<LoginProtocol> parseLoginProtocol(std::string_view text)
{
    if (text == "auto") return LoginProtocol::Auto;
    if (text == "2.1") return LoginProtocol::V21;
    if (text == "2.2") return LoginProtocol::V22;
    return std::nullopt;
}

LoginProtocol defaultLoginProtocol()
{
    if (const char* env = std::getenv("XI_LOGIN_PROTOCOL"))
    {
        if (const auto p = parseLoginProtocol(env))
        {
            return *p;
        }
        spdlog::warn("XI_LOGIN_PROTOCOL '{}' is not auto / 2.1 / 2.2; using auto", env);
    }
    return LoginProtocol::Auto;
}

std::array<uint8_t, 3> xiloaderVersion(LoginProtocol p)
{
    return p == LoginProtocol::V21 ? std::array<uint8_t, 3>{ 2, 1, 0 } : std::array<uint8_t, 3>{ 2, 2, 0 };
}

std::optional<LoginProtocol> requiredProtocol(std::string_view errorMessage)
{
    if (errorMessage.find("xiloader") == std::string_view::npos)
    {
        return std::nullopt;
    }
    // "<major>.<minor>.x": the server's SupportedXiloaderVersion, in either LSB wording.
    for (std::size_t at = errorMessage.find(".x"); at != std::string_view::npos; at = errorMessage.find(".x", at + 1))
    {
        if (at >= 3 && errorMessage[at - 2] == '.' && errorMessage[at - 3] == '2')
        {
            const char minor = errorMessage[at - 1];
            if (minor == '1') return LoginProtocol::V21;
            if (minor == '2') return LoginProtocol::V22;
        }
    }
    return std::nullopt;
}

Blowfish::SessionKey legacyMapKey()
{
    return kDefaultKey;
}

Blowfish::SessionKey mapKey(const SessionHash& hash, uint32_t viewKey)
{
    Blowfish::SessionKey k{};
    const auto           digest = md5(hash);
    std::copy(digest.begin(), digest.end(), k.begin());
    store<uint32_t>(std::span(k), 16, viewKey + 9);
    return k;
}

AuthResult authenticate(const Config& cfg)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto [json, protocol] = authWithProtocol(cfg, 0x10);
        const int  result = json.value("result", -1);
        if (result != 1)
        {
            throw LobbyError(fmt::format("auth: login failed (result {})", result), static_cast<uint16_t>(result));
        }

        AuthResult r;
        r.protocol        = protocol;
        r.accountId       = json.at("account_id").get<uint32_t>();
        const auto& hash  = json.at("session_hash");
        if (!hash.is_array() || hash.size() != 16)
        {
            throw LobbyError("auth: malformed session_hash");
        }
        for (std::size_t i = 0; i < 16; ++i)
        {
            r.sessionHash[i] = std::byte{ hash[i].get<uint8_t>() };
        }

        // Server keys sessions by the hash as a NUL-terminated string; a leading 0 makes it unusable (§1.3).
        if (r.sessionHash[0] == std::byte{ 0 })
        {
            spdlog::warn("auth: session hash starts with 0x00 (server quirk), re-authenticating");
            continue;
        }
        return r;
    }
    throw LobbyError("auth: could not obtain a usable session hash");
}

MapHandoff enterWorld(const Config& cfg, const AuthResult& auth,
                      const std::function<std::optional<std::size_t>(const std::vector<CharacterSlot>&)>& choose)
{
    const auto& H = auth.sessionHash;
    auto [data, view, slots, viewKey] = openLobby(cfg, auth);

    const auto pick = choose(slots);
    if (!pick || *pick >= slots.size() || !slots[*pick].occupied)
    {
        throw LobbyError("no character selected");
    }
    const auto& chosen = slots[*pick];

    // 0x07 RequestSelectChr -> data 0x02 -> 0xA2 key -> view 0x0B.
    {
        auto p = viewPacket(0x07, 0x58, H);
        store<uint32_t>(p, 28, chosen.charId);
        store<uint32_t>(p, 32, chosen.charId & 0xFFFF);
        storeString(p, 36, 16, chosen.name);
        store<uint32_t>(p, 68, 3);
        view.sendAll(p);
    }
    expectData(data, 0x02, 5);
    {
        std::array<std::byte, 28> p{};
        p[0] = std::byte{ 0xA2 };
        storeBytes(p, 1, kDefaultKey);
        data.sendAll(p);
    }
    const auto next = expectView(view, 0x0B);

    MapHandoff h;
    h.accountId   = auth.accountId;
    h.sessionHash = H;
    h.charId      = load<uint32_t>(next, 28);
    h.charName    = loadString(next, 36, 16);
    h.map         = net::Endpoint{ load<uint32_t>(next, 56), static_cast<uint16_t>(load<uint32_t>(next, 60)) };
    h.search      = net::Endpoint{ load<uint32_t>(next, 64), static_cast<uint16_t>(load<uint32_t>(next, 68)) };
    // No create / delete / rename / retry happened this visit (§3.2). 2.1: the server keeps the key we sent in 0xA2;
    // 2.2: it stores MD5(session hash) + the key counter.
    h.key = auth.protocol == LoginProtocol::V21 ? legacyMapKey() : mapKey(H, viewKey);

    view.waitForClose(2s);
    return h;
}

void createAccount(const Config& cfg)
{
    // LOGIN_CREATE (§1.2): result 3 = created, 4 = name taken, 8 = creation disabled, 9 = error.
    const int result = authWithProtocol(cfg, 0x20).first.value("result", -1);
    switch (result)
    {
        case 3: return;
        case 4: throw LobbyError("create account: that name is taken", 4);
        case 8: throw LobbyError("create account: the server has account creation off (login.ACCOUNT_CREATION)", 8);
        default: throw LobbyError(fmt::format("create account: failed (result {})", result), static_cast<uint16_t>(result));
    }
}

void createCharacter(const Config& cfg, const AuthResult& auth, const NewCharacter& c)
{
    const auto& H     = auth.sessionHash;
    auto        lobby = openLobby(cfg, auth);

    // 0x22: reserve the name (+32, 15 chars); the server answers 0x03, or 0x04 with an error code (FFXI-3xxx).
    {
        auto p = viewPacket(0x22, 0x30, H);
        storeString(p, 32, 15, c.name);
        lobby.view.sendAll(p);
        expectView(lobby.view, 0x03);
    }
    // 0x21: create it. The server reads race +48, main job +50, nation +54, size +57, face +60
    // (login_helpers.cpp createCharacter); anything else in the packet is ignored.
    {
        auto p = viewPacket(0x21, 0x40, H);
        store<uint8_t>(p, 48, c.race);
        store<uint8_t>(p, 50, c.job);
        store<uint8_t>(p, 54, c.nation);
        store<uint8_t>(p, 57, c.size);
        store<uint8_t>(p, 60, c.face);
        lobby.view.sendAll(p);
        expectView(lobby.view, 0x03);
    }
    // Leave without selecting: entering now would need the key adjusted for the creation (§3.2).
}

} // namespace xi::lobby
