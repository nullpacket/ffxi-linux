// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D textures[];
// Captured images of the scene (docs/effect-format.md §S.3 ops 0x0E / 0x10, particle classes 0x22 / 0x24): [0] this
// frame's, copied before the screen-sampling effects draw; [1] the one frozen by op 0x10.
layout(set = 1, binding = 0) uniform sampler2D screenTex[2];

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in int vTex;
layout(location = 3) in vec4 vStage;
layout(location = 0) out vec4 outColor;
// 1: nothing outside UV 0..1 (ground shadows: a decal over larger floor triangles).
layout(constant_id = 0) const int kClampUv = 0;

void main()
{
    // The game's two texture stages (§M.2; client::EffectSystem::fxCombine): vColor x texture is stage 0, vStage the
    // element colour of stage 1, both factors already holding the 2x / 4x modulation. Direct3D clamps each stage.
    if (kClampUv == 1 && (any(lessThan(vUv, vec2(0.0))) || any(greaterThan(vUv, vec2(1.0)))))
    {
        discard;
    }
    vec4 c = vColor;
    if (vTex >= 0)
    {
        c *= texture(textures[nonuniformEXT(vTex)], vUv);
    }
    else if (vTex == -2)
    {
        // The scene behind this fragment (the game's UVs are the vertices' own projections), moved by vUv.
        c.rgb *= texture(screenTex[0], gl_FragCoord.xy / vec2(textureSize(screenTex[0], 0)) + vUv).rgb;
    }
    else if (vTex == -3 || vTex == -4)
    {
        // A full-screen layer: the frozen (-3) or this frame's (-4) image at vUv.
        c.rgb *= texture(screenTex[vTex == -3 ? 1 : 0], vUv).rgb;
    }
    outColor = clamp(clamp(c, 0.0, 1.0) * vStage, 0.0, 1.0);
    if (outColor.a <= 0.0)
    {
        discard;
    }
}
