/*
 * hakuX HLE GPU — the last GPU use per guest page: the timeline value of
 * the last submission that reads it (the write barrier, mem/barrier.c).
 */
#ifndef HW_XBOX_HLE_MEM_PAGEUSE_H
#define HW_XBOX_HLE_MEM_PAGEUSE_H

#include "hw/xbox/hle/hle_pure.h"

#define HLE_PAGE_SHIFT 12

typedef struct HlePageUse {
    uint64_t *v;                /* last GPU use per page */
    uint32_t npages;
    uint64_t max;               /* highest value ever marked */
} HlePageUse;

bool hle_pageuse_init(HlePageUse *p, uint64_t ram_bytes);
void hle_pageuse_fini(HlePageUse *p);
void hle_pageuse_mark(HlePageUse *p, uint32_t pa, uint32_t len, uint64_t value);
uint64_t hle_pageuse_get(const HlePageUse *p, uint32_t pa, uint32_t len);

#endif
