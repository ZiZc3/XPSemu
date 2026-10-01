//
// xemu User Interface: Vulkan presentation
//
// Builds without OpenGL (the PS5) draw the UI with the NV2A Vulkan renderer's
// own device and queue, and present it through a VK_KHR_display swapchain.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <unordered_map>
#include <vector>

#include "common.hh"
extern "C" {
#include "qemu/thread.h"
}
#include "gl-helpers.hh"
#include "vk-present.hh"
#include "xemu-hud.h"
#include "../xemu-texture.h"
#include "hw/xbox/nv2a/pgraph/vk/vk-ui.h"

#ifdef __PROSPERO__
// The system keeps the title's launch image up until the title hides it.
extern "C" int sceSystemServiceHideSplashScreen(void);
#endif

// The display offers a single 3840x2160 mode (see CreateDisplaySurface).
static const int kMinImageCount = 2;
static const uint32_t kDescriptorPoolSize = 256;

struct UiTexture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    int width, height;
    bool bgrx;
};

static struct {
    bool initialized;
    bool failed;
    PGRAPHVkUIContext ctx;
    VkSurfaceKHR surface;
    ImGui_ImplVulkanH_Window wd;
    VkSampler linear_sampler, nearest_sampler;
    VkCommandPool upload_pool;
    bool swapchain_rebuild;
    bool frame_begun;

    // Textures made by xemu_vk_texture_create, by ImTextureID.
    std::unordered_map<XemuTexture, UiTexture> textures;

    // The thread that draws the UI: the only one that may read textures back.
    QemuThread ui_thread;

    // The NV2A display image, registered with ImGui while it stays the same.
    VkImage display_image;
    VkImageView display_view;
    VkSampler display_sampler;
    XemuTexture display_tex;
    int display_width, display_height;

    // The VGA fallback surface (guest writes the framebuffer directly).
    XemuTexture surface_tex;
} g_vk;

static void CheckVkResult(VkResult err)
{
    if (err != VK_SUCCESS) {
        fprintf(stderr, "Vulkan UI: VkResult %d\n", err);
        assert(err >= 0);
    }
}

static PFN_vkVoidFunction LoadFunction(const char *name, void *user_data)
{
    (void)user_data;
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(g_vk.ctx.device, name);
    return f ? f : vkGetInstanceProcAddr(g_vk.ctx.instance, name);
}

static uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(g_vk.ctx.physical_device, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

#ifdef __PROSPERO__
// PS5: no window system; the one display is VideoOut, driven through
// VK_KHR_display. The driver reports a single 3840x2160 mode and refuses a
// surface of any other extent (as found by the PS5SX2 port).
static VkSurfaceKHR CreateDisplaySurface(VkExtent2D *extent)
{
    VkPhysicalDevice pd = g_vk.ctx.physical_device;

    uint32_t display_count = 0;
    vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &display_count, NULL);
    if (display_count == 0) {
        fprintf(stderr, "Vulkan UI: the device reports no display\n");
        return VK_NULL_HANDLE;
    }
    std::vector<VkDisplayPropertiesKHR> displays(display_count);
    vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &display_count,
                                            displays.data());

    uint32_t plane_count = 0;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &plane_count, NULL);
    if (plane_count == 0) {
        fprintf(stderr, "Vulkan UI: the device reports no display plane\n");
        return VK_NULL_HANDLE;
    }
    std::vector<VkDisplayPlanePropertiesKHR> planes(plane_count);
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &plane_count,
                                                 planes.data());

    // The first display with a mode, and the largest mode it offers.
    VkDisplayKHR display = VK_NULL_HANDLE;
    VkDisplayModePropertiesKHR mode = {};
    for (const VkDisplayPropertiesKHR &d : displays) {
        uint32_t mode_count = 0;
        vkGetDisplayModePropertiesKHR(pd, d.display, &mode_count, NULL);
        std::vector<VkDisplayModePropertiesKHR> modes(mode_count);
        vkGetDisplayModePropertiesKHR(pd, d.display, &mode_count,
                                      modes.data());
        for (const VkDisplayModePropertiesKHR &m : modes) {
            const VkExtent2D &r = m.parameters.visibleRegion;
            const VkExtent2D &best = mode.parameters.visibleRegion;
            if (display == VK_NULL_HANDLE ||
                r.width * r.height > best.width * best.height) {
                display = d.display;
                mode = m;
            }
        }
        if (display != VK_NULL_HANDLE) {
            break;
        }
    }
    if (display == VK_NULL_HANDLE) {
        fprintf(stderr, "Vulkan UI: no display offers a mode\n");
        return VK_NULL_HANDLE;
    }

    // The plane that can drive that display.
    uint32_t plane = UINT32_MAX;
    for (uint32_t i = 0; i < plane_count; i++) {
        if (planes[i].currentDisplay == VK_NULL_HANDLE ||
            planes[i].currentDisplay == display) {
            plane = i;
            break;
        }
    }
    if (plane == UINT32_MAX) {
        fprintf(stderr, "Vulkan UI: no display plane can drive the display\n");
        return VK_NULL_HANDLE;
    }

    VkDisplaySurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mode.displayMode,
        .planeIndex = plane,
        .planeStackIndex = planes[plane].currentStackIndex,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = mode.parameters.visibleRegion,
    };
    VkSurfaceKHR surface;
    VkResult res = vkCreateDisplayPlaneSurfaceKHR(g_vk.ctx.instance, &info,
                                                  NULL, &surface);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "Vulkan UI: vkCreateDisplayPlaneSurfaceKHR: %d\n", res);
        return VK_NULL_HANDLE;
    }

    *extent = mode.parameters.visibleRegion;
    fprintf(stderr, "Vulkan UI: display surface %ux%u on plane %u\n",
            extent->width, extent->height, plane);
    return surface;
}
#else
static VkSurfaceKHR CreateDisplaySurface(VkExtent2D *extent)
{
    (void)extent;
    fprintf(stderr, "Vulkan UI: no surface support on this platform\n");
    return VK_NULL_HANDLE;
}
#endif

static VkSampler CreateSampler(VkFilter filter)
{
    VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.25f,
    };
    VkSampler sampler;
    CheckVkResult(vkCreateSampler(g_vk.ctx.device, &info, NULL, &sampler));
    return sampler;
}

static void CreateOrResizeSwapchain(int width, int height)
{
    ImGui_ImplVulkanH_Window *wd = &g_vk.wd;

    // Lock so the renderer's thread doesn't submit while the old swapchain's
    // images are waited on and destroyed.
    pgraph_vk_lock_queue();
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        g_vk.ctx.instance, g_vk.ctx.physical_device, g_vk.ctx.device, wd,
        g_vk.ctx.queue_family, NULL, width, height, kMinImageCount);
    pgraph_vk_unlock_queue();
    wd->FrameIndex = 0;
    g_vk.swapchain_rebuild = false;
}

static bool Initialize(void)
{
    if (!pgraph_vk_get_ui_context(&g_vk.ctx)) {
        return false; // The renderer hasn't created its device yet
    }
    if (!g_vk.ctx.display_extension_enabled) {
        fprintf(stderr, "Vulkan UI: VK_KHR_display is not enabled\n");
        g_vk.failed = true;
        return false;
    }

    VkExtent2D extent;
    g_vk.surface = CreateDisplaySurface(&extent);
    if (g_vk.surface == VK_NULL_HANDLE) {
        g_vk.failed = true;
        return false;
    }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.ctx.physical_device,
                                         g_vk.ctx.queue_family, g_vk.surface,
                                         &supported);
    if (!supported) {
        fprintf(stderr, "Vulkan UI: the renderer's queue cannot present\n");
        g_vk.failed = true;
        return false;
    }

    // Before any ImGui_ImplVulkan(H) call: they use these (VK_NO_PROTOTYPES).
    ImGui_ImplVulkan_LoadFunctions(LoadFunction);

    ImGui_ImplVulkanH_Window *wd = &g_vk.wd;
    wd->Surface = g_vk.surface;
    const VkFormat formats[] = { VK_FORMAT_B8G8R8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM };
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_vk.ctx.physical_device, g_vk.surface, formats, IM_ARRAYSIZE(formats),
        VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    const VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_vk.ctx.physical_device, g_vk.surface, &present_mode, 1);
    wd->ClearEnable = true;

    CreateOrResizeSwapchain(extent.width, extent.height);
    fprintf(stderr, "Vulkan UI: swapchain %dx%d, %u images, format %d\n",
            wd->Width, wd->Height, wd->ImageCount, wd->SurfaceFormat.format);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_vk.ctx.instance;
    init_info.PhysicalDevice = g_vk.ctx.physical_device;
    init_info.Device = g_vk.ctx.device;
    init_info.QueueFamily = g_vk.ctx.queue_family;
    init_info.Queue = g_vk.ctx.queue;
    init_info.DescriptorPoolSize = kDescriptorPoolSize;
    init_info.RenderPass = wd->RenderPass;
    init_info.MinImageCount = kMinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = CheckVkResult;
    ImGui_ImplVulkan_Init(&init_info);

    g_vk.linear_sampler = CreateSampler(VK_FILTER_LINEAR);
    g_vk.nearest_sampler = CreateSampler(VK_FILTER_NEAREST);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = g_vk.ctx.queue_family,
    };
    CheckVkResult(vkCreateCommandPool(g_vk.ctx.device, &pool_info, NULL,
                                      &g_vk.upload_pool));

    g_vk.initialized = true;

    xemu_vk_rebuild_fonts();

    // Textures the OpenGL UI creates at startup need the device.
    InitCustomRendering();
    fprintf(stderr, "Vulkan UI: ready\n");
    return true;
}

void xemu_vk_rebuild_fonts(void)
{
    if (!g_vk.initialized) {
        return;
    }

    // No frame is in flight: xemu_vk_present_frame waits for each one.
    pgraph_vk_lock_queue();
    ImGui_ImplVulkan_DestroyFontsTexture();
    ImGui_ImplVulkan_CreateFontsTexture();
    pgraph_vk_unlock_queue();
}

//
// Textures
//

// Records into a one-time command buffer, submits it and waits for it.
template <typename F> static void RunCommands(F record)
{
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_vk.upload_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CheckVkResult(vkAllocateCommandBuffers(g_vk.ctx.device, &alloc, &cmd));

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CheckVkResult(vkBeginCommandBuffer(cmd, &begin));
    record(cmd);
    CheckVkResult(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fence_info = { .sType =
                                         VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CheckVkResult(vkCreateFence(g_vk.ctx.device, &fence_info, NULL, &fence));

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    pgraph_vk_lock_queue();
    CheckVkResult(vkQueueSubmit(g_vk.ctx.queue, 1, &submit, fence));
    pgraph_vk_unlock_queue();
    CheckVkResult(
        vkWaitForFences(g_vk.ctx.device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(g_vk.ctx.device, fence, NULL);
    vkFreeCommandBuffers(g_vk.ctx.device, g_vk.upload_pool, 1, &cmd);
}

static void ImageBarrier(VkCommandBuffer cmd, VkImage image,
                         VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags src_stage,
                         VkAccessFlags src_access,
                         VkPipelineStageFlags dst_stage,
                         VkAccessFlags dst_access)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
}

// Uploads tightly packed 4-byte pixels (row_pixels wide in memory) into
// tex.image, which must be (or be about to be) sampled.
static void UploadPixels(UiTexture &tex, const void *pixels, int row_pixels,
                         bool first_upload)
{
    VkDevice dev = g_vk.ctx.device;
    VkDeviceSize size = (VkDeviceSize)row_pixels * tex.height * 4;

    VkBufferCreateInfo buf_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer staging;
    CheckVkResult(vkCreateBuffer(dev, &buf_info, NULL, &staging));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, staging, &req);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex =
            FindMemoryType(req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory staging_mem;
    CheckVkResult(vkAllocateMemory(dev, &alloc, NULL, &staging_mem));
    CheckVkResult(vkBindBufferMemory(dev, staging, staging_mem, 0));

    void *map;
    CheckVkResult(vkMapMemory(dev, staging_mem, 0, size, 0, &map));
    memcpy(map, pixels, size);
    vkUnmapMemory(dev, staging_mem);

    RunCommands([&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, tex.image,
                     first_upload ? VK_IMAGE_LAYOUT_UNDEFINED :
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region = {
            .bufferRowLength = (uint32_t)row_pixels,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)tex.width, (uint32_t)tex.height, 1 },
        };
        vkCmdCopyBufferToImage(cmd, staging, tex.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        ImageBarrier(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    });

    vkDestroyBuffer(dev, staging, NULL);
    vkFreeMemory(dev, staging_mem, NULL);
}

// An RGBA8 (or, with bgrx, B8G8R8 with an ignored fourth byte) image.
static XemuTexture CreateTexture(int width, int height, bool bgrx)
{
    VkDevice dev = g_vk.ctx.device;
    UiTexture tex = {};
    tex.width = width;
    tex.height = height;
    tex.bgrx = bgrx;

    VkFormat format =
        bgrx ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CheckVkResult(vkCreateImage(dev, &image_info, NULL, &tex.image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, tex.image, &req);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    CheckVkResult(vkAllocateMemory(dev, &alloc, NULL, &tex.memory));
    CheckVkResult(vkBindImageMemory(dev, tex.image, tex.memory, 0));

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = tex.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        // The fourth byte of a BGRX surface is padding, not alpha.
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY,
                        bgrx ? VK_COMPONENT_SWIZZLE_ONE :
                               VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    CheckVkResult(vkCreateImageView(dev, &view_info, NULL, &tex.view));

    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
        g_vk.linear_sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    XemuTexture id = (XemuTexture)(uintptr_t)set;
    g_vk.textures[id] = tex;
    return id;
}

static void DestroyTexture(XemuTexture id)
{
    auto it = g_vk.textures.find(id);
    if (it == g_vk.textures.end()) {
        return;
    }

    // It may still be in use by the frame in flight.
    vkDeviceWaitIdle(g_vk.ctx.device);

    UiTexture &tex = it->second;
    ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)(uintptr_t)id);
    vkDestroyImageView(g_vk.ctx.device, tex.view, NULL);
    vkDestroyImage(g_vk.ctx.device, tex.image, NULL);
    vkFreeMemory(g_vk.ctx.device, tex.memory, NULL);
    g_vk.textures.erase(it);
}

XemuTexture xemu_vk_texture_create(const void *pixels, int width, int height,
                                   int channels)
{
    if (!g_vk.initialized || width <= 0 || height <= 0) {
        return 0;
    }

    std::vector<uint8_t> rgba;
    if (channels == 3) {
        rgba.resize((size_t)width * height * 4);
        const uint8_t *src = (const uint8_t *)pixels;
        for (size_t i = 0; i < (size_t)width * height; i++) {
            rgba[i * 4 + 0] = src[i * 3 + 0];
            rgba[i * 4 + 1] = src[i * 3 + 1];
            rgba[i * 4 + 2] = src[i * 3 + 2];
            rgba[i * 4 + 3] = 0xff;
        }
        pixels = rgba.data();
    } else {
        assert(channels == 4);
    }

    XemuTexture id = CreateTexture(width, height, false);
    UploadPixels(g_vk.textures[id], pixels, width, true);
    return id;
}

void xemu_vk_texture_destroy(XemuTexture tex)
{
    if (g_vk.initialized) {
        DestroyTexture(tex);
    }
}

bool xemu_vk_texture_get_size(XemuTexture tex, int *width, int *height)
{
    auto it = g_vk.textures.find(tex);
    if (it != g_vk.textures.end()) {
        *width = it->second.width;
        *height = it->second.height;
        return true;
    }
    if (tex && tex == g_vk.display_tex) {
        *width = g_vk.display_width;
        *height = g_vk.display_height;
        return true;
    }
    return false;
}

//
// Frames
//

bool xemu_vk_present_ready(void)
{
    if (!g_vk.initialized && !g_vk.failed) {
        qemu_thread_get_self(&g_vk.ui_thread);
        Initialize();
    }
    return g_vk.initialized;
}

XemuTexture xemu_vk_get_display_texture(void)
{
    VkImage image;
    VkImageView view;
    int width, height;
    if (!g_vk.initialized ||
        !pgraph_vk_get_display(&image, &view, &width, &height)) {
        return 0;
    }
    g_vk.display_image = image;

    VkSampler sampler =
        g_config.display.filtering == CONFIG_DISPLAY_FILTERING_NEAREST ?
            g_vk.nearest_sampler :
            g_vk.linear_sampler;

    if (view != g_vk.display_view || sampler != g_vk.display_sampler) {
        if (g_vk.display_tex) {
            // The previous frame has finished (xemu_vk_present_frame waits).
            ImGui_ImplVulkan_RemoveTexture(
                (VkDescriptorSet)(uintptr_t)g_vk.display_tex);
        }
        g_vk.display_tex = (XemuTexture)(uintptr_t)ImGui_ImplVulkan_AddTexture(
            sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        g_vk.display_view = view;
        g_vk.display_sampler = sampler;
    }
    g_vk.display_width = width;
    g_vk.display_height = height;
    return g_vk.display_tex;
}

bool xemu_vk_read_texture(XemuTexture tex, std::vector<uint8_t> &rgba,
                          int *width, int *height)
{
    if (!g_vk.initialized || !tex || !qemu_thread_is_self(&g_vk.ui_thread)) {
        return false;
    }

    VkImage image;
    bool bgrx = false;
    auto it = g_vk.textures.find(tex);
    if (it != g_vk.textures.end()) {
        image = it->second.image;
        *width = it->second.width;
        *height = it->second.height;
        bgrx = it->second.bgrx;
    } else if (tex == g_vk.display_tex) {
        // Valid here: the UI thread holds the frame (see vk_render_frame).
        image = g_vk.display_image;
        *width = g_vk.display_width;
        *height = g_vk.display_height;
    } else {
        return false;
    }

    VkDevice dev = g_vk.ctx.device;
    VkDeviceSize size = (VkDeviceSize)*width * *height * 4;
    VkBufferCreateInfo buf_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    CheckVkResult(vkCreateBuffer(dev, &buf_info, NULL, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex =
            FindMemoryType(req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory memory;
    CheckVkResult(vkAllocateMemory(dev, &alloc, NULL, &memory));
    CheckVkResult(vkBindBufferMemory(dev, buffer, memory, 0));

    RunCommands([&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)*width, (uint32_t)*height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffer, 1, &region);
        ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    });

    void *map;
    CheckVkResult(vkMapMemory(dev, memory, 0, size, 0, &map));
    rgba.resize(size);
    memcpy(rgba.data(), map, size);
    vkUnmapMemory(dev, memory);
    vkDestroyBuffer(dev, buffer, NULL);
    vkFreeMemory(dev, memory, NULL);

    for (size_t i = 0; i < size; i += 4) {
        if (bgrx) {
            std::swap(rgba[i], rgba[i + 2]);
        }
        rgba[i + 3] = 0xff;
    }
    return true;
}

XemuTexture xemu_vk_update_surface_texture(const void *pixels, int width,
                                           int height, int stride)
{
    if (!g_vk.initialized) {
        return 0;
    }

    auto it = g_vk.textures.find(g_vk.surface_tex);
    if (it == g_vk.textures.end() || it->second.width != width ||
        it->second.height != height) {
        if (g_vk.surface_tex) {
            DestroyTexture(g_vk.surface_tex);
        }
        g_vk.surface_tex = CreateTexture(width, height, true);
        UploadPixels(g_vk.textures[g_vk.surface_tex], pixels, stride / 4,
                     true);
    } else {
        UploadPixels(it->second, pixels, stride / 4, false);
    }
    return g_vk.surface_tex;
}

void xemu_vk_present_frame(ImDrawData *draw_data)
{
    ImGui_ImplVulkanH_Window *wd = &g_vk.wd;
    VkDevice dev = g_vk.ctx.device;

    if (g_vk.swapchain_rebuild) {
        CreateOrResizeSwapchain(wd->Width, wd->Height);
    }

    VkSemaphore image_acquired =
        wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete =
        wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    static bool acquired_once;
    if (!acquired_once) {
        fprintf(stderr, "Vulkan UI: acquiring the first swapchain image\n");
    }
    VkResult err = vkAcquireNextImageKHR(dev, wd->Swapchain, UINT64_MAX,
                                         image_acquired, VK_NULL_HANDLE,
                                         &wd->FrameIndex);
    if (!acquired_once || err != VK_SUCCESS) {
        acquired_once = true;
        fprintf(stderr, "Vulkan UI: acquire -> %d (image %u)\n", err,
                wd->FrameIndex);
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_vk.swapchain_rebuild = true;
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR) {
        return;
    }
    if (err != VK_SUBOPTIMAL_KHR) {
        CheckVkResult(err);
    }

    ImGui_ImplVulkanH_Frame *fd = &wd->Frames[wd->FrameIndex];
    CheckVkResult(vkWaitForFences(dev, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
    CheckVkResult(vkResetFences(dev, 1, &fd->Fence));
    CheckVkResult(vkResetCommandPool(dev, fd->CommandPool, 0));

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CheckVkResult(vkBeginCommandBuffer(fd->CommandBuffer, &begin));

    VkClearValue clear = {};
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = wd->RenderPass,
        .framebuffer = fd->Framebuffer,
        .renderArea = { { 0, 0 },
                        { (uint32_t)wd->Width, (uint32_t)wd->Height } },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    vkCmdEndRenderPass(fd->CommandBuffer);
    CheckVkResult(vkEndCommandBuffer(fd->CommandBuffer));

    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &image_acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &fd->CommandBuffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &render_complete,
    };
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_complete,
        .swapchainCount = 1,
        .pSwapchains = &wd->Swapchain,
        .pImageIndices = &wd->FrameIndex,
    };

    pgraph_vk_lock_queue();
    CheckVkResult(vkQueueSubmit(g_vk.ctx.queue, 1, &submit, fd->Fence));
    err = vkQueuePresentKHR(g_vk.ctx.queue, &present);
    pgraph_vk_unlock_queue();
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_vk.swapchain_rebuild = true;
    } else {
        CheckVkResult(err);
    }
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;

    static uint64_t frames;
    if (frames++ == 0) {
        fprintf(stderr, "Vulkan UI: first frame presented (%d)\n", err);
#ifdef __PROSPERO__
        int rc = sceSystemServiceHideSplashScreen();
        fprintf(stderr, "Vulkan UI: splash screen hidden (%#x)\n", rc);
#endif
    } else if (frames % 600 == 0) {
        fprintf(stderr, "Vulkan UI: %llu frames\n", (unsigned long long)frames);
    }

    // The NV2A display image is sampled by this frame and rewritten by the
    // renderer once the UI releases it, so let the frame finish first.
    CheckVkResult(vkWaitForFences(dev, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
}
