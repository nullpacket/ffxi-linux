// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require

// The refraction pass of effect ops 0x48 / 0x49 (CODE draw 0x1002b947, strength actor +0x744): the character's model
// again, textured with the screen copy (device +0x194) at its own projected position. The texture matrix maps clip
// space to the screen (0.5 / -0.5 / 0.5 entries) and translates by the strength on both axes, so the silhouette shows
// the scene behind shifted by it. Colour (0x80 - the actor's alpha) | 0x808080: the stronger, the more the actor has
// faded (pop-in / pop-out). Blend SRCALPHA / INVSRCALPHA (INFERRED).

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 1, binding = 0) uniform sampler2D screenTex[2];

layout(buffer_reference, std430) readonly buffer Bones { mat4 m[]; };
layout(buffer_reference, std430) readonly buffer Frame { vec4 eyeFogStart; };

// Must match SkinPush (skinned.vert); `tint` = the weight (0..255) in the alpha byte, `envTexture` = the strength.
layout(push_constant) uniform Push
{
    mat4  viewProj;
    Bones bones;
    Bones unused;
    Frame frame;
    uint  boneBase;
    uint  texture;
    uint  flags; // bit0 alpha test
    uint  tint;
    uint  envTexture;
    uint  specTexture;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 4) flat in uint vTex;
layout(location = 5) flat in uint vFlags;
layout(location = 0) out vec4 outColor;

void main()
{
    if ((vFlags & 1u) != 0u && texture(textures[nonuniformEXT(vTex)], vUv).a * 4.0 <= 96.0 / 255.0)
    {
        discard;
    }
    float strength = uintBitsToFloat(pc.envTexture);
    vec2  uv       = gl_FragCoord.xy / vec2(textureSize(screenTex[0], 0)) + vec2(strength);
    outColor       = vec4(texture(screenTex[0], fract(uv)).rgb, float(pc.tint >> 24) / 255.0);
}
