/*
 * hakuX native core — guest-write tracking for the HLE layer.
 *
 * The HLE layer asks whether the guest CPU wrote guest RAM since it last
 * looked (host.h, hle_host_mem_*), and needs to hear about the first
 * write to a page it cleaned (hle_mem_first_write: the first-write barrier may
 * wait for the GPU before the write happens). A clean page is write-protected
 * in every guest mapping, so the guest's first store faults: the handler
 * calls hle_mem_first_write, marks the page dirty, unprotects it and the store
 * runs again. Invariant: protected <=> clean.
 *
 * Host code writes through the RAM view (vm->phys), which is never protected;
 * host stores by guest VA report themselves (hle_va_store).
 */
#include "native/hle/hle_native.h"

#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "native/core/mmio.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

static NcVm *g_vm;
/* Per page: written before hle_host_mem_rearm protected it again, which a
 * test_and_clear still reports (a page's protection is its dirty bit
 * otherwise: protected <=> clean) */
static uint8_t *g_hdirty;


/* A page without access, given it back the way a guest access's fault does
 * (hle_mem_stale_access_rw: the image newer than its memory goes in first;
 * write: the host is about to write it). A page several ranges share takes
 * one call each. */
static void stale_open(uint32_t pa, bool write)
{
    for (unsigned k = 0; k < 16 && nc_vm_noaccess_has(g_vm, pa); k++) {
        hle_mem_stale_access_rw(pa, write);
    }
}

#if defined(__PROSPERO__)
static void first_write(uint32_t pa, uint32_t len);
static void dirty_sample(uint32_t pa);
extern void rx_vm_block_apply(NcVm *, uint32_t);

/* The console protects 16 KiB at a time, so an access to any 4 KiB of a block needs the
 * whole block settled: what its pages are owed goes in first; for a store, each tracked
 * page is reported written (over-reporting a neighbour's write at worst, never missing
 * one); then the flags reach the host. For a guest store's fault, and before a system
 * call touches a guest buffer, where a closed page would fail the call with EFAULT. */
extern bool rx_ram_emulate(NcVm *vm, void *ucontext, uint32_t addr, void (*stored)(uint32_t pa, uint32_t n));
extern NcCpu *g_cpu_for_ram(void);
#define g_cpu_of_faults g_cpu_for_ram
static void guest_stored(uint32_t pa, uint32_t len);
extern uint8_t *g_va_soft;
extern uint32_t g_soft_emulated, g_soft_failed;
extern bool rx_ram_hole_set, rx_ram_hole_store;
extern uint32_t rx_ram_hole;
extern bool rx_guest_page_fault(void *ucontext, uint32_t va, bool store);
extern bool rx_step_begin(NcVm *vm, uint32_t va);
extern void rx_soft_site_note(void);
extern bool rx_soft_fill(void *ucontext, uint32_t va);
extern volatile uint32_t g_steps_n;
extern void nc_cpu_fault_step(void *ucontext);
#define rx_steps_running() (g_steps_n != 0)
extern bool rx_shadow_make(NcVm *vm, uint32_t va, bool code);
extern bool nc_cpu_fault_is_fetch(const void *ucontext);

/* What faults turned out to be (xog-faults, once a second). */
enum { RX_F_WATCHED, RX_F_OPENED, RX_F_TARGET_READ, RX_F_TARGET_WRITE, RX_F_BESIDE, RX_F_BLOCK, RX_F_HOT, RX_F_NO_EMULATE, RX_F_N };
static uint32_t g_fault_kinds[RX_F_N];

void nc_fault_kinds(uint32_t *out, unsigned n)
{
    for (unsigned i = 0; i < n && i < RX_F_N; i++) {
        out[i] = g_fault_kinds[i];
        g_fault_kinds[i] = 0;
    }
}

static void rx_open_block(uint32_t pa, bool write)
{
    const uint32_t first = pa & ~(16384u - 1);
    for (uint32_t o = 0; o < 16384u; o += NC_PAGE) {
        stale_open(first + o, write);
    }
    if (write) {
        /* first: opening one page opens the whole block */
        uint8_t was[16384u / NC_PAGE];
        for (uint32_t o = 0; o < 16384u; o += NC_PAGE) {
            was[o / NC_PAGE] = __atomic_load_n(&g_vm->wp[(first + o) / NC_PAGE], __ATOMIC_ACQUIRE);
        }
        for (uint32_t o = 0; o < 16384u; o += NC_PAGE) {
            const uint8_t w = was[o / NC_PAGE];
            if (w == NC_WP_CLEAN) {
                dirty_sample(first + o);
                first_write(first + o, 4);
            } else if (w == NC_WP_WRITTEN) {
                nc_vm_protect_page(g_vm, first + o, false);
            }
        }
    }
    rx_vm_block_apply(g_vm, pa);
}

/* Each block of [va, va + len). */
static void rx_open_range(uint32_t va, uint32_t len, bool write)
{
    if (!g_vm || !len) {
        return;
    }
    const uint64_t end = (uint64_t)va + len;
    for (uint64_t b = va & ~(uint64_t)(16384u - 1); b < end; b += 16384u) {
        for (uint32_t o = 0; o < 16384u; o += NC_PAGE) {
            const uint32_t pa = nc_vm_phys(g_vm, (uint32_t)b + o);
            if (pa < g_vm->ram_size) {
                rx_open_block(pa, write);
                break;
            }
        }
    }
}
#endif

/* Host code about to read [va, va + len) through a system call (a page
 * without access fails it with EFAULT rather than faulting). */
static void va_load(uint32_t va, uint32_t len)
{
#if defined(__PROSPERO__)
    rx_open_range(va, len, false);
    return;
#endif
    if (!g_vm) {
        return;
    }
    for (uint32_t a = va & ~(NC_PAGE - 1); a < va + len; a += NC_PAGE) {
        const uint32_t pa = nc_vm_phys(g_vm, a);
        if (pa < g_vm->ram_size) {
            stale_open(pa, false);
        }
    }
}

/* Memory the host or a device wrote through the RAM view (no fault: it
 * isn't the guest's view), or freed: newer than an image guarded over it,
 * which no fill may land over it now. The fill's own landing comes here too,
 * and drops its guard. */
static void unguard_written(uint32_t pa, uint32_t len)
{
    for (uint32_t p = pa & ~(NC_PAGE - 1); p < pa + len && p < g_vm->ram_size; p += NC_PAGE) {
        if (nc_vm_noaccess_has(g_vm, p)) {
            hle_barrier_unguard_range(pa, len);
            return;
        }
    }
}

static void freed(uint32_t pa, uint32_t len)
{
    if (g_vm) {
        hle_barrier_freed(pa, len);
    }
}

#if defined(__PROSPERO__)
/* What a system call is about to write: reported as hakuX reports it, then every block
 * it touches open, the pages sharing them too. */
static void rx_va_store(uint32_t va, uint32_t len)
{
    hle_va_store(va, len);
    rx_open_range(va, len, true);
}
#endif

void nc_hle_dirty_init(NcVm *vm)
{
    g_vm = vm; /* every page starts dirty (unprotected) */
    g_hdirty = calloc(vm->ram_size / NC_PAGE, 1);
#if defined(__PROSPERO__)
    nc_vm_write_hook = rx_va_store;
#else
    nc_vm_write_hook = hle_va_store;
#endif
    nc_vm_read_hook = va_load;
    nc_vm_free_hook = freed;
    /* overlapped file reads: the reader waits for the GPU, not the guest */
    nc_vm_dma = (NcVmDma){hle_mem_write_fence, hle_mem_wait_fence, hle_host_mem_set_dirty};
}

uint32_t hle_host_mem_block(void)
{
#if defined(__PROSPERO__)
    return 16384u;
#else
    return NC_PAGE;
#endif
}

bool hle_host_mem_dirty(uint32_t pa)
{
    if (!g_vm || pa >= g_vm->ram_size) {
        return true;
    }
    return __atomic_load_n(&g_vm->wp[pa / NC_PAGE], __ATOMIC_ACQUIRE) != NC_WP_CLEAN ||
           (g_hdirty && __atomic_load_n(&g_hdirty[pa / NC_PAGE], __ATOMIC_ACQUIRE));
}

bool hle_host_mem_open(uint32_t pa)
{
    if (!g_vm || pa >= g_vm->ram_size) {
        return true;
    }
    return __atomic_load_n(&g_vm->wp[pa / NC_PAGE], __ATOMIC_ACQUIRE) != NC_WP_CLEAN &&
           !nc_vm_noaccess_has(g_vm, pa);  /* (closed: a store faults) */
}

static uint64_t ram_end(uint32_t pa, uint32_t len)
{
    const uint64_t end = (uint64_t)pa + len;
    return end < g_vm->ram_size ? end : g_vm->ram_size;
}

/* Pages of [pa, pa + len) whose protection is not `ro` get it, a run of
 * them at a time; true if there were any. */
static bool protect_runs(uint32_t pa, uint32_t len, bool ro)
{
    bool any = false;
    const uint64_t end = ram_end(pa, len);
    uint32_t run_pa = 0, run_n = 0;
    for (uint64_t a = pa & ~(uint64_t)(NC_PAGE - 1); a < end; a += NC_PAGE) {
        if (g_vm->wp[a / NC_PAGE] == ro) {
            nc_vm_protect_pages(g_vm, run_pa, run_n, ro);
            run_n = 0;
            continue;
        }
        any = true;
        if (!run_n) {
            run_pa = (uint32_t)a;
        }
        run_n++;
    }
    nc_vm_protect_pages(g_vm, run_pa, run_n, ro);
    return any;
}

/* g_hdirty over [pa, pa + len): whether any was set; cleared (clear) */
static bool hdirty_take(uint32_t pa, uint32_t len, bool clear)
{
    bool any = false;
    const uint64_t end = ram_end(pa, len);
    for (uint64_t a = pa & ~(uint64_t)(NC_PAGE - 1); g_hdirty && a < end; a += NC_PAGE) {
        if (__atomic_load_n(&g_hdirty[a / NC_PAGE], __ATOMIC_ACQUIRE)) {
            any = true;
            if (clear) {
                __atomic_store_n(&g_hdirty[a / NC_PAGE], 0, __ATOMIC_RELEASE);
            }
        }
    }
    return any;
}

bool hle_host_mem_test_and_clear(uint32_t pa, uint32_t len)
{
    if (!g_vm) {
        return false;
    }
    const bool sticky = hdirty_take(pa, len, true);
    return protect_runs(pa, len, true) || sticky;
}

/* Protected again so the guest's next store faults and is counted
 * (hle_mem_first_write), without losing that the pages were written: a
 * later test_and_clear still sees it. */
void hle_host_mem_rearm(uint32_t pa, uint32_t len)
{
    if (!g_vm || !g_hdirty) {
        return;
    }
    const uint64_t end = ram_end(pa, len);
    for (uint64_t a = pa & ~(uint64_t)(NC_PAGE - 1); a < end; a += NC_PAGE) {
        if (__atomic_load_n(&g_vm->wp[a / NC_PAGE], __ATOMIC_ACQUIRE) != NC_WP_CLEAN) {
            __atomic_store_n(&g_hdirty[a / NC_PAGE], 1, __ATOMIC_RELEASE);
        }
    }
    protect_runs(pa, len, true);
}

/* A host write: every view of the page writable (the host writes through
 * the guest's), untracked until protected again. */
static void make_dirty(uint32_t pa)
{
    if (g_vm->wp[pa / NC_PAGE]) {
        nc_vm_protect_page(g_vm, pa & ~(NC_PAGE - 1), false);
    }
}

/* A guest store's fault: only the view it came through (at) made writable,
 * then the page marked written (NC_WP_WRITTEN; writable first, as
 * nc_vm_protect_pages clears its flag after). Making a view of the shared
 * RAM writable is the dear part of a fault: all three views 13.5 us a fault
 * on the Odin 3, this one 6.5 (DOA2 takes ~10k a second). A store through
 * another view faults once more and opens that one. */
static void open_view(uint32_t pa, void *at)
{
    void *page = (void *)((uintptr_t)at & ~(uintptr_t)(NC_PAGE - 1));
    if (mprotect(page, NC_PAGE, PROT_READ | PROT_WRITE)) {
        nc_vm_protect_page(g_vm, pa & ~(NC_PAGE - 1), false);   /* every view, as before */
        return;
    }
    __atomic_store_n(&g_vm->wp[pa / NC_PAGE], NC_WP_WRITTEN, __ATOMIC_RELEASE);
}

/* debug: NC_DEVWATCH="pa:len" (hex) logs each device write into that guest
 * RAM that reports itself dirty (the APU's DMA, a device model writing guest
 * memory through the RAM view, which nothing protects), with the host code
 * that wrote it as offsets in this library (llvm-addr2line). */
static int g_watch_on = -1;
static uint32_t g_watch_lo, g_watch_hi;

/* (the callers' return addresses are taken only when it is) */
static bool dev_watch_on(void)
{
    if (g_watch_on < 0) {
        const char *s = getenv("NC_DEVWATCH");
        g_watch_on = s && sscanf(s, "%x:%x", &g_watch_lo, &g_watch_hi) == 2 && g_watch_hi;
        g_watch_hi += g_watch_lo;
    }
    return g_watch_on;
}

static void dev_watch(uint32_t start, uint32_t length, void *ra0, void *ra1)
{
    static unsigned logged;
    const uint32_t lo = g_watch_lo, hi = g_watch_hi;
    if (start >= hi || start + length <= lo || logged >= 200) {
        return;
    }
    logged++;
    Dl_info di;
    const uintptr_t base = dladdr((void *)dev_watch, &di) ? (uintptr_t)di.dli_fbase : 0;
    nc_log("hakuX-native-watch", "device write to pa %08x, %u bytes, from +%lx +%lx",
           start, length, (unsigned long)((uintptr_t)ra0 - base),
           (unsigned long)((uintptr_t)ra1 - base));
}

void hle_host_mem_set_dirty(uint32_t pa, uint32_t len)
{
    if (dev_watch_on()) {
        dev_watch(pa, len, __builtin_return_address(0), __builtin_return_address(1));
    }
    if (!g_vm) {
        return;
    }
    hle_mem_host_wrote(pa, len);        /* its cache lines, before the GPU reads them */
    unguard_written(pa, len);
    protect_runs(pa, len, false);
}

/* Dirty to the trackers through g_hdirty, each page's protection left as it
 * is (no guest access while guarded; a tracked page still faults at the
 * guest's next store, and counts it) */
void hle_host_mem_landed(uint32_t pa, uint32_t len)
{
    if (!g_vm || !g_hdirty) {
        return;
    }
    hle_mem_host_wrote(pa, len);
    const uint64_t end = ram_end(pa, len);
    for (uint64_t a = pa & ~(uint64_t)(NC_PAGE - 1); a < end; a += NC_PAGE) {
        if (__atomic_load_n(&g_vm->wp[a / NC_PAGE], __ATOMIC_ACQUIRE) == NC_WP_CLEAN) {
            __atomic_store_n(&g_hdirty[a / NC_PAGE], 1, __ATOMIC_RELEASE);
        }
    }
}

void hle_host_mem_noaccess(uint32_t pa, uint32_t len, bool on)
{
    if (g_vm && len) {
        const uint32_t lo = pa & ~(NC_PAGE - 1), hi = (pa + len + NC_PAGE - 1) & ~(NC_PAGE - 1);
        nc_vm_noaccess(g_vm, lo, (hi - lo) / NC_PAGE, on);
    }
}

/* A guest store to a clean page: the first-write barrier, then the write. */
static void first_write(uint32_t pa, uint32_t len)
{
    hle_mem_first_write(pa, len);
    make_dirty(pa);
}

/* A store the host makes for the guest into [pa, pa + len), within a page
 * (an emulated one, nc_ram_emulate): counted as the guest's would be */
static void guest_stored(uint32_t pa, uint32_t len)
{
    if (g_vm->wp[pa / NC_PAGE] == NC_WP_CLEAN) {
        first_write(pa, len);
    } else if (g_vm->wp[pa / NC_PAGE] == NC_WP_WRITTEN) {
        make_dirty(pa);
    }
}

void rx_guest_stored(uint32_t pa, uint32_t n)
{
    guest_stored(pa, n);
}

static uint32_t g_wlo, g_whi, g_wdelay; /* NC_WATCHPA="pa:len[:seconds]" */

static bool watch_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *s = getenv("NC_WATCHPA");
        on = s && sscanf(s, "%x:%x:%u", &g_wlo, &g_whi, &g_wdelay) >= 2 && g_whi;
        g_whi += g_wlo;
        if (on) {
            nc_log("hakuX-native-watch", "stores to pa %08x..%08x, from %u s", g_wlo, g_whi, g_wdelay);
        }
    }
    return on;
}

/* Armed once g_wdelay seconds have passed (the clock read every 4096 calls). */
static bool watch_armed(void)
{
    static bool armed;
    static uint64_t t0;
    static unsigned calls;
    if (armed) {
        return true;
    }
    if (calls++ & 4095) {
        return false;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (!t0) {
        t0 = now;
    }
    if (now - t0 >= (uint64_t)g_wdelay * 1000000000ull) {
        armed = true;
        nc_log("hakuX-native-watch", "armed");
    }
    return armed;
}

bool nc_hle_watch_pa(uint32_t pa)
{
    return watch_on() && pa >= g_wlo && pa < g_whi;
}

void nc_hle_watch_store(uint32_t va, uint32_t pa, uint32_t eip, uint32_t len)
{
    static unsigned logged;
    if (logged++ < 300) {
        if (len) {
            nc_log("hakuX-native-watch", "host store to %08x (pa %08x), %u bytes", va, pa, len);
        } else {
            nc_log("hakuX-native-watch", "guest store to %08x (pa %08x) at eip %08x", va, pa, eip);
        }
    }
}

void nc_hle_watch_rearm(void)
{
    if (!g_vm || !watch_on() || !watch_armed()) {
        return;
    }
    /* Protected again when written, or when a new mapping of the page came
     * up writable (a page protected before it was mapped). */
    static uint32_t armed[16];
    unsigned i = 0;
    for (uint32_t a = g_wlo & ~(NC_PAGE - 1); a < g_whi && a < g_vm->ram_size && i < 16; a += NC_PAGE, i++) {
        const uint32_t va = g_vm->rmap[a / NC_PAGE];
        if (g_vm->wp[a / NC_PAGE] != NC_WP_CLEAN || armed[i] != va) {
            nc_vm_protect_page(g_vm, a, true);
            armed[i] = va;
        }
    }
}

void hle_va_store(uint32_t va, uint32_t len)
{
    if (!g_vm) {
        return;
    }
    {
        const uint32_t spa = nc_vm_phys(g_vm, va);
        if (spa != UINT32_MAX && watch_on() && spa < g_whi && spa + len > g_wlo) {
            nc_hle_watch_store(va, spa, 0, len);
        }
    }
    for (uint32_t a = va & ~(NC_PAGE - 1); a < va + len; a += NC_PAGE) {
        const uint32_t pa = nc_vm_phys(g_vm, a);
        if (pa == UINT32_MAX || pa >= g_vm->ram_size) {
            continue;
        }
        /* a page without access (barrier.c: a frame buffer or target whose
         * image is newer, a report page): what a guest access's fault does,
         * as a host write into it fails instead (EFAULT). Halo 2 streams
         * bitmaps from its map cache into an old frame buffer's memory; the
         * reads failed and its scope reticle sampled the frame buffer's
         * leftover bytes. */
        stale_open(pa, true);
        if (g_vm->wp[pa / NC_PAGE] == NC_WP_CLEAN) {
            first_write(pa, len);
        } else if (g_vm->wp[pa / NC_PAGE] == NC_WP_WRITTEN) {
            make_dirty(pa);             /* written already: its other views opened for the host */
        }
    }
}

static struct {
    uint32_t page, n;
} g_dtop[16];
static uint32_t g_dfaults;

unsigned nc_hle_dirty_top(uint32_t *pages, uint32_t *counts, unsigned max)
{
    unsigned n = 0;
    for (; n < max; n++) {
        int best = -1;
        for (unsigned k = 0; k < 16; k++) {
            if (g_dtop[k].n && (best < 0 || g_dtop[k].n > g_dtop[best].n)) {
                best = (int)k;
            }
        }
        if (best < 0) {
            break;
        }
        pages[n] = g_dtop[best].page;
        counts[n] = g_dtop[best].n * 64;
        g_dtop[best].n = 0;
    }
    memset(g_dtop, 0, sizeof(g_dtop));
    return n;
}

static void dirty_sample(uint32_t pa)
{
    if (++g_dfaults & 63) {
        return;
    }
    unsigned k = 0;
    while (k < 16 && g_dtop[k].n && g_dtop[k].page != pa >> 12) {
        k++;
    }
    if (k < 16) {
        g_dtop[k].page = pa >> 12;
        g_dtop[k].n++;
    }
}

bool nc_hle_dirty_fault(const siginfo_t *info, void *ucontext)
{
    if (!g_vm) {
        return false;
    }
    const uint64_t guest = nc_vm_guest_of(g_vm, info->si_addr);
    if (guest == UINT64_MAX) {
        return false;
    }
    const uint32_t pa = nc_vm_phys(g_vm, (uint32_t)guest);
    if (pa == UINT32_MAX || pa >= g_vm->ram_size) {
        return false;
    }
#if defined(__PROSPERO__)
    if (g_va_soft[(uint32_t)guest >> 14] == 1) {
        /* A block the console can't map (rx_map_pages). */
        const bool fetch = nc_cpu_fault_is_fetch(ucontext);
        const bool store = nc_cpu_fault_is_write(ucontext);
        if (rx_shadow_make(g_vm, (uint32_t)guest, fetch)) {
            return true;
        }
        if (rx_soft_fill(ucontext, (uint32_t)guest)) {
            return true;    /* the instruction runs again after the title's fault */
        }
        stale_open(pa, store);
        static int noemu = -1;
        if (noemu < 0) {
            noemu = access("/data/xpsemu/noemu", F_OK) == 0;
            nc_log("xog-soft", "emulator %s", noemu ? "off (/data/xpsemu/noemu): every access stepped" : "on");
        }
        if (!noemu && !fetch && !rx_steps_running()) {
            if (rx_ram_emulate(g_vm, ucontext, (uint32_t)guest, guest_stored)) {
                g_soft_emulated++;
                rx_soft_site_note();
                return true;
            }
            if (rx_ram_hole_set) {
                return rx_guest_page_fault(ucontext, rx_ram_hole, rx_ram_hole_store);
            }
        }
        {
            /* what the emulator doesn't know yet, by how often it comes */
            const NcFrame *sfr = nc_cpu_fault_frame(g_cpu_of_faults(), ucontext);
            static unsigned told;
            if (sfr && !noemu && !fetch && !rx_steps_running() && told++ < 96) {
                const uint32_t e = nc_frame_eip(sfr), ep = nc_vm_phys(g_vm, e);
                const uint8_t *cb = ep != UINT32_MAX ? (g_va_soft[e >> 14] == 2 ? (const uint8_t *)(uintptr_t)e : g_vm->phys + ep) : NULL;
                if (cb) {
                    nc_log("xog-soft", "stepped eip %08x at %08x: %02x %02x %02x %02x %02x %02x", e, (uint32_t)guest, cb[0], cb[1], cb[2], cb[3], cb[4],
                           cb[5]);
                }
            }
        }
        if (nc_cpu_fault_frame(g_cpu_of_faults(), ucontext) && rx_step_begin(g_vm, (uint32_t)guest)) {
            nc_cpu_fault_step(ucontext);
            return true;    /* the instruction runs for real, once */
        }
        g_soft_failed++;
        const NcFrame *sf = nc_cpu_fault_frame(g_cpu_of_faults(), ucontext);
        nc_log("xog-soft", "%s at %08x (pa %08x), eip %08x: can't be served", fetch ? "code" : store ? "store" : "load", (uint32_t)guest, pa,
               sf ? nc_frame_eip(sf) : 0);
        return false;
    }
    {
        /* What the access is decides. A block closes for any page in it, so an access can
         * land here that has nothing to do with the page that closed it. */
        const bool store = nc_cpu_fault_is_write(ucontext);
        const NcFrame *fr = nc_cpu_fault_frame(g_cpu_of_faults(), ucontext);
        const bool fetch = fr && nc_frame_eip(fr) >> 12 == (uint32_t)guest >> 12;
        const uint32_t first = pa & ~(16384u - 1);
        const bool here = nc_vm_noaccess_has(g_vm, pa);
        bool closed = here;
        for (uint32_t o = 0; o < 16384u && !closed; o += NC_PAGE) {
            closed = nc_vm_noaccess_has(g_vm, first + o);
        }
        if (closed && !fetch) {
            if (here && store) {
                extern bool hle_mem_report_store(uint32_t pa);
                if (hle_mem_report_store(pa) && rx_ram_emulate(g_vm, ucontext, (uint32_t)guest, guest_stored)) {
                    g_fault_kinds[RX_F_BESIDE]++;
                    rx_soft_site_note();
                    return true;
                }
            }
            if (here) {
                const uint32_t end = (pa | (NC_PAGE - 1)) + 1, hi = pa + 16 < end ? pa + 16 : end;
                const int nb = hle_mem_stale_neighbour(pa, hi);
                if (nb == 2) {
                    g_fault_kinds[RX_F_HOT]++;
                    return true;    /* its page given back: the access runs again */
                }
                if (nb == 1 && rx_ram_emulate(g_vm, ucontext, (uint32_t)guest, guest_stored)) {
                    g_fault_kinds[RX_F_BESIDE]++;
                    return true;
                }
                g_fault_kinds[store ? RX_F_TARGET_WRITE : RX_F_TARGET_READ]++;
                hle_mem_stale_access_rw(pa, store);
                return true;
            }
            dirty_sample(pa);
            if (hle_mem_stale_block_beside(pa) == 2) {
                g_fault_kinds[RX_F_HOT]++;
                return true;    /* the block given back: the access runs again */
            }
            if (rx_ram_emulate(g_vm, ucontext, (uint32_t)guest, guest_stored)) {
                g_fault_kinds[RX_F_BLOCK]++;
                rx_soft_site_note();
                return true;
            }
            g_fault_kinds[RX_F_NO_EMULATE]++;
        }
        g_fault_kinds[store ? RX_F_WATCHED : RX_F_OPENED]++;
        rx_open_block(pa, store);
        return true;
    }
#endif
    if (nc_vm_noaccess_has(g_vm, pa)) {
        /* a read or write of memory an image is newer than (the frame on
         * screen, a render target): the image goes in first, then the
         * access runs again. The guest's own bytes on a page a target only
         * shares (barrier.c hle_mem_stale_neighbour) are reached through
         * the host's view instead: the page stays closed and nothing waits
         * for the GPU. */
        uint32_t lo, hi;
        bool store = true;
        const bool known = nc_ram_access_span(g_vm, info, ucontext, &lo, &hi, &store);
        const int nb = known ? hle_mem_stale_neighbour(lo, hi) : 0;
        if (nb == 2 || (nb == 1 && nc_ram_emulate(g_vm, info, ucontext, guest_stored))) {
            return true;                    /* (2: the page given back, the access runs again) */
        }
        hle_mem_stale_access_rw(pa, !known || store);
        return true;
    }
    const uint8_t wp = __atomic_load_n(&g_vm->wp[pa / NC_PAGE], __ATOMIC_ACQUIRE);
    if (!wp) {
        /* protected, but not by write tracking: someone else's (the JIT's
         * code pages), or tracking lost the page (a crash follows) */
        static unsigned s_logged;
        if (s_logged++ < 8) {
            nc_log("hakuX-native-watch", "write fault at guest %08x (pa %08x), a page tracking "
                   "doesn't hold protected", (uint32_t)guest, pa);
        }
        return false;
    }
    if (wp == NC_WP_WRITTEN) {
        open_view(pa, info->si_addr);   /* written through another view already */
        return true;
    }
    dirty_sample(pa);
    static int all_views = -1;          /* NC_WP_ALL_VIEWS: every view opened, as before (A/B) */
    if (all_views < 0) {
        all_views = getenv("NC_WP_ALL_VIEWS") != NULL;
    }
    if (all_views) {
        first_write(pa, 4);
        return true;
    }
    hle_mem_first_write(pa, 4);
    open_view(pa, info->si_addr);
    return true;
}
