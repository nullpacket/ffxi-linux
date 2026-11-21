// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_GOOGLE_include_directive : require
#include "pt_common.glsl"

// --pathtrace (goal 41): the scene re-lit by the traced light. Traced pixels: material x E x exposure under the
// game's fog. The rest (characters, blended surfaces in front of the depth): the drawn colour times traced / game
// lighting, the fog part unchanged (as rt_composite.frag). Then blended with the game's look by --pathtrace-blend.
layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform sampler2D eTex;
layout(set = 0, binding = 2) uniform sampler2D albedoTex;
layout(set = 0, binding = 3) uniform sampler2D auxTex;
layout(set = 0, binding = 4) uniform sampler2D depthTex;

layout(push_constant) uniform Push
{
    PtParams p;
} pc;

layout(location = 0) out vec4 outColor;

layout(buffer_reference, std430) readonly buffer Exposure { float v[4]; };

// The game clamps lit colours at 1; the traced light can go well past it (a sunlit patch after the exposure). Up to
// 0.75 nothing changes; above, the luminance rolls off toward 1 with the hue kept, so lit grass stays green.
vec3 shoulder(vec3 c)
{
    if ((pc.p.opts.w & 4u) != 0u) // --post's ACES round trip rolls the highlights off: clip at its inverse's limit
    {
        return min(c, vec3(0.97));
    }
    float l = max(max(c.r, c.g), c.b);
    if (l <= 0.75)
    {
        return c;
    }
    float t = 0.75 + 0.25 * (1.0 - exp(-(l - 0.75) / 0.25));
    return c * (t / l);
}

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4  c  = texelFetch(sceneTex, px, 0);
    float d  = texelFetch(depthTex, px, 0).r;
    vec4  a  = texelFetch(auxTex, px, 0);
    if (d <= 0.0 || a.x <= 0.0)
    {
        outColor = c;
        return;
    }
    float auto_ = Exposure(pc.p.exposure).v[0];
    vec3  E    = texelFetch(eTex, px, 0).rgb * pc.p.fogStart.y * (auto_ > 0.0 ? auto_ : 1.0);
    vec3  m    = texelFetch(albedoTex, px, 0).rgb;
    vec3  n    = octDecode(a.yz);
    vec2  uv   = (vec2(px) + 0.5) * pc.p.size.zw;
    vec3  w    = ptWorldAt(pc.p, uv, d);
    float f    = ptFog(pc.p, distance(w, pc.p.eye.xyz));
    vec3  fog  = pc.p.fogColorEnd.rgb;
    vec3  res;
    if (a.w > 0.5)
    {
        res = mix(fog, shoulder(m * E), f);
    }
    else
    {
        vec3 game = pc.p.ambient.rgb + pc.p.light0Color.rgb * max(dot(n, normalize(pc.p.light0Dir.xyz)), 0.0) +
                    pc.p.light1Color.rgb * max(dot(n, normalize(pc.p.light1Dir.xyz)), 0.0);
        vec3 k = E / max(game, vec3(0.03));
        res    = shoulder(k * c.rgb + (1.0 - k) * (1.0 - f) * fog);
    }
    outColor = vec4(mix(c.rgb, clamp(res, 0.0, 1.0), pc.p.fogStart.z), c.a);
}
