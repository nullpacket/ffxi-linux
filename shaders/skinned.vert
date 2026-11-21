// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_buffer_reference : require

layout(buffer_reference, std430) readonly buffer Bones { mat4 m[]; };
layout(buffer_reference, std430) readonly buffer Frame { vec4 eyeFogStart; };

// Must match SkinPush (and skinned.frag).
layout(push_constant) uniform Push
{
    mat4     viewProj;
    Bones    bones;
    Bones    unused; // padding slot (mesh.frag "draws")
    Frame    frame;
    uint     boneBase;
    uint     texture;
    uint     flags;
    uint     tint; // RGBA8, 0x80 = 1.0
    uint     envTexture;
    uint     specTexture;
    vec4  origin; // the actor's position (render space)
} pc;

layout(location = 0) in vec3  inP0;
layout(location = 1) in vec3  inP1;
layout(location = 2) in vec3  inN0;
layout(location = 3) in vec3  inN1;
layout(location = 4) in vec2  inUv;
layout(location = 5) in uvec4 inBones;
layout(location = 6) in float inWeight1;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vNormal;
layout(location = 3) out vec3 vWorld;
layout(location = 4) flat out uint vTex;
layout(location = 5) flat out uint vFlags;
layout(location = 6) flat out uint vLights; // zone point lights: none for characters (effect-format §Z)

void main()
{
    // FFXI two-bone skinning (docs/dat-format.md §10.3): each bone has its own bone-local position and normal, stored
    // already multiplied by that bone's weight, so only the translations take the weights: p0·R0 + w0·T0 + p1·R1 + w1·T1.
    // One-bone vertices have p1 = n1 = 0 and weight 0.
    mat4 b0    = pc.bones.m[pc.boneBase + inBones.x];
    mat4 b1    = pc.bones.m[pc.boneBase + inBones.y];
    vec4 world = b0 * vec4(inP0, 1.0 - inWeight1) + b1 * vec4(inP1, inWeight1);
    vec3 n     = mat3(b0) * inN0 + mat3(b1) * inN1;
    vUv        = inUv;
    vColor     = unpackUnorm4x8(pc.tint) * (255.0 / 256.0); // no vertex colour: the actor's tint, 0x80 = 1.0 (0.5 here)
    vNormal    = normalize(n);
    vWorld     = world.xyz;
    vTex       = pc.texture;
    vFlags     = pc.flags;
    gl_Position = pc.viewProj * world;
    vLights = 0u;
}
