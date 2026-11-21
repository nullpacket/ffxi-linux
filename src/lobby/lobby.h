// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/socket.h"
#include "xi/blowfish.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xi::lobby
{

using SessionHash = std::array<std::byte, 16>;

// Login protocol, by the xiloader version it speaks (docs/protocol-login.md §1.4):
//   V21  LSB before 2026-10: the map key is xiloader's constant (16 zero bytes, 58 E0 5D AD);
//   V22  LSB since 2026-10: the map key is MD5(session hash) + the lobby key counter (mapKey());
//   Auto try 2.2 and, if the server refuses the version, the one its error message names (once).
enum class LoginProtocol
{
    Auto,
    V21,
    V22,
};

// "auto", "2.1", "2.2" -> the protocol; nullopt for anything else.
std::optional<LoginProtocol> parseLoginProtocol(std::string_view text);
// The default: XI_LOGIN_PROTOCOL if set and valid, else Auto.
LoginProtocol defaultLoginProtocol();
// The xiloader version sent in the auth request (Auto counts as 2.2, tried first).
std::array<uint8_t, 3> xiloaderVersion(LoginProtocol p);
// The protocol a server's "unsupported version" auth error asks for ("... version '2.1.x'" from LSB before 2026-10,
// "... requires version 2.2.x" since), or nullopt.
std::optional<LoginProtocol> requiredProtocol(std::string_view errorMessage);

struct Config
{
    std::string host     = "127.0.0.1";
    uint16_t    authPort = 54231;
    uint16_t    dataPort = 54230;
    uint16_t    viewPort = 54001;

    std::string username;
    std::string password;
    std::string otp;

    // Must satisfy the server's SupportedXiloaderVersion major.minor (src/login/auth_session.h).
    // Which login protocol to speak (--login-protocol, client.ini login_protocol, XI_LOGIN_PROTOCOL).
    LoginProtocol protocol = defaultLoginProtocol();

    // Compared against login.CLIENT_VER (first 6 chars, VER_LOCK rules).
    std::string clientVersion = "30260604_0";

    // Optional pinned login.cert for TLS verification.
    std::string pinnedCert;
};

struct AuthResult
{
    uint32_t      accountId = 0;
    SessionHash   sessionHash{};
    LoginProtocol protocol = LoginProtocol::V22; // the one the server accepted (never Auto)
};

struct CharacterSlot
{
    uint32_t    charId = 0;
    std::string name;
    std::string world;
    uint16_t    race     = 0;
    uint8_t     mainJob  = 0;
    uint8_t     mainLvl  = 0;
    uint8_t     subJob   = 0;
    uint16_t    zone     = 0;
    bool        occupied = false;
};

struct MapHandoff
{
    uint32_t                accountId = 0;
    uint32_t                charId    = 0;
    std::string             charName;
    net::Endpoint           map;
    net::Endpoint           search;
    Blowfish::SessionKey    key{};
    SessionHash             sessionHash{};
};

// Thrown for lobby error packets (0x04) and protocol violations.
struct LobbyError : std::runtime_error
{
    LobbyError(const std::string& what, uint16_t errorCode = 0)
    : std::runtime_error(what)
    , code(errorCode)
    {
    }
    uint16_t code;
};

// Auth (TLS/JSON) step. Retries automatically on the server's session_hash[0] == 0 quirk, and with Auto on a
// refused xiloader version.
AuthResult authenticate(const Config& cfg);

// Runs data + view sockets through to the map handoff. `choose` picks a character from the list
// (return its index); if it returns nullopt the lobby is abandoned.
MapHandoff enterWorld(const Config& cfg, const AuthResult& auth,
                      const std::function<std::optional<std::size_t>(const std::vector<CharacterSlot>&)>& choose);

// The map session key for a plain select visit with the 2.1 protocol (§3.2): xiloader's constant.
Blowfish::SessionKey legacyMapKey();

// The map session key for a plain select visit with the 2.2 protocol (§3.2): MD5(session hash), then the client's lobby key counter. The
// counter starts at the 0x05 key + 1 and steps for 0x1F, 0x20, 0x24, 0x23 and 0x07, plus 3 (0x0B and the zone
// connect), so the 0x05 key + 9; LSB's 0x05 key 0xAD5DE04F gives the old constant 58 E0 5D AD.
Blowfish::SessionKey mapKey(const SessionHash& hash, uint32_t viewKey);

// The auth request's JSON body (§1.2); the protocol only changes "version".
std::string authRequestJson(const Config& cfg, uint32_t command, LoginProtocol protocol);

// Creates an account (the auth server's LOGIN_CREATE; LSB's login.ACCOUNT_CREATION must be on). Throws LobbyError.
void createAccount(const Config& cfg);

struct NewCharacter
{
    std::string name;       // 3..15 letters, first upper case (the server checks)
    uint8_t     race   = 1; // 1 Hume M, 2 Hume F, 3 Elvaan M, 4 Elvaan F, 5 Tarutaru M, 6 Tarutaru F, 7 Mithra, 8 Galka
    uint8_t     face   = 0; // 0..15
    uint8_t     size   = 1; // 0 small, 1 medium, 2 large
    uint8_t     job    = 1; // 1..6: WAR MNK WHM BLM RDM THF
    uint8_t     nation = 0; // 0 San d'Oria, 1 Bastok, 2 Windurst
};

// Creates a character on the account through the lobby (0x22 name, 0x21 create). Throws LobbyError (e.g. FFXI-3xxx for
// a taken name). Leaves the lobby afterwards; log in again to play it.
void createCharacter(const Config& cfg, const AuthResult& auth, const NewCharacter& c);

} // namespace xi::lobby
