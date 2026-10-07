/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
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

#include "renderer.h"
#include "hw/xbox/xemu-timing.h"

static void create_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.queue_family,
    };
    VK_CHECK(
        vkCreateCommandPool(r->device, &create_info, NULL, &r->command_pool));
}

static void destroy_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyCommandPool(r->device, r->command_pool, NULL);
}

static void create_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ARRAY_SIZE(r->command_buffers),
    };
    VK_CHECK(
        vkAllocateCommandBuffers(r->device, &alloc_info, r->command_buffers));

    r->command_buffer = r->command_buffers[0];
    r->aux_command_buffer = r->command_buffers[1];

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &r->aux_fence));
}

static void destroy_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeCommandBuffers(r->device, r->command_pool,
                         ARRAY_SIZE(r->command_buffers), r->command_buffers);
    vkDestroyFence(r->device, r->aux_fence, NULL);
    r->aux_fence = VK_NULL_HANDLE;

    r->command_buffer = VK_NULL_HANDLE;
    r->aux_command_buffer = VK_NULL_HANDLE;
}

/*
 * XPSemu: the PS5's Vulkan driver has no GPU-done interrupt: its waits poll
 * for 1.5 ms, then sleep in 1 ms steps, so a wait for work that takes 2 ms
 * can cost 3. The GPU thread has a core of its own (pinned), so it keeps
 * polling here instead, up to 50 ms; the driver's wait after it then
 * returns at once. Other threads (the UI) share cores: no polling there.
 */
void pgraph_vk_poll_fence(PGRAPHState *pg, VkFence fence)
{
#ifdef __PROSPERO__
    PGRAPHVkState *r = pg->vk_renderer_state;
    NV2AState *d = container_of(pg, NV2AState, pgraph);

    if (!qemu_thread_is_self(&d->pfifo.thread)) {
        return;
    }
    int64_t until = get_clock() + 50 * SCALE_MS;
    while (vkGetFenceStatus(r->device, fence) == VK_NOT_READY &&
           get_clock() < until) {
        for (int i = 0; i < 32; i++) {
            __builtin_ia32_pause();
        }
    }
#endif
}

VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_aux_command_buffer);
    r->in_aux_command_buffer = true;

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->aux_command_buffer, &begin_info));

    return r->aux_command_buffer;
}

void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_aux_command_buffer);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    uint64_t xt_aux = xemu_timing_now();
    qemu_mutex_lock(&r->queue_lock);
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, r->aux_fence));
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_AUX);
    pgraph_vk_poll_fence(pg, r->aux_fence);
    VK_CHECK(vkQueueWaitIdle(r->queue));
    VK_CHECK(vkResetFences(r->device, 1, &r->aux_fence));
    qemu_mutex_unlock(&r->queue_lock);
    xemu_timing_add(XT_AUX_WAIT, xt_aux);

    r->in_aux_command_buffer = false;
}

void pgraph_vk_init_command_buffers(PGRAPHState *pg)
{
    create_command_pool(pg);
    create_command_buffers(pg);
}

void pgraph_vk_finalize_command_buffers(PGRAPHState *pg)
{
    destroy_command_buffers(pg);
    destroy_command_pool(pg);
}