# Graphics options: a reference guide

Every optional graphics feature of the client, what each flag does, how they combine, what they cost, and
ready-made combinations to start from. All of them are **off by default**: without a flag the client draws exactly
the game's look (each feature was checked against a build without it: the images match within run-to-run noise).

The deep technical write-ups are in [dlss.md](dlss.md), [fsr.md](fsr.md) (FSR 3.1, AMD and Intel GPUs),
[raytracing.md](raytracing.md) and [enhanced-graphics.md](enhanced-graphics.md); this page is the one to keep open while
trying things.

**AMD and Intel GPUs:** everything below works on them except DLSS, which is NVIDIA's; `--dlss` there starts FSR 3.1
with the same mode, and ray tracing needs an RX 6000 / Arc or newer. These GPUs are supported but not yet tested on
real hardware ([fsr.md](fsr.md) has the per-vendor table and a checklist for testers).

## Quick start

Add the flags after your usual launch line, for example:

```
~/git/ffxi-linux/scripts/xi-client.sh --user test --pass test --char Name --autoconnect --dlss --rt
```

To make a set of flags your default, put them in `XI_ARGS` in the launcher's settings file (see the top of
`scripts/xi-client.sh`).

| I want... | Flags |
|---|---|
| The original game, sharper | `--dlss=dlaa` |
| The original look with subtle modern lighting | `--rt` |
| Clearly better lighting, still FFXI | `--rt-boost=high --post` |
| The most dramatic lighting | `--rt-boost=max --post --post-bloom=3 --post-shafts=3 --post-fog=3` |
| A "remaster" look (lighting rebuilt from scratch) | `--dlss --pathtrace --post --hd-textures` |
| Sharper textures only | `--hd-textures` |

## What you need

| Feature | Needs |
|---|---|
| `--dlss` | A build with DLSS (`-DXI_DLSS=ON`, SDK fetched by `tools/fetch_dlss_sdk.sh`; the main `build/` is set up this way, SDK in `~/.local/share/ffxi-linux/dlss-sdk`) and an NVIDIA RTX GPU. Without them the client uses FSR 3.1 in the same mode (or renders normally if FSR can't run either). |
| `--fsr` | Any Vulkan 1.3 GPU (AMD, Intel, NVIDIA) and a build with FSR (on by default; the SDK is fetched at configure time). |
| `--rt`, `--rt-boost`, `--pathtrace` | A GPU with Vulkan ray queries: NVIDIA RTX, AMD RX 6000 (RDNA2) or newer, Intel Arc. Without one the flag logs a warning and is ignored. |
| `--post` | Nothing extra (light shafts use ray queries when `--rt` is on, a screen-space version otherwise). |
| `--hd-textures` | The texture cache, built once on your machine: install the upscaler (one line in [enhanced-graphics.md](enhanced-graphics.md)), then `xi-dat hdtex build --zones ...` or `--all`. Missing textures fall back to the originals. |

## The options

### DLSS 4: `--dlss[=MODE]`

Upscales the 3D scene with NVIDIA DLSS (the newest transformer model); the interface always stays at full resolution.

| Mode | Renders at (per axis) | Use |
|---|---|---|
| `quality` (plain `--dlss`) | ~67 % | the best balance |
| `balanced` | ~58 % | |
| `performance` | 50 % | |
| `ultra-performance` | ~33 % | |
| `dlaa` | 100 % | no upscale: the best anti-aliasing, the sharpest image |

- On this machine the game's own GPU work is tiny (~0.2 ms), so DLSS is an **image-quality** option, not a speed-up:
  it costs a fixed 0.6-0.9 ms.
- With any ray-traced option (`--rt`, `--rt-boost`, `--pathtrace`) DLSS also turns on **Ray Reconstruction**, which
  denoises the traced lighting better than the built-in filter (and usually costs less overall).
- Frame Generation is not available on Linux (NVIDIA's route is Windows-only; goal 42 would add it).

### FSR 3.1: `--fsr[=MODE]`

AMD's temporal upscaler, for every GPU: the same modes as DLSS (`quality` 1.5x, `balanced` 1.7x, `performance` 2x,
`ultra-performance` 3x, `native` for anti-aliasing only), `--fsr-sharpness=0..1` (default 0.2). It doesn't denoise ray
tracing (the client's own filter does). `--upscaler=auto` picks DLSS where it runs, else FSR. Details: [fsr.md](fsr.md).

### Ray-traced lighting: `--rt[=EFFECTS]`

Adds traced effects on top of the game's own lighting, keeping its colours, light directions and fog. Subtle by design.

| Effect | Name in the list | What you see |
|---|---|---|
| Ambient occlusion | `ao` | soft darkening in corners, under objects, where walls meet floors |
| Shadows | `shadows` | real sun / moon shadows from everything (trees, walls, characters) and from up to 4 lamps |
| Bounce light | `gi` | light reflecting off nearby surfaces, colouring them slightly |
| Water reflections | `reflections` | a mirror-like sheen on harbours and canals |

Plain `--rt` = `ao,shadows`. `--rt=all` = all four. Indoors (floors the game marks as roofed) the sun's shadow fades
and the game's own shadow patches return, so rooms don't go dark.

| Tuning flag | Range | Default | Effect |
|---|---|---|---|
| `--rt-strength=` | 0..2 | 0.7 | how far the traced terms replace the game's lighting |
| `--rt-shadow-cap=` | 0..1 | 0.4 | the most of a pixel's light a sun / moon shadow can remove (1 = full, near-black at night) |
| `--rt-reflect=` | 0..3 | 1 | the water reflection's weight |

### Stronger ray tracing: `--rt-boost[=low|high|max]`

A preset that turns `--rt` up. Plain `--rt-boost` = `high`. Without `--rt` on the line it turns on every effect.

| Preset | Strength | Shadow cap | Reflections | Bounce | AO radius |
|---|---|---|---|---|---|
| `--rt` | 0.7 | 0.4 | 1.0 | 0.6 | 1.6 yalms |
| `low` | 1.0 | 0.6 | 1.3 | 0.8 | 2.0 |
| `high` | 1.3 | 0.85 | 1.7 | 1.1 | 2.6 |
| `max` | 1.6 | 1.0 | 2.2 | 1.5 | 3.2 |

An explicit `--rt-strength`, `--rt-shadow-cap` or `--rt-reflect` wins over the preset. The presets cost the same as
`--rt=all`; they only change weights.

### Post-processing: `--post[=hdr,bloom,godrays,fog,all]`

Modern image effects on the finished scene, before the interface (which is never touched). Plain `--post` = all.

| Part | What you see | Strength flag (0..5, 1 = default) |
|---|---|---|
| `hdr` | nothing on its own: the HDR pipeline the others run in (today's look is kept exactly) | |
| `bloom` | a glow around the brightest things: lamps, torches, sky, sun glints on water | `--post-bloom=` |
| `godrays` | shafts of sun / moon light through trees, arches and gaps (traced with `--rt`, screen-space without) | `--post-shafts=` |
| `fog` | a height fog in the zone's own fog colour, thicker near the ground, on top of the game's fog | `--post-fog=` |

The defaults are mild; around 3 gives the dramatic look. With `--dlss` the post effects run after the upscale.

### Path-traced lighting: `--pathtrace[=BOUNCES]`

The biggest change: the zones' baked lighting is set aside and every pixel is lit by tracing light from the sun /
moon, the sky, and the zone's lamps and fires, with 1-3 bounces (default 2). The game's colours, light directions and
fog stay; what changes is where light reaches (dappled forest shade, pools of torchlight, rooms lit through doorways).

| Flag | Range | Default | Effect |
|---|---|---|---|
| `--pathtrace=` | 1..3 | 2 | bounces: more = softer, brighter shade |
| `--pathtrace-blend=` | 0..1 | 1 | 0 = the game's baked look, 1 = fully traced; 0.5 is a half-way look |
| `--pathtrace-exposure=` | EV | 0 | brightness on top of the automatic exposure |
| `--pathtrace-baked=` | 0..1 | 0.3 | how much of the game's baked shading to keep |

- Exposure adjusts by itself, halfway toward the game's brightness, so scenes stay near the original level.
- Caves and dungeons keep the game's fill light (without it they would go black).
- With `--dlss`, Ray Reconstruction cleans it up; with `--post`, the HDR tone map handles the highlights.

### HD textures: `--hd-textures[=DIR]`

The game's textures upscaled 4x by an AI model (Real-ESRGAN), built locally from your own game files into
`~/.cache/ffxi-linux/hd-textures` (or `DIR`); nothing is ever committed. Compression noise disappears; paving,
planks, mortar and bark get sharper; colours match the originals. Grass and soil gain little (their originals are
already soft). Zone, prop, character, gear and effect textures are covered; the interface is not.

```
xi-dat hdtex build --zones 100,230,252     # these zones (or --all)
XI_HDTEX_DUMP=1 xi-client ... --hd-textures   # play: textures the cache lacks are saved
xi-dat hdtex build --pending               # then upscale those (characters, gear, effects)
xi-dat hdtex stats
```

## How the options combine

| Combination | What happens |
|---|---|
| `--rt` + `--rt-boost` | the boost preset applies to the listed effects |
| `--rt-boost` alone | turns on `--rt=all` with the preset |
| `--pathtrace` + `--rt` | the path tracer lights everything; only `--rt`'s water reflections stay from `--rt` |
| `--pathtrace` + `--rt-boost` | as above: the boost has nothing left to change but reflections |
| any ray tracing + `--dlss` | Ray Reconstruction replaces the built-in denoiser; the effects draw after it, at full resolution (not with `--rt` reflections) |
| `--post` + `--dlss` | post effects run after the upscale, on the full-resolution image |
| `--post` + `--pathtrace` | the path tracer hands its highlights to `--post`'s tone map |
| `godrays` + `--rt` | shafts traced against the scene (blocked by real geometry) instead of screen-space |
| `--hd-textures` + `--pathtrace` | the HD textures are the surfaces' colours; their generated surface-detail maps are not used yet |
| `--hd-textures` + anything | independent: works with every other option |

## Recommended combinations

| Name | Flags | Look | GPU cost* |
|---|---|---|---|
| Original | (none) | the game as it was | 0.1-0.3 ms |
| Original, sharp | `--dlss=dlaa` | the same, best anti-aliasing | ~0.9-1.3 ms |
| Subtle | `--rt` | soft occlusion and shadows, clearly FFXI | 1-2.7 ms |
| Enhanced | `--rt-boost=high --post` | deeper shadows, glow, light shafts | 1.4-3.8 ms |
| Dramatic | `--rt-boost=max --post --post-bloom=3 --post-shafts=3 --post-fog=3` | hazy sunlit sky, deep shadows, mirror-like water | 1.4-3.8 ms |
| Remaster | `--dlss --pathtrace --post --hd-textures` | lighting rebuilt from scratch, sharp textures | 1.7-3.4 ms |
| Remaster, half-way | `--dlss --pathtrace --pathtrace-blend=0.5 --post` | between the original and the remaster | ~2-3 ms |

\* RTX 4090, 1600x900 scene, the six comparison scenes; the frame itself stays at the display's 60 Hz in all of them.
Full per-scene tables: [enhanced-graphics.md](enhanced-graphics.md), [raytracing.md](raytracing.md),
[performance.md](performance.md).

### Per-scene GPU cost (ms, p50, from the comparison runs)

| Flags | West Ronfaure | S. San d'Oria | Oubliette | Port Windurst | Norg | Mog House |
|---|---|---|---|---|---|---|
| none | 0.27 | 0.20 | 0.15 | 0.17 | 0.16 | 0.13 |
| `--rt` | 2.27 | 1.86 | 2.72 | 1.87 | 2.02 | 0.98 |
| `--rt-boost=high --post` | 3.83 | 2.08 | 2.51 | 2.53 | 2.75 | 1.39 |
| `--dlss --rt-boost=high --post --hd-textures` | 3.08 | 2.17 | 2.23 | 2.44 | 2.30 | 1.57 |
| `--post` | 0.45 | 0.46 | 0.32 | 0.34 | 0.32 | 0.29 |
| `--hd-textures` | 0.29 | 0.20 | 0.15 | 0.18 | 0.16 | 0.13 |
| `--pathtrace` | 3.96 | 1.81 | 2.94 | 2.21 | 2.28 | 1.80 |
| `--dlss --pathtrace` | 2.45 | 1.83 | 2.03 | 1.90 | 1.92 | 1.54 |
| `--pathtrace --post` | 5.57 | 2.43 | 3.70 | 3.15 | 3.07 | 1.96 |
| `--dlss --pathtrace --post --hd-textures` | 3.43 | 2.27 | 2.51 | 2.56 | 2.41 | 1.69 |

Other costs: `--hd-textures` makes zone loads slower (~0.7 s instead of ~0.06 s, decoding the large images) and uses
more video memory (~600 MiB instead of ~200 MiB).

## Where each option shines

| Scene | Best options |
|---|---|
| Forests (West Ronfaure) | `--pathtrace` (dappled canopy shade), `--post` light shafts |
| Cities in daylight (San d'Oria) | `--rt-boost=high` shadows, `--post` haze and bloom on the sky |
| Night | `--rt` with a moderate shadow cap (0.4-0.6), `--pathtrace` lamp-lit doorways |
| Caves and dungeons (Norg, the Oubliette) | `--pathtrace` (pools of torchlight); `--post` bloom on the flames. `--rt-boost` and `--post` alone change little here. |
| Harbours (Port Windurst) | `--rt=reflections` or `--rt-boost`, `--hd-textures` for the docks |
| Interiors (the Mog House) | `--rt` occlusion and contact shadows |

## Debugging and comparing

| Variable / tool | Use |
|---|---|
| `XI_FRAME_TRACE=2` (or `=10`) | per-stage frame and GPU times in the log |
| `XI_RT_DEBUG=reflect` / `reflectcolor` | where the water reflection lands / the reflected colour alone |
| `XI_RT_SKIP=actors,trace,denoise,composite,reflect` | leave parts of `--rt` out (profiling) |
| `XI_DLSS_LOG=1` | NVIDIA's own log (in `~/.cache/ffxi-linux/ngx`) |
| `XI_HDTEX_DUMP=1` | save textures the HD cache doesn't have yet |
| `--validation` | the Vulkan validation layers |
| Comparison tool | a scratch script (not in the repository) that renders the six scenes above with a fixed camera, time and size for any list of flag sets and composes labelled side-by-side grids; ask Claude to run it for a new combination |

## Known limits

- Tree canopies cast solid shadows: the game models them as solid meshes with opaque textures, so there are no leaf
  gaps for light to pass through.
- The tuning values (strengths, the 40 % shadow cap, the path tracer's sky and exposure rules) are choices, not values
  from the game.
- `--hd-textures`' generated surface-detail (normal / roughness) maps are built but not yet used by the lighting.
- No DLSS Frame Generation on Linux.
- With `--dlss` and `--rt` water reflections (also `--rt-boost`), the effects still go through Ray Reconstruction and
  glows look softer than without DLSS (issue #101 covers `--pathtrace` and `--rt` without reflections).
- `--pathtrace` lights white rooms (the Garden of Ru'Hmet) brighter than the game does, so additive glows show less
  contrast there with or without DLSS.
