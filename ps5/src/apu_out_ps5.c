/*
 * XOG: the APU's output on the PS5's AudioOut, in place of hakuX's AAudio file.
 * The frame thread pushes into a ring; a thread here feeds the console 256 stereo
 * samples at a time, sceAudioOutOutput blocking until they are taken.
 * Our own code. GPL-3.0-or-later.
 */
#include "native/dev/apu/apu_int.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void *buf);

#define RX_USER_SYSTEM 255
#define RX_PORT_MAIN 0
#define RX_FORMAT_S16_STEREO 1
#define RX_SAMPLES 256
#define RX_BYTES (RX_SAMPLES * 4)

static struct {
    uint8_t *buf;
    uint32_t cap;
    uint64_t head, tail;
    pthread_cond_t *wake;
    int handle;
    volatile bool paused;
    volatile bool ff;       /* patch 253: fast-forward: two blocks taken a block's time, silence played */
    uint32_t callbacks, short_callbacks;
} g_out;

int apu_out_queued(void)
{
    return (int)(__atomic_load_n(&g_out.head, __ATOMIC_ACQUIRE) - __atomic_load_n(&g_out.tail, __ATOMIC_ACQUIRE));
}

void apu_out_push(const void *frame, int bytes)
{
    const uint64_t head = g_out.head;
    const uint32_t at = (uint32_t)(head % g_out.cap);
    const uint32_t first = (uint32_t)bytes < g_out.cap - at ? (uint32_t)bytes : g_out.cap - at;
    memcpy(g_out.buf + at, frame, first);
    memcpy(g_out.buf, (const uint8_t *)frame + first, (uint32_t)bytes - first);
    __atomic_store_n(&g_out.head, head + (uint64_t)bytes, __ATOMIC_RELEASE);
}

void apu_out_stats(uint32_t *callbacks, uint32_t *short_callbacks)
{
    *callbacks = __atomic_exchange_n(&g_out.callbacks, 0, __ATOMIC_RELAXED);
    *short_callbacks = __atomic_exchange_n(&g_out.short_callbacks, 0, __ATOMIC_RELAXED);
}

static int g_volume = 10;   /* tens of percent */

void rx_audio_volume(int tens)
{
    g_volume = tens < 0 ? 0 : tens > 10 ? 10 : tens;
}

static void *out_thread(void *arg)
{
    (void)arg;
    static uint8_t block[RX_BYTES] __attribute__((aligned(64)));
    for (;;) {
        int got = 0;
        if (!g_out.paused) {
            const uint64_t tail = g_out.tail;
            const uint64_t have = __atomic_load_n(&g_out.head, __ATOMIC_ACQUIRE) - tail;
            got = have < RX_BYTES ? (int)have : RX_BYTES;
            const uint32_t at = (uint32_t)(tail % g_out.cap);
            const uint32_t first = (uint32_t)got < g_out.cap - at ? (uint32_t)got : g_out.cap - at;
            memcpy(block, g_out.buf + at, first);
            memcpy(block + first, g_out.buf, (uint32_t)got - first);
            __atomic_store_n(&g_out.tail, tail + (uint64_t)got, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_out.callbacks, 1, __ATOMIC_RELAXED);
            if (got < RX_BYTES) {
                __atomic_add_fetch(&g_out.short_callbacks, 1, __ATOMIC_RELAXED);
            }
            if (g_out.ff) {
                /* patch 253: the APU makes its frames twice as fast: a second block taken too, both muted */
                const uint64_t t2 = g_out.tail;
                const uint64_t more = __atomic_load_n(&g_out.head, __ATOMIC_ACQUIRE) - t2;
                __atomic_store_n(&g_out.tail, t2 + (more < RX_BYTES ? more : RX_BYTES), __ATOMIC_RELEASE);
                got = 0;
            }
        }
        memset(block + got, 0, (size_t)(RX_BYTES - got));
        if (g_out.wake) {
            pthread_cond_signal(g_out.wake);
        }
        if (g_volume < 10) {   /* RX_VOL_APPLY */
            int16_t *v = (int16_t *)block;
            for (int i = 0; i < RX_SAMPLES * 2; i++) {
                v[i] = (int16_t)(v[i] * g_volume / 10);
            }
        }
        sceAudioOutOutput(g_out.handle, block);
    }
    return NULL;
}

int apu_out_open(int capacity_bytes, int samples, pthread_cond_t *wake)
{
    (void)samples;
    g_out.cap = (uint32_t)capacity_bytes;
    g_out.buf = calloc(1, g_out.cap);
    g_out.wake = wake;
    if (!g_out.buf) {
        return -1;
    }
    const int rc = sceAudioOutInit();
    g_out.handle = sceAudioOutOpen(RX_USER_SYSTEM, RX_PORT_MAIN, 0, RX_SAMPLES, 48000, RX_FORMAT_S16_STEREO);
    nc_log("xog-audio", "AudioOut: init %08x, port %08x", (unsigned)rc, (unsigned)g_out.handle);
    if (g_out.handle < 0) {
        return -1;
    }
    pthread_t t;
    pthread_create(&t, NULL, out_thread, NULL);
    pthread_detach(t);
    return samples * 4; /* what apu.c drains by */
}

void nc_audio_set_paused(bool on)
{
    g_out.paused = on;
}

/* patch 253: fast-forward: the game's sound muted (the quick menu's pause stops the output; this drains it at
 * the APU's x2 pace instead, so the APU isn't held to real time) */
void nc_audio_set_ff(bool on)
{
    g_out.ff = on;
}
