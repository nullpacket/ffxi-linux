// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "client/effects.h"
#include "dat/dat_index.h"
#include "dat/texture.h"
#include "gfx/character_model.h"
#include "gfx/skinned_renderer.h"
#include "gfx/zone_scene.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

// The blob shadow under each actor (shadow_re.md; CODE 0x10033cd0): texture `kage` from file 7052 multiplied into the
// frame (ZERO / INVSRCCOLOR). Within 20 of the camera it is a decal over the zone's ground triangles, fading over 2
// units of height difference; farther away a flat quad at the feet.
// Detailed shadows (shadow_re.md §2a mode 1, CODE 0x10185bb0): a silhouette of the model, flattened along the zone
// light into a cell of a render texture (SkinnedRenderer::drawSilhouettes), projected onto the same ground triangles
// instead of `kage`. The game gives it to every actor within 40 of the camera with Config shadows "detailed" (cfg 58 =
// 2), and to the local player in the default "simple" mode (0).
namespace xi::app
{

class GroundShadows
{
public:
    explicit GroundShadows(const dat::DatIndex& index);

    void setEnabled(bool on)
    {
        enabled_ = on;
    }
    // Config shadows: 2 detailed, 0 simple, 1 none.
    void setMode(int cfg58)
    {
        mode_    = cfg58;
        enabled_ = cfg58 != 1;
    }
    // The zone light (render space, toward the light; SceneParams::light0Dir), for the silhouettes.
    void setLight(const glm::vec3& towardLight)
    {
        light_ = towardLight;
    }
    // Start of a frame.
    void clear()
    {
        casts_.clear();
    }
    // An actor drawn this frame: its feet (render space), model and visibility (1 = opaque); `instance` = its index in
    // the frame's SkinnedRenderer instances (for a silhouette), `player` = the local player.
    void cast(const glm::vec3& feet, const gfx::CharacterModel& model, float alpha = 1.0f, std::size_t instance = SIZE_MAX, bool player = false);
    // Geometry for this frame's casts, appended as Multiply batches; also picks this frame's silhouettes.
    void build(const gfx::ZoneScene* scene, const glm::vec3& camera, std::vector<client::EffectSystem::Batch>& out);
    // The silhouettes build() picked, for SkinnedRenderer::drawSilhouettes into the texture keyed silhouetteKey().
    [[nodiscard]] const std::vector<gfx::SkinnedRenderer::SilhouetteJob>& silhouettes() const
    {
        return jobs_;
    }
    [[nodiscard]] const dat::Texture* silhouetteKey() const
    {
        return &silhouetteKey_;
    }
    static constexpr uint32_t kAtlas = 1024, kCell = 128; // the silhouette texture: 8 x 8 cells

    // Half the width of an actor's shadow (render units) for a model.
    [[nodiscard]] static float halfSize(const gfx::CharacterModel& model);
    // A silhouette's flattening (render space): along `towardLight` onto the plane through `feet`, then the square of
    // half-width `h` around the feet to clip x / y (x -> x, z -> y).
    [[nodiscard]] static glm::mat4 silhouetteMatrix(const glm::vec3& feet, const glm::vec3& towardLight, float h);

private:
    struct Cast
    {
        glm::vec3   feet;
        float       s;
        float       alpha;
        std::size_t instance = SIZE_MAX;
        bool        player   = false;
        float       height   = 0.0f;
    };
    std::optional<dat::Texture> texture_;
    std::vector<Cast>           casts_;
    bool                        enabled_ = true;
    int                         mode_    = 0;
    glm::vec3                   light_{ 0.0f, 1.0f, 0.0f };
    dat::Texture                silhouetteKey_; // only its address is used (Batch::texture)
    std::vector<gfx::SkinnedRenderer::SilhouetteJob> jobs_;
};

} // namespace xi::app
