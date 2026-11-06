/*
 * Device test of the APU's output (native/dev/apu/out.c): a real AAudio
 * stream drains the ring a producer keeps between apu.c's watermarks; the
 * stream is then lost the way AAudio reports a disconnect (its error
 * callback; no route change can be made from adb), and the ring must keep
 * draining (reopen_thread) until a new stream plays it again.
 *
 *   $NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android28-clang -O1 -I. \
 *       tests/native/apu_out/out_test.c -laaudio -llog -o out_test
 *   adb push out_test /data/local/tmp/ && adb shell /data/local/tmp/out_test
 */
#include <stdarg.h>
#include <stdio.h>

#include "native/dev/apu/out.c"

/* The core's services out.c uses */
void nc_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  [%s] ", tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

bool nc_paused(void)
{
    return false;
}

void apu_thread_start(pthread_t *t, const char *name, void *(*fn)(void *), void *arg, bool joinable)
{
    pthread_create(t, NULL, fn, arg);
    if (!joinable) {
        pthread_detach(*t);
    }
}

#define FRAME 1024              /* an EP frame, as apu.c pushes them */
#define SAMPLES 2048            /* apu.c's callback size */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static int g_high, g_pushed;
static bool g_stop;

/* apu.c's frame thread, reduced: a frame whenever the ring is under its
 * high watermark (three callbacks), else wait for a drain */
static void *producer(void *arg)
{
    static uint8_t frame[FRAME];
    pthread_mutex_lock(&g_mu);
    while (!g_stop) {
        if (apu_out_queued() < g_high) {
            apu_out_push(frame, FRAME);
            g_pushed++;
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5000000;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_nsec -= 1000000000L;
                ts.tv_sec++;
            }
            pthread_cond_timedwait(&g_wake, &g_mu, &ts);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static int g_failed;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    g_failed += !ok;
}

/* frames pushed (the ring drained) and callbacks over one second */
static void second(int *pushed, uint32_t *cb, uint32_t *short_cb)
{
    uint32_t c, s;
    apu_out_stats(&c, &s);
    const int p0 = __atomic_load_n(&g_pushed, __ATOMIC_RELAXED);
    sleep(1);
    *pushed = __atomic_load_n(&g_pushed, __ATOMIC_RELAXED) - p0;
    apu_out_stats(cb, short_cb);
}

int main(void)
{
    const int drain = apu_out_open(48 * FRAME, SAMPLES, &g_wake);
    g_high = 3 * drain;
    check(g_out.stream != NULL, "a stream opened");
    pthread_t t;
    pthread_create(&t, NULL, producer, NULL);
    sleep(1);                   /* the stream starting */

    int pushed;
    uint32_t cb, short_cb;
    second(&pushed, &cb, &short_cb);
    printf("  playing: %d frames pushed, %u callbacks, %u short\n", pushed, cb, short_cb);
    check(cb >= 20 && pushed >= 160, "the stream drains the ring (~23 callbacks, ~188 frames a second)");
    check(short_cb == 0, "no short callbacks");

    AAudioStream *lost = g_out.stream;
    on_error(lost, NULL, AAUDIO_ERROR_DISCONNECTED);
    on_error(lost, NULL, AAUDIO_ERROR_DISCONNECTED);    /* a second report: one reopen */
    second(&pushed, &cb, &short_cb);
    printf("  after the loss: %d frames pushed, %u callbacks\n", pushed, cb);
    check(pushed >= 160, "the ring kept draining at the device's rate");

    /* (the new stream can have the closed one's address) */
    pthread_mutex_lock(&g_out.mu);
    AAudioStream *now = g_out.stream;
    pthread_mutex_unlock(&g_out.mu);
    check(now != NULL, "a new stream opened");
    check(!__atomic_load_n(&g_out.reopening, __ATOMIC_ACQUIRE), "reopen_thread finished");

    second(&pushed, &cb, &short_cb);
    printf("  playing again: %d frames pushed, %u callbacks, %u short\n", pushed, cb, short_cb);
    check(cb >= 20 && pushed >= 160, "the new stream drains the ring");

    on_error((AAudioStream *)&g_failed, NULL, AAUDIO_ERROR_DISCONNECTED);  /* not the current one */
    usleep(100000);
    check(g_out.stream == now && !g_out.reopening, "an error from another stream is ignored");

    nc_audio_set_paused(true);
    second(&pushed, &cb, &short_cb);
    check(cb == 0, "paused: no callbacks");
    nc_audio_set_paused(false);
    sleep(1);
    second(&pushed, &cb, &short_cb);
    check(cb >= 20, "resumed: callbacks again");

    pthread_mutex_lock(&g_mu);
    g_stop = true;
    pthread_mutex_unlock(&g_mu);
    pthread_join(t, NULL);
    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed != 0;
}
