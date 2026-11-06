#define RX_NO_THREAD_MACRO 1
/*
 * XOG: libc functions the console leaves out or breaks for a title, bound by
 * --defsym in tools/build.sh, so the title exports no libc names. XPSemu's notes:
 * a missing import links fine and jumps to 0 at run time; pthread_once is a no-op
 * on the PS5; strdup must use the platform heap. Our own code. GPL-3.0-or-later.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "native/net/net.h"

void *rx_memmem(const void *hay, size_t hn, const void *needle, size_t nn)
{
    const uint8_t *h = hay;
    if (nn == 0) {
        return (void *)h;
    }
    if (hn < nn) {
        return NULL;
    }
    const uint8_t first = *(const uint8_t *)needle;
    const uint8_t *end = h + hn - nn;
    while (h <= end) {
        const uint8_t *p = memchr(h, first, (size_t)(end - h) + 1);
        if (!p) {
            return NULL;
        }
        if (!memcmp(p, needle, nn)) {
            return (void *)p;
        }
        h = p + 1;
    }
    return NULL;
}

/* Only the disk-usage count walks a tree: it reports nothing for now. */
int rx_nftw(const char *path, void *fn, int nfds, int flags)
{
    (void)path; (void)fn; (void)nfds; (void)flags;
    errno = ENOSYS;
    return -1;
}

char *rx_strdup(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

const char *rx_gai_strerror(int e)
{
    (void)e;
    return "name lookup failed";
}

int rx_pthread_once(pthread_once_t *once, void (*fn)(void))
{
    /* The first word of pthread_once_t is its state: 0 at PTHREAD_ONCE_INIT. */
    int *state = (int *)once;
    int expected = 0;
    if (__atomic_compare_exchange_n(state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(state, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2) {
        sched_yield();
    }
    return 0;
}

int rx_clock_nanosleep(clockid_t clk, int flags, const struct timespec *t, struct timespec *rem)
{
    struct timespec d = *t;
    if (flags & 1) {    /* TIMER_ABSTIME */
        struct timespec now;
        clock_gettime(clk, &now);
        d.tv_sec -= now.tv_sec;
        d.tv_nsec -= now.tv_nsec;
        if (d.tv_nsec < 0) {
            d.tv_nsec += 1000000000L;
            d.tv_sec--;
        }
        if (d.tv_sec < 0) {
            return 0;
        }
    }
    return nanosleep(&d, rem) == 0 ? 0 : errno;
}

#undef pthread_create
#undef clock_gettime

uint64_t sceKernelGetTscFrequency(void);

static inline uint64_t rx_tsc(void)
{
    uint32_t a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}

/* Taken once from the kernel, then counted on the TSC. Other clocks go to the kernel. */
int rx_clock_gettime(clockid_t clk, struct timespec *ts)
{
    static volatile int ready;
    static uint64_t base_tsc, hz, base_mono_ns, base_real_ns;
    if (clk != CLOCK_MONOTONIC && clk != CLOCK_REALTIME) {
        return clock_gettime(clk, ts);
    }
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {
        struct timespec m, r;
        clock_gettime(CLOCK_MONOTONIC, &m);
        clock_gettime(CLOCK_REALTIME, &r);
        base_mono_ns = (uint64_t)m.tv_sec * 1000000000ull + (uint64_t)m.tv_nsec;
        base_real_ns = (uint64_t)r.tv_sec * 1000000000ull + (uint64_t)r.tv_nsec;
        hz = sceKernelGetTscFrequency();
        base_tsc = rx_tsc();
        __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    }
    const uint64_t ticks = rx_tsc() - base_tsc;
    const uint64_t ns = (uint64_t)(((unsigned __int128)ticks * 1000000000ull) / hz);
    const uint64_t t = (clk == CLOCK_MONOTONIC ? base_mono_ns : base_real_ns) + ns;
    ts->tv_sec = (time_t)(t / 1000000000ull);
    ts->tv_nsec = (long)(t % 1000000000ull);
    return 0;
}

/* Mesa's disk cache turns itself off unless the effective IDs are the real ones, and
 * after the HEN's jailbreak they may not be (XPSemu). */
uid_t getuid(void);
gid_t getgid(void);
uid_t __wrap_geteuid(void) { return getuid(); }
gid_t __wrap_getegid(void) { return getgid(); }

int scePthreadSetaffinity(pthread_t thread, unsigned long long mask);
void nc_log(const char *tag, const char *fmt, ...);

/* The layout, once rx_core_layout has made it. Until then threads stay where they start. */
static unsigned long long g_cpus_rest, g_cpus_decoder, g_cpus_render;
static pthread_t g_thread_decoder, g_thread_render;
static volatile int g_have_decoder, g_have_render;

#define RX_PRIO_SOUND 690
#define RX_PRIO_BACKGROUND 730

/* Every thread round-robin, on the rest of the processors: one started by the GPU's thread
 * or the CPU's would otherwise share that thread's core. The graphics driver's own threads
 * aren't made here: they stay with the render thread that starts them, near their work. */
int rx_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    const int r = pthread_create(t, a, fn, arg);
    if (r == 0) {
        int policy = 0;
        struct sched_param sp;
        if (pthread_getschedparam(*t, &policy, &sp) == 0) {
            pthread_setschedparam(*t, SCHED_RR, &sp);
        }
        if (g_cpus_rest) {
            scePthreadSetaffinity(*t, g_cpus_rest);
        }
    }
    return r;
}

static void set_prio(pthread_t t, int prio)
{
    int policy = 0;
    struct sched_param sp;
    if (pthread_getschedparam(t, &policy, &sp) == 0) {
        sp.sched_priority = prio;
        pthread_setschedparam(t, policy, &sp);
    }
}

void rx_pthread_setname(pthread_t t, const char *name)
{
    pthread_set_name_np(t, name);
    if (!name) {
        return;
    }
    if (!strcmp(name, "hle-pb")) {
        g_thread_decoder = t;
        g_have_decoder = 1;
        if (g_cpus_decoder) {
            scePthreadSetaffinity(t, g_cpus_decoder);
        }
    } else if (!strcmp(name, "hle.render")) {
        g_thread_render = t;
        g_have_render = 1;
        if (g_cpus_render) {
            scePthreadSetaffinity(t, g_cpus_render);
        }
    } else if (!strcmp(name, "apu.out")) {
        set_prio(t, RX_PRIO_SOUND);
    } else if (!strncmp(name, "hle.pipe", 8) || !strncmp(name, "hle.prebuild", 12) || !strncmp(name, "hle.pcache", 10) ||
               !strcmp(name, "io-closer")) {
        set_prio(t, RX_PRIO_BACKGROUND);
    }
}

/* From rx_core_layout; the GPU's threads may have been named already. */
void rx_thread_layout(unsigned long long rest, unsigned long long decoder, unsigned long long render)
{
    g_cpus_decoder = decoder;
    g_cpus_render = render;
    g_cpus_rest = rest;
    if (g_have_decoder) {
        scePthreadSetaffinity(g_thread_decoder, decoder);
    }
    if (g_have_render) {
        scePthreadSetaffinity(g_thread_render, render);
    }
}

/* Mesa's threads block every signal; a fault on one then ends the process with no
 * handler run and nothing in the log. Faults stay deliverable on every thread. */
int __real_pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    if (!set || how == SIG_UNBLOCK) {
        return __real_pthread_sigmask(how, set, old);
    }
    sigset_t s = *set;
    const int keep[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGSYS, SIGABRT};
    for (unsigned i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        sigdelset(&s, keep[i]);
    }
    return __real_pthread_sigmask(how, &s, old);
}

/* Who ends the title, when it isn't a fault. */
extern void rx_log_raw(const char *text, size_t len);
void rx_stubs_leave(void);
void __real_exit(int code) __attribute__((noreturn));
void __real__exit(int code) __attribute__((noreturn));
void __real_abort(void) __attribute__((noreturn));

static void told(const char *what, int code, void *from)
{
    char b[120];
    const int n = snprintf(b, sizeof(b), "xog: %s(%d) called from %p\n", what, code, from);
    rx_log_raw(b, (size_t)n);
}

void __wrap_exit(int code)
{
    told("exit", code, __builtin_return_address(0));
    rx_stubs_leave();
    __real_exit(code);
}

void __wrap__exit(int code)
{
    told("_exit", code, __builtin_return_address(0));
    rx_stubs_leave();
    __real__exit(code);
}

void __wrap_abort(void)
{
    told("abort", 0, __builtin_return_address(0));
    __real_abort();
}

/* Reported unbound by the first console run: a call would jump to 0. */
/* futimens and utimensat aren't there on the console: ENOSYS. futimes and utimes take microseconds
 * and no "leave this one", so a time left alone is given back as it stands. */
static void times_for_host(const struct timespec in[2], const struct stat *st, struct timeval out[2])
{
    struct timeval now;
    gettimeofday(&now, NULL);
    for (int i = 0; i < 2; i++) {
        if (in[i].tv_nsec == UTIME_NOW) {
            out[i] = now;
        } else if (in[i].tv_nsec == UTIME_OMIT) {
            const struct timespec *was = i ? &st->st_mtim : &st->st_atim;
            out[i].tv_sec = was->tv_sec;
            out[i].tv_usec = was->tv_nsec / 1000;
        } else {
            out[i].tv_sec = in[i].tv_sec;
            out[i].tv_usec = in[i].tv_nsec / 1000;
        }
    }
}

int rx_futimens(int fd, const struct timespec t[2])
{
    struct stat st;
    struct timeval tv[2];
    if (!t) {
        return futimes(fd, NULL);
    }
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    times_for_host(t, &st, tv);
    return futimes(fd, tv);
}

int rx_utimensat(int dir, const char *path, const struct timespec t[2], int flags)
{
    struct stat st;
    struct timeval tv[2];
    if (dir != AT_FDCWD || flags) {
        errno = EINVAL;     /* nothing here asks for these */
        return -1;
    }
    if (!t) {
        return utimes(path, NULL);
    }
    if (stat(path, &st) != 0) {
        return -1;
    }
    times_for_host(t, &st, tv);
    return utimes(path, tv);
}

/* curl asks; the console's libc doesn't have them. */
int rx_pipe2(int fds[2], int flags)
{
    if (pipe(fds) != 0) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if (flags & O_NONBLOCK) {
            fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        }
        if (flags & O_CLOEXEC) {
            fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        }
    }
    return 0;
}

unsigned int rx_if_nametoindex(const char *name)
{
    (void)name;
    return 0;
}

long rx_readlink(const char *path, char *buf, size_t n)
{
    (void)path; (void)buf; (void)n;
    errno = EINVAL;     /* the console has no symlinks */
    return -1;
}

long rx_pathconf(const char *path, int name)
{
    (void)path; (void)name;
    return 255;
}

int rx_mkstemp(char *tmpl)
{
    const size_t n = strlen(tmpl);
    if (n < 6) {
        errno = EINVAL;
        return -1;
    }
    static const char abc[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    uint64_t r = (uint64_t)time(NULL) * 2654435761u + (uint64_t)(uintptr_t)tmpl;
    for (int tries = 0; tries < 1000; tries++) {
        for (size_t i = n - 6; i < n; i++) {
            r = r * 6364136223846793005ull + 1442695040888963407ull;
            tmpl[i] = abc[(r >> 40) % 36];
        }
        const int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }
    }
    return -1;
}

/* Network play has its own later step: the NAT backend says no. */
static bool nat_init(void) { return false; }
static void nat_input(const uint8_t *frame, size_t len) { (void)frame; (void)len; }
static int nat_fill(struct pollfd *fds, int max, int *timeout_ms) { (void)fds; (void)max; (void)timeout_ms; return 0; }
static void nat_poll(struct pollfd *fds, int n) { (void)fds; (void)n; }
const NcNetBackend nc_net_nat = {"nat (not on the PS5 yet)", nat_init, nat_input, nat_fill, nat_poll};
