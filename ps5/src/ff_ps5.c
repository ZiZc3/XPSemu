/*
 * XOG patch 253: fast-forward. The guest's sense of time runs x2 while it is on.
 *
 * One virtual clock for the whole core: nc_ff_now_ns() (host CLOCK_MONOTONIC ns) and nc_ff_tsc() (the host TSC)
 * run at the host's rate while FF is off and at twice it while on. Each switch anchors both at the moment it is
 * made (v = v0 + (raw - r0) * mul), so neither ever jumps or goes back. Off and never used, they equal the host's.
 * Everything the guest reads as time is made from them: the kernel's time (ke.c raw_ns: KeTickCount,
 * KeInterruptTime, KeSystemTime, KeQueryPerformanceCounter, the timers, the clock interrupt, the PTIMER, the ACPI
 * timer, the APU's sample counter) and rdtsc (cpu_ps5.c); nc_ff_host_ns() turns a virtual deadline back into the
 * host's time for the waits. The vblank, the APU's pacing, the drive's read time, the FPS cap and the display's
 * slots read nc_ff_mul().
 *
 * Our own code. GPL-3.0-or-later.
 */
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

void nc_log(const char *tag, const char *fmt, ...);
uint64_t nc_ke_now_ns(void);
uint64_t nc_dev_nv2a_vblanks(void);
void nc_audio_set_ff(bool on);
void xog_qm_ff_changed(void);

volatile int g_xog_ff;              /* on now (the quick menu's overlay draws the badge) */
static volatile int g_ff_allowed;   /* ff=1 in the start lines, not a Chihiro run */

static uint32_t g_seq;                          /* odd while a switch writes */
static uint64_t g_r0, g_v0, g_t0, g_tv0;        /* host ns / virtual ns, host TSC / virtual TSC at the switch */
static uint32_t g_mul = 1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static inline uint64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t host_tsc(void)
{
    uint32_t a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}

static inline uint64_t scale(uint64_t raw, uint64_t r0, uint64_t v0, uint32_t mul)
{
    return raw >= r0 ? v0 + (raw - r0) * mul : v0 - (r0 - raw);
}

unsigned nc_ff_mul(void)
{
    return __atomic_load_n(&g_mul, __ATOMIC_RELAXED);
}

uint64_t nc_ff_now_ns(void)
{
    for (;;) {
        const uint32_t s = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
        if (s & 1) {
            continue;
        }
        const uint64_t r0 = __atomic_load_n(&g_r0, __ATOMIC_RELAXED), v0 = __atomic_load_n(&g_v0, __ATOMIC_RELAXED);
        const uint32_t mul = __atomic_load_n(&g_mul, __ATOMIC_RELAXED);
        const uint64_t raw = host_ns();
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_seq, __ATOMIC_RELAXED) == s) {
            return scale(raw, r0, v0, mul);
        }
    }
}

uint64_t nc_ff_tsc(void)
{
    for (;;) {
        const uint32_t s = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
        if (s & 1) {
            continue;
        }
        const uint64_t t0 = __atomic_load_n(&g_t0, __ATOMIC_RELAXED), tv0 = __atomic_load_n(&g_tv0, __ATOMIC_RELAXED);
        const uint32_t mul = __atomic_load_n(&g_mul, __ATOMIC_RELAXED);
        const uint64_t raw = host_tsc();
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_seq, __ATOMIC_RELAXED) == s) {
            return scale(raw, t0, tv0, mul);
        }
    }
}

/* A virtual time (nc_ff_now_ns's) on the host's CLOCK_MONOTONIC: when a wait for it should end. One already past
 * the last switch comes back as at least that far in the past (due now). */
uint64_t nc_ff_host_ns(uint64_t v)
{
    for (;;) {
        const uint32_t s = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
        if (s & 1) {
            continue;
        }
        const uint64_t r0 = __atomic_load_n(&g_r0, __ATOMIC_RELAXED), v0 = __atomic_load_n(&g_v0, __ATOMIC_RELAXED);
        const uint32_t mul = __atomic_load_n(&g_mul, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_seq, __ATOMIC_RELAXED) == s) {
            return v >= v0 ? r0 + (v - v0) / mul : r0 - (v0 - v);
        }
    }
}

void nc_ff_allow(bool on)
{
    g_ff_allowed = on;
    nc_log("xog-ff", "fast-forward %s for this run (the quick menu's Settings)", on ? "allowed" : "not allowed");
}

bool nc_ff_allowed(void)
{
    return g_ff_allowed != 0;
}

static uint64_t g_meas_host, g_meas_guest, g_meas_vbl;

/* On or off, from any thread; why: for the log. */
void nc_ff_set(bool on, const char *why)
{
    pthread_mutex_lock(&g_mu);
    if (on && !g_ff_allowed) {      /* patch 255: Settings > Advanced on (ff=1), never Chihiro */
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if ((g_xog_ff != 0) == on) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    __atomic_fetch_add(&g_seq, 1, __ATOMIC_ACQ_REL);
    const uint64_t raw = host_ns(), tsc = host_tsc();
    const uint64_t v = scale(raw, g_r0, g_v0, g_mul), tv = scale(tsc, g_t0, g_tv0, g_mul);
    __atomic_store_n(&g_r0, raw, __ATOMIC_RELAXED);
    __atomic_store_n(&g_v0, v, __ATOMIC_RELAXED);
    __atomic_store_n(&g_t0, tsc, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tv0, tv, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mul, on ? 2u : 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_seq, 1, __ATOMIC_RELEASE);
    g_xog_ff = on;
    nc_audio_set_ff(on);
    pthread_mutex_unlock(&g_mu);
    if (on) {
        g_meas_host = raw;
        g_meas_guest = nc_ke_now_ns();
        g_meas_vbl = nc_dev_nv2a_vblanks();
    }
    nc_log("xog-ff", "fast-forward %s (%s); clock now %+.3f s ahead of the host's", on ? "ON x2" : "off", why,
           (double)(int64_t)(v - raw) / 1e9);
    xog_qm_ff_changed();
}

void nc_ff_off(const char *why)
{
    if (g_xog_ff) {
        nc_ff_set(false, why);
    }
}

/* The controller's thread, each turn: the guest's measured speed once a second while on. */
void nc_ff_poll(void)
{
    if (!g_xog_ff) {
        return;
    }
    const uint64_t now = host_ns();
    if (now - g_meas_host < 1000000000ull) {
        return;
    }
    const uint64_t guest = nc_ke_now_ns(), vbl = nc_dev_nv2a_vblanks();
    const double secs = (double)(now - g_meas_host) / 1e9;
    nc_log("xog-ff", "guest %.2fx (guest time %.2f s in %.2f s; vblanks %.0f/s)", (double)(guest - g_meas_guest) / 1e9 / secs,
           (double)(guest - g_meas_guest) / 1e9, secs, (double)(vbl - g_meas_vbl) / secs);
    g_meas_host = now;
    g_meas_guest = guest;
    g_meas_vbl = vbl;
}
