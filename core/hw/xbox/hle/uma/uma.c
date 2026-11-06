/*
 * hakuX HLE GPU — guest RAM in GPU-importable memory (G0.3 strategy (a)).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/uma/uma.h"

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static AHardwareBuffer *g_uma_ahb;

static void *uma_alloc_usage(size_t size, uint64_t usage, AHardwareBuffer **out)
{
    AHardwareBuffer_Desc d = {
        .width = (uint32_t)size,
        .height = 1,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_BLOB,
        .usage = usage,
    };
    AHardwareBuffer *ahb = NULL;
    void *ptr = NULL;
    if (AHardwareBuffer_allocate(&d, &ahb) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "hakuX-hle",
                            "uma: AHB allocate(%zu) failed", size);
        return NULL;
    }
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                             AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL,
                             &ptr) != 0 || !ptr) {
        __android_log_print(ANDROID_LOG_ERROR, "hakuX-hle", "uma: AHB lock failed");
        AHardwareBuffer_release(ahb);
        return NULL;
    }
    if ((uintptr_t)ptr & (hle_host_page_size() - 1)) {
        __android_log_print(ANDROID_LOG_ERROR, "hakuX-hle",
                            "uma: AHB pointer %p not page aligned", ptr);
        AHardwareBuffer_unlock(ahb, NULL);
        AHardwareBuffer_release(ahb);
        return NULL;
    }
    memset(ptr, 0, size);
    __android_log_print(ANDROID_LOG_INFO, "hakuX-hle",
                        "uma: guest RAM from AHB size=%zu ptr=%p", size, ptr);
    *out = ahb;
    return ptr;   /* lives as long as the VM; intentionally never released */
}

struct AHardwareBuffer *hle_uma_ahb(void)
{
    return g_uma_ahb;
}

#define UMA_LOG(...) __android_log_print(ANDROID_LOG_INFO, "hakuX-hle", __VA_ARGS__)

static int g_uma_fd = -1;
static size_t g_uma_size;

/* The file descriptors of the buffer's native handle: the public way to
 * them is AHardwareBuffer_sendHandleToUnixSocket, over a socket pair to
 * ourselves (the fds arrive as SCM_RIGHTS). Returns how many. */
static int handle_fds(AHardwareBuffer *ahb, int *fds, int max)
{
    int sv[2], n = 0;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        return 0;
    }
    if (AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0]) == 0) {
        char data[4096];
        char ctl[CMSG_SPACE(sizeof(int) * 32)];
        struct iovec iov = { data, sizeof(data) };
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl,
                            .msg_controllen = sizeof(ctl) };
        if (recvmsg(sv[1], &m, MSG_CMSG_CLOEXEC) >= 0) {
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
                    continue;
                }
                const int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                const int *p = (const int *)CMSG_DATA(c);
                for (int i = 0; i < k; i++) {
                    if (n < max) {
                        fds[n++] = p[i];
                    } else {
                        close(p[i]);
                    }
                }
            }
        }
    }
    close(sv[0]);
    close(sv[1]);
    return n;
}

/* Whether fd maps the buffer's memory: a page mapped at an offset sees what
 * the CPU pointer wrote there, and the other way round. */
static bool same_memory(int fd, uint8_t *ptr, size_t size)
{
    const long pg = sysconf(_SC_PAGESIZE);
    const size_t off = (size / 2) & ~(size_t)(pg - 1);
    uint8_t *m = mmap(NULL, (size_t)pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
    if (m == MAP_FAILED) {
        return false;
    }
    ptr[off] = 0x5A;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    bool ok = m[0] == 0x5A;
    m[1] = 0xA5;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ok = ok && ptr[off + 1] == 0xA5;
    ptr[off] = ptr[off + 1] = 0;
    munmap(m, (size_t)pg);
    return ok;
}

static sigjmp_buf g_probe_jb;

static void probe_fault(int sig, siginfo_t *si, void *uc)
{
    siglongjmp(g_probe_jb, 1);
}

/* The VM's write tracking on these mappings: two maps of one page alias,
 * a write to a read-only one faults, and it writes again once unprotected.
 * In a child process, so the app's fault handlers stay as they are. */
static bool tracking_works(int fd)
{
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        const long pg = sysconf(_SC_PAGESIZE);
        volatile uint8_t *a = mmap(NULL, (size_t)pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        volatile uint8_t *b = mmap(NULL, (size_t)pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (a == MAP_FAILED || b == MAP_FAILED) {
            _exit(2);
        }
        a[8] = 0x3C;
        if (b[8] != 0x3C) {
            _exit(3);
        }
        struct sigaction sa = { 0 };
        sa.sa_sigaction = probe_fault;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        if (mprotect((void *)a, (size_t)pg, PROT_READ) != 0) {
            _exit(4);
        }
        if (sigsetjmp(g_probe_jb, 1) == 0) {
            a[8] = 0x3D;                        /* must fault */
            _exit(5);
        }
        if (mprotect((void *)a, (size_t)pg, PROT_READ | PROT_WRITE) != 0) {
            _exit(6);
        }
        a[8] = 0;
        _exit(b[8] == 0 ? 0 : 7);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        UMA_LOG("uma: RAM fd write tracking check failed (%d)",
                WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st));
        return false;
    }
    return true;
}

/* debug, once: CPU write and read rates of 16 MiB through a mapping of fd
 * (the AHB's) and of a memfd, for the log (a cached and an uncached buffer
 * differ by an order of magnitude) */
static double rate_gbs(uint8_t *p, size_t n, bool write)
{
    struct timespec t0, t1;
    volatile uint64_t sink = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < 4; r++) {
        if (write) {
            memset(p, r, n);
        } else {
            const uint64_t *q = (const uint64_t *)p;
            uint64_t s = 0;
            for (size_t i = 0; i < n / 8; i++) {
                s += q[i];
            }
            sink += s;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    (void)sink;
    return s > 0 ? 4.0 * (double)n / s / 1e9 : 0;
}

static void rate_log(int fd)
{
    const size_t n = 16u << 20;
    uint8_t *a = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int mfd = (int)syscall(__NR_memfd_create, "hakux-rate", 1u);
    uint8_t *b = mfd >= 0 && ftruncate(mfd, (off_t)n) == 0
                     ? mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0) : MAP_FAILED;
    if (a != MAP_FAILED && b != MAP_FAILED) {
        const double aw = rate_gbs(a, n, true), ar = rate_gbs(a, n, false);
        const double bw = rate_gbs(b, n, true), br = rate_gbs(b, n, false);
        memset(a, 0, n);
        UMA_LOG("uma: CPU rates, AHB write %.1f read %.1f GB/s, memfd write %.1f read %.1f GB/s",
                aw, ar, bw, br);
    }
    if (a != MAP_FAILED) {
        munmap(a, n);
    }
    if (b != MAP_FAILED) {
        munmap(b, n);
    }
    if (mfd >= 0) {
        close(mfd);
    }
}

/* debug, once: the CPU rates of 16 MiB AHBs by usage, through the lock
 * pointer and a mapping of the handle's fd (which usage gets cached memory) */
static void usage_probe(void)
{
    static const struct { const char *name; uint64_t usage; } k[] = {
        { "cpu+gpu_data", AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                          AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER },
        { "cpu", AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN },
        { "cpu+sampled", AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE },
        { "cpu+sensor", AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                        AHARDWAREBUFFER_USAGE_SENSOR_DIRECT_DATA },
    };
    const size_t n = 16u << 20;
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        AHardwareBuffer_Desc d = { .width = (uint32_t)n, .height = 1, .layers = 1,
                                   .format = AHARDWAREBUFFER_FORMAT_BLOB, .usage = k[i].usage };
        AHardwareBuffer *ahb = NULL;
        if (AHardwareBuffer_allocate(&d, &ahb) != 0) {
            UMA_LOG("uma probe %s: allocate failed", k[i].name);
            continue;
        }
        void *ptr = NULL;
        double lw = 0, lr = 0, fw = 0, fr = 0;
        if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &ptr) == 0 && ptr) {
            lw = rate_gbs(ptr, n, true);
            lr = rate_gbs(ptr, n, false);
            AHardwareBuffer_unlock(ahb, NULL);
        }
        int fds[32];
        const int nf = handle_fds(ahb, fds, 32);
        for (int j = 0; j < nf; j++) {
            if (fw == 0 && lseek(fds[j], 0, SEEK_END) >= (off_t)n) {
                uint8_t *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fds[j], 0);
                if (m != MAP_FAILED) {
                    fw = rate_gbs(m, n, true);
                    fr = rate_gbs(m, n, false);
                    munmap(m, n);
                }
            }
            close(fds[j]);
        }
        UMA_LOG("uma probe %s: lock write %.1f read %.1f GB/s, fd write %.1f read %.1f GB/s",
                k[i].name, lw, lr, fw, fr);
        AHardwareBuffer_release(ahb);
    }
}

int hle_uma_ram_fd(size_t size)
{
    static bool probed;
    if (!probed && getenv("NC_UMA_PROBE")) {
        probed = true;
        usage_probe();
    }
    if (g_uma_fd >= 0) {
        if (g_uma_size != size) {
            return -1;
        }
        /* a relaunch: the same buffer, cleared as a new memfd would be */
        void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, g_uma_fd, 0);
        if (p == MAP_FAILED) {
            return -1;
        }
        memset(p, 0, size);
        munmap(p, size);
        return dup(g_uma_fd);
    }
    if (g_uma_ahb) {
        return -1;                              /* the QEMU path's own */
    }
    /* CPU usage only: Qualcomm's gralloc gives a GPU_DATA_BUFFER buffer
     * uncached memory (CPU reads 1.6 GB/s on the RP5, the guest's every
     * load); this one is cached (25 GB/s). The GPU doesn't snoop it through
     * the import (guest_ram.c), whatever its memory type says: the barrier
     * cleans the CPU's lines over what the GPU reads or writes in place
     * (mem/barrier.c) */
    AHardwareBuffer *ahb = NULL;
    uint8_t *ptr = uma_alloc_usage(size, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                         AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, &ahb);
    if (!ptr) {
        return -1;
    }
    int fds[32], fd = -1;
    const int n = handle_fds(ahb, fds, 32);
    for (int i = 0; i < n; i++) {
        const off_t len = lseek(fds[i], 0, SEEK_END);
        if (fd < 0 && len >= (off_t)size && same_memory(fds[i], ptr, size)) {
            fd = fds[i];
        } else {
            close(fds[i]);
        }
    }
    AHardwareBuffer_unlock(ahb, NULL);
    if (fd < 0 || !tracking_works(fd)) {
        UMA_LOG("uma: no usable RAM fd in the AHB's handle (%d fds): memfd RAM, no zero-copy", n);
        if (fd >= 0) {
            close(fd);
        }
        AHardwareBuffer_release(ahb);
        return -1;
    }
    rate_log(fd);
    g_uma_ahb = ahb;
    g_uma_fd = fd;
    g_uma_size = size;
    UMA_LOG("uma: guest RAM is the AHB's memory (fd %d of %d, %zu MiB): mapped by the VM, "
            "imported by Vulkan", fd, n, size >> 20);
    return dup(fd);
}

#else

struct AHardwareBuffer *hle_uma_ahb(void)
{
    return NULL;
}

int hle_uma_ram_fd(size_t size)
{
    (void)size;
    return -1;
}

#endif

