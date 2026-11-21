// SPDX-License-Identifier: GPL-3.0-or-later
// --post=fog / godrays: the light the air between the camera and each pixel scatters toward it, at half resolution.
// rgb = in-scattered light (linear HDR), a = how much of the scene behind still shows. Marched in steps; with --rt each
// step tests light 0 against the TLAS (shafts through trees and arches), else light 0 counts as unshadowed (the
// screen-space rays in post_rays.comp then make the shafts).
#include "post_common.glsl"
#ifdef POST_RQ_ON
layout(set = 0, binding = 3) uniform accelerationStructureEXT tlas;
#endif
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba16f) uniform writeonly image2D outVol;
layout(set = 0, binding = 1) uniform sampler2D depthTex;

float visibility(vec3 p, vec3 l)
{
#ifdef POST_RQ_ON
    rayQueryEXT q;
    rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, p, 0.05, l, 250.0);
    while (rayQueryProceedEXT(q))
    {
        rayQueryConfirmIntersectionEXT(q); // foliage cards block as solid: the canopies are solid meshes anyway
    }
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
#else
    return 1.0;
#endif
}

void main()
{
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(pc.size.xy))))
    {
        return;
    }
    vec2  uv     = (vec2(px) + 0.5) * pc.size.zw;
    float d      = textureLod(depthTex, uv, 0.0).r;
    float fogEnd = max(pc.lightDir.w, 10.0);
    vec3  e      = pc.eye.xyz;
    vec3  target = worldAt(uv, max(d, 1e-6));
    vec3  rd     = normalize(target - e);
    float tEnd   = d <= 0.0 ? fogEnd : min(distance(target, e), fogEnd);
    uint  f      = flags();
    vec3  l      = normalize(pc.lightDir.xyz);
    float sunOn  = luma(pc.lightColor.rgb) > 0.01 ? 1.0 : 0.0;

    // Densities per yalm, scaled to the zone's fog range so a small dungeon and a wide field read alike.
    float fogSigma = (f & POST_FOG) != 0u ? 0.25 * pc.fogColor.w / fogEnd : 0.0;
#ifdef POST_RQ_ON
    float shaftSigma = (f & POST_GODRAYS) != 0u ? 0.15 * pc.ambient.w / fogEnd : 0.0;
#else
    float shaftSigma = 0.0; // unshadowed it would only be a glow: the shafts come from post_rays.comp instead
#endif
    float       cosT  = dot(rd, l);
    const float g     = 0.4;
    float       phase      = (1.0 - g * g) / (4.0 * 3.14159 * pow(1.0 + g * g - 2.0 * g * cosT, 1.5));
    vec3        lightLin   = toLinear(pc.lightColor.rgb) * sunOn;
    vec3        fogLin     = toLinear(pc.fogColor.rgb);

    const int kSteps = 24;
    float     dt     = tEnd / float(kSteps);
    float     j      = hash(uvec2(px) + uvec2(uint(max(pc.eye.w, 0.0)) * 7u, 0u));
    vec3      ins    = vec3(0.0);
    float     trans  = 1.0;
    for (int i = 0; i < kSteps; ++i)
    {
        float t  = (float(i) + j) * dt;
        vec3  p  = e + rd * t;
        float h  = exp(-max(p.y - e.y + 2.0, 0.0) * 0.06); // thicker low down
        float sf = fogSigma * h;
        float ss = shaftSigma * h;
        float v  = (ss > 0.0 && sunOn > 0.0) ? visibility(p, l) : 0.0;
        ins += trans * (sf * fogLin * 0.8 + ss * lightLin * phase * 12.566 * v) * dt;
        trans *= exp(-sf * dt * 0.5);
    }
    imageStore(outVol, px, vec4(ins, trans));
}
