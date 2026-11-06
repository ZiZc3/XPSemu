/*
 * hakuX HLE GPU — single-producer single-consumer command ring (§5.3).
 * Producer: vCPU thread. Consumer: HLE render thread.
 */
#ifndef HW_XBOX_HLE_FRONT_RING_H
#define HW_XBOX_HLE_FRONT_RING_H

#include "hw/xbox/hle/front/cmd.h"

#define HLE_RING_HDR ((uint32_t)sizeof(HleCmdHdr))

/* head, tail and the producer's own words on separate cache lines: each is
 * written by one side only, so a write does not evict what the other side
 * keeps reading. */
typedef struct HleRing {
    uint8_t *buf;
    uint32_t size, mask;
    _Alignas(64) uint32_t head;     /* published by the producer (release) */
    _Alignas(64) uint32_t tail;     /* published by the consumer (release) */
    _Alignas(64) uint32_t wpos;     /* producer-private reservation end */
    uint32_t tail_seen;             /* producer-private: tail as last read */
} HleRing;

/* size: power of two >= 4096; buf 8-byte aligned. */
int hle_ring_init(HleRing *r, uint8_t *buf, uint32_t size);
/* Payload pointer, or NULL when the ring is full (len <= size / 2). */
void *hle_ring_reserve(HleRing *r, uint16_t op, uint32_t len);
void hle_ring_publish(HleRing *r);
const HleCmdHdr *hle_ring_peek(HleRing *r);
void hle_ring_consume(HleRing *r);
uint32_t hle_ring_used(HleRing *r);

#endif
