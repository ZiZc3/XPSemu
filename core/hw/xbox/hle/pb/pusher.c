/*
 * hakuX HLE GPU — the NV2A's DMA pusher (pusher.h).
 */
#include "hw/xbox/hle/pb/pusher.h"

/* Words fetched at once for a packet's data. */
#define FETCH_MAX 1024u
/* Branches in a row without a method before the stream counts as lost. */
#define BRANCH_MAX 65536u

void hle_pb_pusher_init(HlePusher *p, uint32_t dma_base, uint32_t dma_limit)
{
    memset(p, 0, sizeof(*p));
    p->dma_base = dma_base;
    p->dma_limit = dma_limit;
}

const char *hle_pb_error_name(int e)
{
    switch (e) {
    case HLE_PB_OK:          return "ok";
    case HLE_PB_ERR_INVALID: return "invalid command";
    case HLE_PB_ERR_CALL:    return "call inside a call";
    case HLE_PB_ERR_RETURN:  return "return outside a call";
    case HLE_PB_ERR_LIMIT:   return "past the DMA object's limit";
    case HLE_PB_ERR_FETCH:   return "not in guest RAM";
    case HLE_PB_ERR_LOOP:    return "branches that never reach PUT";
    default:                 return "?";
    }
}

static int fail(HlePusher *p, int e, uint32_t word)
{
    if (!p->error) {
        p->error = e;
        p->error_get = p->get;
        p->error_word = word;
    }
    return e;
}

/* How many words from GET the object holds (0: GET is past its limit). */
static uint32_t room(const HlePusher *p)
{
    return p->get <= p->dma_limit ? (p->dma_limit - p->get) / 4 + 1 : 0;
}

/* The packet's data from GET, fetched once: each word to the sink (as much
 * as it takes at once, else one), GET and the packet's state kept exact
 * after each, until the sink stalls or holds or the fetch ends. Returns the
 * sink's stall. */
static int data(HlePusher *p, uint32_t put, HlePbFetch fetch, void *ctx, const HlePbSink *s)
{
    uint32_t n = p->left;
    if (put > p->get) {
        n = MIN(n, (put - p->get) / 4);
    }
    n = MIN(MIN(n, FETCH_MAX), room(p));
    if (!n) {
        fail(p, HLE_PB_ERR_LIMIT, 0);
        return HLE_PB_STALL;
    }
    const uint32_t *w = fetch(ctx, p->dma_base + p->get, n);
    if (!w) {
        fail(p, HLE_PB_ERR_FETCH, 0);
        return HLE_PB_STALL;
    }
    int stall = HLE_PB_GO;
    const uint8_t run_bit = p->noninc ? HLE_PB_RUN_NONINC : HLE_PB_RUN_INC;
    for (uint32_t i = 0; i < n && stall == HLE_PB_GO;) {
        uint32_t k = 0;
        if (s->run && (!s->run_map || (s->run_map[(p->method >> 2) & 0x7FF] & run_bit))) {
            k = MIN(s->run(s->opaque, p->subch, p->method, p->noninc, w + i, n - i, &stall), n - i);
        }
        if (!k) {
            stall = s->method(s->opaque, p->subch, p->method, w[i]);
            if (stall == HLE_PB_HOLD) {
                return HLE_PB_STALL;                /* not taken: GET stays */
            }
            k = 1;
        }
        p->get += 4 * k;
        p->left -= k;
        p->data += k;
        p->words += k;
        if (!p->noninc) {
            p->method += 4 * k;
        }
        i += k;
    }
    return stall;
}

int hle_pb_pusher_run(HlePusher *p, uint32_t put, HlePbFetch fetch, void *fetch_ctx,
                      const HlePbSink *sink)
{
    if (p->error) {
        return p->error;
    }
    unsigned branches = 0;
    const uint64_t words0 = p->words;
    while (p->get != put) {
        if (p->budget && p->words - words0 > p->budget) {
            return fail(p, HLE_PB_ERR_LOOP, 0);
        }
        if (p->left) {
            branches = 0;
            if (data(p, put, fetch, fetch_ctx, sink) == HLE_PB_STALL) {
                return p->error;
            }
            continue;
        }
        if (!room(p)) {
            return fail(p, HLE_PB_ERR_LIMIT, 0);
        }
        const uint32_t *wp = fetch(fetch_ctx, p->dma_base + p->get, 1);
        if (!wp) {
            return fail(p, HLE_PB_ERR_FETCH, 0);
        }
        const uint32_t w = *wp;
        if ((w & 0xE0000003u) == 0x20000000u || (w & 3) == 1 || (w & 3) == 2 ||
            w == 0x00020000u) {
            if (++branches > BRANCH_MAX) {
                return fail(p, HLE_PB_ERR_LOOP, w);
            }
            p->branches++;
            p->words++;
            if ((w & 0xE0000003u) == 0x20000000u) {        /* old jump */
                p->get = w & 0x1FFFFFFCu;
            } else if ((w & 3) == 1) {                      /* jump */
                p->get = w & ~3u;
            } else if ((w & 3) == 2) {                      /* call */
                if (sink->branch) {
                    sink->branch(sink->opaque, p);
                }
                if (p->sub_active) {
                    return fail(p, HLE_PB_ERR_CALL, w);
                }
                p->sub_active = true;
                p->sub_return = p->get + 4;
                p->get = w & ~3u;
            } else {                                        /* return */
                if (sink->branch) {
                    sink->branch(sink->opaque, p);
                }
                if (!p->sub_active) {
                    return fail(p, HLE_PB_ERR_RETURN, w);
                }
                p->sub_active = false;
                p->get = p->sub_return;
            }
            continue;
        }
        const uint32_t kind = w & 0xE0030003u;
        if (kind != 0 && kind != 0x40000000u) {
            return fail(p, HLE_PB_ERR_INVALID, w);
        }
        branches = 0;
        p->get += 4;
        p->words++;
        p->headers++;
        p->method = w & 0x1FFCu;
        p->subch = (uint8_t)((w >> 13) & 7);
        p->left = (w >> 18) & 0x7FFu;
        p->count = p->left;
        p->noninc = kind != 0;
    }
    return HLE_PB_OK;
}
