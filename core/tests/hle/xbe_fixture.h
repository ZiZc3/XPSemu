/* Builds a synthetic XBE header image (base 0x10000) for parser tests.
 * Layout: header 0x000..0x178, certificate 0x200, section headers 0x400,
 * section names 0x800, library versions 0xA00. Returns sizeof_headers. */
#ifndef TESTS_HLE_XBE_FIXTURE_H
#define TESTS_HLE_XBE_FIXTURE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct FxSection {
    const char *name;
    uint32_t flags, va, vsize, raw_off, raw_size;
} FxSection;

typedef struct FxLib {
    const char *name;
    uint16_t major, minor, build, flags;
} FxLib;

static inline void fx_w32(uint8_t *b, uint32_t off, uint32_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16);
    b[off + 3] = (uint8_t)(v >> 24);
}

static inline void fx_w16(uint8_t *b, uint32_t off, uint16_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
}

static inline uint32_t fx_build_xbe(uint8_t *buf, uint32_t buflen,
                                    uint32_t entry, bool debug,
                                    const FxSection *secs, unsigned nsec,
                                    const FxLib *libs, unsigned nlib,
                                    uint32_t title_id)
{
    const uint32_t base = 0x10000;
    memset(buf, 0, buflen);
    fx_w32(buf, 0x000, 0x48454258u);             /* "XBEH" */
    fx_w32(buf, 0x104, base);
    fx_w32(buf, 0x108, 0x1000);                  /* sizeof_headers */
    fx_w32(buf, 0x10C, 0x400000);                /* sizeof_image */
    fx_w32(buf, 0x114, 0x3C000000);              /* timedate */
    fx_w32(buf, 0x118, base + 0x200);            /* certificate */
    fx_w32(buf, 0x11C, nsec);
    fx_w32(buf, 0x120, base + 0x400);
    fx_w32(buf, 0x128, entry ^ (debug ? 0x94859D4Bu : 0xA8FC57ABu));
    fx_w32(buf, 0x158, 0x00012000u ^ (debug ? 0xEFB1F152u : 0x5B6D40B6u));
    fx_w32(buf, 0x160, nlib);
    fx_w32(buf, 0x164, base + 0xA00);
    fx_w32(buf, 0x200 + 0x08, title_id);

    uint32_t name_off = 0x800;
    for (unsigned i = 0; i < nsec; i++) {
        uint32_t s = 0x400 + i * 56;
        fx_w32(buf, s + 0x00, secs[i].flags);
        fx_w32(buf, s + 0x04, secs[i].va);
        fx_w32(buf, s + 0x08, secs[i].vsize);
        fx_w32(buf, s + 0x0C, secs[i].raw_off);
        fx_w32(buf, s + 0x10, secs[i].raw_size);
        fx_w32(buf, s + 0x14, base + name_off);
        size_t n = strlen(secs[i].name);
        memcpy(buf + name_off, secs[i].name, n);
        buf[name_off + n] = 0;
        name_off += (uint32_t)n + 1;
    }
    for (unsigned i = 0; i < nlib; i++) {
        uint32_t l = 0xA00 + i * 16;
        size_t n = strlen(libs[i].name);
        memcpy(buf + l, libs[i].name, n > 8 ? 8 : n);
        fx_w16(buf, l + 8, libs[i].major);
        fx_w16(buf, l + 10, libs[i].minor);
        fx_w16(buf, l + 12, libs[i].build);
        fx_w16(buf, l + 14, libs[i].flags);
    }
    return 0x1000;
}

#endif
