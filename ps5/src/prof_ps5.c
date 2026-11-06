/* A test's profiler: XOG_PROF=1 in /data/xpsemu/debug.txt. A thread samples the decoder and the render thread 1000 times a second
 * by signal, and every 5 s logs where they were (absolute addresses, 16-byte buckets; tools/profsym.py names them). A sample
 * outside our code (a system call, a library) also counts its caller. No file, nothing runs. */
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>

void nc_log(const char *tag, const char *fmt, ...);
extern int rx_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

#define PROF_THREADS 4
#define PROF_GAME (PROF_THREADS)
#define PROF_BINS 4096
#define TEXT_LO 0x3000000000ull
#define TEXT_HI 0x3004000000ull

typedef struct Bin {
    uint64_t pc;
    uint32_t n;
} Bin;

static struct {
    const char *name;
    pthread_t t;
    volatile int on;
    uint32_t samples, outside;
    Bin bins[PROF_BINS];
    Bin callers[PROF_BINS];
} g_th[PROF_THREADS + 1];
static volatile int g_nth, g_running;

static void bump(Bin *b, uint64_t pc)
{
    pc &= ~15ull;
    uint32_t h = (uint32_t)((pc >> 4) * 2654435761u) % PROF_BINS;
    for (int i = 0; i < 16; i++, h = (h + 1) % PROF_BINS) {
        if (!b[h].pc) {
            b[h].pc = pc;
        }
        if (b[h].pc == pc) {
            b[h].n++;
            return;
        }
    }
}

static void on_sample(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)si;
    const pthread_t me = pthread_self();
    const ucontext_t *u = uc;
    for (int i = 0; i < g_nth; i++) {
        if (pthread_equal(g_th[i].t, me)) {
            const uint64_t rip = (uint64_t)u->uc_mcontext.mc_rip;
            g_th[i].samples++;
            bump(g_th[i].bins, rip);
            if (rip < TEXT_LO || rip >= TEXT_HI) {
                g_th[i].outside++;
                bump(g_th[i].callers, *(const uint64_t *)(uintptr_t)u->uc_mcontext.mc_rsp);
            }
            return;
        }
    }
}

/* In the game thread's signal: where it was. Inside the signal's mask, so the time a fault takes lands on its instruction. */
void xog_prof_game(bool guest, uint64_t rip, const uint64_t *sp)
{
    g_th[PROF_GAME].samples++;
    bump(g_th[PROF_GAME].bins, rip);
    if (!guest && sp) {
        g_th[PROF_GAME].outside++;
        for (unsigned w = 0; w < 96; w++) {
            if (sp[w] >= TEXT_LO && sp[w] < TEXT_HI) {
                bump(g_th[PROF_GAME].callers, sp[w]);
                break;
            }
        }
    }
}

void xog_prof_thread(const char *name)
{
    if (!getenv("XOG_PROF")) {
        return;
    }
    const int i = g_nth;
    if (i < PROF_THREADS) {
        g_th[i].name = name;
        g_th[i].t = pthread_self();
        g_nth = i + 1;
    }
}

static int by_count(const void *a, const void *b)
{
    const Bin *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static void dump(const char *name, const char *what, Bin *b, uint32_t total)
{
    static Bin sorted[PROF_BINS];
    memcpy(sorted, b, sizeof(sorted));
    memset(b, 0, sizeof(sorted));
    qsort(sorted, PROF_BINS, sizeof(Bin), by_count);
    for (int i = 0; i < 40 && sorted[i].n; i++) {
        nc_log("xog-prof", "%s %s %llx %u of %u", name, what, (unsigned long long)sorted[i].pc, sorted[i].n, total);
    }
}

static void *sampler(void *arg)
{
    (void)arg;
    /* XOG_PROF_MS: ms between samples (1 by default; 1 ms starved Crash Nitro Kart's CPU thread: try 5-10) */
    const char *pe = getenv("XOG_PROF_MS");
    const long period = pe && atoi(pe) > 0 ? atoi(pe) : 1;
    const struct timespec ms = { period / 1000, (period % 1000) * 1000000 };
    const unsigned report = (unsigned)(5000 / period ? 5000 / period : 1);
    unsigned ticks = 0;
    extern void rx_prof_kick(void);
    g_th[PROF_GAME].name = "game";
    for (;;) {
        nanosleep(&ms, NULL);
        for (int i = 0; i < g_nth; i++) {
            pthread_kill(g_th[i].t, SIGPROF);
        }
        rx_prof_kick();
        if (++ticks >= report) {
            ticks = 0;
            const struct timespec settle = { 0, 2000000 };
            nanosleep(&settle, NULL);
            extern void xog_prof_fpu_report(void);
            xog_prof_fpu_report();
            for (int n = 0; n <= g_nth; n++) {
                const int i = n == g_nth ? PROF_GAME : n;
                const uint32_t total = g_th[i].samples, outside = g_th[i].outside;
                g_th[i].samples = g_th[i].outside = 0;
                dump(g_th[i].name, "at", g_th[i].bins, total);
                dump(g_th[i].name, "called-from", g_th[i].callers, outside);
            }
        }
    }
    return NULL;
}

void xog_prof_start(void)
{
    if (!getenv("XOG_PROF") || g_running) {
        return;
    }
    g_running = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    pthread_t t;
    if (rx_pthread_create(&t, NULL, sampler, NULL) == 0) {
        pthread_detach(t);
        nc_log("xog-prof", "on");
    }
}
