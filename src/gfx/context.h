// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/capabilities.h"
#include "gfx/vk.h"

#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace xi::gfx
{

struct Image
{
    VkImage       image      = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat      format     = VK_FORMAT_UNDEFINED;
    VkExtent2D    extent{};
    uint32_t      mipLevels  = 1;
};

struct Buffer
{
    VkBuffer      buffer     = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkDeviceSize  size       = 0;
    void*         mapped     = nullptr; // set for host-visible buffers
};

// Extensions a feature needs on top of the client's own (DLSS / NGX, goal 15). Empty = the default context.
struct ContextExtensions
{
    std::vector<std::string>                                                         instance;
    std::function<std::vector<std::string>(VkInstance, VkPhysicalDevice)>           device; // asked once the GPU is picked
    bool                                                                             rayQuery = false; // --rt (goal 38): acceleration structures + ray queries, if the GPU has them
};

// A surface the application sizes (currentExtent undefined, as on Wayland) never reports the swapchain out of date:
// when the window's size in pixels changes (resized, or moved to an output with another scale), the swapchain must
// be rebuilt by hand, or the interface (drawn at the window's pixel size) covers only part of the old image (#78).
[[nodiscard]] inline bool swapchainStale(bool appSized, VkExtent2D swap, int pixelW, int pixelH)
{
    return appSized && pixelW > 0 && pixelH > 0 &&
           (static_cast<uint32_t>(pixelW) != swap.width || static_cast<uint32_t>(pixelH) != swap.height);
}

// Owns the Vulkan instance/device/swapchain and per-frame synchronisation.
// Vulkan 1.3 core: dynamic rendering + synchronization2; bindless textures via descriptor indexing.
class Context
{
public:
    static constexpr uint32_t kFramesInFlight = 2;

    explicit Context(SDL_Window* window, bool validation, const ContextExtensions* extra = nullptr);
    ~Context();
    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;

    struct Frame
    {
        VkCommandBuffer cmd        = VK_NULL_HANDLE;
        uint32_t        imageIndex = 0;
        VkImage         image      = VK_NULL_HANDLE;
        VkImageView     view       = VK_NULL_HANDLE;
        uint32_t        slot       = 0; // frame-in-flight index, for per-frame buffers
    };

    // Returns false if the swapchain had to be recreated (skip this frame).
    bool beginFrame(Frame& frame);
    void endFrame(const Frame& frame);
    void waitIdle();

    // Records commands into a one-shot command buffer and waits for completion (for uploads).
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);
    // The same without waiting (goal 25: character uploads off the frame's critical path): returns a ticket; the
    // staging buffers are destroyed once the GPU is done. Nothing may use the uploaded resources before
    // uploadDone(ticket) (which also reclaims every finished upload) returns true.
    uint64_t submitAsync(const std::function<void(VkCommandBuffer)>& record, std::vector<Buffer> staging);
    bool     uploadDone(uint64_t ticket);
    void     waitUpload(uint64_t ticket);
    // More staging buffers for a submitted upload (ones only known after recording).
    void     adoptStaging(uint64_t ticket, std::vector<Buffer> staging);

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void   destroyBuffer(Buffer& b);
    // Device-local buffer filled through a staging copy (blocks until done).
    Buffer          uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    VkDeviceAddress address(const Buffer& b) const;
    Image  createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t mipLevels = 1);
    void   destroyImage(Image& img);

    [[nodiscard]] VkDevice device() const
    {
        return device_;
    }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const
    {
        return physical_;
    }
    [[nodiscard]] VkExtent2D extent() const
    {
        return swapExtent_;
    }
    [[nodiscard]] VkFormat colorFormat() const
    {
        return swapFormat_;
    }
    [[nodiscard]] const Image& depth() const
    {
        return depth_;
    }
    [[nodiscard]] VmaAllocator allocator() const
    {
        return allocator_;
    }
    [[nodiscard]] VkInstance instance() const
    {
        return instance_;
    }
    [[nodiscard]] uint32_t queueFamily() const
    {
        return queueFamily_;
    }
    [[nodiscard]] VkQueue queue() const
    {
        return queue_;
    }
    [[nodiscard]] uint32_t imageCount() const
    {
        return static_cast<uint32_t>(swapImages_.size());
    }
    // The GPU's PCI vendor (XI_GPU_PRETEND's when set; gfx/capabilities.h) and the limits the client works within.
    [[nodiscard]] uint32_t vendorId() const
    {
        return vendorId_;
    }
    [[nodiscard]] const GpuLimits& limits() const
    {
        return support_.limits;
    }
    // Subgroup size control (FSR's wave64 path) and 16-bit shader arithmetic + storage were enabled.
    [[nodiscard]] bool subgroupControl() const
    {
        return support_.subgroupControl;
    }
    [[nodiscard]] bool float16() const
    {
        return support_.float16;
    }
    // FSR 3.1 can run on this GPU (the features above were enabled); fsrMissing() says why not.
    [[nodiscard]] bool fsr() const
    {
        return support_.fsr;
    }
    [[nodiscard]] const std::string& fsrMissing() const
    {
        return support_.fsrMissing;
    }
    // --rt (goal 38): VK_KHR_acceleration_structure + VK_KHR_ray_query were asked for and enabled.
    [[nodiscard]] bool rayQuery() const
    {
        return rayQuery_;
    }
    [[nodiscard]] const char* deviceName() const
    {
        return deviceName_.data();
    }
    // GPU time of the last finished frame's command buffer (timestamps at its start and end), ms; 0 if unsupported.
    [[nodiscard]] double gpuFrameMs() const
    {
        return gpuMs_;
    }

    // Image layout transition helper (synchronization2).
    static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                           uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS);

private:
    void createSwapchain();
    void destroySwapchain();

    SDL_Window*              window_   = nullptr;
    VkInstance               instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_    = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_  = VK_NULL_HANDLE;
    VkPhysicalDevice         physical_ = VK_NULL_HANDLE;
    VkDevice                 device_   = VK_NULL_HANDLE;
    VkQueue                  queue_    = VK_NULL_HANDLE;
    uint32_t                 queueFamily_ = 0;
    VmaAllocator             allocator_   = nullptr;
    std::array<char, 256>    deviceName_{};
    bool                     rayQuery_ = false;
    uint32_t                 vendorId_ = 0;
    DeviceSupport            support_;

    VkSwapchainKHR           swapchain_  = VK_NULL_HANDLE;
    VkFormat                 swapFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D               swapExtent_{};
    bool                     swapAppSized_ = false; // the surface left the extent to us (swapchainStale)
    std::vector<VkImage>     swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> renderDone_; // per swapchain image
    Image                    depth_;

    struct PerFrame
    {
        VkCommandPool   pool      = VK_NULL_HANDLE;
        VkCommandBuffer cmd       = VK_NULL_HANDLE;
        VkFence         fence     = VK_NULL_HANDLE;
        VkSemaphore     acquired  = VK_NULL_HANDLE;
    };
    std::array<PerFrame, kFramesInFlight> frames_{};
    uint32_t                              frameIndex_ = 0;

    VkQueryPool                       timestamps_ = VK_NULL_HANDLE; // two per frame in flight
    std::array<bool, kFramesInFlight> stamped_{};
    double                            timestampMs_ = 0; // ms per tick
    double                            gpuMs_       = 0;

    VkCommandPool uploadPool_  = VK_NULL_HANDLE;
    VkFence       uploadFence_ = VK_NULL_HANDLE;

    struct AsyncUpload
    {
        uint64_t            ticket = 0;
        VkCommandBuffer     cmd    = VK_NULL_HANDLE;
        VkFence             fence  = VK_NULL_HANDLE;
        std::vector<Buffer> staging;
    };
    void                     reclaimUploads(bool wait);
    std::vector<AsyncUpload> async_;
    std::vector<VkFence>     spareFences_;
    uint64_t                 nextTicket_ = 1;
};

} // namespace xi::gfx
