// SPDX-License-Identifier: GPL-3.0-or-later
// --post (goal 41; docs/enhanced-graphics.md): shared by the post_*.comp passes (src/gfx/post.cpp).
// The scene is the game's 8-bit image; the passes lift it into a linear HDR working buffer through the inverse of the
// tone map, add light there (volumetric light, bloom), and tone map back. With nothing added the round trip returns
// the same image, so `--post=hdr` alone keeps today's look.
#extension GL_EXT_buffer_reference : require

// The inverse view-projection lives in a small buffer (PostProcess::view_): with it in the push block the block was
// 176 bytes, over the 128 bytes Vulkan guarantees (AMD's own driver reports exactly 128; issue #139).
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PostView
{
    mat4 invViewProj;
};

layout(push_constant) uniform Push
{
    vec4 eye;        // xyz the camera, w frame (noise) or -1 for the first bloom level
    vec4 lightDir;   // xyz toward light 0 (sun / moon), w fog end
    vec4 lightColor; // rgb light 0, w bloom strength
    vec4 fogColor;   // rgb the zone's fog colour, w fog density scale
    vec4 ambient;    // rgb ambient, w light shaft strength
    vec4 sun;        // xy light 0's screen uv, z how far it is on screen (0..1), w flags
    vec4 size;       // xy this pass's output pixels, zw 1 / pixels
    PostView view;   // the frame's inverse view-projection
} pc;

const uint POST_HDR = 1u, POST_BLOOM = 2u, POST_GODRAYS = 4u, POST_FOG = 8u, POST_RQ = 16u;

uint flags()
{
    return uint(pc.sun.w);
}

// ACES filmic fit (Narkowicz 2015) and its exact inverse on [0, 0.97] (white comes back as about 3.4).
vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

float acesInv1(float y)
{
    y       = clamp(y, 0.0, 0.97);
    float a = 2.51 - 2.43 * y;
    float b = 0.03 - 0.59 * y;
    float c = -0.14 * y;
    return (-b + sqrt(max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a);
}

vec3 acesInv(vec3 y)
{
    return vec3(acesInv1(y.r), acesInv1(y.g), acesInv1(y.b));
}

// The stored 8-bit values are gamma-encoded (the swap chain is UNORM; context.cpp): light adds in linear.
vec3 toLinear(vec3 c)
{
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

vec3 toGamma(vec3 c)
{
    return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
}

float luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 worldAt(vec2 uv, float d)
{
    vec4 w = pc.view.invViewProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return w.xyz / w.w;
}

float hash(uvec2 p)
{
    uint h = p.x * 1973u + p.y * 9277u + 0x68E31DA4u;
    h      = (h ^ (h >> 15u)) * 0x2C1B3C6Du;
    h      = (h ^ (h >> 12u)) * 0x297A2D39u;
    return float(h ^ (h >> 15u)) / 4294967296.0;
}
