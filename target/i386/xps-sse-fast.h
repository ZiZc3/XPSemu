/*
 * XPSemu: more of the Xbox's SSE float math on the host's SSE (the PS5 is
 * x86 too): min/max, sqrt, rcp/rsqrt and compares, ps (4 lanes) and ss
 * (lane 0, the other lanes from v). Each one only runs where its result
 * and flags are provably the same as softfloat's (no NaN, no denormal,
 * round to nearest, results that can't underflow); it returns false
 * otherwise, before touching d, and the caller takes the emulated way.
 */
#ifndef XPS_SSE_FAST_H
#define XPS_SSE_FAST_H

#include <immintrin.h>
#include <string.h>

/* Zero, normal or infinity: not NaN, not denormal. */
static inline bool xps_lane_plain(uint32_t x)
{
    uint32_t e = x & 0x7f800000, m = x & 0x007fffff;
    return e == 0x7f800000 ? m == 0 : (e != 0 || m == 0);
}

/* Normal (not zero, denormal, infinity or NaN). */
static inline bool xps_lane_normal(uint32_t x)
{
    uint32_t e = x & 0x7f800000;
    return e != 0 && e != 0x7f800000;
}

/*
 * minps/maxps/minss/maxss: softfloat's float32_lt(a, b) ? a : b (max:
 * lt(b, a)) is Intel's definition, which the host's instruction follows.
 * Without NaNs and denormals the compare raises no flag.
 */
static inline bool xps_sse_minmax(int max, void *d, const void *v,
                                  const void *s, int lanes)
{
    uint32_t a[4], b[4];
    __m128 va = _mm_loadu_ps((const float *)v);
    __m128 vb = _mm_loadu_ps((const float *)s);
    __m128 vr;
    int i;

    _mm_storeu_ps((float *)a, va);
    _mm_storeu_ps((float *)b, vb);
    for (i = 0; i < lanes; i++) {
        if (!xps_lane_plain(a[i]) || !xps_lane_plain(b[i])) {
            return false;
        }
    }
    if (lanes == 1) {
        vr = max ? _mm_max_ss(va, vb) : _mm_min_ss(va, vb);
    } else {
        vr = max ? _mm_max_ps(va, vb) : _mm_min_ps(va, vb);
    }
    _mm_storeu_ps((float *)d, vr);
    return true;
}

/*
 * sqrtps (lanes 4, v unused) / sqrtss: of +normal, +inf or a zero the
 * result is exact-or-inexact and never tiny; with inexact already raised
 * no new flag can arise.
 */
static inline bool xps_sse_sqrt(float_status *st, void *d, const void *v,
                                const void *s, int lanes)
{
    uint32_t x[4];
    __m128 vs = _mm_loadu_ps((const float *)s), vr;
    int i;

    if (!(st->float_exception_flags & float_flag_inexact) ||
        st->float_rounding_mode != float_round_nearest_even) {
        return false;
    }
    _mm_storeu_ps((float *)x, vs);
    for (i = 0; i < lanes; i++) {
        if (!(x[i] & 0x7fffffff)) {
            continue; /* +-0 */
        }
        if ((x[i] & 0x80000000) || !xps_lane_plain(x[i])) {
            return false;
        }
    }
    if (lanes == 1) {
        vr = _mm_move_ss(_mm_loadu_ps((const float *)v), _mm_sqrt_ss(vs));
    } else {
        vr = _mm_sqrt_ps(vs);
    }
    _mm_storeu_ps((float *)d, vr);
    return true;
}

/*
 * rcpps/rcpss and rsqrtps/rsqrtss as QEMU does them: 1 / x and
 * 1 / sqrt(x), each step correctly rounded, flags put back afterwards
 * (so only the values matter). Normal x (rsqrt: positive), and a normal
 * result above FLT_MIN.
 */
static inline bool xps_sse_recip(float_status *st, int root, void *d,
                                 const void *v, const void *s, int lanes)
{
    uint32_t x[4], r[4];
    __m128 vs = _mm_loadu_ps((const float *)s), vr;
    __m128 one = _mm_set1_ps(1.0f);
    int i;

    if (st->float_rounding_mode != float_round_nearest_even) {
        return false;
    }
    _mm_storeu_ps((float *)x, vs);
    for (i = 0; i < lanes; i++) {
        if (!xps_lane_normal(x[i]) || (root && (x[i] & 0x80000000))) {
            return false;
        }
    }
    if (lanes == 1) {
        vr = _mm_div_ss(one, root ? _mm_sqrt_ss(vs) : vs);
    } else {
        vr = _mm_div_ps(one, root ? _mm_sqrt_ps(vs) : vs);
    }
    _mm_storeu_ps((float *)r, vr);
    for (i = 0; i < lanes; i++) {
        uint32_t m = r[i] & 0x7fffffff;
        if (m <= 0x00800000 || m >= 0x7f800000) {
            return false;
        }
    }
    if (lanes == 1) {
        vr = _mm_move_ss(_mm_loadu_ps((const float *)v), vr);
    }
    _mm_storeu_ps((float *)d, vr);
    return true;
}

/*
 * cmpXXps/ss: the relation of each lane pair (as float32_compare and
 * float32_compare_quiet give it) when neither is NaN or denormal; then
 * neither compare raises a flag.
 */
static inline bool xps_sse_rel(const void *v, const void *s, int lanes,
                               int *rel)
{
    uint32_t a[4], b[4];
    float fa, fb;
    int i;

    memcpy(a, v, sizeof(a));
    memcpy(b, s, sizeof(b));
    for (i = 0; i < lanes; i++) {
        if (!xps_lane_plain(a[i]) || !xps_lane_plain(b[i])) {
            return false;
        }
    }
    for (i = 0; i < lanes; i++) {
        memcpy(&fa, &a[i], 4);
        memcpy(&fb, &b[i], 4);
        rel[i] = fa < fb ? float_relation_less :
                 fa == fb ? float_relation_equal : float_relation_greater;
    }
    return true;
}

#endif
