// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require

// DLSS motion vectors for characters: previous minus current position, in render pixels (MV scale 1).
layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(buffer_reference, std430) readonly buffer Bones { mat4 m[]; };
layout(buffer_reference, std430) readonly buffer Motion
{
    mat4 cur;
    mat4 prev;
    vec4 size;
};
layout(push_constant) uniform Push
{
    mat4   viewProj;
    Bones  bones;
    Bones  prevBones;
    Motion motion;
    uint   boneBase;
    uint   texture;
    uint   flags;
    uint   tint;
    uint   envTexture;
    uint   specTexture;
    vec4   origin;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vCur;
layout(location = 2) in vec4 vPrev;
layout(location = 3) flat in uint vTex;
layout(location = 4) flat in uint vFlags;
layout(location = 0) out vec2 outMotion;

void main()
{
    // The colour pass's alpha test (skinned.frag): cut-out texels keep the background's motion.
    if ((vFlags & 1u) != 0u && texture(textures[nonuniformEXT(vTex)], vUv).a * 4.0 <= 96.0 / 255.0)
    {
        discard;
    }
    outMotion = (vPrev.xy / vPrev.w - vCur.xy / vCur.w) * 0.5 * pc.motion.size.xy;
}
