// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require

// Character meshes: mesh.frag's lighting (no zone point lights) plus the Os2 0x8010 material (dat-format §10.3):
// lighting scale, environment reflection and specular highlight on texture stage 1 (CODE 0x1002e02c..0x1002e3f9).

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(buffer_reference, std430) readonly buffer Bones { mat4 m[]; };
layout(buffer_reference, std430) readonly buffer Frame
{
    vec4 eyeFogStart;
    vec4 fogColorEnd;
    vec4 ambient;
    vec4 light0Dir;
    vec4 light0Color;
    vec4 light1Dir;
    vec4 light1Color;
    vec4 camRight; // view basis in render space (D3D camera space: x right, y up, z forward)
    vec4 camUp;
    vec4 camForward;
    vec4 specMode; // x = 1: the game's specular mapping, 0: the centred blob (XI_SPEC_OLD)
};

// Must match SkinPush (skinned.vert).
layout(push_constant) uniform Push
{
    mat4  viewProj;
    Bones bones;
    Bones unused;
    Frame frame;
    uint  boneBase;
    uint  texture;
    uint  flags; // bit0 alpha test, bit1 overlay pass, bit2 env, bit3 spec, 8..15 dissolve, 16..23 light scale x 64 (0 = 1.0), 24..31 strength
    uint  tint;
    uint  envTexture;
    uint  specTexture;
    vec4  origin; // the actor's position (render space)
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vNormal;
layout(location = 3) in vec3 vWorld;
layout(location = 4) flat in uint vTex;
layout(location = 5) flat in uint vFlags;
layout(location = 6) flat in uint vLights;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 t = texture(textures[nonuniformEXT(vTex)], vUv);
    vec3 n = normalize(vNormal);
    if ((vFlags & 2u) != 0u)
    {
        // Effect texture overlay (scheduler ops 0x40 / 0x41 / 0x54 / 0x55, CODE draw 0x1002b885): envTexture = the
        // overlay, specTexture = the scroll (half floats, ops 0x42-0x45) added to 0.5 + the camera-space normal / 2
        // (INFERRED from the 0.5 entries of the texture matrix 0x10457158). The model's own alpha test still cuts.
        if ((vFlags & 1u) != 0u && t.a * 4.0 <= 96.0 / 255.0)
        {
            discard;
        }
        vec2 uv  = 0.5 + 0.5 * vec2(dot(n, pc.frame.camRight.xyz), -dot(n, pc.frame.camUp.xyz)) + unpackHalf2x16(pc.specTexture);
        vec4 o   = texture(textures[nonuniformEXT(pc.envTexture)], uv);
        float d  = distance(vWorld, pc.frame.eyeFogStart.xyz);
        float f  = clamp((pc.frame.fogColorEnd.w - d) / max(pc.frame.fogColorEnd.w - pc.frame.eyeFogStart.w, 1e-3), 0.0, 1.0);
        // Colour slot 1 (ops 0x46 / 0x47, actor +0x74c) multiplies it; INFERRED: how the draw applies that colour.
        vec4 k   = clamp(vColor * 2.0, 0.0, 2.0);
        outColor = vec4(o.rgb * f * k.rgb, clamp(o.a * 2.0 * k.a, 0.0, 1.0));
        return;
    }
    // Lighting scale (+0x10): the material multiplies the lights' diffuse and the ambient colour (CODE 0x1002e02c).
    uint  ls    = (vFlags >> 16) & 0xFFu;
    float scale = ls == 0u ? 1.0 : float(ls) / 64.0;
    vec3  lit   = vColor.rgb * scale * (pc.frame.ambient.rgb + pc.frame.light0Color.rgb * max(dot(n, pc.frame.light0Dir.xyz), 0.0) +
                                       pc.frame.light1Color.rgb * max(dot(n, pc.frame.light1Dir.xyz), 0.0));
    lit = clamp(lit, 0.0, 1.0);

    // Texture stage 0: colour = MODULATE2X(tex, lit diffuse), alpha = MODULATE4X(tex, vertex alpha).
    vec3  rgb = clamp(2.0 * t.rgb * lit, 0.0, 1.0);
    float a   = clamp(4.0 * t.a * vColor.a, 0.0, 1.0);

    // Stage 1 (CODE 0x1002e1c0 / 0x1002e326; arg 1 = CURRENT from the base setup 0x1002bc8a, arg 2 = TEXTURE):
    // MODULATEALPHA_ADDCOLOR adds the texture x the current alpha, and stage 0's alpha is MODULATE2X(texture alpha,
    // TFACTOR alpha = the material strength +0x24 x 0.5; 0x1002bbb5 / 0x1002bbe3), so the gain is 2 x tex.a x strength
    // (= strength on the usual 0x80 = 1.0 alpha). Stage 1 addresses CLAMP (0x1002bc42 / 0x1002bc5a).
    float strength = 2.0 * t.a * float((vFlags >> 24) & 0xFFu) / 255.0;
    if ((vFlags & 4u) != 0u)
    {
        // Environment: TEXCOORDINDEX = CAMERASPACENORMAL with an identity texture matrix (0x10457108): uv = normal.xy in
        // camera space, clamped (CODE 0x1002e159).
        vec2 uv = clamp(vec2(dot(n, pc.frame.camRight.xyz), dot(n, pc.frame.camUp.xyz)), 0.0, 1.0);
        rgb     = clamp(rgb + strength * texture(textures[nonuniformEXT(pc.envTexture)], uv).rgb, 0.0, 1.0);
    }
    else if ((vFlags & 8u) != 0u)
    {
        // Specular: TEXCOORDINDEX = CAMERASPACEREFLECTIONVECTOR | 1 with TTFF COUNT3 (CODE 0x1002e3ab / 0x1002e3c6)
        // through the matrix 0x10457198 = diag(1, -1, -1) x the camera's full inverse view x the actor's light look-at
        // (0x1045b7c0: at = the actor, eye = at - its light direction, up = game (0, -1, 0); CODE 0x100cc263..0x100cc2b5).
        // A generated coordinate carries w = 1 (wined3d's fixed-function code builds it so), so the reflection vector
        // acts as a point: in world terms p = eye + 2 camRight (r . camRight) - r, and uv = the look-at's x, y of p
        // relative to the actor, clamped (stage 1 CLAMP). XI_SPEC_OLD=1: the earlier centred mapping.
        vec3 v = normalize(vWorld - pc.frame.eyeFogStart.xyz);
        vec3 r = reflect(v, n);
        vec2 uv;
        bool on = true;
        if (pc.frame.specMode.x > 0.5)
        {
            vec3 p  = pc.frame.eyeFogStart.xyz + 2.0 * pc.frame.camRight.xyz * dot(r, pc.frame.camRight.xyz) - r;
            vec3 z  = -normalize(pc.frame.light0Dir.xyz); // the light's travel
            vec3 x  = cross(vec3(0.0, 1.0, 0.0), z);      // up: game (0, -1, 0) in render space
            x       = dot(x, x) > 1e-6 ? normalize(x) : vec3(1.0, 0.0, 0.0);
            vec3 y  = cross(z, x);
            vec3 q  = p - pc.origin.xyz;
            uv      = clamp(vec2(dot(q, x), dot(q, y)), 0.0, 1.0);
        }
        else
        {
            vec3 l  = normalize(pc.frame.light0Dir.xyz);
            vec3 lr = normalize(abs(l.y) > 0.99 ? cross(l, vec3(1, 0, 0)) : cross(l, vec3(0, 1, 0)));
            vec3 lu = cross(lr, l);
            on      = dot(r, l) > 0.0;
            uv      = vec2(0.5 + 0.5 * dot(r, lr), 0.5 - 0.5 * dot(r, lu));
        }
        if (on)
        {
            rgb = clamp(rgb + strength * texture(textures[nonuniformEXT(pc.specTexture)], uv).rgb, 0.0, 1.0);
        }
    }

    if ((vFlags & 1u) != 0u && a <= 96.0 / 255.0)
    {
        discard;
    }
    uint fade = (vFlags >> 8) & 0xFFu;
    if (fade != 0u)
    {
        const float bayer[16] = float[](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
        ivec2       q         = ivec2(gl_FragCoord.xy) & 3;
        if ((bayer[q.y * 4 + q.x] + 0.5) / 16.0 < float(fade) / 255.0)
        {
            discard;
        }
    }
    float d  = distance(vWorld, pc.frame.eyeFogStart.xyz);
    float f  = clamp((pc.frame.fogColorEnd.w - d) / max(pc.frame.fogColorEnd.w - pc.frame.eyeFogStart.w, 1e-3), 0.0, 1.0);
    outColor = vec4(mix(pc.frame.fogColorEnd.rgb, rgb, f), a);
}
