/*
 * hakuX native core — files, devices and the object namespace.
 *
 * Devices: \Device\CdRom0 is the title's disc (XDVDFS, read-only);
 * \Device\Harddisk0\PartitionN are host directories (hdd root/PartitionN),
 * matched case-insensitively as FATX does. \??\X: names are symbolic links
 * (IoCreateSymbolicLink); D: starts out at the disc.
 *
 * Xbox names are ANSI: OBJECT_ATTRIBUTES { RootDirectory, ObjectName
 * (ANSI_STRING*), Attributes }; FILE_DIRECTORY_INFORMATION carries ANSI
 * names. I/O completes synchronously: IoStatusBlock and the event are set
 * before the call returns; APC completion routines are not delivered yet.
 */
#include "native/kernel/kernel.h"
#include "native/kernel/xc_crypto.h"
#include "native/core/hostio.h"
#include "native/dev/dev.h"
#include "native/core/irq.h"
#include "native/core/xdvdfs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include "hw/xbox/hle/prop.h"
#endif

static void writes_drain(void);
bool nc_pread_sure(int fd, void *buf, size_t len, uint64_t off);
bool nc_pwrite_sure(int fd, const void *buf, size_t len, uint64_t off);

#define STATUS_NO_MORE_FILES          0x80000006u
#define STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
#define STATUS_END_OF_FILE            0xC0000011u
#define STATUS_NO_SUCH_FILE           0xC000000Fu
#define STATUS_ACCESS_DENIED          0xC0000022u
#define STATUS_BUFFER_TOO_SMALL       0xC0000023u
#define STATUS_OBJECT_NAME_INVALID    0xC0000033u
#define STATUS_OBJECT_NAME_NOT_FOUND  0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION  0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND  0xC000003Au
#define STATUS_DELETE_PENDING         0xC0000056u
#define STATUS_MEDIA_WRITE_PROTECTED  0xC00000A2u
#define STATUS_NO_MEDIA_IN_DEVICE     0xC0000013u
#define STATUS_FILE_IS_A_DIRECTORY    0xC00000BAu
#define STATUS_NOT_A_DIRECTORY        0xC0000103u
#define STATUS_DIRECTORY_NOT_EMPTY    0xC0000101u

#define FILE_SUPERSEDE    0
#define FILE_OPEN         1
#define FILE_CREATE       2
#define FILE_OPEN_IF      3
#define FILE_OVERWRITE    4
#define FILE_OVERWRITE_IF 5
#define FILE_DIRECTORY_FILE     0x01u
#define FILE_SYNCHRONOUS_IO     0x30u /* _ALERT 0x10, _NONALERT 0x20 */
#define FILE_NON_DIRECTORY_FILE 0x40u
#define FILE_NO_INTERMEDIATE_BUFFERING 0x08u
#define STATUS_PENDING          0x00000103u

/* OBJECT_ATTRIBUTES.RootDirectory pseudo-handles (ObDosDevicesDirectory). */
#define OB_DOS_DEVICES 0xFFFFFFFDu

#define FILE_ATTRIBUTE_READONLY  0x01u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL    0x80u
#define FILE_ATTRIBUTE_ARCHIVE   0x20u

#define DISC_TIME 0x01C0739BEB8F8000ull /* 2001-01-01 */

enum FileKind { F_DISC, F_HOST, F_VOLUME, F_LINK };
struct DirEntry;

typedef struct NcFile {
    bool used;
    enum FileKind kind;
    char path[512];      /* canonical device path */
    NcDiscFile disc;     /* F_DISC */
    char host[512];      /* F_HOST: host path */
    int fd;              /* F_HOST files */
    bool is_dir;
    uint64_t pos;
    unsigned dir_next;   /* directory enumeration */
    char mask[128];
    int partition;       /* F_VOLUME: -1 disc, else partition number */
    /* Opened with an overwrite disposition: the old contents moved to a side
     * file outside the partitions, and the file itself is empty on the host
     * from the start, so the disk always holds what the guest sees. FATX
     * doesn't zero what a file grows back into: growing it again (SetEndOfFile,
     * a write past the end) brings the old bytes back in that range (Forza
     * trims its unpacked cabs so). The side file goes when the handle closes;
     * one a dead process left behind is swept at the next overwrite. */
    char old_path[320];  /* "" = none */
    uint64_t old_size;
    bool wrote;          /* written through this handle: synced when it closes */
    /* A directory's enumeration works on a snapshot taken when it starts:
     * XDeleteSaveGame deletes each file as the listing returns it, and a live
     * listing shifted under it and skipped every other file. */
    struct DirEntry *listing;
    unsigned nlisting;
    /* Opened without FILE_SYNCHRONOUS_IO_*: a read or write with an event or
     * APC (on the disc, any) that completes reports STATUS_PENDING, as the
     * Xbox's DMA disk and DVD transfers do, with its result in the
     * IoStatusBlock (io_finish); a write only with no_buffering. */
    bool async;
    /* The handle may change the file's data or size (its DesiredAccess has
     * FILE_WRITE_DATA / FILE_APPEND_DATA, GENERIC_WRITE / _ALL or
     * MAXIMUM_ALLOWED): writes and SetEndOfFile through one that can't are
     * refused, as the Xbox kernel refuses them (handle_can_write). */
    bool can_write;
    /* Deleted through this handle (FileDispositionInformation): the file
     * stays, as the Xbox's does, until the last handle on it closes. Sudeki
     * deletes its boot.baf with a handle on it still open: unlinked at
     * once, a savestate's copy of the drive left it out, and the state
     * couldn't reopen it. */
    bool del;
    /* Opened with FILE_NO_INTERMEDIATE_BUFFERING: its writes go to the disk
     * and pend (async). Without it they go through the file system's cache
     * and complete in the call, as on the Xbox: Breakdown copies an area to
     * its cache partition in 16 KiB overlapped writes and takes a write that
     * pends for a failed one; its copy stopped after the first, and the area
     * then loaded from a file of zeros (no room drawn, nor in cutscenes). */
    bool no_buffering;
    /* patch 307: a handle on the persistent write-helper (wbwriter.elf, a
     * process outside the sandbox that isn't write-throttled) when this is a
     * writable cache-drive file and the helper is up; 0 = none, <0 = tried and
     * failed (write in place). Not part of the save state (opened fresh after a
     * state load). */
    int wb_handle;
} NcFile;

/* DesiredAccess bits that grant writing the data. */
#define ACCESS_WRITES (0x00000002u /* FILE_WRITE_DATA */ | 0x00000004u /* FILE_APPEND_DATA */ | \
                       0x40000000u /* GENERIC_WRITE */ | 0x10000000u /* GENERIC_ALL */ |       \
                       0x02000000u /* MAXIMUM_ALLOWED */)

/* Open files at once: the handle table's size. Fatal Frame opens most of its
 * disc's 1821 files as it boots and keeps them open; at 1024 the rest failed
 * (insufficient resources), and the load waited for one forever. Untouched
 * slots cost nothing: a slot is cleared as it is taken (file_new). */
#define MAX_FILES NC_MAX_HANDLES
#define MAX_LINKS 64
/* FILE_OBJECT as the Xbox lays it out: Type +0, FsContext +8, Lock (a
 * KEVENT) +0x28, Event (a KEVENT, NC_FO_EVENT) +0x38; past its 0x48 bytes,
 * our file index, then the file system's control block FsContext points at.
 * XAPI's XGetFilePhysicalSortKey reads it by the volume's name: GDFX, the
 * start sector at +0; FATX, flags at +0 (bit 0: no key) and the first
 * cluster at +0x1C. */
#define FILE_OBJECT_SIZE 0x70u
#define FO_FSCONTEXT 0x08u
#define FO_LOCK 0x28u
#define FO_INDEX 0x48u
#define FO_FCB 0x4Cu
#define FCB_FATX_FLAGS 0x00u
#define FCB_FATX_CLUSTER 0x1Cu

static NcFile g_files[MAX_FILES];
/* Console timing: each open file's pages in the file system's cache, those
 * its last read from the drive brought in ([lo, hi), FS_PAGE pages). A read
 * inside them takes no drive time; not kept in states. */
#define FS_PAGE 4096u
static struct FsCache {
    uint64_t lo, hi;
} g_fs_cache[MAX_FILES];
typedef struct NcLink {
    char name[64];
    char target[256];
} NcLink;

static NcLink g_links[MAX_LINKS];
static char g_hdd_root[256] = "/data/local/tmp/native/hdd";

/* The retail hard disk's layout (8 GB, 512-byte sectors): partition 0 is the
 * config area, 1 the data drive (E:), 2 the system drive (C:), 3-5 the
 * title cache drives. Offset and length per partition. */
#define HDD_BYTES 0x1DD156000ull
/* Sectors per cluster a partition was last formatted with (its FATX superblock). */
static uint32_t g_cluster_sectors[6];
static const uint64_t k_partitions[6][2] = {
    {0x00000000ull, 0x00080000ull},  {0xABE80000ull, 0x1312D6000ull}, {0x8CA80000ull, 0x1F400000ull},
    {0x00080000ull, 0x2EE00000ull}, {0x2EE80000ull, 0x2EE00000ull},  {0x5DC80000ull, 0x2EE00000ull},
};
static bool g_trace_io; /* NC_TRACE_IO: every read */

void nc_io_set_hdd_root(const char *path)
{
    snprintf(g_hdd_root, sizeof(g_hdd_root), "%s", path);
}

const char *nc_io_hdd_root(void)
{
    return g_hdd_root;
}

/* ------------------------------------------------------------- strings */

static bool prefix_ci(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static void ansi(NcKernel *k, uint32_t str, char *out, size_t cap)
{
    out[0] = 0;
    if (!str) {
        return;
    }
    uint16_t len = *(uint16_t *)nc_vm_host(k->vm, str);
    uint32_t buf = nc_vm_rd32(k->vm, str + 4);
    if (len >= cap) {
        len = (uint16_t)(cap - 1);
    }
    memcpy(out, nc_vm_host(k->vm, buf), len);
    out[len] = 0;
}

static bool wildcard(const char *pat, const char *s)
{
    if (!*pat) {
        return !*s;
    }
    if (*pat == '*') {
        for (;; s++) {
            if (wildcard(pat + 1, s)) {
                return true;
            }
            if (!*s) {
                return false;
            }
        }
    }
    return *s && (*pat == '?' || toupper((unsigned char)*pat) == toupper((unsigned char)*s)) &&
           wildcard(pat + 1, s + 1);
}

/* ----------------------------------------------------------- namespace */

static int link_find(const char *name)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        if (g_links[i].name[0] && strcasecmp(g_links[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool link_set(const char *name, const char *target)
{
    int i = link_find(name);
    for (int j = 0; i < 0 && j < MAX_LINKS; j++) {
        if (!g_links[j].name[0]) {
            i = j;
        }
    }
    if (i < 0) {
        return false;
    }
    snprintf(g_links[i].name, sizeof(g_links[i].name), "%s", name);
    snprintf(g_links[i].target, sizeof(g_links[i].target), "%s", target);
    nc_log("hakuX-native-io", "link %s -> %s", name, target);
    return true;
}

/* Canonical device path: '\' separators, no duplicate separators, '.' and
 * '..' folded, \??\ links resolved. */
static bool canon(const char *in, char *out, size_t cap)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", in);
    for (char *p = tmp; *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    if (prefix_ci(tmp, "\\??\\") && isalpha((unsigned char)tmp[4]) && tmp[5] == ':') {
        char name[8] = {'\\', '?', '?', '\\', (char)toupper((unsigned char)tmp[4]), ':', 0};
        int l = link_find(name);
        if (l < 0) {
            return false;
        }
        char joined[512];
        snprintf(joined, sizeof(joined), "%s\\%s", g_links[l].target, tmp + 6);
        snprintf(tmp, sizeof(tmp), "%s", joined);
    } else if (prefix_ci(tmp, "\\??\\")) {
        /* a longer drive name (the Chihiro media board's mbfs:, mbcom:, mbrom0:, mbrom1:; XPSemu patch 187) */
        const char *colon = strchr(tmp + 4, ':');
        const char *slash = strchr(tmp + 4, '\\');
        if (colon && (!slash || colon < slash) && colon - tmp < 40) {
            char name[48];
            snprintf(name, sizeof(name), "%.*s", (int)(colon - tmp + 1), tmp);
            const int l = link_find(name);
            if (l >= 0) {
                char joined[512];
                snprintf(joined, sizeof(joined), "%s\\%s", g_links[l].target, colon + 1);
                snprintf(tmp, sizeof(tmp), "%s", joined);
            }
        }
    }
    /* Fold components. */
    char *parts[64];
    int n = 0;
    for (char *tok = strtok(tmp, "\\"); tok; tok = strtok(NULL, "\\")) {
        if (!strcmp(tok, ".")) {
            continue;
        }
        if (!strcmp(tok, "..")) {
            if (n) {
                n--;
            }
            continue;
        }
        if (n < 64) {
            parts[n++] = tok;
        }
    }
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len < cap - 2; i++) {
        len += (size_t)snprintf(out + len, cap - len, "\\%s", parts[i]);
    }
    if (!n) {
        snprintf(out, cap, "\\");
    }
    return true;
}

/* Which device a canonical path is on: -1 the disc, 0..7 a partition, -2
 * unknown. rel receives the path inside the device ("" for its root). */
#define DEV_MB0 8   /* \Device\MediaBoard\Partition0 .. 3: 8 .. 11 (Chihiro) */

static int device_of(const char *path, const char **rel)
{
    static const char cd[] = "\\Device\\CdRom0";
    static const char mb[] = "\\Device\\MediaBoard\\Partition";
    if (getenv("NC_CHIHIRO") && prefix_ci(path, mb) && path[sizeof(mb) - 1] >= '0' && path[sizeof(mb) - 1] <= '3' &&
        (!path[sizeof(mb)] || path[sizeof(mb)] == '\\')) {
        *rel = path + sizeof(mb);
        return DEV_MB0 + (path[sizeof(mb) - 1] - '0');
    }
    static const char hd[] = "\\Device\\Harddisk0\\Partition";
    if (prefix_ci(path, cd) && (!path[sizeof(cd) - 1] || path[sizeof(cd) - 1] == '\\')) {
        *rel = path + sizeof(cd) - 1;
        return -1;
    }
    if (prefix_ci(path, hd) && isdigit((unsigned char)path[sizeof(hd) - 1])) {
        const char *p = path + sizeof(hd) - 1;
        int part = *p - '0';
        p++;
        if (*p && *p != '\\') {
            return -2;
        }
        *rel = p;
        return part;
    }
    return -2;
}

static int host_path(int part, const char *rel, char *out, size_t cap);

bool nc_io_host_file(const char *xbox_path, char *out, size_t n)
{
    char path[512];
    const char *rel;
    if (!canon(xbox_path, path, sizeof(path))) {
        return false;
    }
    const int part = device_of(path, &rel);
    struct stat st;
    return part >= 1 && part <= 5 && *rel && host_path(part, rel, out, n) == 1 && stat(out, &st) == 0 &&
           S_ISREG(st.st_mode);
}

void nc_io_set_launch_dir(const char *dir)
{
    link_set("\\??\\D:", dir);
}

static bool mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return mkdir(tmp, 0777) == 0 || errno == EEXIST;
}

/* Host path for a path inside partition part, matching existing names
 * case-insensitively. Returns 1 if it exists, 0 if only the last component is
 * missing, -1 if a parent is missing. */
/* XOG: the Xbox's three cache drives are shared and a game formats the oldest. Here there's room for
 * each game to have its own: another game can't wipe them, a state finds its files. */
static char g_cache_root[256];

static bool ram_enabled(void);   /* patch 288 (defined below) */

/* Delete our own stale 0-byte placeholder cache files (left by a prior ramcache run that exited or
 * crashed with files still resident in RAM, or by ramcache since being disabled). A 0-byte file
 * where the game wrote real data makes the guest read nothing and hang (patch 293: this bit Fable -
 * fonts.big left 0 bytes, read empty, black screen). Only when ramcache is OFF this launch: with it
 * on, those files are served from RAM, not disk. A deleted (missing) file is safe - the game rebuilds
 * it; a present 0-byte file is not. Markers and other non-empty files are untouched. */
static void cache_sweep_empty(const char *dir, unsigned depth, unsigned *n)
{
    if (depth > 24) {
        return;
    }
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        char p[600];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        if (lstat(p, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            cache_sweep_empty(p, depth + 1, n);
        } else if (S_ISREG(st.st_mode) && st.st_size == 0) {
            if (unlink(p) == 0) {
                (*n)++;
            }
        }
    }
    if (d) {
        closedir(d);
    }
}

void nc_io_set_cache_game(uint32_t title)
{
    snprintf(g_cache_root, sizeof(g_cache_root), "%s/cache/%08x", g_hdd_root, title);
    char dir[300];
    snprintf(dir, sizeof(dir), "%s", g_cache_root);
    mkdirs(dir);
    nc_log("hakuX-native-io", "cache drives: %s", g_cache_root);
    if (!ram_enabled() && g_cache_root[0]) {
        unsigned n = 0;
        cache_sweep_empty(g_cache_root, 0, &n);
        if (n) {
            nc_log("hakuX-native-io", "cache: deleted %u stale 0-byte placeholder file(s) under %s so the game "
                   "rebuilds them (patch 293: never serve a 0-byte file as real data)", n, g_cache_root);
        }
    }
}

const char *nc_io_part_root(int part)
{
    return g_cache_root[0] && (part == 0 || (part >= 3 && part <= 5)) ? g_cache_root : g_hdd_root;
}

static int host_path(int part, const char *rel, char *out, size_t cap)
{
    if (part >= DEV_MB0) {
        /* the game's folder (mbfs:), else one file each: mbcom:, mbrom0:, mbrom1: */
        static const char *const file[4] = { NULL, "/data/xpsemu/chihiro/run/mbcom.bin",
                                             "/data/xpsemu/chihiro/boot/segaboot_old.xbe",
                                             "/data/xpsemu/chihiro/boot/default.xbe" };
        struct stat st;
        if (part > DEV_MB0) {
            if (*rel) {
                return -1;
            }
            snprintf(out, cap, "%s", file[part - DEV_MB0]);
            return stat(out, &st) == 0 ? 1 : 0;
        }
        const char *root = getenv("NC_CHIHIRO_FS");
        snprintf(out, cap, "%s%s", root ? root : "/data/xpsemu/chihiro/run/none", rel);
        for (char *p = out; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
        return stat(out, &st) == 0 ? 1 : 0;
    }
    size_t len = (size_t)snprintf(out, cap, "%s/Partition%d", nc_io_part_root(part), part);
    mkdirs(out);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", rel);
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        const bool last = !save || !*save;
        DIR *d = opendir(out);
        const char *match = NULL;
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (strcasecmp(de->d_name, tok) == 0) {
                match = de->d_name;
                break;
            }
        }
        len += (size_t)snprintf(out + len, cap - len, "/%s", match ? match : tok);
        if (d) {
            closedir(d);
        }
        if (!match) {
            return last ? 0 : -1;
        }
    }
    return 1;
}

/* --------------------------------------------------------- file objects */

static NcFile *file_new(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            memset(&g_files[i], 0, sizeof(g_files[i]));
            g_fs_cache[i] = (struct FsCache){0};
            g_files[i].used = true;
            g_files[i].fd = -1;
            return &g_files[i];
        }
    }
    return NULL;
}

/* Another open handle on the host file, or NULL; with del: one that
 * deleted it. */
static NcFile *file_other(const NcFile *nf, const char *host, bool del)
{
    for (int i = 0; i < MAX_FILES; i++) {
        NcFile *o = &g_files[i];
        if (o != nf && o->used && o->kind == F_HOST && !o->is_dir && (o->del || !del) &&
            !strcmp(o->host, host)) {
            return o;
        }
    }
    return NULL;
}

static uint32_t file_handle(NcKernel *k, NcFile *nf)
{
    uint32_t obj = nc_ob_create(k, nf->kind == F_LINK ? NC_OB_SYMLINK : NC_OB_FILE, FILE_OBJECT_SIZE);
    if (!obj) {
        return 0;
    }
    *(uint16_t *)nc_vm_host(k->vm, obj) = 5; /* IO_TYPE_FILE */
    nc_ke_init_event(k, obj + FO_LOCK, 1, false);
    nc_ke_init_event(k, obj + NC_FO_EVENT, 0, false);
    nc_vm_wr32(k->vm, obj + FO_INDEX, (uint32_t)(nf - g_files));
    const uint32_t fcb = obj + FO_FCB;
    nc_vm_wr32(k->vm, obj + FO_FSCONTEXT, fcb);
    if (nf->kind == F_DISC) {
        nc_vm_wr32(k->vm, fcb, nf->disc.sector);
    } else if (nf->kind == F_HOST && !nf->is_dir) {
        /* Host files have no clusters: the inode orders them stably. */
        struct stat st;
        nc_vm_wr32(k->vm, fcb + FCB_FATX_CLUSTER, stat(nf->host, &st) == 0 ? (uint32_t)st.st_ino : 0);
    } else {
        nc_vm_wr32(k->vm, fcb + FCB_FATX_FLAGS, 1);
    }
    return nc_ob_handle(k, obj, nf->kind == F_LINK ? NC_OB_SYMLINK : NC_OB_FILE);
}

static NcFile *file_of(NcKernel *k, uint32_t handle)
{
    enum NcObType type;
    uint32_t obj = nc_ob_lookup(k, handle, &type);
    if (!obj || (type != NC_OB_FILE && type != NC_OB_SYMLINK)) {
        return NULL;
    }
    uint32_t i = nc_vm_rd32(k->vm, obj + FO_INDEX);
    return i < MAX_FILES && g_files[i].used ? &g_files[i] : NULL;
}

/* What the title wrote is on the disk soon after it closes the file, so an
 * app killed or hung later can't leave a half-written save: the closer
 * thread syncs it, closes it and drops its overwritten contents, in that
 * order. On the guest's CPU the sync took 10-110 ms per file with nothing
 * else running, interrupts included: Forza's frames came out late while it
 * wrote its caches and replays. Reopening the path meanwhile reads the same
 * data (the page cache). A full queue closes in place. */
typedef struct CloseJob {
    int fd;
    int wb_handle;       /* patch 307: the write-helper handle to close too, or 0 */
    int fsync_helper;    /* patch 308b: a flush's helper handle (fsync, not close), or 0 */
    char path[64];       /* for the log */
    char old_path[320];
} CloseJob;

/* patch 307: the persistent write-helper (ps5/src/wbwriter_client.c). It streams
 * wbwriter.elf to elfldr once and keeps a socket to a process outside the sandbox
 * that is NOT write-throttled; cache-drive writes go there. Every call is
 * fail-soft - any failure and the helper goes unavailable and we write in place. */
extern int wbwr_available(void);
extern int wbwr_open(const char *host);
extern long wbwr_write(int handle, uint64_t off, const void *data, uint32_t len);
extern int wbwr_fsync(int handle);
extern void wbwr_close(int handle);

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    CloseJob q[64];
    unsigned head, n;
    bool started;
} g_closer = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void close_now(const CloseJob *j)
{
    /* patch 308b: a file the helper wrote is fsynced by its own descriptor.
     * Syncing the app's descriptor too is charged to the app and was still
     * throttled (a 49 MiB state took 16 s that way); only a file with local
     * fallback bytes needs the app's fsync. */
    const int fh = j->fsync_helper > 0 ? j->fsync_helper : j->wb_handle;
    if (fh > 0) {
        if (wbwr_fsync(fh) != 0 && j->fd >= 0 && fsync(j->fd) != 0) {
            nc_log("hakuX-native-io", "sync %s: %s", j->path, strerror(errno));
        }
    } else if (j->fd >= 0 && fsync(j->fd) != 0) {
        nc_log("hakuX-native-io", "sync %s: %s", j->path, strerror(errno));
    }
    if (j->fd >= 0) {
        close(j->fd);
    }
    if (j->wb_handle > 0) {
        wbwr_close(j->wb_handle);       /* patch 307: the helper's own descriptor, after its writes */
    }
    if (j->old_path[0]) {
        unlink(j->old_path);
    }
}

static void *closer_thread(void *arg)
{
    pthread_mutex_lock(&g_closer.mu);
    for (;;) {
        while (!g_closer.n) {
            pthread_cond_wait(&g_closer.cv, &g_closer.mu);
        }
        const CloseJob j = g_closer.q[g_closer.head];
        g_closer.head = (g_closer.head + 1) % 64u;
        g_closer.n--;
        pthread_mutex_unlock(&g_closer.mu);
        close_now(&j);
        pthread_mutex_lock(&g_closer.mu);
    }
    return NULL;
}

static void close_queue(CloseJob j)
{
    pthread_mutex_lock(&g_closer.mu);
    if (!g_closer.started) {
        pthread_t t;
        g_closer.started = pthread_create(&t, NULL, closer_thread, NULL) == 0;
        if (g_closer.started) {
            pthread_detach(t);
        }
    }
    const bool queued = g_closer.started && g_closer.n < 64u;
    if (queued) {
        g_closer.q[(g_closer.head + g_closer.n++) % 64u] = j;
        pthread_cond_signal(&g_closer.cv);
    }
    pthread_mutex_unlock(&g_closer.mu);
    if (!queued) {
        close_now(&j);
    }
}

static void close_later(const NcFile *nf)
{
    CloseJob j = {.fd = nf->fd, .wb_handle = nf->wb_handle};    /* patch 307: close the helper handle too */
    snprintf(j.path, sizeof(j.path), "%s", nf->path);
    snprintf(j.old_path, sizeof(j.old_path), "%s", nf->old_path);
    close_queue(j);
}

/* NtFlushBuffersFile: the same sync on the closer thread, of a duplicate of
 * the descriptor (the title may close the file meanwhile), in order with the
 * closes. On the guest's CPU it took up to 211 ms (Forza's saves in the
 * menus), nothing else running. */
static void flush_later(const NcFile *nf)
{
    CloseJob j = {.fd = dup(nf->fd)};
    if (j.fd < 0) {
        if (nf->wb_handle > 0) {
            wbwr_fsync(nf->wb_handle);
        } else {
            fsync(nf->fd);
        }
        return;
    }
    snprintf(j.path, sizeof(j.path), "%s", nf->path);
    j.fsync_helper = nf->wb_handle;    /* patch 308b: flush through the helper, do not close it */
    j.old_path[0] = 0;                 /* the overwritten contents go at the close */
    close_queue(j);
}

static bool soft_range(uint32_t va, uint32_t len);
static bool ram_enabled(void);          /* patch 288: RAM cache (defined below) */
static void ram_drop(const char *host);

/* patch 356: a host read that the PS5 refuses for a moment (ENOMEM / EAGAIN / EINTR, or a short read) is tried again,
 * then through a buffer of our own: RalliSport Challenge 2's music read from Z: (32 KiB into the game's buffer) failed
 * "Cannot allocate memory" once, the game saw STATUS_DEVICE_DATA_ERROR and showed its dirty-disc screen (issue #33). */
bool nc_pread_sure(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    size_t done = 0;
    for (unsigned tries = 0; done < len && tries < 6;) {
        const ssize_t n = pread(fd, p + done, len - done, (off_t)(off + done));
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == 0) {
            break;                      /* the end of the file */
        }
        tries++;
        static unsigned told;
        if (told++ < 16) {
            nc_log("hakuX-native-io", "read of %zu bytes at +%llx refused (%s): tried again", len - done,
                   (unsigned long long)(off + done), strerror(errno));
        }
        const struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
    }
    if (done < len) {
        uint8_t *b = malloc(len - done);
        if (b) {
            const ssize_t n = pread(fd, b, len - done, (off_t)(off + done));
            if (n == (ssize_t)(len - done)) {
                memcpy(p + done, b, len - done);
                done = len;
            }
            free(b);
        }
    }
    return done == len;
}

/* patch 356: the same for a host write: a refused or short pwrite tried again, then from a buffer of our own */
bool nc_pwrite_sure(int fd, const void *buf, size_t len, uint64_t off)
{
    const uint8_t *p = buf;
    size_t done = 0;
    for (unsigned tries = 0; done < len && tries < 6;) {
        const ssize_t n = pwrite(fd, p + done, len - done, (off_t)(off + done));
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        tries++;
        static unsigned told;
        if (told++ < 16) {
            nc_log("hakuX-native-io", "write of %zu bytes at +%llx refused (%s): tried again", len - done,
                   (unsigned long long)(off + done), strerror(errno));
        }
        const struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
    }
    if (done < len) {
        uint8_t *b = malloc(len - done);
        if (b) {
            memcpy(b, p + done, len - done);
            if (pwrite(fd, b, len - done, (off_t)(off + done)) == (ssize_t)(len - done)) {
                done = len;
            }
            free(b);
        }
    }
    return done == len;
}

/* ---- Write-behind for the cache drives (XOG patch 261, xogfix=cachewrite, per game) ----
 * A write to a file on a cache partition (Partition3-5, the utility drives)
 * is copied into a host buffer and completes in the call, as it did, same
 * status, IoStatusBlock, event and APC; the writer thread does the pwrite,
 * in order. Far Cry Instincts copies its sounds from the disc to Z: while
 * its Ubisoft and Crytek intros play: each pwrite in place held the guest's
 * one CPU 9-15 ms and the Bink decoder ran at 20-26 fps.
 * Anything else on a file with writes queued (a read, its size or times,
 * set-size, rename, delete, an open of the same path, a listing of its
 * folder, a format) waits for that path's queue first; a close or flush goes
 * into the queue behind its writes (the descriptor stays open until they are
 * done, so its number can't be reused under them). Save states, a state load
 * and leaving the game drain the whole queue. At most WB_CAP bytes wait:
 * past it the game waits for room. */
#define WB_CAP (64u << 20)
#define WB_PATHS 64

typedef struct WbJob {
    struct WbJob *next;
    int fd;
    int wb_handle;          /* patch 307: the write-helper handle, or 0 to pwrite in place */
    int slot;               /* g_wb.path[] */
    uint64_t off;
    uint32_t len;
    bool is_close;          /* a close or flush (cj) behind the writes */
    CloseJob cj;
    uint8_t data[];
} WbJob;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;      /* a job queued */
    pthread_cond_t done;    /* a job done */
    WbJob *head, *tail;
    uint64_t backlog;       /* bytes queued, not yet written */
    struct {
        char host[512];
        unsigned pending;   /* 0: free */
    } path[WB_PATHS];
    unsigned npaths;        /* paths with jobs (read without the lock as a hint) */
    bool started;
    /* this second's figures, for the log */
    uint64_t s_writes, s_in, s_done, s_out, s_write_ns, s_longest_ns, s_waits, s_wait_ns, s_wait_longest_ns;
    uint64_t total_in, total_out, s_t0;
} g_wb = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER};
static int g_wb_on = -1;

static uint64_t wb_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int g_wb_helper = -1;    /* patch 307: 1 while the write-helper is routing (set once) */

static bool wb_enabled(void)
{
    if (g_wb_on < 0) {
        const char *e = getenv("XOG_FIX");
        const int cachewrite = e && strstr(e, "cachewrite") ? 1 : 0;
        /* patch 307: general when the un-throttled helper loads (it only changes
         * HOW cache writes reach the disk, via a process outside the sandbox);
         * without it, keep today's behaviour (synchronous in place unless the
         * game opted into xogfix=cachewrite). */
        g_wb_helper = wbwr_available() ? 1 : 0;
        g_wb_on = (cachewrite || g_wb_helper) ? 1 : 0;
        if (g_wb_on) {
            nc_log("hakuX-native-io",
                   "cachewrite: cache-drive (Partition3-5) writes go to a writer thread%s (at most %u MiB waiting)",
                   g_wb_helper ? ", then to an un-throttled helper process (patch 307, all titles)"
                               : " (xogfix=cachewrite)",
                   WB_CAP >> 20);
        }
    }
    return g_wb_on == 1;
}

/* With g_wb.mu held: the slot of host's queue, or -1. */
static int wb_slot(const char *host)
{
    for (int i = 0; i < WB_PATHS; i++) {
        if (g_wb.path[i].pending && !strcasecmp(g_wb.path[i].host, host)) {
            return i;
        }
    }
    return -1;
}

/* With g_wb.mu held: a queue for host or anything under it (a folder's). */
static bool wb_busy_under(const char *host)
{
    const size_t n = strlen(host);
    for (int i = 0; i < WB_PATHS; i++) {
        const char *p = g_wb.path[i].host;
        if (g_wb.path[i].pending && !strncasecmp(p, host, n) && (p[n] == 0 || p[n] == '/')) {
            return true;
        }
    }
    return false;
}

/* With g_wb.mu held: the game waited from t0 (the log's figures). */
static void wb_waited(uint64_t t0)
{
    const uint64_t d = wb_ns() - t0;
    g_wb.s_waits++;
    g_wb.s_wait_ns += d;
    if (d > g_wb.s_wait_longest_ns) {
        g_wb.s_wait_longest_ns = d;
    }
}

/* Waits for the queued writes to host and to anything under it. */
static void wb_wait_under(const char *host)
{
    if (!__atomic_load_n(&g_wb.npaths, __ATOMIC_ACQUIRE)) {
        return;
    }
    pthread_mutex_lock(&g_wb.mu);
    if (wb_busy_under(host)) {
        const uint64_t t0 = wb_ns();
        while (wb_busy_under(host)) {
            pthread_cond_wait(&g_wb.done, &g_wb.mu);
        }
        wb_waited(t0);
    }
    pthread_mutex_unlock(&g_wb.mu);
}

/* With g_wb.mu held: the second's figures to the log. */
static void wb_log(const char *when)
{
    nc_log("hakuX-native-io", "cachewrite%s: %llu writes %llu KiB queued, %llu done %llu KiB in %.1f ms (longest "
           "%.1f ms), backlog %llu KiB; the game waited %llu times %.1f ms (longest %.1f ms); in all %.1f of %.1f MiB "
           "written", when, (unsigned long long)g_wb.s_writes, (unsigned long long)(g_wb.s_in >> 10),
           (unsigned long long)g_wb.s_done, (unsigned long long)(g_wb.s_out >> 10), g_wb.s_write_ns / 1e6,
           g_wb.s_longest_ns / 1e6, (unsigned long long)(g_wb.backlog >> 10), (unsigned long long)g_wb.s_waits,
           g_wb.s_wait_ns / 1e6, g_wb.s_wait_longest_ns / 1e6, g_wb.total_out / 1048576.0,
           g_wb.total_in / 1048576.0);
    g_wb.s_writes = g_wb.s_in = g_wb.s_done = g_wb.s_out = g_wb.s_write_ns = g_wb.s_longest_ns = 0;
    g_wb.s_waits = g_wb.s_wait_ns = g_wb.s_wait_longest_ns = 0;
    g_wb.s_t0 = wb_ns();
}

static void *wb_thread(void *arg)
{
    pthread_mutex_lock(&g_wb.mu);
    for (;;) {
        while (!g_wb.head) {
            if (g_wb.s_writes || g_wb.s_done || g_wb.s_waits) {
                wb_log(" (queue empty)");
            }
            pthread_cond_wait(&g_wb.cv, &g_wb.mu);
        }
        WbJob *j = g_wb.head;
        g_wb.head = j->next;
        if (!g_wb.head) {
            g_wb.tail = NULL;
        }
        pthread_mutex_unlock(&g_wb.mu);
        const uint64_t t0 = wb_ns();
        uint64_t put = 0;
        if (j->is_close) {
            close_now(&j->cj);
        } else {
            /* patch 307: hand the write to the un-throttled helper process. It
             * only returns once the helper has done the pwrite, so the job isn't
             * marked done (and wb_wait_under can't release a reader) until the
             * bytes are in the shared page cache - the same ordering the local
             * pwrite gives. On any helper trouble, write it in place (fail-soft). */
            if (j->wb_handle > 0 && wbwr_write(j->wb_handle, j->off, j->data, j->len) == (long)j->len) {
                put = j->len;
            }
            if (put < j->len && nc_pwrite_sure(j->fd, j->data + put, j->len - put, j->off + put)) {
                put = j->len;           /* patch 356: a refused write tried again */
            }
            while (put < j->len) {
                const ssize_t w = pwrite(j->fd, j->data + put, j->len - put, (off_t)(j->off + put));
                if (w <= 0) {
                    static unsigned told;
                    if (told++ < 16) {
                        nc_log("hakuX-native-io", "cachewrite: writing fd %d +%llx (%u bytes) failed: %s (first 16 "
                               "logged)", j->fd, (unsigned long long)j->off, j->len,
                               w < 0 ? strerror(errno) : "nothing written");
                    }
                    break;
                }
                put += (uint64_t)w;
            }
            /* when a game last wrote: leaving waits for it (main_ps5.c) */
            extern volatile uint64_t g_xog_hdd_write_ms;
            extern uint64_t rx_now_ms(void);
            g_xog_hdd_write_ms = rx_now_ms();
        }
        const uint64_t took = wb_ns() - t0;
        pthread_mutex_lock(&g_wb.mu);
        if (!--g_wb.path[j->slot].pending) {
            g_wb.path[j->slot].host[0] = 0;
            __atomic_store_n(&g_wb.npaths, g_wb.npaths - 1, __ATOMIC_RELEASE);
        }
        g_wb.backlog -= j->len;
        if (!j->is_close) {
            g_wb.s_done++;
            g_wb.s_out += put;
            g_wb.total_out += put;
        }
        g_wb.s_write_ns += took;
        if (took > g_wb.s_longest_ns) {
            g_wb.s_longest_ns = took;
        }
        pthread_cond_broadcast(&g_wb.done);
        if (wb_ns() - g_wb.s_t0 >= 1000000000ull) {
            wb_log("");
        }
        pthread_mutex_unlock(&g_wb.mu);
        free(j);
        pthread_mutex_lock(&g_wb.mu);
    }
    return NULL;
}

/* With g_wb.mu held: j (its slot set here) to the queue; false without the writer thread. */
static bool wb_push(WbJob *j, const char *host)
{
    if (!g_wb.started) {
        pthread_t t;
        g_wb.started = pthread_create(&t, NULL, wb_thread, NULL) == 0;
        if (!g_wb.started) {
            return false;
        }
        pthread_detach(t);
        g_wb.s_t0 = wb_ns();
    }
    int s = wb_slot(host);
    if (s < 0) {
        uint64_t t0 = 0;
        while ((s = wb_slot(host)) < 0) {
            for (int i = 0; i < WB_PATHS && s < 0; i++) {
                if (!g_wb.path[i].pending) {
                    s = i;
                }
            }
            if (s >= 0) {
                snprintf(g_wb.path[s].host, sizeof(g_wb.path[s].host), "%s", host);
                __atomic_store_n(&g_wb.npaths, g_wb.npaths + 1, __ATOMIC_RELEASE);
                break;
            }
            t0 = t0 ? t0 : wb_ns();
            pthread_cond_wait(&g_wb.done, &g_wb.mu);  /* every slot busy: one frees */
        }
        if (t0) {
            wb_waited(t0);
        }
    }
    g_wb.path[s].pending++;
    j->slot = s;
    j->next = NULL;
    if (g_wb.tail) {
        g_wb.tail->next = j;
    } else {
        g_wb.head = j;
    }
    g_wb.tail = j;
    g_wb.backlog += j->len;
    pthread_cond_signal(&g_wb.cv);
    return true;
}

/* A write of len bytes at off to nf, from the guest's va0: queued (true), or
 * false to write it in place (too big for the queue, no memory). */
static bool wb_write(NcKernel *k, NcFile *nf, uint64_t off, uint32_t va0, uint32_t len)
{
    if (!len || len > WB_CAP / 4) {
        return false;
    }
    /* patch 307: open this cache file on the helper the first time it is written
     * (once per handle, off the writer thread). 0 = not tried, <0 = tried and
     * failed (write in place from now on). */
    if (g_wb_helper == 1 && nf->wb_handle == 0) {
        const int h = wbwr_open(nf->host);
        nf->wb_handle = h > 0 ? h : -1;
    }
    WbJob *j = malloc(sizeof(*j) + len);
    if (!j) {
        return false;
    }
    memset(j, 0, sizeof(*j));
    j->fd = nf->fd;
    j->wb_handle = nf->wb_handle > 0 ? nf->wb_handle : 0;
    j->off = off;
    j->len = len;
    for (uint32_t put = 0; put < len;) {
        const uint32_t va = va0 + put, left = len - put, room = NC_PAGE - (va & (NC_PAGE - 1));
        const uint32_t chunk = soft_range(va, left) && left > room ? room : left;
        memcpy(j->data + put, nc_vm_host(k->vm, va), chunk);
        put += chunk;
    }
    pthread_mutex_lock(&g_wb.mu);
    if (g_wb.backlog + len > WB_CAP) {
        const uint64_t t0 = wb_ns();
        while (g_wb.backlog + len > WB_CAP) {
            pthread_cond_wait(&g_wb.done, &g_wb.mu);    /* full: the game waits for room */
        }
        wb_waited(t0);
    }
    const bool ok = wb_push(j, nf->host);
    if (ok) {
        g_wb.s_writes++;
        g_wb.s_in += len;
        g_wb.total_in += len;
    }
    pthread_mutex_unlock(&g_wb.mu);
    if (!ok) {
        free(j);
    }
    return ok;
}

/* A close (or a flush's duplicate) of host's descriptor behind its queued
 * writes: true if queued, false (nothing queued for host) to do it as before. */
static bool wb_close(const CloseJob *cj, const char *host)
{
    if (!__atomic_load_n(&g_wb.npaths, __ATOMIC_ACQUIRE)) {
        return false;
    }
    WbJob *j = calloc(1, sizeof(*j));
    if (!j) {
        wb_wait_under(host);
        return false;
    }
    j->is_close = true;
    j->cj = *cj;
    j->fd = cj->fd;
    pthread_mutex_lock(&g_wb.mu);
    const bool ok = wb_slot(host) >= 0 && wb_push(j, host);
    pthread_mutex_unlock(&g_wb.mu);
    if (!ok) {
        free(j);
    }
    return ok;
}

/* Every queued write on the disk (a save state, a state load, leaving the
 * game): false if it took over timeout_ms. why: for the log. */
bool nc_io_writes_drain(uint64_t timeout_ms, const char *why)
{
    writes_drain();                 /* the reader thread's host writes first */
    if (!__atomic_load_n(&g_wb.npaths, __ATOMIC_ACQUIRE)) {
        return true;
    }
    const uint64_t t0 = wb_ns();
    pthread_mutex_lock(&g_wb.mu);
    const uint64_t had = g_wb.backlog;
    bool ok = true;
    while (g_wb.npaths) {
        pthread_mutex_unlock(&g_wb.mu);
        if (wb_ns() - t0 > timeout_ms * 1000000ull) {
            ok = false;
            pthread_mutex_lock(&g_wb.mu);
            break;
        }
        usleep(2000);
        pthread_mutex_lock(&g_wb.mu);
    }
    const uint64_t left = g_wb.backlog;
    pthread_mutex_unlock(&g_wb.mu);
    nc_log("hakuX-native-io", "cachewrite: %s: %llu KiB waiting written in %.0f ms%s", why,
           (unsigned long long)((had - left) >> 10), (wb_ns() - t0) / 1e6, ok ? "" : " (not all: timed out)");
    return ok;
}

void nc_io_close(NcKernel *k, uint32_t object)
{
    writes_drain();                 /* a close is behind the writes already queued */
    uint32_t i = nc_vm_rd32(k->vm, object + FO_INDEX);
    if (i < MAX_FILES && g_files[i].used) {
        NcFile *nf = &g_files[i];
        if (nf->fd >= 0 && nf->wrote) {
            CloseJob j = {.fd = nf->fd, .wb_handle = nf->wb_handle};   /* patch 307: close the helper handle too */
            snprintf(j.path, sizeof(j.path), "%s", nf->path);
            snprintf(j.old_path, sizeof(j.old_path), "%s", nf->old_path);
            if (!wb_close(&j, nf->host)) {     /* patch 261: behind its queued writes */
                close_later(nf);
            }
        } else {
            if (nf->fd >= 0) {
                close(nf->fd);
            }
            if (nf->wb_handle > 0) {
                wbwr_close(nf->wb_handle);     /* patch 307: an unwritten cache file's helper handle */
            }
            if (nf->old_path[0]) {
                unlink(nf->old_path);
            }
        }
        if (nf->del) {
            NcFile *o = file_other(nf, nf->host, false);
            if (o) {
                o->del = true;                      /* the last handle deletes it */
            } else {
                if (ram_enabled()) {
                    ram_drop(nf->host);
                }
                unlink(nf->host);
            }
        }
        free(nf->listing);
        nf->listing = NULL;
        nf->used = false;
    }
}

static void io_status(NcKernel *k, uint32_t iosb, uint32_t status, uint32_t info)
{
    if (iosb) {
        nc_vm_wr32(k->vm, iosb, status);
        nc_vm_wr32(k->vm, iosb + 4, info);
    }
}

/* An I/O call finished: a[] is (FileHandle, Event, ApcRoutine, ApcContext,
 * IoStatusBlock, ...), as NtReadFile and its siblings take it. The event is
 * set; the APC routine runs as ApcRoutine(ApcContext, IoStatusBlock, 0) when
 * the caller next waits alertably. */
static void complete(NcKernel *k, const uint32_t *a)
{
    if (a[1]) {
        uint32_t ev = nc_ob_lookup(k, a[1], NULL);
        if (ev) {
            nc_ke_set_event(k, ev);
        }
    } else {
        /* no event of its own: the file object's, which a wait on the file
         * handle waits for (cleared as an I/O starts, set as it completes:
         * ours complete at once) */
        enum NcObType type;
        const uint32_t fo = nc_ob_lookup(k, a[0], &type);
        if (fo && type == NC_OB_FILE) {
            nc_ke_set_event(k, fo + NC_FO_EVENT);
        }
    }
    if (a[2] && k->current) {
        const NcApc apc = {.normal = a[2], .ctx = a[3], .a1 = a[4], .a2 = 0};
        nc_apc_queue(k, k->current, &apc);
    }
}

/* ---- RAM cache for the utility/cache drives (XOG patch 288, xogfix=ramcache, per game) ----
 * Cache-partition files (Partition3-5) are throwaway: games rebuild them. We keep their DATA in
 * host RAM so writes during play never reach PS5 storage, and so they never count toward the
 * per-launch write cap that makes big games crawl after ~1 GiB written in a launch. A real 0-byte
 * placeholder file still lives on disk for each cache file, so every path/dir/stat/delete/rename
 * path below is unchanged; only the bytes and the reported size come from RAM. Strictly the cache
 * partitions, never the save partitions (Partition1 TDATA/UDATA, which must persist). Capped; past
 * the cap a file falls back to a normal on-disk write. RAM is per launch (freed on exit): the game
 * rebuilds its cache each launch, to RAM, which is fast and uncapped. */
#define RAM_FILES 256
#define RAM_CEILING (512ull << 20)  /* never hold more than this, whatever is free */
#define RAM_MARGIN  (384ull << 20)  /* host memory kept free for everything else */
typedef struct RamFile {
    bool used;
    char host[512];
    uint8_t *data;
    uint64_t size, cap;             /* cap = bytes allocated */
    uint64_t atime;                 /* last-access tick, for LRU eviction */
    bool spilled;                   /* flushed to disk: a tombstone, so it is not re-cached into RAM */
} RamFile;
static RamFile g_ram[RAM_FILES];
static uint64_t g_ram_total;        /* bytes allocated across all RAM files */
static uint64_t g_ram_limit;        /* adaptive cap, set in ram_enabled() */
static uint64_t g_ram_tick;
static int g_ram_on = -1;

static bool ram_enabled(void)
{
    if (g_ram_on < 0) {
        const char *e = getenv("XOG_FIX");
        g_ram_on = e && strstr(e, "ramcache") ? 1 : 0;
        if (g_ram_on) {
            /* Patch 292: adaptive cap so we can never run the app out of memory (Fable's ~675 MiB cache
             * OOM'd a fixed 1280 MiB cap). Keep RAM_MARGIN free for everything else, never hold more than
             * RAM_CEILING; a file past the cap spills to disk (LRU) instead of crashing. */
            extern int sceKernelAvailableFlexibleMemorySize(size_t * out);
            size_t avail = 0;
            if (sceKernelAvailableFlexibleMemorySize(&avail) != 0) {
                avail = 0;
            }
            g_ram_limit = (uint64_t)avail > RAM_MARGIN ? (uint64_t)avail - RAM_MARGIN : (uint64_t)avail / 4;
            if (g_ram_limit > RAM_CEILING) {
                g_ram_limit = RAM_CEILING;
            }
            if (g_ram_limit < (64ull << 20)) {
                g_ram_limit = 64ull << 20;   /* a floor; realloc returning NULL still guards OOM */
            }
            nc_log("hakuX-native-io", "ramcache: cache-drive files kept in RAM (xogfix=ramcache); %llu MiB flexible "
                   "free -> cap %llu MiB, overflow spills to disk", (unsigned long long)((uint64_t)avail >> 20),
                   (unsigned long long)(g_ram_limit >> 20));
        }
    }
    return g_ram_on == 1;
}

/* A host path on a cache/utility partition (under the per-game cache root). */
static bool ram_is_cache(const char *host)
{
    if (!g_cache_root[0]) {
        return false;
    }
    const size_t n = strlen(g_cache_root);
    return strncmp(host, g_cache_root, n) == 0 && host[n] == '/';
}

static int ram_find(const char *host)
{
    for (int i = 0; i < RAM_FILES; i++) {
        if (g_ram[i].used && !g_ram[i].spilled && strcmp(g_ram[i].host, host) == 0) {
            return i;
        }
    }
    return -1;
}

static void ram_free(int i)
{
    if (i < 0 || !g_ram[i].used) {
        return;
    }
    free(g_ram[i].data);
    g_ram_total -= g_ram[i].cap;
    memset(&g_ram[i], 0, sizeof(g_ram[i]));
}

/* Ensure RAM file i can hold need bytes; false if that would pass the cap. */
static bool ram_reserve(int i, uint64_t need)
{
    if (need <= g_ram[i].cap) {
        return true;
    }
    uint64_t want = g_ram[i].cap ? g_ram[i].cap : (1ull << 20);
    while (want < need) {
        want += want / 2 + (1ull << 20);
    }
    if (g_ram_total - g_ram[i].cap + want > g_ram_limit) {
        return false;
    }
    uint8_t *d = realloc(g_ram[i].data, want);
    if (!d) {
        return false;
    }
    g_ram_total += want - g_ram[i].cap;
    g_ram[i].data = d;
    g_ram[i].cap = want;
    return true;
}

/* Track a cache file. reset: start it empty (a create or overwrite). Returns its index, or -1 if
 * RAM is full (the caller then uses the on-disk file). */
static int ram_track(const char *host, bool reset)
{
    for (int j = 0; j < RAM_FILES; j++) {
        if (g_ram[j].used && strcmp(g_ram[j].host, host) == 0) {
            if (g_ram[j].spilled) {
                if (!reset) {
                    return -1;              /* already on disk: stay disk-backed */
                }
                g_ram[j].spilled = false;   /* recreated (create/overwrite): cache it again, empty */
                g_ram[j].size = 0;
                return j;
            }
            if (reset) {
                g_ram[j].size = 0;
            }
            return j;
        }
    }
    for (int j = 0; j < RAM_FILES; j++) {
        if (!g_ram[j].used) {
            memset(&g_ram[j], 0, sizeof(g_ram[j]));
            g_ram[j].used = true;
            snprintf(g_ram[j].host, sizeof(g_ram[j].host), "%s", host);
            return j;
        }
    }
    return -1;
}

static ssize_t ram_pwrite(int i, const uint8_t *src, uint64_t off, uint32_t len)
{
    if (!ram_reserve(i, off + len)) {
        return -1;
    }
    if (off > g_ram[i].size) {
        memset(g_ram[i].data + g_ram[i].size, 0, off - g_ram[i].size);
    }
    memcpy(g_ram[i].data + off, src, len);
    if (off + len > g_ram[i].size) {
        g_ram[i].size = off + len;
    }
    g_ram[i].atime = ++g_ram_tick;
    return (ssize_t)len;
}

static uint32_t ram_pread(int i, uint8_t *dst, uint64_t off, uint32_t len)
{
    if (off >= g_ram[i].size) {
        return 0;
    }
    const uint64_t avail = g_ram[i].size - off;
    const uint32_t n = len < avail ? len : (uint32_t)avail;
    memcpy(dst, g_ram[i].data + off, n);
    g_ram[i].atime = ++g_ram_tick;
    return n;
}

static void ram_truncate(int i, uint64_t v)
{
    if (v < g_ram[i].size) {
        g_ram[i].size = v;
    } else if (v > g_ram[i].size && ram_reserve(i, v)) {
        memset(g_ram[i].data + g_ram[i].size, 0, v - g_ram[i].size);
        g_ram[i].size = v;
    }
}

static void ram_drop(const char *host)
{
    for (int i = 0; i < RAM_FILES; i++) {
        if (g_ram[i].used && strcmp(g_ram[i].host, host) == 0) {
            ram_free(i);   /* clears the slot, tombstone included */
            return;
        }
    }
}

static void ram_rename(const char *from, const char *to)
{
    int src = -1, dst = -1;
    for (int i = 0; i < RAM_FILES; i++) {
        if (!g_ram[i].used) {
            continue;
        }
        if (strcmp(g_ram[i].host, to) == 0) {
            dst = i;
        } else if (strcmp(g_ram[i].host, from) == 0) {
            src = i;
        }
    }
    if (dst >= 0) {
        ram_free(dst);
    }
    if (src >= 0) {
        snprintf(g_ram[src].host, sizeof(g_ram[src].host), "%s", to);
    }
}

/* Would RAM file i still be within the cap if it had to hold need bytes? */
static bool ram_fits(int i, uint64_t need)
{
    if (need <= g_ram[i].cap) {
        return true;
    }
    uint64_t want = g_ram[i].cap ? g_ram[i].cap : (1ull << 20);
    while (want < need) {
        want += want / 2 + (1ull << 20);
    }
    return g_ram_total - g_ram[i].cap + want <= g_ram_limit;
}

/* Write RAM file i out to fd (its on-disk placeholder) and free its RAM, so it becomes disk-backed
 * (ram_find then returns -1 and later reads/writes/size use the real file). */
static void ram_flush(int i, int fd)
{
    if (g_ram[i].data) {
        /* XOG patch 308: a spill is the app writing the whole file (8-10 MiB
         * in the logs). Past the per-launch cap that pwrite ran at a few
         * hundred KB/s and held the game. Through the helper when it is up;
         * nc_host_write falls back to its own descriptor (or fd) on failure. */
        NcHostOut out;
        if (nc_host_out_open(&out, g_ram[i].host, 0666, false) == 0) {
            const long w = nc_host_write(&out, g_ram[i].data, g_ram[i].size);
            if (w != (long)g_ram[i].size && fd >= 0) {
                uint64_t off = w > 0 ? (uint64_t)w : 0;
                while (off < g_ram[i].size) {
                    const ssize_t n = pwrite(fd, g_ram[i].data + off, (size_t)(g_ram[i].size - off), (off_t)off);
                    if (n <= 0) {
                        break;
                    }
                    off += (uint64_t)n;
                }
            }
            (void)nc_host_out_close(&out);   /* cache data: no fsync, as the old pwrite path */
        } else if (fd >= 0) {
            uint64_t off = 0;
            while (off < g_ram[i].size) {
                const ssize_t w = pwrite(fd, g_ram[i].data + off, (size_t)(g_ram[i].size - off), (off_t)off);
                if (w <= 0) {
                    break;
                }
                off += (uint64_t)w;
            }
        }
    }
    free(g_ram[i].data);
    g_ram_total -= g_ram[i].cap;
    g_ram[i].data = NULL;
    g_ram[i].cap = 0;
    g_ram[i].size = 0;
    g_ram[i].spilled = true;            /* keep the slot as a tombstone (ram_track won't re-cache it) */
}

/* Spill the least-recently-used RAM file other than keep to disk, to make room. False if none. */
static bool ram_evict_one(int keep)
{
    int lru = -1;
    uint64_t best = UINT64_MAX;
    for (int j = 0; j < RAM_FILES; j++) {
        if (g_ram[j].used && !g_ram[j].spilled && j != keep && g_ram[j].atime < best) {
            best = g_ram[j].atime;
            lru = j;
        }
    }
    if (lru < 0) {
        return false;
    }
    const int fd = open(g_ram[lru].host, O_WRONLY | O_CLOEXEC);
    nc_log("hakuX-native-io", "ramcache: spill %s (%llu KiB) to disk (LRU; cap %llu MiB reached)",
           g_ram[lru].host, (unsigned long long)(g_ram[lru].size >> 10),
           (unsigned long long)(g_ram_limit >> 20));
    ram_flush(lru, fd);
    if (fd >= 0) {
        close(fd);
    }
    return true;
}

/* ------------------------------------------------------- the disc reader */

/* An overlapped disc read (NcFile.async) runs on the reader thread and the
 * call reports STATUS_PENDING at once, as the Xbox's DVD transfers do. Read
 * in place, one from the image on the SD card held the guest's CPU for
 * 10-70 ms, interrupts included: Dead or Alive 2 Ultimate streams during its
 * fights, and each read was a dropped frame. The kernel completes a read on
 * the guest's CPU (nc_io_poll): its IoStatusBlock, then its event (the file
 * object's without one of its own, cleared as the read starts, as NT does)
 * and its APC, queued to the thread that issued it. A full queue reads in
 * place. Overlapped reads of hard disk files go the same way (a duplicate
 * of the descriptor, closed when done): Forza streams its track from the Z:
 * cache during a race, 8-28 ms a read in place, a frame a vblank late now
 * and then. Long synchronous reads go there too, only their thread waiting
 * (NtReadFile, SYNC_LATER_MIN).
 *
 * The reader writes the buffer's physical pages through the RAM view, as a
 * device does: a page the GPU may still read (the HLE GPU's first-write
 * barrier) is waited for there, not on the guest's CPU (Forza's buffers
 * held it 10-19 ms that way, the read itself queued), and the write is
 * reported dirty when the read completes (nc_vm_dma). */
#define DISC_JOBS 64

/* Console timing (the app's setting, nc_io_set_device_time): a read takes at
 * least a quarter of the time the console's drives take at their best (the
 * whole of it made loads too slow), and the thread that asked waits it
 * out. At the phone's speed a load ran without its
 * thread ever waiting, so the game's lower-priority threads didn't run
 * until it was done: Morrowind starts its loading screen's thread as a cell
 * load begins, and on the Xbox the thread sets up its first picture while
 * the load still reads. A load done first (a cell it held) found that
 * setup half done, asked the thread to end and waited for it, while the
 * thread waited for that request to clear: a freeze leaving a building.
 * So every synchronous read the file system's cache doesn't hold goes to
 * the reader, its caller waiting (NtReadFile), and the reader spends a
 * request's time plus the transfer at the drive's rate on each, one at a
 * time. NC_DEVTIME="disc_us,disc_kBps,hdd_us,hdd_kBps" (or the property
 * debug.hakux.devtime, read at boot) tunes the drives. */
static bool g_devtime;
static struct DevRate {
    uint32_t req_us, kbps;
} g_dev_disc = {125, 27600},    /* the DVD drive at 5x: 500 us, 6900 kB/s */
  g_dev_hdd = {50, 80000};      /* the hard disk: 200 us, 20000 kB/s */

void nc_io_set_device_time(bool on)
{
    if (on && getenv("NC_CHIHIRO")) {
        /* Sega Chihiro (XPSemu patch 208): the game runs from the media board's DIMM (RAM), not a DVD drive */
        on = false;
        nc_log("hakuX-native-io", "console timing: off for Sega Chihiro (the game is in the media board's RAM)");
    }
    g_devtime = on;
    const char *e = getenv("NC_DEVTIME");
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX];
    if (!e && hle_prop_get("debug.hakux.devtime", v) > 0) {
        e = v;
    }
#endif
    if (e) {
        sscanf(e, "%u,%u,%u,%u", &g_dev_disc.req_us, &g_dev_disc.kbps, &g_dev_hdd.req_us, &g_dev_hdd.kbps);
    }
    nc_log("hakuX-native-io", "console timing %s: disc %u us + %u kB/s, hard disk %u us + %u kB/s", on ? "on" : "off",
           g_dev_disc.req_us, g_dev_disc.kbps, g_dev_hdd.req_us, g_dev_hdd.kbps);
}

/* Console timing paces what runs beside the picture. A title that stopped flipping is in a
 * blocking load: nothing to pace, and waiting out the drive only lengthens the freeze. */
#define DEVTIME_NO_FLIP_NS 100000000ull
static uint32_t g_reads_paced, g_reads_free;

void nc_io_read_kinds(uint32_t *paced, uint32_t *free_reads)
{
    *paced = g_reads_paced;
    *free_reads = g_reads_free;
    g_reads_paced = g_reads_free = 0;
}

/* xogfix=drivetime (patch 329, per game: Breakdown). The drive's own time even while no frame shows: Breakdown
 * starts a 1 MB read of a level's event file on its file-server thread (above the main thread) and only then stores
 * the buffer pointer its completion uses; with the read done from the page cache in microseconds, the server thread
 * ran the completion first, wrote through a NULL+0x10 and the game crashed as its first level loaded. At the
 * drive's speed (as a console's ~13 ms for 1 MB) the main thread always stores it first. */
/* patch 332: a per-game xogfix word (the start line's xogfix=, read once a name) */
static bool xogfix_has(const char *w, int *cache)
{
    if (*cache < 0) {
        const char *e = getenv("XOG_FIX");
        *cache = e && strstr(e, w) ? 1 : 0;
    }
    return *cache == 1;
}

static bool drivetime_fix(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XOG_FIX");
        on = e && strstr(e, "drivetime") ? 1 : 0;
    }
    return on;
}

static bool devtime_now(void)
{
    if (drivetime_fix()) {
        g_reads_paced++;
        return true;
    }
    if (!g_devtime) {
        return false;
    }
    const uint64_t flip = nc_dev_nv2a_last_flip_ns();
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    const bool on = !flip || (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec - flip < DEVTIME_NO_FLIP_NS;
    if (on) {
        g_reads_paced++;
    } else {
        g_reads_free++;
    }
    return on;
}

/* What each page last took from a file (at a stop or a bad table, the log says). */
#define READ_PAGES (0x10000000u / NC_PAGE)
static struct {
    uint32_t va, serial;
    uint64_t off;
} g_page_read[READ_PAGES];
static uint32_t g_read_serial;
extern bool rx_vm_is_table(uint32_t pa);

static void nc_io_note_read(uint32_t pa, uint32_t len, uint32_t va, uint64_t off)
{
    const uint32_t serial = __atomic_add_fetch(&g_read_serial, 1, __ATOMIC_RELAXED);
    for (uint32_t p = pa / NC_PAGE; p <= (pa + (len ? len - 1 : 0)) / NC_PAGE && p < READ_PAGES; p++) {
        g_page_read[p].va = va;
        g_page_read[p].off = off;
        g_page_read[p].serial = serial;
        if (rx_vm_is_table(p * NC_PAGE)) {
            static unsigned told;
            if (told++ < 32) {
                nc_log("xog-tables", "a file read for va %08x, offset %llx, lands on table page %08x", va, (unsigned long long)off, p * NC_PAGE);
            }
        }
    }
}

void nc_io_page_report(uint32_t pa)
{
    const uint32_t p = pa / NC_PAGE;
    if (p < READ_PAGES && g_page_read[p].serial) {
        nc_log("xog-tables", "  page %08x last took file data as read %u of %u: va %08x, offset %llx", pa, g_page_read[p].serial, g_read_serial,
               g_page_read[p].va, (unsigned long long)g_page_read[p].off);
    } else {
        nc_log("xog-tables", "  page %08x never took file data", pa);
    }
}

extern uint8_t *g_va_soft;

static bool soft_range(uint32_t va, uint32_t len)
{
    for (uint64_t b = va >> 14, e = ((uint64_t)va + len - 1) >> 14; len && b <= e; b++) {
        if (g_va_soft[b]) {
            return true;
        }
    }
    return false;
}

/* The drive's time for n bytes of nf (console timing on), in microseconds. */
static uint32_t device_us(const NcFile *nf, uint32_t n)
{
    if (!devtime_now()) {
        return 0;
    }
    const struct DevRate *r = nf->kind == F_DISC ? &g_dev_disc : &g_dev_hdd;
    return r->req_us + (r->kbps ? (uint32_t)((uint64_t)n * 1000 / r->kbps) : 0);
}

/* A read's buffer as guest virtual runs, filled in order: NtReadFile's one,
 * NtReadFileScatter's pages. */
typedef struct IoRun {
    uint32_t va, len;
} IoRun;

typedef struct DiscJob {
    const NcDisc *disc;
    NcDiscFile file;
    int fd;                  /* a hard disk file's (a duplicate), else -1 */
    uint64_t off;
    struct DmaSeg {
        uint8_t *host;       /* the RAM view's */
        uint32_t pa, len;
    } *seg;                  /* the buffer's physical runs */
    unsigned nseg;
    uint64_t gpu;            /* the GPU value to wait for first (nc_vm_dma.fence), or 0 */
    uint32_t va, n;
    uint32_t dev_us;         /* the drive's time for it (console timing), 0: none */
    uint32_t iosb, event, apc, apc_ctx;
    NcThread *thread;
    uint32_t thread_id;      /* its id then: the slot may hold another thread by the end */
    bool sync;               /* a synchronous handle's: the thread waits for it (nc_ke_block) */
    bool ok;
    int err;
    bool write;              /* a write to a host file (the guest's buffer to the file) */
    int wb;                  /* patch 330: the file's handle on the write helper (>0), so the write isn't throttled */
    char path[64]; /* for the log */
} DiscJob;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    DiscJob q[DISC_JOBS];    /* for the reader */
    unsigned head, n;
    DiscJob done[DISC_JOBS]; /* read, for the kernel to complete */
    unsigned ndone;
    unsigned busy;           /* queued, reading or done: at most DISC_JOBS */
    bool started;
    bool hurry;              /* a savestate's drain: no drive time left to wait (atomic) */
    unsigned writes;         /* writes queued or being written */
    pthread_cond_t written;  /* signalled when writes reaches 0 */
} g_reader = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, .written = PTHREAD_COND_INITIALIZER};

/* Host writes go to the reader thread too (as the reference does: The Punisher
 * copies archives to the cache drive while the guest CPU runs on). Any file
 * query, close, delete or rename waits for them first, so it sees the bytes. */
static void writes_drain(void)
{
    pthread_mutex_lock(&g_reader.mu);
    while (g_reader.writes) {
        pthread_cond_wait(&g_reader.written, &g_reader.mu);
    }
    pthread_mutex_unlock(&g_reader.mu);
}

/* patch 330: a queued write's bytes, through the un-throttled helper when the file has a handle there (patch 307/308),
 * else (or should that fail) by the app's own descriptor. */
static bool job_write(const DiscJob *j, const void *p, size_t len, uint64_t o)
{
    if (j->wb > 0 && len <= 0xFFFFFFFFu && wbwr_write(j->wb, o, p, (uint32_t)len) == (long)len) {
        return true;
    }
    return nc_pwrite_sure(j->fd, p, len, o);
}

/* XOG patch 253: the drive's time on the guest's clock (ps5/src/ff_ps5.c): twice as fast with fast-forward on */
uint64_t nc_ff_now_ns(void);
unsigned nc_ff_mul(void);

static uint64_t mono_ns(void)
{
    return nc_ff_now_ns();
}

/* A request's drive time, until due (mono_ns), in slices a savestate's drain
 * cuts short: the guest stands still then, and sees the read land as it goes
 * on. The next request waits for it too, as the drive's. */
static void drive_wait(uint64_t due)
{
    for (uint64_t now; !__atomic_load_n(&g_reader.hurry, __ATOMIC_ACQUIRE) && (now = mono_ns()) < due;) {
        const uint64_t left = due - now;
        const struct timespec ts = {0, (long)((left < 1000000 ? left : 1000000) / nc_ff_mul())};
        nanosleep(&ts, NULL);
    }
}

static void *reader_thread(void *arg)
{
    pthread_mutex_lock(&g_reader.mu);
    for (;;) {
        while (!g_reader.n) {
            pthread_cond_wait(&g_reader.cv, &g_reader.mu);
        }
        DiscJob j = g_reader.q[g_reader.head];
        g_reader.head = (g_reader.head + 1) % DISC_JOBS;
        g_reader.n--;
        pthread_mutex_unlock(&g_reader.mu);
        const uint64_t due = mono_ns() + (uint64_t)j.dev_us * 1000;
        if (j.gpu && nc_vm_dma.wait) {
            nc_vm_dma.wait(j.gpu);
        }
        errno = 0;
        j.ok = true;
        uint64_t o = j.off;
        size_t total = 0;
        for (unsigned i = 0; i < j.nseg; i++) {
            total += j.seg[i].len;
        }
        uint8_t *bounce = j.nseg > 1 && total <= (16u << 20) ? malloc(total) : NULL;
        const bool gathered = bounce != NULL;
        if (gathered) {
            if (j.write) {
                size_t at = 0;
                for (unsigned i = 0; i < j.nseg; i++) {
                    memcpy(bounce + at, j.seg[i].host, j.seg[i].len);
                    at += j.seg[i].len;
                }
                j.ok = job_write(&j, bounce, total, o);
            } else {
                j.ok = j.fd >= 0 ? nc_pread_sure(j.fd, bounce, total, o)
                                 : nc_disc_read_file(j.disc, &j.file, o, bounce, total);
                size_t at = 0;
                for (unsigned i = 0; j.ok && i < j.nseg; i++) {
                    memcpy(j.seg[i].host, bounce + at, j.seg[i].len);
                    at += j.seg[i].len;
                }
            }
            free(bounce);
        }
        for (unsigned i = 0; !gathered && j.ok && i < j.nseg; i++) {
            const struct DmaSeg *g = &j.seg[i];
            if (!j.write) {
                nc_io_note_read(g->pa, g->len, j.va, j.off);
            }
            j.ok = j.write ? job_write(&j, g->host, g->len, o)
                   : j.fd >= 0 ? nc_pread_sure(j.fd, g->host, g->len, o)
                             : nc_disc_read_file(j.disc, &j.file, o, g->host, g->len);
            o += g->len;
        }
        j.err = j.ok ? 0 : errno;
        if (j.dev_us) {
            drive_wait(due);
        }
        pthread_mutex_lock(&g_reader.mu);
        if (j.write && g_reader.writes && !--g_reader.writes) {
            pthread_cond_broadcast(&g_reader.written);
        }
        g_reader.done[g_reader.ndone++] = j;
        pthread_mutex_unlock(&g_reader.mu);
        nc_irq_post_work();
        pthread_mutex_lock(&g_reader.mu);
    }
    return NULL;
}

/* The buffer's pages stay put while the reader thread writes them: a
 * contiguous allocation relocates virtual pages (vm.c), and a page copied
 * while the read went on lost what the reader wrote after the copy. Pinned
 * and unpinned on the guest's CPU, where relocation runs. */
static void pin_buffer(NcKernel *k, const DiscJob *j, int lock)
{
    for (unsigned i = 0; i < j->nseg; i++) {
        const uint32_t pa = j->seg[i].pa, end = pa + j->seg[i].len;
        for (uint32_t p = pa & ~(NC_PAGE - 1); p < end; p += NC_PAGE) {
            nc_vm_pin(k->vm, p, false, lock);
        }
    }
}

/* Queues a read of n bytes at off into the buffer's runs (resident and
 * writable); false when it must happen in place. sync: the calling thread
 * waits for it, every other one runs on. dev_us: the drive's time for it. */
static bool disc_read_later(NcKernel *k, const NcFile *nf, const uint32_t *a, const IoRun *run, unsigned nrun,
                            uint64_t off, uint32_t n, bool sync, uint32_t dev_us, bool write)
{
    static int in_place = -1; /* NC_DISC_SYNC: every disc read in place, as before */
    if (in_place < 0) {
        in_place = getenv("NC_DISC_SYNC") != NULL;
    }
    if (in_place) {
        if (g_trace_io) {
            nc_log("hakuX-native-io", "%s not queued: NC_DISC_SYNC", nf->path);
        }
        return false;
    }
    uint32_t event = 0;
    if (a[1]) {
        event = nc_ob_lookup(k, a[1], NULL);
        if (!event) {
            if (g_trace_io) {
                nc_log("hakuX-native-io", "%s not queued: event handle %08x unknown", nf->path, a[1]);
            }
            return false;
        }
    } else {
        enum NcObType type;
        const uint32_t fo = nc_ob_lookup(k, a[0], &type);
        if (fo && type == NC_OB_FILE) {
            event = fo + NC_FO_EVENT;
        }
    }
    DiscJob j = {.disc = k->disc, .file = nf->disc, .fd = -1, .off = off, .va = run[0].va, .n = n,
                 .dev_us = dev_us, .iosb = a[4], .event = event, .apc = a[2], .apc_ctx = a[3],
                 .thread = k->current, .thread_id = k->current ? k->current->id : 0, .sync = sync,
                 .write = write, .wb = write && nf->wb_handle > 0 ? nf->wb_handle : 0};
    snprintf(j.path, sizeof(j.path), "%s", nf->path);
    size_t segs = 0;
    for (unsigned r = 0; r < nrun; r++) {
        segs += run[r].len / NC_PAGE + 2;
    }
    j.seg = malloc(sizeof(*j.seg) * segs);
    if (!j.seg) {
        nc_log("hakuX-native-io", "%s not queued: no memory for %zu runs", nf->path, segs);
        return false;
    }
    for (unsigned r = 0; r < nrun; r++) {
        for (uint32_t va = run[r].va, left = run[r].len; left;) {
            const uint32_t room = NC_PAGE - (va & (NC_PAGE - 1)), chunk = left < room ? left : room;
            const uint32_t pa = nc_vm_phys(k->vm, va);
            if (pa == UINT32_MAX || (uint64_t)pa + chunk > k->vm->ram_size || g_va_soft[va >> 14] == 2) {
                if (g_trace_io) {
                    nc_log("hakuX-native-io", "%s not queued: va %08x pa %08x soft %u", nf->path, va, pa,
                           g_va_soft[va >> 14]);
                }
                free(j.seg);
                return false;               /* not all RAM, or a shadow's: in place */
            }
            struct DmaSeg *last = j.nseg ? &j.seg[j.nseg - 1] : NULL;
            if (last && last->pa + last->len == pa) {
                last->len += chunk;
            } else {
                j.seg[j.nseg++] = (struct DmaSeg){k->vm->phys + pa, pa, chunk};
            }
            va += chunk;
            left -= chunk;
        }
    }
    /* patch 331: the job's own descriptor. The PS5 refuses dup() to this app (EPERM), so every hard-disk read was
     * done in the call and completed at once, where a console's (and the disc's here) pends: Breakdown's file
     * thread ran a read's completion before the main thread had stored what it uses, and crashed loading its
     * first level. Where dup() fails the file is opened again by its path. */
    /* patch 332: per game (xogfix=hddqueue, Breakdown); every other game keeps hard-disk reads in the call */
    static int hddq = -1;
    if (nf->kind == F_HOST && !xogfix_has("hddqueue", &hddq) && (j.fd = dup(nf->fd)) < 0) {
        free(j.seg);
        return false;
    }
    if (nf->kind == F_HOST && hddq == 1) {
        static int no_dup;
        if (!no_dup && (j.fd = dup(nf->fd)) < 0) {
            no_dup = 1;
            nc_log("hakuX-native-io", "dup: %s: queued reads and writes open the file again", strerror(errno));
        }
        if (j.fd < 0) {
            j.fd = open(nf->host, (write ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
        }
        if (j.fd < 0) {
            static unsigned told;
            if (told++ < 8 || g_trace_io) {
                nc_log("hakuX-native-io", "%s not queued: open %s: %s", nf->path, nf->host, strerror(errno));
            }
            free(j.seg);
            return false;
        }
    }
    for (unsigned i = 0; nc_vm_dma.fence && i < j.nseg; i++) {
        const uint64_t v = nc_vm_dma.fence(j.seg[i].pa, j.seg[i].len);
        j.gpu = v > j.gpu ? v : j.gpu;
    }
    pthread_mutex_lock(&g_reader.mu);
    if (!g_reader.started) {
        pthread_t t;
        g_reader.started = pthread_create(&t, NULL, reader_thread, NULL) == 0;
        if (g_reader.started) {
            pthread_detach(t);
        }
    }
    const bool queued = g_reader.started && g_reader.busy < DISC_JOBS;
    if (queued) {
        pin_buffer(k, &j, 1);
        g_reader.writes += write;
        g_reader.q[(g_reader.head + g_reader.n++) % DISC_JOBS] = j;
        g_reader.busy++;
        pthread_cond_signal(&g_reader.cv);
    }
    pthread_mutex_unlock(&g_reader.mu);
    if (!queued) {
        if (g_trace_io) {
            nc_log("hakuX-native-io", "%s not queued: reader busy %u started %d", nf->path, g_reader.busy,
                   g_reader.started);
        }
        if (j.fd >= 0) {
            close(j.fd);
        }
        free(j.seg);
    }
    if (queued && event) {
        nc_ke_clear_event(k, event);
    }
    return queued;
}

static void disc_read_done(NcKernel *k, const DiscJob *j)
{
    pin_buffer(k, j, -1);
    const bool ok = j->ok;
    const int err = j->err;
    for (unsigned i = 0; !j->write && i < j->nseg; i++) {
        if (nc_vm_dma.wrote) {
            nc_vm_dma.wrote(j->seg[i].pa, j->seg[i].len);    /* what a guest store would mark */
        }
    }
    free(j->seg);
    if (j->fd >= 0) {
        close(j->fd);
    }
    uint32_t st = NC_STATUS_SUCCESS, n = j->n;
    if (!ok) {
        nc_log("hakuX-native-io", "%s %s +%llx (%u) %s %08x failed: %s", j->write ? "write" : "read", j->path,
               (unsigned long long)j->off, j->n, j->write ? "from" : "into", j->va, strerror(err));
        st = j->write ? 0xC000007Fu /* STATUS_DISK_FULL */ : 0xC000009Cu; /* STATUS_DEVICE_DATA_ERROR */
        n = 0;
    }
    io_status(k, j->iosb, st, n);
    if (j->event) {
        nc_ke_set_event(k, j->event);
    }
    if (j->sync && j->thread && j->thread->id == j->thread_id && j->thread->state == NC_T_WAITING) {
        nc_ke_wake(k, j->thread, st);   /* its NtReadFile returns now */
    }
    if (j->apc && j->thread && j->thread->id == j->thread_id && j->thread->state != NC_T_FREE &&
        j->thread->state != NC_T_TERMINATED) {
        const NcApc apc = {.normal = j->apc, .ctx = j->apc_ctx, .a1 = j->iosb, .a2 = 0};
        nc_apc_queue(k, j->thread, &apc);
    }
}

void nc_io_poll(NcKernel *k)
{
    if (!nc_irq_take_work()) {
        return;
    }
    static DiscJob done[DISC_JOBS]; /* the guest CPU's thread only */
    pthread_mutex_lock(&g_reader.mu);
    const unsigned n = g_reader.ndone;
    memcpy(done, g_reader.done, n * sizeof(done[0]));
    g_reader.ndone = 0;
    g_reader.busy -= n;
    pthread_mutex_unlock(&g_reader.mu);
    for (unsigned i = 0; i < n; i++) {
        disc_read_done(k, &done[i]);
    }
}

/* ---------------------------------------------------------------- open */

static void remove_tree(const char *path, bool self);

/* side files a dead process left were swept (or kept: a savestate's files
 * use theirs) */
static bool g_swept;

/* Moves an overwritten file's old contents to <hdd>/.overwrite (NcFile.old_path),
 * outside every partition. The first time in a run, side files a dead process
 * left there go. */
static bool set_aside(const char *host, char *old_path, size_t len)
{
    static unsigned serial;
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/.overwrite", g_hdd_root);
    if (!g_swept) {
        remove_tree(dir, false);
        g_swept = true;
    }
    mkdir(dir, 0777);
    snprintf(old_path, len, "%s/%u-%u", dir, (unsigned)getpid(), ++serial);
    if (rename(host, old_path) != 0) {
        nc_log("hakuX-native-io", "overwrite %s: keeping its old bytes failed: %s", host, strerror(errno));
        return false;
    }
    return true;
}

/* An overwritten file still empty, growing: its side file is what it should hold, whole. */
static bool regrow_by_rename(NcFile *nf)
{
    if (rename(nf->old_path, nf->host) != 0) {
        return false;
    }
    const int fd = open(nf->host, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        nc_log("hakuX-native-io", "regrow %s: can't open it again: %s", nf->path, strerror(errno));
        return false;
    }
    close(nf->fd);
    nf->fd = fd;
    nf->old_path[0] = 0;
    nf->old_size = 0;
    return true;
}

/* An overwritten file growing into [from, to): FATX shows what its clusters
 * held, the old contents (NcFile.old_path). */
static void regrow(NcFile *nf, uint64_t from, uint64_t to)
{
    if (!nf->old_path[0] || from >= nf->old_size) {
        return;
    }
    to = to < nf->old_size ? to : nf->old_size;
    const int old = open(nf->old_path, O_RDONLY | O_CLOEXEC);
    if (old < 0) {
        return;
    }
    /* XOG patch 308: the bytes a FATX overwrite brings back are written
     * through the helper too (an overwritten file kept all its writes in the
     * app before this). */
    int h = 0;
    if (wb_enabled() && g_wb_helper == 1) {
        if (nf->wb_handle == 0) {
            const int x = wbwr_open(nf->host);
            nf->wb_handle = x > 0 ? x : -1;
        }
        h = nf->wb_handle > 0 ? nf->wb_handle : 0;
    }
    static uint8_t buf[1u << 16];
    for (uint64_t o = from; o < to;) {
        const size_t want = to - o < sizeof(buf) ? (size_t)(to - o) : sizeof(buf);
        const ssize_t r = pread(old, buf, want, (off_t)o);
        bool ok = r > 0;
        if (ok) {
            ok = h && wbwr_write(h, o, buf, (uint32_t)r) == r;
            if (!ok) {
                ok = pwrite(nf->fd, buf, (size_t)r, (off_t)o) == r;
            }
        }
        if (!ok) {
            break;
        }
        o += (uint64_t)r;
    }
    close(old);
}

/* name relative to a RootDirectory (ObjectAttributes, a rename's): \?? for
 * XAPI's DOS paths ("D:\x"), else an open directory's path. False when the
 * handle isn't a file's. */
static bool resolve_name(NcKernel *k, uint32_t root, char *name, size_t cap)
{
    char joined[1024];
    if (root == OB_DOS_DEVICES) {
        snprintf(joined, sizeof(joined), "\\??\\%s", name);
    } else if (root) {
        const NcFile *rf = file_of(k, root);
        if (!rf) {
            return false;
        }
        snprintf(joined, sizeof(joined), "%s\\%s", rf->path, name);
    } else {
        return true;
    }
    snprintf(name, cap, "%s", joined);
    return true;
}

/* Opens (and creates) a file for NtCreateFile/NtOpenFile. Returns the status;
 * *info receives the IoStatusBlock information. */
/* An open whose FILE_DIRECTORY_FILE / FILE_NON_DIRECTORY_FILE doesn't fit
 * what the path is: logged (the first 20), as the title may retry it. */
static uint32_t dir_mismatch(const char *path, uint32_t options, uint32_t st)
{
    static unsigned logged;
    if (logged++ < 20) {
        nc_log("hakuX-native-io", "open %s (options %x): %s", path, options,
               st == STATUS_FILE_IS_A_DIRECTORY ? "a directory" : "not a directory");
    }
    return st;
}

static uint32_t do_open(NcKernel *k, uint32_t access, uint32_t attrs, uint32_t disposition, uint32_t options,
                        uint32_t *handle, uint32_t *info)
{
    writes_drain();                 /* an open sees writes already queued */
    char name[512], path[512];
    ansi(k, nc_vm_rd32(k->vm, attrs + 4), name, sizeof(name));
    const uint32_t root = nc_vm_rd32(k->vm, attrs);
    if (!resolve_name(k, root, name, sizeof(name))) {
        nc_log("hakuX-native-io", "open %s relative to handle %08x: not a directory handle", name, root);
        return NC_STATUS_INVALID_HANDLE;
    }
    if (!canon(name, path, sizeof(path))) {
        nc_log("hakuX-native-io", "open %s: no such drive", name);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    const char *rel;
    const int dev = device_of(path, &rel);
    *info = 5; /* FILE_DOES_NOT_EXIST */
    NcFile *nf = NULL;

    if (dev == -2) {
        /* No such object. As the object manager's lookup has it, a name
         * missing from an object directory (\, \??, \Device) is a missing
         * name, one under a missing component a missing path. Titles tell
         * them apart: GTA III probes a bare file name ("\??\x.dds" from
         * CreateFile("x.dds")) and shows its bad-disc screen on a bad path. */
        const char *last = strrchr(path, '\\');
        const size_t at = last ? (size_t)(last - path) : 0;
        const bool in_dir = at == 0 || (at == 3 && !strncmp(path, "\\??", 3)) ||
                            (at == 7 && !strncasecmp(path, "\\Device", 7));
        nc_log("hakuX-native-io", "open %s: unknown device (a missing %s)", path, in_dir ? "name" : "path");
        return in_dir ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (dev == -1) {
        if (k->disc->fd < 0 && !k->disc->ov) {
            return STATUS_NO_MEDIA_IN_DEVICE;   /* the dashboard booted with the tray empty */
        }
        if (disposition != FILE_OPEN && disposition != FILE_OPEN_IF) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        NcDiscFile df;
        if (!nc_disc_find(k->disc, rel, &df)) {
            nc_log("hakuX-native-io", "open %s: not on the disc", path);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        bool dir = df.attr & 0x10;
        if ((options & FILE_DIRECTORY_FILE) && !dir && !df.size) {
            dir = true;     /* an empty folder an image tool wrote as a 0-byte file */
            df.attr |= 0x10;
            nc_log("hakuX-native-io", "open %s: a 0-byte file on the disc, opened as the empty folder it was", path);
        }
        if ((options & FILE_DIRECTORY_FILE) && !dir) {
            return dir_mismatch(path, options, STATUS_NOT_A_DIRECTORY);
        }
        if ((options & FILE_NON_DIRECTORY_FILE) && dir) {
            return dir_mismatch(path, options, STATUS_FILE_IS_A_DIRECTORY);
        }
        nf = file_new();
        if (!nf) {
            return NC_STATUS_INSUFFICIENT_RESOURCES;
        }
        nf->kind = *rel ? F_DISC : F_VOLUME;
        nf->partition = -1;
        nf->disc = df;
        nf->is_dir = dir;
        *info = 1; /* FILE_OPENED */
    } else if (dev == 0) {
        /* Partition0 is the whole disk, opened raw for its config sectors:
         * they read as zeros (no refurb or cache records) and writes drop. */
        if (*rel) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        nf = file_new();
        if (!nf) {
            return NC_STATUS_INSUFFICIENT_RESOURCES;
        }
        nf->kind = F_VOLUME;
        nf->partition = 0;
        *info = 1;
    } else {
        char host[512];
        int exists = host_path(dev, rel, host, sizeof(host));
        if (exists >= 0) {
            wb_wait_under(host);        /* patch 261: its queued writes first */
        }
        if (exists < 0) {
            if (g_trace_io) {
                nc_log("hakuX-native-io", "open %s: path not found", path);
            }
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        struct stat st;
        bool is_dir = exists && stat(host, &st) == 0 && S_ISDIR(st.st_mode);
        if (!*rel && dev <= DEV_MB0) {
            is_dir = true; /* the partition root (mbcom: and mbrom0/1: are files) */
        }
        if (exists && !is_dir && file_other(NULL, host, true)) {
            nc_log("hakuX-native-io", "open %s: delete pending", path);
            return STATUS_DELETE_PENDING;
        }
        if (exists && disposition == FILE_CREATE) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
        if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)) {
            if (g_trace_io) {
                nc_log("hakuX-native-io", "open %s: not found", path);
            }
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (exists && is_dir && (options & FILE_NON_DIRECTORY_FILE)) {
            return dir_mismatch(path, options, STATUS_FILE_IS_A_DIRECTORY);
        }
        if (exists && !is_dir && (options & FILE_DIRECTORY_FILE)) {
            return dir_mismatch(path, options, STATUS_NOT_A_DIRECTORY);
        }
        int fd = -1;
        char old_path[320] = "";
        uint64_t old_size = 0;
        if (!exists && (options & FILE_DIRECTORY_FILE)) {
            if (mkdir(host, 0777) != 0) {
                return STATUS_ACCESS_DENIED;
            }
            is_dir = true;
            *info = 2; /* FILE_CREATED */
        } else if (!is_dir) {
            int flags = O_RDWR | O_CLOEXEC;
            if (!exists) {
                flags |= O_CREAT;
                *info = 2;
            } else if (disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF ||
                       disposition == FILE_SUPERSEDE) {
                /* Empty now; the old bytes wait in a side file (NcFile.old_path). */
                old_size = (uint64_t)st.st_size;
                if (!set_aside(host, old_path, sizeof(old_path))) {
                    old_path[0] = '\0';
                }
                flags |= O_CREAT | O_TRUNC;
                *info = disposition == FILE_SUPERSEDE ? 0 : 3;
            } else {
                *info = 1;
            }
            fd = open(host, flags, 0666);
            if (fd < 0 && errno == EACCES && exists) {
                /* XOG patch 286d: a file created earlier under a different
                 * credential (before the app ran as the system user) that this
                 * credential may not open (uid 0 does not bypass the check on
                 * this console). Open its permissions and retry once; the one-
                 * time tree chmod at startup does the same for the whole tree. */
                if (chmod(host, 0666) == 0) {
                    fd = open(host, flags, 0666);
                    if (fd >= 0) {
                        nc_log("hakuX-native-io", "open %s: was EACCES, chmod 0666 and reopened", path);
                    }
                }
            }
            if (fd < 0) {
                nc_log("hakuX-native-io", "open %s (%s): %s", path, host, strerror(errno));
                if (old_path[0]) {
                    rename(old_path, host); /* the file as it was */
                }
                return STATUS_ACCESS_DENIED;
            }
            if (ram_enabled() && ram_is_cache(host)) {
                /* back this cache file with RAM; empty it on a create/overwrite */
                const bool fresh = (*info == 2) || (flags & O_TRUNC);
                (void)ram_track(host, fresh);
            }
        } else {
            *info = 1;
        }
        nf = file_new();
        if (!nf) {
            if (fd >= 0) {
                close(fd);
            }
            if (old_path[0]) {
                rename(old_path, host);
            }
            return NC_STATUS_INSUFFICIENT_RESOURCES;
        }
        nf->kind = F_HOST;
        nf->partition = dev;
        nf->fd = fd;
        nf->is_dir = is_dir;
        snprintf(nf->old_path, sizeof(nf->old_path), "%s", old_path);
        nf->old_size = old_size;
        snprintf(nf->host, sizeof(nf->host), "%s", host);
    }
    snprintf(nf->path, sizeof(nf->path), "%s", path);
    nf->async = !(options & FILE_SYNCHRONOUS_IO);
    nf->no_buffering = (options & FILE_NO_INTERMEDIATE_BUFFERING) != 0;
    nf->can_write = (access & ACCESS_WRITES) != 0;
    nc_kernel_trace_trigger(path);
    *handle = file_handle(k, nf);
    nc_log("hakuX-native-io", "open %s -> %s%s", path, nf->is_dir ? "dir " : "",
           *info == 2 ? "(created)" : *info == 3 ? "(overwritten)" : *info == 0 ? "(superseded)" : "");
    return *handle ? NC_STATUS_SUCCESS : NC_STATUS_INSUFFICIENT_RESOURCES;
}

/* NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
 *   FileAttributes, ShareAccess, CreateDisposition, CreateOptions) */
static uint64_t NtCreateFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t h = 0, info = 0;
    uint32_t st = do_open(k, a[1], a[2], a[7], a[8], &h, &info);
    io_status(k, a[3], st, info);
    if (st == NC_STATUS_SUCCESS) {
        nc_vm_wr32(k->vm, a[0], h);
    }
    return st;
}

/* NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions) */
static uint64_t NtOpenFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t h = 0, info = 0;
    uint32_t st = do_open(k, a[1], a[2], FILE_OPEN, a[5], &h, &info);
    io_status(k, a[3], st, info);
    if (st == NC_STATUS_SUCCESS) {
        nc_vm_wr32(k->vm, a[0], h);
    }
    return st;
}

/* --------------------------------------------------------- read, write */

static uint64_t file_size(const NcFile *nf)
{
    if (nf->kind == F_DISC) {
        return nf->disc.size;
    }
    writes_drain();                 /* its size with the writes already queued in */
    if (nf->kind == F_HOST && ram_enabled() && ram_is_cache(nf->host)) {
        const int ri = ram_find(nf->host);
        if (ri >= 0) {
            return g_ram[ri].size;
        }
    }
    struct stat st;
    return nf->fd >= 0 && fstat(nf->fd, &st) == 0 ? (uint64_t)st.st_size : 0;
}

/* NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset) */
/* Raw sectors that must survive a reboot: the disk's config area before the
 * first partition (XAPI's cache drive records, refurb info) and the head of
 * each cache partition (its FATX superblock), kept in <hdd>/raw-partitionN.bin.
 * Past RAW_KEEP bytes they read as zeros and writes drop. */
#define RAW_KEEP 0x80000u

static void raw_io(int part, uint64_t off, uint8_t *buf, uint32_t len, bool write)
{
    const uint32_t n = off < RAW_KEEP ? (uint32_t)(RAW_KEEP - off < len ? RAW_KEEP - off : len) : 0;
    if (!write) {
        memset(buf, 0, len);
    }
    if (!n) {
        return;
    }
    char p[320];
    snprintf(p, sizeof(p), "%s/raw-partition%d.bin", nc_io_part_root(part), part);
    const int fd = open(p, write ? O_RDWR | O_CREAT | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0666);
    if (fd < 0) {
        return;
    }
    if (write ? pwrite(fd, buf, n, (off_t)off) != (ssize_t)n : pread(fd, buf, n, (off_t)off) < 0) {
        nc_log("hakuX-native-io", "raw partition%d %s +%llx: %s", part, write ? "write" : "read",
               (unsigned long long)off, strerror(errno));
    }
    close(fd);
}

/* ExReadWriteRefurbInfo(RefurbInfo, ValueLength, DoWrite): XBOX_REFURB_INFO
 * { Signature, PowerCycleCount, FirstSetTime } in the config area's sector 3;
 * read without its signature there, zeros. */
#define REFURB_SECTOR    3
#define REFURB_SIGNATURE 0x52465242u    /* 'RFRB' */

static uint64_t ExReadWriteRefurbInfo(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (a[1] != 16) {
        return 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
    }
    uint8_t sector[512];
    raw_io(0, REFURB_SECTOR * 512, sector, sizeof(sector), false);
    uint8_t *info = nc_vm_host(k->vm, a[0]);
    if (a[2] & 0xFF) {
        memcpy(sector, info, 16);
        nc_wr32(sector, REFURB_SIGNATURE);
        raw_io(0, REFURB_SECTOR * 512, sector, sizeof(sector), true);
    } else if (nc_rd32(sector) == REFURB_SIGNATURE) {
        memcpy(info, sector, 16);
    } else {
        memset(info, 0, 16);
    }
    return NC_STATUS_SUCCESS;
}

/* IoDismountVolumeByName(DeviceName): the volumes are host folders, with
 * nothing cached to write back; the dashboard dismounts the disc and the
 * drives it formats. */
static uint64_t IoDismountVolumeByName(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return NC_STATUS_SUCCESS;
}

/* The offset an NtReadFile/NtWriteFile works at: ByteOffset, or the file position. */
static uint64_t io_offset(NcKernel *k, const NcFile *nf, uint32_t byte_offset)
{
    if (byte_offset) {
        const uint64_t o = (uint64_t)nc_vm_rd32(k->vm, byte_offset) | ((uint64_t)nc_vm_rd32(k->vm, byte_offset + 4) << 32);
        if (o != 0xFFFFFFFFFFFFFFFEull) { /* FILE_USE_FILE_POINTER_POSITION */
            return o;
        }
    }
    return nf->pos;
}

/* A read or write through nf ended at end: the file's position goes there
 * through a handle opened for synchronous I/O, as the Xbox's file systems
 * keep it (NT's, FO_SYNCHRONOUS_IO only); an overlapped request names its
 * offset and leaves the position to the title. Burnout 2 reads its 4.6 MiB
 * traffic data in 128 KiB overlapped reads, moving the position itself
 * (SetFilePointer) after each: moved here too, each read came from twice
 * its offset, and the data's pointer walk faulted as the game started. One
 * without an offset of its own (the position's) moves it still. */
static void io_moved(NcKernel *k, NcFile *nf, uint32_t byte_offset, uint64_t end)
{
    const bool named = byte_offset && !(nc_vm_rd32(k->vm, byte_offset) == 0xFFFFFFFEu &&
                                        nc_vm_rd32(k->vm, byte_offset + 4) == 0xFFFFFFFFu);
    if (!nf->async || !named) {
        nf->pos = end;
    }
}

static int partition_root(const NcFile *nf);

/* Ends a read or write: its IoStatusBlock, event and APC, and what it returns.
 * An overlapped request that completes reports STATUS_PENDING (NcFile.async);
 * one that fails up front (end of file) returns the error and, as NT does for
 * a failure before pending, leaves the IoStatusBlock, event and APC alone:
 * XAPI set the OVERLAPPED to STATUS_PENDING, which GetOverlappedResult reads
 * as success. Forza's read-ahead past the end of a file relies on that.
 * The DVD drive pends every transfer, an overlapped one with neither event
 * nor APC too (its completion sets the file object's event): Otogi's stream
 * reader (XDK 5455) counts the bytes only after ERROR_IO_PENDING, through
 * GetOverlappedResult, and took a disc read that succeeded at once as 0 bytes
 * read, a dirty disc. pend: a success may pend (not a read through the cache,
 * disc_cached). */
static uint32_t io_finish(NcKernel *k, const NcFile *nf, const uint32_t *a, uint32_t st, uint32_t n, bool pend)
{
    if (nf->async && st >= 0xC0000000u) {
        return st;
    }
    io_status(k, a[4], st, n);
    complete(k, a);
    return pend && nf->async && (a[1] || a[2] || nf->kind == F_DISC) ? STATUS_PENDING : st;
}

/* A disc read of part of a sector goes through the file system's cache, which
 * reads in the caller's thread: it completes in the call, overlapped or not
 * (only whole sectors go to the drive and pend). Wreckless reads Font18.xpr's
 * 12-byte header overlapped, then loads the OVERLAPPED's status into a
 * register once and loops while it is STATUS_PENDING: pended, it hung at
 * boot unless the read happened to land before that load. */
#define DISC_SECTOR 2048u

static bool disc_cached(const NcFile *nf, uint64_t off, uint32_t len)
{
    return nf->kind == F_DISC && ((off | len) & (DISC_SECTOR - 1));
}

/* Console timing: the file system's cache holds the n bytes at off (the
 * pages the file's last drive read brought in). */
static bool fs_cache_holds(const NcFile *nf, uint64_t off, uint32_t n)
{
    const struct FsCache *c = &g_fs_cache[nf - g_files];
    return off >= c->lo && off + n <= c->hi;
}

/* A drive read of the n bytes at off brings their pages into the cache, in
 * place of the file's last ones: the bytes the drive transfers. */
static uint32_t fs_cache_fill(const NcFile *nf, uint64_t off, uint32_t n)
{
    struct FsCache *c = &g_fs_cache[nf - g_files];
    c->lo = off & ~(uint64_t)(FS_PAGE - 1);
    c->hi = (off + n + FS_PAGE - 1) & ~(uint64_t)(FS_PAGE - 1);
    return (uint32_t)(c->hi - c->lo);
}

/* A synchronous read this long goes to the reader thread too, the calling
 * thread waiting for it (XAPI's ReadFile on an ordinary handle): in place,
 * every guest thread, the vblank DPC and the interrupts stopped for the
 * whole read (8-70 ms from the SD card, as the overlapped ones were).
 * Shorter ones cost less than the trip to the reader. */
#define SYNC_LATER_MIN 0x8000u

/* The running thread may wait in its call: an ordinary thread at
 * PASSIVE_LEVEL, not a DPC or an ISR. */
static bool can_block(const NcKernel *k)
{
    static int in_place = -1; /* NC_SYNC_IN_PLACE: synchronous reads in place, as before (A/B) */
    if (in_place < 0) {
        in_place = getenv("NC_SYNC_IN_PLACE") != NULL;
    }
    const NcThread *t = k->current;
    return !in_place && t && t->state == NC_T_RUNNING && t != k->dpc_thread && t != k->isr_thread && !k->in_isr &&
           !k->redirected && *(const uint8_t *)nc_vm_host(k->vm, k->kpcr_va + 0x24) == 0;
}

/* The runs' first n bytes: how many runs hold them, the last cut to fit. */
static unsigned runs_cut(IoRun *run, unsigned nrun, uint32_t n)
{
    unsigned i = 0;
    for (; i < nrun && n; i++) {
        run[i].len = run[i].len < n ? run[i].len : n;
        n -= run[i].len;
    }
    return i;
}

static bool runs_resident(NcKernel *k, const IoRun *run, unsigned nrun)
{
    for (unsigned i = 0; i < nrun; i++) {
        if (!nc_kernel_resident(k, run[i].va, run[i].len, true)) {
            return false;
        }
    }
    return true;
}

/* NtReadFile's work for a buffer of runs (len bytes in all): a[] as
 * NtReadFile takes it, its Buffer and Length aside. */
static uint64_t read_runs(NcKernel *k, const uint32_t *a, IoRun *run, unsigned nrun, uint32_t len)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    writes_drain();                 /* a read sees writes already queued */
    const uint32_t va0 = nrun ? run[0].va : 0;
    const int raw = nf->kind == F_VOLUME && nf->partition == 0 ? 0
                  : nf->kind == F_HOST && nf->is_dir ? partition_root(nf) : -1;
    if (raw == 0 || raw >= 3) {
        /* The raw disk's config sectors, or a cache partition read raw. */
        if (!runs_resident(k, run, nrun)) {
            return 0;
        }
        const uint64_t off = io_offset(k, nf, a[7]);
        uint64_t o = off;
        for (unsigned i = 0; i < nrun; i++) {
            nc_vm_will_write(run[i].va, run[i].len);
            raw_io(raw, o, nc_vm_host(k->vm, run[i].va), run[i].len, false);
            o += run[i].len;
        }
        io_moved(k, nf, a[7], off + len);
        if (g_trace_io) {
            nc_log("hakuX-native-io", "read %s raw +%llx %u", nf->path, (unsigned long long)off, len);
        }
        return io_finish(k, nf, a, NC_STATUS_SUCCESS, len, true);
    }
    if (nf->is_dir || nf->kind == F_VOLUME) {
        io_status(k, a[4], STATUS_INVALID_DEVICE_REQUEST, 0);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (nf->kind == F_HOST) {
        wb_wait_under(nf->host);        /* patch 261: its queued writes first */
    }
    uint64_t off = nf->pos;
    if (a[7]) {
        const uint64_t o = (uint64_t)nc_vm_rd32(k->vm, a[7]) | ((uint64_t)nc_vm_rd32(k->vm, a[7] + 4) << 32);
        if (o != 0xFFFFFFFFFFFFFFFEull) { /* FILE_USE_FILE_POINTER_POSITION */
            off = o;
        }
    }
    const uint64_t size = file_size(nf);
    /* patch 329b (xogfix=drivetime): cached data too takes the drive's time - Breakdown's level file was cached (it
     * had just copied it to Z:), so its overlapped read still completed at once and the race stayed */
    const bool async = nf->async && (drivetime_fix() || !disc_cached(nf, off, len));
    uint32_t st = NC_STATUS_SUCCESS, n = 0;
    if (!len) {
        /* Zero bytes succeed, past the end too, as the file systems check
         * the length first: Halo's map streaming queues reads of what is
         * left, 0 at the end, and waits for each one's completion. */
    } else if (off >= size) {
        st = STATUS_END_OF_FILE;
    } else {
        n = (uint32_t)(size - off < len ? size - off : len);
        nrun = runs_cut(run, nrun, n);
        if (!runs_resident(k, run, nrun)) {
            return 0;
        }
        /* console timing: a synchronous read the cache doesn't hold waits for the drive */
        const bool drive = !async && can_block(k) && (drivetime_fix() || !fs_cache_holds(nf, off, n)) && devtime_now();
        const bool later = async || drive || (n >= SYNC_LATER_MIN && can_block(k));
        const bool ram_rd = nf->kind == F_HOST && ram_enabled() && ram_is_cache(nf->host) && ram_find(nf->host) >= 0;
        const uint32_t dev_us = async ? device_us(nf, n) :
                                drive ? device_us(nf, drivetime_fix() ? n : fs_cache_fill(nf, off, n)) : 0;
        if (g_trace_io && nf->kind == F_HOST) {
            nc_log("hakuX-native-io", "read %s: later %d drive %d ram %d fd %d dev %u us", nf->path, later, drive, ram_rd,
                   nf->fd, dev_us);
        }
        if ((nf->kind == F_DISC || (nf->kind == F_HOST && nf->fd >= 0)) && later && !ram_rd &&
            disc_read_later(k, nf, a, run, nrun, off, n, !async, dev_us, false)) {
            io_moved(k, nf, a[7], off + n);
            if (g_trace_io) {
                nc_log("hakuX-native-io", "read %s +%llx %u into %08x%s %s%s%s", nf->path, (unsigned long long)off,
                       n, va0, nrun > 1 ? " (scattered)" : "", async ? "pending" : "waited for",
                       a[1] ? " event" : "", a[2] ? " apc" : "");
            }
            if (!async) {
                nc_ke_block(k);         /* the read's status replaces this when it completes */
            }
            return STATUS_PENDING;
        }
        bool ok = true;
        uint64_t o = off;
        for (unsigned i = 0; ok && i < nrun; i++) {
            nc_vm_will_write(run[i].va, run[i].len); /* pread into a protected page fails rather than faults */
            /* a soft block's pages aren't next to each other: page by page there */
            for (uint32_t got = 0; ok && got < run[i].len;) {
                const uint32_t va = run[i].va + got, left = run[i].len - got, room = NC_PAGE - (va & (NC_PAGE - 1));
                const uint32_t chunk = soft_range(va, left) && left > room ? room : left;
                uint8_t *h = nc_vm_host(k->vm, va);
                if (nc_vm_phys(k->vm, va) != UINT32_MAX) {
                    nc_io_note_read(nc_vm_phys(k->vm, va), chunk, va, o);
                }
                if (nf->kind == F_DISC) {
                    ok = nc_disc_read_file(k->disc, &nf->disc, o, h, chunk);
                } else {
                    const int ri = ram_enabled() && ram_is_cache(nf->host) ? ram_find(nf->host) : -1;
                    ok = ri >= 0 ? ram_pread(ri, h, o, chunk) == chunk
                                 : nc_pread_sure(nf->fd, h, chunk, (uint64_t)o);
                }
                o += chunk;
                got += chunk;
            }
        }
        if (!ok) {
            nc_log("hakuX-native-io", "read %s +%llx (%u) into %08x failed: %s", nf->path, (unsigned long long)off, n,
                   va0, strerror(errno));
            st = 0xC000009Cu; /* STATUS_DEVICE_DATA_ERROR */
            n = 0;
        }
    }
    io_moved(k, nf, a[7], off + n);
    if (g_trace_io) {
        uint8_t b[8] = {0};
        if (n >= 8 && run[0].len >= 8) {
            memcpy(b, nc_vm_host(k->vm, va0), 8);
        }
        nc_log("hakuX-native-io", "read %s +%llx %u into %08x -> %08x n=%u%s%s async %d/%d block %d fd %d t%u "
               "[%02x%02x%02x%02x %02x%02x%02x%02x]",
               nf->path, (unsigned long long)off, len, va0, st, n, a[1] ? " event" : "", a[2] ? " apc" : "",
               nf->async, async, can_block(k), nf->fd, k->current ? k->current->id : 0,
               b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    return io_finish(k, nf, a, st, n, async);
}

/* ---- Sega Chihiro media board, the command channel mbcom: (XPSemu patch 193). Ported from Cxbx-Reloaded
 * src/devices/chihiro/MediaBoard.cpp (GPL-2.0-or-later; Luke Usher and the Cxbx-Reloaded team): a command
 * written at 0x900200 (seq u16, command u16, arguments) is answered at 0x900000 (seq, command | 0x8000, data)
 * and raises IRQ 10; 0x800000/0x800200 the network channel; 0x5FFCE9 the DMA flag. ---- */
#include "native/core/irq.h"

static uint8_t g_mb_900000[512], g_mb_800000[512];
static void mb_answer(NcKernel *k, const uint8_t *in, uint8_t *out);

static void mb_com_read(uint32_t off, uint8_t *buf, uint32_t len)
{
    memset(buf, 0, len);
    if (off == 0x005FFCE9u) {
        if (len >= 4) {
            nc_wr32(buf, 0x10000);
        }
    } else if (off == 0x800000u || off == 0x800200u) {
        memcpy(buf, g_mb_800000, len < sizeof(g_mb_800000) ? len : sizeof(g_mb_800000));
    } else if (off == 0x900000u || off == 0x900200u) {
        memcpy(buf, g_mb_900000, len < sizeof(g_mb_900000) ? len : sizeof(g_mb_900000));
    } else if (off != 0x00D00000u) {
        nc_log("hakuX-native-mb", "mbcom read +%08x %u: not known", off, len);
    }
}

static void mb_com_write(NcKernel *k, uint32_t off, const uint8_t *buf, uint32_t len)
{
    static unsigned s_logged;
    {
        static unsigned w_logged;
        if (w_logged++ < 200) {
            char hex[3 * 12 + 4] = "";
            int o = 0;
            for (uint32_t i = 0; i < len && i < 12; i++) {
                o += snprintf(hex + o, sizeof(hex) - (size_t)o, " %02x", buf[i]);
            }
            nc_log("hakuX-native-mb", "mbcom write +%08x %u:%s (patch 197 tool)", off, len, hex);
        }
    }
    if (off == 0x005FFCE9u) {
        nc_irq_set(10, true);
        return;
    }
    if (off == 0x800000u) {
        memcpy(g_mb_800000, buf, len < sizeof(g_mb_800000) ? len : sizeof(g_mb_800000));
        nc_irq_set(10, true);
        return;
    }
    if (off == 0x800200u) {
        nc_wr32(g_mb_800000, 0x0A000001u);      /* an IP asked for: 10.0.0.1 */
        nc_irq_set(10, true);
        return;
    }
    if (off == 0x900000u) {
        memcpy(g_mb_900000, buf, len < sizeof(g_mb_900000) ? len : sizeof(g_mb_900000));
        nc_irq_set(10, true);
        return;
    }
    if (off != 0x900200u || len < 4) {
        nc_log("hakuX-native-mb", "mbcom write +%08x %u: not known", off, len);
        return;
    }
    uint8_t in[512];
    memcpy(in, buf, len < sizeof(in) ? len : sizeof(in));
    if (!(in[2] | in[3] << 8)) {
        return;
    }
    mb_answer(k, in, g_mb_900000);
    nc_irq_set(10, true);
}

/* The media board's answer to a command block (seq u16, command u16, arguments) into out (seq, command | 0x8000,
 * data at +4): Type-1 through mbcom: (patch 193), Type-3 through the ASIC window (patch 256). k NULL: no guest
 * memory (0x0301 leaves its "TEST OK" out). */
static void mb_answer(NcKernel *k, const uint8_t *in, uint8_t *out)
{
    static unsigned s_logged;
    const uint16_t seq = (uint16_t)(in[0] | in[1] << 8), cmd = (uint16_t)(in[2] | in[3] << 8);
    switch (cmd) {
    case 0x0001: nc_wr32(out + 4, 1024u << 20); break;            /* DIMM size */
    case 0x0100: nc_wr32(out + 4, 5); nc_wr32(out + 8, 100); break; /* status READY, 100% */
    case 0x0101: nc_wr32(out + 4, 0x00001399u); break;   /* patch 227: 13.99: games read major/minor apart (the fork: Gundam needs minor >= 09 and refuses the dumped 13.05); a later-looking version passes any ">=" check */ //            /* firmware version 13.05 as the fork answers this channel (a 16-bit word, zeros after: chihiro_mbcom_process); 0x0317 and 00011305 (its network board path) gave Maximum Tune Error 14 (patches 223, 225) */
    case 0x0102: nc_wr32(out + 4, 0x8000u | 0x0001u); break;       /* system type: developer | GD-ROM */
    case 0x0103: memcpy(out + 4, "A89E-25A47354512", 16); break;  /* serial */
    case 0x0104: case 0x0204: case 0x0601: case 0x0605: case 0x0606:
        nc_wr32(out + 4, 0);
        break;
    case 0x0301: {                                                /* hardware test: "TEST OK" */
        const uint32_t type = nc_rd32(in + 4), result = nc_rd32(in + 8);
        nc_wr32(out + 4, type);
        if (k && result && nc_vm_phys(k->vm, result) != UINT32_MAX) {
            memcpy(nc_vm_host(k->vm, result), "TEST OK", 8);
        }
        break;
    }
    case 0x0415: case 0x0608: nc_wr32(out + 4, 0x0A000001u); break;  /* 10.0.0.1 */
    case 0x0602: nc_wr32(out + 4, 0xFFFF); break;
    case 0x0607: nc_wr32(out + 4, 0); nc_wr32(out + 8, 0); break;
    default:
        nc_log("hakuX-native-mb", "mbcom: command %04x not known", cmd);
        break;
    }
    out[0] = (uint8_t)seq;
    out[1] = (uint8_t)(seq >> 8);
    out[2] = (uint8_t)cmd;
    out[3] = (uint8_t)((cmd | 0x8000u) >> 8);
    if (s_logged++ < 64) {
        nc_log("hakuX-native-mb", "mbcom: command %04x seq %04x -> %08x %08x", cmd, seq, nc_rd32(out + 4), nc_rd32(out + 8));
    }
}

/* ---- XPSemu patch 256: a Type-3 (ASIC) media board, for the games whose acLib needs one ----
 * Maximum Tune (SBHQ, \A\V307.xbe, acLib 0.71) reads the board type at port 40F0 (0x000b88f0): high byte 0, the
 * Type-1 FPGA, gives device '!' (the mbcom: file, what every other game here uses); 1, the Type-3 ASIC, gives
 * device ')'. Only with ')' does it add the network board, device 0xa9 (0x000b8690), and ask that board's firmware
 * version (command 0101 with 0x80000000, 0x000b5da0) into its info block +0x14/+0x15 (0x000b70a2). The game waits
 * up to 1200 x 16 ms for both bytes (0x0006e17d, 0x0006e1cb), else sets 0x24740e, and its error check (0x000631f0)
 * returns 20 = "Error14: Network firmware version does not fulfill the game spec" (Data/2D_Usa/Menu/error_002.png).
 * So the board is a Type-3 for this game, as on the cabinet and in the fork's cabinet table (tattoohanz's QEMU
 * Chihiro fork, hw/xbox/chihiro/chihiro.c, LGPL-2.1-or-later: register map and the network board's answers
 * adapted from there; the game's side read from V307.xbe).
 * Registers: port 4004 = an address, 4000 = its data (4008 bit 7: the address steps by 4 per access). Through
 * A0000020 (address) / A0000000 (data, steps by 4) / A0000040 (bit 31 burst) the ASIC's window: 80000140 bit 0 its
 * CPU runs (no firmware.asic upload then), 80000160 bit 0 clear = network board present, 84000000 the answer,
 * 84000020 the command, 84000040 bit 0 = run it (answered at once: 40E0 bit 0 + IRQ 10). The network board:
 * 91000000 its answer, 91000200 its command, a read of 90000000 rings it (40E0 bit 2 + IRQ 10). 40E0 written:
 * those bits cleared. NC_CHIHIRO_TYPE3=1 / 0 forces it on / off. */
#include <stdbool.h>

static int g_t3_on = -1;

static void t3_game_id(char id[5])
{
    id[0] = '\0';
    const char *fs = getenv("NC_CHIHIRO_FS");
    if (!fs) {
        return;
    }
    char bp[600];
    snprintf(bp, sizeof(bp), "%s/boot.id", fs);
    const int fd = open(bp, O_RDONLY);
    if (fd < 0) {
        return;
    }
    uint8_t b[0x34];
    if (read(fd, b, sizeof(b)) == (ssize_t)sizeof(b)) {
        memcpy(id, b + 0x30, 4);
        id[4] = '\0';
        for (int i = 0; i < 4; i++) {
            if (id[i] < 0x20 || id[i] > 0x7E) {
                id[0] = '\0';
                break;
            }
        }
    }
    close(fd);
}

static bool nc_mb_type3(void)
{
    if (g_t3_on < 0) {
        g_t3_on = 0;
        if (getenv("NC_CHIHIRO")) {
            char id[5];
            t3_game_id(id);
            const char *env = getenv("NC_CHIHIRO_TYPE3");
            /* Maximum Tune 1 and 2 (patch 258: SBKD \A\V322.xbe, gdx-0015/0015a/0016/0016a, the same gate: 40F0
             * at 0x00120xxx, network device only for ')' at 0x00120xxx, the same 1200 x 16 ms version wait at
             * 0x000541xx); the other games here run on the Type-1 path (OutRun 2, Crazy Taxi, Ollie King, Ghost
             * Squad, Virtua Cop 3, HOTD3) */
            g_t3_on = env ? atoi(env) != 0 : (!strcmp(id, "SBHQ") || !strcmp(id, "SBKD"));
            nc_log("hakuX-native-mb", "media board: %s for game %s%s (patch 256)",
                   g_t3_on ? "Type-3 (ASIC) with its network board" : "Type-1 (FPGA, mbcom:)", id[0] ? id : "?",
                   env ? " (NC_CHIHIRO_TYPE3)" : "");
        }
    }
    return g_t3_on;
}

static struct {
    uint32_t reg;               /* 4004 */
    uint8_t mode;               /* 4008 */
    uint32_t saddr, sctl;       /* A0000020, A0000040 */
    uint32_t asic_ctrl;         /* 80000140 */
    uint32_t dma;               /* A0001E60 */
    uint32_t e0;                /* 40E0: bit 0 the ASIC's answer, bit 2 the network board's */
    uint32_t mresp[8], mcmd[8]; /* 84000000, 84000020 */
    uint32_t nresp[8], ncmd[8]; /* 91000000, 91000200 */
} g_t3 = { .asic_ctrl = 1, .dma = 2 };

static void t3_irq(void)
{
    if (g_t3.e0) {
        nc_irq_set(10, true);
    } else {
        nc_irq_set(10, false);
    }
}

/* 84000040 bit 0: the ASIC runs the command at 84000020 (the same block as Type-1's) */
static void t3_asic_run(void)
{
    uint8_t in[32], out[32];
    memcpy(in, g_t3.mcmd, sizeof(in));
    if (!(in[2] | in[3] << 8)) {
        return;
    }
    memset(out, 0, sizeof(out));
    mb_answer(NULL, in, out);
    memcpy(g_t3.mresp, out, sizeof(out));
    memset(g_t3.mcmd, 0, sizeof(g_t3.mcmd));    /* taken: the game polls for a zero command word */
    g_t3.e0 |= 1;
    t3_irq();
}

/* The network board (the fork's chihiro_netdimm_answer, no cable): rung by a read of 90000000 */
static void t3_net_ring(void)
{
    static unsigned logged;
    const uint32_t c0 = g_t3.ncmd[0], arg = g_t3.ncmd[1];
    const uint16_t op = (uint16_t)(c0 >> 16);
    if (!op) {
        return;
    }
    uint32_t r[8] = { c0 | 0x80000000u };
    switch (op) {
    case 0x0001: r[1] = 1024u << 20; break;                      /* connect: the DIMM size */
    case 0x0100: r[1] = 5; r[2] = 100; break;                   /* READY, 100% */
    case 0x0101: r[1] = 0x00011305u; break;                     /* firmware 13.05: the fork's measured board */
    case 0x0103: memcpy(&r[1], "A89E-25A47354512", 16); break; /* serial: the media board's */
    default:
        if ((op & 0xFF00) == 0x0400) {
            /* sockets with no cable (the fork, measured): socket, setsockopt, bind, listen, gethostbyname
             * answer 0, whatever needs a peer fails */
            r[1] = (op == 0x040B || op == 0x040E || op == 0x0402 || op == 0x0408 || op == 0x0405) ? 0 : 0xFFFFFFFFu;
        }
        break;
    }
    memcpy(g_t3.nresp, r, sizeof(r));
    memset(g_t3.ncmd, 0, sizeof(g_t3.ncmd));
    if (op == 0x0101) {
        nc_log("hakuX-native-mb", "network board: firmware version asked (command 0101 seq %04x) -> 13.05 (%08x): "
               "Maximum Tune's Error 14 check answered (patch 256)", c0 & 0xFFFF, r[1]);
    } else if (logged++ < 64) {
        nc_log("hakuX-native-mb", "network board: command %04x seq %04x arg %08x -> %08x %08x (patch 256)", op,
               c0 & 0xFFFF, arg, r[0], r[1]);
    }
    g_t3.e0 |= 4;
    t3_irq();
}

static void t3_unknown(const char *what, uint32_t addr, uint32_t v)
{
    static unsigned logged;
    if (logged++ < 40) {
        nc_log("hakuX-native-mb", "type-3 window: %s %08x %08x not known (patch 256)", what, addr, v);
    }
}

static uint32_t t3_mem_read(uint32_t a)
{
    if (a >= 0x84000000u && a <= 0x8400001Cu) {
        return g_t3.mresp[(a - 0x84000000u) / 4];
    }
    if (a >= 0x84000020u && a <= 0x8400003Cu) {
        return g_t3.mcmd[(a - 0x84000020u) / 4];
    }
    if (a >= 0x91000000u && a <= 0x9100001Cu) {
        return g_t3.nresp[(a - 0x91000000u) / 4];
    }
    if (a >= 0x91000200u && a <= 0x9100021Cu) {
        return g_t3.ncmd[(a - 0x91000200u) / 4];
    }
    switch (a) {
    case 0x80000140u: return g_t3.asic_ctrl;
    case 0x80000160u: return 0;             /* bit 0 clear: the network board is there */
    case 0x80000164u: return 1;
    case 0xA0001E60u: return g_t3.dma;
    case 0x90000000u:                       /* shared memory ready; the read rings the network board */
        t3_net_ring();
        return 1;
    default:
        t3_unknown("read", a, 0);
        return 0;
    }
}

static void t3_mem_write(uint32_t a, uint32_t v)
{
    if (a >= 0x84000000u && a <= 0x8400001Cu) {
        g_t3.mresp[(a - 0x84000000u) / 4] = v;
    } else if (a >= 0x84000020u && a <= 0x8400003Cu) {
        g_t3.mcmd[(a - 0x84000020u) / 4] = v;
    } else if (a >= 0x91000000u && a <= 0x9100001Cu) {
        g_t3.nresp[(a - 0x91000000u) / 4] = v;
    } else if (a >= 0x91000200u && a <= 0x9100021Cu) {
        g_t3.ncmd[(a - 0x91000200u) / 4] = v;
    } else if (a == 0x84000040u) {
        if (v & 1) {
            t3_asic_run();
        }
    } else if (a == 0x80000140u) {
        g_t3.asic_ctrl = v | 1;             /* its CPU keeps running (no firmware.asic to start) */
    } else if (a == 0xA0001E60u) {
        g_t3.dma = v;
    } else if (a >= 0x84800000u && a < 0x84818000u) {
        /* a firmware.asic upload: not modelled, the CPU already runs */
    } else {
        t3_unknown("write", a, v);
    }
}

/* port 4000: the register at 4004, or through it the ASIC window */
static uint32_t t3_data(bool write, uint32_t v)
{
    uint32_t r = 0;
    switch (g_t3.reg) {
    case 0xA0000020u:
        if (write) {
            g_t3.saddr = v;
        }
        r = g_t3.saddr;
        break;
    case 0xA0000040u:
        if (write) {
            g_t3.sctl = v;
        }
        r = g_t3.sctl;
        break;
    case 0xA0000000u:
        if (write) {
            t3_mem_write(g_t3.saddr, v);
            if (g_t3.sctl & 0x80000000u) {
                g_t3.saddr += 4;            /* a burst steps on */
            }
        } else {
            r = t3_mem_read(g_t3.saddr);
            g_t3.saddr += 4;
        }
        break;
    default:
        if (write) {
            t3_mem_write(g_t3.reg, v);
        } else {
            r = t3_mem_read(g_t3.reg);
        }
        if (g_t3.mode & 0x80) {
            g_t3.reg += 4;                  /* 4008 bit 7: the address steps on */
        }
        break;
    }
    return r;
}

bool nc_mb_t3_port_in(uint16_t port, unsigned bytes, uint32_t *v)
{
    if (!nc_mb_type3()) {
        return false;
    }
    switch (port) {
    case 0x4000: *v = t3_data(false, 0); return true;
    case 0x4004: *v = g_t3.reg; return true;
    case 0x4008: *v = g_t3.mode; return true;
    case 0x40E0: *v = g_t3.e0; return true;
    case 0x40F0: *v = 0x0100; return true;          /* chip ID: the ASIC (Type-3) */
    case 0x40F4: *v = 0x0003 | 0x40; return true;   /* 1 GB, and the Type-3's switch bit (the fork) */
    default: return false;
    }
}

bool nc_mb_t3_port_out(uint16_t port, unsigned bytes, uint32_t v)
{
    if (!nc_mb_type3()) {
        return false;
    }
    switch (port) {
    case 0x4000:
        t3_data(true, bytes == 4 ? v : v & ((1u << (8 * bytes)) - 1));
        return true;
    case 0x4004: g_t3.reg = v; return true;
    case 0x4008: g_t3.mode = (uint8_t)v; return true;
    case 0x40E0:                                    /* acknowledged: those bits cleared */
        g_t3.e0 &= ~v;
        t3_irq();
        return true;
    case 0x40E1: case 0x40E2:                       /* the receiver's arm / IRQ off: down only with nothing left */
        if (!g_t3.e0) {
            nc_irq_set(10, false);
        }
        return true;
    default:
        return false;
    }
}

static uint64_t NtReadFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    {
        NcFile *mb = file_of(k, a[0]);
        if (mb && mb->kind == F_HOST && mb->partition == DEV_MB0 + 1) {     /* mbcom: (patch 193) */
            if (!nc_kernel_resident(k, a[5], a[6], true)) {
                return 0;
            }
            const uint64_t o = io_offset(k, mb, a[7]);
            mb_com_read((uint32_t)o, nc_vm_host(k->vm, a[5]), a[6]);
            io_moved(k, mb, a[7], o + a[6]);
            return io_finish(k, mb, a, NC_STATUS_SUCCESS, a[6], false);
        }
    }
    IoRun run = {a[5], a[6]};
    return read_runs(k, a, &run, 1, a[6]);
}

/* NtReadFileScatter(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
 * SegmentArray, Length, ByteOffset): NtReadFile into pages, SegmentArray
 * holding a page's address (FILE_SEGMENT_ELEMENT, 4 bytes on the Xbox) for
 * each page of Length, the device writing whole pages (the address's offset
 * in its page left out). XAPI's ReadFileScatter, for handles opened without
 * the cache: Double-S.T.E.A.L 2's streamer reads 24 pages at a time through
 * it (its requests: an OVERLAPPED, then 25 elements, the last NULL), and the
 * core stopped there as its first scene began. */
static uint64_t NtReadFileScatter(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t len = a[6], pages = (uint32_t)(((uint64_t)len + NC_PAGE - 1) / NC_PAGE);
    if (!nc_kernel_resident(k, a[5], pages * 4, false)) {
        return 0;
    }
    IoRun *run = malloc(sizeof(*run) * (pages ? pages : 1));
    if (!run) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    unsigned nrun = 0;
    for (uint32_t i = 0; i < pages; i++) {
        const uint32_t va = nc_vm_rd32(k->vm, a[5] + 4 * i) & ~(NC_PAGE - 1);
        const uint32_t n = len - i * NC_PAGE < NC_PAGE ? len - i * NC_PAGE : NC_PAGE;
        if (nrun && run[nrun - 1].va + run[nrun - 1].len == va) {
            run[nrun - 1].len += n;
        } else {
            run[nrun++] = (IoRun){va, n};
        }
    }
    const uint64_t r = read_runs(k, a, run, nrun, len);
    free(run);
    return r;
}

/* The partition number if nf is a hard disk partition's root, else -1. */
static int partition_root(const NcFile *nf)
{
    const char *rel;
    const int dev = device_of(nf->path, &rel);
    return dev >= 0 && !*rel ? dev : -1;
}

/* Deletes everything under path (and path itself if self). */
static void remove_tree(const char *path, bool self)
{
    DIR *d = opendir(path);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        char p[600];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", path, de->d_name);
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(p, true);
        } else {
            if (ram_enabled()) {
                ram_drop(p);
            }
            unlink(p);
        }
    }
    if (d) {
        closedir(d);
    }
    if (self) {
        rmdir(path);
    }
}

/* A host file's data or size changed through nf (what: for the log): true
 * if its handle was opened to write. The Xbox kernel refuses the rest
 * (STATUS_ACCESS_DENIED), and titles rely on it: Splinter Cell reads its
 * profile (Profile.inf, 2040 bytes and a 20-byte signature) through a
 * GENERIC_READ handle and then calls SetEndOfFile at the read's end; that
 * fails on the console, and here cut the signature off, so every profile
 * failed to load ("corrupted") the next time. */
static bool handle_can_write(const NcFile *nf, const char *what)
{
    if (nf->can_write) {
        return true;
    }
    static unsigned logged;
    if (logged++ < 16) {
        nc_log("hakuX-native-io", "%s %s through a handle opened without write access: refused "
               "(first 16 logged)", what, nf->path);
    }
    return false;
}

/* NtWriteFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset) */
static uint64_t NtWriteFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    {
        /* when a game last wrote: leaving waits for it (main_ps5.c) */
        extern volatile uint64_t g_xog_hdd_write_ms;
        extern uint64_t rx_now_ms(void);
        g_xog_hdd_write_ms = rx_now_ms();
    }
    if (!nc_kernel_resident(k, a[5], a[6], false)) {
        return 0;
    }
    if (nf->kind == F_HOST && nf->partition == DEV_MB0 + 1) {      /* mbcom: (patch 193) */
        const uint64_t o = io_offset(k, nf, a[7]);
        mb_com_write(k, (uint32_t)o, nc_vm_host(k->vm, a[5]), a[6]);
        io_moved(k, nf, a[7], o + a[6]);
        return io_finish(k, nf, a, NC_STATUS_SUCCESS, a[6], false);
    }
    if (nf->kind == F_VOLUME) {
        if (nf->partition == 0) {
            const uint64_t off = io_offset(k, nf, a[7]);
            nc_vm_will_read(a[5], a[6]);
            raw_io(0, off, nc_vm_host(k->vm, a[5]), a[6], true);
            io_moved(k, nf, a[7], off + a[6]);
            if (g_trace_io) {
                nc_log("hakuX-native-io", "write %s raw +%llx %u", nf->path, (unsigned long long)off, a[6]);
            }
        }
        return io_finish(k, nf, a, NC_STATUS_SUCCESS, a[6], true);
    }
    if (nf->kind == F_HOST && nf->is_dir && partition_root(nf) >= 3) {
        /* A cache drive formatted raw (XMountUtilityDrive): the superblock
         * write empties the folder; the head of the partition is kept. */
        const uint64_t o = io_offset(k, nf, a[7]);
        io_moved(k, nf, a[7], o + a[6]);
        nc_vm_will_read(a[5], a[6]);
        raw_io(partition_root(nf), o, nc_vm_host(k->vm, a[5]), a[6], true);
        /* The FATX superblock: "FATX", volume id, sectors per cluster. */
        const uint8_t *sb = nc_vm_host(k->vm, a[5]);
        if (o == 0 && a[6] >= 12 && !memcmp(sb, "FATX", 4)) {
            const int part = partition_root(nf);
            g_cluster_sectors[part] = nc_rd32(sb + 8);
            nc_log("hakuX-native-io", "%s formatted (%u-sector clusters): emptied %s", nf->path,
                   g_cluster_sectors[part], nf->host);
            wb_wait_under(nf->host);    /* patch 261: the queued writes there land first */
            remove_tree(nf->host, false);
        }
        return io_finish(k, nf, a, NC_STATUS_SUCCESS, a[6], true);
    }
    if (nf->kind != F_HOST || nf->is_dir || !handle_can_write(nf, "write to")) {
        io_status(k, a[4], STATUS_ACCESS_DENIED, 0);
        return STATUS_ACCESS_DENIED;
    }
    /* patch 261 (xogfix=cachewrite): a cache drive's file written behind;
     * else, or to its end, or an overwritten one's (regrow), queued writes first */
    const bool wb = nf->partition >= 3 && nf->partition <= 5 && !nf->old_path[0] && nf->fd >= 0 && wb_enabled();
    const bool to_end = a[7] && nc_vm_rd32(k->vm, a[7]) == 0xFFFFFFFFu && nc_vm_rd32(k->vm, a[7] + 4) == 0xFFFFFFFFu;
    if (!wb || to_end) {
        wb_wait_under(nf->host);
    }
    uint64_t off = nf->pos;
    if (a[7]) {
        const uint64_t o = (uint64_t)nc_vm_rd32(k->vm, a[7]) | ((uint64_t)nc_vm_rd32(k->vm, a[7] + 4) << 32);
        if (o == 0xFFFFFFFFFFFFFFFFull) { /* FILE_WRITE_TO_END_OF_FILE */
            off = file_size(nf);
        } else if (o != 0xFFFFFFFFFFFFFFFEull) {
            off = o;
        }
    }
    if (nf->old_path[0] && !(ram_enabled() && ram_is_cache(nf->host))) {
        const uint64_t size = file_size(nf);
        if (off > size) {
            regrow(nf, size, off); /* the gap a write past the end leaves */
        }
    }
    if (ram_enabled() && ram_is_cache(nf->host)) {
        int ri = ram_find(nf->host);
        if (ri < 0) {
            ri = ram_track(nf->host, false);
        }
        if (ri >= 0) {
            /* free room by spilling cold files to disk (patch 292: bounded, never OOM) */
            while (!ram_fits(ri, off + a[6]) && ram_evict_one(ri)) {
            }
            if (ram_fits(ri, off + a[6])) {
                nc_vm_will_read(a[5], a[6]);
                const ssize_t rn = ram_pwrite(ri, nc_vm_host(k->vm, a[5]), off, a[6]);
                if (rn >= 0) {
                    nf->wrote = true;
                    io_moved(k, nf, a[7], off + (uint64_t)rn);
                    return io_finish(k, nf, a, NC_STATUS_SUCCESS, (uint32_t)rn, nf->no_buffering);
                }
            }
            /* this file alone is over the cap (or realloc failed): spill it, write it on disk */
            ram_flush(ri, nf->fd);
        }
    }
    nc_vm_will_read(a[5], a[6]);   /* pwrite from a page without access fails rather than faults */
    ssize_t n = 0;
    /* patch 330: an overlapped handle's write goes to the reader thread and completes later (IoStatusBlock, event,
     * APC), as a console hard disk's does and as its reads here already do. Done in the call, its completion ran
     * before the caller had finished setting up what the completion uses. Its bytes go through the write helper
     * (patch 307/308) when that is on, so it isn't throttled. A synchronous handle's write stays as it was: the
     * write-behind queue and the helper already keep those fast. NC_WRITE_IN_PLACE=1: as before. */
    /* patch 332: per game only (xogfix=asyncwrite): on for every game, Breakdown's own save crashed (its save
     * write completed later and the game called through a NULL) */
    static int aw = -1;
    if (a[6] && nf->async && nf->fd >= 0 && xogfix_has("asyncwrite", &aw)) {
        wb_wait_under(nf->host);        /* the path's queued writes land first: this goes behind them */
        if (nf->partition >= 0 && nf->partition <= 5 && wb_enabled() && g_wb_helper == 1 && nf->wb_handle == 0) {
            const int h = wbwr_open(nf->host);
            nf->wb_handle = h > 0 ? h : -1;
        }
        const IoRun run = {a[5], a[6]};
        if (disc_read_later(k, nf, a, &run, 1, off, a[6], false, 0, true)) {
            nf->wrote = true;
            io_moved(k, nf, a[7], off + a[6]);
            if (g_trace_io) {
                nc_log("hakuX-native-io", "write %s +%llx %u from %08x pending", nf->path, (unsigned long long)off,
                       a[6], a[5]);
            }
            return STATUS_PENDING;
        }
    }
    writes_drain();                 /* in place: after the writes already queued */
    /* XOG patch 308: every write patch 307's write-behind does not take goes
     * through the helper in the calling thread, so the Xbox's "write completed"
     * semantics are kept: the save partitions (0-2), a cache file opened with
     * an overwrite disposition (old_path set: patch 261 keeps those in place),
     * and a write to end. These are what kept the app's per-launch budget
     * reachable after the cache drives were routed. */
    if ((!wb || to_end) && nf->partition >= 0 && nf->partition <= 5 && nf->fd >= 0 && wb_enabled() &&
        g_wb_helper == 1) {
        if (nf->wb_handle == 0) {
            const int h = wbwr_open(nf->host);
            nf->wb_handle = h > 0 ? h : -1;
        }
        if (nf->wb_handle > 0 && wbwr_write(nf->wb_handle, off, nc_vm_host(k->vm, a[5]), a[6]) == (long)a[6]) {
            n = (ssize_t)a[6];
        }
    }
    if (!n && wb && wb_write(k, nf, off, a[5], a[6])) {
        n = (ssize_t)a[6];              /* patch 261: the writer thread puts it on the disk */
    } else if (!n && wb) {
        wb_wait_under(nf->host);        /* in place: behind what is queued */
    }
    for (uint32_t put = (uint32_t)n; put < a[6];) {
        const uint32_t va = a[5] + put, left = a[6] - put, room = NC_PAGE - (va & (NC_PAGE - 1));
        const uint32_t chunk = soft_range(va, left) && left > room ? room : left;
        if (!nc_pwrite_sure(nf->fd, nc_vm_host(k->vm, va), chunk, off + put)) {
            break;                      /* patch 356: refused after its retries */
        }
        n += chunk;
        put += chunk;
    }
    uint32_t st = n == (ssize_t)a[6] ? NC_STATUS_SUCCESS : 0xC000007Fu; /* STATUS_DISK_FULL */
    nf->wrote |= n > 0;
    if (g_trace_io) {
        uint8_t b[8] = {0};
        if (n >= 8) {
            memcpy(b, nc_vm_host(k->vm, a[5]), 8);
        }
        nc_log("hakuX-native-io", "write %s +%llx %u from %08x -> %08x [%02x%02x%02x%02x %02x%02x%02x%02x]", nf->path,
               (unsigned long long)off, a[6], a[5], st, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    io_moved(k, nf, a[7], off + (n > 0 ? (uint64_t)n : 0));
    return io_finish(k, nf, a, st, n > 0 ? (uint32_t)n : 0, nf->no_buffering);   /* cached: in the call */
}

/* ---------------------------------------------------------- information */

static uint64_t host_time(time_t t)
{
    return (uint64_t)(t + 11644473600ll) * 10000000ull;
}

/* patch 281 (xogfix=archive, Morrowind's launchers): FILE_ATTRIBUTE_ARCHIVE kept on a hard disk file as its
 * owner-execute bit. Morrowind's default.xbe copies Morrowind.bsa/.esm/.esm.map to Z:, marks each finished copy
 * with SetFileAttributes(ARCHIVE) and at the next start recopies (~480 MB) every file without that mark. */
static bool archive_kept(void)
{
    static int on = -1;
    if (on < 0) {
        /* patch 282 (user): every game, as the Xbox's own disk keeps a file's attributes */
        on = 1;
        nc_log("hakuX-native-io", "archive: a hard disk file's ARCHIVE attribute is kept (patch 282, every game)");
    }
    return on == 1;
}

/* patch 285: a hard disk file's CreationTime is its birth time, as FATX keeps it (set below with
 * FileBasicInformation). st_ctime, used before, is the last change time: it moves with every write and
 * never matched what a game stamped. Riddick (56550026) copies its FileCache to Z:, stamps each copy
 * with the disc file's times, and at every start (1ec2a0 -> 1f23c0) recopies a copy whose size differs
 * or whose CreationTime is over 4 s off the disc file's. Without a birth time: the older of ctime/mtime. */
static time_t host_birth(const struct stat *st)
{
#ifdef st_birthtime
    if (st->st_birthtime > 0) {
        return st->st_birthtime;
    }
#endif
    return st->st_mtime < st->st_ctime ? st->st_mtime : st->st_ctime;
}

/* Times, size and attributes common to the information classes. */
static void file_facts(const NcFile *nf, uint64_t times[4], uint64_t *size, uint32_t *attrs)
{
    if (nf->kind == F_HOST) {
        struct stat st;
        if (stat(nf->host, &st) == 0) {
            times[0] = host_time(host_birth(&st));
            times[1] = host_time(st.st_atime);
            times[2] = times[3] = host_time(st.st_mtime);
            const int rfi = !S_ISDIR(st.st_mode) && ram_enabled() && ram_is_cache(nf->host) ? ram_find(nf->host) : -1;
            *size = S_ISDIR(st.st_mode) ? 0 : rfi >= 0 ? g_ram[rfi].size : (uint64_t)st.st_size;
            *attrs = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (!S_ISDIR(st.st_mode) && (st.st_mode & S_IXUSR) && archive_kept()) {
                *attrs = FILE_ATTRIBUTE_ARCHIVE;
            }
            return;
        }
    }
    times[0] = times[1] = times[2] = times[3] = DISC_TIME;
    *size = nf->is_dir ? 0 : file_size(nf);
    *attrs = (nf->is_dir ? FILE_ATTRIBUTE_DIRECTORY : 0) | (nf->kind == F_HOST ? 0 : FILE_ATTRIBUTE_READONLY);
    if (!*attrs) {
        *attrs = FILE_ATTRIBUTE_NORMAL;
    }
}

static void put64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

/* FILE_NETWORK_OPEN_INFORMATION: four times, AllocationSize, EndOfFile, FileAttributes (0x38). */
static void network_open_info(const NcFile *nf, uint8_t *o)
{
    uint64_t t[4], size;
    uint32_t attrs;
    file_facts(nf, t, &size, &attrs);
    memset(o, 0, 0x38);
    for (int i = 0; i < 4; i++) {
        put64(o + 8 * i, t[i]);
    }
    put64(o + 0x20, nc_align_up((uint32_t)size, 4096));
    put64(o + 0x28, size);
    nc_wr32(o + 0x30, attrs);
}

/* NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
static uint64_t NtQueryInformationFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint8_t o[0x38];                    /* the largest class's; the caller's buffer once it fits */
    uint64_t t[4], size;
    uint32_t attrs, len;
    writes_drain();
    if (nf->kind == F_HOST && a[4] != 14) {
        wb_wait_under(nf->host);        /* patch 261: its queued writes first */
    }
    file_facts(nf, t, &size, &attrs);
    switch (a[4]) {
    case 4: /* FileBasicInformation: times, FileAttributes */
        len = 0x28;
        memset(o, 0, len);
        for (int i = 0; i < 4; i++) {
            put64(o + 8 * i, t[i]);
        }
        nc_wr32(o + 0x20, attrs);
        break;
    case 5: /* FileStandardInformation: AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory */
        len = 0x18;
        memset(o, 0, len);
        put64(o, nc_align_up((uint32_t)size, 4096));
        put64(o + 8, size);
        nc_wr32(o + 0x10, 1);
        o[0x15] = nf->is_dir;
        break;
    case 14: /* FilePositionInformation */
        len = 8;
        put64(o, nf->pos);
        break;
    case 6: { /* FileInternalInformation: IndexNumber, one per file on its volume */
        /* GetFileInformationByHandle asks for it and gives up without it,
         * leaving the rest unset: DOA3 sized its cache copy of loadfile.afs
         * from that garbage and waited for the last chunk forever. */
        uint64_t index = 0;
        struct stat st;
        if (nf->kind == F_DISC) {
            index = nf->disc.sector;
        } else if (nf->kind == F_HOST && stat(nf->host, &st) == 0) {
            index = (uint64_t)st.st_ino;
        }
        len = 8;
        put64(o, index);
        break;
    }
    case 34: /* FileNetworkOpenInformation */
        len = 0x38;
        network_open_info(nf, o);
        break;
    case 17: /* FileAlignmentInformation */
        len = 4;
        nc_wr32(o, 0);
        break;
    default:
        nc_log("hakuX-native-io", "query information class %u on %s not implemented", a[4], nf->path);
        io_status(k, a[1], 0xC0000003u, 0); /* STATUS_INVALID_INFO_CLASS */
        return 0xC0000003u;
    }
    if (a[3] < len) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    memcpy(nc_vm_host(k->vm, a[2]), o, len);
    io_status(k, a[1], NC_STATUS_SUCCESS, len);
    return NC_STATUS_SUCCESS;
}

/* The PS5's libkernel has no futimens/utimensat (ENOSYS): set a file's access
 * and modification times with futimes/utimes, to the microsecond, instead.
 * Without this, FileBasicInformation silently failed and a title that checks
 * a cache copy's time against its source recopied it at every start. */
static bool set_times_us(const NcFile *nf, const struct timespec ts[2])
{
    struct stat st;
    if ((nf->fd >= 0 ? fstat(nf->fd, &st) : stat(nf->host, &st)) != 0) {
        return false;
    }
    struct timeval tv[2];
    for (int i = 0; i < 2; i++) {
        const struct timespec now = i ? st.st_mtim : st.st_atim;
        const struct timespec *t = ts[i].tv_nsec == UTIME_OMIT ? &now : &ts[i];
        tv[i].tv_sec = t->tv_sec;
        tv[i].tv_usec = (suseconds_t)(t->tv_nsec / 1000);
    }
    return (nf->fd >= 0 ? futimes(nf->fd, tv) : utimes(nf->host, tv)) == 0;
}

/* NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
static uint64_t NtSetInformationFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    const uint8_t *in = nc_vm_host(k->vm, a[2]);
    uint64_t v;
    uint32_t st = NC_STATUS_SUCCESS;
    writes_drain();
    if (nf->kind == F_HOST && a[4] != 14) {
        wb_wait_under(nf->host);        /* patch 261: its queued writes first (size, times, delete, rename) */
    }
    switch (a[4]) {
    case 14: /* FilePositionInformation */
        memcpy(&v, in, 8);
        nf->pos = v;
        break;
    case 19: /* FileAllocationInformation (the size is FATX's to keep: nothing to do) */
        if (nf->kind == F_HOST && !nf->is_dir && !handle_can_write(nf, "allocation size of")) {
            st = STATUS_ACCESS_DENIED;
        }
        break;
    case 4: /* FileBasicInformation: Creation, LastAccess, LastWrite, Change times; 0 keeps one */
        if (nf->kind == F_HOST && !nf->is_dir) {
            /* Forza's installer stamps each copy with its source's time and
             * recopies a Z: file whose LastWriteTime is over 4 s off it. */
            struct timespec ts[2];
            for (int i = 0; i < 2; i++) {
                memcpy(&v, in + 8 + 8 * i, 8);
                if (v && v != ~0ull) {
                    const int64_t unix100 = (int64_t)v - 116444736000000000ll;
                    ts[i].tv_sec = (time_t)(unix100 / 10000000);
                    ts[i].tv_nsec = (long)(unix100 % 10000000) * 100;
                    if (ts[i].tv_nsec < 0) {
                        ts[i].tv_sec--;
                        ts[i].tv_nsec += 1000000000;
                    }
                } else {
                    ts[i].tv_sec = 0;
                    ts[i].tv_nsec = UTIME_OMIT;
                }
            }
            /* patch 285: CreationTime too. The host has no call that sets a birth time, but its utimes
             * moves the birth time back to a modification time set before it: set that first, then
             * the times asked for (or the old modification time again). */
            memcpy(&v, in, 8);
            struct stat bst;
            if (v && v != ~0ull && stat(nf->host, &bst) == 0) {
                const int64_t unix100 = (int64_t)v - 116444736000000000ll;
                struct timespec cr[2];
                cr[0].tv_sec = 0;
                cr[0].tv_nsec = UTIME_OMIT;
                cr[1].tv_sec = (time_t)(unix100 / 10000000);
                cr[1].tv_nsec = 0;
                if (unix100 < 0 && unix100 % 10000000) {
                    cr[1].tv_sec--;
                }
                if (cr[1].tv_sec != host_birth(&bst)) {
                    if (ts[1].tv_nsec == UTIME_OMIT) {
                        ts[1] = bst.st_mtim;    /* the modification time stays as it was */
                    }
                    if ((nf->fd >= 0 ? futimens(nf->fd, cr) : utimensat(AT_FDCWD, nf->host, cr, 0)) != 0) {
                        if (errno != ENOSYS || !set_times_us(nf, cr)) {
                            nc_log("hakuX-native-io", "set creation time on %s: %s", nf->path, strerror(errno));
                        }
                    }
                }
            }
            if ((nf->fd >= 0 ? futimens(nf->fd, ts) : utimensat(AT_FDCWD, nf->host, ts, 0)) != 0) {
                if (errno != ENOSYS || !set_times_us(nf, ts)) {
                    nc_log("hakuX-native-io", "set times on %s: %s", nf->path, strerror(errno));
                }
            }
            if (v && v != ~0ull && stat(nf->host, &bst) == 0) {
                const int64_t want = ((int64_t)v - 116444736000000000ll) / 10000000;
                const int64_t got = (int64_t)host_birth(&bst);
                static unsigned told;
                if ((got > want + 2 || got < want - 2) && told++ < 8) {
                    nc_log("hakuX-native-io", "creation time on %s: asked %lld, the host keeps %lld (birth time %s)",
                           nf->path, (long long)want, (long long)got,
#ifdef st_birthtime
                           bst.st_birthtime > 0 ? "kept" : "not kept"
#else
                           "unknown"
#endif
                    );
                }
            }
            uint32_t fa;
            memcpy(&fa, in + 0x20, 4);              /* FileAttributes: 0 leaves them as they are */
            struct stat ast;
            if (fa && archive_kept() && stat(nf->host, &ast) == 0) {
                const mode_t m = (fa & FILE_ATTRIBUTE_ARCHIVE) ? (ast.st_mode | S_IXUSR) : (ast.st_mode & ~(mode_t)S_IXUSR);
                if (m != ast.st_mode &&
                    (nf->fd >= 0 ? fchmod(nf->fd, m & 07777) : chmod(nf->host, m & 07777)) != 0) {
                    nc_log("hakuX-native-io", "archive: set attributes %x on %s: %s", fa, nf->path, strerror(errno));
                } else {
                    static unsigned told;
                    if (told++ < 16) {
                        nc_log("hakuX-native-io", "archive: attributes %x on %s", fa, nf->path);
                    }
                }
            }
        }
        break;
    case 20: { /* FileEndOfFileInformation */
        memcpy(&v, in, 8);
        const uint64_t was = file_size(nf);
        if (nf->kind == F_HOST && ram_enabled() && ram_is_cache(nf->host)) {
            const int ri = ram_find(nf->host);
            if (ri >= 0) {
                if (handle_can_write(nf, "end of file of")) {
                    ram_truncate(ri, v);
                    nf->wrote = true;
                } else {
                    st = STATUS_ACCESS_DENIED;
                }
                break;
            }
        }
        if (nf->kind == F_HOST && nf->fd >= 0 && was == 0 && v > 0 && nf->old_path[0] &&
            handle_can_write(nf, "end of file of") && regrow_by_rename(nf)) {
            if (ftruncate(nf->fd, (off_t)v) != 0) {
                st = STATUS_ACCESS_DENIED;
            }
            nf->wrote = true;
            break;
        }
        if (nf->kind != F_HOST || nf->fd < 0 || !handle_can_write(nf, "end of file of") ||
            ftruncate(nf->fd, (off_t)v) != 0) {
            st = STATUS_ACCESS_DENIED;
        } else {
            nf->wrote = true;
            if (v > was) {
                regrow(nf, was, v);
            }
        }
        break;
    }
    case 13: /* FileDispositionInformation: DeleteFile */
        if (nf->kind == F_HOST && !nf->is_dir) {
            nf->del = in[0] != 0;                   /* the file goes at its last close (NcFile.del) */
        }
        if (in[0]) {
            if (nf->kind != F_HOST) {
                st = STATUS_ACCESS_DENIED;
            } else if (nf->is_dir && rmdir(nf->host) != 0) {
                st = errno == ENOTEMPTY ? STATUS_DIRECTORY_NOT_EMPTY : STATUS_ACCESS_DENIED;
            }
            /* deletes are few, and a save that loses a file is hard to trace back */
            char calls[96];
            nc_kernel_frame_chain(k, f, calls, sizeof(calls));
            nc_log("hakuX-native-io", "delete %s -> %x (from %08x |%s)", nf->path, st,
                   nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)), calls);
        }
        break;
    case 10: { /* FileRenameInformation: ReplaceIfExists, RootDirectory, FileName (ANSI_STRING) */
        /* The new name is relative to RootDirectory as an open's is: XAPI's
         * MoveFile passes "Z:\x" under \?? (Riddick's file cache renames each
         * finished X.tmp to X; read as a bare name every rename failed and
         * the cache never completed). */
        char name[1024], path[512], host[512];
        ansi(k, a[2] + 8, name, sizeof(name));
        const char *rel;
        int dev, exists = -1;
        if (nf->kind != F_HOST || !resolve_name(k, nc_rd32(in + 4), name, sizeof(name)) ||
            !canon(name, path, sizeof(path)) || (dev = device_of(path, &rel)) < 0 ||
            (exists = host_path(dev, rel, host, sizeof(host))) < 0) {
            st = exists < 0 && nf->kind == F_HOST ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_ACCESS_DENIED;
        } else if (exists && !in[0]) {
            st = STATUS_OBJECT_NAME_COLLISION;
        } else if ((wb_wait_under(host), rename(nf->host, host) != 0)) {
            st = STATUS_ACCESS_DENIED;
        } else {
            if (ram_enabled()) {
                ram_rename(nf->host, host);
            }
            snprintf(nf->host, sizeof(nf->host), "%s", host);
            snprintf(nf->path, sizeof(nf->path), "%s", path);
        }
        if (st != NC_STATUS_SUCCESS) {
            nc_log("hakuX-native-io", "rename %s to %s (replace %u) -> %x", nf->path, name, in[0], st);
        }
        break;
    }
    default:
        nc_log("hakuX-native-io", "set information class %u on %s not implemented", a[4], nf->path);
        st = 0xC0000003u;
        break;
    }
    io_status(k, a[1], st, 0);
    return st;
}

/* A partition's cluster size in sectors: from its last format (this boot's,
 * or the superblock kept from an earlier one), else FATX's 16 KiB. */
static uint32_t cluster_sectors(int part)
{
    if (!g_cluster_sectors[part] && part >= 3) {
        uint8_t sb[12];
        raw_io(part, 0, sb, sizeof(sb), false);
        if (!memcmp(sb, "FATX", 4)) {
            g_cluster_sectors[part] = nc_rd32(sb + 8);
        }
    }
    return g_cluster_sectors[part] ? g_cluster_sectors[part] : 32;
}

/* Bytes the files under a partition's directory take, in whole clusters. */
static uint64_t g_usage_bytes, g_usage_cluster;

static int usage_add(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    if (type == FTW_F) {
        g_usage_bytes += ((uint64_t)st->st_size + g_usage_cluster - 1) / g_usage_cluster * g_usage_cluster;
    }
    return 0;
}

static uint64_t partition_used(int part, uint32_t cluster_bytes)
{
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/Partition%d", nc_io_part_root(part), part);
    g_usage_bytes = 0;
    g_usage_cluster = cluster_bytes ? cluster_bytes : 16384;
    nftw(dir, usage_add, 16, FTW_PHYS);
    return g_usage_bytes;
}

/* A partition's use, from a scan of its folder at most every 2 s. */
static uint64_t g_part_used[6], g_part_scanned_ns[6];

static uint32_t cluster_sectors(int part);

static uint64_t part_used(int part)
{
    const uint64_t now = nc_ke_now_ns();
    if (!g_part_scanned_ns[part] || now - g_part_scanned_ns[part] > 2000000000ull) {
        g_part_used[part] = partition_used(part, cluster_sectors(part) * 512u);
        g_part_scanned_ns[part] = now;
    }
    return g_part_used[part];
}

/* Volume information of the volume a file is on; *len receives the bytes written. */
static uint32_t volume_info(NcKernel *k, const NcFile *nf, uint32_t cls, uint32_t buf, uint32_t *len_out)
{
    const bool disc = nf->partition < 0 && nf->kind != F_HOST;
    uint8_t *o = nc_vm_host(k->vm, buf);
    uint32_t len;
    switch (cls) {
    case 1: /* FileFsVolumeInformation: CreationTime, SerialNumber, LabelLength, SupportsObjects, Label */
        len = 0x12;
        memset(o, 0, len);
        put64(o, DISC_TIME);
        nc_wr32(o + 8, disc ? 0x12345678u : 0x5678ABCDu);
        break;
    case 3: /* FileFsSizeInformation: TotalAllocationUnits, AvailableAllocationUnits,
               SectorsPerAllocationUnit, BytesPerSector */
    case 7: { /* FileFsFullSizeInformation: Total, CallerAvailable, ActualAvailable, Sectors, Bytes */
        uint64_t total, avail;
        uint32_t spu, bps;
        if (disc) {
            total = (uint64_t)7 * 1024 * 1024 * 1024 / 2048; /* a full DVD-9 */
            avail = 0;
            spu = 1;
            bps = 2048;
        } else {
            struct statvfs vfs;
            uint64_t freeb = statvfs(g_hdd_root, &vfs) == 0 ? (uint64_t)vfs.f_bavail * vfs.f_frsize : 0;
            const int part = nf->partition >= 1 && nf->partition <= 5 ? nf->partition : 1;
            spu = cluster_sectors(part);
            bps = 512;
            /* The partition's own size, less what its files hold (a title
             * filling a cache drive sees it fill). The folders themselves have
             * no limit: Riddick's file cache copies what it opens and never
             * asks, so it outgrows a 750 MB drive (on hardware it apparently
             * doesn't; how is open). */
            const uint64_t cluster = (uint64_t)spu * 512u, size = k_partitions[part][1];
            const uint64_t used = part_used(part);
            uint64_t free_part = used < size ? size - used : 0;
            if (freeb < free_part) {
                free_part = freeb;
            }
            total = size / cluster;
            avail = free_part / cluster;
        }
        if (cls == 3) {
            len = 0x18;
            put64(o, total);
            put64(o + 8, avail);
            nc_wr32(o + 0x10, spu);
            nc_wr32(o + 0x14, bps);
        } else {
            len = 0x20;
            put64(o, total);
            put64(o + 8, avail);
            put64(o + 0x10, avail);
            nc_wr32(o + 0x18, spu);
            nc_wr32(o + 0x1C, bps);
        }
        break;
    }
    case 4: /* FileFsDeviceInformation: DeviceType, Characteristics */
        len = 8;
        nc_wr32(o, disc ? 0x02u : 0x07u); /* FILE_DEVICE_CD_ROM / FILE_DEVICE_DISK */
        nc_wr32(o + 4, disc ? 0x01u : 0u); /* FILE_REMOVABLE_MEDIA */
        break;
    case 5: { /* FileFsAttributeInformation: Attributes, MaxComponentNameLength, NameLength, Name */
        /* The Xbox's: GDFX the game disc's (XDVDFS), FATX the hard disk's.
         * XGetFilePhysicalSortKey knows the file control block by this name
         * (FILE_OBJECT_SIZE) and fails for another: with CDFS every disc
         * file's key was -1 (invalid parameter), and Fatal Frame II, which
         * sorts its load queue by them to read in disc order, read in
         * whatever order its sort left. */
        const char *fs = disc ? "GDFX" : "FATX";
        len = 12 + 4;
        nc_wr32(o, 0);
        nc_wr32(o + 4, 42);
        nc_wr32(o + 8, 4);
        memcpy(o + 12, fs, 4);
        break;
    }
    default:
        nc_log("hakuX-native-io", "volume information class %u not implemented", cls);
        *len_out = 0;
        return 0xC0000003u;
    }
    *len_out = len;
    return NC_STATUS_SUCCESS;
}

/* NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass) */
static uint64_t NtQueryVolumeInformationFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t len;
    const uint32_t st = volume_info(k, nf, a[4], a[2], &len);
    io_status(k, a[1], st, len);
    return st;
}

/* IoQueryVolumeInformation(FileObject, FsInformationClass, Length, FsInformation, ReturnedLength):
 * the same, on a file object from ObReferenceObjectByHandle. */
static uint64_t IoQueryVolumeInformation(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (!a[0] || nc_rd16(nc_vm_host(k->vm, a[0])) != 5) { /* IO_TYPE_FILE */
        return 0xC0000008u; /* STATUS_INVALID_HANDLE */
    }
    const uint32_t i = nc_vm_rd32(k->vm, a[0] + FO_INDEX);
    if (i >= MAX_FILES || !g_files[i].used) {
        return 0xC0000008u;
    }
    uint32_t len;
    const uint32_t st = volume_info(k, &g_files[i], a[1], a[3], &len);
    if (a[4]) {
        nc_vm_wr32(k->vm, a[4], len);
    }
    return st;
}

/* ------------------------------------------------------------ directories */

typedef struct DirEntry {
    char name[64];
    uint64_t size;
    bool dir;
} DirEntry;

static void disc_dir_walk(const uint8_t *dir, uint32_t size, uint32_t off, DirEntry *out, unsigned *n, unsigned cap,
                          unsigned depth)
{
    if (off + 14 > size || depth > 512 || *n >= cap) {
        return;
    }
    const uint8_t *e = dir + off;
    const uint16_t left = nc_rd16(e), right = nc_rd16(e + 2);
    if (left == 0xFFFF && right == 0xFFFF && e[12] == 0xFF) {
        return;
    }
    if (left && left != 0xFFFF) { /* 0: no subtree */
        disc_dir_walk(dir, size, left * 4u, out, n, cap, depth + 1);
    }
    const uint8_t nlen = e[13];
    if (*n < cap && off + 14 + nlen <= size && nlen < sizeof(out->name)) {
        DirEntry *d = &out[(*n)++];
        memcpy(d->name, e + 14, nlen);
        d->name[nlen] = 0;
        d->size = nc_rd32(e + 8);
        d->dir = e[12] & 0x10;
    }
    if (right && right != 0xFFFF) {
        disc_dir_walk(dir, size, right * 4u, out, n, cap, depth + 1);
    }
}

/* A mod's file or folder in a disc folder's listing (nc_disc_overlay_list):
 * a replaced file with its new size, an added one at the end. */
typedef struct OvList {
    DirEntry *out;
    unsigned *n, cap;
} OvList;

static void ov_entry(void *ctx, const char *name, uint32_t size, bool is_dir)
{
    OvList *l = ctx;
    for (unsigned i = 0; i < *l->n; i++) {
        if (!strcasecmp(l->out[i].name, name)) {
            l->out[i].size = size;
            l->out[i].dir = is_dir;
            return;
        }
    }
    if (*l->n < l->cap && strlen(name) < sizeof(l->out->name)) {
        DirEntry *e = &l->out[(*l->n)++];
        snprintf(e->name, sizeof(e->name), "%s", name);
        e->size = size;
        e->dir = is_dir;
    }
}

static unsigned list_dir(NcKernel *k, const NcFile *nf, DirEntry *out, unsigned cap)
{
    unsigned n = 0;
    if (nf->kind != F_HOST) {
        if (nf->disc.size && nf->disc.sector < NC_DISC_OVERLAY) {
            uint8_t *dir = malloc(nf->disc.size);
            if (dir && nc_disc_read(k->disc, &nf->disc, 0, dir, nf->disc.size)) {
                disc_dir_walk(dir, nf->disc.size, 0, out, &n, cap, 0);
            }
            free(dir);
        }
        const char *rel;
        if (k->disc->ov && device_of(nf->path, &rel) == -1) {
            OvList l = {out, &n, cap};
            nc_disc_overlay_list(k->disc, rel, ov_entry, &l);
        }
        return n;
    }
    wb_wait_under(nf->host);            /* patch 261: sizes with the queued writes in */
    DIR *d = opendir(nf->host);
    struct dirent *de;
    while (d && n < cap && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || strlen(de->d_name) >= sizeof(out->name)) {
            continue;
        }
        char p[600];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", nf->host, de->d_name);
        DirEntry *e = &out[n++];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        const int rdi = !e->dir && ram_enabled() && ram_is_cache(p) ? ram_find(p) : -1;
        e->size = e->dir ? 0 : rdi >= 0 ? g_ram[rdi].size : (uint64_t)st.st_size;
    }
    if (d) {
        closedir(d);
    }
    return n;
}

/* NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation,
 *   Length, FileInformationClass, FileMask, RestartScan): one entry per call.
 * FILE_DIRECTORY_INFORMATION: NextEntryOffset, FileIndex, four times, EndOfFile, AllocationSize,
 * FileAttributes 0x38, FileNameLength 0x3C, FileName (ANSI) 0x40. */
static uint64_t NtQueryDirectoryFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    writes_drain();
    NcFile *nf = file_of(k, a[0]);
    if (!nf || !nf->is_dir) {
        return nf ? STATUS_INVALID_DEVICE_REQUEST : NC_STATUS_INVALID_HANDLE;
    }
    if (a[7] != 1) {
        nc_log("hakuX-native-io", "directory information class %u not implemented", a[7]);
        return 0xC0000003u;
    }
    char mask[128] = "*";
    if (a[8]) {
        ansi(k, a[8], mask, sizeof(mask));
        if (!mask[0]) {
            snprintf(mask, sizeof(mask), "*"); /* an empty mask matches everything */
        }
    } else if (nf->listing) {
        /* FindNextFile passes no name: the enumeration goes on with its first
         * call's mask, as NT keeps it per handle. Read as '*', Riddick's
         * FindFirstFile("_filler???") / FindNextFile loop deleting its save
         * fillers deleted every file of the save, SaveMeta.xbx and its profile
         * with them, and the next checkpoint failed (a null object, then an
         * unhandled CCException and KeBugCheck). */
        snprintf(mask, sizeof(mask), "%s", nf->mask);
    }
    if ((a[9] & 0xff) || strcmp(mask, nf->mask) != 0 || !nf->listing) {
        /* A new enumeration lists the directory once (NcFile.listing). */
        static DirEntry scratch[2048];
        nf->dir_next = 0;
        snprintf(nf->mask, sizeof(nf->mask), "%s", mask);
        const unsigned count = list_dir(k, nf, scratch, 2048);
        free(nf->listing);
        nf->listing = malloc((count ? count : 1) * sizeof(DirEntry));
        nf->nlisting = nf->listing ? count : 0;
        if (nf->listing) {
            memcpy(nf->listing, scratch, count * sizeof(DirEntry));
        }
    }
    const DirEntry *entries = nf->listing;
    const unsigned n = nf->nlisting;
    unsigned i = nf->dir_next;
    while (i < n && !wildcard(nf->mask, entries[i].name)) {
        i++;
    }
    uint32_t st;
    if (g_trace_io) {
        nc_log("hakuX-native-io", "list %s mask '%s' (%u entries): %s", nf->path, nf->mask, n,
               i < n ? entries[i].name : "no more");
    }
    if (i >= n) {
        st = nf->dir_next == 0 ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
        static unsigned none_logged;
        if (st == STATUS_NO_SUCH_FILE && none_logged++ < 50) {
            nc_log("hakuX-native-io", "list %s mask '%s': nothing matches (%u entries)", nf->path, nf->mask, n);
        }
        io_status(k, a[4], st, 0);
        complete(k, a);
        return st;
    }
    nf->dir_next = i + 1;
    const DirEntry *e = &entries[i];
    const uint32_t nlen = (uint32_t)strlen(e->name);
    if (a[6] < 0x40 + nlen) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    uint8_t *o = nc_vm_host(k->vm, a[5]);
    memset(o, 0, 0x40);
    nc_wr32(o + 4, i);
    for (int t = 0; t < 4; t++) {
        put64(o + 8 + 8 * t, DISC_TIME);
    }
    put64(o + 0x28, e->size);
    put64(o + 0x30, nc_align_up((uint32_t)e->size, 4096));
    nc_wr32(o + 0x38, e->dir ? FILE_ATTRIBUTE_DIRECTORY : (nf->kind == F_HOST ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY));
    nc_wr32(o + 0x3C, nlen);
    memcpy(o + 0x40, e->name, nlen);
    io_status(k, a[4], NC_STATUS_SUCCESS, 0x40 + nlen);
    complete(k, a);
    return NC_STATUS_SUCCESS;
}

/* ------------------------------------------------------- path operations */

/* NtQueryFullAttributesFile(ObjectAttributes, FileInformation (FILE_NETWORK_OPEN_INFORMATION)) */
static uint64_t NtQueryFullAttributesFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    writes_drain();
    uint32_t h = 0, info = 0;
    uint32_t st = do_open(k, 0, a[0], FILE_OPEN, 0, &h, &info);
    if (st != NC_STATUS_SUCCESS) {
        return st;
    }
    const NcFile *nf = file_of(k, h);
    network_open_info(nf, nc_vm_host(k->vm, a[1]));
    if (nf && nf->kind == F_DISC && !nf->is_dir && !nf->disc.size) {
        /* a 0-byte disc entry: an empty folder an image tool wrote as a file (what a title asking this wants to hear) */
        nc_vm_wr32(k->vm, a[1] + 0x30, nc_vm_rd32(k->vm, a[1] + 0x30) | 0x10u);
        static unsigned told;
        if (told++ < 8) {
            nc_log("hakuX-native-io", "attributes: a 0-byte file on the disc, answered as an empty folder");
        }
    }
    nc_ob_close(k, h);
    return NC_STATUS_SUCCESS;
}

/* NtDeleteFile(ObjectAttributes) */
static uint64_t NtDeleteFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    writes_drain();
    char name[512], path[512], host[512];
    ansi(k, nc_vm_rd32(k->vm, a[0] + 4), name, sizeof(name));
    if (!resolve_name(k, nc_vm_rd32(k->vm, a[0]), name, sizeof(name))) {
        return NC_STATUS_INVALID_HANDLE;
    }
    const char *rel;
    int dev;
    if (!canon(name, path, sizeof(path)) || (dev = device_of(path, &rel)) < 0) {
        nc_log("hakuX-native-io", "delete %s: no such drive", name);
        return STATUS_ACCESS_DENIED;
    }
    if (host_path(dev, rel, host, sizeof(host)) != 1) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    wb_wait_under(host);                /* patch 261: its queued writes first */
    struct stat st;
    bool dir = stat(host, &st) == 0 && S_ISDIR(st.st_mode);
    NcFile *open_on = dir ? NULL : file_other(NULL, host, false);
    if (open_on) {
        open_on->del = true;                        /* it goes with the last handle on it */
        nc_log("hakuX-native-io", "delete %s: at its last close (from %08x)", path,
               nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)));
        return NC_STATUS_SUCCESS;
    }
    if (ram_enabled() && !dir) {
        ram_drop(host);
    }
    const uint32_t r = (dir ? rmdir(host) : unlink(host)) == 0 ? NC_STATUS_SUCCESS : STATUS_ACCESS_DENIED;
    nc_log("hakuX-native-io", "delete %s: %s (from %08x)", path, r == NC_STATUS_SUCCESS ? "done" : strerror(errno),
           nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)));
    return r;
}

/* NtFlushBuffersFile(FileHandle, IoStatusBlock) */
static uint64_t NtFlushBuffersFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    writes_drain();
    NcFile *nf = file_of(k, a[0]);
    if (nf && nf->fd >= 0) {
        /* patch 261: with writes queued, the sync goes behind them (a duplicate, as flush_later's) */
        CloseJob j = {.fd = __atomic_load_n(&g_wb.npaths, __ATOMIC_ACQUIRE) ? dup(nf->fd) : -1};
        snprintf(j.path, sizeof(j.path), "%s", nf->path);
        if (j.fd < 0 || !wb_close(&j, nf->host)) {
            if (j.fd >= 0) {
                close(j.fd);
            }
            flush_later(nf);
        }
    }
    io_status(k, a[1], NC_STATUS_SUCCESS, 0);
    return NC_STATUS_SUCCESS;
}

#define IOCTL_DISK_GET_DRIVE_GEOMETRY  0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO  0x00074004u

/* Disk ioctls on the hard disk's partitions; returns the output length. */
static uint32_t disk_ioctl(const NcFile *nf, uint32_t code, uint8_t *out, uint32_t out_len, uint32_t *status)
{
    const int part = nf->partition;
    if (part < 0 || part > 5) {
        *status = STATUS_INVALID_DEVICE_REQUEST;
        return 0;
    }
    switch (code) {
    case IOCTL_DISK_GET_DRIVE_GEOMETRY: /* Cylinders, MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector */
        if (out_len < 24) {
            break;
        }
        put64(out, HDD_BYTES / 512);
        nc_wr32(out + 8, 12); /* FixedMedia */
        nc_wr32(out + 12, 1);
        nc_wr32(out + 16, 1);
        nc_wr32(out + 20, 512);
        return 24;
    case IOCTL_DISK_GET_PARTITION_INFO: /* StartingOffset, PartitionLength, HiddenSectors,
                                           PartitionNumber, Type, BootIndicator, Recognized, Rewrite */
        if (out_len < 32) {
            break;
        }
        memset(out, 0, 32);
        put64(out, k_partitions[part][0]);
        put64(out + 8, part ? k_partitions[part][1] : HDD_BYTES);
        nc_wr32(out + 16, (uint32_t)(k_partitions[part][0] / 512));
        nc_wr32(out + 20, (uint32_t)part);
        out[26] = 1;
        return 32;
    default:
        *status = STATUS_INVALID_DEVICE_REQUEST;
        return 0;
    }
    *status = STATUS_BUFFER_TOO_SMALL;
    return 0;
}

/* IOCTL_SCSI_PASS_THROUGH_DIRECT to the DVD drive: a SCSI_PASS_THROUGH_DIRECT
 * (0x2C bytes: ScsiStatus +2, DataTransferLength +0xC, DataBuffer +0x14,
 * Cdb +0x1C). XAPI checks the disc with MODE SENSE(10) of the Xbox security
 * page (0x3E, the DVDX2 authentication page) and reboots to the dashboard
 * unless the drive reports the Xbox partition authenticated (Panzer Dragoon
 * Orta, XDK 4928): CDFValid, PartitionArea 1, Authentication. Other commands
 * read zeros. */
#define IOCTL_SCSI_PASS_THROUGH_DIRECT 0x4D014u

/* READ DVD STRUCTURE, format 0, address FF02FDFF, layer FE: the security sector, after a 4-byte
 * header. A disc image doesn't have it, so the drive makes one. XAPI 5233 reads it when the
 * certificate's flags at +0x1D8 ask, and looks at: +0x4AB the certificate's time stamp, +0x4CF the
 * SHA-1 of the length 0x4CB (4 bytes) and the 0x4CB bytes from +4, +0x4E3 a signature of that. */
#define SS_CERT_TIME 0x4ABu
#define SS_SIGNED    0x4CBu
#define SS_DIGEST    0x4CFu
#define SS_SIG       0x4E3u

static bool security_sector_cdb(const uint8_t *cdb)
{
    return cdb[0] == 0xAD && cdb[2] == 0xFF && cdb[3] == 0x02 && cdb[4] == 0xFD && cdb[5] == 0xFF &&
           cdb[6] == 0xFE && cdb[7] == 0x00;
}

static void security_sector(NcKernel *k, uint8_t *d, uint32_t n)
{
    d[0] = (uint8_t)((n - 2) >> 8);
    d[1] = (uint8_t)(n - 2);
    const uint32_t cert = nc_vm_rd32(k->vm, k->xbe_base + 0x118);
    nc_wr32(d + SS_CERT_TIME, nc_vm_rd32(k->vm, cert + 4));
    uint8_t length[4];
    nc_wr32(length, SS_SIGNED);
    NcSha1 s;
    nc_sha1_init(&s);
    nc_sha1_update(&s, length, 4);
    nc_sha1_update(&s, d + 4, SS_SIGNED);
    nc_sha1_final(&s, d + SS_DIGEST);
    nc_xc_sign(d + SS_DIGEST, d + SS_SIG);
}

static void scsi_pass_through(NcKernel *k, uint32_t sptd, uint32_t len)
{
    if (!sptd || len < 0x2C) {
        return;
    }
    const uint8_t *cdb = nc_vm_host(k->vm, sptd + 0x1C);
    const uint32_t n = nc_vm_rd32(k->vm, sptd + 0x0C), buf = nc_vm_rd32(k->vm, sptd + 0x14);
    *(uint8_t *)nc_vm_host(k->vm, sptd + 2) = 0; /* SCSI status: good */
    if (!buf || !n) {
        return;
    }
    uint8_t *d = nc_vm_host(k->vm, buf);
    memset(d, 0, n);
    if (cdb[0] == 0x5A && (cdb[2] & 0x3F) == 0x3E && n >= 13) {
        d[1] = (uint8_t)(n < 28 ? n - 2 : 26); /* mode data length: past itself */
        d[8] = 0x3E;                           /* the page, 0x12 bytes past its header */
        d[9] = 0x12;
        d[10] = 1;                             /* CDFValid */
        d[11] = 1;                             /* PartitionArea: the Xbox partition */
        d[12] = 1;                             /* Authentication */
    } else if (security_sector_cdb(cdb) && n >= SS_SIG + NC_XC_SIG_SIZE) {
        security_sector(k, d, n);
        nc_log("hakuX-native-io", "scsi ad: the security sector made (%u bytes)", n);
    } else {
        nc_log("hakuX-native-io", "scsi %02x (%u bytes): zeros", cdb[0], n);
    }
}

/* A cache drive's format (XMountUtilityDrive) locks and dismounts its
 * partition: there's no volume state here to hold or drop. */
#define FSCTL_LOCK_VOLUME     0x00090018u
#define FSCTL_UNLOCK_VOLUME   0x0009001Cu
#define FSCTL_DISMOUNT_VOLUME 0x00090020u

/* NtFsControlFile / NtDeviceIoControlFile(FileHandle, Event, ApcRoutine, ApcContext,
 *   IoStatusBlock, ControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength).
 * A code not known here fails as the kernel's own does (zeros and success let a
 * title carry on with them); each told once. 38 titles' first 20 s sent only
 * the ones handled. */
static uint64_t NtControlFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    nc_log("hakuX-native-io", "control %08x on %s (in %u, out %u)", a[5], nf ? nf->path : "?", a[7], a[9]);
    uint32_t status = NC_STATUS_SUCCESS, info = 0;
    if (nf && nf->partition >= 0 && (a[5] >> 16) == 7) { /* FILE_DEVICE_DISK */
        info = disk_ioctl(nf, a[5], a[8] ? nc_vm_host(k->vm, a[8]) : NULL, a[8] ? a[9] : 0, &status);
    } else if (a[5] == IOCTL_SCSI_PASS_THROUGH_DIRECT) {
        scsi_pass_through(k, a[6], a[7]);
    } else if (a[5] != FSCTL_LOCK_VOLUME && a[5] != FSCTL_UNLOCK_VOLUME && a[5] != FSCTL_DISMOUNT_VOLUME) {
        static uint32_t told[16];
        unsigned i = 0;
        while (i < 16 && told[i] && told[i] != a[5]) {
            i++;
        }
        if (i < 16 && !told[i]) {
            told[i] = a[5];
            nc_log("hakuX-native-io", "control %08x on %s: not known here (invalid device request)", a[5],
                   nf ? nf->path : "?");
        }
        status = STATUS_INVALID_DEVICE_REQUEST;
    }
    io_status(k, a[4], status, info);
    complete(k, a);
    return status;
}

/* IoSynchronousDeviceIoControlRequest(IoControlCode, DeviceObject, InputBuffer, InputBufferLength,
 *   OutputBuffer, OutputBufferLength, ReturnedOutputBufferLength, InternalDeviceIoControl) */
static uint64_t IoSynchronousDeviceIoControlRequest(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native-io", "device control %08x on device %08x (out %u)", a[0], a[1], a[5]);
    if (a[4] && a[5]) {
        memset(nc_vm_host(k->vm, a[4]), 0, a[5]);
    }
    if (a[6]) {
        nc_vm_wr32(k->vm, a[6], 0);
    }
    return NC_STATUS_SUCCESS;
}

/* -------------------------------------------------------- symbolic links */

/* IoCreateSymbolicLink(SymbolicLinkName, DeviceName) */
static uint64_t IoCreateSymbolicLink(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[64], target[256], canon_target[256];
    ansi(k, a[0], name, sizeof(name));
    ansi(k, a[1], target, sizeof(target));
    if (!canon(target, canon_target, sizeof(canon_target))) {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    return link_set(name, canon_target) ? NC_STATUS_SUCCESS : NC_STATUS_INSUFFICIENT_RESOURCES;
}

/* IoDeleteSymbolicLink(SymbolicLinkName) */
static uint64_t IoDeleteSymbolicLink(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[64];
    ansi(k, a[0], name, sizeof(name));
    int i = link_find(name);
    if (i < 0) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    g_links[i].name[0] = 0;
    return NC_STATUS_SUCCESS;
}

/* NtOpenSymbolicLinkObject(LinkHandle, ObjectAttributes) */
static uint64_t NtOpenSymbolicLinkObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[64];
    ansi(k, nc_vm_rd32(k->vm, a[1] + 4), name, sizeof(name));
    int i = link_find(name);
    if (i < 0) {
        nc_log("hakuX-native-io", "symbolic link %s: none", name);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    NcFile *nf = file_new();
    if (!nf) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    nf->kind = F_LINK;
    snprintf(nf->path, sizeof(nf->path), "%s", g_links[i].target);
    nc_vm_wr32(k->vm, a[0], file_handle(k, nf));
    return NC_STATUS_SUCCESS;
}

/* NtQuerySymbolicLinkObject(LinkHandle, LinkTarget (ANSI_STRING), ReturnedLength) */
static uint64_t NtQuerySymbolicLinkObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf || nf->kind != F_LINK) {
        return NC_STATUS_INVALID_HANDLE;
    }
    const uint16_t len = (uint16_t)strlen(nf->path);
    const uint16_t max = *(uint16_t *)nc_vm_host(k->vm, a[1] + 2);
    if (a[2]) {
        nc_vm_wr32(k->vm, a[2], len);
    }
    if (max < len) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    memcpy(nc_vm_host(k->vm, nc_vm_rd32(k->vm, a[1] + 4)), nf->path, len);
    *(uint16_t *)nc_vm_host(k->vm, a[1]) = len;
    return NC_STATUS_SUCCESS;
}

/* ------------------------------------------------------- device objects */

/* DEVICE_OBJECT (nxdk xboxkrnl.h, CC0): Type 0x00, Size 0x02, ReferenceCount
 * 0x04, DriverObject 0x08, MountedOrSelfDevice 0x0C, CurrentIrp 0x10, Flags
 * 0x14, DeviceExtension 0x18, DeviceType 0x1C, StartIoFlags 0x1D, StackSize
 * 0x1E, DeletePending 0x1F, SectorSize 0x20, AlignmentRequirement 0x24,
 * DeviceQueue 0x28 (KDEVICE_QUEUE: Type, Size, Busy, DeviceListHead),
 * DeviceLock 0x34 (KEVENT), StartIoKey 0x44; 0x48 bytes, the extension after it. */
#define DEVOBJ_SIZE     0x48u
#define DO_DEVICE_INITIALIZING 0x80u

static struct {
    char name[64];
    uint32_t object;
} g_devices[32];

static uint32_t device_named(const char *path)
{
    for (int i = 0; i < 32; i++) {
        if (g_devices[i].object && !strcasecmp(g_devices[i].name, path)) {
            return g_devices[i].object;
        }
    }
    return 0;
}

static void device_add(const char *path, uint32_t object)
{
    for (int i = 0; i < 32; i++) {
        if (!g_devices[i].object) {
            snprintf(g_devices[i].name, sizeof(g_devices[i].name), "%s", path);
            g_devices[i].object = object;
            return;
        }
    }
    nc_log("hakuX-native-io", "device table full: %s not named", path);
}

static uint32_t device_new(NcKernel *k, uint32_t driver, uint32_t ext_size, uint32_t type)
{
    const uint32_t ext = nc_align_up(ext_size, 16);
    const uint32_t d = nc_ob_create(k, NC_OB_DEVICE, DEVOBJ_SIZE + ext);
    if (!d) {
        return 0;
    }
    uint8_t *p = nc_vm_host(k->vm, d);
    *(uint16_t *)p = 3; /* IO_TYPE_DEVICE */
    *(uint16_t *)(p + 2) = (uint16_t)(DEVOBJ_SIZE + ext);
    nc_vm_wr32(k->vm, d + 0x08, driver);
    nc_vm_wr32(k->vm, d + 0x14, DO_DEVICE_INITIALIZING);
    nc_vm_wr32(k->vm, d + 0x18, ext_size ? d + DEVOBJ_SIZE : 0);
    p[0x1C] = (uint8_t)type;
    p[0x1E] = 1; /* StackSize */
    nc_vm_wr32(k->vm, d + 0x24, 0);
    *(int16_t *)(p + 0x28) = 0x14;      /* DeviceQueueObject */
    p[0x2A] = 0x0C / 4;
    nc_vm_wr32(k->vm, d + 0x2C, d + 0x2C);
    nc_vm_wr32(k->vm, d + 0x30, d + 0x2C);
    p[0x34] = 1;                        /* DeviceLock: synchronization event, signaled */
    p[0x36] = 4;
    nc_vm_wr32(k->vm, d + 0x38, 1);
    nc_vm_wr32(k->vm, d + 0x3C, d + 0x3C);
    nc_vm_wr32(k->vm, d + 0x40, d + 0x3C);
    return d;
}

/* IoCreateDevice(DriverObject, DeviceExtensionSize, DeviceName, DeviceType, Exclusive, DeviceObject) */
static uint64_t IoCreateDevice(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[128] = "", path[256] = "";
    if (a[2]) {
        ansi(k, a[2], name, sizeof(name));
        if (!canon(name, path, sizeof(path))) {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        if (device_named(path)) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }
    const uint32_t d = device_new(k, a[0], a[1], a[3]);
    if (!d) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (path[0]) {
        device_add(path, d);
    }
    nc_vm_wr32(k->vm, a[5], d);
    nc_log("hakuX-native-io", "device %s created: type %02x, driver %08x, extension %u", path[0] ? path : "(unnamed)",
           a[3], a[0], a[1]);
    return NC_STATUS_SUCCESS;
}

/* IoDeleteDevice(DeviceObject) */
static uint64_t IoDeleteDevice(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    for (int i = 0; i < 32; i++) {
        if (g_devices[i].object == a[0]) {
            g_devices[i].object = 0;
        }
    }
    *((uint8_t *)nc_vm_host(k->vm, a[0]) + 0x1F) = 1; /* DeletePending */
    return 0;
}

/* ObReferenceObjectByName(ObjectName, Attributes, ObjectType, ParseContext, Object): devices only.
 * Created devices by name; the built-in disc and partitions on first reference. */
static uint64_t ObReferenceObjectByName(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[512], path[512];
    ansi(k, a[0], name, sizeof(name));
    const char *rel;
    if (!canon(name, path, sizeof(path))) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    uint32_t d = device_named(path);
    if (!d) {
        if (device_of(path, &rel) == -2 || *rel) {
            nc_log("hakuX-native-io", "reference by name %s: not found", name);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        d = device_new(k, 0, 0, device_of(path, &rel) == -1 ? 0x02 : 0x07);
        if (!d) {
            return NC_STATUS_INSUFFICIENT_RESOURCES;
        }
        device_add(path, d);
    }
    nc_vm_wr32(k->vm, a[4], d);
    return NC_STATUS_SUCCESS;
}

void nc_io_register(NcKernel *k)
{
    g_trace_io = getenv("NC_TRACE_IO") != NULL || access("/data/xpsemu/traceio", F_OK) == 0;
    link_set("\\??\\D:", "\\Device\\CdRom0");
    link_set("\\??\\CdRom0:", "\\Device\\CdRom0");     /* the kernel's own, as on the Xbox (the dashboard's) */
    if (getenv("NC_CHIHIRO")) {
        /* Sega Chihiro: the media board's drives (XPSemu patch 187) */
        link_set("\\??\\mbfs:", "\\Device\\MediaBoard\\Partition0");
        link_set("\\??\\mbcom:", "\\Device\\MediaBoard\\Partition1");
        link_set("\\??\\mbrom0:", "\\Device\\MediaBoard\\Partition2");
        link_set("\\??\\mbrom1:", "\\Device\\MediaBoard\\Partition3");
        mkdirs("/data/xpsemu/chihiro/run");
        const int fd = open("/data/xpsemu/chihiro/run/mbcom.bin", O_RDWR | O_CREAT, 0666);
        if (fd >= 0) {
            close(fd);
        }
    }
    nc_kernel_impl(k, "ExReadWriteRefurbInfo", ExReadWriteRefurbInfo);
    nc_kernel_impl(k, "IoDismountVolumeByName", IoDismountVolumeByName);

    nc_kernel_impl(k, "NtCreateFile", NtCreateFile);
    nc_kernel_impl(k, "NtOpenFile", NtOpenFile);
    nc_kernel_impl(k, "NtReadFile", NtReadFile);
    nc_kernel_impl(k, "NtReadFileScatter", NtReadFileScatter);
    nc_kernel_impl(k, "NtWriteFile", NtWriteFile);
    nc_kernel_impl(k, "NtQueryInformationFile", NtQueryInformationFile);
    nc_kernel_impl(k, "NtSetInformationFile", NtSetInformationFile);
    nc_kernel_impl(k, "NtQueryVolumeInformationFile", NtQueryVolumeInformationFile);
    nc_kernel_impl(k, "NtQueryDirectoryFile", NtQueryDirectoryFile);
    nc_kernel_impl(k, "IoQueryVolumeInformation", IoQueryVolumeInformation);
    nc_kernel_impl(k, "NtQueryFullAttributesFile", NtQueryFullAttributesFile);
    nc_kernel_impl(k, "NtDeleteFile", NtDeleteFile);
    nc_kernel_impl(k, "NtFlushBuffersFile", NtFlushBuffersFile);
    nc_kernel_impl(k, "NtFsControlFile", NtControlFile);
    nc_kernel_impl(k, "NtDeviceIoControlFile", NtControlFile);
    nc_kernel_impl(k, "IoSynchronousDeviceIoControlRequest", IoSynchronousDeviceIoControlRequest);
    nc_kernel_impl(k, "IoCreateSymbolicLink", IoCreateSymbolicLink);
    nc_kernel_impl(k, "IoDeleteSymbolicLink", IoDeleteSymbolicLink);
    nc_kernel_impl(k, "NtOpenSymbolicLinkObject", NtOpenSymbolicLinkObject);
    nc_kernel_impl(k, "NtQuerySymbolicLinkObject", NtQuerySymbolicLinkObject);
    nc_kernel_impl(k, "ObReferenceObjectByName", ObReferenceObjectByName);
    nc_kernel_impl(k, "IoCreateDevice", IoCreateDevice);
    nc_kernel_impl(k, "IoDeleteDevice", IoDeleteDevice);
}

/* ---------------------------------------------------------- savestates */

unsigned nc_io_cache_partitions(void)
{
    /* the utility drive (XMountUtilityDrive's link) and any file open there */
    static const char hd[] = "\\Device\\Harddisk0\\Partition";
    unsigned mask = 0;
    for (unsigned i = 0; i < MAX_LINKS; i++) {
        const char *t = g_links[i].target;
        if (g_links[i].name[0] && prefix_ci(t, hd) && t[sizeof(hd) - 1] >= '3' && t[sizeof(hd) - 1] <= '5') {
            mask |= 1u << (t[sizeof(hd) - 1] - '0');
        }
    }
    for (unsigned i = 0; i < MAX_FILES; i++) {
        const NcFile *nf = &g_files[i];
        if (nf->used && nf->kind == F_HOST && nf->partition >= 3 && nf->partition <= 5) {
            mask |= 1u << nf->partition;
        }
    }
    return mask;
}

typedef struct NcFileState {
    uint32_t nfiles;
} NcFileState;

#define NC_DEF_SECTION FILE
#define NC_DEF_FILE "native/state/file.def"
#include "native/state/def.h"
#include "native/state/sections.h"

const char *nc_io_state_drain(NcKernel *k, uint64_t timeout_ns)
{
    /* patch 261: the cache drives' queued writes on the disk before the state copies them */
    if (!nc_io_writes_drain(timeout_ns / 1000000u * 8u, "a save state")) {
        return "the game is writing its cache drive; try again in a moment";
    }
    /* the game's own save on E: half written: not a state to come back to */
    for (unsigned i = 0; i < MAX_FILES; i++) {
        const NcFile *nf = &g_files[i];
        if (nf->used && nf->kind == F_HOST && nf->partition == 1 && nf->wrote) {
            nc_log("hakuX-native-state", "%s is being written", nf->path);
            return "the game is writing its save; try again in a moment";
        }
    }
    /* the reads in flight land, and the kernel completes them (the host's
     * clock: the guest's stands still), the drive's time not waited out */
    __atomic_store_n(&g_reader.hurry, true, __ATOMIC_RELEASE);
    for (uint64_t waited = 0;; waited += 1000000) {
        pthread_mutex_lock(&g_reader.mu);
        const bool idle = g_reader.busy == g_reader.ndone;
        pthread_mutex_unlock(&g_reader.mu);
        if (idle) {
            break;
        }
        if (waited > timeout_ns) {
            __atomic_store_n(&g_reader.hurry, false, __ATOMIC_RELEASE);
            return "a disc read did not finish";
        }
        usleep(1000);
    }
    __atomic_store_n(&g_reader.hurry, false, __ATOMIC_RELEASE);
    nc_irq_post_work();
    nc_io_poll(k);
    return NULL;
}

bool nc_file_state_save(NcBuf *b)
{
    NcFileState st = {0};
    for (unsigned i = 0; i < MAX_FILES; i++) {
        st.nfiles += g_files[i].used;
    }
    nc_rec_put(b, &nc_rec_NcFileState, &st);
    nc_recs_put(b, &nc_rec_NcLink, g_links, sizeof(g_links[0]), MAX_LINKS);
    for (unsigned i = 0; i < MAX_FILES; i++) {
        const NcFile *nf = &g_files[i];
        if (!nf->used) {
            continue;
        }
        nc_buf_u32(b, i);
        nc_rec_put(b, &nc_rec_NcFile, nf);
        nc_recs_put(b, &nc_rec_DirEntry, nf->listing, sizeof(DirEntry), nf->listing ? nf->nlisting : 0);
    }
    return true;
}

static void file_drop(NcFile *nf)
{
    if (nf->used) {
        if (nf->fd >= 0) {
            close(nf->fd);
        }
        if (nf->wb_handle > 0) {
            wbwr_close(nf->wb_handle);   /* patch 307: release the helper's descriptor (a state load drops files) */
        }
        free(nf->listing);
        memset(nf, 0, sizeof(*nf));     /* only those used: the rest stay untouched */
    }
}

bool nc_file_state_load(NcRd *r, unsigned v)
{
    NcFileState st = {0};
    nc_rec_get(r, &nc_rec_NcFileState, &st, v);
    nc_io_writes_drain(60000, "a state load");     /* patch 261: nothing queued on the descriptors dropped here */
    for (unsigned i = 0; i < MAX_FILES; i++) {
        file_drop(&g_files[i]);
    }
    memset(g_fs_cache, 0, sizeof(g_fs_cache));
    memset(g_links, 0, sizeof(g_links));
    nc_recs_get(r, &nc_rec_NcLink, g_links, sizeof(g_links[0]), MAX_LINKS, v);
    bool ok = !r->fail && st.nfiles <= MAX_FILES;
    for (unsigned n = 0; ok && n < st.nfiles; n++) {
        const uint32_t i = nc_rd_u32(r);
        if (i >= MAX_FILES || g_files[i].used) {
            ok = false;
            break;
        }
        NcFile *nf = &g_files[i];
        nc_rec_get(r, &nc_rec_NcFile, nf, v);
        nf->path[sizeof(nf->path) - 1] = nf->host[sizeof(nf->host) - 1] = 0;
        nf->mask[sizeof(nf->mask) - 1] = nf->old_path[sizeof(nf->old_path) - 1] = 0;
        nf->listing = NULL;
        if (nf->nlisting) {
            nf->listing = calloc(nf->nlisting, sizeof(DirEntry));
            if (!nf->listing) {
                ok = false;
                break;
            }
            nc_recs_get(r, &nc_rec_DirEntry, nf->listing, sizeof(DirEntry), nf->nlisting, v);
        }
        nf->fd = -1;
        if (nf->kind == F_HOST && !nf->is_dir) {
            nf->fd = open(nf->host, O_RDWR | O_CLOEXEC);
            if (nf->fd < 0 && errno == ENOENT && nf->partition >= 3 && v < 2) {
                /* not on the drive as saved (CACHE, restored first): deleted
                 * with this handle open, in a state from before deletes waited
                 * for the last close (NcFile.del). Its bytes weren't kept:
                 * empty, deleted at its last close as it would be now, so the
                 * next state keeps it (Sudeki's boot.baf). */
                nf->fd = open(nf->host, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
                if (nf->fd >= 0) {
                    nf->del = true;
                    nc_log("hakuX-native-state", "%s: deleted while open when the state was saved; empty",
                           nf->path);
                }
            }
            if (nf->fd < 0) {
                nc_log("hakuX-native-state", "%s (%s) can't be opened again: %s", nf->path, nf->host,
                       strerror(errno));
                if (nf->partition >= 3) {
                    nc_state_fail("the game's cache file %s is gone: another game has used its cache drive "
                                  "since this state was saved", nf->path);
                } else {
                    nc_state_fail("a file the game had open, %s, can't be opened (%s)", nf->path, strerror(errno));
                }
                ok = false;
                break;
            }
        }
        if (nf->old_path[0]) {
            struct stat sb;
            if (stat(nf->old_path, &sb) != 0) {
                nc_log("hakuX-native-state", "%s: its overwritten bytes (%s) are gone", nf->path, nf->old_path);
                nf->old_path[0] = 0;
                nf->old_size = 0;
            } else {
                g_swept = true;                 /* not this run: the file still needs it */
            }
        }
        if (nc_state_trace()) {
            nc_log("hakuX-native-state", "file %u: %s at %llu", i, nf->path, (unsigned long long)nf->pos);
        }
    }
    return ok && !r->fail;
}
