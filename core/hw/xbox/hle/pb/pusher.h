/*
 * hakuX HLE GPU — the NV2A's DMA pusher (push-buffer mode): reads command
 * words from the push buffer's DMA object between DMA_GET and DMA_PUT,
 * follows jumps, calls and returns as the GPU does, and hands the methods
 * to a sink (the class decoders). Pure: guest memory comes through a fetch
 * callback.
 *
 * Command words (a method packet's header, or a branch):
 *   001x xxxx xxxx xxxx xxxx xxxx xxxx xx00  old jump to bits 28:2
 *   xxxx xxxx xxxx xxxx xxxx xxxx xxxx xx01  jump to bits 31:2
 *   xxxx xxxx xxxx xxxx xxxx xxxx xxxx xx10  call bits 31:2 (one level)
 *   0000 0000 0000 0010 0000 0000 0000 0000  return
 *   000c cccc cccc cc00 sss m mmmm mmmm mm00  count c increasing methods m
 *   010c cccc cccc cc00 sss m mmmm mmmm mm00  count c methods, all to m
 * (s: the subchannel). The count words after a header are the methods'
 * data. Anything else is an invalid command: the pusher stops with an error,
 * as the GPU's DMA pusher raises its error interrupt.
 */
#ifndef HW_XBOX_HLE_PB_PUSHER_H
#define HW_XBOX_HLE_PB_PUSHER_H

#include "hw/xbox/hle/hle_pure.h"

enum {
    HLE_PB_OK = 0,
    HLE_PB_ERR_INVALID,         /* not a header nor a branch */
    HLE_PB_ERR_CALL,            /* a call inside a call */
    HLE_PB_ERR_RETURN,          /* a return outside a call */
    HLE_PB_ERR_LIMIT,           /* GET past the DMA object's limit */
    HLE_PB_ERR_FETCH,           /* the words aren't in guest RAM */
    HLE_PB_ERR_LOOP,            /* a run of branches that never reaches PUT, or more
                                   words than the run's budget */
};

/* What a sink returns for a method: go on, stop after it (a PGRAPH trap:
 * the pusher resumes on the next run, after the interrupt is handled), or
 * hold: the method isn't taken (PGRAPH's FIFO access is off) and comes
 * again on the next run. */
enum { HLE_PB_GO = 0, HLE_PB_STALL = 1, HLE_PB_HOLD = 2 };

struct HlePusher;

typedef struct HlePbSink {
    void *opaque;
    /* One method write: subchannel, method (byte offset), its data word. */
    int (*method)(void *opaque, unsigned subch, uint32_t method, uint32_t value);
    /* Optional: n (>= 1) data words of one packet at once, method being the
     * first word's (increasing: then +4 per word; noninc: all the same).
     * Returns how many it took (at least 1; the rest come again); NULL or
     * a method it doesn't batch (*stall untouched, returns 0): word by word.
     * Never holds: a sink that would returns 0, and method holds.
     * *stall as method's return, after the words taken. */
    uint32_t (*run)(void *opaque, unsigned subch, uint32_t method, bool noninc,
                    const uint32_t *w, uint32_t n, int *stall);
    /* Optional: before a CALL or RETURN is taken, the sink may update the
     * pusher's subroutine state (the guest wrote DMA_SUBROUTINE itself). */
    void (*branch)(void *opaque, struct HlePusher *p);
    /* Optional: the methods run batches, by method / 4 (2048): HLE_PB_RUN_INC
     * in an increasing packet, HLE_PB_RUN_NONINC in a non-increasing one.
     * Others go to method without a call to run. NULL: run sees every one. */
    const uint8_t *run_map;
} HlePbSink;

#define HLE_PB_RUN_INC    1u
#define HLE_PB_RUN_NONINC 2u

/* Guest words at physical address pa, n of them contiguous, or NULL. */
typedef const uint32_t *(*HlePbFetch)(void *opaque, uint32_t pa, uint32_t n);

typedef struct HlePusher {
    /* the push buffer's DMA object: GET, PUT and branch targets are offsets
     * into it */
    uint32_t dma_base, dma_limit;
    uint32_t get;
    /* one level of call */
    bool sub_active;
    uint32_t sub_return;
    /* the packet in progress (a packet's data can arrive over several kicks) */
    uint32_t method;
    uint32_t left;
    uint32_t count;                     /* the packet's words, as its header says */
    uint8_t subch;
    bool noninc;
    /* the first error, and where (GET of the bad word); sticky until reset */
    int error;
    uint32_t error_get, error_word;
    /* words one run may take before the stream counts as lost (a reader off
     * the guest's stream follows its old commands round forever); 0: no limit */
    uint32_t budget;
    /* counters */
    uint64_t words, headers, data, branches;
} HlePusher;

/* Empty, with the DMA object [base, base + limit]. */
void hle_pb_pusher_init(HlePusher *p, uint32_t dma_base, uint32_t dma_limit);

/* Runs from p->get until GET reaches put, a sink stalls, or an error.
 * Returns HLE_PB_OK (at put or stalled: p->get says which) or the error. */
int hle_pb_pusher_run(HlePusher *p, uint32_t put, HlePbFetch fetch, void *fetch_ctx,
                      const HlePbSink *sink);

const char *hle_pb_error_name(int e);

#endif
