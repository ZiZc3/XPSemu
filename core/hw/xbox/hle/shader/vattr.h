/*
 * hakuX HLE GPU — NV2A vertex attribute formats (pure).
 *
 * An attribute format byte is the low byte of NV097_SET_VERTEX_DATA_ARRAY_FORMAT
 * (and the Xbox D3DVSDT_* value): bits 0-3 type, bits 4-7 component count
 * (0 = attribute disabled, 7 = three components, "3W").
 */
#ifndef HW_XBOX_HLE_SHADER_VATTR_H
#define HW_XBOX_HLE_SHADER_VATTR_H

#include <stdint.h>

/* NV2A attribute types */
enum {
    HLE_VAT_UB_D3D = 0,                 /* 4 unsigned bytes, B G R A (D3DCOLOR) */
    HLE_VAT_S1 = 1,                     /* signed shorts, normalized */
    HLE_VAT_F = 2,                      /* floats */
    HLE_VAT_UB_OGL = 4,                 /* unsigned bytes, normalized, in order */
    HLE_VAT_S32K = 5,                   /* signed shorts, not normalized */
    HLE_VAT_CMP = 6,                    /* 11:11:10 signed normalized, one dword */
};

static inline unsigned hle_vat_type(uint8_t fmt)
{
    return fmt & 0xF;
}

/* Components fetched (0 = disabled). */
static inline unsigned hle_vat_count(uint8_t fmt)
{
    unsigned n = fmt >> 4;
    return n == 7 ? 3 : n;
}

#endif
