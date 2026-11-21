// SPDX-License-Identifier: GPL-3.0-or-later
#define VMA_IMPLEMENTATION
#include "gfx/context.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace xi::gfx
{

namespace
{

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        spdlog::error("vulkan: {}", data->pMessage);
    }
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        spdlog::warn("vulkan: {}", data->pMessage);
    }
    return VK_FALSE;
}

} // namespace

Context::Context(SDL_Window* window, bool validation, const ContextExtensions* extra)
: window_(window)
{
    // Instance
    Uint32             sdlExtCount = 0;
    const char* const* sdlExts     = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    std::vector<const char*> exts(sdlExts, sdlExts + sdlExtCount);
    std::vector<const char*> layers;
    if (validation)
    {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    if (extra)
    {
        for (const auto& e : extra->instance)
        {
            if (std::ranges::none_of(exts, [&](const char* x) { return e == x; }))
            {
                exts.push_back(e.c_str());
            }
        }
    }
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ffxi-linux";
    app.apiVersion       = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames     = layers.data();
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance_));

    if (validation)
    {
        VkDebugUtilsMessengerCreateInfoEXT dci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debugCallback;
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create)
        {
            VK_CHECK(create(instance_, &dci, nullptr, &debug_));
        }
    }

    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_))
    {
        throw std::runtime_error(fmt::format("SDL_Vulkan_CreateSurface: {}", SDL_GetError()));
    }

    // Physical device: prefer discrete GPU with a graphics+present queue.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    int              bestScore = -1;
    const GpuPretend pretend   = gpuPretendFromEnv();
    std::string      rejected; // why devices were passed over, for the error below
    for (auto pd : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        // Every feature the device below is created with, checked first: a GPU without one is passed over with a
        // reason instead of failing vkCreateDevice (issue #139).
        const DeviceSupport ds = checkDevice(pd, pretend);
        if (!ds.missing.empty())
        {
            std::string what;
            for (const auto& m : ds.missing)
            {
                what += (what.empty() ? "" : ", ") + m;
            }
            spdlog::warn("gpu: {} lacks {}; not using it", props.deviceName, what);
            rejected += fmt::format("; {} lacks {}", props.deviceName, what);
            continue;
        }
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qprops.data());
        for (uint32_t q = 0; q < qcount; ++q)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
            if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            {
                const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > bestScore)
                {
                    bestScore    = score;
                    physical_    = pd;
                    queueFamily_ = q;
                    support_     = ds;
                    vendorId_    = pretend.vendor.empty() ? props.vendorID : pretend.vendorId;
                    timestampMs_ = qprops[q].timestampValidBits != 0 ? props.limits.timestampPeriod * 1e-6 : 0.0;
                    std::memcpy(deviceName_.data(), props.deviceName, deviceName_.size() - 1);
                }
                break;
            }
        }
    }
    if (!physical_)
    {
        throw std::runtime_error("no Vulkan 1.3 GPU with graphics+present support and the features the client needs" +
                                 rejected);
    }
    if (!pretend.vendor.empty())
    {
        spdlog::info(
            "gpu: XI_GPU_PRETEND={}: treating {} as vendor {:#06x}, limits clamped (push constants {} B, subgroups {}-{})",
            pretend.vendor, deviceName_.data(), vendorId_, support_.limits.maxPushConstantsSize,
            support_.limits.minSubgroupSize, support_.limits.maxSubgroupSize);
    }

    // Device
    const float             prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE; // GLSL discard under SPIR-V 1.6
    f13.subgroupSizeControl            = support_.subgroupControl ? VK_TRUE : VK_FALSE; // FSR's wave64 passes (AMD)
    f13.computeFullSubgroups           = support_.subgroupControl ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceVulkan11Features f11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    f11.pNext                = &f13;
    f11.shaderDrawParameters = VK_TRUE; // gl_DrawID for multi-draw-indirect
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.pNext                                     = &f11;
    f12.descriptorIndexing                        = VK_TRUE;
    f12.runtimeDescriptorArray                    = VK_TRUE;
    f12.descriptorBindingPartiallyBound           = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount  = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE; // characters stream textures in while frames are in flight
    f12.bufferDeviceAddress                       = VK_TRUE;
    f12.shaderFloat16                             = support_.float16 ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext                              = &f12;
    f2.features.textureCompressionBC      = VK_TRUE;
    f2.features.samplerAnisotropy         = VK_TRUE;
    f2.features.multiDrawIndirect         = VK_TRUE;
    // FSR 3.1's passes (gfx/fsr.cpp), when the GPU has them (support_.fsr says whether it can run).
    f2.features.shaderStorageImageExtendedFormats    = support_.fsr ? VK_TRUE : VK_FALSE;
    f2.features.shaderStorageImageWriteWithoutFormat = support_.fsr ? VK_TRUE : VK_FALSE;
    f2.features.shaderInt16                          = support_.float16 ? VK_TRUE : VK_FALSE;

    std::vector<const char*> devExts{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    std::vector<std::string> extraDev;
    if (extra && extra->device)
    {
        extraDev = extra->device(instance_, physical_);
        for (const auto& e : extraDev)
        {
            if (std::ranges::none_of(devExts, [&](const char* x) { return e == x; }))
            {
                devExts.push_back(e.c_str());
            }
        }
    }
    // FSR 3.1's SDK (gfx/fsr.cpp) decides what to use from the device's extension list, not from what is enabled:
    // with VK_KHR_dedicated_allocation listed it calls vkGetBufferMemoryRequirements2KHR, with VK_AMD_buffer_marker the
    // AMD markers, with VK_AMD_device_coherent_memory it allocates AMD's device-coherent memory. Enable those that are
    // there, so its calls and allocations are valid (#139; gfx/fsr/fsr_device.h covers what is still missing).
    VkPhysicalDeviceCoherentMemoryFeaturesAMD fcm{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD };
    if (support_.fsr)
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have(n);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, have.data());
        const auto has = [&](std::string_view e) {
            return std::ranges::any_of(have, [&](const VkExtensionProperties& p) { return e == p.extensionName; });
        };
        for (const char* e : { VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                               VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_AMD_BUFFER_MARKER_EXTENSION_NAME })
        {
            if (has(e) && std::ranges::none_of(devExts, [&](const char* x) { return std::string_view(x) == e; }))
            {
                devExts.push_back(e);
            }
        }
        if (has(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME))
        {
            VkPhysicalDeviceFeatures2 q{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            q.pNext = &fcm;
            vkGetPhysicalDeviceFeatures2(physical_, &q);
            if (fcm.deviceCoherentMemory)
            {
                devExts.push_back(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME);
                fcm.pNext = f2.pNext;
                f2.pNext  = &fcm;
            }
        }
    }
    // --rt (goal 38): ray queries against acceleration structures, only when asked for and the GPU has them.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fas{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR              frq{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    if (extra && extra->rayQuery)
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have(n);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, have.data());
        const auto has = [&](const char* e) { return std::ranges::any_of(have, [&](const VkExtensionProperties& p) { return std::string_view(p.extensionName) == e; }); };
        VkPhysicalDeviceRayQueryFeaturesKHR              qrq{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
        VkPhysicalDeviceAccelerationStructureFeaturesKHR qas{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
        qas.pNext = &qrq;
        VkPhysicalDeviceFeatures2 q2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        q2.pNext = &qas;
        vkGetPhysicalDeviceFeatures2(physical_, &q2);
        // support_.rayQuery also needs shaderInt64 (the passes' 64-bit buffer addresses) and room for the bindless
        // arrays; a GPU with ray queries but without those renders without ray tracing instead of failing a pipeline.
        if (support_.rayQuery && has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
            has(VK_KHR_RAY_QUERY_EXTENSION_NAME) && has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) &&
            qas.accelerationStructure && qrq.rayQuery)
        {
            for (const char* e : { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME })
            {
                if (std::ranges::none_of(devExts, [&](const char* x) { return std::string_view(x) == e; }))
                {
                    devExts.push_back(e);
                }
            }
            f2.features.shaderInt64   = VK_TRUE; // the passes' 64-bit buffer addresses (checked in support_.rayQuery)
            fas.accelerationStructure = VK_TRUE;
            frq.rayQuery              = VK_TRUE;
            fas.pNext                 = &frq;
            frq.pNext                 = f2.pNext;
            f2.pNext                  = &fas;
            rayQuery_                 = true;
        }
        else
        {
            spdlog::warn(
                "rt: this GPU / driver has no ray queries (VK_KHR_acceleration_structure + VK_KHR_ray_query + shaderInt64); rendering "
                "without ray tracing");
        }
    }
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext                   = &f2;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = static_cast<uint32_t>(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    VK_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_));
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VmaAllocatorCreateInfo aci{};
    aci.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    aci.physicalDevice   = physical_;
    aci.device           = device_;
    aci.instance         = instance_;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    VK_CHECK(vmaCreateAllocator(&aci, &allocator_));

    for (auto& f : frames_)
    {
        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = queueFamily_;
        VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool        = f.pool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device_, &cai, &f.cmd));
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &f.acquired));
    }
    {
        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily_;
        VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &uploadPool_));
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &uploadFence_));
    }

    if (timestampMs_ > 0)
    {
        VkQueryPoolCreateInfo qpi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2 * kFramesInFlight;
        VK_CHECK(vkCreateQueryPool(device_, &qpi, nullptr, &timestamps_));
    }

    createSwapchain();
    spdlog::info("gfx: {} ({}x{}, {} swapchain images)", deviceName_.data(), swapExtent_.width, swapExtent_.height, swapImages_.size());
}

Context::~Context()
{
    if (device_)
    {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        for (auto& f : frames_)
        {
            vkDestroySemaphore(device_, f.acquired, nullptr);
            vkDestroyFence(device_, f.fence, nullptr);
            vkDestroyCommandPool(device_, f.pool, nullptr);
        }
        reclaimUploads(true);
        for (auto f : spareFences_)
        {
            vkDestroyFence(device_, f, nullptr);
        }
        vkDestroyFence(device_, uploadFence_, nullptr);
        vkDestroyCommandPool(device_, uploadPool_, nullptr);
        if (timestamps_)
        {
            vkDestroyQueryPool(device_, timestamps_, nullptr);
        }
        vmaDestroyAllocator(allocator_);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_)
    {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (debug_)
    {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        destroy(instance_, debug_, nullptr);
    }
    if (instance_)
    {
        vkDestroyInstance(instance_, nullptr);
    }
}

void Context::createSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
    // UNORM on purpose: FFXI's Direct3D 8 pipeline blends, lights and fogs the stored 0..255 values directly (no sRGB
    // decode/encode). Doing the maths on the raw values reproduces its look exactly.
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
    {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = f;
            break;
        }
    }
    swapFormat_ = chosen.format;

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    swapAppSized_ = caps.currentExtent.width == UINT32_MAX;
    swapExtent_   = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent
                                                         : VkExtent2D{ std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width),
                                                                       std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height) };

    // Mailbox for low latency without tearing; FIFO is always available as fallback.
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());
    VkPresentModeKHR mode = std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end() ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    // XI_PRESENT=immediate: no vsync, for frame-time measurements (docs/performance.md).
    if (const char* p = std::getenv("XI_PRESENT"); p && std::string_view(p) == "immediate" && std::ranges::find(modes, VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end())
    {
        mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (static bool logged = false; !std::exchange(logged, true))
    {
        spdlog::info("gfx: present mode {}", mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount)
    {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }

    VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface          = surface_;
    sci.minImageCount    = imageCount;
    sci.imageFormat      = chosen.format;
    sci.imageColorSpace  = chosen.colorSpace;
    sci.imageExtent      = swapExtent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode      = mode;
    sci.clipped          = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_));

    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapImages_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data());
    swapViews_.resize(n);
    renderDone_.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = swapImages_[i];
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = swapFormat_;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &swapViews_[i]));
        VkSemaphoreCreateInfo semci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(device_, &semci, nullptr, &renderDone_[i]));
    }

    depth_ = createImage(swapExtent_, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Context::destroySwapchain()
{
    destroyImage(depth_);
    for (auto s : renderDone_)
    {
        vkDestroySemaphore(device_, s, nullptr);
    }
    for (auto v : swapViews_)
    {
        vkDestroyImageView(device_, v, nullptr);
    }
    renderDone_.clear();
    swapViews_.clear();
    swapImages_.clear();
    if (swapchain_)
    {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool Context::beginFrame(Frame& frame)
{
    auto& f = frames_[frameIndex_];
    VK_CHECK(vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX));

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    if (swapchainStale(swapAppSized_, swapExtent_, pw, ph))
    {
        spdlog::info("gfx: window is {}x{} pixels now (swapchain {}x{}): rebuilding the swapchain", pw, ph, swapExtent_.width,
                     swapExtent_.height);
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        createSwapchain();
        return false;
    }

    const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &frame.imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        createSwapchain();
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        VK_CHECK(r);
    }
    VK_CHECK(vkResetFences(device_, 1, &f.fence));
    VK_CHECK(vkResetCommandPool(device_, f.pool, 0));

    frame.cmd   = f.cmd;
    frame.slot  = frameIndex_;
    frame.image = swapImages_[frame.imageIndex];
    frame.view  = swapViews_[frame.imageIndex];
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(frame.cmd, &bi));
    if (timestamps_)
    {
        // The fence above finished this slot's previous frame, so its two timestamps are ready.
        const uint32_t q = 2 * frameIndex_;
        if (stamped_[frameIndex_])
        {
            std::array<uint64_t, 2> t{};
            if (vkGetQueryPoolResults(device_, timestamps_, q, 2, sizeof(t), t.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                gpuMs_ = static_cast<double>(t[1] - t[0]) * timestampMs_;
            }
        }
        vkCmdResetQueryPool(frame.cmd, timestamps_, q, 2);
        vkCmdWriteTimestamp2(frame.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps_, q);
        stamped_[frameIndex_] = true;
    }
    return true;
}

void Context::endFrame(const Frame& frame)
{
    auto& f = frames_[frameIndex_];
    if (timestamps_)
    {
        vkCmdWriteTimestamp2(frame.cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps_, 2 * frameIndex_ + 1);
    }
    VK_CHECK(vkEndCommandBuffer(frame.cmd));

    VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    wait.semaphore = f.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = renderDone_[frame.imageIndex];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    VkCommandBufferSubmitInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = frame.cmd;
    VkSubmitInfo2 si{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount   = 1;
    si.pWaitSemaphoreInfos      = &wait;
    si.commandBufferInfoCount   = 1;
    si.pCommandBufferInfos      = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos    = &signal;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &si, f.fence));

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &renderDone_[frame.imageIndex];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &swapchain_;
    pi.pImageIndices      = &frame.imageIndex;
    const VkResult r      = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        createSwapchain();
    }
    else if (r != VK_SUCCESS)
    {
        VK_CHECK(r);
    }
    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

void Context::waitIdle()
{
    vkDeviceWaitIdle(device_);
}

void Context::immediateSubmit(const std::function<void(VkCommandBuffer)>& record)
{
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool        = uploadPool_;
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd    = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device_, &cai, &cmd));
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    record(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbi;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &si, uploadFence_));
    VK_CHECK(vkWaitForFences(device_, 1, &uploadFence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(device_, 1, &uploadFence_));
    vkFreeCommandBuffers(device_, uploadPool_, 1, &cmd);
}

uint64_t Context::submitAsync(const std::function<void(VkCommandBuffer)>& record, std::vector<Buffer> staging)
{
    AsyncUpload u;
    u.ticket = nextTicket_++;
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool        = uploadPool_;
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device_, &cai, &u.cmd));
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(u.cmd, &bi));
    record(u.cmd);
    VK_CHECK(vkEndCommandBuffer(u.cmd));
    if (spareFences_.empty())
    {
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &u.fence));
    }
    else
    {
        u.fence = spareFences_.back();
        spareFences_.pop_back();
    }
    VkCommandBufferSubmitInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = u.cmd;
    VkSubmitInfo2 si{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbi;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &si, u.fence));
    u.staging = std::move(staging);
    async_.push_back(std::move(u));
    return async_.back().ticket;
}

void Context::reclaimUploads(bool wait)
{
    for (std::size_t i = 0; i < async_.size();)
    {
        auto& u = async_[i];
        if (wait)
        {
            VK_CHECK(vkWaitForFences(device_, 1, &u.fence, VK_TRUE, UINT64_MAX));
        }
        else if (vkGetFenceStatus(device_, u.fence) != VK_SUCCESS)
        {
            ++i;
            continue;
        }
        VK_CHECK(vkResetFences(device_, 1, &u.fence));
        spareFences_.push_back(u.fence);
        vkFreeCommandBuffers(device_, uploadPool_, 1, &u.cmd);
        for (auto& b : u.staging)
        {
            destroyBuffer(b);
        }
        async_.erase(async_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

bool Context::uploadDone(uint64_t ticket)
{
    reclaimUploads(false);
    return std::ranges::none_of(async_, [&](const AsyncUpload& u) { return u.ticket == ticket; });
}

void Context::adoptStaging(uint64_t ticket, std::vector<Buffer> staging)
{
    for (auto& u : async_)
    {
        if (u.ticket == ticket)
        {
            u.staging.insert(u.staging.end(), staging.begin(), staging.end());
            return;
        }
    }
    for (auto& b : staging) // already finished
    {
        destroyBuffer(b);
    }
}

void Context::waitUpload(uint64_t ticket)
{
    for (auto& u : async_)
    {
        if (u.ticket == ticket)
        {
            VK_CHECK(vkWaitForFences(device_, 1, &u.fence, VK_TRUE, UINT64_MAX));
        }
    }
    reclaimUploads(false);
}

Buffer Context::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible)
{
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible)
    {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        // Nothing flushes its writes (callers memcpy into `mapped`): NVIDIA's host-visible memory is always coherent,
        // but not every driver's is (Intel's on some parts), so ask for coherent memory rather than flush (issue #139).
        aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    Buffer            b;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(allocator_, &bci, &aci, &b.buffer, &b.allocation, &info));
    b.size   = size;
    b.mapped = info.pMappedData;
    return b;
}

Buffer Context::uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buffer dst     = createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, data, size);
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{ 0, 0, size };
        vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &copy);
    });
    destroyBuffer(staging);
    return dst;
}

VkDeviceAddress Context::address(const Buffer& b) const
{
    VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    info.buffer = b.buffer;
    return vkGetBufferDeviceAddress(device_, &info);
}

void Context::destroyBuffer(Buffer& b)
{
    if (b.buffer)
    {
        vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
    }
    b = {};
}

Image Context::createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t mipLevels)
{
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = format;
    ici.extent      = { extent.width, extent.height, 1 };
    ici.mipLevels   = mipLevels;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    Image img;
    img.format    = format;
    img.extent    = extent;
    img.mipLevels = mipLevels;
    VK_CHECK(vmaCreateImage(allocator_, &ici, &aci, &img.image, &img.allocation, nullptr));
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image            = img.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = format;
    vci.subresourceRange = { aspect, 0, mipLevels, 0, 1 };
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &img.view));
    return img;
}

void Context::destroyImage(Image& img)
{
    if (img.view)
    {
        vkDestroyImageView(device_, img.view, nullptr);
    }
    if (img.image)
    {
        vmaDestroyImage(allocator_, img.image, img.allocation);
    }
    img = {};
}

void Context::transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect, uint32_t baseMip,
                         uint32_t mipCount)
{
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask    = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask    = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.oldLayout        = from;
    b.newLayout        = to;
    b.image            = image;
    b.subresourceRange = { aspect, baseMip, mipCount, 0, 1 };
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace xi::gfx
