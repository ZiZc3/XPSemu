/*
 * hakuX native core — setjmp/longjmp for the fault scopes (kernel.h).
 *
 * Bionic's _setjmp disables SME's ZA state on every call (__arm_za_disable)
 * where the CPU has SME, which made it 14% of the guest thread when every
 * kernel call and HLE hook entered a scope. These save only the AAPCS64
 * callee-saved registers (x18-x30, sp, d8-d15) and leave the signal mask
 * alone: the fault handler unblocks its signals before jumping.
 */
#ifndef NC_JMP_H
#define NC_JMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t nc_jmp_buf[24];

__attribute__((returns_twice)) int nc_setjmp(nc_jmp_buf env);
__attribute__((noreturn)) void nc_longjmp(nc_jmp_buf env, int val);

#ifdef __cplusplus
}
#endif

#endif
