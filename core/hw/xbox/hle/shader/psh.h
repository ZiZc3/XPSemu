/*
 * hakuX HLE GPU — Xbox pixel shaders (NV2A register combiners and texture
 * modes) -> GLSL 4.50 (spec §5.8; pure). The key is the pixel-shader
 * render-state block (5659 render states 0..56, which SetPixelShader loads
 * from the D3DPIXELSHADERDEF); the combiner constants are push constants.
 *
 * Interface: inputs location 0 vDiffuse, 1 vSpecular, 2..5 vTex0..3, 6 vFog
 * (as vsh.h / ff.h); set 0 bindless images and samplers; HleFfPush.
 */
#ifndef HW_XBOX_HLE_SHADER_PSH_H
#define HW_XBOX_HLE_SHADER_PSH_H

#include <stddef.h>
#include <stdint.h>

/* D3DPIXELSHADERDEF / render-state indices (5659: D3DRS_PSALPHAINPUTS0 = 0) */
enum {
    HLE_PS_ALPHA_IN = 0,                /* [8] */
    HLE_PS_FINAL_ABCD = 8,
    HLE_PS_FINAL_EFG = 9,
    HLE_PS_C0 = 10,                     /* [8] */
    HLE_PS_C1 = 18,                     /* [8] */
    HLE_PS_ALPHA_OUT = 26,              /* [8] */
    HLE_PS_RGB_IN = 34,                 /* [8] */
    HLE_PS_COMPARE = 42,
    HLE_PS_FINAL_C0 = 43,
    HLE_PS_FINAL_C1 = 44,
    HLE_PS_RGB_OUT = 45,                /* [8] */
    HLE_PS_COUNT = 53,
    HLE_PS_TEXMODES = 54,
    HLE_PS_DOTMAP = 55,
    HLE_PS_INPUTTEX = 56,
    HLE_PS_REGS = 57,
};

/* PSTextureModes values */
enum {
    HLE_PST_NONE = 0, HLE_PST_PROJECT2D, HLE_PST_PROJECT3D, HLE_PST_CUBEMAP,
    HLE_PST_PASSTHRU, HLE_PST_CLIPPLANE, HLE_PST_BUMPENVMAP, HLE_PST_BUMPENVMAP_LUM,
    HLE_PST_BRDF, HLE_PST_DOT_ST, HLE_PST_DOT_ZW, HLE_PST_DOT_RFLCT_DIFF,
    HLE_PST_DOT_RFLCT_SPEC, HLE_PST_DOT_STR_3D, HLE_PST_DOT_STR_CUBE, HLE_PST_DPNDNT_AR,
    HLE_PST_DPNDNT_GB, HLE_PST_DOTPRODUCT, HLE_PST_DOT_RFLCT_SPEC_CONST,
};

typedef struct HlePshKey {
    uint32_t rgb_in[8], rgb_out[8], alpha_in[8], alpha_out[8];
    uint32_t final_abcd, final_efg;
    uint32_t count;                     /* PSCombinerCount */
    uint32_t tex_modes;                 /* PSTextureModes */
    /* patch 334: PSDotMapping (12 bits) and PSCompareMode (16 bits) were 32-bit
     * words whose top halves stayed 0 (hle_psh_key masks them): those halves
     * hold the colour sign, so the key's size and every unsigned draw's key
     * bytes are as before (the pipeline record). Per stage 4 bits (bit 0 r ..
     * 3 a): sgn_ob the channels uploaded offset binary (tex/xdk_res.h), mapped
     * back exactly; sgn_raw those read as stored (targets, formats not
     * flipped), two's complement after filtering. */
    uint16_t dot_map;                   /* PSDotMapping */
    uint16_t sgn_ob;
    uint32_t input_tex;                 /* PSInputTexture */
    uint16_t compare;                   /* PSCompareMode */
    uint16_t sgn_raw;
    uint8_t alpha_test;                 /* ALPHATESTENABLE */
    uint8_t alpha_func;                 /* D3DCMP_* - 0x200 (0..7); bit 3 (patch 354): MSAA
                                           alpha-to-coverage (HLE_A2C_BIT) */
    uint8_t flat;
    uint8_t fog;                        /* 0 off; FOGTABLEMODE + 1 (NONE 1 .. LINEAR 4) */
    uint8_t fog_abs;                    /* the factor of |distance|: an NV2A _ABS fog mode
                                           the title pushed (D3D's are never _ABS) */
    uint8_t debug;                      /* output instead: 1 t0, 2 v0, 3 r0, 4 fog, 5 v1,
                                           6 t3, 7 r0.a, 8 t2, 9 shadow bands, 10 v1.a,
                                           11 t0.a, 12 t3.a, 13 fract(T3 s/q, t/q),
                                           14 |T3 str| / 4, 15 t3 and 16 t0 as sampled,
                                           17 shadow-buffer draws: (stored depth, r,
                                           result) / range, unblended; 18 magenta,
                                           19 t1, 20 r1, 21 |T2 str| / 4, 22 T2 str
                                           direction, 23 T2 NaN / Inf, 24 stage 2's
                                           cube along (0.3, 0.5, 0.8) */
    uint8_t point_sprite;               /* POINTSPRITEENABLE on a point list: texture
                                           unit 3's coordinates are the point's */
    /* Shadow buffers: stages whose texture is a depth surface (bits 0-3),
     * D3DRS_SHADOWFUNC - 0x200 (bits 4-6); per stage (2 bits) the depth
     * range the r coordinate is in: 1 D24 (2^24 - 1), 2 D16, 3 float; 0
     * (patch 335) an uploaded F24 texture, its float as it is. */
    uint8_t shadow;
    uint8_t shadow_fmt;
    uint8_t out_bgra;                   /* 8-bit (B8) target: it stores blue */
    uint8_t alpha_kill;                 /* stages with D3DTSS_ALPHAKILL (bits 0-3) */
    uint8_t vol;                        /* stages whose texture is a volume (bits 0-3):
                                           2D-coordinate modes fetch it in 3D, as the NV2A
                                           (LLE): PROJECT2D at r = 0, bump-env at the
                                           coordinates' r; HLE_PSH_RECT: the stage the
                                           push block's fog.w names has its linear
                                           texture kept to its own size, part of a
                                           larger surface (ff.h HLE_NVRECT_GLSL); bits
                                           5-7 free */
    uint8_t wbuf;                       /* W-buffering: depth = vW (HLE_WBUF_*_GLSL) */
    uint8_t fbfetch;                    /* stages (bits 0-3) whose texture is the colour
                                           target drawn into: its pixel, read as the
                                           input attachment (set 2) */
    uint8_t specular;                   /* SPECULARENABLE: D3D's own final combiner adds
                                           v1 (a shader without one) */
    uint8_t sblend;                     /* the NV2A's signed blend equations (ff.h
                                           HleFfPsKey.sblend) */
} HlePshKey;
#define HLE_PSH_RECT 0x10u              /* HlePshKey.vol: a linear texture kept to its size */

/* Bits PSInputTexture and PSTextureModes never have (hle_psh_key masks them:
 * 0 in every key that uses none of these, its size and bytes as before),
 * set by vk/draw.c for program draws only; their values in the push block's
 * fixed-function matrix (a program's vertex shader never reads it; column 0
 * is xogfix=rect's):
 * - patch 340, texture colour key: input_tex bits 2s..2s+1 stage s's
 *   COLORKEYOP (1 alpha 0, 2 colour and alpha 0, 3 discard), bit 8 + s its
 *   alpha ignored (an X format); the key colours (ARGB) as 16-bit halves,
 *   pc.mvp[1][s] the low, pc.mvp[2][s] the high (exact as floats);
 * - patch 342, convolution: input_tex bit 12 + s, stage s's PROJECT2D texel
 *   the 3x3 gaussian of its neighbours (xemu's, quincunx too);
 * - patch 338, depth clip range: tex_modes bit 20 the fragment depth kept to
 *   [pc.mvp[3].x, pc.mvp[3].y] (normalised), bit 21 by discarding (ZCLAMP_EN
 *   CULL), else by clamping. */
#define HLE_PSH_CKEY_OP(s)   (3u << (2 * (s)))
#define HLE_PSH_CKEY_X(s)    (0x100u << (s))
#define HLE_PSH_CONV(s)      (0x1000u << (s))
#define HLE_PSH_ZCLIP        (1u << 20)
#define HLE_PSH_ZCLIP_CULL   (1u << 21)

/* Key from the render-state array (rs[0..56] = the pixel-shader block). */
void hle_psh_key(const uint32_t *rs, HlePshKey *k);

/* GLSL into buf; returns its length, 0 when it does not fit. Unsupported
 * texture modes sample as PROJECT2D and set *unsupported (may be NULL). */
size_t hle_psh_glsl(const HlePshKey *k, char *buf, size_t cap, unsigned *unsupported);

#endif
