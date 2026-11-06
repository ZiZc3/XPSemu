#include "hw/xbox/hle/front/ring.h"
#include "hle_test.h"

static uint64_t mem[4096 / 8];

static void test_basic(void)
{
    HleRing r;
    HT_EQ_INT(hle_ring_init(&r, (uint8_t *)mem, 4096), 0);
    HleCmdFence *f = hle_ring_reserve(&r, HLE_OP_KICK, sizeof(*f));
    HT_CHECK(f != NULL);
    f->value = 0x1122334455667788ull;
    HT_CHECK(hle_ring_peek(&r) == NULL);            /* not published yet */
    hle_ring_publish(&r);
    const HleCmdHdr *h = hle_ring_peek(&r);
    HT_CHECK(h != NULL);
    HT_EQ_INT(h->op, HLE_OP_KICK);
    HT_EQ_INT(h->len, sizeof(HleCmdFence));
    HT_CHECK(((const HleCmdFence *)hle_cmd_payload(h))->value == 0x1122334455667788ull);
    hle_ring_consume(&r);
    HT_CHECK(hle_ring_peek(&r) == NULL);
    HT_EQ_INT(hle_ring_used(&r), 0);
}

static void test_wrap_and_full(void)
{
    HleRing r;
    hle_ring_init(&r, (uint8_t *)mem, 4096);
    for (uint32_t i = 0; i < 4; i++) {                 /* 4 x 1008-byte records */
        uint32_t *p = hle_ring_reserve(&r, HLE_OP_KICK, 1000);
        HT_CHECK(p != NULL);
        p[0] = i;
    }
    HT_CHECK(hle_ring_reserve(&r, HLE_OP_KICK, 1000) == NULL);   /* full */
    hle_ring_publish(&r);
    for (uint32_t i = 0; i < 2; i++) {
        const HleCmdHdr *h = hle_ring_peek(&r);
        HT_EQ_U32(((const uint32_t *)hle_cmd_payload(h))[0], i);
        hle_ring_consume(&r);
    }
    uint32_t *p = hle_ring_reserve(&r, HLE_OP_KICK, 1000);      /* pads 64 B, wraps */
    HT_CHECK(p == (uint32_t *)((uint8_t *)mem + HLE_RING_HDR));
    p[0] = 4;
    hle_ring_publish(&r);
    uint32_t seen[3], n = 0;
    const HleCmdHdr *h;
    while ((h = hle_ring_peek(&r)) != NULL) {
        HT_EQ_INT(h->op, HLE_OP_KICK);                 /* the pad is never returned */
        seen[n++] = ((const uint32_t *)hle_cmd_payload(h))[0];
        hle_ring_consume(&r);
    }
    HT_EQ_INT(n, 3);
    HT_EQ_U32(seen[0], 2);
    HT_EQ_U32(seen[1], 3);
    HT_EQ_U32(seen[2], 4);
    HT_CHECK(hle_ring_reserve(&r, HLE_OP_KICK, 3000) == NULL);   /* > half ring */
}

int main(void)
{
    HT_RUN(test_basic);
    HT_RUN(test_wrap_and_full);
    HT_DONE();
}
