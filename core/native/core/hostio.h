/*
 * hakuX native core - an output file that can live outside the sandbox.
 *
 * On the PS5 the app's own writes count toward a per-launch storage budget
 * (after about 1 GiB its writes drop to ~0.5-25 MB/s). A payload process
 * loaded by elfldr runs outside that sandbox and is NOT throttled (measured:
 * 123-127 MB/s past 1.5 GiB). This is the small abstraction for "write this
 * file through that process when it is up": savestates, the cache blob store,
 * RAM-cache spills and the game's own save files all go through here, so the
 * app itself writes almost nothing and never reaches the throttle.
 *
 * The platform side is weak in hostio.c (ordinary POSIX) and overridden by
 * ps5/src/wbwriter_client.c in the XPSemu build. Every failure falls back to
 * the app's own descriptor, so this can only help or be a no-op.
 */
#ifndef NATIVE_CORE_HOSTIO_H
#define NATIVE_CORE_HOSTIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NcHostOut {
    int fd;             /* the app's descriptor: used for every local write, and
                         * as the fallback should the helper fail halfway */
    int helper;         /* helper handle (>0), or 0 when the app writes itself */
    bool local_wrote;   /* a byte went through fd (normally: helper failure) */
    uint64_t off;       /* bytes written through this sink */
    char path[512];
} NcHostOut;

/* True when the un-throttled helper is up. */
bool nc_host_fast(void);

/* path opened for writing (created, truncated when truncate). 0 or -errno. */
int nc_host_out_open(NcHostOut *o, const char *path, int mode, bool truncate);

/* The whole buffer. Returns bytes written, or -1 (hostio has already retried
 * any helper failure through the local descriptor). */
long nc_host_write(NcHostOut *o, const void *p, size_t n);

/* Make the bytes durable: the helper's fsync and the app's, so a helper write
 * that reached the shared page cache is on storage even if the helper dies. */
int nc_host_out_sync(NcHostOut *o);

/* Close both ends. 0 when the app's descriptor closed cleanly. */
int nc_host_out_close(NcHostOut *o);

/* rename/unlink through the helper when it is up, else by the app. 0 or -errno. */
int nc_host_rename(const char *from, const char *to);
int nc_host_unlink(const char *path);

/* One-shot: path = p[0..n) (created/truncated), synced, closed. 0 or -errno. */
int nc_host_write_file(const char *path, const void *p, size_t n);

#endif
