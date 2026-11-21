// SPDX-License-Identifier: GPL-3.0-or-later
// --rt (goal 38): shared by rt_trace.comp, rt_denoise.comp and rt_composite.frag (RayTracing, src/gfx/raytracing.cpp).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

struct Record
{
    uint64_t vertices;
    uint64_t indices;
    uint     texture;
    uint     flags; // bit0 alpha test, bits 1-2 texture set (0 zone, 1 sub-map, 2 none), bit3 positions only, bit4 blended
    uint     stride;
    uint     pad;
};
layout(buffer_reference, std430) readonly buffer Records { Record r[]; };
layout(buffer_reference, std430) readonly buffer Params
{
    mat4     invViewProj;
    mat4     viewProj;
    mat4     prevViewProj;
    vec4     eye; // w = frame
    vec4     light0Dir;
    vec4     light0Color;
    vec4     light1Dir;
    vec4     light1Color;
    vec4     ambient; // w = strength
    vec4     fogColorEnd;
    vec4     fogStart; // x fog start, y light 0 shadow weight (indoors less), z water planes
    uvec4    opts; // x effect bits (1 ao, 2 shadows, 4 points, 8 gi, 16 reflections), y point lights, z frame, w reset
    vec4     size;
    vec4     tune; // x light 0 shadow cap (0.4), y reflection weight scale (1), z bounce weight (0.6), w AO radius (1.6)
    Records  records;
    uint64_t pad;
    vec4     points[512];
    vec4     water[32]; // per water plane: box (x0, z0, x1, z1), then (y, 0, 0, 0); count in fogStart.z
};

const uint RT_AO = 1u, RT_SHADOWS = 2u, RT_POINTS = 4u, RT_GI = 8u, RT_REFLECT = 16u, RT_DEBUG_REFLECT = 32u;

vec3 worldAt(Params p, vec2 uv, float d)
{
    vec4 w = p.invViewProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return w.xyz / w.w;
}

vec2 octEncode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
}

vec3 octDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

float luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}
