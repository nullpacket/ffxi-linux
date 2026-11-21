// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_buffer_reference : require

// DLSS motion vectors for characters (gfx::SkinnedRenderer::drawMotion, goal 15): skinned.vert's skinning, once with
// this frame's bones and once with the previous frame's, projected by the unjittered matrices of both frames.
layout(buffer_reference, std430) readonly buffer Bones { mat4 m[]; };
layout(buffer_reference, std430) readonly buffer Motion
{
    mat4 cur;  // this frame's view-projection without the DLSS jitter
    mat4 prev; // the previous frame's, without jitter
    vec4 size; // render extent in pixels
};

// Must match SkinPush (skinned.vert): `unused` carries the previous palette, `frame` the Motion block.
layout(push_constant) uniform Push
{
    mat4   viewProj; // jittered, as the colour pass (the depth test must match it)
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

layout(location = 0) in vec3  inP0;
layout(location = 1) in vec3  inP1;
layout(location = 2) in vec3  inN0;
layout(location = 3) in vec3  inN1;
layout(location = 4) in vec2  inUv;
layout(location = 5) in uvec4 inBones;
layout(location = 6) in float inWeight1;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vCur;
layout(location = 2) out vec4 vPrev;
layout(location = 3) flat out uint vTex;
layout(location = 4) flat out uint vFlags;

vec4 skin(Bones b)
{
    mat4 b0 = b.m[pc.boneBase + inBones.x];
    mat4 b1 = b.m[pc.boneBase + inBones.y];
    return b0 * vec4(inP0, 1.0 - inWeight1) + b1 * vec4(inP1, inWeight1);
}

void main()
{
    vec4 world  = skin(pc.bones);
    vUv         = inUv;
    vCur        = pc.motion.cur * world;
    vPrev       = pc.motion.prev * skin(pc.prevBones);
    vTex        = pc.texture;
    vFlags      = pc.flags;
    gl_Position = pc.viewProj * world;
}
