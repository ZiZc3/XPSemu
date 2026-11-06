/*
 * hakuX HLE GPU — upload arena allocator (§5.3): a ring of allocations,
 * each tagged with the timeline value of the submission that reads it,
 * reclaimed when the GPU passes that value. Consumers arrive in P3.
 */
#ifndef HW_XBOX_HLE_FRONT_ARENA_H
#define HW_XBOX_HLE_FRONT_ARENA_H

#include "hw/xbox/hle/hle_pure.h"

#define HLE_ARENA_FULL    0xFFFFFFFFu
#define HLE_ARENA_MAX_BLK 1024

typedef struct HleArenaBlk {
    uint32_t start, end;        /* end < start when the block wrapped */
    uint32_t bytes;             /* including alignment and wrap waste */
    uint64_t value;
} HleArenaBlk;

typedef struct HleArena {
    uint32_t size, head, used;
    HleArenaBlk blk[HLE_ARENA_MAX_BLK];
    unsigned b0, nb;            /* FIFO of live blocks */
} HleArena;

void hle_arena_init(HleArena *a, uint32_t size);
uint32_t hle_arena_alloc(HleArena *a, uint32_t len, uint32_t align, uint64_t value);
void hle_arena_retire(HleArena *a, uint64_t completed);

static inline uint32_t hle_arena_used(const HleArena *a)
{
    return a->used;
}

/* The oldest live block's value (the first the GPU's progress frees), 0 if
 * none. */
static inline uint64_t hle_arena_oldest(const HleArena *a)
{
    return a->nb ? a->blk[a->b0].value : 0;
}

#endif
