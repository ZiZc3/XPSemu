/*
 * hakuX native core — asynchronous procedure calls.
 *
 * KAPC (nxdk xboxkrnl.h, CC0): Type 0x00, ApcMode 0x02, Inserted 0x03,
 * Thread 0x04, ApcListEntry 0x08, KernelRoutine 0x10, RundownRoutine 0x14,
 * NormalRoutine 0x18, NormalContext 0x1C, SystemArgument1 0x20,
 * SystemArgument2 0x24.
 *
 * A thread's APCs wait on the host until it waits alertably. Then each runs
 * as guest code on that thread: KernelRoutine(Apc, &NormalRoutine,
 * &NormalContext, &SystemArgument1, &SystemArgument2) for a KAPC, then
 * NormalRoutine(NormalContext, SystemArgument1, SystemArgument2), each
 * returning to a stub that starts the next; after the last, the wait
 * returns STATUS_USER_APC. An APC queued to a thread already in an alertable
 * wait ends the wait the same way. APCs queued by NtQueueApcThread and by
 * I/O completion (NtReadFile's ApcRoutine) have no KAPC: only the normal
 * routine runs.
 *
 * An APC leaves the queue as its delivery begins, as NT takes a user APC off
 * the list before it calls the routine. An APC routine may wait alertably
 * itself: that wait runs the APCs queued since as a chain of its own and
 * returns to the routine, whose chain then goes on. Project Gotham Racing's
 * read completion writes the file it read to its cache, opens the next one
 * and waits for its buffer, all in the APC: kept in the queue, the same
 * completion ran again in that wait ("read done" with no read), and the
 * title wrote its cache file from a null buffer and faulted.
 */
#include "native/kernel/kernel.h"

#include <stdlib.h>

#define KA_INSERTED 0x03
#define KA_THREAD   0x04
#define KA_KERNEL   0x10
#define KA_RUNDOWN  0x14
#define KA_NORMAL   0x18
#define KA_CONTEXT  0x1C
#define KA_ARG1     0x20
#define KA_ARG2     0x24
#define KAPC_SIZE   0x28

#define STATUS_USER_APC 0x000000C0u

bool nc_apc_pending(const NcThread *t)
{
    return t->napcs != 0 && (!t->apc.in_apc || t->apc_nest < NC_MAX_APC_NEST);
}

/* The next call of t's APC chain as {routine, args, nargs}; false when done. */
static bool next_call(NcKernel *k, NcThread *t, uint32_t *routine, uint32_t args[5], unsigned *nargs)
{
    NcApcChain *c = &t->apc;
    for (;;) {
        if (!c->has_cur) {
            if (!t->napcs) {
                return false;
            }
            c->cur = t->apcs[0];            /* out of the queue: delivery begins */
            memmove(&t->apcs[0], &t->apcs[1], (t->napcs - 1) * sizeof(NcApc));
            t->napcs--;
            c->has_cur = true;
            c->stage = 0;
            if (c->cur.kapc) {
                ((uint8_t *)nc_vm_host(k->vm, c->cur.kapc))[KA_INSERTED] = 0;
            }
        }
        NcApc *a = &c->cur;
        if (c->stage == 0 && a->kapc && nc_vm_rd32(k->vm, a->kapc + KA_KERNEL)) {
            c->stage = 1;
            *routine = nc_vm_rd32(k->vm, a->kapc + KA_KERNEL);
            args[0] = a->kapc;
            args[1] = a->kapc + KA_NORMAL;
            args[2] = a->kapc + KA_CONTEXT;
            args[3] = a->kapc + KA_ARG1;
            args[4] = a->kapc + KA_ARG2;
            *nargs = 5;
            return true;
        }
        if (c->stage < 2) {
            if (a->kapc) { /* the kernel routine may have changed or cleared them */
                a->normal = nc_vm_rd32(k->vm, a->kapc + KA_NORMAL);
                a->ctx = nc_vm_rd32(k->vm, a->kapc + KA_CONTEXT);
                a->a1 = nc_vm_rd32(k->vm, a->kapc + KA_ARG1);
                a->a2 = nc_vm_rd32(k->vm, a->kapc + KA_ARG2);
            }
            c->stage = 2;
            if (a->normal) {
                *routine = a->normal;
                args[0] = a->ctx;
                args[1] = a->a1;
                args[2] = a->a2;
                *nargs = 3;
                return true;
            }
        }
        c->has_cur = false;                 /* this APC is done */
    }
}

/* Builds the call below the final stack pointer: [return stub, args...]. */
static uint32_t call_frame(NcKernel *k, uint32_t below, const uint32_t *args, unsigned nargs)
{
    const uint32_t sp = below - 16 - 4 * nargs;
    nc_vm_wr32(k->vm, sp, k->apc_return_va);
    for (unsigned i = 0; i < nargs; i++) {
        nc_vm_wr32(k->vm, sp + 4 + 4 * i, args[i]);
    }
    return sp;
}

/* A chain starts for a wait returning to esp / eip; the one running (a wait
 * in an APC routine) is kept, to go on once this one ends. False when no
 * more can nest. */
static bool chain_start(NcThread *t, uint32_t esp, uint32_t eip)
{
    if (t->apc.in_apc) {
        if (t->apc_nest == NC_MAX_APC_NEST) {
            return false;
        }
        t->apc_outer[t->apc_nest++] = t->apc;
    }
    t->apc = (NcApcChain){.esp = esp, .eip = eip};
    return true;
}

/* The chain ended: an interrupted one goes on. */
static void chain_end(NcThread *t)
{
    t->apc = t->apc_nest ? t->apc_outer[--t->apc_nest] : (NcApcChain){0};
}

bool nc_apc_begin_frame(NcKernel *k, NcFrame *f, NcThread *t, uint32_t final_esp, uint32_t final_eip)
{
    uint32_t routine, args[5];
    unsigned nargs;
    if (!chain_start(t, final_esp, final_eip)) {
        return false;
    }
    if (!next_call(k, t, &routine, args, &nargs)) {
        chain_end(t);
        return false;
    }
    t->apc.in_apc = true;
    nc_frame_set_gpr(f, NC_ESP, call_frame(k, final_esp, args, nargs));
    nc_frame_set_eip(f, routine);
    return true;
}

/* A thread waiting alertably: its saved context continues in the APCs. */
static void begin_ctx(NcKernel *k, NcThread *t)
{
    uint32_t routine, args[5];
    unsigned nargs;
    const uint32_t esp = nc_context_gpr(&t->ctx, NC_ESP), eip = nc_context_eip(&t->ctx);
    if (!chain_start(t, esp, eip)) {
        return;
    }
    if (!next_call(k, t, &routine, args, &nargs)) {
        chain_end(t);
        nc_context_set_gpr(&t->ctx, NC_EAX, STATUS_USER_APC);
        return;
    }
    t->apc.in_apc = true;
    nc_context_set_gpr(&t->ctx, NC_ESP, call_frame(k, esp, args, nargs));
    nc_context_set_eip(&t->ctx, routine);
}

void nc_apc_return(NcKernel *k, NcFrame *f)
{
    NcThread *t = k->current;
    if (!t || !t->apc.in_apc) {
        nc_kernel_request_stop(k, f, "APC return with no APC running");
        return;
    }
    uint32_t routine, args[5];
    unsigned nargs;
    if (next_call(k, t, &routine, args, &nargs)) {
        nc_frame_set_gpr(f, NC_ESP, call_frame(k, t->apc.esp, args, nargs));
        nc_frame_set_eip(f, routine);
        return;
    }
    const uint32_t esp = t->apc.esp, eip = t->apc.eip;
    chain_end(t);
    nc_frame_set_gpr(f, NC_ESP, esp);
    nc_frame_set_eip(f, eip);
    nc_frame_set_gpr(f, NC_EAX, STATUS_USER_APC);
}

bool nc_apc_queue(NcKernel *k, NcThread *t, const NcApc *a)
{
    if (t->napcs == t->apc_cap) {
        const unsigned cap = t->apc_cap ? t->apc_cap * 2 : 16;
        NcApc *grown = realloc(t->apcs, cap * sizeof(NcApc));
        if (!grown) {
            nc_log("hakuX-native", "ke: no memory for thread %u's APC queue (%u queued)", t->id, t->napcs);
            return false;
        }
        t->apcs = grown;
        t->apc_cap = cap;
    }
    t->apcs[t->napcs++] = *a;
    if (t->state == NC_T_WAITING && t->wait.alertable && nc_apc_pending(t)) {
        /* The wait ends with STATUS_USER_APC once the APCs ran (one in an
         * APC routine too: its chain goes on after). */
        t->state = NC_T_READY;
        begin_ctx(k, t);
        k->resched = true;
    }
    return true;
}

/* KeInitializeApc(Apc, Thread, KernelRoutine, RundownRoutine, NormalRoutine, ProcessorMode, NormalContext) */
static uint64_t KeInitializeApc(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint8_t *p = nc_vm_host(k->vm, a[0]);
    memset(p, 0, KAPC_SIZE);
    *(int16_t *)p = 18; /* ApcObject */
    p[2] = a[4] ? (uint8_t)a[5] : 0;
    nc_vm_wr32(k->vm, a[0] + KA_THREAD, a[1]);
    nc_vm_wr32(k->vm, a[0] + KA_KERNEL, a[2]);
    nc_vm_wr32(k->vm, a[0] + KA_RUNDOWN, a[3]);
    nc_vm_wr32(k->vm, a[0] + KA_NORMAL, a[4]);
    nc_vm_wr32(k->vm, a[0] + KA_CONTEXT, a[4] ? a[6] : 0);
    return 0;
}

/* KeInsertQueueApc(Apc, SystemArgument1, SystemArgument2, Increment) -> queued */
static uint64_t KeInsertQueueApc(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint8_t *p = nc_vm_host(k->vm, a[0]);
    NcThread *t = nc_ps_thread_of(k, nc_vm_rd32(k->vm, a[0] + KA_THREAD));
    if (p[KA_INSERTED] || !t) {
        return 0;
    }
    nc_vm_wr32(k->vm, a[0] + KA_ARG1, a[1]);
    nc_vm_wr32(k->vm, a[0] + KA_ARG2, a[2]);
    p[KA_INSERTED] = 1;
    const NcApc apc = {.kapc = a[0]};
    if (!nc_apc_queue(k, t, &apc)) {
        p[KA_INSERTED] = 0;
        return 0;
    }
    return 1; /* Inserted until its delivery begins */
}

/* NtQueueApcThread(ThreadHandle, ApcRoutine, ApcArgument1, ApcArgument2, ApcArgument3) */
static uint64_t NtQueueApcThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = NULL;
    if (a[0] == 0xFFFFFFFEu) {
        t = k->current;
    } else {
        enum NcObType type;
        uint32_t obj = nc_ob_lookup(k, a[0], &type);
        t = obj && type == NC_OB_THREAD ? nc_ps_thread_of(k, obj) : NULL;
    }
    if (!t) {
        return NC_STATUS_INVALID_HANDLE;
    }
    const NcApc apc = {.normal = a[1], .ctx = a[2], .a1 = a[3], .a2 = a[4]};
    return nc_apc_queue(k, t, &apc) ? NC_STATUS_SUCCESS : NC_STATUS_NO_MEMORY;
}

/* NtUserIoApcDispatcher(ApcContext, IoStatusBlock, Reserved): the APC routine
 * XAPI gives file I/O for a completion routine (ApcContext). It calls
 * CompletionRoutine(dwErrorCode, dwNumberOfBytesTransfered, lpOverlapped),
 * the OVERLAPPED starting with the IO_STATUS_BLOCK. */
static uint64_t NtUserIoApcDispatcher(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t status = nc_vm_rd32(k->vm, a[1]);
    const uint32_t args[3] = {(status & 0x80000000u) ? nc_rtl_status_to_dos(status) : 0, nc_vm_rd32(k->vm, a[1] + 4),
                              a[1]};
    nc_kernel_callout(k, f, a[0], args, 3);
    return 0;
}

/* KeTestAlertThread(ProcessorMode) -> alerted: alerts aren't kept. */
static uint64_t KeTestAlertThread(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return 0;
}

void nc_apc_register(NcKernel *k)
{
    nc_kernel_impl(k, "KeInitializeApc", KeInitializeApc);
    nc_kernel_impl(k, "KeInsertQueueApc", KeInsertQueueApc);
    nc_kernel_impl(k, "NtQueueApcThread", NtQueueApcThread);
    nc_kernel_impl(k, "NtUserIoApcDispatcher", NtUserIoApcDispatcher);
    nc_kernel_impl(k, "KeTestAlertThread", KeTestAlertThread);
}
