/*
 * hakuX HLE GPU — XDK resource model: formats, header encoding, sizes and
 * subresource offsets (spec §4.9 "compute size and alignment as the XDK
 * does ... write bit-exact XDK headers").
 */
#ifndef HW_XBOX_HLE_TEX_XDK_RES_H
#define HW_XBOX_HLE_TEX_XDK_RES_H

#include "hw/xbox/hle/tex/xdk.h"

#define HLE_FI_LIN   0x01   /* linear: Size field, pitch aligned to 64 */
#define HLE_FI_SWZ   0x02   /* swizzled: power-of-two dims in Format */
#define HLE_FI_DXT   0x04   /* 4x4 blocks; bpp = bits per texel */
#define HLE_FI_DEPTH 0x08
#define HLE_FI_YUV   0x10

typedef struct HleFmtInfo {
    uint8_t code;
    uint8_t bpp;            /* bits per texel */
    uint8_t flags;
} HleFmtInfo;

const HleFmtInfo *hle_fmt_info(uint32_t code);

enum HleResKind {
    HLE_RK_VB = 1, HLE_RK_IB, HLE_RK_PUSHBUF, HLE_RK_FIXUP, HLE_RK_PALETTE,
    HLE_RK_TEXTURE, HLE_RK_CUBE, HLE_RK_VOLUME, HLE_RK_SURFACE,
};

typedef struct HleResDesc {
    uint8_t kind;           /* HleResKind */
    uint32_t fmt;           /* X_D3DFORMAT code (pixel containers) */
    uint32_t w, h, d;       /* texels; d = 1 except volumes */
    uint32_t levels;        /* 0 = full chain */
    uint32_t length;        /* VB/IB/push/fixup bytes; palette entries */
    uint32_t pitch;         /* linear formats: row pitch in bytes, 0 = XDK
                               minimum (align64(w * bpp / 8)); otherwise a
                               multiple of 64 in [minimum, 16384]. Ignored
                               for non-linear formats. */
    bool border;            /* texels stored with the NV2A's texture border
                               (Format BORDER_SOURCE_TEXTURE, bit 3 clear):
                               swizzled 2D and cube textures, hle_res_level */
} HleResDesc;

typedef struct HleResInfo {
    uint32_t common;        /* refcount 1 | type | D3DCREATED */
    uint32_t format;
    uint32_t size;
    uint32_t levels;
    uint32_t bytes;         /* data allocation */
    uint32_t hdr_bytes;     /* header allocation */
    uint32_t pitch;         /* level-0 row pitch */
    bool contiguous;        /* Data = PA of MmAllocateContiguousMemoryEx
                               memory; else Data = VA of pool memory */
} HleResInfo;

bool hle_res_info(const HleResDesc *d, HleResInfo *out);
/* Level `level` as stored: *sw x *sh texels, the image (w >> level x h >>
 * level) at (*ofs, *ofs). Without a border that is the image itself. With
 * one the NV2A addresses a texture twice the size, at least 16, with the
 * image 4 texels in, and its mip chain halves that: Stranger's Wrath fills
 * its 8x8 lighting cubes as 16x16 faces, the image at (4, 4) and the border
 * from the neighbouring faces. */
void hle_res_level(const HleResDesc *d, uint32_t level, uint32_t *sw, uint32_t *sh,
                   uint32_t *ofs);
/* Offset of (face, level) inside the data; *pitch = that level's row pitch. */
uint32_t hle_res_subresource(const HleResDesc *d, uint32_t face, uint32_t level,
                             uint32_t *pitch);
/* Decodes a header back into a description (adopted / XPR resources). A
 * linear header keeps its Size pitch; one whose pitch is below the row's
 * minimum is rejected. */
bool hle_res_from_header(uint32_t common, uint32_t format, uint32_t size,
                         HleResDesc *out);

/* Data field -> CPU pointer (kernel physical window) and -> PA. */
static inline uint32_t hle_res_data_va(uint32_t data)
{
    return data >= 0x80000000u ? data : (data | 0x80000000u);
}

static inline uint32_t hle_res_data_pa(uint32_t data)
{
    return data & 0x0FFFFFFFu;
}

#endif
