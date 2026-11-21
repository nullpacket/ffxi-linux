// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_buffer_reference : require

struct Instance { vec4 feetHeight; vec4 color; vec4 size; };
layout(buffer_reference, std430) readonly buffer Instances { Instance i[]; };

layout(push_constant) uniform Push
{
    mat4      viewProj;
    Instances instances;
} pc;

layout(location = 0) out vec4 vColor;

// 36-vertex unit box (x/z in [-0.5, 0.5], y in [0, 1]), counter-clockwise faces seen from outside.
const vec3 kCorners[8] = vec3[](vec3(-0.5, 0, -0.5), vec3(0.5, 0, -0.5), vec3(0.5, 0, 0.5), vec3(-0.5, 0, 0.5),
                                vec3(-0.5, 1, -0.5), vec3(0.5, 1, -0.5), vec3(0.5, 1, 0.5), vec3(-0.5, 1, 0.5));
const int  kIndex[36]  = int[](0, 1, 2, 0, 2, 3,  4, 6, 5, 4, 7, 6,  0, 4, 5, 0, 5, 1,  1, 5, 6, 1, 6, 2,  2, 6, 7, 2, 7, 3,  3, 7, 4, 3, 4, 0);
const float kShade[6]  = float[](0.5, 1.0, 0.75, 0.85, 0.7, 0.8);

void main()
{
    Instance inst = pc.instances.i[gl_InstanceIndex];
    vec3     c    = kCorners[kIndex[gl_VertexIndex]];
    vec3     p    = inst.feetHeight.xyz + vec3(c.x * inst.size.x, c.y * inst.feetHeight.w, c.z * inst.size.x);
    vColor        = vec4(inst.color.rgb * kShade[gl_VertexIndex / 6], 1.0);
    gl_Position   = pc.viewProj * vec4(p, 1.0);
}
