# FFXI login + lobby protocol: client-side spec (LandSandBoat)

Scope: everything a native client has to do, from typing a password up to holding a map-server address and a blowfish key. The client replaces both **xiloader** (auth + data socket) and the retail **FFXiMain lobby code** (view socket).

Server reference: a LandSandBoat checkout (`~/git/server` here) @ `5f2d9f3714` (2026-09-07). Every `src/...` citation below is relative to that tree.
Client references:
- LandSandBoat/xiloader tag **v2.1.2**: `xl212:src/network.cpp`, `xl212:src/main.cpp`. This tag matches the server's supported version 2.1.
- xiloader **master** (`ceb79a0`, v2.2.0): `xlm:src/...`.
- atom0s/XiPackets `lobby/*.md`: `XiP:<file>`.

Conventions:
- All multi-byte integers are **little-endian** unless marked "network order".
- "Offset" means a byte offset from the start of the message.
- **INFERRED** marks anything I reasoned out rather than read directly in code.

---

## 0. Transport summary

| Port (default) | Name | Transport | Server class | Who speaks first |
|---|---|---|---|---|
| 54231 | auth | **TLS** over TCP, JSON payload | `auth_session` | client |
| 54230 | data | **plain TCP** (no TLS) | `data_session` | client (0xFE) |
| 54001 | view | **plain TCP** (no TLS) | `view_session` | client (0x26) |
| 54230 | map | **UDP** (separate process) | `MapNetworking` | client (0x00A) |
| 54002 | search ("cache") | TCP | search server | out of scope |

Evidence:
- All three login listeners are created by the same `handler<T>`, which wraps every accepted socket in `asio::ssl::stream` (`src/login/handler.h:43-52,67`; ports from `src/login/connect_engine.cpp:55-57`).
- Only `auth_session` runs a TLS handshake (`src/login/auth_session.cpp:50-56`) and reads and writes through the TLS stream (`src/login/auth_session.cpp:82`, `auth_session.cpp:604`).
- `data_session` and `view_session` don't override `start()`. They use `handler_session::start/do_read/do_write`, which call `socket_.next_layer()`, the raw TCP socket (`src/login/handler_session.cpp:38-55,62,81`). **Data and view are plaintext.**
- The listener binds `tcp::v4()` on the port only, so the `LOGIN_*_IP` settings are not used for binding (`src/login/handler.h:43`).
- Map is UDP on `MAP_PORT` (`src/map/map_networking.cpp:66-67`, `src/map/map_socket.cpp:33`). The local server's settings are in `settings/network.lua:20-31`.

### Framing rules that apply to every login socket

1. **There is no length framing on receive.** Each server read is a single `async_read_some` into a zeroed 4096-byte buffer (`src/login/handler_session.cpp:58-75`, `src/login/handler_session.h` `buffer_` = `std::array<uint8,4096>`, `src/login/auth_session.cpp:78-96`). One read is treated as one message.
   - Send each message with **one** `send()`.
   - Set `TCP_NODELAY`.
   - **Never pipeline.** Wait for the expected reply, or for the expected trigger on the other socket, before sending the next message. If two messages are coalesced into one read, the second is silently dropped. (The dropping is INFERRED from the read-once design.)
2. **The server only re-arms a read after it writes something on that same socket** (`write_func() { do_read(); }`: `src/login/data_session.h:55-58`, `src/login/view_session.h:55-58`, `src/login/auth_session.h:128-131`). A request that gets no reply on its own socket leaves that socket deaf until the server writes to it for some other reason. The flows below are built around this.
3. **Session identity is (source IP as the server sees it, 16-byte session hash).** Sessions live in `authenticatedSessions_[ip][hash]` (`src/login/login_helpers.cpp:330-336,400-403`). All three connections (auth, data, view) **must come from the same source IP**.

---

## 1. Auth socket (TCP 54231, TLS, JSON)

### 1.1 TLS

- **Server side:**
  - Context is `asio::ssl::context::tls_server`, a generic TLS method (`src/login/handler.h:44`).
  - Options are `default_workarounds | verify_fail_if_no_peer_cert` (`handler.h:49`). The server never sets `verify_peer`, so **no client certificate is requested**. That conclusion is INFERRED from asio/OpenSSL semantics: `verify_fail_if_no_peer_cert` has no effect without `verify_peer`.
  - Key and cert files are `login.key` and `login.cert` in the server's working directory (`handler.h:51-52`).
  - If neither file exists, the server generates a **self-signed** RSA-4096 / SHA-384 cert valid for 20 years with `CN = LOGIN_AUTH_IP` (default `"0.0.0.0"`, local `"127.0.0.1"`) and `O = "LSB self-signed certificate for login server"` (`src/login/cert_helpers.h:79-148`, especially `:99,109-118`).
- **Client side (xiloader):**
  - mbedTLS client with `MBEDTLS_SSL_VERIFY_OPTIONAL` (`xl212:src/network.cpp:181`). Verification failures, including "not trusted", only produce warnings (`xlm:src/network.cpp:204-220`).
  - xiloader **never calls `mbedtls_ssl_set_hostname`**, so it sends **no SNI** and does no hostname check (`xlm:src/network.cpp:166-200`).
  - master adds `min_tls_version = TLS1_3` (`xlm:src/network.cpp:181`). v2.1.2 doesn't set it.
- **Requirement for our client:**
  - TLS 1.2+ (1.3 works).
  - No client cert.
  - SNI is optional; the server doesn't look at it (INFERRED: nothing in the code reads it).
  - Certificate verification must be **disabled or pinned to `login.cert`**, because the default cert is self-signed and its CN is an IP or `0.0.0.0`.

### 1.2 Request

Send one TLS record containing one UTF-8 JSON object (no terminator needed). It must be **under 4096 bytes**, and the whole object must arrive in a **single** `SSL_read` on the server (`auth_session.cpp:78-100`). The server parses the zero-padded 4096-byte buffer with `nlohmann::json::parse(buffer_, nullptr, false)`, and trailing NULs read as end of input (INFERRED from nlohmann's lexer behaviour).

| Key | JSON type (strictly checked) | Required | Server handling |
|---|---|---|---|
| `command` | **unsigned** integer | yes | `jsonGet<int8>` accepts only `is_number_unsigned()` (`src/login/login_helpers.h:157-163`). A missing or wrong type becomes 0 (NOOP) (`auth_session.cpp:152`). `0x10` = login. |
| `version` | array of exactly 3 unsigned ints | yes | `auth_session.cpp:159`, array check at `login_helpers.h:180-222`. `version[0]` and `version[1]` must equal `SupportedXiloaderVersion = {2,1,0}` (`src/login/auth_session.h:70`). Patch is ignored (`auth_session.cpp:162`). **Send `[2,1,0]`** (xiloader v2.1.2 sends `[2,1,2]`, `xl212:src/main.cpp:59`). |
| `username` | string | yes | Must be non-empty, ≤16 bytes, no byte < 0x20 (`auth_session.cpp:172`, `login_helpers.cpp:382-398`). |
| `password` | string | yes | Must be non-empty, ≤32 bytes, no byte < 0x20 (`auth_session.cpp:178`). |
| `otp` | string | only if the account has TOTP | `auth_session.cpp:156,226-254` |
| `new_password` | string | change-password (0x30) only | `auth_session.cpp:155` |
| `trust_token` | string | optional | `auth_session.cpp:157` |
| `trust_this_computer` | bool | optional | `auth_session.cpp:158` |

xiloader sends all of `username, password, otp, new_password, version, command`, and for `command==0x10` also `trust_token` and `trust_this_computer` (`xl212:src/network.cpp:489-499`). Extra keys are ignored.

Minimal login request:
```json
{"command":16,"version":[2,1,0],"username":"alice","password":"hunter22"}
```

Command codes (`src/login/auth_session.h:35-47`):

| Code | Command |
|---|---|
| `0x00` | NOOP |
| `0x10` | LOGIN_ATTEMPT |
| `0x20` | LOGIN_CREATE |
| `0x30` | CHANGE_PASSWORD |
| `0x31` | CREATE_TOTP |
| `0x32` | REMOVE_TOTP |
| `0x33` | REGENERATE_RECOVERY |
| `0x34` | VERIFY_TOTP |

### 1.3 Response

The server sends one JSON object (`json.dump()`, no NUL, no length prefix) in a single write (`auth_session.cpp:102-112`). nlohmann's default object sorts keys alphabetically (INFERRED; irrelevant to parsing).

**Success (`command 0x10`)** (`auth_session.cpp:300-335`):
```json
{"account_id":1000,"result":1,"session_hash":[12,250,3,...16 ints total...]}
```
- `result` = `1` (`LOGIN_SUCCESS`, `auth_session.h:53`).
- `account_id` = unsigned 32-bit account id (`auth_session.cpp:312`).
- `session_hash` = **JSON array of 16 integers 0..255**. It is MD5(`{timestamp ^ pid, seq}` as two uint32), assigned straight from an `unsigned char[16]` (`auth_session.cpp:302-313`). xiloader reads it with `jsonGet<char,16>` (`xlm:src/command_handler.h:72-77`). **Store all 16 bytes verbatim**; this is the session token for data and view.
- `trust_token` (string) appears only if `trust_this_computer` was true **and** a TOTP was verified (`auth_session.cpp:315-327`).
- After replying, the server registers `session[ip][hash] = {accountID, authorizedTime}` (`auth_session.cpp:333-335`). An authorized session that never opens data or view is purged after 15 minutes (`src/login/connect_engine.cpp:48,71-105`).

**Failures** (`result` values from `auth_session.h:50-68`):

| JSON | Meaning | Source |
|---|---|---|
| `{"result":2}` | bad user/pass, bad OTP, or IP lockout (5 failures/60 s) | `auth_session.cpp:195-206,248`, `login_helpers.cpp:341-375` |
| `{"result":0}` | account status lacks NORMAL (banned etc.) | `auth_session.cpp:213-222` |
| `{"result":19}` (0x13) | trust token invalid and no OTP given | `auth_session.cpp:239-243` |
| `{"error_message":"Your xiloader is too old..."}` (2.1 servers), `"Unsupported xiloader version ..."` (2.2) | version major/minor mismatch; the text names the wanted version (§1.4) | `auth_session.cpp:162-167` |
| *(no reply at all)* | malformed username/password (empty, too long, control chars) | `auth_session.cpp:172-182`. The server returns without writing **or** re-reading, so the client must time out. |
| single raw byte `0x02` or `0x0B`, then close | payload was not JSON | `auth_session.cpp:130-150` |

**Client action on success:**
- Keep `account_id` (uint32) and `session_hash` (16 bytes).
- Close the auth socket. xiloader does `shutdown(SD_BOTH)` (`xlm:src/command_handler.h:99`). The server would otherwise keep reading; it re-arms `do_read` after the write (`auth_session.h:128-131`).

**Hash quirk (server bug, affects us):**
- The server keys the session by `asStringFromUntrustedSource(hash,16)`, which is `strnlen`-truncated at the first `0x00` (`src/common/logging.h:148-159`, `auth_session.cpp:333`, `login_helpers.cpp:740-748`).
- If `session_hash[0] == 0x00` (probability 1/256), the key is the empty string. Both `view_session` and `data_session` treat `""` as "no session" (`view_session.cpp:38-43`, `data_session.cpp:74-84`), so the login silently hangs.
- **Client mitigation:** if `session_hash[0] == 0`, discard the hash and log in again. (The consequence is INFERRED from the code paths above.)
- A zero byte later in the hash is harmless, because both sides truncate identically.

### 1.4 Login protocol versions 2.1 and 2.2 (`--login-protocol`)

LSB raised `SupportedXiloaderVersion` from `{2,1,0}` to `{2,2,0}` in October 2026 (LSB `auth_session.h`, upstream
base `1a3c233b6a`; the change came with `f660a273e5`, `01e5b447e1`, `393e176732`). ffxi-linux speaks both:

| | 2.1 (LSB before 2026-10) | 2.2 (LSB since 2026-10) |
|---|---|---|
| auth `version` | `[2,1,0]` | `[2,2,0]` |
| `session_hash` | MD5 of a timestamp and a counter | 16 random bytes, also stored in `accounts_profile.session_hash` (the profile server's credential) |
| data / view packets | unchanged | unchanged (0x22 now also reads a Gold World Pass code at +0x50; we send none) |
| map session key (§3.2) | the 20 bytes the client sent in 0xA2 | `MD5(session_hash)` (16 bytes) then the client's lobby key counter (u32) |

The 2.2 key counter (LSB `view_session.cpp` / `data_session.cpp`): it starts at the 0x05 ResponseKey's `key` (+0x1C) + 1,
steps once for each request 0x07 / 0x14 / 0x1F / 0x21 / 0x22 / 0x24 / 0x28 / 0x2B and each reply 0x03 / 0x0B / 0x20 /
0x23 (and an 0x04 error answering 0x22 / 0x24 / 0x28 / 0x2B), and the server stores it + 3 when the 0xA2 arrives (one
step for the 0x0B, two at zone connect). For ffxi-linux's select visit (0x26, 0x1F, 0x24, 0x07) that is the 0x05 key
+ 9: LSB sends `0xAD5DE04F`, so the counter is `0xAD5DE058`, the very bytes `58 E0 5D AD` of xiloader's old constant.
Only the first 16 bytes changed. Live 2026-10-18: the server's log printed `key counter AD5DE058` for our logins.

The 2.2 server also hands each account behind one address its own game UDP port slot (`accounts_profile.udp_port_slot`),
announced through the profile (POL IRC) server's `profileAvailableNotice`. That matters to the retail client's fixed
UDP port; ffxi-linux uses an ephemeral port and no profile server, so it ignores it (two accounts from 127.0.0.1 at once
got slots 1 and 2 and both played).

**Choosing** (`lobby::LoginProtocol`): `--login-protocol auto|2.1|2.2` (xi-client, xi-headless, xi-setup), else
`login_protocol = ...` in the `[ffxi-linux]` section of `client.ini` (xi-client), else `XI_LOGIN_PROTOCOL`, else
`auto`. `auto` sends 2.2 first; if the auth server refuses the version, its `error_message` names the one it wants
(2.1 servers: "Please update to version '2.1.x'"; 2.2 servers: "This server requires version 2.2.x"), and the client
retries once with that protocol. The protocol that got in decides the map key.

**Testing:** 2.2 is verified live (2026-10-18: xi-client, two clients at once, xi-headless). No 2.1 server is left to
test against, so 2.1 is covered by tests that compare byte for byte with what ffxi-linux sent before the change
(`dc9c9c5`: the auth JSON body and the `00×16 58 E0 5D AD` key), test "login protocol 2.1 keeps the pre-2.2 auth body
and map key", plus the fallback parser on both servers' error texts.

---

## 2. Data socket (TCP 54230) and view socket (TCP 54001)

### 2.1 Connection order

1. Auth succeeds, then close auth (§1).
2. **Connect data (54230)**. xiloader does this right after auth (`xl212:src/main.cpp` login flow; `xlm:src/main.cpp:712-727`).
3. **Send data 0xFE** (session-hash registration). The server's reply is a **zero-length write**: the client receives nothing (`data_session.cpp:599-606`).
4. **Connect view (54001)**. In xiloader's world, FFXiMain does this after polcore launches the game.
5. Run the view state machine (§2.4). Two of the view requests are answered **on the data socket**, and the client must answer those on the data socket to release the view reply.

Ordering hazard: view `0x1F` errors with code 332 if `session.data_session` doesn't exist yet (`view_session.cpp:439-450`). The data and view handlers run on worker threads (`handler.h:67-71`). There is **no acknowledgement** of 0xFE, so make sure 0xFE has been sent before view `0x1F`. Doing the `0x26`→`0x05` round-trip in between gives enough slack in practice (INFERRED). A more robust alternative: send `0xFE`, then send `0x26` on view, await `0x05`, and only then send `0x1F`.

### 2.2 Lobby ("IXFF") packet header (view socket, both directions)

`struct packet_t` (`src/login/login_packets.h:27-37`, `XiP:Header.md`):

| Off | Size | Field | C→S (what the server checks) | S→C (what the server sends) |
|---|---|---|---|---|
| 0 | 4 | `packet_size` | **ignored** | total size |
| 4 | 4 | `terminator` | **ignored** (xiloader's hook uses it to recognise lobby packets, `xl212:src/main.cpp` `isLobbyCommand`; `xlm:src/main.cpp:245`) | bytes `49 58 46 46` "IXFF" (`login_packets.h:42-47`) |
| 8 | 4 | `command` | **only byte 8 is read** (`view_session.cpp:36`) | opcode |
| 12 | 16 | `identifer` | **must be the 16-byte session_hash**, on **every** view packet (`view_session.cpp:38-43`) | MD5 of the whole packet computed with this field zeroed (`login_packets.h:49-63`; e.g. `data_session.cpp:297-300`, `view_session.cpp:419-425`) |

- Retail puts an MD5 at offset 12. xiloader's `send` detour **overwrites bytes 12..27 with the session hash** for any lobby command to the data or view port (`xl212:src/main.cpp:258`, `xlm:src/main.cpp:263-267`). The server never verifies a client-side MD5.
- Recommendation: fill `packet_size`, `terminator` and `command` correctly anyway, and put the hash at 12.
- All responses are standard MD5 (verified: the server's `md5()` gives `900150983cd24fb0d6963f7d28e17f72` for "abc"). Our client may verify S2C identifiers but is not required to.

### 2.3 Data socket messages

The data socket uses a raw byte protocol, **not** IXFF. Byte 0 is the command (`data_session.cpp:93`). The session hash is read from bytes 12..27 (`data_session.cpp:74`). If that lookup fails, the server falls back to the hash remembered from the first packet on this socket (`data_session.cpp:75-91`).

#### C→S 0xFE "register" (28 bytes), sent first on connect (`xl212:src/network.cpp:585-586`)

| Off | Size | Value |
|---|---|---|
| 0 | 1 | `0xFE` |
| 1 | 11 | `00` |
| 12 | 16 | session_hash |

The server creates `session.data_session` (`data_session.cpp:86-91`) and replies with a 0-byte write (`:599-606`).

#### S→C 0x01 "send account" (5 bytes): `01 00 00 00 00`

The server sends this when view receives `0x1F` (`view_session.cpp:437-444`).

#### C→S 0xA1 "account id" (28 bytes), the reply to 0x01 (`xl212:src/network.cpp:618-625`)

| Off | Size | Value | Server use |
|---|---|---|---|
| 0 | 1 | `0xA1` | |
| 1 | 4 | account_id (LE) | **checked**: must equal `session.accountID`. On mismatch the server does **nothing** (no reply, socket deaf) (`data_session.cpp:101-103`). |
| 5 | 4 | server IPv4, **network order** (memory = octets in order, like `in_addr.s_addr`) | Stored as `session.serverIP` and echoed back as `cache_ip` (search server) in 0x0B (`data_session.cpp:105,370,457`). xiloader sends `inet_addr(serverAddress)` (`xlm:src/network.cpp:134`). |
| 9 | 3 | `00` | |
| 12 | 16 | session_hash | |

The server then loads up to `accounts.content_ids` characters (`data_session.cpp:109-139`) and, in this order:
1. Writes **0x03 on data** (`:280-289`).
2. Writes **0x20 on view** (`:291-305`).

#### S→C 0x03 "character id list" (exactly 0x148 = 328 bytes) (`data_session.cpp:150,222-228,282-288`)

| Off | Size | Field |
|---|---|---|
| 0 | 1 | `0x03` |
| 1 | 1 | count (= `characters` in 0x20, empty slots included) |
| 2 | 14 | `00` |
| 16·(i+1) + 0 | 4 | content_id = charid |
| 16·(i+1) + 4 | 2 | charid & 0xFFFF |
| 16·(i+1) + 6 | 1 | worldId (0) |
| 16·(i+1) + 7 | 1 | (charid >> 16) & 0xFF |

- Entries exist only for real characters (i < number of DB rows). Empty slots are zero here (INFERRED: `uList` is zero-initialised at `:150` and only filled for DB rows).
- xiloader uses this to patch polcore's character table (`xlm:src/network.cpp:579-594`). **Our client can ignore it** and use 0x20 instead, but it must read and consume the 328 bytes.

#### S→C 0x02 "send key" (5 bytes): `02 00 00 00 00`

The server sends this when view `0x07` passes its checks (`view_session.cpp:86-91`).

#### C→S 0xA2 "key" (28 bytes), the reply to 0x02 (`xl212:src/network.cpp:630-633`)

| Off | Size | xiloader value | Server use |
|---|---|---|---|
| 0 | 1 | `0xA2` | |
| 1 | 20 | **key3**: `00×16` then `58 E0 5D AD` | Copied verbatim into `key3[20]` (`data_session.cpp:312-313`), mutated (§3), stored as `accounts_sessions.session_key` (`:483-491`). |
| 21 | 7 | `00 00 00 00` + 3 stale bytes | ignored |

- Decoded literal: `a2 00×16 58 e0 5d ad 00 00 00 00`, 25 bytes, `sendSize = 28`. In xiloader, bytes 25..27 are leftovers of the previous 0xA1 buffer (session_hash[13..15]); the server ignores them.
- **Byte 12 must be 0x00.** Bytes 12..27 overlap the hash slot. With `buf[12]==0` the hash lookup yields `""`, so the server always falls back to the remembered hash (`data_session.cpp:74-84`, `login_helpers.cpp:740-748`). xiloader's layout guarantees this because key3[11] = 0.
- **The session hash is not needed here.**

The server responds with 0x0B on **view** (or an error 0x04 on view) and closes view (§2.4).

#### Other data-socket notes

- `0xFE` may be re-sent at any time; it is answered with a 0-byte write and is effectively a no-op keepalive (`data_session.cpp:599-606`).
- Unknown commands are ignored with no reply, so the socket goes deaf.
- Close semantics: when the data socket closes **and** view is gone, the server erases the whole session (`data_session.cpp:611-640`, `view_session.cpp:487-516`). To return to character select later without re-authenticating, **keep the data socket open while in-game** (INFERRED from the cleanup logic; the retail flow re-runs the lobby login on logout, `XiP:Protocol.md` "Exiting FFXI Back To Main Menu").

### 2.4 View socket messages

The first view packet creates `session.view_session` (`view_session.cpp:45-50`). Only one view socket per session is tracked: if an old one is still registered, replies go to the old socket (INFERRED from `:46-49`). The server nulls it after 0x0B (`data_session.cpp:560`).

#### C→S 0x26 RequestLobbyLogin (0x98 = 152 bytes) (`XiP:C2S_0x0026_RequestLobbyLogin.md`)

| Off | Size | Field | Server |
|---|---|---|---|
| 0 | 28 | header, command `0x26`, size `0x98`, identifier = session_hash | hash checked |
| 28 | 16 | `pol_account` | ignored (retail sends zeros) |
| 44 | 8 | `client_code` | ignored (retail: `05 00 00 00 00 00 00 00` in sample) |
| 52 | 64 | `authCode` | ignored |
| **116 (0x74)** | 16 | `versionCode` ASCII, e.g. `"30260604_0"` | **first 6 chars compared** (`view_session.cpp:338`) |
| 132 | 4 | `excode_client` | ignored |
| 136 | 16 | 4× uint32 `0` | ignored |

Version check (`view_session.cpp:336-387`):
- The server builds `first6(client) + "xx_x"` and compares it against `first6(login.CLIENT_VER) + "xx_x"`.
- `VER_LOCK` (`settings/login.lua:22`, local = 2):
  - 1: exact match required.
  - 2: client ≥ server, using a lexicographic string comparison.
- On a fatal mismatch the reply is error 0x04 code **331**.
- Local `CLIENT_VER = '30260604_0'` (`settings/login.lua:15`); the default is `'30260805_0'` (`settings/default/login.lua:15`). **Send the server's exact CLIENT_VER string.**

#### S→C 0x05 ResponseKey (0x28 = 40 bytes) (`view_session.cpp:391-434`, `XiP:S2C_0x0005_ResponseKey.md`)

| Off | Size | Value |
|---|---|---|
| 0 | 4 | `0x28` |
| 4 | 4 | "IXFF" |
| 8 | 4 | `0x05` |
| 12 | 16 | MD5 (computed over 0x28 bytes with the identifier zeroed) |
| 28 | 4 | `key` = constant `0xAD5DE04F` (`:412`) |
| 32 | 4 | `excode_server`: expansion bitmask (`login_helpers.cpp:489-516`, bits in `login_helpers.h:46-63`) |
| 36 | 4 | `excode_server2`: features (wardrobes 3-8, `SECURE_TOKEN` = bit0 if TOTP) (`login_helpers.cpp:518-546`, `login_helpers.h:67-84`) |

Our client can ignore `key`. It is retail's lobby MD5 key and is unrelated to what the server stores for the map. Note that 0xAD5DE04F + 9 = 0xAD5DE058, xiloader's constant (INFERRED: retail derives its own map key from this value).

#### C→S 0x1F RequestGetChr (0x2C = 44 bytes) (`XiP:C2S_0x001F_RequestGetChr.md`)

- Layout: header (command `0x1F`, hash at 12) followed by `passwd[16]` (ignored).
- The server writes **0x01 on data**, not view (`view_session.cpp:437-444`).
- **The view socket gets no reply until you answer 0x01 with 0xA1 on data.** Then 0x20 arrives on view.
- If no data session exists, view gets error 332 (`:445-450`).

#### S→C 0x20 ResponseChrInfo2 (size = 32 + 140·N) (`data_session.cpp:157-160,291-305`; struct in `login_packets.h:84-154`)

Packet layout:

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `packet_size` = 28 + 4 + 140·`characters` |
| 4 | 4 | "IXFF" |
| 8 | 4 | `0x20` |
| 12 | 16 | MD5 |
| 28 | 4 | `characters` = number of DB characters (filtered to GMs if MAINT_MODE) + empty slots up to `content_ids` if CHARACTER_CREATION (`data_session.cpp:231,237-247`) |
| 32 + 140·i | 140 | `lpkt_chr_info_sub2[i]` |

`lpkt_chr_info_sub2` layout. Offsets were verified by compiling `login_packets.h` with g++ x86-64: sub2 = 140, TC_OPERATION_MAKE = 96, next_login = 72, world_list = 52.

| Off | Size | Field | LSB value |
|---|---|---|---|
| 0 | 4 | `ffxi_id` | content id = **charid** (`data_session.cpp:177,185`) |
| 4 | 2 | `ffxi_id_world` | charid & 0xFFFF |
| 6 | 2 | `worldid` | 0 |
| 8 | 2 | `status` | 1 = available (also used for empty slots) |
| 10 | 1 | bit0 `renamef`, bit1 `race_change` | |
| 11 | 1 | `ffxi_id_world_tbl` | (charid >> 16) & 0xFF |
| 12 | 16 | `character_name` | NUL-padded. **Empty slot = `" "` (0x20, 0x00)** (`:244`) |
| 28 | 16 | `world_name` | SERVER_NAME (≤15) |
| 44 | 96 | `character_info` (TC_OPERATION_MAKE) | race u16 @44 (mon_no), mjob u8 @46, sjob u8 @47, face u16 @48, nation u8 @50, hair u8 @52, size u8 @53, GrapIDTbl[8] u16 @56..71 (face, head, body, hands, legs, feet, main, sub), zone_no u8 @72, mjob_level u8 @73, zone_no2 (zone high bit) u8 @79 (`data_session.cpp:196-220`) |

"Slot 0" is `character_info[0]`. Slot order is DB row order (no `ORDER BY` in `data_session.cpp:123-139`; INFERRED to be effectively charid/insert order). The client should pick by `ffxi_id` + name, not trust a slot index across sessions.

#### C→S 0x24 RequestQueryWorldList (0x2C = 44 bytes)

- Layout: header (command `0x24`, hash) followed by `passwd[16]` (ignored).
- The reply arrives directly on view (`view_session.cpp:453-483`).

#### S→C 0x23 ResponseWorldList (0x34 = 52 bytes)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `packet_size` = 0x34 |
| 4 | 4 | "IXFF" |
| 8 | 4 | `0x23` |
| 12 | 16 | MD5 |
| 28 | 4 | `sumofworld` = 1 |
| 32 | 4 | `no` = 0x20 |
| 36 | 16 | world name = SERVER_NAME |

Sources: `view_session.cpp:458-480`, `login_packets.h:156-168`.

#### C→S 0x07 RequestSelectChr (0x58 = 88 bytes) (`XiP:C2S_0x0007_RequestSelectChr.md`)

| Off | Size | Field | Server |
|---|---|---|---|
| 0 | 28 | header, command `0x07`, hash | hash checked |
| **28** | 4 | `ffxi_id` = charid | **checked** (`view_session.cpp:58`) |
| 32 | 4 | `ffxi_id_world` | ignored |
| **36** | 16 | `character_name` | **first 15 bytes checked** against `chars.charname` (`:59-66`) |
| 52 | 16 | `passwd` | ignored |
| 68 | 4 | unknown (retail always 3) | ignored |
| 72 | 16 | `authcode_checksum` | ignored |

- Checks: `SELECT accid FROM chars WHERE charid=? AND charname=?` must return a row (`view_session.cpp:64-77`), and that accid must equal `session.accountID` (`:79-84`). Name matching uses the DB collation (INFERRED: probably case-insensitive).
- On failure the server **closes the view socket**; there is no error packet.
- On success it sets `session.requestedCharacterID` (`:70`) and writes **0x02 on data** (`:86-91`). There is no view reply yet.

#### S→C 0x0B ResponseNextLogin (0x48 = 72 bytes), sent after the client's 0xA2 (`data_session.cpp:309-575`)

| Off | Size | Field | LSB value |
|---|---|---|---|
| 0 | 4 | `packet_size` | 0x48 |
| 4 | 4 | "IXFF" | |
| 8 | 4 | `command` | 0x0B |
| 12 | 16 | MD5 | over 72 bytes, identifier zeroed (`:537-540`) |
| 28 | 4 | `ffxi_id` | **charid** (`:380`) |
| 32 | 4 | `ffxi_id_world` | charid & 0xFFFF (`:381`) |
| 36 | 16 | `character_name` | DB name, NUL-padded (`:373-378`) |
| 52 | 4 | `server_id` | (charid >> 16) & 0xFF (`:382`) |
| **56** | 4 | `server_ip` | **map server IPv4, network order** = `inet_pton(zone_settings.zoneip)` (`:364,367`; `src/common/ipp.cpp:34-40`) |
| **60** | 4 | `server_port` | **map UDP port** (uint32 LE, value = `zone_settings.zoneport`, normally 54230) (`:365,368`) |
| 64 | 4 | `cache_ip` | search server IP = whatever the client sent at 0xA1[5..8] (`:370,457`) |
| 68 | 4 | `cache_port` | `network.SEARCH_PORT` (54002) (`:371,458`) |

The zone used is `pos_zone`, or `pos_prevzone` if `pos_zone = 0` (`data_session.cpp:342-347`).

After writing 0x0B **synchronously**, the server:
- Shuts down and closes the **view socket** (the "client waits for us to close the socket" comment, `:542-560`).
- Resets `incrementKeyValue`, `justCreatedNewChar` and `generatedCharInfo` (`:562-564`).
- Sets `char_stats.zoning = 2` and clears `disconnecting` (`:571-572`).
- Sends IPC `CharZone{charid, zone}` to the map server (`:566-574`). That IPC is what creates the map's pending session (§4).

#### S→C 0x04 ResponseError (0x24 = 36 bytes) (`login_helpers.cpp:466-487`)

| Off | Size | Value |
|---|---|---|
| 0 | 4 | `0x24` |
| 4 | 4 | "IXFF" |
| 8 | 4 | `0x04` |
| 12 | 16 | MD5 |
| 28 | 4 | `0x10` (byte 28) |
| 32 | 2 | error code (uint16) |
| 34 | 2 | 0 |

Codes used (`src/login/login_errors.h:28-38`; retail displays code + 3000, per `XiP:Protocol.md`):

| Code | Meaning |
|---|---|
| 201 | already logged in |
| 208 | world full |
| 305 | unable to connect to world server |
| 313 | name unavailable |
| 314 | name-server registration failed |
| 321 | bad character parameters |
| 331 | version |
| 332 | could not connect to lobby |

Error sites relevant to the happy path:
- After 0xA2 (`data_session.cpp:323-331,434-535`):
  - account 0 / corrupt: 332 written **on the data socket**, which is then closed.
  - zone at player cap: 208.
  - same char already in `accounts_sessions`: **201**, and `incrementKeyValue += 1`.
  - INSERT failure: 305.
  - MAINT_MODE or LOGIN_LIMIT block: 332.
  - no zone row: 305.
  - `server_ip == 0` (zone disabled): 305.
- After 0xA1 with MAINT_MODE and no GM characters: 332 on view (`:252-261`).

#### S→C 0x03 ResponseOk (0x20 = 32 bytes)

- Layout: header with command `0x03`, MD5 over 0x20 bytes, then 4 zero bytes.
- Used for create (0x22 and 0x21), delete (0x14) and rename (0x28) (`view_session.cpp:106-121,188-204,225-240,318-333`). Not needed for the happy path.

#### C→S 0x22 / 0x21 character creation (`xi-setup create-char`, live 2026-10-18)

- After the character and world lists (§2.4), on the view socket:
  - **0x22** (we send 0x30 bytes): the name at +32 (15 chars). Answer 0x03, or 0x04 with the error code at +32
    (FFXI-3313: name taken; `characterCreationError`, `login_helpers.cpp:310-327`: creation off, name invalid, or
    `content_ids` slots used up).
  - **0x21** (we send 0x40 bytes): race +48 (1..8), main job +50 (clamped to 1..6), nation +54 (0..2), size +57
    (0..2), face +60 (0..15) (`login_helpers.cpp:628-690`); the start zone is a random one of the nation's three cities.
    Answer 0x03.
- A select in the same visit would need the key bumped by 6 (§3.2); `xi-setup` leaves instead, and the next login lists
  the new character.
- Account creation is the auth command `0x20` (§1.2) with the same JSON; result 3 = created, 4 = taken, 8 = creation
  off (`login.ACCOUNT_CREATION`), 9 = error (`auth_session.cpp:338-405`).

---

## 3. Blowfish key material

### 3.1 What the server does with the 0xA2 key (`data_session.cpp:309-491`)

```
key3[0..19] = 0xA2_packet[1..20]                       // data_session.cpp:312-313
if (session.justCreatedNewChar)  key3[16] += 6;        // :356-359  (uint8 add, wraps mod 256)
key3[16] += session.incrementKeyValue;                 // :362      (uint8 + uint8, wraps)
INSERT accounts_sessions(..., session_key = key3, ...) // :483-491
```

- `session_key` is `binary(20)` (`sql/accounts_sessions.sql:28`). `uint8[20]` arrays bind as blobs (`src/common/database/traits.h:60`).
- `incrementKeyValue` (uint8, `src/login/session.h:43`) accumulates during the current lobby visit:
  - `+4` per successful character delete (`view_session.cpp:158-159`).
  - `+4` per successful rename (`view_session.cpp:306-307`).
  - `+1` each time 0xA2 hits "same character already logged in" (`data_session.cpp:475`).
- `justCreatedNewChar` is set by a successful 0x21 create (`view_session.cpp:184`).
- Both reset to 0/false after a successful 0x0B (`data_session.cpp:562-563`).
- These mirror increments the **retail** client applies to its own key: "The client only advances its key for character creation once" (`data_session.cpp:563`). That is why xiloader can send a constant.
- **The server never sends the key back.** The client must predict the stored key.

### 3.2 What the client must keep

(This section is the 2.1 protocol. Since LSB's 2026-10 change (2.2, §1.4) the server replaces `K[0..15]` with
`MD5(session_hash)` and `K[16..19]` with the lobby key counter, whatever the 0xA2 carried.)

Let `K[20]` be the exact 20 bytes the client put at 0xA2 offset 1..20. When 0x0B arrives, compute:

```
K'      = K
K'[16]  = uint8(K[16] + (justCreatedThisVisit ? 6 : 0) + incrementKeyValueThisVisit)
```

For the happy path (no create, delete, rename or retry before select), `K' = K`.

Choosing K:
- Mirror xiloader for v1: `K = 00×16 || 58 E0 5D AD`. As uint32[5] LE that is `{0,0,0,0,0xAD5DE058}`.
- The server accepts **any** 20 bytes, so a random K is fine. The only constraint is that `K[11]` (packet byte 12) must be `0x00` (§2.3 0xA2).
- Picking K randomly per login is INFERRED-safe: the map loads whatever is in `session_key` (§3.3). This is untested against the retail client, which this project doesn't use.

### 3.3 How the map server turns that into a blowfish key

- First unencrypted 0x00A from the client: the map loads `accounts_sessions.session_key` into `blowfish.key` (`uint32 key[5]`, 20 bytes) and calls `initBlowfish()` (`src/map/map_networking.cpp:293-310`, `src/common/blowfish.h:34-41`).
- `initBlowfish` (`src/map/map_session.cpp:40-53`):
  1. `hash = MD5(key, 20 bytes)` (standard MD5).
  2. Find the **first** `hash[i] == 0` and zero `hash[i..15]` (truncate at the first NUL).
  3. `blowfish_init(hash, 16, P, S)`.
- `blowfish_init` (`src/common/blowfish.cpp:452-505`) is the standard Blowfish key schedule over the standard pi-digit P/S table (`subkey[4168]`, `blowfish.cpp:25-287`), with **two non-standard quirks the client must copy bit-exactly**:
  - Key bytes are `int8` (signed): `data = (data << 8) | key[j]` **sign-extends** bytes ≥ 0x80 (`blowfish.cpp:452,472`).
  - The round function is **not** standard Blowfish F (`blowfish.cpp:290-293`):
    ```
    TT(x) = ((S[256 + ((x>>8)&0xff)] & 1) ^ 32) + ((S[768 + (x>>24)] & 1) ^ 32) + S[512 + ((x>>16)&0xff)] + S[x & 0xff]
    ```
    The encipher and decipher round order is in `blowfish.cpp:295-358`.
- Mode is ECB on 8-byte blocks, starting at **byte 28 (0x1C)** of the UDP datagram (header unencrypted). The client→server direction is deciphered as `(uint32*)buff + 7`, using `(len-0x1C)/4` words rounded down to an even count (`map_networking.cpp:162-189`). The server→client direction is enciphered with the **same** P/S (`map_networking.cpp:705-737`).
- **On every zone change**, after the server sends world 0x00B, `incrementBlowfish()` runs:
  - `prev_blowfish = blowfish; key[4] += 2` (a **uint32** add, so it can carry out of byte 16), then re-init (`map_session.cpp:30-38`).
  - The new key is written back to the DB (`map_networking.cpp:642-644` region).
  - The client must do the same: `u32le(K'[16..19]) += 2`, then re-derive.

Login and map add to the key differently. Login adds to **byte** 16 with no carry; map adds to **uint32** `key[4]`. The client must use the matching one at each step.

---

## 4. What the client carries into the map server

| Item | Source |
|---|---|
| `account_id` (uint32) | auth JSON `account_id` (§1.3) |
| `charid` (uint32) | 0x0B @28 (`ffxi_id`), the same value as 0x20 `ffxi_id` |
| character name | 0x0B @36 |
| map IPv4 (network order) | 0x0B @56 |
| map UDP port | 0x0B @60 (uint32 LE, use the low 16 bits) |
| search IP:port | 0x0B @64 / @68 |
| 20-byte key `K'` | §3.2 |
| session_hash (16) | auth. Not used by map; keep it if you want to re-enter the lobby. |

Map entry pre-requisites (pointer only; the world-packet details belong in a separate spec):
- The map accepts the first 0x00A for a charid only if a **pending session** exists. That session is created when the map receives the login server's IPC `CharZone` (`src/map/ipc_client.cpp:212-223`, `src/map/map_session_container.cpp:69-90`, consumed at `map_networking.cpp:250-267`).
- The IPC is sent **after** 0x0B is written (`data_session.cpp:542-574`). A 0x00A that races ahead of the IPC is dropped silently (`return -1`), so **retry 0x00A until the server answers** (INFERRED; the retail client resends).
- The first 0x00A is **unencrypted**. The map server validates:
  - MD5 of `buff[0x1C .. len-16)` must equal the last 16 bytes (`map_networking.cpp:200-212`; `src/common/utils.cpp:41-52`).
  - The sub-packet id `& 0x1FF` must be `0x00A` (`:214-219`).
  - The packet must be at least `0x1C + sizeof(GP_CLI_LOGIN)` = 0x1C + 92 bytes (`:222-225`).
  - `LoginPacketCheck` (byte 4 of the sub-packet) must be the uint8 sum of sub-packet bytes `8 .. 91` (`:231-246`; struct at `src/common/mmo.h:424-442`).
  - `UniqueNo` (sub-packet offset 12) must be the charid (`:248`).
- From then on, packets are blowfish'd with `K'` (§3.3).

---

## 5. Minimal happy path: existing account, slot 0, get map address

Preconditions: the account exists, has ≥1 character, `MAINT_MODE=0`, the character isn't in `accounts_sessions`, and the zone isn't disabled.

```
AUTH  (TLS, 127.0.0.1:54231)
 C→S  {"command":16,"version":[2,1,0],"username":U,"password":P}
 S→C  {"account_id":A,"result":1,"session_hash":[h0..h15]}
      if h0==0 → reconnect auth and log in again (§1.3 quirk)
      close auth

DATA  (plain TCP, :54230)            VIEW  (plain TCP, :54001)
 C→S  [FE 00×11 H(16)]   (28 B)
      (no reply)
                                      C→S  0x26 (152 B): hdr{0x98,"IXFF",0x26,H}
                                           @0x74 = CLIENT_VER (e.g. "30260604_0")
                                      S→C  0x05 (40 B) ResponseKey (ignore key)
                                      C→S  0x1F (44 B): hdr{0x2C,"IXFF",0x1F,H} + 16×00
 S→C  [01 00 00 00 00]   (5 B)
 C→S  [A1 A(4,LE) SRVIP(4,netorder) 00 00 00 H(16)]  (28 B)
 S→C  [03 n ... ]        (328 B)  — consume
                                      S→C  0x20 (32+140·n B) ResponseChrInfo2
                                           slot0 = entry @32: ffxi_id(@0) = CHARID,
                                           name(@12,16B) = NAME  (skip if name==" ")
                                      C→S  0x24 (44 B): hdr{0x2C,"IXFF",0x24,H} + 16×00
                                      S→C  0x23 (52 B) world list (1 world, 0x20, SERVER_NAME)
                                      C→S  0x07 (88 B): hdr{0x58,"IXFF",0x07,H}
                                           @28 CHARID (u32) @32 CHARID&0xFFFF (u32)
                                           @36 NAME (16B NUL-padded) @52 16×00
                                           @68 u32 3  @72 16×00
 S→C  [02 00 00 00 00]   (5 B)
 C→S  [A2 K(20) 00×7]    (28 B)   K = 00×16 58 E0 5D AD ; K[11] must be 0
                                      S→C  0x0B (72 B) ResponseNextLogin:
                                           @28 charid  @56 map IP (net order)
                                           @60 map port (u32)  @64/@68 search ip/port
                                      server closes VIEW
      keep DATA open (optional, for return-to-lobby)

MAP   (UDP, map IP:port from 0x0B)
      key = K' = K (no create/delete/rename/retry this visit)
      send unencrypted 0x00A with UniqueNo=charid; retry until answered (§4)
```

Waiting rules (see §0 framing):
- Wait for `0x05` before `0x1F`.
- After `0x1F`, wait for data `0x01`; after `0xA1`, read **both** data `0x03` and view `0x20`.
- Wait for `0x23` before `0x07`.
- After `0x07`, wait for data `0x02`; after `0xA2`, read view `0x0B` (or `0x04`), then expect EOF on view.
- Read view messages by their `packet_size` (first u32). Read data messages by fixed size: 0x01 and 0x02 are 5 bytes, 0x03 is 328 bytes.

Retail sends `0x26`, `0x1F`, `0x24` in that order (`XiP:Protocol.md` "Client Login"). The server has no ordering requirement between `0x24` and `0x1F`. It does require the data session before `0x1F` (§2.1).

---

## 6. Open questions / things to verify against the live server

1. **xiloader version skew.** xiloader master is now v2.2.0 and sends `[2,2,0]` (`xlm:src/main.cpp:62`), which this server checkout rejects (it needs 2.1.x, `auth_session.h:70`). Our client should send `[2,1,0]` to *this* tree, and probably make the value configurable for when the server is updated. Did v2.2 change anything on the wire besides `min TLS 1.3` and polcore relay work? The diff of `network.cpp` v2.1.2→master shows no wire changes on auth/data (checked: only TLS min version, error logging, and removal of the old listen-server/PolDataComm code).
2. **`session_hash[0]==0` hang.** This is a real server bug (1/256 logins). Worth an upstream report: `asStringFromUntrustedSource` truncates binary hashes at NUL. Our client mitigates by re-authenticating.
3. **Docker/NAT source IP.** Session lookup keys on the source IP as the server sees it. Under Docker with port publishing, all three TCP connections normally appear from the same gateway IP, but confirm this with a capture. `zone_settings.zoneip` must be an address reachable from the client, because it lands verbatim in 0x0B @56 (memory note: "advertised IP gotcha").
4. **0xFE vs 0x1F race.** There's no ack for 0xFE. Confirm empirically that 0x26→0x05 is enough delay, or add a small sleep.
5. **Key choice.** Confirm on the live server that a non-xiloader K (random 20 bytes with K[11]=0) works end-to-end on the map. Everything in the code says yes, but it hasn't been tested.
6. **Character name matching in 0x07** depends on the MariaDB collation of `chars.charname`. Send the exact bytes from 0x20 to avoid ambiguity.
7. **Map world-packet framing** (0x00A body fields beyond `UniqueNo`, the S2C 0x00A reply, sequence numbers, zlib compression) needs its own spec. This document stops at the handoff.
