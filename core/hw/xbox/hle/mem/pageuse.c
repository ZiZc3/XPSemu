/*
 * hakuX HLE GPU — the page-use table (pageuse.h).
 */
#include "hw/xbox/hle/mem/pageuse.h"

bool hle_pageuse_init(HlePageUse *p, uint64_t ram_bytes)
{
    memset(p, 0, sizeof(*p));
    p->npages = (uint32_t)(ram_bytes >> HLE_PAGE_SHIFT);
    p->v = calloc(p->npages ? p->npages : 1, sizeof(uint64_t));
    return p->v != NULL;
}

void hle_pageuse_fini(HlePageUse *p)
{
    free(p->v);
    memset(p, 0, sizeof(*p));
}

void hle_pageuse_mark(HlePageUse *p, uint32_t pa, uint32_t len, uint64_t value)
{
    if (!len) {
        return;
    }
    uint64_t first = pa >> HLE_PAGE_SHIFT;
    uint64_t last = ((uint64_t)pa + len - 1) >> HLE_PAGE_SHIFT;
    for (uint64_t i = first; i <= last && i < p->npages; i++) {
        if (p->v[i] < value) {
            p->v[i] = value;
        }
    }
    if (p->max < value) {
        p->max = value;
    }
}

uint64_t hle_pageuse_get(const HlePageUse *p, uint32_t pa, uint32_t len)
{
    uint64_t m = 0;
    if (!len) {
        return 0;
    }
    uint64_t first = pa >> HLE_PAGE_SHIFT;
    uint64_t last = ((uint64_t)pa + len - 1) >> HLE_PAGE_SHIFT;
    for (uint64_t i = first; i <= last && i < p->npages; i++) {
        if (p->v[i] > m) {
            m = p->v[i];
        }
    }
    return m;
}
