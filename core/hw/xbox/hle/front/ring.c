/*
 * hakuX HLE GPU — SPSC command ring.
 */
#include "hw/xbox/hle/front/ring.h"

static inline uint32_t rec_bytes(uint32_t len)
{
    return (HLE_RING_HDR + len + 7u) & ~7u;
}

int hle_ring_init(HleRing *r, uint8_t *buf, uint32_t size)
{
    if (size < 4096 || (size & (size - 1)) || ((uintptr_t)buf & 7)) {
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->size = size;
    r->mask = size - 1;
    return 0;
}

void *hle_ring_reserve(HleRing *r, uint16_t op, uint32_t len)
{
    /* Check len first: rec_bytes() wraps for len near 4 GiB. */
    if (len > r->size / 2) {
        return NULL;
    }
    uint32_t rec = rec_bytes(len);
    if (rec > r->size / 2) {
        return NULL;
    }
    uint32_t pos = r->wpos, off = pos & r->mask;
    uint32_t pad = off + rec > r->size ? r->size - off : 0;
    /* the consumer's tail only when the space seen last time is too small
     * (reading it every command pulled its cache line across cores) */
    if (r->size - (pos - r->tail_seen) < pad + rec) {
        r->tail_seen = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
        if (r->size - (pos - r->tail_seen) < pad + rec) {
            return NULL;
        }
    }
    if (pad) {
        HleCmdHdr *ph = (HleCmdHdr *)(r->buf + off);
        ph->op = HLE_OP_PAD;
        ph->rsvd = 0;
        ph->len = pad - HLE_RING_HDR;
        pos += pad;
        off = 0;
    }
    HleCmdHdr *h = (HleCmdHdr *)(r->buf + off);
    h->op = op;
    h->rsvd = 0;
    h->len = len;
    r->wpos = pos + rec;
    return r->buf + off + HLE_RING_HDR;
}

void hle_ring_publish(HleRing *r)
{
    __atomic_store_n(&r->head, r->wpos, __ATOMIC_RELEASE);
}

const HleCmdHdr *hle_ring_peek(HleRing *r)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    while (tail != head) {
        const HleCmdHdr *h = (const HleCmdHdr *)(r->buf + (tail & r->mask));
        if (h->op != HLE_OP_PAD) {
            return h;
        }
        tail += rec_bytes(h->len);
        __atomic_store_n(&r->tail, tail, __ATOMIC_RELEASE);
    }
    return NULL;
}

void hle_ring_consume(HleRing *r)
{
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    const HleCmdHdr *h = (const HleCmdHdr *)(r->buf + (tail & r->mask));
    __atomic_store_n(&r->tail, tail + rec_bytes(h->len), __ATOMIC_RELEASE);
}

uint32_t hle_ring_used(HleRing *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) -
           __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}
