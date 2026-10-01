/*
 * XPSemu: stopwatches on the emulator's costly steps, for the game log
 * (ui/xui/game-log.cc): total nanoseconds and how many times each, since
 * start. Atomic adds, so any thread can time.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef HW_XBOX_XEMU_TIMING_H
#define HW_XBOX_XEMU_TIMING_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XT_FINISH_SUBMIT, /* GPU thread: pgraph_vk_finish's vkQueueSubmit */
    XT_FINISH_WAIT,   /* ...and its wait for the PS5 GPU */
    XT_AUX_WAIT,      /* One-off GPU jobs (downloads, uploads): wait idle */
    XT_DOWNLOAD,      /* A surface copied back to the Xbox's memory */
    XT_GPU_IDLE,      /* GPU thread with nothing to do */
    XT_CPU_EXEC,      /* Xbox CPU thread running the game */
    XT_CPU_IDLE,      /* ...waiting (halted, or for the machine lock) */
    XT_UI_FRAME,      /* UI: getting the game's frame from the GPU thread */
    XT_UI_PRESENT,    /* UI: drawing and presenting to the TV */
    XT_TB_INVAL,      /* (count only) translated code blocks thrown away */
    XT_JC_FLUSH,      /* (count only) whole jump cache wiped */
    XT__COUNT
};

extern uint64_t xemu_timing_ns[XT__COUNT];
extern uint32_t xemu_timing_count[XT__COUNT];

static inline uint64_t xemu_timing_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

static inline void xemu_timing_add(int what, uint64_t since)
{
    __atomic_fetch_add(&xemu_timing_ns[what], xemu_timing_now() - since,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&xemu_timing_count[what], 1, __ATOMIC_RELAXED);
}

#ifdef __cplusplus
}
#endif

#endif
