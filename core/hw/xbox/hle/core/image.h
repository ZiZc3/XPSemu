/*
 * hakuX HLE GPU — XBE section helpers (the title's D3D library ranges).
 */
#ifndef HW_XBOX_HLE_CORE_IMAGE_H
#define HW_XBOX_HLE_CORE_IMAGE_H

#include "hw/xbox/hle/core/xbe.h"

/*
 * Section [va, va + vsize) checked against the image [base, base + size).
 * Returns false for a malformed section: one that starts outside the image
 * or whose end wraps past the 32-bit address space. Otherwise *end is the
 * section end clamped to the image end.
 */
static inline bool hle_img_section_end(uint32_t base, uint32_t size, uint32_t va,
                                       uint32_t vsize, uint32_t *end)
{
    uint64_t img_end = (uint64_t)base + size;
    uint64_t sec_end = (uint64_t)va + vsize;
    if (va < base || va >= img_end || sec_end > UINT32_MAX) {
        return false;
    }
    *end = (uint32_t)MIN(sec_end, img_end);
    return true;
}

/* Library section test: D3D* (not D3DX*) and XGRPH*. */
static inline bool hle_is_lib_section_name(const char *n)
{
    if (strncmp(n, "XGRPH", 5) == 0) {
        return true;
    }
    return strncmp(n, "D3D", 3) == 0 && strncmp(n, "D3DX", 4) != 0;
}

#endif
