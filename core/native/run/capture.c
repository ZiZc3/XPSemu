/*
 * xboxrun — frame capture without the app's display.
 *
 * Every NC_CAPTURE_EVERY seconds a thread reads the HLE GPU's front image
 * back (vk/display.h hle_disp_read_front) and writes frames/frame_<n>.png in
 * the working directory (up to NC_CAPTURE_MAX, default 20).
 *
 * By frame (nc_capture_frames: xboxrun's capture= and frames=), the render
 * thread hands over the frames asked for (vk/back.h HleFrameHook): each
 * frames/f<n>.png as it was presented, and the run ends at frames=.
 */
#include "native/core/nc.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/vk/display.h"

#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) {
        fwrite(data, 1, len, f);
    }
    uLong crc = crc32(0, (const Bytef *)type, 4);
    crc = crc32(crc, data, len);
    uint8_t c[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(c, 1, 4, f);
}

/* RGB PNG from RGBA rows, top row first. */
static bool write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    const size_t row = 1 + (size_t)w * 3;
    uint8_t *raw = malloc(row * h);
    if (!raw) {
        return false;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *d = raw + row * y;
        const uint8_t *s = rgba + (size_t)y * w * 4;
        d[0] = 0;
        for (uint32_t x = 0; x < w; x++) {
            d[1 + 3 * x] = s[4 * x];
            d[2 + 3 * x] = s[4 * x + 1];
            d[3 + 3 * x] = s[4 * x + 2];
        }
    }
    uLongf zlen = compressBound(row * h);
    uint8_t *z = malloc(zlen);
    bool ok = z && compress2(z, &zlen, raw, row * h, 6) == Z_OK;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                            (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                            8, 2, 0, 0, 0};
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (uint32_t)zlen);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw);
    free(z);
    return f != NULL;
}

/* The front image now, or false if there is none yet. */
static bool capture_one(const char *path)
{
    uint32_t w = 0, h = 0;
    uint8_t *px = hle_disp_read_front(&w, &h);
    if (!px) {
        return false;
    }
    const bool ok = write_png(path, px, w, h);
    g_free(px);
    if (ok) {
        nc_log("hakuX-native-frontier", "captured %s (%ux%u)", path, w, h);
    }
    return ok;
}

static void *capture_thread(void *arg)
{
    const unsigned every = (unsigned)(uintptr_t)arg;
    const char *m = getenv("NC_CAPTURE_MAX");
    const unsigned max = m ? (unsigned)atoi(m) : 20;
    mkdir("frames", 0775);
    for (unsigned n = 0, t = 0; n < max;) {
        sleep(every);
        t += every;
        char path[64];
        snprintf(path, sizeof(path), "frames/frame_%03us.png", t);
        n += capture_one(path);
    }
    return NULL;
}

static struct {
    uint64_t at[64];            /* frames to capture */
    unsigned n;
    uint64_t stop;              /* the last frame (0: none) */
} g_cf;

static bool frame_want(uint64_t n, void *opaque)
{
    if (n == g_cf.stop) {
        return true;
    }
    for (unsigned i = 0; i < g_cf.n; i++) {
        if (g_cf.at[i] == n) {
            return true;
        }
    }
    return false;
}

static void frame_got(uint64_t n, const uint8_t *rgba, uint32_t w, uint32_t h, void *opaque)
{
    bool asked = false;
    for (unsigned i = 0; i < g_cf.n; i++) {
        asked |= g_cf.at[i] == n;
    }
    if (asked) {
        char path[64];
        snprintf(path, sizeof(path), "frames/f%llu.png", (unsigned long long)n);
        if (!rgba) {
            nc_log("hakuX-native-frontier", "frame %llu: no picture (no display image)", (unsigned long long)n);
        } else if (write_png(path, rgba, w, h)) {
            nc_log("hakuX-native-frontier", "frame %llu: %s (%ux%u)", (unsigned long long)n, path, w, h);
        } else {
            nc_log("hakuX-native-frontier", "frame %llu: %s not written", (unsigned long long)n, path);
        }
    }
    if (n == g_cf.stop) {
        hle_back_save_caches();         /* the next run's shaders and pipelines */
        nc_log("hakuX-native-frontier", "frames: %llu reached", (unsigned long long)n);
        _exit(0);
    }
}

void nc_capture_frames(const char *list, uint64_t stop)
{
    for (const char *s = list; s && *s && g_cf.n < sizeof(g_cf.at) / sizeof(g_cf.at[0]);) {
        char *end;
        const uint64_t n = strtoull(s, &end, 10);
        if (end == s) {
            break;
        }
        if (n) {
            g_cf.at[g_cf.n++] = n;
        }
        s = *end == ',' ? end + 1 : end;
    }
    g_cf.stop = stop;                   /* 0: the run goes on after the captures */
    if (!g_cf.n && !g_cf.stop) {
        return;
    }
    mkdir("frames", 0775);
    static const HleFrameHook hook = { frame_want, frame_got, NULL };
    hle_back_set_frame_hook(&hook);
}

void nc_capture_start(void)
{
    const char *e = getenv("NC_CAPTURE_EVERY");
    if (!e || atoi(e) <= 0) {
        return;
    }
    pthread_t t;
    pthread_create(&t, NULL, capture_thread, (void *)(uintptr_t)atoi(e));
    pthread_detach(t);
}
