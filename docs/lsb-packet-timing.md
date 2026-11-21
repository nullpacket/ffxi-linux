# LSB packet timing: how fast should the client send?

Issue #77 (related: #43, §6 below). Source read: the local LSB checkout `~/git/server`, branch `base`
at `1a3c233b` (read only). All `src/...` and `settings/...` paths below are in that tree. Retail references are CODE in
`FFXiMain.unpacked.dll` (the DLL with its LZSS-packed `.text` unpacked).

**Answer in short.** LSB sets no rate for the client and expects none: it answers each datagram once and has no
send loop of its own, so the client's rate is the rate at which it receives game state. The retail client's base
interval traces to **400 ms** (the `/pcnt` launch argument's default), the same as LSB's 400 ms logic tick. Our client
now uses 400 ms instead of the INFERRED 200 ms.

## 1. What the map server does with a datagram

- **One reply per datagram, no other sends.** `MapNetworking::handle_incoming_packet`
  (`src/map/map_networking.cpp:96-182`) decrypts, parses, builds one reply (`send_parse`, `:123`) and queues it
  (`socket_->send`, `:171`). That is the only send call in the map server (grep `socket_->send` / `send_parse`); no
  timer sends to clients. Downlink rate = uplink rate.
- **Stop-and-wait ack.** If the datagram's header ack (`buff[2]`) is not the server's current `server_packet_id`, the
  server treats its last reply as lost and re-sends the cached one instead of new data, without draining its queue
  (`:532-550`). The sub-packets in the stale datagram are still handled (`:466-503`, before the check). Sending again
  before the previous reply arrives therefore buys nothing but a duplicate.
- **Dedup window.** A sub-packet is handled only if `client_packet_id < sync <= header[0]` (`:472-475`); re-sent
  sub-packets are dropped silently.
- **Per-reply capacity.** At most `kMaxPacketPerCompression` = 32 sub-packets per reply
  (`src/map/map_constants.h:79`, clamp at `map_networking.cpp:572`), shrunk by a third until the compressed body fits
  1300 - 28 - 16 bytes (`:644`, `:661`). The rest waits for the next datagram. With `logging.DEBUG_PACKET_BACKLOG`
  the server warns once more than `kMaxPacketBacklogSize` = 192 are queued (`map_constants.h:80`,
  `map_networking.cpp:696-713`; off by default, `settings/default/logging.lua:58`).
- **What one 0x015 costs the server.** Every handled 0x015 runs `SpawnNPCs`, `SpawnMOBs` (which also runs the aggro
  check `tapMobAggro` on every mob in range, `src/map/zone_entities.cpp:954-979`), `SpawnPETs`, `SpawnTRUSTs` and flags a
  PC sync (`src/map/packets/c2s/0x015_pos.cpp:102-106`), all on the main thread. The PC sync is batched: the zone tick
  serves at most 20 characters' `requestedInfoSync` per tick (`zone_entities.cpp:2199-2214`). So the per-player main-
  thread cost scales with how many 0x015 the client sends.

## 2. Server clocks, limits and timeouts

| What | Value | Where |
|---|---|---|
| Logic update (AI, zone tick) | 400 ms ("DO NOT CHANGE") | `src/map/map_constants.h:44-50`, used `src/map/zone.cpp:1125` |
| Trigger areas | 200 ms | `map_constants.h:53` |
| Time server (status ticks, regen) | 2400 ms | `map_constants.h:39` |
| Ships / lifts | 400 ms | `map_constants.h:42` |
| Session sweep | every 5 s | `map_constants.h:63`, `src/map/map_engine.cpp:235` |
| Link-dead | > 5 s since the last datagram that decrypted with the current key (seen by the next sweep, so 5-10 s) | `src/map/map_session_container.cpp:204-218`; tap in `map_networking.cpp:459-464`, `src/map/map_session.cpp:60-66` |
| Session dropped | > `map.MAX_TIME_LASTUPDATE` = 60 s | `map_session_container.cpp:220-283`, `settings/default/map.lua:18` |
| 0x00A from a new address | ignored if the session moved < 1 s ago | `map_networking.cpp:284-288` |
| Main-thread watchdog | kills the server if the tick stalls >= `main.INACTIVITY_WATCHDOG_PERIOD` = 2000 ms | `map_engine.cpp:316-373`, `settings/default/main.lua:327-328` |
| UDP socket | 4 MB buffers, 4096-datagram ingress / egress rings; overflow is dropped and logged | `src/map/map_socket.cpp:52-55`, `:251-258`, `:321-325`, `:470-492` |

The ingress drain runs on the main thread and is unbounded (`map_socket.cpp:289-308`): a flood from a client would
lengthen the tick, but a few datagrams per second per player is far below anything that matters.

**Per-packet rate limits.** `PacketRateLimiter` (`src/map/packets/c2s/rate_limiter.cpp:30-94`, checked in
`PacketSystem::dispatch`, `src/map/packet_system.cpp:375-397`) drops a second copy of the same packet id that arrives
within its minimum gap and logs "Rate-limiting packet ...". Defaults (`settings/default/network.lua:45-58`):

| Packet | Min gap (ms) |
|---|---|
| `GP_CLI_COMMAND_SUBCONTAINER` (mannequin equip) | 1000 |
| `GP_CLI_COMMAND_MOTION` (emotes) | 1000 |
| `GP_CLI_COMMAND_SHOP_BUY`, `GP_CLI_COMMAND_GUILD_BUY` | 250 |
| `GP_CLI_COMMAND_ASSIST_CHANNEL` | 1000 |
| `GP_CLI_COMMAND_TRACKING_LIST`, `GP_CLI_COMMAND_TRACKING_START` (wide scan) | 1000 |
| `GP_CLI_COMMAND_MASTERY_DISPLAY` | 2000 |
| `GP_CLI_COMMAND_JUMP` | 2000 |
| `GP_CLI_COMMAND_SUBMAPCHANGE` | 200 |

The local server's own `settings/network.lua` has no `PACKET_RATE_LIMITS` table, so which limits apply here depends
on how LSB merges the defaults (not checked); the map log of the last 72 h has no "Rate-limiting" line. 0x015 (POS)
and 0x01A (actions) are not limited.

Handler-level cooldowns (reject, not throttle): synthesis 15 s (`0x096_combine_ask.cpp:92`), inventory sort 1 s
(`0x03a_item_stack.cpp:42`), a trade invite to someone with an unanswered invite < 60 s (`0x032_trade_req.cpp:103-111`),
yell `map.YELL_COOLDOWN` = 30 s (`0x0b5_chat_std.cpp:260-273`, `settings/default/map.lua:249`), self-unstuck 24 h
(`0x0f0_rescue.cpp:48-55`).

**Server-side rate control of the client.** S2C 0x005 `GP_SERV_COMMAND_PACKETCONTROL` exists to set the client's rate
("configure the clients rate limiting", `src/map/packets/s2c/0x005_packetcontrol.h:26-40`) but is "TODO: Unimplemented";
LSB never sends it. Nothing else in the server mentions the client's send interval.

## 3. What retail does (CODE, traced for this issue)

The flush timer and back-off were traced in goal 43 (docs/protocol-map.md "C2S cadence"). New here: where the base
and the first interval come from.

- `0x10103260` initialises the launch-options block; its +0xd0 is set to **400** (`param_1[0x34] = 400`).
- `0x10102270` parses the command line: `/pcnt N` overwrites that field (`0x10102879-0x10102897`, string `/pcnt` at
  0x10364744). Both write the same stack slot (block at ESP+0x88 before `PUSH EDI`, `[ESP+0x15c]` after; same frame
  offset).
- `0x101032f0` passes block +0xd0 to `0x100f0060` (7th argument), which stores it at +0x118 of the login state
  (`0x100f013b`). The login state machine `0x100f01e0` (state 7) hands it to `0x100f9960` as its 9th argument.
- `0x100f9960` calls `0x100f9870(pcnt)`, which stores it as the **base interval** at global +0x44bac (`0x100f98bb`), and
  `0x100f9de0(..., pcnt, ...)`, which sets the session's **current interval** at +0x417c to `pcnt * 5`
  (`piVar1[0x105f] = param_9 * 5`).
- The timer is in milliseconds: timer 0 adds the frame delta from `0x100dbde0`, the difference of a `timeGetTime`
  millisecond counter wrapped at 1000 (`0x100dbe50`).
- Before each send in zone (session state byte +0xc == 3), `0x100fa920` steps the interval: more than one datagram
  unacknowledged adds 50 ms (cap base + 3000); otherwise, while above the base, it subtracts one base step, floored at
  the base. Outside zone the interval does not change.
- S2C 0x005 (`0x100fabd0`, no Ghidra function; bytes `8b44240c ... 898aac4b0400 ... 898a7c410000 c3`) stores the
  packet's u32 @+4 as the new base and also as the session's current interval.

So with no `/pcnt` and no 0x005: the session starts at 2000 ms, and in zone falls 2000 -> 1600 -> 1200 -> 800 -> 400
over its first sends, then sends every **400 ms** (2.5 datagrams/s), backing off by 50 ms steps while replies are
missing. **INFERRED:** that the launchers (PlayOnline, xiloader, Ashita) pass no `/pcnt`; the user's pacecap capture
(#43) will confirm.

## 4. Our client

`src/map/map_client.cpp` (goal 43) already flushed on the game's timer with its back-off and 0x005 handling. Changed
for #77:

- the base is `kDefaultSendInterval` = 400 ms (was 200 ms, INFERRED); `XI_NET_INTERVAL=ms` still overrides;
- the step is `nextSendInterval()` (`src/map/map_client.h`), tested in `tests/test_pacing.cpp`; like the game it only
  changes the interval in zone (before, our ease-back also ran outside zone).
- Not adopted: the first interval of 5 x base. Our handshake starts at the base so zone-in is not slowed by the
  2 s first interval; this only affects the first second or two after each zone-in.

What 400 ms means against LSB:

- matches the server's 400 ms logic tick: a shorter interval does not get fresher AI / position results, it only
  re-runs the spawn and aggro scans (§1) more often; 400 ms halves that per-player main-thread work compared with 200 ms;
- downlink capacity is 2.5 replies/s x up to 32 sub-packets = 80 sub-packets/s per player (more than the busiest zones
  produce outside zone-in); queued packets just arrive over a few more replies during zone-in;
- reaction latency: a server message waits up to one interval for the next datagram (up to 400 ms instead of 200 ms),
  the same as retail;
- keepalive: 400 ms (or the 3.4 s back-off cap) is well inside the 5 s link-dead limit.

The map-server hangs from #43 come from the Debug LSB build running out of main-thread headroom, made worse by builds on
the same machine, not from packet rate (§6); 400 ms reduces load but does not remove them.

## 5. Command latency (#97)

Measured with BotA against the local (Debug) LSB, a `/ja "Berserk" <me>` every 2–3 s (`XI_NET_TRACE=1`, 2026-10-24):

| | queued -> sent | sent -> first 028/029 | server answer time per datagram |
|---|---|---|---|
| timer only (`XI_NET_URGENT=0`, retail's behaviour) | 27–390 ms (avg ~170) | 0–1285 ms (median ~755) | |
| commands at once + quick window | 0–90 ms (avg ~20) | 118–1259 ms (median ~490) | 150–215 ms avg, up to ~470 ms |

The client-side wait is gone; what remains is the server's: it answers a datagram 150–215 ms after it arrives on
average (up to about one 400 ms tick) and produces an ability's result on its zone tick
(`CPlayerController::Ability` → `CAbilityState::Update`). Before #97 the session's network thread also waited up to
100 ms (`map.tick(100 ms)`) before it even queued the command; it now waits 10 ms. Details in docs/protocol-map.md §1.4
"Player commands".


### 5.1 /check (Ctrl+C) and other commands (#114)

Measured with BotC checking itself in Southern San d'Oria, three `/check` 3 s apart (`XI_NET_TRACE=1`, 2026-10-26,
server image `landsandboat/server:testing` still the Debug build of 2026-10-18):

| /check | key -> 0DD sent | 0DD sent -> server answers it | what the trace shows |
|---|---|---|---|
| 1 | 0 ms | 997 ms | our datagrams 30, 31, 32 (0, 82, 583 ms) all answered in one burst at 997 ms (one reply, two re-sends) |
| 2 | 0 ms | 35 ms | answered at once |
| 3 | 0 ms | 481 ms | datagrams 49, 50 answered together at 481 ms |

The client side is immediate: Ctrl+C runs `/check` on the next frame, the command reaches the network thread within
10 ms and goes out in its own datagram at once (§5). The wait is the map server's. LSB's socket thread only queues
datagrams (`MapSocket::Impl`, `src/map/map_socket.cpp`); they are parsed and answered on the main thread
(`drainIngress`, posted to the main-thread scheduler), and that thread is busy 100% of the time in the Debug build
(§6), so the datagrams that arrived during a busy stretch are answered together when it frees up, 0–1000 ms later. The
check itself is answered in the same pass (`GP_CLI_COMMAND_EQUIP_INSPECT::process` pushes the 0x029 / 0x0C9 at once).
Retail would be slower still on this server: it only sends on its 400 ms timer (§3).

So nothing in the client delays a command; the fix for the delay is the server image as a release build (§6, the
command on #43). `XI_NET_TRACE=1` now traces every command, not only actions: "NNN queued -> sent", each datagram sent
and received within 2 s of it ("sent datagram N (acks M)", "datagram N (acks M) ... after", "re-sent datagram ...",
which shows the server's re-sends for datagrams that arrived while its main thread was busy).

## 6. Disconnects and the map server's watchdog (#43)

The user's report (2026-10-24): "We are still experiencing client timeouts and disconnects from the map server". The
server logs (`~/git/server/log/map-server.log`, `docker logs server-map-1`, read only) show what ends the sessions.

**What happens.** Every disconnect in the log is the map server's inactivity watchdog killing the process
(`!!! INACTIVITY WATCHDOG HAS TRIGGERED !!! / Process main tick has taken 2000ms or more. / Killing Process!!!`,
`src/map/map_engine.cpp:316-373`); Docker restarts it and it takes 55-100 s to load again. A killed server never closes
our session, so the client just stops getting replies. Example: a character logged in at 04:44:27 UTC on 2026-10-24 and the
watchdog killed the server at 04:46:11; the user's note followed at 04:48. No session in the logs was ever closed for a
packet problem: there is no link-dead or `MAX_TIME_LASTUPDATE` drop of a client that was still sending, no rate-limit
line. The `map_decipher_packet: bad packet` errors (1340) are nearly all from 2026-10-20 / 21 (older client builds
sending with a key the server no longer held); since then only one burst (2026-10-24 05:01, a client still
sending to the server that had just restarted at 04:59).

**Why it fires so often.** Watchdog kills per day: 4 / 21 / 2 on three days in September, then 9, 40, 51, 33, 15, 27,
35, 24 from 2026-10-18 to 10-25 (270 in all). The jump starts with the server image `landsandboat/server:testing` built
2026-10-18 20:10 PDT from `dev.docker-compose.yml`'s `build` service, which is a **Debug** build
(`CMAKE_BUILD_TYPE: "Debug"`, `PCH_ENABLE: "OFF"`; the log prints `Build type: Debug` at every start). That build runs
all 300 zones' AI on one main thread at no optimisation, and the main thread is busy all the time:

| Map server main thread, sampled once a second for 9-10 min (`/proc/<pid>/task/<pid>/schedstat`) | CPU | run-queue wait, avg / max per second | watchdog kills |
|---|---|---|---|
| A. no clients, no builds | 100 % | 0 / 1 ms | 0 |
| B. 6 headless bots logged in, 400 ms pacing (zone 230) | 100 % | 0 / 0 ms | 0 |
| C. the same 6 bots, the old reply pacing (`XI_NET_PACING=reply`) | 100 % | 0 / 1 ms | 0 |
| D. no clients, a local build (`tools/build.sh`, nproc-2 at nice 10, no ccache) | 100 % | 6 / 431 ms | 0 |
| E. 6 bots logging in and out every ~30 s, retail gaps | 100 % | 0 / 4 ms | 0 |
| F. E plus a local build | 100 % | 5 / 213 ms | 0 |
| G. E during this branch's CI pipeline (10 jobs, 08:34-08:44) | 100 % (p95 112 %) | 14 / 261 ms | 0 |

A sampled main thread is always runnable (state R, never in a syscall); the scheduler blocks in `io_context::run`, so
100 % is work, not a spin. The stacks the watchdog prints at each kill are spread over ordinary mob AI (roaming path
finds, regen ticks, `UpdateEntityPacket`, `dynamic_cast`s; `tools/pacing/stacks.py`): no single hang, just a main thread
with no headroom, so any extra delay pushes one turn of the loop past 2 s.

None of these 10-minute runs hit a kill: at the long-run rate (about one every three hours outside CI jobs, about ten an hour inside them)
that is not surprising, and the runs used zone 230, which has no mobs. They do show the two ingredients: no headroom on
the main thread at all, and builds adding up to 0.2-0.4 s of waiting per second.

**What pushes it over.** Builds on the same machine. Of the 110 kills between the first CI job (2026-10-22 18:03 UTC)
and 2026-10-26 06:51, **87 happened while a CI job was running, although CI ran only 10 % of that time**, nearly all
in `build-test:default` (54) and `build-test:dlss` (31); most of the rest fall in the hours the threads were building
locally. While a build runs, the map server's main thread waits up to 0.2-0.4 s per second for a CPU (D, F), on top of
a loop that already uses all of it. The CI job containers and the map server are separate cgroups of equal weight, so
`nice` in `tools/build.sh` does not lower a CI build against the server.

**Our client's share.** Next to none: the client's rate (2.4 datagrams/s idle at the 400 ms base, one 0x015 each) and
even the old reply pacing (4.4/s) change nothing measurable (B vs C), and login / logout churn at retail gaps neither
(E). The client is not what overloads the server.

**What to do (the user's call; no server changes from Claude).**
1. Rebuild the server image as a release build, which is what LSB ships (`docker/ubuntu.Dockerfile` defaults to
   `CMAKE_BUILD_TYPE=Release`). From `~/git/server`:
   `docker compose -f dev.docker-compose.yml -f docker-compose.override.yml build --build-arg CMAKE_BUILD_TYPE=RelWithDebInfo build && docker compose -f dev.docker-compose.yml -f docker-compose.override.yml up -d --force-recreate connect search world map`
   (RelWithDebInfo keeps the watchdog's stack traces readable.)
2. Optionally, give CI jobs a lower CPU weight than the server: `cpu_shares = 256` under `[runners.docker]` in
   `~/gitlab-local/runner/config.toml` (the runner reloads it on its own).
3. Anything that still trips the watchdog with a release build goes to issue #29 as an LSB note.

**The client side (#43).** The session already gives up after 45 s without a reply (time INFERRED). It used to leave a
frozen window with the reason only in the F11 overlay; now the client goes back to the login form with "Lost
connection to the server." (or "Could not connect: ..." when the login itself fails, such as lobby error 201 while the
old session is still open on the server), and Enter logs in again once the server is back
(`src/client/session_end.h`, `ClientApp::returnToLoginIfEnded`). `XI_NET_DEAF_AFTER=s` makes the client ignore the
server from s seconds on, to try this without killing anything.

**Cadence measured (our client, BotA in West Ronfaure, `XI_NET_TRACE=1`, 2026-10-26):**

| | datagrams/s | 0x015/s | sub-packets per datagram | reply time avg / max |
|---|---|---|---|---|
| goal 43 start (reply-paced), idle / running | 4.4 / 4.5 | same | 1.0 | bursts: gaps 3 ms median, 708 ms p90 |
| timer, base 200 ms (INFERRED), idle | 3.5 | 3.5 | 1.0 | |
| timer, base 400 ms (traced, #77), idle | 2.4 | 2.4 | 1.0 | 160-200 / 300-420 ms |
| the same during the /logout countdown | 2.0 | 0 (none while logging out) | 0.0-0.2 (0x0E7 re-asked every 10 s) | 110-200 ms |
| retail | pending the user's pacecap capture | | | |

Tools: `XI_NET_TRACE=1` (every 5 s: datagrams/s, B/s, sub-packets per datagram, each id's rate); `tools/pacing/`:
`stress.py` (headless crowd login / stay / leave cycles: `old` = SIGKILL and instant relog, `rapid` = clean logout and
instant relog, `retail` = clean logout, wait for `accounts_sessions` to clear, plus GAP), `analyze.py` (a pacecap CSV ->
per-segment datagrams/s, 0x015/s, gaps), `kills.py` (what the map server logged before each watchdog kill),
`stacks.py` (main-thread stacks at kills). `tools/perf/measure.py` / `crowdrun.py` keep a `RELOG_GAP` (10 s) after the
server drops a session.

The one step left is the retail baseline: the user's pacecap capture (goal 43 "User test", issue #43); `analyze.py`
checks the measured base against the traced 400 ms (a different value means the launcher passes `/pcnt`).
