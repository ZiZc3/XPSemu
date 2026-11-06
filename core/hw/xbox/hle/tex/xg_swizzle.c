/*
 * hakuX HLE GPU — NV2A swizzle (own implementation of the standard
 * interleaved-bit layout; scalar mask-increment loop + 4x4 blocks, NEON for
 * 1, 2 and 4 bytes a texel).
 */
#include "hw/xbox/hle/tex/xg_swizzle.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

static void masks(uint32_t w, uint32_t h, uint32_t d,
                  uint32_t *mx, uint32_t *my, uint32_t *mz)
{
    uint32_t x = 0, y = 0, z = 0, bit = 1, m = 1;
    bool more;
    do {
        more = false;
        if (bit < w) { x |= m; m <<= 1; more = true; }
        if (bit < h) { y |= m; m <<= 1; more = true; }
        if (bit < d) { z |= m; m <<= 1; more = true; }
        bit <<= 1;
    } while (more && bit);  /* bit == 0: a dimension > 2^31 would never end */
    *mx = x;
    *my = y;
    *mz = z;
}

static uint32_t deposit(uint32_t mask, uint32_t v)
{
    uint32_t r = 0;
    while (mask && v) {
        uint32_t low = mask & (0u - mask);
        if (v & 1) {
            r |= low;
        }
        v >>= 1;
        mask &= mask - 1;
    }
    return r;
}

static inline uint32_t next_in(uint32_t s, uint32_t mask)
{
    return ((s | ~mask) + 1) & mask;
}

static inline void copy_texel(uint8_t *d, const uint8_t *s, uint32_t bpp)
{
    switch (bpp) {
    case 4: memcpy(d, s, 4); break;
    case 2: memcpy(d, s, 2); break;
    case 1: *d = *s; break;
    default: memcpy(d, s, bpp); break;
    }
}

uint32_t hle_xg_swizzle_offset(uint32_t w, uint32_t h, uint32_t d,
                               uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t mx, my, mz;
    masks(w, h, d, &mx, &my, &mz);
    return deposit(mx, x) | deposit(my, y) | deposit(mz, z);
}

void hle_xg_swizzle_rect_ref(const uint8_t *src, uint32_t pitch, const HleXgRect *r,
                             uint8_t *dst, uint32_t w, uint32_t h,
                             uint32_t px, uint32_t py, uint32_t bpp)
{
    HleXgRect all = { 0, 0, w, h };
    const HleXgRect *rr = r ? r : &all;
    for (uint32_t j = 0; j < rr->h; j++) {
        for (uint32_t i = 0; i < rr->w; i++) {
            uint32_t o = hle_xg_swizzle_offset(w, h, 1, px + i, py + j, 0);
            copy_texel(dst + (size_t)o * bpp,
                       src + (size_t)(rr->y + j) * pitch + (size_t)(rr->x + i) * bpp, bpp);
        }
    }
}

#if defined(__aarch64__)
/*
 * Byte-pointer loads/stores: guest pitches and base addresses need not be
 * 4/8-byte aligned, and LD1/ST1 of bytes carry no alignment assumption.
 */
static inline uint64x2_t ld_q(const uint8_t *p)
{
    return vreinterpretq_u64_u8(vld1q_u8(p));
}

static inline void st_q(uint8_t *p, uint64x2_t v)
{
    vst1q_u8(p, vreinterpretq_u8_u64(v));
}

/* 4 bytes/texel, w,h >= 4, rect and point 4-aligned. */
static void swz_neon(const uint8_t *src, uint32_t pitch, const HleXgRect *r,
                     uint8_t *dst, uint32_t mx, uint32_t my, uint32_t px, uint32_t py)
{
    uint32_t mxh = mx & ~0x5u, myh = my & ~0xAu;
    uint32_t sy = deposit(my, py);
    for (uint32_t j = 0; j < r->h; j += 4) {
        const uint8_t *r0 = src + (size_t)(r->y + j) * pitch + (size_t)r->x * 4;
        const uint8_t *r1 = r0 + pitch;
        const uint8_t *r2 = r1 + pitch;
        const uint8_t *r3 = r2 + pitch;
        uint32_t sx = deposit(mx, px);
        for (uint32_t i = 0; i < r->w; i += 4) {
            uint64x2_t a = ld_q(r0 + (size_t)i * 4);
            uint64x2_t b = ld_q(r1 + (size_t)i * 4);
            uint64x2_t c = ld_q(r2 + (size_t)i * 4);
            uint64x2_t d = ld_q(r3 + (size_t)i * 4);
            uint8_t *o = dst + (size_t)(sx | sy) * 4;
            st_q(o, vzip1q_u64(a, b));
            st_q(o + 16, vzip2q_u64(a, b));
            st_q(o + 32, vzip1q_u64(c, d));
            st_q(o + 48, vzip2q_u64(c, d));
            sx = next_in(sx, mxh);
        }
        sy = next_in(sy, myh);
    }
}

static void unswz_neon(const uint8_t *src, const HleXgRect *r, uint8_t *dst,
                       uint32_t pitch, uint32_t mx, uint32_t my,
                       uint32_t px, uint32_t py)
{
    uint32_t mxh = mx & ~0x5u, myh = my & ~0xAu;
    uint32_t sy = deposit(my, r->y);
    for (uint32_t j = 0; j < r->h; j += 4) {
        uint8_t *d0 = dst + (size_t)(py + j) * pitch + (size_t)px * 4;
        uint8_t *d1 = d0 + pitch;
        uint8_t *d2 = d1 + pitch;
        uint8_t *d3 = d2 + pitch;
        uint32_t sx = deposit(mx, r->x);
        for (uint32_t i = 0; i < r->w; i += 4) {
            const uint8_t *s = src + (size_t)(sx | sy) * 4;
            uint64x2_t q0 = ld_q(s), q1 = ld_q(s + 16);
            uint64x2_t q2 = ld_q(s + 32), q3 = ld_q(s + 48);
            st_q(d0 + (size_t)i * 4, vzip1q_u64(q0, q1));
            st_q(d1 + (size_t)i * 4, vzip2q_u64(q0, q1));
            st_q(d2 + (size_t)i * 4, vzip1q_u64(q2, q3));
            st_q(d3 + (size_t)i * 4, vzip2q_u64(q2, q3));
            sx = next_in(sx, mxh);
        }
        sy = next_in(sy, myh);
    }
}

/* 2 bytes/texel: a block's words (2 texels each) are rows 0, 1, 0, 1, then
 * 2, 3, 2, 3: the even ones rows 0 and 2, the odd ones rows 1 and 3. */
static void unswz_neon2(const uint8_t *src, const HleXgRect *r, uint8_t *dst,
                        uint32_t pitch, uint32_t mx, uint32_t my,
                        uint32_t px, uint32_t py)
{
    uint32_t mxh = mx & ~0x5u, myh = my & ~0xAu;
    uint32_t sy = deposit(my, r->y);
    for (uint32_t j = 0; j < r->h; j += 4) {
        uint8_t *d0 = dst + (size_t)(py + j) * pitch + (size_t)px * 2;
        uint8_t *d1 = d0 + pitch;
        uint8_t *d2 = d1 + pitch;
        uint8_t *d3 = d2 + pitch;
        uint32_t sx = deposit(mx, r->x);
        for (uint32_t i = 0; i < r->w; i += 4) {
            const uint8_t *s = src + (size_t)(sx | sy) * 2;
            uint32x4_t a = vreinterpretq_u32_u8(vld1q_u8(s));
            uint32x4_t b = vreinterpretq_u32_u8(vld1q_u8(s + 16));
            uint8x16_t e = vreinterpretq_u8_u32(vuzp1q_u32(a, b));
            uint8x16_t o = vreinterpretq_u8_u32(vuzp2q_u32(a, b));
            vst1_u8(d0 + (size_t)i * 2, vget_low_u8(e));
            vst1_u8(d1 + (size_t)i * 2, vget_low_u8(o));
            vst1_u8(d2 + (size_t)i * 2, vget_high_u8(e));
            vst1_u8(d3 + (size_t)i * 2, vget_high_u8(o));
            sx = next_in(sx, mxh);
        }
        sy = next_in(sy, myh);
    }
}

/* 1 byte/texel: the same with halfwords (2 texels each) in one register. */
static void unswz_neon1(const uint8_t *src, const HleXgRect *r, uint8_t *dst,
                        uint32_t pitch, uint32_t mx, uint32_t my,
                        uint32_t px, uint32_t py)
{
    uint32_t mxh = mx & ~0x5u, myh = my & ~0xAu;
    uint32_t sy = deposit(my, r->y);
    for (uint32_t j = 0; j < r->h; j += 4) {
        uint8_t *d0 = dst + (size_t)(py + j) * pitch + px;
        uint8_t *d1 = d0 + pitch;
        uint8_t *d2 = d1 + pitch;
        uint8_t *d3 = d2 + pitch;
        uint32_t sx = deposit(mx, r->x);
        for (uint32_t i = 0; i < r->w; i += 4) {
            uint16x8_t q = vreinterpretq_u16_u8(vld1q_u8(src + (sx | sy)));
            uint32x4_t e = vreinterpretq_u32_u16(vuzp1q_u16(q, q));
            uint32x4_t o = vreinterpretq_u32_u16(vuzp2q_u16(q, q));
            uint32_t r0 = vgetq_lane_u32(e, 0), r1 = vgetq_lane_u32(o, 0);
            uint32_t r2 = vgetq_lane_u32(e, 1), r3 = vgetq_lane_u32(o, 1);
            memcpy(d0 + i, &r0, 4);
            memcpy(d1 + i, &r1, 4);
            memcpy(d2 + i, &r2, 4);
            memcpy(d3 + i, &r3, 4);
            sx = next_in(sx, mxh);
        }
        sy = next_in(sy, myh);
    }
}
#endif

/* Whole 4x4 blocks: each one's 16 texels are contiguous in the swizzled
 * texture (both sides at least 4, the rect and point 4-aligned). */
static bool blocks_ok(uint32_t mx, uint32_t my, const HleXgRect *r, uint32_t px, uint32_t py)
{
    return (mx & 0xFu) == 0x5u && (my & 0xFu) == 0xAu &&
           !((r->x | r->y | r->w | r->h | px | py) & 3u);
}

static bool neon_ok(uint32_t mx, uint32_t my, const HleXgRect *r, uint32_t px,
                    uint32_t py, uint32_t bpp)
{
    return bpp == 4 && blocks_ok(mx, my, r, px, py);
}

void hle_xg_swizzle_rect(const uint8_t *src, uint32_t pitch, const HleXgRect *r,
                         uint8_t *dst, uint32_t w, uint32_t h,
                         uint32_t px, uint32_t py, uint32_t bpp)
{
    HleXgRect all = { 0, 0, w, h };
    const HleXgRect *rr = r ? r : &all;
    uint32_t mx, my, mz;
    masks(w, h, 1, &mx, &my, &mz);
#if defined(__aarch64__)
    if (neon_ok(mx, my, rr, px, py, bpp)) {
        swz_neon(src, pitch, rr, dst, mx, my, px, py);
        return;
    }
#else
    (void)neon_ok;
#endif
    uint32_t sy = deposit(my, py);
    for (uint32_t j = 0; j < rr->h; j++) {
        const uint8_t *s = src + (size_t)(rr->y + j) * pitch + (size_t)rr->x * bpp;
        uint32_t sx = deposit(mx, px);
        for (uint32_t i = 0; i < rr->w; i++) {
            copy_texel(dst + (size_t)(sx | sy) * bpp, s + (size_t)i * bpp, bpp);
            sx = next_in(sx, mx);
        }
        sy = next_in(sy, my);
    }
}

void hle_xg_unswizzle_rect(const uint8_t *src, uint32_t w, uint32_t h,
                           const HleXgRect *r, uint8_t *dst, uint32_t pitch,
                           uint32_t px, uint32_t py, uint32_t bpp)
{
    HleXgRect all = { 0, 0, w, h };
    const HleXgRect *rr = r ? r : &all;
    uint32_t mx, my, mz;
    masks(w, h, 1, &mx, &my, &mz);
#if defined(__aarch64__)
    if (blocks_ok(mx, my, rr, px, py) && (bpp == 4 || bpp == 2 || bpp == 1)) {
        (bpp == 4 ? unswz_neon : bpp == 2 ? unswz_neon2 : unswz_neon1)(src, rr, dst, pitch, mx, my,
                                                                       px, py);
        return;
    }
#endif
    uint32_t sy = deposit(my, rr->y);
    for (uint32_t j = 0; j < rr->h; j++) {
        uint8_t *d = dst + (size_t)(py + j) * pitch + (size_t)px * bpp;
        uint32_t sx = deposit(mx, rr->x);
        for (uint32_t i = 0; i < rr->w; i++) {
            copy_texel(d + (size_t)i * bpp, src + (size_t)(sx | sy) * bpp, bpp);
            sx = next_in(sx, mx);
        }
        sy = next_in(sy, my);
    }
}

void hle_xg_unswizzle_rect_p8(const uint8_t *src, uint32_t w, uint32_t h,
                              const HleXgRect *r, const uint32_t *pal, uint8_t *dst,
                              uint32_t pitch, uint32_t px, uint32_t py)
{
    HleXgRect all = { 0, 0, w, h };
    const HleXgRect *rr = r ? r : &all;
    uint32_t mx, my, mz;
    masks(w, h, 1, &mx, &my, &mz);
    if (blocks_ok(mx, my, rr, px, py)) {
        /* each texel of a block, in row order: its index's place in the block */
        static const uint8_t k_at[16] = { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 };
        uint32_t mxh = mx & ~0x5u, myh = my & ~0xAu;
        uint32_t sy = deposit(my, rr->y);
        for (uint32_t j = 0; j < rr->h; j += 4) {
            uint8_t *d = dst + (size_t)(py + j) * pitch + (size_t)px * 4;
            uint32_t sx = deposit(mx, rr->x);
            for (uint32_t i = 0; i < rr->w; i += 4) {
                const uint8_t *s = src + (sx | sy);
                for (unsigned y = 0; y < 4; y++) {
                    /* a row's 16 bytes in one store (the arena is write-combined) */
                    const uint8_t *at = k_at + y * 4;
                    uint8_t *o = d + (size_t)y * pitch + (size_t)i * 4;
#if defined(__aarch64__)
                    uint32x4_t v = vld1q_dup_u32(&pal[s[at[0]]]);
                    v = vld1q_lane_u32(&pal[s[at[1]]], v, 1);
                    v = vld1q_lane_u32(&pal[s[at[2]]], v, 2);
                    v = vld1q_lane_u32(&pal[s[at[3]]], v, 3);
                    vst1q_u8(o, vreinterpretq_u8_u32(v));
#else
                    uint32_t v[4] = { pal[s[at[0]]], pal[s[at[1]]], pal[s[at[2]]], pal[s[at[3]]] };
                    memcpy(o, v, 16);
#endif
                }
                sx = next_in(sx, mxh);
            }
            sy = next_in(sy, myh);
        }
        return;
    }
    uint32_t sy = deposit(my, rr->y);
    for (uint32_t j = 0; j < rr->h; j++) {
        uint8_t *d = dst + (size_t)(py + j) * pitch + (size_t)px * 4;
        uint32_t sx = deposit(mx, rr->x);
        for (uint32_t i = 0; i < rr->w; i++) {
            uint32_t v = pal[src[sx | sy]];
            memcpy(d + (size_t)i * 4, &v, 4);
            sx = next_in(sx, mx);
        }
        sy = next_in(sy, my);
    }
}

void hle_xg_swizzle_box(const uint8_t *src, uint32_t row_pitch, uint32_t slice_pitch,
                        const HleXgBox *b, uint8_t *dst, uint32_t w, uint32_t h,
                        uint32_t d, uint32_t px, uint32_t py, uint32_t pz,
                        uint32_t bpp)
{
    HleXgBox all = { 0, 0, 0, w, h, d };
    const HleXgBox *bb = b ? b : &all;
    uint32_t mx, my, mz;
    masks(w, h, d, &mx, &my, &mz);
    uint32_t sz = deposit(mz, pz);
    for (uint32_t k = 0; k < bb->d; k++) {
        uint32_t sy = deposit(my, py);
        for (uint32_t j = 0; j < bb->h; j++) {
            const uint8_t *s = src + (size_t)(bb->z + k) * slice_pitch +
                               (size_t)(bb->y + j) * row_pitch + (size_t)bb->x * bpp;
            uint32_t sx = deposit(mx, px);
            for (uint32_t i = 0; i < bb->w; i++) {
                copy_texel(dst + (size_t)(sx | sy | sz) * bpp, s + (size_t)i * bpp, bpp);
                sx = next_in(sx, mx);
            }
            sy = next_in(sy, my);
        }
        sz = next_in(sz, mz);
    }
}

void hle_xg_unswizzle_box(const uint8_t *src, uint32_t w, uint32_t h, uint32_t d,
                          const HleXgBox *b, uint8_t *dst, uint32_t row_pitch,
                          uint32_t slice_pitch, uint32_t px, uint32_t py,
                          uint32_t pz, uint32_t bpp)
{
    HleXgBox all = { 0, 0, 0, w, h, d };
    const HleXgBox *bb = b ? b : &all;
    uint32_t mx, my, mz;
    masks(w, h, d, &mx, &my, &mz);
    uint32_t sz = deposit(mz, bb->z);
    for (uint32_t k = 0; k < bb->d; k++) {
        uint32_t sy = deposit(my, bb->y);
        for (uint32_t j = 0; j < bb->h; j++) {
            uint8_t *dd = dst + (size_t)(pz + k) * slice_pitch +
                          (size_t)(py + j) * row_pitch + (size_t)px * bpp;
            uint32_t sx = deposit(mx, bb->x);
            for (uint32_t i = 0; i < bb->w; i++) {
                copy_texel(dd + (size_t)i * bpp, src + (size_t)(sx | sy | sz) * bpp, bpp);
                sx = next_in(sx, mx);
            }
            sy = next_in(sy, my);
        }
        sz = next_in(sz, mz);
    }
}
