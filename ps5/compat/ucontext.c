/*
 * User-space getcontext/makecontext/swapcontext for QEMU's coroutines.
 *
 * On the PS5, getcontext and swapcontext are libkernel system calls (as on
 * FreeBSD) and makecontext comes from libc, and a title's sandbox may refuse
 * the calls. QEMU's ucontext coroutine backend only uses them to start a
 * coroutine on its own stack (util/coroutine-ucontext.c: getcontext, then
 * makecontext, then one swapcontext in); every later switch is sigsetjmp and
 * siglongjmp. So these do just that, with no system calls: the link wraps
 * the three names (ps5/build-title.sh).
 *
 * The registers live at the start of uc_mcontext, which only these touch.
 * Signal masks and FPU state are left alone: every switch stays on one thread.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

struct ctx {
    uint64_t rsp, rip, rbx, rbp, r12, r13, r14, r15, rdi, rsi;
};
_Static_assert(sizeof(struct ctx) <= sizeof(mcontext_t),
               "the registers fit in uc_mcontext");

#define CTX(ucp) ((struct ctx *)&(ucp)->uc_mcontext)

/* Saves the callee-saved registers in save, then resumes load. */
void ps5_ctx_switch(struct ctx *save, const struct ctx *load);
__asm__(
    ".text\n"
    ".globl ps5_ctx_switch\n"
    ".type ps5_ctx_switch, @function\n"
    "ps5_ctx_switch:\n"
    "    movq (%rsp), %rax\n"       /* resume at our return address, */
    "    leaq 8(%rsp), %rcx\n"      /* with the stack as after ret */
    "    movq %rcx, 0(%rdi)\n"
    "    movq %rax, 8(%rdi)\n"
    "    movq %rbx, 16(%rdi)\n"
    "    movq %rbp, 24(%rdi)\n"
    "    movq %r12, 32(%rdi)\n"
    "    movq %r13, 40(%rdi)\n"
    "    movq %r14, 48(%rdi)\n"
    "    movq %r15, 56(%rdi)\n"
    "    movq 16(%rsi), %rbx\n"
    "    movq 24(%rsi), %rbp\n"
    "    movq 32(%rsi), %r12\n"
    "    movq 40(%rsi), %r13\n"
    "    movq 48(%rsi), %r14\n"
    "    movq 56(%rsi), %r15\n"
    "    movq 0(%rsi), %rsp\n"
    "    movq 8(%rsi), %rax\n"
    "    movq 64(%rsi), %rdi\n"     /* a new context's two arguments */
    "    movq 72(%rsi), %rsi\n"
    "    jmp *%rax\n"
    ".size ps5_ctx_switch, .-ps5_ctx_switch\n");

/* Where a context's function returns to; QEMU's never do. */
static void context_returned(void)
{
    abort();
}

int __wrap_getcontext(ucontext_t *ucp)
{
    memset(CTX(ucp), 0, sizeof(struct ctx));
    return 0;
}

void __wrap_makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...)
{
    /* QEMU passes two ints (a pointer split in halves). */
    if (argc > 2) {
        abort();
    }
    uint64_t args[2] = { 0, 0 };
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++) {
        args[i] = (uint64_t)(unsigned int)va_arg(ap, int);
    }
    va_end(ap);

    /* At the function's entry, rsp + 8 is 16-byte aligned (the SysV ABI). */
    uintptr_t top = (uintptr_t)ucp->uc_stack.ss_sp + ucp->uc_stack.ss_size;
    uint64_t *sp = (uint64_t *)((top & ~(uintptr_t)15) - 8);
    *sp = (uint64_t)(uintptr_t)context_returned;

    struct ctx *c = CTX(ucp);
    memset(c, 0, sizeof(*c));
    c->rsp = (uint64_t)(uintptr_t)sp;
    c->rip = (uint64_t)(uintptr_t)func;
    c->rdi = args[0];
    c->rsi = args[1];
}

int __wrap_swapcontext(ucontext_t *oucp, const ucontext_t *ucp)
{
    ps5_ctx_switch(CTX(oucp), CTX(ucp));
    return 0;
}
