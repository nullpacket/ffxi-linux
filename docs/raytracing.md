# Ray-traced lighting (`--rt`, goal 38)

An optional layer over the game's own lighting: ambient occlusion, sun / moon shadows, zone point-light shadows, one
diffuse bounce and water reflections, traced with Vulkan ray queries and denoised by a small temporal + spatial filter,
or by NVIDIA DLSS Ray Reconstruction when `--dlss` runs in a DLSS build. Without `--rt` nothing of it exists: no
extensions, no acceleration structures, no extra passes; a screenshot matches the build before it (mean difference
0.02 / 255 against main, Southern San d'Oria 13:00).

```
xi-client ... --rt                         # ao + shadows (sun / moon and point lights)
xi-client ... --rt=ao,shadows,gi,reflections
xi-client ... --rt=all --rt-strength=1     # 0..1: how far the traced terms replace the game's (default 0.7)
xi-client ... --dlss --rt                  # in a DLSS build: Ray Reconstruction denoises (docs/dlss.md)
```

Needs `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` and `shaderInt64`; a GPU or
driver without them logs one warning and renders as without the flag.

## How it fits the game's lighting

The game lights a zone surface with D3D fixed-function terms (mesh.frag; dat-format §6 / §9):

    lit = vertex colour x (ambient + light0 x N.L0 + light1 x N.L1 + point lights), then linear fog

The ray-traced pass does not replace that model. After the zone, props and characters are drawn (before effects and the
interface), each pixel's lighting is re-weighted term by term:

    after = ambient x AO + light0 x N.L0 x shadow + light1 x N.L1 + points x their visibility + bounce
    pixel = (after / before) x (pixel under its fog) + the fog part unchanged

The light colours and directions, fog distances and colour, materials and textures all stay the game's; only how much
of each term reaches the pixel changes. The normal comes from the depth buffer.

Where it would stop reading as FFXI, the effect backs off:

- **Indoors** (the player stands on a floor the zone collision marks roofed, the same flag the client uses for wind and
  ambience) the zone's directional light is the room's fill light, not a sun: its traced shadow fades to 20 % over half
  a second, and the game's own ground-shadow decals come back under the characters. Without this the Château
  d'Oraguille darkened by a third (mean luma 125 -> 89).
- **Light 0's shadow** removes at most 40 % of a pixel's lighting. By day the sun's share is about that anyway; at night
  the moon outweighs the dim ambient, and an unlimited shadow went near black where the game's moonlight is soft.
- Default weights: AO 85 %, shadows 90 %, bounce 60 % of what the rays found, all scaled by `--rt-strength`, which
  defaults to 0.7 so plain `--rt` stays subtle: under West Ronfaure's canopy at full strength the forest floor lost a
  quarter of its brightness (mean luma 51 -> 39); at 0.7 it loses 6 % (45.8 -> 43.0) and reads as soft shade.
- Outdoors with shadows on, the characters cast traced shadows and the ground-shadow decals (`kage`, silhouettes) are
  not drawn.

## The geometry (src/gfx/raytracing.cpp)

- **BLAS per mesh** of every `MeshRenderer` drawn this frame: the zone, an event sub-map, ship / prop models
  (`PropLayer`) and Mog House furniture (which is in the zone scene's renderer). Built once, in one batched call on the
  frame they first appear (Southern San d'Oria: 227-234 BLASes, 3.5 MB scratch); freed with their renderer
  (`rtForget` from `~MeshRenderer`). Instances follow the LOD band picked this frame, doors as moved, hidden prop slots
  skipped.
- **Alpha:** sub-meshes the game alpha-tests (names starting `_`, ALPHAREF 0x60) and blended ones (flag 0x8: foliage
  cards, glass, water) are non-opaque: each candidate hit samples the texture alpha (x vertex alpha x 4, as mesh.frag)
  and counts only where it passes (blended: > 0.5). The large tree canopies in San d'Oria are solid opaque meshes with
  opaque textures (treating every piece by its alpha changed the image by 0.24 / 255, `XI_RT_ALL_ALPHA=1`), so their
  shadow is a solid shape: the geometry has no leaf gaps to let light through.
- **Characters:** the instances `SkinnedRenderer::draw` submitted (within 80 yalms) are skinned again by a compute
  pass (rt_skin.comp, the same two-bone rule as skinned.vert) into a per-frame buffer and get a fast-build BLAS each
  frame. Dissolving or see-through characters are left out.
- **TLAS** rebuilt every frame from all instances. Geometry buffers get the acceleration-structure usage flags only with
  `--rt` (`rtGeometryUsage()`); texture-set layouts also allow compute then.

## The passes

1. **rt_trace.comp** (render resolution, 8x8): from each pixel's depth, position and normal; 2 AO rays (cosine, 1.6
   yalms), 1 shadow ray toward light 0 in a 0.025 rad cone, up to 4 point-light shadow rays (as mesh.frag's falloff,
   within range), 1 bounce ray (24 yalms) shaded at the hit as the game lights the zone (ambient + shadowed light 0 +
   light 1, albedo = 2 x texture x vertex colour), and the water reflection: the view ray's nearest blended level
   surface in front of the depth (or, over pixels with no depth, out to the fog's end), one mirror ray shaded the same
   way. Noise: interleaved gradient noise moving each frame.
2. **rt_denoise.comp:** temporal accumulation (reprojected by depth with the previous jittered view-projection; history
   rejected on depth or normal mismatch, reset on zone change / camera cuts), then a 5x5 depth- and normal-aware filter.
   Reflections are a single deterministic ray and skip it.
3. **rt_composite:** copies the scene colour, writes the re-weighted result over it.
4. With `--dlss` and Ray Reconstruction available: no filter; the raw terms are composited, **rt_guides.comp** writes
   the guides (diffuse albedo = the unfogged pixel / its lighting, specular albedo 0.04, normal with roughness 1 packed
   in w) and DLSS-RR (NGX `dlssd`, preset D, HDR flag: it refuses LDR with "HDR Color required") denoises and upscales
   in one step. If NGX refuses the feature the client falls back to Super Resolution and the plain filter. The effects
   draw over DLSS's output (dlss.md, "Effects with Ray Reconstruction"), except with `reflections` (step 5 needs the
   sea the effect pass draws in the scene).

5. **rt_reflect.frag** (after the effect pass): blends the water reflection over the finished scene.

Debug: `XI_RT_SKIP=actors,trace,denoise,composite,reflect` leaves parts out (profiling); `XI_RT_DEBUG=reflect` tints
where the reflection lands (red = its weight), `XI_RT_DEBUG=reflectcolor` shows the reflected colour alone;
`XI_RT_WATER_TRACE=1` logs the large effect batches the water planes are looked for in.

## Water reflections (`--rt=reflections`)

A harbour's or river's water is mostly not zone geometry: the sea is a zone effect (the zone's `sea` Mmbs drawn by
the effect pass, Port Windurst's two 9573-vertex layers at game y 0), so it is in neither the depth nor the traced
geometry. Two sources of water:

- **Blended level zone meshes** (flag 0x8, a normal within 0.9 of up), found along the view ray in the TLAS.
- **Water planes from the effects:** every 15 frames, the effect batches that are flat (height range under 0.15) and
  wide (over 16 yalms each way) give a plane at their height over their XZ box (merged by height, at most 16). The
  view ray hitting a plane inside its box before the opaque depth counts as water. (The collision's water floors are
  no use: in Port Windurst they are the sea bed, 10 below the surface.)

The reflection has to go on after the effects (the sea's layers are drawn over whatever was there), so it is a separate
alpha-blended pass, not part of the composite. Its weight: 0.4 rising with Schlick's Fresnel term (F0 0.02) toward
grazing views (the game's water is a painted sheet; at a real water's 2 % looking down nothing showed), times 0.8 x
strength, at most 0.75. The mirror ray is shaded as the bounce (the hit's texture x the game's ambient + light 0 +
light 1, under the fog); a miss takes the fog colour the game clears to.

## Cost (RTX 4090, 1600x900, Southern San d'Oria 13:00, ~20 characters, `XI_FRAME_TRACE`, GPU p50 / p99)

| `--rt` | GPU ms | record ms |
|---|---|---|
| off | 0.14 / 0.14 | 0.11 |
| ao | 1.19 / 1.24 | 0.44 |
| shadows (+ point lights) | 1.16 / 1.18 | 0.41 |
| ao,shadows (plain `--rt`) | 1.26 / 1.28 | 0.38 |
| gi | 1.28 / 1.30 | 0.42 |
| reflections | 1.13 / 1.15 | 0.40 |
| all | 1.53 / 1.55 | 0.38 |

(2026-10-21 re-measured: off, ao,shadows, reflections, all with BotD; the water-plane scan every frame had cost up to
5 ms of recording, every 15 frames it is lost in the noise.)

Most of it is fixed (shadows only, by `XI_RT_SKIP`): the characters' per-frame BLASes ~0.3 ms, the trace ~0.3 ms, the
filter ~0.2 ms, the composite ~0.04 ms, TLAS + the offscreen copy ~0.2 ms. 4 AO rays cost ~0.8 ms more than 2. The
frame stays at the display's 60 Hz.

## Seen live (side-by-side images, not in the repository)

- Southern San d'Oria 13:00 (BotC): tree and character shadows on the grass and paving; the rest unchanged.
- The same at 22:00: moonlit shadows, soft.
- Château d'Oraguille (BotD): corner and floor-wall occlusion, the room's light kept.
- Bostaunieux Oubliette: occlusion in the corridor, a faint bounce on the walls.
- Port Bastok harbour stairs: the harbour wall's shadow over the stairs.
- Port Windurst dock (BotD at (62.184, -5, 137.657)), 2026-10-21: the canal's water mirrors the dock's underside, the
  bridge and the waterfall's foot (`XI_RT_DEBUG=reflectcolor` shows the mirrored scene; blended it reads as a gentle
  sheen, strongest toward the far end). Before the fix nothing showed: the sea is an effect, and the reflection was
  composited before the effect pass drew the sea over it.
- BotD's Mog House (zone 230, entered by its `zmr0` line): with `--rt=all` occlusion in the room's corners and
  under the fireplace, the bounce brightening the walls a little (mean luma 58.5 -> 61.9), the character's contact
  shadow; the indoor fade keeps the room's light. Placed furniture was not tested (none placed; its BLASes come from the
  same zone-scene renderer).
- West Ronfaure in rain (`!setweather rain`), day: canopy shade and contact shadows; at the default strength 0.7 the
  scene's mean luma 45.8 -> 43.0.
- `--dlss --rt`: Ray Reconstruction 1067x600 -> 1600x900, the same shadows, no validation messages.
- `--validation` in every configuration: no messages besides the old "Location 6" warning.

## Not done / INFERRED

- Reflections: only water (blended level zone meshes and the sea effects' planes); zone materials carry no specular
  information, characters' specular stays the game's. Effects drawn in front of the water (splashes) are blended over by
  the reflection.
- The bounce is one diffuse bounce; its strength (60 %) is a choice, as are the AO radius, the 40 % shadow cap, the
  default strength 0.7 and the water's 0.4 minimum weight.
- Ray Reconstruction's matrices are passed in glm's column-major order (the SDK names no convention for Vulkan:
  INFERRED); the guides' albedo is derived from the game's lighting, not a G-buffer.
- Characters' lighting is re-weighted with the zone's light terms (characters use their own environment values).
