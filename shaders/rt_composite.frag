// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_GOOGLE_include_directive : require
#include "rt_common.glsl"

// --rt (goal 38): the scene re-lit with the traced terms. The game lights a surface with
//   lit = colour x (ambient + light0 x N.L0 + light1 x N.L1 + point lights)      (mesh.frag, dat-format §6 / §9)
// and fogs the result linearly. Here each term is weighted by what the rays found (ambient by the occlusion, light 0 by
// the sun / moon visibility, the point lights by theirs, plus the bounce) and the pixel is scaled by new / old lighting
// under its fog: the light colours, directions and fog stay the game's.
layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform sampler2D tA;
layout(set = 0, binding = 2) uniform sampler2D tB;
layout(set = 0, binding = 3) uniform sampler2D tC;
layout(set = 0, binding = 4) uniform sampler2D depthTex;
layout(set = 0, binding = 5) uniform sampler2D tD; // water reflection (drawn by rt_reflect.frag after the effects)

layout(push_constant) uniform Push
{
    Params p;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4  c  = texelFetch(sceneTex, px, 0);
    float d  = texelFetch(depthTex, px, 0).r;
    if (d <= 0.0)
    {
        outColor = c;
        return;
    }
    vec4  a  = texelFetch(tA, px, 0);
    vec4  b  = texelFetch(tB, px, 0);
    vec4  cc = texelFetch(tC, px, 0);
    vec3  n  = octDecode(a.zw);
    uint  fx = pc.p.opts.x;
    float S  = pc.p.ambient.w;

    vec3  amb  = pc.p.ambient.rgb;
    vec3  l0   = pc.p.light0Color.rgb * max(dot(n, normalize(pc.p.light0Dir.xyz)), 0.0);
    vec3  l1   = pc.p.light1Color.rgb * max(dot(n, normalize(pc.p.light1Dir.xyz)), 0.0);
    vec3  pts  = (fx & RT_POINTS) != 0u ? cc.rgb : vec3(0.0);
    float ao   = (fx & RT_AO) != 0u ? max(mix(1.0, a.x, 0.85 * S), 0.0) : 1.0;
    // Light 0's shadow removes at most tune.x (40 % by default; `--rt-boost` raises it) of the pixel's lighting: by day the sun's share is about that anyway; at night
    // the moon outweighs the dim ambient, and a full shadow would go near black where the game's moonlight is soft.
    float share = luma(l0) / max(luma(amb + l0 + l1 + pts), 1e-3);
    float l0w   = min(1.0, pc.p.tune.x / max(share, 1e-3));
    float vis   = (fx & RT_SHADOWS) != 0u ? max(mix(1.0, a.y, 0.9 * S * pc.p.fogStart.y * l0w), 0.0) : 1.0;
    float pvis = (fx & RT_POINTS) != 0u ? max(mix(1.0, cc.a, 0.9 * S), 0.0) : 1.0;
    vec3  gi   = (fx & RT_GI) != 0u ? b.rgb * (pc.p.tune.z * S) * ao : vec3(0.0);

    vec3 before = amb + l0 + l1 + pts;
    vec3 after  = amb * ao + l0 * vis + l1 + pts * pvis + gi;
    vec3 k      = after / max(before, vec3(0.03));

    // Under the game's linear fog: c = f * lit + (1 - f) * fog, so only the lit part scales.
    vec2  uv   = (vec2(px) + 0.5) * pc.p.size.zw;
    vec3  w    = worldAt(pc.p, uv, d);
    float dist = distance(w, pc.p.eye.xyz);
    float fogE = pc.p.fogColorEnd.w, fogS = pc.p.fogStart.x;
    float f    = clamp((fogE - dist) / max(fogE - fogS, 1e-3), 0.0, 1.0);
    vec3  fog  = pc.p.fogColorEnd.rgb;
    vec3  res  = k * c.rgb + (1.0 - k) * (1.0 - f) * fog;
    outColor = vec4(clamp(res, 0.0, 1.0), c.a);
}
