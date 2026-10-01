//
// XPSemu: the dashboard's sounds
//
// XPSemu's own sounds: recordings embedded in the app (ps5/sounds, made
// into ui-sounds-data.h by ps5/sounds/embed-sounds.py), and the rest
// synthesized here (glassy ticks and chimes, airy whooshes). A WAV in
// /data/xemu/sounds replaces either. They're mixed into the console audio
// output by the audio thread (hw/xbox/mcpx/apu/monitor.c), which keeps
// running, playing silence, while the emulated Xbox is paused behind the
// dashboard.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <algorithm>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "ui-sounds.hh"
#include "ui-sounds-data.h"
#include "../xemu-settings.h"

static const int kRate = 48000;

// Interleaved stereo, -1 .. 1.
typedef std::vector<float> Samples;

//
// Synthesis
//

struct Synth {
    Samples out;
    explicit Synth(float seconds) : out((size_t)(seconds * kRate) * 2, 0) {}
    size_t frames() const { return out.size() / 2; }

    // A tone from start (s) with an attack, then an exponential decay (tau,
    // s), its pitch gliding from f0 to f1, a little louder on one side.
    void Tone(float start, float length, float f0, float f1, float amp,
              float attack, float tau, float pan = 0, float harmonic = 0)
    {
        double phase = 0;
        size_t first = (size_t)(start * kRate);
        size_t n = (size_t)(length * kRate);
        for (size_t i = 0; i < n && first + i < frames(); i++) {
            float t = (float)i / kRate;
            float f = f0 + (f1 - f0) * (t / length);
            phase += 2 * M_PI * f / kRate;
            float env = (t < attack ? t / attack : 1) * expf(-t / tau);
            float v = sinf(phase) + harmonic * sinf(2 * phase) +
                      harmonic * 0.5f * sinf(3 * phase);
            v *= amp * env;
            out[(first + i) * 2] += v * (1 - pan) * 0.5f * 2;
            out[(first + i) * 2 + 1] += v * (1 + pan) * 0.5f * 2;
        }
    }

    // Filtered noise (a whoosh): a one-pole low-pass whose cutoff glides
    // from c0 to c1 Hz, shaped by a rise then fall envelope.
    void Whoosh(float start, float length, float c0, float c1, float amp,
                float peak)
    {
        uint32_t seed = 12345;
        float lp = 0, lp2 = 0;
        size_t first = (size_t)(start * kRate);
        size_t n = (size_t)(length * kRate);
        for (size_t i = 0; i < n && first + i < frames(); i++) {
            float t = (float)i / n;
            seed = seed * 1664525u + 1013904223u;
            float white = ((seed >> 9) / 8388608.0f) - 1;
            float cutoff = c0 + (c1 - c0) * t;
            float k = 1 - expf(-2 * M_PI * cutoff / kRate);
            lp += k * (white - lp);
            lp2 += k * (lp - lp2);
            float band = lp - lp2 * 0.6f; // Less rumble
            float env = t < peak ? t / peak : (1 - t) / (1 - peak);
            env = env * env;
            float v = band * amp * env * 2.5f;
            // Sweep across the stereo field as it goes.
            out[(first + i) * 2] += v * (1.2f - t);
            out[(first + i) * 2 + 1] += v * (0.2f + t);
        }
    }

    // A simple echo, for space.
    void Echo(float delay, float feedback)
    {
        size_t d = (size_t)(delay * kRate) * 2;
        for (size_t i = d; i < out.size(); i++) {
            out[i] += out[i - d] * feedback;
        }
    }
};

static Samples Make(UiSound sound)
{
    switch (sound) {
    case UI_SOUND_MOVE: { // A short glassy tick
        Synth s(0.09f);
        s.Tone(0, 0.08f, 2090, 2000, 0.30f, 0.001f, 0.018f, 0, 0.25f);
        s.Tone(0, 0.06f, 3140, 3100, 0.10f, 0.001f, 0.010f);
        return s.out;
    }
    case UI_SOUND_CHANGE: { // A lower, softer tick
        Synth s(0.09f);
        s.Tone(0, 0.08f, 1480, 1400, 0.28f, 0.001f, 0.020f, 0, 0.3f);
        return s.out;
    }
    case UI_SOUND_SELECT: { // Two rising chime notes
        Synth s(0.45f);
        s.Tone(0.000f, 0.25f, 988, 988, 0.26f, 0.002f, 0.06f, -0.3f, 0.2f);
        s.Tone(0.055f, 0.35f, 1480, 1480, 0.26f, 0.002f, 0.09f, 0.3f, 0.2f);
        s.Tone(0.055f, 0.35f, 2960, 2960, 0.05f, 0.002f, 0.05f, 0.3f);
        s.Echo(0.085f, 0.22f);
        return s.out;
    }
    case UI_SOUND_BACK: { // A falling blip with a puff of air
        Synth s(0.25f);
        s.Tone(0, 0.14f, 1250, 560, 0.26f, 0.002f, 0.05f, 0, 0.2f);
        s.Whoosh(0, 0.18f, 2500, 600, 0.10f, 0.15f);
        return s.out;
    }
    case UI_SOUND_OPEN: { // A soft rising whoosh and a low chord
        Synth s(0.9f);
        s.Whoosh(0, 0.55f, 300, 3500, 0.22f, 0.7f);
        s.Tone(0.25f, 0.6f, 330, 330, 0.12f, 0.12f, 0.25f, -0.4f);
        s.Tone(0.25f, 0.6f, 494, 494, 0.10f, 0.12f, 0.25f, 0.4f);
        s.Tone(0.30f, 0.5f, 1976, 1976, 0.04f, 0.05f, 0.12f);
        s.Echo(0.12f, 0.25f);
        return s.out;
    }
    case UI_SOUND_LAUNCH: { // A big swell into a shimmering chord
        Synth s(1.6f);
        s.Whoosh(0, 0.9f, 200, 6000, 0.30f, 0.85f);
        const float chord[] = { 329.6f, 415.3f, 493.9f, 659.3f, 987.8f };
        for (int i = 0; i < 5; i++) {
            s.Tone(0.7f + i * 0.03f, 0.8f, chord[i], chord[i] * 1.003f,
                   0.10f, 0.01f, 0.30f, i % 2 ? 0.4f : -0.4f, 0.15f);
        }
        s.Tone(0.7f, 0.6f, 1975.5f, 1975.5f, 0.05f, 0.005f, 0.12f);
        s.Echo(0.14f, 0.3f);
        return s.out;
    }
    case UI_SOUND_ERROR: { // Two dull low bumps
        Synth s(0.3f);
        s.Tone(0.00f, 0.09f, 220, 200, 0.28f, 0.003f, 0.04f, 0, 0.6f);
        s.Tone(0.12f, 0.09f, 200, 180, 0.28f, 0.003f, 0.04f, 0, 0.6f);
        return s.out;
    }
    default:
        return {};
    }
}

// Any WAV SDL reads (from a file or memory; the stream is closed),
// converted to 48 kHz stereo.
static bool LoadWav(SDL_IOStream *io, const char *what, Samples *out)
{
    SDL_AudioSpec spec;
    Uint8 *data = NULL;
    Uint32 len = 0;
    if (!io || !SDL_LoadWAV_IO(io, true, &spec, &data, &len)) {
        return false;
    }
    SDL_AudioSpec want = { SDL_AUDIO_F32, 2, kRate };
    Uint8 *conv = NULL;
    int conv_len = 0;
    bool ok = SDL_ConvertAudioSamples(&spec, data, (int)len, &want, &conv,
                                      &conv_len);
    SDL_free(data);
    if (!ok) {
        fprintf(stderr, "XPSemu: sound %s: %s\n", what, SDL_GetError());
        return false;
    }
    const float *f = (const float *)conv;
    out->assign(f, f + conv_len / sizeof(float));
    SDL_free(conv);
    return true;
}

//
// Playing
//

static const char *const kNames[UI_SOUND__COUNT] = {
    "move", "change", "select", "back", "open", "launch", "error",
};

static std::mutex g_lock;
static Samples g_sounds[UI_SOUND__COUNT];
static bool g_made[UI_SOUND__COUNT];

struct Voice {
    const Samples *samples;
    size_t pos; // In samples (both channels)
};
static Voice g_voices[6];

bool g_ui_sound_played;

void UiSoundPlay(UiSound sound)
{
    g_ui_sound_played = true;
    if (!g_config.display.ui.menu_sounds || sound >= UI_SOUND__COUNT) {
        return;
    }
    if (!g_made[sound]) { // On the UI thread; the mixer only reads voices
        // The user's own, else the recording in the app, else synthesized.
        std::string file = std::string(xemu_settings_get_base_path()) +
                           "sounds/" + kNames[sound] + ".wav";
        const auto &embedded = kEmbeddedSounds[sound];
        Samples s;
        if (LoadWav(SDL_IOFromFile(file.c_str(), "rb"), file.c_str(), &s)) {
        } else if (embedded.data &&
                   LoadWav(SDL_IOFromConstMem(embedded.data, embedded.size),
                           kNames[sound], &s)) {
            // Recordings are made to be as loud as they sound at full
            // scale; the mixer's gain is for the synthesized ones.
            for (float &v : s) {
                v *= 1.25f;
            }
        } else {
            s = Make(sound);
        }
        std::lock_guard<std::mutex> guard(g_lock);
        g_sounds[sound] = std::move(s);
        g_made[sound] = true;
    }

    std::lock_guard<std::mutex> guard(g_lock);
    // A free voice, else the one nearest its end.
    Voice *best = &g_voices[0];
    for (Voice &v : g_voices) {
        if (!v.samples) {
            best = &v;
            break;
        }
        if (v.pos > best->pos) {
            best = &v;
        }
    }
    // The same sound again (fast scrolling) replaces its last voice.
    for (Voice &v : g_voices) {
        if (v.samples == &g_sounds[sound] && v.pos < 2 * kRate / 50) {
            best = &v;
        }
    }
    best->samples = &g_sounds[sound];
    best->pos = 0;
}

// Called by the audio thread for each block it's about to output.
extern "C" void xemu_ps5_ui_sound_mix(int16_t *buf, int frames)
{
    std::lock_guard<std::mutex> guard(g_lock);
    const float gain = 0.32f; // Under the games: 60% down from 0.8
    for (Voice &v : g_voices) {
        if (!v.samples) {
            continue;
        }
        size_t n = std::min((size_t)frames * 2, v.samples->size() - v.pos);
        const float *src = v.samples->data() + v.pos;
        for (size_t i = 0; i < n; i++) {
            int mixed = buf[i] + (int)(src[i] * gain * 32767);
            buf[i] = (int16_t)(mixed > 32767 ? 32767 :
                               mixed < -32768 ? -32768 : mixed);
        }
        v.pos += n;
        if (v.pos >= v.samples->size()) {
            v.samples = NULL;
        }
    }
}
