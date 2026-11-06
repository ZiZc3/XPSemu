/* XOG: small PS5 answers to the Linux and Android calls hakuX's core makes.
 * Included first in every source of the core. Our own code. */
#ifndef REXEMU_PS5_H
#define REXEMU_PS5_H

#ifndef __cplusplus
#include <stdint.h>
#endif
#include <pthread.h>
#include <pthread_np.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The console's coarse clock isn't there. Hot time reads move to the TSC later:
 * clock_gettime enters the kernel on the PS5 (880 ns measured, TSC 23 ns). */
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif

/* The console's clock_gettime enters the kernel (880 ns; TSC 23 ns). libc_ps5.c. */
int rx_clock_gettime(clockid_t clk, struct timespec *ts);
#ifndef RX_NO_THREAD_MACRO
#define clock_gettime rx_clock_gettime
#endif

/* hakuX compares thread ids for equality; a thread's pthread handle does that. */
static inline int xog_gettid(void)
{
    return (int)(uintptr_t)pthread_self();
}
#define gettid xog_gettid

/* On the CPU's thread, formatting without the C library's lock (log_ps5.c). */
#ifndef __cplusplus
#include <stdarg.h>
int rx_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int rx_snprintf(char *buf, size_t cap, const char *fmt, ...);
#define vsnprintf rx_vsnprintf
#define snprintf rx_snprintf
#endif
/* libslirp 4.7.0 names ssize_t plainly; hakuX pins a later commit */
#define slirp_ssize_t ssize_t

/* A name is a thread's role: its core and priority (libc_ps5.c). */
void rx_pthread_setname(pthread_t thread, const char *name);
#define pthread_setname_np(thread, name) (rx_pthread_setname((thread), (name)), 0)

/* Every thread round-robin (libc_ps5.c). */
int rx_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
#ifndef RX_NO_THREAD_MACRO
#define pthread_create rx_pthread_create
#endif

#ifdef __cplusplus
}
#endif

#endif
