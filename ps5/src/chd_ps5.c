/*
 * XPSemu: CHD disc images (MAME's Compressed Hunks of Data, as chdman createdvd makes them from an
 * XISO or a full disc), read through libchdr (BSD-3-Clause, deps/libchdr). The image's bytes come
 * hunk by hunk; the hunk last read is kept, and libchdr keeps its own. Read through the same handle
 * the CCI reader gives out (cci_ps5.c sends .chd here).
 * Our own code. GPL-3.0-or-later.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <libchdr/chd.h>

extern void nc_log(const char *tag, const char *fmt, ...);

typedef struct Chd {
    int key;                /* a descriptor of the file: what callers hold */
    chd_file *chd;
    uint32_t hunk_bytes, hunks;
    uint64_t size;
    uint8_t *hunk;          /* the hunk last read */
    uint32_t hunk_at;       /* which one, or UINT32_MAX */
    pthread_mutex_t lock;
} Chd;

#define CHD_MAX 4
static Chd *g_chd[CHD_MAX];

bool rx_chd_name(const char *path)
{
    const size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".chd");
}

/* The handle (a descriptor), or -1. */
int rx_chd_open(const char *path)
{
    Chd *c = calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }
    chd_error err = chd_open(path, CHD_OPEN_READ, NULL, &c->chd);
    if (err != CHDERR_NONE) {
        nc_log("xog-chd", "%s: %s", path, chd_error_string(err));
        free(c);
        return -1;
    }
    const chd_header *h = chd_get_header(c->chd);
    c->hunk_bytes = h->hunkbytes;
    c->hunks = h->totalhunks;
    c->size = h->logicalbytes;
    c->hunk = c->hunk_bytes ? malloc(c->hunk_bytes) : NULL;
    c->hunk_at = UINT32_MAX;
    c->key = open(path, O_RDONLY);       /* only a number nobody else holds */
    if (!c->hunk || c->key < 0 || !c->size) {
        nc_log("xog-chd", "%s: hunk %u bytes, %llu bytes in all: not usable", path, c->hunk_bytes,
               (unsigned long long)c->size);
        if (c->key >= 0) {
            close(c->key);
        }
        chd_close(c->chd);
        free(c->hunk);
        free(c);
        return -1;
    }
    pthread_mutex_init(&c->lock, NULL);
    for (unsigned i = 0; i < CHD_MAX; i++) {
        if (!g_chd[i]) {
            g_chd[i] = c;
            nc_log("xog-chd", "%s: v%u, %u hunks of %u bytes, %llu bytes", path, h->version, c->hunks,
                   c->hunk_bytes, (unsigned long long)c->size);
            return c->key;
        }
    }
    close(c->key);
    chd_close(c->chd);
    free(c->hunk);
    free(c);
    return -1;
}

static Chd *find(int fd)
{
    for (unsigned i = 0; i < CHD_MAX; i++) {
        if (g_chd[i] && g_chd[i]->key == fd) {
            return g_chd[i];
        }
    }
    return NULL;
}

bool rx_chd_is(int fd)
{
    return fd >= 0 && find(fd) != NULL;
}

void rx_chd_close(int fd)
{
    for (unsigned i = 0; i < CHD_MAX; i++) {
        Chd *c = g_chd[i];
        if (c && c->key == fd) {
            g_chd[i] = NULL;
            close(c->key);
            chd_close(c->chd);
            pthread_mutex_destroy(&c->lock);
            free(c->hunk);
            free(c);
            return;
        }
    }
}

bool rx_chd_read(int fd, void *buf, size_t len, uint64_t off)
{
    Chd *c = find(fd);
    if (!c || off > c->size || len > c->size - off) {
        return false;
    }
    uint8_t *to = buf;
    bool ok = true;
    pthread_mutex_lock(&c->lock);
    while (len && ok) {
        const uint32_t n = (uint32_t)(off / c->hunk_bytes);
        const size_t skip = (size_t)(off % c->hunk_bytes);
        const size_t take = len < c->hunk_bytes - skip ? len : c->hunk_bytes - skip;
        if (skip == 0 && take == c->hunk_bytes && n != c->hunk_at) {
            /* a whole hunk: straight into the caller's buffer */
            const chd_error err = chd_read(c->chd, n, to);
            if (err != CHDERR_NONE) {
                nc_log("xog-chd", "hunk %u: %s", n, chd_error_string(err));
                ok = false;
                break;
            }
        } else {
            if (n != c->hunk_at) {
                const chd_error err = chd_read(c->chd, n, c->hunk);
                if (err != CHDERR_NONE) {
                    nc_log("xog-chd", "hunk %u: %s", n, chd_error_string(err));
                    c->hunk_at = UINT32_MAX;
                    ok = false;
                    break;
                }
                c->hunk_at = n;
            }
            memcpy(to, c->hunk + skip, take);
        }
        to += take;
        off += take;
        len -= take;
    }
    pthread_mutex_unlock(&c->lock);
    return ok;
}
