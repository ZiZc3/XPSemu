/*
 * XOG: CCI disc images: an ISO in 2 KiB blocks, each LZ4 or as it was, with an index. One file, or
 * parts named name.1.cci, name.2.cci, ... Read through a handle the rest takes for a file's.
 * CHD images (.chd) go to chd_ps5.c through these same calls (patch 173); CSO (.cso, patch 174) and split XISOs (Game.1.iso + Game.2.iso, patch 175) are below.
 * Our own code. GPL-3.0-or-later.
 */
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

bool rx_chd_name(const char *path);
int rx_chd_open(const char *path);
bool rx_chd_is(int fd);
void rx_chd_close(int fd);
bool rx_chd_read(int fd, void *buf, size_t len, uint64_t off);

#define CCI_BLOCK 2048u

static bool cso_name(const char *path);
static bool split_name(const char *path);
static bool split_later(const char *path);
static int split_open(const char *path);
struct Split;
static struct Split *split_find(int fd);
static bool split_close(int fd);
static bool split_read(const struct Split *c, void *buf, size_t len, uint64_t off);
static int cso_open(const char *path);
struct Cso;
static struct Cso *cso_find(int fd);
static bool cso_close(int fd);
static bool cso_read(const struct Cso *c, void *buf, size_t len, uint64_t off);
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
    return (n > 6 && (!strcasecmp(path + n - 4, ".cci") || !strcasecmp(path + n - 4, ".cso")) && path[n - 6] == '.' &&
            path[n - 5] > '1' && path[n - 5] <= '9') ||
           split_later(path);
}

bool rx_cci_name(const char *path)
{
    const size_t n = strlen(path);
    return (n > 4 && !strcasecmp(path + n - 4, ".cci")) || rx_chd_name(path) || cso_name(path) || split_name(path);
}

/* The handle (a descriptor), or -1. */
int rx_cci_open(const char *path)
{
    if (rx_chd_name(path)) {
        return rx_chd_open(path);
    }
    if (cso_name(path)) {
        return cso_open(path);
    }
    if (split_name(path)) {
        return split_open(path);
    }
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
    return find(fd) != NULL || rx_chd_is(fd) || cso_find(fd) != NULL || split_find(fd) != NULL;
}

void rx_cci_close(int fd)
{
    if (rx_chd_is(fd)) {
        rx_chd_close(fd);
        return;
    }
    if (cso_close(fd) || split_close(fd)) {
        return;
    }
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

/* ---- CSO (patch 174): the Xbox CISO of MakeMHz's ciso.py (Stellar): "CISO" header of 24 bytes, an
 * index of blocks + 1 words (offset >> align; top bit: an LZ4 frame chunk, a 4-byte size and an LZ4
 * block, else the 2 KiB as they were), name.1.cso and, past 0xFFBF6000 bytes, name.2.cso whose
 * offsets start again from 0. ---- */

#define CSO_MAX 4

typedef struct Cso {
    int key;                /* the first part's descriptor */
    int fd[2];
    uint32_t *index;        /* blocks + 1 */
    uint32_t blocks, block, second;     /* block size; first block in part 2 (blocks if none) */
    uint8_t align;
    uint64_t size;
} Cso;

static Cso *g_cso[CSO_MAX];

static bool cso_name(const char *path)
{
    const size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".cso");
}

static int cso_open(const char *path)
{
    const int fd = open(path, O_RDONLY);
    uint8_t h[24];
    if (fd < 0 || !read_at(fd, h, sizeof(h), 0) || memcmp(h, "CISO", 4)) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    Cso *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        return -1;
    }
    uint32_t header;
    memcpy(&header, h + 4, 4);
    memcpy(&c->size, h + 8, 8);
    memcpy(&c->block, h + 16, 4);
    c->align = h[21];           /* h[20] is the version */
    c->key = c->fd[0] = fd;
    c->fd[1] = -1;
    if (c->block != CCI_BLOCK || !c->size || header < 24 || c->align > 8) {
        close(fd);
        free(c);
        return -1;
    }
    c->blocks = (uint32_t)(c->size / c->block);
    c->index = malloc(((size_t)c->blocks + 1) * 4);
    if (!c->index || !read_at(fd, c->index, ((size_t)c->blocks + 1) * 4, header)) {
        close(fd);
        free(c->index);
        free(c);
        return -1;
    }
    /* part 2: where the offsets start again */
    c->second = c->blocks;
    for (uint32_t b = 1; b < c->blocks; b++) {
        if ((c->index[b] & 0x7FFFFFFFu) < (c->index[b - 1] & 0x7FFFFFFFu)) {
            c->second = b;
            break;
        }
    }
    const size_t n = strlen(path);
    if (n > 6 && path[n - 6] == '.' && path[n - 5] == '1') {
        char *next = strdup(path);
        if (next) {
            next[n - 5] = '2';
            c->fd[1] = open(next, O_RDONLY);
            free(next);
        }
    }
    if (c->second < c->blocks && c->fd[1] < 0) {
        close(fd);
        free(c->index);
        free(c);
        return -1;      /* its second part isn't there */
    }
    for (unsigned i = 0; i < CSO_MAX; i++) {
        if (!g_cso[i]) {
            g_cso[i] = c;
            return fd;
        }
    }
    close(fd);
    if (c->fd[1] >= 0) {
        close(c->fd[1]);
    }
    free(c->index);
    free(c);
    return -1;
}

static Cso *cso_find(int fd)
{
    for (unsigned i = 0; i < CSO_MAX; i++) {
        if (g_cso[i] && g_cso[i]->key == fd) {
            return g_cso[i];
        }
    }
    return NULL;
}

static bool cso_close(int fd)
{
    for (unsigned i = 0; i < CSO_MAX; i++) {
        Cso *c = g_cso[i];
        if (c && c->key == fd) {
            g_cso[i] = NULL;
            close(c->fd[0]);
            if (c->fd[1] >= 0) {
                close(c->fd[1]);
            }
            free(c->index);
            free(c);
            return true;
        }
    }
    return false;
}

static bool cso_block(const Cso *c, uint32_t b, uint8_t *out)
{
    const int fd = b >= c->second ? c->fd[1] : c->fd[0];
    const uint64_t at = (uint64_t)(c->index[b] & 0x7FFFFFFFu) << c->align;
    if (!(c->index[b] & 0x80000000u)) {
        return read_at(fd, out, CCI_BLOCK, at);
    }
    uint32_t n;
    uint8_t in[CCI_BLOCK + 64];
    if (!read_at(fd, &n, 4, at)) {
        return false;
    }
    if (n & 0x80000000u) {      /* an LZ4 frame's block kept as it was */
        return (n & 0x7FFFFFFFu) == CCI_BLOCK && read_at(fd, out, CCI_BLOCK, at + 4);
    }
    return n && n <= sizeof(in) && read_at(fd, in, n, at + 4) && lz4_block(in, n, out, CCI_BLOCK);
}

static bool cso_read(const Cso *c, void *buf, size_t len, uint64_t off)
{
    if (off > c->size || len > c->size - off) {
        return false;
    }
    uint8_t *to = buf;
    uint8_t block[CCI_BLOCK];
    while (len) {
        const uint64_t b = off / CCI_BLOCK;
        const size_t skip = (size_t)(off % CCI_BLOCK);
        const size_t take = len < CCI_BLOCK - skip ? len : CCI_BLOCK - skip;
        if (b >= c->blocks) {
            memset(to, 0, take);        /* a tail past the last whole block */
        } else if (!cso_block(c, (uint32_t)b, block)) {
            return false;
        } else {
            memcpy(to, block + skip, take);
        }
        to += take;
        off += take;
        len -= take;
    }
    return true;
}

/* ---- Split XISOs (patch 175): Game.1.iso, Game.2.iso, ... (a FATX drive's 4 GiB file limit) read as
 * one disc, part after part. A name.1.iso without a name.2.iso beside it is a plain ISO. ---- */

#define SPLIT_MAX 4
#define SPLIT_PARTS 9

typedef struct Split {
    int key;
    unsigned nparts;
    int fd[SPLIT_PARTS];
    uint64_t start[SPLIT_PARTS + 1];    /* part i holds [start[i], start[i + 1]) */
} Split;

static Split *g_split[SPLIT_MAX];

/* "Game.N.iso" / "Game.N.xiso": N, else 0 */
static int split_part_of(const char *path)
{
    const size_t n = strlen(path);
    size_t ext = 0;
    if (n > 4 && !strcasecmp(path + n - 4, ".iso")) {
        ext = 4;
    }
    if (n > 5 && !strcasecmp(path + n - 5, ".xiso")) {
        ext = 5;
    }
    if (!ext || n < ext + 3 || path[n - ext - 2] != '.') {
        return 0;
    }
    const char d = path[n - ext - 1];
    return d >= '1' && d <= '9' ? d - '0' : 0;
}

/* path with its part number changed to part */
static char *split_part_path(const char *path, int part)
{
    const size_t n = strlen(path);
    const size_t ext = !strcasecmp(path + n - 4, ".iso") ? 4 : 5;
    char *p = strdup(path);
    if (p) {
        p[n - ext - 1] = (char)('0' + part);
    }
    return p;
}

static bool split_exists(const char *path, int part)
{
    char *p = split_part_path(path, part);
    const bool there = p && access(p, R_OK) == 0;
    free(p);
    return there;
}

static bool split_name(const char *path)
{
    return split_part_of(path) == 1 && split_exists(path, 2);
}

static bool split_later(const char *path)
{
    return split_part_of(path) > 1 && split_exists(path, 1);
}

static int split_open(const char *path)
{
    Split *c = calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }
    for (int part = 1; part <= SPLIT_PARTS; part++) {
        char *p = split_part_path(path, part);
        const int fd = p ? open(p, O_RDONLY) : -1;
        free(p);
        if (fd < 0) {
            break;
        }
        const off_t end = lseek(fd, 0, SEEK_END);
        if (end <= 0) {
            close(fd);
            break;
        }
        c->fd[c->nparts] = fd;
        c->start[c->nparts + 1] = c->start[c->nparts] + (uint64_t)end;
        c->nparts++;
    }
    if (c->nparts < 2) {
        for (unsigned i = 0; i < c->nparts; i++) {
            close(c->fd[i]);
        }
        free(c);
        return -1;
    }
    c->key = c->fd[0];
    for (unsigned i = 0; i < SPLIT_MAX; i++) {
        if (!g_split[i]) {
            g_split[i] = c;
            return c->key;
        }
    }
    for (unsigned i = 0; i < c->nparts; i++) {
        close(c->fd[i]);
    }
    free(c);
    return -1;
}

static Split *split_find(int fd)
{
    for (unsigned i = 0; i < SPLIT_MAX; i++) {
        if (g_split[i] && g_split[i]->key == fd) {
            return g_split[i];
        }
    }
    return NULL;
}

static bool split_close(int fd)
{
    for (unsigned i = 0; i < SPLIT_MAX; i++) {
        Split *c = g_split[i];
        if (c && c->key == fd) {
            g_split[i] = NULL;
            for (unsigned p = 0; p < c->nparts; p++) {
                close(c->fd[p]);
            }
            free(c);
            return true;
        }
    }
    return false;
}

static bool split_read(const Split *c, void *buf, size_t len, uint64_t off)
{
    if (off > c->start[c->nparts] || len > c->start[c->nparts] - off) {
        return false;
    }
    uint8_t *to = buf;
    for (unsigned i = 0; i < c->nparts && len; i++) {
        if (off >= c->start[i + 1]) {
            continue;
        }
        const uint64_t room = c->start[i + 1] - off;
        const size_t take = len < room ? len : (size_t)room;
        if (!read_at(c->fd[i], to, take, off - c->start[i])) {
            return false;
        }
        to += take;
        off += take;
        len -= take;
    }
    return len == 0;
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
    if (rx_chd_is(fd)) {
        return rx_chd_read(fd, buf, len, off);
    }
    const struct Cso *cso = cso_find(fd);
    if (cso) {
        return cso_read(cso, buf, len, off);
    }
    const struct Split *split = split_find(fd);
    if (split) {
        return split_read(split, buf, len, off);
    }
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
