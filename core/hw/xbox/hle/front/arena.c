/*
 * hakuX HLE GPU — upload arena allocator.
 */
#include "hw/xbox/hle/front/arena.h"

void hle_arena_init(HleArena *a, uint32_t size)
{
    memset(a, 0, sizeof(*a));
    a->size = size;
}

static HleArenaBlk *last_blk(HleArena *a)
{
    return &a->blk[(a->b0 + a->nb - 1) % HLE_ARENA_MAX_BLK];
}

uint32_t hle_arena_alloc(HleArena *a, uint32_t len, uint32_t align, uint64_t value)
{
    if (!len || len > a->size || !align) {
        return HLE_ARENA_FULL;
    }
    if (!a->nb) {
        a->head = 0;
        a->used = 0;
    }
    uint32_t tail = a->nb ? a->blk[a->b0].start : 0;
    uint32_t start = a->head;
    uint32_t off = (align & (align - 1)) ? (start + align - 1) / align * align
                                         : (start + align - 1) & ~(align - 1);
    bool wrapped = false;
    if (a->nb && start == tail) {
        return HLE_ARENA_FULL;                      /* head caught up: full */
    }
    if (!a->nb || start > tail) {                   /* free: [head,size)+[0,tail) */
        if (off + len > a->size) {
            if (len > tail) {
                return HLE_ARENA_FULL;
            }
            off = 0;
            wrapped = true;
        }
    } else if (off + len > tail) {                  /* free: [head,tail) */
        return HLE_ARENA_FULL;
    }
    uint32_t end = off + len;
    uint32_t bytes = wrapped ? (a->size - start) + end : end - start;
    HleArenaBlk *lb = a->nb ? last_blk(a) : NULL;
    if (lb && !wrapped && lb->value == value && lb->end == start) {
        lb->end = end;
        lb->bytes += bytes;
    } else {
        if (a->nb == HLE_ARENA_MAX_BLK) {
            return HLE_ARENA_FULL;
        }
        a->nb++;
        HleArenaBlk *b = last_blk(a);
        b->start = start;
        b->end = end;
        b->bytes = bytes;
        b->value = value;
    }
    a->used += bytes;
    a->head = end;
    return off;
}

void hle_arena_retire(HleArena *a, uint64_t completed)
{
    while (a->nb && a->blk[a->b0].value <= completed) {
        a->used -= a->blk[a->b0].bytes;
        a->b0 = (a->b0 + 1) % HLE_ARENA_MAX_BLK;
        a->nb--;
    }
    if (!a->nb) {
        a->head = 0;
        a->used = 0;
    }
}
