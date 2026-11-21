// SPDX-License-Identifier: GPL-3.0-or-later
#version 460
#extension GL_EXT_buffer_reference : require

layout(buffer_reference, std430) readonly buffer Gradient
{
    vec4 colors[8];
    vec4 positions[2];
};

layout(push_constant) uniform Push
{
    vec4     forward; // w = tan(fovY / 2)
    vec4     right;   // w = aspect
    vec4     up;
    Gradient gradient;
} pc;

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

void main()
{
    // Vulkan NDC y points down.
    vec3  ray = normalize(pc.forward.xyz + pc.right.xyz * (vNdc.x * pc.forward.w * pc.right.w) - pc.up.xyz * (vNdc.y * pc.forward.w));
    // Elevation above the horizon mapped 0 (horizon) .. 1 (zenith). INFERRED mapping: the client's sky consumer of the
    // gradient was not located (docs/dat-format.md §9.2); below the horizon use stop 0.
    float e   = clamp(asin(clamp(ray.y, -1.0, 1.0)) / 1.5707963, 0.0, 1.0);
    float pos[8] = float[](pc.gradient.positions[0].x, pc.gradient.positions[0].y, pc.gradient.positions[0].z, pc.gradient.positions[0].w,
                           pc.gradient.positions[1].x, pc.gradient.positions[1].y, pc.gradient.positions[1].z, pc.gradient.positions[1].w);
    vec3 c = pc.gradient.colors[0].rgb;
    for (int i = 0; i < 7; ++i)
    {
        if (e >= pos[i] && e <= pos[i + 1])
        {
            float span = max(pos[i + 1] - pos[i], 1e-4);
            c          = mix(pc.gradient.colors[i].rgb, pc.gradient.colors[i + 1].rgb, (e - pos[i]) / span);
        }
    }
    if (e > pos[7])
    {
        c = pc.gradient.colors[7].rgb;
    }
    outColor = vec4(c, 1.0);
}
