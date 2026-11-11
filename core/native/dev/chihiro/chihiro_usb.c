/*
 * hakuX native core — the Sega Chihiro base boards: the QC (Cypress AN2131,
 * VID 0CA3 PID 0002: JVS, EEPROMs, backup RAM, RTC) and the SC (PID 0003:
 * the serial lines to the drive board and card readers), each running its
 * real 8051 firmware from the user's EEPROM dumps (ic10 = QC, pc20 = SC,
 * ic11 = the settings EEPROM). Chihiro mode (NC_CHIHIRO) only.
 *
 * XPSemu phase 3, our adaptation of the USB side of tattoohanz's xemu-based
 * Chihiro fork (hw/xbox/chihiro/chihiro-usb.c and the hot plug in chihiro.c;
 * LGPL-2.1-or-later; Copyright (c) 2016 espes, (c) 2026 Réda Chérif-Touil;
 * xemu by Matt Borgerson; Chihiro work by Tovarichtch and tattoohanz), with
 * the region byte rule as Cxbx-Reloaded's JVS.cpp has it (GPL-2.0-or-later,
 * Luke Usher and the Cxbx-Reloaded team) and the fork's chihiro.c refines it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * What is ours: NcUsbDev callbacks instead of QEMU's USBDevice; timing from
 * the OHCI frame thread (1 ms: the 8051s' 6000-cycle tick, the hot plug 50
 * and 100 frames after each bus start); the dumps found under
 * /data/xpsemu/chihiro (one level of subfolders); writes kept in
 * /data/xpsemu/chihiro/run. Not here (yet): freeplay and cabinet link
 * settings. The SC's peers by the game ID: the Sega 838 (patch 205), the
 * V257 and the HW210 / CRP-1231 card readers (patch 225); cards kept in
 * /data/xpsemu/chihiro/run/<gameid>.card<N>.bin.
 *
 * Topology (the fork's: usb_create_simple takes the first free root ports):
 * QC on root port 1, SC on root port 2; the Xbox pads keep ports 3 and 4.
 */
#include "native/dev/chihiro/chihiro_usb.h"
#include "native/dev/chihiro/an2131.h"
#include "native/dev/chihiro/jvs.h"
#include "native/dev/chihiro/driveboard.h"
#include "native/dev/chihiro/driveboard_v257.h"
#include "native/dev/chihiro/cardreader_hw210.h"
#include "native/dev/chihiro/cardreader_crp1231.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "hakuX-native-chihiro"
#define ROOT_DIR "/data/xpsemu/chihiro"
#define RUN_DIR  ROOT_DIR "/run"

#define QC_PORT 1
#define SC_PORT 2
#define QC_PLUG_FRAMES 50
#define SC_PLUG_FRAMES 100
#define LOG_REQS 200

/* The boards' lock (XPSemu patch 196): the 8051s' millisecond runs on the OHCI frame thread after it lets the
 * OHCI lock go; a request, a poll or a USB reset (under the OHCI lock) takes this one too. Order: OHCI, boards. */
static pthread_mutex_t g_bl = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_run_max_us, g_run_sum_us, g_run_n, g_req_max_us;

static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

typedef struct Board {
    NcUsbDev dev;
    bool is_qc;
    const char *id;
    unsigned port;
    bool attached;
    bool use_lle;               /* the firmware runs */
    uint32_t fw_bytes_written;  /* ANCHOR_LOAD (0xA0) bytes */
    bool fw_cpu_held;

    uint8_t eeprom[8192];       /* ic10 (QC) / pc20 (SC) */
    uint8_t ic11[512];          /* the settings EEPROM; 256-byte I2C window */
    uint8_t extmem[2][65536];   /* the battery-backed SRAM, 128 KB */
    ChihiroJVSState jvs;
    AN2131State an;

    unsigned reqs_logged, std_logged, bulk_logged;
    uint64_t reqs, bulk_in, bulk_out;
    uint64_t last_steps, last_cycles, last_idle, last_reqs;
} Board;

static Board *g_qc, *g_sc;
static bool g_enabled, g_checked;
static uint64_t g_frame;
static uint64_t g_qc_at, g_sc_at;   /* frame to plug in at, 0: none due */
static unsigned g_bus_starts;
static uint8_t g_saved_ic11[256];
static uint8_t g_saved_backup[2][65536];

bool nc_chihiro_usb_enabled(void)
{
    if (!g_checked) {
        g_checked = true;
        g_enabled = getenv("NC_CHIHIRO") != NULL;
    }
    return g_enabled;
}

/* ------------------------------------------------------------- files */

static long read_file(const char *path, uint8_t *buf, long cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    long n = -1;
    if (size >= 0 && size <= cap) {
        n = (long)fread(buf, 1, (size_t)size, f) == size ? size : -1;
    } else if (size > cap) {
        n = -2;     /* too big */
    }
    fclose(f);
    return n;
}

static bool write_file(const char *path, const void *buf, size_t len)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        nc_log(TAG, "save: cannot write %s (errno %d)", tmp, errno);
        return false;
    }
    const bool ok = fwrite(buf, 1, len, f) == len;
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
        nc_log(TAG, "save: writing %s failed (errno %d)", path, errno);
        unlink(tmp);
        return false;
    }
    return true;
}

/* A dump by its name: /data/xpsemu/chihiro/<name>, else in one of its
 * subfolders (not run/), sized min..max. */
static long find_dump(const char *name, uint8_t *buf, long min, long max, char *used, size_t used_cap)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ROOT_DIR, name);
    long n = read_file(path, buf, max);
    if (n >= min) {
        snprintf(used, used_cap, "%s", path);
        return n;
    }
    if (n != -1) {
        nc_log(TAG, "dump %s: wrong size (%ld, want %ld..%ld), skipped", path, n, min, max);
    }
    DIR *d = opendir(ROOT_DIR);
    if (!d) {
        nc_log(TAG, "dump %s: %s does not exist", name, ROOT_DIR);
        return -1;
    }
    struct dirent *de;
    long found = -1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || !strcmp(de->d_name, "run")) {
            continue;
        }
        char dir[400];
        snprintf(dir, sizeof(dir), "%s/%s", ROOT_DIR, de->d_name);
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        n = read_file(path, buf, max);
        if (n >= min) {
            snprintf(used, used_cap, "%s", path);
            found = n;
            break;
        }
        if (n != -1) {
            nc_log(TAG, "dump %s: wrong size (%ld, want %ld..%ld), skipped", path, n, min, max);
        }
    }
    closedir(d);
    return found;
}

/* The persisted copies, written when the guest changed them. */
/* The saves go to the disk on their own thread (XPSemu patch 202): written on the OHCI frame thread, a 128 KiB
 * write (the PS5's app writes can take hundreds of ms) stopped the USB frames meanwhile; OutRun 2's requests timed
 * out and its cancel crashed. Here the frame thread only copies what changed and signals. */
static pthread_mutex_t g_sv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sv_cond = PTHREAD_COND_INITIALIZER;
static bool g_sv_started, g_sv_ic11, g_sv_backup;
static uint8_t g_sv_ic11_buf[256];
static uint8_t g_sv_backup_buf[2][65536];
/* XPSemu patch 264: where the backup RAM and the game's half of the ic11 window are kept. Every game's acLib puts
 * its record at the start of the backup device (SRAM 0x8400; Maximum Tune "SBHQ0000" 0x589c bytes, MT2
 * "SBKD0000", OutRun 2 "OUTRUN2") and the game's EEPROM record at ic11 0x80: with one shared file the game played
 * last overwrote the others', and each game found a foreign record at boot and reset its test-menu settings,
 * bookkeeping and rankings. Now <gameid>.backup_ram.bin and <gameid>.ic11_game.bin, per game; the system half of
 * ic11 (0x00-0x7F: SEGABOOT's coin / free play settings) stays shared, as in a cabinet. Set at load. */
static char g_backup_path[512] = RUN_DIR "/backup_ram.bin";
static char g_ic11_game_path[512];

/* patch 225: the cards' files (HW210 2048 bytes, CRP-1231 69), written by the same thread. Each slot keeps the
 * latest data stored for its file this run, so a card taken out and pushed in again reads what it was given even
 * before the thread wrote it. */
#define SV_CARDS 4
static struct {
    bool valid, due;
    char path[512];
    uint8_t buf[CARD_TOTAL_SIZE];
    size_t len;
} g_sv_card[SV_CARDS];

static bool sv_card_due(void)
{
    for (int i = 0; i < SV_CARDS; i++) {
        if (g_sv_card[i].due) {
            return true;
        }
    }
    return false;
}

static void *save_thread(void *arg);

/* g_sv_lock held */
static void sv_start_locked(void)
{
    if (!g_sv_started) {
        pthread_t t;
        g_sv_started = pthread_create(&t, NULL, save_thread, NULL) == 0;
        if (g_sv_started) {
            pthread_detach(t);
        }
    }
}

bool chihiro_card_store(const char *path, const uint8_t *data, size_t len)
{
    if (!path || !path[0] || len > CARD_TOTAL_SIZE) {
        return false;
    }
    pthread_mutex_lock(&g_sv_lock);
    int k = -1;
    for (int i = 0; i < SV_CARDS && k < 0; i++) {
        if (g_sv_card[i].valid && !strcmp(g_sv_card[i].path, path)) {
            k = i;
        }
    }
    for (int i = 0; i < SV_CARDS && k < 0; i++) {
        if (!g_sv_card[i].valid || !g_sv_card[i].due) {
            k = i;      /* a free slot, or one already on the disk */
        }
    }
    if (k < 0) {
        pthread_mutex_unlock(&g_sv_lock);
        nc_log(TAG, "card: %s not saved (every card slot still waits for the disk)", path);
        return false;
    }
    sv_start_locked();
    g_sv_card[k].valid = g_sv_card[k].due = true;
    snprintf(g_sv_card[k].path, sizeof(g_sv_card[k].path), "%s", path);
    memcpy(g_sv_card[k].buf, data, len);
    g_sv_card[k].len = len;
    pthread_cond_signal(&g_sv_cond);
    pthread_mutex_unlock(&g_sv_lock);
    return true;
}

long chihiro_card_read(const char *path, uint8_t *buf, long cap)
{
    pthread_mutex_lock(&g_sv_lock);
    for (int i = 0; i < SV_CARDS; i++) {
        if (g_sv_card[i].valid && !strcmp(g_sv_card[i].path, path)) {
            const long n = (long)g_sv_card[i].len;
            if (n <= cap) {
                memcpy(buf, g_sv_card[i].buf, (size_t)n);
            }
            pthread_mutex_unlock(&g_sv_lock);
            return n <= cap ? n : -2;
        }
    }
    pthread_mutex_unlock(&g_sv_lock);
    return read_file(path, buf, cap);
}

static void *save_thread(void *arg)
{
    (void)arg;
    static uint8_t ic11[256];
    static uint8_t backup[2][65536];
    static struct {
        bool due;
        char path[512];
        uint8_t buf[CARD_TOTAL_SIZE];
        size_t len;
    } cards[SV_CARDS];
    for (;;) {
        pthread_mutex_lock(&g_sv_lock);
        while (!g_sv_ic11 && !g_sv_backup && !sv_card_due()) {
            pthread_cond_wait(&g_sv_cond, &g_sv_lock);
        }
        const bool w11 = g_sv_ic11, wb = g_sv_backup;
        if (w11) {
            memcpy(ic11, g_sv_ic11_buf, sizeof(ic11));
        }
        if (wb) {
            memcpy(backup, g_sv_backup_buf, sizeof(backup));
        }
        for (int i = 0; i < SV_CARDS; i++) {
            cards[i].due = g_sv_card[i].due;
            if (cards[i].due) {
                memcpy(cards[i].path, g_sv_card[i].path, sizeof(cards[i].path));
                memcpy(cards[i].buf, g_sv_card[i].buf, g_sv_card[i].len);
                cards[i].len = g_sv_card[i].len;
                g_sv_card[i].due = false;
            }
        }
        g_sv_ic11 = g_sv_backup = false;
        pthread_mutex_unlock(&g_sv_lock);
        for (int i = 0; i < SV_CARDS; i++) {
            if (cards[i].due && write_file(cards[i].path, cards[i].buf, cards[i].len)) {
                static unsigned logged;
                if (logged++ < 40) {
                    nc_log(TAG, "save: card %s written (%zu bytes)", cards[i].path, cards[i].len);
                }
            }
        }
        const uint64_t t0 = mono_us();
        if (w11 && write_file(RUN_DIR "/ic11_eeprom.bin", ic11, sizeof(ic11))) {
            nc_log(TAG, "save: ic11 EEPROM written (%s/ic11_eeprom.bin)", RUN_DIR);
        }
        /* patch 264: the game's half of the ic11 window (0x80-0xFF) also to the game's own file */
        if (w11 && g_ic11_game_path[0] && write_file(g_ic11_game_path, ic11 + 0x80, 0x80)) {
            nc_log(TAG, "save: the game's ic11 half written (%s) (patch 264)", g_ic11_game_path);
        }
        if (wb && write_file(g_backup_path, backup, sizeof(backup))) {
            static unsigned logged;
            if (logged++ < 20) {
                nc_log(TAG, "save: backup RAM written (%s, 128 KiB) in %.1f ms, off the USB thread",
                       g_backup_path, (mono_us() - t0) / 1000.0);
            }
        }
    }
    return NULL;
}

static void save_flush(bool force)
{
    Board *b = g_qc;
    if (!b || (!force && !b->an.backup_dirty && !b->an.ic11_dirty)) {
        return;
    }
    b->an.backup_dirty = false;
    b->an.ic11_dirty = false;
    const bool c11 = memcmp(g_saved_ic11, b->ic11, sizeof(g_saved_ic11)) != 0;
    const bool cb = memcmp(g_saved_backup, b->extmem, sizeof(g_saved_backup)) != 0;
    if (!c11 && !cb) {
        return;
    }
    pthread_mutex_lock(&g_sv_lock);
    sv_start_locked();
    if (c11) {
        memcpy(g_saved_ic11, b->ic11, sizeof(g_saved_ic11));
        memcpy(g_sv_ic11_buf, b->ic11, sizeof(g_sv_ic11_buf));
        g_sv_ic11 = true;
    }
    if (cb) {
        memcpy(g_saved_backup, b->extmem, sizeof(g_saved_backup));
        memcpy(g_sv_backup_buf, b->extmem, sizeof(g_sv_backup_buf));
        g_sv_backup = true;
    }
    pthread_cond_signal(&g_sv_cond);
    pthread_mutex_unlock(&g_sv_lock);
}

/* ------------------------------------------------------------- region */

/* The cabinet's region, the byte at ic10 0x1F00: 1 Japan, 2 USA, 3 Export.
 * SEGABOOT tests bit (1 << byte) of the mask at boot.id + 0x38 and stops on
 * Error 05 when it is clear (Cxbx-Reloaded JVS.cpp; the fork's
 * chihiro_region_byte). Preferred: NC_CHIHIRO_REGION (1-3), else Export; a
 * game whose mask lacks it gets the first of Export, USA, Japan it has. */
static uint8_t region_byte(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return (uint8_t)cached;
    }
    static const char *const names[] = {"?", "Japan", "USA", "Export"};
    const char *e = getenv("NC_CHIHIRO_REGION");
    int set = e ? atoi(e) : 3;
    if (set < 1 || set > 3) {
        set = 3;
    }
    uint8_t bid[0x100];
    const char *fs = getenv("NC_CHIHIRO_FS");
    bool have = false;
    static const char *const files[] = {"boot.id", "BOOT.ID", "Boot.id"};
    for (unsigned i = 0; fs && i < 3 && !have; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", fs, files[i]);
        FILE *f = fopen(path, "rb");    /* its head only: boot.id is longer */
        if (f) {
            have = fread(bid, 1, sizeof(bid), f) >= 0x40 && !memcmp(bid, "BTID", 4);
            fclose(f);
        }
    }
    const uint8_t mask = have ? bid[0x38] : 0;
    int region = set;
    if (!have) {
        nc_log(TAG, "region: %s (preferred); no boot.id under NC_CHIHIRO_FS (%s) to check it against",
               names[region], fs ? fs : "unset");
    } else if (mask & (1u << set)) {
        nc_log(TAG, "region: %s (preferred), game %.4s accepts it (boot.id mask %02x)", names[region],
               (const char *)bid + 0x30, mask);
    } else if (mask & 0x0E) {
        for (int r = 3; r >= 1; r--) {
            if (mask & (1u << r)) {
                region = r;
                break;
            }
        }
        nc_log(TAG, "region: %s; game %.4s has no %s (boot.id mask %02x)", names[region],
               (const char *)bid + 0x30, names[set], mask);
    } else {
        nc_log(TAG, "region: %s (preferred); boot.id mask %02x lists none", names[region], mask);
    }
    cached = region;
    return (uint8_t)region;
}

/* ---------------------------------------------------- the USB device */

/* Descriptors as the fork declares them, for the log and a fallback only:
 * the firmware answers GET_DESCRIPTOR itself (QC: from CODE:0B7A). */
static const uint8_t k_qc_dev[18] = {
    18, 1, 0x00, 0x01, 0x60, 0x00, 0x00, 0x40,
    0xA3, 0x0C, 0x02, 0x00, 0x08, 0x01, 1, 2, 0, 1,
};
static const uint8_t k_sc_dev[18] = {
    18, 1, 0x00, 0x01, 0x60, 0x01, 0x00, 0x40,
    0xA3, 0x0C, 0x03, 0x00, 0x10, 0x01, 1, 2, 0, 1,
};

static int fallback_cfg(bool qc, uint8_t *out, int cap)
{
    uint8_t c[9 + 9 + 10 * 7];
    const int neps = qc ? 10 : 6, half = neps / 2;
    int n = 0;
    const uint8_t cfg[9] = {9, 2, 0, 0, 1, 1, 0, 0x80, 0x96};
    memcpy(c, cfg, 9);
    n = 9;
    const uint8_t itf[9] = {9, 4, 0, 0, (uint8_t)neps, 0xFF, 0, 0, 0};
    memcpy(c + n, itf, 9);
    n += 9;
    for (int i = 0; i < neps; i++) {
        const uint8_t addr = (uint8_t)(i < half ? (i + 1) : (0x80 | (i - half + 1)));
        const uint8_t ep[7] = {7, 5, addr, 0x02, 0x40, 0x00, 0};
        memcpy(c + n, ep, 7);
        n += 7;
    }
    c[2] = (uint8_t)n;
    memcpy(out, c, MIN(n, cap));
    return MIN(n, cap);
}

static const char *std_name(uint8_t req)
{
    static const char *const k[] = {"GET_STATUS", "CLEAR_FEATURE", "?", "SET_FEATURE", "?", "SET_ADDRESS",
                                    "GET_DESCRIPTOR", "SET_DESCRIPTOR", "GET_CONFIGURATION",
                                    "SET_CONFIGURATION", "GET_INTERFACE", "SET_INTERFACE", "SYNCH_FRAME"};
    return req < 13 ? k[req] : "?";
}

static void hex_line(char *out, size_t cap, const uint8_t *b, int n, int max)
{
    out[0] = 0;
    int o = 0;
    for (int i = 0; i < n && i < max && (size_t)o + 4 < cap; i++) {
        o += snprintf(out + o, cap - (size_t)o, " %02x", b[i]);
    }
    if (n > max && (size_t)o + 4 < cap) {
        snprintf(out + o, cap - (size_t)o, " ..");
    }
}

static int board_control_locked(NcUsbDev *d, const uint8_t *s, uint8_t *data, int len, uint64_t t0);

static int board_control(NcUsbDev *d, const uint8_t *s, uint8_t *data, int len)
{
    pthread_mutex_lock(&g_bl);
    const int r = board_control_locked(d, s, data, len, mono_us());
    pthread_mutex_unlock(&g_bl);
    return r;
}

static int board_control_locked(NcUsbDev *d, const uint8_t *s, uint8_t *data, int len, uint64_t t0)
{
    Board *b = (Board *)d;
    const uint8_t type = s[0], req = s[1];
    const uint16_t value = nc_rd16(s + 2), index = nc_rd16(s + 4), wlen = nc_rd16(s + 6);
    const bool in = type & 0x80;
    const bool standard = (type & 0x60) == 0;
    int r;
    const char *how = "firmware";

    b->reqs++;
    if (req == 0xA0 && (type & 0x60) == 0x40) {
        /* ANCHOR_LOAD: the silicon's own firmware download, not the 8051's */
        how = "anchor load";
        if (!in) {
            /* The game starting the 8051 (CPUCS RUN): its init (8M cycles) runs here at once, as the fork, whose
             * QEMU clock stands still meanwhile; the guest's time stands still too (patch 201): it took 0.75 s,
             * OutRun 2's USB requests timed out, the cancel crashed. */
            extern bool nc_ke_freeze_begin(void);
            extern uint64_t nc_ke_freeze_end(void);
            const bool start = value == AN_CPUCS && len > 0 && !(data[0] & 0x01) && b->fw_cpu_held;
            const bool frozen = start && nc_ke_freeze_begin();
            const uint64_t f0 = mono_us();
            an2131_anchor_load(&b->an, value, data, len);
            if (frozen) {
                const uint64_t out = nc_ke_freeze_end();
                nc_log(TAG, "%s: 8051 started by the game: the guest's time stood still %.1f ms (%.1f ms left out)",
                       b->id, (mono_us() - f0) / 1000.0, out / 1e6);
            }
            b->fw_bytes_written += (uint32_t)len;
            if (value == AN_CPUCS) {
                const bool hold = len > 0 && (data[0] & 0x01);
                nc_log(TAG, "%s: ANCHOR_LOAD CPUCS=%s (%u bytes loaded by the host)", b->id,
                       hold ? "HOLD" : "RUN", b->fw_bytes_written);
                if (b->fw_cpu_held && !hold) {
                    b->use_lle = b->an.cpu_running;
                    nc_log(TAG, "%s: host firmware loaded, 8051 %s", b->id, b->use_lle ? "running" : "stopped");
                }
                b->fw_cpu_held = hold;
            }
            r = 0;
        } else {
            for (int i = 0; i < len; i++) {
                data[i] = (uint32_t)value + (uint32_t)i < AN2131_RAM_SIZE ? b->an.ram[value + i] : 0xFF;
            }
            r = len;
        }
    } else if (!b->use_lle) {
        how = "no firmware";
        r = NC_USB_STALL;
    } else {
        uint8_t resp[512];
        r = an2131_setup_packet(&b->an, s, in ? NULL : data, in ? 0 : len, resp, sizeof(resp));
        if (in) {
            if (r > 0) {
                r = MIN(r, len);
                memcpy(data, resp, (size_t)r);
            } else if (standard && req == 0x06 && (value >> 8) == 1) {
                how = "FALLBACK (the firmware gave no descriptor)";
                r = MIN(len, 18);
                memcpy(data, b->is_qc ? k_qc_dev : k_sc_dev, (size_t)r);
            } else if (standard && req == 0x06 && (value >> 8) == 2) {
                how = "FALLBACK (the firmware gave no descriptor)";
                r = fallback_cfg(b->is_qc, data, len);
            } else {
                /* as the fork: the whole length, here zeros */
                how = "no answer from the firmware: zeros";
                memset(data, 0, (size_t)len);
                r = len;
            }
        } else {
            r = 0;
        }
    }

    char hex[3 * 16 + 8];
    hex_line(hex, sizeof(hex), data, r > 0 ? r : (!in ? len : 0), 16);
    const uint64_t us = mono_us() - t0;
    if (us > g_req_max_us) {
        g_req_max_us = us;
    }
    static unsigned s_capped_seen[2];
    const bool capped_now = b->an.setup_capped != s_capped_seen[b->is_qc];
    s_capped_seen[b->is_qc] = b->an.setup_capped;
    if (standard) {
        if (b->std_logged < 100) {
            b->std_logged++;
            nc_log(TAG, "%s@%u enum: %s value %04x index %04x len %u -> %d (%s)%s [%llu us%s]", b->id, d->addr,
                   std_name(req), value, index, wlen, r, how, hex, (unsigned long long)us,
                   capped_now ? ", ran to the cycle cap" : "");
        }
    } else if (b->reqs_logged < LOG_REQS) {
        b->reqs_logged++;
        nc_log(TAG, "%s@%u req %02x %02x value %04x index %04x len %u %s -> %d (%s)%s [%llu us%s; ep2 IN %s %u, "
               "ep4 IN %s]", b->id, d->addr, type, req, value, index, wlen, in ? "in" : "out", r, how, hex,
               (unsigned long long)us, capped_now ? ", ran to the cycle cap" : "",
               b->an.ep[2].in_armed ? "armed" : "not armed", b->an.ep[2].bc_in,
               b->an.ep[4].in_armed ? "armed" : "not armed");
    }
    return r;
}

static int board_data_locked(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len);

static int board_data(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len)
{
    pthread_mutex_lock(&g_bl);
    const int r = board_data_locked(d, in, ep, buf, len);
    pthread_mutex_unlock(&g_bl);
    return r;
}

static int board_data_locked(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len)
{
    Board *b = (Board *)d;
    if (ep >= AN2131_EP_COUNT) {
        return NC_USB_STALL;
    }
    if (!b->use_lle) {
        return NC_USB_NAK;
    }
    char hex[3 * 16 + 8];
    if (in) {
        const int avail = an2131_ep_in_poll(&b->an, ep);
        if (avail < 0) {
            return NC_USB_NAK;
        }
        const int got = an2131_ep_in_read(&b->an, ep, buf, MIN(len, AN2131_EP_BUFSZ));
        b->bulk_in++;
        if (b->bulk_logged < LOG_REQS) {
            b->bulk_logged++;
            hex_line(hex, sizeof(hex), buf, got, 16);
            nc_log(TAG, "%s@%u bulk in ep%d: %d of %d bytes:%s", b->id, d->addr, ep, got, len, hex);
        }
        return got;
    }
    /* OUT: the endpoint NAKs while the firmware has the last packet (it
     * re-arms by writing OUTnBC); a TD over 64 bytes goes in packets. */
    if (b->an.ep[ep].cs_out & EPCS_BSY) {
        return NC_USB_NAK;
    }
    int off = 0;
    while (off < len) {
        if (b->an.ep[ep].cs_out & EPCS_BSY) {
            for (int i = 0; i < 10 && (b->an.ep[ep].cs_out & EPCS_BSY); i++) {
                an2131_run(&b->an, 2000);
            }
            if (b->an.ep[ep].cs_out & EPCS_BSY) {
                nc_log(TAG, "%s bulk out ep%d: firmware busy, %d of %d bytes dropped", b->id, ep, len - off, len);
                break;
            }
        }
        const int chunk = MIN(len - off, AN2131_EP_BUFSZ);
        an2131_ep_out_write(&b->an, ep, buf + off, chunk);
        an2131_run(&b->an, 2000);
        off += chunk;
    }
    b->bulk_out++;
    if (b->bulk_logged < LOG_REQS) {
        b->bulk_logged++;
        hex_line(hex, sizeof(hex), buf, len, 16);
        nc_log(TAG, "%s@%u bulk out ep%d: %d bytes:%s", b->id, d->addr, ep, len, hex);
    }
    return len;
}

/* A port reset (USB reset): the QC's region byte set again, the firmware
 * told (URES). */
static void board_reset(NcUsbDev *d)
{
    Board *b = (Board *)d;
    static unsigned logged;
    if (logged++ < 40) {
        nc_log(TAG, "%s: USB reset (port %u, firmware %s)", b->id, b->port, b->use_lle ? "running" : "not running");
    }
    if (b->is_qc) {
        b->eeprom[0x1F00] = region_byte();
    }
    if (b->use_lle) {
        const uint64_t w0 = mono_us();
        pthread_mutex_lock(&g_bl);
        const uint64_t w1 = mono_us();
        b->an.usbirq |= USBIRQ_URES;
        b->an.cpu.irq_recheck = true;
        an2131_run(&b->an, 10000);
        pthread_mutex_unlock(&g_bl);
        const uint64_t w2 = mono_us();
        if (w2 - w0 > 20000) {
            nc_log(TAG, "%s: USB reset waited %.1f ms for the boards, ran %.1f ms (patch 203 tool)", b->id,
                   (w1 - w0) / 1000.0, (w2 - w1) / 1000.0);
        }
    }
}

static Board *board_new(bool qc, const uint8_t *fw, const uint8_t *ic11, long ic11_len, const uint8_t *backup)
{
    Board *b = calloc(1, sizeof(Board));
    if (!b) {
        nc_log(TAG, "%s: out of memory", qc ? "QC" : "SC");
        return NULL;
    }
    b->is_qc = qc;
    b->id = qc ? "QC" : "SC";
    b->port = qc ? QC_PORT : SC_PORT;
    memcpy(b->eeprom, fw, sizeof(b->eeprom));
    if (qc) {
        nc_log(TAG, "QC: ic10 region byte (0x1F00) was %02x", b->eeprom[0x1F00]);
        b->eeprom[0x1F00] = region_byte();
    }
    memcpy(b->ic11, ic11, (size_t)MIN(ic11_len, (long)sizeof(b->ic11)));
    if (backup) {
        memcpy(b->extmem, backup, sizeof(b->extmem));
    }

    b->dev.name = qc ? "chihiro-qc" : "chihiro-sc";
    /* No descriptor tables: usb.c leaves every standard request but
     * SET_ADDRESS to the control callback, which hands it to the firmware. */
    b->dev.dev_desc = NULL;
    b->dev.cfg_desc = NULL;
    b->dev.control = board_control;
    b->dev.data = board_data;
    b->dev.reset = board_reset;
    b->dev.pending_addr = -1;
    b->dev.opaque = b;

    chihiro_jvs_init(&b->jvs);
    if (qc) {
        chihiro_jvs_global = &b->jvs;
    }

    an2131_init(&b->an);
    b->an.is_qc = qc;
    b->an.name = b->id;
    b->an.ic10_eeprom = b->eeprom;
    b->an.ic10_size = sizeof(b->eeprom);
    b->an.ic11_eeprom = b->ic11;
    b->an.ic11_size = 256;
    b->an.extmem = b->extmem[0];

    an2131_b2_boot(&b->an, b->eeprom, sizeof(b->eeprom));
    if (!qc && b->an.cpu_running) {
        /* the SC firmware's init (serial ISRs, its spin polls) finishes in
         * these, as in the fork */
        an2131_run(&b->an, 8000000);
    }
    b->use_lle = b->an.cpu_running;
    b->last_steps = b->an.steps;
    b->last_cycles = b->an.total_cycles;
    b->last_idle = b->an.idle_cycles;
    nc_log(TAG, "%s: board ready, firmware %s (PC %04x, %llu instructions so far); attaches to root port %u "
           "%u ms after each bus start", b->id, b->use_lle ? "RUNNING" : "NOT RUNNING", b->an.cpu.pc,
           (unsigned long long)b->an.steps, b->port, qc ? QC_PLUG_FRAMES : SC_PLUG_FRAMES);
    return b;
}

/* The game ID at boot.id 0x30 ("" when there is none). */
static const char *boot_game_id(void)
{
    static char id[5];
    static bool done;
    if (!done) {
        done = true;
        const char *fs = getenv("NC_CHIHIRO_FS");
        char bp[600];
        uint8_t bid[0x400];
        snprintf(bp, sizeof(bp), "%s/boot.id", fs ? fs : "");
        if (fs && read_file(bp, bid, sizeof(bid)) >= 0x34) {
            memcpy(id, bid + 0x30, 4);
            for (int i = 0; i < 4; i++) {
                if (id[i] < 0x20 || id[i] > 0x7E) {
                    id[0] = '\0';   /* not an ID */
                    break;
                }
            }
        }
    }
    return id;
}

/* ---- patch 225: the cabinet's peers on the SC's UARTs (the fork's, see their files) ---- */
static V257DriveBoard g_v257;
static CRP1231State g_crp;
static ChihiroHW210 g_hw210;
static char g_crp_hand[512];        /* the CRP-1231 card in the player's hand: its file */
static bool g_crp_auto, g_crp_auto_done;

/* A card's file: /data/xpsemu/chihiro/run/<gameid>.card<N>.bin */
static void card_path(char *out, size_t cap, int n)
{
    const char *id = boot_game_id();
    snprintf(out, cap, "%s/%s.card%d.bin", RUN_DIR, id[0] ? id : "game", n);
}

/* The card in hand at the start: the newest card with data (the highest N), else card1. */
static void crp_pick_hand(void)
{
    static uint8_t tmp[CARD_TOTAL_SIZE];
    card_path(g_crp_hand, sizeof(g_crp_hand), 1);
    for (int n = 1; n < 100; n++) {
        char p[512];
        card_path(p, sizeof(p), n);
        if (chihiro_card_read(p, tmp, sizeof(tmp)) > 0) {
            snprintf(g_crp_hand, sizeof(g_crp_hand), "%s", p);
        }
    }
}

/* A blank from the dispenser, written for the first time: the first card file not holding a card yet; the card in
 * hand becomes it (the fork names a new file per card too). */
bool chihiro_crp1231_card_issue(char *out, size_t out_len)
{
    static uint8_t tmp[CARD_TOTAL_SIZE];
    for (int n = 1; n < 100; n++) {
        char p[512];
        card_path(p, sizeof(p), n);
        if (chihiro_card_read(p, tmp, sizeof(tmp)) <= 0) {
            snprintf(out, out_len, "%s", p);
            snprintf(g_crp_hand, sizeof(g_crp_hand), "%s", p);
            nc_log(TAG, "cards: new card issued: %s (now the card in hand)", p);
            return true;
        }
    }
    return false;
}

/* The cabinet's 16 ms service tick (the fork's chihiro_diag_timer_cb): the V257's clock, the HW210 slots; the
 * CRP-1231's card pushed in by itself the first time the game waits for one. Boards' lock held. */
static void cabinet_tick(void)
{
    if (chihiro_v257_global) {
        v257_tick(chihiro_v257_global);
    }
    if (chihiro_hw210_enabled && chihiro_hw210_global) {
        hw210_tick(chihiro_hw210_global);
    }
    if (chihiro_crp1231_enabled && chihiro_crp1231_global && g_crp_auto && !g_crp_auto_done &&
        chihiro_crp1231_global->waiting && chihiro_crp1231_global->card_pos == CRP1231_CARD_NONE) {
        static uint8_t tmp[CARD_TOTAL_SIZE];
        g_crp_auto_done = true;
        if (chihiro_card_read(g_crp_hand, tmp, sizeof(tmp)) == CRP1231_CARD_BYTES) {
            nc_log(TAG, "cards: the game asks for a card: %s pushed in by itself", g_crp_hand);
            crp1231_card_key(chihiro_crp1231_global, g_crp_hand);
        } else {
            nc_log(TAG, "cards: the game asks for a card; no saved card (%s): Circle pushes a blank in",
                   g_crp_hand);
        }
    }
}

void nc_chihiro_usb_init(void)
{
    if (!nc_chihiro_usb_enabled() || g_qc || g_sc) {
        return;
    }
    const char *mon = getenv("NC_CHIHIRO_MONITOR");
    an2131_monitor_31khz = mon && atoi(mon) == 31;
    {
        /* patch 205: OutRun 2's drive board (Sega 838-13683) on the SC's MIDI line; patch 225: Maximum Tune's
         * V257 there instead (SBHQ, SBKD). NC_CHIHIRO_DRIVE=sega838 / v257 / none overrides the game ID. */
        static DriveBoardState s_db;
        const char *dv = getenv("NC_CHIHIRO_DRIVE");
        const char *id = boot_game_id();
        bool want = false, want_v257 = false;
        if (dv) {
            want = !strcmp(dv, "sega838");
            want_v257 = !strcmp(dv, "v257");
        } else {
            want = !strcmp(id, "SGBZ");
            want_v257 = !strcmp(id, "SBHQ") || !strcmp(id, "SBKD");
        }
        if (want) {
            driveboard_init(&s_db);
            chihiro_driveboard_global = &s_db;
            an2131_drive_sega838 = true;
        } else if (want_v257) {
            v257_init(&g_v257);
            chihiro_v257_global = &g_v257;
        }
        nc_log(TAG, "drive board: %s", want ? "Sega 838-13683 (OutRun 2) on the SC's MIDI line"
                                       : want_v257 ? "Namco V257 (Maximum Tune) on the SC's MIDI line" : "none");
    }
    {
        /* patch 225: the card readers by the game ID: Ghost Squad (SBHU) two HW210s (player 1 on the SC's UART1,
         * player 2 on UART0), Maximum Tune 1/2 (SBHQ, SBKD) a CRP-1231 on UART0. NC_CHIHIRO_CARDS=0: none. */
        const char *ce = getenv("NC_CHIHIRO_CARDS");
        const bool cards_on = !(ce && atoi(ce) == 0);
        const char *id = boot_game_id();
        const char *what = "none";
        if (cards_on && !strcmp(id, "SBHU")) {
            char p1[512], p2[512];
            card_path(p1, sizeof(p1), 1);
            card_path(p2, sizeof(p2), 2);
            hw210_setup(&g_hw210, p1, p2, &chihiro_card_stock_vsg);
            chihiro_jvs_card_lock_bits[0] = 0x20;   /* the fork's cabinet table (vsg.xbe tables 0x20D52C..) */
            chihiro_jvs_card_lock_bits[1] = 0x01;
            chihiro_hw210_enabled = true;
            what = "two Tamura HW210 (Ghost Squad): P1 on the SC's UART1, P2 on UART0";
            nc_log(TAG, "cards: P1 %s, P2 %s (Circle pushes P1's card in when the game asks for it)", p1, p2);
        } else if (cards_on && (!strcmp(id, "SBHQ") || !strcmp(id, "SBKD"))) {
            crp1231_init(&g_crp);
            chihiro_crp1231_global = &g_crp;
            chihiro_crp1231_enabled = true;
            crp_pick_hand();
            const char *ae = getenv("NC_CHIHIRO_CARD_AUTO");
            g_crp_auto = !(ae && atoi(ae) == 0);
            what = "Sanwa CRP-1231 (Maximum Tune) on the SC's UART0";
            nc_log(TAG, "cards: the card in hand is %s (Circle in/out; %s)", g_crp_hand,
                   g_crp_auto ? "pushed in by itself the first time the game asks for a card, if it holds data"
                              : "NC_CHIHIRO_CARD_AUTO=0: never by itself");
        }
        nc_log(TAG, "card reader: %s", what);
    }
    nc_log(TAG, "base boards: loading (monitor %s kHz)", an2131_monitor_31khz ? "31" : "15");

    static uint8_t ic10[8192], pc20[8192], ic11[512];
    char p10[512] = "", p20[512] = "", p11[512] = "", used[512];
    const long n10 = find_dump("ic10_g24lc64.bin", ic10, 8192, 8192, p10, sizeof(p10));
    const long n20 = find_dump("pc20_g24lc64.bin", pc20, 8192, 8192, p20, sizeof(p20));
    long n11 = find_dump("ic11_24lc024.bin", ic11, 1, 512, p11, sizeof(p11));
    nc_log(TAG, "dump ic10 (QC firmware): %s", n10 > 0 ? p10 : "MISSING (ic10_g24lc64.bin, 8192 bytes)");
    nc_log(TAG, "dump pc20 (SC firmware): %s", n20 > 0 ? p20 : "MISSING (pc20_g24lc64.bin, 8192 bytes)");
    nc_log(TAG, "dump ic11 (settings EEPROM): %s", n11 > 0 ? p11 : "MISSING (ic11_24lc024.bin, 128 bytes)");

    /* The persisted copies, made from the dumps on first use. */
    mkdir(ROOT_DIR, 0775);
    mkdir(RUN_DIR, 0775);
    {
        uint8_t saved[256];
        snprintf(used, sizeof(used), "%s/ic11_eeprom.bin", RUN_DIR);
        const long n = read_file(used, saved, sizeof(saved));
        if (n == (long)sizeof(saved)) {
            memcpy(ic11, saved, sizeof(saved));
            n11 = sizeof(saved);
            nc_log(TAG, "ic11: the saved copy %s is used", used);
        } else if (n11 > 0) {
            memset(saved, 0, sizeof(saved));
            memcpy(saved, ic11, (size_t)MIN(n11, 256));
            nc_log(TAG, "ic11: first use, %s made from the dump", used);
            write_file(used, saved, sizeof(saved));
        }
        /* patch 264: the game's half (0x80-0xFF) from the game's own file; the first time, from the shared copy */
        const char *gid = boot_game_id();
        if (gid[0] && n11 > 0) {
            snprintf(g_ic11_game_path, sizeof(g_ic11_game_path), "%s/%s.ic11_game.bin", RUN_DIR, gid);
            uint8_t half[0x80];
            if (read_file(g_ic11_game_path, half, sizeof(half)) == (long)sizeof(half)) {
                memcpy(ic11 + 0x80, half, sizeof(half));
                if (n11 < 256) {
                    n11 = 256;
                }
                nc_log(TAG, "ic11: the game's half (0x80-0xFF) from %s (patch 264)", g_ic11_game_path);
            } else {
                write_file(g_ic11_game_path, ic11 + 0x80, 0x80);
                nc_log(TAG, "ic11: first run of %s, %s made from the shared copy (patch 264)", gid, g_ic11_game_path);
            }
        }
        memcpy(g_saved_ic11, ic11, sizeof(g_saved_ic11));
    }
    static uint8_t backup[2][65536];
    {
        const char *gid = boot_game_id();
        if (gid[0]) {
            snprintf(g_backup_path, sizeof(g_backup_path), "%s/%s.backup_ram.bin", RUN_DIR, gid);
        }
        snprintf(used, sizeof(used), "%s", g_backup_path);
        long n = read_file(used, &backup[0][0], sizeof(backup));
        if (n != (long)sizeof(backup) && gid[0]) {
            /* patch 264: a game's first run with its own file starts from the shared copy (it holds this game's
             * record when it was the game played last); written to the game's file from then on */
            snprintf(used, sizeof(used), "%s/backup_ram.bin", RUN_DIR);
            n = read_file(used, &backup[0][0], sizeof(backup));
            if (n == (long)sizeof(backup)) {
                nc_log(TAG, "backup RAM: first run of %s with its own file: %s made from the shared copy %s "
                       "(patch 264)", gid, g_backup_path, used);
                write_file(g_backup_path, backup, sizeof(backup));
                snprintf(used, sizeof(used), "%s", g_backup_path);
            }
        }
        if (n == (long)sizeof(backup)) {
            nc_log(TAG, "backup RAM: the saved copy %s is used (128 KiB)", used);
            /* patch 264: the acLib record at the start of the backup device (SRAM 0x8400, the upper half) */
            const uint8_t *rec = &backup[1][0x8400];
            char name[9];
            for (int i = 0; i < 8; i++) {
                name[i] = rec[i] >= 0x20 && rec[i] < 0x7F ? (char)rec[i] : '.';
            }
            name[8] = '\0';
            nc_log(TAG, "backup RAM: the game record at 8400 is \"%s\" (game %s) (patch 264)", name,
                   gid[0] ? gid : "?");
        } else {
            memset(backup, 0, sizeof(backup));
            nc_log(TAG, "backup RAM: first use, %s made (128 KiB, empty)%s", g_backup_path,
                   n >= 0 ? " (the old one had the wrong size)" : "");
            write_file(g_backup_path, backup, sizeof(backup));
        }
        memcpy(g_saved_backup, backup, sizeof(g_saved_backup));
    }

    if (n10 <= 0 || n20 <= 0 || n11 <= 0) {
        /* as the fork: without the three dumps the boards aren't made */
        nc_log(TAG, "base boards NOT attached: a dump is missing (put ic10_g24lc64.bin, pc20_g24lc64.bin and "
               "ic11_24lc024.bin in %s or a folder in it); SEGABOOT will show Error 02", ROOT_DIR);
        return;
    }
    g_qc = board_new(true, ic10, ic11, n11, &backup[0][0]);
    g_sc = board_new(false, pc20, ic11, n11, NULL);
}

static void plug(Board *b, bool in)
{
    if (!b || b->attached == in) {
        return;
    }
    b->attached = in;
    nc_ohci_root_plug(b->port, in ? &b->dev : NULL);
    nc_log(TAG, "%s %s root port %u (bus start #%u, frame %llu)", b->id, in ? "plugged into" : "pulled out of",
           b->port, g_bus_starts, (unsigned long long)g_frame);
}

/* As the fork (chihiro_on_ohci_bus_start): each bus start, the boards come
 * out and go back in 50 (QC) and 100 ms (SC) later, so the host stack,
 * its root hub interrupt enabled by then, sees fresh connections and does
 * the whole enumeration (the chips' own boot takes ~200 ms on the board). */
void nc_chihiro_usb_bus_start(void)
{
    if (!g_qc && !g_sc) {
        if (nc_chihiro_usb_enabled()) {
            nc_log(TAG, "bus start: no base boards (see the dump lines above)");
        }
        return;
    }
    g_bus_starts++;
    nc_log(TAG, "bus start #%u: QC in %u ms, SC in %u ms", g_bus_starts, QC_PLUG_FRAMES, SC_PLUG_FRAMES);
    plug(g_qc, false);
    plug(g_sc, false);
    g_qc_at = g_frame + QC_PLUG_FRAMES;
    g_sc_at = g_frame + SC_PLUG_FRAMES;
}

static void sanity_log(Board *b)
{
    if (!b) {
        return;
    }
    const uint64_t steps = b->an.steps - b->last_steps, cyc = b->an.total_cycles - b->last_cycles,
                   idle = b->an.idle_cycles - b->last_idle, reqs = b->reqs - b->last_reqs;
    nc_log(TAG, "%s 8051: %llu instr/s, %llu cycles/s (%llu of them idle turns skipped), PC %04x SP %02x IE %02x "
           "EIE %02x USBIEN %02x; %llu requests/10 s, bulk in %llu out %llu, %u capped; %s",
           b->id, (unsigned long long)(steps / 10), (unsigned long long)(cyc / 10), (unsigned long long)(idle / 10),
           b->an.cpu.pc, b->an.cpu.sp, b->an.cpu.sfr[SFR_IE - 0x80], b->an.eie, b->an.usbien,
           (unsigned long long)reqs, (unsigned long long)b->bulk_in, (unsigned long long)b->bulk_out,
           b->an.setup_capped, b->attached ? "attached" : "not attached");
    b->last_steps = b->an.steps;
    b->last_cycles = b->an.total_cycles;
    b->last_idle = b->an.idle_cycles;
    b->last_reqs = b->reqs;
    if (b->is_qc && chihiro_jvs_global) {
        nc_log(TAG, "QC jvs: sense %u id %u, switches %02x %02x%02x %02x%02x, coins %u %u",
               chihiro_jvs_global->sense, chihiro_jvs_global->device_id, chihiro_jvs_global->system_switches,
               chihiro_jvs_global->player_switches[0][0], chihiro_jvs_global->player_switches[0][1],
               chihiro_jvs_global->player_switches[1][0], chihiro_jvs_global->player_switches[1][1],
               chihiro_jvs_global->coin_count[0], chihiro_jvs_global->coin_count[1]);
    }
}

void nc_chihiro_usb_frame(void)
{
    if (!g_qc && !g_sc) {
        return;
    }
    g_frame++;
    if (g_qc_at && g_frame >= g_qc_at) {
        g_qc_at = 0;
        plug(g_qc, true);
    }
    if (g_sc_at && g_frame >= g_sc_at) {
        g_sc_at = 0;
        plug(g_sc, true);
    }
}

/* Phase 4 (XPSemu patch 207): player 1's controller to the JVS I/O board, the fork's layouts
 * (ui/xemu-input-chihiro.c, LGPL-2.1+): racing (OutRun 2): steering on the left stick (analog 0, 0x8000 the
 * centre), gas RT (analog 1), brake LT (analog 2), view change Y (P1 sw0 bit 4), gear up Black / down White on
 * player 2's up/down pins (P2 sw0 bits 5/4, as the cabinet's sequential shifter). Every game: Start or A = start
 * (P1 sw0 bit 7), D-pad left = service (P1 sw0 bit 6), D-pad right = test (system bit 7), D-pad up or Back = a
 * coin. On the DualSense (the default map): left stick, R2, L2, Triangle, R1, L1, Options/Cross, D-pad, Create. */
static float pad_axis(int16_t raw, float dead)
{
    float v = raw / 32767.0f;
    const float m = v < 0 ? -v : v;
    if (m < dead) {
        return 0.0f;
    }
    const float o = (m - dead) / (1.0f - dead);
    return v < 0 ? -(o > 1.0f ? 1.0f : o) : (o > 1.0f ? 1.0f : o);
}

/* Phase 4, every game (patch 220): the fork's layouts (ui/xemu-input-chihiro.c, GPL-2.0-or-later), by the game
 * ID at boot.id 0x30, player 1's controller in its Xbox layout:
 *   driving (OutRun 2, OR2 Special Tours): left stick steering, RT gas, LT brake, Black (R1) gear up / White (L1)
 *     down on player 2's up/down pins, Y view change
 *   Crazy Taxi: same pedals and steering, Black (R1) drive, White (L1) reverse, X (Square) jump
 *   Maximum Tune 1/2: same, Black/White move the H shifter's lever one gear, Y view, X intrude change
 *   Ollie King: left stick the board (swing, front/rear), White/Black the grabs
 *   gun games (House of the Dead 3, Virtua Cop 3, Ghost Squad): a crosshair moved by the left stick (finer near
 *     the centre), RT the trigger, LT reload (the gun off the picture while held), Black (R1) the body button,
 *     White (L1) change (Ghost Squad) or the pedal (Virtua Cop 3)
 *   every game: Start or A start, D-pad up or Back a coin; D-pad left/right service/test only with
 *     NC_CHIHIRO_SERVICE_KEYS=1 (patch 209) */
enum { PRO_DRIVE, PRO_CTX, PRO_WMMT, PRO_OLLIE, PRO_HOTD3, PRO_VC3, PRO_GS };

static int game_profile(void)
{
    static int pro = -1;
    if (pro >= 0) {
        return pro;
    }
    pro = PRO_DRIVE;
    const char *fs = getenv("NC_CHIHIRO_FS");
    char bp[600];
    uint8_t bid[0x400];
    snprintf(bp, sizeof(bp), "%s/boot.id", fs ? fs : "");
    char id[5] = "";
    if (fs && read_file(bp, bid, sizeof(bid)) >= 0x34) {
        memcpy(id, bid + 0x30, 4);
    }
    static const struct {
        const char *id;
        int pro;
        const char *name;
    } k[] = {
        {"SGBZ", PRO_DRIVE, "driving"}, {"SBJE", PRO_DRIVE, "driving"}, {"SBFY", PRO_CTX, "Crazy Taxi"},
        {"SBHQ", PRO_WMMT, "Maximum Tune"}, {"SBKD", PRO_WMMT, "Maximum Tune"}, {"SBHF", PRO_OLLIE, "Ollie King"},
        {"SBFN", PRO_HOTD3, "gun (House of the Dead 3)"}, {"SBFZ", PRO_VC3, "gun (Virtua Cop 3)"},
        {"SBHU", PRO_GS, "gun (Ghost Squad)"},
    };
    const char *name = "driving (the default)";
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (!strcmp(id, k[i].id)) {
            pro = k[i].pro;
            name = k[i].name;
        }
    }
    nc_log(TAG, "controls: game %s, the %s layout", id[0] ? id : "?", name);
    return pro;
}

static bool g_gun, g_aim_off;
static float g_aim_x = 0.5f, g_aim_y = 0.5f;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* the fork's aim: the deadzone taken out and squared (fine near the centre) */
static float aim_axis(int16_t raw)
{
    const float v = raw / 32767.0f, m = v < 0 ? -v : v;
    if (m < 0.10f) {
        return 0.0f;
    }
    /* patch 222: a gentler curve than the fork's square (small tilts moved too little, then it ran off) */
    const float o = clampf((m - 0.10f) / 0.90f, 0.0f, 1.0f);
    const float c = o * sqrtf(o);
    return v < 0 ? -c : c;
}

/* patch 224: the aim moves with the time, not the controller's samples: the stick's last values move it from
 * the last update to now, whoever asks (the controller thread for the JVS analogs, the overlay at each picture for
 * the crosshair). Moved only on the controller's samples, it stepped and the drawn crosshair jumped. */
static pthread_mutex_t g_aim_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_aim_sx, g_aim_sy;
static uint64_t g_aim_us;

/* patch 226, the user's: ABSOLUTE aiming by default: the stick IS the crosshair (centre = the picture's centre,
 * full tilt = its edge), eased toward it over ~25 ms so it glides; NC_CHIHIRO_AIM=relative keeps the fork's way
 * (the stick pushes the aim) */
static float g_aim_tx = 0.5f, g_aim_ty = 0.5f;

static bool aim_relative(void)
{
    static int rel = -1;
    if (rel < 0) {
        const char *e = getenv("NC_CHIHIRO_AIM");
        rel = !(e && !strcmp(e, "absolute"));       /* patch 228: Hydroid's glide by default */
    }
    return rel;
}

/* patch 227: the mouse (the fork's scale: 1000 counts across the picture); while one moved in the last 3 s the
 * stick leaves the aim alone; its buttons are the gun's */
static uint64_t g_mouse_us;
static uint32_t g_mouse_buttons;

void nc_chihiro_mouse(int dx, int dy, uint32_t buttons)
{
    if (!dx && !dy && !buttons && !g_mouse_us) {
        return;
    }
    pthread_mutex_lock(&g_aim_lock);
    g_mouse_us = mono_us();
    g_mouse_buttons = buttons;
    g_aim_x = clampf(g_aim_x + dx / 1000.0f, 0.02f, 0.98f);
    g_aim_y = clampf(g_aim_y + dy / 1000.0f * 4.0f / 3.0f, 0.02f, 0.98f);
    g_aim_tx = g_aim_x;
    g_aim_ty = g_aim_y;
    pthread_mutex_unlock(&g_aim_lock);
}

static bool mouse_active(uint64_t now)
{
    return g_mouse_us && now - g_mouse_us < 3000000;
}

static void aim_advance(uint64_t now)
{
    if (mouse_active(now)) {
        g_aim_us = now;
        return;             /* the mouse placed it */
    }
    float dt = g_aim_us ? (now - g_aim_us) / 1e6f : 0.0f;
    g_aim_us = now;
    dt = clampf(dt, 0.0f, 0.1f);
    if (!aim_relative()) {
        const float k = 1.0f - expf(-dt / 0.045f);    /* eased over ~45 ms: the stick's noise smoothed */
        g_aim_x = clampf(g_aim_x + (g_aim_tx - g_aim_x) * k, 0.02f, 0.98f);
        g_aim_y = clampf(g_aim_y + (g_aim_ty - g_aim_y) * k, 0.02f, 0.98f);
        return;
    }
    /* patch 228, Hydroid's cursor (unity_host.c / cocos_host.c): a glide, speed by the tilt (15% rest zone, a
     * 1.6 curve, ~1 picture a second at full tilt), its direction the stick's; the stick itself filtered first
     * (60 ms) so bursty or noisy samples (remote play, a drifting stick) give a steady speed, not steps */
    static float fx, fy;
    const float kf = 1.0f - expf(-dt / 0.06f);
    fx += (g_aim_sx - fx) * kf;
    fy += (g_aim_sy - fy) * kf;
    float m = sqrtf(fx * fx + fy * fy);
    if (m < 0.15f) {
        return;
    }
    const float mm = m > 1.0f ? 1.0f : m;
    const float speed = 1.0f * powf((mm - 0.15f) / 0.85f, 1.6f) * dt;    /* pictures per second */
    g_aim_x = clampf(g_aim_x + fx / m * speed, 0.02f, 0.98f);
    g_aim_y = clampf(g_aim_y + fy / m * speed * 4.0f / 3.0f, 0.02f, 0.98f);
}

bool nc_chihiro_crosshair(float *x, float *y)
{
    if (!g_gun || g_aim_off) {     /* patch 231: Ghost Squad keeps ours too (its sight vanishes when shooting) */
        return false;
    }
    pthread_mutex_lock(&g_aim_lock);
    aim_advance(mono_us());
    *x = g_aim_x;
    *y = g_aim_y;
    pthread_mutex_unlock(&g_aim_lock);
    return true;
}

void nc_chihiro_pad_input(const NcPadState *s)
{
    if (!g_qc) {
        return;
    }
    const int pro = game_profile();
    uint8_t p1[2] = {0, 0}, p2[2] = {0, 0}, sys = 0;
    uint16_t an[JVS_MAX_ANALOG];
    for (int i = 0; i < JVS_MAX_ANALOG; i++) {
        an[i] = 0x8000;
    }
    const bool b_a = s->analog[0] > 127, b_x = s->analog[2] > 127, b_y = s->analog[3] > 127,
               black = s->analog[4] > 127, white = s->analog[5] > 127;
    const bool circle = s->analog[1] > 127;          /* patch 225: B (Circle), the card key */
    g_gun = pro == PRO_HOTD3 || pro == PRO_VC3 || pro == PRO_GS;
    if (g_gun) {
        pthread_mutex_lock(&g_aim_lock);
        aim_advance(mono_us());                         /* to now with the stick as it was */
        g_aim_sx = clampf(s->lx / 32767.0f, -1.0f, 1.0f);
        g_aim_sy = clampf(-s->ly / 32767.0f, -1.0f, 1.0f);  /* screen down is positive; the stick's up is */
        {
            /* a tool (patch 228): the first 400 stick samples while it moves: their spacing and values */
            static unsigned logged;
            static uint64_t last;
            const uint64_t t = mono_us();
            if ((s->lx > 4000 || s->lx < -4000 || s->ly > 4000 || s->ly < -4000) && logged < 400) {
                logged++;
                nc_log(TAG, "aim: stick %6d %6d after %4.1f ms -> aim %.3f %.3f", s->lx, s->ly,
                       last ? (t - last) / 1000.0 : 0.0, g_aim_x, g_aim_y);
            }
            last = t;
        }
        {
            /* the absolute target: linear, a small rest zone (drift) */
            float lx = s->lx / 32767.0f, ly = s->ly / 32767.0f;
            /* a 12% rest zone, scaled from its edge: the stick's drift (~7% seen) no longer moves it */
            lx = fabsf(lx) < 0.12f ? 0.0f : clampf((lx - copysignf(0.12f, lx)) / 0.88f, -1.0f, 1.0f);
            ly = fabsf(ly) < 0.12f ? 0.0f : clampf((ly - copysignf(0.12f, ly)) / 0.88f, -1.0f, 1.0f);
            g_aim_tx = 0.5f + lx * 0.48f;
            g_aim_ty = 0.5f - ly * 0.48f;               /* the stick's up is the picture's up */
        }
        const float ax = g_aim_x, ay = g_aim_y;
        pthread_mutex_unlock(&g_aim_lock);
        const uint32_t mb = mouse_active(mono_us()) ? g_mouse_buttons : 0;
        const bool fire = s->analog[7] > 127 || (mb & 1), reload = s->analog[6] > 127 || (mb & 0x18);
        if (mb & 2) {
            p1[1] |= 0x80;                           /* right button: the gun's body button */
        }
        if ((mb & 4) && pro != PRO_HOTD3) {
            p1[1] |= 0x40;                           /* middle: change / the pedal */
        }
        g_aim_off = reload;
        if (reload) {
            an[0] = an[1] = 0;
            p1[0] |= 0x01;                           /* SCREEN: off the picture */
        } else {
            an[0] = (uint16_t)(ax * 0xFFFF);
            an[1] = (uint16_t)(ay * 0xFFFF);
        }
        if (fire) {
            p1[0] |= 0x02;
        }
        if (black) {
            p1[1] |= 0x80;                           /* the gun's body button */
        }
        if (white && pro != PRO_HOTD3) {
            p1[1] |= 0x40;                           /* change / the pedal */
        }
        if (pro == PRO_GS && chihiro_hw210_global) {
            /* patch 225 (the fork's): Circle pushes player 1's card in; the slot's switch follows the card */
            static bool card_was;
            if (circle && !card_was) {
                pthread_mutex_lock(&g_bl);
                hw210_card_key(chihiro_hw210_global, 0);
                pthread_mutex_unlock(&g_bl);
            }
            card_was = circle;
            if (chihiro_hw210_enabled && hw210_card_present(chihiro_hw210_global, 0)) {
                p1[1] |= 0x20;
            }
        }
    } else if (pro == PRO_OLLIE) {
        const float sw = -pad_axis(s->lx, 0.08f);
        an[1] = (uint16_t)(0x8000 + (int)(sw * 32767.0f));
        if (s->ly > 16000) {
            p1[0] |= 0x20;                           /* board front */
        } else if (s->ly < -16000) {
            p1[0] |= 0x10;                           /* board rear */
        }
        if (white) {
            p1[0] |= 0x02;
        }
        if (black) {
            p1[0] |= 0x01;
        }
    } else {
        const float steer = pad_axis(s->lx, 0.08f);
        an[0] = (uint16_t)(0x8000 + (int)(steer * 32767.0f));
        an[1] = (uint16_t)(s->analog[7] * 257u);     /* gas: RT */
        an[2] = (uint16_t)(s->analog[6] * 257u);     /* brake: LT */
        if (pro == PRO_CTX) {
            if (black) {
                p1[0] |= 0x20;                       /* drive */
            }
            if (white) {
                p1[0] |= 0x10;                       /* reverse */
            }
            if (b_x) {
                p1[0] |= 0x02;                       /* jump */
            }
        } else if (pro == PRO_WMMT) {
            /* the H gate: a gear at a time, left there (V322.xbe 0x55AA0) */
            static const uint8_t gate[7] = {0x00, 0xA0, 0x60, 0x80, 0x40, 0x90, 0x50};
            static int gear;
            static bool up_was, down_was;
            if (black && !up_was && gear < 6) {
                gear++;
            }
            if (white && !down_was && gear > 0) {
                gear--;
            }
            up_was = black;
            down_was = white;
            p1[1] |= gate[gear];
            if (b_y) {
                p1[1] |= 0x02;                       /* view change */
            }
            if (b_x) {
                p1[1] |= 0x01;                       /* intrude change */
            }
            /* patch 225 (the fork's): the wheel's own encoder for the V257, in its units (0-0x3FF) */
            if (chihiro_v257_global) {
                v257_set_wheel(chihiro_v257_global, (uint16_t)(an[0] >> 6));
            }
            /* the card slot is the reader's own mouth, not a switch: Circle pushes the card in hand in, or takes
             * back one still in the mouth */
            static bool card_was;
            if (circle && !card_was && chihiro_crp1231_global) {
                pthread_mutex_lock(&g_bl);
                crp1231_card_key(chihiro_crp1231_global, g_crp_hand);
                pthread_mutex_unlock(&g_bl);
            }
            card_was = circle;
        } else {
            if (b_y) {
                p1[0] |= 0x10;                       /* view change */
            }
            if (black) {
                p2[0] |= 0x20;                       /* gear up: player 2's up pin */
            }
            if (white) {
                p2[0] |= 0x10;                       /* gear down */
            }
        }
    }
    if ((s->buttons & NC_PAD_START) || (b_a && !g_gun)) {
        p1[0] |= 0x80;                               /* start (A too, but not in the gun games: A is nothing there) */
    }
    if (g_gun && b_a) {
        p1[0] |= 0x80;
    }
    static int svc = -1;
    if (svc < 0) {
        const char *e = getenv("NC_CHIHIRO_SERVICE_KEYS");
        svc = e && atoi(e) == 1;
    }
    if (svc && (s->buttons & NC_PAD_DPAD_LEFT)) {
        p1[0] |= 0x40;                               /* service */
    }
    if (svc && (s->buttons & NC_PAD_DPAD_RIGHT)) {
        sys |= 0x80;                                 /* test */
    }
    nc_chihiro_jvs_set(sys, p1, p2, an);

    static bool coin_was;
    const bool coin = (s->buttons & (NC_PAD_DPAD_UP | NC_PAD_BACK)) != 0;
    if (coin && !coin_was) {
        nc_chihiro_jvs_coin(0);
    }
    coin_was = coin;

    static uint8_t last[5];
    static unsigned logged;
    const uint8_t now5[5] = {sys, p1[0], p1[1], p2[0], p2[1]};
    if (memcmp(now5, last, sizeof(now5)) && logged++ < 120) {
        nc_log(TAG, "controls: system %02x, P1 %02x %02x, P2 %02x %02x (analog %04x %04x %04x)", sys, p1[0], p1[1],
               p2[0], p2[1], an[0], an[1], an[2]);
        /* patch 257 (ps5/src/cpu_ps5.c): Maximum Tune steers by analog 0 - 8000; the centre in its backup, unused */
        extern int rx_wmmt_backup_centre(void) __attribute__((weak));
        const int c = rx_wmmt_backup_centre ? rx_wmmt_backup_centre() : -1;
        if (c >= 0) {
            nc_log(TAG, "controls: Maximum Tune steering %+d (analog %04x - centre 8000, / 64; its backup centre %04x "
                   "unused) (patch 257)", ((int)an[0] - 0x8000) / 64, an[0], c);
        }
    }
    memcpy(last, now5, sizeof(now5));
}

/* patch 209: the drive board's force feedback as player 1's rumble, both motors the same (the fork's), strength
 * NC_CHIHIRO_FFB percent (100 default, 0 off, up to 200) */
void nc_chihiro_rumble(uint16_t *left, uint16_t *right)
{
    *left = *right = 0;
    if (!chihiro_driveboard_global && !chihiro_v257_global) {
        return;
    }
    static int str = -1;
    if (str < 0) {
        const char *e = getenv("NC_CHIHIRO_FFB");
        str = e && *e ? atoi(e) : 100;
        str = str < 0 ? 0 : str > 200 ? 200 : str;
    }
    if (chihiro_v257_global) {
        /* patch 225 (the fork's ffb_level, V257 case): a pad has no wheel to turn, so it feels the board's push;
         * a collision's push (|torque| >= 96, new or turned around) begins as a knock, full force for 80 ms. */
        enum { V257_KNOCK_MS = 80, V257_KNOCK_MIN = 96 };
        static int prev, knock_push;
        static uint64_t knock_until;
        int target, spring, damping, torque;
        pthread_mutex_lock(&g_bl);
        v257_drive(chihiro_v257_global, &target, &spring, &damping, &torque);
        pthread_mutex_unlock(&g_bl);
        const uint64_t now = mono_us() / 1000;
        if (abs(torque) >= V257_KNOCK_MIN && (!prev || (torque > 0) != (prev > 0))) {
            knock_push = torque > 0 ? 0x7F : -0x7F;
            knock_until = now + V257_KNOCK_MS;
        }
        prev = torque;
        if (now < knock_until) {
            torque = knock_push;
        }
        const uint32_t r = (uint32_t)abs(torque) * 0xFFFF / 0x7F * (uint32_t)str / 100;
        *left = *right = (uint16_t)(r > 0xFFFF ? 0xFFFF : r);
        static unsigned logged;
        static uint16_t last;
        if (*left != last && logged < 80) {
            logged++;
            nc_log(TAG, "force feedback (V257): %04x (push %d, spring %d, damping %d)", *left, torque, spring,
                   damping);
        }
        last = *left;
        return;
    }
    pthread_mutex_lock(&g_bl);
    const uint32_t r = (uint32_t)driveboard_rumble(chihiro_driveboard_global, mono_us() / 1000) * (uint32_t)str / 100;
    pthread_mutex_unlock(&g_bl);
    *left = *right = (uint16_t)(r > 0xFFFF ? 0xFFFF : r);
    static unsigned logged;
    static uint16_t last;
    if (*left != last && logged < 80) {
        logged++;
        nc_log(TAG, "force feedback: %04x (motor %s, package %d step %u, vibration %02x)", *left,
               chihiro_driveboard_global->motor_active ? "on" : "off", chihiro_driveboard_global->play_pkg,
               chihiro_driveboard_global->play_pos, chihiro_driveboard_global->vibration_power);
    }
    last = *left;
}

/* ---- savestates (patch 209): the CHIH section, only in a Chihiro run: the two boards (their 8051s, RAM, EEPROMs,
 * battery SRAM, JVS), the drive board, the hot plug's clock. Raw structures, their sizes checked: a state loads in
 * the build that made it (or one with the same structures). ---- */
#include "native/state/state.h"

#define CHIH_MAGIC 0x32484843u      /* "CHH2" (patch 225: the V257 and the card readers too) */

static void put_board(NcBuf *b, Board *bd)
{
    nc_buf_u8(b, bd != NULL);
    if (!bd) {
        return;
    }
    nc_buf_u8(b, bd->attached);
    nc_buf_u8(b, bd->use_lle);
    nc_buf_u8(b, bd->fw_cpu_held);
    nc_buf_u32(b, bd->fw_bytes_written);
    nc_buf_put(b, &bd->dev.addr, 1);
    nc_buf_put(b, &bd->dev.config, 1);
    nc_buf_put(b, bd->dev.setup, sizeof(bd->dev.setup));
    nc_buf_put(b, bd->dev.buf, sizeof(bd->dev.buf));
    nc_buf_put(b, &bd->dev.len, sizeof(bd->dev.len));
    nc_buf_put(b, &bd->dev.pos, sizeof(bd->dev.pos));
    nc_buf_put(b, &bd->dev.in, sizeof(bd->dev.in));
    nc_buf_put(b, &bd->dev.data_stage, sizeof(bd->dev.data_stage));
    nc_buf_put(b, &bd->dev.stalled, sizeof(bd->dev.stalled));
    nc_buf_put(b, &bd->dev.pending_addr, sizeof(bd->dev.pending_addr));
    nc_buf_put(b, bd->eeprom, sizeof(bd->eeprom));
    nc_buf_put(b, bd->ic11, sizeof(bd->ic11));
    nc_buf_put(b, bd->extmem, sizeof(bd->extmem));
    nc_buf_put(b, &bd->jvs, sizeof(bd->jvs));
    nc_buf_put(b, &bd->an, sizeof(bd->an));
}

static bool get_board(NcRd *r, Board *bd, const char *id)
{
    const bool had = nc_rd_u8(r);
    if (had != (bd != NULL)) {
        nc_log(TAG, "state: the %s board %s the state but %s now", id, had ? "is in" : "isn't in",
               bd ? "is made" : "isn't made");
        return false;
    }
    if (!bd) {
        return true;
    }
    bd->attached = nc_rd_u8(r);
    bd->use_lle = nc_rd_u8(r);
    bd->fw_cpu_held = nc_rd_u8(r);
    bd->fw_bytes_written = nc_rd_u32(r);
    nc_rd_get(r, &bd->dev.addr, 1);
    nc_rd_get(r, &bd->dev.config, 1);
    nc_rd_get(r, bd->dev.setup, sizeof(bd->dev.setup));
    nc_rd_get(r, bd->dev.buf, sizeof(bd->dev.buf));
    nc_rd_get(r, &bd->dev.len, sizeof(bd->dev.len));
    nc_rd_get(r, &bd->dev.pos, sizeof(bd->dev.pos));
    nc_rd_get(r, &bd->dev.in, sizeof(bd->dev.in));
    nc_rd_get(r, &bd->dev.data_stage, sizeof(bd->dev.data_stage));
    nc_rd_get(r, &bd->dev.stalled, sizeof(bd->dev.stalled));
    nc_rd_get(r, &bd->dev.pending_addr, sizeof(bd->dev.pending_addr));
    nc_rd_get(r, bd->eeprom, sizeof(bd->eeprom));
    nc_rd_get(r, bd->ic11, sizeof(bd->ic11));
    nc_rd_get(r, bd->extmem, sizeof(bd->extmem));
    nc_rd_get(r, &bd->jvs, sizeof(bd->jvs));
    nc_rd_get(r, &bd->an, sizeof(bd->an));
    /* the pointers are this run's */
    an2131_relink(&bd->an);
    bd->an.ic10_eeprom = bd->eeprom;
    bd->an.ic10_size = sizeof(bd->eeprom);
    bd->an.ic11_eeprom = bd->ic11;
    bd->an.extmem = bd->extmem[0];
    bd->an.name = bd->id;
    bd->an.is_qc = bd->is_qc;
    nc_ohci_root_restore(bd->port, bd->attached ? &bd->dev : NULL);
    return !r->fail;
}

bool nc_chih_state_save(NcBuf *b)
{
    pthread_mutex_lock(&g_bl);
    nc_buf_u32(b, CHIH_MAGIC);
    nc_buf_u32(b, (uint32_t)sizeof(AN2131State));
    nc_buf_u32(b, (uint32_t)sizeof(ChihiroJVSState));
    nc_buf_u32(b, (uint32_t)sizeof(DriveBoardState));
    nc_buf_u32(b, (uint32_t)(sizeof(V257DriveBoard) + sizeof(CRP1231State) + sizeof(ChihiroHW210)));
    put_board(b, g_qc);
    put_board(b, g_sc);
    nc_buf_put(b, &g_frame, sizeof(g_frame));
    nc_buf_put(b, &g_qc_at, sizeof(g_qc_at));
    nc_buf_put(b, &g_sc_at, sizeof(g_sc_at));
    nc_buf_u32(b, g_bus_starts);
    nc_buf_u8(b, chihiro_driveboard_global != NULL);
    if (chihiro_driveboard_global) {
        nc_buf_put(b, chihiro_driveboard_global, sizeof(DriveBoardState));
    }
    /* patch 225: the V257, the CRP-1231, the HW210 slots with the two JVS values they follow */
    nc_buf_u8(b, chihiro_v257_global != NULL);
    if (chihiro_v257_global) {
        nc_buf_put(b, chihiro_v257_global, sizeof(V257DriveBoard));
    }
    nc_buf_u8(b, chihiro_crp1231_global != NULL);
    if (chihiro_crp1231_global) {
        nc_buf_put(b, chihiro_crp1231_global, sizeof(CRP1231State));
        nc_buf_put(b, g_crp_hand, sizeof(g_crp_hand));
    }
    nc_buf_u8(b, chihiro_hw210_global != NULL);
    if (chihiro_hw210_global) {
        nc_buf_put(b, chihiro_hw210_global, sizeof(ChihiroHW210));
        nc_buf_put(b, chihiro_jvs_card_lock, sizeof(chihiro_jvs_card_lock));
        nc_buf_u32(b, chihiro_jvs_switch_reads);
    }
    pthread_mutex_unlock(&g_bl);
    nc_log(TAG, "state: the base boards saved");
    return true;
}

bool nc_chih_state_load(NcRd *r, unsigned v)
{
    (void)v;
    if (nc_rd_u32(r) != CHIH_MAGIC || nc_rd_u32(r) != (uint32_t)sizeof(AN2131State) ||
        nc_rd_u32(r) != (uint32_t)sizeof(ChihiroJVSState) || nc_rd_u32(r) != (uint32_t)sizeof(DriveBoardState) ||
        nc_rd_u32(r) != (uint32_t)(sizeof(V257DriveBoard) + sizeof(CRP1231State) + sizeof(ChihiroHW210))) {
        nc_log(TAG, "state: the base boards were saved by another build (their layout differs): not loaded");
        return false;
    }
    pthread_mutex_lock(&g_bl);
    bool ok = get_board(r, g_qc, "QC") && get_board(r, g_sc, "SC");
    if (ok) {
        nc_rd_get(r, &g_frame, sizeof(g_frame));
        nc_rd_get(r, &g_qc_at, sizeof(g_qc_at));
        nc_rd_get(r, &g_sc_at, sizeof(g_sc_at));
        g_bus_starts = nc_rd_u32(r);
        if (nc_rd_u8(r)) {
            DriveBoardState db;
            nc_rd_get(r, &db, sizeof(db));
            if (chihiro_driveboard_global) {
                *chihiro_driveboard_global = db;
                chihiro_driveboard_global->play_ms = 0;
            }
        }
        /* patch 225: the peers the state has go into the ones this run made (the same game: the same peers) */
        if (nc_rd_u8(r)) {
            static V257DriveBoard v;
            nc_rd_get(r, &v, sizeof(v));
            if (chihiro_v257_global && !r->fail) {
                *chihiro_v257_global = v;
            }
        }
        if (nc_rd_u8(r)) {
            static CRP1231State c;
            static char hand[sizeof(g_crp_hand)];
            nc_rd_get(r, &c, sizeof(c));
            nc_rd_get(r, hand, sizeof(hand));
            if (chihiro_crp1231_global && !r->fail) {
                *chihiro_crp1231_global = c;
                hand[sizeof(hand) - 1] = '\0';
                memcpy(g_crp_hand, hand, sizeof(g_crp_hand));
            }
        }
        if (nc_rd_u8(r)) {
            static ChihiroHW210 h;
            bool lock[2];
            nc_rd_get(r, &h, sizeof(h));
            nc_rd_get(r, lock, sizeof(lock));
            const uint32_t reads = nc_rd_u32(r);
            if (chihiro_hw210_global && !r->fail) {
                const CardStock *stock = chihiro_hw210_global->stock;
                *chihiro_hw210_global = h;
                chihiro_hw210_global->stock = stock;       /* this run's pointer */
                memcpy(chihiro_jvs_card_lock, lock, sizeof(lock));
                chihiro_jvs_switch_reads = reads;
            }
        }
        ok = !r->fail;
    }
    pthread_mutex_unlock(&g_bl);
    nc_log(TAG, "state: the base boards %s", ok ? "loaded" : "NOT loaded");
    return ok;
}

/* The 8051s' millisecond (6000 cycles, the fork's tick), after the OHCI frame let its lock go (patch 196). */
void nc_chihiro_usb_run(void)
{
    if (!g_qc && !g_sc) {
        return;
    }
    static uint64_t s_last_frame;
    if (g_frame == s_last_frame) {
        return;
    }
    s_last_frame = g_frame;
    pthread_mutex_lock(&g_bl);
    const uint64_t t0 = mono_us();
    Board *const boards[2] = {g_qc, g_sc};
    for (int i = 0; i < 2; i++) {
        if (boards[i] && boards[i]->use_lle) {
            an2131_run(&boards[i]->an, 6000);
        }
    }
    static uint64_t s_tick_at;
    if (g_frame >= s_tick_at || s_tick_at > g_frame + 16) {    /* the second: g_frame went back (a state load) */
        s_tick_at = g_frame + 16;
        cabinet_tick();     /* patch 225: the cabinet's 16 ms service tick (frames are 1 ms) */
    }
    const uint64_t us = mono_us() - t0;
    if (us > 20000) {
        static unsigned logged;
        if (logged++ < 50) {
            nc_log(TAG, "8051 millisecond took %.1f ms (QC PC %04x, SC PC %04x) (patch 203 tool)", us / 1000.0,
                   g_qc ? g_qc->an.cpu.pc : 0, g_sc ? g_sc->an.cpu.pc : 0);
        }
    }
    g_run_sum_us += us;
    g_run_n++;
    if (us > g_run_max_us) {
        g_run_max_us = us;
    }
    if (g_frame % 2000 == 0) {
        save_flush(false);
    }
    if (g_frame % 10000 == 0) {
        sanity_log(g_qc);
        sanity_log(g_sc);
        nc_log(TAG, "host time: 8051 millisecond avg %llu us, longest %llu us; longest request %llu us (10 s)",
               (unsigned long long)(g_run_n ? g_run_sum_us / g_run_n : 0), (unsigned long long)g_run_max_us,
               (unsigned long long)g_req_max_us);
        g_run_sum_us = g_run_n = g_run_max_us = g_req_max_us = 0;
    }
    pthread_mutex_unlock(&g_bl);
}
