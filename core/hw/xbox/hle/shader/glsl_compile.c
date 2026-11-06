/*
 * hakuX HLE GPU — GLSL -> SPIR-V via glslang C API.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/shader/glsl_compile.h"
#include "hw/xbox/hle/shader/ff.h"

#include <glslang/Include/glslang_c_interface.h>
#ifdef __ANDROID__
#include <android/log.h>
#define GLSL_LOG(...) __android_log_print(ANDROID_LOG_ERROR, "hakuX-hle-glsl", __VA_ARGS__)
#else
#define GLSL_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

bool g_hle_glsl_vk11;

/* Resource limits: data copied verbatim from hw/xbox/nv2a/pgraph/vk/glsl.c
 * (resource_limits) so HLE does not depend on the LLE renderer. */
static const glslang_resource_t k_hle_glsl_limits = {
    .max_lights = 32,
    .max_clip_planes = 6,
    .max_texture_units = 32,
    .max_texture_coords = 32,
    .max_vertex_attribs = 64,
    .max_vertex_uniform_components = 4096,
    .max_varying_floats = 64,
    .max_vertex_texture_image_units = 32,
    .max_combined_texture_image_units = 80,
    .max_texture_image_units = 32,
    .max_fragment_uniform_components = 4096,
    .max_draw_buffers = 32,
    .max_vertex_uniform_vectors = 128,
    .max_varying_vectors = 8,
    .max_fragment_uniform_vectors = 16,
    .max_vertex_output_vectors = 16,
    .max_fragment_input_vectors = 15,
    .min_program_texel_offset = -8,
    .max_program_texel_offset = 7,
    .max_clip_distances = 8,
    .max_compute_work_group_count_x = 65535,
    .max_compute_work_group_count_y = 65535,
    .max_compute_work_group_count_z = 65535,
    .max_compute_work_group_size_x = 1024,
    .max_compute_work_group_size_y = 1024,
    .max_compute_work_group_size_z = 64,
    .max_compute_uniform_components = 1024,
    .max_compute_texture_image_units = 16,
    .max_compute_image_uniforms = 8,
    .max_compute_atomic_counters = 8,
    .max_compute_atomic_counter_buffers = 1,
    .max_varying_components = 60,
    .max_vertex_output_components = 64,
    .max_geometry_input_components = 64,
    .max_geometry_output_components = 128,
    .max_fragment_input_components = 128,
    .max_image_units = 8,
    .max_combined_image_units_and_fragment_outputs = 8,
    .max_combined_shader_output_resources = 8,
    .max_image_samples = 0,
    .max_vertex_image_uniforms = 0,
    .max_tess_control_image_uniforms = 0,
    .max_tess_evaluation_image_uniforms = 0,
    .max_geometry_image_uniforms = 0,
    .max_fragment_image_uniforms = 8,
    .max_combined_image_uniforms = 8,
    .max_geometry_texture_image_units = 16,
    .max_geometry_output_vertices = 256,
    .max_geometry_total_output_components = 1024,
    .max_geometry_uniform_components = 1024,
    .max_geometry_varying_components = 64,
    .max_tess_control_input_components = 128,
    .max_tess_control_output_components = 128,
    .max_tess_control_texture_image_units = 16,
    .max_tess_control_uniform_components = 1024,
    .max_tess_control_total_output_components = 4096,
    .max_tess_evaluation_input_components = 128,
    .max_tess_evaluation_output_components = 128,
    .max_tess_evaluation_texture_image_units = 16,
    .max_tess_evaluation_uniform_components = 1024,
    .max_tess_patch_components = 120,
    .max_patch_vertices = 32,
    .max_tess_gen_level = 64,
    .max_viewports = 16,
    .max_vertex_atomic_counters = 0,
    .max_tess_control_atomic_counters = 0,
    .max_tess_evaluation_atomic_counters = 0,
    .max_geometry_atomic_counters = 0,
    .max_fragment_atomic_counters = 8,
    .max_combined_atomic_counters = 8,
    .max_atomic_counter_bindings = 1,
    .max_vertex_atomic_counter_buffers = 0,
    .max_tess_control_atomic_counter_buffers = 0,
    .max_tess_evaluation_atomic_counter_buffers = 0,
    .max_geometry_atomic_counter_buffers = 0,
    .max_fragment_atomic_counter_buffers = 1,
    .max_combined_atomic_counter_buffers = 1,
    .max_atomic_counter_buffer_size = 16384,
    .max_transform_feedback_buffers = 4,
    .max_transform_feedback_interleaved_components = 64,
    .max_cull_distances = 8,
    .max_combined_clip_and_cull_distances = 8,
    .max_samples = 4,
    .max_mesh_output_vertices_nv = 256,
    .max_mesh_output_primitives_nv = 512,
    .max_mesh_work_group_size_x_nv = 32,
    .max_mesh_work_group_size_y_nv = 1,
    .max_mesh_work_group_size_z_nv = 1,
    .max_task_work_group_size_x_nv = 32,
    .max_task_work_group_size_y_nv = 1,
    .max_task_work_group_size_z_nv = 1,
    .max_mesh_view_count_nv = 4,
    .maxDualSourceDrawBuffersEXT = 1,
    .limits = {
        .non_inductive_for_loops = 1,
        .while_loops = 1,
        .do_while_loops = 1,
        .general_uniform_indexing = 1,
        .general_attribute_matrix_vector_indexing = 1,
        .general_varying_indexing = 1,
        .general_sampler_indexing = 1,
        .general_variable_indexing = 1,
        .general_constant_matrix_vector_indexing = 1,
    } };

void hle_glsl_init(void)
{
    glslang_initialize_process();
}

void hle_glsl_finalize(void)
{
    glslang_finalize_process();
}

static void log_failure(const char *phase, const char *info,
                        const char *debug, const char *src)
{
    GLSL_LOG("GLSL %s failed", phase);
    if (info && info[0]) {
        GLSL_LOG("[INFO] %s", info);
    }
    if (debug && debug[0]) {
        GLSL_LOG("[DEBUG] %s", debug);
    }
    if (src) {
        GLSL_LOG("[SOURCE]\n%s", src);
    }
}

/* Input attachment index HLE_IA_DEPTH (shader/ff.h) out of the module: a
 * subpassInput without one reads the depth attachment (local read's default
 * mapping), which GLSL cannot declare. */
static size_t strip_ia_depth(uint32_t *w, size_t n)
{
    size_t o = 5;
    for (size_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16;
        if (!len || i + len > n) {
            return n;                   /* malformed: as is */
        }
        bool drop = (w[i] & 0xFFFF) == 71 /* OpDecorate */ && len == 4 &&
                    w[i + 2] == 43 /* InputAttachmentIndex */ && w[i + 3] == HLE_IA_DEPTH;
        if (!drop) {
            memmove(&w[o], &w[i], len * sizeof(*w));
            o += len;
        }
        i += len;
    }
    return o;
}

uint32_t *hle_glsl_compile(HleShaderStage stage, const char *src,
                           size_t *nwords)
{
    glslang_stage_t st = stage == HLE_SHADER_VERTEX ? GLSLANG_STAGE_VERTEX :
                         stage == HLE_SHADER_FRAGMENT ? GLSLANG_STAGE_FRAGMENT :
                         GLSLANG_STAGE_COMPUTE;
    const glslang_input_t input = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = st,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = g_hle_glsl_vk11 ? GLSLANG_TARGET_VULKAN_1_1 : GLSLANG_TARGET_VULKAN_1_3,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = g_hle_glsl_vk11 ? GLSLANG_TARGET_SPV_1_3
                                                   : GLSLANG_TARGET_SPV_1_6,
        .code = src,
        .default_version = 450,
        .default_profile = GLSLANG_NO_PROFILE,
        .force_default_version_and_profile = false,
        .forward_compatible = false,
        .messages = GLSLANG_MSG_DEFAULT_BIT,
        .resource = &k_hle_glsl_limits,
    };
    *nwords = 0;
    glslang_shader_t *sh = glslang_shader_create(&input);
    if (!glslang_shader_preprocess(sh, &input)) {
        log_failure("preprocess", glslang_shader_get_info_log(sh),
                    glslang_shader_get_info_debug_log(sh), src);
        glslang_shader_delete(sh);
        return NULL;
    }
    if (!glslang_shader_parse(sh, &input)) {
        log_failure("parse", glslang_shader_get_info_log(sh),
                    glslang_shader_get_info_debug_log(sh), src);
        glslang_shader_delete(sh);
        return NULL;
    }
    glslang_program_t *pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT |
                                  GLSLANG_MSG_VULKAN_RULES_BIT)) {
        log_failure("link", glslang_program_get_info_log(pr),
                    glslang_program_get_info_debug_log(pr), NULL);
        glslang_program_delete(pr);
        glslang_shader_delete(sh);
        return NULL;
    }
    glslang_spv_options_t opts = { .validate = true };
    glslang_program_SPIRV_generate_with_options(pr, st, &opts);
    const char *msgs = glslang_program_SPIRV_get_messages(pr);
    if (msgs && msgs[0]) {
        GLSL_LOG("[SPIRV] %s", msgs);
    }
    size_t n = glslang_program_SPIRV_get_size(pr);
    uint32_t *words = g_new(uint32_t, n);
    glslang_program_SPIRV_get(pr, words);
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
    if (stage == HLE_SHADER_FRAGMENT && strstr(src, "uniform subpassInput uZ;")) {
        n = strip_ia_depth(words, n);
    }
    *nwords = n;
    return words;
}

