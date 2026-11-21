// SPDX-License-Identifier: GPL-3.0-or-later
#version 460

// DLSS motion vectors for everything only the camera moves (gfx::MotionVectors, goal 15): each pixel's depth back to a
// world point through the jittered inverse, then that point in the previous frame (unjittered), minus the pixel's own
// unjittered position. Homogeneous throughout, so the sky (depth 0 = infinitely far, reverse-Z) gets the rotation's
// motion only.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(push_constant) uniform Push
{
    mat4 reproject; // the previous frame's unjittered view-projection times the inverse of this frame's jittered one
    vec4 jitter;      // xy: this frame's jitter in NDC (added to the projection)
} pc;

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec2 outMotion;

void main()
{
    vec2  size  = vec2(textureSize(depthTex, 0));
    float d     = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
    vec2  ndc   = gl_FragCoord.xy / size * 2.0 - 1.0;
    vec4  prev  = pc.reproject * vec4(ndc, d, 1.0);
    outMotion   = (prev.xy / prev.w - (ndc - pc.jitter.xy)) * 0.5 * size;
}
