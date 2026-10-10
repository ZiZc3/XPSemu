/*
 * XPSemu: removes broken saves from the Xbox hard disk's FATX E: partition
 * (see xemu-save-repair.h).
 *
 * FATX: a superblock (4 KB: "FATX", volume ID, sectors per cluster, root
 * directory cluster), then the FAT (16-bit entries for fewer than 65520
 * clusters, else 32-bit; rounded up to 4 KB), then the clusters from 1.
 * A directory is a cluster chain of 64-byte entries: name length (0x00 or
 * 0xFF ends the list, 0xE5 is a deleted entry), attributes, 42 name bytes,
 * first cluster, size, times.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XSR_TEST
#include "qemu/osdep.h"
#include "system/block-backend.h"
#endif
#include "xemu-save-repair.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define XSR_ATTR_DIR  0x10
#define XSR_DELETED   0xE5
#define XSR_MAX_CHAIN 256  /* clusters followed in one directory */
#define XSR_MAX_FOUND 16   /* broken folders removed at once */

typedef struct Fatx {
    const XsrDisk *disk;
    uint64_t part_off, fat_off, data_off;
    uint32_t cluster_size, clusters, fat_entry;
} Fatx;

typedef struct Entry {
    char name[43];
    uint8_t attr;
    uint32_t first, size;
    uint64_t at; /* Where the entry is on the disk */
} Entry;

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool fat_get(const Fatx *f, uint32_t c, uint32_t *next)
{
    uint8_t b[4] = { 0 };
    if (c >= f->clusters + 1 ||
        !f->disk->read(f->disk->opaque, f->fat_off + (uint64_t)c * f->fat_entry,
                       b, f->fat_entry)) {
        return false;
    }
    *next = f->fat_entry == 4 ? le32(b) : (uint32_t)(b[0] | b[1] << 8);
    return true;
}

static bool chain_end(const Fatx *f, uint32_t v)
{
    return f->fat_entry == 4 ? v >= 0xFFFFFFF0u : v >= 0xFFF0u;
}

/* The clusters of a chain (a directory); false if it looks broken */
static bool chain(const Fatx *f, uint32_t first, uint32_t *out, int *n)
{
    *n = 0;
    for (uint32_t c = first; *n < XSR_MAX_CHAIN;) {
        if (c < 1 || c > f->clusters) {
            return false;
        }
        out[(*n)++] = c;
        uint32_t next;
        if (!fat_get(f, c, &next)) {
            return false;
        }
        if (chain_end(f, next)) {
            return true;
        }
        c = next;
    }
    return false;
}

/* The live entries of a directory; -1 if it can't be read */
static int list(const Fatx *f, uint32_t first, Entry *out, int max)
{
    uint32_t clusters[XSR_MAX_CHAIN];
    int nc, n = 0;
    if (!chain(f, first, clusters, &nc)) {
        return -1;
    }
    uint8_t *buf = malloc(f->cluster_size);
    if (!buf) {
        return -1;
    }
    for (int i = 0; i < nc; i++) {
        uint64_t at = f->data_off + (uint64_t)(clusters[i] - 1) * f->cluster_size;
        if (!f->disk->read(f->disk->opaque, at, buf, f->cluster_size)) {
            free(buf);
            return -1;
        }
        for (uint32_t o = 0; o < f->cluster_size; o += 64) {
            const uint8_t *e = buf + o;
            if (e[0] == 0x00 || e[0] == 0xFF) {
                free(buf);
                return n;
            }
            if (e[0] == XSR_DELETED) {
                continue;
            }
            if (e[0] > 42) {
                free(buf);
                return -1; /* Not a directory we understand */
            }
            if (n < max) {
                Entry *d = &out[n];
                memcpy(d->name, e + 2, e[0]);
                d->name[e[0]] = 0;
                d->attr = e[1];
                d->first = le32(e + 44);
                d->size = le32(e + 48);
                d->at = at + o;
            }
            n++;
        }
    }
    free(buf);
    return n;
}

static const Entry *find_dir(const Entry *e, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if ((e[i].attr & XSR_ATTR_DIR) && !strcasecmp(e[i].name, name)) {
            return &e[i];
        }
    }
    return NULL;
}

/* A save folder cut off while being made: only empty files, among them
 * an empty SaveMeta.xbx, and none with clusters of its own */
static bool broken_save(const Fatx *f, const Entry *dir)
{
    Entry e[16];
    int n = list(f, dir->first, e, 16);
    if (n < 1 || n > 16) {
        return false;
    }
    bool meta = false;
    for (int i = 0; i < n; i++) {
        if ((e[i].attr & XSR_ATTR_DIR) || e[i].size || e[i].first) {
            return false;
        }
        meta |= !strcasecmp(e[i].name, "SaveMeta.xbx");
    }
    return meta;
}

/* Writes @len bytes at @off, the whole sector they're in going to @undo
 * first */
static bool patch(const Fatx *f, FILE *undo, uint64_t off, const void *src,
                  size_t len)
{
    uint64_t sector = off & ~511ull;
    uint8_t buf[1024];
    size_t span = (off + len - sector + 511) & ~511ull;
    if (span > sizeof(buf) ||
        !f->disk->read(f->disk->opaque, sector, buf, span)) {
        return false;
    }
    if (undo) {
        uint64_t o = sector;
        uint32_t l = (uint32_t)span;
        if (fwrite(&o, 8, 1, undo) != 1 || fwrite(&l, 4, 1, undo) != 1 ||
            fwrite(buf, span, 1, undo) != 1 || fflush(undo)) {
            return false; /* No backup, no change */
        }
    }
    memcpy(buf + (off - sector), src, len);
    return f->disk->write(f->disk->opaque, sector, buf, span);
}

int xsr_repair_title(const XsrDisk *disk, uint64_t part_off,
                     uint64_t part_size, uint32_t title_id, FILE *undo,
                     char *names, size_t names_len)
{
    Fatx f = { .disk = disk, .part_off = part_off };
    uint8_t sb[16];
    if (names_len) {
        names[0] = 0;
    }
    if (!disk->read(disk->opaque, part_off, sb, sizeof(sb)) ||
        memcmp(sb, "FATX", 4)) {
        return -1;
    }
    uint32_t spc = le32(sb + 8), root = le32(sb + 12);
    if (spc < 1 || spc > 128 || (spc & (spc - 1))) {
        return -1;
    }
    f.cluster_size = spc * 512;
    f.clusters = (uint32_t)(part_size / f.cluster_size);
    f.fat_entry = f.clusters < 0xFFF0 ? 2 : 4;
    f.fat_off = part_off + 0x1000;
    uint64_t fat_bytes = ((uint64_t)(f.clusters + 1) * f.fat_entry + 0xFFF) &
                         ~0xFFFull;
    f.data_off = f.fat_off + fat_bytes;

    Entry top[64];
    int n = list(&f, root, top, 64);
    if (n < 0) {
        return -1;
    }
    const Entry *udata = find_dir(top, n < 64 ? n : 64, "UDATA");
    if (!udata) {
        return 0;
    }
    Entry *titles = malloc(sizeof(Entry) * 1024);
    if (!titles) {
        return -1;
    }
    n = list(&f, udata->first, titles, 1024);
    char want[9];
    snprintf(want, sizeof(want), "%08x", title_id);
    const Entry *title = n > 0 ? find_dir(titles, n < 1024 ? n : 1024, want)
                               : NULL;
    if (!title) {
        free(titles);
        return n < 0 ? -1 : 0;
    }
    Entry saves[64];
    int ns = list(&f, title->first, saves, 64);
    free(titles);
    if (ns < 0) {
        return -1;
    }

    int removed = 0;
    for (int i = 0; i < ns && i < 64 && removed < XSR_MAX_FOUND; i++) {
        if (!(saves[i].attr & XSR_ATTR_DIR) || !broken_save(&f, &saves[i])) {
            continue;
        }
        uint32_t own[XSR_MAX_CHAIN];
        int nown;
        if (!chain(&f, saves[i].first, own, &nown)) {
            continue;
        }
        /* The entry first (a stop half-way only leaks a cluster), then its
         * clusters back to free */
        uint8_t deleted = XSR_DELETED, zero[4] = { 0 };
        if (!patch(&f, undo, saves[i].at, &deleted, 1)) {
            return removed ? removed : -1;
        }
        for (int c = 0; c < nown; c++) {
            if (!patch(&f, undo,
                       f.fat_off + (uint64_t)own[c] * f.fat_entry, zero,
                       f.fat_entry)) {
                return removed + 1;
            }
        }
        removed++;
        size_t used = strlen(names);
        if (used + strlen(saves[i].name) + 2 < names_len) {
            snprintf(names + used, names_len - used, "%s%s",
                     used ? ", " : "", saves[i].name);
        }
    }
    return removed;
}

#ifndef XSR_TEST

static BlockBackend *hdd(void)
{
    return blk_by_name("ide0-hd0");
}

static bool hdd_read(void *opaque, uint64_t off, void *buf, size_t len)
{
    return blk_pread(opaque, off, len, buf, 0) >= 0;
}

static bool hdd_write(void *opaque, uint64_t off, const void *buf, size_t len)
{
    return blk_pwrite(opaque, off, len, buf, 0) >= 0;
}

int xemu_repair_saves(uint32_t title_id)
{
    BlockBackend *blk = hdd();
    if (!blk || !blk_is_inserted(blk) || blk_getlength(blk) <
                                             (int64_t)(XSR_E_OFFSET + XSR_E_SIZE)) {
        return 0;
    }
    XsrDisk disk = { blk, hdd_read, hdd_write };
    g_mkdir_with_parents("/data/xemu/save-repair", 0777);
    char *path = g_strdup_printf("/data/xemu/save-repair/%08x-%" PRId64 ".undo",
                                 title_id, g_get_real_time() / G_USEC_PER_SEC);
    FILE *undo = fopen(path, "wb");
    char names[256];
    int n = undo ? xsr_repair_title(&disk, XSR_E_OFFSET, XSR_E_SIZE, title_id,
                                    undo, names, sizeof(names))
                 : 0;
    if (undo) {
        long size = ftell(undo);
        fclose(undo);
        if (size <= 0) {
            unlink(path); /* Nothing changed */
        }
    }
    if (n > 0) {
        blk_flush(blk);
        fprintf(stderr, "XPSemu: removed %d broken save%s of %08x (%s); "
                "the old sectors are in %s\n", n, n == 1 ? "" : "s",
                title_id, names, path);
    } else if (n < 0) {
        fprintf(stderr, "XPSemu: saves of %08x not checked (the hard disk "
                "isn't laid out as expected)\n", title_id);
    }
    g_free(path);
    return n;
}

static uint64_t g_save_writes;

void xemu_note_hdd_write(uint64_t offset)
{
    if (offset >= XSR_SAVE_PARTS_FROM) {
        qatomic_inc(&g_save_writes);
    }
}

uint64_t xemu_hdd_write_count(void)
{
    return qatomic_read(&g_save_writes);
}

#endif
