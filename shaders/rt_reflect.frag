// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
// --rt=reflections (goal 38): the water reflection over the finished scene, after the effects. A harbour's sea is a
// zone effect (its layers drawn by the effect pass), so the reflection has to go on after it, not in rt_composite.
// Alpha-blended over the colour: rgb = the mirrored scene, a = its weight.
#extension GL_GOOGLE_include_directive : require
#include "rt_common.glsl"

layout(set = 0, binding = 5) uniform sampler2D tD; // water reflection rgb, weight

layout(push_constant) uniform Push
{
    Params p;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4  rf = texelFetch(tD, px, 0);
    uint  fx = pc.p.opts.x;
    if ((fx & RT_REFLECT) == 0u || rf.a <= 0.0)
    {
        discard;
    }
    if ((fx & RT_DEBUG_REFLECT) != 0u)
    {
        outColor = vec4(rf.a, 0.0, 0.0, 1.0);
        return;
    }
    if ((fx & 64u) != 0u) // XI_RT_DEBUG=reflectcolor
    {
        outColor = vec4(rf.rgb, 1.0);
        return;
    }
    outColor = vec4(rf.rgb, clamp(rf.a * 0.8 * pc.p.ambient.w * pc.p.tune.y, 0.0, min(0.75 * pc.p.tune.y, 0.95)));
}
