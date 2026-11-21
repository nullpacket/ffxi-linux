// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
// Dear ImGui's vertex layout and push constants (imgui_impl_vulkan.cpp), so this pipeline shares its layout.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(push_constant) uniform Push { vec2 scale; vec2 translate; } pc;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

void main()
{
    vColor      = aColor;
    vUV         = aUV;
    gl_Position = vec4(aPos * pc.scale + pc.translate, 0, 1);
}
