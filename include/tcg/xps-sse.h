/*
 * XPSemu: the Xbox's SSE in the translated code as the host's own SSE
 * instructions (both CPUs are x86), instead of a helper call per guest
 * instruction. Selectors of the xps_sse_* TCG ops (tcg/i386 encodes them).
 */
#ifndef TCG_XPS_SSE_H
#define TCG_XPS_SSE_H

#if defined(__PROSPERO__) || defined(XPS_RIG)
#define XPS_SSE_INLINE 1
#endif

enum {
    /* xps_sse_vec: r = op(a, b) on xmm registers */
    XPSV_ADD, XPSV_SUB, XPSV_MUL, XPSV_DIV, XPSV_MIN, XPSV_MAX,
    XPSV_SQRT,      /* ps: r = sqrt(b); ss: lane 0 sqrt(b), rest from a */
    XPSV_CMP,       /* predicate in XPSV_IMM */
    XPSV_CVTPS2DQ,  /* r = cvtps2dq(b) */
    XPSV_CVTTPS2DQ, /* r = cvttps2dq(b) */
    XPSV_CVTPI2PS,  /* r = a with lanes 0-1 = cvtdq2ps(b) lanes 0-1 */
    XPSV_PACKSSWB,  /* MMX: r = pack(a's 64 bits, b's 64 bits) */
    XPSV_PACKUSWB,
    XPSV_PACKSSDW,
    /* xps_sse_x2r: r (i32) = op(a) */
    XPSR_CVTSS2SI,
    XPSR_CVTTSS2SI,
    /* xps_sse_r2x: r = a with lane 0 = op(b (i32)) */
    XPSX_CVTSI2SS,
};
#define XPSV_SS       0x100   /* scalar (lane 0; the others from a) */
#define XPSV_IMM(i)   ((i) << 16)

#endif
