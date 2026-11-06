#include "hw/xbox/hle/front/arena.h"
#include "hle_test.h"

static HleArena a;

static void test_alloc_wrap_retire(void)
{
    hle_arena_init(&a, 1024);
    HT_EQ_U32(hle_arena_alloc(&a, 100, 16, 1), 0);
    HT_EQ_U32(hle_arena_alloc(&a, 100, 16, 1), 112);        /* merged block [0,212) */
    HT_EQ_U32(hle_arena_alloc(&a, 500, 16, 2), 224);        /* block [212,724) */
    HT_EQ_U32(hle_arena_alloc(&a, 400, 16, 3), HLE_ARENA_FULL);
    hle_arena_retire(&a, 1);
    HT_EQ_U32(hle_arena_alloc(&a, 200, 16, 3), 736);        /* [724,936) */
    HT_EQ_U32(hle_arena_alloc(&a, 200, 16, 3), 0);          /* wraps; tail is 212 */
    HT_EQ_U32(hle_arena_alloc(&a, 100, 16, 4), HLE_ARENA_FULL);
    HT_EQ_U32(hle_arena_used(&a), 512 + 212 + 288);
    hle_arena_retire(&a, 3);
    HT_EQ_U32(hle_arena_used(&a), 0);
    HT_EQ_U32(hle_arena_alloc(&a, 1024, 16, 5), 0);         /* empty -> rewinds */
}

int main(void)
{
    HT_RUN(test_alloc_wrap_retire);
    HT_DONE();
}
