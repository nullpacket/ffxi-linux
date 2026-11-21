# AMD and Intel GPUs: FSR 3.1 and vendor-neutral graphics

Issue #139: every graphics improvement works on AMD and Intel GPUs, not only on NVIDIA. There is no AMD or Intel
hardware on the development machine, so the code queries what it uses instead of assuming NVIDIA, a test mode makes
the NVIDIA GPU behave like an AMD one, and AMD is listed as **supported, untested on hardware** until someone reports
in (the checklist at the end).

## What works where

| Feature | NVIDIA RTX | AMD | Intel |
|---|---|---|---|
| The game's own look | any Vulkan 1.3 GPU | any Vulkan 1.3 GPU (RADV, AMDVLK) | any Vulkan 1.3 GPU (ANV) |
| `--rt`, `--rt-boost`, `--pathtrace` | RTX 20 and newer | RX 6000 (RDNA2) and newer | Arc and newer |
| `--post` | yes | yes | yes |
| `--hd-textures` | yes | yes (the upscaler is Vulkan, `realesrgan-ncnn-vulkan`) | yes |
| `--dlss` (DLSS 4, Ray Reconstruction) | RTX, DLSS build | no: falls back to FSR | no: falls back to FSR |
| `--fsr` (FSR 3.1) | yes | yes | yes |
| Ray tracing denoiser | DLSS Ray Reconstruction with `--dlss`, else the client's own | the client's own | the client's own |

AMD and Intel are untested on real hardware: they are covered by the checks below and by the masquerade mode.

## FSR 3.1: `--fsr[=MODE]`

AMD FidelityFX Super Resolution 3.1 (the upscaler, not frame generation), through the FidelityFX SDK's own Vulkan
back end. It takes the same inputs as DLSS (the scene's colour and reverse-Z depth at the render size, the motion
vectors, the jitter) and writes the window-size image the interface draws over, so everything after it (the effects
for Ray Reconstruction's path, `--post`) is the same as with DLSS.

| Mode | Renders at (per axis) |
|---|---|
| `quality` (plain `--fsr`) | 1 / 1.5 |
| `balanced` | 1 / 1.7 |
| `performance` | 1 / 2 |
| `ultra-performance` | 1 / 3 |
| `native` (or `dlaa`) | 1: anti-aliasing only |

- `--fsr-sharpness=0..1` (default 0.2) is FSR's RCAS sharpening; 0 turns it off.
- `--upscaler=dlss|fsr|auto|off` picks explicitly; `auto` is DLSS on an NVIDIA RTX GPU with a DLSS build, FSR
  elsewhere.
- **`--dlss` on a GPU without DLSS** (AMD, Intel, a build without the SDK) starts FSR with the same mode and logs
  `upscaler: DLSS needs an NVIDIA RTX GPU and a DLSS build: using FSR instead`. So a profile made on an NVIDIA machine
  works unchanged on an AMD one.
- With ray tracing, FSR doesn't denoise: the client's own denoiser runs, as without an upscaler.
- The texture LOD bias follows AMD's guide: log2(render / output) - 1 (`XI_FSR_LOD_BIAS` overrides).
- `XI_FSR_DEBUG=1` turns on the SDK's dispatch checks; its messages go to the log as `fsr: ...`.

### How it is built

`XI_FSR` is on by default. The SDK (MIT, `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`, tag v1.1.4, commit
`c6efa6bf7f20`) is not in this repository:

- `XI_FSR_SDK=DIR` uses a checkout made with `tools/fetch_fsr_sdk.sh DIR` (a sparse clone of what the upscaler
  needs). Without it CMake looks in `~/.local/share/ffxi-linux/fidelityfx-sdk`, then fetches the pinned tag into the
  build folder (`build/_deps/fidelityfx-sdk`) and checks its commit.
- The SDK compiles its shaders with FidelityFX_SC, a Windows-only tool. `tools/fsr_shaders.py` replaces it: it compiles
  the upscaler's GLSL passes with glslangValidator, reads each module's bindings from the SPIR-V, and writes the
  permutation headers the SDK's code includes by name. Only the permutations the client uses are compiled (8-bit
  colour, render-size motion vectors without jitter, inverted depth; with and without the Lanczos table and
  sharpening; 32- and 16-bit), 80 modules.
- `src/gfx/fsr/ffx_linux_compat.h` is force-included into the SDK's sources: the MSVC string functions it calls
  (`wcscpy_s`, `sprintf_s`, `wcstombs_s`), and a bigger opaque context, since the SDK sizes it for Windows' 2-byte
  `wchar_t`.
- Without the SDK (offline at configure time, no glslangValidator) the client builds without FSR; `--fsr` then logs why.

FSR's shaders need `shaderStorageImageExtendedFormats`, `shaderStorageImageWriteWithoutFormat` and quad subgroup
operations in compute shaders; the 16-bit passes `shaderFloat16` and `shaderInt16` (the SDK picks those passes when the
GPU has `shaderFloat16`). The client checks them at start-up and logs which one is missing; INFERRED from the drivers'
published feature lists (gpuinfo.org): current RADV, AMDVLK and ANV report all of them on RDNA and Arc GPUs.

## What the audit changed (issue #139)

The code was read for NVIDIA-only assumptions; what came out:

- **Push constants above 128 bytes.** Vulkan guarantees 128 bytes, and AMD's own driver (AMDVLK and the proprietary
  one) reports exactly 128 (RADV and NVIDIA 256). `--post`'s block was 176 bytes and the motion-vector pass 144:
  the inverse view-projection moved into a small buffer (`post_common.glsl`'s `PostView`), and the motion vectors take
  one precomputed reprojection matrix. Every push block now has a `static_assert` against 128.
- **Host-visible memory without flushes.** Nothing flushes after writing mapped buffers, which is only safe on
  coherent memory (always so on NVIDIA, not on every Intel part): host-visible buffers now ask for
  `HOST_COHERENT` memory.
- **Required features not checked.** The device is created with dynamic rendering, synchronization2, descriptor
  indexing, buffer device addresses, BC textures, multi-draw-indirect and anisotropy; a GPU without one is now passed
  over with the reason (`gpu: NAME lacks ...`) instead of failing `vkCreateDevice`.
- **Ray tracing without `shaderInt64`.** The ray-traced passes use 64-bit buffer addresses; ray tracing now also
  needs `shaderInt64` (and room for the bindless texture arrays) and is turned off with a message otherwise.
- **Push-constant ranges larger than what is pushed** (the ray-traced and path-traced composites declared 16 bytes,
  pushed 8): flagged by the validation layer, now exact.
- Nothing else keys off the vendor: the only vendor test left is DLSS's. No shader uses subgroup operations, all
  formats used are ones Vulkan requires for their use, and the bindless arrays (2 x 4096 per stage) fit every
  vendor's limits.

## Testing without AMD hardware

- **`XI_GPU_PRETEND=amd`** (or `intel`): the client and `xi-client --capabilities` treat the GPU as that vendor's.
  The vendor id changes (so DLSS goes away and `--dlss` falls back to FSR, as on a real AMD card), and the limits the
  client checks against are clamped to an RX 6800's (AMDVLK's 128 push-constant bytes, wave32 to wave64) or an
  Arc A770's. The launcher's Tools page shows when it is on. It can't make the driver enforce those limits or run
  wave64: the FSR SDK reads the subgroup range from the device itself, so its Lanczos-table path (the one AMD GPUs
  take) is compiled but not run here.
- **Validation with AMD's best-practice checks:**
  `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT:VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_AMD`
  with `--validation`. On 2026-11-07, Southern San d'Oria with `XI_GPU_PRETEND=amd --rt=all --post`: no errors. The
  remaining warnings are performance advice (a pipeline bound twice in a frame, `vkCmdClearColorImage`, the size of
  the ray tracer's pipeline layout) and an unused vertex output, not correctness.
- **Mesa lavapipe** (software Vulkan, a non-NVIDIA driver with ray queries in current Mesa) would run the headless
  tests on a second driver; it needs `vulkan-swrast` installed, which this machine doesn't have yet.

## AMD / Intel tester checklist

For someone with an AMD or Intel GPU (please paste the output of 1 and the log lines of the others in an issue):

1. `xi-client --capabilities`: the GPU, `"ray_query"`, `"fsr"` and `"gpu_missing"` (should be empty).
2. Log in with the plain client: the game's look, no warnings starting `gpu:`.
3. `--fsr` and `--fsr=performance`: the log says `fsr: upscaler WxH -> WxH`; walk around, turn the camera (look for
   smearing or shimmering on fences, trees and the interface's edges); a screenshot of each.
4. `--dlss`: the log says it uses FSR instead; the picture is the same as 3.
5. `--rt` (RX 6000 / Arc and newer): `rt: ray queries on`; shadows and occlusion appear; a screenshot in Norg
   (zone 252, many lamps) and West Ronfaure (forest shade).
6. `--rt-boost=high --post` and `--pathtrace --fsr`: screenshots, and the FPS from the F11 overlay.
7. The same with `--validation`: any line starting `vulkan:` with `[error]`.
