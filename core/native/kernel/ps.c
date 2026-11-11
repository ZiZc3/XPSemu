/*
 * hakuX native core — threads and the scheduler.
 *
 * Every guest thread runs on the core's one JIT thread. A thread that isn't
 * running keeps its CPU state in NcThread.ctx; switching happens inside a
 * system op (a kernel call) by saving the frame and loading another context.
 * Like the Xbox's one CPU, only one guest thread runs at a time; the highest
 * priority ready thread runs, round-robin within a priority.
 *
 * Guest layouts (nxdk xboxkrnl.h, CC0; 32-bit):
 *   KTHREAD (0x110): Header 0x00 (Type, Size 0x02, SignalState 0x04, WaitListHead 0x08),
 *     MutantListHead 0x10, StackBase 0x1C, StackLimit 0x20, KernelStack 0x24, TlsData 0x28,
 *     State 0x2C, NpxState 0x30, Priority 0x32, ApcState.ApcListHead[2] 0x34, ApcState.Process 0x44,
 *     Quantum 0x6C, BasePriority 0x70, ThreadListEntry 0x104
 *   KPROCESS (0x1C): ReadyListHead 0x00, ThreadListHead 0x08, StackCount 0x10,
 *     ThreadQuantum 0x14 (XSetProcessQuantumLength: ms * 3), BasePriority 0x18
 *   ETHREAD (0x140): Tcb 0x00, ExitStatus 0x120, UniqueThread 0x12C, StartAddress 0x130
 *   KPCR: NT_TIB (ExceptionList 0x00, StackBase 0x04, StackLimit 0x08),
 *     PRCB at 0x28 starting with CurrentThread
 */
#include <stdlib.h>
#include <string.h>
#include "native/kernel/kernel.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define KT_TYPE          0x00
#define KT_SIZE          0x02
#define KT_SIGNAL_STATE  0x04
#define KT_WAIT_LIST     0x08
#define KT_MUTANT_LIST   0x10
#define KT_STACK_BASE    0x1C
#define KT_STACK_LIMIT   0x20
#define KT_KERNEL_STACK  0x24
#define KT_TLS_DATA      0x28
#define KT_STATE         0x2C
#define KT_NPX_STATE     0x30
#define KT_PRIORITY      0x32
#define KT_APC_LISTS     0x34
#define KT_APC_PROCESS   0x44
#define KT_QUANTUM       0x6C
#define KT_BASE_PRIORITY 0x70
#define KT_THREAD_LIST   0x104
#define KTHREAD_SIZE     0x110
#define ET_EXIT_STATUS   0x120
#define ET_UNIQUE_THREAD 0x12C
#define ET_START_ADDRESS 0x130
#define ETHREAD_SIZE     0x140
#define THREAD_OBJECT    6
#define KP_READY_LIST    0x00
#define KP_THREAD_LIST   0x08
#define KP_QUANTUM       0x14
#define KP_BASE_PRIORITY 0x18
#define KPROCESS_SIZE    0x1C

#define PCR_EXCEPTION_LIST 0x00
#define PCR_STACK_BASE     0x04
#define PCR_STACK_LIMIT    0x08
#define PCR_CURRENT_THREAD 0x28

/* FX_SAVE_AREA (nboxkrnl ki.hpp): FLOATING_SAVE_AREA (an FXSAVE image, 512
 * bytes, then Cr0NpxState) and 12 bytes of alignment */
#define FX_SAVE_AREA          0x210
#define FX_CONTROL_WORD       0x00
#define FX_MXCSR              0x18
#define FX_CR0_NPX_STATE      0x200
#define NPX_STATE_NOT_LOADED  0x0A      /* CR0_TS | CR0_MP */

#define STACK_REGION   0xD0000000u
#define STACK_GUARD    0x10000u
#define DEFAULT_STACK  0x10000u
#define KERNEL_STACK_SIZE 0x3000u       /* the least the kernel makes (nboxkrnl kernel.hpp) */
#define DEFAULT_PRIORITY 8

static void list_init(NcVm *vm, uint32_t head)
{
    nc_vm_wr32(vm, head, head);
    nc_vm_wr32(vm, head + 4, head);
}

/* Stack slots in the kernel stack region, each with an unmapped guard below.
 * A slot given back (a thread's end, MmDeleteKernelStack) is taken again by
 * a stack it holds, what it has over kept as a slot of its own; the region
 * ends at the GPU's view of RAM, where an allocation fails instead of
 * mapping over it (titles start a thread per movie and load). */
bool nc_ps_stack_alloc(NcKernel *k, uint32_t size, uint32_t *base)
{
    if (!k->next_stack_va) {
        k->next_stack_va = STACK_REGION;
    }
    const uint32_t span = STACK_GUARD + nc_align_up(size, STACK_GUARD);
    for (unsigned i = 0; i < k->nstack_free; i++) {
        const uint32_t va = k->stack_free[i].va, have = k->stack_free[i].span;
        if (have < span || !nc_vm_alloc_at(k->vm, va + STACK_GUARD, size, NULL)) {
            continue;
        }
        k->stack_free[i] = k->stack_free[--k->nstack_free];
        if (have - span >= 2 * STACK_GUARD) {
            k->stack_free[k->nstack_free].va = va + span;   /* the rest, a slot of its own */
            k->stack_free[k->nstack_free++].span = have - span;
        }
        *base = va + STACK_GUARD;
        return true;
    }
    const uint32_t slot = k->next_stack_va;
    if (span > NC_GPU_WINDOW - slot) {
        nc_log("hakuX-native", "ps: the stack region is full (%u KiB more asked)", size >> 10);
        return false;
    }
    if (!nc_vm_alloc_at(k->vm, slot + STACK_GUARD, size, NULL)) {
        return false;
    }
    k->next_stack_va = slot + span;
    *base = slot + STACK_GUARD;
    return true;
}

void nc_ps_stack_free(NcKernel *k, uint32_t base, uint32_t size)
{
    const uint32_t slot = base - STACK_GUARD, span = STACK_GUARD + nc_align_up(size, STACK_GUARD);
    if (base < STACK_REGION + STACK_GUARD || base % STACK_GUARD || !size || span > k->next_stack_va - slot ||
        k->nstack_free == sizeof(k->stack_free) / sizeof(k->stack_free[0])) {
        return;     /* not a slot of the region, or no room to keep it: its addresses stay used */
    }
    for (unsigned i = 0; i < k->nstack_free; i++) {
        if (slot < k->stack_free[i].va + k->stack_free[i].span && k->stack_free[i].va < slot + span) {
            return; /* given back already */
        }
    }
    k->stack_free[k->nstack_free].va = slot;
    k->stack_free[k->nstack_free++].span = span;
}

static NcThread *thread_create(NcKernel *k, uint32_t stack_size, uint32_t tls_size, uint32_t ext_size)
{
    NcThread *t = NULL;
    for (unsigned i = 0; i < NC_MAX_THREADS && !t; i++) {
        if (k->threads[i].state == NC_T_FREE) {
            t = &k->threads[i];
        }
    }
    if (!t) {
        nc_log("hakuX-native", "ps: out of thread slots");
        return NULL;
    }
    if ((unsigned)(t - k->threads) + 1 > k->threads_hi) {
        k->threads_hi = (unsigned)(t - k->threads) + 1;
    }
    uint32_t et = nc_ob_create(k, NC_OB_THREAD, ETHREAD_SIZE + ext_size);
    if (!et) {
        return NULL;
    }
    /* The stack's top as the Xbox kernel lays it out (KiInitializeContextThread;
     * nboxkrnl, after Cxbx-Reloaded): the FX save area right under StackBase,
     * the TLS under it, the thread's frames under that. The TIB's StackBase
     * (fs:[4]) is the save area's bottom: XAPI sizes the TLS as
     * align16(template) + 4 and indexes it from there with a negative TLS
     * index, so its first dword (pointing at the 16-byte aligned block after
     * it) is at exactly fs:[4] - TlsDataSize. Without the save area a title's
     * frames sat 0x210 bytes nearer the top than on the console: Silent
     * Hill 2 reads 0x170 bytes over its stack pointer near the bottom of its
     * calls, into the save area there, past the stack here (a fault the
     * title doesn't handle: the game gone, loading a room). The kernel
     * makes no stack under KERNEL_STACK_SIZE, the save area and TLS in it
     * (PsCreateSystemThreadEx): Splinter Cell - Double Agent asks for less
     * than a page and its thread's frames went past the one page it got,
     * the save area and TLS taking 0x2A4 of it. */
    const uint32_t tls = nc_align_up(tls_size, 4);
    uint32_t size = nc_align_up((stack_size ? stack_size : DEFAULT_STACK) + FX_SAVE_AREA + tls + 16, NC_PAGE);
    if (size < KERNEL_STACK_SIZE) {
        size = KERNEL_STACK_SIZE;
    }
    uint32_t base;
    if (!nc_ps_stack_alloc(k, size, &base)) {
        return NULL;
    }
    const uint32_t top = base + size;
    const uint32_t tib = top - FX_SAVE_AREA;

    NcVm *vm = k->vm;
    uint8_t *fx = nc_vm_host(vm, tib);         /* the top page's: a slot may come back used */
    memset(fx, 0, FX_SAVE_AREA);
    *(uint16_t *)(fx + FX_CONTROL_WORD) = 0x27F;
    nc_vm_wr32(vm, tib + FX_MXCSR, 0x1F80);
    nc_vm_wr32(vm, tib + FX_CR0_NPX_STATE, NPX_STATE_NOT_LOADED);
    uint8_t *kt = nc_vm_host(vm, et);
    kt[KT_TYPE] = THREAD_OBJECT;
    kt[KT_SIZE] = KTHREAD_SIZE / 4;
    kt[KT_NPX_STATE] = NPX_STATE_NOT_LOADED;
    list_init(vm, et + KT_WAIT_LIST);
    list_init(vm, et + KT_MUTANT_LIST);
    list_init(vm, et + KT_APC_LISTS);
    list_init(vm, et + KT_APC_LISTS + 8);
    nc_vm_wr32(vm, et + KT_STACK_BASE, top);
    nc_vm_wr32(vm, et + KT_STACK_LIMIT, base);
    nc_vm_wr32(vm, et + KT_KERNEL_STACK, (tib - tls) & ~15u);
    nc_vm_wr32(vm, et + KT_TLS_DATA, tls ? tib - tls : 0);
    if (tls >= 4) {
        nc_vm_wr32(vm, tib - tls, tib - tls + 4);
    }
    kt[KT_PRIORITY] = DEFAULT_PRIORITY;
    kt[KT_BASE_PRIORITY] = DEFAULT_PRIORITY;
    nc_vm_wr32(vm, et + KT_QUANTUM, 36);
    if (!k->kprocess && (k->kprocess = nc_pool_alloc(k, KPROCESS_SIZE)) != 0) {
        memset(nc_vm_host(vm, k->kprocess), 0, KPROCESS_SIZE);
        list_init(vm, k->kprocess + KP_READY_LIST);
        list_init(vm, k->kprocess + KP_THREAD_LIST);
        nc_vm_wr32(vm, k->kprocess + KP_QUANTUM, 60); /* 20 ms */
        *(uint8_t *)nc_vm_host(vm, k->kprocess + KP_BASE_PRIORITY) = DEFAULT_PRIORITY;
    }
    nc_vm_wr32(vm, et + KT_APC_PROCESS, k->kprocess);

    NcApc *apcs = t->apcs; /* the slot's APC queue storage is reused */
    const unsigned apc_cap = t->apc_cap;
    memset(t, 0, sizeof(*t));
    t->apcs = apcs;
    t->apc_cap = apc_cap;
    t->state = NC_T_SUSPENDED;
    t->ethread = et;
    t->id = ++k->next_thread_id * 4;
    t->priority = DEFAULT_PRIORITY;
    t->stack_base = top;
    t->stack_limit = base;
    t->tib_base = tib;
    t->frame_top = (tib - tls) & ~15u;
    t->exception_list = 0xFFFFFFFFu;
    nc_vm_wr32(vm, et + ET_UNIQUE_THREAD, t->id);
    return t;
}

/* After a state saved before the FX save area (THRD v1): its threads' frames
 * sit where they were, 0x210 bytes nearer their stacks' tops than the
 * console has them. A page over each such stack, zeroed, gives a title's
 * reads past the top what the save area gives them there (Silent Hill 2's
 * main thread reads 12 bytes over its top loading a room); it goes with the
 * stack (the scheduler's free of a terminated thread). Nothing else is ever
 * there: the region's slots begin with their guards. */
void nc_ps_old_stacks(NcKernel *k)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        const NcThread *t = &k->threads[i];
        if (t->state == NC_T_FREE || t->tib_base || t->stack_base >= NC_GPU_WINDOW ||
            nc_vm_phys(k->vm, t->stack_base) != UINT32_MAX) {
            continue;
        }
        if (nc_vm_alloc_at(k->vm, t->stack_base, NC_PAGE, NULL)) {
            memset(nc_vm_host(k->vm, t->stack_base), 0, NC_PAGE);
            n++;
        }
    }
    if (n) {
        nc_log("hakuX-native", "ps: %u threads from a state without the FX save area: a page over each stack", n);
    }
}

/* KPCR fields that follow the running thread. */
static void pcr_enter(NcKernel *k, NcThread *t)
{
    nc_vm_wr32(k->vm, k->kpcr_va + PCR_EXCEPTION_LIST, t->exception_list);
    nc_vm_wr32(k->vm, k->kpcr_va + PCR_STACK_BASE, t->tib_base ? t->tib_base : t->stack_base);
    nc_vm_wr32(k->vm, k->kpcr_va + PCR_STACK_LIMIT, t->stack_limit);
    nc_vm_wr32(k->vm, k->kpcr_va + PCR_CURRENT_THREAD, t->ethread);
}

NcThread *nc_kernel_boot_thread(NcKernel *k, NcCpu *cpu, uint32_t entry, uint32_t stack_size)
{
    k->cpu = cpu;
    NcThread *t = thread_create(k, stack_size, 0, 0);
    if (!t) {
        return NULL;
    }
    nc_vm_wr32(k->vm, t->ethread + ET_START_ADDRESS, entry);
    uint32_t esp = t->frame_top - 16 - 4;
    nc_vm_wr32(k->vm, esp, k->thread_return_va);
    nc_context_init(cpu, &t->ctx, entry, esp);
    t->state = NC_T_RUNNING;
    k->current = t;
    pcr_enter(k, t);
    nc_irq_attach(cpu);
    return t;
}

NcThread *nc_ps_thread_of(NcKernel *k, uint32_t ethread)
{
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        if (k->threads[i].state != NC_T_FREE && k->threads[i].ethread == ethread) {
            return &k->threads[i];
        }
    }
    return NULL;
}

NcThread *nc_ps_kernel_thread(NcKernel *k, uint32_t stack_size, int priority)
{
    NcThread *t = thread_create(k, stack_size, 0, 0);
    if (t) {
        t->priority = priority;
        nc_log("hakuX-native", "ps: kernel thread %u, priority %d, stack %08x-%08x", t->id, priority,
               t->stack_limit, t->stack_base);
    }
    return t;
}

/* The process quantum: KPROCESS ThreadQuantum, in thirds of a millisecond
 * (60: 20 ms; XSetProcessQuantumLength writes it). */
static uint64_t quantum_ns(NcKernel *k)
{
    const uint32_t q = k->kprocess ? nc_vm_rd32(k->vm, k->kprocess + KP_QUANTUM) : 0;
    return (uint64_t)(q ? q : 60) * 1000000u / 3;
}

/* Threads of one priority share the CPU in slices. Without them a thread
 * polling at that priority kept it for good once interrupts handed it back:
 * Bink's scheduler in Halo spins on a zero-timeout wait for a task mutex the
 * title's thread holds, ready at the same priority, and the movie stopped. */
static uint8_t g_boost[NC_MAX_THREADS];     /* beside the thread: a state's layout stays, and a load starts with none */

int nc_eff_priority(const NcKernel *k, const NcThread *t)
{
    const int p = t->priority;
    return p < 15 && g_boost[t - k->threads] ? p + 1 : p;
}

/* xogfix=waitquantum (patch 315, per game: SSX 3): NT's quantum rules for a thread that waits. NT takes a
 * wait's charge off the slice it has (KiUnwaitThread, WAIT_QUANTUM_DECREMENT) and doesn't renew a boost the
 * thread still holds; here a waiting thread got a whole new 20 ms slice and its boost again every time. SSX 3's
 * render thread blocks ~60 times a second, so its slice never ran out and the XAPI timer thread at the same
 * priority (timeSetEvent: EA's 60 Hz movie clock) ran about once in 5 s: the THX intro froze. */
static int g_waitquantum = -1;      /* -1 not read yet; on by the game's start line or by the watch below */

static bool waitquantum_on(void)
{
    if (g_waitquantum < 0) {
        const char *e = getenv("XOG_FIX");
        g_waitquantum = e && strstr(e, "waitquantum") ? 1 : 0;
    }
    return g_waitquantum;
}

/* Patch 316: both scheduling fixes turn on by themselves, only in a game that gets stuck (user: "only when
 * the game needs it, so no game gets hurt"); one that never does keeps today's rules. Each turn-on is logged.
 * g_ready_since: when a ready thread became ready (0: running or waiting). */
static uint64_t g_ready_since[NC_MAX_THREADS];
/* the wait-quantum watch, over one-second windows: how long each thread was ready but not run, how long each
 * ran, and how many slices ended (a same-priority turn) */
static uint64_t g_ready_ns[NC_MAX_THREADS], g_run_ns[NC_MAX_THREADS], g_watch_from, g_watch_last;
static unsigned g_watch_slices;
static unsigned g_yields[NC_MAX_THREADS];      /* NtYieldExecution calls per thread in the window */
static int g_yield_mode = -1;       /* as g_waitquantum, for xogfix=yield */
static NcThread *g_yield_asker;     /* the thread whose NtYieldExecution this schedule is */

static bool yield_on(void)
{
    if (g_yield_mode < 0) {
        const char *e = getenv("XOG_FIX");
        g_yield_mode = e && strstr(e, "yield") ? 1 : 0;
    }
    return g_yield_mode;
}

void nc_ps_boost(const NcKernel *k, const NcThread *t, int increment)
{
    if (waitquantum_on() && g_boost[t - k->threads]) {
        return;     /* it still holds one: it decays at the end of its slice */
    }
    g_boost[t - k->threads] = increment > 0 && t->priority < 16;
}

void nc_ps_unboost(const NcKernel *k, const NcThread *t)
{
    g_boost[t - k->threads] = 0;
}

void nc_ps_slice_end(NcKernel *k, uint64_t now)
{
    NcThread *cur = k->current;
    if (!cur || cur != k->slice_thread || cur->state != NC_T_RUNNING) {
        return; /* an ISR or a DPC runs: the slice ends once its thread is back */
    }
    g_boost[cur - k->threads] = 0;
    for (unsigned i = 0; i < k->threads_hi; i++) {
        const NcThread *t = &k->threads[i];
        if (t != cur && t->state == NC_T_READY && !t->suspend && nc_eff_priority(k, t) >= nc_eff_priority(k, cur)) {
            /* the back of its priority's queue (nc_ps_schedule), a new quantum */
            cur->slice_expired = true;
            k->resched = true;
            k->slice_due_ns = 0;
            g_nc_sched.slices++;
            g_watch_slices++;
            return;
        }
    }
    cur->slice_used_ns = 0;
    cur->slice_start_ns = now;
    k->slice_due_ns = now + quantum_ns(k);
}

/* A wait satisfied without blocking still costs quantum, as NT charges it. A loop that waits on a free
 * object all the time then gives way to its priority's other ready threads a few times per quantum, not once
 * in 20 ms. Priorities 14 and up aren't charged. */
void nc_ps_charge_wait(NcKernel *k)
{
    NcThread *cur = k->current;
    if (!cur || cur != k->slice_thread || cur->priority >= 14 || !k->slice_due_ns) {
        return;
    }
    const uint64_t charge = 10ull * 1000000u / 3;
    k->slice_due_ns = k->slice_due_ns > charge ? k->slice_due_ns - charge : 1;
    const uint64_t now = nc_ke_now_ns();
    if (now >= k->slice_due_ns) {
        nc_ps_slice_end(k, now);
    }
}

/* NtYieldExecution's caller: any other ready thread first, of any priority (not in NcKernel: a state's size stays) */
static NcThread *g_yielder;

void nc_ps_yield(NcKernel *k)
{
    if (k->current) {
        k->current->slice_expired = true;   /* behind the threads it gives way to */
    }
    /* As the Xbox's NtYieldExecution: another ready thread runs if there is one, whatever its priority (NT picks
     * the highest ready from priority 1 up, not only the yielder's own); else the yielder goes on. */
    /* only for a game that asks for it (xogfix=yield, patch 126) or got stuck yielding (patch 316): Halo's
     * sound went with it for all */
    g_yield_asker = k->current;
    if (k->current && k->current >= k->threads && k->current < k->threads + NC_MAX_THREADS) {
        g_yields[k->current - k->threads]++;
    }
    if (yield_on()) {
        g_yielder = k->current;
    }
    k->resched = true;
}

/* XPSemu patch 196 (Sega Chihiro only): a thread that waits at DISPATCH_LEVEL or above takes its IRQL with it,
 * as NT's KiSwapThread: the others (and the idle loop's DPCs) run at theirs, it gets it back when it runs again.
 * SEGABOOT cancels a USB request that timed out and waits for the cancel at DISPATCH_LEVEL; the IRQL stayed up
 * for everyone, the OHCI DPC that finishes the cancel never ran: a black screen forever. */
static uint8_t g_wait_irql[NC_MAX_THREADS];
static bool g_wait_irql_held[NC_MAX_THREADS];

static bool chihiro_irql_on(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("NC_CHIHIRO") != NULL;
    }
    return on;
}

static uint8_t *ps_irql(NcKernel *k)
{
    return (uint8_t *)nc_vm_host(k->vm, k->kpcr_va + 0x24);
}

void nc_ps_schedule(NcKernel *k, NcFrame *f)
{
    g_nc_sched.schedules++;
    if (chihiro_irql_on()) {
        static uint64_t s_last_ns;
        const uint64_t t = nc_ke_now_ns();
        if (t - s_last_ns > 10000000000ull) {
            s_last_ns = t;
            char held[160] = "";
            int o = 0;
            for (unsigned i = 0; i < NC_MAX_THREADS && o < (int)sizeof(held) - 16; i++) {
                if (g_wait_irql_held[i]) {
                    o += snprintf(held + o, sizeof(held) - (size_t)o, " t%u:%u", k->threads[i].id, g_wait_irql[i]);
                }
            }
            nc_log("hakuX-native", "ps: IRQL %u, current t%u, in ISR %d, in DPC %d; held:%s (patch 199 tool)",
                   *ps_irql(k), k->current ? k->current->id : 0, k->in_isr, k->in_dpc, o ? held : " none");
        }
    }
    k->resched = false;
    NcThread *cur = k->current;
    /* The frame always goes back through the saved context: a wait that ends
     * in this call writes its status there, even when the same thread
     * continues. */
    if (cur && cur->state != NC_T_TERMINATED && !cur->fresh) {
        nc_frame_save(f, &cur->ctx);
        cur->exception_list = nc_vm_rd32(k->vm, k->kpcr_va + PCR_EXCEPTION_LIST);
    }
    /* Threads of one priority take turns in the order they became ready, as
     * the NT scheduler's queues have them. Round-robin by slot after the
     * thread that switched out, the threads in the slots between two
     * periodic threads and a spinning one never ran: Splinter Cell Chaos
     * Theory's DirectSound thread polls at the main thread's priority, three
     * timer threads woke every few milliseconds, and each wait handed the CPU
     * back to the poller (the slots after them), which got a new quantum
     * every time; the main thread, ready in an earlier slot, starved and the
     * load stalled behind its loading picture. A thread switched out while
     * running keeps its place and the rest of its quantum; one whose quantum
     * ran out goes to the back with a new one; one that waits gets a new one
     * and its place when it is ready again. */
    const uint64_t now = nc_ke_now_ns();
    if (cur && cur->state == NC_T_RUNNING) {
        if (cur->slice_expired) {
            cur->ready_seq = ++k->ready_clock;
            cur->slice_used_ns = 0;
        } else {
            cur->ready_seq = cur->run_seq ? cur->run_seq : ++k->ready_clock;
            if (cur == k->slice_thread && cur->slice_start_ns) {
                cur->slice_used_ns += now - cur->slice_start_ns;
            }
        }
        cur->slice_expired = false;
        cur->state = NC_T_READY;
    } else if (cur) {
        g_ready_since[cur - k->threads] = 0;
        if (waitquantum_on() && cur->state == NC_T_WAITING && cur->priority < 14) {
            /* NT: the slice it has, less the wait's charge; a new one (and no boost) once it is used up */
            if (cur == k->slice_thread && cur->slice_start_ns) {
                cur->slice_used_ns += now - cur->slice_start_ns;
            }
            cur->slice_used_ns += 10ull * 1000000u / 3;
            if (cur->slice_used_ns >= quantum_ns(k)) {
                cur->slice_used_ns = 0;
                g_boost[cur - k->threads] = 0;
            }
        } else {
            cur->slice_used_ns = 0;     /* it waits or ends: a new quantum when it runs again */
        }
        cur->slice_expired = false;
        cur->ready_seq = 0;
        if (cur == k->slice_thread) {
            k->slice_thread = NULL;
        }
    }

    k->force_switch = false;

    if (chihiro_irql_on() && cur && cur->state == NC_T_WAITING && cur != k->dpc_thread && cur != k->isr_thread &&
        !k->in_dpc) {
        /* Its IRQL: the PCR's, or, when an ISR came in as it began the wait, the one the ISR keeps to put back
         * (patch 199: put back, the 2 stayed for everyone, the main thread spun at it and the media board's
         * sender thread, readied above it, never ran). */
        uint8_t *ir = !k->in_isr ? ps_irql(k) : (k->isr_interrupted == cur ? &k->isr_saved_irql : NULL);
        if (ir && *ir >= 2 && *ir < 26) {
            const unsigned i = (unsigned)(cur - k->threads);
            static unsigned logged;
            if (logged++ < 20) {
                nc_log("hakuX-native", "ps: thread %u waits at IRQL %u: the others run at theirs, its IRQL back when it "
                       "runs (patch 196)", i, *ir);
            }
            g_wait_irql[i] = *ir;
            g_wait_irql_held[i] = true;
            *ir = 0;
        }
    }

    /* Code an ISR interrupted above DISPATCH_LEVEL continues first. Then the
     * highest priority ready thread, round-robin after the current one. When
     * every thread waits, the guest CPU idles until the next timeout, timer
     * or interrupt. */
    const unsigned start = cur ? (unsigned)(cur - k->threads) + 1 : 0;
    NcThread *next = NULL;
    uint64_t idle_since = 0;
    for (;;) {
        bool fired;
        const uint64_t timer_due = nc_timer_poll(k, &fired);
        nc_io_poll(k); /* a disc read done sets the event a thread waits on */
        nc_intr_dispatch(k, true);
        nc_dpc_dispatch(k);
        uint64_t deadline = nc_ke_check_waits(k);
        if (timer_due && (!deadline || timer_due < deadline)) {
            deadline = timer_due;
        }
        if (k->resume_first && k->resume_first->state == NC_T_READY && !k->in_isr) {
            next = k->resume_first;
        } else {
            for (unsigned n = 0; n < NC_MAX_THREADS; n++) {
                NcThread *t = &k->threads[(start + n) % NC_MAX_THREADS];
                if (t->state != NC_T_READY || t->suspend || t == g_yielder) {
                    continue;
                }
                if (!t->ready_seq) {
                    t->ready_seq = ++k->ready_clock;    /* ready since the last pass */
                }
                if (!g_ready_since[t - k->threads]) {
                    g_ready_since[t - k->threads] = nc_ke_now_ns();
                }
                const int tp = nc_eff_priority(k, t), np = next ? nc_eff_priority(k, next) : 0;
                if (!next || tp > np || (tp == np && t->ready_seq < next->ready_seq)) {
                    next = t;
                }
            }
        }
        if (!next && g_yielder && g_yielder->state == NC_T_READY && !g_yielder->suspend) {
            next = g_yielder;      /* nothing else ready: it goes on */
        }
        if (next) {
            break;
        }
        /* Idle. With no timeout pending only an interrupt can wake a thread:
         * give up after a while without one. Paused meanwhile, here it
         * waits (the pause's wake brings it round). */
        nc_kernel_pause_point(k, f, true);
        const uint64_t now = nc_ke_now_ns();
        if (!idle_since) {
            idle_since = now;
        }
        if (!deadline && now - idle_since > 30ull * 1000000000u) {
            nc_kernel_request_stop(k, f, "no runnable thread: every thread waits with no timeout (idle 30 s)");
            return;
        }
        g_nc_sched.idles++;
        {
            extern volatile uint32_t g_xog_idle;
            g_xog_idle = 1;
            nc_irq_idle_wait(deadline ? nc_ke_host_ns(deadline) : 0, 1000000000u);    /* on the host's clock */
            g_xog_idle = 0;
        }
        nc_ke_update_time(k);
    }
    if (next == k->resume_first) {
        k->resume_first = NULL;
    }
    g_yielder = NULL;
    {
        const uint64_t t_now = nc_ke_now_ns();
        const bool real = next != k->isr_thread && next != k->dpc_thread;
        /* Wait quantum (patch 316): over a second, a thread at priority below 14 ready more than 60% of it yet
         * hardly run, while a thread of the same priority ran most of it and no slice ended: the turns of that
         * priority don't rotate (SSX 3: its XAPI timer thread, ready all the time behind the render thread, which
         * got a new slice and its boost back on every wait). */
        /* Yield (patch 316): over a second, a thread that yielded again and again and ran most of it, while a
         * thread below its priority was ready more than 60% of it and hardly ran: NtYieldExecution never gave way
         * (FIFA 07, Shadow, RalliSport 2: loading spun on SwitchToThread waiting for that thread, forever). Measured
         * over the whole second: the ISR / DPC threads running in between don't reset it. */
        if (!waitquantum_on() || !yield_on()) {
            if (g_watch_last) {
                const uint64_t dt = t_now - g_watch_last;
                for (unsigned i = 0; i < k->threads_hi; i++) {
                    const NcThread *t = &k->threads[i];
                    if (t->state == NC_T_READY && !t->suspend && t != cur) {
                        g_ready_ns[i] += dt;
                    }
                }
                if (cur && cur >= k->threads && cur < k->threads + NC_MAX_THREADS) {
                    g_run_ns[cur - k->threads] += dt;
                }
            } else {
                g_watch_from = t_now;
            }
            g_watch_last = t_now;
            if (t_now - g_watch_from >= 1000000000ull) {
                const uint64_t win = t_now - g_watch_from;
                for (unsigned i = 0; i < k->threads_hi; i++) {
                    const NcThread *t = &k->threads[i];
                    if (t->state == NC_T_FREE || t->priority >= 14 || g_ready_ns[i] * 10 < win * 6 ||
                        g_run_ns[i] * 10 > win) {
                        continue;   /* not starved */
                    }
                    for (unsigned j = 0; j < k->threads_hi; j++) {
                        const NcThread *o = &k->threads[j];
                        if (j == i || o->state == NC_T_FREE || g_run_ns[j] * 2 <= win) {
                            continue;
                        }
                        if (!waitquantum_on() && g_watch_slices < 2 && o->priority == t->priority) {
                            g_waitquantum = 1;
                            nc_log("hakuX-native", "ps: thread %u (priority %d) ready %llu ms of the last %llu, run %llu, "
                                   "behind thread %u at its priority (ran %llu ms), %u slices: the Xbox's wait quantum on "
                                   "for this game (patch 316, automatic)", t->id, t->priority,
                                   (unsigned long long)(g_ready_ns[i] / 1000000u), (unsigned long long)(win / 1000000u),
                                   (unsigned long long)(g_run_ns[i] / 1000000u), o->id,
                                   (unsigned long long)(g_run_ns[j] / 1000000u), g_watch_slices);
                        }
                        /* below it, or at its priority (RalliSport 2: the spinner's boost kept it a step above the
                         * worker it waited for, same base priority 8, so its yields never gave way) */
                        if (!yield_on() && o->priority >= t->priority && g_yields[j] >= 50) {
                            g_yield_mode = 1;
                            nc_log("hakuX-native", "ps: thread %u yielded %u times and ran %llu ms of the last %llu, while "
                                   "thread %u (priority %d, at or below it) was ready %llu ms and ran %llu: yield gives way to it "
                                   "now, for this game (patch 316, automatic)", o->id, g_yields[j],
                                   (unsigned long long)(g_run_ns[j] / 1000000u), (unsigned long long)(win / 1000000u),
                                   t->id, t->priority, (unsigned long long)(g_ready_ns[i] / 1000000u),
                                   (unsigned long long)(g_run_ns[i] / 1000000u));
                        }
                    }
                }
                memset(g_ready_ns, 0, sizeof(g_ready_ns));
                memset(g_run_ns, 0, sizeof(g_run_ns));
                memset(g_yields, 0, sizeof(g_yields));
                g_watch_slices = 0;
                g_watch_from = t_now;
            }
        }
        g_yield_asker = NULL;
        g_ready_since[next - k->threads] = 0;
        /* debug (NC_PSWATCH=1 in debug.txt): once a second, every thread's state and how long a ready one waits */
        static int watch = -1;
        static uint64_t watch_last;
        if (watch < 0) {
            watch = getenv("NC_PSWATCH") != NULL;
        }
        if (watch && t_now - watch_last > 1000000000ull) {
            watch_last = t_now;
            char line[900];
            int o = snprintf(line, sizeof(line), "next t%u p%d/e%d:", next->id, next->priority, nc_eff_priority(k, next));
            for (unsigned i = 0; i < k->threads_hi && o < (int)sizeof(line) - 40; i++) {
                const NcThread *t = &k->threads[i];
                if (t->state == NC_T_FREE) {
                    continue;
                }
                o += snprintf(line + o, sizeof(line) - (size_t)o, " t%u:p%d/e%d:%c%s", t->id, t->priority,
                              nc_eff_priority(k, t), t->state == NC_T_READY ? 'R' : t->state == NC_T_RUNNING ? 'X' :
                              t->state == NC_T_WAITING ? 'W' : '?', t->suspend ? "s" : "");
                if (t->state == NC_T_READY && g_ready_since[i]) {
                    o += snprintf(line + o, sizeof(line) - (size_t)o, "%llums",
                                  (unsigned long long)((t_now - g_ready_since[i]) / 1000000u));
                }
            }
            nc_log("hakuX-native-pswatch", "%s", line);
        }
    }
    if (next != cur) {
        g_nc_sched.switches++;
    }
    if (cur && cur != next && cur->state == NC_T_TERMINATED) {
        /* Off its stack for good: the pages go back (titles start threads per
         * movie and load), the slot can hold a new thread. Its ETHREAD stays,
         * signaled, for whoever still holds a handle. */
        const uint32_t end = cur->stack_base + (cur->tib_base ? 0 : NC_PAGE);  /* nc_ps_old_stacks's page */
        for (uint32_t p = cur->stack_limit; p < end; p += NC_PAGE) {
            const uint32_t pa = nc_vm_phys(k->vm, p);
            if (pa != UINT32_MAX) {
                nc_vm_unmap(k->vm, p, NC_PAGE); /* unmapped before it can be handed out again */
                nc_vm_free_phys(k->vm, pa & ~(NC_PAGE - 1), NC_PAGE);
            }
        }
        nc_ps_stack_free(k, cur->stack_limit, cur->stack_base - cur->stack_limit);
        cur->state = NC_T_FREE;
    }
    if (chihiro_irql_on()) {
        const unsigned i = (unsigned)(next - k->threads);
        if (i < NC_MAX_THREADS && g_wait_irql_held[i] && next != k->dpc_thread && next != k->isr_thread) {
            g_wait_irql_held[i] = false;
            *ps_irql(k) = g_wait_irql[i];       /* patch 196: its own IRQL again */
        }
    }
    nc_frame_load(f, &next->ctx);
    next->fresh = false;
    pcr_enter(k, next);
    k->current = next;
    next->state = NC_T_RUNNING;
    next->run_seq = next->ready_seq;    /* its place, kept if it is switched out */
    next->ready_seq = 0;
    if (next != k->isr_thread && next != k->dpc_thread) {
        /* the rest of its quantum (an ISR or a DPC neither ends nor restarts it) */
        const uint64_t q = quantum_ns(k), t0 = nc_ke_now_ns();
        k->slice_thread = next;
        next->slice_start_ns = t0;
        k->slice_due_ns = t0 + (next->slice_used_ns < q ? q - next->slice_used_ns : 1);
        nc_kernel_clock_arm(k); /* back from an ISR, its slice may be over */
    }
}

void nc_ps_terminate_current(NcKernel *k, uint32_t status)
{
    NcThread *t = k->current;
    if (!t) {
        return;
    }
    t->state = NC_T_TERMINATED;
    nc_vm_wr32(k->vm, t->ethread + ET_EXIT_STATUS, status);
    nc_vm_wr32(k->vm, t->ethread + KT_SIGNAL_STATE, 1); /* the thread object is signaled */
    k->resched = true;
}

/* PsCreateSystemThreadEx(ThreadHandle, ThreadExtensionSize, KernelStackSize, TlsDataSize,
 *   ThreadId, StartRoutine, StartContext, CreateSuspended, DebuggerThread, SystemRoutine).
 * TLS data sits at the top of the stack. With a SystemRoutine the thread starts in
 * SystemRoutine(StartRoutine, StartContext), else in StartRoutine(StartContext). */
static uint64_t PsCreateSystemThreadEx(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = thread_create(k, a[2], a[3], a[1]);
    if (!t) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    nc_vm_wr32(k->vm, t->ethread + ET_START_ADDRESS, a[5]);
    uint32_t esp = t->frame_top - 16;
    uint32_t eip;
    if (a[9]) {
        esp -= 12;
        nc_vm_wr32(k->vm, esp, k->thread_return_va);
        nc_vm_wr32(k->vm, esp + 4, a[5]);
        nc_vm_wr32(k->vm, esp + 8, a[6]);
        eip = a[9];
    } else {
        esp -= 8;
        nc_vm_wr32(k->vm, esp, k->thread_return_va);
        nc_vm_wr32(k->vm, esp + 4, a[6]);
        eip = a[5];
    }
    nc_context_init(k->cpu, &t->ctx, eip, esp);

    uint32_t h = nc_ob_handle(k, t->ethread, NC_OB_THREAD);
    if (a[0]) {
        nc_vm_wr32(k->vm, a[0], h);
    }
    if (a[4]) {
        nc_vm_wr32(k->vm, a[4], t->id);
    }
    t->state = NC_T_READY;
    t->suspend = (a[7] & 0xff) ? 1 : 0;
    nc_log("hakuX-native", "ps: thread %u created: start %08x (%s), stack %08x-%08x, tls %u%s", t->id,
           a[5], a[9] ? "via system routine" : "direct", t->stack_limit, t->stack_base, a[3],
           t->suspend ? ", suspended" : "");
    return NC_STATUS_SUCCESS;
}

/* The thread a handle names (NtCurrentThread is -2). */
static NcThread *thread_of_handle(NcKernel *k, uint32_t handle)
{
    if (handle == 0xFFFFFFFEu) {
        return k->current;
    }
    enum NcObType type;
    uint32_t obj = nc_ob_lookup(k, handle, &type);
    return obj && type == NC_OB_THREAD ? nc_ps_thread_of(k, obj) : NULL;
}

static uint32_t suspend(NcKernel *k, NcThread *t)
{
    uint32_t prev = t->suspend++;
    if (t == k->current) {
        k->resched = true;
    }
    return prev;
}

static uint32_t resume(NcKernel *k, NcThread *t)
{
    uint32_t prev = t->suspend;
    if (t->suspend && !--t->suspend) {
        k->resched = true;
    }
    return prev;
}

void nc_ps_debug_resume(NcKernel *k, unsigned id)
{
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        NcThread *t = &k->threads[i];
        if (t->state != NC_T_FREE && t->id == id) {
            nc_log("hakuX-native", "debug: t%u resumed (suspend count was %u)", id, resume(k, t));
            return;
        }
    }
}

/* NtSuspendThread(ThreadHandle, PreviousSuspendCount) / NtResumeThread(...) */
static uint64_t NtSuspendThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = thread_of_handle(k, a[0]);
    if (!t) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t prev = suspend(k, t);
    if (a[1]) {
        nc_vm_wr32(k->vm, a[1], prev);
    }
    return NC_STATUS_SUCCESS;
}

static uint64_t NtResumeThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = thread_of_handle(k, a[0]);
    if (!t) {
        /* a thread that has exited: nothing to resume (a valid handle still) */
        enum NcObType type;
        if (!nc_ob_lookup(k, a[0], &type) || type != NC_OB_THREAD) {
            return NC_STATUS_INVALID_HANDLE;
        }
        if (a[1]) {
            nc_vm_wr32(k->vm, a[1], 0);
        }
        return NC_STATUS_SUCCESS;
    }
    uint32_t prev = resume(k, t);
    if (a[1]) {
        nc_vm_wr32(k->vm, a[1], prev);
    }
    return NC_STATUS_SUCCESS;
}

/* KeSuspendThread(Thread) / KeResumeThread(Thread) -> previous suspend count */
static uint64_t KeSuspendThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = nc_ps_thread_of(k, a[0]);
    return t ? suspend(k, t) : 0;
}

static uint64_t KeResumeThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = nc_ps_thread_of(k, a[0]);
    return t ? resume(k, t) : 0;
}

/* PsTerminateSystemThread(ExitStatus): never returns. */
static uint64_t PsTerminateSystemThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native", "ps: thread %u exits (%08x)", k->current ? k->current->id : 0, a[0]);
    nc_ps_terminate_current(k, a[0]);
    return 0;
}

void nc_ps_register(NcKernel *k)
{
    nc_kernel_impl(k, "PsCreateSystemThreadEx", PsCreateSystemThreadEx);
    nc_kernel_impl(k, "PsTerminateSystemThread", PsTerminateSystemThread);
    nc_kernel_impl(k, "NtSuspendThread", NtSuspendThread);
    nc_kernel_impl(k, "NtResumeThread", NtResumeThread);
    nc_kernel_impl(k, "KeSuspendThread", KeSuspendThread);
    nc_kernel_impl(k, "KeResumeThread", KeResumeThread);
}

/* Every thread for a fault or timeout report: what it is doing or waiting
 * for (dispatcher type, signal state), and the top of its stack. */
void nc_kernel_dump_threads(NcKernel *k, int fd)
{
    static const char *const k_state[] = {"free", "ready", "running", "waiting", "suspended", "terminated"};
    char line[640];
    const uint64_t now = nc_ke_now_ns();
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        const NcThread *t = &k->threads[i];
        if (t->state == NC_T_FREE) {
            continue;
        }
        const bool cur = t == k->current;
        const uint32_t eip = cur && k->frame ? nc_frame_eip(k->frame) : nc_context_eip(&t->ctx);
        const uint32_t esp = cur && k->frame ? nc_frame_gpr(k->frame, NC_ESP) : nc_context_gpr(&t->ctx, NC_ESP);
        int n = snprintf(line, sizeof(line), "hakuX-native-frontier:   thread t%u %s prio %d%s eip %08x esp %08x",
                         t->id, k_state[t->state], t->priority, t->suspend ? " suspended" : "", eip, esp);
        if (t->state == NC_T_WAITING) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " | wait%s%s", t->wait.wait_all ? " all" : "",
                          t->wait.alertable ? " alertable" : "");
            if (t->wait.deadline_ns) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " in %lld ms",
                              (long long)((int64_t)(t->wait.deadline_ns - now) / 1000000));
            }
            for (unsigned w = 0; w < t->wait.count && w < 6; w++) {
                const uint32_t o = t->wait.objects[w];
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x(type %u sig %d)", o,
                              nc_vm_phys(k->vm, o) != UINT32_MAX ? *(const uint8_t *)nc_vm_host(k->vm, o) : 0xFF,
                              nc_vm_phys(k->vm, o + 4) != UINT32_MAX ? (int)nc_vm_rd32(k->vm, o + 4) : -1);
            }
            if (t->wait.cs) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " cs %08x", t->wait.cs);
            }
        }
        if (nc_vm_phys(k->vm, esp) != UINT32_MAX) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " | stack");
            for (unsigned w = 0; w < 8 && nc_vm_phys(k->vm, esp + 4 * w) != UINT32_MAX; w++) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", nc_vm_rd32(k->vm, esp + 4 * w));
            }
            /* Return address candidates deeper down: dwords inside the title image. */
            n += snprintf(line + n, sizeof(line) - (size_t)n, " | calls");
            unsigned found = 0;
            for (unsigned w = 8; w < 512 && found < 12 && nc_vm_phys(k->vm, esp + 4 * w) != UINT32_MAX; w++) {
                const uint32_t v = nc_vm_rd32(k->vm, esp + 4 * w);
                if (v >= k->xbe_base && v < k->xbe_base + k->xbe_size) {
                    n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", v);
                    found++;
                }
            }
        }
        line[n++] = '\n';
        nc_dump_line(fd, line, (size_t)n);
    }
}
