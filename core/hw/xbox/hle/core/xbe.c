/*
 * hakuX HLE GPU — pure XBE header parser.
 */
#include "hw/xbox/hle/core/xbe.h"

/* Header field offsets (xemu-xbe.h struct xbe_header, packed). */
#define XO_MAGIC            0x000
#define XO_BASE             0x104
#define XO_SIZEOF_HEADERS   0x108
#define XO_SIZEOF_IMAGE     0x10C
#define XO_TIMEDATE         0x114
#define XO_CERT_ADDR        0x118
#define XO_NSECTIONS        0x11C
#define XO_SECTIONS_ADDR    0x120
#define XO_ENTRY            0x128
#define XO_KERNEL_THUNK     0x158
#define XO_NLIBS            0x160
#define XO_LIBS_ADDR        0x164
#define XO_MIN_LEN          0x178

#define SEC_HDR_SIZE        56
#define LIB_SIZE            16
#define CERT_TITLE_ID       0x08

/* Maps header VA range [va, va+len) to an offset in hdr[]. */
static bool hva(uint32_t base, uint32_t hdr_len, uint32_t va, uint32_t len,
                uint32_t *off)
{
    if (va < base) {
        return false;
    }
    uint32_t o = va - base;
    if (o > hdr_len || len > hdr_len - o) {
        return false;
    }
    *off = o;
    return true;
}

static void copy_name(char dst[9], const uint8_t *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < 8 && i < max && src[i]; i++) {
        dst[i] = (char)src[i];
    }
    dst[i] = '\0';
}

int hle_xbe_parse(const uint8_t *hdr, uint32_t hdr_len, HleXbeInfo *out)
{
    memset(out, 0, sizeof(*out));
    if (hdr_len < XO_MIN_LEN) {
        return HLE_XBE_ERR_SHORT;
    }
    if (hle_rd32(hdr + XO_MAGIC) != HLE_XBE_MAGIC) {
        return HLE_XBE_ERR_MAGIC;
    }

    out->base = hle_rd32(hdr + XO_BASE);
    out->sizeof_headers = hle_rd32(hdr + XO_SIZEOF_HEADERS);
    out->sizeof_image = hle_rd32(hdr + XO_SIZEOF_IMAGE);
    out->timedate = hle_rd32(hdr + XO_TIMEDATE);

    uint32_t enc_entry = hle_rd32(hdr + XO_ENTRY);
    uint32_t enc_thunk = hle_rd32(hdr + XO_KERNEL_THUNK);
    uint32_t lo = out->base;
    uint32_t hi = out->base + out->sizeof_image;
    uint32_t e_retail = enc_entry ^ HLE_XBE_ENTRY_KEY_RETAIL;
    uint32_t e_debug = enc_entry ^ HLE_XBE_ENTRY_KEY_DEBUG;
    if (e_retail >= lo && e_retail < hi) {
        out->entry = e_retail;
        out->kernel_thunk = enc_thunk ^ HLE_XBE_THUNK_KEY_RETAIL;
    } else if (e_debug >= lo && e_debug < hi) {
        out->entry = e_debug;
        out->kernel_thunk = enc_thunk ^ HLE_XBE_THUNK_KEY_DEBUG;
        out->debug_keys = true;
    } else if (enc_entry >= lo && enc_entry < hi) {
        /* The loader leaves the in-memory header at 0x10000 decoded. */
        out->entry = enc_entry;
        out->kernel_thunk = enc_thunk;
        out->plain_entry = true;
    } else {
        return HLE_XBE_ERR_ENTRY;
    }

    uint32_t off;
    uint32_t cert = hle_rd32(hdr + XO_CERT_ADDR);
    if (hva(out->base, hdr_len, cert, CERT_TITLE_ID + 4, &off)) {
        out->title_id = hle_rd32(hdr + off + CERT_TITLE_ID);
    }

    uint32_t nsec = hle_rd32(hdr + XO_NSECTIONS);
    uint32_t sec_addr = hle_rd32(hdr + XO_SECTIONS_ADDR);
    if (nsec > HLE_XBE_MAX_SECTIONS ||
        !hva(out->base, hdr_len, sec_addr, nsec * SEC_HDR_SIZE, &off)) {
        return HLE_XBE_ERR_RANGE;
    }
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *s = hdr + off + i * SEC_HDR_SIZE;
        HleXbeSection *d = &out->sections[i];
        d->flags = hle_rd32(s + 0x00);
        d->va = hle_rd32(s + 0x04);
        d->vsize = hle_rd32(s + 0x08);
        d->raw_off = hle_rd32(s + 0x0C);
        d->raw_size = hle_rd32(s + 0x10);
        uint32_t noff;
        if (hva(out->base, hdr_len, hle_rd32(s + 0x14), 1, &noff)) {
            copy_name(d->name, hdr + noff, hdr_len - noff);
        }
    }
    out->nsections = nsec;

    uint32_t nlib = hle_rd32(hdr + XO_NLIBS);
    uint32_t lib_addr = hle_rd32(hdr + XO_LIBS_ADDR);
    if (nlib > HLE_XBE_MAX_LIBS ||
        !hva(out->base, hdr_len, lib_addr, nlib * LIB_SIZE, &off)) {
        return HLE_XBE_ERR_RANGE;
    }
    for (uint32_t i = 0; i < nlib; i++) {
        const uint8_t *l = hdr + off + i * LIB_SIZE;
        HleXbeLib *d = &out->libs[i];
        copy_name(d->name, l, 8);
        d->major = hle_rd16(l + 8);
        d->minor = hle_rd16(l + 10);
        d->build = hle_rd16(l + 12);
        uint16_t flags = hle_rd16(l + 14);
        d->qfe = flags & 0x1FFF;
        d->debug_build = (flags & 0x8000) != 0;
    }
    out->nlibs = nlib;
    return HLE_XBE_OK;
}

const HleXbeSection *hle_xbe_find_section(const HleXbeInfo *x,
                                          const char *name)
{
    for (unsigned i = 0; i < x->nsections; i++) {
        if (strcmp(x->sections[i].name, name) == 0) {
            return &x->sections[i];
        }
    }
    return NULL;
}

const HleXbeLib *hle_xbe_find_lib(const HleXbeInfo *x, const char *name)
{
    for (unsigned i = 0; i < x->nlibs; i++) {
        if (strcmp(x->libs[i].name, name) == 0) {
            return &x->libs[i];
        }
    }
    return NULL;
}

const HleXbeLib *hle_xbe_d3d_lib(const HleXbeInfo *x)
{
    const HleXbeLib *l = hle_xbe_find_lib(x, "D3D8");
    return l ? l : hle_xbe_find_lib(x, "D3D8LTCG");
}

const HleXbeSection *hle_xbe_section_at(const HleXbeInfo *x, uint32_t va)
{
    for (unsigned i = 0; i < x->nsections; i++) {
        const HleXbeSection *s = &x->sections[i];
        if (va >= s->va && va - s->va < s->vsize) {
            return s;
        }
    }
    return NULL;
}
