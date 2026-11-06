/*
 * hakuX native core — the MCPX APU's platform: the C library, atomics, locks,
 * threads, clocks and guest RAM, on the core's own services (no QEMU layer).
 * The APU's device code (apu.c, vp/, dsp/) came from xemu; this is what it
 * runs on here.
 */
#ifndef NATIVE_DEV_APU_BASE_H
#define NATIVE_DEV_APU_BASE_H

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static inline int ctz32(uint32_t v)
{
    return v ? __builtin_ctz(v) : 32;
}

static inline int ctz64(uint64_t v)
{
    return v ? __builtin_ctzll(v) : 64;
}

static inline int clz32(uint32_t v)
{
    return v ? __builtin_clz(v) : 32;
}

/* ---- atomics: plain loads / stores relaxed, read-modify-writes ordered */
#define apu_ld(p) __atomic_load_n(p, __ATOMIC_RELAXED)
#define apu_ld_acq(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define apu_st(p, v) __atomic_store_n(p, v, __ATOMIC_RELAXED)
#define apu_st_rel(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define apu_or(p, v) ((void)__atomic_fetch_or(p, v, __ATOMIC_SEQ_CST))
#define apu_and(p, v) ((void)__atomic_fetch_and(p, v, __ATOMIC_SEQ_CST))
#define apu_inc(p) ((void)__atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST))
#define apu_xchg(p, v) __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)

/* ---- a spinlock (short sections: a voice's state, the FE against the VP).
 * A held lock is waited for with WFE: the core sleeps until the holder's
 * release store clears its exclusive monitor (or the kernel's event stream
 * ticks). The voice locks are held while a worker processes the voice, and
 * the guest thread waits on them from DirectSound's register writes. */
typedef struct ApuSpin {
    unsigned v;
} ApuSpin;

static inline void apu_spin_init(ApuSpin *s)
{
    s->v = 0;
}

/* true when taken */
static inline bool apu_spin_trylock(ApuSpin *s)
{
    return !__atomic_exchange_n(&s->v, 1, __ATOMIC_ACQUIRE);
}

static inline void apu_spin_lock(ApuSpin *s)
{
    while (__atomic_exchange_n(&s->v, 1, __ATOMIC_ACQUIRE)) {
#if defined(__aarch64__)
        unsigned v;
        __asm__ __volatile__("sevl" ::: "memory");
        do {
            __asm__ __volatile__("wfe\n\tldaxr %w0, [%1]" : "=&r"(v) : "r"(&s->v) : "memory");
        } while (v);
#else
        while (__atomic_load_n(&s->v, __ATOMIC_RELAXED)) {
        }
#endif
    }
}

static inline void apu_spin_unlock(ApuSpin *s)
{
    __atomic_store_n(&s->v, 0, __ATOMIC_RELEASE);
}

/* ---- mutexes, condition variables (the monotonic clock), threads */
void apu_cond_init(pthread_cond_t *c);
/* false on timeout */
bool apu_cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, int ms);
bool apu_cond_timedwait_us(pthread_cond_t *c, pthread_mutex_t *m, int64_t us);
/* name: at most 15 characters; joinable or detached */
void apu_thread_start(pthread_t *t, const char *name, void *(*fn)(void *), void *arg, bool joinable);

/* ---- clocks: host time (pacing the output device), guest time (registers) */
static inline int64_t apu_host_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static inline int64_t apu_host_ms(void)
{
    return apu_host_us() / 1000;
}

uint64_t nc_ke_now_ns(void);   /* native/kernel/ke.c: the guest's time */

/* ---- guest RAM: the host's view of it, never protected; the APU's writes
 * are reported to the HLE's write tracking (host.h hle_host_mem_set_dirty) */
extern uint8_t *g_apu_ram;
extern uint32_t g_apu_ram_size;
void hle_host_mem_set_dirty(uint32_t pa, uint32_t len);

static inline uint8_t *apu_ram_ptr(uint64_t pa, uint64_t len)
{
    return pa + len <= g_apu_ram_size ? g_apu_ram + pa : NULL;
}

static inline uint32_t apu_ram_ld32(uint64_t pa)
{
    uint32_t v = 0;
    const uint8_t *p = apu_ram_ptr(pa, 4);
    if (p) {
        memcpy(&v, p, 4);
    }
    return v;
}

static inline void apu_ram_st32(uint64_t pa, uint32_t v)
{
    uint8_t *p = apu_ram_ptr(pa, 4);
    if (p) {
        memcpy(p, &v, 4);
        hle_host_mem_set_dirty((uint32_t)pa, 4);
    }
}

static inline void apu_ram_st8(uint64_t pa, uint32_t v)
{
    uint8_t *p = apu_ram_ptr(pa, 1);
    if (p) {
        *p = (uint8_t)v;
        hle_host_mem_set_dirty((uint32_t)pa, 1);
    }
}

/* A guest table the VP reads every frame (the voices, the SSLs): a pointer
 * into guest RAM, checked against its size when the guest sets the base. RAM
 * is one mapping for the whole run, so a worker holding an old pointer reads
 * valid memory (the table it started the frame with). */
static inline uint32_t apu_mem_ld32(const uint8_t *t, uint32_t off)
{
    uint32_t v;
    memcpy(&v, t + off, 4);
    return v;
}

static inline void apu_mem_st32(uint8_t *t, uint32_t off, uint32_t v)
{
    memcpy(t + off, &v, 4);
    hle_host_mem_set_dirty((uint32_t)(t - g_apu_ram) + off, 4);
}

/* little-endian loads and stores at host addresses (the host is too) */
static inline uint32_t apu_rd16(const void *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static inline uint32_t apu_rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void apu_wr32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

/* the SGE tables' pages */
#define APU_PAGE_SIZE 4096

/* ---- the app's audio settings (defaults; NC_APU_* environment overrides) */
typedef struct ApuConfig {
    bool use_dsp;           /* GP / EP frames run to completion (else 1000 DSP cycles) */
    bool hrtf;              /* the 3D voices' HRTF filters */
    float volume_limit;     /* output scale, 1: none */
    int num_workers;        /* VP voice workers, 0: by the CPU count */
} ApuConfig;
extern ApuConfig g_apu_config;

#include "native/core/irq.h"   /* nc_irq_set, nc_paused, nc_pause_register */
#include "native/core/nc.h"    /* nc_log */

/* A value from the guest (a register, a method's argument, its tables, its
 * DSP code) that the device indexes host memory with. xemu asserted these
 * and refused builds without assertions; the core is built with NDEBUG, so
 * they are checked here: false drops the access, logged once per site. */
#define APU_CHECK(cond)                                                       \
    __extension__({                                                           \
        const bool apu_ok_ = (cond);                                          \
        if (unlikely(!apu_ok_)) {                                             \
            static bool apu_said_;                                            \
            if (!__atomic_exchange_n(&apu_said_, true, __ATOMIC_RELAXED)) {   \
                nc_log("hakuX-apu", "%s: a guest value fails %s; dropped",    \
                       __func__, #cond);                                      \
            }                                                                 \
        }                                                                     \
        apu_ok_;                                                              \
    })

#endif
