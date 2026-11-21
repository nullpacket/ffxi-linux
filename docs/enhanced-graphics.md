# Enhanced graphics options (goal 41)

Four independent flags, each off by default (off renders exactly as without it), meant to be combined and compared
side by side. Each section below belongs to one option.

## `--rt-boost[=low|high|max]`: goal 38's lighting turned up

Goal 38's `--rt` keeps the game's look: each traced term only re-weights the game's own lighting, light 0's shadow may
remove at most 40 % of a pixel's light, and the default strength is 0.7. `--rt-boost` is a preset on top of it that
turns everything up. Without `--rt` / `--rt=` on the line it turns on every effect (`--rt=all`); with them it keeps the
listed effects.

| Preset | strength | shadow cap | reflection weight | bounce | AO radius |
|---|---|---|---|---|---|
| goal 38 (`--rt`) | 0.7 | 0.4 | 1.0 | 0.6 | 1.6 yalms |
| `low` | 1.0 | 0.6 | 1.3 | 0.8 | 2.0 |
| `high` (plain `--rt-boost`) | 1.3 | 0.85 | 1.7 | 1.1 | 2.6 |
| `max` | 1.6 | 1.0 | 2.2 | 1.5 | 3.2 |

- **Strength** above 1 extrapolates the traced terms past "fully replace the game's" (the factors are clamped at 0, so
  nothing goes negative): AO darkens corners more, shadows go deeper, the bounce brightens more.
- **Shadow cap** (`--rt-shadow-cap=0..1`, also on its own with `--rt`): the most of a pixel's lighting light 0's shadow
  removes. 1 = full shadows; at night the moon then casts near-black shadows.
- **Reflection weight** (`--rt-reflect=0..3`): scales the water reflection's weight; its ceiling rises with it (0.75 x
  the scale, at most 0.95).
- **Bounce** and **AO radius** follow the preset only.
- `--rt-strength` given explicitly wins over the preset's strength. The chosen values are logged
  (`rt: strength 1.30, shadow cap 0.85, ...`).
- Code: `gfx::applyRtBoost` and `RtOptions::shadowCap / reflect / bounce / aoRadius` (src/gfx/raytracing.*), passed to
  the shaders in `Params.tune` (rt_common.glsl). With the defaults the shaders compute exactly what goal 38's did.

## `--post[=hdr,bloom,godrays,fog,all]`: HDR, bloom, light shafts, fog

Post-processing of the finished scene, before the interface (the interface is never touched). `--post` alone = all.

### How it works (src/gfx/post.*, shaders/post_*)

The game draws into an 8-bit, gamma-encoded target (the swap chain format; context.cpp) and its shaders clamp, so
there is no real HDR scene to start from, and making one would change every scene pipeline. Instead the passes:

1. copy the scene into a half-float working buffer (a blit);
2. lift it to linear HDR through the exact inverse of an ACES filmic tone map (Narkowicz's fit, after gamma 2.2 to
   linear; white comes back as about 3.4);
3. add light there: the volumetric in-scatter (fog, light shafts) and bloom;
4. tone map back with the same ACES curve and re-encode gamma; copy into the scene.

With nothing added the round trip returns the same image, so `--post=hdr` alone keeps today's look (it is the
pipeline the other effects run in, and they imply it). All passes are compute shaders over images in GENERAL layout.

- **Bloom:** the bright part of the HDR scene (soft knee from 1.8, i.e. near-white pixels: lamps, sky, glints on
  water) goes down a 5-level half-resolution chain (4 bilinear taps a level) and back up with a 3x3 tent, added at
  0.14.
- **Light shafts (`godrays`):** with `--rt`, a half-resolution march (24 steps from the camera to the pixel or the
  fog end) tests light 0 (sun / moon) against `--rt`'s TLAS at every step with a ray query, so trees, arches and walls
  cut the scattered light into shafts (Henyey-Greenstein phase, g = 0.4; denser low down). Without `--rt`, a
  screen-space radial blur of the bright open sky toward light 0's screen position (48 samples, decay 0.965), faded as
  the light leaves the screen.
- **Fog:** the same march adds in-scattered light in the zone's fog colour with a little extinction, thicker close to
  the ground, on top of the game's own linear fog. Densities are per yalm scaled by the zone's fog end, so a dungeon and
  a field read alike.
- **Strength:** `--post-bloom=S`, `--post-shafts=S`, `--post-fog=S` (0..5) multiply each effect's default (bloom 0.14,
  shafts and fog 1). The defaults are mild; 3 gives the drastic look (haze over the city's sky, thick shafts).
- **With `--dlss`:** the post passes run on DLSS's window-size output, after the upscale, so DLSS gets the scene before
  any bloom or fog (NVIDIA's guidance: post effects after DLSS); the depth they read is at the render size.
- Without `--post` none of it exists (no images, pipelines or passes).

## `--hd-textures[=DIR]`: AI-upscaled textures

The game's textures (mostly 64-256 texels, DXT-compressed) upscaled 4x by a local AI model, built on this machine from
the user's own DATs into a cache outside the repository. Nothing of the cache is ever committed.

### Using it

```
xi-dat hdtex build --zones 100,230,252   # zone textures for those zones (or --all)
xi-client ... --hd-textures              # the default cache; --hd-textures=DIR for another
XI_HDTEX_DUMP=1 xi-client ... --hd-textures   # also save every loaded texture the cache lacks
xi-dat hdtex build --pending             # then upscale those (characters, gear, effects, furniture)
xi-dat hdtex stats
```

Builder options: `--files ID,...` (any DAT's textures), `--scale 4|2|1`, `--max N` (largest side, default 1024),
`--model NAME`, `--no-maps`, `--force`, `--dir DIR`, `--upscaler PATH`.

### The upscaler

[Real-ESRGAN ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN) (v0.2.5.0 release binary; runs on the GPU through
Vulkan, no Python or root needed). Install once:

```
mkdir -p ~/.cache/ffxi-linux/hd-textures/tools && cd ~/.cache/ffxi-linux/hd-textures/tools && curl -L -o r.zip https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip && unzip -d realesrgan r.zip && chmod +x realesrgan/realesrgan-ncnn-vulkan && rm r.zip
```

Of its three models, `realesr-animevideov3-x4` (the default) keeps the most of the originals' grain; `realesrgan-x4plus`
smooths grass and soil into flat colour, and `realesrgan-x4plus-anime` blotches tiled ground (compared in West
Ronfaure and Southern San d'Oria).

### How a texture is built

- **Key:** the texture's name plus a 64-bit FNV-1a hash of its size, format and data (`dat::hd::key`): the same picture
  in two DATs shares one entry, an updated texture gets a new one. Entries are
  `<cache>/tex/<last 2 hex>/<key>.png` with `_n.png` / `_r.png` beside them.
- **Tiling:** each texture goes to the model wrapped by 16 texels of itself on every side and is cropped back after,
  so the edges of tiling textures meet without seams.
- **Colour:** the model shifts brightness and flattens contrast a little, so each channel's mean and spread are matched
  back to the original's.
- **Alpha:** not given to the model. A cut-out (two alpha values, e.g. leaves at 0 / 0x80) is resized bilinearly and
  snapped back to its two values, so edges stay crisp; graded alpha is resized smoothly.
- **Size:** 4x, then halved until the largest side is at most `--max` (never below the original).
- **Maps:** a tangent-space normal map from the luminance as height (Sobel, strength 2) and a roughness map (busier and
  darker = rougher). Both are INFERRED heuristics, for `--rt` / `--pathtrace` to use: `TextureSet::hdKey(i)` names a
  bound texture's entry and `dat::hd::load(dir, key, "_n")` reads its map.
- Textures under 8x8 (flat fills) are left as they are.

### At run time

`TextureSet::decode` (every zone, prop, character, gear and effect texture) takes the cached copy when there is one,
else the original; UVs are unchanged, mips are built on the GPU as before. The interface (UiRenderer) is not touched.
Without the flag nothing is read and the image matches main exactly (mean difference 0.03 / 255 in Southern San d'Oria
at a fixed 12:00, clear weather; run-to-run noise is about 1-2).

### Results and costs

RTX 4090, 1600x900, BotC in Southern San d'Oria (live) and offline xi-viewer shots:

| | off | `--hd-textures` |
|---|---|---|
| Southern San d'Oria zone load | 49-73 ms | 715-762 ms (PNG decode of 4x textures) |
| Client VRAM (nvidia-smi) | 199 MiB | 603 MiB |
| Cache: 6 test zones + what a San d'Oria login showed (characters, gear, effects) | | 290 textures + 580 maps, 549 MB |
| Build time | | about 0.27 s a texture (53 s for the 194 textures of five zones) |

What it changes: DXT block noise disappears and edges (paving joints, planks, mortar, bark) are sharper; colours match
the originals. Organic textures (grass, soil) gain little: their originals are already soft, and the models cannot add
real detail there.

## Comparing the options

The comparison tool (scratch, not in the repository) renders fixed scenes for any list of flag sets and composes
labelled grids. It logs BotD in once per flag set, moves it with `!pos` / `!zone`, places the camera with the test
key `camat:YAW;PITCH;DIST` (new: a fixed view of the character, world yaw in degrees), fixes the time with `--time` and
the scene size with `--bg-res 1600x900`, and shoots. Scenes: West Ronfaure, Southern San d'Oria, Bostaunieux
Oubliette, Port Windurst's dock, Norg, the Mog House.

The tool also reads each run's frame trace (`XI_FRAME_TRACE=2`) and records the GPU time p50 of the 2-second report
just before each shot. Grids of the run below (left to right, top to bottom: the flag sets in the table) are in the
scratchpad, not in the repository.

GPU ms per frame, RTX 4090, scene 1600x900 (`--bg-res`), Vana'diel 16:00, BotD (each run's report noise is about
+-0.5 ms with `--rt`: a BLAS rebuild or an NPC walking in can land in the window):

| Flags | West Ronfaure | S. San d'Oria | Oubliette | Port Windurst | Norg | Mog House |
|---|---|---|---|---|---|---|
| none | 0.27 | 0.20 | 0.15 | 0.17 | 0.16 | 0.13 |
| `--rt` | 2.27 | 1.86 | 2.72 | 1.87 | 2.02 | 0.98 |
| `--rt-boost=low` | 4.66 | 2.75 | 3.14 | 0.97 | 1.77 | 1.19 |
| `--rt-boost=high` | 2.38 | 1.42 | 1.73 | 1.58 | 1.77 | 1.20 |
| `--rt-boost=max` | 2.40 | 1.99 | 3.10 | 2.17 | 1.75 | 1.20 |
| `--post=hdr` | 0.38 | 0.28 | 0.23 | 0.25 | 0.24 | 0.21 |
| `--post=bloom` | 0.40 | 0.33 | 0.29 | 0.29 | 0.29 | 0.19 |
| `--post=godrays` (screen-space) | 0.37 | 0.27 | 0.25 | 0.27 | 0.26 | 0.23 |
| `--post=fog` | 0.39 | 0.30 | 0.26 | 0.28 | 0.25 | 0.23 |
| `--post` | 0.45 | 0.46 | 0.32 | 0.34 | 0.32 | 0.29 |
| `--hd-textures` | 0.29 | 0.20 | 0.15 | 0.18 | 0.16 | 0.13 |
| `--rt-boost=high --post` | 3.83 | 2.08 | 2.51 | 2.53 | 2.75 | 1.39 |
| `--rt-boost=high --post --hd-textures` | 4.08 | 2.07 | 2.49 | 2.54 | 2.49 | 1.39 |
| `--dlss --rt-boost=high --post --hd-textures` | 3.08 | 2.17 | 2.23 | 2.44 | 2.30 | 1.57 |

- The boost presets cost the same as `--rt=all` (they change weights, not work): the spread between them is the
  report noise. `--post` costs about 0.15-0.3 ms; with `--rt` its ray-queried shafts add about 0.4-0.8 ms more.
- What changes on screen: `--rt-boost` darkens the ground under trees and the shadows of characters and walls
  (high / max: clearly); `--post` at its defaults is mild (a warm haze in fog, a glow on the brightest sky and torches);
  `--rt-boost=max --post --post-bloom=3 --post-shafts=3 --post-fog=3` gives the most drastic result (hazy sun-lit sky
  over San d'Oria, deep shadows, a mirror-like Port Windurst canal).

## `--pathtrace[=1..3]`: path-traced lighting instead of the baked

The zones' baked shading is set aside and every pixel is lit by tracing rays: the sun / moon (the game's light 0, and
light 1) with shadow rays, the sky through whatever gaps there are, the zone's point lights (lamps, fires) with shadow
rays, and 1-3 diffuse bounces (default 2). RTX Remix in spirit: the game's colours, light directions and fog stay;
what changes is where the light reaches. Built on goal 38's geometry (`RayTracing`'s BLASes / TLAS, made for it with no
`--rt` effects); src/gfx/pathtrace.*, shaders/pt_*.

```
xi-client ... --pathtrace                    # 2 bounces
xi-client ... --pathtrace=3                  # 1..3 bounces
xi-client ... --pathtrace --pathtrace-blend=0.5     # half the game's baked look, half traced
xi-client ... --pathtrace --pathtrace-exposure=0.5  # EV on top of the automatic exposure
xi-client ... --pathtrace --pathtrace-baked=0.3     # how much of the vertex colours' baked shade stays (0..1)
xi-client ... --dlss --pathtrace             # DLSS Ray Reconstruction denoises (docs/dlss.md)
```

Without the flag nothing of it exists (Southern San d'Oria 13:00, clear weather: mean difference 0.02 / 255 against
main's build). With `--rt` as well, only `--rt`'s water reflections stay (the path tracer lights everything else).

### What the vertex colours hold

The game lights a zone surface with `2 x tex x clamp(vc x (ambient + light0 x N.L0 + light1 x N.L1 + points))`
(mesh.frag, dat-format §6; vc 0x80 = 0.5). `xi-dat vcstats ZONE...` measures the vertex colours:

| Zone | luma mean (1 = 0x80) | floors / walls | chroma | spread in a piece | in 0.75..1.0 |
|---|---|---|---|---|---|
| 100 West Ronfaure | 0.934 | 0.959 / 0.948 | 0.004 | 0.079 | 84.8 % |
| 230 Southern San d'Oria | 0.931 | 0.946 / 0.951 | 0.002 | 0.057 | 89.6 % |
| 231 Northern San d'Oria | 0.894 | 0.918 / 0.887 | 0.012 | 0.093 | 77.7 % |
| 167 Bostaunieux Oubliette | 0.925 | 0.809 / 0.971 | 0.000 | 0.085 | 49.4 % |
| 236 Port Bastok | 0.956 | 0.967 / 0.962 | 0.003 | 0.034 | 88.1 % |
| 240 Port Windurst | 0.822 | 0.849 / 0.788 | 0.008 | 0.108 | 63.8 % |

Grey (chroma under 0.02), mostly at 0x80, darker in corners and undersides and not by facing (floors and walls alike):
baked shade and occlusion, not a tint and not sunlight. So the albedo divides the vertex colour's luminance out and
keeps its hue: `material = tex x vc / luma(vc) x mix(1, luma(vc), baked)` (baked = `--pathtrace-baked`, 0.3), the
traced light then supplying the shade. Vertex colours brighter than 0x80 (glowing pieces) stay as they are. Characters
and props without vertex colours keep the drawn colour (below).

### The light

Per pixel, one path a frame (pt_trace.comp):

- **The visible surface:** a primary ray to the depth buffer's point. Where it hits within 2 % (+0.08) of the depth, the
  surface's own texture and vertex colour give the material and its triangle the normal. Elsewhere (characters, blended
  surfaces in front of the depth) the drawn colour is kept and scaled by traced / game lighting, under the fog
  ("ratio" mode, as `--rt`'s composite).
- **Direct:** light 0 and light 1 with a shadow ray each (a 0.012 rad cone: soft edges), the game's colours and
  directions for the time of day; the zone's point lights with the game's falloff (mesh.frag), the four strongest with
  a shadow ray. Indoors (the player on a floor the zone marks roofed, as goal 38) the directional lights' shadows fade
  to 20 % and the ambient comes back as the room's fill, darkened by what is within 2 yalms. The same happens where the
  sky hardly reaches at all (caves, dungeons: the share of first bounce rays that escape, averaged over the frame and
  about a second, under 2..12 %): there light 0 is the zone's fill light, not a sun, and shadowing it by the ceiling
  turned the Bostaunieux Oubliette black.
- **Sky:** cosine-sampled rays that escape take the zone's sky gradient (sky.frag's colours, a quarter of their hue)
  at 1.15 x the game's ambient; open ground then gets about the game's ambient, shade only what reaches it.
- **Bounces:** at each hit, direct light again, carried by the hit's material as a linear reflectance (`pow(m, 2.2)`,
  the game's colours being display values), up to 0.9; after the last bounce the game's ambient stands in for the
  rest. A floor of a quarter of the game's ambient everywhere stands in for light the geometry cannot carry (through
  canopies the game models as solid; bounces past the last).
- **Denoise** (pt_denoise.comp): temporal accumulation of the light (reprojected by depth, up to 48 frames, history kept
  while depth, normal and mode agree), then an a-trous filter (steps 1, 2, 4) edge-stopped by depth and normal, wider
  while the history is young. The light is filtered before the material, so textures stay sharp. With `--dlss` and
  Ray Reconstruction the filter is skipped: the noisy result goes to DLSS-RR with the guides (the material as diffuse
  albedo, normal, specular 0.04). The effects then draw over DLSS's output, not into its input (issue #101; see
  dlss.md, "Effects with Ray Reconstruction").
- **Exposure** (pt_expose.comp): one workgroup compares a 16 x 16 grid of traced light with the game's lighting at the
  same pixels and moves the exposure halfway (in log terms) toward the game's, within 0.6..2.2, over about a second: a
  shaded scene stays darker than the game's and a lit one brighter, the overall level near the game's.
- **Composite** (pt_composite.frag): material x light x exposure through a luminance shoulder (unchanged up to 0.75,
  rolling off toward 1 with the hue kept: the game clamps at 1, and a clamp washed sunlit grass to yellow-white), the
  game's linear fog, then `--pathtrace-blend` with the drawn image. Effects, the interface and `--rt`'s water
  reflections go over it. With `--post` the shoulder steps aside (a plain clip at 0.97, the limit of `--post`'s inverse
  ACES): the ACES round trip rolls the highlights off, so the exposure feeds its tone map.
- The characters' ground-shadow decals are not drawn outdoors (they cast traced shadows), as with `--rt` shadows.

### Results (side-by-side images, not in the repository)

- West Ronfaure 13:00 (BotA): the forest floor in the canopy's dappled shade, sunlit patches between; the game's
  version is evenly lit. Mean luma 90.4 -> 79.5 (first try, without the exposure: 30.6, most of the floor black); in
  clear weather 98.6 -> 66.4 with the ambient floor (51.8 without it).
- West Ronfaure 22:00: moonlit dapples under the trees, the clearing soft.
- The comparison tool (BotD, 16:00, 1600x900; flag sets off, `--pathtrace`, `--dlss --pathtrace`, `--pathtrace
  --post`, `--pathtrace --hd-textures`, all four), six scenes:
  - Norg (the torch-lit tunnel at the zone-in): each torch throws a warm pool onto the pillar beside it and the floor,
    the walls between fall into shadow; with `--post` the bloom glows round the flames.
  - Bostaunieux Oubliette: as bright as the game's, with occlusion in the grille's corners and along the corridor
    (before the sky-share rule it was black).
  - The Mog House: contact shadows under the character and in the room's corners, the light pools of the game kept.
  - Port Windurst: the cliffs and the dock shade each other; the water is the game's (an effect).
  - West Ronfaure, Southern San d'Oria: as above.
- Southern San d'Oria 13:00 (BotC): the walls and doorways in their own shade, the tent's and the character's
  shadows on the paving, the open square as bright as the game's (luma 123.4 -> 128.6). A first sky at the gradient's
  full hue turned the square blue; now a quarter of it.
- Southern San d'Oria 21:00: the tent's moon shadow, lamp-lit doorways brighter and warmer.
- `--dlss --pathtrace`: Ray Reconstruction denoises (preset D), the same lighting (luma 138.5 vs 128.6 with the filter).
- `--validation`: no messages besides the old "Location 6" warning (and NGX's own config-file notices with `--dlss`).

### Cost (RTX 4090, Southern San d'Oria 13:00, clear, a 1248 x 666 window, `XI_FRAME_TRACE`, GPU p50 / p99 ms)

| | GPU | record |
|---|---|---|
| off | 0.11 / 0.11 | 0.11 |
| `--pathtrace=1` | 1.25 / 1.62 | 0.48 |
| `--pathtrace` (2) | 2.67 / 2.85 | 0.42 |
| `--pathtrace=3` | 1.47 / 2.18 | 0.40 |
| `--dlss --pathtrace` | 2.82 / 2.85 | 0.57 |

(One run each; NPCs and the camera's height vary between runs, so 2 and 3 bounces overlap: more bounces stop early when
the path's throughput falls under 0.2 %.) The frame stays at the display's 60 Hz.

The comparison tool's scenes (1600x900 render, GPU p50 ms before each shot):

| flags | West Ronfaure | S. San d'Oria | Oubliette | Port Windurst | Norg | Mog House |
|---|---|---|---|---|---|---|
| off | 0.28 | 0.19 | 0.15 | 0.17 | 0.15 | 0.13 |
| `--pathtrace` | 3.96 | 1.81 | 2.94 | 2.21 | 2.28 | 1.80 |
| `--dlss --pathtrace` | 2.45 | 1.83 | 2.03 | 1.90 | 1.92 | 1.54 |
| `--pathtrace --post` | 5.57 | 2.43 | 3.70 | 3.15 | 3.07 | 1.96 |
| `--pathtrace --hd-textures` | 3.87 | 1.85 | 2.95 | 2.24 | 2.30 | 1.74 |
| all four | 3.43 | 2.27 | 2.51 | 2.56 | 2.41 | 1.69 |

The forest costs most (alpha-tested leaves: every candidate hit samples a texture); DLSS traces fewer pixels.

### Not done / INFERRED

- Specular: zone materials carry no specular information and characters keep the game's (ratio mode), so there is no
  traced specular; `--hd-textures`' normal / roughness maps are not used yet (they would need tangent frames and the
  maps' indices in the traced geometry's records).
- Point lights: at the visible surface all of them (the four strongest shadowed); at bounces one picked by its share and
  shadowed, scaling the unshadowed sum. Lamps are lights, not emissive meshes.
- The sky's 1.15 x ambient, its quarter hue, the bounce's `pow(m, 2.2)`, the exposure's half-way rule and range, the
  0.3 baked share: tuning choices, not traced.
- Characters' lighting is re-weighted with the zone's light terms (they use their own environment values).
