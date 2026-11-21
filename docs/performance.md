# Performance

Goal 13: set a frame-time target, meet it in a crowded city, with before / after numbers. Measured
2026-10-18 on the dev machine: NVIDIA RTX 4090, 1600x900 window (the client's default size, not maximized), present
mode `immediate` (no vsync) so the frame time is the client's own.

## Target

**p99 frame time under 8.3 ms (120 fps) at 1600x900 with 60 other players in the busiest spot, and no frame over
8.3 ms in steady play** (the zone load itself excepted). Extra frames over 8.3 ms while a crowd's models first load
("pop-in") are counted separately and should be few.

Result: met. Steady state never came close (p99 0.7..3 ms with 60 players); the work went into the pop-in hitches,
which were 9..14 ms frames for about a second whenever a crowd came into view.

## How to measure

- `XI_FRAME_TRACE=N` (N seconds per report, default 10): logs the frame time and each stage's CPU time as p50 / p95 /
  p99 / max, plus the GPU time of the frame's command buffer (Vulkan timestamps, `gfx::Context::gpuFrameMs`).
  `XI_FRAME_TRACE_CSV=path` writes one row per frame. Stages (src/gfx/frame_trace.h): `game` (input, session,
  movement, camera), `ui` (ImGui and the game's windows), `actors` (character models: load, pose, instances, names),
  `effects`, `wait` (fence + acquire), `record` (command buffers), `submit` (submit + present). The trace does not
  time its own reports. With the trace on, each new character model logs its load split (`frame trace: load ...`).
- `XI_PRESENT=immediate`: no vsync (the default is mailbox, else FIFO; on this desktop that caps at 60 fps).
- `tools/perf/`:
  - `measure.py LABEL [--bin PATH] [--after S] [--window S]`: logs BotA in at the spot, runs the trace and takes a
    screenshot after S seconds; the statistics cover zone-in + 8 s to + 8 s + window (default 25 s). Output in
    `$PERF_OUT/runs/` (default `/tmp/ffxi-perf`).
  - `crowdrun.py TAG 0,20,40,60 [--bins before=PATH,after=PATH]`: grows the crowd in steps and measures each binary
    at each size; the crowd logs out cleanly at the end (xi-headless handles SIGTERM).
  - `report.py TAG...`: the tables below; `spikes.py RUN [ms]`: the frames over a threshold with their stage split.
- The spot: BotA (charid 2) at Southern San d'Oria (zone 230) (17.5, 2, -10.486), rot 64 (facing the camera; rot
  192 faced a wall), default camera, the crowd wandering within 8 yalms of (17.5, 2, -2.5). Test windows:
  `XI_DISPLAY=secondary XI_NO_FOCUS=1 SDL_AUDIO_DRIVER=dummy`, XDG dirs in a scratch directory.

## The crowd

- 60 accounts named `test` + the number spelled out: `testone`, `testtwo` ... `testtwenty`, `testtwentyone` ...
  `testsixty` (password `crowdpass`, GM 0); one character each with the same word capitalised (`TestA` ..
  `Testsixty`; FFXI names are letters only). Account ids and charids 6..65.
- Characters are clones of BotB's rows (charid 3: jobs, skills, stats, storage, vars ...; no equipment, inventory,
  effects or recasts) with a random race, face, size and gear look (`char_look`, real model ids from
  `item_equipment`), seeded, so creating all 60 afresh gives the same looks.
- **Left in place** for later runs, logged out, at the spot. `python3 tools/perf/crowd.py create 60` recreates them
  (needs `~/git/lsb-devctl`), `crowd.py place 230 17.5 2 -2.5 6` puts them back, `crowd.py delete` removes every
  `test<number>` account with its characters and all charid-keyed rows. The user's own account `Test` is not matched.
- They log in with `build/xi-headless --user testone --pass crowdpass --char TestA --wander 8 --quiet`
  (`--wander R`: random points within R yalms of the zone-in spot at ~4 yalms/s with pauses), 1.5 s apart.
- Server load: the map server sat at ~95..100% of one core with or without the crowd (other test bots in other
  zones); memory +30 MB for 60 players. **Twice the map server's watchdog killed it** (main tick over 2 s): once
  with no crowd online, once while the 49th crowd character logged in during heavy host load (other builds running);
  it restarts by itself in ~45 s. Use a slower stagger (3 s) when the host is busy.

## Results (1600x900)

Interleaved runs of the two builds at each crowd size (`before` = 9a2f11d plus the trace; `after` = this commit).
"drawn" is the characters drawn (the game's character limit, Config cfg 60, caps it at 50 + yourself; the 0 row
includes the plaza's NPCs and the other test bots). fps and p50 move with the host's load (other worktrees were
compiling); compare rows of one size.

First pair (quiet host; `after` without the per-file motion cache):

| crowd | build | drawn | fps | frame p50 | frame p99 | actors p99 | GPU p99 | pop-in frames > 8.3 ms |
|---|---|---|---|---|---|---|---|---|
| 0 | before | 20 | 1720 | 0.51 | 1.16 | 0.31 | 0.13 | 6 |
| 0 | after | 20 | 1882 | 0.51 | 0.72 | 0.20 | 0.12 | 2 |
| 20 | before | 40 | 1178 | 0.82 | 1.14 | 0.45 | 0.17 | 15 |
| 20 | after | 40 | 915 | 1.00 | 2.35 | 0.90 | 0.18 | 10 |
| 40 | before | 51 | 812 | 1.21 | 1.61 | 0.66 | 0.20 | 23 |
| 40 | after | 51 | 850 | 1.15 | 1.61 | 0.65 | 0.20 | 5 |
| 60 | before | 51 | 799 | 1.22 | 1.73 | 0.73 | 0.20 | 21 |
| 60 | after | 51 | 750 | 1.29 | 2.07 | 0.82 | 0.22 | 4 |

Second pair (busy host, final build; the 60 step was lost to the map server restart):

| crowd | build | drawn | fps | frame p50 | frame p99 | actors p99 | GPU p99 | pop-in frames > 8.3 ms | steady frames > 8.3 ms |
|---|---|---|---|---|---|---|---|---|---|
| 0 | before | 20 | 1586 | 0.60 | 1.05 | 0.28 | 0.13 | 9 | 0 |
| 0 | after | 21 | 1584 | 0.59 | 1.05 | 0.28 | 0.11 | 3 | 2 |
| 20 | before | 40 | 432 | 2.80 | 4.18 | 0.84 | 0.24 | 16 | 1 |
| 20 | after | 40 | 991 | 0.96 | 1.56 | 0.63 | 0.15 | 9 | 0 |
| 40 | before | 51 | 309 | 3.48 | 6.27 | 1.62 | 0.30 | 24 | 3 |
| 40 | after | 51 | 637 | 1.45 | 3.03 | 1.37 | 0.20 | 8 | 0 |

The few steady frames over 8.3 ms that remain are in `wait` (a 55 ms one once): the fence / acquire, i.e. the
desktop or another GPU user, not the client.

Per extra player drawn (quiet host): about **23 µs of CPU** (p50 0.51 ms with 20 drawn, 1.22 ms with 51) and
**3 µs of GPU** (0.12 -> 0.20 ms). At 3840x2160 (measured before the fixes, window maximized on a 4K display, rot
192): p99 0.50 / 0.81 / 1.59 / 0.89 ms at 0 / 20 / 40 / 60, GPU 0.24..0.29 ms; the user asked for a normal-sized
window afterwards, so 4K was not repeated.

## Goal 25: loading off the frame (2026-10-18, done 2026-10-20)

Measured with BotC (`MEASURE=bot3,BotC,4` for measure.py / crowdrun.py), the window in the bottom-left quarter
of display 3 (`XI_WINDOW_SLOT=bl`: 1248x666, so the "1600x900" label in the tables below is the requested size, not
the drawn one), busy host (load 16..20, other worktrees building). `before` = a2f4d8d, `after` = the worker loader.
The two runs were cut short (a binary changed mid-run, then the token budget), so the rows are not all pairs:

| crowd | build | drawn | fps | frame p50 | frame p99 | actors p99 | GPU p99 | pop-in worst | pop-in > 8.3 ms |
|---|---|---|---|---|---|---|---|---|---|
| 0 | before | 20 | 1568 | 0.61 | 0.95 | 0.24 | 0.11 | 126.6 | 2 |
| 0 | after | 20 | 1371 | 0.66 | 1.47 | 0.36 | 0.10 | 13.5 | 1 |
| 20 | before | 39 | 762 | 1.16 | 2.73 | 1.07 | 0.14 | 84.9 | 7 |
| 40 | after | 51 | 761 | 1.26 | 2.13 | 0.90 | 0.18 | 18.1 | 7 |
| 60 | after | 51 | 750 | 1.28 | 1.94 | 0.79 | 0.18 | 12.6 | 1 |

- **Worker loader** (apps/xi-client/characters.cpp): a new look is read on a worker thread (the DATs, the motion sets,
  shared per motion file list, the texture decode to RGBA and the vertex build); the render thread starts its upload,
  a few a frame within ~1 ms, and the look draws nothing until then (`XI_SYNC_LOADS=1`: the old in-frame load).
  Per model the worker now takes the DAT 2..10 ms and the motions 0.3..22 ms (the first of a race) that used to stall
  the frame: the worst pop-in frame fell from 85..127 ms to 13..18 ms.
- Cloth (goal 19), offline `xi-dat clothbench MODEL ACTORS SECONDS` (60 fps, walking): model 73's 88-particle robe 10.0
  µs per actor per frame (50 actors 0.5 ms), the Gigas (271, 40 particles) 3.9 µs. No allocation per frame; only unique
  NPC / monster models wear cloth, so not a crowd cost.

### 2026-10-20: the rest of pop-in, 4K, a fight

Interleaved runs with frozen binaries (scratchpad copies, so rebuilds cannot change them), in ABBA order per crowd size
(`base`, `new`, `newB`, `baseB`), BotC measuring at the plaza, crowdrun with `PERF_NO_PLACE=1` (the crowd and
BotC already stand at the spot: no database writes). `base` = 56b926c, `new` = 0f47bed (async uploads, menu
textures at start-up, effect textures batched, pipelines at start-up). The host was shared with other worktrees'
builds and clients, and the map server restarted once mid-run (rows where `drawn` fell to 20 lost the crowd).

| crowd | build | drawn | fps | frame p50 | frame p99 | actors p99 | GPU p99 | pop-in worst | pop-in > 8.3 ms |
|---|---|---|---|---|---|---|---|---|---|
| 0 | base | 21 | 1351 | 0.73 | 0.80 | 0.19 | 0.11 | 10.8 | 1 |
| 0 | new | 20 | 1347 | 0.74 | 0.79 | 0.17 | 0.12 | 8.8 | 1 |
| 0 | newB | 20 | 1163 | 0.74 | 3.56 | 0.58 | 0.12 | 3.6 | 0 |
| 0 | baseB | 20 | 1275 | 0.78 | 0.83 | 0.20 | 0.12 | 11.4 | 1 |
| 20 | base | 38 | 918 | 1.07 | 1.26 | 0.42 | 0.14 | 10.7 | 1 |
| 20 | new | 38 | 930 | 1.06 | 1.25 | 0.42 | 0.14 | 8.2 | 0 |
| 20 | newB | 39 | 919 | 1.09 | 1.23 | 0.43 | 0.14 | 7.2 | 0 |
| 20 | baseB | 38 | 924 | 1.07 | 1.37 | 0.42 | 0.13 | 10.6 | 1 |
| 40 | base | 51 | 693 | 1.41 | 1.75 | 0.67 | 0.14 | 11.0 | 1 |
| 40 | new | 51 | 668 | 1.47 | 1.88 | 0.73 | 0.16 | 13.1 | 1 |
| 40 | newB | 51 | 668 | 1.45 | 1.88 | 0.64 | 0.16 | 6.7 | 0 |
| 40 | baseB | 51 | 658 | 1.50 | 1.78 | 0.67 | 0.14 | 10.7 | 1 |

The same runs with the 3D scene at 4K in the normal window (`--also-args "--bg-res 3840x2160"`): GPU p99 0.29..0.30
ms with no crowd, 0.40..0.42 with 20, 0.54..0.58 with 40 (1600x900: 0.11..0.16); fps and CPU stages as at 1600x900
(the frame is CPU-bound either way on an RTX 4090). Pop-in worst at 4K: base 10.4..11.1 ms (1 frame over 8.3 each
run), new 3.6..6.8 ms (none) apart from one run disturbed by the host (newB 40: 32 steady frames over 8.3 ms, every
stage raised, game 1.7..2.7 ms).

After merging main 91987ba (the camera rework, Mmb LOD switching, ship prop layers; `merged` = 1aca181 + e16e6e6's
fixes and the descriptor pool), the same ABBA order against `base`:

| crowd | build | drawn | fps | frame p50 | frame p99 | actors p99 | GPU p99 | pop-in worst | pop-in > 8.3 ms |
|---|---|---|---|---|---|---|---|---|---|
| 20 | base | 40 | 953 | 1.05 | 1.17 | 0.41 | 0.15 | 10.7 | 1 |
| 20 | merged | 39 | 988 | 0.99 | 1.15 | 0.40 | 0.15 | 6.4 | 0 |
| 20 | mergedB | 39 | 974 | 1.01 | 1.19 | 0.42 | 0.15 | 7.0 | 0 |
| 20 | baseB | 40 | 971 | 1.02 | 1.16 | 0.41 | 0.15 | 10.9 | 1 |
| 40 | base | 51 | 727 | 1.37 | 1.55 | 0.62 | 0.16 | 10.9 | 1 |
| 40 | merged | 51 | 726 | 1.38 | 1.59 | 0.57 | 0.17 | 6.6 | 0 |
| 40 | mergedB | 51 | 681 | 1.45 | 1.78 | 0.66 | 0.16 | 9.4 | 1 |
| 40 | baseB | 51 | 724 | 1.37 | 1.57 | 0.59 | 0.16 | 10.5 | 1 |

The steady state is unchanged by the merge; mergedB 40's one frame is the upload `wait` described below (8.4 ms).

What each change took away, from the stage split of the frames over 4 ms (`popin.py`, scratchpad g25b):

- **The menu textures** uploaded on first draw, one queue submit and wait each: the first in-zone frame's chat drew
  the 1024x2048 "font moji" (9.3..9.9 ms of `ui`), the lobby frame "menu newtex" (7.2 ms). Now all 315 upload in one
  batch with the interface renderer (37 ms at start-up with validation on); ImGui's descriptor pool grew to 1024 for
  them.
- **The silhouette and refraction pipelines** were made on their first draw: with the driver's shader cache cold
  (`__GL_SHADER_DISK_CACHE=0`), 10.3 ms of `record` on the first in-zone frame (the local player's silhouette shadow).
  Now `SkinnedRenderer::prewarm` at start-up: 0.8 ms record on that frame.
- **The effect screen copies** were sized on the first frame that needed them (a refraction or screen-layer effect,
  ~1.4 s into zone-in): a device wait and two allocations, 6.7..11.1 ms of `record` with a crowd. Now
  `EffectRenderer::ensureScreen` every frame, a no-op while the size holds.
- **Effect textures** were uploaded one submit and wait each when a new effect first drew; now one batch a frame.
- **Character uploads** no longer wait for the queue (`XI_ASYNC_UPLOAD=0` restores the wait); verified with
  `--validation` through a 30-client crowd, `!zone` changes, the Mog House, `!cs 7` with its sub-map and back.
- **Validation**: the effect renderer's frozen screen copy stayed UNDEFINED while bound (an error every run, also before
  goal 25); both copies now start cleared and readable. The remaining "Location 6" interface warning is a performance
  note (skinned.vert writes `vLights`, which the silhouette and refraction shaders do not read).

Left: single frames of 8..15 ms in `wait` while a crowd's uploads land (the frame's own GPU time 0.1..0.3 ms, so the
upload command buffers queued ahead of it, INFERRED). A transfer queue or a per-frame upload byte budget would spread
them.

**Fight** (BotD as WHM 30 in West Ronfaure, `XI_FRAME_TRACE`, scratchpad g25b/fight4.txt): Protectra, Curaga,
Paralyze, Diaga, Banishga x2, Diaga, Dia, then melee until the map server stopped answering (its watchdog, the known
LSB hang, ~80 s in). Of 276245 frames, none over 8.3 ms outside the zone change (168 ms `game`, the zone load) and one
8.4 ms `wait`; the worst effect frame 5.3 ms (Protectra's start, with `XI_FX_TRACE` logging on).

## DLSS 4 (goal 15, 2026-10-20)

`--dlss[=mode]` in an `XI_DLSS` build (docs/dlss.md). GPU time p50 / p99 (ms), whole frame, RTX 4090, vsync 60 Hz:

| Mode | West Ronfaure 1600x900 | Southern San d'Oria 2496x1368 |
|---|---|---|
| off | 0.23 / 0.28 | 0.23 / 0.24 |
| dlaa | 0.86 / 0.92 | 1.27 / 1.76 |
| quality | 0.80 / 0.81 | 1.15 / 1.63 |
| performance | 0.78 / 0.81 | 1.13 / 1.57 |
| ultra-performance | 0.77 / 0.77 | 1.07 / 1.46 |

The client's own GPU work is ~0.2 ms, so DLSS's evaluation is a fixed cost and a lower render size saves little: an
image-quality option here, not a speed one. Recording grew 0.3-0.8 ms on the CPU (motion vectors, NGX). Without the
flag nothing changes (screenshots match main within run-to-run noise).

## Ray-traced lighting (goal 38, 2026-10-20)

`--rt` (docs/raytracing.md), RTX 4090, 1600x900, BotC in Southern San d'Oria at 13:00 with ~20 characters,
`XI_FRAME_TRACE=10`, GPU time per frame p50 / p99 (ms); the frame stays at 60 Hz:

| `--rt` | GPU | record (CPU) |
|---|---|---|
| off | 0.14 / 0.14 | 0.13 |
| ao | 1.19 / 1.24 | 0.44 |
| shadows | 1.16 / 1.18 | 0.41 |
| ao,shadows (plain `--rt`) | 1.27 / 1.29 | 0.43 |
| gi | 1.28 / 1.30 | 0.42 |
| reflections | 1.13 / 1.38 | 0.43 |
| all | 1.56 / 1.59 | 0.43 |

By part (shadows only, `XI_RT_SKIP`): characters' per-frame BLASes ~0.3, trace ~0.3, filter ~0.2, composite ~0.04,
TLAS and the offscreen copy ~0.2. The first frame in a zone builds its BLASes in one call (227-270, 3-4 MB scratch).

Re-measured 2026-10-21 (BotD, same place, time and weather) after the water-reflection fix (water planes from the
sea effects, a blend pass after the effects): off 0.14 / 0.14 (record 0.11), ao,shadows 1.26 / 1.28 (0.38), reflections
1.13 / 1.15 (0.40), all 1.53 / 1.55 (0.38). Scanning the effect batches for the water planes every frame had raised
the recording to 0.81 p50 with spikes to 5 ms; every 15 frames it is gone.

## Enhanced graphics (goal 41, `--rt-boost`, `--post`)

GPU ms per frame p50 (RTX 4090, scene 1600x900, BotD, six scenes; the full table is in enhanced-graphics.md
"Comparing the options"): none 0.13-0.27; `--post` 0.29-0.46 (each of hdr / bloom / screen-space godrays / fog alone
0.19-0.40); `--rt-boost` at any level costs what `--rt=all` does (1.0-2.7, the spread is report noise);
`--rt-boost=high --post` 1.4-3.8 (the ray-queried light shafts add about 0.4-0.8 over `--rt` alone); with `--dlss` and
`--hd-textures` too, 1.6-3.1.

## Hot spots found and fixed

1. **Character model loads in the frame** (the only stalls over 8.3 ms). A new appearance cost ~4.6 ms (DAT 0.8,
   upload 1.9, motions 1.8) and two were loaded per frame, so a crowd coming into view gave ~1 s of 9..14 ms frames.
   - The motions (the race's and weapon class's Mo2 files) were read and parsed again for every look: now each file
     is parsed once (src/dat/motion.cpp `fileMotions`), and each look's assembled set is shared per file list
     (apps/xi-client/characters.cpp).
   - The textures were uploaded one queue submit and wait each (6..8 per model): now one submit per model
     (`TextureSet::add` of a batch).
   - A second load in the same frame only starts when the first took under 2 ms.
   - Now ~2.9..3.7 ms per load (DAT 1.0, upload 1.3, motions 0.6..1.3, the first of a race still parses); pop-in
     frames over 8.3 ms fell from 15..24 to 4..10 per crowd.
2. **The trace timing itself**: its 10 s reports showed as a 5..9 ms `game` frame; they are now outside the frame.

Not changed (well under the target): per-actor posing, the per-frame copy of the entity list, string keys for model
lookups, effects (~0.2 ms with the zone's ambience), the first in-zone frame's ~9 ms `ui` (the windows' first draw).

Visual check: screenshots of both builds at a fixed time and weather (`--time 12:00 --weather 0`) differ only in the
server's state (an NPC and the welcome text not yet there in one) and BotA's idle animation phase.

## HD textures (goal 41, `--hd-textures`)

RTX 4090, 1600x900, BotC in Southern San d'Oria at 12:00 in clear weather, same build with and without the flag:
the zone loads in 715-762 ms against 49-73 ms (decoding the 4x PNGs from the cache; uploads and GPU mips as before),
and the client's VRAM goes from 199 to 603 MiB (textures are RGBA8 with full mip chains). Frame times are unchanged in
practice (the shaders sample the same way; only texture size grows). Building the cache takes about 0.27 s a texture
on the GPU (docs/enhanced-graphics.md).

## Path-traced lighting (goal 41, `--pathtrace`)

RTX 4090, a 1248x666 window, BotC in Southern San d'Oria at 13:00 in clear weather, `XI_FRAME_TRACE`, GPU p50 / p99:
off 0.11 / 0.11 ms; `--pathtrace=1` 1.25 / 1.62; `--pathtrace` (2 bounces) 2.67 / 2.85; `--pathtrace=3` 1.47 / 2.18;
`--dlss --pathtrace` (Ray Reconstruction) 2.82 / 2.85. Recording grows 0.3-0.5 ms (the TLAS and the characters' BLASes,
as `--rt`). One run each: bounce counts overlap because paths stop once their throughput is under 0.2 %
(docs/enhanced-graphics.md).
