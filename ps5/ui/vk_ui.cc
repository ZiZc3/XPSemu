// XOG: the dashboard's Vulkan: its own instance and device on the console's display. It goes with
// the process when a game starts. The textures and frames after XPSemu's vk-present.cc.
// GPL-3.0-or-later.
#include "common.hh"
#include <assert.h>
#include <unordered_map>

#include <volk.h>
#define IMGUI_IMPL_VULKAN_USE_VOLK
#include <imgui_impl_vulkan.h>

extern "C" {
VkResult rx_display_surface(VkInstance inst, VkPhysicalDevice phys, VkSurfaceKHR *out);
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
int sceSystemServiceHideSplashScreen(void);
}

static const int kMinImageCount = 2;
static const uint32_t kDescriptorPoolSize = 256;

struct UiTexture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    int width, height;
};

static struct {
    bool initialized;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkSurfaceKHR surface;
    ImGui_ImplVulkanH_Window wd;
    VkSampler sampler;
    VkCommandPool upload_pool;
    bool rebuild;
    std::unordered_map<XemuTexture, UiTexture> textures;
    char device_name[256];
} g_vk;

static void CheckVkResult(VkResult err)
{
    if (err < 0) {
        xog_ui_log("Vulkan: error %d", err);
    }
}

static PFN_vkVoidFunction LoadFunction(const char *name, void *)
{
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(g_vk.device, name);
    return f ? f : vkGetInstanceProcAddr(g_vk.instance, name);
}

static uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

static void CreateOrResize(int w, int h)
{
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_vk.instance, g_vk.phys, g_vk.device, &g_vk.wd, g_vk.family, NULL, w, h,
                                           kMinImageCount);
    g_vk.wd.FrameIndex = 0;
    g_vk.rebuild = false;
}

bool xog_vk_init(void)
{
    volkInitializeCustom((PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr);
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "XOG",
                              .apiVersion = VK_API_VERSION_1_1 };
    const char *iexts[] = { VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_display" };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts };
    VkResult r = vkCreateInstance(&ici, NULL, &g_vk.instance);
    if (r != VK_SUCCESS) {
        xog_ui_log("Vulkan: no instance: %d", r);
        return false;
    }
    volkLoadInstanceOnly(g_vk.instance);
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(g_vk.instance, &n, &g_vk.phys);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_vk.phys, &props);
    snprintf(g_vk.device_name, sizeof(g_vk.device_name), "%s", props.deviceName);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    std::vector<VkQueueFamilyProperties> q(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, q.data());
    for (uint32_t i = 0; i < nq; i++) {
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_vk.family = i;
            break;
        }
    }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.family,
                                   .queueCount = 1, .pQueuePriorities = &prio };
    const char *dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts };
    r = vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.device);
    if (r != VK_SUCCESS) {
        xog_ui_log("Vulkan: no device: %d", r);
        return false;
    }
    volkLoadDevice(g_vk.device);
    vkGetDeviceQueue(g_vk.device, g_vk.family, 0, &g_vk.queue);

    r = rx_display_surface(g_vk.instance, g_vk.phys, &g_vk.surface);
    if (r != VK_SUCCESS) {
        xog_ui_log("Vulkan: no surface: %d", r);
        return false;
    }
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    const int w = caps.currentExtent.width != UINT32_MAX ? (int)caps.currentExtent.width : 3840;
    const int h = caps.currentExtent.height != UINT32_MAX ? (int)caps.currentExtent.height : 2160;

    ImGui_ImplVulkan_LoadFunctions(LoadFunction);
    ImGui_ImplVulkanH_Window *wd = &g_vk.wd;
    wd->Surface = g_vk.surface;
    const VkFormat formats[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_vk.phys, g_vk.surface, formats, 2,
                                                              VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    const VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_vk.phys, g_vk.surface, &mode, 1);
    wd->ClearEnable = true;
    CreateOrResize(w, h);
    xog_ui_log("Vulkan: %s, swapchain %dx%d, %u images", g_vk.device_name, wd->Width, wd->Height, wd->ImageCount);

    ImGui_ImplVulkan_InitInfo init = {};
    init.Instance = g_vk.instance;
    init.PhysicalDevice = g_vk.phys;
    init.Device = g_vk.device;
    init.QueueFamily = g_vk.family;
    init.Queue = g_vk.queue;
    init.DescriptorPoolSize = kDescriptorPoolSize;
    init.RenderPass = wd->RenderPass;
    init.MinImageCount = kMinImageCount;
    init.ImageCount = wd->ImageCount;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.CheckVkResultFn = CheckVkResult;
    ImGui_ImplVulkan_Init(&init);

    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    vkCreateSampler(g_vk.device, &si, NULL, &g_vk.sampler);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = g_vk.family };
    vkCreateCommandPool(g_vk.device, &pi, NULL, &g_vk.upload_pool);
    g_vk.initialized = true;
    return true;
}

const char *pgraph_vk_get_device_name(void)
{
    return g_vk.device_name;
}

void xemu_vk_rebuild_fonts(void)
{
    if (!g_vk.initialized) {
        return;
    }
    vkDeviceWaitIdle(g_vk.device);
    ImGui_ImplVulkan_DestroyFontsTexture();
    ImGui_ImplVulkan_CreateFontsTexture();
}

template <typename F> static void RunCommands(F record)
{
    VkCommandBufferAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = g_vk.upload_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    CheckVkResult(vkAllocateCommandBuffers(g_vk.device, &alloc, &cmd));
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CheckVkResult(vkBeginCommandBuffer(cmd, &begin));
    record(cmd);
    CheckVkResult(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CheckVkResult(vkCreateFence(g_vk.device, &fi, NULL, &fence));
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CheckVkResult(vkQueueSubmit(g_vk.queue, 1, &submit, fence));
    CheckVkResult(vkWaitForFences(g_vk.device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(g_vk.device, fence, NULL);
    vkFreeCommandBuffers(g_vk.device, g_vk.upload_pool, 1, &cmd);
}

static void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss,
                    VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da,
                               .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                               .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

XemuTexture xemu_vk_texture_create(const void *pixels, int width, int height, int channels)
{
    if (!g_vk.initialized || width <= 0 || height <= 0) {
        return 0;
    }
    std::vector<uint8_t> rgba;
    if (channels == 3) {
        rgba.resize((size_t)width * height * 4);
        const uint8_t *src = (const uint8_t *)pixels;
        for (size_t i = 0; i < (size_t)width * height; i++) {
            rgba[i * 4] = src[i * 3];
            rgba[i * 4 + 1] = src[i * 3 + 1];
            rgba[i * 4 + 2] = src[i * 3 + 2];
            rgba[i * 4 + 3] = 0xff;
        }
        pixels = rgba.data();
    }
    VkDevice dev = g_vk.device;
    UiTexture tex = {};
    tex.width = width;
    tex.height = height;
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { (uint32_t)width, (uint32_t)height, 1 },
                             .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CheckVkResult(vkCreateImage(dev, &ii, NULL, &tex.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, tex.image, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CheckVkResult(vkAllocateMemory(dev, &ai, NULL, &tex.memory));
    CheckVkResult(vkBindImageMemory(dev, tex.image, tex.memory, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = tex.image,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CheckVkResult(vkCreateImageView(dev, &vi, NULL, &tex.view));

    const VkDeviceSize size = (VkDeviceSize)width * height * 4;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer staging;
    CheckVkResult(vkCreateBuffer(dev, &bi, NULL, &staging));
    vkGetBufferMemoryRequirements(dev, staging, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory staging_mem;
    CheckVkResult(vkAllocateMemory(dev, &ai, NULL, &staging_mem));
    CheckVkResult(vkBindBufferMemory(dev, staging, staging_mem, 0));
    void *map;
    CheckVkResult(vkMapMemory(dev, staging_mem, 0, size, 0, &map));
    memcpy(map, pixels, size);
    vkUnmapMemory(dev, staging_mem);
    RunCommands([&](VkCommandBuffer cmd) {
        Barrier(cmd, tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
        vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        Barrier(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    });
    vkDestroyBuffer(dev, staging, NULL);
    vkFreeMemory(dev, staging_mem, NULL);

    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(g_vk.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    XemuTexture id = (XemuTexture)(uintptr_t)set;
    g_vk.textures[id] = tex;
    return id;
}

void xemu_vk_texture_destroy(XemuTexture id)
{
    auto it = g_vk.textures.find(id);
    if (!g_vk.initialized || it == g_vk.textures.end()) {
        return;
    }
    vkDeviceWaitIdle(g_vk.device);
    UiTexture &tex = it->second;
    ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)(uintptr_t)id);
    vkDestroyImageView(g_vk.device, tex.view, NULL);
    vkDestroyImage(g_vk.device, tex.image, NULL);
    vkFreeMemory(g_vk.device, tex.memory, NULL);
    g_vk.textures.erase(it);
}

bool xemu_vk_texture_get_size(XemuTexture id, int *width, int *height)
{
    auto it = g_vk.textures.find(id);
    if (it == g_vk.textures.end()) {
        return false;
    }
    *width = it->second.width;
    *height = it->second.height;
    return true;
}

void xog_vk_present(ImDrawData *draw_data)
{
    ImGui_ImplVulkanH_Window *wd = &g_vk.wd;
    VkDevice dev = g_vk.device;
    if (g_vk.rebuild) {
        CreateOrResize(wd->Width, wd->Height);
    }
    VkSemaphore acquired = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore done = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(dev, wd->Swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_vk.rebuild = true;
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR) {
        return;
    }
    ImGui_ImplVulkanH_Frame *fd = &wd->Frames[wd->FrameIndex];
    CheckVkResult(vkWaitForFences(dev, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
    CheckVkResult(vkResetFences(dev, 1, &fd->Fence));
    CheckVkResult(vkResetCommandPool(dev, fd->CommandPool, 0));
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CheckVkResult(vkBeginCommandBuffer(fd->CommandBuffer, &begin));
    VkClearValue clear = {};
    VkRenderPassBeginInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = wd->RenderPass,
                                 .framebuffer = fd->Framebuffer,
                                 .renderArea = { { 0, 0 }, { (uint32_t)wd->Width, (uint32_t)wd->Height } },
                                 .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);
    CheckVkResult(vkEndCommandBuffer(fd->CommandBuffer));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired,
                            .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &fd->CommandBuffer,
                            .signalSemaphoreCount = 1, .pSignalSemaphores = &done };
    CheckVkResult(vkQueueSubmit(g_vk.queue, 1, &submit, fd->Fence));
    VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                 .pWaitSemaphores = &done, .swapchainCount = 1, .pSwapchains = &wd->Swapchain,
                                 .pImageIndices = &wd->FrameIndex };
    err = vkQueuePresentKHR(g_vk.queue, &present);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_vk.rebuild = true;
    }
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
    static bool first = true;
    if (first) {
        first = false;
        const int rc = sceSystemServiceHideSplashScreen();
        xog_ui_log("first frame presented, splash off: %08x", (unsigned)rc);
    }
}
