/*
 * hakuX HLE GPU — portability header for "pure" modules.
 *
 * Pure modules build both in the HLE library and in the standalone host
 * test harness (tests/hle, which defines HLE_HOST_TEST). They include this
 * header first instead of base.h.
 */
#ifndef HW_XBOX_HLE_PURE_H
#define HW_XBOX_HLE_PURE_H

#ifdef HLE_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#else
#include "hw/xbox/hle/base.h"
#endif

static inline uint16_t hle_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t hle_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif
