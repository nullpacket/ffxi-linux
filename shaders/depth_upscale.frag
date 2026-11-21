// SPDX-License-Identifier: GPL-3.0-or-later
#version 460

// The scene's depth at the window size (gfx::MotionVectors::drawDepth, issue #101): with Ray Reconstruction the
// effects draw over DLSS's output, depth-tested against this. Each window pixel takes the render-size depth under its
// unjittered position (the scene was drawn with the jitter, so it sits jitter pixels off), the farthest of the 2x2
// texels around it (reverse-Z: the smallest), so effects lying on a surface are not cut by the coarser depth.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(push_constant) uniform Push
{
    vec4 scale; // xy: render size / window size; zw: the jitter in render pixels
} pc;

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor; // not written (the pipeline's colour mask is empty)

void main()
{
    ivec2 size = textureSize(depthTex, 0);
    vec2  p    = gl_FragCoord.xy * pc.scale.xy + pc.scale.zw - 0.5;
    ivec2 a    = clamp(ivec2(floor(p)), ivec2(0), size - 1);
    ivec2 b    = clamp(a + 1, ivec2(0), size - 1);
    float d    = min(min(texelFetch(depthTex, a, 0).r, texelFetch(depthTex, ivec2(b.x, a.y), 0).r),
                     min(texelFetch(depthTex, ivec2(a.x, b.y), 0).r, texelFetch(depthTex, b, 0).r));
    gl_FragDepth = d;
    outColor     = vec4(0.0);
}
