/*
 * XOG: guest memory on the PS5. With code running natively a guest address
 * IS a host address, so the Xbox's ranges below 4 GiB are reserved, and its RAM,
 * one block of direct memory, is viewed wherever the kernel maps it.
 * Confirmed on the console by probe 2. Our own code. GPL-3.0-or-later.
 *
 * The title's sce_module/libc.prx loads at 0x80000000, where the Xbox's kernel
 * belongs. Games built with the XDK read the kernel's header at 0x80010000, and
 * that range is the module's execute-only text: a read ends the process at once,
 * with no signal (SYSTEM_XO_VIOLATION, seen as "no crash report" in our runs).
 *
 * So guest RAM goes over the text. The module's code is only a few stubs, generated
 * by PS5_Vulkan's libc_builder, and libkernel calls one at each thread exit: it is
 * told about a stub of ours instead (libc_release), so RAM stays the game's. Its data
 * (0x800cc000-0x80118000) is readable, and stays: that part of the window stays a
 * hole, with the physical pages behind it never handed out.
 */
#include "xog_cpu.h"

#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "native/core/nc.h"

#define RX_FIXED 0x10
#define RX_TEXT_LO 0x80000000u
#define RX_TEXT_HI 0x800CC000u  /* the module's execute-only text ends here */

static struct ps5_shm g_ram;
static bool g_have_ram;

/* pxor xmm0, xmm0; xor eax, eax; ret: what every entry of the module's does, or less */
static const uint8_t k_stub[] = {0x66, 0x0F, 0xEF, 0xC0, 0x31, 0xC0, 0xC3};
static const uint32_t k_entries[] = {0x10, 0x30, 0x40, 0x50, 0x100, 0x200, 0x210, 0x220, 0x230, 0xC8010};
#define RX_ENTRIES (sizeof(k_entries) / sizeof(k_entries[0]))

static uint8_t *g_phys;
static bool g_libc_released;
#define RX_DATA_LO 0x800CC000u
#define RX_DATA_HI 0x80118000u
static uint8_t g_libc_shadow[RX_DATA_HI - RX_DATA_LO] __attribute__((aligned(0x4000)));
static bool g_shadow_ready;     /* libc.prx's data lives in g_libc_shadow: no hole */

bool rx_hole_on(void)
{
    return !g_shadow_ready;
}

/* The stub libkernel calls now. In the title's own code, so nothing of it is in guest memory. */
__asm__(".text\n.globl rx_libc_stub\n.type rx_libc_stub,@function\nrx_libc_stub:\n"
        "pxor %xmm0, %xmm0\nxor %eax, %eax\nret\n");
extern void rx_libc_stub(void);

static void entries_write(uint8_t *phys)
{
    for (unsigned i = 0; i < RX_ENTRIES; i++) {
        memcpy(phys + k_entries[i], k_stub, sizeof(k_stub));
    }
}

/* Physical page 0 is the game's (XDK input code keeps its tables there): nothing is written there
 * unless libkernel could not be told about our stub, and then once, before the game runs. */
void rx_stubs_write(uint8_t *phys)
{
    g_phys = phys;
    if (!g_libc_released) {
        nc_log("xog-vm", "libc.prx's entries written into RAM: libkernel still calls them");
        entries_write(phys);
    }
}

/* The process is ending: its way out runs libc.prx's DT_FINI. The game has stopped mattering. */
void rx_stubs_leave(void)
{
    if (g_phys) {
        entries_write(g_phys);
        if (g_shadow_ready) {
            memcpy(g_phys + (RX_DATA_LO - 0x80000000u), g_libc_shadow, sizeof(g_libc_shadow));
        }
    }
}

/* ---- libkernel told, before guest RAM goes over the module ---- */
int sceKernelDlsym(int handle, const char *name, void **addr);
int sceKernelVirtualQuery(const void *addr, int flags, void *info, size_t size);

static void *g_scan_escape[5];  /* the compiler's own, no import: the title has no libc to give one */
static void scan_fault(int sig)
{
    (void)sig;
    __builtin_longjmp(g_scan_escape, 1);
}

/* libkernel's own copy of a thread-exit entry, wherever it keeps one. Only the three it calls, by
 * exact value, in memory that is neither ours nor a stack's. */
static unsigned rewrite_exit_entries(unsigned *mappings, unsigned *faulted, uint64_t *where)
{
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = scan_fault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    unsigned done = 0;
    uint64_t q = 0;
    for (;;) {
        struct {
            uint64_t start, end, offset;
            int32_t protection, memory_type;
            uint32_t bits;
            char name[32];
            uint32_t pad;
        } info;
        memset(&info, 0, sizeof(info));
        if (sceKernelVirtualQuery((const void *)(uintptr_t)q, 1, &info, sizeof(info)) != 0 || info.end <= q) {
            break;
        }
        q = info.end;
        const uint64_t bytes = info.end - info.start;
        const bool rw = (info.protection & 7) == 3;
        const bool stack = info.bits & 4, unnamed_direct = (info.bits & 2) && !(info.bits & 1) && !info.name[0];
        const bool eboot = info.start >= 0x3000000000ull && info.start < 0x3010000000ull;
        if (!rw || stack || unnamed_direct || eboot || bytes > 0x20000000ull ||
            (info.start < 0x80200000ull && info.end > 0x80000000ull)) {
            continue;
        }
        (*mappings)++;
        volatile uint64_t *w = (volatile uint64_t *)(uintptr_t)info.start;
        const uint64_t words = bytes / 8;
        volatile uint64_t k = 0;
        if (__builtin_setjmp(g_scan_escape) == 0) {
            for (; k < words; k++) {
                const uint64_t v = w[k];
                if (v == 0x80000200ull || v == 0x80000210ull || v == 0x80000220ull) {
                    w[k] = (uint64_t)(uintptr_t)&rx_libc_stub;
                    *where = (uint64_t)(uintptr_t)&w[k];
                    done++;
                } else if (g_shadow_ready && v >= RX_DATA_LO && v < RX_DATA_HI) {
                    w[k] = (uint64_t)(uintptr_t)g_libc_shadow + (v - RX_DATA_LO);
                    *where = (uint64_t)(uintptr_t)&w[k];
                    done++;
                }
            }
        } else {
            (*faulted)++;
        }
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    return done;
}

/* libc.prx's data into our copy, and what points to it over there. False: the hole stays, nothing changed. */
static void libc_take_data(void)
{
    /* all of it readable, as libc_builder lays it out */
    for (uint32_t va = RX_DATA_LO; va < RX_DATA_HI;) {
        void *s = 0, *e = 0;
        uint32_t prot = 0;
        if (sceKernelQueryMemoryProtection((void *)(uintptr_t)va, &s, &e, &prot) != 0 || (uintptr_t)s > va || !(prot & 1) ||
            (uintptr_t)e <= va) {
            nc_log("xog-vm", "libc.prx's data at %08x isn't as expected: the hole stays", va);
            return;
        }
        va = (uint32_t)(uintptr_t)e;
    }
    const uint64_t heap_fn = *(const volatile uint64_t *)(uintptr_t)0x8010F8E0u;     /* its import of the heap call */
    memcpy(g_libc_shadow, (const void *)(uintptr_t)RX_DATA_LO, sizeof(g_libc_shadow));
    unsigned inner = 0;
    for (size_t i = 0; i + 8 <= sizeof(g_libc_shadow); i += 8) {
        uint64_t v;
        memcpy(&v, g_libc_shadow + i, 8);
        uint64_t to = 0;
        if (v >= RX_DATA_LO && v < RX_DATA_HI) {
            to = (uint64_t)(uintptr_t)g_libc_shadow + (v - RX_DATA_LO);
        } else {
            for (unsigned k = 0; k < RX_ENTRIES; k++) {
                if (v == 0x80000000ull + k_entries[k]) {
                    to = (uint64_t)(uintptr_t)&rx_libc_stub;
                }
            }
        }
        if (to) {
            memcpy(g_libc_shadow + i, &to, 8);
            inner++;
        }
    }
    g_shadow_ready = true;
    if (heap_fn >= 0x100000000ull) {
        ((int (*)(void *))(uintptr_t)heap_fn)(g_libc_shadow + (0x80110100u - RX_DATA_LO));
    }
    unsigned mappings = 0, faulted = 0;
    uint64_t where = 0;
    const unsigned rewritten = rewrite_exit_entries(&mappings, &faulted, &where);
    nc_log("xog-vm", "libc.prx's data moved: %u of its own pointers, heap table %s, %u others moved (last at %llx) in %u mappings, %u faulted: no hole",
           inner, heap_fn >= 0x100000000ull ? "told" : "not told", rewritten, (unsigned long long)where, mappings, faulted);
}

static void libc_release(void)
{
    static const char *const setters[] = {"_sceKernelSetThreadDtors", "_sceKernelSetThreadAtexitCount",
                                          "_sceKernelSetThreadAtexitReport"};
    /* libc.prx called them when it started: its table of imports holds their addresses
     * (libc_builder's GOT, in the order of tooling/native/runtime/imports.txt) */
    static const uint32_t slots[] = {0x8010F908u, 0x8010F8F8u, 0x8010F900u};
    unsigned told = 0;
    for (unsigned i = 0; i < 3; i++) {
        const uint64_t at = *(const volatile uint64_t *)(uintptr_t)slots[i];
        void *fn = at >= 0x100000000ull ? (void *)(uintptr_t)at : NULL;
        if (!fn && (sceKernelDlsym(1, setters[i], &fn) != 0 || !fn) &&
            (sceKernelDlsym(0x2001, setters[i], &fn) != 0 || !fn)) {
            nc_log("xog-vm", "%s: not found", setters[i]);
            continue;
        }
        ((int (*)(void *))fn)((void *)&rx_libc_stub);
        told++;
    }
    /* the heap table libkernel was given: its functions are stubs too */
    void *s = 0, *e = 0;
    uint32_t prot = 0;
    unsigned heap = 0;
    const int q = sceKernelQueryMemoryProtection((void *)(uintptr_t)0x80110100u, &s, &e, &prot);
    if (q == 0 && (prot & 2)) {
        volatile uint64_t *t = (volatile uint64_t *)(uintptr_t)0x80110100u;
        for (unsigned i = 0; i < 9; i++) {
            for (unsigned k = 0; k < RX_ENTRIES; k++) {
                if (t[i] == 0x80000000ull + k_entries[k]) {
                    t[i] = (uint64_t)(uintptr_t)&rx_libc_stub;
                    heap++;
                    break;
                }
            }
        }
    }
    unsigned mappings = 0, faulted = 0;
    uint64_t where = 0;
    const unsigned rewritten = rewrite_exit_entries(&mappings, &faulted, &where);
    g_libc_released = told == 3 || rewritten > 0;
    nc_log("xog-vm", "libkernel told: %u of 3; heap table %u (query %08x prot %x); %u copies rewritten, last at %llx, in %u mappings (%u faulted): page 0 %s",
           told, heap, (unsigned)q, prot, rewritten, (unsigned long long)where, mappings, faulted,
           g_libc_released ? "is the game's" : "keeps the entries");
    if (g_libc_released) {
        libc_take_data();
    }
}

/* Reserves the free parts of [lo, hi); what is already there stays. */
static bool reserve_free(uint64_t lo, uint64_t hi)
{
    uint64_t va = lo, gap = lo;
    bool ok = true;
    while (va < hi) {
        void *s = 0, *e = 0;
        uint32_t prot = 0;
        const int q = sceKernelQueryMemoryProtection((void *)(uintptr_t)va, &s, &e, &prot);
        const bool used = q == 0 && (uint64_t)(uintptr_t)e > va;
        if (used) {
            nc_log("xog-vm", "in use: %p-%p prot %x", s, e, prot);
            ok = false;
        }
        const uint64_t next = used ? (((uint64_t)(uintptr_t)e + 0x3FFF) & ~0x3FFFull) : va + 0x4000;
        if ((used || next >= hi) && (used ? va : next) > gap) {
            const uint64_t end = used ? va : next;
            void *a = (void *)(uintptr_t)gap;
            const int r = sceKernelReserveVirtualRange(&a, (size_t)(end - gap), RX_FIXED, 0);
            if (r != 0 || (uint64_t)(uintptr_t)a != gap) {
                nc_log("xog-vm", "reserve %08llx-%08llx failed: %08x", (unsigned long long)gap,
                       (unsigned long long)end, (unsigned)r);
                ok = false;
            }
        }
        if (used) {
            gap = next;
        }
        va = next;
    }
    return ok;
}

bool rx_vm_reserve(void)
{
    libc_release();
    /* the module as PS5_Vulkan's builder lays it out, or nothing below is safe */
    void *s = 0, *e = 0;
    uint32_t prot = 0;
    const int q = sceKernelQueryMemoryProtection((void *)(uintptr_t)RX_TEXT_LO, &s, &e, &prot);
    nc_log("xog-vm", "libc.prx text: rc %08x, %p-%p prot %x", (unsigned)q, s, e, prot);
    if (q != 0 || (uint64_t)(uintptr_t)e != RX_TEXT_HI) {
        nc_log("xog-vm", "libc.prx isn't laid out as expected: its text stays, XDK games will end");
    }
    const bool low = reserve_free(0x10000, 0x80000000u);
    const bool win = reserve_free(g_shadow_ready ? RX_DATA_HI : RX_HOLE_END, 0x100000000ull);
    nc_log("xog-vm", "Xbox addresses reserved: low %s, window and devices %s", low ? "clean" : "NOT CLEAN",
           win ? "clean" : "NOT CLEAN");
    return low && win;
}

bool rx_ram_create(uint32_t bytes)
{
    const int r = ps5_shm_create(bytes, &g_ram);
    if (r != 0) {
        nc_log("xog-vm", "guest RAM (%u MiB): %08x", bytes >> 20, (unsigned)r);
        return false;
    }
    g_have_ram = true;
    return true;
}

void *rx_ram_view(uint32_t bytes)
{
    void *v = NULL;
    const int r = ps5_shm_map(&g_ram, 0, bytes, NULL, PS5_SHM_READ | PS5_SHM_WRITE, 0, &v);
    if (r != 0) {
        nc_log("xog-vm", "RAM view: %08x", (unsigned)r);
        return NULL;
    }
    return v;
}

/* [va, va + len) without the hole, as up to two parts. Returns how many. */
static int split(uint32_t va, uint32_t len, uint32_t part[2][2])
{
    const uint64_t end = (uint64_t)va + len;
    int n = 0;
    if (g_shadow_ready) {
        part[0][0] = va;
        part[0][1] = len;
        return 1;
    }
    if (va < RX_TEXT_HI) {
        const uint64_t e = end < RX_TEXT_HI ? end : RX_TEXT_HI;
        if (e > va) {
            part[n][0] = va;
            part[n][1] = (uint32_t)(e - va);
            n++;
        }
    }
    if (end > RX_HOLE_END) {
        const uint32_t s = va > RX_HOLE_END ? va : RX_HOLE_END;
        part[n][0] = s;
        part[n][1] = (uint32_t)(end - s);
        n++;
    }
    return n;
}

bool rx_map(uint32_t va, uint32_t pa, uint32_t len)
{
    if (!g_have_ram) {
        return false;
    }
    uint32_t part[2][2];
    const int n = split(va, len, part);
    for (int i = 0; i < n; i++) {
        void *v = NULL;
        const int r = ps5_shm_map(&g_ram, pa + (part[i][0] - va), part[i][1], (void *)(uintptr_t)part[i][0],
                                  PS5_SHM_READ | PS5_SHM_WRITE | PS5_SHM_EXEC, PS5_SHM_FIXED, &v);
        if (r != 0) {
            nc_log("xog-vm", "map va %08x pa %08x len %x: %08x", part[i][0], pa + (part[i][0] - va), part[i][1],
                   (unsigned)r);
            return false;
        }
    }
    return true;
}

bool rx_unmap(uint32_t va, uint32_t len)
{
    uint32_t part[2][2];
    const int n = split(va, len, part);
    for (int i = 0; i < n; i++) {
        if (ps5_shm_unmap((void *)(uintptr_t)part[i][0], part[i][1], PS5_SHM_KEEP_RESERVED) != 0) {
            return false;
        }
    }
    return true;
}

/* The console makes read+execute into execute-only, and a game's read of that ends
 * the title with no signal. So a write-tracked page is read-only without execute;
 * code run from one comes back as a fault, and cpu_ps5.c opens the page. */
int rx_protect(void *addr, size_t len, int prot)
{
    uint32_t part[2][2];
    const int n = split((uint32_t)(uintptr_t)addr, (uint32_t)len, part);
    const int p = prot == 0 ? 0 : (prot & 2) ? (0x1 | 0x2 | 0x4) : 0x1;
    int r = 0;
    for (int i = 0; i < n; i++) {
        const int q = sceKernelMprotect((void *)(uintptr_t)part[i][0], part[i][1], p);
        r = q ? q : r;
    }
    return r;
}
