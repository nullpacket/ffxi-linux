// SPDX-License-Identifier: GPL-3.0-or-later
#version 460

// Config gamma (gfx::GammaPass): each channel of the finished frame raised to 1.5 / g (an exponent per channel).
layout(constant_id = 0) const int kSrgb = 0; // the swap chain is sRGB: work on the encoded values, as a ramp does
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(push_constant) uniform Push
{
    vec4 exponent;
} pc;

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

vec3 encode(vec3 l)
{
    return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l));
}

vec3 decode(vec3 e)
{
    return mix(e / 12.92, pow((e + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), e));
}

void main()
{
    vec3 c = texture(frame, vNdc * 0.5 + 0.5).rgb;
    if (kSrgb != 0)
    {
        c = encode(c);
    }
    c        = pow(max(c, vec3(0.0)), pc.exponent.rgb);
    outColor = vec4(kSrgb != 0 ? decode(c) : c, 1.0);
}
