// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shader_draw_parameters : require

layout(buffer_reference, std430) readonly buffer Transforms { mat4 m[]; };
struct DrawData { uint transform; uint texture; uint flags; uint lights; };
layout(buffer_reference, std430) readonly buffer Draws { DrawData d[]; };

layout(buffer_reference, std430) readonly buffer Frame
{
    vec4 eyeFogStart;
    vec4 fogColorEnd;
    vec4 ambient;
    vec4 light0Dir;
    vec4 light0Color;
    vec4 light1Dir;
    vec4 light1Color;
};

layout(push_constant) uniform Push
{
    mat4       viewProj;
    Transforms transforms;
    Draws      draws;
    Frame      frame;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inUv;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vNormal;
layout(location = 3) out vec3 vWorld;
layout(location = 4) flat out uint vTex;
layout(location = 5) flat out uint vFlags;
layout(location = 6) flat out uint vLights;

void main()
{
    DrawData dd    = pc.draws.d[gl_DrawIDARB];
    mat4     m     = pc.transforms.m[dd.transform];
    vec4     world = m * vec4(inPos, 1.0);
    vUv            = inUv;
    vColor         = inColor;
    vNormal        = normalize(mat3(m) * inNormal);
    vWorld         = world.xyz;
    vTex           = dd.texture;
    vFlags         = dd.flags;
    vLights        = dd.lights;
    gl_Position    = pc.viewProj * world;
}
