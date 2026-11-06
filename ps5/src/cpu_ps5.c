/*
 * XOG: the guest CPU on the PS5. Xbox code runs directly on the console's
 * CPU in 32-bit compatibility mode; this file is hakuX's native/cpu/cpu.h for
 * that. Our own code. GPL-3.0-or-later.
 *
 * Measured on the console (probe 2): 32-bit code at 1.000x 64-bit speed; ud2 and
 * page-fault traps about 5 us; int3, cli and int n about 11 us.
 *
 * First version: every way out of the guest is a signal on the guest's thread,
 * and the kernel runs inside that signal, on an alternate stack. A far-call gate
 * for kernel calls (0.062 us measured) comes in the speed step.
 *
 * The interrupted FPU state is an image in the signal's context, which the signal's
 * return loads (first console run: a handler's own FXSAVE saw host values, and its
 * FXRSTOR was overwritten). The start-up check finds the image, by a value the guest
 * puts in xmm0, and tests it and fs/gs on the console, and logs the result.
 */
#include "native/cpu/cpu.h"
#include "native/core/core.h"
#include "native/core/mmio.h"
#include "native/core/vm.h"
#include "native/kernel/kernel.h"

#include <errno.h>
#include <machine/sysarch.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <ucontext.h>
#include <unistd.h>

#include <ps5platform/kernel.h>

#include "xog_cpu.h"

#define RX_GUEST_CS 0x33u
#define RX_ALTSTACK (8u << 20)
#define RX_EFLAGS_ARITH 0x00000CD5u /* CF PF AF ZF SF DF OF */
#define RX_EFLAGS_KEPT (RX_EFLAGS_ARITH | 0x00040000u)
#define RX_TF 0x100u
#define RX_T_TRCTRAP 10u    /* the single-step trap; 1 is the invalid-instruction one */
#define RX_IF 0x200u

/* Inside these the guest keeps a call half made: no interrupts there. */
#define RX_NOINT_LO (NC_KERNEL_VA + 0x12000u)
#define RX_NOINT_HI (NC_KERNEL_VA + 0x13000u)
/* A scratch corner of the kernel area for the start-up check. */
#define RX_CHECK_VA (NC_KERNEL_VA + 0x3F000u)

struct NcFrame {
    uint64_t regs[8];
    uint64_t rip;
    uint32_t eflags;
    uint32_t vif;
    uint32_t bypass;
    uint32_t pad;
    uint8_t fx[512] __attribute__((aligned(16)));
};

typedef struct RxContext {
    uint64_t regs[8];
    uint64_t rip;
    uint32_t eflags, vif;
    uint8_t fx[512];
} RxContext;
_Static_assert(sizeof(RxContext) <= sizeof(NcCpuContext), "context size");

typedef struct RxHook {
    uint32_t eip;
    uint8_t orig;
} RxHook;
#define RX_HOOKS 65536u

struct NcCpu {
    NcCpuConfig cfg;
    uint32_t fs_base;
    pthread_t thread;
    uint16_t host_cs, host_ss;
    volatile uint32_t pending;
    uint32_t step_hook;     /* a hook's byte to put back after one step */
    uint32_t image_lo, image_hi;
    uint64_t cycle_offset;
    bool running;
    uint8_t *altstack;
    RxHook *hooks;
};

/* The stub in cpu_ps5_asm.S reaches these two. */
struct NcFrame rx_frame __attribute__((aligned(16)));
uint64_t rx_host_rsp, rx_gate_stack;
volatile uint32_t rx_irq_pending;
uint16_t rx_data_sel, rx_guest_cs = RX_GUEST_CS;
uint32_t rx_host_mxcsr = 0x1F80;

void rx_sig_c(int sig, siginfo_t *si, void *uc);
extern void rx_enter(void);
extern void rx_leave(void);
extern void rx_gate_entry(void), rx_gate_exit(void), rx_resume_end(void);

static NcCpu *g_cpu;
static NcCore *g_core_for_faults;
static volatile int g_prof_want;

/* The profiler's sample of the game's thread (prof_ps5.c). */
void rx_prof_kick(void)
{
    if (g_cpu) {
        g_prof_want = 1;
        pthread_kill(g_cpu->thread, SIGUSR2);
    }
}
static bool g_frame_changed;
static long g_fx_off = -1;      /* the FXSAVE image's place in a ucontext */
/* patch 130 (XOG_PROF): the guest's FPU at the game thread's samples */
static struct { volatile uint32_t n, full, sf, ie, de, used, mxcsr; volatile uint16_t fcw, top; } g_fpu;

void xog_prof_fpu_report(void)
{
    if (g_fpu.n) {
        nc_log("xog-prof", "fpu at %u guest samples: stack full %u, stack fault %u, invalid %u, denormal %u, regs in use %.1f | "
               "last fcw %04x mxcsr %08x top %u", g_fpu.n, g_fpu.full, g_fpu.sf, g_fpu.ie, g_fpu.de,
               (double)g_fpu.used / g_fpu.n, g_fpu.fcw, g_fpu.mxcsr, g_fpu.top);
    }
    g_fpu.n = g_fpu.full = g_fpu.sf = g_fpu.ie = g_fpu.de = g_fpu.used = 0;
}
static volatile int g_in_check;
static int g_check_int3s;

/* The console's signal context: FreeBSD's mcontext, at +0x40 of the ucontext. */
typedef struct RxMc {
    uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp;
    uint64_t r10, r11, r12, r13, r14, r15;
    uint32_t trapno;
    uint16_t fs, gs;
    uint64_t addr;
    uint32_t flags;
    uint16_t es, ds;
    uint64_t err, rip, cs, rflags, rsp, ss;
} RxMc;
_Static_assert(__builtin_offsetof(RxMc, rip) == 0xa0, "rip");
_Static_assert(__builtin_offsetof(RxMc, cs) == 0xa8, "cs: the stub reads it at +0xe8 of the ucontext");

static inline RxMc *mc_of(void *uc)
{
    return (RxMc *)((char *)uc + 0x40);
}

static inline uint64_t rx_rdtsc(void)
{
    uint32_t a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}

/* ---- the frame ---- */

uint32_t nc_frame_gpr(const NcFrame *f, int reg) { return (uint32_t)f->regs[reg]; }
void nc_frame_set_gpr(NcFrame *f, int reg, uint32_t v) { f->regs[reg] = v; }
uint32_t nc_frame_eip(const NcFrame *f) { return (uint32_t)f->rip; }
void nc_frame_set_eip(NcFrame *f, uint32_t eip) { f->rip = eip; }
bool nc_frame_if(const NcFrame *f) { return f->vif != 0; }
void nc_frame_set_if(NcFrame *f, bool on) { f->vif = on; }
uint16_t nc_frame_cs(const NcFrame *f) { (void)f; return 0x08; }
uint32_t nc_frame_eflags(NcFrame *f) { return (f->eflags & ~RX_IF) | (f->vif ? RX_IF : 0) | 2u; }
void nc_frame_set_eflags(NcFrame *f, uint32_t e) { f->eflags = e; f->vif = (e & RX_IF) != 0; }
uint64_t *nc_frame_regs(NcFrame *f) { return f->regs; }
uint64_t *nc_frame_rip(NcFrame *f) { return &f->rip; }
void nc_frame_clear_interrupt(NcFrame *f) { (void)f; rx_irq_pending = 0; }
void nc_frame_bypass_hook(NcFrame *f) { f->bypass = (uint32_t)f->rip; }

void nc_frame_save(const NcFrame *f, NcCpuContext *c)
{
    RxContext *x = (RxContext *)c;
    memcpy(x->regs, f->regs, sizeof(x->regs));
    x->rip = f->rip;
    x->eflags = f->eflags;
    x->vif = f->vif;
    memcpy(x->fx, f->fx, sizeof(x->fx));
}

void nc_frame_load(NcFrame *f, const NcCpuContext *c)
{
    const RxContext *x = (const RxContext *)c;
    memcpy(f->regs, x->regs, sizeof(f->regs));
    f->rip = x->rip;
    f->eflags = x->eflags;
    f->vif = x->vif;
    memcpy(f->fx, x->fx, sizeof(f->fx));
}

/* ---- contexts ---- */

void nc_context_init(NcCpu *cpu, NcCpuContext *c, uint32_t eip, uint32_t esp)
{
    (void)cpu;
    memset(c, 0, sizeof(*c));
    RxContext *x = (RxContext *)c;
    x->rip = eip;
    x->regs[NC_ESP] = esp;
    x->eflags = 0x202;
    x->vif = 1;
    const uint16_t fcw = 0x27F;     /* the Xbox kernel's: double precision */
    const uint32_t mxcsr = 0x1F80, mask = 0xFFFF;
    memcpy(x->fx, &fcw, 2);
    memcpy(x->fx + 24, &mxcsr, 4);
    memcpy(x->fx + 28, &mask, 4);
}

void nc_context_set_gpr(NcCpuContext *c, int reg, uint32_t v) { ((RxContext *)c)->regs[reg] = v; }
uint32_t nc_context_gpr(const NcCpuContext *c, int reg) { return (uint32_t)((const RxContext *)c)->regs[reg]; }
uint32_t nc_context_eip(const NcCpuContext *c) { return (uint32_t)((const RxContext *)c)->rip; }
void nc_context_set_eip(NcCpuContext *c, uint32_t eip) { ((RxContext *)c)->rip = eip; }

void nc_context_export(const NcCpuContext *c, NcCpuState *s)
{
    const RxContext *x = (const RxContext *)c;
    memset(s, 0, sizeof(*s));
    s->rip = x->rip;
    memcpy(s->gregs, x->regs, sizeof(x->regs));
    s->pf_raw = x->eflags;
    s->af_raw = x->vif;
    memcpy(s->xmm, x->fx + 160, 8 * 16);
    memcpy(&s->mxcsr, x->fx + 24, 4);
    memcpy(&s->fcw, x->fx, 2);
    for (int i = 0; i < 8; i++) {
        memcpy(s->mm[i], x->fx + 32 + 16 * i, 16);
    }
}

void nc_context_import(NcCpuContext *c, const NcCpuState *s)
{
    RxContext *x = (RxContext *)c;
    memset(x, 0, sizeof(*x));
    x->rip = s->rip;
    memcpy(x->regs, s->gregs, sizeof(x->regs));
    x->eflags = s->pf_raw;
    x->vif = s->af_raw;
    memcpy(x->fx + 160, s->xmm, 8 * 16);
    memcpy(x->fx + 24, &s->mxcsr, 4);
    memcpy(x->fx, &s->fcw, 2);
    const uint32_t mask = 0xFFFF;
    memcpy(x->fx + 28, &mask, 4);
    for (int i = 0; i < 8; i++) {
        memcpy(x->fx + 32 + 16 * i, s->mm[i], 16);
    }
}

/* ---- hooks: the guest's byte becomes int3 ---- */

static RxHook *hook_slot(NcCpu *cpu, uint32_t eip, bool add)
{
    unsigned h = (eip * 2654435761u) % RX_HOOKS;
    for (unsigned n = 0; n < RX_HOOKS; n++, h = (h + 1) % RX_HOOKS) {
        if (cpu->hooks[h].eip == eip) {
            return &cpu->hooks[h];
        }
        if (cpu->hooks[h].eip == 0) {
            return add ? &cpu->hooks[h] : NULL;
        }
    }
    return NULL;
}

/* Code bytes change through the host's view: the guest's may be write-tracked. */
extern uint8_t *g_va_soft;
extern volatile uint32_t g_steps_n;
extern bool rx_step_active(uint32_t va);
extern void rx_steps_end(NcVm *vm);

extern volatile int rx_pf_depth;

static uint8_t *code_byte(uint32_t va)
{
    NcVm *vm = g_core_for_faults ? &g_core_for_faults->vm : NULL;
    const uint32_t pa = vm ? nc_vm_phys(vm, va) : UINT32_MAX;
    if (pa == UINT32_MAX) {
        return NULL;
    }
    if (g_va_soft[va >> 14] == 2 || rx_step_active(va)) {
        return (uint8_t *)(uintptr_t)va;    /* a shadow or a step: the code that runs is there */
    }
    return vm->phys + pa;
}

static void hook_on(NcCpu *cpu, uint32_t eip)
{
    uint8_t *p = code_byte(eip);
    RxHook *h = p ? hook_slot(cpu, eip, true) : NULL;
    if (!h || h->eip == eip) {
        return;
    }
    h->eip = eip;
    h->orig = *p;
    *p = 0xCC;
}

static void hook_off(NcCpu *cpu, uint32_t eip)
{
    RxHook *h = hook_slot(cpu, eip, false);
    uint8_t *p = code_byte(eip);
    if (h && p && *p == 0xCC) {
        *p = h->orig;
    }
    if (h) {
        h->eip = 0xFFFFFFFFu;   /* the probe chain stays whole */
    }
}

static void hooks_sync(NcCpu *cpu, uint32_t lo, uint32_t hi)
{
    if (!cpu->cfg.hook_query) {
        return;
    }
    for (uint64_t a = lo; a < hi; a++) {
        const uint32_t eip = (uint32_t)a;
        const bool want = cpu->cfg.hook_query(cpu->cfg.opaque, eip);
        const RxHook *h = hook_slot(cpu, eip, false);
        if (want && !h) {
            hook_on(cpu, eip);
        } else if (!want && h) {
            hook_off(cpu, eip);
        }
    }
}

/* ---- the gate's way in: a far call to a 64-bit pad in the kernel area ---- */
static uint32_t g_gate_pad;
static uint16_t g_gate_cs;

void rx_gate_pad(uint8_t *pad, uint32_t pad_va)
{
    uint64_t land = (uint64_t)(uintptr_t)rx_gate_entry;
    pad[0] = 0x49;  /* movabs r11, rx_gate_entry: 32-bit code has nothing in r11 */
    pad[1] = 0xBB;
    memcpy(pad + 2, &land, 8);
    pad[10] = 0x41; /* jmp r11 */
    pad[11] = 0xFF;
    pad[12] = 0xE3;
    g_gate_pad = pad_va;
    __asm__ volatile("mov %%cs, %0" : "=r"(g_gate_cs));
}

/* The far call, at `at`. */
size_t rx_gate_call(uint8_t *at)
{
    at[0] = 0x9A;
    nc_wr32(at + 1, g_gate_pad);
    nc_wr16(at + 5, g_gate_cs);
    return 7;
}

/* ---- the code sweep ----
 * rdtsc counts at the console's rate, not the Xbox's 733 MHz, and doesn't trap. It
 * becomes ud2 in its own two bytes. Found by decoding each code section from its
 * start; a match counts only where the next six instructions decode too. A wrong
 * match writes into data (Burnout 3's movie decoder and loading screen, for one), so
 * nothing else is swept, and sections of data and libraries that never read the TSC
 * are left out.
 *
 * pushfd and popfd see the real IF, not the guest's: after pushfd, cli, popfd the
 * guest's interrupts would stay off. Better than a sweep: a cli that traps proves the
 * code around it is real, so a pushfd right before it and the first popfd after it
 * become int3 then (rx_flags_near_cli). */
enum { RX_K_RDTSC = 1, RX_K_CPUID, RX_K_PUSHFD, RX_K_POPFD };
typedef struct RxSite {
    uint32_t eip;
    uint8_t kind;
} RxSite;
#define RX_SITES 131072u
static RxSite *g_sites;
static unsigned g_nsites, g_sweep_counts[5];
static uint64_t g_tsc_ratio;    /* Xbox Hz / console Hz, 32.32 */

static RxSite *site_slot(uint32_t eip, bool add)
{
    unsigned h = (eip * 2654435761u) % RX_SITES;
    for (unsigned n = 0; n < RX_SITES; n++, h = (h + 1) % RX_SITES) {
        if (g_sites[h].eip == eip) {
            return &g_sites[h];
        }
        if (!g_sites[h].eip) {
            return add ? &g_sites[h] : NULL;
        }
    }
    return NULL;
}

/* An x86 instruction's length in 32-bit mode, or 0 if it doesn't look like code. */
static unsigned insn_len(const uint8_t *p)
{
    unsigned n = 0;
    bool op16 = false, addr16 = false;
    for (; n < 8; n++) {
        const uint8_t b = p[n];
        if (b == 0x66) {
            op16 = true;
        } else if (b == 0x67) {
            addr16 = true;
        } else if (!(b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 || b == 0xF0 ||
                     b == 0xF2 || b == 0xF3)) {
            break;
        }
    }
    if (addr16) {
        return 0;   /* real titles' compilers don't: treat as not code */
    }
    const uint8_t op = p[n++];
    const unsigned iv = op16 ? 2 : 4;
    bool m = false;
    unsigned imm = 0;
    if (op == 0x0F) {
        const uint8_t o2 = p[n++];
        if ((o2 >= 0x80 && o2 <= 0x8F)) {
            return n + 4;
        }
        if (o2 == 0x31 || o2 == 0xA2 || o2 == 0x77 || o2 == 0x0B || o2 == 0x08 || o2 == 0x09 || o2 == 0x06 ||
            o2 == 0x30 || o2 == 0x32 || o2 == 0x33 || o2 == 0xA0 || o2 == 0xA1 || o2 == 0xA8 || o2 == 0xA9 ||
            (o2 >= 0xC8 && o2 <= 0xCF)) {
            return n;
        }
        if (o2 == 0x04 || o2 == 0x05 || o2 == 0x07 || o2 == 0x0A || o2 == 0x0C || o2 == 0x0E || o2 == 0x0F ||
            (o2 >= 0x24 && o2 <= 0x27) || (o2 >= 0x34 && o2 <= 0x3F) || o2 == 0xFF || o2 == 0x7A || o2 == 0x7B) {
            return 0;
        }
        m = true;
        if ((o2 >= 0x70 && o2 <= 0x73) || o2 == 0xA4 || o2 == 0xAC || o2 == 0xBA || o2 == 0xC2 || o2 == 0xC4 ||
            o2 == 0xC5 || o2 == 0xC6) {
            imm = 1;
        }
    } else if (op < 0x40 && (op & 7) < 4) {
        m = true;
    } else if (op < 0x40 && (op & 7) == 4) {
        imm = 1;
    } else if (op < 0x40 && (op & 7) == 5) {
        imm = iv;
    } else if (op == 0x62 || op == 0x63 || op == 0x8D || (op >= 0x84 && op <= 0x8C) || op == 0x8E || op == 0x8F ||
               (op >= 0xD0 && op <= 0xD3) || (op >= 0xD8 && op <= 0xDF) || op == 0xFE || op == 0xFF) {
        m = true;
    } else if (op == 0x69 || op == 0x81 || op == 0xC7) {
        m = true;
        imm = iv;
    } else if (op == 0x6B || op == 0x80 || op == 0x82 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6) {
        m = true;
        imm = 1;
    } else if (op == 0xF6 || op == 0xF7) {
        m = true;
        if (((p[n] >> 3) & 7) < 2) {
            imm = op == 0xF6 ? 1 : iv;
        }
    } else if (op == 0x68 || op == 0xA9 || op == 0xE8 || op == 0xE9 || (op >= 0xB8 && op <= 0xBF)) {
        imm = (op == 0xE8 || op == 0xE9) ? 4 : iv;
    } else if (op == 0x6A || (op >= 0x70 && op <= 0x7F) || op == 0xA8 || (op >= 0xB0 && op <= 0xB7) || op == 0xCD ||
               op == 0xD4 || op == 0xD5 || (op >= 0xE0 && op <= 0xE7) || op == 0xEB) {
        imm = 1;
    } else if (op >= 0xA0 && op <= 0xA3) {
        imm = 4;
    } else if (op == 0xC2 || op == 0xCA) {
        imm = 2;
    } else if (op == 0xC8) {
        imm = 3;
    } else if (op == 0xEA || op == 0x9A) {
        imm = 6;
    } else if (op == 0x60 || op == 0x61 || (op >= 0x40 && op <= 0x5F) || op == 0x90 || (op >= 0x91 && op <= 0x99) ||
               op == 0x9B || op == 0x9C || op == 0x9D || op == 0x9E || op == 0x9F || (op >= 0xA4 && op <= 0xA7) ||
               (op >= 0xAA && op <= 0xAF) || op == 0xC3 || op == 0xC9 || op == 0xCB || op == 0xCC || op == 0xCF ||
               op == 0xD7 || op == 0xF4 || op == 0xF5 || (op >= 0xF8 && op <= 0xFD)) {
        imm = 0;
    } else {
        return 0;   /* 64 65 etc. already taken as prefixes; the rest aren't what compilers emit */
    }
    if (m) {
        const uint8_t mrm = p[n++];
        const unsigned mod = mrm >> 6, rm = mrm & 7;
        if (mod != 3) {
            if (rm == 4) {
                const uint8_t sib = p[n++];
                if (mod == 0 && (sib & 7) == 5) {
                    n += 4;
                }
            } else if (mod == 0 && rm == 5) {
                n += 4;
            }
            n += mod == 1 ? 1 : mod == 2 ? 4 : 0;
        }
    }
    return n + imm;
}

/* 0F 31 in a section is real code if what's around it says so: a good run of instructions before it, the
 * start of a function just before it, or the next instruction using the result (edx:eax). */
static bool rdtsc_is_code(uint32_t lo, uint32_t va, unsigned run)
{
    const uint8_t *q = (const uint8_t *)(uintptr_t)va;
    if (run >= 8) {
        return true;
    }
    if (va >= lo + 3) {
        const uint8_t a = q[-3], b = q[-2], c = q[-1];
        if (c == 0xC3 || c == 0xCC || c == 0x90 || (a == 0x55 && b == 0x8B && c == 0xEC) || (a == 0x55 && b == 0x89 && c == 0xE5)) {
            return true;
        }
    }
    const uint8_t *n = q + 2;
    switch (n[0]) {
    case 0x50: case 0x52:               /* push eax, edx */
    case 0xA3: case 0x05: case 0x2D:    /* mov [mem], eax; add, sub eax, imm */
        return true;
    case 0x01: case 0x03: case 0x29: case 0x2B: case 0x31: case 0x33: case 0x39: case 0x3B:
    case 0x85: case 0x89: case 0x8B: {
        const unsigned reg = (n[1] >> 3) & 7, rm = n[1] & 7, mod = n[1] >> 6;
        return reg == 0 || reg == 2 || (mod == 3 && (rm == 0 || rm == 2));
    }
    case 0x0F:
        return n[1] == 0xA4 || n[1] == 0xA5 || n[1] == 0xAC || n[1] == 0xAD;    /* shld, shrd */
    default:
        return false;
    }
}

static void sweep_range(uint32_t lo, uint32_t hi)
{
    if (!g_sites) {
        g_sites = calloc(RX_SITES, sizeof(RxSite));
        if (!g_sites) {
            return;
        }
    }
    NcVm *vm = &g_core_for_faults->vm;
    uint32_t va = lo;
    unsigned run = 0;                   /* instructions decoded in a row, up to here */
    while (va + 16 < hi) {
        if (nc_vm_phys(vm, va) == UINT32_MAX || nc_vm_phys(vm, va + 15) == UINT32_MAX) {
            va = (va | (NC_PAGE - 1)) + 1;
            continue;
        }
        const uint8_t *q = (const uint8_t *)(uintptr_t)va;
        const unsigned len = insn_len(q);
        if (!len || len > 15) {
            va++;   /* padding or data: decode again from the next byte */
            run = 0;
            continue;
        }
        int kind = 0;
        if (len == 2 && q[0] == 0x0F && q[1] == 0x31) {
            kind = RX_K_RDTSC;
        }
        if (kind) {
            /* six instructions after it decode too: code, not constants */
            uint32_t at = va + len;
            bool code = true;
            for (int i = 0; i < 6 && code; i++) {
                if (at + 16 >= hi) {
                    break;
                }
                const unsigned l = insn_len((const uint8_t *)(uintptr_t)at);
                code = l && l <= 15;
                at += l;
            }
            if (code && !rdtsc_is_code(lo, va, run)) {
                static unsigned told;
                if (told++ < 16) {
                    nc_log("xog-sweep", "0F 31 at %08x looks like data: left alone", va);
                }
                code = false;
            }
            RxSite *s = code ? site_slot(va, true) : NULL;
            if (s && (!s->eip || !s->kind) && g_nsites < RX_SITES / 2) {
                uint8_t *w = code_byte(va);
                if (w && (len == 1 || code_byte(va + 1))) {
                    s->eip = va;
                    s->kind = (uint8_t)kind;
                    g_nsites++;
                    g_sweep_counts[kind]++;
                    if (len == 2) {
                        code_byte(va + 1)[0] = 0x0B;
                        w[0] = 0x0F;
                    } else {
                        w[0] = 0xCC;
                    }
                }
            }
        }
        va += len;
        run++;
    }
}

/* A guest cli that trapped: the code here is real. */
static void patch_flag_site(uint32_t va, int kind)
{
    RxSite *s = site_slot(va, true);
    uint8_t *w = code_byte(va);
    if (s && (!s->eip || !s->kind) && w) {
        s->eip = va;
        s->kind = (uint8_t)kind;
        g_nsites++;
        g_sweep_counts[kind]++;
        *w = 0xCC;
    }
}

static unsigned site_skip(uint32_t at);
static uint8_t *code_byte(uint32_t va);
extern uint8_t *g_va_soft;

static void rx_flags_near_cli(uint32_t cli)
{
    if (!g_sites && !(g_sites = calloc(RX_SITES, sizeof(RxSite)))) {
        return;
    }
    if (*(const uint8_t *)(uintptr_t)(cli - 1) == 0x9C) {
        /* only if an instruction ends there: 9C may be the last byte of one in front */
        bool boundary = false;
        for (unsigned back = 1; back <= 15 && !boundary; back++) {
            boundary = code_byte(cli - 1 - back) && insn_len((const uint8_t *)(uintptr_t)(cli - 1 - back)) == back;
        }
        if (boundary) {
            patch_flag_site(cli - 1, RX_K_PUSHFD);
        }
    }
    uint32_t at = cli + 1;
    for (int i = 0; i < 96; i++) {
        const uint8_t *q = (const uint8_t *)(uintptr_t)at;
        if (!code_byte(at) || !code_byte(at + 15) || g_va_soft[at >> 14] == 1 || g_va_soft[(at + 15) >> 14] == 1) {
            return;
        }
        const unsigned rewritten = site_skip(at);
        if (rewritten) {
            at += rewritten;    /* its jump isn't the end of the line: no flag op was moved */
            continue;
        }
        if (*q == 0x9D) {
            patch_flag_site(at, RX_K_POPFD);
            return;
        }
        /* a direct jump: the line goes on where it lands */
        if (*q == 0xEB) {
            at += 2 + (uint32_t)(int32_t)(int8_t)q[1];
            continue;
        }
        if (*q == 0xE9) {
            int32_t rel;
            memcpy(&rel, q + 1, 4);
            at += 5 + (uint32_t)rel;
            continue;
        }
        /* the end of the line: sti, ret, or something that isn't code */
        if (*q == 0xFB || *q == 0xC3 || *q == 0xC2 || *q == 0xCC) {
            return;
        }
        const unsigned l = insn_len(q);
        if (!l || l > 15) {
            return;
        }
        at += l;
    }
}

/* The XBE's executable sections, from its header in memory. */
static void sweep_image(NcCpu *cpu, uint32_t lo, uint32_t hi)
{
    NcVm *vm = &g_core_for_faults->vm;
    /* the image range from the loader; else the XBE where every one loads (0x10000), its size from its header */
    uint32_t base = cpu->image_lo;
    const bool given = base && nc_vm_phys(vm, base) != UINT32_MAX && !memcmp((const void *)(uintptr_t)base, "XBEH", 4);
    if (!given) {
        base = 0x10000;
        if (nc_vm_phys(vm, base) == UINT32_MAX || memcmp((const void *)(uintptr_t)base, "XBEH", 4)) {
            nc_log("xog-cpu", "sweep: no XBE header at %08x or %08x: nothing swept", cpu->image_lo, base);
            return;
        }
        nc_log("xog-cpu", "sweep: image range %08x-%08x not usable: the XBE at %08x", lo, hi, base);
    }
    if (!given || hi <= lo || lo > base) {
        lo = base;
        hi = base + nc_rd32((const void *)(uintptr_t)(base + 0x10C));   /* SizeOfImage */
    }
    const uint32_t n = nc_rd32((const void *)(uintptr_t)(base + 0x11C));
    const uint32_t tab = nc_rd32((const void *)(uintptr_t)(base + 0x120));
    if (n > 256 || tab < base || nc_vm_phys(vm, tab) == UINT32_MAX) {
        nc_log("xog-cpu", "sweep: section table %08x (%u sections) not readable: nothing swept", tab, n);
        return;
    }
    nc_log("xog-cpu", "sweep: %08x-%08x, %u sections", lo, hi, n);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *h = (const uint8_t *)(uintptr_t)(tab + i * 56);
        const uint32_t flags = nc_rd32(h), sva = nc_rd32(h + 4), size = nc_rd32(h + 8);
        const uint32_t name = nc_rd32(h + 20);
        char nm[9] = {0};
        if (name >= base && nc_vm_phys(vm, name) != UINT32_MAX) {
            memcpy(nm, (const void *)(uintptr_t)name, 8);
        }
        static const char *const not_swept[] = {
            /* data, whatever its flags say */
            ".rdata", ".data", ".bss", ".idata", ".edata", ".reloc", ".tls", ".CRT", ".rsrc", ".pdata", "XTLID", "$$",
            /* libraries that never read the TSC */
            "DSOUND", "XGRPH", "XMV", "XONLINE", "MDLPL", "WMADEC", "WMVDEC", "XNET", "XON_RD", "DOLBY", "XACTENG", "XPP",
        };
        bool exec = (flags & 4) != 0;
        for (unsigned j = 0; j < sizeof(not_swept) / sizeof(not_swept[0]) && exec; j++) {
            exec = strncmp(nm, not_swept[j], strlen(not_swept[j])) != 0;
        }
        const uint32_t a = sva > lo ? sva : lo, b = sva + size < hi ? sva + size : hi;
        if (exec && a < b) {
            sweep_range(a, b);
        }
    }
}

static bool finish_common(NcCpu *cpu, bool *step_out);
#include "sites_ps5.inc"

/* ---- save states: what this CPU keeps beside the console ---- */
#include "native/state/state.h"
extern void rx_shadow_flush_all(void);
extern uint8_t *rx_pa_table_map(void);
extern volatile int rx_pf_depth;
static bool g_state_loaded;

void rx_state_before_save(void)
{
    rx_shadow_flush_all();
}

bool rx_state_save(NcBuf *b)
{
    const uint32_t pages = g_core_for_faults->vm.ram_size / NC_PAGE;
    nc_buf_u32(b, g_nstubs);
    nc_buf_put(b, g_stubs, g_nstubs * sizeof(g_stubs[0]));
    nc_buf_u32(b, (uint32_t)rx_pf_depth);
    nc_buf_u32(b, pages);
    nc_buf_put(b, rx_pa_table_map(), pages);
    /* v2: where rdtsc and the flag ops are patched in RAM: without it they'd be strange instructions */
    uint32_t sites = 0;
    for (unsigned i = 0; g_sites && i < RX_SITES; i++) {
        sites += g_sites[i].eip && g_sites[i].kind;
    }
    nc_buf_u32(b, sites);
    for (unsigned i = 0; g_sites && i < RX_SITES; i++) {
        if (g_sites[i].eip && g_sites[i].kind) {
            nc_buf_u32(b, g_sites[i].eip);
            nc_buf_u32(b, g_sites[i].kind);
        }
    }
    return !b->fail;
}

bool rx_state_load(NcRd *r, unsigned v)
{
    (void)v;
    const uint32_t n = nc_rd_u32(r);
    if (n > RX_SITE_MAX) {
        return false;
    }
    g_nstubs = n;
    nc_rd_get(r, g_stubs, n * sizeof(g_stubs[0]));
    rx_pf_depth = (int)nc_rd_u32(r);
    const uint32_t pages = nc_rd_u32(r);
    if (pages != g_core_for_faults->vm.ram_size / NC_PAGE) {
        return false;
    }
    nc_rd_get(r, rx_pa_table_map(), pages);
    if (v >= 2) {
        const uint32_t sites = nc_rd_u32(r);
        if (sites > RX_SITES / 2 || (!g_sites && !(g_sites = calloc(RX_SITES, sizeof(RxSite))))) {
            return false;
        }
        for (uint32_t i = 0; i < sites && !r->fail; i++) {
            const uint32_t eip = nc_rd_u32(r), kind = nc_rd_u32(r);
            RxSite *s = site_slot(eip, true);
            if (s && !s->eip) {
                s->eip = eip;
                s->kind = (uint8_t)kind;
                g_nsites++;
            }
        }
    }
    g_state_loaded = !r->fail;
    return !r->fail;
}

/* ---- a title's patches, from the start's file: address and bytes ---- */
static struct RxPoke {
    uint32_t va;
    uint16_t len;
    uint8_t *bytes;
} *g_pokes;
static unsigned g_npokes, g_pokes_cap;

void rx_poke_add(const char *text)
{
    char *end;
    const uint32_t va = (uint32_t)strtoul(text, &end, 16);
    if (*end != ':') {
        return;
    }
    if (g_npokes == g_pokes_cap) {
        const unsigned cap = g_pokes_cap ? g_pokes_cap * 2 : 64;
        struct RxPoke *grown = realloc(g_pokes, cap * sizeof(*grown));
        if (!grown) {
            return;
        }
        g_pokes = grown;
        g_pokes_cap = cap;
    }
    const size_t digits = strlen(end + 1), n = digits / 2;
    uint8_t *b = n ? malloc(n) : NULL;
    if (!b) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        char two[3] = {end[1 + i * 2], end[2 + i * 2], 0};
        b[i] = (uint8_t)strtoul(two, NULL, 16);
    }
    g_pokes[g_npokes].va = va;
    g_pokes[g_npokes].len = (uint16_t)n;
    g_pokes[g_npokes++].bytes = b;
}

/* Doom 3's floor on a frame's milliseconds, for 60 frames a second (patch 83). */
static void rx_unlock_patches(void)
{
    extern int g_xog_vblank_factor;
    if (g_xog_vblank_factor < 2 || nc_vm_phys(&g_core_for_faults->vm, 0x2ED2D) == UINT32_MAX) {
        return;
    }
    const uint8_t *q = (const uint8_t *)(uintptr_t)0x2ED2D;
    static const uint8_t want[] = {0x83, 0xF8, 0x21, 0x7D, 0x0C, 0xC7, 0x05, 0xC0, 0xBF, 0x50, 0x00, 0x21};
    if (!memcmp(q, want, sizeof(want)) && code_byte(0x2ED2F) && code_byte(0x2ED38)) {
        *code_byte(0x2ED2F) = 0x10;
        *code_byte(0x2ED38) = 0x10;
        nc_log("xog-title", "Doom 3: its frames may be 16 ms now");
    }
}

/* xogfix=ftol: Need for Speed: Hot Pursuit 2's _ftol2 (0014a724) as fisttp qword: truncate st(0) to 64 bits and pop,
 * what its fistp/fild/fsub/fstp and integer reloads did (patch 129) */
static void rx_ftol_patch(void)
{
    const char *e = getenv("XOG_FIX");
    if (!e || !strstr(e, "ftol") || nc_vm_phys(&g_core_for_faults->vm, 0x14a724) == UINT32_MAX) {
        return;
    }
    static const uint8_t was[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0x83, 0xE4, 0xF0, 0xD9, 0xC0, 0xD9, 0x54, 0x24,
                                  0x18, 0xDF, 0x7C, 0x24, 0x10, 0xDF, 0x6C, 0x24, 0x10};
    /* sub esp,8; fisttp qword [esp]; pop eax; pop edx; ret */
    static const uint8_t now[] = {0x83, 0xEC, 0x08, 0xDD, 0x0C, 0x24, 0x58, 0x5A, 0xC3};
    if (memcmp((const void *)(uintptr_t)0x14a724, was, sizeof(was))) {
        nc_log("xog-title", "ftol: not the expected bytes at 0014a724, left as it is");
        return;
    }
    for (unsigned i = 0; i < sizeof(now); i++) {
        if (!code_byte(0x14a724 + i)) {
            return;
        }
    }
    for (unsigned i = 0; i < sizeof(now); i++) {
        *code_byte(0x14a724 + i) = now[i];
    }
    nc_log("xog-title", "ftol: _ftol2 at 0014a724 is fisttp now");
}

/* xogfix=dsnull: Morrowind's music Pause calls skip a stream not made yet (patch 135) */
static void rx_dsnull_patch(void)
{
    const char *e = getenv("XOG_FIX");
    if (!e || !strstr(e, "dsnull") || nc_vm_phys(&g_core_for_faults->vm, 0x18c141) == UINT32_MAX) {
        return;
    }
    static const uint8_t call1[] = {0xE8, 0xB4, 0x61, 0x17, 0x00}, call2[] = {0xE8, 0x68, 0x60, 0x17, 0x00};
    const uint8_t *cave = (const uint8_t *)(uintptr_t)0x18c141;
    for (unsigned i = 0; i < 15; i++) {
        if (cave[i] != 0x90) {
            nc_log("xog-title", "dsnull: not the expected bytes, left as it is");
            return;
        }
    }
    if (memcmp((const void *)(uintptr_t)0x18c11d, call1, 5) || memcmp((const void *)(uintptr_t)0x18c269, call2, 5)) {
        nc_log("xog-title", "dsnull: not the expected calls, left as it is");
        return;
    }
    /* cmp dword [esp+4],0; jne 0x3022d6; ret 8 */
    uint8_t now[14] = {0x83, 0x7C, 0x24, 0x04, 0x00, 0x0F, 0x85, 0, 0, 0, 0, 0xC2, 0x08, 0x00};
    const int32_t jne = (int32_t)(0x3022d6 - (0x18c141 + 11));
    memcpy(now + 7, &jne, 4);
    for (unsigned i = 0; i < sizeof(now); i++) {
        if (!code_byte(0x18c141 + i)) {
            return;
        }
    }
    for (unsigned i = 0; i < sizeof(now); i++) {
        *code_byte(0x18c141 + i) = now[i];
    }
    const uint32_t sites[2] = {0x18c11d, 0x18c269};
    for (unsigned s = 0; s < 2; s++) {
        const int32_t rel = (int32_t)(0x18c141 - (sites[s] + 5));
        for (unsigned i = 0; i < 4; i++) {
            *code_byte(sites[s] + 1 + i) = (uint8_t)((uint32_t)rel >> (8 * i));
        }
    }
    nc_log("xog-title", "dsnull: Morrowind's music pause checks its stream");
}

static void rx_title_patches(NcCpu *cpu)
{
    (void)cpu;
    unsigned done = 0, missed = 0;
    rx_unlock_patches();
    rx_ftol_patch();
    rx_dsnull_patch();
    for (unsigned p = 0; p < g_npokes; p++) {
        extern bool rx_mod_overlaps(uint32_t va, uint32_t len);
        if (rx_mod_overlaps(g_pokes[p].va, g_pokes[p].len)) {
            missed++;   /* the mod's bytes: it stays whole */
            nc_log("xog-title", "patch at %08x, %u bytes: on the mod's, left off", g_pokes[p].va, g_pokes[p].len);
            continue;
        }
        bool all = true;
        for (unsigned i = 0; i < g_pokes[p].len && all; i++) {
            all = code_byte(g_pokes[p].va + i) != NULL;
        }
        if (!all) {
            missed++;   /* a section not loaded at the start */
            nc_log("xog-title", "patch at %08x, %u bytes: not in memory, left off", g_pokes[p].va, g_pokes[p].len);
            continue;
        }
        for (unsigned i = 0; i < g_pokes[p].len; i++) {
            *code_byte(g_pokes[p].va + i) = g_pokes[p].bytes[i];
        }
        done++;
    }
    if (g_npokes) {
        nc_log("xog-title", "patches: %u changes made, %u left off", done, missed);
    }
}

void rx_cpu_set_image(NcCore *core, uint32_t lo, uint32_t hi)
{
    g_core_for_faults = core;
    if (core->cpu) {
        core->cpu->image_lo = lo;
        core->cpu->image_hi = hi;
    }
}

void nc_cpu_invalidate(NcCpu *cpu, uint32_t va, uint32_t len)
{
    /* Native code has no translations to drop, only hooks to bring up to date. */
    if (len > (1u << 24)) {
        hooks_sync(cpu, cpu->image_lo, cpu->image_hi);
        return;
    }
    /* a section loaded, or a mod's bytes: its new code is swept too */
    sites_replaced(va, len);
    if (len >= 64 && g_core_for_faults) {
        sweep_image(cpu, va, va + len);
    }
    hooks_sync(cpu, va, va + len);
}

/* ---- reading instructions ---- */

static unsigned skip_prefixes(const uint8_t *p, bool *op16)
{
    unsigned n = 0;
    while (n < 14) {
        const uint8_t b = p[n];
        if (b == 0x66) {
            *op16 = true;
        } else if (!(b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 || b == 0x67 ||
                     b == 0xF0 || b == 0xF2 || b == 0xF3)) {
            break;
        }
        n++;
    }
    return n;
}

static unsigned modrm_len(const uint8_t *p)
{
    const unsigned mod = p[0] >> 6, rm = p[0] & 7;
    unsigned n = 1;
    if (mod == 3) {
        return n;
    }
    if (rm == 4) {
        n++;
        if (mod == 0 && (p[1] & 7) == 5) {
            n += 4;
        }
    } else if (mod == 0 && rm == 5) {
        n += 4;
    }
    return n + (mod == 1 ? 1 : mod == 2 ? 4 : 0);
}

/* What a #GP's instruction is, as the core's system ops name them. */
static enum NcSysOp classify(const uint8_t *p, unsigned *size)
{
    bool op16 = false;
    const unsigned pre = skip_prefixes(p, &op16);
    const uint8_t *q = p + pre;
    *size = pre + 1;
    switch (q[0]) {
    case 0xCD: *size = pre + 2; return NC_SYSOP_INT;
    case 0xCE: return NC_SYSOP_INTO;
    case 0xF1: return NC_SYSOP_INT1;
    case 0xFA: return NC_SYSOP_CLI;
    case 0xFB: return NC_SYSOP_STI;
    case 0xF4: return NC_SYSOP_HLT;
    case 0xE4: case 0xE5: *size = pre + 2; return NC_SYSOP_IN;
    case 0xE6: case 0xE7: *size = pre + 2; return NC_SYSOP_OUT;
    case 0xEC: case 0xED: return NC_SYSOP_IN;
    case 0xEE: case 0xEF: return NC_SYSOP_OUT;
    case 0x6C: case 0x6D: return NC_SYSOP_INS;
    case 0x6E: case 0x6F: return NC_SYSOP_OUTS;
    case 0x0F:
        *size = pre + 2;
        switch (q[1]) {
        case 0x06: return NC_SYSOP_CLTS;
        case 0x08: return NC_SYSOP_INVD;
        case 0x09: return NC_SYSOP_WBINVD;
        case 0x30: return NC_SYSOP_WRMSR;
        case 0x32: return NC_SYSOP_RDMSR;
        case 0x33: return NC_SYSOP_RDPMC;
        case 0x20: case 0x22: *size = pre + 3; return NC_SYSOP_MOVCR;
        case 0x21: case 0x23: *size = pre + 3; return NC_SYSOP_MOVDR;
        case 0x00:
            *size = pre + 2 + modrm_len(q + 2);
            return ((q[2] >> 3) & 7) == 2 ? NC_SYSOP_LLDT : ((q[2] >> 3) & 7) == 3 ? NC_SYSOP_LTR : NC_SYSOP_OTHER;
        case 0x01: {
            const unsigned sub = (q[2] >> 3) & 7;
            *size = pre + 2 + modrm_len(q + 2);
            return sub == 7 ? NC_SYSOP_INVLPG : sub == 2 ? NC_SYSOP_LGDT : sub == 3 ? NC_SYSOP_LIDT
                   : sub == 6 ? NC_SYSOP_LMSW : NC_SYSOP_OTHER;
        }
        default: return NC_SYSOP_OTHER;
        }
    default:
        return NC_SYSOP_OTHER;
    }
}

/* fs: in 32-bit mode uses the host's FS base, cut to 32 bits, which is the host's
 * own TLS and faults or worse. The Xbox's thread block is on gs here: the prefix
 * changes once and the instruction runs again. */
static bool fix_fs_prefix(uint32_t eip)
{
    bool op16 = false;
    const uint8_t *code = (const uint8_t *)(uintptr_t)eip;
    const unsigned pre = skip_prefixes(code, &op16);
    for (unsigned i = 0; i < pre; i++) {
        if (code[i] == 0x64) {
            uint8_t *w = code_byte(eip + i);
            if (w) {
                *w = 0x65;
                return true;
            }
        }
    }
    return false;
}

/* ---- the signal ---- */

static void frame_pull(const RxMc *m)
{
    NcFrame *f = &rx_frame;
    if (g_fx_off >= 0) {
        memcpy(f->fx, (const char *)m - 0x40 + g_fx_off, sizeof(f->fx));
    }
    f->regs[NC_EAX] = (uint32_t)m->rax;
    f->regs[NC_ECX] = (uint32_t)m->rcx;
    f->regs[NC_EDX] = (uint32_t)m->rdx;
    f->regs[NC_EBX] = (uint32_t)m->rbx;
    f->regs[NC_ESP] = (uint32_t)m->rsp;
    f->regs[NC_EBP] = (uint32_t)m->rbp;
    f->regs[NC_ESI] = (uint32_t)m->rsi;
    f->regs[NC_EDI] = (uint32_t)m->rdi;
    f->rip = (uint32_t)m->rip;
    f->eflags = (uint32_t)m->rflags;
}

static void frame_push(RxMc *m, bool step)
{
    const NcFrame *f = &rx_frame;
    if (g_fx_off >= 0) {
        memcpy((char *)m - 0x40 + g_fx_off, f->fx, sizeof(f->fx));
    }
    m->rax = f->regs[NC_EAX];
    m->rcx = f->regs[NC_ECX];
    m->rdx = f->regs[NC_EDX];
    m->rbx = f->regs[NC_EBX];
    m->rsp = f->regs[NC_ESP];
    m->rbp = f->regs[NC_EBP];
    m->rsi = f->regs[NC_ESI];
    m->rdi = f->regs[NC_EDI];
    m->rip = f->rip;
    m->rflags = (f->eflags & RX_EFLAGS_KEPT) | RX_IF | 2u | (step ? RX_TF : 0);
}

/* Off the guest: nc_cpu_run returns. */
static void leave_guest(NcCpu *cpu, RxMc *m)
{
    m->rip = (uint64_t)(uintptr_t)rx_leave;
    m->cs = cpu->host_cs;
    m->ss = cpu->host_ss;
}

static void log_line(const char *fmt, ...)
{
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    rx_log_raw(b, n < (int)sizeof(b) ? (size_t)n : sizeof(b) - 1);
}

static bool readable(uint64_t a)
{
    NcVm *vm = g_core_for_faults ? &g_core_for_faults->vm : NULL;
    return vm && a < (1ull << 32) && nc_vm_phys(vm, (uint32_t)a) != UINT32_MAX;
}

extern volatile uint32_t rx_entry_tf;

/* Words at a guest address through the host's view, which can't fault. */
static void dump_words(const char *name, uint64_t a, unsigned n)
{
    log_line("  %s %08llx:", name, (unsigned long long)a);
    for (unsigned i = 0; i < n; i++) {
        const uint64_t w = a + i * 4;
        if (readable(w)) {
            log_line(" %08x", nc_rd32(g_core_for_faults->vm.phys + nc_vm_phys(&g_core_for_faults->vm, (uint32_t)w)));
        } else {
            log_line(" --------");
        }
    }
    log_line("\n");
}

/* A title stopped itself: what it had in hand. */
void rx_dump_stop(void)
{
    const NcFrame *f = &rx_frame;
    log_line("xog-stop: eip %08x eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x\n", (uint32_t)f->rip,
             (uint32_t)f->regs[NC_EAX], (uint32_t)f->regs[NC_ECX], (uint32_t)f->regs[NC_EDX], (uint32_t)f->regs[NC_EBX],
             (uint32_t)f->regs[NC_ESP], (uint32_t)f->regs[NC_EBP], (uint32_t)f->regs[NC_ESI], (uint32_t)f->regs[NC_EDI]);
    dump_words("xog-stop: code before", f->rip >= 64 ? f->rip - 64 : 0, 16);
    dump_words("xog-stop: stack", (uint32_t)f->regs[NC_ESP], 48);
    {
        extern void nc_paging_dump(NcKernel *k, uint32_t va);
        NcKernel *k = g_cpu->cfg.opaque;
        log_line("xog-stop: last page fault at %08x\n", k->cr2);
        nc_paging_dump(k, k->cr2);
    }
}

/* Everything a fault can tell, at once. */
static void dump_state(const RxMc *m)
{
    log_line("  eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x\n", (uint32_t)m->rax,
             (uint32_t)m->rcx, (uint32_t)m->rdx, (uint32_t)m->rbx, (uint32_t)m->rsp, (uint32_t)m->rbp,
             (uint32_t)m->rsi, (uint32_t)m->rdi);
    log_line("  rflags %llx ds %x es %x fs %x gs %x\n", (unsigned long long)m->rflags, m->ds, m->es, m->fs, m->gs);
    if (m->rip < (1ull << 32)) {
        dump_words("code before", m->rip >= 32 ? m->rip - 32 : 0, 8);
        dump_words("code", m->rip, 8);
    }
    log_line("  frame rip %08x esp %08x vif %u pending %u tf %u; gate calls %llu, sites %u\n", (uint32_t)rx_frame.rip,
             (uint32_t)rx_frame.regs[NC_ESP], rx_frame.vif, rx_irq_pending, rx_entry_tf, (unsigned long long)g_gate_n, g_nstubs);
    dump_words("stack", (uint32_t)m->rsp, 32);
    dump_words("frame-0x40", (uint32_t)m->rbp - 0x40, 32);
    dump_words("eax->", (uint32_t)m->rax, 8);
    dump_words("esi->", (uint32_t)m->rsi, 8);
    dump_words("edi->", (uint32_t)m->rdi, 8);
    if (g_core_for_faults) {
        const NcKernel *k = &g_core_for_faults->kernel;
        log_line("  HalDiskCachePartitionCount at %08x\n", k->data_va[40]);
        dump_words("export 40", k->data_va[40], 4);
    }
}

static void fatal(const char *what, int sig, const RxMc *m, const siginfo_t *si)
{
    char b[256];
    int n = snprintf(b, sizeof(b), "xog: %s: signal %d rip %llx cs %llx addr %llx trap %u err %llx\n", what, sig,
                     (unsigned long long)m->rip, (unsigned long long)m->cs,
                     (unsigned long long)(uintptr_t)si->si_addr, m->trapno, (unsigned long long)m->err);
    rx_log_raw(b, (size_t)n);
    dump_state(m);
    signal(SIGSYS, SIG_DFL);
    _exit(3);
}

/* After a system op: a bypass, pending interrupts. True: off the guest. */
static bool finish_common(NcCpu *cpu, bool *step_out)
{
    NcFrame *f = &rx_frame;
    if (rx_irq_pending) {
        const uint32_t e = (uint32_t)f->rip;
        if (!(e >= RX_NOINT_LO && e < RX_NOINT_HI)) {
            cpu->cfg.sysop(cpu->cfg.opaque, f, NC_SYSOP_INTERRUPT, 0);
        }
    }
    if (*(const uint8_t *)(uintptr_t)(uint32_t)f->rip == 0xF4 && (uint32_t)f->rip >= NC_KERNEL_VA &&
        (uint32_t)f->rip < NC_KERNEL_VA + 0x2000u) {
        return true;            /* the kernel's stop stub */
    }
    bool step = false;
    if (f->bypass && f->bypass == (uint32_t)f->rip) {
        const RxHook *h = hook_slot(cpu, f->bypass, false);
        uint8_t *p = code_byte(f->bypass);
        if (h && p) {
            *p = h->orig;
            cpu->step_hook = f->bypass;
            step = true;
        }
    }
    f->bypass = 0;
    const uint32_t e = (uint32_t)f->rip;
    if (rx_irq_pending && e >= RX_NOINT_LO && e < RX_NOINT_HI) {
        step = true;    /* out of the fast path first, one instruction at a time */
    }
    *step_out = step;
    return false;
}

static void finish(NcCpu *cpu, RxMc *m, bool halted)
{
    (void)halted;
    bool step = false;
    if (finish_common(cpu, &step)) {
        leave_guest(cpu, m);
    } else {
        frame_push(m, step);
    }
}

static void run_sysop(NcCpu *cpu, int sig, siginfo_t *si, RxMc *m, enum NcSysOp op, unsigned size)
{
    if (!cpu->cfg.sysop(cpu->cfg.opaque, &rx_frame, op, size)) {
        fatal("system op failed", sig, m, si);
    }
    finish(cpu, m, false);
}

extern void nc_hold_report(void);
uint32_t g_xog_invlpg_va = UINT32_MAX;

/* What the heartbeat reads. */
static volatile uint64_t g_sig_n[33];
static volatile uint32_t g_want_sample, g_sample_seen, g_sample_rip, g_sample_esp, g_sample_ebp, g_sample_guest;
static volatile uint64_t g_sample_rip64, g_sample_ret64;
uint64_t rx_frames_shown;
volatile uint32_t rx_last_mmio;

static volatile int g_depth;     /* the guest thread's handlers inside each other */
static volatile uint32_t g_trace_skip = 0, g_trace_left = 200;

static void rx_sig_inner(int sig, siginfo_t *si, void *uc);

/* Each trap, one line, by the thread that took it: the last line before a death
 * nobody reports says what it was doing. */
static void trace(int sig, const siginfo_t *si, const RxMc *m, bool guest)
{
    if (!g_trace_left || sig == SIGUSR2) {
        return;
    }
    /* every host-side fault shows, wherever it comes */
    if (guest && g_trace_skip) {
        g_trace_skip--;
        return;
    }
    g_trace_left--;
    char b[160];
    const int n = snprintf(b, sizeof(b), "t %s sig %d trap %u rip %llx addr %llx err %llx esp %llx depth %d\n",
                           guest ? "guest" : "host", sig, m->trapno, (unsigned long long)m->rip,
                           (unsigned long long)(uintptr_t)si->si_addr, (unsigned long long)m->err,
                           (unsigned long long)m->rsp, g_depth);
    rx_log_raw(b, (size_t)n);
}

void rx_sig_c(int sig, siginfo_t *si, void *uc)
{
    RxMc *m = mc_of(uc);
    const bool me = g_cpu && g_cpu->running && pthread_equal(pthread_self(), g_cpu->thread);
    trace(sig, si, m, (uint16_t)m->cs == RX_GUEST_CS);
    if (me) {
        if (++g_depth > 6) {
            g_depth = 0;
            fatal("handler re-entered itself", sig, m, si);
        }
    }
    rx_sig_inner(sig, si, uc);
    if (me) {
        g_depth--;
    }
}

/* sldt, str, smsw, where the console's CPU refuses them to user code: what the Xbox would answer. */
static bool sysreg_read(NcFrame *f, const uint8_t *code, uint32_t eip, unsigned size)
{
    extern bool rx_mmio_ea(const NcFrame *f, const uint8_t *code, uint32_t *addr);
    unsigned p = 0;
    while (p < 4 && (code[p] == 0x66 || code[p] == 0x26 || code[p] == 0x2E || code[p] == 0x36 || code[p] == 0x3E ||
                     code[p] == 0x64 || code[p] == 0x65)) {
        p++;
    }
    const uint8_t *q = code + p;
    if (q[0] != 0x0F) {
        return false;
    }
    const unsigned reg = (q[2] >> 3) & 7;
    uint16_t v;
    if (q[1] == 0x00 && reg <= 1) {
        v = 0;              /* no LDT, and no task register a game has a use for */
    } else if (q[1] == 0x01 && reg == 4) {
        v = 0x003B;         /* CR0's low word as the Xbox kernel leaves it */
    } else {
        return false;
    }
    if ((q[2] >> 6) == 3) {
        nc_frame_set_gpr(f, q[2] & 7, v);
    } else {
        uint32_t ea = 0;
        if (!rx_mmio_ea(f, code, &ea) || nc_vm_phys(&g_core_for_faults->vm, ea) == UINT32_MAX) {
            return false;
        }
        nc_wr16((void *)(uintptr_t)ea, v);
    }
    f->rip = eip + size;
    return true;
}

uint8_t *rx_frame_fx(NcFrame *f)
{
    return f->fx;
}

static void rx_sig_inner(int sig, siginfo_t *si, void *uc)
{
    RxMc *m = mc_of(uc);
    NcCpu *cpu = g_cpu;
    const bool guest = (uint16_t)m->cs == RX_GUEST_CS;

    if (sig > 0 && sig < 33) {
        g_sig_n[sig]++;
    }
    if (sig == SIGSYS) {
        static const char msg[] = "xog: SIGSYS: a refused system call, the usual end of the title's exit\n";
        rx_log_raw(msg, sizeof(msg) - 1);
        signal(SIGSYS, SIG_DFL);
        return;     /* it runs again, and ends the process */
    }
    if (!cpu && sig != SIGUSR2) {
        fatal("fault in start-up", sig, m, si);
    }
    /* A step that followed the guest through a far call. */
    if (sig == SIGTRAP && !guest && cpu && (m->rflags & RX_TF) &&
        (m->rip < (1ull << 32) || (m->rip >= (uint64_t)(uintptr_t)rx_gate_entry && m->rip < (uint64_t)(uintptr_t)rx_resume_end))) {
        m->rflags &= ~(uint64_t)RX_TF;
        if (g_steps_n) {
            rx_steps_end(&g_core_for_faults->vm);
        }
        return;
    }
    if (sig == SIGUSR2) {
        if (g_steps_n && guest) {
            return;     /* the step's trap comes next, and takes the interrupt */
        }
        /* On the way back through the gate: it looks at the request again. */
        if (!guest && m->rip >= (uint64_t)(uintptr_t)rx_gate_exit && m->rip < (uint64_t)(uintptr_t)rx_resume_end) {
            m->rip = (uint64_t)(uintptr_t)rx_gate_exit;
        }
        if (g_prof_want) {
            extern void xog_prof_game(bool guest, uint64_t rip, const uint64_t *sp);
            g_prof_want = 0;
            /* a stack to scan only when it's the host's (in the gate it may still be the guest's, ending anywhere) */
            xog_prof_game(guest, m->rip, guest || m->rsp < (1ull << 32) ? NULL : (const uint64_t *)(uintptr_t)m->rsp);
            if (guest && g_fx_off >= 0) {
                uint8_t *fx = (uint8_t *)m - 0x40 + g_fx_off;
                const uint16_t fcw = (uint16_t)(fx[0] | fx[1] << 8), fsw = (uint16_t)(fx[2] | fx[3] << 8);
                g_fpu.n++;
                g_fpu.full += fx[4] == 0xFF;
                g_fpu.sf += (fsw >> 6) & 1;
                g_fpu.ie += fsw & 1;
                g_fpu.de += (fsw >> 1) & 1;
                g_fpu.used += (unsigned)__builtin_popcount(fx[4]);
                g_fpu.fcw = fcw;
                g_fpu.mxcsr = (uint32_t)(fx[24] | fx[25] << 8 | fx[26] << 16 | (uint32_t)fx[27] << 24);
                g_fpu.top = (fsw >> 11) & 7;
                fx[2] &= 0x80;          /* the sticky flags (IE..PE, SF) cleared: the next sample's are fresh */
                fx[24] &= 0xC0;         /* MXCSR's too */
            }
        }
        if (g_want_sample) {
            g_want_sample = 0;
            g_sample_rip = (uint32_t)m->rip;
            g_sample_esp = (uint32_t)m->rsp;
            g_sample_ebp = (uint32_t)m->rbp;
            g_sample_guest = guest;
            g_sample_rip64 = m->rip;
            g_sample_ret64 = 0;
            if (!guest && m->rsp >= (1ull << 32)) {
                /* the first return address in our own code, past the kernel library's frames (the host's stack only) */
                const uint64_t *sp = (const uint64_t *)(uintptr_t)m->rsp;
                for (unsigned w = 0; w < 96; w++) {
                    if (sp[w] >= 0x3000000000ull && sp[w] < 0x3002000000ull) {
                        g_sample_ret64 = sp[w];
                        break;
                    }
                }
            }
            g_sample_seen = 1;
        }
        if (guest && cpu && rx_irq_pending) {
            frame_pull(m);
            finish(cpu, m, false);
        }
        return;
    }
    if (!guest) {
        /* Host code: a device thread or the kernel touching guest memory. */
        if (g_core_for_faults && nc_core_fault(g_core_for_faults, sig, si, uc)) {
            return;
        }
        fatal("fault in host code", sig, m, si);
    }

    frame_pull(m);
    NcFrame *f = &rx_frame;
    const uint32_t eip = (uint32_t)m->rip;

    /* A step under way ends at its trap, or goes on through a fault on another soft block.
     * Anything else takes the instruction elsewhere: what it did so far stands. */
    if (g_steps_n && !(sig == SIGTRAP && m->trapno != 3)) {
        const uint32_t fa = (uint32_t)(uintptr_t)si->si_addr;
        if (!(sig == SIGSEGV && g_va_soft[fa >> 14] == 1 && nc_vm_phys(&g_core_for_faults->vm, fa) != UINT32_MAX)) {
            rx_steps_end(&g_core_for_faults->vm);
        }
    }

    if (g_in_check) {
        if (sig == SIGTRAP && m->trapno == 3) {
            if (++g_check_int3s == 2) {
                /* xmm0 holds the thread block's address, its other three words 0 */
                const uint32_t want[4] = {cpu->fs_base, 0, 0, 0};
                const char *top = (const char *)cpu->altstack + RX_ALTSTACK;
                for (long off = 0x100; off < 0x1000 && (const char *)uc + off + 16 <= top; off += 16) {
                    if (!memcmp((const char *)uc + off, want, 16) && off >= 160) {
                        g_fx_off = off - 160;
                        break;
                    }
                }
                if (g_fx_off >= 0) {
                    frame_pull(m);
                    f->regs[NC_ESI] = cpu->fs_base;
                    const uint32_t mark = 0x12345678u;
                    memcpy(f->fx + 160, &mark, 4);
                }
                frame_push(m, false);
            }
            return;
        }
        if (sig == SIGBUS && *(const uint8_t *)(uintptr_t)eip == 0xF4) {
            leave_guest(cpu, m);
            return;
        }
        if (fix_fs_prefix(eip)) {
            return;
        }
        fatal("start-up check", sig, m, si);
    }

    switch (sig) {
    case SIGTRAP:
        if (m->trapno != 3) {    /* not a breakpoint: the one step a hook's bypass or a fast path takes */
            if (g_steps_n) {
                rx_steps_end(&g_core_for_faults->vm);
            }
            if (cpu->step_hook) {
                uint8_t *p = code_byte(cpu->step_hook);
                if (p) {
                    *p = 0xCC;
                }
                cpu->step_hook = 0;
            }
            finish(cpu, m, false);
            return;
        }
        {
            const uint32_t at = eip - 1;    /* int3 reports past itself */
            const RxSite *st = g_sites ? site_slot(at, false) : NULL;
            if (st && st->kind == RX_K_PUSHFD) {
                if (site_note(cpu, at, RX_S_PUSHFD, 1, NULL)) {
                    f->rip = at;
                    finish(cpu, m, false);
                    return;
                }
                const uint32_t esp = (uint32_t)f->regs[NC_ESP] - 4;
                nc_wr32((void *)(uintptr_t)esp, nc_frame_eflags(f));
                f->regs[NC_ESP] = esp;
                f->rip = eip;
                finish(cpu, m, false);
                return;
            }
            if (st && st->kind == RX_K_POPFD) {
                if (site_note(cpu, at, RX_S_POPFD, 1, NULL)) {
                    f->rip = at;
                    finish(cpu, m, false);
                    return;
                }
                const uint32_t esp = (uint32_t)f->regs[NC_ESP];
                nc_frame_set_eflags(f, nc_rd32((const void *)(uintptr_t)esp));
                f->regs[NC_ESP] = esp + 4;
                f->rip = eip;
                finish(cpu, m, false);
                return;
            }
            if (hook_slot(cpu, at, false)) {
                f->rip = at;
                run_sysop(cpu, sig, si, m, NC_SYSOP_HOOK, 0);
            } else {
                f->rip = at;
                run_sysop(cpu, sig, si, m, NC_SYSOP_INT3, 1);
            }
        }
        return;
    case SIGILL: {
        const RxSite *st = g_sites ? site_slot(eip, false) : NULL;
        if (st && st->kind == RX_K_RDTSC) {
            if (site_note(cpu, eip, RX_S_RDTSC, 2, NULL)) {
                f->rip = eip;
                finish(cpu, m, false);
                return;
            }
            const uint64_t t = (uint64_t)(((unsigned __int128)(rx_rdtsc() - cpu->cycle_offset) * g_tsc_ratio) >> 32);
            f->regs[NC_EAX] = (uint32_t)t;
            f->regs[NC_EDX] = (uint32_t)(t >> 32);
            f->rip = eip + 2;
            finish(cpu, m, false);
            return;
        }
        if (st && st->kind == RX_K_CPUID) {
            run_sysop(cpu, sig, si, m, NC_SYSOP_CPUID, 2);
            return;
        }
        run_sysop(cpu, sig, si, m, NC_SYSOP_UD2, 2);
        return;
    }
    case SIGBUS:
        if (m->trapno == 9) {       /* #GP */
            const uint8_t *code = (const uint8_t *)(uintptr_t)eip;
            bool op16 = false;
            const unsigned pre = skip_prefixes(code, &op16);
            if (code[pre] == 0x0F && code[pre + 1] == 0x01 && ((code[pre + 2] >> 3) & 7) <= 1 && (code[pre + 2] >> 6) != 3) {
                /* sgdt, sidt: the kernel's IDT (a title's pager edits its gates), a flat GDT */
                const bool idt = ((code[pre + 2] >> 3) & 7) == 1;
                const NcKernel *k = cpu->cfg.opaque;
                uint32_t ea = 0;
                if (rx_mmio_ea(f, code, &ea)) {
                    NcVm *tv = &g_core_for_faults->vm;
                    nc_wr16(nc_vm_host(tv, ea), idt ? 0x7FFu : 0x3Fu);
                    nc_wr32(nc_vm_host(tv, ea + 2), idt ? k->idt_va : 0u);
                    f->rip = eip + pre + 2 + modrm_len(code + pre + 2);
                    finish(cpu, m, false);
                    return;
                }
            }
            if (code[pre] == 0xCF) {
                /* iretd out of a title's fault handler: virtual IF from what it pops */
                if (rx_pf_depth > 0) {
                    rx_pf_depth--;
                }
                const uint32_t esp = (uint32_t)f->regs[NC_ESP];
                f->rip = nc_rd32((const void *)(uintptr_t)esp);
                nc_frame_set_eflags(f, nc_rd32((const void *)(uintptr_t)(esp + 8)));
                f->regs[NC_ESP] = esp + 12;
                finish(cpu, m, false);
                return;
            }
            unsigned size = 0;
            const enum NcSysOp op = classify(code, &size);
            if (op == NC_SYSOP_CLI) {
                rx_flags_near_cli(eip);
                if (site_note(cpu, eip, RX_S_CLI, 1, NULL)) {
                    f->rip = eip;
                    finish(cpu, m, false);
                    return;
                }
            } else if (op == NC_SYSOP_STI) {
                if (site_note(cpu, eip, RX_S_STI, 1, NULL)) {
                    f->rip = eip;
                    finish(cpu, m, false);
                    return;
                }
            } else if (op == NC_SYSOP_IN || op == NC_SYSOP_OUT) {
                /* the stub's copy is taken before the jump goes in; the gate does the work */
                if (site_note(cpu, eip, op == NC_SYSOP_IN ? RX_S_IN : RX_S_OUT, size, NULL)) {
                    f->rip = eip;
                    finish(cpu, m, false);
                    return;
                }
            }
            if (op == NC_SYSOP_HLT) {
                leave_guest(cpu, m);
                return;
            }
            if (op == NC_SYSOP_INVLPG) {
                uint32_t ea = 0;
                g_xog_invlpg_va = rx_mmio_ea(f, code, &ea) ? ea : UINT32_MAX;
            }
            if (op == NC_SYSOP_OTHER && sysreg_read(f, code, eip, size)) {
                finish(cpu, m, false);
                return;
            }
            run_sysop(cpu, sig, si, m, op, size);
            return;
        }
        break;
    default:
        break;
    }

    /* Code on a write-tracked page, which has no execute: open it for good. */
    if (sig == SIGSEGV && (m->err & 0x10) && (uint32_t)(uintptr_t)si->si_addr >> 14 == eip >> 14 &&
        nc_vm_phys(&g_core_for_faults->vm, eip) != UINT32_MAX && !g_va_soft[eip >> 14]) {
        sceKernelMprotect((void *)(uintptr_t)(eip & ~0x3FFFu), 0x4000, 0x1 | 0x2 | 0x4);
        return;
    }
    /* A page fault. First the Xbox's fs: (a fetch has no instruction to look at) */
    if (sig == SIGSEGV && !(m->err & 0x10) && fix_fs_prefix(eip)) {
        return;
    }
    g_frame_changed = false;
    if (!g_core_for_faults || !nc_core_fault(g_core_for_faults, sig, si, uc)) {
        fatal("guest fault nobody handled", sig, m, si);
    }
    if (g_frame_changed) {
        finish(cpu, m, false);
    }
}

/* ---- faults, as the core asks ---- */

NcFrame *nc_cpu_fault_frame(NcCpu *cpu, void *ucontext)
{
    (void)cpu;
    return (uint16_t)mc_of(ucontext)->cs == RX_GUEST_CS ? &rx_frame : NULL;
}

void nc_cpu_fault_resume(NcCpu *cpu, void *ucontext)
{
    (void)cpu;
    (void)ucontext;
    g_frame_changed = true;
}

volatile int rx_pf_depth;
uint32_t g_soft_filled;

/* A hole of a soft block the title has never used: its own page fault, here and now. */
bool rx_soft_fill(void *ucontext, uint32_t va)
{
    extern bool nc_paging_never_used(NcKernel *k, uint32_t va);
    NcKernel *k = g_cpu->cfg.opaque;
    NcFrame *f = nc_cpu_fault_frame(g_cpu, ucontext);
    if (!f || !nc_frame_if(f) || k->in_dpc || rx_pf_depth > 0 || g_steps_n) {
        return false;
    }
    NcVm *vm = &g_core_for_faults->vm;
    const uint32_t first = va & ~0x3FFFu;
    for (uint32_t o = 0; o < 0x4000u; o += NC_PAGE) {
        const uint32_t hole = first + o;
        if (nc_vm_phys(vm, hole) == UINT32_MAX && nc_paging_never_used(k, hole)) {
            if (!nc_kernel_page_fault(k, f, hole, false)) {
                return false;
            }
            g_soft_filled++;
            nc_cpu_fault_resume(g_cpu, ucontext);
            return true;
        }
    }
    return false;
}

static bool rx_frame_page_fault(NcFrame *f, uint32_t va, bool store)
{
    NcKernel *k = g_cpu->cfg.opaque;
    return nc_paging_fault(k, va) || nc_kernel_page_fault(k, f, va, store);
}

/* The title's own page fault at va, as the CPU would raise it. A table may already say otherwise. */
bool rx_guest_page_fault(void *ucontext, uint32_t va, bool store)
{
    NcKernel *k = g_cpu->cfg.opaque;
    NcFrame *f = nc_cpu_fault_frame(g_cpu, ucontext);
    if (!f) {
        return false;
    }
    if (nc_paging_fault(k, va)) {
        return true;    /* mapped now: the access runs again */
    }
    if (!nc_kernel_page_fault(k, f, va, store)) {
        return false;
    }
    nc_cpu_fault_resume(g_cpu, ucontext);
    return true;
}

void nc_cpu_fault_step(void *ucontext)
{
    mc_of(ucontext)->rflags |= RX_TF;
}

bool nc_cpu_fault_is_fetch(const void *ucontext)
{
    return (mc_of((void *)ucontext)->err & 0x10) != 0;
}

NcCpu *g_cpu_for_ram(void)
{
    return g_cpu;
}

bool nc_cpu_fault_is_write(const void *ucontext)
{
    return (mc_of((void *)ucontext)->err & 2) != 0;
}

bool nc_cpu_unaligned_fault(NcCpu *cpu, int code, void *ucontext)
{
    (void)cpu; (void)code; (void)ucontext;
    return false;   /* x86 does these itself */
}

bool nc_cpu_callret_fault(NcCpu *cpu, uintptr_t fault_addr, void *ucontext)
{
    (void)cpu; (void)fault_addr; (void)ucontext;
    return false;   /* no shadow stack: guest calls are real calls */
}

uint32_t nc_cpu_guest_eip_at(NcCpu *cpu, uintptr_t host_pc)
{
    (void)cpu;
    return host_pc < (1ull << 32) ? (uint32_t)host_pc : 0;
}

bool nc_cpu_sample_pc(NcCpu *cpu, uintptr_t host_pc, uint32_t *eip)
{
    (void)cpu;
    if (host_pc < (1ull << 32)) {
        *eip = (uint32_t)host_pc;
        return true;
    }
    *eip = (uint32_t)rx_frame.rip;
    return false;
}

/* A device register reached by a guest load or store: the move done here. */
bool rx_mmio_emulate(NcFrame *f, uint32_t addr);

bool rx_mmio_fault(void *ucontext, uint32_t addr)
{
    NcCpu *cpu = g_cpu;
    NcFrame *f = nc_cpu_fault_frame(cpu, ucontext);
    if (!f) {
        return false;
    }
    const uint32_t eip = (uint32_t)f->rip;
    uint8_t insn[15];
    memcpy(insn, (const void *)(uintptr_t)eip, sizeof(insn));
    if (!rx_mmio_emulate(f, addr)) {
        return false;
    }
    /* A move of the string kind stays in the emulator: it comes back by itself. */
    if ((uint32_t)f->rip != eip) {
        const unsigned len = (uint32_t)f->rip - eip;
        bool string = false;
        for (unsigned i = 0; i < len && i < sizeof(insn); i++) {
            string = string || insn[i] == 0xF3 || insn[i] == 0xF2 || (insn[i] >= 0xA4 && insn[i] <= 0xA7 && i + 1 == len) ||
                     (insn[i] >= 0xAA && insn[i] <= 0xAF && i + 1 == len);
        }
        if (!string && len <= 15) {
            const uint32_t moved_to = site_note(cpu, eip, RX_S_MMIO, len, insn);
            if (moved_to) {
                f->rip = moved_to;  /* the access is done; its neighbour is in the stub now */
            }
        }
    }
    g_frame_changed = true;
    return true;
}

/* ---- time ---- */

uint64_t nc_cpu_host_cycles(void) { return rx_rdtsc(); }
void nc_cpu_cycle_offset_add(NcCpu *cpu, uint64_t ticks) { cpu->cycle_offset += ticks; }
uint64_t nc_cpu_guest_cycles(NcCpu *cpu) { return rx_rdtsc() - cpu->cycle_offset; }
void nc_cpu_guest_cycles_set(NcCpu *cpu, uint64_t ticks) { cpu->cycle_offset = rx_rdtsc() - ticks; }
bool nc_cpu_x87_single_check(NcCpu *cpu) { (void)cpu; return false; }   /* a real x87 */
bool nc_cpu_x87_single(NcCpu *cpu) { (void)cpu; return false; }
void nc_cpu_x87_single_restore(NcCpu *cpu) { (void)cpu; }

/* ---- boot stages: what changed the kernel's memory ---- */

static uint8_t *g_ksnap;

void rx_kernel_stage(const char *stage)
{
    const uint8_t *km = (const uint8_t *)(uintptr_t)NC_KERNEL_VA;
    if (!g_ksnap) {
        g_ksnap = malloc(NC_KERNEL_SIZE);
        if (g_ksnap) {
            memcpy(g_ksnap, km, NC_KERNEL_SIZE);
        }
        nc_log("xog-boot", "%s: first snapshot; count export at +0x2a00 = %u", stage, nc_rd32(km + 0x2A00));
        return;
    }
    uint32_t first = UINT32_MAX, last = 0, changed = 0, zeroed = 0, in_data = 0;
    for (uint32_t i = 0; i < NC_KERNEL_SIZE; i++) {
        if (km[i] != g_ksnap[i]) {
            if (first == UINT32_MAX) {
                first = i;
            }
            last = i;
            changed++;
            if (!km[i]) {
                zeroed++;
            }
            if (i >= 0x2000 && i < 0xA000) {
                in_data++;
            }
        }
    }
    nc_log("xog-boot", "%s: %u bytes changed (%u to zero, %u in the data exports), +0x%x to +0x%x; count = %u", stage,
           changed, zeroed, in_data, first == UINT32_MAX ? 0 : first, last, nc_rd32(km + 0x2A00));
    memcpy(g_ksnap, km, NC_KERNEL_SIZE);
}

/* ---- life ---- */

/* From the title's first line: a fault in start-up reports itself too. */
void rx_signals_install(void)
{
    static char early_stack[1 << 20];
    stack_t ss = {.ss_sp = early_stack, .ss_size = sizeof(early_stack), .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = rx_sig_c;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGSYS, SIGABRT, SIGUSR2};
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}

NcCpu *nc_cpu_create(const NcCpuConfig *cfg, uint32_t fs_base)
{
    NcCpu *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) {
        return NULL;
    }
    cpu->cfg = *cfg;
    cpu->fs_base = fs_base;
    cpu->hooks = calloc(RX_HOOKS, sizeof(RxHook));
    cpu->altstack = malloc(RX_ALTSTACK);
    if (!cpu->hooks || !cpu->altstack) {
        return NULL;
    }
    /* touched now: the console can't write a signal frame on memory it would have to
     * bring in, and closes the title */
    memset(cpu->altstack, 0, RX_ALTSTACK);
    {
        uint8_t *gs = malloc(RX_ALTSTACK);
        if (!gs) {
            return NULL;
        }
        memset(gs, 0, RX_ALTSTACK);
        rx_gate_stack = (uint64_t)(uintptr_t)(gs + RX_ALTSTACK - 64) & ~(uint64_t)15;
    }
    __asm__ volatile("mov %%cs, %0" : "=r"(cpu->host_cs));
    __asm__ volatile("mov %%ss, %0" : "=r"(cpu->host_ss));
    __asm__ volatile("stmxcsr %0" : "=m"(rx_host_mxcsr));
    rx_data_sel = cpu->host_ss;
    g_cpu = cpu;

    rx_signals_install();
    nc_log("xog-cpu", "native CPU: guest cs %x, host cs %x ss %x, thread block %08x", RX_GUEST_CS, cpu->host_cs,
           cpu->host_ss, fs_base);
    return cpu;
}

/* The console's answers, once, before any game code: gs before and after a
 * signal, FPU state across a signal, fs turned into gs. */
static void rx_selfcheck(NcCpu *cpu)
{
    uint8_t *p = code_byte(RX_CHECK_VA);
    if (!p) {
        nc_log("xog-cpu", "check: scratch page isn't mapped");
        return;
    }
    static const uint8_t prog[] = {
        0x65, 0xA1, 0x1C, 0x00, 0x00, 0x00, /* mov eax, gs:[0x1C]   the block's self pointer */
        0x89, 0xC3,                         /* mov ebx, eax                                  */
        0xCC,                               /* int3                 a signal                 */
        0x65, 0xA1, 0x1C, 0x00, 0x00, 0x00, /* mov eax, gs:[0x1C]   after it                 */
        0x89, 0xC1,                         /* mov ecx, eax                                  */
        0x66, 0x0F, 0x6E, 0xC3,             /* movd xmm0, ebx                                */
        0xCC,                               /* int3                 the handler sees, sets it */
        0x66, 0x0F, 0x7E, 0xC2,             /* movd edx, xmm0                                */
        0x64, 0xA1, 0x1C, 0x00, 0x00, 0x00, /* mov eax, fs:[0x1C]   fs becomes gs            */
        0xF4,                               /* hlt                                           */
    };
    uint8_t saved[sizeof(prog)];
    memcpy(saved, p, sizeof(prog));
    memcpy(p, prog, sizeof(prog));
    const uint32_t kpcr = cpu->fs_base;
    RxContext c;
    nc_context_init(cpu, (NcCpuContext *)&c, RX_CHECK_VA, RX_CHECK_VA + 0xF00u);
    nc_frame_load(&rx_frame, (const NcCpuContext *)&c);
    /* a loaded state's pending interrupt waits for the game: here it would run the game's handler inside the check */
    rx_frame.vif = 0;
    const int irq_was = rx_irq_pending;
    rx_irq_pending = 0;
    g_in_check = 1;
    g_check_int3s = 0;
    rx_enter();
    g_in_check = 0;
    rx_irq_pending = rx_irq_pending || irq_was;
    memcpy(p, saved, sizeof(prog));
    const NcFrame *f = &rx_frame;
    const uint32_t ebx = (uint32_t)f->regs[NC_EBX], ecx = (uint32_t)f->regs[NC_ECX];
    const uint32_t edx = (uint32_t)f->regs[NC_EDX], esi = (uint32_t)f->regs[NC_ESI];
    const uint32_t eax = (uint32_t)f->regs[NC_EAX];
    nc_log("xog-cpu", "check: gs before signal %s (%08x), after %s (%08x), FPU seen %s (%08x), set %s (%08x), fs->gs %s (%08x)",
           ebx == kpcr ? "ok" : "BAD", ebx, ecx == kpcr ? "ok" : "BAD", ecx, esi == kpcr ? "ok" : "BAD", esi,
           edx == 0x12345678u ? "ok" : "BAD", edx, eax == kpcr ? "ok" : "BAD", eax);
    nc_log("xog-cpu", "check: FPU image at ucontext +0x%lx", g_fx_off);
    if (g_core_for_faults) {
        const NcKernel *k = &g_core_for_faults->kernel;
        nc_log("xog-cpu", "HalDiskCachePartitionCount at %08x reads %u before the game", k->data_va[40],
               nc_rd32(g_core_for_faults->vm.phys + nc_vm_phys(&g_core_for_faults->vm, k->data_va[40])));
    }
}

/* ---- the freeze probe ---- */
extern const char *volatile g_xog_kcall;
extern volatile uint64_t g_xog_kcalls;
volatile uint32_t g_xog_idle;
extern void nc_io_read_kinds(uint32_t *paced, uint32_t *free_reads);
extern uint64_t nc_present_new_frames(void);

#define FZ_SLOTS 12
typedef struct FzHist {
    uintptr_t key[FZ_SLOTS];
    uint32_t n[FZ_SLOTS];
} FzHist;

static void fz_add(FzHist *h, uintptr_t key)
{
    for (unsigned i = 0; i < FZ_SLOTS; i++) {
        if (h->key[i] == key || !h->n[i]) {
            h->key[i] = key;
            h->n[i]++;
            return;
        }
    }
}

/* the slots in order of count, the emptied one's count returned */
static uint32_t fz_top(FzHist *h, uintptr_t *key)
{
    unsigned best = 0;
    for (unsigned i = 1; i < FZ_SLOTS; i++) {
        if (h->n[i] > h->n[best]) {
            best = i;
        }
    }
    const uint32_t n = h->n[best];
    *key = h->key[best];
    h->n[best] = 0;
    return n;
}

volatile uint64_t g_xog_hdd_write_ms;

uint64_t rx_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t fz_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void fz_threads(const NcKernel *k)
{
    NcVm *vm = &g_core_for_faults->vm;
    const uint64_t now = nc_ke_now_ns();
    for (unsigned i = 0; i < k->threads_hi && i < NC_MAX_THREADS; i++) {
        const NcThread *t = &k->threads[i];
        if (t->state == NC_T_FREE || t->state == NC_T_TERMINATED) {
            continue;
        }
        const uint32_t obj = t->wait.count ? t->wait.objects[0] : 0;
        const uint32_t pa = obj ? nc_vm_phys(vm, obj) : UINT32_MAX;
        nc_log("xog-freeze", "  thread %u prio %d %s%s eip %08x esp %08x | waits on %u, first %08x type %d, %lld ms left%s",
               t->id, t->priority,
               t->state == NC_T_READY ? "ready" : t->state == NC_T_RUNNING ? "running" : t->state == NC_T_WAITING ? "waiting" : "suspended",
               t == k->current ? " (current)" : "", (uint32_t)((const RxContext *)&t->ctx)->rip, (uint32_t)((const RxContext *)&t->ctx)->regs[NC_ESP], t->wait.count, obj,
               pa != UINT32_MAX ? (int)vm->phys[pa] : -1,
               t->wait.deadline_ns ? (long long)((int64_t)(t->wait.deadline_ns - now) / 1000000) : -1ll, t->wait.cs ? ", critical section" : "");
    }
}

static void *freeze_probe(void *arg)
{
    NcCpu *cpu = arg;
    FzHist guest, host, host_ret, kernel;
    uint64_t last_frames = 0, last_t = fz_now_ms(), sig0[33], kc0 = 0, shown = 0;
    uint32_t paced0 = 0, free0 = 0, samples = 0, idle = 0, stacks = 0, stack_eip[3], stack[3][12];
    bool in = false, said = false;
    for (;;) {
        usleep(20000);
        const uint64_t t = fz_now_ms(), frames = nc_present_new_frames();
        if (frames && !said) {
            said = true;
            nc_log("xog-freeze", "probe running, first frames seen");
        }
        if (frames != last_frames) {
            if (in && t - last_t >= 200 && shown < 256 && g_core_for_faults) {
                shown++;
                uint32_t paced, free_reads;
                nc_io_read_kinds(&paced, &free_reads);
                nc_log("xog-freeze", "#%llu: %llu ms without a frame at frame %llu | %u samples: idle %u | signals segv +%llu bus +%llu ill +%llu trap +%llu | kernel calls +%llu | reads paced +%u, free +%u",
                       (unsigned long long)shown, (unsigned long long)(t - last_t), (unsigned long long)frames, samples, idle,
                       (unsigned long long)(g_sig_n[SIGSEGV] - sig0[SIGSEGV]), (unsigned long long)(g_sig_n[SIGBUS] - sig0[SIGBUS]),
                       (unsigned long long)(g_sig_n[SIGILL] - sig0[SIGILL]), (unsigned long long)(g_sig_n[SIGTRAP] - sig0[SIGTRAP]),
                       (unsigned long long)(g_xog_kcalls - kc0), paced - paced0, free_reads - free0);
                for (unsigned i = 0; i < 6; i++) {
                    uintptr_t key;
                    uint32_t n = fz_top(&guest, &key);
                    if (n) {
                        nc_log("xog-freeze", "  guest %08x: %u", (uint32_t)key, n);
                    }
                    n = fz_top(&host, &key);
                    if (n) {
                        nc_log("xog-freeze", "  host %llx: %u", (unsigned long long)key, n);
                    }
                    n = fz_top(&host_ret, &key);
                    if (n) {
                        nc_log("xog-freeze", "  host caller %llx: %u", (unsigned long long)key, n);
                    }
                    n = fz_top(&kernel, &key);
                    if (n) {
                        nc_log("xog-freeze", "  kernel %s: %u", (const char *)key, n);
                    }
                }
                for (unsigned i = 0; i < stacks; i++) {
                    char line[160];
                    int o = snprintf(line, sizeof(line), "  stack at %08x:", stack_eip[i]);
                    for (unsigned w = 0; w < 12 && o < (int)sizeof(line) - 10; w++) {
                        o += snprintf(line + o, sizeof(line) - o, " %08x", stack[i][w]);
                    }
                    nc_log("xog-freeze", "%s", line);
                }
            }
            in = false;
            last_frames = frames;
            last_t = t;
            continue;
        }
        if (nc_paused()) {
            in = false;
            last_t = t;
            continue;
        }
        if (!frames || t - last_t < 100 || !g_core_for_faults) {
            continue;
        }
        if (!in) {
            in = true;
            memset(&guest, 0, sizeof(guest));
            memset(&host, 0, sizeof(host));
            memset(&host_ret, 0, sizeof(host_ret));
            memset(&kernel, 0, sizeof(kernel));
            samples = idle = stacks = 0;
            for (unsigned i = 0; i < 33; i++) {
                sig0[i] = g_sig_n[i];
            }
            kc0 = g_xog_kcalls;
            nc_io_read_kinds(&paced0, &free0);
        }
        g_sample_seen = 0;
        g_want_sample = 1;
        pthread_kill(cpu->thread, SIGUSR2);
        usleep(1500);
        samples++;
        if (!g_sample_seen) {
            if (g_xog_idle) {
                idle++;
            } else {
                fz_add(&kernel, (uintptr_t)(g_xog_kcall ? g_xog_kcall : "(none yet)"));
            }
        } else if (g_sample_guest) {
            fz_add(&guest, g_sample_rip);
            if (stacks < 3 && (samples == 3 || samples == 10 || samples == 25)) {
                stack_eip[stacks] = g_sample_rip;
                for (unsigned w = 0; w < 12; w++) {
                    const uint64_t a = (uint64_t)g_sample_esp + w * 4;
                    stack[stacks][w] = readable(a) ? nc_rd32(g_core_for_faults->vm.phys + nc_vm_phys(&g_core_for_faults->vm, (uint32_t)a)) : 0xdeaddeadu;
                }
                stacks++;
            }
        } else {
            fz_add(&host, (uintptr_t)g_sample_rip64);
            fz_add(&host_ret, (uintptr_t)g_sample_ret64);
        }
        if (samples == 8) {
            fz_threads(&g_core_for_faults->kernel);     /* comes before its freeze's line */
        }
    }
    return NULL;
}

/* Every 3 s: is it alive, where is the guest, what is it doing. The kick is held
 * while the kernel runs, so no sample means the guest is inside a kernel call. */
static void *heartbeat(void *arg)
{
    NcCpu *cpu = arg;
    uint64_t last[33] = {0}, last_mmio = 0, last_frames = 0;
    for (;;) {
        usleep(1000000);
        g_sample_seen = 0;
        g_want_sample = 1;
        pthread_kill(cpu->thread, SIGUSR2);
        usleep(3000);
        const uint64_t mmio = nc_mmio_total(), frames = rx_frames_shown;
        static uint64_t last_gate, last_site;
        nc_hold_report();
        nc_log("xog-beat", "gate +%llu, rewritten sites ran +%llu, %u sites", (unsigned long long)(g_gate_n - last_gate),
               (unsigned long long)(g_site_n - last_site), g_nstubs);
        last_gate = g_gate_n;
        last_site = g_site_n;
        nc_log("xog-beat", "frames +%llu (%llu) | segv +%llu bus +%llu ill +%llu trap +%llu kick +%llu | device +%llu, last %08x | "
               "guest %s eip %08x esp %08x | last frame eip %08x",
               (unsigned long long)(frames - last_frames), (unsigned long long)frames,
               (unsigned long long)(g_sig_n[SIGSEGV] - last[SIGSEGV]), (unsigned long long)(g_sig_n[SIGBUS] - last[SIGBUS]),
               (unsigned long long)(g_sig_n[SIGILL] - last[SIGILL]), (unsigned long long)(g_sig_n[SIGTRAP] - last[SIGTRAP]),
               (unsigned long long)(g_sig_n[SIGUSR2] - last[SIGUSR2]), (unsigned long long)(mmio - last_mmio), rx_last_mmio,
               !g_sample_seen ? "in kernel" : g_sample_guest ? "running" : "in host", g_sample_seen ? g_sample_rip : 0,
               g_sample_seen ? g_sample_esp : 0, (uint32_t)rx_frame.rip);
        if (g_sample_seen && g_sample_guest) {
            dump_words("code", g_sample_rip, 6);
            dump_words("stack", g_sample_esp, 12);
        }
        for (unsigned i = 0; i < 33; i++) {
            last[i] = g_sig_n[i];
        }
        last_mmio = mmio;
        last_frames = frames;
    }
    return NULL;
}

/* The Xbox's CPU gets a whole console core, its other hardware thread left idle; everything
 * else, Mesa's threads too, runs on the rest (as XPSemu lays it out, and the usual thread
 * layout). Applied from here so that threads made so far move too. */
int scePthreadGetaffinity(pthread_t thread, unsigned long long *mask);
int scePthreadSetaffinity(pthread_t thread, unsigned long long mask);

extern void rx_thread_layout(unsigned long long rest, unsigned long long decoder, unsigned long long render);

static void rx_core_layout(void)
{
    unsigned long long all = 0;
    if (scePthreadGetaffinity(pthread_self(), &all) != 0 || !all) {
        nc_log("xog-cpu", "core layout: no mask, left as it is");
        return;
    }
    /* Whole cores from the top: the CPU, then the GPU's decoder, then its render thread. Both
     * threads of each, so a system thread on one doesn't stop ours with nowhere to go. */
    unsigned long long core[3] = {0, 0, 0};
    unsigned found = 0;
    for (int i = 62; i >= 0 && found < 3; i -= 2) {
        if ((all >> i & 1) && (all >> (i + 1) & 1)) {
            core[found++] = 3ull << i;
        }
    }
    if (found < 3 || !(all & ~(core[0] | core[1] | core[2]))) {
        nc_log("xog-cpu", "core layout: not enough whole cores in %llx", all);
        return;
    }
    const unsigned long long mine = core[0], rest = all & ~(core[0] | core[1] | core[2]);
    cpuset_t set;
    int rc = -1;
    static const size_t sizes[] = {8, 16, 32};
    for (unsigned i = 0; i < 3 && rc != 0; i++) {
        memset(&set, 0, sizeof(set));
        memcpy(&set, &rest, sizeof(rest));
        rc = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizes[i], &set);
    }
    const int r2 = scePthreadSetaffinity(pthread_self(), mine);
    rx_thread_layout(rest, core[1], core[2]);
    nc_log("xog-cpu", "core layout: all %llx, the Xbox's CPU %llx (rc %08x), decoder %llx, render %llx, the rest %llx (rc %d)", all, mine,
           (unsigned)r2, core[1], core[2], rest, rc);
}

extern __thread bool rx_on_vcpu;

void nc_cpu_run(NcCpu *cpu, const NcCpuContext *c)
{
    cpu->thread = pthread_self();
    rx_on_vcpu = true;
    stack_t ss = {.ss_sp = cpu->altstack, .ss_size = RX_ALTSTACK, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    uint64_t fsb = 0, gsb = cpu->fs_base;
    sysarch(AMD64_GET_FSBASE, &fsb);
    const int r = sysarch(AMD64_SET_GSBASE, &gsb);
    /* fs: cut to 32 bits must fault, not land in memory the guest has. */
    nc_log("xog-cpu", "this thread: fs base %llx, gs base set to %llx (rc %d)", (unsigned long long)fsb,
           (unsigned long long)gsb, r);
    rx_kernel_stage("before the check");
    rx_selfcheck(cpu);
    rx_kernel_stage("after the check");
    {
        const uint64_t hz = sceKernelGetTscFrequency();
        g_tsc_ratio = (uint64_t)(((unsigned __int128)733333333ull << 32) / hz);
        if (!g_state_loaded) {
            cpu->cycle_offset = rx_rdtsc();     /* a state brought its own */
        }
        if (g_state_loaded) {
            /* patched and swept when it was saved: the stubs just get this run's gate, and the pad its
             * address (the state's RAM has the address of the build that saved it) */
            rx_gate_pad((uint8_t *)nc_vm_host(&g_core_for_faults->vm, g_gate_pad), g_gate_pad);
            for (unsigned i = 0; i < g_nstubs; i++) {
                rx_gate_call((uint8_t *)(uintptr_t)RX_SITE_VA(i));
            }
            nc_log("xog-cpu", "from a state: %u sites kept", g_nstubs);
            rx_ftol_patch();    /* xogfix=ftol: the state's memory has the game's own helper */
            rx_dsnull_patch();
        } else {
            rx_title_patches(cpu);
            sweep_image(cpu, cpu->image_lo, cpu->image_hi);
        }
        nc_log("xog-cpu", "sweep: rdtsc %u, cpuid %u, pushfd %u, popfd %u; TSC %llu Hz", g_sweep_counts[RX_K_RDTSC],
               g_sweep_counts[RX_K_CPUID], g_sweep_counts[RX_K_PUSHFD], g_sweep_counts[RX_K_POPFD],
               (unsigned long long)hz);
    }
    hooks_sync(cpu, cpu->image_lo, cpu->image_hi);
    rx_kernel_stage("after the hooks");
    rx_core_layout();
    cpu->running = true;
    {
        pthread_t hb;
        pthread_create(&hb, NULL, heartbeat, cpu);
        pthread_detach(hb);
        pthread_create(&hb, NULL, freeze_probe, cpu);
        pthread_detach(hb);
    }
    nc_frame_load(&rx_frame, c);
    rx_enter();
    cpu->running = false;
}

#define RX_KICK_WAIT_TSC 48000u     /* 30 us at the console's 1.5963 GHz */

void nc_cpu_request_interrupt(NcCpu *cpu)
{
    if (__atomic_exchange_n(&rx_irq_pending, 1u, __ATOMIC_SEQ_CST) != 0 || !cpu->running ||
        pthread_equal(pthread_self(), cpu->thread)) {
        return;
    }
    /* The gate's way back looks at the flag, and in game it comes every 20-50 us: a short
     * wait here on a device thread saves the guest's core a signal of about 11 us. Bounded,
     * so a guest in a long loop still gets its interrupt almost at once. */
    const uint64_t t0 = rx_rdtsc();
    while (__atomic_load_n(&rx_irq_pending, __ATOMIC_SEQ_CST) && !__atomic_load_n(&rx_in_gate, __ATOMIC_SEQ_CST)) {
        if (rx_rdtsc() - t0 > RX_KICK_WAIT_TSC) {
            pthread_kill(cpu->thread, SIGUSR2);
            return;
        }
        __builtin_ia32_pause();
    }
}
