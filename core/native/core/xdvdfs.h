/*
 * hakuX native core — XDVDFS (Xbox game disc filesystem) reader.
 *
 * Reads files from an Xbox disc image: an extracted XISO or a full disc dump
 * (the game partition at one of the XGD1/2/3 offsets).
 */
#ifndef NATIVE_CORE_XDVDFS_H
#define NATIVE_CORE_XDVDFS_H

#include "native/core/nc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_XDVDFS_SECTOR 2048u

/* Mods (native/core/mods.c): host files over the disc's and byte patches of
 * its files. An overlay file or folder's sector is NC_DISC_OVERLAY plus its
 * index in the overlay, sorted by path (the same whatever the mods' order). */
#define NC_DISC_OVERLAY 0xF0000000u

typedef struct NcDiscOverlay NcDiscOverlay;
struct NcDirCache;

typedef struct NcDisc {
    int fd;
    uint64_t part_off;     /* byte offset of the game partition in the image */
    uint32_t root_sector;
    uint32_t root_size;
    NcDiscOverlay *ov;     /* NULL: the disc as it is */
    struct NcDirCache *dirs; /* the folders' tables in memory (nc_disc_cache_folders), or NULL */
} NcDisc;

typedef struct NcDiscFile {
    uint32_t sector;
    uint32_t size;
    uint8_t attr;          /* 0x10: directory */
} NcDiscFile;

bool nc_disc_open(NcDisc *d, int fd);
void nc_disc_close(NcDisc *d);

/* The disc's folder tables kept in memory once read, and all of them read
 * ahead on a helper thread. A file's open looked its path up on the disc,
 * folder by folder, on the emulated CPU's thread: from a share the read
 * waited behind the disc reader's 1.5 MiB reads (30-65 ms an open,
 * interrupts held off meanwhile: a game loading while its intro movie plays
 * starved the movie's sound, and the movie, timed by it, stalled). The
 * game's disc only (the core's). */
void nc_disc_cache_folders(NcDisc *d);

/* Visits a directory's entries in sorted order until fn returns false. dir
 * holds the directory's size bytes. Each entry is visited once, so a
 * malformed tree that loops still ends. fn only sees entries whose name
 * (e[13] bytes at e + 14) fits inside dir. Returns false if out of memory. */
typedef bool (*NcXdvdfsEntryFn)(void *ctx, const uint8_t *e, uint32_t off);
bool nc_xdvdfs_walk(const uint8_t *dir, uint32_t size, NcXdvdfsEntryFn fn, void *ctx);

/* Finds a path such as "\\default.xbe" or "media/x.bin" (either slash,
 * case-insensitive). */
bool nc_disc_find(const NcDisc *d, const char *path, NcDiscFile *out);

/* Reads len bytes at offset off of a file. */
bool nc_disc_read(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len);
/* ... as the title's file I/O sees it: the overlay's files, but not the XBE
 * patches. Those patch the image the core boots, and a title reading an XBE
 * as a file gets the disc's bytes: a title update's patch, built against the
 * disc's XBE and applied by the title's updater, came out damaged over a mod's
 * byte (PGR2, Forza with their 60 fps mods). */
bool nc_disc_read_file(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len);

/* The overlay, before the title boots: a host file at a disc path (either
 * slash); a later one at the same path takes its place. Folders the disc
 * doesn't have come with it. Then seal (sorted; nc_disc_find sees it), then
 * patches: bytes over a file's content at off (the overlay's file if it
 * replaced it). */
bool nc_disc_overlay_file(NcDisc *d, const char *path, const char *host);
/* XPSemu patch 219: a file served by a hook (a Sega Chihiro image's, read in place: no unpacking) */
bool nc_disc_overlay_virtual(NcDisc *d, const char *path, uint32_t size, unsigned id);
extern bool (*nc_disc_vread_hook)(unsigned id, uint64_t off, void *buf, size_t len);
void nc_disc_overlay_seal(NcDisc *d);
bool nc_disc_overlay_patch(NcDisc *d, const char *path, uint64_t off, const uint8_t *bytes, uint32_t len);
/* The disc's own file and bytes, past the overlay and its patches (what
 * identifies the game for savestates, the bytes a patch replaced). */
bool nc_disc_find_orig(const NcDisc *d, const char *path, NcDiscFile *out);
bool nc_disc_read_orig(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len);
/* The overlay's files and folders in the disc folder dir: replaced ones
 * with their new size, added ones (directory listings). */
void nc_disc_overlay_list(const NcDisc *d, const char *dir,
                          void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx);

#ifdef __cplusplus
}
#endif

#endif
