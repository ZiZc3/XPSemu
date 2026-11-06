/*
 * hakuX native core — what the native core gives the HLE GPU
 * (hw/xbox/hle/host.h): the guest's registers at a probe, guest memory
 * by VA, the JIT and configuration. Write tracking of guest RAM
 * is dirty.c's, the GPU device's interrupts and display timing dev/nv2a.c's.
 */
#include "native/hle/hle_native.h"
#include "native/dev/dev.h"

#include "hw/xbox/hle/host.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

HleCpu nc_hle_regs;

static NcVm *g_vm;
static NcCpu *g_cpu;
static bool g_flush;
static unsigned g_flushes;

void nc_hle_host_init(NcKernel *k, NcVm *vm, NcCpu *cpu)
{
    g_vm = vm;
    g_cpu = cpu;
}

/* ---------------------------------------------------------- the guest CPU */

HleCpu *hle_host_cpu(void)
{
    return g_vm ? &nc_hle_regs : NULL;
}

void hle_host_kick(void)
{
    if (g_cpu) {
        nc_cpu_request_interrupt(g_cpu); /* leave the JIT at the next block */
    }
}

/* The guest CPU's thread alone counts its holds; RDTSC leaves out what the
 * kernel's time does (nc_ke_hold_end), the APU's sample count not (it
 * played on). Within a pause (its work storing into guest memory) a hold
 * has nothing to do. */
static unsigned g_holds;
static bool g_hold_on;
static uint64_t g_hold_c0;

/* The guest's CPU waiting for the GPU: time, count and longest, by what asked (xog-gpu-wait,
 * once a second); what the DPCs had of it (xog-dpc). */
#define HOLD_KINDS 12
static struct {
    const char *why;
    uint32_t n;
    uint64_t ns, max_ns;
} g_hold_kinds[HOLD_KINDS];
static uint64_t g_hold_t0, g_hold_all_ns;
static const char *g_hold_why;

static uint64_t hold_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void hold_note(uint64_t ns)
{
    g_hold_all_ns += ns;
    for (unsigned i = 0; i < HOLD_KINDS; i++) {
        if (!g_hold_kinds[i].why || g_hold_kinds[i].why == g_hold_why) {
            g_hold_kinds[i].why = g_hold_why;
            g_hold_kinds[i].n++;
            g_hold_kinds[i].ns += ns;
            g_hold_kinds[i].max_ns = ns > g_hold_kinds[i].max_ns ? ns : g_hold_kinds[i].max_ns;
            return;
        }
    }
}

uint64_t nc_hold_total_ns(void)
{
    return g_hold_all_ns;
}

extern void nc_dpc_worst(uint64_t *run_ns, uint64_t *wait_ns, uint32_t *routine);
extern void nc_fault_kinds(uint32_t *out, unsigned n);

void nc_hold_report(void)
{
    char b[360];
    int o = 0;
    uint64_t ns = 0, longest = 0;
    uint32_t count = 0;
    for (unsigned i = 0; i < HOLD_KINDS && g_hold_kinds[i].why && o < 300; i++) {
        if (g_hold_kinds[i].n) {
            o += snprintf(b + o, sizeof(b) - (size_t)o, " | %s %u, %.2f ms, longest %.2f", g_hold_kinds[i].why, g_hold_kinds[i].n,
                          (double)g_hold_kinds[i].ns / 1e6, (double)g_hold_kinds[i].max_ns / 1e6);
        }
        ns += g_hold_kinds[i].ns;
        count += g_hold_kinds[i].n;
        longest = g_hold_kinds[i].max_ns > longest ? g_hold_kinds[i].max_ns : longest;
        g_hold_kinds[i].n = 0;
        g_hold_kinds[i].ns = g_hold_kinds[i].max_ns = 0;
    }
    b[o] = 0;
    nc_log("xog-gpu-wait", "%u waits, %.2f ms this second, longest %.2f ms%s", count, (double)ns / 1e6, (double)longest / 1e6, b);
    uint64_t run = 0, wait = 0;
    uint32_t routine = 0;
    nc_dpc_worst(&run, &wait, &routine);
    nc_log("xog-dpc", "longest %.2f ms (routine %08x), %.2f ms of it a GPU wait", (double)run / 1e6, routine, (double)wait / 1e6);
    extern void nc_io_read_kinds(uint32_t *paced, uint32_t *free_reads);
    uint32_t paced = 0, free_reads = 0;
    nc_io_read_kinds(&paced, &free_reads);
    if (paced || free_reads) {
        nc_log("xog-io", "reads paced as the drive %u, straight through %u", paced, free_reads);
    }
    extern void nc_soft_report(void);
    nc_soft_report();
    uint32_t f[8];
    nc_fault_kinds(f, 8);
    nc_log("xog-faults", "watched store %u, opened %u | target read %u, target write %u | beside a target %u, in its block %u, handed back %u, no emulation %u",
           f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
}

void hle_host_guest_hold_at(bool on, const char *why)
{
    if (!g_cpu || !nc_irq_cpu_thread()) {
        return;
    }
    if (on) {
        if (g_holds == 0) {
            g_hold_why = why;
            g_hold_t0 = hold_now();
        }
        if (g_holds++ == 0 && (g_hold_on = nc_ke_hold_begin())) {
            g_hold_c0 = nc_cpu_host_cycles();
        }
    } else if (g_holds && --g_holds == 0 && (hold_note(hold_now() - g_hold_t0), g_hold_on)) {
        g_hold_on = false;
        const uint64_t cycles = nc_cpu_host_cycles() - g_hold_c0;
        uint64_t held;
        const uint64_t out = nc_ke_hold_end(&held);
        if (out && held) {
            nc_cpu_cycle_offset_add(g_cpu, (uint64_t)((unsigned __int128)cycles * out / held));
            nc_dev_apu_time_held(out);
        }
    }
}

void hle_host_code_changed(void)
{
    g_flush = true;
}

unsigned hle_host_code_flushes(void)
{
    return __atomic_load_n(&g_flushes, __ATOMIC_RELAXED);
}

/* At the end of a system op: the JIT drops what the HLE made stale. */
void nc_hle_host_flush(void)
{
    if (g_flush) {
        g_flush = false;
        __atomic_add_fetch(&g_flushes, 1, __ATOMIC_RELAXED);
        nc_cpu_invalidate(g_cpu, 0, 0xFFFFF000u);
    }
}

/* The title starts before any of its code is translated: nothing to drop. */
void nc_hle_host_flush_discard(void)
{
    g_flush = false;
}

/* ----------------------------------------------------------- guest memory */

static uint32_t va_to_pa(uint32_t va)
{
    return g_vm ? nc_vm_phys(g_vm, va) : UINT32_MAX;
}

int hle_va_rw(uint32_t va, void *buf, size_t len, bool write)
{
    uint8_t *b = buf;
    while (len) {
        const size_t left = NC_PAGE - (va & (NC_PAGE - 1));
        const size_t n = len < left ? len : left;
        const uint32_t pa = va_to_pa(va);
        if (pa == UINT32_MAX || va >= 0xFD000000u) {
            return -1; /* unmapped, or a device window */
        }
        if (pa < g_vm->ram_size) {
            /* The RAM view, never protected; a write is a guest write. */
            if (write) {
                memcpy(g_vm->phys + pa, b, n);
                hle_host_mem_set_dirty(pa, (uint32_t)n);
            } else {
                memcpy(b, g_vm->phys + pa, n);
            }
        }
        va += (uint32_t)n;
        b += n;
        len -= n;
    }
    return 0;
}

/* ---------------------------------------------------------- configuration */

const char *hle_host_base_path(void)
{
    static char path[256];
    if (!path[0]) {
        const char *b = getenv("HAKUX_NATIVE_BASE");
        snprintf(path, sizeof(path), "%s/", b ? b : "/data/local/tmp/native/hle");
        for (char *p = path + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                mkdir(path, 0775);
                *p = '/';
            }
        }
    }
    return path;
}

static int g_render_scale = 1;

void nc_hle_set_render_scale(int scale)
{
    g_render_scale = scale < 1 ? 1 : scale > 6 ? 6 : scale;
}

int hle_host_render_scale(void)
{
    return g_render_scale;
}

/* A custom Vulkan driver's entry point (the app loads Turnip through
 * adrenotools), or the system driver's. */
static PFN_vkGetInstanceProcAddr g_custom_gipa;

void nc_hle_set_vk_loader(void *gipa)
{
    g_custom_gipa = (PFN_vkGetInstanceProcAddr)gipa;
}

void *hle_host_vk_loader(void)
{
    static PFN_vkGetInstanceProcAddr gipa;
    if (g_custom_gipa) {
        return (void *)g_custom_gipa;
    }
    if (!gipa) {
        void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
        if (!gipa) {
            nc_log("hakuX-native-hle", "no Vulkan loader: %s", dlerror());
        }
    }
    return (void *)gipa;
}

/* The app tells the user and goes back to the library (nc_hle_fatal). */
void hle_host_gpu_lost(const char *why)
{
    nc_hle_set_fatal(NC_FATAL_GPU_LOST, why);
}

/* Disc reads aren't counted here. */
bool hle_host_disc_stats(uint64_t *ops, uint64_t *bytes, uint64_t *ns)
{
    return false;
}
