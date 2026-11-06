/*
 * hakuX native core — setjmp/longjmp for the fault scopes (jmp.h).
 * Layout: x18-x30 at 0..0x60, sp at 0x68, d8-d15 at 0x70..0xA8.
 */
#include "native/core/jmp.h"

#if defined(__x86_64__)
/* x86-64: rbx, rbp, r12-r15, rsp and the return address; x87 control word and
 * MXCSR. The signal mask stays alone, as on ARM. */
__attribute__((naked, returns_twice)) int nc_setjmp(nc_jmp_buf env)
{
    __asm__ volatile("movq %rbx, 0x00(%rdi)\n"
                     "movq %rbp, 0x08(%rdi)\n"
                     "movq %r12, 0x10(%rdi)\n"
                     "movq %r13, 0x18(%rdi)\n"
                     "movq %r14, 0x20(%rdi)\n"
                     "movq %r15, 0x28(%rdi)\n"
                     "leaq 8(%rsp), %rax\n"
                     "movq %rax, 0x30(%rdi)\n"
                     "movq (%rsp), %rax\n"
                     "movq %rax, 0x38(%rdi)\n"
                     "fnstcw 0x40(%rdi)\n"
                     "stmxcsr 0x48(%rdi)\n"
                     "xorl %eax, %eax\n"
                     "ret\n");
}

__attribute__((naked, noreturn)) void nc_longjmp(nc_jmp_buf env, int val)
{
    __asm__ volatile("movq 0x00(%rdi), %rbx\n"
                     "movq 0x08(%rdi), %rbp\n"
                     "movq 0x10(%rdi), %r12\n"
                     "movq 0x18(%rdi), %r13\n"
                     "movq 0x20(%rdi), %r14\n"
                     "movq 0x28(%rdi), %r15\n"
                     "movq 0x30(%rdi), %rsp\n"
                     "fldcw 0x40(%rdi)\n"
                     "ldmxcsr 0x48(%rdi)\n"
                     "movl %esi, %eax\n"
                     "testl %eax, %eax\n"
                     "jnz 1f\n"
                     "incl %eax\n" /* 0 comes back as 1 */
                     "1:\n"
                     "jmpq *0x38(%rdi)\n");
}
#else

__attribute__((naked, returns_twice)) int nc_setjmp(nc_jmp_buf env)
{
    __asm__ volatile("stp x18, x19, [x0, #0x00]\n"
                     "stp x20, x21, [x0, #0x10]\n"
                     "stp x22, x23, [x0, #0x20]\n"
                     "stp x24, x25, [x0, #0x30]\n"
                     "stp x26, x27, [x0, #0x40]\n"
                     "stp x28, x29, [x0, #0x50]\n"
                     "mov x1, sp\n"
                     "stp x30, x1, [x0, #0x60]\n"
                     "stp d8, d9, [x0, #0x70]\n"
                     "stp d10, d11, [x0, #0x80]\n"
                     "stp d12, d13, [x0, #0x90]\n"
                     "stp d14, d15, [x0, #0xa0]\n"
                     "mov w0, #0\n"
                     "ret\n");
}

__attribute__((naked, noreturn)) void nc_longjmp(nc_jmp_buf env, int val)
{
    __asm__ volatile("ldp x18, x19, [x0, #0x00]\n"
                     "ldp x20, x21, [x0, #0x10]\n"
                     "ldp x22, x23, [x0, #0x20]\n"
                     "ldp x24, x25, [x0, #0x30]\n"
                     "ldp x26, x27, [x0, #0x40]\n"
                     "ldp x28, x29, [x0, #0x50]\n"
                     "ldp x30, x2, [x0, #0x60]\n"
                     "mov sp, x2\n"
                     "ldp d8, d9, [x0, #0x70]\n"
                     "ldp d10, d11, [x0, #0x80]\n"
                     "ldp d12, d13, [x0, #0x90]\n"
                     "ldp d14, d15, [x0, #0xa0]\n"
                     "cmp w1, #0\n"
                     "csinc w0, w1, wzr, ne\n" /* 0 comes back as 1 */
                     "ret\n");
}
#endif
