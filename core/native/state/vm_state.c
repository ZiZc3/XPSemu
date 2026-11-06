/*
 * hakuX native core — guest memory in a savestate: RAM (RAM) and the page
 * tables over it (VM).
 */
#include "native/core/vm.h"
#include "native/kernel/kernel.h"

#include <pthread.h>
#include <stdlib.h>
#include <zlib.h>

typedef struct NcRamState {
    uint32_t ram_size, block;
} NcRamState;

typedef struct NcVmState {
    uint32_t ram_size, nruns;
} NcVmState;

typedef struct NcVmRun {
    uint32_t va, pa, pages;
} NcVmRun;

#define NC_DEF_SECTION RAM
#define NC_DEF_FILE "native/state/ram.def"
#include "native/state/def.h"
#undef NC_DEF_SECTION
#undef NC_DEF_FILE
#define NC_DEF_SECTION VM
#define NC_DEF_FILE "native/state/vm.def"
#include "native/state/def.h"
#include "native/state/sections.h"

#define RAM_BLOCK 0x10000u

enum { BLK_ZERO = 0, BLK_DEFLATE = 1, BLK_STORED = 2 };

static bool all_zero(const uint8_t *p, size_t n)
{
    const uint64_t *q = (const uint64_t *)p;
    for (size_t i = 0; i < n / 8; i++) {
        if (q[i]) {
            return false;
        }
    }
    return true;
}

/* A run of RAM's blocks, encoded into its own buffer as the section holds
 * them: the runs one after another are the section. */
typedef struct RamPart {
    const uint8_t *ram;
    uint32_t from, to;                  /* byte offsets */
    NcBuf out;
    uint32_t zero, deflated;
    uint64_t bytes;
    pthread_t th;
    bool started;
} RamPart;

static void *ram_part(void *arg)
{
    RamPart *rp = arg;
    uint8_t *z = malloc(compressBound(RAM_BLOCK));
    if (!z) {
        rp->out.fail = true;
        return NULL;
    }
    for (uint32_t off = rp->from; off < rp->to; off += RAM_BLOCK) {
        const uint8_t *p = rp->ram + off;
        if (all_zero(p, RAM_BLOCK)) {
            nc_buf_u8(&rp->out, BLK_ZERO);
            rp->zero++;
            continue;
        }
        uLongf zl = compressBound(RAM_BLOCK);
        if (compress2(z, &zl, p, RAM_BLOCK, 1) == Z_OK && zl < RAM_BLOCK) {
            nc_buf_u8(&rp->out, BLK_DEFLATE);
            nc_buf_u32(&rp->out, (uint32_t)zl);
            nc_buf_put(&rp->out, z, zl);
            rp->bytes += zl;
            rp->deflated++;
        } else {
            nc_buf_u8(&rp->out, BLK_STORED);
            nc_buf_put(&rp->out, p, RAM_BLOCK);
            rp->bytes += RAM_BLOCK;
        }
    }
    free(z);
    return NULL;
}

/* The blocks are deflated a quarter on each of 4 threads while the guest
 * waits: on one, they were ~1.05 s of Halo 2's 1.2 s save (the Odin). */
#define RAM_PARTS 4

bool nc_ram_state_save(NcBuf *b)
{
    const NcVm *vm = nc_state_kernel()->vm;
    const NcRamState st = {vm->ram_size, RAM_BLOCK};
    nc_rec_put(b, &nc_rec_NcRamState, &st);
    const uint32_t nblk = vm->ram_size / RAM_BLOCK;
    RamPart parts[RAM_PARTS];
    memset(parts, 0, sizeof(parts));
    for (unsigned i = 0; i < RAM_PARTS; i++) {
        parts[i].ram = vm->phys;
        parts[i].from = nblk * i / RAM_PARTS * RAM_BLOCK;
        parts[i].to = nblk * (i + 1) / RAM_PARTS * RAM_BLOCK;
        if (i) {
            parts[i].started = pthread_create(&parts[i].th, NULL, ram_part, &parts[i]) == 0;
        }
    }
    ram_part(&parts[0]);
    uint32_t zero = 0, deflated = 0;
    uint64_t bytes = 0;
    for (unsigned i = 0; i < RAM_PARTS; i++) {
        if (i && parts[i].started) {
            pthread_join(parts[i].th, NULL);
        } else if (i) {
            ram_part(&parts[i]);                /* no thread: here */
        }
        nc_buf_put(b, parts[i].out.p, parts[i].out.len);
        b->fail |= parts[i].out.fail;
        nc_buf_free(&parts[i].out);
        zero += parts[i].zero;
        deflated += parts[i].deflated;
        bytes += parts[i].bytes;
    }
    nc_log("hakuX-native-state", "RAM: %u MiB, %u blocks zero, %u deflated, %.1f MiB stored", vm->ram_size >> 20, zero,
           deflated, bytes / 1048576.0);
    return !b->fail;
}

bool nc_ram_state_load(NcRd *r, unsigned v)
{
    NcVm *vm = nc_state_kernel()->vm;
    NcRamState st = {0};
    nc_rec_get(r, &nc_rec_NcRamState, &st, v);
    if (st.ram_size != vm->ram_size || st.block != RAM_BLOCK) {
        nc_log("hakuX-native-state", "RAM: the state's is %u MiB in %u-byte blocks, this one %u MiB", st.ram_size >> 20,
               st.block, vm->ram_size >> 20);
        return false;
    }
    for (uint32_t off = 0; off < vm->ram_size; off += RAM_BLOCK) {
        uint8_t *p = vm->phys + off;
        const uint8_t kind = nc_rd_u8(r);
        if (kind == BLK_ZERO) {
            memset(p, 0, RAM_BLOCK);
        } else if (kind == BLK_DEFLATE) {
            const uint32_t zl = nc_rd_u32(r);
            const void *z = nc_rd_ptr(r, zl);
            uLongf out = RAM_BLOCK;
            if (!z || uncompress(p, &out, z, zl) != Z_OK || out != RAM_BLOCK) {
                return false;
            }
        } else if (kind == BLK_STORED) {
            if (!nc_rd_get(r, p, RAM_BLOCK)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return !r->fail;
}

/* ------------------------------------------------------------------- VM */

static bool in_window(const NcVm *vm, uint32_t va)
{
    return (va >= NC_CONTIG_WINDOW && va - NC_CONTIG_WINDOW < vm->ram_size) ||
           (va >= NC_GPU_WINDOW && va - NC_GPU_WINDOW < vm->ram_size);
}

/* The mappings outside the windows, as runs of pages mapped in order:
 * fn(run) each; returns how many. */
static uint32_t runs(const NcVm *vm, void (*fn)(void *ctx, const NcVmRun *run), void *ctx)
{
    uint32_t n = 0;
    NcVmRun run = {0, 0, 0};
    for (uint64_t page = 0; page <= (1ull << 32) / NC_PAGE; page++) {
        const uint32_t va = (uint32_t)(page * NC_PAGE);
        const uint32_t e = page < (1ull << 32) / NC_PAGE && !in_window(vm, va) ? vm->pte[page] : 0;
        const uint32_t pa = e & ~(NC_PAGE - 1);
        if (run.pages && (e & NC_PTE_VALID) && va == run.va + run.pages * NC_PAGE &&
            pa == run.pa + run.pages * NC_PAGE) {
            run.pages++;
            continue;
        }
        if (run.pages) {
            if (fn) {
                fn(ctx, &run);
            }
            n++;
        }
        run = (NcVmRun){va, pa, (e & NC_PTE_VALID) ? 1u : 0u};
    }
    return n;
}

static void put_run(void *ctx, const NcVmRun *run)
{
    nc_rec_put(ctx, &nc_rec_NcVmRun, run);
}

bool nc_vm_state_save(NcBuf *b)
{
    const NcVm *vm = nc_state_kernel()->vm;
    const NcVmState st = {vm->ram_size, runs(vm, NULL, NULL)};
    nc_rec_put(b, &nc_rec_NcVmState, &st);
    runs(vm, put_run, b);
    const uint32_t pages = vm->ram_size / NC_PAGE;
    nc_buf_put(b, vm->page_used, pages);
    nc_buf_put(b, vm->rmap, (size_t)pages * sizeof(uint32_t));
    nc_buf_put(b, vm->pin, pages);
    nc_log("hakuX-native-state", "VM: %u runs mapped", st.nruns);
    return !b->fail;
}

static void unmap_run(void *ctx, const NcVmRun *run)
{
    nc_vm_unmap(ctx, run->va, run->pages * NC_PAGE);
}

bool nc_vm_state_load(NcRd *r, unsigned v)
{
    NcVm *vm = nc_state_kernel()->vm;
    NcVmState st = {0};
    nc_rec_get(r, &nc_rec_NcVmState, &st, v);
    if (st.ram_size != vm->ram_size) {
        return false;
    }
    /* what this boot mapped goes; the state's mappings come back */
    runs(vm, unmap_run, vm);
    for (uint32_t i = 0; i < st.nruns; i++) {
        NcVmRun run = {0};
        nc_rec_get(r, &nc_rec_NcVmRun, &run, v);
        if (r->fail || !run.pages || in_window(vm, run.va) ||
            !nc_vm_map(vm, run.va, run.pa, run.pages * NC_PAGE)) {
            return false;
        }
    }
    const uint32_t pages = vm->ram_size / NC_PAGE;
    nc_rd_get(r, vm->page_used, pages);
    nc_rd_get(r, vm->rmap, (size_t)pages * sizeof(uint32_t));
    nc_rd_get(r, vm->pin, pages);
    nc_log("hakuX-native-state", "VM: %u runs mapped", st.nruns);
    return !r->fail;
}
