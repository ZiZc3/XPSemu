/*
 * XPSemu (XOG) native core: Xbox trainers (.etm). See trainer.h.
 * Our own code (patch 246). GPL-3.0-or-later.
 *
 * The .etm header (all little endian), as read from the user's pack:
 *   +0x00 u32  the trainer's length (<= the file's)
 *   +0x04 u16  1 (version)
 *   +0x06 u16  0x23 (the header's size: code may start there)
 *   +0x0A u32  option bytes: one per option, 1 on
 *   +0x0E u32  text table: u32 offsets of C strings (name, author, then one per
 *              option), ended by 0
 *   +0x12 u32  title IDs (two, or three when the third isn't the option bytes)
 *   +0x16 u32  entry
 *   +0x1A u32  0 (an .xbtf's own entry)
 */
#include "native/core/trainer.h"
#include "native/core/nc.h"
#include "native/core/vm.h"
#include "native/kernel/kernel.h"
#include "hw/xbox/hle/core/xbe.h"

#include "native/core/irq.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "hakuX-native-trainer"
#define STUB_AREA 0x40u                 /* the stub, then the trainer at block + STUB_AREA */
#define SLACK 0x1000u                   /* zeroed room after it, for trainers that keep data past their end */
#define PORT_DONE 0xEC00u               /* the stub: the runs are over, the title starts */
#define SMB_BASE 0xC000u                /* the MCPX SMBus ports some trainers write (the LED) */

static char g_path[512];
static char g_bits[NC_TRAINER_MAX_OPTIONS + 1];
static volatile bool g_pending;
static char g_name[NC_TRAINER_TEXT];
static uint8_t g_smb[16];
static unsigned g_smb_logged;

/* ---- reading ---- */

static void say(char *why, size_t n, const char *text)
{
    if (why && n) {
        snprintf(why, n, "%s", text);
    }
}

/* A C string at off, cut to the text size; false when it runs past the end. */
static bool text_at(const uint8_t *buf, size_t n, uint32_t off, char *out)
{
    if (off < 0x23 || off >= n) {
        return false;
    }
    size_t k = 0;
    while (off + k < n && buf[off + k]) {
        k++;
    }
    if (off + k >= n) {
        return false;
    }
    size_t o = 0;
    for (size_t i = 0; i < k && o + 1 < NC_TRAINER_TEXT; i++) {
        const uint8_t c = buf[off + i];
        out[o++] = c < 0x20 ? ' ' : (char)c;
    }
    out[o] = 0;
    while (o && out[o - 1] == ' ') {
        out[--o] = 0;
    }
    return true;
}

static void add_date(NcTrainerInfo *info, uint32_t d)
{
    for (unsigned i = 0; i < info->ndates; i++) {
        if (info->dates[i] == d) {
            return;
        }
    }
    if (info->ndates < NC_TRAINER_MAX_DATES) {
        info->dates[info->ndates++] = d;
    }
}

/* The release checks in its code: "cmp dword [0x10114], date", or "mov edi, 0x10114"
 * then "cmp dword [edi], date" (one or more, for trainers made for two releases). */
static void find_dates(const uint8_t *buf, size_t n, NcTrainerInfo *info)
{
    static const uint8_t cmp_abs[6] = {0x81, 0x3D, 0x14, 0x01, 0x01, 0x00};
    static const uint8_t mov_edi[5] = {0xBF, 0x14, 0x01, 0x01, 0x00};
    for (size_t i = 0x23; i + 10 <= n; i++) {
        if (!memcmp(buf + i, cmp_abs, sizeof(cmp_abs))) {
            add_date(info, nc_rd32(buf + i + 6));
        } else if (!memcmp(buf + i, mov_edi, sizeof(mov_edi))) {
            for (size_t j = i + 5; j + 6 <= n && j < i + 5 + 192; j++) {
                if (buf[j] == 0x81 && buf[j + 1] == 0x3F) {
                    add_date(info, nc_rd32(buf + j + 2));
                }
            }
        }
    }
}

bool nc_trainer_parse(const uint8_t *buf, size_t n, NcTrainerInfo *info, char *why, size_t why_size)
{
    memset(info, 0, sizeof(*info));
    if (n >= 4 && !memcmp(buf, "Rar!", 4)) {
        say(why, why_size, "a RAR archive named .etm, not a trainer");
        return false;
    }
    if (n < 0x23 + 8) {
        say(why, why_size, "too short for a trainer");
        return false;
    }
    if (n > NC_TRAINER_MAX_SIZE) {
        say(why, why_size, "too big for a trainer (more than 64 KiB)");
        return false;
    }
    const uint32_t len = nc_rd32(buf), ver = nc_rd16(buf + 4), hdr = nc_rd16(buf + 6);
    if (ver != 1 || hdr != 0x23) {
        say(why, why_size, "not an .etm trainer (its header isn't version 1, size 0x23)");
        return false;
    }
    if (len < 0x23 || len > n) {
        say(why, why_size, "a broken trainer (its length is past the file's end)");
        return false;
    }
    info->size = (uint32_t)n;
    info->sel = nc_rd32(buf + 0x0A);
    info->txt = nc_rd32(buf + 0x0E);
    info->ids = nc_rd32(buf + 0x12);
    info->entry = nc_rd32(buf + 0x16);
    if (nc_rd32(buf + 0x1A)) {
        say(why, why_size, "an .xbtf-style trainer (its own entry at +0x1A): not supported yet");
        return false;
    }
    if (info->sel < 0x23 || info->sel >= n || info->txt < 0x23 || info->txt + 4 > n || info->ids < 0x23 ||
        info->ids + 4 > n || info->entry < 0x23 || info->entry >= n) {
        say(why, why_size, "a broken trainer (its header's offsets are past its end)");
        return false;
    }
    /* the texts: name, author, then the options */
    unsigned texts = 0;
    for (uint32_t at = info->txt; at + 4 <= n; at += 4) {
        const uint32_t off = nc_rd32(buf + at);
        if (!off) {
            break;
        }
        char t[NC_TRAINER_TEXT];
        if (!text_at(buf, n, off, t)) {
            say(why, why_size, "a broken trainer (a text is past its end)");
            return false;
        }
        if (texts == 0) {
            memcpy(info->name, t, sizeof(t));
        } else if (texts == 1) {
            memcpy(info->author, t, sizeof(t));
        } else if (texts - 2 < NC_TRAINER_MAX_OPTIONS) {
            memcpy(info->options[texts - 2], t, sizeof(t));
        } else {
            say(why, why_size, "a broken trainer (more than 100 options)");
            return false;
        }
        texts++;
    }
    info->noptions = texts > 2 ? texts - 2 : 0;
    if (info->sel + info->noptions > n) {
        say(why, why_size, "a broken trainer (its option bytes are past its end)");
        return false;
    }
    /* the title IDs: up to three, the third only when it isn't the option bytes */
    for (unsigned i = 0; i < 3; i++) {
        const uint32_t at = info->ids + 4 * i;
        if (at + 4 > n || (info->ids < info->sel && at + 4 > info->sel)) {
            break;
        }
        const uint32_t id = nc_rd32(buf + at);
        if (id && id != 0xFFFFFFFFu) {
            info->title_ids[info->nids++] = id;
        }
    }
    if (!info->nids) {
        say(why, why_size, "a broken trainer (no title ID)");
        return false;
    }
    find_dates(buf, n, info);
    return true;
}

static uint8_t *read_file(const char *path, size_t *size, char *why, size_t why_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        say(why, why_size, "the file can't be opened");
        return NULL;
    }
    uint8_t *buf = malloc(NC_TRAINER_MAX_SIZE + 1);
    const size_t n = buf ? fread(buf, 1, NC_TRAINER_MAX_SIZE + 1, fp) : 0;
    fclose(fp);
    if (!buf) {
        say(why, why_size, "no memory to read it");
        return NULL;
    }
    *size = n;
    return buf;
}

bool nc_trainer_read(const char *path, NcTrainerInfo *info, char *why, size_t why_size)
{
    size_t n = 0;
    uint8_t *buf = read_file(path, &n, why, why_size);
    if (!buf) {
        memset(info, 0, sizeof(*info));
        return false;
    }
    const bool ok = nc_trainer_parse(buf, n, info, why, why_size);
    free(buf);
    return ok;
}

bool nc_trainer_for_title(const NcTrainerInfo *info, uint32_t title_id)
{
    for (unsigned i = 0; i < info->nids; i++) {
        if (info->title_ids[i] == title_id) {
            return true;
        }
    }
    return false;
}

/* ---- the start line ---- */

void nc_trainer_set(const char *path)
{
    snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
}

void nc_trainer_set_options(const char *bits)
{
    snprintf(g_bits, sizeof(g_bits), "%s", bits ? bits : "");
}

bool nc_trainer_wanted(void)
{
    return g_path[0] != 0;
}

bool nc_trainer_pending(void)
{
    return g_pending;
}

/* ---- live switches (patch 252) ----
 *
 * Recorded at the start, before the title's entry runs (the game's code still as loaded): the stub
 * asks on PORT_REC whether to run the trainer's entry again, and reports each return there.
 *   run "chosen"  the options as chosen: the run the game keeps (today's single run). If the kernel
 *                 was called in it (or its code names the kernel's thunks), nothing more: restart only
 *   run 0         every option off: what the trainer always does (hooks, code caves): "the base"
 *   run 1..n      option i alone: the bytes that differ from the base are option i's
 * Before each run the copy (snapshot) of the XBE image and the trainer's block is put back (a trainer
 * whose code names other places, the kernel or the old loaders' 0x8000000C/10/14, is restart only). At the end
 * the chosen run's bytes go in again, and base + the chosen options one by one must give the same
 * bytes, else the trainer is restart only. A switch then writes an option's new or old bytes
 * (rx_cpu_code_write: our hooks and sweep kept right) and its option byte, on the guest CPU's thread
 * at its pause point (the quick menu pauses the game). Options whose bytes overlap are exclusive. */
#define PORT_REC 0xEC01u
#define LIVE_MAX_IMAGE (32u << 20)
#define LIVE_MAX_RANGES 4096u
#define LIVE_MAX_POOL (256u * 1024u)
#define STEP_START (-2)
#define STEP_CHOSEN (-1)
#define STEP_DONE 1000

typedef struct LiveArea {
    uint32_t va, len;
    uint8_t *snap, *ref;    /* as loaded; as the base left it */
} LiveArea;
typedef struct LiveRange {
    uint32_t va, len, at;   /* old bytes at g_pool + at, new at g_pool + at + len */
} LiveRange;
typedef struct LiveSet {
    uint32_t first, count, bytes;
} LiveSet;

extern uint8_t *rx_cpu_code_ptr(uint32_t va) __attribute__((weak));
extern bool rx_cpu_code_write(uint32_t va, const uint8_t *bytes, uint32_t n) __attribute__((weak));
extern volatile uint64_t g_xog_kcalls;

static NcVm *g_vm;
static NcTrainerInfo g_info;
static struct {
    int step;
    bool record;
    unsigned n;
    uint32_t base, optva, image_va, image_len, block_len;
    char chosen[NC_TRAINER_MAX_OPTIONS + 1];
    uint64_t kcalls;
    uint32_t ret;
    LiveArea area[2];        /* the XBE image, the trainer's block */
    unsigned nareas;
} g_rec;
static struct {
    bool ok;
    char why[200];
    char bits[NC_TRAINER_MAX_OPTIONS + 1];
} g_live;
static LiveRange *g_rng;
static unsigned g_nrng, g_caprng;
static uint8_t *g_pool;
static uint32_t g_npool, g_cappool;
static LiveSet g_base, g_chosen_set, g_opt[NC_TRAINER_MAX_OPTIONS];

static struct {
    unsigned option;
    bool on;
    int beats;
    uint32_t told;      /* ranges already logged as changed (bit per range of the set, first 32) */
} g_watch[4];

static pthread_mutex_t g_lmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_lcv = PTHREAD_COND_INITIALIZER;
static struct {
    bool pending, taking, done;
    unsigned option;
    bool on;
    int result;
    char msg[200];
} g_lreq;

static void live_no(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void live_no(const char *fmt, ...)
{
    if (g_live.why[0]) {
        return;     /* the first reason stays */
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_live.why, sizeof(g_live.why), fmt, ap);
    va_end(ap);
    g_live.ok = false;
}

/* The host's view of a guest byte: through the CPU layer's code_byte (shadows), else the RAM. */
static uint8_t *host_at(uint32_t va)
{
    if (rx_cpu_code_ptr) {
        return rx_cpu_code_ptr(va);
    }
    const uint32_t pa = g_vm ? nc_vm_phys(g_vm, va) : UINT32_MAX;
    return pa == UINT32_MAX || pa >= g_vm->ram_size ? NULL : g_vm->phys + pa;
}

static uint32_t page_left(uint32_t va, uint32_t left)
{
    const uint32_t n = NC_PAGE - (va & (NC_PAGE - 1));
    return n < left ? n : left;
}

static bool is_opt_byte(uint32_t va)
{
    return va >= g_rec.optva && va - g_rec.optva < g_rec.n;
}

static void set_opt_bytes(const char *bits)
{
    for (unsigned i = 0; i < g_rec.n; i++) {
        uint8_t *p = host_at(g_rec.optva + i);
        if (p) {
            *p = bits && bits[i] == '1' ? 1 : 0;
        }
    }
}

static void areas_free(void)
{
    for (unsigned i = 0; i < g_rec.nareas; i++) {
        free(g_rec.area[i].snap);
        free(g_rec.area[i].ref);
    }
    memset(g_rec.area, 0, sizeof(g_rec.area));
    g_rec.nareas = 0;
}

static bool area_add(uint32_t va, uint32_t len)
{
    LiveArea *a = &g_rec.area[g_rec.nareas];
    a->va = va;
    a->len = len;
    a->snap = malloc(len);
    a->ref = malloc(len);
    if (!a->snap || !a->ref) {
        free(a->snap);
        free(a->ref);
        a->snap = a->ref = NULL;
        return false;
    }
    for (uint32_t off = 0; off < len;) {
        const uint32_t n = page_left(va + off, len - off);
        const uint8_t *p = host_at(va + off);
        if (p) {
            memcpy(a->snap + off, p, n);
        } else {
            memset(a->snap + off, 0, n);
        }
        off += n;
    }
    memcpy(a->ref, a->snap, len);
    g_rec.nareas++;
    return true;
}

static uint8_t *ref_at(uint32_t va)
{
    for (unsigned i = 0; i < g_rec.nareas; i++) {
        const LiveArea *a = &g_rec.area[i];
        if (va >= a->va && va - a->va < a->len) {
            return a->ref + (va - a->va);
        }
    }
    return NULL;
}

static bool range_add(LiveSet *set, uint32_t va, const uint8_t *old, const uint8_t *now, uint32_t len)
{
    if (g_nrng == g_caprng) {
        const unsigned cap = g_caprng ? g_caprng * 2 : 64;
        LiveRange *r = cap <= LIVE_MAX_RANGES ? realloc(g_rng, cap * sizeof(*r)) : NULL;
        if (!r) {
            return false;
        }
        g_rng = r;
        g_caprng = cap;
    }
    while (g_npool + 2 * len > g_cappool) {
        const uint32_t cap = g_cappool ? g_cappool * 2 : 4096;
        uint8_t *p = cap <= LIVE_MAX_POOL ? realloc(g_pool, cap) : NULL;
        if (!p) {
            return false;
        }
        g_pool = p;
        g_cappool = cap;
    }
    g_rng[g_nrng].va = va;
    g_rng[g_nrng].len = len;
    g_rng[g_nrng].at = g_npool;
    memcpy(g_pool + g_npool, old, len);
    memcpy(g_pool + g_npool + len, now, len);
    g_npool += 2 * len;
    g_nrng++;
    set->count++;
    set->bytes += len;
    return true;
}

/* The bytes of the areas that differ from the copy as loaded (vs_ref false) or as the base left it,
 * the option bytes left out: set's ranges. False when the tables are full. */
static bool diff_into(LiveSet *set, bool vs_ref)
{
    set->first = g_nrng;
    set->count = set->bytes = 0;
    for (unsigned k = 0; k < g_rec.nareas; k++) {
        const LiveArea *a = &g_rec.area[k];
        const uint8_t *cmp = vs_ref ? a->ref : a->snap;
        for (uint32_t off = 0; off < a->len;) {
            const uint32_t n = page_left(a->va + off, a->len - off);
            const uint8_t *p = host_at(a->va + off);
            if (p && memcmp(p, cmp + off, n)) {
                for (uint32_t i = 0; i < n;) {
                    if (p[i] == cmp[off + i] || is_opt_byte(a->va + off + i)) {
                        i++;
                        continue;
                    }
                    uint32_t j = i;
                    while (j < n && p[j] != cmp[off + j] && !is_opt_byte(a->va + off + j)) {
                        j++;
                    }
                    if (!range_add(set, a->va + off + i, cmp + off + i, p + i, j - i)) {
                        return false;
                    }
                    i = j;
                }
            }
            off += n;
        }
    }
    return true;
}

/* Memory as loaded again (the option bytes too: they are set before each run). */
static void restore_snap(void)
{
    for (unsigned k = 0; k < g_rec.nareas; k++) {
        const LiveArea *a = &g_rec.area[k];
        for (uint32_t off = 0; off < a->len;) {
            const uint32_t n = page_left(a->va + off, a->len - off);
            uint8_t *p = host_at(a->va + off);
            if (p && memcmp(p, a->snap + off, n)) {
                memcpy(p, a->snap + off, n);
            }
            off += n;
        }
    }
}

/* At the start (nothing of ours in the code yet): a set's new bytes straight in. */
static void apply_boot(const LiveSet *s)
{
    for (uint32_t r = s->first; r < s->first + s->count; r++) {
        const LiveRange *x = &g_rng[r];
        for (uint32_t i = 0; i < x->len; i++) {
            uint8_t *p = host_at(x->va + i);
            if (p) {
                *p = g_pool[x->at + x->len + i];
            }
        }
    }
}

static bool sets_overlap(const LiveSet *a, const LiveSet *b)
{
    for (uint32_t i = a->first; i < a->first + a->count; i++) {
        for (uint32_t j = b->first; j < b->first + b->count; j++) {
            if (g_rng[i].va < g_rng[j].va + g_rng[j].len && g_rng[j].va < g_rng[i].va + g_rng[i].len) {
                return true;
            }
        }
    }
    return false;
}

static void hex(char *out, size_t n, const uint8_t *b, uint32_t len)
{
    out[0] = 0;
    for (uint32_t i = 0; i < len && i < 8; i++) {
        snprintf(out + strlen(out), n - strlen(out), "%s%02x", i ? " " : "", b[i]);
    }
    if (len > 8) {
        snprintf(out + strlen(out), n - strlen(out), " ..");
    }
}

static void log_set(const char *what, const LiveSet *s)
{
    char line[900];
    snprintf(line, sizeof(line), "%s: %u place%s, %u bytes", what, s->count, s->count == 1 ? "" : "s", s->bytes);
    for (uint32_t r = s->first; r < s->first + s->count && r < s->first + 6; r++) {
        const LiveRange *x = &g_rng[r];
        char o[40], w[40];
        hex(o, sizeof(o), g_pool + x->at, x->len);
        hex(w, sizeof(w), g_pool + x->at + x->len, x->len);
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s %08x+%u [%s] -> [%s]", r == s->first ? ":" : ";",
                 x->va, x->len, o, w);
    }
    if (s->count > 6) {
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "; +%u more", s->count - 6);
    }
    nc_log(TAG, "%s", line);
}

/* Recording stopped after the chosen run: its memory is what the game gets. */
static void rec_stop_after_chosen(void)
{
    areas_free();
    set_opt_bytes(g_rec.chosen);
    g_rec.step = STEP_DONE;
}

/* Recording stopped later: the memory as the chosen run left it again. */
static void rec_stop_later(void)
{
    restore_snap();
    apply_boot(&g_chosen_set);
    areas_free();
    set_opt_bytes(g_rec.chosen);
    g_rec.step = STEP_DONE;
}

static void rec_finish(void)
{
    restore_snap();
    apply_boot(&g_chosen_set);      /* the memory the game gets: as the chosen run left it */
    log_set("always (every option off)", &g_base);
    unsigned code = 0, flip = 0;
    for (unsigned i = 0; i < g_rec.n; i++) {
        char what[NC_TRAINER_TEXT + 32];
        snprintf(what, sizeof(what), "option %u \"%s\"", i + 1, g_info.options[i]);
        if (g_opt[i].count) {
            log_set(what, &g_opt[i]);
            code++;
        } else {
            nc_log(TAG, "%s: no bytes at the start: its switch is its option byte (%08x), which the trainer's hooks "
                   "read as the game runs", what, g_rec.optva + i);
            flip++;
        }
        for (uint32_t r = g_opt[i].first; r < g_opt[i].first + g_opt[i].count; r++) {
            const uint32_t va = g_rng[r].va;
            const LiveArea *img = &g_rec.area[0], *blk = &g_rec.area[1];
            if (!(va - img->va < img->len) && !(va - blk->va < blk->len)) {
                live_no("option %u writes %08x, outside the game's image and the trainer (game memory here)", i + 1, va);
            }
        }
    }
    for (unsigned i = 0; i < g_rec.n; i++) {
        for (unsigned j = i + 1; j < g_rec.n; j++) {
            if (g_opt[i].count && g_opt[j].count && sets_overlap(&g_opt[i], &g_opt[j])) {
                nc_log(TAG, "options %u \"%s\" and %u \"%s\" change the same bytes: on, one turns the other off", i + 1,
                       g_info.options[i], j + 1, g_info.options[j]);
            }
        }
    }
    /* base + the chosen options one by one must be the chosen run's bytes */
    for (unsigned i = 0; i < g_rec.n; i++) {
        if (g_rec.chosen[i] != '1') {
            continue;
        }
        for (uint32_t r = g_opt[i].first; r < g_opt[i].first + g_opt[i].count; r++) {
            for (uint32_t b = 0; b < g_rng[r].len; b++) {
                uint8_t *q = ref_at(g_rng[r].va + b);
                if (q) {
                    *q = g_pool[g_rng[r].at + g_rng[r].len + b];
                }
            }
        }
    }
    uint32_t bad = 0, first_bad = 0;
    for (unsigned k = 0; k < g_rec.nareas; k++) {
        const LiveArea *a = &g_rec.area[k];
        for (uint32_t off = 0; off < a->len;) {
            const uint32_t n = page_left(a->va + off, a->len - off);
            const uint8_t *p = host_at(a->va + off);
            if (p && memcmp(p, a->ref + off, n)) {
                for (uint32_t i = 0; i < n; i++) {
                    if (p[i] != a->ref[off + i] && !is_opt_byte(a->va + off + i)) {
                        first_bad = bad++ ? first_bad : a->va + off + i;
                    }
                }
            }
            off += n;
        }
    }
    if (bad) {
        live_no("the options chosen together change other bytes than one by one (%u, first at %08x)", bad, first_bad);
    }
    areas_free();
    set_opt_bytes(g_rec.chosen);
    g_rec.step = STEP_DONE;
    if (!g_live.why[0]) {
        g_live.ok = true;
        nc_log(TAG, "live switches: on (%u options: %u write code, %u only their byte); %u places, %u bytes of the "
               "pool", g_rec.n, code, flip, g_nrng, g_npool);
    } else {
        nc_log(TAG, "live switches: off: %s; its options apply on restart", g_live.why);
    }
}

/* The stub asks: run the entry again? Each run's options set here. */
static uint32_t rec_next(void)
{
    if (g_rec.step == STEP_START) {
        g_rec.step = STEP_CHOSEN;
        if (g_rec.record && !area_add(g_rec.image_va, g_rec.image_len)) {
            g_rec.record = false;
            live_no("no memory to copy the game's image (%u KiB)", g_rec.image_len >> 10);
        }
        if (g_rec.record && !area_add(g_rec.base - STUB_AREA, g_rec.block_len)) {
            g_rec.record = false;
            live_no("no memory to copy the trainer's block");
        }
        if (!g_rec.record) {
            areas_free();
        }
        set_opt_bytes(g_rec.chosen);
        g_rec.kcalls = g_xog_kcalls;
        return 1;
    }
    if (g_rec.step >= STEP_DONE || !g_rec.record) {
        return 0;
    }
    char bits[NC_TRAINER_MAX_OPTIONS + 1];
    memset(bits, '0', g_rec.n);
    bits[g_rec.n] = 0;
    if (g_rec.step >= 1) {
        bits[g_rec.step - 1] = '1';
    }
    set_opt_bytes(bits);
    g_rec.kcalls = g_xog_kcalls;
    return 1;
}

/* The stub reports a run's end (EAX: the entry's return). */
static void rec_done(uint32_t ret)
{
    if (g_rec.step == STEP_CHOSEN) {
        g_rec.ret = ret;
    }
    const bool kernel = g_xog_kcalls != g_rec.kcalls;
    if (!g_rec.record) {
        if (kernel) {
            nc_log(TAG, "its entry called the kernel (%llu calls)", (unsigned long long)(g_xog_kcalls - g_rec.kcalls));
        }
        rec_stop_after_chosen();
        return;
    }
    if (g_rec.step == STEP_CHOSEN) {
        if (kernel) {
            live_no("its entry calls the kernel (%llu calls): run once only",
                    (unsigned long long)(g_xog_kcalls - g_rec.kcalls));
            rec_stop_after_chosen();
        } else if (!diff_into(&g_chosen_set, false)) {
            live_no("it changes too many bytes to record");
            rec_stop_after_chosen();
        } else {
            restore_snap();
            g_rec.step = 0;
        }
        return;
    }
    if (kernel) {
        live_no("with %s its entry calls the kernel", g_rec.step ? g_info.options[g_rec.step - 1] : "every option off");
        rec_stop_later();
        return;
    }
    if (g_rec.step == 0) {
        if (!diff_into(&g_base, false)) {
            live_no("it changes too many bytes to record");
            rec_stop_later();
            return;
        }
        for (unsigned k = 0; k < g_rec.nareas; k++) {   /* the base's bytes: what each option is compared with */
            LiveArea *a = &g_rec.area[k];
            for (uint32_t off = 0; off < a->len;) {
                const uint32_t n = page_left(a->va + off, a->len - off);
                const uint8_t *p = host_at(a->va + off);
                if (p) {
                    memcpy(a->ref + off, p, n);
                }
                off += n;
            }
        }
    } else if (!diff_into(&g_opt[g_rec.step - 1], true)) {
        live_no("it changes too many bytes to record");
        rec_stop_later();
        return;
    }
    restore_snap();
    if (++g_rec.step > (int)g_rec.n) {
        rec_finish();
    }
}

/* ---- live switches: the quick menu (its thread) and the guest CPU's (at its pause point) ---- */

static bool write_set(const LiveSet *s, bool now, unsigned *differ)
{
    bool ok = true;
    for (uint32_t r = s->first; r < s->first + s->count; r++) {
        const LiveRange *x = &g_rng[r];
        const uint8_t *want = g_pool + x->at + (now ? x->len : 0), *was = g_pool + x->at + (now ? 0 : x->len);
        for (uint32_t i = 0; i < x->len; i++) {
            const uint8_t *p = host_at(x->va + i);
            *differ += p && *p != was[i];   /* the game's, or our sweep's or hooks' byte there */
        }
        if (rx_cpu_code_write) {
            ok &= rx_cpu_code_write(x->va, want, x->len);
        } else {
            for (uint32_t i = 0; i < x->len; i++) {
                uint8_t *p = host_at(x->va + i);
                if (p) {
                    *p = want[i];
                }
            }
        }
    }
    return ok;
}

static void opt_byte(unsigned i, bool on)
{
    uint8_t *p = host_at(g_rec.optva + i);
    if (p) {
        *p = on ? 1 : 0;
    }
    g_live.bits[i] = on ? '1' : '0';
}

static int live_apply(unsigned i, bool on, char *msg, size_t n)
{
    unsigned differ = 0;
    bool ok = true;
    char offs[300] = "";
    /* options sharing bytes with it: off first (their bytes back to the base's) */
    for (unsigned j = 0; j < g_rec.n; j++) {
        if (j != i && g_live.bits[j] == '1' && sets_overlap(&g_opt[i], &g_opt[j])) {
            ok &= write_set(&g_opt[j], false, &differ);
            opt_byte(j, false);
            snprintf(offs + strlen(offs), sizeof(offs) - strlen(offs), "%s%s", offs[0] ? ", " : "", g_info.options[j]);
        }
    }
    ok &= write_set(&g_opt[i], on, &differ);
    opt_byte(i, on);
    {
        unsigned w = 0;
        for (unsigned k = 0; k < 4; k++) {
            if (g_watch[k].beats <= 0 || g_watch[k].option == i) {
                w = k;
                break;
            }
        }
        g_watch[w].option = i;
        g_watch[w].on = on;
        g_watch[w].beats = 15;
        g_watch[w].told = 0;
    }
    nc_log(TAG, "live: option %u \"%s\" %s: %u places, %u bytes %s%s%s%s", i + 1, g_info.options[i], on ? "on" : "off",
           g_opt[i].count, g_opt[i].bytes, on ? "written" : "put back",
           g_opt[i].count ? "" : " (none: its option byte only)", offs[0] ? "; turned off (same bytes): " : "", offs);
    if (differ) {
        nc_log(TAG, "live: %u bytes were not what was recorded before the write (the game's own change, or our "
               "sweep/hooks there)", differ);
    }
    if (!ok) {
        nc_log(TAG, "live: a write failed (a page not mapped)");
    }
    snprintf(msg, n, "%s%s%s", offs[0] ? "Turned off (same bytes): " : "", offs, ok ? "" : " (a write failed)");
    return ok ? 1 : -1;
}

void nc_trainer_cpu_work(void)
{
    pthread_mutex_lock(&g_lmu);
    const bool take = g_lreq.pending;
    g_lreq.pending = false;
    g_lreq.taking = take;
    const unsigned i = g_lreq.option;
    const bool on = g_lreq.on;
    pthread_mutex_unlock(&g_lmu);
    if (!take) {
        return;
    }
    char msg[200] = "";
    const int r = live_apply(i, on, msg, sizeof(msg));
    pthread_mutex_lock(&g_lmu);
    g_lreq.result = r;
    snprintf(g_lreq.msg, sizeof(g_lreq.msg), "%s", msg);
    g_lreq.done = true;
    g_lreq.taking = false;
    pthread_cond_broadcast(&g_lcv);
    pthread_mutex_unlock(&g_lmu);
}

int nc_trainer_live(char *why, size_t n)
{
    if (!g_live.ok) {
        say(why, n, g_live.why[0] ? g_live.why : "options apply on restart");
        return 0;
    }
    say(why, n, "");
    return 1;
}

int nc_trainer_live_bits(char *out, size_t n)
{
    if (!out || !n) {
        return 0;
    }
    snprintf(out, n, "%s", g_live.ok ? g_live.bits : "");
    return g_live.ok ? (int)g_rec.n : 0;
}

int nc_trainer_live_overlap(unsigned i, unsigned j)
{
    return g_live.ok && i < g_rec.n && j < g_rec.n && i != j && sets_overlap(&g_opt[i], &g_opt[j]);
}

unsigned nc_trainer_live_bytes(unsigned i)
{
    return g_live.ok && i < g_rec.n ? g_opt[i].bytes : 0;
}

int nc_trainer_live_set(unsigned i, int on, char *msg, size_t n)
{
    if (!g_live.ok || i >= g_rec.n) {
        say(msg, n, g_live.why[0] ? g_live.why : "options apply on restart");
        return 0;
    }
    if (!nc_paused()) {
        say(msg, n, "the game isn't paused");
        return -1;
    }
    pthread_mutex_lock(&g_lmu);
    g_lreq.option = i;
    g_lreq.on = on != 0;
    g_lreq.done = g_lreq.taking = false;
    g_lreq.pending = true;
    pthread_mutex_unlock(&g_lmu);
    nc_pause_kick();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    pthread_mutex_lock(&g_lmu);
    int rc = 0;
    while (!g_lreq.done && (rc == 0 || g_lreq.taking)) {
        rc = g_lreq.taking ? pthread_cond_wait(&g_lcv, &g_lmu) : pthread_cond_timedwait(&g_lcv, &g_lmu, &ts);
    }
    int r;
    if (g_lreq.done) {
        r = g_lreq.result;
        say(msg, n, g_lreq.msg);
    } else {
        g_lreq.pending = false;     /* not taken: withdrawn */
        r = -1;
        say(msg, n, "the game did not stop");
        nc_log(TAG, "live: option %u: the guest CPU did not take the switch within 3 s", i + 1);
    }
    pthread_mutex_unlock(&g_lmu);
    return r;
}

void nc_trainer_live_watch(void)
{
    if (!g_live.ok) {
        return;
    }
    for (unsigned k = 0; k < 4; k++) {
        if (g_watch[k].beats <= 0) {
            continue;
        }
        const unsigned i = g_watch[k].option;
        const bool on = g_watch[k].on;
        const LiveSet *s = &g_opt[i];
        unsigned bad = 0;
        for (uint32_t r = s->first; r < s->first + s->count; r++) {
            const LiveRange *x = &g_rng[r];
            const uint8_t *want = g_pool + x->at + (on ? x->len : 0);
            char now[64] = "";
            bool diff = false;
            for (uint32_t b = 0; b < x->len && b < 16; b++) {
                const uint8_t *p = host_at(x->va + b);
                diff |= !p || *p != want[b];
                snprintf(now + strlen(now), sizeof(now) - strlen(now), "%s%02x", b ? " " : "", p ? *p : 0);
            }
            const uint32_t bit = r - s->first < 32 ? 1u << (r - s->first) : 0;
            if (diff) {
                bad++;
                if (!(g_watch[k].told & bit)) {
                    g_watch[k].told |= bit;
                    nc_log(TAG, "live watch: option %u \"%s\" (%s): %08x+%u changed after the switch: now [%s]", i + 1,
                           g_info.options[i], on ? "on" : "off", x->va, x->len, now);
                }
            }
        }
        const uint8_t *ob = host_at(g_rec.optva + i);
        if (ob && *ob != (on ? 1 : 0)) {
            nc_log(TAG, "live watch: option %u: its option byte (%08x) is %u, not %u", i + 1, g_rec.optva + i, *ob, on ? 1 : 0);
        }
        if (--g_watch[k].beats == 0) {
            nc_log(TAG, "live watch: option %u \"%s\" (%s): 15 s after the switch, %u of %u places as the switch left them",
                   i + 1, g_info.options[i], on ? "on" : "off", s->count - bad, s->count);
        }
    }
}

void nc_trainer_state_loaded(void)
{
    memset(&g_live, 0, sizeof(g_live));
    snprintf(g_live.why, sizeof(g_live.why), "loaded from a save state: options apply on restart");
    nc_log(TAG, "live switches: off: %s", g_live.why);
}

/* An instruction with an absolute address at p: the operand's offset (mov reg,imm32; mov eax,[abs];
 * call/jmp [abs]; an ALU or mov op whose modrm is [disp32]), else 0. Not a decoder: a pattern each. */
static uint32_t abs_operand(const uint8_t *p)
{
    static const uint8_t modrm_ops[] = {0x01, 0x03, 0x09, 0x0B, 0x21, 0x23, 0x29, 0x2B, 0x31, 0x33, 0x39, 0x3B,
                                        0x80, 0x81, 0x83, 0x88, 0x89, 0x8A, 0x8B, 0xC6, 0xC7, 0xF7, 0xFF};
    if ((p[0] >= 0xB8 && p[0] <= 0xBF) || p[0] == 0xA1 || p[0] == 0xA3) {
        return 1;
    }
    if ((p[1] & 0xC7) == 0x05) {
        for (unsigned k = 0; k < sizeof(modrm_ops); k++) {
            if (p[0] == modrm_ops[k]) {
                return 2;
            }
        }
    }
    return 0;
}

/* ---- install ---- */

uint32_t nc_trainer_install(NcKernel *k, NcVm *vm, const HleXbeInfo *xbe, uint32_t entry)
{
    g_pending = false;
    memset(&g_live, 0, sizeof(g_live));
    memset(&g_rec, 0, sizeof(g_rec));
    g_nrng = 0;
    g_npool = 0;
    g_vm = vm;
    if (!g_path[0]) {
        return entry;
    }
    /* patch 252: until it is placed, the menu says it isn't running */
    snprintf(g_live.why, sizeof(g_live.why), "the trainer isn't running in this game (see the log)");
    const char *file = strrchr(g_path, '/');
    file = file ? file + 1 : g_path;
    char why[160] = "";
    size_t size = 0;
    uint8_t *buf = read_file(g_path, &size, why, sizeof(why));
    if (!buf) {
        nc_log(TAG, "refused %s: %s; the game starts without it", g_path, why);
        return entry;
    }
    NcTrainerInfo *const pi = &g_info;
    if (!nc_trainer_parse(buf, size, pi, why, sizeof(why))) {
        nc_log(TAG, "refused %s: %s; the game starts without it", file, why);
        free(buf);
        return entry;
    }
    snprintf(g_name, sizeof(g_name), "%s", pi->name[0] ? pi->name : file);
    nc_log(TAG, "file %s: \"%s\" by \"%s\", %u bytes, %u options, title IDs %08x %08x %08x, entry +%x", g_path,
           pi->name, pi->author, pi->size, pi->noptions, pi->title_ids[0], pi->title_ids[1], pi->title_ids[2],
           pi->entry);
    if (!nc_trainer_for_title(pi, xbe->title_id)) {
        nc_log(TAG, "refused %s: it is for another game (this XBE's title ID is %08x); the game starts without it",
               file, xbe->title_id);
        free(buf);
        return entry;
    }
    if (pi->ndates) {
        bool same = false;
        char list[NC_TRAINER_MAX_DATES * 9 + 1] = "";
        for (unsigned i = 0; i < pi->ndates; i++) {
            same |= pi->dates[i] == xbe->timedate;
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%08x", i ? "," : "", pi->dates[i]);
        }
        nc_log(TAG, "its code checks the XBE date (%s); this XBE's is %08x: %s", list, xbe->timedate,
               same ? "made for this release" : "made for another release: its own check decides (it may change nothing)");
    } else {
        nc_log(TAG, "no XBE date check found in its code (this XBE's date %08x)", xbe->timedate);
    }
    const uint32_t want = nc_align_up(STUB_AREA + pi->size + SLACK, NC_PAGE);
    const uint32_t block = nc_mm_contig_alloc(k, want);
    const uint32_t pa = block ? nc_vm_phys(vm, block) : UINT32_MAX;
    if (!block || pa == UINT32_MAX || pa + want > vm->ram_size) {
        nc_log(TAG, "refused %s: no %u KiB of guest memory for it; the game starts without it", file, want >> 10);
        free(buf);
        return entry;
    }
    /* patch 252: its code names the kernel (a thunk, the kernel's own range) or the old loaders' pointers
     * (0x8000000C/10/14, game memory here): run once, no recording (only the image and its block are copied) */
    uint32_t kat = 0, kval = 0;
    {
        unsigned nthunks = 0;
        while (nthunks < 512) {
            const uint8_t *t = host_at(xbe->kernel_thunk + 4 * nthunks);
            if (!t || !nc_rd32(t)) {
                break;
            }
            nthunks++;
        }
        for (uint32_t i = 0x23; i + 6 <= pi->size && !kat; i++) {
            const uint32_t at = abs_operand(buf + i);
            if (!at) {
                continue;
            }
            const uint32_t v = nc_rd32(buf + i + at);
            if ((nthunks && v >= xbe->kernel_thunk && v < xbe->kernel_thunk + 4 * nthunks) ||
                (v >= 0x80010000u && v < 0x80100000u) || (v >= 0x80000000u && v < 0x80001000u)) {
                kat = i;
                kval = v;
            }
        }
    }
    uint8_t *h = vm->phys + pa;
    memset(h, 0, want);
    const uint32_t base = block + STUB_AREA;
    memcpy(h + STUB_AREA, buf, pi->size);
    free(buf);
    /* the options: one byte each */
    unsigned on = 0;
    char list[600] = "";
    const size_t nbits = strlen(g_bits);
    for (unsigned i = 0; i < pi->noptions; i++) {
        const bool o = i < nbits && g_bits[i] == '1';
        h[STUB_AREA + pi->sel + i] = o ? 1 : 0;
        if (o) {
            on++;
            if (strlen(list) + strlen(pi->options[i]) + 4 < sizeof(list)) {
                snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", list[0] ? ", " : "", pi->options[i]);
            }
        }
    }
    if (nbits != pi->noptions) {
        nc_log(TAG, "the options line has %u flags for its %u options (the rest off)", (unsigned)nbits, pi->noptions);
    }
    /* the stub: the trainer, as the dashboards called it (patch 252: as many runs as PORT_REC says: the
     * chosen options, then each alone for the live switches), then the title */
    uint8_t *s = h;
    *s++ = 0x60;                                            /* pushad */
    uint8_t *const loop = s;
    *s++ = 0x66; *s++ = 0xBA; nc_wr16(s, (uint16_t)PORT_REC); s += 2;    /* loop: mov dx, PORT_REC */
    *s++ = 0xED;                                            /* in eax, dx: run again? */
    *s++ = 0x85; *s++ = 0xC0;                               /* test eax, eax */
    *s++ = 0x74; uint8_t *const jz = s++;                   /* jz done */
    *s++ = 0xBA; nc_wr32(s, base); s += 4;                  /* mov edx, base */
    *s++ = 0x8B; *s++ = 0xEA;                               /* mov ebp, edx */
    *s++ = 0xB9; nc_wr32(s, base + pi->entry); s += 4;     /* mov ecx, its entry */
    *s++ = 0xFF; *s++ = 0xD1;                               /* call ecx */
    *s++ = 0x66; *s++ = 0xBA; nc_wr16(s, (uint16_t)PORT_REC); s += 2;    /* mov dx, PORT_REC */
    *s++ = 0xEF;                                            /* out dx, eax: its return value */
    *s = 0xEB; s[1] = (uint8_t)(loop - (s + 2)); s += 2;    /* jmp loop */
    *jz = (uint8_t)(s - (jz + 1));
    *s++ = 0x66; *s++ = 0xBA; nc_wr16(s, (uint16_t)PORT_DONE); s += 2;   /* done: mov dx, PORT_DONE */
    *s++ = 0xEF;                                            /* out dx, eax */
    *s++ = 0x61;                                            /* popad */
    *s++ = 0x68; nc_wr32(s, entry); s += 4;                 /* push the title's entry */
    *s++ = 0xC3;                                            /* ret */
    /* patch 252: what the recording runs need */
    g_live.why[0] = 0;
    g_rec.step = STEP_START;
    g_rec.n = pi->noptions;
    g_rec.base = base;
    g_rec.optva = base + pi->sel;
    g_rec.image_va = xbe->base;
    g_rec.image_len = xbe->sizeof_image;
    g_rec.block_len = want;
    for (unsigned i = 0; i < pi->noptions; i++) {
        g_rec.chosen[i] = i < nbits && g_bits[i] == '1' ? '1' : '0';
    }
    g_rec.chosen[pi->noptions] = 0;
    memcpy(g_live.bits, g_rec.chosen, sizeof(g_live.bits));
    /* patch 255 (user): no live switches; a cheat's values stay after its code is put back, on 1000+
     * trainers that can't be caught one by one. Options apply when the game starts. */
    g_rec.record = false;
    live_no("options apply when the game starts");
    if (g_rec.record && !pi->noptions) {
        g_rec.record = false;
        live_no("it has no options");
    } else if (kat) {
        g_rec.record = false;
        live_no("its code names %08x (at +%x): %s; it runs once only", kval, kat,
                kval < 0x80001000u ? "the old loaders' pointers" : "the kernel");
    } else if (!xbe->sizeof_image || xbe->sizeof_image > LIVE_MAX_IMAGE) {
        g_rec.record = false;
        live_no("the game's image is too big to copy (%u KiB)", xbe->sizeof_image >> 10);
    }
    if (!g_rec.record) {
        nc_log(TAG, "live switches: off: %s; its options apply on restart", g_live.why);
    } else {
        nc_log(TAG, "live switches: recording each option's bytes (%u runs of its entry, image %08x+%x, block %08x+%x)",
               pi->noptions + 2, g_rec.image_va, g_rec.image_len, block, want);
    }
    if (nc_vm_dma.wrote) {
        nc_vm_dma.wrote(pa, want);
    }
    memset(g_smb, 0, sizeof(g_smb));
    g_smb_logged = 0;
    g_pending = true;
    nc_log(TAG, "options on: %u of %u%s%s", on, pi->noptions, on ? ": " : "", list);
    nc_log(TAG, "placed at %08x-%08x (base %08x, entry %08x, option bytes %08x); the boot thread runs it, then the "
           "title's entry %08x", block, block + want, base, base + pi->entry, base + pi->sel, entry);
    return block;
}

/* ---- ports ---- */

extern void rx_cpu_after_trainer(void) __attribute__((weak));

bool nc_trainer_port_out(uint16_t port, unsigned bytes, uint32_t value)
{
    (void)bytes;
    if (port == PORT_REC && g_pending) {     /* patch 252: a run's end */
        rec_done(value);
        return true;
    }
    if (port == PORT_DONE && g_pending) {
        g_pending = false;
        nc_log(TAG, "%s: its entry returned %08x; the game's code is patched, the title starts", g_name, g_rec.ret);
        if (rx_cpu_after_trainer) {
            rx_cpu_after_trainer();     /* our own code changes, on the patched code */
        }
        return true;
    }
    if (g_path[0] && port >= SMB_BASE && port < SMB_BASE + 16) {
        /* its SMBus writes (the LED as options go on and off): kept, and the first few logged */
        g_smb[port - SMB_BASE] = (uint8_t)value;
        if (port == SMB_BASE + 2 && (value & 0x08) && g_smb_logged < 16) {
            g_smb_logged++;
            nc_log(TAG, "SMBus write by the trainer: device %02x command %02x value %02x", g_smb[4], g_smb[8], g_smb[6]);
        }
        return true;
    }
    return false;
}

bool nc_trainer_port_in(uint16_t port, unsigned bytes, uint32_t *value)
{
    (void)bytes;
    if (port == PORT_REC && g_pending) {     /* patch 252: the stub asks for another run */
        *value = rec_next();
        return true;
    }
    if (g_path[0] && port >= SMB_BASE && port < SMB_BASE + 16) {
        /* the status says done (an unclaimed port reads all ones: busy, and a wait would never end) */
        *value = port == SMB_BASE ? 0x10u : g_smb[port - SMB_BASE];
        return true;
    }
    return false;
}
