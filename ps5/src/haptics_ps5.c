/*
 * haptics_ps5.c - the Xbox pad's rumble on the DualSense's HD haptics.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Xbox controller has two motors: a heavy low one (left) and a light fast
 * one (right). The plain way (main_ps5.c rx_rumble) sends their two levels with
 * scePadSetVibration in the pad's "compatible" mode, where the console turns
 * them into one generic buzz. Here each player instead gets the DualSense's
 * vibration audio port: a 48 kHz stereo signal written to it drives the two
 * voice-coil actuators directly ("advanced" mode). The signal is a low sine for
 * the heavy motor, a higher one for the light motor, plus filtered noise so it
 * reads as texture rather than a single tone, faded so level steps don't click.
 * Anything that fails falls back to the plain motors.
 *
 * The approach and the synthesis (port type 10, advanced mode 1, the sine and
 * noise mix, envelope and headroom figures) are adapted from ProsperoEden
 * (BlackBearReloaded), GPL-3.0-or-later: headless/pad_haptics.cpp. This C port
 * and its wiring into XPSemu's rumble are our own.
 */
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void nc_log(const char *tag, const char *fmt, ...);
int sceAudioOutOpen(int user_id, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void *buf);
int sceAudioOutClose(int handle);
int scePadSetVibrationMode(int32_t handle, int32_t mode);

#define HAPTIC_PORT_TYPE 10        /* the pad's vibration audio port */
#define HAPTIC_GRAIN     256       /* frames per block: 5.3 ms at 48 kHz */
#define HAPTIC_RATE      48000
#define FORMAT_S16_STEREO 1
#define FORMAT_F32_STEREO 4
#define MODE_ADVANCED    1
#define MODE_COMPATIBLE  2
#define FAILURES_TO_FALLBACK 40
#define RETRY_MS         5000
#define MAX_TRIES        5

#define ENVELOPE_RATE    0.02f
#define NOISE_LOW_PASS   0.08f
#define HEADROOM         26000.0f

volatile int g_rx_haptics = 1;     /* Settings: HD haptics (else the plain motors) */
static volatile int g_haptics_closed;   /* leaving: no stream starts again after xog_haptics_stop_all */

typedef struct Haptic {
    int32_t user, handle;          /* the pad this stream drives; handle -1: none */
    int port;
    bool f32;
    volatile int active;           /* 1 while the worker carries the rumble */
    volatile int stop;
    volatile float amp_large, amp_small, hz_large, hz_small;
    pthread_t thread;
    bool thread_on;
    unsigned tries;
    uint64_t last_try_ms;
    /* the voice, the worker's own */
    double ph_large, ph_small;
    float env_large, env_small, noise_l, noise_r;
    uint32_t rng;
} Haptic;

static Haptic g_h[4] = {
    {.handle = -1, .port = -1, .rng = 0x9e3779b9u}, {.handle = -1, .port = -1, .rng = 0x7f4a7c15u},
    {.handle = -1, .port = -1, .rng = 0x85ebca6bu}, {.handle = -1, .port = -1, .rng = 0xc2b2ae35u},
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static float noise(uint32_t *rng)
{
    *rng = *rng * 1664525u + 1013904223u;
    return (float)((*rng >> 9) & 0xffffu) / 32768.0f - 1.0f;
}

static int16_t to_s16(float s)
{
    const float v = s * HEADROOM;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v;
}

/* One block of the signal; both actuators carry both bands, with decorrelated noise. */
static void render(Haptic *h, int16_t *s16, float *f32)
{
    const float al = h->amp_large, as = h->amp_small, hl = h->hz_large, hs = h->hz_small;
    for (unsigned i = 0; i < HAPTIC_GRAIN; i++) {
        h->env_large += (al - h->env_large) * ENVELOPE_RATE;
        h->env_small += (as - h->env_small) * ENVELOPE_RATE;
        h->noise_l += NOISE_LOW_PASS * (noise(&h->rng) - h->noise_l);
        h->noise_r += NOISE_LOW_PASS * (noise(&h->rng) - h->noise_r);
        h->ph_large += hl / HAPTIC_RATE;
        h->ph_small += hs / HAPTIC_RATE;
        if (h->ph_large >= 1.0) h->ph_large -= 1.0;
        if (h->ph_small >= 1.0) h->ph_small -= 1.0;
        const float sl = sinf((float)(6.283185307179586 * h->ph_large));
        const float ss = sinf((float)(6.283185307179586 * h->ph_small));
        const float left = h->env_large * (0.70f * sl + 0.50f * h->noise_l) + h->env_small * (0.55f * ss + 0.45f * h->noise_r);
        const float right = h->env_large * (0.70f * sl + 0.50f * h->noise_r) + h->env_small * (0.55f * ss + 0.45f * h->noise_l);
        s16[2 * i] = to_s16(left);
        s16[2 * i + 1] = to_s16(right);
        if (f32) {
            f32[2 * i] = s16[2 * i] / 32768.0f;
            f32[2 * i + 1] = s16[2 * i + 1] / 32768.0f;
        }
    }
}

static void *worker(void *arg)
{
    Haptic *h = (Haptic *)arg;
    static __thread int16_t s16[HAPTIC_GRAIN * 2];
    static __thread float f32[HAPTIC_GRAIN * 2];
    unsigned failures = 0;
    while (!h->stop) {
        render(h, s16, h->f32 ? f32 : NULL);
        if (sceAudioOutOutput(h->port, h->f32 ? (const void *)f32 : (const void *)s16) < 0) {
            if (++failures >= FAILURES_TO_FALLBACK) {
                nc_log("xog-pad", "haptics: player %d's stream failed %u times: back to the motors",
                       (int)(h - g_h) + 1, failures);
                scePadSetVibrationMode(h->handle, MODE_COMPATIBLE);
                h->active = 0;
                return NULL;
            }
            usleep(2000);
        } else {
            failures = 0;
        }
    }
    return NULL;
}

static void stop_one(Haptic *h)
{
    if (h->thread_on) {
        h->stop = 1;
        pthread_join(h->thread, NULL);
        h->thread_on = false;
    }
    if (h->port >= 0) {
        sceAudioOutOutput(h->port, NULL);   /* drain */
        sceAudioOutClose(h->port);
        h->port = -1;
    }
    if (h->active && h->handle >= 0) {
        scePadSetVibrationMode(h->handle, MODE_COMPATIBLE);
    }
    h->active = 0;
    h->amp_large = h->amp_small = 0.0f;
}

static bool start_one(Haptic *h, unsigned player, int32_t user, int32_t handle)
{
    h->user = user;
    h->handle = handle;
    int port = sceAudioOutOpen(user, HAPTIC_PORT_TYPE, 0, HAPTIC_GRAIN, HAPTIC_RATE, FORMAT_S16_STEREO);
    bool f32 = false;
    if (port < 0) {
        port = sceAudioOutOpen(user, HAPTIC_PORT_TYPE, 0, HAPTIC_GRAIN, HAPTIC_RATE, FORMAT_F32_STEREO);
        f32 = port >= 0;
    }
    if (port < 0) {
        nc_log("xog-pad", "haptics: player %u: no vibration audio port (%#x): plain motors", player + 1,
               (unsigned)port);
        return false;
    }
    const int mode = scePadSetVibrationMode(handle, MODE_ADVANCED);
    if (mode != 0) {
        sceAudioOutClose(port);
        nc_log("xog-pad", "haptics: player %u: advanced mode refused (%#x): plain motors", player + 1,
               (unsigned)mode);
        return false;
    }
    h->port = port;
    h->f32 = f32;
    h->stop = 0;
    h->active = 1;
    if (pthread_create(&h->thread, NULL, worker, h) != 0) {
        h->active = 0;
        stop_one(h);
        scePadSetVibrationMode(handle, MODE_COMPATIBLE);
        return false;
    }
    h->thread_on = true;
    nc_log("xog-pad", "haptics: player %u on HD haptics (port %d, %s)", player + 1, port, f32 ? "f32" : "s16");
    return true;
}

/* The Xbox pad's motor levels (0-65535 each) for a player. True when HD
 * haptics carried them; false: the caller drives the plain motors. */
int xog_haptics_rumble(unsigned player, int32_t user, int32_t handle, uint16_t large, uint16_t small)
{
    if (player >= 4) {
        return 0;
    }
    Haptic *h = &g_h[player];
    if (g_haptics_closed) {
        return 0;               /* leaving: the pad stays on the plain (still) motors */
    }
    if (!g_rx_haptics || handle < 0 || user < 0) {
        if (h->thread_on || h->port >= 0) {
            stop_one(h);
            h->handle = -1;
        }
        return 0;
    }
    if (h->handle != handle) {          /* a new controller for this player */
        stop_one(h);
        h->handle = handle;
        h->tries = 0;
        h->last_try_ms = 0;
    }
    if (!h->active) {
        const uint64_t now = now_ms();
        if (h->thread_on || h->tries >= MAX_TRIES || (h->last_try_ms && now - h->last_try_ms < RETRY_MS)) {
            if (h->thread_on && !h->active) {   /* the worker fell back: reap it */
                pthread_join(h->thread, NULL);
                h->thread_on = false;
                if (h->port >= 0) {
                    sceAudioOutClose(h->port);
                    h->port = -1;
                }
                h->last_try_ms = now;
            }
            return 0;
        }
        h->tries++;
        h->last_try_ms = now;
        if (!start_one(h, player, user, handle)) {
            return 0;
        }
    }
    /* A perceptual curve (weak rumble still felt), and a pitch that rises a
     * little with strength: a hard hit reads sharper than a soft purr. */
    const float al = powf(large / 65535.0f, 0.6f), as = powf(small / 65535.0f, 0.6f);
    h->amp_large = al;
    h->amp_small = as;
    h->hz_large = 45.0f + 30.0f * al;
    h->hz_small = 160.0f + 80.0f * as;
    return 1;
}

/* Leaving: every pad back to the plain motors, still. */
void xog_haptics_stop_all(void)
{
    g_haptics_closed = 1;
    for (unsigned p = 0; p < 4; p++) {
        if (g_h[p].thread_on || g_h[p].port >= 0) {
            stop_one(&g_h[p]);
        }
        g_h[p].handle = -1;
    }
}
