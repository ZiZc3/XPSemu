/*
 * XPSemu: Sega Chihiro game images (patch 184, Chihiro phase 1). A game is the media board's DIMM image: a FATX
 * volume holding boot.id and the game's XBE. It comes as
 *   - a netboot image (.bin, the FATX volume as it is), or
 *   - a GD-ROM dump (.cue or .gdi): its high-density track is ISO9660 (from LBA 45000, 2352-byte sectors) with the
 *     DIMM image as a file, DES-encrypted with the key in the game's security PIC (MAME's naomigd.cpp: a 80-byte
 *     .data file, or a 16 KiB .pic dump, beside the game or under /data/xpsemu/chihiro). Decrypted in place, block
 *     by block, as it is read (chihiro_des.c, BSD-3-Clause, MAME / Réda Chérif-Touil).
 * Phase 1 reads a game's boot.id for the Chihiro page; phase 2 boots from the same reader.
 * Our own code. GPL-3.0-or-later.
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chihiro_ps5.h"
#include <pthread.h>
#include <libchdr/chd.h>
/* the zip reader's inflate: miniz's, in libchdr's build (patch 221) */
size_t tinfl_decompress_mem_to_mem(void *out, size_t out_len, const void *in, size_t in_len, int flags);

void chihiro_des_decrypt_buffer(uint8_t *buf, uint32_t len, uint64_t key);

#define GD_HD_LBA 45000u        /* the GD-ROM's high-density area starts here */

typedef struct Img {
    int fd;
    bool cd;                    /* 2352-byte sectors (a GD-ROM track), else bytes as they are */
    uint64_t file_lba;          /* the track file's first LBA (cd) */
    uint64_t start;             /* the DIMM image's first byte: in the track's 2048-byte data (cd) or the file */
    uint64_t size;
    bool encrypted;
    uint64_t key;
    /* patch 221: a MAME GD-ROM CHD instead of a track file: the high-density track's first frame in it, and the
     * last hunk read (libchdr isn't for two threads at once: a lock) */
    chd_file *chd;
    uint64_t chd_frame0;
    uint32_t hunk_frames, unit;
    uint8_t *hunk;
    int64_t hunk_no;
    pthread_mutex_t chd_lock;
} Img;

/* count raw 2352-byte sectors of the track from sector on (a track file, or the CHD's frames) */
static bool pread_all(int fd, void *buf, size_t len, uint64_t off);
static bool track_raw(const Img *im_c, uint8_t *raw, size_t count, uint64_t sector)
{
    if (!im_c->chd) {
        return pread_all(im_c->fd, raw, count * 2352, sector * 2352);
    }
    Img *im = (Img *)im_c;
    bool ok = true;
    pthread_mutex_lock(&im->chd_lock);
    for (size_t i = 0; i < count && ok; i++) {
        const uint64_t frame = im->chd_frame0 + sector + i;
        const int64_t h = (int64_t)(frame / im->hunk_frames);
        if (h != im->hunk_no) {
            ok = chd_read(im->chd, (uint32_t)h, im->hunk) == CHDERR_NONE;
            im->hunk_no = ok ? h : -1;
        }
        if (ok) {
            memcpy(raw + i * 2352, im->hunk + (frame % im->hunk_frames) * im->unit, 2352);
        }
    }
    pthread_mutex_unlock(&im->chd_lock);
    return ok;
}

static bool pread_all(int fd, void *buf, size_t len, uint64_t off)
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

/* data bytes of a GD-ROM track: off counts 2048-byte sectors from the track file's first LBA */
static bool cd_read(const Img *im, void *buf, size_t len, uint64_t off)
{
    /* up to 64 sectors (147 KB raw) a host read, their 2048 data bytes picked out (patch 188: one read a sector
     * made the first start's unpacking slow) */
    uint8_t *raw = malloc(64 * 2352);      /* each call its own: the page's scan and the unpacking may read at once */
    if (!raw) {
        return false;
    }
    uint8_t *to = buf;
    bool ok = true;
    while (len && ok) {
        const uint64_t sector = off / 2048;
        const size_t in = (size_t)(off % 2048);
        size_t count = (in + len + 2047) / 2048;
        if (count > 64) {
            count = 64;
        }
        if (!track_raw(im, raw, count, sector)) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < count && len; i++) {
            const size_t skip = i == 0 ? in : 0;
            const size_t take = len < 2048 - skip ? len : 2048 - skip;
            memcpy(to, raw + i * 2352 + 16 + skip, take);
            to += take;
            off += take;
            len -= take;
        }
    }
    free(raw);
    return ok;
}

/* raw (still encrypted) bytes of the DIMM image */
static bool raw_read(const Img *im, void *buf, size_t len, uint64_t off)
{
    return im->cd ? cd_read(im, buf, len, im->start + off) : pread_all(im->fd, buf, len, im->start + off);
}

/* the DIMM image's bytes, decrypted: whole 8-byte DES blocks around the range */
bool rx_chihiro_img_read(const void *handle, void *buf, size_t len, uint64_t off)
{
    const Img *im = handle;
    if (off > im->size || len > im->size - off) {
        return false;
    }
    if (!im->encrypted) {
        return raw_read(im, buf, len, off);
    }
    const uint64_t a = off & ~7ull, b = (off + len + 7) & ~7ull;
    uint8_t stack[4096];
    uint8_t *tmp = b - a <= sizeof(stack) ? stack : malloc((size_t)(b - a));
    if (!tmp) {
        return false;
    }
    bool ok = raw_read(im, tmp, (size_t)(b - a), a);
    if (ok) {
        chihiro_des_decrypt_buffer(tmp, (uint32_t)(b - a), im->key);
        memcpy(buf, tmp + (off - a), len);
    }
    if (tmp != stack) {
        free(tmp);
    }
    return ok;
}

/* ---- keys: MAME naomigd.cpp's reading of the security PIC ---- */

static bool key_of_bytes(const uint8_t *d, size_t n, uint64_t *key);

static bool key_of_file(const char *path, uint64_t *key)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    uint8_t d[16384];
    const ssize_t n = read(fd, d, sizeof(d));
    close(fd);
    return n > 0 && key_of_bytes(d, (size_t)n, key);
}

static bool key_of_bytes(const uint8_t *d, size_t n, uint64_t *key)
{
    uint64_t k = 0;
    if (n == 80) {                  /* .data: bytes 0x31..0x37, then 0x29 */
        for (int i = 0; i < 7; i++) {
            k = (k << 8) | d[0x31 + i];
        }
        k = (k << 8) | d[0x29];
    } else if (n == 16384) {        /* .pic: pic[0x780 + 2i], then pic[0x7A0] */
        for (int i = 0; i < 7; i++) {
            k = (k << 8) | d[0x780 + 2 * i];
        }
        k = (k << 8) | d[0x7A0];
    } else {
        return false;
    }
    *key = k;
    return true;
}

static uint64_t bswap64(uint64_t v)
{
    return __builtin_bswap64(v);
}

/* A key fits when the image's first block decrypts to "FATX" (either byte order of the key is tried). */
static bool key_fits(Img *im, uint64_t key)
{
    uint8_t blk[8];
    for (int order = 0; order < 2; order++) {
        if (!raw_read(im, blk, 8, 0)) {
            return false;
        }
        const uint64_t k = order ? bswap64(key) : key;
        chihiro_des_decrypt_buffer(blk, 8, k);
        if (!memcmp(blk, "FATX", 4)) {
            im->key = k;
            return true;
        }
    }
    return false;
}

/* patch 221: the .pic/.data in a small .zip (stored or deflated), each tried */
static uint32_t zrd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t zrd32(const uint8_t *p) { return zrd16(p) | zrd16(p + 2) << 16; }

static bool keys_in_zip(Img *im, const char *path)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    uint8_t *z = malloc(1 << 20);
    const ssize_t len = z ? read(fd, z, 1 << 20) : -1;
    close(fd);
    if (len < 22) {
        free(z);
        return false;
    }
    bool ok = false;
    ssize_t eocd = -1;
    for (ssize_t i = len - 22; i >= 0 && len - i < 70000; i--) {
        if (zrd32(z + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        free(z);
        return false;
    }
    const unsigned count = zrd16(z + eocd + 10);
    size_t at = zrd32(z + eocd + 16);
    for (unsigned k = 0; k < count && at + 46 <= (size_t)len && zrd32(z + at) == 0x02014b50u; k++) {
        const unsigned method = zrd16(z + at + 10), nlen = zrd16(z + at + 28), xlen = zrd16(z + at + 30),
                       clen = zrd16(z + at + 32);
        const uint32_t csize = zrd32(z + at + 20), usize = zrd32(z + at + 24), local = zrd32(z + at + 42);
        const char *name = (const char *)z + at + 46;
        at += 46 + nlen + xlen + clen;
        const bool keyname = (nlen > 5 && !strncasecmp(name + nlen - 5, ".data", 5)) ||
                             (nlen > 4 && !strncasecmp(name + nlen - 4, ".pic", 4));
        if (!keyname || usize > 16384 || (size_t)local + 30 > (size_t)len || zrd32(z + local) != 0x04034b50u) {
            continue;
        }
        const size_t data = local + 30 + zrd16(z + local + 26) + zrd16(z + local + 28);
        if (data + csize > (size_t)len) {
            continue;
        }
        uint8_t out[16384];
        size_t got = 0;
        if (method == 0 && csize == usize) {
            memcpy(out, z + data, usize);
            got = usize;
        } else if (method == 8) {
            got = tinfl_decompress_mem_to_mem(out, usize, z + data, csize, 0);
        }
        uint64_t key;
        if (got == usize && key_of_bytes(out, got, &key) && key_fits(im, key)) {
            ok = true;
            break;
        }
    }
    free(z);
    return ok;
}

static bool try_keys_in(Img *im, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        const size_t n = strlen(e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) {
                found = try_keys_in(im, path, depth - 1);
            }
        } else if ((n > 5 && !strcasecmp(e->d_name + n - 5, ".data")) || (n > 4 && !strcasecmp(e->d_name + n - 4, ".pic"))) {
            uint64_t key;
            found = key_of_file(path, &key) && key_fits(im, key);
        } else if (n > 4 && !strcasecmp(e->d_name + n - 4, ".zip") && st.st_size <= (1 << 20)) {
            found = keys_in_zip(im, path);      /* a MAME set's key zip (crtaxihr.zip: 317-0353-com.pic) */
        }
    }
    closedir(d);
    return found;
}

/* ---- GD-ROM: the high-density track and the DIMM image in its ISO9660 ---- */

static void dir_of(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash) {
        *slash = 0;
    } else {
        snprintf(out, cap, ".");
    }
}

/* .cue: the last MODE1 track's file (the high-density area); .gdi: the track starting at LBA 45000 */
static bool gd_track_path(const char *sheet, char *out, size_t cap)
{
    FILE *f = fopen(sheet, "r");
    if (!f) {
        return false;
    }
    char dir[1024];
    dir_of(sheet, dir, sizeof(dir));
    const size_t n = strlen(sheet);
    const bool gdi = n > 4 && !strcasecmp(sheet + n - 4, ".gdi");
    char line[1024], file[512] = "", pick[512] = "";
    while (fgets(line, sizeof(line), f)) {
        if (gdi) {
            unsigned num, lba, type, sector;
            char name[512];
            if (sscanf(line, "%u %u %u %u \"%511[^\"]\"", &num, &lba, &type, &sector, name) == 5 ||
                sscanf(line, "%u %u %u %u %511s", &num, &lba, &type, &sector, name) == 5) {
                if (lba == GD_HD_LBA && type == 4 && sector == 2352) {
                    snprintf(pick, sizeof(pick), "%s", name);
                }
            }
        } else {
            const char *q = strstr(line, "FILE \"");
            if (q) {
                q += 6;
                const char *end = strchr(q, '"');
                if (end) {
                    snprintf(file, sizeof(file), "%.*s", (int)(end - q), q);
                }
            } else if (strstr(line, "TRACK") && strstr(line, "MODE1/2352") && file[0]) {
                snprintf(pick, sizeof(pick), "%s", file);
            }
        }
    }
    fclose(f);
    if (!pick[0]) {
        return false;
    }
    snprintf(out, cap, "%s/%s", dir, pick);
    return true;
}

typedef struct IsoFile { char name[64]; uint32_t lba; uint32_t size; } IsoFile;

/* the root directory's files, LBAs absolute (the track's first LBA is 45000) */
static int iso_root(Img *im, IsoFile *out, int cap)
{
    uint8_t pvd[2048];
    if (!cd_read(im, pvd, sizeof(pvd), (uint64_t)16 * 2048) || memcmp(pvd + 1, "CD001", 5)) {
        return 0;
    }
    uint32_t root_lba, root_size;
    memcpy(&root_lba, pvd + 156 + 2, 4);
    memcpy(&root_size, pvd + 156 + 10, 4);
    if (root_lba < GD_HD_LBA || root_size > 1u << 20) {
        return 0;
    }
    uint8_t *d = malloc(root_size);
    if (!d || !cd_read(im, d, root_size, (uint64_t)(root_lba - GD_HD_LBA) * 2048)) {
        free(d);
        return 0;
    }
    int count = 0;
    for (uint32_t i = 0; i < root_size && count < cap;) {
        const uint8_t len = d[i];
        if (!len) {
            i = (i / 2048 + 1) * 2048;
            continue;
        }
        if (i + 33 > root_size || i + len > root_size) {
            break;
        }
        const uint8_t nlen = d[i + 32];
        if (nlen > 1 && !(d[i + 25] & 2)) {     /* a file, not "." / ".." or a directory */
            IsoFile *f = &out[count++];
            memcpy(&f->lba, d + i + 2, 4);
            memcpy(&f->size, d + i + 10, 4);
            snprintf(f->name, sizeof(f->name), "%.*s", nlen < 63 ? nlen : 63, (const char *)d + i + 33);
            char *semi = strchr(f->name, ';');
            if (semi) {
                *semi = 0;
            }
        }
        i += len;
    }
    free(d);
    return count;
}

/* ---- opening ---- */

/* the PS5's libc has no strcasestr (it crashed the Chihiro page: an unbound import, patch 185) */
static bool contains_nocase(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (!strncasecmp(hay, needle, n)) {
            return true;
        }
    }
    return false;
}

static bool has_ext(const char *path, const char *ext)
{
    const size_t n = strlen(path), e = strlen(ext);
    return n > e && !strcasecmp(path + n - e, ext);
}

void *rx_chihiro_img_open(const char *path, char *why, size_t why_cap)
{
    Img *im = calloc(1, sizeof(*im));
    if (!im) {
        return NULL;
    }
    im->fd = -1;
    im->hunk_no = -1;
    pthread_mutex_init(&im->chd_lock, NULL);
    if (has_ext(path, ".chd")) {
        /* patch 221: a MAME GD-ROM CHD (gdx-0002b.chd): the high-density track (LBA 45000) found as the frame
         * whose 16th sector holds the ISO9660 volume descriptor, around the tracks before it (each padded to
         * 4 frames, or not) */
        if (chd_open(path, CHD_OPEN_READ, NULL, &im->chd) != CHDERR_NONE) {
            snprintf(why, why_cap, "not a CHD it can read");
            goto fail;
        }
        const chd_header *h = chd_get_header(im->chd);
        im->unit = h->unitbytes;
        if (im->unit < 2352 || !h->hunkbytes || h->hunkbytes % im->unit) {
            snprintf(why, why_cap, "not a CD/GD-ROM CHD");
            goto fail;
        }
        im->hunk_frames = h->hunkbytes / im->unit;
        im->hunk = malloc(h->hunkbytes);
        if (!im->hunk) {
            goto fail;
        }
        uint64_t plain = 0, padded = 0;
        for (int t = 0; t < 2; t++) {
            char meta[256];
            uint32_t ml, tag;
            uint8_t fl;
            int frames = 0;
            if (chd_get_metadata(im->chd, GDROM_TRACK_METADATA_TAG, (uint32_t)t, meta, sizeof(meta) - 1, &ml, &tag, &fl) !=
                    CHDERR_NONE &&
                chd_get_metadata(im->chd, CDROM_TRACK_METADATA2_TAG, (uint32_t)t, meta, sizeof(meta) - 1, &ml, &tag, &fl) !=
                    CHDERR_NONE) {
                break;
            }
            meta[ml < sizeof(meta) - 1 ? ml : sizeof(meta) - 1] = 0;
            const char *f = strstr(meta, "FRAMES:");
            frames = f ? atoi(f + 7) : 0;
            plain += (uint64_t)frames;
            padded += ((uint64_t)frames + 3) & ~3ull;
        }
        im->cd = true;
        const uint64_t tries[] = { plain, padded, GD_HD_LBA, GD_HD_LBA + 150 };
        bool found = false;
        for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]) && !found; i++) {
            uint8_t pvd[2048];
            im->chd_frame0 = tries[i];
            found = cd_read(im, pvd, sizeof(pvd), (uint64_t)16 * 2048) && !memcmp(pvd + 1, "CD001", 5);
        }
        if (!found) {
            snprintf(why, why_cap, "no high-density track in it");
            goto fail;
        }
    }
    if (im->chd || has_ext(path, ".cue") || has_ext(path, ".gdi")) {
        if (!im->chd) {
            char track[1024];
            if (!gd_track_path(path, track, sizeof(track))) {
                snprintf(why, why_cap, "no high-density track in it");
                goto fail;
            }
            im->fd = open(track, O_RDONLY);
            if (im->fd < 0) {
                snprintf(why, why_cap, "can't open its track %s", track);
                goto fail;
            }
        }
        im->cd = true;
        im->file_lba = GD_HD_LBA;
        IsoFile files[32];
        const int n = iso_root(im, files, 32);
        int pick = -1;
        for (int i = 0; i < n; i++) {      /* the 512 MB board's image, else the largest */
            if (contains_nocase(files[i].name, "512")) {      /* OUTRUN2_512MB.BIN, V307_S.512 */
                pick = i;
                break;
            }
            if (pick < 0 || files[i].size > files[pick].size) {
                pick = i;
            }
        }
        if (pick < 0 || files[pick].size < (1u << 20)) {
            snprintf(why, why_cap, "no game image on the disc");
            goto fail;
        }
        im->start = (uint64_t)(files[pick].lba - GD_HD_LBA) * 2048;
        im->size = files[pick].size;
        uint8_t head[4];
        if (!raw_read(im, head, 4, 0)) {
            snprintf(why, why_cap, "can't read the disc");
            goto fail;
        }
        if (memcmp(head, "FATX", 4)) {
            im->encrypted = true;
            char dir[1024], parent[1024];
            dir_of(path, dir, sizeof(dir));
            dir_of(dir, parent, sizeof(parent));
            if (!try_keys_in(im, dir, 1) && !try_keys_in(im, parent, 1) && !try_keys_in(im, "/data/xpsemu/chihiro", 2)) {
                snprintf(why, why_cap, "locked: put its security key (.data or .pic) beside it or in /data/xpsemu/chihiro");
                goto fail;
            }
        }
    } else {
        im->fd = open(path, O_RDONLY);
        if (im->fd < 0) {
            snprintf(why, why_cap, "can't open it");
            goto fail;
        }
        struct stat st;
        uint8_t head[4];
        if (fstat(im->fd, &st) != 0 || !pread_all(im->fd, head, 4, 0) || memcmp(head, "FATX", 4)) {
            snprintf(why, why_cap, "not a Chihiro netboot image");
            goto fail;
        }
        im->size = (uint64_t)st.st_size;
    }
    return im;
fail:
    if (im->fd >= 0) {
        close(im->fd);
    }
    if (im->chd) {
        chd_close(im->chd);
    }
    free(im->hunk);
    free(im);
    return NULL;
}

void rx_chihiro_img_close(void *handle)
{
    Img *im = handle;
    if (im) {
        if (im->fd >= 0) {
            close(im->fd);
        }
        if (im->chd) {
            chd_close(im->chd);
        }
        free(im->hunk);
        free(im);
    }
}

uint64_t rx_chihiro_img_size(const void *handle)
{
    return ((const Img *)handle)->size;
}

/* ---- FATX: the DIMM image's volume (as on the Xbox: 4 KiB header, FAT, then clusters) ---- */

typedef struct Fatx {
    const Img *im;
    uint32_t cluster;           /* bytes */
    uint32_t root;
    uint64_t fat_at, data_at;
    bool fat32;
    uint32_t clusters;
} Fatx;

static bool fatx_layout(Fatx *fx, uint64_t volume, uint32_t extra)
{
    uint8_t h[16];
    if (!rx_chihiro_img_read(fx->im, h, sizeof(h), 0) || memcmp(h, "FATX", 4)) {
        return false;
    }
    uint32_t spc;
    memcpy(&spc, h + 8, 4);
    memcpy(&fx->root, h + 12, 4);
    if (!spc || spc > 1024 || !fx->root) {
        return false;
    }
    fx->cluster = spc * 512;
    fx->clusters = (uint32_t)(volume / fx->cluster) + extra;    /* the Xbox counts one more; OutRun 2's DIMM image doesn't */
    fx->fat32 = fx->clusters >= 0xFFF0;
    const uint64_t fat_size = (((uint64_t)fx->clusters * (fx->fat32 ? 4 : 2)) + 4095) & ~4095ull;
    fx->fat_at = 4096;
    fx->data_at = 4096 + fat_size;
    return true;
}

static uint32_t fatx_next(const Fatx *fx, uint32_t c)
{
    if (fx->fat32) {
        uint32_t v = 0xFFFFFFFF;
        rx_chihiro_img_read(fx->im, &v, 4, fx->fat_at + (uint64_t)c * 4);
        return v >= 0xFFFFFFF0 ? 0 : v;
    }
    uint16_t v = 0xFFFF;
    rx_chihiro_img_read(fx->im, &v, 2, fx->fat_at + (uint64_t)c * 2);
    return v >= 0xFFF0 ? 0 : v;
}

static uint64_t fatx_cluster_at(const Fatx *fx, uint32_t c)
{
    return fx->data_at + (uint64_t)(c - 1) * fx->cluster;
}

/* a file in the root directory: its first cluster and size */
static bool fatx_find(const Fatx *fx, const char *name, uint32_t *first, uint32_t *size)
{
    uint8_t *buf = malloc(fx->cluster);
    if (!buf) {
        return false;
    }
    bool found = false;
    for (uint32_t c = fx->root, hops = 0; c && hops < 256 && !found; c = fatx_next(fx, c), hops++) {
        if (!rx_chihiro_img_read(fx->im, buf, fx->cluster, fatx_cluster_at(fx, c))) {
            break;
        }
        for (uint32_t i = 0; i + 64 <= fx->cluster; i += 64) {
            const uint8_t len = buf[i];
            if (len == 0x00 || len == 0xFF) {
                c = 0;
                break;
            }
            if (len == 0xE5 || len > 42) {
                continue;
            }
            if (len == strlen(name) && !strncasecmp((const char *)buf + i + 2, name, len)) {
                memcpy(first, buf + i + 44, 4);
                memcpy(size, buf + i + 48, 4);
                found = true;
                break;
            }
        }
        if (!c) {
            break;
        }
    }
    free(buf);
    return found;
}

static bool fatx_read_file(const Fatx *fx, uint32_t first, uint32_t size, uint8_t *out, uint32_t cap)
{
    uint32_t done = 0;
    for (uint32_t c = first; c && done < size && done < cap; c = fatx_next(fx, c)) {
        uint32_t take = fx->cluster;
        if (take > size - done) take = size - done;
        if (take > cap - done) take = cap - done;
        if (!rx_chihiro_img_read(fx->im, out + done, take, fatx_cluster_at(fx, c))) {
            return false;
        }
        done += take;
    }
    return done == size || done == cap;
}

/* the volume's size isn't written down: the FAT follows from it. The image's own size first, then the boards'
 * DIMM sizes; the right one finds boot.id in the root. */
static bool fatx_open(Fatx *fx, const Img *im, uint32_t *bid_first, uint32_t *bid_size)
{
    const uint64_t sizes[] = { im->size, 128ull << 20, 256ull << 20, 512ull << 20, 1024ull << 20, 2048ull << 20 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (uint32_t extra = 0; extra < 2; extra++) {
            memset(fx, 0, sizeof(*fx));
            fx->im = im;
            if (fatx_layout(fx, sizes[i], extra) && fatx_find(fx, "boot.id", bid_first, bid_size)) {
                return true;
            }
        }
    }
    /* Netboot images (patch 218): formatted for a volume a little under the DIMM (House of the Dead 3's and Virtua
     * Cop 3's "1gb" images: a 16-bit FAT of 0x1F000 bytes, the root at 0x20000), then cut to the data: every FAT
     * size, 4 KiB at a time, both FAT kinds, until boot.id is in the root */
    /* The FAT's width read from the FAT (patch 222): in a 32-bit one a chain's entries have a zero high half
     * (04 00 00 00), in a 16-bit one they follow each other (04 00 05 00). The size rule doesn't hold here: the
     * "1gb" netboot images have 65536 16-bit entries; Maximum Tune's disc a 32-bit FAT the 16-bit view also
     * found boot.id in (its chains then broken). */
    int width32 = -1;
    {
        uint8_t fat4k[4096];
        if (rx_chihiro_img_read(im, fat4k, sizeof(fat4k), 4096)) {
            unsigned used = 0, zero_hi = 0;
            for (size_t i = 8; i + 4 <= sizeof(fat4k); i += 4) {
                const uint32_t w = fat4k[i] | fat4k[i + 1] << 8 | (uint32_t)fat4k[i + 2] << 16 | (uint32_t)fat4k[i + 3] << 24;
                const uint32_t lo = w & 0xFFFF, hi = w >> 16;
                if (lo != 0 && lo != 0xFFFF) {
                    used++;
                    zero_hi += hi == 0;
                }
            }
            if (used >= 8) {
                width32 = zero_hi * 10 >= used * 9;
            }
        }
    }
    for (uint64_t fat = 4096; fat <= (8ull << 20); fat += 4096) {
        for (int f32 = 0; f32 < 2; f32++) {
            if (width32 >= 0 && f32 != width32) {
                continue;
            }
            memset(fx, 0, sizeof(*fx));
            fx->im = im;
            if (!fatx_layout(fx, 1, 0)) {
                return false;
            }
            fx->fat32 = f32;
            fx->clusters = (uint32_t)(fat / (f32 ? 4 : 2));
            fx->fat_at = 4096;
            fx->data_at = 4096 + fat;
            if (fx->data_at + (uint64_t)fx->root * fx->cluster > im->size) {
                return false;
            }
            if (fatx_find(fx, "boot.id", bid_first, bid_size)) {
                return true;
            }
        }
    }
    return false;
}

static void field(char *out, size_t cap, const uint8_t *p, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && p[i] && k + 1 < cap; i++) {
        out[k++] = isprint(p[i]) ? (char)p[i] : ' ';
    }
    while (k && out[k - 1] == ' ') {
        k--;
    }
    out[k] = 0;
}

bool rx_chihiro_info(const char *path, RxChihiroInfo *info)
{
    memset(info, 0, sizeof(*info));
    snprintf(info->format, sizeof(info->format), "%s",
             has_ext(path, ".chd") ? "GD-ROM (CHD)" : has_ext(path, ".cue") || has_ext(path, ".gdi") ? "GD-ROM" : "Netboot");
    void *im = rx_chihiro_img_open(path, info->why, sizeof(info->why));
    if (!im) {
        return false;
    }
    info->encrypted = ((Img *)im)->encrypted;
    Fatx fx;
    uint32_t first, size;
    uint8_t bid[0x100];
    if (!fatx_open(&fx, im, &first, &size)) {
        snprintf(info->why, sizeof(info->why), "no boot.id in its volume");
        rx_chihiro_img_close(im);
        return false;
    }
    memset(bid, 0, sizeof(bid));
    if (!fatx_read_file(&fx, first, size, bid, sizeof(bid)) || memcmp(bid, "BTID", 4)) {
        snprintf(info->why, sizeof(info->why), "its boot.id isn't one");
        rx_chihiro_img_close(im);
        return false;
    }
    field(info->game_id, sizeof(info->game_id), bid + 0x30, 8);
    field(info->maker, sizeof(info->maker), bid + 0x60, 32);
    field(info->name, sizeof(info->name), bid + 0x80, 32);
    field(info->exe, sizeof(info->exe), bid + 0xA0, 32);
    info->regions = bid[0x38];
    info->type3 = bid[0x2E] != 0;
    info->ok = true;
    rx_chihiro_img_close(im);
    return true;
}

/* ---- unpacking the volume to a host folder (phase 2, patch 187): the media board's mbfs: is served from it ---- */

static bool fatx_extract_dir(const Fatx *fx, uint32_t first, const char *dest, int depth, uint64_t *bytes, unsigned *files)
{
    if (depth > 16) {
        return false;
    }
    mkdir(dest, 0777);
    uint8_t *dir = malloc(fx->cluster);
    uint8_t *buf = malloc(fx->cluster);
    if (!dir || !buf) {
        free(dir);
        free(buf);
        return false;
    }
    bool ok = true;
    bool end = false;
    for (uint32_t c = first, hops = 0; ok && !end && c && hops < 4096; c = fatx_next(fx, c), hops++) {
        if (!rx_chihiro_img_read(fx->im, dir, fx->cluster, fatx_cluster_at(fx, c))) {
            ok = false;
            break;
        }
        for (uint32_t i = 0; ok && i + 64 <= fx->cluster; i += 64) {
            const uint8_t len = dir[i], attr = dir[i + 1];
            if (len == 0x00 || len == 0xFF) {
                end = true;
                break;
            }
            if (len == 0xE5 || len > 42) {
                continue;
            }
            char name[48], path[1024];
            memcpy(name, dir + i + 2, len);
            name[len] = 0;
            if (!strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')) {
                continue;
            }
            uint32_t fc, size;
            memcpy(&fc, dir + i + 44, 4);
            memcpy(&size, dir + i + 48, 4);
            snprintf(path, sizeof(path), "%s/%s", dest, name);
            if (attr & 0x10) {
                ok = fatx_extract_dir(fx, fc, path, depth + 1, bytes, files);
                continue;
            }
            const int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (out < 0) {
                ok = false;
                break;
            }
            uint32_t done = 0;
            for (uint32_t cc = fc; ok && cc && done < size; cc = fatx_next(fx, cc)) {
                const uint32_t take = size - done < fx->cluster ? size - done : fx->cluster;
                ok = rx_chihiro_img_read(fx->im, buf, take, fatx_cluster_at(fx, cc)) && write(out, buf, take) == (ssize_t)take;
                done += take;
            }
            close(out);
            ok = ok && done == size;
            *bytes += size;
            ++*files;
        }
    }
    free(dir);
    free(buf);
    return ok;
}

/* ---- patch 219: the volume read in place (no unpacking): each file's clusters listed once ---- */

typedef struct MFile {
    uint32_t size, n;
    uint64_t *at;               /* each cluster's place in the image */
} MFile;
static struct {
    void *im;
    Fatx fx;
    MFile *f;
    unsigned n, cap;
    uint8_t *fat;               /* the whole FAT, read once */
    uint64_t fat_len;
} g_mnt;

static uint32_t mnext(uint32_t c)
{
    if (!g_mnt.fat) {
        return fatx_next(&g_mnt.fx, c);
    }
    if (g_mnt.fx.fat32) {
        if ((uint64_t)c * 4 + 4 > g_mnt.fat_len) {
            return 0;
        }
        uint32_t v;
        memcpy(&v, g_mnt.fat + (uint64_t)c * 4, 4);
        return v >= 0xFFFFFFF0 ? 0 : v;
    }
    if ((uint64_t)c * 2 + 2 > g_mnt.fat_len) {
        return 0;
    }
    uint16_t v;
    memcpy(&v, g_mnt.fat + (uint64_t)c * 2, 2);
    return v >= 0xFFF0 ? 0 : v;
}

static void mount_dir(uint32_t first, const char *prefix, int depth, RxChihiroAdd add, void *ctx)
{
    if (depth > 16) {
        return;
    }
    const Fatx *fx = &g_mnt.fx;
    uint8_t *dir = malloc(fx->cluster);
    if (!dir) {
        return;
    }
    bool end = false;
    for (uint32_t c = first, hops = 0; !end && c && hops < 4096; c = mnext(c), hops++) {
        if (!rx_chihiro_img_read(fx->im, dir, fx->cluster, fatx_cluster_at(fx, c))) {
            break;
        }
        for (uint32_t i = 0; i + 64 <= fx->cluster; i += 64) {
            const uint8_t len = dir[i], attr = dir[i + 1];
            if (len == 0x00 || len == 0xFF) {
                end = true;
                break;
            }
            if (len == 0xE5 || len > 42) {
                continue;
            }
            char name[48], path[512];
            memcpy(name, dir + i + 2, len);
            name[len] = 0;
            if (!strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
                continue;
            }
            uint32_t fc, size;
            memcpy(&fc, dir + i + 44, 4);
            memcpy(&size, dir + i + 48, 4);
            snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "\\" : "", name);
            if (attr & 0x10) {
                mount_dir(fc, path, depth + 1, add, ctx);
                continue;
            }
            if (g_mnt.n == g_mnt.cap) {
                const unsigned cap = g_mnt.cap ? g_mnt.cap * 2 : 256;
                MFile *nf = realloc(g_mnt.f, cap * sizeof(MFile));
                if (!nf) {
                    continue;
                }
                g_mnt.f = nf;
                g_mnt.cap = cap;
            }
            MFile *m = &g_mnt.f[g_mnt.n];
            m->size = size;
            m->n = (uint32_t)(((uint64_t)size + fx->cluster - 1) / fx->cluster);
            m->at = malloc((m->n ? m->n : 1) * sizeof(uint64_t));
            if (!m->at) {
                continue;
            }
            uint32_t k = 0;
            for (uint32_t cc = fc; cc && k < m->n; cc = mnext(cc)) {
                m->at[k++] = fatx_cluster_at(fx, cc);
            }
            /* Patch 256 (House of the Dead 3): its netboot image carries a stale FAT. Every file is laid out in
             * one run of clusters (its data fills the image exactly), but the FAT chains don't follow the
             * directory: BGM.AFS (first cluster 11, 3305 needed) ends after 1, TITLE_JP_AC.SFD after 1648 of
             * 3350. Left out, the game never found its music archive and stayed on a black screen. A chain that
             * ends early goes on with the clusters that follow, when they are inside the image; Virtua Cop 3's and
             * Ghost Squad's images have no short chain, so only files that were left out before change. */
            if (k && k < m->n) {
                bool printable = true;
                for (uint32_t q = 0; q < len; q++) {
                    printable &= dir[i + 2 + q] >= 0x20 && dir[i + 2 + q] < 0x7F;
                }
                const uint64_t end = m->at[k - 1] + (uint64_t)(m->n - k + 1) * fx->cluster;
                if (printable && end <= rx_chihiro_img_size(fx->im)) {
                    fprintf(stderr, "xog: Chihiro: %s: its FAT chain ends after %u of %u clusters: the rest read from "
                            "the clusters that follow (patch 256)\n", path, k, m->n);
                    for (; k < m->n; k++) {
                        m->at[k] = m->at[k - 1] + fx->cluster;
                    }
                } else {
                    fprintf(stderr, "xog: Chihiro: %s: its FAT chain ends after %u of %u clusters: left out\n",
                            path, k, m->n);
                }
            }
            if (k < m->n) {
                free(m->at);        /* a broken chain: left out */
                continue;
            }
            add(ctx, path, size, g_mnt.n);
            g_mnt.n++;
        }
    }
    free(dir);
}

unsigned rx_chihiro_mount(const char *path, RxChihiroAdd add, void *ctx)
{
    char why[160];
    g_mnt.im = rx_chihiro_img_open(path, why, sizeof(why));
    if (!g_mnt.im) {
        fprintf(stderr, "xog: Chihiro: %s can't be read in place: %s\n", path, why);
        return 0;
    }
    uint32_t first, size;
    if (!fatx_open(&g_mnt.fx, g_mnt.im, &first, &size)) {
        fprintf(stderr, "xog: Chihiro: %s: no boot.id in its volume\n", path);
        return 0;
    }
    g_mnt.fat_len = g_mnt.fx.data_at - g_mnt.fx.fat_at;
    g_mnt.fat = g_mnt.fat_len <= (64u << 20) ? malloc(g_mnt.fat_len) : NULL;
    if (g_mnt.fat && !rx_chihiro_img_read(g_mnt.im, g_mnt.fat, g_mnt.fat_len, g_mnt.fx.fat_at)) {
        free(g_mnt.fat);
        g_mnt.fat = NULL;
    }
    mount_dir(g_mnt.fx.root, "", 0, add, ctx);
    return g_mnt.n;
}

bool rx_chihiro_mount_read(unsigned id, uint64_t off, void *buf, size_t len)
{
    if (id >= g_mnt.n || off + len > g_mnt.f[id].size) {
        return false;
    }
    const MFile *m = &g_mnt.f[id];
    const uint32_t cl = g_mnt.fx.cluster;
    uint8_t *out = buf;
    while (len) {
        const uint64_t k = off / cl, in = off % cl;
        size_t take = cl - in < len ? (size_t)(cl - in) : len;
        /* clusters that follow each other in the image: one read */
        uint64_t kk = k;
        while (take < len && kk + 1 < m->n && m->at[kk + 1] == m->at[kk] + cl) {
            kk++;
            take = (size_t)((kk - k + 1) * cl - in) < len ? (size_t)((kk - k + 1) * cl - in) : len;
        }
        if (!rx_chihiro_img_read(g_mnt.im, out, take, m->at[k] + in)) {
            return false;
        }
        out += take;
        off += take;
        len -= take;
    }
    return true;
}

/* Only boot.id out to dest (the boards' region and the drive board read it there): the rest is read in place. */
bool rx_chihiro_prepare(const char *path, const char *dest, char *why, size_t why_cap)
{
    void *im = rx_chihiro_img_open(path, why, why_cap);
    if (!im) {
        return false;
    }
    Fatx fx;
    uint32_t first, size;
    if (!fatx_open(&fx, im, &first, &size) || size > 65536) {
        snprintf(why, why_cap, "no boot.id in its volume");
        rx_chihiro_img_close(im);
        return false;
    }
    uint8_t *bid = malloc(65536);
    const bool got = bid && fatx_read_file(&fx, first, size, bid, 65536);
    rx_chihiro_img_close(im);
    if (!got) {
        free(bid);
        snprintf(why, why_cap, "its boot.id can't be read");
        return false;
    }
    mkdir(dest, 0777);
    char out[1024];
    snprintf(out, sizeof(out), "%s/boot.id", dest);
    const int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    const bool ok = fd >= 0 && write(fd, bid, size) == (ssize_t)size;
    if (fd >= 0) {
        close(fd);
    }
    free(bid);
    if (!ok) {
        snprintf(why, why_cap, "can't write %s (storage full?)", out);
    }
    return ok;
}

/* The game's volume unpacked into dest (once: dest/.unpacked marks it done). */
bool rx_chihiro_unpack(const char *path, const char *dest, char *why, size_t why_cap)
{
    char mark[1024];
    snprintf(mark, sizeof(mark), "%s/.unpacked", dest);
    struct stat st;
    if (stat(mark, &st) == 0) {
        return true;
    }
    void *im = rx_chihiro_img_open(path, why, why_cap);
    if (!im) {
        return false;
    }
    Fatx fx;
    uint32_t first, size;
    if (!fatx_open(&fx, im, &first, &size)) {
        snprintf(why, why_cap, "no boot.id in its volume");
        rx_chihiro_img_close(im);
        return false;
    }
    uint64_t bytes = 0;
    unsigned files = 0;
    const bool ok = fatx_extract_dir(&fx, fx.root, dest, 0, &bytes, &files);
    rx_chihiro_img_close(im);
    if (!ok) {
        snprintf(why, why_cap, "unpacking stopped after %u files (storage full?)", files);
        return false;
    }
    const int m = open(mark, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (m >= 0) {
        char line[64];
        const int n = snprintf(line, sizeof(line), "%u files, %llu bytes\n", files, (unsigned long long)bytes);
        if (write(m, line, (size_t)n) != n) {
            /* only a note */
        }
        close(m);
    }
    return true;
}
