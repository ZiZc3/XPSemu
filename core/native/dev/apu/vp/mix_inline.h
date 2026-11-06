/*
 * Inline voice mixer helpers for MCPX APU voice processor.
 *
 * Vectorizes the per-voice multiply-add into mixbins (vp.c::voice_process inner
 * loop) using NEON FMA on aarch64; falls back to a scalar reference on other
 * targets. The two paths produce sub-LSB-different results because vfmaq_f32
 * is a single-rounding fused multiply-add while the scalar path multiplies
 * then adds (two roundings). This precision difference is far below human
 * audible thresholds for the gain values used here (typically 0..2).
 *
 * The header is self-contained: it defines NUM_SAMPLES_PER_FRAME locally if
 * not already defined by the surrounding build (apu_regs.h provides it in the
 * QEMU build; tests provide nothing, so the local definition takes effect).
 *
 * See: docs/superpowers/specs/2026-04-28-apu-mixer-neon-design.md
 */
#ifndef HW_XBOX_MCPX_APU_VP_MIX_INLINE_H
#define HW_XBOX_MCPX_APU_VP_MIX_INLINE_H

#ifndef NUM_SAMPLES_PER_FRAME
#define NUM_SAMPLES_PER_FRAME 32
#endif

/* NEON loop steps by 4; this static_assert prevents past-end writes if a
 * future change to apu_regs.h desyncs NUM_SAMPLES_PER_FRAME. */
_Static_assert(NUM_SAMPLES_PER_FRAME % 4 == 0,
               "NUM_SAMPLES_PER_FRAME must be a multiple of 4 for NEON loop");

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/* Reference implementation, two-rounding semantics. Used as both the
 * fallback for non-aarch64 builds and the host unit test's ground truth. */
static inline void mix_one_bin_scalar(float *dst, const float *src, float g) {
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        dst[i] += g * src[i];
    }
}

/* Hot-path dispatcher: NEON FMA on aarch64, scalar everywhere else. */
static inline void mix_one_bin(float *dst, const float *src, float g) {
#if defined(__aarch64__)
    float32x4_t gv = vdupq_n_f32(g);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i += 4) {
        float32x4_t d = vld1q_f32(dst + i);
        float32x4_t s = vld1q_f32(src + i);
        d = vfmaq_f32(d, gv, s);
        vst1q_f32(dst + i, d);
    }
#else
    mix_one_bin_scalar(dst, src, g);
#endif
}

#endif /* HW_XBOX_MCPX_APU_VP_MIX_INLINE_H */
