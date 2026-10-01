/*
 * OS-specific Helpers
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XEMU_OS_UTILS_H
#define XEMU_OS_UTILS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *xemu_get_os_info(void);

#ifdef __PROSPERO__
// Logs to /data/xemu/xemu.log and reports crashes there. First thing in main.
void xemu_ps5_early_init(void);

// The busiest threads, each given a core of its own (perf.cpu_pinning):
// they register as they start, and the layout is applied once start-up is
// over, from the UI thread.
enum {
    XEMU_PS5_THREAD_CPU, // The Xbox's CPU (TCG)
    XEMU_PS5_THREAD_GPU, // The Xbox's GPU (nv2a's pfifo thread)
    XEMU_PS5_THREAD__COUNT, // The pinned ones
    XEMU_PS5_THREAD_UI = XEMU_PS5_THREAD__COUNT, // Profiled, not pinned
    XEMU_PS5_THREAD__ALL
};
void xemu_ps5_register_thread(int role);
void xemu_ps5_apply_pinning(void);
const char *xemu_ps5_pinning_summary(void);
// CPU time a registered thread has used, in seconds (as of its last
// profiler sample); false if unknown.
bool xemu_ps5_thread_cpu_seconds(int role, double *seconds);

// The sampling profiler (for the game log): counting from start, the
// report of where each thread's time went (malloc'd).
void xemu_ps5_profile_start(void);
void xemu_ps5_profile_stop(void);
char *xemu_ps5_profile_report(void);
#endif

#ifdef CONFIG_CPUID_H
#include <cpuid.h>
#endif

static inline const char *xemu_get_cpu_info(void)
{
    const char *cpu_info = "";
#ifdef CONFIG_CPUID_H
    static uint32_t brand[12];
    if (__get_cpuid_max(0x80000004, NULL)) {
        __get_cpuid(0x80000002, brand+0x0, brand+0x1, brand+0x2, brand+0x3);
        __get_cpuid(0x80000003, brand+0x4, brand+0x5, brand+0x6, brand+0x7);
        __get_cpuid(0x80000004, brand+0x8, brand+0x9, brand+0xa, brand+0xb);
    }
    cpu_info = (const char *)brand;
#endif
    // FIXME: Support other architectures (e.g. ARM)
    return cpu_info;
}

#ifdef __cplusplus
}
#endif

#endif
