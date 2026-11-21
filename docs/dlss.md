# NVIDIA DLSS 4 (goal 15)

`xi-client --dlss[=MODE]` renders the 3D scene through NVIDIA DLSS 4: Super Resolution (an upscale from a lower
render resolution) or DLAA (anti-aliasing at native resolution). The interface is drawn afterwards at the window's own
resolution, so text and windows are never upscaled. Without the flag the client starts and renders exactly as before.
It makes no NGX call, adds no Vulkan extension and creates no extra image (the screenshots and frame times below match
main's within run-to-run noise).

| `--dlss=` | Render size (per axis) | 1600x900 window renders at |
|---|---|---|
| (none) | DLSS off | 1600x900 |
| `dlaa` | 100 % (anti-aliasing only) | 1600x900 |
| `quality` (also plain `--dlss`) | ~67 % | 1067x600 |
| `balanced` | ~58 % | 928x522 |
| `performance` | 50 % | 800x450 |
| `ultra-performance` | ~33 % | 533x300 |

Every mode uses DLSS 4's transformer model (render preset K).

## What runs on Linux

Checked 2026-10-20 against DLSS SDK 310.9.16 (github.com/NVIDIA/DLSS) on an RTX 4090 with driver 615.78.08.

- **NGX on Vulkan works on Linux.** The driver ships the NGX core (`/usr/lib/libnvidia-ngx.so.615.78.08`). The SDK
  ships the static loader `lib/Linux_x86_64/libnvsdk_ngx.a` and the feature libraries `libnvidia-ngx-dlss.so.310.9.16`
  (Super Resolution / DLAA), `libnvidia-ngx-dlssd.so` (Ray Reconstruction) and `libnvidia-ngx-dlssg.so` (Frame
  Generation). The guide (§9.7) says any driver from 22 March 2022 runs current snippets.
- **Streamline** is Windows-only (D3D11 / D3D12 / Vulkan on Windows), so this client uses NGX directly.
- **Super Resolution and DLAA:** done, both live (below).
- **Ray Reconstruction:** not applicable. It denoises ray-traced lighting, and the client has none.
- **Frame Generation / Multi Frame Generation:** the SDK has a Linux `libnvidia-ngx-dlssg.so` and a Vulkan path
  (DLSS-FG Programming Guide). Raw NGX leaves the frame pacing to the application. It would have to present each
  generated frame between real ones on its own timer, and supply a HUD-less colour image, the HUD separately, camera
  data and its own motion and depth inputs. NVIDIA recommends doing this through Streamline, which doesn't exist on
  Linux. Not attempted here; it is the next step if wanted. On this client the GPU work per frame is about 0.2 ms
  (see below), so FG's main win, doubling the frame rate of a GPU-bound game, has little to give.

### What DLSS needs from the renderer, and where it comes from

| Input | Here |
|---|---|
| Colour at the render size | `SceneTarget` (the existing background-resolution target), made `SAMPLED` with `--dlss`. LDR: the game's colours are display-ready, so no HDR flag. |
| Depth | `SceneTarget`'s D32 depth, reverse-Z (`DepthInverted`). Stored instead of discarded with `--dlss`. |
| Sub-pixel jitter | Halton (2, 3), 8 × (output / render)² phases, added to the view-projection (`gfx::jitterViewProj`). Every scene draw uses the jittered matrix; the sky gradient doesn't (it has no edges). |
| Motion vectors | `gfx::MotionVectors`, an R16G16_SFLOAT image at the render size, previous − current position in render pixels (MV scale 1, unjittered, `MVLowRes`). `motion_camera.frag` reprojects every pixel's depth through the previous frame's view-projection; that covers the zone, props and sky. Characters then draw over it (`SkinnedRenderer::drawMotion`, `motion_skinned.*`), skinned with this frame's and the previous frame's bones and depth-tested against the scene. |
| Exposure | `AutoExposure` (LDR input; pre-exposure 1). |
| Reset | On the first frame, a zone change (zone id or scene), and a camera jump over 8 yalms in one frame (event camera cuts, warps). |
| Texture LOD bias | log2(render / output) − 1 (guide §3.5): −1.58 at Quality, −2 at Performance, −1 at DLAA. It applies to every texture set made after start-up; `XI_DLSS_LOD_BIAS` overrides it. |

The output is an RGBA16F storage image at the window size (`GENERAL` while DLSS writes it), blitted into the swap
chain image before the interface pass. If evaluation ever fails, the frame falls back to the usual scaling.

## Building it

DLSS is an optional part of the build: CMake option `XI_DLSS`, off by default. Nothing from NVIDIA is in this
repository.

```
tools/fetch_dlss_sdk.sh ~/src/dlss-sdk
cmake -B build-dlss -DXI_DLSS=ON -DXI_DLSS_SDK=$HOME/src/dlss-sdk
cmake --build build-dlss
build-dlss/xi-client --dlss=quality ...
```

- The script sparse-clones only the headers, `libnvsdk_ngx.a`, the release feature libraries, the licence and the
  programming guide (about 115 MB).
- Linked: `libnvsdk_ngx.a` and `libdl`. The feature library is loaded by NGX at run time. NGX looks in
  `XI_DLSS_LIB_DIR`, then beside the executable, then in the SDK the build was configured with. `cmake --install`
  copies `libnvidia-ngx-dlss.so.*` beside the installed executable.
- Without `XI_DLSS` the code builds with stubs. `--dlss` then logs "this build has no DLSS" and the client renders as
  usual. The same happens on a machine where NGX or Super Resolution is unavailable, such as a non-NVIDIA GPU or an
  old driver: one warning, then normal rendering.
- Debugging: `__NGX_SHOW_INDICATOR=1` shows NVIDIA's on-screen DLSS indicator, `__NGX_LOG_LEVEL=1` (or our
  `XI_DLSS_LOG=1`) turns on NGX's log, and NGX writes its files under `~/.cache/ffxi-linux/ngx`.

### Licence

The SDK is under NVIDIA's RTX SDKs licence (`LICENSE.txt` in the SDK):
- It may be used, and its runtime distributed "as incorporated in object code format into a software application",
  under terms at least as protective as NVIDIA's.
- It may not be distributed as a stand-alone product.
- It may not be made subject to an open-source licence that would require its disclosure or free redistribution.
- Reverse engineering is not allowed.

That is why the SDK is fetched separately and nothing of it is committed. An installed or packaged build that
includes `libnvidia-ngx-dlss.so` is a distribution of NVIDIA's runtime under those terms.

## Measured (2026-10-20, RTX 4090, driver 615.78.08, windowed, vsync 60 Hz)

GPU time is the whole frame's GPU work (timestamps), p50 / p99 in ms, `XI_FRAME_TRACE=10`.
- **West Ronfaure:** BotA at 1600x900, noon, fine weather.
- **Southern San d'Oria:** BotC, with walking NPCs.
- **Full size:** 2496x1404, the largest window the test display allows (the client area is 2496x1368).

| Mode | West Ronfaure 1600x900 | Southern San d'Oria 1600x900 | Southern San d'Oria 2496x1368 |
|---|---|---|---|
| off | 0.23 / 0.28 | 0.15 / 0.17 | 0.23 / 0.24 |
| dlaa | 0.86 / 0.92 | — | 1.27 / 1.76 |
| quality | 0.80 / 0.81 | 0.73 / 0.75 | 1.15 / 1.63 |
| balanced | 0.79 / 0.79 | — | — |
| performance | 0.78 / 0.81 | 0.74 / 0.75 | 1.13 / 1.57 |
| ultra-performance | 0.77 / 0.77 | — | 1.07 / 1.46 |

- **Frame time:** 16.67 ms p50 in every mode; the frame waits for the 60 Hz vsync.
- **CPU:** recording rose 0.3–0.8 ms, for the motion vector passes and NGX's own recording.
- **Validation:** `--validation` reported nothing new. The only message, the vertex/fragment "Location 6" interface
  warning, predates this work.

What this shows: this client's GPU load is tiny (the CPU sets the frame rate; docs/performance.md), so DLSS costs a
fixed ~0.6 ms at 1600x900 and ~0.9 ms at 2496x1368, and rendering fewer pixels saves almost nothing. On this machine
DLSS is an image-quality option (DLAA's anti-aliasing, or Quality's temporal anti-aliasing with a sharper result than
the plain scaling of `--bg-res`), not a speed one. It would pay off on a GPU-bound setup, such as a weak GPU driving
a 4K screen.

**Image checks (screenshots, same spots):**
- **Without the flag:** identical to main within run-to-run noise. The mean difference was 0.02 grey levels to one
  run of main and 1.67 to another, against 1.67 between two runs of this build.
- **Quality and Performance while running:** no trails on BotA's legs, arms or sword.
- **Quality and Performance in Southern San d'Oria:** the walking NPCs and the paving look the same as with DLSS off.
- **Ultra Performance:** upscaled from 533x300, it keeps the character and the grass readable, with softer textures.
- **Zone change** (BotD, `!zone 230` and back): the history resets on both changes, and the first frame in the new
  zone shows no trace of the old one.

## Effects with Ray Reconstruction (issue #101)

With Ray Reconstruction (`--dlss` with `--pathtrace` or `--rt`) the effect batches (spells, abilities, glows, particles,
the Ghrah's element core, the sea) are left out of the scene DLSS gets and draw over its output instead. Ray
Reconstruction takes its input colour as noisy lit surfaces and its guides hold the surface behind an effect (the
white marble of the Garden of Ru'Hmet), so the additive glows, rings and sparkle rays came out smoothed into the
background: Cure's bubbles faded, the Awghrah's core went grey through its plates (seen live, BotD, zone 35 and Norg).

- Order (render.cpp `dlssResolve` / `drawEffectsOver`): the scene without the effects -> DLSS-RR -> its output copied
  into the window image -> the window's depth image filled with the scene's depth scaled up to the window
  (`MotionVectors::drawDepth`, depth_upscale.frag: the render-size depth under each window pixel's unjittered
  position, the farthest of the 2x2 texels so effects on a floor are not cut) -> the effects with the unjittered camera
  at the window size -> `--post` on the result (as without DLSS, where it also runs after the effects).
- The effects are drawn at the window's resolution and skip DLSS's temporal pass, so they don't smear either; their
  edges behind geometry follow the render-size depth (up to one render pixel coarser).
- Not moved: the screen-sampling effects (haze, rings that bend the scene, freeze frames; their copies are at the
  render size) still go through DLSS; `--rt=reflections` (or `--rt-boost`, which turns it on) keeps the old order,
  because the reflection is blended over the sea after the effect pass. Plain DLSS (Super Resolution, no ray tracing)
  keeps the effects in its input as before.

## Limits

- **Effects (particles, beams, glows) and moving props (ship hulls) get the camera's motion only.** Fast effects can
  smear slightly under DLSS. Per-object motion for them would need the effect and prop renderers to keep last frame's
  transforms, as the characters do.
- **Characters are matched to last frame's** by model and nearest root position. If two identical models swap places
  between frames, one frame's motion can be wrong. Dissolving and see-through characters keep the background's motion.
- **The LOD bias is fixed when the texture sets are made.** If NGX turns out to be unavailable at start-up, the
  character textures keep it. Zone textures made later use 0 again.
- **Frame Generation:** see above. Not done.
