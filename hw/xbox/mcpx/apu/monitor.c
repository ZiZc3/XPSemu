/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_int.h"

#ifdef __PROSPERO__
/*
 * PS5: SDL has no audio driver for it, so the stream isn't bound to a device:
 * it stays the queue apu.c paces the APU by, and a thread feeds the console's
 * audio out from it, 256 frames at a time. sceAudioOutOutput blocks until the
 * previous grain has played, which paces the thread. The calls are PS5SX2's
 * (ps5/coreorbis/orbis-shims/ProsperoAudio.cpp).
 */
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, unsigned int len,
                    unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void *buf);
int sceAudioOutClose(int handle);

#define PS5_AUDIO_USER_SYSTEM 255
#define PS5_AUDIO_PORT_MAIN 0
#define PS5_AUDIO_S16_STEREO 1
#define PS5_AUDIO_GRAIN 256 /* frames */

/* The dashboard's sounds, mixed in (ui/xui/ui-sounds.cc). */
void xemu_ps5_ui_sound_mix(int16_t *buf, int frames);

/* XPSemu: the console's sound under the launch screen (0 while it covers
 * the Xbox's boot, back to 1 as it fades; ui/xui/dashboard.cc). The
 * dashboard's own sounds are mixed in after it. */
extern float g_xemu_apu_ui_gain;
float g_xemu_apu_ui_gain = 1.0f;

static QemuThread ps5_audio_thread;
static int ps5_audio_handle = -1;
static bool ps5_audio_running;

static void *ps5_audio_out(void *opaque)
{
    MCPXAPUState *d = opaque;
    int16_t buf[PS5_AUDIO_GRAIN * 2];

    while (qatomic_read(&ps5_audio_running)) {
        int got = SDL_GetAudioStreamData(d->monitor.stream, buf, sizeof(buf));
        if (got < (int)sizeof(buf)) {
            /* Not enough queued: play the rest as silence */
            got = MAX(got, 0);
            memset((uint8_t *)buf + got, 0, sizeof(buf) - got);
        }
        xemu_ps5_ui_sound_mix(buf, PS5_AUDIO_GRAIN);
        sceAudioOutOutput(ps5_audio_handle, buf);
    }
    return NULL;
}

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 2,
    };

    /* The dummy driver (see main): the stream only needs SDL's audio core */
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    d->monitor.stream = SDL_CreateAudioStream(&spec, &spec);
    if (d->monitor.stream == NULL) {
        error_setg(errp, "SDL_CreateAudioStream failed: %s", SDL_GetError());
        return;
    }

    int rc = sceAudioOutInit();
    ps5_audio_handle =
        sceAudioOutOpen(PS5_AUDIO_USER_SYSTEM, PS5_AUDIO_PORT_MAIN, 0,
                        PS5_AUDIO_GRAIN, spec.freq, PS5_AUDIO_S16_STEREO);
    fprintf(stderr, "xemu PS5 audio: init %#x, open %#x\n", rc,
            ps5_audio_handle);
    if (ps5_audio_handle < 0) {
        error_setg(errp, "sceAudioOutOpen failed: %#x", ps5_audio_handle);
        SDL_DestroyAudioStream(d->monitor.stream);
        d->monitor.stream = NULL;
        return;
    }

    int grain_bytes = PS5_AUDIO_GRAIN * spec.channels *
                      SDL_AUDIO_BYTESIZE(spec.format);
    int drain = MAX(grain_bytes, (int)sizeof(d->monitor.frame_buf));
    d->monitor.queued_bytes_low = drain;
    d->monitor.queued_bytes_high = 3 * drain;

    qatomic_set(&ps5_audio_running, true);
    qemu_thread_create(&ps5_audio_thread, "ps5-audio-out", ps5_audio_out, d,
                       QEMU_THREAD_JOINABLE);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (ps5_audio_handle >= 0) {
        qatomic_set(&ps5_audio_running, false);
        qemu_thread_join(&ps5_audio_thread);
        sceAudioOutClose(ps5_audio_handle);
        ps5_audio_handle = -1;
    }
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
    }
}
#else
void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 2,
    };

    d->monitor.stream = NULL;

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    d->monitor.stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (d->monitor.stream == NULL) {
        error_setg(errp, "SDL_OpenAudioDeviceStream failed: %s",
                   SDL_GetError());
        return;
    }

    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(d->monitor.stream);

    SDL_AudioSpec dev_spec;
    int dev_buf_frames = 0;
    int dev_drain_bytes = 0;
    if (SDL_GetAudioDeviceFormat(dev, &dev_spec, &dev_buf_frames)) {
        dev_drain_bytes = dev_buf_frames * spec.channels *
                          SDL_AUDIO_BYTESIZE(spec.format) *
                          spec.freq / dev_spec.freq;
    }
    int frame_bytes = sizeof(d->monitor.frame_buf);
    int drain = MAX(dev_drain_bytes, frame_bytes);
    d->monitor.queued_bytes_low = drain;
    d->monitor.queued_bytes_high = 3 * drain;

    SDL_ResumeAudioDevice(dev);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
    }
}
#endif

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    if (d->monitor.stream) {
        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
#ifdef __PROSPERO__
        vu *= g_xemu_apu_ui_gain;
#endif
        SDL_SetAudioStreamGain(d->monitor.stream, vu);
        SDL_PutAudioStreamData(d->monitor.stream, d->monitor.frame_buf,
                            sizeof(d->monitor.frame_buf));
    }

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}
