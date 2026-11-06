/*
 * hakuX HLE GPU — synchronization2's stages and accesses as Vulkan 1.0's
 * (vk/compat.c, for drivers without synchronization2). Pure: the host tests
 * include it with the Khronos headers alone.
 */
#ifndef HW_XBOX_HLE_VK_COMPAT_MAP_H
#define HW_XBOX_HLE_VK_COMPAT_MAP_H

#include <vulkan/vulkan_core.h>

/* The low 32 bits are 1.0's own flags; the 64-bit ones fold into the 1.0
 * stage that covers them. Pre-rasterization is the vertex stage: the device
 * has no tessellation or geometry shaders enabled, and a 1.0 mask naming them
 * is invalid then. */
static inline VkPipelineStageFlags hle_compat_stages(VkPipelineStageFlags2 s)
{
    VkPipelineStageFlags o = (VkPipelineStageFlags)(s & 0xFFFFFFFFull);
    if (s & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
             VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
        o |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (s & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT)) {
        o |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    }
    if (s & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) {
        o |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    }
    return o;
}

static inline VkAccessFlags hle_compat_access(VkAccessFlags2 a)
{
    VkAccessFlags o = (VkAccessFlags)(a & 0xFFFFFFFFull);
    if (a & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) {
        o |= VK_ACCESS_SHADER_READ_BIT;
    }
    if (a & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) {
        o |= VK_ACCESS_SHADER_WRITE_BIT;
    }
    return o;
}

/* A barrier call's one source and destination mask: the union of its
 * barriers' (none: the top and the bottom of the pipe, 1.0's "no stage") */
static inline VkPipelineStageFlags hle_compat_src(VkPipelineStageFlags s)
{
    return s ? s : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
}

static inline VkPipelineStageFlags hle_compat_dst(VkPipelineStageFlags s)
{
    return s ? s : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
}

#endif
