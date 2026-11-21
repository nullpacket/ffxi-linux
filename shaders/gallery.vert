// SPDX-License-Identifier: GPL-3.0-or-later
#version 460

// Draws one textured quad per instance; quad corners come from gl_VertexIndex (no vertex buffers).
layout(push_constant) uniform Push
{
    vec2 offset; // NDC top-left
    vec2 size;   // NDC size
    uint tex;
} pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) flat out uint vTex;

void main()
{
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 c      = corners[gl_VertexIndex];
    vUv         = c;
    vTex        = pc.tex;
    gl_Position = vec4(pc.offset + c * pc.size, 0.0, 1.0);
}
