/* The native core's HRTF filter (native/dev/apu/vp/hrtf.h) against xemu's
 * scalar one (the reference below, as it was): the same output within float
 * rounding, over random audio with the parameters moving (new targets every
 * few frames, delays from 0 to the 42-sample limit, fractional and whole)
 * and settled. Built for the host and, with the NDK, for the device (its
 * NEON path). */
#include "hle_test.h"

#include <math.h>
#include <stdlib.h>

#include "native/dev/apu/vp/hrtf.h"

/* ---- xemu's filter */
#define REF_BUFLEN (HRTF_NUM_TAPS + HRTF_MAX_DELAY_SAMPLES)

typedef struct {
    int buf_pos;
    struct {
        float buf[REF_BUFLEN];
        float hrir_coeff_cur[HRTF_NUM_TAPS];
        float hrir_coeff_tar[HRTF_NUM_TAPS];
    } ch[2];
    float itd_cur;
    float itd_tar;
} RefFilter;

static void ref_set_target_params(RefFilter *f, float hrir_coeff[2][HRTF_NUM_TAPS], float itd)
{
    f->itd_tar = fmaxf(-HRTF_MAX_DELAY_SAMPLES, fminf(itd, HRTF_MAX_DELAY_SAMPLES));
    for (int ch = 0; ch < 2; ch++) {
        float *coeff = f->ch[ch].hrir_coeff_tar;
        memcpy(coeff, hrir_coeff[ch], sizeof(f->ch[ch].hrir_coeff_tar));
        float s = 0.0f;
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            s += fabsf(coeff[k]);
        }
        if (s == 0.0f || s == 1.0f) {
            break;
        }
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            coeff[k] /= s;
        }
    }
}

static float ref_smooth(float cur, float tar)
{
    return cur + HRTF_PARAM_SMOOTH_ALPHA * (tar - cur);
}

static void ref_process(RefFilter *f, float in[HRTF_SAMPLES_PER_FRAME][2], float out[HRTF_SAMPLES_PER_FRAME][2])
{
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        for (int ch = 0; ch < 2; ch++) {
            for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                f->ch[ch].hrir_coeff_cur[k] = ref_smooth(f->ch[ch].hrir_coeff_cur[k], f->ch[ch].hrir_coeff_tar[k]);
            }
        }
        f->itd_cur = ref_smooth(f->itd_cur, f->itd_tar);
        for (int ch = 0; ch < 2; ch++) {
            float *buf = f->ch[ch].buf;
            float *coeff = f->ch[ch].hrir_coeff_cur;
            buf[f->buf_pos] = in[n][ch];
            float d = f->itd_cur * (ch == 0 ? +1.0f : -1.0f);
            if (d < 0.0f) {
                d = 0.0f;
            }
            int di = d;
            float dfrac = d - di;
            float acc = 0.0f;
            for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                int idx1 = (f->buf_pos - di - k + REF_BUFLEN) % REF_BUFLEN;
                float s = buf[idx1];
                if (dfrac > 0.0f) {
                    int idx2 = (idx1 - 1 + REF_BUFLEN) % REF_BUFLEN;
                    s = s * (1 - dfrac) + buf[idx2] * dfrac;
                }
                acc += coeff[k] * s;
            }
            out[n][ch] = acc;
        }
        f->buf_pos = (f->buf_pos + 1) % REF_BUFLEN;
    }
}

/* ---- the run */
static uint32_t g_rng = 12345;

static float rnd(void)   /* [-1, 1) */
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(int32_t)g_rng / 2147483648.0f;
}

static void random_hrir(float h[2][HRTF_NUM_TAPS])
{
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            h[ch][k] = rnd() * (k < 8 ? 1.0f : 0.25f);
        }
    }
}

static double g_max_err;

static void run(int frames, int retarget_every, const float *itds, int n_itds, bool settle)
{
    static RefFilter ref;
    static HrtfFilter f;
    memset(&ref, 0, sizeof(ref));
    hrtf_filter_init(&f);
    float h[2][HRTF_NUM_TAPS];
    int itd_i = 0;
    for (int fr = 0; fr < frames; fr++) {
        if (fr % retarget_every == 0 && !(settle && fr > 0)) {
            random_hrir(h);
            const float itd = itds[itd_i++ % n_itds];
            ref_set_target_params(&ref, h, itd);
            hrtf_filter_set_target_params(&f, h, itd);
        }
        float in[HRTF_SAMPLES_PER_FRAME][2], a[HRTF_SAMPLES_PER_FRAME][2], b[HRTF_SAMPLES_PER_FRAME][2];
        for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
            in[n][0] = rnd();
            in[n][1] = rnd();
        }
        memcpy(b, in, sizeof(in));
        ref_process(&ref, in, a);
        hrtf_filter_process(&f, b, b);      /* in place, as the VP calls it */
        for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
            for (int ch = 0; ch < 2; ch++) {
                const double e = fabs((double)a[n][ch] - (double)b[n][ch]);
                if (e > g_max_err) {
                    g_max_err = e;
                }
            }
        }
    }
    /* the parameters moved the same way */
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            HT_CHECK(fabsf(ref.ch[ch].hrir_coeff_cur[k] - f.ch[ch].hrir_coeff_cur[HRTF_NUM_TAPS - 1 - k]) < 1e-6f);
        }
        HT_CHECK(f.ch[ch].hrir_coeff_cur[HRTF_NUM_TAPS] == 0.0f);
    }
    HT_CHECK(fabsf(ref.itd_cur - f.itd_cur) < 1e-5f);
    HT_EQ_INT(f.buf_pos, ref.buf_pos);
}

int main(void)
{
    const float moving[] = {0.0f, 7.25f, -13.5f, 42.0f, -42.0f, 41.9f, -0.3f, 30.0f, 100.0f, -100.0f};
    const float whole[] = {0.0f, 5.0f, -12.0f, 42.0f, -42.0f};
    run(400, 3, moving, 10, false);         /* parameters always moving */
    run(400, 40, moving, 10, false);        /* moving, then near their targets */
    run(3000, 1, whole, 5, true);           /* one target: settles (the fast path) */
    run(3000, 1, moving + 1, 1, true);      /* settles on a fractional delay */
    run(3000, 1, moving + 5, 1, true);      /* ... next to the delay limit */
    fprintf(stderr, "hrtf: max |difference| %.3g\n", g_max_err);
    HT_CHECK(g_max_err < 2e-6);
    return ht_failures != 0;
}
