// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_nonuniform_qualifier : require

// The detailed shadows' silhouette (SkinnedRenderer::drawSilhouettes): the model flattened along the light, written as
// the shadow's strength (the ground decal multiplies the frame by 1 - this). Alpha-tested parts keep their holes.
layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec2 vUv;
layout(location = 4) flat in uint vTex;
layout(location = 5) flat in uint vFlags;

layout(location = 0) out vec4 outColor;

void main()
{
    if ((vFlags & 1u) != 0u && texture(textures[nonuniformEXT(vTex)], vUv).a * 4.0 <= 96.0 / 255.0) // skinned.frag's alpha test
    {
        discard;
    }
    outColor = vec4(0.5, 0.5, 0.5, 1.0);
}
