/* synchronization2's stages and accesses as Vulkan 1.0's (vk/compat_map.h) */
#include "hw/xbox/hle/vk/compat_map.h"
#include "hle_test.h"

static void test_stages(void)
{
    /* 1.0's own bits are kept */
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT),
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_TRANSFER_BIT), VK_PIPELINE_STAGE_TRANSFER_BIT);
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_HOST_BIT), VK_PIPELINE_STAGE_HOST_BIT);
    /* the 64-bit ones fold into the 1.0 stage that covers them */
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_COPY_BIT), VK_PIPELINE_STAGE_TRANSFER_BIT);
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT |
                                VK_PIPELINE_STAGE_2_RESOLVE_BIT),
              VK_PIPELINE_STAGE_TRANSFER_BIT);
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                                VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT),
              VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT),
              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT),
              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    /* none: the ends of the pipe */
    HT_EQ_U32(hle_compat_stages(VK_PIPELINE_STAGE_2_NONE), 0);
    HT_EQ_U32(hle_compat_src(0), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    HT_EQ_U32(hle_compat_dst(0), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    HT_EQ_U32(hle_compat_src(VK_PIPELINE_STAGE_TRANSFER_BIT), VK_PIPELINE_STAGE_TRANSFER_BIT);
}

static void test_access(void)
{
    HT_EQ_U32(hle_compat_access(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT),
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    HT_EQ_U32(hle_compat_access(VK_ACCESS_2_TRANSFER_WRITE_BIT), VK_ACCESS_TRANSFER_WRITE_BIT);
    HT_EQ_U32(hle_compat_access(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT), VK_ACCESS_SHADER_READ_BIT);
    HT_EQ_U32(hle_compat_access(VK_ACCESS_2_SHADER_STORAGE_READ_BIT), VK_ACCESS_SHADER_READ_BIT);
    HT_EQ_U32(hle_compat_access(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT), VK_ACCESS_SHADER_WRITE_BIT);
    HT_EQ_U32(hle_compat_access(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT),
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT);
    HT_EQ_U32(hle_compat_access(VK_ACCESS_2_NONE), 0);
}

int main(void)
{
    HT_RUN(test_stages);
    HT_RUN(test_access);
    HT_DONE();
}
