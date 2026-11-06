/*
 * hakuX HLE GPU — NV2A vertex program (XDK xvs microcode) -> GLSL 4.50
 * (spec §5.8; pure). Decoding uses subprojects/nv2a_vsh_cpu; the GLSL is
 * HLE's own (independent of the LLE render path).
 *
 * Interface (shared with vk/draw.c and the pixel generators):
 * - inputs: layout(location = N) in vN_in for each bit of in_mask (vec4;
 *   ivec4 for int_mask: S32K shorts; uvec2 for cmp_mask: the two 16-bit
 *   halves of a packed 11:11:10 dword, unpacked here); unbound registers
 *   read their current values vcur[N] (SetVertexData*);
 * - constants: set 1, binding 0, std140 { vec4 c[192]; vec4 vcur[16]; };
 * - push constants: HleFfPush (vp = screen -> NDC mapping, zscale);
 * - outputs: location 0 oD0, 1 oD1, 2..5 oT0..oT3, 6 fog (x), 7 oB0, 8 oB1.
 */
#ifndef HW_XBOX_HLE_SHADER_VSH_H
#define HW_XBOX_HLE_SHADER_VSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HLE_VSH_MAX_SLOTS 136
#define HLE_VSH_CONSTANTS 192
#define HLE_VSH_CUR_REGS  16    /* current attribute values after the constants */

typedef struct HleVshKey {
    uint32_t in_mask;           /* vertex attribute registers bound (bit per vN) */
    uint16_t int_mask;          /* of those, fetched as signed integers */
    uint16_t cmp_mask;          /* of those, packed 11:11:10 dwords */
    uint8_t flat;               /* SHADEMODE FLAT */
    uint8_t spec_off;           /* specular off: oD1 / oB1 read as (0, 0, 0, 1) */
    uint8_t wbuf;               /* W-buffering: oPos.w to the pixel stage (vW) */
    uint8_t zfree;              /* no depth test: the program's z clips nothing */
    uint8_t fog_table;          /* FOGTABLEMODE other than NONE (a program writing
                                   no oFog: distance 0, else factor 1) */
    uint8_t zclamp;             /* depth clamped, not clipped (the pipeline's: in the
                                   padding, the key's size is the pipeline record's) */
} HleVshKey;

/* Parses nslots 4-dword instructions (stopping at the FINAL one) and writes
 * the GLSL into buf. Returns the length, or 0 when a slot does not decode,
 * no instruction is FINAL, or the text does not fit. */
size_t hle_vsh_glsl(const uint32_t *tokens, unsigned nslots, const HleVshKey *k,
                    char *buf, size_t cap);
/* The same shader around body, GLSL statements in main that set the
 * registers (v0..v15 read, R0..R11 scratch; oPos in screen space as a
 * program leaves it, oD0, oD1, oB0, oB1, oT0..oT3, oFog, oPts); fogw: body
 * writes oFog. Returns the length, 0 when it does not fit. */
size_t hle_vsh_glsl_wrap(const HleVshKey *k, const char *body, bool fogw, char *buf, size_t cap);

#endif
