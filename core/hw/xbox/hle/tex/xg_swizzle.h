/*
 * hakuX HLE GPU — NV2A swizzle for the XG helpers (spec §4.9).
 * Rect/box NULL = the whole texture; (px,py[,pz]) = destination point
 * (swizzle) or destination point in the linear image (unswizzle).
 */
#ifndef HW_XBOX_HLE_TEX_XG_SWIZZLE_H
#define HW_XBOX_HLE_TEX_XG_SWIZZLE_H

#include "hw/xbox/hle/hle_pure.h"

typedef struct HleXgRect {
    uint32_t x, y, w, h;
} HleXgRect;

typedef struct HleXgBox {
    uint32_t x, y, z, w, h, d;
} HleXgBox;

/* Texel index of (x, y, z) in a swizzled w x h x d texture. */
uint32_t hle_xg_swizzle_offset(uint32_t w, uint32_t h, uint32_t d,
                               uint32_t x, uint32_t y, uint32_t z);

/* Linear src (pitch) rect -> swizzled dst (w x h) at (px, py). */
void hle_xg_swizzle_rect(const uint8_t *src, uint32_t pitch, const HleXgRect *r,
                         uint8_t *dst, uint32_t w, uint32_t h,
                         uint32_t px, uint32_t py, uint32_t bpp);
/* Swizzled src (w x h) rect -> linear dst (pitch) at (px, py). */
void hle_xg_unswizzle_rect(const uint8_t *src, uint32_t w, uint32_t h,
                           const HleXgRect *r, uint8_t *dst, uint32_t pitch,
                           uint32_t px, uint32_t py, uint32_t bpp);
/* The same for P8 (1 byte a texel), each index looked up in pal: dst holds
 * 4-byte texels. */
void hle_xg_unswizzle_rect_p8(const uint8_t *src, uint32_t w, uint32_t h,
                              const HleXgRect *r, const uint32_t *pal, uint8_t *dst,
                              uint32_t pitch, uint32_t px, uint32_t py);
void hle_xg_swizzle_box(const uint8_t *src, uint32_t row_pitch, uint32_t slice_pitch,
                        const HleXgBox *b, uint8_t *dst, uint32_t w, uint32_t h,
                        uint32_t d, uint32_t px, uint32_t py, uint32_t pz,
                        uint32_t bpp);
void hle_xg_unswizzle_box(const uint8_t *src, uint32_t w, uint32_t h, uint32_t d,
                          const HleXgBox *b, uint8_t *dst, uint32_t row_pitch,
                          uint32_t slice_pitch, uint32_t px, uint32_t py,
                          uint32_t pz, uint32_t bpp);
/* Per-texel reference (tests). */
void hle_xg_swizzle_rect_ref(const uint8_t *src, uint32_t pitch, const HleXgRect *r,
                             uint8_t *dst, uint32_t w, uint32_t h,
                             uint32_t px, uint32_t py, uint32_t bpp);

#endif
