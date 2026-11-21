// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec2 vUv;
layout(location = 1) flat in uint vTex;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 c = texture(textures[nonuniformEXT(vTex)], vUv);
    // FFXI (PS2 heritage) treats alpha 0x80 as fully opaque. INFERRED from the data; confirm in the client's
    // texture-stage setup before relying on it for blending.
    c.a = min(c.a * 2.0, 1.0);
    // Checkerboard behind transparent texels so alpha is visible.
    float check = mod(floor(gl_FragCoord.x / 8.0) + floor(gl_FragCoord.y / 8.0), 2.0) * 0.1 + 0.15;
    outColor    = vec4(mix(vec3(check), c.rgb, c.a), 1.0);
}
