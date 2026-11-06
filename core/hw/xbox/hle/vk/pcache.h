/*
 * hakuX HLE GPU — on-disk shader and pipeline caches (P3 Task 13): SPIR-V
 * by the generated GLSL, and the Vulkan pipeline cache, under the xemu base
 * path. A later boot skips glslang and most of the driver's compile.
 */
#ifndef HW_XBOX_HLE_VK_PCACHE_H
#define HW_XBOX_HLE_VK_PCACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <volk.h>
#include "hw/xbox/hle/shader/glsl_compile.h"

/* The pipeline cache, loaded from disk (empty if none or rejected). */
void hle_pcache_init(void);
VkPipelineCache hle_pcache_get(void);
/* A pipeline the driver compiled (not one its cache held): the cache has
 * something new to write. Any thread. */
void hle_pcache_compiled(void);
/* Writes the pipeline cache when pipelines were compiled since the last
 * write, at most once a minute unless forced. Back-end thread. */
void hle_pcache_save(bool force);
void hle_pcache_destroy(void);

/* SPIR-V for src (g_malloc'd, *nwords): from the disk cache, else compiled
 * with glslang and stored. NULL if it does not compile. */
uint32_t *hle_spirv_get(HleShaderStage stage, const char *src, size_t *nwords);
/* The same from the disk cache only: NULL when glslang would compile. */
uint32_t *hle_spirv_peek(HleShaderStage stage, const char *src, size_t *nwords);

#endif
