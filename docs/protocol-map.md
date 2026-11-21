# FFXI Map Server UDP Protocol: Client-Side Spec (LandSandBoat)

Source of truth: a LandSandBoat checkout (`~/git/server` here). All paths below are relative to that root.
Every claim cites `file:line`. Anything marked **INFERRED** is not directly enforced or produced by server code: it is a conclusion about retail behaviour or a recommendation for our client.

Scope: everything after the lobby has handed us map IP/port, char id, and the blowfish key material. Lobby (TCP 54230/54001, 0xA1/0xA2/etc.) is documented separately.

Byte order: everything is little-endian (x86) unless stated otherwise. The server reads and writes raw structs on LE hosts (`src/common/zlib.cpp:34-62` is the only place with a big-endian path, and it only byte-swaps the compression table).

---

## 0. Quick reference

```
Datagram (both directions)
+--------------------+---------------------------------------------+--------+
| 28-byte header     | body                                        | MD5    |
| (never encrypted)  |                                             | 16 B   |
+--------------------+---------------------------------------------+--------+
                     |<---- blowfish-ECB over floor(N/8) 8-byte blocks ---->|   N = len - 28
                     |<---- MD5 input --------------------------->|

S2C body = [0x01][Huffman bitstream ...][u32 bitCount]      (always compressed)
C2S body = [sub-packet][sub-packet]...                      (uncompressed; compressed form also accepted)

Sub-packet = u16 (id:9 | sizeWords:7 << 9), u16 sync, payload... (total = sizeWords*4 bytes)
```

Constants:

| Name | Value | Source |
|---|---|---|
| `FFXI_HEADER_SIZE` | 0x1C (28) | `src/common/mmo.h:41` |
| Server scratch / max datagram buffer | 2500 bytes | `src/map/map_constants.h:78` |
| Max sub-packets per S2C datagram | 32 | `src/map/map_constants.h:79`, `src/map/map_networking.cpp:528` |
| S2C compressed-payload cap | (compressed bytes + 4) <= 1300 - 28 - 16 = 1256 | `src/map/map_networking.cpp:593` |
| Per-sub-packet buffer | 0x1FF bytes | `src/map/packets/basic.h:32` |
| Link-dead threshold | 5 s with no accepted packet | `src/map/map_session_container.cpp:210` |
| Session drop | `map.MAX_TIME_LASTUPDATE` = 60 s | `settings/default/map.lua:18`, `src/map/map_session_container.cpp:200,226` |
| Cleanup sweep cadence | 5 s | `src/map/map_constants.h:63`, `src/map/map_engine.cpp:233` |
| Default map UDP port | `network.MAP_PORT` (54230) | `src/map/map_networking.cpp:66` |

---

## 1. Datagram framing

### 1.1 The 28-byte header (offsets 0x00-0x1B, never encrypted)

The server reads and writes only three fields. The other bytes are ignored on receive.

| Off | Size | C2S (client writes) | S2C (server writes) | Source |
|---|---|---|---|---|
| 0x00 | u16 | Client datagram sequence number ("SmallPD_Code") | Server datagram sequence number (`server_packet_id`) | `src/map/map_networking.cpp:416`, `:675` |
| 0x02 | u16 | Ack: header[0x00] of the last server datagram the client accepted | Ack: last client sequence the server accepted (`client_packet_id`) | `src/map/map_networking.cpp:488`, `:676` |
| 0x04 | 4 | ignored | **echo** of the client's bytes 0x04-0x07 (see note) | inferred from `:84`, `:675-679` |
| 0x08 | u32 | ignored by server | Unix timestamp, seconds (`earth_time::timestamp()`) | `src/map/map_networking.cpp:679`, `src/common/earth_time.h:321` |
| 0x0C | 16 | ignored | **echo** of the client's bytes 0x0C-0x1B | inferred from `:84`, `:675-679` |

Note on the echoed bytes: the incoming datagram is copied into `PBuff` (`src/map/map_networking.cpp:84`). The reply is then built in the same buffer, and `preparePacket` overwrites only offsets 0, 2 and 8 (`:675-679`). Bytes 4-7 and 12-27 of every S2C reply are therefore whatever the client put there. **INFERRED** recommendation: write zeros in those bytes, write a timestamp at 0x08 to look like retail, and ignore all three regions on receive.

### 1.2 Sub-packet header (inside the body, after decompression or decryption)

```
struct GP_CLI_HEADER / GP_SERV_HEADER {   // src/map/packets/c2s/base.h:29, s2c/base.h (same layout)
    uint16_t id   : 9;   // bits 0-8
    uint16_t size : 7;   // bits 9-15 = length in 4-byte words, header included
    uint16_t sync;       // sequence tag
};
```

- Type = `u16[0] & 0x1FF` (`src/map/packets/basic.h:96`; `src/map/map_networking.cpp:432`).
- Byte length = `4 * (u16[0] >> 9)`. The server computes it as `2 * (byte[1] & 0xFE)` (`src/map/packets/basic.h:99-101`; `src/map/map_networking.cpp:428-431`), which is the same value.
- Setting the size: `byte[1] = (byte[1] & 1) | (roundUp4(len) / 2)` (`src/map/packets/basic.h:118-121`).
- XiPackets says the retail client caps ids at 285 and sub-packet size at 260 bytes (https://github.com/atom0s/XiPackets/blob/main/world/Header.md). **INFERRED**: those are client-side limits, and our client should accept up to 508 bytes.
- Sub-packets are packed back-to-back with no extra padding. Each one is already a multiple of 4 bytes (`src/map/map_networking.cpp:575-577`).
- The parse loop stops at the first sub-packet whose size field is 0, or at the first one that would run past the end of the buffer (`src/map/map_networking.cpp:428`).

### 1.3 `sync` and sequencing rules

**Server-side acceptance of C2S sub-packets** (`src/map/map_networking.cpp:434-437`). A sub-packet is processed only if:

```
session.client_packet_id < sub.sync <= datagram.header[0x00]
```

Otherwise it is silently skipped. This deduplicates sub-packets that the client re-sends in a later datagram. **INFERRED**: retail keeps re-sending a sub-packet until it is acked.

After the loop the server sets `client_packet_id = header[0x00]` (`:482`). The value is not monotonic-checked, so a lower header[0] moves the window backwards.

**Server-side ack check** (`src/map/map_networking.cpp:488-512`):
- If the client's `header[0x02]` equals the server's current `server_packet_id`, the server increments `server_packet_id` (`:510`) and builds a new datagram from its queue (`send_parse`, `:101-104`).
- Otherwise the server treats the last reply as lost. It re-sends the cached previous datagram unchanged (same ciphertext; only header offsets 2 and 8 are refreshed), does not increment, and does not drain its queue (`:497-505`). Exception: if the last sub-packet in the datagram was 0x00A, it builds a fresh reply instead (`:492-495`).

**S2C side.** Every sub-packet in a server datagram gets `sync = server_packet_id`, which equals that datagram's header[0x00] (`src/map/map_networking.cpp:547`, `:675`).

**Client rules (INFERRED, derived from the server rules above):**
1. Keep `clientSeq` (u16). Increment it for every datagram sent and write it to header[0x00].
2. Each new sub-packet's `sync` = the `clientSeq` of the first datagram that carries it.
3. Write header[0x02] = header[0x00] of the most recent server datagram that passed MD5.
4. Drop S2C datagrams whose header[0x00] is <= the last one you processed. These are the server's re-sends.
5. Re-sending a sub-packet in a later datagram is safe because of the dedup window.

### 1.4 Reply-driven model (critical)

The server sends UDP **only in response to a received datagram**. The only send call site is `handle_incoming_packet` (`src/map/map_networking.cpp:149`; `MapSocket::send` at `src/map/map_socket.cpp:493` has no other caller). Each accepted C2S datagram produces exactly one S2C datagram: either a new one, the cached re-send, or an empty keepalive.

The S2C queue drains at no more than 32 sub-packets per reply (`:528`), and the server shrinks the batch by a third until the compressed size fits (`:582-593`). **The client must keep sending datagrams to receive game state.** **INFERRED**: retail sends one roughly every frame-tick, usually carrying 0x015. Plan on 0x015 every 200-400 ms plus an empty datagram whenever we need to pull more queued data.

#### C2S cadence (goal 43)

The game does not send per reply: it flushes its outgoing queue on its own timer (CODE 0x100fa3d0 runs timer 0,
0x100dbf10, against the interval at session +0x417c, then 0x100fa920 builds and sends one datagram; the only `sendto`,
WS2_32 ordinal 20, is at 0x100dc62b). Before each send in zone (state 3), if our sequence and the server's ack of us
differ by more than one the interval grows by 50 ms, capped at base + 3000 ms; otherwise it eases back toward the
base. S2C **0x005** `GP_SERV_COMMAND_PACKETCONTROL` (handler 0x100fabd0, registered at 0x100f9a74) sets the base from
its u32 @+4; LSB declares the packet but never sends it (`src/map/packets/s2c/0x005_packetcontrol.h`, "TODO:
Unimplemented"), so the client keeps the base it starts with: the `/pcnt` launch argument, 400 ms by default, with
the session's first interval 5 x that (0x100f9e29), stepping down by one base per send in zone (traced in
docs/lsb-packet-timing.md §3).

Our client (`src/map/map_client.cpp`) did the reverse until goal 43: one datagram in flight, the next as soon as the
reply arrived (`XI_NET_PACING=reply` still gives that). It now sends on the timer with the game's back-off and 0x005
handling; `XI_NET_INTERVAL=ms` overrides the base. `XI_NET_TRACE=1` logs datagrams and sub-packets per second and
each id's rate every 5 s. The cadence table, the stress comparison and the disconnect analysis are in docs/lsb-packet-timing.md §6.

**Player commands (#97).** The game never sends early: the only callers of the send 0x100fa920 are 0x100fa3d0 and
0x100fa0b0, both gated by timer 0 (0x100dbf10), nothing else re-arms timer 0, and the interval at +0x417c is only
written by the session init (0x100f9de0), the back-off in 0x100fa920 and the 0x005 handler. So in retail an action
(0x01A) waits up to one interval (400 ms) for the next timer send. Our client deliberately does better (not a traced
behaviour): any queued sub-packet goes out at once in a datagram without a position update (at most one per
`kQuickSendGap`, 100 ms), then for `kQuickWindow` (1.2 s) it polls every 100 ms once the previous datagram has been
answered, so the server's result comes back with the next reply after it is ready. A command does not wait for the
reply: LSB dispatches the sub-packets of a datagram before its ack check (`src/map/map_networking.cpp` `parse()`), so
even a datagram with a stale ack delivers the command. Positions stay on the timer. `XI_NET_URGENT=0` restores
timer-only sends. `XI_NET_TRACE=1` also logs how long each datagram took to be answered and, per command, "action
reached the network thread" (actions), "NNN queued -> sent", "first 028/029 after the NNN", and every datagram sent or
received in the 2 s after it (docs/lsb-packet-timing.md §5.1).

An "empty" reply has zero sub-packets. Its body is `[0x01]` plus u32 bitCount = 8 plus MD5, so the datagram is 28 + 1 + 4 + 16 = 49 bytes (`src/map/map_networking.cpp:538`, `:586`, `:698-700`; `src/common/zlib.cpp:261-262`).

### 1.5 MD5 position and coverage

- Algorithm: standard RFC 1321 MD5 (`src/common/md52.cpp:41-266`; `md5()` at `:260-266`).
- Position: the last 16 bytes of the datagram.
- Coverage: bytes `[28, len-16)`, i.e. the whole body excluding the 28-byte header and the hash itself.
  - Receive check: `src/map/map_networking.cpp:177`, `:204`; compare function at `src/common/utils.cpp:41-52`.
  - S2C build: MD5 over `compressed bytes + u32 bitCount` (`src/map/map_networking.cpp:711-713`).
- Order: MD5 is computed on **plaintext** and then encrypted along with the body. The receiver decrypts first, then verifies (`:175-177`).
- Minimum datagram the server will consider: more than 28 + 16 = 44 bytes (`:200`).

### 1.6 Alignment and padding

- The datagram has no padding. The cipher covers `floor((len-28)/8)` whole 8-byte blocks. **Any 1-7 trailing bytes stay plaintext** (`src/map/map_networking.cpp:169-170`, `:723`).
- Sub-packets are 4-byte multiples. A C2S body made only of sub-packets plus a 16-byte MD5 is therefore always a multiple of 4, but not necessarily of 8.

---

## 2. Encryption (FFXI-modified Blowfish, ECB)

### 2.1 Key material and derivation

Session key = 20 bytes, held as `uint32 key[5]` (`src/common/blowfish.h:34-41`).

**Where it comes from:**
- It is the 20 bytes the client itself sends in the lobby 0xA2 packet at data offset 1 (`src/login/data_session.cpp:312-313`).
- The lobby then adjusts byte 16 with **byte-wise uint8 addition (no carry)** before storing it:
  - `key3[16] += 6` if a character was created in this lobby session (`src/login/data_session.cpp:356-359`);
  - `key3[16] += incrementKeyValue` (`:362`). That counter is +4 after a char delete (`src/login/view_session.cpp:159`), +4 on 0x21 name registration (`:307`), and +1 on "character already logged in" (`src/login/data_session.cpp:475`).
- The result is stored as `accounts_sessions.session_key` (`src/login/data_session.cpp:483-491`).
- The map server loads it when the client's 0x00A arrives (`src/map/map_networking.cpp:293-310`). **The initial map key is used as-is: there is no +2 at first login.**
- **Our client must apply the same byte-16 adjustments to its local copy** so both sides agree. The lobby-side document owns the exact conditions.

**Building the cipher state from the key** (`MapSession::initBlowfish`, `src/map/map_session.cpp:40-53`):
1. `hash = MD5(key[0..20])`, 16 bytes (`:42`).
2. Find the first `hash[i] == 0` and zero every byte from `i` to 15 (`:44-51`). The key length passed on is **still 16** (`:52`).
3. `blowfish_init(hash, 16)` (`:52`).

### 2.2 `blowfish_init`: key schedule (`src/common/blowfish.cpp:452-505`)

- Initial state: `P[18]` then `S[4][256]`, copied from the 4168-byte `subkey` table (`:25`, `:462-463`).
  - I verified that this table is exactly the standard Blowfish pi-digit init constants, stored as LE uint32 (P[0] = 0x243F6A88 ... S[3][255] = 0x3AC372E6). Any standard Blowfish P/S init table works.
- **Non-standard key mixing.** The key bytes are read as **signed `int8`** (`blowfish.h:49`; `:472`):
  ```
  j = 0
  for i in 0..17:
      data = 0
      for k in 0..3:
          data = (data << 8) | (uint32)(int32)(int8)key[j]   // sign-extends! byte >= 0x80 sets bits 8..31
          j = (j + 1) % 16
      P[i] ^= data
  ```
  For example, bytes `01 80 02 03` give `0xFF800203`, not `0x01800203`.
- Then, as standard Blowfish: `L = R = 0`. For i in 0, 2, ..., 16: `encipher(L, R)`, `P[i] = L`, `P[i+1] = R`. Continue the same chain over S[0][0..255], S[1], S[2], S[3] in pairs (`:482-502`). This uses the modified F function below.

### 2.3 F function: **non-standard** (`src/common/blowfish.cpp:290-293`)

```
F(x) = ((S[1][(x >> 8) & 0xFF] & 1) ^ 32)
     + ((S[3][x >> 24]        & 1) ^ 32)
     +   S[2][(x >> 16) & 0xFF]
     +   S[0][x & 0xFF]                       (all uint32 wrapping adds)
```

The flat-array indices are S0 = `S[0..255]`, S1 = `S[256..]`, S2 = `S[512..]`, S3 = `S[768..]`.

This is **not** standard Blowfish's `((S0[a] + S1[b]) ^ S2[c]) + S3[d]`:
- the byte-to-box mapping is reversed, and
- S1 and S3 contribute only `32` or `33`.

This form has been unchanged since the original DSP import (checked with `git log -p -- src/common/blowfish.cpp`). Copy it exactly.

### 2.4 Encipher and decipher of one 64-bit block

The block is two **little-endian** uint32 words: `L = word[0]`, `R = word[1]`.

Encipher (`src/common/blowfish.cpp:295-325`):
```
for i in 0..15:  L ^= P[i];  R ^= F(L);  swap(L, R)
swap(L, R);  R ^= P[16];  L ^= P[17]
```

Decipher (`:327-358`):
```
for i in 17 down to 2:  L ^= P[i];  R ^= F(L);  swap(L, R)
swap(L, R);  R ^= P[1];  L ^= P[0]
```

Mode is ECB: each 8-byte block is independent, with no IV or chaining. The 4-way interleaved versions at `:364-450` give bit-identical output.

### 2.5 Which bytes are encrypted

- Start at byte offset 28 (`(uint32*)buff + 7`).
- Encrypt `floor((len-28)/8)` consecutive 8-byte blocks (`src/map/map_networking.cpp:169-175`, `:723-737`).
- The 28-byte header is never encrypted. The final `(len-28) % 8` bytes are never encrypted.
- This applies to both directions, with the same key for S2C and C2S.

### 2.6 What is plaintext and what is encrypted

| Datagram | Encryption |
|---|---|
| C2S containing **0x00A** (zone login) | **Plaintext**, with MD5 still required. The server tries an MD5 check on the raw bytes first and accepts only 0x00A that way (`src/map/map_networking.cpp:204-219`). |
| All other C2S | Encrypted with the current key. If MD5 fails after decryption the datagram is dropped (`:354-369`). |
| All S2C, including the reply to 0x00A | Encrypted with the current key (`:715-737`). |

### 2.7 Key increment on zone change or logout

- The server sends sub-packet 0x00B (`GP_SERV_COMMAND_LOGOUT`). That datagram is encrypted with the **old** key (`src/map/map_networking.cpp:551-558`, `:615`). Immediately after building it, the server:
  - copies the current state into `prev_blowfish` (`src/map/map_session.cpp:33`);
  - runs `key[4] += 2`, a **uint32 add over key bytes 16-19, LE, with carry** (`:35`). Contrast the lobby's uint8 add in 2.1;
  - re-runs `initBlowfish` (`:37`);
  - persists the new key to the DB (`src/map/map_networking.cpp:642-646`);
  - sets status `BLOWFISH_PENDING_ZONE` and destroys the char object (`:658-659`).
- **Client rule:** after you successfully decrypt and verify a datagram containing 0x00B, set `key[4] += 2` (as uint32) and re-derive (MD5, zero-truncate, init). Every later encrypted C2S, and every S2C from the next zone, uses the new key.
- Old-key fallback: while the server is PENDING_ZONE it tries the new key first, then `prev_blowfish` (`src/map/map_networking.cpp:354-364`).
  - If a C2S datagram decrypts only with the old key, the server assumes you missed 0x00B. It rebuilds 0x00B and re-sends it encrypted with the old key (`:106-147`).
  - If a C2S datagram decrypts with the new key while the char object is gone, nothing is parsed, but `handle_incoming_packet` still sends `PBuff` back (`:94-153`). That reply contains our own decrypted, unencrypted body. **Client must drop any datagram that fails decrypt+MD5** (that path produces exactly such garbage).

Test vectors, produced by compiling `src/common/blowfish.cpp` + `md52.cpp` unmodified. Plaintext is 16 bytes `00 01 .. 0f`, enciphered as 2 blocks:

```
A: key = 20 x 00
   md5/trunc = 44 10 18 52 52 08 45 77 05 bf 09 a8 ee 3c 10 93
   P[0]=4c5590be P[17]=00eb7941 S[0][0]=d973900f S[3][255]=80b5f7bb
   enc = f4 0a 34 af 3e 4a bf 20 fd da 34 b5 bf 63 ed f3
B: key = 01 02 .. 14
   md5/trunc = 4d 55 55 e0 67 dd 97 d0 8f ef 90 95 9b 15 10 cb
   P[0]=6ad0d8fa P[17]=08ff8516 S[0][0]=c7ab0527 S[3][255]=7ecde307
   enc = 82 cb a3 2c 5d e5 d9 ac a3 07 86 f9 5d 9d 82 dd
B after one zone (key[4]+=2 -> byte16 = 0x13):
   md5/trunc = 90 f6 d0 40 b2 6c 5b 5f ab c1 de b4 2a e8 ed a6
   P[0]=8d8e37cc P[17]=b9f26b38 S[0][0]=bf67c742 S[3][255]=427ec178
   enc = 86 24 28 48 94 45 60 1c a6 8e 1a dd 88 38 13 f8
```

(None of these MD5s contain a 0x00 byte, so the truncation rule is not exercised. Add a unit test for it separately.)

---

## 3. Compression (named "zlib" in LSB, but it is NOT zlib)

It is a **static Huffman (prefix) code with a fixed 256-symbol table**, packed LSB-first.

### 3.1 Table data

- Files: `res/compress.dat` (2048 bytes = 512 LE uint32) and `res/decompress.dat` (10224 bytes; a pointer tree that only makes sense with its own base address). Both ship **in the server repo**. They are loaded at startup from the working directory (`src/common/zlib.cpp:183-203`). They are not read from the FFXI install.
  - **INFERRED**: the retail client embeds equivalent tables in FFXiMain.dll.
- Layout of `compress.dat`, indexed by the **signed** value `s = (int8)byte`:
  - `enc[s + 0x80]` = code bits (`src/common/zlib.cpp:239`);
  - `enc[s + 0x180]` = code length in bits (`:224`).

  So for byte value b (0..255), with idx = (b < 128 ? b : b - 256): code at `enc[idx+128]`, length at `enc[idx+384]`. The code uses only its low `len` bits (`:241`).
- Facts I verified with a script over the shipped tables:
  - lengths range from 1 to 12 bits;
  - Kraft sum = 1.0, so the code is complete;
  - it is prefix-free;
  - no code word has stray high bits;
  - walking `decompress.dat` with every `compress.dat` code returns the right symbol in all 256 cases.

  **So a client can build its decoder from `compress.dat` alone** (a 4096-entry LUT on 12 bits, or a trie), and does not need `decompress.dat`.
- Sample codes, written as a bit string where position i = bit i in emission order: `0x00` = `1` (1 bit), `0x01` = `01010`, `0xFF` = `010001011000`, `0x20` = `01111101111`.
- `decompress.dat` format, for reference only (`src/common/zlib.cpp:114-141`, `:268-322`):
  - entries > 0xFF are absolute pointers; `(ptr - (dec[0]-4)) / 4` is the entry index;
  - the root node is at index `(dec[0]-base)/4` = 1;
  - a node is `[child0, child1, ?, symbol]`;
  - a node is a leaf when child0 and child1 are both 0, and its symbol is at `node[3]`.

### 3.2 Compressed block format

```
byte 0        : 0x01 (flag; decompress rejects anything else)             zlib.cpp:261, :278-282
bytes 1..     : bitstream; symbol codes appended LSB-first                 zlib.cpp:243-252
                (bit k of the stream = byte (k>>3), bit (k&7));
                unused high bits of the final byte are 0                   zlib.cpp:255-259
u32 (LE)      : bitCount = (number of code bits) + 8                       zlib.cpp:262; map_networking.cpp:698
```

- Compressed byte length = `ceil(bitCount/8)` (`src/common/zlib.h:26-29`). That length already counts the 0x01 flag byte, because of the +8.
- The u32 sits immediately after those bytes (`src/map/map_networking.cpp:698-700`).

Test vector: compressing `00 01 ff 20` gives `01 95 68 f8 1e`, bitCount = 37 (`25 00 00 00`).

### 3.3 S2C decode steps (exact)

1. Require `len > 44`. Decrypt `floor((len-28)/8)` blocks starting at offset 28.
2. Verify `MD5(buf[28 .. len-16)) == buf[len-16 .. len)`. On failure, drop the datagram.
3. `bitCount = u32 at buf[len-20]` (`src/map/map_networking.cpp:380` reads it the same way on C2S).
4. Require `buf[28] == 0x01`.
5. Decode Huffman symbols starting at `buf[29]`, bit 0. **Stop after consuming `bitCount - 8` bits.**
   - The server's own decoder loops while `bitpos < bitCount` with `bitpos` counted from byte 1 (`src/common/zlib.cpp:284-290`). That overruns by 8 bits and appends trailing 0x00-ish junk. A strict decoder avoids this.
6. The output is the concatenated sub-packets. Iterate them per 1.2, stopping at size 0 or at the end of the buffer.

### 3.4 Client to server: compression is optional

- `recv_parse` always reads `u32 at len-20` and calls the decompressor (`src/map/map_networking.cpp:379-383`).
- If `body[0] != 0x01`, the decompressor returns -1 and the server **parses the decrypted body as raw sub-packets** (`:386-396`; `src/common/zlib.cpp:278-282`).
- The plaintext 0x00A path never decompresses (`:212-342`).
- So C2S may be sent **uncompressed**: `header | sub-packets | MD5`, with no u32 bitCount. The stale reference client in the repo does this too (`tools/headlessxi/packets.py:17-19`, `:76-88`; `tools/headlessxi/util.py:157-161`). **INFERRED**: retail sends uncompressed C2S.
  - Constraint: the first body byte must not be 0x01. The first sub-packet's id must not be 0x001 or 0x101.
- **Hazard:** with uncompressed C2S, `buffsize` still includes the 16 MD5 bytes. The parse loop (`src/map/map_networking.cpp:428-437`) can read the MD5 as one more "sub-packet" if its fake size fits and its fake sync passes the window check.
  - **INFERRED mitigation:** append a 4-byte all-zero terminator sub-packet after the last real one, before the MD5. The loop stops at the zero size field, and the terminator is included in the MD5 coverage.

---

## 4. Zone-in handshake

### 4.1 Preconditions (server state set up by the lobby)

- After 0xA2, the lobby inserts `accounts_sessions(accid, charid, session_key, server_addr, server_port, client_addr, ...)` (`src/login/data_session.cpp:483-491`). It then sends IPC `CharZone`, and the map process creates a **pending session** keyed by char id (`src/login/data_session.cpp:566-571` → `src/map/ipc_client.cpp:209-224` → `src/map/map_session_container.cpp:69-91`).
- A pending session expires after `MAX_TIME_LASTUPDATE` (60 s) (`src/map/map_session_container.cpp:307-325`). **Send 0x00A within 60 s of the lobby handoff.**
- On the first 0x00A, the pending session becomes a real session keyed by **UDP source IP+port** (`src/map/map_networking.cpp:250-267`). This requires `accounts_sessions.client_addr` to equal our UDP source IP, i.e. the IP we used for the lobby TCP connection (`src/map/map_session_container.cpp:44-56`).
- **Use one UDP socket (fixed source port) for the whole game session, across zones.** All later lookup is `getSessionByIPP` (`src/map/map_networking.cpp:81`). On a same-process zone change no new pending session is created when one already exists for the char (`src/map/ipc_client.cpp:213-218`), so a new source port would be rejected.

### 4.2 C2S 0x00A `GP_CLI_COMMAND_LOGIN`: plaintext, 92 bytes (size byte = 0x2E)

Defined at `src/map/packets/c2s/0x00a_login.h:29-43`. Offsets were verified with a compiled replica.

| Off | Type | Field | Server use |
|---|---|---|---|
| 0x00 | u16 | id=0x00A, size=23 words | must be >= 92 bytes total (`map_networking.cpp:222`); exact size 92 enforced by dispatcher (`packet_system.cpp:170-204`) |
| 0x02 | u16 | sync | must satisfy the window in 1.3 (session `client_packet_id` was reset to 0 → use sync >= 1 and <= header[0]). If `sync > 1` the server sets `server_packet_id = sync` and clears its queue (`0x00a_login.cpp:55-63`) |
| 0x04 | u8 | LoginPacketCheck | `= (uint8) sum of bytes 0x08..0x5B` (84 bytes), checked before anything else (`map_networking.cpp:231-246`) |
| 0x05 | u8 | padding | |
| 0x06 | u16 | unknown (PS2 MyPort) | ignored |
| 0x08 | u32 | unknown (PS2 MyIP) | ignored (but summed) |
| 0x0C | u32 | UniqueNo = char id | must match a pending session or existing session's char (`map_networking.cpp:248-272`); `validate` requires `== PChar->id` (`0x00a_login.cpp:37-42`) |
| 0x10 | u16[9] | GrapIDTbl | ignored |
| 0x22 | char[15] | sName | ignored |
| 0x31 | char[15] | sAccunt | ignored |
| 0x40 | u8[16] | Ticket | ignored by LSB (XiPackets: MD5 of account name + blowfish key) |
| 0x50 | u32 | Ver | ignored |
| 0x54 | u8[4] | sPlatform ("WIN") | ignored |
| 0x58 | u16 | uCliLang (0 JP, 1 EN, 2 FR, 3 DE) | read then ignored (`map_networking.cpp:288-291`) |
| 0x5A | u16 | dammyArea | |

Datagram: header (header[0] = clientSeq, header[2] = 0) + this 92-byte sub-packet + (optional 4-byte zero terminator) + MD5. **Not encrypted.**

Server-side rejections, all silent:
- MD5 fails;
- first sub-packet isn't 0x00A;
- datagram too short;
- byte-sum check fails;
- no session or pending session for the char id;
- the session's char id differs;
- a 0x00A arrives **after** any encrypted packet has already decrypted with the current key (`hasDecryptedPacket`, `src/map/map_networking.cpp:274-277`, `:374-377`).

Before that first encrypted packet, **0x00A may be re-sent** (for example, if its reply was lost). Each accepted 0x00A resets the server's `client_packet_id` and `server_packet_id` to 0 (`:337-340`), so re-send it with header[2] = 0.

### 4.3 What the server does with 0x00A

`src/map/packets/c2s/0x00a_login.cpp:44-154`:
- loads the character and session key (`map_networking.cpp:286-335`);
- on first acceptance, places the char in the destination zone and calls `OnZoneIn` / `OnGameIn` (`0x00a_login.cpp:68-132`);
- sets status `BLOWFISH_ACCEPTED` (`:70`) and char status Normal (`:151`).

It queues, in this order (`:139-150`):

| S2C id | Name | Notes |
|---|---|---|
| 0x04F | EQUIP_CLEAR | |
| 0x051 | GRAP_LIST | |
| 0x01C | ITEM_MAX | |
| **0x00A** | **LOGIN** | zone init: `PosHead` (UniqueNo u32 @+4, ActIndex u16 @+8, dir @+0xB, x/z/y floats @+0xC/+0x10/+0x14, ...), `ZoneNo` u32, times, music, weather, name[16], `Dancer` (jobs/levels), etc. Full struct at `src/map/packets/s2c/0x00a_login.h:30-132` |
| 0x050 | EQUIP_LIST | one per equipped slot |

These arrive in the **encrypted** reply to the plaintext 0x00A datagram, using the session key from 2.1. The reply's header[0] is 1 when header[2] was 0 (`map_networking.cpp:488-510`).

### 4.4 Client's next packets

Everything from here on is encrypted.

| C2S | Size | Required content | Server response | Source |
|---|---|---|---|---|
| **0x00C** GAMEOK | 12 B (byte1 = 0x06) | `ClientState` u32 @+4 **must be 0**; `DebugClientFlg` bit0 of u32 @+8 **must be 0** | Queues the bulk init: 0x008 ENTERZONE, 0x01C, 0x0B4 CONFIG, 0x051, 0x01B JOB_INFO, AE points, **0x037** char status, MISCDATA 0x063s, extended job, unity, key items, quest/mission logs, RoE, 0x0AA magic, mount, dungeon, 0x0AC command data, **0x067** char sync, inspect msg, 0x08C merits, inventory (0x01D/0x01E/0x01F/0x020), blacklist, treasure pool, transport, pet | `src/map/packets/c2s/0x00c_gameok.h:28-32`, `0x00c_gameok.cpp:48-108` |
| **0x011** ZONE_TRANSITION | 8 B (byte1 = 0x04) | byte @+4 **must be 2**, byte @+5 **must be 0** | none (Kupowers TODO) | `0x011_zone_transition.h:28-32`, `.cpp:24-36` |
| **0x061** CLISTATUS | 8 B | byte @+4 in [0,1] | 0x0DF GROUP_ATTR, 0x061, 0x062, 0x119 recasts, 0x063 merits/monstrosity/JP | `0x061_clistatus.h:28-32`, `.cpp:26-35`; `src/map/utils/charutils.cpp:1577-1586` |
| **0x015** POS | 32 B | see §5.1 | triggers NPC/mob/pet/trust spawn checks and PC info sync, which produce **0x00E** (entity) / **0x00D** (PCs) | `0x015_pos.cpp:99-104` |
| 0x00F CLSTAT | 36 B | anything | none | `0x00f_clstat.cpp:24-36` |
| 0x016 CHARREQ | 8 B | ActIndex u16 @+4 | entity data for that index | `0x016_charreq.h:28-31` |

- Every handler requires the **exact** size `roundUp4(sizeof struct)` (or a min/max range for VLA packets). Wrong-size sub-packets are dropped with a warning (`src/map/packet_system.cpp:170-204`).
- Any non-0x00A sub-packet that arrives before the char has a zone is ignored (`src/map/map_networking.cpp:449-456`).

**INFERRED minimal sequence for a "fully in zone" client:**
1. Plaintext 0x00A.
2. Receive the encrypted reply containing 0x00A.
3. Send 0x00C (+0x011, +0x061) in one datagram.
4. Keep sending datagrams (0x015 at least) to drain the init burst.

No server state machine waits for 0x00C or 0x011. The char is "in zone" as soon as 0x00A is processed (`0x00a_login.cpp:99-151`). But nothing beyond the 0x00A burst is sent until 0x00C, and entities arrive only via 0x015-triggered spawn checks and the zone tick.

Entity packet ids, for the receive side:
- 0x00D PC update (`src/map/packets/char_update.cpp:230`);
- 0x00E NPC/mob update, 0x58 bytes, UniqueNo @+4, ActIndex @+8 (`src/map/packets/entity_update.cpp:262-286`);
- 0x037 own char status (`src/map/packets/char_status.cpp:207`);
- 0x067 char/entity sync (`src/map/packets/char_sync.cpp:30`, `entity_set_name.cpp:34`);
- 0x068 pet sync (`pet_sync.cpp:36`).

### 4.5 Keepalive and timeouts

- `last_update` is refreshed on every datagram that decrypts with the current key and reaches `parse()`, while the session is not WAITING or PENDING_ZONE (`src/map/map_networking.cpp:421-426`; `src/map/map_session.cpp:60-66`).
- Any packet counts, not only 0x015.
- Timeline, checked by a sweep every 5 s:
  - **> 5 s** silent: char flagged link-dead, other players see it (`src/map/map_session_container.cpp:210-224`);
  - **> 60 s** silent: session destroyed, char saved and removed (`:226-289`).
  - Resuming traffic before 60 s clears link-dead (`:291-303`).
- **INFERRED target:** send at least one datagram per ~1 s even when idle. Moving, or pulling queued data, needs a faster rate (§1.4).

---

## 5. Position and chat

### 5.1 C2S 0x015 `GP_CLI_COMMAND_POS`: 32 bytes (byte1 = 0x10)

Defined at `src/map/packets/c2s/0x015_pos.h:28-42`. Offsets verified.

| Off | Type | Field | Server use (`0x015_pos.cpp`) |
|---|---|---|---|
| 0x04 | f32 | x | `loc.p.x` (`:48`) |
| 0x08 | f32 | "z" (atom0s naming) | stored as LSB `loc.p.y` (`:49`, "Not a typo") |
| 0x0C | f32 | "y" | stored as LSB `loc.p.z` (`:50`) |
| 0x10 | u16 | MovTime | ignored |
| 0x12 | u16 | MoveFlame | `loc.p.moving` (`:72`) |
| 0x14 | i8 | dir | `loc.p.rotation` (u8 0-255 heading) (`:52`, `:73`) |
| 0x15 | u8 bits | b0 TargetMode, b1 RunMode, b2 GroundMode | ignored |
| 0x16 | u16 | facetarget | `m_TargID` (target index) (`:51`, `:75`) |
| 0x18 | u32 | TimeNow | ignored |
| 0x1C | u32 | padding | |

- Wire float order is therefore `(x, LSB-y, LSB-z)`. **INFERRED**: LSB `y` is the vertical axis, so the wire order is (X, height, Z). This matches the S2C `PosHead` order x/z/y at +0x0C/+0x10/+0x14 (`src/map/packets/s2c/0x00a_login.h:36-38`).
- Rejected (silently) when char status is Disappear or Shutdown (`:29-34`), for example right after a zone request. Non-finite coordinates are ignored (`:43-46`). Ignored while a server-forced position update is pending (`:38-41`).

### 5.2 C2S 0x0B5 `GP_CLI_COMMAND_CHAT_STD`: variable size

Defined at `src/map/packets/c2s/0x0b5_chat_std.h:43-47`.

| Off | Type | Field |
|---|---|---|
| 0x04 | u8 | Kind: 0x00 Say, 0x01 Shout, 0x04 Party, 0x05 LS1, 0x08 Emote, 0x18 LS-PvP, 0x1A Yell, 0x1B LS2, 0x21 Unity, 0x22 AssistJ, 0x23 AssistE (enum `:26-39`, validated `oneOf` `0x0b5_chat_std.cpp:104-108`) |
| 0x05 | u8 | unused |
| 0x06 | u8[<=128] | message bytes (Shift-JIS / FFXI encoding, auto-translate tokens) |

- Sub-packet size = `roundUp4(6 + len [+1 NUL])`. The allowed range is 6..136 bytes (`src/map/packet_system.cpp:170-181`), so in practice 8..136 in 4-byte steps.
- The server reads `min(size*4 - 6, 128)` bytes, cut at the first NUL (`0x0b5_chat_std.cpp:115-116`).
- **INFERRED**: always NUL-terminate and zero-pad.
- A leading `!` runs a GM/server command (`:121-131`).
- Say and Emote are broadcast to in-range PCs **excluding the sender** (`src/map/zone_entities.cpp:1552-1561`). The client should echo its own Say locally. **INFERRED** (it matches retail behaviour).

### 5.3 S2C 0x017 `GP_SERV_COMMAND_CHAT_STD`

Defined at `src/map/packets/s2c/0x017_chat_std.h:46-53`, built in `.cpp:28-85`.

| Off | Type | Field |
|---|---|---|
| 0x04 | u8 | Kind (`CHAT_MESSAGE_TYPE`: 0 Say, 1 Shout, 3 Tell, 4 Party, 5 LS, 6/7/29 System, 8 Emote, 13-16 "NS" variants, 26 Yell, 27 LS2, 33 Unity, 34/35 Assist; `src/map/enums/chat_message_type.h:25-61`) |
| 0x05 | u8 | Attr (0x01 = GM sender, level >= 3) |
| 0x06 | u16 | Data: zone id of sender (assist: mastery low byte, mentor high byte) |
| 0x08 | char[15] | sName (not guaranteed NUL-terminated if 15 chars) |
| 0x17 | u8[<=150] | Mes |

- Size = `roundUp4(0x17 + msgLen)` (`.cpp:33-34`).
- The message length is **not** transmitted separately. Read `size - 0x17` bytes and stop at the first NUL. The buffer is zero-filled (`src/map/packets/s2c/base.h:53`), but there is no NUL when `0x17 + msgLen` is already a multiple of 4.

### 5.4 S2C 0x05B `GP_SERV_COMMAND_WPOS`: server-set position

Defined at `src/map/packets/s2c/0x05b_wpos.h` (0x065 `WPOS2` has the same body).

| Off | Type | Field |
|---|---|---|
| 0x04 | f32[3] | x, y (height), z |
| 0x10 | u32 | UniqueNo |
| 0x14 | u16 | ActIndex |
| 0x16 | u8 | Mode (`POSMODE`): 0 NORMAL, 1 EVENT, 2 CLEAR, 3 POP, 5 RESET, 6 MATERIALIZE, 8 LOCK, 9 UNLOCK, 0x0A ROTATE |
| 0x17 | u8 | dir |

- Modes 0, 1, 3, 5 and 6 move the entity, ours or another's.
- Modes 3 and 6 also store the mode in entity +0x144 (CODE 0x1009efd4). On the entity's next update (0x1009095b, once its
  model is loaded), 3 plays the model's `pop0` and 6 plays file 0 `pop1` (a fade-in with the refraction pass), then +0x144
  is cleared. xi-client: `WorldSnapshot::posEffects`, played in `ActionEffects::update` (2026-10-19).
- LSB sends modes 3 and 6 only from Lua `positionSpecial(pos, mode)`. No script calls it, so test it with
  `!exec player:positionSpecial(player:getPos(), 6)` (gmlevel 4).

---

## 6. Zoning out and in

### 6.1 Trigger

- Client walks into a zone line and sends **C2S 0x05E `GP_CLI_COMMAND_MAPRECT`**, 24 bytes (`src/map/packets/c2s/0x05e_maprect.h:54-62`):

  | Off | Type | Field |
  |---|---|---|
  | +4 | u32 | RectID (fourcc zone-line tag) |
  | +8 | f32 | x |
  | +0xC | f32 | y |
  | +0x10 | f32 | z |
  | +0x14 | u16 | ActIndex |
  | +0x16 | u8 | MyRoomExitBit |
  | +0x17 | u8 | MyRoomExitMode |

- The server validates (`0x05e_maprect.cpp:63-382`):
  - not in an event;
  - enum ranges;
  - the zone line exists;
  - the char's last server-known position is within 40 yalms of the line;
  - the destination is open and not at its cap.

  On failure it calls `denyZone`. On success it sets status Disappear and `requestedZoneChange`.
- Other triggers (warps, GM `!zone`, logout) go through `charutils::SendToZone` / `SendDisconnect` (`src/map/utils/charutils.cpp:6680-6767`).

### 6.2 S2C 0x00B `GP_SERV_COMMAND_LOGOUT`: 28 bytes

Defined at `src/map/packets/s2c/0x00b_logout.h:28-76`, built in `.cpp:24-32`. It is pushed by `SendToZone` (`charutils.cpp:6736`) or `SendDisconnect` (`:6766`).

| Off | Type | Field |
|---|---|---|
| 0x04 | u8 | LogoutState: 1 LOGOUT, 2 ZONECHANGE, 3 MYROOM, ... (`0x00b_logout.h:28-41`) |
| 0x08 | u32 | new map IP, **network byte order** (bytes read a.b.c.d in memory order) (`src/common/ipp.h:87-91`, `src/common/ipp.cpp:35-40`) |
| 0x0C | u32 | new map UDP port, host order, in the low 16 bits |
| 0x10 | u8[8] | padding |
| 0x18 | u32 | cliErrCode (0 = NOERR) |

### 6.3 Client procedure (server-required parts cited, the rest INFERRED)

1. Receive the datagram containing 0x00B. It was encrypted with the **old** key (§2.7).
2. Increment the local key: `key[4] += 2` (u32), then re-derive (`src/map/map_session.cpp:30-38`).
3. (Retail) send C2S **0x00D NETEND** (8 B: State u16 @+4, pad) (`0x00d_netend.h:28-31`). The server ignores it (`0x00d_netend.cpp:24-33`; comment at `map_networking.cpp:108`). Optional.
4. Reset `clientSeq` / ack state for the new connection.
   - **INFERRED**: retail keeps counting, and that is why 0x00A's `sync > 1` re-bases the server counter (`0x00a_login.cpp:55-63`).
   - Both work. Simplest: start a fresh `clientSeq` at 1 and send 0x00A with sync = 1, header[2] = 0.
5. Send a **plaintext 0x00A** (§4.2) to the IP:port from 0x00B, **from the same UDP socket**.
   - The server (same or other map process) loads the incremented key from `accounts_sessions.session_key`, which was persisted at `map_networking.cpp:644-646`.
   - Its reply is encrypted with the new key.
   - Same-process case: the existing session is found by IPP, PENDING_ZONE drops the old char object, and the char reloads from the DB (`map_networking.cpp:281-284`, `:286-335`).
   - Other-process case: the `CharZone` IPC creates a pending session there (`map_networking.cpp:650-656` → `ipc_client.cpp:222`).
6. Continue as in §4.4: 0x00C / 0x011 / 0x061 / 0x015.
7. If, while in PENDING_ZONE, you keep sending old-key datagrams to the old server, it keeps re-sending 0x00B (`map_networking.cpp:106-147`). That is the recovery path for a lost 0x00B.

- **Logout:** C2S 0x0E7 REQLOGOUT (8 B: Mode u16 @+4, Kind u16 @+6) (`0x0e7_reqlogout.h:42-45`) eventually yields 0x00B with LogoutState = 1, IP = 0. The key is still incremented (`map_networking.cpp:639-660`). The session ends when `shuttingDown` is set (`:156-159`) or on timeout.
- **The zone change fade (CODE, issue #52):** the client fades the picture, not the server. The zone sequence task
  (0x1008ab10, run each frame from 0x100daf17) has two state bytes:
  - fade out (0x10482f1e = 1, set at 0x100daeed when the session sees the zone-out flags from 0x00B): the music and
    sound volume go to 0 over 30 ticks (0x10035c70 / 0x10035cc0 with 0, 30), scheduler `fdo0` of file 30904 (ROM/62/110)
    plays (op 0x0F: to R G B A = 00 00 00 80, black, over 30 ticks = 0.5 s) for 60 ticks, then stops; the black stays;
  - fade in (0x104807f3 = 1, set at 0x10093d32 right after C2S 0x00C goes out, i.e. once the new zone is loaded):
    the volumes back to 1.0 over 30 ticks and `fdi0` (op 0x0F: to 80 80 80 00, clear, over 30 ticks).
  The screen colour is drawn after the scene and before the interface (0x1000a820), so the windows stay up.
  xi-client (zone_frame.cpp): black over the scene under the windows, linear 0 -> 1 over 0.5 s from the moment the
  zone is left (0x00B) or a new zone arrives, the old zone kept on screen (with movement and the camera held) until it
  is black, the new zone built, and a 0.5 s fade in from the third frame it is drawn; the first zone-in after login
  starts black and fades in; never more than 20 s black (a failed or slow zone-in fades in anyway). The music fades with
  it (SE fade not done). `XI_ZONE_FADE=0` turns it off, `XI_FADE_TRACE=1` logs the level per frame.
- **Lobby re-entry:** a session left in "zoning out" with `client_port = 0` for more than 2 minutes is purged at the next lobby 0xA2 (`src/login/data_session.cpp:392-402`).

---

### 6.1 Mog House (CODE + DATA, live 2026-10-18; RE notes moghouse_re.md)

- **Entering:** the residential entrance is an ordinary Rid zone line (`zmr0` in 230, `zmr2` 231, `zmr4` 232); the
  client sends 0x05E with it like any line. LSB answers 0x00B ZONECHANGE, then 0x00A with **LoginState (+0x80) 1**
  (5 also means a Mog House), **MyroomMapNumber (+0xAA)** = the interior model, MyroomSubMapNumber (+0xA8; floor =
  (byte >> 1) & 3), **MyRoomExitBit (+0xAE)**, ZoneNo / MapNumber still the city, position (0, 0, 0) dir 192.
- **Zone model:** outside, MapNumber (+0x42) + 0x1BC when >= 0x100; in a Mog House, MyroomMapNumber. The file is
  model + 100 below 600, else model + 83191 (CODE 0x100fac20, 0x10178ca0; DATA: San d'Oria 0x121 -> 389 `m_sa`,
  Western Adoulin 256 -> 700 -> 83891 `_256`). Interiors are normal zone files (Mzb, Mmb, collision, `weat`).
- **Moogle:** LSB spawns no city NPCs inside; it sends the Moogle (model 82 at (0, 0, 1.5)) only after C2S 0x01A
  ActionID 0x14 ("resources ready", sent once per zone, CODE 0x10088200). Talking to it gets S2C 0x02E (no body),
  which opens "menu myroom" (also the main menu's Map slot inside).
- **Job change:** C2S 0x100 {+4 main job, +5 support job; 0 = unchanged} (CODE 0x100f0e60). Jobs and levels come
  from S2C 0x01B (unlocked mask +0x0C, bit 0 = support jobs; levels by job id at +0x48). Live: BotB BLM -> WAR -> BLM.
- **Exit:** the interior's door group `_720` (a client-side "Door: Back to Town" in the game) leads to C2S 0x05E with
  RectID **`zmrq`**, MyRoomExitBit at +0x16 and MyRoomExitMode at +0x17: mode 0 = back where you came in, 1..n the
  city's areas (table 0x1037ec30: exit bit 1 = 230, 231, 232), 125 / 126 = 2F / 1F, 127 Mog Garden. xi-client asks
  when you step up to the door box. Live: mode 0 put BotB at (159.5, -2, 160.3) in 230.

- **Furniture** (CODE + DATA, live 2026-10-18; RE notes furniture_re.md): installed furnishings (item type 10 / 12,
  exdata byte 1 bit 0x40, floor = byte 1 & 3) in the Mog Safe (containers 1 / 9) are drawn as static models: file =
  item + 32927 (< 0x100), + 51183 (< 0x200), + 59227 (0xE00..0xFFF) (CODE 0x101cc9b0), its Mmb `fune` / `_fun` / first.
  Position (CODE 0x101cd940): x = (col - 10) x 0.6 + w' x 0.3, y = -level x 0.1, z = (row - 12 or 13 on 2F) x 0.6 +
  d' x 0.3 with exdata col / level / row = bytes 6 / 7 / 8, rotation = byte 9 x 90 degrees (w', d' swapped when odd),
  footprint from the `furn` table (file 32922). The exdata comes in S2C 0x020 +0x11 (24 bytes). Live: a Simple Bed
  (item 2, file 32929) and a San d'Orian Flag in wall slot 0 in BotB's Mog House. Wall items take fixed slots by col
  (1F: x ±6.233 in the San d'Oria native room, ±6.5 in the others; y -3; z 4.55 / -4.8 / -4.8 / 0 / 4.55). Not done:
  the 2F wall table, the statues' real scale (items 264..299 are skinned NPC-format models in the furniture files,
  drawn as figurines as tall as the footprint's longer side: INFERRED; live: a Fafnir Statue), the campfire's effect,
  mannequins, placing / moving furniture (C2S 0x0FA / 0x0FB). Since done: the 2F wall table, mannequins, layout mode,
  the door and "menu mogdoor" (ui-format-notes §7.6); gardening (C2S 0x0FC..0x0FF), Open / Close Mog House and
  Remodel (C2S 0x0CB; S2C 0x067 kind 2 +0x24 MogHouseFlag), the Mog Garden (exit mode 127) (§7.8).

## 7. Combat

Verified live on 2026-10-16: BotA (WAR20) against Forest Hares and a Goblin Digger in West Ronfaure. Sources are
LSB `src/map/packets/c2s/0x01a_action.*` and `s2c/0x028_battle2.cpp`, `0x029_battle_message.h` and
`0x02d_battle_message2.h`; code is in `src/client/combat.*`.

### 7.1 C2S 0x01A `GP_CLI_COMMAND_ACTION`: 28 bytes

| Offset | Field |
|---|---|
| +0x04 | `UniqueNo` u32 (the target's id) |
| +0x08 | `ActIndex` u16 |
| +0x0A | `ActionID` u16 |
| +0x0C | `ActionBuf` u32[4] |

ActionID values: 0 talk, 2 attack (engage), 3 cast, 4 attack off, 7 weapon skill, 9 job ability, 0x0F change target.
`AttackOff` is validated with `isEngaged()`.

Engaging can fail:
- If the previous engage was less than one weapon delay ago, the server answers basic message 94 ("You must wait
  longer"). This happens straight after zone-in (`player_controller.cpp:79`). The retail client simply retries.
- If the target is 30 yalms or more away, the answer is message 78.

Melee rounds need the target to be in range and in front of us. While engaged, the client turns to face the target
and sends 0x015 with the target index.

### 7.2 S2C 0x028 `BATTLE2`: bit-packed

The bit stream starts at byte 5; byte 4 is the payload size. Despite the name `packBitsBE`, values are packed
**least-significant bit first** into little-endian bytes.

Header fields, in order (widths in bits):

| Field | Bits | Notes |
|---|---|---|
| actor | 32 | |
| target count | 6 | |
| res_sum | 4 | always 0 |
| action type | 4 | 1 melee round, 2 ranged finish, 3 WS finish, 4 magic finish, ... |
| action id | 32 | |
| info | 32 | |

Then, for each target: id (32 bits) and a result count (4 bits), followed by that many results. Each result is:

| Field | Bits |
|---|---|
| resolution | 3 |
| kind | 2 |
| animation | 12 |
| info | 5 |
| hit distortion | 2 |
| knockback | 3 |
| param | 17 |
| message | 10 |
| modifier | 31 |
| add-effect flag | 1 |
| spikes flag | 1 |

If the add-effect flag is set, it is followed by effect 6, info 4, param 17 and message 10 bits. If the spikes flag is
set, it is followed by effect 6, info 4, param 14 and message 10 bits.

`message` is a basic message id (LSB `enums/msg_basic.h`): 1 hit, 15 miss, 67 critical hit, 6 defeats, 31 shadows,
70 parry, and so on.

### 7.3 S2C 0x029 / 0x02D: one basic message

Both packets carry the caster (`UniqueNoCas`) at +0x04, the target at +0x08 and `MessageNum` u16 at +0x18. They
differ in where the two data words and the act indexes sit:

| Packet | Data | Data2 | Act indexes |
|---|---|---|---|
| 0x029 | +0x0C | +0x10 | +0x14 / +0x16 |
| 0x02D | +0x10 | +0x14 | +0x0C / +0x0E |

0x02D carries end-of-battle messages such as experience (msg 8) and chains (253). Skill-ups use msg 38 (Data =
skill id, Data2 = tenths) and msg 53 (Data2 = new level).

### 7.4 Status

- Our own animation is S2C 0x037 `server_status` at +0x30: 0 idle, 1 engaged, 2/3 dead, 33 resting.
- Other entities' animation is at 0x00D/0x00E +0x1F, valid with update-mask bit 0x04.
- Logging out (0x0E7) applies LEAVEGAME, which rests the character (HEALING). Taking damage cancels it. The client
  therefore disengages first and repeats 0x0E7 every 10 s.
- Our status icons: S2C 0x063 type 9, icons u16[32] at +8 (0xFF = none) and expiries u32[32] at +0x48.
- **C2S 0x0F1 `GP_CLI_COMMAND_BUFFCANCEL`** (8 bytes): `BuffNo u16` at +4, 2 bytes padding. Retail sends it at once when
  Confirm is pressed on a status icon whose file-87 record has "can cancel" (CODE 0x10227a10 -> 0x100a04c0; no
  Yes/No), and blanks that id in its own icon table (0x10485a30) until the next 0x063. LSB removes every effect with
  that icon unless it has `StatusEffectFlag::NoCancel` (`DelStatusEffectsByIcon`), then sends the new 0x063.
  xi-client: `GameUi` (Select Window twice; ui-format-notes §9.5), without the local blanking (INFERRED harmless:
  the server's 0x063 follows within a frame or two).

### 7.5 Weapon skills, job abilities, inventory and equipment

**Usable commands.** S2C 0x0AC `COMMAND_DATA` holds four bitfields, where bit n means id n:

| Offset | Bitfield | Size |
|---|---|---|
| +0x04 | weapon skills | 64 bytes |
| +0x44 | job abilities | 64 bytes |
| +0x84 | pet abilities | 64 bytes |
| +0xC4 | traits | 32 bytes |

Weapon skills appear only once the weapon's skill is high enough: BotA's great axe at level 1 had none, while its
sword at level 24 gave Fast Blade. Names come from the abilities table: the id directly for weapon skills, 512 + id
for job abilities (dat-format §11).

**Using one.** C2S 0x01A with ActionID 7 (weapon skill) or 9 (job ability) and the id in ActionBuf[0], at +0x0C.
- Weapon skills need 1000 TP and an engaged target.
- Results arrive as 0x028 action type 7 ("readies", message 43) and then type 3 (message 185, "uses X. The Y takes N
  points of damage").
- Verified live: Fast Blade.

**Inventory and equipment packets:**

| Packet | Fields | Notes |
|---|---|---|
| S2C 0x01F `ITEM_LIST` | ItemNum u32 @4, ItemNo u16 @8, Category @0xA, ItemIndex @0xB | |
| S2C 0x020 `ITEM_ATTR` | ItemNum @4, Price @8, ItemNo @0xC, Category @0xE, ItemIndex @0xF, Attr[24] @0x11 | |
| S2C 0x01E `ITEM_NUM` | ItemNum @4, Category @8, ItemIndex @9 | count change; 0 removes |
| S2C 0x050 `EQUIP_LIST` | PropertyItemIndex @4, EquipKind @5, Category @6 | |
| S2C 0x051 `GRAP_LIST` | GrapIDTbl[9] @4 | our new appearance after an equip change |
| C2S 0x050 `EQUIP_SET` | PropertyItemIndex @4, EquipKind @5, Category @6 | index 0 unequips |
| S2C 0x01B `JOB_INFO` +0x60 | encumbrance u32: low 16 bits = equip slots blocked (LSB m_EquipBlock), high = stats lowered | the equipment list greys those slots' gear (goal #45) |
| C2S 0x029 `ITEM_MOVE` | ItemNum u32 @4, Category1 @8, Category2 @9, ItemIndex1 @0xA, ItemIndex2 @0xB | LSB: ItemNum below the stack splits into Category2; else ItemIndex2 < 82 unites with that stack, 82+ moves to a free slot. Wardrobes take gear only |
| C2S 0x03A `ITEM_STACK` | Category u32 @4 | the server merges the container's part stacks (Auto-sort) |

Inventory slot 0 of container 0 is gil. Verified live: equipping a Bee Spatha +1 from the inventory.

**Magic.**
- S2C 0x0AA `MAGIC_DATA` carries `MagicDataTbl[128]` at +4: bit n = spell n is known.
- Casting is C2S 0x01A ActionID 3 with ActionBuf = { SpellId u32, PosX, PosZ, PosY } (the position is used only for
  luopan placement).
- Results arrive as 0x028 type 8 ("starts casting", message 327) and then type 4 (message 2 "casts X. Y takes N
  points of damage").
- Verified live: BotB (BLM1) cast Stone on a Forest Hare. The test bots had no learned spells, so `char_spells`
  (3, 159) was added for BotB.

**Using items.** C2S 0x037 `ITEM_USE` carries UniqueNo (the target) @4, ItemNum @8, ActIndex @0xC,
PropertyItemIndex @0xE and Category u32 @0x10. ItemNum must be 0, and the target is yourself for most items. Verified
live: BotC used a Scroll of Cure and learned the spell ("BotC learns a new spell!").

**Recasts.**
- Spells: LSB sends no spell-recast packet. The 0x028 header `info` field holds `action.recast` in seconds, and the
  client keeps spell timers from its own magic-finish actions (type 4).
- Job abilities: S2C 0x119 `ABIL_RECAST` sends Timers[31] @4, each { u16 seconds left, u8 Calc1, u8 TimerId (recast
  group), u16 Calc2, u16 pad }, followed by mount recast @0xFC. `info` is 0 for abilities.
- Verified live: Berserk showed 297 s and Mighty Strikes 3380 s.

**Talking to NPCs.**
- The client sends C2S 0x01A ActionID 0 (Talk) on the NPC.
- The server answers with S2C 0x036 `TALKNUM` (UniqueNo @4, ActIndex @8, MesNum @0xA, where bit 15 = no speaker name,
  and Type @0xC), or with 0x02A `TALKNUMWORK` (UniqueNo @4, num[4] @8, ActIndex @0x18, MesNum @0x1A, Type @0x1C,
  String[32] @0x1E), or by starting an event (0x032/0x034, still auto-finished).
- The text is zone dialog message `MesNum & 0x7FFF` (dat-format §11.4).
- Verified live: Vaquelage in Southern San d'Oria.

**Death and the home point.**
- When we are knocked out, 0x037 `server_status` becomes 2/3, and the death message (97, "was defeated by") arrives
  in 0x028/0x029.
- The client sends C2S 0x01A ActionID 0x0B (`HomepointMenu`) on itself with StatusId 0 (accept) in ActionBuf[0]. The
  server validates `isDead()`, sets `requestedWarp = HomePoint`, and the warp arrives as a normal zone change (0x00B,
  then 0x00A).
- Verified live: BotB (BLM1) was killed by a Goblin Weaver in zone 100 and returned to zone 231.

- **Character stats: S2C 0x061 CLISTATUS** (LSB `0x061_clistatus.h`; offsets include the 4-byte header):
  hpmax i32 @4, mpmax i32 @8, main/sub job and level @0xC..0xF, exp now/next i16 @0x10/0x12, base stats
  STR..CHR u16[7] @0x14, bonuses i16[7] @0x22, attack i16 @0x30, defense i16 @0x32, elemental defense i16[8] @0x34
  (fire, ice, wind, earth, lightning, water, light, dark), title @0x44, rank @0x46, nation u8 @0x50. Feeds the Status
  and Equipment windows.

- **Party** (LSB `party.cpp`; offsets include the header):
  - S2C 0x0C8 GROUP_TBL: Kind @4, 20 entries @8 of 12 bytes `{UniqueNo u32, ActIndex u16, flags u8 (bits 0-1 party
    number, bit 2 leader, bit 3 alliance leader), pad, ZoneNo u16, pad}`. It is the membership list; a table holding
    only ourselves means solo.
  - S2C 0x0DD GROUP_LIST: UniqueNo @4, Hp/Mp/Tp @8/0xC/0x10, GAttr @0x14 (as the flags above), ActIndex @0x18,
    MemberNumber @0x1A, Hpp/Mpp @0x1D/0x1E, ZoneNo @0x20, jobs and levels @0x22..0x25, Name @0x28. **LSB trims the
    packet** to the name's length rounded up to 4, plus 4, so a short name gives a packet shorter than 0x38.
  - S2C 0x0DF GROUP_ATTR updates any member's vitals; 0x0DC is an invite (UniqueNo @4, ActIndex @8, Kind @0xB, name @0xC).
  - C2S 0x06E invite `{UniqueNo, ActIndex, Kind 0 party / 5 alliance}`; 0x074 answer `{Res 1 accept / 0 decline}`;
    0x06F leave `{Kind}`; 0x070 disband `{Kind}`; 0x071 remove `{UniqueNo, ActIndex, Kind, pad, sName[15]}`; 0x077 change
    `{sName[16], Kind (0 party, 5 alliance), ChangeKind (0 party leader, 1 alliance leader, ...)}`. The client maps these to `/pcmd add <name>`, `/join`, `/decline` and `/pcmd leave`.
  - Map markers: C2S 0x0D2 MAP_GROUP `{ZoneNo u32}` (sent when the map's Markers list opens); the server answers one S2C
    0x0A0 MAP_GROUP per alliance member in the zone, us included: UniqueID @4, zone i16 @8, x / y / z f32 @0xC / 0x10 /
    0x14 (ui-format-notes.md §6.5).
  - Treasure pool (#115): S2C 0x0D2 TROPHY_LIST puts an item in a slot (TrophyItemNo @0x10, TargetUniqueNo @8 the
    monster or chest, TrophyItemIndex @0x14, Entry @0x15 = 1 when the pool is re-sent on zone-in, IsContainer @0x16);
    S2C 0x0D3 TROPHY_SOLUTION reports a lot or the outcome (LootUniqueNo @4 / sLootName @0x16 the best lotter or winner,
    EntryUniqueNo @8 / EntryPoint @0x12 / sLootName2 @0x26 who just lotted, TrophyItemIndex @0x14, JudgeFlg @0x15:
    0 lot, 1 won, 2 the winner could not take it, 3 lost). LSB sends **no message packet** with them, so the client
    writes the log lines itself from the standard messages (file 7031): 0x0D2 #16 "You find a <item> on the <monster>."
    (#218 "... in the <chest>." for a container; nothing when Entry is set), 0x0D3 #17 "<name>'s lot for the <Item>: <n>
    points." (a pass, EntryPoint 0xFFFF: nothing), #18 "<name> obtains a <item>.", #15 / #31 (us) "... does not meet the
    necessary requirements ... / <item> lost.", #164 "A <item> was lost." (judge 3, or 2 with no winner: LSB's
    `treasureLost` sends 2). `client/loot_text.h`. INFERRED: the message per case, nothing for a pass and the capital
    of #164 (its 7F 80 code); the 0x0D2 / 0x0D3 handlers in FFXiMain.dll were not traced.
  - Currencies: C2S 0x10F CURRENCIES_1 (header only) when Status -> Currencies opens; the server answers S2C 0x113
    CURRENCIES_1 (0xFC bytes: conquest points, seals, guild points, sparks, allied notes, crystals stored ...; LSB
    `0x113_currencies_1.h`). C2S 0x115 / S2C 0x118 are the Currencies 2 pair (not used by this install's menu).
    Field selection by row: ui-format-notes.md §10.10.

- **Status effects and claims.**
  - S2C 0x063 type 9 lists our status icons: u16[32] @8 (0xFF = none) and expiry u32[32] @0x48. The expiry is
    `(seconds since the Vana'diel epoch 1009810800) * 60`, which wraps around; 0x7FFFFFFF means no timer. Remaining
    seconds = `u32(expiry - now * 60) / 60`.
  - S2C 0x00D/0x00E send flags: 0x01 position, 0x02 claim, 0x04 general (HP% @0x1E, animation @0x1F), 0x08 name,
    0x10 model, 0x20 despawn. HP is only valid with 0x04: position-only updates carry 0 there.
  - S2C 0x00E: when the send flag has bit 0x02 set, +0x2C is the claim owner (UniqueNo). +0x25 bit 0x08 is set only for
    monsters (while alive); NPCs never set it. +0x28 bit 0x40 means "triggerable" and is set for both. These give the
    name colours. +0x2B is namevis (sent with the HP block, bit 0x04): 0x01 icon, 0x08 hide name, 0x80 ghost phase.
  - **Helper NPCs that are never shown or targeted** (warp01, DIRECTOR ..., issue #96), CODE:
    - Flags1 (u32 +0x20) bit 19 is the TargetOffFlag. The 0x00D/0x00E handler copies it to the actor's +0x128 bit 4
      (0x1009bfd7). The target filter 0x10083360 (Tab, Enter's front pick, the sub-target cursor) rejects any actor
      with it set (0x100844b0). LSB writes an NPC's `entity_flags` as a u32 at +0x21, so its bit 0x800 is bit 19.
      Port Windurst's warp01 has `entity_flags: 2051` (0x803).
    - The no-name bit is +0x128 bit 3, read by the name pass 0x100d0440 through 0x100844e0. For look kind 0 the
      handler sets it for standard models 0x32-0x3B, 0x32E-0x331, 0x3C0-0x3C9, 0x737-0x746, 0x974-0x97D and
      0x9BA-0x9C3 (0x1009c96c..0x1009ca2f). LSB gives its helpers model 50 (0x32), an empty model.
    - Before the look switch the same bit is cleared when Flags1 bit 16 is set (0x1009c591), otherwise it is taken
      from u32 +0x28 bit 29 (0x1009c5d1). Not wired here: the client keeps LSB's namevis 0x08 for that (the two may be
      the same flag through LSB's byte layout; not checked).
    - Here: `Entity::targetOff` (session.cpp) is honoured by `ClientApp::targetable`, the sub-target list
      (ui_frame.cpp) and the `/target*` lookups (chat_commands.cpp); `Entity::labelHidden` hides the name label
      (actors_draw.cpp); Ashita addons see no targetable render flags for such actors (prelude_memory.lua).

- **Tells:** C2S 0x0B6 `{u8 3 (LSB rejects anything else), u8 0, name[15] @2, NUL-terminated message @0x11}`.
- **Emotes:** C2S 0x05D `{UniqueNo, ActIndex, Number (emote id), Mode (0 all, 1 text, 2 motion), Param u16}`, broadcast as
  S2C 0x05A `{caster @4, target @8, caster index @0xC, target index @0xE, emote @0x10, param @0x12, mode @0x16}`.
- **Rest:** C2S 0x0E8 CAMP `{Mode u32: 0 toggle, 1 on, 2 off}`; our status becomes 33 (resting) in 0x037.
  While kneeling (status 33: /heal, or the /logout / /shutdown countdown, which LSB shows as resting) the character does
  not walk. A movement key sends the cancel once (`Session::standUp`: with our countdown running C2S 0x0E7 Mode 2 = off
  with its Kind, then 0x0E8 Mode 2; otherwise 0x0E8 Mode 2), and walking resumes only after the server's status update
  and the stand-up motion (`rest up`) have played (about 1.2 s + 1 s live). LSB does not end a rest on movement, so the
  client must; that the game sends it on a movement key is INFERRED (its actor plays res0 / loop / res2 by status 33 at
  CODE 0x1008bcca; its own /heal key toggles by status 33 at 0x10123de0 / 0x10157ed0). `XI_REST_TRACE=1` logs held
  frames.
- **Check:** C2S 0x0DD `{UniqueNo, ActIndex u32, Kind 0}`; the answer comes as battle messages (0x029).
- **Trading.**
  - With an NPC: C2S 0x036 ITEM_TRANSFER `{UniqueNo, ItemNumTbl u32[10] @4, PropertyItemIndexTbl u8[10] @0x2C,
    ActIndex u16 @0x36, ItemNum u8 @0x38}`.
  - With a player:
    - C2S 0x032 request `{UniqueNo, ActIndex}`; the partner gets S2C 0x021 `{UniqueNo @4, ActIndex @8}`.
    - C2S 0x033 `{Kind u32 (0 start/accept, 1 cancel, 2 confirm, 3 unconfirm), TradeCounter u16}`.
    - C2S 0x034 `{ItemNum u32, ItemNo u16, ItemIndex u8, TradeIndex u8}` puts an item in a box. TradeIndex 0 is
      the gil slot (ItemNo 0xFFFF); items use 1..8, and LSB rejects anything else.
    - S2C 0x022 `{UniqueNo @4, Kind u32 @8, ActIndex u16 @0x0C}`: 0 start (Start), 1 canceled (Cancell), 2 a side
      confirmed (Make; UniqueNo = who), 3 unconfirmed (MakeCancell), 4-8 errors, 6 closed, 9 done (End). The game
      (CODE 0x10152520 / 0x10152170) logs standard message 26 "You cannot trade with <name> at this time." for 1 / 4 /
      5 / 7 / 8 from the partner or the asker, 30 "Trade complete." for 9; 6 closes silently. S2C 0x021 logs 24
      "<name> wishes to trade with you." (only while no trade is on). The request itself logs string 20 of table 10
      "You offer to trade with %s.".
    - A cancel (0x033 Kind 1) is answered to the partner only; the canceller's client closes its windows itself.
      The client also cancels at 6 yalms (squared 3D distance >= 36; LSB's TradeRange is 6.0 too).
    - LSB: a full inventory makes the commit fail with MsgStd TradeCanceled ("Trade canceled. Either your or your
      target's inventory is full, ...") and Kind 1 to both. After a cancel, LSB keeps the target's TradePending: the same
      pair's next request is dropped until a relog or a completed trade (goal 29).
    - A player whose first 0x00D came without a name (e.g. warped in by a GM command) is asked for with C2S 0x016
      CHAR_REQ `{ActIndex u16}`; LSB answers with a full 0x00D (when the game asks is INFERRED).
    - S2C 0x023 is the partner's box `{ItemNum @4, TradeCounter @8, ItemNo @0xA, TradeIndex @0xD}`. 0x025 echoes our
      own `{ItemNum @4, ItemNo @8, TradeIndex @0xA, ItemIndex @0xB}`.
    - LSB refuses Ex items, Rare items the partner already owns, wrong ids and excess counts. It still sends both
      packets, with ItemNo 0xFFFF and count 0, so our boxes follow 0x025.

### 7.6 Entity names (S2C 0x00E, the rename list; #138)

**What one 0x00E does to an NPC's or monster's name** (CODE: FUNC_Packet_Incoming_0x000E, FFXiMain.dll
0x1009b480; the name block 0x1009b596..0x1009b6e0, run for every 0x00E whose mask (+0x0A) is not 0). kind = the look
word at +0x30 & 7, idx = ActIndex (+0x08), size = the header's size field x 4. The first rule that matches sets the
actor's name (+0x7c, through 0x10093360):

1. kind 0, 1, 5, 6 or 7, mask 0x08, idx >= 0x700: the string at +0x34, at most min(15, size - 0x38) characters
   (0x100ddd70 is a bounded strncpy).
2. kind 1, mask 0x40, idx >= 0x700: the string at +0x44, at most min(15, size - 0x48).
3. kind 1, mask 0x40, idx < 0x400, byte +0x44 > 0x20: the same.
4. idx < 0x400, mask 0x08, byte +0x34 == 1, byte +0x35 > 0x20: the 24 bytes from +0x35.
5. idx < 0x700: the zone's name DAT (0x1008a860 by UniqueNo; an empty default otherwise).
6. Otherwise (a dynamic entity whose update carries no name) the name is kept.

Kinds 2-4 (doors, lifts, ships) never take a packet name: their +0x34 is a door id. The look itself (kinds 1 and 7)
is only read with mask 0x10 (0x1009c8a6, 0x1009cd97).

LSB (`entity_update.cpp`):
- A renamed zone entity (targid < 1024) gets 0x34 = 1 and its name at 0x35 on every update, so rule 5 doesn't put the
  DAT name back.
- A renamed dynamic entity gets UPDATE_NAME and its name at 0x34, size 0x48 (rule 1).
- A renamed dynamic entity with an equipped look gets a 0x56-byte first spawn instead: mask 0x57 (no 0x08, with 0x40),
  byte 0x18 = 1, look_t at 0x30, the name at 0x44 (rule 2). The 0x18 byte is not part of the name rules; it sits in
  the facing-target word. Its later UPDATE_NAME updates (size 0x48) write the name at 0x34 over the look's gear
  words. Rule 1 reads it there, and the gear isn't re-read because those updates have no mask 0x10. An update with
  both UPDATE_LOOK and UPDATE_NAME would show garbage gear, in retail too.

The client: `client::entityNameFromUpdate` (src/client/entity_names.cpp). Rule 5 leaves the name empty, so the
client's DAT fallback (`updateTargets`) fills it in. LSB's "blank" placeholder stays nameless, and `_` becomes a space,
as before. Player names (0x00D, +0x5A with mask 0x08) are unchanged.

**The rename list (HorizonXI).** HorizonXI's dynamic_entity_renamer addon implements a server-side list, and the
client now does the same natively:
- **Request:** after zoning in (the addon: on load and on each outgoing 0x011), the client sends an empty C2S 0x001,
  `{0x01, 0x04, 0x00, 0x00}` padded to its size field: 8 bytes, with no payload. The addon skips this on retail.
- **Reply:** S2C 0x1FF. Its text after the 4-byte header, up to the first NUL, is `zoneId|original:new|original:new|...`.
  Empty pieces are dropped, a piece without `:` is ignored, and a list whose first piece isn't a number is ignored.
- **Matching:** each dynamic entity (ActIndex >= 0x700) is keyed by the string at +0x34 of its last update with mask
  0x08, less that string's last character. The addon does `name:sub(1, -2)`; INFERRED why. The list's `original` is
  matched against that key, only while the list's zone is the current one.
- **Timing:** a list that comes before the entities applies as they spawn, and one that comes after renames the
  entities already there. A zone change clears it.
- **Where it shows:** the new name replaces the entity's name, so nameplates, the target window, the log and /check
  all show it.
- **Option:** the request is sent only with `--entity-renames` (`XI_ENTITY_RENAMES=1`, or the launcher profile's
  "Server sends a rename list (HorizonXI)"). It is off by default, because LSB doesn't handle C2S 0x001.
- **Testing:** there is no HorizonXI server to test against (issue #126 has not started), so this path is covered by
  the unit tests only (tests/test_entity_names.cpp).

## 8. Open questions / risks

1. **Retail C2S compression.** The server accepts both forms. Uncompressed is simpler and matches the in-repo `headlessxi` tool, but nothing in the repo proves which form retail uses. It does not matter for LSB.
2. **MD5-as-subpacket hazard (§3.4)** with uncompressed C2S. The zero-terminator mitigation is reasoned from code and not yet tested against a live server.
3. **Exact send cadence and pull strategy.** The server is strictly 1 request → 1 reply with ≤32 sub-packets. Zone-in bursts (inventory, key items, quest logs) may need dozens of datagrams. Our client needs a "drain while backlog" policy.
4. **Lobby key adjustments.** The byte-16 uint8 adds in 2.1 must mirror the lobby flows our client triggers (char creation +6, delete +4, rename +4). Coordinate with the lobby doc.
5. **`compress.dat` licensing.** It comes from a GPLv3 repo. If ffxi-linux is not GPL-compatible, we need to source the table elsewhere, e.g. extract it from the user's FFXiMain.dll. The Huffman code is fully determined by the 256 (code, length) pairs.
6. **Axis convention.** The (X, height, Z) reading of 0x015 / PosHead is INFERRED from LSB's `loc.p.y` usage. Confirm against a capture or the S2C 0x00A of a known location.
7. **Ticket/Ver/sPlatform** in 0x00A are not validated by LSB. Retail values are only needed for retail parity.

- **Raise / tractor / home point (S2C 0x0F9 GP_SERV_COMMAND_RES):** `{UniqueNo @4, ActIndex @8, u16 type @0x0A}`, type 0
  home point, 1 raise, 2 tractor. The answer is C2S 0x01A: HomepointMenu 0x0B (StatusId 0 = accept), RaiseMenu 0x0D (0
  accept, anything else declines) and TractorMenu 0x13 (0 accept, 1 reject). xi-client prompts in "menu comyn":
  - home point: help 70 "Return to Home Point?", opened from the K.O. window;
  - tractor: help 96 "Accept Tractor?";
  - raise: "Accept Raise?". This wording is the client's own; the game's text for it was not found.

- **Key items (S2C 0x055 SCENARIOITEM):** `{u32 GetItemFlag[16] @4, u32 LookItemFlag[16] @0x44, u16 TableIndex @0x84}`.
  Held key item id = TableIndex × 512 + bit; LookItemFlag marks the ones already seen. Names and descriptions are in
  d_msg 55695 (dat::KeyItemTable). The menu is "menu evitem", opened from main menu page 2 → Key Items (help 470).

- **Quests and missions (S2C 0x056 GP_SERV_COMMAND_MISSION):** `{u32 Data[8] @4, u16 Port @0x24}`, as LSB enums
  quest_log.h / mission_log.h define them:
  - Ports 0x50..0x88 (step 8): quests in progress of logs 0..7 (San d'Oria, Bastok, Windurst, Jeuno, Other Areas,
    Outlands, Aht Urhgan, Crystal War). Ports 0x90..0xC8: the completed ones. Bit n = quest id n. For Aht Urhgan,
    Data[4..7] carry assault, ToAU / WotG mission and campaign state.
  - Port 0xFFFF: nation @4, current nation mission @8, Zilart @0x0C, Promathia @0x10, ... (0xFFFF = none).
  - Port 0xD0: completed nation and Zilart missions, two words per log (0..3).
  - xi-client shows them in "menu quset" → "menu quest01" and "menu missionm" → "menu miss00". Names come from d_msg
    55706.. (quests) and 55715.. (missions); see dat::LogNameTable.

- **Synthesis:**
  - The client sends C2S 0x096 COMBINE_ASK `{u8 HashNo, u8 pad, u16 Crystal @2, u8 CrystalIdx @4, u8 Items @5,
    u16 ItemNo[8] @6, u8 TableNo[8] @0x16}`; LSB ignores HashNo.
  - The result comes back as S2C 0x06F COMBINE_ANS `{Result @4, Grade @5, Count @6, ItemNo @8, ...}`, with the results
    from LSB enums/synthesis_result.h. Skill-ups arrive as battle messages.
  - xi-client: main menu → Synthesis ("menu cmbmenu", help 816 / 819) → crystal list → ingredient window. The
    ingredient window is "menu handover" (the trade window) **borrowed**: the game's own synthesis window was not
    located among the menu layouts.
  - Verified live: Wind Crystal + Arrowwood Log gave a woodworking skill-up and a result.
- **Zone dialog ids vs LSB:** in zone 230, the installed client's dialog file (6650) has one more message than
  LSB's `IDs.lua` expects, starting around 6425 (an anniversary gift line, "You sense a tiny sparkle…" and a campaign
  notice). Every id from there on is one behind: LSB ITEM_OBTAINED 6439 is "Obtained: <item>" at index 6438, and 6439
  is "Obtained <number> gil". xi-client shows what the data says; any client with this data would show the same.

- **Linkshell:**
  - Equip / unequip with C2S 0x0C4 GROUP_COMLINK_ACTIVE `{u16 rgba @0 (LSB rejects alpha != 15), ItemIndex @2,
    Category @3, ActiveFlg @4 (1 equip / create, 0 unequip), sComLinkName[15] @8, LinkshellId @0x17 (1 or 2)}`.
  - The active pearl arrives in S2C 0x0E0 GROUP_COMLINK `{LinkshellNum @4, ItemIndex @5 (0 = none), Category @6}`,
    also at login.
  - Chat kinds 5 (/l) and 27 (/l2). LSB sends party and linkshell lines to the other members only, so the client
    echoes its own.
  - Menus: main → Linkshell (label 16) → "menu inventor" (linkshell items 513–515) → "menu link2". Equip works;
    Link List and Create Pearl are dimmed.

- **Guild shops** (issue #94; ui-format-notes §10.11):
  - Talking to a guild merchant (LSB `xi.guildShops.onTrigger` -> `player:openGuildShop`) sends S2C 0x086 GUILD_OPEN
    `{u8 Stat @4 (0 open, 1 closed, 2 holiday), u32 Time @8}` and, when open, the NPC's greeting; no event starts.
    Closed: Time bits open..close-1 set (packBitsBE), bit 31 = passive (the hour tick to a player who had it open).
  - Lists: C2S 0x0AB / 0x0AD (no body) -> S2C 0x083 / 0x085 `{GP_GUILD_ITEM List[30] @4 (u16 ItemNo, u8 Count, u8 Max,
    i32 Price; sell-list Price bit 31 hides the row), u8 Count @0xF4, u8 Stat @0xF5 (0x40 first of several, 0x80 last)}`.
  - Buy: C2S 0x0AA `{u16 ItemNo, u8 PropertyItemIndex = 0, u8 ItemNum}` -> S2C 0x082 `{u16 ItemNo @4, u8 Count @6 (new
    stock), i8 Trade @7}`; sell: C2S 0x0AC `{u16 ItemNo, u8 PropertyItemIndex, u8 ItemNum}` -> S2C 0x084 (same layout).
    LSB answers a refusal with ItemNo 0, Trade -1 (buy) / -4 (sell); a partial sale with Trade -1.
- **NPC shops:**
  - S2C 0x03E SHOP_OPEN opens the shop; S2C 0x03C SHOP_LIST follows with the goods: `{u16 offset @4, entries @8 of
    {u32 price, u16 item, u8 index, pad, u16 skill, u16 guild}}`.
  - Buying is C2S 0x083 SHOP_BUY `{u32 ItemNum, u16 ShopNo, u16 ShopItemIndex @6, u8 PropertyItemIndex @8 (must be 0)}`,
    and the result comes back in S2C 0x03F.
  - xi-client: "menu shop" lists the goods (unaffordable ones greyed), then "menu shopbuy" asks "Purchase this
    merchandise?" and buys one at a time. The shop opens on "menu shopmain" (Buy / Sell).
  - Selling: C2S 0x084 SHOP_SELL_REQ `{u32 ItemNum, u16 ItemNo @4, u8 ItemIndex @6}`; the vendor quotes in S2C 0x03D
    `{Price @4, PropertyItemIndex @8, Count @0xC (not filled by LSB)}`; then C2S 0x085 `{u16 SellFlag = 1}`.
  - LSB requires 0x085 to follow 0x084 directly, and position updates (0x015) go out in between, so xi-client sends
    0x084 again in the same datagram, just before 0x085.
  - Verified: sold a Distilled Water to Benaige for 2 gil.
  - Verified with Benaige (zone 230): bought a Distilled Water for 12 gil.
