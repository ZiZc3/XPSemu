/*
 * wbwriter client - XPSemu's side of the persistent write-helper.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Streams wbwriter.elf to the PS5 payload loader (elfldr, 127.0.0.1:9021) ONCE,
 * keeps the same socket, and speaks wbwriter_proto.h over it so the core's
 * write-behind thread can hand cache-partition writes to a process that runs
 * outside the sandbox and is NOT write-throttled. Every call is fail-soft: if
 * elfldr isn't there, an import isn't bound, or the socket errors at any point,
 * the helper is marked unavailable and the core falls back to its own in-process
 * pwrite (today's behaviour) - so this can only help or be a no-op.
 *
 * The elfldr socket plumbing is our own (from the removed xpsemu_system
 * experiment). The keep-the-socket-and-speak-a-protocol design is adapted from
 * ProsperoEden (BlackBearReloaded), GPL-3.0-or-later. The sceNet* imports are
 * re-added here, each checked bound before first use.
 */
#include "xog_title.h"
#include "wbwriter_proto.h"
#include "native/core/hostio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void nc_log(const char *tag, const char *fmt, ...);

/* libSceNet: reaches the loopback payload loader from inside the sandbox. Each
 * is checked bound (import_bound) before the helper is first used. */
int sceNetSocket(const char *name, int domain, int type, int protocol);
int sceNetSocketClose(int s);
int sceNetConnect(int s, const void *addr, unsigned addrlen);
int sceNetSend(int s, const void *buf, size_t len, int flags);
int sceNetRecv(int s, void *buf, size_t len, int flags);
int sceNetSetsockopt(int s, int level, int opt, const void *val, unsigned len);
int sceKernelQueryMemoryProtection(void *addr, void **start, void **end, uint32_t *prot);

/* ---- import guard (the same GOT-slot test main_ps5.c uses for its imports) ---- */
static int import_bound(const char *name)
{
    FILE *f = fopen("/data/homebrew/" XOG_TITLE "/imports.txt", "r");
    if (!f) {
        return -1;
    }
    char have[256];
    unsigned long long off;
    int result = -1;
    while (fscanf(f, "%llx %255s", &off, have) == 2) {
        if (strcmp(have, name) != 0) {
            continue;
        }
        const uint64_t *slot = (const uint64_t *)(uintptr_t)(off + 0x400000ull);
        void *rs = 0, *re = 0;
        uint32_t rp = 0;
        if (sceKernelQueryMemoryProtection((void *)slot, &rs, &re, &rp) != 0 ||
            (uintptr_t)rs > (uintptr_t)slot || (uintptr_t)re <= (uintptr_t)slot || !(rp & 1)) {
            result = -1;
        } else {
            result = *slot != 0 ? 1 : 0;
        }
        break;
    }
    fclose(f);
    return result;
}

static int net_imports_ok(void)
{
    static const char *const needed[] = {"sceNetSocket", "sceNetSocketClose", "sceNetConnect",
                                         "sceNetSend",   "sceNetRecv",        "sceNetSetsockopt"};
    for (unsigned i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
        if (import_bound(needed[i]) != 1) {
            nc_log("hakuX-native-io", "wbwriter: import %s not bound; writes stay in-process", needed[i]);
            return 0;
        }
    }
    return 1;
}

/* ---- connection state ---- */
struct NetSockaddrIn {
    uint8_t len;
    uint8_t family;
    uint16_t port; /* network order */
    uint32_t addr; /* network order */
    uint16_t vport;
    uint8_t zero[6];
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_sock = -1;
static int g_active;  /* 1 while the helper is usable */
static int g_tried;   /* we attempt the load once per run */
static uint32_t g_seq;

static void mark_dead(const char *why)
{
    if (g_active) {
        nc_log("hakuX-native-io", "wbwriter: helper lost (%s); writes fall back to in-process", why);
    }
    g_active = 0;
    if (g_sock >= 0) {
        sceNetSocketClose(g_sock);
        g_sock = -1;
    }
}

static int send_all(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    while (n) {
        const int put = sceNetSend(g_sock, p, n, 0);
        if (put <= 0 || (size_t)put > n) {
            return 0;
        }
        p += put;
        n -= (size_t)put;
    }
    return 1;
}

static int recv_all(void *data, size_t n)
{
    uint8_t *p = (uint8_t *)data;
    while (n) {
        const int got = sceNetRecv(g_sock, p, n, 0);
        if (got <= 0 || (size_t)got > n) {
            return 0;
        }
        p += got;
        n -= (size_t)got;
    }
    return 1;
}

/* Stream wbwriter.elf to elfldr and keep the socket. g_mu held. */
static int load_helper(void)
{
    if (!net_imports_ok()) {
        return 0;
    }
    static const char *const paths[] = {"/app0/wbwriter.elf", "/data/homebrew/" XOG_TITLE "/wbwriter.elf"};
    int fd = -1;
    for (unsigned i = 0; fd < 0 && i < sizeof(paths) / sizeof(paths[0]); i++) {
        fd = open(paths[i], O_RDONLY);
    }
    if (fd < 0) {
        nc_log("hakuX-native-io", "wbwriter: wbwriter.elf not found (%s); writes stay in-process", strerror(errno));
        return 0;
    }
    const int s = sceNetSocket("wbwriter", 2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 6 /*TCP*/);
    if (s < 0) {
        close(fd);
        return 0;
    }
    const int io_tmo = 30000000;     /* 30 s: a WRITE's pwrite+ack */
    const int conn_tmo = 2000000;    /* 2 s connect: at most a one-time hitch if no elfldr */
    sceNetSetsockopt(s, 0xffff, 0x1105, &io_tmo, sizeof(io_tmo));   /* recv timeout */
    sceNetSetsockopt(s, 0xffff, 0x1106, &io_tmo, sizeof(io_tmo));   /* send timeout */
    sceNetSetsockopt(s, 0xffff, 0x1109, &conn_tmo, sizeof(conn_tmo));
    const uint16_t port = WBW_LOADER_PORT;
    const struct NetSockaddrIn addr = {sizeof(struct NetSockaddrIn), 2,
                                       (uint16_t)((port << 8) | (port >> 8)), 0x0100007f, 0, {0}};
    if (sceNetConnect(s, &addr, sizeof(addr)) < 0) {
        close(fd);
        sceNetSocketClose(s);
        nc_log("hakuX-native-io", "wbwriter: no payload loader on 9021 (elfldr not running); writes stay in-process");
        return 0;
    }
    g_sock = s;
    /* Kill Nagle. Every request ends with the helper's 16-byte reply, which we
     * then block on; with Nagle + delayed-ACK those tiny replies stall ~40-200 ms
     * on loopback (the 100 ms/1 KiB writes seen in the logs). TCP_NODELAY makes
     * each reply go out at once, so the un-throttled helper actually runs fast. */
    {
        const int one = 1;
        sceNetSetsockopt(s, 6 /*IPPROTO_TCP*/, 1 /*TCP_NODELAY*/, &one, sizeof(one));
    }
    /* stream the ELF; elfldr reads up to its section table and hands us the rest */
    uint8_t buf[4096];
    ssize_t n;
    int ok = 1;
    size_t sent = 0;
    while (ok && (n = read(fd, buf, sizeof(buf))) > 0) {
        ok = send_all(buf, (size_t)n);
        sent += (size_t)n;
    }
    close(fd);
    if (!ok || n < 0) {
        mark_dead("streaming the helper broke off");
        return 0;
    }
    /* handshake: we speak first (as ProsperoEden's app does), the helper echoes */
    WbwReq req;
    memset(&req, 0, sizeof(req));
    req.magic = WBW_MAGIC;
    req.op = WBW_HELLO;
    req.seq = ++g_seq;
    WbwRep rep;
    if (!send_all(&req, sizeof(req)) || !recv_all(&rep, sizeof(rep)) || rep.magic != WBW_MAGIC ||
        rep.op != WBW_HELLO || rep.seq != req.seq) {
        mark_dead("no handshake");
        return 0;
    }
    g_active = 1;
    nc_log("hakuX-native-io",
           "wbwriter: helper up (%zu bytes streamed, protocol v%u); cache, save, state and blob writes "
           "now go to an un-throttled process",
           sent, (unsigned)rep.result);
    return 1;
}

/* One request/reply. g_mu held. payload p1(n1) then p2(n2) follow the header.
 * Returns the reply status (0 ok, negative -errno), or -1000 on a dead link. */
static int transact(uint16_t op, uint32_t handle, uint64_t off, const void *p1, uint32_t n1,
                    const void *p2, uint32_t n2, uint32_t *result)
{
    if (!g_active) {
        return -1000;
    }
    WbwReq req;
    req.magic = WBW_MAGIC;
    req.op = op;
    req.flags = 0;
    req.seq = ++g_seq;
    req.handle = handle;
    req.off = off;
    req.dlen = n1;
    req.dlen2 = n2;
    if (!send_all(&req, sizeof(req)) || (n1 && !send_all(p1, n1)) || (n2 && !send_all(p2, n2))) {
        mark_dead("send failed");
        return -1000;
    }
    WbwRep rep;
    if (!recv_all(&rep, sizeof(rep)) || rep.magic != WBW_MAGIC || rep.seq != req.seq || rep.op != op) {
        mark_dead("reply lost or out of order");
        return -1000;
    }
    if (result) {
        *result = rep.result;
    }
    return rep.status;
}

/* ---- public API (called from the core's io.c) ---- */

/* 1 if the helper is usable; loads it once on the first call. */
int wbwr_available(void)
{
    int active;
    pthread_mutex_lock(&g_mu);
    if (!g_tried) {
        g_tried = 1;
        (void)load_helper();
    }
    active = g_active;
    pthread_mutex_unlock(&g_mu);
    return active;
}

/* Open host for writing on the helper; returns a handle (>=1) or 0 on failure. */
int wbwr_open(const char *host)
{
    const size_t len = strlen(host);
    if (len == 0 || len > WBW_MAX_PATH) {
        return 0;
    }
    uint32_t result = 0;
    pthread_mutex_lock(&g_mu);
    const int st = transact(WBW_OPEN, 0, 0, host, (uint32_t)len, NULL, 0, &result);
    pthread_mutex_unlock(&g_mu);
    return st == 0 ? (int)result : 0;
}

/* Write len bytes at off through handle. Returns bytes written, or -1 (the
 * caller then writes it in-process; order is kept because the writer thread is
 * serial and a barrier drains the helper before any read). */
long wbwr_write(int handle, uint64_t off, const void *data, uint32_t len)
{
    if (handle <= 0) {
        return -1;
    }
    const uint8_t *p = (const uint8_t *)data;
    uint64_t done = 0;
    pthread_mutex_lock(&g_mu);
    while (done < len) {
        const uint32_t chunk = (uint32_t)(len - done > WBW_CHUNK ? WBW_CHUNK : len - done);
        uint32_t wrote = 0;
        const int st = transact(WBW_WRITE, (uint32_t)handle, off + done, p + done, chunk, NULL, 0, &wrote);
        if (st != 0 || wrote != chunk) {
            pthread_mutex_unlock(&g_mu);
            return -1;
        }
        done += chunk;
    }
    pthread_mutex_unlock(&g_mu);
    return (long)done;
}

int wbwr_fsync(int handle)
{
    if (handle <= 0) {
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    const int st = transact(WBW_FSYNC, (uint32_t)handle, 0, NULL, 0, NULL, 0, NULL);
    pthread_mutex_unlock(&g_mu);
    return st == 0 ? 0 : -1;
}

int wbwr_truncate(int handle, uint64_t size)
{
    if (handle <= 0) {
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    const int st = transact(WBW_TRUNCATE, (uint32_t)handle, size, NULL, 0, NULL, 0, NULL);
    pthread_mutex_unlock(&g_mu);
    return st == 0 ? 0 : -1;
}

void wbwr_close(int handle)
{
    if (handle <= 0) {
        return;
    }
    pthread_mutex_lock(&g_mu);
    (void)transact(WBW_CLOSE, (uint32_t)handle, 0, NULL, 0, NULL, 0, NULL);
    pthread_mutex_unlock(&g_mu);
}

/* XOG patch 308: state files are written to a temporary name and renamed,
 * and a blob replaced only after its bytes are stored. Both go through the
 * helper so even the metadata step is outside the app's write budget. */
int wbwr_rename(const char *from, const char *to)
{
    const size_t n1 = from ? strlen(from) : 0, n2 = to ? strlen(to) : 0;
    if (!n1 || !n2 || n1 > WBW_MAX_PATH || n2 > WBW_MAX_PATH) {
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    const int st = transact(WBW_RENAME, 0, 0, from, (uint32_t)n1, to, (uint32_t)n2, NULL);
    pthread_mutex_unlock(&g_mu);
    return st == 0 ? 0 : -1;
}

int wbwr_delete(const char *path)
{
    const size_t n = path ? strlen(path) : 0;
    if (!n || n > WBW_MAX_PATH) {
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    const int st = transact(WBW_DELETE, 0, 0, path, (uint32_t)n, NULL, 0, NULL);
    pthread_mutex_unlock(&g_mu);
    return st == 0 ? 0 : -1;
}

/* Tell the helper to exit (XPSemu shutting down). Best-effort. */
void wbwr_quit(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_active) {
        (void)transact(WBW_QUIT, 0, 0, NULL, 0, NULL, 0, NULL);
        mark_dead("quit");
    }
    pthread_mutex_unlock(&g_mu);
}

/* ---- platform side of native/core/hostio.c (patch 308) ----
 * Strong definitions: the weak defaults in the core use the app's own
 * descriptors. Anything that has a host path can then be written through the
 * un-throttled process - savestates, cache blobs, RAM-cache spills, saves. */

bool nc_host_platform_fast(void)
{
    return wbwr_available() != 0;
}

int nc_host_platform_open(const char *path)
{
    return wbwr_open(path);
}

long nc_host_platform_write(int handle, uint64_t off, const void *p, size_t n)
{
    return wbwr_write(handle, off, p, (uint32_t)n);
}

int nc_host_platform_sync(int handle)
{
    return wbwr_fsync(handle);
}

void nc_host_platform_close(int handle)
{
    wbwr_close(handle);
}

int nc_host_platform_truncate(int handle, uint64_t size)
{
    return wbwr_truncate(handle, size);
}

int nc_host_platform_rename(const char *from, const char *to)
{
    return wbwr_rename(from, to);
}

int nc_host_platform_unlink(const char *path)
{
    return wbwr_delete(path);
}
