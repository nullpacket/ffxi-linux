// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_buffer_reference : require

// Effect particles (docs/effect-format.md): triangles built on the CPU in render space.
struct Vertex { vec4 posTex; vec4 color; vec4 uv; vec4 stage; }; // posTex.w = texture index, < 0 = untextured
layout(buffer_reference, std430) readonly buffer Vertices { Vertex v[]; };

layout(push_constant) uniform Push
{
    mat4     viewProj;
    Vertices vertices;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out int vTex;
layout(location = 3) out vec4 vStage;

void main()
{
    Vertex v    = pc.vertices.v[gl_VertexIndex];
    vColor      = v.color;
    vStage      = v.stage;
    vUv         = v.uv.xy;
    vTex        = int(v.posTex.w);
    gl_Position = pc.viewProj * vec4(v.posTex.xyz, 1.0);
}
