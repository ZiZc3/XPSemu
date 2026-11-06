/*
 * hakuX native core — Unicode case for the runtime library's upcase exports
 * (RtlUpcaseUnicodeChar and the string forms): the simple uppercase of the
 * scripts titles write names in. ASCII, Latin-1, Latin Extended-A, Greek,
 * Cyrillic and the fullwidth forms; everything else is its own uppercase.
 * Header-only, for the host test (tests/native/rtl).
 */
#ifndef NC_RTL_CASE_H
#define NC_RTL_CASE_H

#include <stdint.h>

static inline uint16_t nc_rtl_upcase(uint16_t c)
{
    if (c < 'a') {
        return c;
    }
    if (c <= 'z') {
        return (uint16_t)(c - 32);
    }
    if (c < 0xB5) {
        return c;
    }
    if (c == 0xB5) {
        return 0x39C;                       /* micro sign: Greek capital mu */
    }
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
        return (uint16_t)(c - 32);          /* Latin-1 letters (not the division sign) */
    }
    if (c == 0xFF) {
        return 0x178;
    }
    if (c >= 0x100 && c <= 0x17F) {        /* Latin Extended-A: case pairs */
        if (c <= 0x137 || (c >= 0x14A && c <= 0x177)) {
            return (uint16_t)(c & 1 ? c - 1 : c);     /* upper even, lower odd */
        }
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
            return (uint16_t)(c & 1 ? c : c - 1);     /* upper odd, lower even */
        }
        return c == 0x17F ? 'S' : c;                  /* long s */
    }
    if (c >= 0x3B1 && c <= 0x3CB) {         /* Greek */
        return c == 0x3C2 ? 0x3A3 : (uint16_t)(c - 32);
    }
    if (c >= 0x430 && c <= 0x44F) {         /* Cyrillic */
        return (uint16_t)(c - 32);
    }
    if (c >= 0x450 && c <= 0x45F) {
        return (uint16_t)(c - 80);
    }
    if (c >= 0xFF41 && c <= 0xFF5A) {       /* fullwidth a-z */
        return (uint16_t)(c - 32);
    }
    return c;
}

#endif
