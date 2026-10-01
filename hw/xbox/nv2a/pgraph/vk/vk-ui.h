/*
 * Geforce NV2A PGRAPH Vulkan Renderer: interface for a Vulkan UI
 *
 * Builds without OpenGL (the PS5) draw the UI with the renderer's own Vulkan
 * device, sampling the display image directly instead of sharing it with GL.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_VK_UI_H
#define HW_XBOX_NV2A_PGRAPH_VK_VK_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PGRAPHVkUIContext {
    uint32_t api_version;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    bool display_extension_enabled; /* VK_KHR_surface + VK_KHR_display */
} PGRAPHVkUIContext;

/* False until the Vulkan renderer has created its device. */
bool pgraph_vk_get_ui_context(PGRAPHVkUIContext *ctx);

/* Hold around every vkQueueSubmit/vkQueuePresentKHR on the shared queue. */
void pgraph_vk_lock_queue(void);
void pgraph_vk_unlock_queue(void);

/*
 * The display image, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL. Valid between
 * a successful nv2a_get_framebuffer_surface() and
 * nv2a_release_framebuffer_surface().
 */
bool pgraph_vk_get_display(VkImage *image, VkImageView *view, int *width,
                           int *height);

/* The device's name, for the UI's system information. */
const char *pgraph_vk_get_device_name(void);

/*
 * Disk caches (XPSemu): compiled SPIR-V in cache/spirv (perf.cache_shaders)
 * and the Vulkan pipeline cache in cache/vk-pipelines.bin, both under
 * xemu's base path. Asking saves the pipeline cache from the GPU thread
 * (the next time it runs). The counters are since start-up.
 */
void pgraph_vk_request_cache_save(void);
extern unsigned int pgraph_vk_spirv_cache_hits, pgraph_vk_spirv_cache_misses;

#ifdef __cplusplus
}
#endif

#endif
