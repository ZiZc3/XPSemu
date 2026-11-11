/*
 * hakuX HLE GPU — the display's gamma ramp (SetGammaRamp) at swap: the
 * swap's blit becomes a fullscreen pass that samples the presented surface
 * and maps each channel through the ramp (the NV2A DAC does this at
 * scanout). Identity ramps keep the blit (back.c).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/shader/glsl_compile.h"
#include "hw/xbox/hle/vk/back_int.h"

#define T (g_hvk.t)
#define GM_SLOTS  HLE_FRAMES            /* one per command buffer in flight */
#define GM_STRIDE 1024                  /* per slot in the ramp buffer (768 used) */
#define GM_VIEWS  8

static const char k_vs[] =
    "#version 450\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char k_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(set = 0, binding = 1, std140) uniform G { uvec4 w[48]; } g;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "uint lut(uint i) {\n"
    "  uint word = g.w[i >> 4][(i >> 2) & 3u];\n"
    "  return (word >> ((i & 3u) * 8u)) & 0xFFu;\n"
    "}\n"
    "void main() {\n"
    "  vec4 c = texture(src, uv);\n"
    "  uvec3 v = uvec3(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  o = vec4(float(lut(v.r)), float(lut(256u + v.g)), float(lut(512u + v.b)), 255.0)"
    " / 255.0;\n"
    "  o.a = c.a;\n"
    "}\n";

static struct {
    bool ready, failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkSampler smp;
    VkDescriptorPool pool;
    VkDescriptorSet set[GM_SLOTS];
    VkBuffer ubo;
    VkDeviceMemory mem;
    uint8_t *map;
    struct {
        VkImage img;
        VkImageView view;
    } dst[GM_VIEWS];
    unsigned ndst;
    uint32_t dst_gen;                   /* the display images' generation they are views of */
} g_gm;

static VkShaderModule module(HleShaderStage st, const char *src)
{
    size_t n = 0;
    uint32_t *spv = hle_glsl_compile(st, src, &n);
    if (!spv) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = n * 4, .pCode = spv };
    VkShaderModule m = VK_NULL_HANDLE;
    T.vkCreateShaderModule(g_hvk.dev, &ci, NULL, &m);
    g_free(spv);
    return m;
}

static bool init(void)
{
    if (g_gm.ready || g_gm.failed) {
        return g_gm.ready;
    }
    g_gm.failed = true;                 /* until everything below succeeds */
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = b };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &g_gm.dsl };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GM_SLOTS },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, GM_SLOTS },
    };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = GM_SLOTS, .poolSizeCount = 2,
                                       .pPoolSizes = ps };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = GM_SLOTS * GM_STRIDE,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &dci, NULL, &g_gm.dsl) != VK_SUCCESS ||
        T.vkCreatePipelineLayout(g_hvk.dev, &pli, NULL, &g_gm.pl) != VK_SUCCESS ||
        T.vkCreateSampler(g_hvk.dev, &sci, NULL, &g_gm.smp) != VK_SUCCESS ||
        T.vkCreateDescriptorPool(g_hvk.dev, &dpi, NULL, &g_gm.pool) != VK_SUCCESS ||
        T.vkCreateBuffer(g_hvk.dev, &bci, NULL, &g_gm.ubo) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "gamma: setup failed (ramps not applied)");
        return false;
    }
    VkMemoryRequirements mr;
    T.vkGetBufferMemoryRequirements(g_hvk.dev, g_gm.ubo, &mr);
    int mt = hle_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_gm.mem) != VK_SUCCESS ||
        T.vkBindBufferMemory(g_hvk.dev, g_gm.ubo, g_gm.mem, 0) != VK_SUCCESS ||
        T.vkMapMemory(g_hvk.dev, g_gm.mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_gm.map) !=
            VK_SUCCESS) {
        hle_log("hakuX-hle-back", "gamma: buffer failed (ramps not applied)");
        return false;
    }
    VkDescriptorSetLayout ls[GM_SLOTS] = { g_gm.dsl, g_gm.dsl, g_gm.dsl };
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = g_gm.pool,
                                        .descriptorSetCount = GM_SLOTS, .pSetLayouts = ls };
    if (T.vkAllocateDescriptorSets(g_hvk.dev, &dai, g_gm.set) != VK_SUCCESS) {
        return false;
    }
    VkShaderModule vm = module(HLE_SHADER_VERTEX, k_vs), fm = module(HLE_SHADER_FRAGMENT, k_fs);
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo ri = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
        VkGraphicsPipelineCreateInfo gci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
            .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
            .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy,
            .layout = g_gm.pl };
        T.vkCreateGraphicsPipelines(g_hvk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &g_gm.pipe);
    }
    if (vm) {
        T.vkDestroyShaderModule(g_hvk.dev, vm, NULL);
    }
    if (fm) {
        T.vkDestroyShaderModule(g_hvk.dev, fm, NULL);
    }
    if (!g_gm.pipe) {
        hle_log("hakuX-hle-back", "gamma: pipeline failed (ramps not applied)");
        return false;
    }
    g_gm.failed = false;
    g_gm.ready = true;
    hle_log("hakuX-hle-back", "gamma: display pass ready");
    return true;
}

/* A view of display image img, kept while the display's images are the same
 * ones: made again (a new size), a handle can repeat a destroyed image's,
 * and the views of the old ones go (their GPU work done: the display waited
 * for the device before it destroyed them). */
static VkImageView dst_view(VkImage img)
{
    if (g_gm.dst_gen != hle_disp_gen()) {
        for (unsigned i = 0; i < g_gm.ndst; i++) {
            T.vkDestroyImageView(g_hvk.dev, g_gm.dst[i].view, NULL);
        }
        g_gm.ndst = 0;
        g_gm.dst_gen = hle_disp_gen();
    }
    for (unsigned i = 0; i < g_gm.ndst; i++) {
        if (g_gm.dst[i].img == img) {
            return g_gm.dst[i].view;
        }
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView v = VK_NULL_HANDLE;
    if (T.vkCreateImageView(g_hvk.dev, &vci, NULL, &v) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    unsigned i = g_gm.ndst < GM_VIEWS ? g_gm.ndst++ : 0;  /* display slots: a few */
    if (g_gm.dst[i].view) {
        T.vkDestroyImageView(g_hvk.dev, g_gm.dst[i].view, NULL);
    }
    g_gm.dst[i].img = img;
    g_gm.dst[i].view = v;
    return v;
}

bool hle_gamma_pass(VkCommandBuffer cb, unsigned slot, const uint8_t ramp[3][256],
                    HleVkSurf *src, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                    VkImage dst, uint32_t dw, uint32_t dh)
{
    if (!init()) {
        return false;
    }
    VkImageView dv = dst_view(dst);
    if (!dv) {
        return false;
    }
    slot %= GM_SLOTS;                   /* this frame slot's earlier use has completed */
    memcpy(g_gm.map + slot * GM_STRIDE, ramp, 768);
    hle_surf_transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii = { g_gm.smp, src->view, src->layout };   /* or GENERAL */
    VkDescriptorBufferInfo bi = { g_gm.ubo, slot * GM_STRIDE, 768 };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_gm.set[slot],
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_gm.set[slot],
          .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi },
    };
    T.vkUpdateDescriptorSets(g_hvk.dev, 2, w, 0, NULL);
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = { { 0, 0 }, { dw, dh } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &ca };
    T.vkCmdBeginRendering(cb, &ri);
    /* the whole of src over a viewport its shown part (sx..sw x sy..sh) fills dw x dh of
     * (sx, sy: XOG patch 274, the picture's first rows/columns left out, xogfix=edgecrop) */
    sx = sw > 1u ? MIN(sx, sw - 1u) : 0u;
    sy = sh > 1u ? MIN(sy, sh - 1u) : 0u;
    const float pw = (float)MAX(sw - sx, 1u), ph = (float)MAX(sh - sy, 1u);
    VkViewport vp = { -(float)sx * (float)dw / pw, -(float)sy * (float)dh / ph,
                      (float)dw * src->w / pw, (float)dh * src->h / ph, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { dw, dh } };
    T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gm.pipe);
    hle_draw_state_lost();
    T.vkCmdSetViewport(cb, 0, 1, &vp);
    T.vkCmdSetScissor(cb, 0, 1, &sc);
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gm.pl, 0, 1,
                              &g_gm.set[slot], 0, NULL);
    T.vkCmdDraw(cb, 3, 1, 0, 0);
    T.vkCmdEndRendering(cb);
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

/* ---- patch 337: the video overlay's colour key ---------------------------------- */

/* PVIDEO's colour key (as xemu's display shader): the overlay shows where
 * the frame's pixel is the key (its alpha ignored); the frame read as the
 * swap copied it (its pixel under each display pixel, nearest), the
 * overlay's source rect scaled onto its output rect. */
static const char k_ok_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D frame;\n"
    "layout(set = 0, binding = 1) uniform sampler2D ovl;\n"
    "layout(push_constant, std430) uniform P { vec4 fmap; vec4 omap; uint key; } pc;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  ivec2 fs = textureSize(frame, 0);\n"
    "  ivec2 fp = clamp(ivec2(floor(gl_FragCoord.xy * pc.fmap.xy + pc.fmap.zw)), ivec2(0), fs - 1);\n"
    "  uvec3 u = uvec3(clamp(texelFetch(frame, fp, 0).rgb, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  if ((u.r << 16 | u.g << 8 | u.b) != (pc.key & 0xFFFFFFu)) discard;\n"
    "  o = vec4(texture(ovl, gl_FragCoord.xy * pc.omap.xy + pc.omap.zw).rgb, 1.0);\n"
    "}\n";

static struct {
    bool ready, failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkSampler near;
    VkDescriptorPool pool;
    VkDescriptorSet set[GM_SLOTS];
} g_ok;

static bool ok_init(void)
{
    if (g_ok.ready || g_ok.failed) {
        return g_ok.ready;
    }
    g_ok.failed = true;
    if (!init()) {                      /* the gamma pass's sampler and display views */
        return false;
    }
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = b };
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36 };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * GM_SLOTS };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = GM_SLOTS, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &dci, NULL, &g_ok.dsl) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "overlay key: setup failed (overlay drawn without its key)");
        return false;
    }
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &g_ok.dsl,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    if (T.vkCreatePipelineLayout(g_hvk.dev, &pli, NULL, &g_ok.pl) != VK_SUCCESS ||
        T.vkCreateSampler(g_hvk.dev, &sci, NULL, &g_ok.near) != VK_SUCCESS ||
        T.vkCreateDescriptorPool(g_hvk.dev, &dpi, NULL, &g_ok.pool) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "overlay key: setup failed (overlay drawn without its key)");
        return false;
    }
    VkDescriptorSetLayout ls[GM_SLOTS];
    for (unsigned i = 0; i < GM_SLOTS; i++) {
        ls[i] = g_ok.dsl;
    }
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = g_ok.pool,
                                        .descriptorSetCount = GM_SLOTS, .pSetLayouts = ls };
    if (T.vkAllocateDescriptorSets(g_hvk.dev, &dai, g_ok.set) != VK_SUCCESS) {
        return false;
    }
    VkShaderModule vm = module(HLE_SHADER_VERTEX, k_vs), fm = module(HLE_SHADER_FRAGMENT, k_ok_fs);
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo ri = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
        VkGraphicsPipelineCreateInfo gci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
            .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
            .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy,
            .layout = g_ok.pl };
        T.vkCreateGraphicsPipelines(g_hvk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &g_ok.pipe);
    }
    if (vm) {
        T.vkDestroyShaderModule(g_hvk.dev, vm, NULL);
    }
    if (fm) {
        T.vkDestroyShaderModule(g_hvk.dev, fm, NULL);
    }
    if (!g_ok.pipe) {
        hle_log("hakuX-hle-back", "overlay key: pipeline failed (overlay drawn without its key)");
        return false;
    }
    g_ok.failed = false;
    g_ok.ready = true;
    hle_log("hakuX-hle-back", "overlay key: pass ready (patch 337)");
    return true;
}

bool hle_ovlkey_pass(VkCommandBuffer cb, unsigned slot, HleVkSurf *src, uint32_t sx, uint32_t sy,
                     uint32_t sw, uint32_t sh, VkImageView ovl, uint32_t ow, uint32_t oh,
                     const int32_t s[4], const int32_t d[4], uint32_t key, VkImage dst,
                     uint32_t dw, uint32_t dh)
{
    if (!ovl || !ow || !oh || !dw || !dh || d[2] <= d[0] || d[3] <= d[1] || s[2] <= s[0] ||
        s[3] <= s[1] || !ok_init()) {
        return false;
    }
    VkImageView dv = dst_view(dst);
    if (!dv) {
        return false;
    }
    slot %= GM_SLOTS;                   /* this frame slot's earlier use has completed */
    hle_surf_transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii[2] = {
        { g_ok.near, src->view, src->layout },
        { g_gm.smp, ovl, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_ok.set[slot],
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_ok.set[slot],
          .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1] },
    };
    T.vkUpdateDescriptorSets(g_hvk.dev, 2, w, 0, NULL);
    /* the frame's pixel under a display pixel: the swap's copy of sx..sw x sy..sh
     * over dw x dh; the overlay's texel: its src rect over its dst rect */
    sx = sw > 1u ? MIN(sx, sw - 1u) : 0u;
    sy = sh > 1u ? MIN(sy, sh - 1u) : 0u;
    const float fx = (float)MAX(sw - sx, 1u) / (float)dw, fy = (float)MAX(sh - sy, 1u) / (float)dh;
    const float ox = (float)(s[2] - s[0]) / (float)(d[2] - d[0]);
    const float oy = (float)(s[3] - s[1]) / (float)(d[3] - d[1]);
    struct { float fmap[4], omap[4]; uint32_t key; } pc = {
        { fx, fy, (float)sx, (float)sy },
        { ox / (float)ow, oy / (float)oh, ((float)s[0] - (float)d[0] * ox) / (float)ow,
          ((float)s[1] - (float)d[1] * oy) / (float)oh },
        key };
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = { { 0, 0 }, { dw, dh } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &ca };
    T.vkCmdBeginRendering(cb, &ri);
    const int32_t x0 = MAX(d[0], 0), y0 = MAX(d[1], 0);
    const int32_t x1 = MIN(d[2], (int32_t)dw), y1 = MIN(d[3], (int32_t)dh);
    VkViewport vp = { 0.0f, 0.0f, (float)dw, (float)dh, 0.0f, 1.0f };
    VkRect2D sc = { { x0, y0 }, { x1 > x0 ? (uint32_t)(x1 - x0) : 0u, y1 > y0 ? (uint32_t)(y1 - y0) : 0u } };
    T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ok.pipe);
    hle_draw_state_lost();
    T.vkCmdSetViewport(cb, 0, 1, &vp);
    T.vkCmdSetScissor(cb, 0, 1, &sc);
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ok.pl, 0, 1,
                              &g_ok.set[slot], 0, NULL);
    T.vkCmdPushConstants(cb, g_ok.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36, &pc);
    if (sc.extent.width && sc.extent.height) {
        T.vkCmdDraw(cb, 3, 1, 0, 0);
    }
    T.vkCmdEndRendering(cb);
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

/* ---- patch 355: the movie upscaler ---------------------------------------------- */

/* A movie frame (a texture the CPU decodes and uploads again and again, a
 * YUV texture, the video overlay) enlarged into an image of its own at the
 * resolution scale, once per upload, instead of being stretched bilinearly
 * by every draw: Catmull-Rom (bicubic, 16 texels) kept inside the four
 * nearest texels' range (no dark or bright rings around edges, as FSR1's
 * deringing), sharper than bilinear without halos. The source is sampled as
 * its format's own channels (an identity view); the target is R8G8B8A8, its
 * sampled view with the texture's swizzle. */
static const char k_mu_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "vec4 cr(float f) {\n"
    "  return vec4(f * (-0.5 + f * (1.0 - 0.5 * f)), 1.0 + f * f * (-2.5 + 1.5 * f),\n"
    "              f * (0.5 + f * (2.0 - 1.5 * f)), f * f * (-0.5 + 0.5 * f));\n"
    "}\n"
    "void main() {\n"
    "  ivec2 sz = textureSize(src, 0);\n"
    "  vec2 p = uv * vec2(sz) - 0.5;\n"
    "  vec2 b = floor(p);\n"
    "  vec2 f = p - b;\n"
    "  ivec2 i = ivec2(b), hi = sz - 1;\n"
    "  vec4 wx = cr(f.x), wy = cr(f.y);\n"
    "  vec4 c = vec4(0.0), mn = vec4(1.0), mx = vec4(0.0);\n"
    "  for (int y = 0; y < 4; y++) {\n"
    "    vec4 row = vec4(0.0);\n"
    "    for (int x = 0; x < 4; x++) {\n"
    "      vec4 t = texelFetch(src, clamp(i + ivec2(x - 1, y - 1), ivec2(0), hi), 0);\n"
    "      row += t * wx[x];\n"
    "      if ((x == 1 || x == 2) && (y == 1 || y == 2)) {\n"
    "        mn = min(mn, t);\n"
    "        mx = max(mx, t);\n"
    "      }\n"
    "    }\n"
    "    c += row * wy[y];\n"
    "  }\n"
    "  o = clamp(c, mn, mx);\n"
    "}\n";

#define MU_SETS 32                      /* the textures' and the overlay's, live or retiring */

static struct {
    bool ready, failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkSampler near;
    VkDescriptorPool pool;
} g_mu;

static bool mu_init(void)
{
    if (g_mu.ready || g_mu.failed) {
        return g_mu.ready;
    }
    g_mu.failed = true;
    VkDescriptorSetLayoutBinding b = {
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MU_SETS };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                       .maxSets = MU_SETS, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &dci, NULL, &g_mu.dsl) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "movie upscaler: setup failed (movies drawn as before)");
        return false;
    }
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &g_mu.dsl };
    if (T.vkCreatePipelineLayout(g_hvk.dev, &pli, NULL, &g_mu.pl) != VK_SUCCESS ||
        T.vkCreateSampler(g_hvk.dev, &sci, NULL, &g_mu.near) != VK_SUCCESS ||
        T.vkCreateDescriptorPool(g_hvk.dev, &dpi, NULL, &g_mu.pool) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "movie upscaler: setup failed (movies drawn as before)");
        return false;
    }
    VkShaderModule vm = module(HLE_SHADER_VERTEX, k_vs), fm = module(HLE_SHADER_FRAGMENT, k_mu_fs);
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo ri = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
        VkGraphicsPipelineCreateInfo gci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
            .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
            .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy,
            .layout = g_mu.pl };
        T.vkCreateGraphicsPipelines(g_hvk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &g_mu.pipe);
    }
    if (vm) {
        T.vkDestroyShaderModule(g_hvk.dev, vm, NULL);
    }
    if (fm) {
        T.vkDestroyShaderModule(g_hvk.dev, fm, NULL);
    }
    if (!g_mu.pipe) {
        hle_log("hakuX-hle-back", "movie upscaler: pipeline failed (movies drawn as before)");
        return false;
    }
    g_mu.failed = false;
    g_mu.ready = true;
    hle_log("hakuX-hle-back", "movie upscaler: pass ready (patch 355)");
    return true;
}

VkDescriptorSet hle_movieup_set_new(VkImageView src)
{
    if (!src || !mu_init()) {
        return VK_NULL_HANDLE;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = g_mu.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &g_mu.dsl };
    if (T.vkAllocateDescriptorSets(g_hvk.dev, &dai, &set) != VK_SUCCESS) {
        return VK_NULL_HANDLE;          /* all in use: this one drawn as before */
    }
    VkDescriptorImageInfo ii = { g_mu.near, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &ii };
    T.vkUpdateDescriptorSets(g_hvk.dev, 1, &w, 0, NULL);
    return set;
}

void hle_movieup_set_free(VkDescriptorSet set)
{
    if (set && g_mu.pool) {
        T.vkFreeDescriptorSets(g_hvk.dev, g_mu.pool, 1, &set);
    }
}

void hle_movieup_pass(VkCommandBuffer cb, VkDescriptorSet set, VkImage dst, VkImageView dst_view,
                      uint32_t dw, uint32_t dh, VkImageLayout to)
{
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = { { 0, 0 }, { dw, dh } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &ca };
    T.vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0.0f, 0.0f, (float)dw, (float)dh, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { dw, dh } };
    T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mu.pipe);
    hle_draw_state_lost();
    T.vkCmdSetViewport(cb, 0, 1, &vp);
    T.vkCmdSetScissor(cb, 0, 1, &sc);
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mu.pl, 0, 1, &set, 0, NULL);
    T.vkCmdDraw(cb, 3, 1, 0, 0);
    T.vkCmdEndRendering(cb);
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, to);
}

/* ---- patch 363: ambient occlusion (SSAO) ---------------------------------------- */

/* "Ambient occlusion" (Off / Low / High, ssao= start line, the quick menu's
 * Picture row at once; XOG_FIX nossao: never). Off: nothing below runs.
 *
 * Applied at the swap to the display image (after the gamma pass / blit, before
 * the video overlay and the window's post passes: upscaler, CAS, CRT), from the
 * frame's main depth buffer (back.c ao_depth: the one drawn with the presented
 * target the most this frame), as it is at the swap. No projection is known,
 * so it is depth-only: the NV2A's window depth (z-buffer) is affine in screen
 * space over a plane, so pairs of samples at +o and -o around a pixel average
 * to its own depth on any flat surface and to a nearer one inside a crease or
 * where objects meet (the pair's second difference, by 1 - depth: a relative
 * depth, scale-free); a pair crossing a big jump (a silhouette) is left out,
 * so nothing is darkened around objects in front of others (no halos), and
 * the far plane (sky, cleared depth) never is. Three passes:
 *   1. half the depth buffer's size: 8 pairs a pixel (rotated per pixel),
 *      AO and the pixel's depth (R16G16F)
 *   2. half size: a 5x5 blur that keeps to similar depths (R8)
 *   3. the display image multiplied by it (bilinear), far-plane pixels kept.
 * Images grow only (a smaller frame uses their corner), so a size change
 * waits for the GPU once (hle_ssao_regrow) and never again for a smaller one. */
static const char k_ao_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
    "layout(push_constant, std430) uniform P { vec4 a; vec4 b; } pc;\n"
    /* a: strength, radius (guest pixels), 1 / guest w, 1 / guest h;
     * b: 1 / out w, 1 / out h, the crease scale (~focal length, guest pixels), a depth step */
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  ivec2 dsz = textureSize(dep, 0), hi = dsz - 1;\n"
    "  vec2 fs = vec2(dsz);\n"
    "  vec2 uv = gl_FragCoord.xy * pc.b.xy;\n"
    "  float dc = texelFetch(dep, clamp(ivec2(uv * fs), ivec2(0), hi), 0).r;\n"
    "  if (dc >= 0.99999) { o = vec4(1.0, 1.0, 0.0, 0.0); return; }\n"
    "  float inv = 1.0 / max(1.0 - dc, 1e-6);\n"
    "  float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "  float occ = 0.0;\n"
    "  for (int i = 0; i < 8; i++) {\n"
    "    float ang = (float(i) + n) * 0.39269908;\n"
    "    float r = pc.a.y * (0.3 + 0.7 * fract(n * 7.0 + float(i) * 0.618034));\n"
    "    vec2 off = vec2(cos(ang), sin(ang)) * r * pc.a.zw;\n"
    "    float d1 = texelFetch(dep, clamp(ivec2((uv + off) * fs), ivec2(0), hi), 0).r;\n"
    "    float d2 = texelFetch(dep, clamp(ivec2((uv - off) * fs), ivec2(0), hi), 0).r;\n"
    "    float k = inv * pc.b.z / r;\n"
    "    float crease = (dc - 0.5 * (d1 + d2)) * k;\n"
    "    float jump = max(abs(d1 - dc), abs(d2 - dc)) * k;\n"
    "    float lo = 0.1 + 3.0 * pc.b.w * k;\n"  /* above the depth's own steps (D16) */
    "    occ += smoothstep(lo, lo + 0.7, crease) * (1.0 - smoothstep(3.0, 8.0, jump));\n"
    "  }\n"
    "  o = vec4(1.0 - pc.a.x * occ * 0.125, dc, 0.0, 0.0);\n"
    "}\n";

static const char k_aoblur_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D ao;\n"
    "layout(push_constant, std430) uniform P { vec4 a; vec4 b; } pc;\n"   /* a.xy: the last texel */
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy), hi = ivec2(pc.a.xy);\n"
    "  vec2 c = texelFetch(ao, p, 0).rg;\n"
    "  if (c.y >= 0.99999) { o = vec4(1.0); return; }\n"
    "  float inv = 10.0 / max(1.0 - c.y, 1e-6);\n"
    "  float s = 0.0, w = 0.0;\n"
    "  for (int y = -2; y <= 2; y++) {\n"
    "    for (int x = -2; x <= 2; x++) {\n"
    "      vec2 t = texelFetch(ao, clamp(p + ivec2(x, y), ivec2(0), hi), 0).rg;\n"
    "      float wt = exp(-0.22 * float(x * x + y * y)) * max(0.0, 1.0 - abs(t.y - c.y) * inv);\n"
    "      s += t.x * wt;\n"
    "      w += wt;\n"
    "    }\n"
    "  }\n"
    "  o = vec4(s / max(w, 1e-4), 0.0, 0.0, 1.0);\n"
    "}\n";

static const char k_aocomp_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D aob;\n"
    "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
    "layout(push_constant, std430) uniform P { vec4 a; vec4 b; } pc;\n"
    /* a: display pixel -> the depth buffer's uv (scale, offset); b: AO's used size, 1 / its image size */
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 uv = gl_FragCoord.xy * pc.a.xy + pc.a.zw;\n"
    "  ivec2 dsz = textureSize(dep, 0);\n"
    "  float d = texelFetch(dep, clamp(ivec2(uv * vec2(dsz)), ivec2(0), dsz - 1), 0).r;\n"
    "  vec2 t = clamp(uv * pc.b.xy, vec2(0.5), pc.b.xy - 0.5) * pc.b.zw;\n"
    "  float v = d >= 0.99999 ? 1.0 : texture(aob, t).r;\n"
    "  o = vec4(v, v, v, 1.0);\n"
    "}\n";

volatile int g_hle_ssao;                /* 0 off, 1 low, 2 high (the setting) */

void hle_vk_set_ssao(int level)
{
    g_hle_ssao = level <= 0 ? 0 : level >= 2 ? 2 : 1;
}

int hle_ssao_level(void)
{
    static int s_fix = -1;
    if (s_fix < 0) {
        s_fix = !hle_xog_fix_word("nossao");
        if (!s_fix) {
            hle_log("hakuX-hle-back", "ambient occlusion: off (XOG_FIX nossao, patch 363)");
        }
    }
    return s_fix ? g_hle_ssao : 0;
}

static struct {
    bool ready, failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline ao, blur, comp;
    VkSampler near;
    VkDescriptorPool pool;
    VkDescriptorSet set[GM_SLOTS][3];
    VkImage img[2];                     /* 0: AO + depth (R16G16F), 1: blurred (R8) */
    VkDeviceMemory mem[2];
    VkImageView view[2];
    uint32_t w, h;                      /* their size (half the largest depth buffer yet) */
} g_ao;

static VkPipeline ao_pipe(const char *fs, VkFormat cf, bool mul)
{
    VkShaderModule vm = module(HLE_SHADER_VERTEX, k_vs), fm = module(HLE_SHADER_FRAGMENT, fs);
    VkPipeline p = VK_NULL_HANDLE;
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        /* mul: the display image times the AO (its alpha kept) */
        VkPipelineColorBlendAttachmentState cba = {
            .blendEnable = mul ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO, .dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = mul ? 0x7u : 0xFu };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkPipelineRenderingCreateInfo ri = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
        VkGraphicsPipelineCreateInfo gci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
            .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
            .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy,
            .layout = g_ao.pl };
        if (T.vkCreateGraphicsPipelines(g_hvk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &p) != VK_SUCCESS) {
            p = VK_NULL_HANDLE;
        }
    }
    if (vm) {
        T.vkDestroyShaderModule(g_hvk.dev, vm, NULL);
    }
    if (fm) {
        T.vkDestroyShaderModule(g_hvk.dev, fm, NULL);
    }
    return p;
}

static bool ao_init(void)
{
    if (g_ao.ready || g_ao.failed) {
        return g_ao.ready;
    }
    g_ao.failed = true;
    if (!init()) {                      /* the gamma pass's linear sampler and display views */
        return false;
    }
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = b };
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 3 * GM_SLOTS };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 3 * GM_SLOTS, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &dci, NULL, &g_ao.dsl) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "ambient occlusion: setup failed (not applied)");
        return false;
    }
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &g_ao.dsl,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    if (T.vkCreatePipelineLayout(g_hvk.dev, &pli, NULL, &g_ao.pl) != VK_SUCCESS ||
        T.vkCreateSampler(g_hvk.dev, &sci, NULL, &g_ao.near) != VK_SUCCESS ||
        T.vkCreateDescriptorPool(g_hvk.dev, &dpi, NULL, &g_ao.pool) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "ambient occlusion: setup failed (not applied)");
        return false;
    }
    VkDescriptorSetLayout ls[3 * GM_SLOTS];
    for (unsigned i = 0; i < 3 * GM_SLOTS; i++) {
        ls[i] = g_ao.dsl;
    }
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = g_ao.pool,
                                        .descriptorSetCount = 3 * GM_SLOTS, .pSetLayouts = ls };
    if (T.vkAllocateDescriptorSets(g_hvk.dev, &dai, &g_ao.set[0][0]) != VK_SUCCESS) {
        return false;
    }
    g_ao.ao = ao_pipe(k_ao_fs, VK_FORMAT_R16G16_SFLOAT, false);
    g_ao.blur = ao_pipe(k_aoblur_fs, VK_FORMAT_R8_UNORM, false);
    g_ao.comp = ao_pipe(k_aocomp_fs, VK_FORMAT_R8G8B8A8_UNORM, true);
    if (!g_ao.ao || !g_ao.blur || !g_ao.comp) {
        hle_log("hakuX-hle-back", "ambient occlusion: pipeline failed (not applied)");
        return false;
    }
    g_ao.failed = false;
    g_ao.ready = true;
    hle_log("hakuX-hle-back", "ambient occlusion: passes ready (patch 363)");
    return true;
}

/* Only when nothing uses them (the GPU idle: hle_ssao_regrow's caller) */
static void ao_free(void)
{
    for (unsigned i = 0; i < 2; i++) {
        if (g_ao.view[i]) {
            T.vkDestroyImageView(g_hvk.dev, g_ao.view[i], NULL);
        }
        if (g_ao.img[i]) {
            T.vkDestroyImage(g_hvk.dev, g_ao.img[i], NULL);
        }
        if (g_ao.mem[i]) {
            T.vkFreeMemory(g_hvk.dev, g_ao.mem[i], NULL);
        }
        g_ao.view[i] = VK_NULL_HANDLE;
        g_ao.img[i] = VK_NULL_HANDLE;
        g_ao.mem[i] = VK_NULL_HANDLE;
    }
    g_ao.w = g_ao.h = 0;
}

static bool ao_make(uint32_t w, uint32_t h)
{
    static const VkFormat f[2] = { VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8_UNORM };
    for (unsigned i = 0; i < 2; i++) {
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = f[i], .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &g_ao.img[i]) != VK_SUCCESS) {
            g_ao.img[i] = VK_NULL_HANDLE;
            ao_free();
            return false;
        }
        VkMemoryRequirements req;
        T.vkGetImageMemoryRequirements(g_hvk.dev, g_ao.img[i], &req);
        int mt = hle_vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
        if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_ao.mem[i]) != VK_SUCCESS) {
            g_ao.mem[i] = VK_NULL_HANDLE;
            ao_free();
            return false;
        }
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_ao.img[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f[i],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (T.vkBindImageMemory(g_hvk.dev, g_ao.img[i], g_ao.mem[i], 0) != VK_SUCCESS ||
            T.vkCreateImageView(g_hvk.dev, &vci, NULL, &g_ao.view[i]) != VK_SUCCESS) {
            g_ao.view[i] = VK_NULL_HANDLE;
            ao_free();
            return false;
        }
    }
    g_ao.w = w;
    g_ao.h = h;
    hle_log("hakuX-hle-back", "ambient occlusion: %ux%u working images (patch 363)", w, h);
    return true;
}

bool hle_ssao_regrow(uint32_t dw, uint32_t dh)
{
    const uint32_t w = (dw + 1) / 2, h = (dh + 1) / 2;
    return g_ao.img[0] && (w > g_ao.w || h > g_ao.h);
}

static void ao_draw(VkCommandBuffer cb, VkPipeline p, VkDescriptorSet set, VkImage img,
                    VkImageView view, uint32_t w, uint32_t h, bool load, const float pc[8])
{
    hle_img_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT,
                    load ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = { { 0, 0 }, { w, h } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &ca };
    T.vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    T.vkCmdSetViewport(cb, 0, 1, &vp);
    T.vkCmdSetScissor(cb, 0, 1, &sc);
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ao.pl, 0, 1, &set, 0, NULL);
    T.vkCmdPushConstants(cb, g_ao.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    T.vkCmdDraw(cb, 3, 1, 0, 0);
    T.vkCmdEndRendering(cb);
    hle_img_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool hle_ssao_pass(VkCommandBuffer cb, unsigned slot, int level, HleVkSurf *ds, uint32_t gw,
                   uint32_t gh, const float map[4], VkImage dst, uint32_t dw, uint32_t dh)
{
    if (level <= 0 || !ds || !gw || !gh || !dw || !dh || !ao_init()) {
        return false;
    }
    VkImageView zv = hle_zread_depth_view(ds);
    VkImageView dv = dst_view(dst);
    if (!zv || !dv) {
        return false;
    }
    const uint32_t w = (ds->w + 1) / 2, h = (ds->h + 1) / 2;
    if (w > g_ao.w || h > g_ao.h) {     /* none yet, or the caller waited for the GPU (hle_ssao_regrow) */
        const uint32_t nw = MAX(w, g_ao.w), nh = MAX(h, g_ao.h);
        ao_free();
        if (!ao_make(nw, nh)) {
            return false;
        }
    }
    slot %= GM_SLOTS;                   /* this frame slot's earlier use has completed */
    hle_surf_transition(cb, ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii[4] = {
        { g_ao.near, zv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },     /* AO: the depth */
        { g_ao.near, g_ao.view[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },  /* blur: the AO */
        { g_gm.smp, g_ao.view[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },   /* the blurred one */
        { g_ao.near, zv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },     /* binding 1: depth */
    };
    VkWriteDescriptorSet wr[6];
    for (unsigned i = 0; i < 3; i++) {
        wr[2 * i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_ao.set[slot][i],
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i] };
        wr[2 * i + 1] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_ao.set[slot][i],
            .dstBinding = 1, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[3] };
    }
    T.vkUpdateDescriptorSets(g_hvk.dev, 6, wr, 0, NULL);
    hle_draw_state_lost();
    /* Low: lighter and tighter; High: darker, wider */
    const float strength = level >= 2 ? 0.65f : 0.4f, radius = level >= 2 ? 14.0f : 10.0f;
    /* the depth's smallest step (D16: its noise far away kept out) */
    const float step = ds->fmt == VK_FORMAT_D16_UNORM ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
    const float pa[8] = { strength, radius, 1.0f / (float)gw, 1.0f / (float)gh,
                          1.0f / (float)w, 1.0f / (float)h, 0.9f * (float)gh, step };
    ao_draw(cb, g_ao.ao, g_ao.set[slot][0], g_ao.img[0], g_ao.view[0], w, h, false, pa);
    const float pb[8] = { (float)(w - 1), (float)(h - 1), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    ao_draw(cb, g_ao.blur, g_ao.set[slot][1], g_ao.img[1], g_ao.view[1], w, h, false, pb);
    const float pc[8] = { map[0], map[1], map[2], map[3], (float)w, (float)h,
                          1.0f / (float)g_ao.w, 1.0f / (float)g_ao.h };
    ao_draw(cb, g_ao.comp, g_ao.set[slot][2], dst, dv, dw, dh, true, pc);
    return true;
}
