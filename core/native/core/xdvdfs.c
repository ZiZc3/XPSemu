/*
 * hakuX native core — XDVDFS reader.
 *
 * Volume descriptor at sector 32 of the game partition: "MICROSOFT*XBOX*MEDIA",
 * root directory sector and size. A directory is a binary tree of entries:
 *   u16 left, u16 right   (child offsets in dwords; 0 = none: the root is
 *                          nobody's child; 0xFFFF pads a sector's end)
 *   u32 sector, u32 size
 *   u8 attributes, u8 name length, name
 */
#include "native/core/xdvdfs.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char k_magic[] = "MICROSOFT*XBOX*MEDIA";

/* Game partition offsets: extracted XISO, then full XGD1/XGD2/XGD3 dumps. */
static const uint64_t k_part_offsets[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};

extern bool rx_cci_is(int fd);
extern bool rx_cci_read(int fd, void *buf, size_t len, uint64_t off);

static bool pread_all(int fd, void *buf, size_t len, uint64_t off)
{
    if (rx_cci_is(fd)) {
        return rx_cci_read(fd, buf, len, off);
    }
    extern bool nc_pread_sure(int fd, void *buf, size_t len, uint64_t off);
    return nc_pread_sure(fd, buf, len, off);    /* patch 356: a refused read tried again */
    return true;
}

bool nc_disc_open(NcDisc *d, int fd)
{
    memset(d, 0, sizeof(*d));
    d->fd = fd;
    for (size_t i = 0; i < sizeof(k_part_offsets) / sizeof(k_part_offsets[0]); i++) {
        uint8_t vd[NC_XDVDFS_SECTOR];
        uint64_t off = k_part_offsets[i];
        if (!pread_all(fd, vd, sizeof(vd), off + 32ull * NC_XDVDFS_SECTOR)) {
            continue;
        }
        if (memcmp(vd, k_magic, 20) == 0 && memcmp(vd + 0x7EC, k_magic, 20) == 0) {
            d->part_off = off;
            d->root_sector = nc_rd32(vd + 20);
            d->root_size = nc_rd32(vd + 24);
            nc_log("hakuX-native", "disc: XDVDFS at +0x%llx, root sector %u (%u bytes)",
                   (unsigned long long)off, d->root_sector, d->root_size);
            return true;
        }
    }
    nc_log("hakuX-native", "disc: no XDVDFS volume found");
    return false;
}

/* ---- the folders' tables ----
 * A disc doesn't change: a table read once is kept, keyed by its sector, up to DIRS_MAX bytes in
 * all. The helper thread reads every folder's table, breadth first from the root, then ends. */
#define DIRS_BUCKETS 1024u
#define DIRS_MAX (32u << 20)

typedef struct DirTab {
    struct DirTab *next;
    uint32_t sector, size;
    uint8_t bytes[];
} DirTab;

struct NcDirCache {
    pthread_mutex_t lock;
    DirTab *bucket[DIRS_BUCKETS];
    size_t bytes;
    unsigned tables;
    pthread_t warm;
    bool warming;
    int stop;                   /* atomic: nc_disc_close */
    const NcDisc *disc;
};

/* The image's bytes: an ISO's, or a CCI's (pread_all). */
static bool image_read(const NcDisc *d, void *buf, size_t len, uint64_t off)
{
    return pread_all(d->fd, buf, len, off);
}

static const DirTab *dirs_find(struct NcDirCache *c, uint32_t sector, uint32_t size)
{
    for (const DirTab *t = c->bucket[sector % DIRS_BUCKETS]; t; t = t->next) {
        if (t->sector == sector && t->size == size) {
            return t;
        }
    }
    return NULL;
}

/* A folder's table: the cache's (*own false) or read now (*own true: the caller frees it). */
static uint8_t *dir_table(const NcDisc *d, uint32_t sector, uint32_t size, bool *own)
{
    struct NcDirCache *c = d->dirs;
    *own = false;
    if (c) {
        pthread_mutex_lock(&c->lock);
        const DirTab *t = dirs_find(c, sector, size);
        pthread_mutex_unlock(&c->lock);
        if (t) {
            return (uint8_t *)t->bytes;    /* never freed while the disc is open */
        }
    }
    uint8_t *buf = malloc(size);
    if (!buf || !image_read(d, buf, size, d->part_off + (uint64_t)sector * NC_XDVDFS_SECTOR)) {
        free(buf);
        return NULL;
    }
    DirTab *t = c ? malloc(sizeof(*t) + size) : NULL;
    if (t) {
        t->sector = sector;
        t->size = size;
        memcpy(t->bytes, buf, size);
        pthread_mutex_lock(&c->lock);
        const DirTab *had = dirs_find(c, sector, size);
        const bool keep = !had && c->bytes + size <= DIRS_MAX;
        if (keep) {
            t->next = c->bucket[sector % DIRS_BUCKETS];
            c->bucket[sector % DIRS_BUCKETS] = t;
            c->bytes += size;
            c->tables++;
        }
        pthread_mutex_unlock(&c->lock);
        if (keep || had) {
            if (!keep) {
                free(t);                   /* another thread's came first */
            }
            free(buf);
            return (uint8_t *)(keep ? t->bytes : had->bytes);
        }
        free(t);                           /* the cache is full */
    }
    *own = true;
    return buf;
}

typedef struct WarmQueue {
    NcDiscFile *q;
    unsigned head, n, cap;
} WarmQueue;

static bool warm_entry(void *ctx, const uint8_t *e, uint32_t off)
{
    WarmQueue *w = ctx;
    (void)off;
    const uint32_t sector = nc_rd32(e + 4), size = nc_rd32(e + 8);
    if (!(e[12] & 0x10) || !size || size > (4u << 20)) {
        return true;
    }
    if (w->n == w->cap) {
        NcDiscFile *grown = realloc(w->q, (w->cap ? w->cap * 2 : 256) * sizeof(*grown));
        if (!grown) {
            return false;
        }
        w->q = grown;
        w->cap = w->cap ? w->cap * 2 : 256;
    }
    w->q[w->n++] = (NcDiscFile){sector, size, e[12]};
    return true;
}

static uint64_t dirs_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void *dirs_warm(void *arg)
{
    struct NcDirCache *c = arg;
    const NcDisc *d = c->disc;
    WarmQueue w = {0};
    const uint64_t t0 = dirs_now_ms();
    warm_entry(&w, (const uint8_t[14]){0, 0, 0, 0, (uint8_t)d->root_sector, (uint8_t)(d->root_sector >> 8),
                                       (uint8_t)(d->root_sector >> 16), (uint8_t)(d->root_sector >> 24),
                                       (uint8_t)d->root_size, (uint8_t)(d->root_size >> 8),
                                       (uint8_t)(d->root_size >> 16), (uint8_t)(d->root_size >> 24), 0x10, 0},
               0);
    unsigned visits = 0;
    bool full = false;
    while (w.head < w.n && !__atomic_load_n(&c->stop, __ATOMIC_ACQUIRE) && visits < 65536) {
        const NcDiscFile f = w.q[w.head++];
        visits++;
        bool own;
        uint8_t *tab = dir_table(d, f.sector, f.size, &own);
        if (!tab) {
            continue;
        }
        nc_xdvdfs_walk(tab, f.size, warm_entry, &w);
        if (own) {
            free(tab);
            full = true;                   /* not kept: the cache is full */
            break;
        }
    }
    pthread_mutex_lock(&c->lock);
    const unsigned tables = c->tables;
    const size_t bytes = c->bytes;
    pthread_mutex_unlock(&c->lock);
    nc_log("hakuX-native", "disc: %u folders' tables in memory (%zu KiB), read in %llu ms%s", tables, bytes >> 10,
           (unsigned long long)(dirs_now_ms() - t0), full ? " (the cache is full: the rest read when opened)" : "");
    free(w.q);
    return NULL;
}

void nc_disc_cache_folders(NcDisc *d)
{
    if (d->dirs || d->fd < 0) {
        return;
    }
    struct NcDirCache *c = calloc(1, sizeof(*c));
    if (!c) {
        return;
    }
    pthread_mutex_init(&c->lock, NULL);
    c->disc = d;
    d->dirs = c;
    c->warming = pthread_create(&c->warm, NULL, dirs_warm, c) == 0;
    if (c->warming) {
        pthread_setname_np(c->warm, "disc-folders");
    }
}

static void dirs_free(NcDisc *d)
{
    struct NcDirCache *c = d->dirs;
    if (!c) {
        return;
    }
    __atomic_store_n(&c->stop, 1, __ATOMIC_RELEASE);
    if (c->warming) {
        pthread_join(c->warm, NULL);
    }
    for (unsigned i = 0; i < DIRS_BUCKETS; i++) {
        for (DirTab *t = c->bucket[i], *n; t; t = n) {
            n = t->next;
            free(t);
        }
    }
    pthread_mutex_destroy(&c->lock);
    free(c);
    d->dirs = NULL;
}

void nc_disc_close(NcDisc *d)
{
    dirs_free(d);
    if (d->fd >= 0) {
        close(d->fd);
    }
    d->fd = -1;
}

static bool name_eq(const uint8_t *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) {
        return false;
    }
    for (size_t i = 0; i < alen; i++) {
        if (toupper(a[i]) != toupper((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

/* Depth-first over a folder's tree, each entry once: a malformed tree that
 * loops still ends. fn only sees entries whose name fits inside dir. */
bool nc_xdvdfs_walk(const uint8_t *dir, uint32_t size, NcXdvdfsEntryFn fn, void *ctx)
{
    /* Entries start at multiples of 4: one bit each marks an entry seen. */
    uint8_t *seen = calloc(size / 32 + 1, 1);
    uint32_t *stack = NULL, n = 0, cap = 0, off = 0;
    bool ok = seen != NULL, more = true, down = true;
    while (ok && more) {
        /* Push the entry and its left children. Stop at a seen one. */
        while (down) {
            down = false;
            if (off + 14 > size || (seen[off / 32] >> (off / 4 % 8) & 1)) {
                break;
            }
            const uint8_t *e = dir + off;
            const uint16_t left = nc_rd16(e), right = nc_rd16(e + 2);
            if (left == 0xFFFF && right == 0xFFFF && e[12] == 0xFF) {
                break; /* padding */
            }
            seen[off / 32] |= (uint8_t)(1u << (off / 4 % 8));
            if (n == cap) {
                uint32_t *grown = realloc(stack, (cap ? cap * 2 : 64) * sizeof(*stack));
                if (!grown) {
                    ok = false;
                    break;
                }
                stack = grown;
                cap = cap ? cap * 2 : 64;
            }
            stack[n++] = off;
            if (left && left != 0xFFFF) {
                off = left * 4u;
                down = true;
            }
        }
        if (!ok || !n) {
            break;
        }
        off = stack[--n];
        const uint8_t *e = dir + off;
        if (off + 14 + e[13] <= size) {
            more = fn(ctx, e, off);
        }
        const uint16_t right = nc_rd16(e + 2);
        down = right && right != 0xFFFF;
        off = right * 4u;
    }
    free(stack);
    free(seen);
    return ok;
}

typedef struct DirFind {
    const char *name;
    size_t len;
    NcDiscFile *out;
    bool found;
} DirFind;

static bool find_entry(void *ctx, const uint8_t *e, uint32_t off)
{
    DirFind *f = ctx;
    (void)off;
    if (!name_eq(e + 14, e[13], f->name, f->len)) {
        return true;
    }
    f->out->sector = nc_rd32(e + 4);
    f->out->size = nc_rd32(e + 8);
    f->out->attr = e[12];
    f->found = true;
    return false;
}

static bool dir_search(const uint8_t *dir, uint32_t size, const char *name, size_t namelen, NcDiscFile *out)
{
    DirFind f = {name, namelen, out, false};
    return nc_xdvdfs_walk(dir, size, find_entry, &f) && f.found;
}

/* the disc's own files */
static bool disc_find(const NcDisc *d, const char *path, NcDiscFile *out)
{
    NcDiscFile cur = {d->root_sector, d->root_size, 0x10};
    const char *p = path;
    while (*p) {
        while (*p == '\\' || *p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *end = p;
        while (*end && *end != '\\' && *end != '/') {
            end++;
        }
        if (!(cur.attr & 0x10) || !cur.size) {
            return false;
        }
        bool own;
        uint8_t *dir = dir_table(d, cur.sector, cur.size, &own);
        if (!dir) {
            return false;
        }
        const uint32_t size = cur.size;
        bool ok = dir_search(dir, size, p, (size_t)(end - p), &cur);
        if (own) {
            free(dir);
        }
        if (!ok) {
            return false;
        }
        p = end;
    }
    *out = cur;
    return true;
}

/* ---- the overlay (mods) ----
 * Entries keyed by their path, normalized: no leading slash, backslashes,
 * lower case. Added in the mods' order, then sorted (the later of two at a
 * path stays); a file's descriptor opens at its first read. */
typedef struct OvEntry {
    char path[256];             /* normalized: the key */
    char shown[256];            /* as the mod has it (listings) */
    char *host;                 /* NULL: a folder */
    uint32_t size;
    unsigned seq;               /* the order added: the later wins */
    int fd;                     /* atomic: -1 until the first read */
    unsigned vid;               /* patch 219: 1 + the hook's file, 0: a host file */
} OvEntry;

typedef struct OvPatch {
    uint32_t sector;            /* the file it patches (its NcDiscFile) */
    uint64_t off;
    uint32_t len;
    uint8_t *bytes;
} OvPatch;

struct NcDiscOverlay {
    OvEntry *e;
    unsigned n, cap;
    bool sealed;
    OvPatch *p;
    unsigned np, pcap;
};

static void norm_case(const char *in, char *out, size_t cap, bool lower)
{
    size_t n = 0;
    while (*in == '\\' || *in == '/') {
        in++;
    }
    for (; *in && n + 1 < cap; in++) {
        char c = *in == '/' ? '\\' : lower ? (char)tolower((unsigned char)*in) : *in;
        if (c == '\\' && n && out[n - 1] == '\\') {
            continue;
        }
        out[n++] = c;
    }
    while (n && out[n - 1] == '\\') {
        n--;
    }
    out[n] = 0;
}

static void norm_path(const char *in, char *out, size_t cap)
{
    norm_case(in, out, cap, true);
}

static NcDiscOverlay *ov_of(NcDisc *d)
{
    if (!d->ov) {
        d->ov = calloc(1, sizeof(*d->ov));
    }
    return d->ov;
}

/* shown: the path as the mod has it (normalized but its case) */
static OvEntry *ov_push(NcDiscOverlay *o, const char *path, const char *shown)
{
    if (o->n == o->cap) {
        unsigned cap = o->cap ? o->cap * 2 : 64;
        OvEntry *e = realloc(o->e, cap * sizeof(*e));
        if (!e) {
            return NULL;
        }
        o->e = e;
        o->cap = cap;
    }
    OvEntry *e = &o->e[o->n];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->shown, sizeof(e->shown), "%s", shown);
    e->seq = o->n++;
    e->fd = -1;
    return e;
}

bool nc_disc_overlay_file(NcDisc *d, const char *path, const char *host)
{
    NcDiscOverlay *o = ov_of(d);
    char p[256], shown[256];
    norm_path(path, p, sizeof(p));
    norm_case(path, shown, sizeof(shown), false);   /* the same length: only the case differs */
    struct stat st;
    if (!o || o->sealed || !*p || stat(host, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 0xFFFFFFFFll) {
        return false;
    }
    OvEntry *e = ov_push(o, p, shown);
    if (!e || !(e->host = strdup(host))) {
        return false;
    }
    e->size = (uint32_t)st.st_size;
    /* the folders on its way the disc doesn't have */
    for (char *s = strchr(p, '\\'); s; s = strchr(s + 1, '\\')) {
        const size_t at = (size_t)(s - p);
        *s = 0;
        shown[at] = 0;
        NcDiscFile f;
        if (!disc_find(d, p, &f)) {
            ov_push(o, p, shown);
        } else if (!(f.attr & 0x10)) {
            nc_log("hakuX-native-mods", "%s: %s is a file on the disc", path, p);
        }
        *s = '\\';
        shown[at] = '\\';
    }
    return true;
}

bool nc_disc_overlay_virtual(NcDisc *d, const char *path, uint32_t size, unsigned id)
{
    NcDiscOverlay *o = ov_of(d);
    char p[256], shown[256];
    norm_path(path, p, sizeof(p));
    norm_case(path, shown, sizeof(shown), false);
    if (!o || o->sealed || !*p) {
        return false;
    }
    OvEntry *e = ov_push(o, p, shown);
    if (!e || !(e->host = strdup("(image)"))) {
        return false;
    }
    e->size = size;
    e->vid = id + 1;
    for (char *s = strchr(p, '\\'); s; s = strchr(s + 1, '\\')) {     /* its folders */
        const size_t at = (size_t)(s - p);
        *s = 0;
        shown[at] = 0;
        NcDiscFile f;
        if (!disc_find(d, p, &f)) {
            ov_push(o, p, shown);
        }
        *s = '\\';
        shown[at] = '\\';
    }
    return true;
}

static int ov_cmp(const void *a, const void *b)
{
    const OvEntry *x = a, *y = b;
    const int c = strcmp(x->path, y->path);
    return c ? c : (x->seq > y->seq) - (x->seq < y->seq);
}

void nc_disc_overlay_seal(NcDisc *d)
{
    NcDiscOverlay *o = d->ov;
    if (!o || o->sealed) {
        return;
    }
    qsort(o->e, o->n, sizeof(*o->e), ov_cmp);
    unsigned n = 0;
    for (unsigned i = 0; i < o->n; i++) {
        /* of the entries at a path the last added stays; a file over a folder */
        if (n && !strcmp(o->e[n - 1].path, o->e[i].path)) {
            if (!o->e[i].host && o->e[n - 1].host) {
                continue;
            }
            free(o->e[n - 1].host);
            o->e[n - 1] = o->e[i];
            continue;
        }
        o->e[n++] = o->e[i];
    }
    o->n = n;
    o->sealed = true;
}

static const OvEntry *ov_find(const NcDiscOverlay *o, const char *norm)
{
    if (!o || !o->sealed) {
        return NULL;
    }
    unsigned lo = 0, hi = o->n;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        const int c = strcmp(o->e[mid].path, norm);
        if (!c) {
            return &o->e[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

bool nc_disc_find(const NcDisc *d, const char *path, NcDiscFile *out)
{
    if (d->ov) {
        char p[256];
        norm_path(path, p, sizeof(p));
        const OvEntry *e = ov_find(d->ov, p);
        if (e) {
            out->sector = NC_DISC_OVERLAY + (uint32_t)(e - d->ov->e);
            out->size = e->size;
            out->attr = e->host ? 0x01 : 0x10;      /* read-only, or a folder */
            return true;
        }
    }
    return disc_find(d, path, out);
}

bool nc_disc_overlay_patch(NcDisc *d, const char *path, uint64_t off, const uint8_t *bytes, uint32_t len)
{
    NcDiscOverlay *o = ov_of(d);
    NcDiscFile f;
    if (!o || !len || !nc_disc_find(d, path, &f) || (f.attr & 0x10) || off + len > f.size) {
        return false;
    }
    if (o->np == o->pcap) {
        unsigned cap = o->pcap ? o->pcap * 2 : 16;
        OvPatch *p = realloc(o->p, cap * sizeof(*p));
        if (!p) {
            return false;
        }
        o->p = p;
        o->pcap = cap;
    }
    OvPatch *p = &o->p[o->np];
    if (!(p->bytes = malloc(len))) {
        return false;
    }
    memcpy(p->bytes, bytes, len);
    p->sector = f.sector;
    p->off = off;
    p->len = len;
    o->np++;
    return true;
}

void nc_disc_overlay_list(const NcDisc *d, const char *dir,
                          void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx)
{
    const NcDiscOverlay *o = d->ov;
    if (!o || !o->sealed) {
        return;
    }
    char p[256];
    norm_path(dir, p, sizeof(p));
    const size_t n = strlen(p);
    for (unsigned i = 0; i < o->n; i++) {
        const char *e = o->e[i].path;
        if (n && (strncmp(e, p, n) || e[n] != '\\')) {
            continue;
        }
        const size_t at = n ? n + 1 : 0;
        if (e[at] && !strchr(e + at, '\\')) {
            fn(ctx, o->e[i].shown + at, o->e[i].size, !o->e[i].host);
        }
    }
}

bool (*nc_disc_vread_hook)(unsigned id, uint64_t off, void *buf, size_t len);

static bool ov_read(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len)
{
    const NcDiscOverlay *o = d->ov;
    const uint32_t i = f->sector - NC_DISC_OVERLAY;
    if (!o || i >= o->n || !o->e[i].host) {
        return false;
    }
    OvEntry *e = &o->e[i];
    if (e->vid) {
        return nc_disc_vread_hook && nc_disc_vread_hook(e->vid - 1, off, buf, len);
    }
    int fd = __atomic_load_n(&e->fd, __ATOMIC_ACQUIRE);
    if (fd < 0) {
        int mine = open(e->host, O_RDONLY | O_CLOEXEC), none = -1;
        if (mine < 0) {
            nc_log("hakuX-native-mods", "%s: can't open %s", e->path, e->host);
            return false;
        }
        if (!__atomic_compare_exchange_n(&e->fd, &none, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            close(mine);                /* another thread's came first */
        }
        fd = __atomic_load_n(&e->fd, __ATOMIC_ACQUIRE);
    }
    return pread_all(fd, buf, len, off);
}

bool nc_disc_find_orig(const NcDisc *d, const char *path, NcDiscFile *out)
{
    return disc_find(d, path, out);
}

bool nc_disc_read_orig(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len)
{
    return f->sector < NC_DISC_OVERLAY && off + len <= f->size &&
           pread_all(d->fd, buf, len, d->part_off + (uint64_t)f->sector * NC_XDVDFS_SECTOR + off);
}

bool nc_disc_read_file(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len)
{
    if (off + len > f->size) {
        return false;
    }
    return f->sector >= NC_DISC_OVERLAY
               ? ov_read(d, f, off, buf, len)
               : pread_all(d->fd, buf, len, d->part_off + (uint64_t)f->sector * NC_XDVDFS_SECTOR + off);
}

bool nc_disc_read(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len)
{
    const bool ok = nc_disc_read_file(d, f, off, buf, len);
    if (ok && d->ov) {
        for (unsigned i = 0; i < d->ov->np; i++) {
            const OvPatch *p = &d->ov->p[i];
            if (p->sector != f->sector || p->off >= off + len || p->off + p->len <= off) {
                continue;
            }
            const uint64_t a = p->off > off ? p->off : off;
            const uint64_t b = p->off + p->len < off + len ? p->off + p->len : off + len;
            memcpy((uint8_t *)buf + (a - off), p->bytes + (a - p->off), (size_t)(b - a));
        }
    }
    return ok;
}
