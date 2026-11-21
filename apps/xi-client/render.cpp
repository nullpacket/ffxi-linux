// SPDX-License-Identifier: GPL-3.0-or-later
// Recording and submitting the frame; screenshots.
#include "client_app.h"

namespace xi::app
{

gfx::ContextExtensions ClientApp::prepareDlss(const Args& a)
{
    if (!gfx::dlssBuilt())
    {
        return {};
    }
    if (const auto mode = gfx::parseDlssMode(a.dlss))
    {
        gfx::TextureSet::lodBias() = gfx::dlssLodBias(*mode);
    }
    return gfx::Dlss::extensions();
}

void ClientApp::startDlss()
{
    const auto mode = gfx::parseDlssMode(args.dlss);
    if (!mode)
    {
        spdlog::warn("dlss: unknown mode '{}' (quality, balanced, performance, ultra-performance, dlaa); rendering without DLSS", args.dlss);
        return;
    }
    auto d = std::make_unique<gfx::Dlss>(ctx, *mode);
    if (!d->ok())
    {
        gfx::TextureSet::lodBias() = 0.0f; // zone textures made from now on sample as without DLSS
        return;
    }
    spdlog::info("dlss: texture LOD bias {:.2f}", gfx::TextureSet::lodBias());
    dlss_.dlss   = std::move(d);
    dlss_.motion = std::make_unique<gfx::MotionVectors>(ctx);
    sceneTarget.setSampled(true); // DLSS samples the scene's colour and depth
}

void ClientApp::startFsr()
{
    // --dlss's mode names work too (dlaa = native), for the fallback when DLSS didn't start.
    const std::string want = !args.fsr.empty() ? args.fsr : args.dlss;
    const auto        mode = gfx::parseFsrMode(want);
    if (!mode)
    {
        spdlog::warn(
            "fsr: unknown mode '{}' (quality, balanced, performance, ultra-performance, native); rendering without FSR",
            want);
        return;
    }
    if (!args.dlss.empty() && args.fsr.empty())
    {
        spdlog::info("upscaler: DLSS did not start; trying FSR {}", gfx::fsrModeName(*mode));
    }
    auto f = std::make_unique<gfx::Fsr>(ctx, *mode, args.fsrSharpness);
    if (!f->ok())
    {
        return;
    }
    // Texture sets made from now on sample with FSR's LOD bias (the zone's are made after this, at zone-in).
    gfx::TextureSet::lodBias() = gfx::fsrLodBias(*mode);
    spdlog::info("fsr: texture LOD bias {:.2f}", gfx::TextureSet::lodBias());
    dlss_.dlss   = std::move(f);
    dlss_.motion = std::make_unique<gfx::MotionVectors>(ctx);
    sceneTarget.setSampled(true); // FSR samples the scene's colour and depth
}

gfx::ContextExtensions ClientApp::prepareExtensions(const Args& a)
{
    auto e     = a.dlss.empty() ? gfx::ContextExtensions{} : prepareDlss(a);
    e.rayQuery = a.rt || a.pathtrace;
    return e;
}

void ClientApp::startRt()
{
    // --rt-boost without --rt / --rt=: every effect.
    const auto opts = gfx::parseRtOptions(!args.rtBoost.empty() && !args.rtEffectsGiven ? std::string_view("all") : std::string_view(args.rtEffects));
    if (!opts)
    {
        spdlog::warn("rt: unknown effect list '{}' (ao, shadows, gi, reflections, all); rendering without ray tracing", args.rtEffects);
        return;
    }
    const auto pto = args.pathtrace ? gfx::parsePtOptions(args.ptBounces) : std::nullopt;
    if (args.pathtrace && !pto)
    {
        spdlog::warn("pathtrace: unknown value '{}' (1, 2 or 3 bounces); rendering without path tracing", args.ptBounces);
    }
    auto o     = *opts;
    o.strength = args.rtStrength;
    if (!args.rtBoost.empty())
    {
        if (!gfx::applyRtBoost(args.rtBoost, o))
        {
            spdlog::warn("rt: unknown --rt-boost level '{}' (low, high, max); using goal 38's tuning", args.rtBoost);
        }
        else if (args.rtStrengthGiven)
        {
            o.strength = args.rtStrength; // an explicit --rt-strength wins over the preset's
        }
    }
    if (args.rtShadowCap)
    {
        o.shadowCap = *args.rtShadowCap;
    }
    if (args.rtReflect)
    {
        o.reflect = *args.rtReflect;
    }
    if (pto)
    {
        // The path tracer lights everything itself: of --rt only the water reflections (drawn after the effects) stay.
        o.ao = o.shadows = o.points = o.gi = false;
        o.reflections = o.reflections && args.rt;
    }
    spdlog::info("rt: strength {:.2f}, shadow cap {:.2f}, reflect {:.2f}, bounce {:.2f}, AO radius {:.1f}", o.strength, o.shadowCap, o.reflect, o.bounce, o.aoRadius);
    auto r     = std::make_unique<gfx::RayTracing>(ctx, o);
    if (!r->ok())
    {
        return;
    }
    rt_ = std::move(r);
    sceneTarget.setSampled(true); // the passes read the scene's depth
    const bool rr = dlss_.dlss && dlss_.dlss->ok() && dlss_.dlss->rayReconstructionAvailable();
    if (pto)
    {
        auto p     = *pto;
        p.blend    = args.ptBlend;
        p.exposure = args.ptExposure;
        p.baked    = args.ptBaked;
        pt_        = std::make_unique<gfx::PathTracer>(ctx, *rt_, p);
        if (rr)
        {
            pt_->setRayReconstruction(true); // --dlss: DLSS Ray Reconstruction denoises the paths
            spdlog::info("pathtrace: DLSS Ray Reconstruction denoises");
        }
    }
    else if (rr)
    {
        rt_->setRayReconstruction(true); // --dlss: DLSS Ray Reconstruction denoises the traced terms
        spdlog::info("rt: DLSS Ray Reconstruction denoises");
    }
}

void ClientApp::startPost()
{
    const auto opts = gfx::parsePostOptions(args.post == "all" ? std::string_view{} : std::string_view(args.post));
    if (!opts)
    {
        spdlog::warn("post: unknown effect list '{}' (hdr, bloom, godrays, fog, all); rendering without post-processing", args.post);
        return;
    }
    auto o = *opts;
    o.bloomStrength *= args.postBloom;
    o.shaftStrength *= args.postShafts;
    o.fogDensity *= args.postFog;
    post_ = std::make_unique<gfx::PostProcess>(ctx, o, rt_ && rt_->ok());
    if (pt_)
    {
        pt_->setToneMapped(true); // --pathtrace leaves the highlights to --post's tone map
    }
    sceneTarget.setSampled(true); // the passes read the scene's depth
}

void ClientApp::rtLighting(VkCommandBuffer cmd, uint32_t slot, VkExtent2D render, const glm::mat4& vpScene, const gfx::SceneParams& sp,
                           const gfx::SkyRenderer::Gradient& sky)
{
    auto& r = *rt_;
    r.beginFrame(slot);
    r.addMeshes(scene->meshRenderer(), 0);
    if (subScene)
    {
        r.addMeshes(subScene->meshRenderer(), 1);
    }
    if (propLayer)
    {
        propLayer->forEachMeshes([&](const gfx::MeshRenderer& m) { r.addMeshes(m, 2); });
    }
    skinned.rtRemember(charInstances);
    r.addActors(skinned, cam.position, 80.0f);
    r.build(cmd);
    const auto& depth = sceneTarget.depth();
    gfx::Context::transition(cmd, depth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    const bool newZone = rtState_.prevScene != scene.get() || rtState_.prevZone != world.zone;
    if (r.options().reflections && (newZone || frameTick % 15 == 0)) // the sea hardly moves: every 15 frames
    {
        // The water to mirror off: a harbour's sea is an effect (the zone's `sea` Mmbs drawn by the effect pass, so
        // not in the traced geometry); its flat, wide layers this frame give the surface's height and extent.
        rtState_.water.clear();
        for (const auto& fb : fxBatches)
        {
            if (fb.vertices.size() < 6 || fb.screen || rtState_.water.size() >= 2 * gfx::RayTracing::kMaxWaterPlanes)
            {
                continue;
            }
            glm::vec3 lo(fb.vertices[0].pos), hi(lo);
            bool      flat = true;
            for (const auto& vx : fb.vertices)
            {
                lo = glm::min(lo, vx.pos);
                hi = glm::max(hi, vx.pos);
                if (hi.y - lo.y > 0.15f)
                {
                    flat = false;
                    break;
                }
            }
            static const bool trace = std::getenv("XI_RT_WATER_TRACE") != nullptr;
            if (trace && frameTick % 120 == 0)
            {
                glm::vec3 l2(fb.vertices[0].pos), h2(l2);
                for (const auto& vx : fb.vertices)
                {
                    l2 = glm::min(l2, vx.pos);
                    h2 = glm::max(h2, vx.pos);
                }
                if (h2.x - l2.x > 8.0f && h2.z - l2.z > 8.0f)
                {
                    spdlog::info("rt water trace: batch {} verts key {} blend {} x {:.0f}..{:.0f} y {:.2f}..{:.2f} z {:.0f}..{:.0f}", fb.vertices.size(), fb.depthKey,
                                 static_cast<int>(fb.blend), l2.x, h2.x, l2.y, h2.y, l2.z, h2.z);
                }
            }
            if (!flat || hi.x - lo.x < 16.0f || hi.z - lo.z < 16.0f)
            {
                continue;
            }
            const glm::vec3 a = gfx::ZoneScene::gameToRender(lo), b = gfx::ZoneScene::gameToRender(hi);
            const glm::vec4 box(std::min(a.x, b.x), std::min(a.z, b.z), std::max(a.x, b.x), std::max(a.z, b.z));
            const float     y = std::max(a.y, b.y); // the top layer of the sea's stack
            bool            merged = false;
            for (std::size_t i = 0; i < rtState_.water.size(); i += 2)
            {
                if (std::abs(rtState_.water[i + 1].x - y) < 0.3f)
                {
                    auto& w = rtState_.water[i];
                    w       = glm::vec4(std::min(w.x, box.x), std::min(w.y, box.y), std::max(w.z, box.z), std::max(w.w, box.w));
                    merged  = true;
                    break;
                }
            }
            if (!merged)
            {
                rtState_.water.push_back(box);
                rtState_.water.push_back(glm::vec4(y, 0.0f, 0.0f, 0.0f));
            }
        }
        if (rtState_.water.size() != rtState_.waterLogged)
        {
            rtState_.waterLogged = rtState_.water.size();
            spdlog::info("rt: {} water planes from the effects", rtState_.water.size() / 2);
            for (std::size_t i = 0; i < rtState_.water.size(); i += 2)
            {
                const auto& w = rtState_.water[i];
                spdlog::debug("rt: water y {:.2f} box x {:.0f}..{:.0f} z {:.0f}..{:.0f}", rtState_.water[i + 1].x, w.x, w.z, w.y, w.w);
            }
        }
    }
    gfx::RayTracing::View v;
    v.viewProj     = vpScene;
    v.prevViewProj = newZone ? vpScene : rtState_.prevViewProj;
    v.eye          = cam.position;
    v.reset        = newZone || glm::distance(rtState_.prevEye, cam.position) > 8.0f;
    constexpr float kIndoor = 0.2f;
    const float     goal    = scene->roofedAt(feet.x, feet.z, feet.y + 0.51f) ? kIndoor : 1.0f;
    rtState_.sunShadow      = newZone ? goal : rtState_.sunShadow + (goal - rtState_.sunShadow) * std::min(1.0f, dt * 2.0f);
    v.sunShadow             = rtState_.sunShadow;
    v.water                 = &rtState_.water;
    const auto& zt = scene->textureSet();
    if (r.anyEffect()) // with --pathtrace alone the ray tracer only builds the geometry
    {
        r.trace(cmd, depth, render, v, sp, zt.set(), subScene ? subScene->textureSet().set() : VK_NULL_HANDLE, zt.layout());
        r.composite(cmd, sceneTarget.color(), sceneTarget.colorView(), depth, render, v, sp);
    }
    if (pt_)
    {
        gfx::PathTracer::View pv;
        pv.viewProj     = v.viewProj;
        pv.prevViewProj = v.prevViewProj;
        pv.eye          = v.eye;
        pv.reset        = v.reset;
        pv.sunShadow    = v.sunShadow;
        pt_->render(cmd, slot, sceneTarget.color(), sceneTarget.colorView(), depth, render, pv, sp, sky, zt.set(),
                    subScene ? subScene->textureSet().set() : VK_NULL_HANDLE);
    }
    gfx::Context::transition(cmd, depth.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    rtState_.prevViewProj = vpScene;
    rtState_.prevEye      = cam.position;
    rtState_.prevScene    = scene.get();
    rtState_.prevZone     = world.zone;
}

void ClientApp::drawEffectsOver(VkCommandBuffer cmd, uint32_t slot, VkImageView dst, VkExtent2D ext, glm::vec2 jitterPx)
{
    // The window's depth image (otherwise unused once the scene is offscreen) takes the scene's depth at the window
    // size, then the effects draw over the upscaled colour with the unjittered camera.
    VkRenderingAttachmentInfo c{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    c.imageView   = dst;
    c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    c.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    c.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo d{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    d.imageView               = ctx.depth().view;
    d.imageLayout             = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    d.loadOp                  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    d.storeOp                 = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    d.clearValue.depthStencil = { 0.0f, 0 };
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, ext };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &c;
    ri.pDepthAttachment     = &d;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
    const VkRect2D   sc{ { 0, 0 }, ext };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    dlss_.motion->drawDepth(cmd, sceneTarget.depth(), ext, jitterPx);
    effectRenderer.draw(cmd, slot, viewProj, fxBatches);
    vkCmdEndRendering(cmd);
    gfx::Context::transition(cmd, ctx.depth().image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
}

bool ClientApp::dlssResolve(VkCommandBuffer cmd, uint32_t slot, VkImage dst, VkImageView dstView, VkExtent2D ext,
                            VkExtent2D render, const glm::mat4& vpScene, glm::vec2 jitterPx, glm::vec2 jitterNdc,
                            bool fxAfter)
{
    // The window-size output DLSS writes (storage), then copies into the swap chain image.
    if (!dlss_.output || dlss_.output->extent.width != ext.width || dlss_.output->extent.height != ext.height)
    {
        ctx.waitIdle();
        auto* c       = &ctx;
        dlss_.output = std::shared_ptr<gfx::Image>(new gfx::Image(ctx.createImage(ext, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                                                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                                                    VK_IMAGE_ASPECT_COLOR_BIT)),
                                                   [c](gfx::Image* i) {
                                                       c->waitIdle();
                                                       c->destroyImage(*i);
                                                       delete i;
                                                   });
        dlss_.history = false;
    }
    // A cut (zone change, event camera jump) drops the history: DLSS would smear the old view into the new one.
    const bool newZone = dlss_.prevScene != scene.get() || dlss_.prevZone != world.zone;
    const bool cut     = !dlss_.history || newZone || glm::distance(dlss_.prevEye, cam.position) > 8.0f;
    if (cut && dlss_.history)
    {
        spdlog::info("upscaler: {} history reset ({})", dlss_.dlss->name(), newZone ? "new zone" : "camera cut");
    }
    const glm::mat4 prev = cut ? viewProj : dlss_.prevViewProj;
    auto& mv = *dlss_.motion;
    mv.ensure(render);
    const auto& depth = sceneTarget.depth();
    mv.drawCamera(cmd, depth, vpScene, prev, jitterNdc);
    // Characters over it with their own motion, depth-tested against the scene.
    VkRenderingAttachmentInfo mc{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    mc.imageView   = mv.image().view;
    mc.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    mc.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    mc.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo md{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    md.imageView   = depth.view;
    md.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    md.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    md.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo mri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    mri.renderArea           = { { 0, 0 }, render };
    mri.layerCount           = 1;
    mri.colorAttachmentCount = 1;
    mri.pColorAttachments    = &mc;
    mri.pDepthAttachment     = &md;
    vkCmdBeginRendering(cmd, &mri);
    const VkViewport vp{ 0, 0, static_cast<float>(render.width), static_cast<float>(render.height), 0, 1 };
    const VkRect2D   sc{ { 0, 0 }, render };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    skinned.drawMotion(cmd, slot, vpScene, mv.frameBlock(slot, viewProj, prev), charInstances);
    vkCmdEndRendering(cmd);

    const auto& color = sceneTarget.color();
    gfx::Context::transition(cmd, color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    gfx::Context::transition(cmd, depth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    gfx::Context::transition(cmd, mv.image().image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    gfx::Context::transition(cmd, dlss_.output->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    gfx::Dlss::Inputs in;
    in.color     = &color;
    in.colorView = sceneTarget.colorView();
    in.depth     = &depth;
    in.motion    = &mv.image();
    in.output    = dlss_.output.get();
    in.render    = render;
    in.jitter    = jitterPx;
    in.reset     = cut;
    const auto tick = std::chrono::steady_clock::now();
    in.frameMs      = upscaleLast_.time_since_epoch().count()
                          ? std::chrono::duration<float, std::milli>(tick - upscaleLast_).count()
                          : 16.7f;
    upscaleLast_    = tick;
    in.fovY         = cam.fovY;
    in.nearZ        = cam.nearZ;
    if (pt_ && pt_->rayReconstruction() && pt_->guideAlbedo().image) // --pathtrace: Ray Reconstruction with its guides
    {
        in.diffuseAlbedo  = &pt_->guideAlbedo();
        in.specularAlbedo = &pt_->guideSpecular();
        in.normals        = &pt_->guideNormal();
        in.worldToView    = cam.view();
        in.viewToClip     = viewProj * glm::inverse(cam.view());
    }
    else if (rt_ && rt_->rayReconstruction() && rt_->guideAlbedo().image) // --rt: Ray Reconstruction with the guides
    {
        in.diffuseAlbedo  = &rt_->guideAlbedo();
        in.specularAlbedo = &rt_->guideSpecular();
        in.normals        = &rt_->guideNormal();
        in.worldToView    = cam.view();
        in.viewToClip     = viewProj * glm::inverse(cam.view());
    }
    const bool ok = dlss_.dlss->evaluate(cmd, in);
    if (pt_ && pt_->rayReconstruction() && !dlss_.dlss->rayReconstructionAvailable())
    {
        pt_->setRayReconstruction(false); // Ray Reconstruction refused: the path tracer's own filter from the next frame
    }
    if (rt_ && rt_->rayReconstruction() && !dlss_.dlss->rayReconstructionAvailable())
    {
        rt_->setRayReconstruction(false); // Ray Reconstruction refused: the passes' own denoiser from the next frame
    }
    dlss_.prevViewProj = viewProj;
    dlss_.prevEye      = cam.position;
    dlss_.prevScene    = scene.get();
    dlss_.prevZone     = world.zone;
    dlss_.history      = ok;
    if (!ok)
    {
        // Back to the plain scaling (SceneTarget::resolve wants mip 0 as a colour attachment).
        gfx::Context::transition(cmd, color.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        return false;
    }
    // --post works on DLSS's output, so DLSS gets the scene before any of it (bloom is not upscaled).
    if (post_ && !fxAfter)
    {
        post_->apply(cmd, slot, dlss_.output->image, VK_IMAGE_LAYOUT_GENERAL, ext, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, postView_);
    }
    gfx::Context::transition(cmd, dlss_.output->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    gfx::Context::transition(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.srcOffsets[1]  = { static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), 1 };
    b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.dstOffsets[1]  = { static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), 1 };
    vkCmdBlitImage(cmd, dlss_.output->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    gfx::Context::transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (fxAfter) // the effects over the upscaled scene, then --post on both (as without DLSS)
    {
        drawEffectsOver(cmd, slot, dstView, ext, jitterPx);
        if (post_)
        {
            post_->apply(cmd, slot, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, ext, depth,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, postView_);
        }
    }
    return true;
}

bool ClientApp::renderFrame()
{
    // --- Render ---
    gfx::Context::Frame frame;
    if (!ctx.beginFrame(frame))
    {
        ImGui_ImplVulkan_SetMinImageCount(2);
        return false;
    }
    frameTrace.mark(gfx::FrameTrace::Wait);
    const auto ext = ctx.extent();
    auto       cmd = frame.cmd;
    // The scene renders at the background resolution: offscreen when it differs from the window.
    VkExtent2D bgExt = args.display_.bgWidth > 0 && args.display_.bgHeight > 0
                           ? VkExtent2D{ static_cast<uint32_t>(args.display_.bgWidth), static_cast<uint32_t>(args.display_.bgHeight) }
                           : ext;
    // --dlss (goal 15): the scene at DLSS's render size with a sub-pixel jitter, upscaled to the window before the
    // interface. Without the flag `vpScene` is viewProj and nothing below changes.
    const bool useDlss  = dlss_.dlss && dlss_.dlss->ok() && scene;
    glm::mat4  vpScene  = viewProj;
    glm::vec2  jitterPx{ 0.0f }, jitterNdc{ 0.0f };
    if (useDlss)
    {
        bgExt     = dlss_.dlss->renderExtent(ext);
        jitterPx  = gfx::dlssJitter(dlss_.frame++, static_cast<float>(ext.width) / static_cast<float>(std::max(1u, bgExt.width)));
        jitterNdc = { 2.0f * jitterPx.x / static_cast<float>(bgExt.width), 2.0f * jitterPx.y / static_cast<float>(bgExt.height) };
        vpScene   = gfx::jitterViewProj(viewProj, jitterNdc);
    }
    // --rt (goal 38): the scene offscreen too (its depth is read); nothing changes without the flag.
    const bool useRt     = rt_ && scene;
    const bool usePost   = post_ && scene; // --post (goal 41): likewise
    const bool offscreen = useDlss || useRt || usePost || bgExt.width != ext.width || bgExt.height != ext.height;
    effectRenderer.ensureScreen(bgExt); // before any frame needs the copies (it waited mid-zone-in, goal 25)
    effectRenderer.beginFrame(frame.slot);
    // Issue #101: with Ray Reconstruction the effects draw over DLSS's output rather than going through it. It takes its
    // input as noisy lit surfaces (the guides hold the surfaces behind the effects), so glows, rings and particles
    // came out smoothed into the background. Not with --rt's water reflection, which goes over the sea the effect pass
    // draws in the scene. The screen-sampling effects stay in the scene (their copies are at the render size).
    const bool rayRecon    = (pt_ && pt_->rayReconstruction()) || (rt_ && rt_->rayReconstruction());
    const bool fxAfterDlss = useDlss && rayRecon && !(rt_ && rt_->options().reflections);
    if (offscreen)
    {
        sceneTarget.ensure(bgExt);
        gfx::Context::transition(cmd, sceneTarget.colorImage(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        gfx::Context::transition(cmd, sceneTarget.depth().image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    else
    {
        gfx::Context::transition(cmd, frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    gfx::Context::transition(cmd, ctx.depth().image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    gfx::SceneParams           sp;
    gfx::SceneParams           actorSp;
    gfx::SkyRenderer::Gradient skyGrad;
    sp.eye      = cam.position;
    actorSp.eye = cam.position;
    actorSp.camRight = cam.right(), actorSp.camForward = cam.forward(), actorSp.camUp = glm::normalize(glm::cross(cam.right(), cam.forward()));
    if (scene)
    {
        const double   tod     = args.timeOverride >= 0 ? args.timeOverride : eventTimeOfDay();
        const uint16_t weather = args.weatherOverride >= 0 ? static_cast<uint16_t>(args.weatherOverride) : world.weather;
        if (weather != renderState_.shownWeather)
        {
            renderState_.fromWeather    = renderState_.shownWeather == 0xFFFF ? weather : renderState_.shownWeather;
            renderState_.shownWeather   = weather;
            renderState_.weatherChanged = now;
        }
        // Weather changes cross-fade (client: over the packet's OffsetTime; here a fixed 10 s).
        const auto& env = effects.environment();
        auto rec = !env.record.empty() ? dat::ZoneWeather::decodeRecord(env.record)
                   : !env.fourcc.empty() || env.altTable ? scene->weather().sample(std::string_view(weatherGroup()), tod)
                                                         : scene->weather().sample(weather, tod);
        const float fade = std::chrono::duration<float>(now - renderState_.weatherChanged).count() / 10.0f;
        if (renderState_.fromWeather != weather && fade < 1.0f && env.fourcc.empty() && env.record.empty() && !env.altTable)
        {
            rec = dat::ZoneWeather::lerp(scene->weather().sample(renderState_.fromWeather, tod), rec, fade);
        }
        // An effect's environment request (ops 0x7C / 0xAE / 0x7E) cross-fades from what was shown over its time.
        if (env.serial != renderState_.envSerial)
        {
            renderState_.envSerial  = env.serial;
            renderState_.envFrom    = renderState_.lastWeatherRec;
            renderState_.envBlend   = env.blendSeconds;
            renderState_.envChanged = now;
        }
        if (const float t = renderState_.envBlend > 0 ? std::chrono::duration<float>(now - renderState_.envChanged).count() / renderState_.envBlend : 1.0f; t < 1.0f)
        {
            rec = dat::ZoneWeather::lerp(renderState_.envFrom, rec, t);
        }
        renderState_.lastWeatherRec = rec;
        // Config Misc. 2: the clipping plane (cfg 62, 0..25; CODE 0x101aa37c / 0x101891c0) scales the fog
        // distances by f = (v + 25) / 50, and by another 0.5 below the full setting when they reach past 500.
        if (const float f = static_cast<float>(std::clamp(cfg(62, 25), 0, 25) + 25) * 0.02f; f < 0.99f)
        {
            const float k = f * (rec.fogEndZ > 500.0f ? 0.5f : 1.0f);
            rec.fogStartZ *= k, rec.fogEndZ *= k, rec.fogStartA *= k, rec.fogEndA *= k;
        }
        // Ashita's drawdistance (addon_native.cpp): factors on the zone's and the actors' fog distances.
        if (addonOverrides_.drawWorld != 1.0f)
        {
            rec.fogStartZ *= addonOverrides_.drawWorld, rec.fogEndZ *= addonOverrides_.drawWorld;
        }
        if (addonOverrides_.drawEntity != 1.0f)
        {
            rec.fogStartA *= addonOverrides_.drawEntity, rec.fogEndA *= addonOverrides_.drawEntity;
        }
        gfx::applyEnvironment(rec, tod, sp, skyGrad);
        gfx::applyActorEnvironment(rec, tod, actorSp);
        const auto brighter = [](const gfx::SceneParams& p) {
            const glm::vec3 a = p.light0Color + p.ambient, b = p.light1Color + p.ambient;
            return glm::dot(b, b) > glm::dot(a, a) ? b : a;
        };
        effects.setLights(brighter(actorSp), brighter(sp));
        // Zone point lights (lanterns) by the Mzb light table: each index gets the light generator of that name.
        sp.pointLights.assign(std::min(scene->lightKeys().size(), gfx::SceneParams::kMaxPointLights), {});
        for (const auto& l : effects.pointLights())
        {
            for (std::size_t k = 0; k < sp.pointLights.size(); ++k)
            {
                if (scene->lightKeys()[k] == l.key)
                {
                    sp.pointLights[k] = { gfx::ZoneScene::gameToRender(l.pos), l.range, l.colour };
                }
            }
        }
        actorSp.eye = cam.position;
        actorSp.camRight = cam.right(), actorSp.camForward = cam.forward(), actorSp.camUp = glm::normalize(glm::cross(cam.right(), cam.forward()));
        if (usePost)
        {
            postView_.viewProj   = vpScene;
            postView_.eye        = cam.position;
            postView_.lightDir   = sp.light0Dir;
            postView_.lightColor = sp.light0Color;
            postView_.ambient    = sp.ambient;
            postView_.fogColor   = sp.fogColor;
            postView_.fogEnd     = sp.fogEnd;
            postView_.tlas       = VK_NULL_HANDLE;
            postView_.frame      = static_cast<uint32_t>(frameTick);
        }
    }
    if (scene)
    {
        scene->updateLod(cam.position); // h / m / l meshes by the camera's distance
        scene->flush(cmd);              // moved door parts, LOD changes
    }
    if (subScene)
    {
        subScene->updateLod(cam.position);
        subScene->flush(cmd);
    }
    if (propLayer)
    {
        propLayer->flush(cmd); // ships along their paths
    }
    VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView        = offscreen ? sceneTarget.colorView() : frame.view;
    color.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = { { sp.skyColor.r, sp.skyColor.g, sp.skyColor.b, 1.0f } };
    if (const auto& c = addonOverrides_.clearColour) // Ashita's clearcolor: D3DCOLOR (ARGB)
    {
        color.clearValue.color = { { static_cast<float>((*c >> 16) & 0xFF) / 255.0f, static_cast<float>((*c >> 8) & 0xFF) / 255.0f,
                                     static_cast<float>(*c & 0xFF) / 255.0f, 1.0f } };
    }
    VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    depth.imageView               = offscreen ? sceneTarget.depth().view : ctx.depth().view;
    depth.imageLayout             = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp                  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp                 = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = { 0.0f, 0 };
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, bgExt };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    ri.pDepthAttachment     = &depth;
    // Effects that sample the screen (rings, haze, freeze frames) need the scene copied mid-pass: keep depth.
    const auto fxLayers = effects.screenLayers();
    const bool fxScreen = effectRenderer.needsScreen(fxBatches, fxLayers, effects.freezeSerial()) || (scene && gfx::SkinnedRenderer::needsScreen(charInstances));
    // Detailed shadows: the silhouettes render between the characters and the effects (rendering resumes after them).
    const bool silhouettes = scene && !shadows.silhouettes().empty();
    if (fxScreen || silhouettes || useDlss || useRt || usePost) // DLSS, the ray tracing and the post passes read the depth
    {
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{ 0, 0, static_cast<float>(bgExt.width), static_cast<float>(bgExt.height), 0, 1 };
    VkRect2D   sc{ { 0, 0 }, bgExt };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    if (scene)
    {
        const float aspect2 = gfx::gameAspect(static_cast<float>(ext.width) / static_cast<float>(std::max(1u, ext.height)));
        skyRenderer.draw(cmd, frame.slot, cam.forward(), cam.right(), glm::normalize(glm::cross(cam.right(), cam.forward())), cam.fovY, aspect2, skyGrad);
        scene->draw(cmd, frame.slot, vpScene, sp);
        if (propLayer)
        {
            propLayer->draw(cmd, frame.slot, vpScene, sp);
        }
        if (subScene)
        {
            subScene->draw(cmd, frame.slot, vpScene, sp); // an event's sub-map adds to the zone (furniture, rooms)
        }
        skinned.draw(cmd, frame.slot, vpScene, actorSp, charInstances);
        shadows.setLight(actorSp.light0Dir); // next frame's silhouettes
        if (silhouettes)
        {
            vkCmdEndRendering(cmd);
            skinned.drawSilhouettes(cmd, effectRenderer.renderTexture(shadows.silhouetteKey(), { GroundShadows::kAtlas, GroundShadows::kAtlas }),
                                    charInstances, shadows.silhouettes());
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
        }
        if (useRt) // the zone, props and characters are drawn: re-light them before the effects go over them
        {
            vkCmdEndRendering(cmd);
            rtLighting(cmd, frame.slot, bgExt, vpScene, sp, skyGrad);
            postView_.tlas = rt_->tlas(); // --post's light shafts test it
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
        }
    }
    markers.draw(cmd, frame.slot, vpScene, mk);
    if (!fxAfterDlss)
    {
        effectRenderer.draw(cmd, frame.slot, vpScene, fxBatches);
    }
    if (useRt && rt_->options().reflections) // the water reflection goes over the sea the effect pass drew
    {
        vkCmdEndRendering(cmd);
        rt_->reflect(cmd, sceneTarget.colorView(), bgExt);
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }
    if (fxScreen)
    {
        vkCmdEndRendering(cmd);
        effectRenderer.copyScene(cmd, offscreen ? sceneTarget.colorImage() : frame.image, bgExt, effects.freezeSerial());
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        if (scene) // effect ops 0x48 / 0x49: fading characters refract the scene behind them
        {
            skinned.drawRefraction(cmd, vpScene, charInstances, effectRenderer.screenLayout(), effectRenderer.screenSet());
        }
        effectRenderer.drawScreen(cmd, frame.slot, vpScene, fxBatches, fxLayers);
    }
    if (offscreen)
    {
        // Scale the scene into the window, then draw the interface at window resolution.
        vkCmdEndRendering(cmd);
        if (usePost && !useDlss) // with DLSS it runs on DLSS's output (dlssResolve)
        {
            post_->apply(cmd, frame.slot, sceneTarget.colorImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, bgExt, sceneTarget.depth(),
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, postView_);
        }
        if (!useDlss || !dlssResolve(cmd, frame.slot, frame.image, frame.view, ext, bgExt, vpScene, jitterPx, jitterNdc,
                                     fxAfterDlss))
        {
            sceneTarget.resolve(cmd, frame.image, ext);
            if (fxAfterDlss) // DLSS failed this frame: the effects over the plain scaling
            {
                drawEffectsOver(cmd, frame.slot, frame.view, ext, jitterPx);
            }
        }
        VkRenderingAttachmentInfo uiColor{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        uiColor.imageView   = frame.view;
        uiColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        uiColor.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        uiColor.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo uiDepth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        uiDepth.imageView   = ctx.depth().view; // unused, but the interface pipelines declare a depth format
        uiDepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        uiDepth.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        uiDepth.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkRenderingInfo uri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        uri.renderArea           = { { 0, 0 }, ext };
        uri.layerCount           = 1;
        uri.colorAttachmentCount = 1;
        uri.pColorAttachments    = &uiColor;
        uri.pDepthAttachment     = &uiDepth;
        vkCmdBeginRendering(cmd, &uri);
        const VkViewport uvp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
        const VkRect2D   usc{ { 0, 0 }, ext };
        vkCmdSetViewport(cmd, 0, 1, &uvp);
        vkCmdSetScissor(cmd, 0, 1, &usc);
    }
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
    if (gammaPass.active()) // Config gamma: the whole frame, as the game's ramp
    {
        gammaPass.apply(cmd, frame.image, frame.view, ext);
    }
    gfx::Buffer readback;
    if (captureNext)
    {
        readback = ctx.createBuffer(VkDeviceSize{ ext.width } * ext.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        gfx::Context::transition(cmd, frame.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent      = { ext.width, ext.height, 1 };
        vkCmdCopyImageToBuffer(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        gfx::Context::transition(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    else
    {
        gfx::Context::transition(cmd, frame.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    frameTrace.mark(gfx::FrameTrace::Record);
    ctx.endFrame(frame);
    frameTrace.mark(gfx::FrameTrace::Submit);
    if (captureNext)
    {
        ctx.waitIdle();
        const bool bgr = ctx.colorFormat() == VK_FORMAT_B8G8R8A8_SRGB || ctx.colorFormat() == VK_FORMAT_B8G8R8A8_UNORM;
        std::vector<uint8_t> px(static_cast<const uint8_t*>(readback.mapped), static_cast<const uint8_t*>(readback.mapped) + readback.size);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            if (bgr)
            {
                std::swap(px[i], px[i + 2]);
            }
            px[i + 3] = 255;
        }
        const std::string out = shotPath.empty() ? args.screenshot : shotPath;
        dat::writePng(out, ext.width, ext.height, px);
        ctx.destroyBuffer(readback);
        spdlog::info("screenshot written to {}", out);
        captureNext = false;
        quitting    = quitting || shotPath.empty(); // the --screenshot test hook quits; the key does not
        shotPath.clear();
    }
    return true;
}

} // namespace xi::app
