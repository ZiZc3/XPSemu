/*
 * wbwriter - XPSemu's persistent write-helper (payload, outside the sandbox).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XPSemu streams this program to the PS5 payload loader (elfldr, loopback port
 * 9021) ONCE at start-up. The loader runs it in its own process - outside the
 * app's sandbox, where the console does NOT throttle file writes (confirmed on
 * the console: a payload process held ~123-127 MB/s past 1 GB while the app was
 * throttled to ~25 MB/s) - and hands it the rest of that connection as its
 * standard input and output. We read request frames from stdin and write reply
 * frames to stdout, owning the file descriptors and doing the pwrite/fsync here
 * so the emulator never blocks on throttled storage. We stay alive until the
 * app disconnects or sends QUIT.
 *
 * The loader-hands-the-socket-as-stdin/stdout mechanism and the stay-alive
 * design are adapted from ProsperoEden (BlackBearReloaded), GPL-3.0-or-later
 * (headless/self_update_helper/main.cpp + updater.cpp). Our protocol is binary
 * (see wbwriter_proto.h) rather than its text lines.
 *
 * Built with the PS5 payload SDK (prospero-clang); see tools/build-wbwriter.sh.
 * Must end exactly at its section-header table: elfldr reads the ELF up to the
 * end of that table and gives the rest of the stream to us, so trailing bytes
 * would corrupt the first frames.
 */
#include "wbwriter_proto.h"

#include <ps5/klog.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAXH 1024 /* open file descriptors we can hold at once */

static int g_fd[MAXH];

static int read_exact(void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        const ssize_t r = read(STDIN_FILENO, p, n);
        if (r <= 0) {
            return 0; /* app gone or error */
        }
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static int write_exact(const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        const ssize_t w = write(STDOUT_FILENO, p, n);
        if (w <= 0) {
            return 0;
        }
        p += w;
        n -= (size_t)w;
    }
    return 1;
}

/* clamp errno into the int16 status field (negative = -errno). */
static int16_t err_status(void)
{
    int e = errno;
    if (e <= 0) {
        e = EIO;
    }
    if (e > 32000) {
        e = 32000;
    }
    return (int16_t)(-e);
}

static int reply(uint16_t op, int16_t status, uint32_t seq, uint32_t result)
{
    WbwRep rep;
    rep.magic = WBW_MAGIC;
    rep.op = op;
    rep.status = status;
    rep.seq = seq;
    rep.result = result;
    return write_exact(&rep, sizeof(rep));
}

/* a valid, open handle's fd, or -1. */
static int handle_fd(uint32_t handle)
{
    if (handle == 0 || handle > MAXH) {
        return -1;
    }
    return g_fd[handle - 1];
}

int main(void)
{
    static uint8_t data[WBW_CHUNK];
    static char path[WBW_MAX_PATH + 1];
    static char path2[WBW_MAX_PATH + 1];

    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAXH; i++) {
        g_fd[i] = -1;
    }
    klog_printf("[wbwriter] up, waiting for XPSemu's write stream\n");

    for (;;) {
        WbwReq req;
        if (!read_exact(&req, sizeof(req)) || req.magic != WBW_MAGIC) {
            break; /* disconnected or out of sync: exit, app falls back to local writes */
        }

        /* Read the payload first, always, so the stream stays framed even on a
         * rejected request. Reject (and disconnect) only a payload too large to
         * hold - that can only be corruption. */
        if (req.op == WBW_WRITE) {
            if (req.dlen > WBW_CHUNK) {
                break;
            }
            if (req.dlen && !read_exact(data, req.dlen)) {
                break;
            }
        } else {
            if (req.dlen > WBW_MAX_PATH || req.dlen2 > WBW_MAX_PATH) {
                break;
            }
            if (req.dlen && !read_exact(path, req.dlen)) {
                break;
            }
            path[req.dlen] = 0;
            if (req.dlen2 && !read_exact(path2, req.dlen2)) {
                break;
            }
            path2[req.dlen2] = 0;
        }

        int ok = 1;
        switch (req.op) {
        case WBW_HELLO:
            ok = reply(WBW_HELLO, 0, req.seq, WBW_PROTO_VERSION);
            break;

        case WBW_OPEN: {
            int slot = -1;
            for (int i = 0; i < MAXH; i++) {
                if (g_fd[i] < 0) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                errno = EMFILE;
                ok = reply(WBW_OPEN, err_status(), req.seq, 0);
                break;
            }
            const int fd = open(path, O_RDWR | O_CREAT, 0666);
            if (fd < 0) {
                ok = reply(WBW_OPEN, err_status(), req.seq, 0);
                break;
            }
            g_fd[slot] = fd;
            ok = reply(WBW_OPEN, 0, req.seq, (uint32_t)(slot + 1));
            break;
        }

        case WBW_WRITE: {
            const int fd = handle_fd(req.handle);
            if (fd < 0) {
                errno = EBADF;
                ok = reply(WBW_WRITE, err_status(), req.seq, 0);
                break;
            }
            uint32_t put = 0;
            int werr = 0;
            while (put < req.dlen) {
                const ssize_t w = pwrite(fd, data + put, req.dlen - put, (off_t)(req.off + put));
                if (w <= 0) {
                    werr = 1;
                    break;
                }
                put += (uint32_t)w;
            }
            ok = reply(WBW_WRITE, werr ? err_status() : 0, req.seq, put);
            break;
        }

        case WBW_FSYNC: {
            const int fd = handle_fd(req.handle);
            if (fd < 0) {
                errno = EBADF;
                ok = reply(WBW_FSYNC, err_status(), req.seq, 0);
                break;
            }
            ok = reply(WBW_FSYNC, fsync(fd) == 0 ? 0 : err_status(), req.seq, 0);
            break;
        }

        case WBW_TRUNCATE: {
            const int fd = handle_fd(req.handle);
            if (fd < 0) {
                errno = EBADF;
                ok = reply(WBW_TRUNCATE, err_status(), req.seq, 0);
                break;
            }
            ok = reply(WBW_TRUNCATE, ftruncate(fd, (off_t)req.off) == 0 ? 0 : err_status(), req.seq, 0);
            break;
        }

        case WBW_CLOSE: {
            const int fd = handle_fd(req.handle);
            if (fd < 0) {
                ok = reply(WBW_CLOSE, 0, req.seq, 0); /* already gone: fine */
                break;
            }
            const int rc = close(fd);
            g_fd[req.handle - 1] = -1;
            ok = reply(WBW_CLOSE, rc == 0 ? 0 : err_status(), req.seq, 0);
            break;
        }

        case WBW_DELETE:
            ok = reply(WBW_DELETE, unlink(path) == 0 ? 0 : err_status(), req.seq, 0);
            break;

        case WBW_RENAME:
            ok = reply(WBW_RENAME, rename(path, path2) == 0 ? 0 : err_status(), req.seq, 0);
            break;

        case WBW_QUIT:
            (void)reply(WBW_QUIT, 0, req.seq, 0);
            ok = 0; /* leave the loop */
            break;

        default:
            errno = ENOSYS;
            ok = reply((uint16_t)req.op, err_status(), req.seq, 0);
            break;
        }

        if (!ok) {
            break;
        }
    }

    for (int i = 0; i < MAXH; i++) {
        if (g_fd[i] >= 0) {
            (void)fsync(g_fd[i]);
            close(g_fd[i]);
        }
    }
    klog_printf("[wbwriter] exiting (app disconnected or quit)\n");
    return 0;
}
