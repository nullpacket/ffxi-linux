// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/dat_index.h"
#include "dat/texture.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xi::dat
{

// Character models (docs/dat-format.md §10). Game space: Y down, characters face +x in the bind pose.

// D3D row-vector 4x4 (world = [x y z 1] * m), row-major.
using RowMatrix = std::array<float, 16>;

struct Skeleton
{
    struct Bone
    {
        uint8_t              parent = 0;
        std::array<float, 4> rotation{}; // quaternion x, y, z, w
        std::array<float, 3> translation{};
    };
    std::vector<Bone>      bones;
    std::vector<RowMatrix> bindWorld; // bone -> model, bind pose
    // Attach points (effect-format §C.6): after the bones, u16 count (128), u16 0xFFFF, then 26-byte records
    // {s16 bone, f32 rx, ry, rz, f32 offset xyz}. The point is the offset in that bone's posed frame (rotations are 0
    // in all data seen).
    struct Point
    {
        int16_t              bone = 0;
        std::array<float, 3> offset{};
    };
    std::vector<Point> points;
    // Head look-at limits (CODE 0x1002b070, table 0x100351a0): joint 0 = head (point 3), 1 = neck (point 7); {x, y, z}:
    // the target direction projected on the plane z must stay inside the ellipse (x, y). {0, 0} = the joint does not turn.
    std::array<std::array<float, 3>, 2> lookLimits{};
    // Bounds sets (CODE 0x10035180: right after the attach points, the look limits at +0x48): three {f32 top, bottom,
    // x max, x min, z max, z min} (Y down; Hume M: standing -1.9 / 0, riding -2.1 / 0, lying -0.6 / 0). The camera's
    // model height is bottom - top of set 0, or of the set the state picks while riding (CODE 0x1001f2c2 / 0x1001f317 ->
    // 0x100cfa70: chocobo 5 / mount 0x55 -> set 1; also dead 2 / 3 -> set 2).
    std::array<std::array<float, 6>, 3> bounds{};
    [[nodiscard]] float boundsHeight(std::size_t set) const
    {
        return set < bounds.size() ? bounds[set][1] - bounds[set][0] : 0.0f;
    }
    // Soft-body collision spheres (CODE 0x1002b2c0, table 0x100351c0 = look limits + 0x24): 0x12-byte records {s16 bone,
    // f32 radius, f32 offset xyz}, the centre the offset in that bone's posed frame; indexed by a cloth mesh's sphere ids.
    // Only NPC models with soft bodies have them (PC skeletons pad with 0xCD).
    struct Sphere
    {
        int16_t              bone   = 0;
        float                radius = 0.0f;
        std::array<float, 3> offset{};
    };
    std::vector<Sphere> spheres;

    static Skeleton parse(std::span<const std::byte> sk2);
};

// One expanded triangle corner: up to two bone-local positions (vertices are stored per influencing bone).
struct CharVertex
{
    float   p0[3]{}, p1[3]{};
    float   n0[3]{}, n1[3]{};
    float   uv[2]{};
    uint8_t bone0 = 0, bone1 = 0;
    float   weight1 = 0.0f; // weight of bone1 (bone0 gets 1 - weight1); p0/p1 and n0/n1 are stored pre-weighted
    int16_t cloth   = -1;   // Os2 class 1: the asset-wide cloth particle this corner sits on (CharacterAsset::cloth), -1 = none
};

// An Os2 class-1 soft body (hair, cape, skirt; dat-format §10.3, CODE 0x100231c0 / 0x10023630): anchored particles
// follow their bones, the rest are Verlet particles joined by springs.
struct ClothMesh
{
    struct Particle
    {
        uint8_t bone0 = 0, bone1 = 0;
        float   p0[3]{}, p1[3]{}; // bind positions, pre-weighted like CharVertex
        float   weight1 = 0.0f;
        bool    anchor  = false; // skinned every frame; the rest simulate
    };
    struct Spring
    {
        uint16_t a = 0, b = 0; // particle indices within this mesh
        float    rest = 0.0f;
        bool     pinA = false, pinB = false; // that end is not moved (index bit 15)
        uint8_t  set  = 0;                   // 0 / 1: which stiffness
    };
    std::vector<Particle>                particles;
    std::vector<Spring>                  springs;
    std::vector<std::array<uint16_t, 3>> triangles; // for the normals (clockwise fronts, as §10.3)
    float    stiffness[2]{ 0.5f, 0.5f };
    float    damping = 0.9f, gravity = 0.0f, inertia = 0.0f;
    std::vector<uint16_t> spheres; // the skeleton's collision spheres this mesh meets (+0x4c word offset, +0x50 count)
    uint16_t first   = 0; // this mesh's first asset-wide particle index
};

struct CharacterAsset
{
    Skeleton                skeleton;
    std::vector<CharVertex> vertices; // triangle list, clockwise front (game space)
    struct Part
    {
        std::string texture; // 16-char name; empty = untextured
        uint32_t    first = 0;
        uint32_t    count = 0;
        int8_t      wep   = -1; // from an Os2 chunk named wepN: N (effect ops 0x75 / 0xA3 show / hide these per actor)
        // The 0x8010 material in force (dat-format §10.3; CODE 0x1002e02c..0x1002e3f9): lighting scale on the lights and
        // ambient; environment reflection (the actor's `evev` texture by camera-space normal); specular highlight (the
        // global "cubemap spec" texture by reflection vector); strength = +0x24 x 0.5 / 255.
        float       lightScale = 1.0f;
        bool        env        = false;
        bool        spec       = false;
        float       strength   = 0.0f;
    };
    std::vector<Part>    parts;
    std::vector<ClothMesh> cloth;    // Os2 class-1 soft bodies, particles numbered asset-wide in order
    uint16_t               clothParticles = 0;
    std::vector<Texture> textures;    // every texture chunk found in the model files
    std::string          envTexture;  // name of the first texture in a chunk named `evev` (env reflections), or empty
    std::string          specTexture; // name of the global "cubemap spec" texture (file 0) when a part uses it
    std::vector<uint32_t> motionFiles; // files holding this skeleton's motions (race base + 4, or the NPC file)
    // PCs: the weapon class's battle motions (btl / at* / in* / ot*), §10.6. Empty for NPCs, whose model file has them.
    std::vector<uint32_t> battleMotionFiles;
    int                   weaponClass = -1;
    int                   sheathStyle = 0;
    uint16_t              hiddenGroups = 0; // Os2 hide groups the worn meshes switch off (bit n = group n; §10.3)  // the main weapon's `info` byte 4: its draw / sheathe motions (§10.6)
    // Footsteps (sound-format.md): the footwear sound code 1..27 and variant 0..2 from the feet-slot model's type 0x45
    // chunk (+0x31 / +0x32), else the skeleton's (+0x31 / +0x32); -1 = none. footLayout 2 = quadruped (skeleton +0x30).
    int8_t                shoe = -1, shoeVariant = -1;
    // The body model's `info` byte 9 (0 none, 1 or 2; dat-format §10.8): picks piece-2 (cloth) motion files.
    uint8_t               bodyVariant = 0;
    uint8_t               footLayout = 0;
    // PCs: the main weapon's model file (its `ef h` hit spark / `se h` impact sound schedulers, effect-format §Melee).
    std::vector<uint32_t> weaponFiles;
    // PCs: the face and armour model files (head..feet). Their schedulers are looked up after the weapon's: the face
    // holds the voices (`vatk`, `vdam`, `vswy`, `vded`), the body the impact sound `sdam` (effect-format §Melee).
    std::vector<uint32_t> gearFiles;
    int                   rangedClass = -1; // ranged model `info` byte 14 (effect-format §C.5), -1 = none
    // PCs: the skeleton bone the main weapon's mesh hangs from (its sheath slot, an attachment bone; = the `info`
    // byte 6 slot, DATA), -1 = none. See dat::drawnWeaponBone for where it goes while drawn.
    int                   mainWeaponBone = -1;
    // NPC model files with no mesh of their own build the look from their `init` scheduler (EffectLook): that file
    // (its `init`, `cate` ... run on the actor) and the size it sets (op 0x99, 1 = the model's own).
    std::vector<uint32_t> effectFiles;
    float                 scale = 1.0f;
};

// An NPC model file that is only effects (e.g. model 361 = file 1661, block `mith`): its `init` scheduler dresses the
// actor through the scheduler's actor ops (effect-format "Coverage, part 7A"; CODE 0x1005c15b..0x1005c39b -> actor
// vtable 0x1032d710 +0xd8..+0x10c): 0x8D the race (model +0xef), 0x8E..0x96 the look slots face, head, body, hands,
// legs, feet, main, sub, ranged (model +0xfc words; the getter ORs slot << 12), 0x98 slot 0 = another NPC model id
// (an alias, e.g. 2060 `ura_` -> model 790), 0x99 the model scale (model +0x204).
struct EffectLook
{
    int                     race = -1; // -1 = not a PC look
    std::array<uint16_t, 9> slots{};   // face, head, body, hands, legs, feet, main, sub, ranged
    int                     model = -1; // op 0x98's NPC model id, -1 = none
    float                   scale = 1.0f;
};
std::optional<EffectLook> effectLook(const DatIndex& index, uint32_t fileId);

// Appearance → asset. PCs and "equipped" NPCs: race (1..8) / face and the slot look ids head, body, hands, legs, feet,
// main, sub, ranged (S2C 0x00D GrapIDTbl[1..8]). Weapons hang from attachment bones posed by the motions (§10).
std::optional<CharacterAsset> loadPcAsset(const DatIndex& index, uint8_t race, uint8_t face, const std::array<uint16_t, 8>& gear);
// What a mount adds to its rider's camera height: its model file's `info` chunk (type 0x45) byte 14 x 0.01, 0 for 0xFF
// (CODE 0x100cf281 -> actor +0x89d, read by 0x100222e0 for the camera 0x1001f34b; byte 15 is the model scale, +0x89c).
// DATA: chocobos 39275 / 39276 and most mounts have none; 102717 1.5, 102724 1.3, 102726 1.6, 102728 / 102729 1.4,
// 102736 2.0.
float mountCameraHeight(const DatIndex& index, uint32_t fileId);
// NPCs/monsters with a single model id (S2C 0x00E look kind 0).
std::optional<CharacterAsset> loadNpcAsset(const DatIndex& index, uint16_t modelId);
// A one-file skinned model by DAT id (NPC format: skeleton, meshes, motions), e.g. the Mog House statues (item + 51183).
std::optional<CharacterAsset> loadModelFile(const DatIndex& index, uint32_t fileId);

} // namespace xi::dat
