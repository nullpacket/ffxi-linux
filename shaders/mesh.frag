// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(buffer_reference, std430) readonly buffer Transforms { mat4 m[]; };
layout(buffer_reference, std430) readonly buffer Draws { uvec4 d[]; };
layout(buffer_reference, std430) readonly buffer Frame
{
    vec4 eyeFogStart;
    vec4 fogColorEnd;
    vec4 ambient;
    vec4 light0Dir;
    vec4 light0Color;
    vec4 light1Dir;
    vec4 light1Color;
    vec4 points[512]; // 256 point lights: pos.xyz + range, colour.rgb (effect-format §Z)
};

layout(push_constant) uniform Push
{
    mat4       viewProj;
    Transforms transforms;
    Draws      draws;
    Frame      frame;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vNormal;
layout(location = 3) in vec3 vWorld;
layout(location = 4) flat in uint vTex;
layout(location = 5) flat in uint vFlags;
layout(location = 6) flat in uint vLights;
layout(location = 0) out vec4 outColor;


void main()
{
    vec4 t   = texture(textures[nonuniformEXT(vTex)], vUv);
    // D3D fixed-function lighting with the vertex colour as material: ambient + per-light N.L (docs/dat-format.md §6, §9).
    vec3 n   = normalize(vNormal);
    // Zone point lights (lanterns): colour·intensity·max(N·L̂,0)/d² within range, up to 4 per mesh (client vs.1.1
    // shader at 0x10377210, per vertex there).
    vec3 point = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        uint idx = (vLights >> (8 * i)) & 0xFFu;
        if (idx == 0u)
        {
            continue;
        }
        vec4  p  = pc.frame.points[2 * (idx - 1u)];
        vec3  l  = p.xyz - vWorld;
        float d2 = dot(l, l);
        if (d2 <= p.w * p.w && d2 > 1e-4)
        {
            point += pc.frame.points[2 * (idx - 1u) + 1u].rgb * max(dot(l, n), 0.0) * inversesqrt(d2) / d2;
        }
    }
    vec3 lit = vColor.rgb * (pc.frame.ambient.rgb + pc.frame.light0Color.rgb * max(dot(n, pc.frame.light0Dir.xyz), 0.0) +
                             pc.frame.light1Color.rgb * max(dot(n, pc.frame.light1Dir.xyz), 0.0) + point);
    lit      = clamp(lit, 0.0, 1.0); // D3D clamps the lit diffuse colour before the texture stage

    // Client texture stage 0: colour = MODULATE2X(tex, lit diffuse), alpha = MODULATE4X(tex, vertex alpha).
    vec3  rgb = clamp(2.0 * t.rgb * lit, 0.0, 1.0);
    float a   = clamp(4.0 * t.a * vColor.a, 0.0, 1.0);

    // Alpha test only for models whose name starts with '_': ALPHAFUNC GREATER, ALPHAREF 0x60.
    if ((vFlags & 1u) != 0u && a <= 96.0 / 255.0)
    {
        discard;
    }

    // Dissolve (flags bits 8-15, a fading corpse): screen-door discard against a 4x4 Bayer threshold.
    uint fade = (vFlags >> 8) & 0xFFu;
    if (fade != 0u)
    {
        const float bayer[16] = float[](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
        ivec2       q         = ivec2(gl_FragCoord.xy) & 3;
        if ((bayer[q.y * 4 + q.x] + 0.5) / 16.0 < float(fade) / 255.0)
        {
            discard;
        }
    }

    // Linear fog.
    float d = distance(vWorld, pc.frame.eyeFogStart.xyz);
    float f = clamp((pc.frame.fogColorEnd.w - d) / max(pc.frame.fogColorEnd.w - pc.frame.eyeFogStart.w, 1e-3), 0.0, 1.0);
    outColor = vec4(mix(pc.frame.fogColorEnd.rgb, rgb, f), a);
}
