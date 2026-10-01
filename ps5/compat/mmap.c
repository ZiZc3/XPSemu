/*
 * Anonymous mmap for a PS5 title.
 *
 * A title's plain mmap fails for anonymous memory; its anonymous memory is
 * "flexible memory". The link wraps mmap and munmap (ps5/build-title.sh), so
 * every caller, RADV included, gets these; file mappings go to the real mmap.
 *
 * Executable memory (QEMU's JIT buffer, which asks for RWX up front) is JIT
 * shared memory, as PS5SX2's recompilers use it (orbis-shims/orbismem.cpp).
 * It needs the process jailbroken first (xemu_ps5_early_init). RWX flexible
 * memory is the fallback. QEMU finds its buffer already has the protection it
 * wants, so it needs no mprotect, which flexible memory doesn't allow.
 *
 * Big read/write mappings (the Xbox's 128 MiB of RAM) are direct memory,
 * CPU-only, as PS5_Vulkan's driver maps it (driver/ps5vk_direct_memory.c):
 * there are gigabytes of it, where flexible memory is about 400 MiB, all
 * the JIT buffer and the rest share. Flexible memory is the fallback.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags);
int sceKernelReleaseFlexibleMemory(void *addr, size_t len);
int sceKernelReserveVirtualRange(void **addr, size_t len, int flags,
                                 size_t alignment);
int sceKernelAvailableFlexibleMemorySize(size_t *available);
int sceKernelJitCreateSharedMemory(const char *name, size_t len, int max_prot,
                                   int *fd);
int sceKernelJitMapSharedMemory(int fd, int prot, void **addr);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end,
                                  size_t len, size_t alignment, int type,
                                  int64_t *start);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags,
                             int64_t start, size_t alignment);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelMunmap(void *addr, size_t len);

void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd,
                  off_t offset);
int __real_munmap(void *addr, size_t len);

#define KERNEL_PROT_READ 0x01
#define KERNEL_PROT_WRITE 0x02
#define KERNEL_PROT_EXEC 0x04
#define KERNEL_MAP_FIXED 0x10
#define KERNEL_PAGE_SIZE ((size_t)0x4000)

/* RADV's GPU-visible memory must lie here (PS5SX2's orbis_vk.cpp). */
#define GPU_WINDOW_START 0x200000000ULL
#define GPU_WINDOW_END 0x300000000ULL

/*
 * Where the kernel is asked to place mappings that don't name an address, so
 * xemu's memory (guest RAM, the JIT buffer, coroutine stacks) leaves RADV's
 * window free. Without a hint, the first mapping landed at 0x2'0001'c000.
 */
#define ADDRESS_HINT ((void *)0x1000000000ULL) /* 64 GiB */

#define DIRECT_TYPE_CPU 12                  /* CPU-only direct memory */
#define DIRECT_ALIGNMENT ((size_t)0x10000)  /* Its unit */
#define DIRECT_MIN_SIZE ((size_t)96 << 20)  /* Smaller maps stay flexible */

/* The direct memory mapped, to release it on munmap. */
static struct {
    void *addr;
    size_t len;
    int64_t start;
} direct_maps[8];
static pthread_mutex_t direct_lock = PTHREAD_MUTEX_INITIALIZER;

static void report(const char *what, void *addr, size_t len, int rc)
{
    size_t available = 0;
    sceKernelAvailableFlexibleMemorySize(&available);
    uintptr_t a = (uintptr_t)addr;
    fprintf(stderr, "ps5 mmap: %s %zu KiB at %p rc=%#x (flexible left %zu MiB)%s\n",
            what, len >> 10, addr, rc, available >> 20,
            (a < GPU_WINDOW_END && a + len > GPU_WINDOW_START) ?
                " IN RADV'S GPU WINDOW" : "");
}

/* Direct memory mapped at addr (or anywhere, with the hint), or NULL. */
static void *map_direct(void *addr, size_t len, int kernel_prot,
                        int kernel_flags)
{
    int64_t start = -1;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                           len, DIRECT_ALIGNMENT,
                                           DIRECT_TYPE_CPU, &start);
    if (rc != 0) {
        report("allocate direct", NULL, len, rc);
        return NULL;
    }
    void *result = addr ? addr : ADDRESS_HINT;
    rc = sceKernelMapDirectMemory(&result, len, kernel_prot, kernel_flags,
                                  start, DIRECT_ALIGNMENT);
    if (rc != 0 && !(kernel_flags & KERNEL_MAP_FIXED)) {
        result = NULL;
        rc = sceKernelMapDirectMemory(&result, len, kernel_prot, 0, start,
                                      DIRECT_ALIGNMENT);
    }
    report("map direct rw", result, len, rc);
    if (rc == 0 && (kernel_flags & KERNEL_MAP_FIXED) && result != addr) {
        sceKernelMunmap(result, len);
        rc = -1;
    }
    if (rc != 0) {
        sceKernelReleaseDirectMemory(start, len);
        return NULL;
    }

    pthread_mutex_lock(&direct_lock);
    for (size_t i = 0; i < sizeof(direct_maps) / sizeof(direct_maps[0]); i++) {
        if (!direct_maps[i].addr) {
            direct_maps[i].addr = result;
            direct_maps[i].len = len;
            direct_maps[i].start = start;
            break;
        }
    }
    pthread_mutex_unlock(&direct_lock);
    return result;
}

/* Unmaps and releases direct memory mapped at addr: false if it isn't. */
static bool unmap_direct(void *addr, size_t len)
{
    bool found = false;
    pthread_mutex_lock(&direct_lock);
    for (size_t i = 0; i < sizeof(direct_maps) / sizeof(direct_maps[0]); i++) {
        if (direct_maps[i].addr == addr && direct_maps[i].len == len) {
            sceKernelMunmap(addr, len);
            sceKernelReleaseDirectMemory(direct_maps[i].start, len);
            direct_maps[i].addr = NULL;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&direct_lock);
    return found;
}

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd,
                  off_t offset)
{
    if (!(flags & MAP_ANON) || fd != -1) {
        void *mapped = __real_mmap(addr, len, prot, flags, fd, offset);
        if (mapped != MAP_FAILED || fd < 0 || (flags & MAP_FIXED)) {
            return mapped;
        }
        /* The console may not map files for a title: a copy instead. */
        void *copy = __wrap_mmap(NULL, len, PROT_READ | PROT_WRITE,
                                 MAP_ANON | MAP_PRIVATE, -1, 0);
        if (copy == MAP_FAILED) {
            return MAP_FAILED;
        }
        ssize_t got = pread(fd, copy, len, offset);
        fprintf(stderr, "ps5 mmap: file mapping of %zu KiB emulated with a "
                        "copy (read %zd)\n", len >> 10, got);
        return copy;
    }
    if (len == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    len = (len + KERNEL_PAGE_SIZE - 1) & ~(KERNEL_PAGE_SIZE - 1);

    /* An address range to map into later with MAP_FIXED (QEMU's guest RAM). */
    if (prot == PROT_NONE && !(flags & MAP_FIXED)) {
        void *result = addr ? addr : ADDRESS_HINT;
        int rc = sceKernelReserveVirtualRange(&result, len, 0, 0);
        if (rc != 0 && !addr) {
            result = NULL;
            rc = sceKernelReserveVirtualRange(&result, len, 0, 0);
        }
        report("reserve", result, len, rc);
        if (rc != 0) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
        return result;
    }

    int kernel_flags = (flags & MAP_FIXED) ? KERNEL_MAP_FIXED : 0;
    int kernel_prot = 0;
    if (prot & PROT_READ) {
        kernel_prot |= KERNEL_PROT_READ;
    }
    if (prot & PROT_WRITE) {
        kernel_prot |= KERNEL_PROT_READ | KERNEL_PROT_WRITE;
    }
    if (prot & PROT_EXEC) {
        kernel_prot |= KERNEL_PROT_READ | KERNEL_PROT_EXEC;
    }

    void *hint = (flags & MAP_FIXED) || addr ? addr : ADDRESS_HINT;
    void *result = hint;
    int rc;
    if ((prot & PROT_EXEC) && !(flags & MAP_FIXED)) {
        int fd = -1;
        rc = sceKernelJitCreateSharedMemory(NULL, len, kernel_prot, &fd);
        if (rc == 0) {
            result = hint;
            rc = sceKernelJitMapSharedMemory(fd, kernel_prot, &result);
            if (rc != 0) {
                result = NULL;
                rc = sceKernelJitMapSharedMemory(fd, kernel_prot, &result);
            }
        }
        report("map jit", result, len, rc);
        if (rc == 0) {
            return result;
        }
        result = hint; /* Not jailbroken? Try flexible memory. */
    }

    if (!(prot & PROT_EXEC) && len >= DIRECT_MIN_SIZE &&
        len % DIRECT_ALIGNMENT == 0) {
        void *direct = map_direct((flags & MAP_FIXED) ? addr : NULL, len,
                                  kernel_prot, kernel_flags);
        if (direct) {
            return direct;
        }
    }

    rc = sceKernelMapFlexibleMemory(&result, len, kernel_prot, kernel_flags);
    if (rc != 0 && hint != addr) {
        result = addr; /* The hint couldn't be honoured: anywhere, then */
        rc = sceKernelMapFlexibleMemory(&result, len, kernel_prot, kernel_flags);
    }
    if (len >= (1 << 20) || rc != 0) {
        report(kernel_prot & KERNEL_PROT_EXEC ? "map flexible rwx" :
                                                "map flexible rw",
               result, len, rc);
    }
    if (rc != 0) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if ((flags & MAP_FIXED) && result != addr) {
        sceKernelReleaseFlexibleMemory(result, len);
        errno = EINVAL;
        return MAP_FAILED;
    }
    return result;
}

int __wrap_munmap(void *addr, size_t len)
{
    len = (len + KERNEL_PAGE_SIZE - 1) & ~(KERNEL_PAGE_SIZE - 1);
    if (unmap_direct(addr, len)) {
        return 0;
    }
    if (sceKernelReleaseFlexibleMemory(addr, len) == 0) {
        return 0;
    }
    /* A reserved range, or a file mapping. */
    return __real_munmap(addr, len);
}
