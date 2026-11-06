/*
 * hakuX native core — savestate container (state.h).
 *
 * File: magic, container version, the header (NcStateInfo's fields, its
 * length first so that a longer one still reads), the section table (tag,
 * version, offset, length, CRC-32 per section), then the sections.
 */
#include "native/state/state.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static char g_fail[200];

void nc_state_fail(const char *fmt, ...)
{
    if (g_fail[0]) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fail, sizeof(g_fail), fmt, ap);
    va_end(ap);
}

bool nc_state_trace(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NC_STATE_TRACE");
        on = e && atoi(e) != 0;
    }
    return on;
}

/* ---- buffers -------------------------------------------------------------- */

void nc_buf_put(NcBuf *b, const void *p, size_t n)
{
    if (b->fail || !n) {
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) {
            cap *= 2;
        }
        uint8_t *q = realloc(b->p, cap);
        if (!q) {
            b->fail = true;
            return;
        }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

void nc_buf_free(NcBuf *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

bool nc_rd_get(NcRd *r, void *out, size_t n)
{
    if (r->fail || n > r->len - r->pos) {
        r->fail = true;
        memset(out, 0, n);
        return false;
    }
    memcpy(out, r->p + r->pos, n);
    r->pos += n;
    return true;
}

const void *nc_rd_ptr(NcRd *r, size_t n)
{
    if (r->fail || n > r->len - r->pos) {
        r->fail = true;
        return NULL;
    }
    const void *p = r->p + r->pos;
    r->pos += n;
    return p;
}

/* ---- records -------------------------------------------------------------- */

void nc_rec_put(NcBuf *b, const NcRecord *rec, const void *obj)
{
    const uint8_t *o = obj;
    for (unsigned i = 0; i < rec->nfields; i++) {
        const NcField *f = &rec->fields[i];
        if (f->until) {
            continue;                   /* no longer stored */
        }
        if (f->type == NC_FT_REC) {
            nc_recs_put(b, f->rec, o + f->offset, f->esize, f->count);
        } else {
            nc_buf_put(b, o + f->offset, (size_t)f->esize * f->count);
        }
    }
}

void nc_recs_put(NcBuf *b, const NcRecord *rec, const void *obj, size_t stride, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        nc_rec_put(b, rec, (const uint8_t *)obj + i * stride);
    }
}

/* A field the version stored and this core dropped: its bytes, skipped. */
static void skip_field(NcRd *r, const NcField *f, unsigned v);

static void skip_rec(NcRd *r, const NcRecord *rec, unsigned v)
{
    for (unsigned i = 0; i < rec->nfields; i++) {
        const NcField *f = &rec->fields[i];
        if (v >= f->since && (!f->until || v < f->until)) {
            skip_field(r, f, v);
        }
    }
}

static void skip_field(NcRd *r, const NcField *f, unsigned v)
{
    if (f->type == NC_FT_REC) {
        for (unsigned k = 0; k < f->count; k++) {
            skip_rec(r, f->rec, v);
        }
    } else {
        nc_rd_ptr(r, (size_t)f->esize * f->count);
    }
}

void nc_rec_get(NcRd *r, const NcRecord *rec, void *obj, unsigned v)
{
    uint8_t *o = obj;
    for (unsigned i = 0; i < rec->nfields; i++) {
        const NcField *f = &rec->fields[i];
        const bool stored = v >= f->since && (!f->until || v < f->until);
        if (!stored) {
            if (!f->until && nc_state_trace()) {
                nc_log("hakuX-native-state", "%s.%s: not in version %u, default kept", rec->name, f->name, v);
            }
            continue;
        }
        if (f->until) {
            skip_field(r, f, v);        /* stored then, dropped since */
        } else if (f->type == NC_FT_REC) {
            nc_recs_get(r, f->rec, o + f->offset, f->esize, f->count, v);
        } else {
            nc_rd_get(r, o + f->offset, (size_t)f->esize * f->count);
        }
    }
}

void nc_recs_get(NcRd *r, const NcRecord *rec, void *obj, size_t stride, unsigned n, unsigned v)
{
    for (unsigned i = 0; i < n; i++) {
        nc_rec_get(r, rec, (uint8_t *)obj + i * stride, v);
    }
}

/* ---- the file ------------------------------------------------------------- */

typedef struct SecEntry {
    uint32_t tag, version;
    uint64_t off, len;
    uint32_t crc, pad;
} SecEntry;

static void put_info(NcBuf *b, const NcStateInfo *in)
{
    NcBuf h = {0};
    nc_buf_u32(&h, in->title_id);
    nc_buf_put(&h, in->xbe_path, sizeof(in->xbe_path));
    nc_buf_put(&h, in->xbe_sha1, sizeof(in->xbe_sha1));
    nc_buf_u64(&h, in->disc_size);
    nc_buf_u32(&h, in->disc_hash);
    nc_buf_u32(&h, in->ram_size);
    nc_buf_u64(&h, in->guest_ns);
    nc_buf_u64(&h, (uint64_t)in->wall_time);
    nc_buf_u8(&h, in->point);
    nc_buf_put(&h, in->core_version, sizeof(in->core_version));
    nc_buf_put(&h, in->core_git, sizeof(in->core_git));
    nc_buf_u32(b, (uint32_t)h.len);
    nc_buf_put(b, h.p, h.len);
    b->fail |= h.fail;
    nc_buf_free(&h);
}

static bool get_info(NcRd *r, NcStateInfo *in)
{
    const uint32_t len = nc_rd_u32(r);
    const void *p = nc_rd_ptr(r, len);
    if (!p) {
        return false;
    }
    NcRd h = {p, len, 0, false};
    memset(in, 0, sizeof(*in));
    in->title_id = nc_rd_u32(&h);
    nc_rd_get(&h, in->xbe_path, sizeof(in->xbe_path));
    nc_rd_get(&h, in->xbe_sha1, sizeof(in->xbe_sha1));
    in->disc_size = nc_rd_u64(&h);
    in->disc_hash = nc_rd_u32(&h);
    in->ram_size = nc_rd_u32(&h);
    in->guest_ns = nc_rd_u64(&h);
    in->wall_time = (int64_t)nc_rd_u64(&h);
    in->point = nc_rd_u8(&h);
    nc_rd_get(&h, in->core_version, sizeof(in->core_version));
    nc_rd_get(&h, in->core_git, sizeof(in->core_git));
    in->xbe_path[sizeof(in->xbe_path) - 1] = 0;
    in->core_version[sizeof(in->core_version) - 1] = 0;
    in->core_git[sizeof(in->core_git) - 1] = 0;
    return !h.fail;
}

static bool write_all(int fd, const void *p, size_t n)
{
    const uint8_t *q = p;
    while (n) {
        const ssize_t w = write(fd, q, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        q += w;
        n -= (size_t)w;
    }
    return true;
}

const char *nc_state_write(const char *path, const NcStateInfo *info, const NcStateSection *secs,
                           unsigned nsecs)
{
    static char why[256];
    NcBuf *body = calloc(nsecs, sizeof(NcBuf));
    SecEntry *tab = calloc(nsecs, sizeof(SecEntry));
    if (!body || !tab) {
        free(body);
        free(tab);
        return "out of memory";
    }
    const char *err = NULL;
    for (unsigned i = 0; i < nsecs && !err; i++) {
        if (!secs[i].save(&body[i])) {
            snprintf(why, sizeof(why), "the %s state could not be saved", secs[i].name);
            err = why;
        } else if (body[i].fail) {
            snprintf(why, sizeof(why), "out of memory saving the %s state", secs[i].name);
            err = why;
        }
    }
    NcBuf head = {0};
    if (!err) {
        nc_buf_u32(&head, NC_STATE_MAGIC);
        nc_buf_u32(&head, NC_STATE_CONTAINER);
        put_info(&head, info);
        nc_buf_u32(&head, nsecs);
        uint64_t off = head.len + (uint64_t)nsecs * sizeof(SecEntry);
        for (unsigned i = 0; i < nsecs; i++) {
            tab[i].tag = secs[i].tag;
            tab[i].version = secs[i].version;
            tab[i].off = off;
            tab[i].len = body[i].len;
            tab[i].crc = (uint32_t)crc32(0, body[i].p, (uInt)body[i].len);
            off += body[i].len;
        }
        nc_buf_put(&head, tab, (size_t)nsecs * sizeof(SecEntry));
        if (head.fail) {
            err = "out of memory";
        }
    }
    if (!err) {
        char tmp[600];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        bool ok = fd >= 0 && write_all(fd, head.p, head.len);
        for (unsigned i = 0; ok && i < nsecs; i++) {
            ok = write_all(fd, body[i].p, body[i].len);
        }
        ok = ok && fsync(fd) == 0;
        if (fd >= 0) {
            ok = close(fd) == 0 && ok;
        }
        if (!ok || rename(tmp, path) != 0) {
            snprintf(why, sizeof(why), "can't write %s: %s", path, strerror(errno));
            err = why;
            unlink(tmp);
        }
    }
    nc_buf_free(&head);
    for (unsigned i = 0; i < nsecs; i++) {
        nc_buf_free(&body[i]);
    }
    free(body);
    free(tab);
    return err;
}

/* The file mapped whole (NULL: can't). */
static const uint8_t *map_file(const char *path, size_t *len)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 16) {
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        return NULL;
    }
    *len = (size_t)st.st_size;
    return p;
}

static bool read_head(NcRd *r, NcStateInfo *info, uint32_t *nsecs)
{
    if (nc_rd_u32(r) != NC_STATE_MAGIC || nc_rd_u32(r) != NC_STATE_CONTAINER) {
        return false;
    }
    if (!get_info(r, info)) {
        return false;
    }
    *nsecs = nc_rd_u32(r);
    return !r->fail && *nsecs < 256;
}

bool nc_state_read_info(const char *path, NcStateInfo *info)
{
    size_t len = 0;
    const uint8_t *p = map_file(path, &len);
    if (!p) {
        return false;
    }
    NcRd r = {p, len, 0, false};
    uint32_t n;
    const bool ok = read_head(&r, info, &n);
    munmap((void *)p, len);
    return ok;
}

static const char *read_state(const char *path, NcStateInfo *info, const NcStateSection *secs, unsigned nsecs,
                              bool restore);

const char *nc_state_read_section(const char *path, uint32_t tag, bool (*fn)(NcRd *r, unsigned v, void *ctx),
                                  void *ctx)
{
    size_t len = 0;
    const uint8_t *p = map_file(path, &len);
    if (!p) {
        return "can't read the state";
    }
    NcRd r = {p, len, 0, false};
    NcStateInfo info;
    uint32_t n = 0;
    const char *err = read_head(&r, &info, &n) ? NULL : "not a state";
    const SecEntry *tab = err ? NULL : nc_rd_ptr(&r, (size_t)n * sizeof(SecEntry));
    bool found = false;
    for (uint32_t i = 0; tab && !err && i < n; i++) {
        if (tab[i].tag != tag) {
            continue;
        }
        found = true;
        if (tab[i].off > len || tab[i].len > len - tab[i].off) {
            err = "cut short";
            break;
        }
        NcRd sr = {p + tab[i].off, (size_t)tab[i].len, 0, false};
        if (!fn(&sr, tab[i].version, ctx)) {
            err = "the section can't be read";
        }
    }
    munmap((void *)p, len);
    (void)found;                            /* a state without it: nothing of it */
    return err;
}

const char *nc_state_read(const char *path, NcStateInfo *info, const NcStateSection *secs, unsigned nsecs)
{
    return read_state(path, info, secs, nsecs, true);
}

const char *nc_state_verify(const char *path, NcStateInfo *info, const NcStateSection *secs, unsigned nsecs)
{
    return read_state(path, info, secs, nsecs, false);
}

static const char *read_state(const char *path, NcStateInfo *info, const NcStateSection *secs, unsigned nsecs,
                              bool restore)
{
    static char why[256];
    size_t len = 0;
    const uint8_t *p = map_file(path, &len);
    if (!p) {
        snprintf(why, sizeof(why), "can't read %s", path);
        return why;
    }
    NcRd r = {p, len, 0, false};
    uint32_t n = 0;
    const char *err = NULL;
    g_fail[0] = 0;
    if (!read_head(&r, info, &n)) {
        err = "not a state this core can read (its container changed)";
    }
    const SecEntry *tab = err ? NULL : nc_rd_ptr(&r, (size_t)n * sizeof(SecEntry));
    if (!err && !tab) {
        err = "the state is cut short";
    }
    /* every section checked before any is restored */
    for (uint32_t i = 0; !err && i < n; i++) {
        const NcStateSection *s = NULL;
        for (unsigned k = 0; k < nsecs && !s; k++) {
            s = secs[k].tag == tab[i].tag ? &secs[k] : NULL;
        }
        if (!s) {
            snprintf(why, sizeof(why), "the state has a section this core doesn't know (%.4s)",
                     (const char *)&tab[i].tag);
            err = why;
        } else if (tab[i].version > s->version) {
            snprintf(why, sizeof(why), "made by a newer core (%s): its %s state is version %u, this core "
                     "reads up to %u", info->core_version, s->name, tab[i].version, s->version);
            err = why;
        } else if (tab[i].version < s->min) {
            snprintf(why, sizeof(why), "made by an older core (%s): its %s state is version %u, this "
                     "core reads %u and up", info->core_version, s->name, tab[i].version, s->min);
            err = why;
        } else if (tab[i].off > len || tab[i].len > len - tab[i].off) {
            err = "the state is cut short";
        } else if ((uint32_t)crc32(0, p + tab[i].off, (uInt)tab[i].len) != tab[i].crc) {
            snprintf(why, sizeof(why), "the state's %s section is damaged", s->name);
            err = why;
        }
    }
    for (unsigned k = 0; !err && k < nsecs; k++) {
        bool found = false;
        for (uint32_t i = 0; i < n && !found; i++) {
            found = tab[i].tag == secs[k].tag;
        }
        if (!found && secs[k].optional) {
            nc_log("hakuX-native-state", "the state has no %s section (older than it): loaded without", secs[k].name);
        } else if (!found) {
            snprintf(why, sizeof(why), "the state has no %s section", secs[k].name);
            err = why;
        }
    }
    /* in the order this core lists them (the RAM before what refers to it) */
    for (unsigned k = 0; restore && !err && k < nsecs; k++) {
        bool found = false;
        for (uint32_t i = 0; i < n; i++) {
            if (tab[i].tag != secs[k].tag) {
                continue;
            }
            found = true;
            NcRd sr = {p + tab[i].off, (size_t)tab[i].len, 0, false};
            if (tab[i].version != secs[k].version) {
                nc_log("hakuX-native-state", "%s: version %u read by a version %u core", secs[k].name,
                       tab[i].version, secs[k].version);
            }
            if (!secs[k].load(&sr, tab[i].version) || sr.fail) {
                if (g_fail[0]) {
                    snprintf(why, sizeof(why), "%s", g_fail);
                } else {
                    snprintf(why, sizeof(why), "the %s state could not be restored", secs[k].name);
                }
                err = why;
            }
            break;
        }
        if (!found && secs[k].absent) {
            secs[k].absent();
        }
    }
    munmap((void *)p, len);
    return err;
}
