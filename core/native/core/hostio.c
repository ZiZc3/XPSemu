/*
 * hakuX native core - output files, locally or through the PS5 write-helper.
 *
 * The platform hooks below are weak here so the core keeps building for every
 * target; the XPSemu layer (ps5/src/wbwriter_client.c) supplies strong ones
 * that speak to the payload process outside the sandbox. Nothing else in the
 * core needs to know whether a write went to the app's descriptor or to that
 * process: every path keeps the same file contents and ordering.
 */
#include "native/core/hostio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void nc_log(const char *tag, const char *fmt, ...);

/* One line, once a run, so the log shows patch 308 actually took the fast
 * path (and stays quiet afterwards). */
static void told_fast(void)
{
    static int told;
    if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED)) {
        nc_log("hakuX-native-io",
               "hostio: state, cache-blob, RAM-spill and save writes go through the un-throttled helper "
               "(patch 308)");
    }
}

/* ---- platform side (weak: absent everywhere but XPSemu on the PS5) ---- */

bool nc_host_platform_fast(void) __attribute__((weak));
int nc_host_platform_open(const char *path) __attribute__((weak));
long nc_host_platform_write(int handle, uint64_t off, const void *p, size_t n) __attribute__((weak));
int nc_host_platform_sync(int handle) __attribute__((weak));
void nc_host_platform_close(int handle) __attribute__((weak));
int nc_host_platform_truncate(int handle, uint64_t size) __attribute__((weak));
int nc_host_platform_rename(const char *from, const char *to) __attribute__((weak));
int nc_host_platform_unlink(const char *path) __attribute__((weak));

bool nc_host_fast(void)
{
    return nc_host_platform_fast && nc_host_platform_fast();
}

static int local_open(const char *path, int mode, bool truncate)
{
    const int flags = O_WRONLY | O_CREAT | (truncate ? O_TRUNC : 0) | O_CLOEXEC;
    const int fd = open(path, flags, mode);
    return fd >= 0 ? fd : -errno;
}

int nc_host_out_open(NcHostOut *o, const char *path, int mode, bool truncate)
{
    memset(o, 0, sizeof(*o));
    o->fd = -1;
    o->helper = 0;
    if (!path || !path[0]) {
        return -EINVAL;
    }
    /* A path past the helper's limit (or our own) is fine: the helper open
     * fails and the app's descriptor takes it, as it always did. */
    if (strlen(path) < sizeof(o->path)) {
        snprintf(o->path, sizeof(o->path), "%s", path);
    }

    if (nc_host_platform_fast && nc_host_platform_fast() && nc_host_platform_open) {
        const int h = nc_host_platform_open(path);
        if (h <= 0) {
            static int told_open;
            if (!__atomic_exchange_n(&told_open, 1, __ATOMIC_RELAXED)) {
                nc_log("hakuX-native-io", "hostio: helper could not open %s; writes stay in-process", path);
            }
        }
        if (h > 0 && (!truncate || (nc_host_platform_truncate && nc_host_platform_truncate(h, 0) == 0))) {
            /* Keep the app's own descriptor too: it is the fallback if the
             * helper breaks off midway, and its fsync makes the helper's
             * writes durable (same inode, same page cache). No O_TRUNC here:
             * the helper already truncated, and truncating again would drop
             * what it has written when open happens before the first write. */
            const int fd = local_open(path, mode, false);
            o->fd = fd >= 0 ? fd : -1;
            o->helper = h;
            told_fast();
            return 0;
        }
        if (h > 0 && nc_host_platform_close) {
            nc_host_platform_close(h);      /* truncate failed: use the app */
        }
    }

    const int fd = local_open(path, mode, truncate);
    if (fd < 0) {
        return fd;
    }
    o->fd = fd;
    return 0;
}

long nc_host_write(NcHostOut *o, const void *p, size_t n)
{
    const uint8_t *q = p;
    size_t done = 0;
    while (done < n) {
        size_t chunk = n - done;
        if (chunk > (1u << 20)) {
            chunk = 1u << 20;
        }
        if (o->helper > 0 && nc_host_platform_write) {
            const long w = nc_host_platform_write(o->helper, o->off, q + done, chunk);
            if (w == (long)chunk) {
                done += chunk;
                o->off += chunk;
                continue;
            }
            /* The helper broke off: finish this chunk and the rest through
             * the app's descriptor. Ordering is kept because every earlier
             * byte was acked before we got here. */
            static int told_write;
            if (!__atomic_exchange_n(&told_write, 1, __ATOMIC_RELAXED)) {
                nc_log("hakuX-native-io", "hostio: helper write failed at +%llu; finishing in-process",
                       (unsigned long long)o->off);
            }
            o->helper = 0;
        }
        if (o->fd < 0) {
            return -1;
        }
        o->local_wrote = true;
        size_t put = 0;
        while (put < chunk) {
            const ssize_t w = pwrite(o->fd, q + done + put, chunk - put, (off_t)(o->off + put));
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                return done ? (long)done : -1;
            }
            put += (size_t)w;
        }
        done += chunk;
        o->off += chunk;
    }
    return (long)done;
}

int nc_host_out_sync(NcHostOut *o)
{
    int rc = 0;
    if (o->helper > 0 && nc_host_platform_sync) {
        rc = nc_host_platform_sync(o->helper);
    }
    /* Only the app's own descriptor when some byte actually went through it.
     * A file written wholly by the helper was already fsynced there; syncing
     * the app's descriptor too is charged to the app and can still be
     * throttled (a 49 MiB state took 16 s that way). */
    if (o->fd >= 0 && o->local_wrote && fsync(o->fd) != 0 && rc == 0) {
        rc = -errno;
    }
    return rc;
}

int nc_host_out_close(NcHostOut *o)
{
    int rc = 0;
    if (o->helper > 0 && nc_host_platform_close) {
        nc_host_platform_close(o->helper);
    }
    o->helper = 0;
    if (o->fd >= 0 && close(o->fd) != 0) {
        rc = -errno;
    }
    o->fd = -1;
    return rc;
}

int nc_host_rename(const char *from, const char *to)
{
    if (nc_host_platform_fast && nc_host_platform_fast() && nc_host_platform_rename &&
        nc_host_platform_rename(from, to) == 0) {
        return 0;
    }
    return rename(from, to) == 0 ? 0 : -errno;
}

int nc_host_unlink(const char *path)
{
    if (nc_host_platform_fast && nc_host_platform_fast() && nc_host_platform_unlink &&
        nc_host_platform_unlink(path) == 0) {
        return 0;
    }
    return unlink(path) == 0 ? 0 : -errno;
}

int nc_host_write_file(const char *path, const void *p, size_t n)
{
    NcHostOut o;
    int rc = nc_host_out_open(&o, path, 0644, true);
    if (rc != 0) {
        return rc;
    }
    const long w = nc_host_write(&o, p, n);
    if (w != (long)n) {
        nc_host_out_close(&o);
        return -EIO;
    }
    if ((rc = nc_host_out_sync(&o)) != 0) {
        nc_host_out_close(&o);
        return rc;
    }
    return nc_host_out_close(&o);
}
