/*
 * hakuX HLE GPU — host-time profile (HAKUX_HLE_PROF=1, debug): nanoseconds
 * per bucket, logged once a second (hle_prof_diag). Off: one branch.
 */
#ifndef HW_XBOX_HLE_PROF_H
#define HW_XBOX_HLE_PROF_H

#include <stdbool.h>
#include <stdint.h>

enum {
    HLE_PROF_BACK,      /* back-end command processing (render thread) */
    HLE_PROF_SUBMIT,    /* back-end queue submits */
    HLE_PROF_N
};

extern bool g_hle_prof_on;
/* accumulated time in counter ticks (g_hle_prof_ns_per_tick converts) */
extern uint64_t g_hle_prof_ns[HLE_PROF_N], g_hle_prof_cnt[HLE_PROF_N];
extern double g_hle_prof_ns_per_tick;

/* The ARM virtual counter: a register read. clock_gettime through the vDSO,
 * at ~1-2 million spans a second, cost the vCPU several percent and made a
 * capture slow the scene it measured. */
static inline int64_t hle_prof_now(void)
{
#if defined(__aarch64__)
    uint64_t t;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    return (int64_t)t;
#else
    return hle_now_ns();
#endif
}

static inline int64_t hle_prof_begin(void)
{
    return g_hle_prof_on ? hle_prof_now() : 0;
}

static inline void hle_prof_end(int k, int64_t t0)
{
    if (g_hle_prof_on && t0) {          /* t0 0: the profile came on mid-span */
        g_hle_prof_ns[k] += hle_prof_now() - t0;
        g_hle_prof_cnt[k]++;
    }
}

void hle_prof_init(void);
void hle_prof_diag(void);

#endif
