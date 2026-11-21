// SPDX-License-Identifier: GPL-3.0-or-later
// --pathtrace (goal 41): shared by pt_trace.comp, pt_denoise.comp and pt_composite.frag (PathTracer, src/gfx/pathtrace.cpp).
#include "rt_common.glsl"

layout(buffer_reference, std430) readonly buffer PtParams
{
    mat4     invViewProj;
    mat4     viewProj;
    mat4     prevViewProj;
    vec4     eye; // w = frame
    vec4     light0Dir;
    vec4     light0Color;
    vec4     light1Dir;
    vec4     light1Color;
    vec4     ambient;     // w = the vertex colours' baked shading kept (0..1)
    vec4     fogColorEnd; // w = fog end
    vec4     fogStart;    // x fog start, y exposure multiplier, z blend (0 the game's look .. 1 traced), w light 0 / 1 shadow weight
    uvec4    opts;        // x bounces, y point lights, z frame, w bit0 reset, bit1 Ray Reconstruction, bit2 --post tone maps
    vec4     size;
    Records  records;
    uint64_t exposure; // the automatic exposure buffer (pt_expose.comp): float v[4], v[0] the exposure
    vec4     sky[8];
    vec4     skyPos[2];
    vec4     skyNorm; // x = 1 / the gradient's mean luma
    vec4     points[512];
};

vec3 ptWorldAt(PtParams p, vec2 uv, float d)
{
    vec4 w = p.invViewProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return w.xyz / w.w;
}

// The game's linear fog factor at a distance (1 = no fog).
float ptFog(PtParams p, float dist)
{
    return clamp((p.fogColorEnd.w - dist) / max(p.fogColorEnd.w - p.fogStart.x, 1e-3), 0.0, 1.0);
}
