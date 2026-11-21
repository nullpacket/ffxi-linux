// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
// The game's interface draws with MODULATE2X on colour and alpha: 0x80 in either input is 1.0 (docs/dat-format.md §14).
layout(set = 0, binding = 0) uniform texture2D uTexture;
layout(set = 1, binding = 0) uniform sampler uSampler;
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = clamp(2.0 * vColor * texture(sampler2D(uTexture, uSampler), vUV), 0.0, 1.0);
    // Quads with blending off (blend byte 0) are alpha-tested, as the game draws them: the name icons' stars (fontshp
    // 0xAC / 0xAD, blend [0 0 1 1]) have black, alpha-0 surrounds (INFERRED: the render state was not traced).
    if (outColor.a <= 0.0)
    {
        discard;
    }
}
