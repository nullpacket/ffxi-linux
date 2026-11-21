// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/character.h"
#include "dat/motion.h"
#include "gfx/cloth.h"
#include "gfx/skinned_renderer.h"
#include "gfx/texture_set.h"

#include <glm/glm.hpp>

#include <vector>

namespace xi::gfx
{

// A character asset uploaded for rendering: the model in the SkinnedRenderer and its bind-pose bone matrices
// (game space, column-vector glm form).
struct CharacterModel
{
    uint32_t               model = 0;
    std::vector<glm::mat4> bindWorld;
    float                  height = 1.8f; // bind-pose height (game units), for labels
    float                  scale  = 1.0f; // drawn size about the feet (effect op 0x99, CODE model +0x204)
    dat::Skeleton          skeleton;
    dat::CharacterMotions  motions;
    // PCs: the main weapon's sheath bone and the bone it follows while drawn (dat::drawnWeaponBone), -1 = none.
    int                    weaponBone = -1, drawnWeaponBone = -1;
    ClothModel             cloth; // Os2 class-1 soft bodies (gfx/cloth.h)
};

// A drawn weapon: the main weapon's bone takes the matrix of the bone the battle stance holds in the grip.
inline void holdWeapon(const CharacterModel& m, std::vector<glm::mat4>& palette)
{
    if (m.weaponBone >= 0 && m.drawnWeaponBone >= 0 && static_cast<std::size_t>(std::max(m.weaponBone, m.drawnWeaponBone)) < palette.size())
    {
        palette[static_cast<std::size_t>(m.weaponBone)] = palette[static_cast<std::size_t>(m.drawnWeaponBone)];
    }
}

// Animation state: which motion set and where in it (frames).
struct AnimState
{
    const dat::MotionSet* set   = nullptr; // nullptr = bind pose
    float                 frame = 0.0f;
    const dat::MotionSet* from  = nullptr; // previous motion while cross-fading
    float                 fromFrame = 0.0f;
    float                 blend     = 1.0f; // 1 = fully `set`
};

CharacterModel uploadCharacter(const dat::CharacterAsset& asset, TextureSet& textures, SkinnedRenderer& renderer);

// The same in two halves (goal 25): the CPU work (texture decode, vertex build) on any thread, then the GPU upload
// recorded and submitted without waiting; the model may only be drawn once Context::uploadDone(ticket).
struct CharacterUpload
{
    std::vector<std::string>          textureNames;
    std::vector<TextureSet::Decoded>  textures; // in textureNames order, then the grey fallback
    std::vector<SkinnedVertex>        vertices;
    std::vector<uint32_t>             indices;
    bool                              simulate = false;
};
CharacterUpload prepareCharacterUpload(const dat::CharacterAsset& asset);
CharacterModel  beginCharacterUpload(const dat::CharacterAsset& asset, const CharacterUpload& up, TextureSet& textures, SkinnedRenderer& renderer,
                                     uint64_t& ticket);

// Render-space bone palette for a character standing at `gamePos` facing heading byte `dir` (LSB convention).
void characterPalette(const CharacterModel& m, const glm::vec3& gamePos, float dir, std::vector<glm::mat4>& out, const AnimState& anim = {});
// `dir` as the LSB heading byte (0..255); the float form takes fractional headings (smoothed turning).
inline void characterPalette(const CharacterModel& m, const glm::vec3& gamePos, uint8_t dir, std::vector<glm::mat4>& out, const AnimState& anim = {})
{
    characterPalette(m, gamePos, static_cast<float>(dir), out, anim);
}

} // namespace xi::gfx
