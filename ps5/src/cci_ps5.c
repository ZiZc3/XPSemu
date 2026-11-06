/*
 * XOG: CCI disc images: an ISO in 2 KiB blocks, each LZ4 or as it was, with an index. One file, or
 * parts named name.1.cci, name.2.cci, ... Read through a handle the rest takes for a file's.
 * Our own code. GPL-3.0-or-later.
 */
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CCI_BLOCK 2048u
#define CCI_PARTS 8

typedef struct CciPart {
    int fd;
    uint64_t start, size;   /* in the image */
    uint8_t shift;
    uint32_t *index;        /* blocks + 1 entries */
} CciPart;

typedef struct Cci {
    int key;                /* the first part's descriptor: what callers hold */
    unsigned nparts;
    CciPart part[CCI_PARTS];
    uint64_t size;
} Cci;

#define CCI_MAX 4
static Cci *g_open[CCI_MAX];

static bool read_at(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    while (len) {
        const ssize_t n = pread(fd, p, len, (off_t)off);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return true;
}

static bool part_open(Cci *c, int fd)
{
    uint8_t h[32];
    if (c->nparts >= CCI_PARTS || !read_at(fd, h, sizeof(h), 0) || memcmp(h, "CCIM", 4)) {
        return false;
    }
    uint64_t size, index_at;
    uint32_t block;
    memcpy(&size, h + 8, 8);
    memcpy(&index_at, h + 16, 8);
    memcpy(&block, h + 24, 4);
    if (block != CCI_BLOCK || !size || size % CCI_BLOCK) {
        return false;
    }
    const uint64_t entries = size / CCI_BLOCK + 1;
    uint32_t *index = malloc(entries * 4);
    if (!index || !read_at(fd, index, entries * 4, index_at)) {
        free(index);
        return false;
    }
    CciPart *p = &c->part[c->nparts++];
    p->fd = fd;
    p->start = c->size;
    p->size = size;
    p->shift = h[29];
    p->index = index;
    c->size += size;
    return true;
}

/* A name that is a part after the first: the listing shows only the first. */
bool rx_cci_is_later_part(const char *path)
{
    const size_t n = strlen(path);
    return n > 6 && !strcasecmp(path + n - 4, ".cci") && path[n - 6] == '.' && path[n - 5] > '1' && path[n - 5] <= '9';
}

bool rx_cci_name(const char *path)
{
    const size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".cci");
}

/* The handle (a descriptor), or -1. */
int rx_cci_open(const char *path)
{
    Cci *c = calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }
    const int fd = open(path, O_RDONLY);
    if (fd < 0 || !part_open(c, fd)) {
        if (fd >= 0) {
            close(fd);
        }
        free(c);
        return -1;
    }
    c->key = fd;
    /* name.1.cci: its other parts */
    const size_t n = strlen(path);
    if (n > 6 && path[n - 6] == '.' && path[n - 5] == '1') {
        char *next = strdup(path);
        for (char d = '2'; next && d <= '9'; d++) {
            next[n - 5] = d;
            const int more = open(next, O_RDONLY);
            if (more < 0) {
                break;
            }
            if (!part_open(c, more)) {
                close(more);
                break;
            }
        }
        free(next);
    }
    for (unsigned i = 0; i < CCI_MAX; i++) {
        if (!g_open[i]) {
            g_open[i] = c;
            return fd;
        }
    }
    close(fd);
    free(c);
    return -1;
}

static Cci *find(int fd)
{
    for (unsigned i = 0; i < CCI_MAX; i++) {
        if (g_open[i] && g_open[i]->key == fd) {
            return g_open[i];
        }
    }
    return NULL;
}

bool rx_cci_is(int fd)
{
    return find(fd) != NULL;
}

void rx_cci_close(int fd)
{
    for (unsigned i = 0; i < CCI_MAX; i++) {
        Cci *c = g_open[i];
        if (c && c->key == fd) {
            for (unsigned p = 0; p < c->nparts; p++) {
                close(c->part[p].fd);
                free(c->part[p].index);
            }
            g_open[i] = NULL;
            free(c);
        }
    }
}

static bool lz4_block(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    size_t s = 0, d = 0;
    while (s < n) {
        const uint8_t token = src[s++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (s >= n) {
                    return false;
                }
                b = src[s++];
                lit += b;
            } while (b == 255);
        }
        if (s + lit > n || d + lit > cap) {
            return false;
        }
        memcpy(dst + d, src + s, lit);
        s += lit;
        d += lit;
        if (s >= n) {
            break;      /* the last sequence has no match */
        }
        if (s + 2 > n) {
            return false;
        }
        const size_t off = src[s] | (size_t)src[s + 1] << 8;
        s += 2;
        size_t len = (token & 15) + 4u;
        if ((token & 15) == 15) {
            uint8_t b;
            do {
                if (s >= n) {
                    return false;
                }
                b = src[s++];
                len += b;
            } while (b == 255);
        }
        if (!off || off > d || d + len > cap) {
            return false;
        }
        for (size_t i = 0; i < len; i++, d++) {     /* it may overlap itself */
            dst[d] = dst[d - off];
        }
    }
    return d == cap;
}

static bool block_read(const CciPart *p, uint64_t block, uint8_t *out)
{
    const uint32_t e0 = p->index[block], e1 = p->index[block + 1];
    const uint64_t at = (uint64_t)(e0 & 0x7FFFFFFFu) << p->shift, end = (uint64_t)(e1 & 0x7FFFFFFFu) << p->shift;
    if (end <= at || end - at > CCI_BLOCK + 512) {
        return false;
    }
    if (!(e0 & 0x80000000u)) {
        return read_at(p->fd, out, CCI_BLOCK, at);
    }
    uint8_t in[CCI_BLOCK + 512];
    const size_t n = (size_t)(end - at);
    if (!read_at(p->fd, in, n, at)) {
        return false;
    }
    const size_t pad = in[0];   /* the block's first byte: the padding after its data */
    return n > 1 + pad && lz4_block(in + 1, n - 1 - pad, out, CCI_BLOCK);
}

bool rx_cci_read(int fd, void *buf, size_t len, uint64_t off)
{
    const Cci *c = find(fd);
    if (!c || off + len > c->size) {
        return false;
    }
    uint8_t *to = buf;
    uint8_t block[CCI_BLOCK];
    for (unsigned i = 0; i < c->nparts && len; i++) {
        const CciPart *p = &c->part[i];
        while (len && off >= p->start && off < p->start + p->size) {
            const uint64_t in = off - p->start, b = in / CCI_BLOCK;
            const size_t skip = (size_t)(in % CCI_BLOCK);
            const size_t take = len < CCI_BLOCK - skip ? len : CCI_BLOCK - skip;
            if (!block_read(p, b, block)) {
                return false;
            }
            memcpy(to, block + skip, take);
            to += take;
            off += take;
            len -= take;
        }
    }
    return len == 0;
}
