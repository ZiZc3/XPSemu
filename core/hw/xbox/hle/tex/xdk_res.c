/*
 * hakuX HLE GPU — XDK resource model.
 */
#include "hw/xbox/hle/tex/xdk_res.h"

static const HleFmtInfo k_fmt[] = {
    { 0x00, 8, HLE_FI_SWZ }, { 0x01, 8, HLE_FI_SWZ }, { 0x02, 16, HLE_FI_SWZ },
    { 0x03, 16, HLE_FI_SWZ }, { 0x04, 16, HLE_FI_SWZ }, { 0x05, 16, HLE_FI_SWZ },
    { 0x06, 32, HLE_FI_SWZ }, { 0x07, 32, HLE_FI_SWZ }, { 0x0B, 8, HLE_FI_SWZ },
    { 0x0C, 4, HLE_FI_DXT }, { 0x0E, 8, HLE_FI_DXT }, { 0x0F, 8, HLE_FI_DXT },
    { 0x10, 16, HLE_FI_LIN }, { 0x11, 16, HLE_FI_LIN }, { 0x12, 32, HLE_FI_LIN },
    { 0x13, 8, HLE_FI_LIN }, { 0x16, 16, HLE_FI_LIN }, { 0x17, 16, HLE_FI_LIN },
    { 0x19, 8, HLE_FI_SWZ }, { 0x1A, 16, HLE_FI_SWZ }, { 0x1B, 8, HLE_FI_LIN },
    { 0x1C, 16, HLE_FI_LIN }, { 0x1D, 16, HLE_FI_LIN }, { 0x1E, 32, HLE_FI_LIN },
    { 0x1F, 8, HLE_FI_LIN }, { 0x20, 16, HLE_FI_LIN },
    { 0x24, 16, HLE_FI_LIN | HLE_FI_YUV }, { 0x25, 16, HLE_FI_LIN | HLE_FI_YUV },
    { 0x27, 16, HLE_FI_SWZ }, { 0x28, 16, HLE_FI_SWZ }, { 0x29, 16, HLE_FI_SWZ },
    { 0x2A, 32, HLE_FI_SWZ | HLE_FI_DEPTH }, { 0x2B, 32, HLE_FI_SWZ | HLE_FI_DEPTH },
    { 0x2C, 16, HLE_FI_SWZ | HLE_FI_DEPTH }, { 0x2D, 16, HLE_FI_SWZ | HLE_FI_DEPTH },
    { 0x2E, 32, HLE_FI_LIN | HLE_FI_DEPTH }, { 0x2F, 32, HLE_FI_LIN | HLE_FI_DEPTH },
    { 0x30, 16, HLE_FI_LIN | HLE_FI_DEPTH }, { 0x31, 16, HLE_FI_LIN | HLE_FI_DEPTH },
    { 0x32, 16, HLE_FI_SWZ }, { 0x33, 32, HLE_FI_SWZ }, { 0x35, 16, HLE_FI_LIN },
    { 0x36, 32, HLE_FI_LIN }, { 0x37, 16, HLE_FI_LIN }, { 0x38, 16, HLE_FI_SWZ },
    { 0x39, 16, HLE_FI_SWZ }, { 0x3A, 32, HLE_FI_SWZ }, { 0x3B, 32, HLE_FI_SWZ },
    { 0x3C, 32, HLE_FI_SWZ }, { 0x3D, 16, HLE_FI_LIN }, { 0x3E, 16, HLE_FI_LIN },
    { 0x3F, 32, HLE_FI_LIN }, { 0x40, 32, HLE_FI_LIN }, { 0x41, 32, HLE_FI_LIN },
};

const HleFmtInfo *hle_fmt_info(uint32_t code)
{
    for (size_t i = 0; i < sizeof(k_fmt) / sizeof(k_fmt[0]); i++) {
        if (k_fmt[i].code == code) {
            return &k_fmt[i];
        }
    }
    return NULL;
}

static int log2_exact(uint32_t v)
{
    if (!v || (v & (v - 1))) {
        return -1;
    }
    int n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static uint32_t level_bytes(const HleFmtInfo *fi, uint32_t w, uint32_t h, uint32_t d)
{
    if (fi->flags & HLE_FI_DXT) {
        return ((w + 3) / 4) * ((h + 3) / 4) * fi->bpp * 2 * d;
    }
    return w * h * d * fi->bpp / 8;
}

static uint32_t level_pitch(const HleFmtInfo *fi, uint32_t w)
{
    if (fi->flags & HLE_FI_DXT) {
        return ((w + 3) / 4) * fi->bpp * 2;
    }
    return w * fi->bpp / 8;
}

void hle_res_level(const HleResDesc *d, uint32_t level, uint32_t *sw, uint32_t *sh,
                   uint32_t *ofs)
{
    if (!d->border) {
        *sw = max_u32(d->w >> level, 1);
        *sh = max_u32(d->h >> level, 1);
        *ofs = 0;
        return;
    }
    *sw = max_u32(max_u32(16, 2 * d->w) >> level, 1);
    *sh = max_u32(max_u32(16, 2 * d->h) >> level, 1);
    *ofs = 4u >> level;
}

/* Linear row pitch: Size[31:24] holds pitch / 64 - 1. */
#define LIN_PITCH_ALIGN 64u
#define LIN_PITCH_MAX   (256u * LIN_PITCH_ALIGN)

static uint32_t lin_min_pitch(const HleFmtInfo *fi, uint32_t w)
{
    return align_up(w * fi->bpp / 8, LIN_PITCH_ALIGN);
}

static uint32_t common_of(uint32_t type)
{
    return 1u | type | HLE_COMMON_D3DCREATED;
}

static bool buffer_info(const HleResDesc *d, HleResInfo *o)
{
    switch (d->kind) {
    case HLE_RK_VB:
        o->common = common_of(HLE_COMMON_TYPE_VERTEXBUFFER);
        o->hdr_bytes = 12;
        o->contiguous = true;
        break;
    case HLE_RK_IB:
        o->common = common_of(HLE_COMMON_TYPE_INDEXBUFFER);
        o->hdr_bytes = 12;
        o->contiguous = false;
        break;
    case HLE_RK_PUSHBUF:
        o->common = common_of(HLE_COMMON_TYPE_PUSHBUFFER);
        o->hdr_bytes = 20;
        o->contiguous = true;
        break;
    case HLE_RK_FIXUP:
        o->common = common_of(HLE_COMMON_TYPE_FIXUP);
        o->hdr_bytes = 28;
        o->contiguous = false;
        break;
    case HLE_RK_PALETTE: {
        uint32_t code = d->length == 256 ? 0 : d->length == 128 ? 1 :
                        d->length == 64 ? 2 : d->length == 32 ? 3 : 4;
        if (code == 4) {
            return false;
        }
        o->common = common_of(HLE_COMMON_TYPE_PALETTE) |
                    (code << HLE_COMMON_PALETTE_SHIFT);
        o->hdr_bytes = 12;
        o->contiguous = true;
        o->bytes = d->length * 4;
        return true;
    }
    default:
        return false;
    }
    o->bytes = d->length;
    return d->length > 0;
}

bool hle_res_info(const HleResDesc *d, HleResInfo *o)
{
    memset(o, 0, sizeof(*o));
    if (d->kind < HLE_RK_TEXTURE) {
        return buffer_info(d, o);
    }
    if (d->kind > HLE_RK_SURFACE) {
        return false;
    }
    const HleFmtInfo *fi = hle_fmt_info(d->fmt);
    if (!fi || !d->w || !d->h) {
        return false;
    }
    bool cube = d->kind == HLE_RK_CUBE, vol = d->kind == HLE_RK_VOLUME;
    bool surf = d->kind == HLE_RK_SURFACE;
    uint32_t depth = vol ? d->d : 1;
    o->common = common_of(surf ? HLE_COMMON_TYPE_SURFACE : HLE_COMMON_TYPE_TEXTURE);
    o->hdr_bytes = surf ? 24 : 20;
    o->contiguous = true;
    /* Every Format the XDK builds has DMA_A | BORDER_COLOR: D3D's header
     * encoder ends with `or eax, 9` (PGR2 0x1c8f5c) and XGRAPHC's with
     * `or esi, 9` (0x1e0581); P2 Task 11's CreateDevice verify showed it on
     * all three device surfaces. */
    const uint32_t base = HLE_FMT_DMA_A | (d->border ? 0 : HLE_FMT_BORDER_COLOR);
    if (fi->flags & HLE_FI_LIN) {
        if (cube || vol || d->w > 4096 || d->h > 4096) {
            return false;
        }
        uint32_t min = lin_min_pitch(fi, d->w);
        uint32_t pitch = d->pitch ? d->pitch : min;
        if (pitch % LIN_PITCH_ALIGN || pitch < min || pitch > LIN_PITCH_MAX) {
            return false;
        }
        o->levels = 1;
        o->pitch = pitch;
        o->bytes = pitch * d->h;
        o->format = base | (2u << HLE_FMT_DIM_SHIFT) |
                    (d->fmt << HLE_FMT_FORMAT_SHIFT) | (1u << HLE_FMT_MIPMAP_SHIFT);
        o->size = (d->w - 1) | ((d->h - 1) << HLE_SIZE_HEIGHT_SHIFT) |
                  ((pitch / LIN_PITCH_ALIGN - 1) << HLE_SIZE_PITCH_SHIFT);
        return true;
    }
    int lw = log2_exact(d->w), lh = log2_exact(d->h), ld = log2_exact(depth);
    if (lw < 0 || lh < 0 || ld < 0 || lw > 12 || lh > 12 || ld > 9) {
        return false;
    }
    uint32_t maxl = max_u32(max_u32((uint32_t)lw, (uint32_t)lh), (uint32_t)ld) + 1;
    uint32_t levels = surf ? 1 : (d->levels && d->levels < maxl ? d->levels : maxl);
    uint32_t chain = 0, sw, sh, ofs;
    for (uint32_t l = 0; l < levels; l++) {
        hle_res_level(d, l, &sw, &sh, &ofs);
        chain += level_bytes(fi, sw, sh, max_u32(depth >> l, 1));
    }
    hle_res_level(d, 0, &sw, &sh, &ofs);
    o->levels = levels;
    o->bytes = cube ? 6 * align_up(chain, 128) : chain;
    o->pitch = level_pitch(fi, sw);
    o->format = base | (cube ? HLE_FMT_CUBEMAP : 0) |
                ((vol ? 3u : 2u) << HLE_FMT_DIM_SHIFT) |
                (d->fmt << HLE_FMT_FORMAT_SHIFT) | (levels << HLE_FMT_MIPMAP_SHIFT) |
                ((uint32_t)lw << HLE_FMT_USIZE_SHIFT) |
                ((uint32_t)lh << HLE_FMT_VSIZE_SHIFT) |
                ((uint32_t)ld << HLE_FMT_PSIZE_SHIFT);
    return true;
}

uint32_t hle_res_subresource(const HleResDesc *d, uint32_t face, uint32_t level,
                             uint32_t *pitch)
{
    HleResInfo ri;
    const HleFmtInfo *fi = hle_fmt_info(d->fmt);
    if (!fi || !hle_res_info(d, &ri)) {
        *pitch = 0;
        return 0;
    }
    if (fi->flags & HLE_FI_LIN) {
        *pitch = ri.pitch;
        return 0;
    }
    uint32_t depth = d->kind == HLE_RK_VOLUME ? d->d : 1;
    uint32_t off = 0, chain = 0, sw, sh, ofs;
    for (uint32_t l = 0; l < ri.levels; l++) {
        hle_res_level(d, l, &sw, &sh, &ofs);
        uint32_t b = level_bytes(fi, sw, sh, max_u32(depth >> l, 1));
        if (l < level) {
            off += b;
        }
        chain += b;
    }
    if (d->kind == HLE_RK_CUBE) {
        off += face * align_up(chain, 128);
    }
    hle_res_level(d, level, &sw, &sh, &ofs);
    *pitch = level_pitch(fi, sw);
    return off;
}

bool hle_res_from_header(uint32_t common, uint32_t format, uint32_t size,
                         HleResDesc *out)
{
    memset(out, 0, sizeof(*out));
    switch (common & HLE_COMMON_TYPE_MASK) {
    case HLE_COMMON_TYPE_VERTEXBUFFER: out->kind = HLE_RK_VB; return true;
    case HLE_COMMON_TYPE_INDEXBUFFER:  out->kind = HLE_RK_IB; return true;
    case HLE_COMMON_TYPE_PUSHBUFFER:   out->kind = HLE_RK_PUSHBUF; return true;
    case HLE_COMMON_TYPE_FIXUP:        out->kind = HLE_RK_FIXUP; return true;
    case HLE_COMMON_TYPE_PALETTE:
        out->kind = HLE_RK_PALETTE;
        out->length = 256u >> (common >> HLE_COMMON_PALETTE_SHIFT);
        return true;
    case HLE_COMMON_TYPE_TEXTURE:
    case HLE_COMMON_TYPE_SURFACE:
        break;
    default:
        return false;
    }
    bool surf = (common & HLE_COMMON_TYPE_MASK) == HLE_COMMON_TYPE_SURFACE;
    out->fmt = HLE_FMT_CODE(format);
    const HleFmtInfo *fi = hle_fmt_info(out->fmt);
    if (!fi) {
        return false;
    }
    if (fi->flags & HLE_FI_LIN) {
        out->kind = surf ? HLE_RK_SURFACE : HLE_RK_TEXTURE;
        out->w = (size & HLE_SIZE_WIDTH_MASK) + 1;
        out->h = ((size >> HLE_SIZE_HEIGHT_SHIFT) & 0xFFFu) + 1;
        out->d = 1;
        out->levels = 1;
        out->pitch = ((size >> HLE_SIZE_PITCH_SHIFT) + 1) * LIN_PITCH_ALIGN;
        return out->pitch >= lin_min_pitch(fi, out->w);
    }
    out->w = 1u << HLE_FMT_GET(format, HLE_FMT_USIZE_SHIFT);
    out->h = 1u << HLE_FMT_GET(format, HLE_FMT_VSIZE_SHIFT);
    out->d = 1u << HLE_FMT_GET(format, HLE_FMT_PSIZE_SHIFT);
    out->levels = HLE_FMT_GET(format, HLE_FMT_MIPMAP_SHIFT);
    if (surf) {
        out->kind = HLE_RK_SURFACE;
    } else if (format & HLE_FMT_CUBEMAP) {
        out->kind = HLE_RK_CUBE;
    } else if (HLE_FMT_GET(format, HLE_FMT_DIM_SHIFT) == 3) {
        out->kind = HLE_RK_VOLUME;
    } else {
        out->kind = HLE_RK_TEXTURE;
    }
    if (out->kind != HLE_RK_VOLUME) {
        out->d = 1;
    }
    /* D3D always sets BORDER_COLOR (hle_res_info); a title that clears it
     * asks for the texture border. DXT, depth and volume borders are not
     * decoded (none seen) and keep the plain layout. */
    out->border = !(format & HLE_FMT_BORDER_COLOR) && fi->flags == HLE_FI_SWZ &&
                  (out->kind == HLE_RK_TEXTURE || out->kind == HLE_RK_CUBE);
    return true;
}
