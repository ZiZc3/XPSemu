/*
 * XOG: the PS5 title's entry. Reads what to boot from /data/xpsemu/boot.txt
 * (first line a disc image, later lines hakuX's options), runs the core on the
 * main thread, and shows frames and reads the DualSense on threads of their own.
 * Our own code. GPL-3.0-or-later.
 */
int sceKernelQueryMemoryProtection(void *addr, void **start, void **end, uint32_t *prot);
#include <dirent.h>
#include "xog_title.h"
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "native/core/core.h"
#include "native/core/options.h"
#include "native/dev/usb.h"
#include "native/hle/hle_native.h"
#include "native/kernel/kernel.h"
#include "xog_cpu.h"
#include "padmap_ps5.h"

#define RX_DIR "/data/xpsemu"
#define RX_LOG RX_DIR "/game.log"

struct ANativeWindow;
bool nc_present_init(void);
bool nc_present_frame(struct ANativeWindow *win);

int sceSystemServiceHideSplashScreen(void);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int scePadReadState(int32_t handle, void *data);

typedef struct RxPadData {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry; /* 0-255, 128 centred, down and right larger */
    uint8_t l2, r2;
    uint8_t pad[2];
    uint8_t rest[256];
} RxPadData;

static NcCore g_core;
extern uint64_t rx_frames_shown;
void rx_signals_install(void);
static int g_log_fd = -1;

extern void rx_log_drain(void);
extern void rx_stubs_leave(void);
extern void rx_log_start(void);

void rx_log_write(const char *text, size_t len)
{
    if (g_log_fd >= 0) {
        (void)!write(g_log_fd, text, len);
    }
}

/* For a report at the end: what is queued goes first, so it stays in order. */
void rx_log_raw(const char *text, size_t len)
{
    rx_log_drain();
    rx_log_write(text, len);
}

/* What the console left unbound: a call there jumps to 0. Names them all at once. */
static void report_imports(void)
{
    FILE *f = fopen("/data/homebrew/" XOG_TITLE "/imports.txt", "r");
    if (!f) {
        nc_log("xog", "imports.txt not found");
        return;
    }
    char name[256];
    unsigned long long off;
    unsigned total = 0, empty = 0;
    while (fscanf(f, "%llx %255s", &off, name) == 2) {
        total++;
        /* the title's link address plus the console's load offset, seen in the probes */
        const uint64_t *slot = (const uint64_t *)(uintptr_t)(off + 0x400000ull);
        /* a file from another build names addresses that aren't there */
        void *rs = 0, *re = 0;
        uint32_t rp = 0;
        if (sceKernelQueryMemoryProtection((void *)slot, &rs, &re, &rp) != 0 || (uintptr_t)rs > (uintptr_t)slot ||
            (uintptr_t)re <= (uintptr_t)slot || !(rp & 1)) {
            nc_log("xog", "imports.txt isn't this build's: not checked");
            break;
        }
        if (*slot == 0) {
            empty++;
            nc_log("xog", "import with no provider: %s", name);
        }
    }
    fclose(f);
    nc_log("xog", "imports: %u, unbound %u", total, empty);
}

extern void rx_log_drain(void);
extern void rx_stubs_leave(void);

/* How long ago a write means the game may still be saving: since just before a pause began. */
static uint64_t g_xog_hdd_quiet_window(void)
{
    extern volatile uint64_t g_qm_paused_ms;
    extern uint64_t rx_now_ms(void);
    return g_qm_paused_ms ? rx_now_ms() - g_qm_paused_ms + 1500 : 1500;
}

int g_xog_vblank_factor = 1;

/* What the mod on changes, for the patches to keep off (rx_title_patches). */
#define RX_MOD_RANGES 8192
static struct {
    uint32_t va, len;
} g_mod_ranges[RX_MOD_RANGES];
static unsigned g_nmod_ranges;

bool rx_mod_overlaps(uint32_t va, uint32_t len)
{
    for (unsigned i = 0; i < g_nmod_ranges; i++) {
        if (va < g_mod_ranges[i].va + g_mod_ranges[i].len && g_mod_ranges[i].va < va + len) {
            return true;
        }
    }
    return false;
}

/* "0xVA: old -> new" or "0xVA: new": the address and how many bytes. */
static void rx_mod_range(const char *t)
{
    char *colon;
    const uint32_t va = (uint32_t)strtoul(t, &colon, 0);
    if (*colon != ':' || g_nmod_ranges == RX_MOD_RANGES) {
        return;
    }
    const char *arrow = strstr(colon, "->"), *from = arrow ? arrow + 2 : colon + 1;
    uint32_t len = 0;
    for (const char *c = from; *c && *c != '#';) {
        if (*c == ' ' || *c == '\t') {
            c++;
        } else {
            len++;
            c += c[1] ? 2 : 1;
        }
    }
    g_mod_ranges[g_nmod_ranges].va = va;
    g_mod_ranges[g_nmod_ranges++].len = len;
}

/* A mod's file into the core's manifest: its patches, a line each under the XBE they're for, and its refresh. */
static void rx_mod_manifest(const char *path)
{
    extern void nc_mods_set_manifest(const char *path);
    static const char out_path[] = RX_DIR "/mods-run.txt";
    FILE *in = fopen(path, "r"), *out = in ? fopen(out_path, "w") : NULL;
    if (!out) {
        nc_log("xog", "mod %s: can't be read", path);
        if (in) {
            fclose(in);
        }
        return;
    }
    char id[64] = "mod", xbe[256] = "", line[1024];
    const char *e = strrchr(path, '/');
    if (e && e > path) {
        const char *d = e - 1;
        while (d > path && *d != '/') {
            d--;
        }
        snprintf(id, sizeof(id), "%.*s", (int)(e - d - 1), d + 1);
    }
    unsigned n = 0;
    while (fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t = line;
        while (*t == ' ' || *t == '\t') {
            t++;
        }
        if (!*t || *t == '#') {
            continue;
        }
        if (*t == '[') {
            char *c = strchr(t, ']');
            if (c) {
                *c = 0;
                snprintf(xbe, sizeof(xbe), "%s", t + 1);
            }
        } else if (xbe[0]) {
            fprintf(out, "patch\t%s\t%s\t%s\n", xbe, id, t);
            rx_mod_range(t);
            n++;
        } else if (!strncmp(t, "refresh:", 8)) {
            fprintf(out, "refresh\t%d\n", atoi(t + 8));
        }
    }
    fclose(in);
    fclose(out);
    nc_mods_set_manifest(out_path);
    if (strstr(path, "/120fps/")) {
        extern int g_xog_want_120hz;
        g_xog_want_120hz = 1;
        nc_log("xog", "120 Hz output asked for this game");
    }
    nc_log("xog", "mod %s on: %u patch lines (%s)", id, n, path);
}

/* A restart ends the process: the guest stopped and the GPU done first. */
static bool g_core_started;     /* before that there is no guest or GPU to wait for */
static void rx_quiesce(void)
{
    if (!g_core_started) {
        return;
    }
    extern int hle_vk_exit_idle(void);
    extern void nc_pause_set(bool on);
    extern volatile uint64_t g_xog_hdd_write_ms;
    extern uint64_t rx_now_ms(void);
    /* a game in the middle of saving finishes first: up to 3 s, until its hard disk is quiet */
    if (g_xog_hdd_write_ms && rx_now_ms() - g_xog_hdd_write_ms < g_xog_hdd_quiet_window()) {
        nc_pause_set(false);
        const uint64_t until = rx_now_ms() + 3000;
        while (rx_now_ms() < until && rx_now_ms() - g_xog_hdd_write_ms < 500) {
            usleep(20000);
        }
        nc_log("xog", "leaving: the hard disk quiet (a game was writing)");
    }
    nc_pause_set(true);
    usleep(100000);     /* the guest at its next kernel entry; the decoder through what it has */
    const int r = hle_vk_exit_idle();
    nc_log("xog", "leaving: the GPU's work done (%d)", r);
}

/* The game is over: the dashboard again, with why if there's anything to say. */
static void back_to_dashboard(const char *why)
{
    int sceSystemServiceLoadExec(const char *path, char *const argv[]);
    FILE *f = fopen(RX_DIR "/from-game", "w");
    if (f) {
        fclose(f);
    }
    if (why) {
        f = fopen(RX_DIR "/game-note.txt", "w");
        if (f) {
            fprintf(f, "%s\n", why);
            fclose(f);
        }
    }
    rx_quiesce();
    rx_log_drain();
    rx_stubs_leave();
    sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
    sceSystemServiceLoadExec("/data/homebrew/" XOG_TITLE "/eboot.bin", NULL);
    nc_log("xog", "couldn't restart into the dashboard");
    rx_log_drain();
}

static void *present_thread(void *arg)
{
    (void)arg;
    static char window_token;
    bool hidden = false;
    nc_present_init();
    for (;;) {
        if (nc_present_frame((struct ANativeWindow *)&window_token)) {
            rx_frames_shown++;
            if (!hidden) {
                hidden = true;
                const int r = sceSystemServiceHideSplashScreen();
                nc_log("xog", "first frame presented, splash off: %08x", (unsigned)r);
            }
        } else {
            usleep(2000);
        }
    }
    return NULL;
}

static int16_t stick(uint8_t v, bool invert)
{
    int s = ((int)v - 128) * 258;
    s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
    return (int16_t)(invert ? (s == -32768 ? 32767 : -s) : s);
}

extern volatile int g_qm_open, g_qm_stick_x, g_qm_stick_y;
extern volatile uint32_t g_qm_buttons;
extern void xog_qm_request(void);
extern void xog_qm_start_line(const char *line);

void rx_back_to_dashboard(const char *why)
{
    back_to_dashboard(why);
}

int rx_game_is_pal(void)
{
    return g_core.kernel.game_region == 4;
}

void rx_restart_game(void)
{
    int sceSystemServiceLoadExec(const char *path, char *const argv[]);
    rx_quiesce();
    rx_log_drain();
    rx_stubs_leave();
    sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
    sceSystemServiceLoadExec("/data/homebrew/" XOG_TITLE "/eboot.bin", NULL);
    nc_log("xog", "couldn't restart the game");
    rx_log_drain();
}

/* Up to 4 players: player 1 is who started the title, the others each signed-in user, looked for
 * every 2 s, plugged in and out as on the console. Only player 1 works the quick menu. */
typedef struct RxPlayer {
    int32_t user;
    int handle;
} RxPlayer;

static void players_scan(RxPlayer *pl, unsigned *mask)
{
    int sceUserServiceGetLoginUserIdList(void *list);
    int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
    int32_t list[4];
    memset(list, 0xFF, sizeof(list));
    if (sceUserServiceGetLoginUserIdList(list) != 0) {
        return;
    }
    unsigned now = 1;
    for (int p = 1; p < 4; p++) {
        bool in = false;
        for (int k = 0; k < 4; k++) {
            in = in || (pl[p].user >= 0 && list[k] == pl[p].user);
        }
        if (pl[p].user >= 0 && !in) {
            nc_log("xog", "player %d signed out", p + 1);
            pl[p].user = -1;
            pl[p].handle = -1;
        }
    }
    for (int k = 0; k < 4; k++) {
        const int32_t u = list[k];
        bool known = u < 0;
        for (int p = 0; p < 4; p++) {
            known = known || pl[p].user == u;
        }
        for (int p = 1; p < 4 && !known; p++) {
            if (pl[p].user < 0) {
                pl[p].user = u;
                pl[p].handle = scePadOpen(u, 0, 0, NULL);
                if (pl[p].handle < 0) {
                    pl[p].handle = scePadGetHandle(u, 0, 0);
                }
                nc_log("xog", "player %d: user %d, handle %d", p + 1, u, pl[p].handle);
                known = true;
            }
        }
    }
    for (int p = 1; p < 4; p++) {
        if (pl[p].user >= 0) {
            now |= 1u << p;
        }
    }
    if (now != *mask) {
        extern void nc_dev_ohci_set_pads(unsigned mask);
        nc_dev_ohci_set_pads(now);
        *mask = now;
    }
}

static void *pad_thread(void *arg)
{
    (void)arg;
    sceUserServiceInitialize(NULL);
    scePadInit();
    RxPlayer pl[4] = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};
    sceUserServiceGetInitialUser(&pl[0].user);
    pl[0].handle = scePadOpen(pl[0].user, 0, 0, NULL);
    nc_log("xog", "DualSense: user %d, handle %d", pl[0].user, pl[0].handle);
    if (pl[0].handle < 0) {
        return NULL;
    }
    RxPadData d;
    bool touch = false, mute = false;
    unsigned mask = 1, turns = 0;
    xemu_ps5_map_load();
    for (;;) {
        if (turns++ % 500 == 0) {
            players_scan(pl, &mask);
        }
        for (int p = 0; p < 4; p++) {
            if (pl[p].handle < 0) {
                continue;
            }
            memset(&d, 0, sizeof(d));
            if (scePadReadState(pl[p].handle, &d) < 0) {
                continue;
            }
            const uint32_t b = d.buttons;
            NcPadState s;
            memset(&s, 0, sizeof(s));
            if (p == 0) {
                /* the touchpad: the quick menu. Open, the game sees controllers at rest; after it,
                 * until everything is let go, so that the press that closed it doesn't reach the game */
                if ((b & 0x00100000u) && !touch && !g_qm_open) {
                    xog_qm_request();
                }
                touch = (b & 0x00100000u) != 0;
                if (g_qm_open || mute) {
                    g_qm_buttons = b;
                    g_qm_stick_x = (int)d.lx - 128;
                    g_qm_stick_y = (int)d.ly - 128;
                    mute = g_qm_open || (b != 0);
                }
            }
            if (g_qm_open || mute) {
                nc_pad_set((unsigned)p, &s);
                continue;
            }
#define RX_LEVEL(x) xemu_ps5_xbox_level((x), b, d.l2, d.r2)
#define RX_PRESSED(x) (RX_LEVEL(x) > 127)
            if (RX_PRESSED(XB_UP)) s.buttons |= NC_PAD_DPAD_UP;
            if (RX_PRESSED(XB_DOWN)) s.buttons |= NC_PAD_DPAD_DOWN;
            if (RX_PRESSED(XB_LEFT)) s.buttons |= NC_PAD_DPAD_LEFT;
            if (RX_PRESSED(XB_RIGHT)) s.buttons |= NC_PAD_DPAD_RIGHT;
            if (RX_PRESSED(XB_START)) s.buttons |= NC_PAD_START;
            if (RX_PRESSED(XB_BACK)) s.buttons |= NC_PAD_BACK;
            if (RX_PRESSED(XB_LSTICK)) s.buttons |= NC_PAD_LTHUMB;
            if (RX_PRESSED(XB_RSTICK)) s.buttons |= NC_PAD_RTHUMB;
            s.analog[0] = (uint8_t)RX_LEVEL(XB_A);
            s.analog[1] = (uint8_t)RX_LEVEL(XB_B);
            s.analog[2] = (uint8_t)RX_LEVEL(XB_X);
            s.analog[3] = (uint8_t)RX_LEVEL(XB_Y);
            s.analog[4] = (uint8_t)RX_LEVEL(XB_BLACK);
            s.analog[5] = (uint8_t)RX_LEVEL(XB_WHITE);
            s.analog[6] = (uint8_t)RX_LEVEL(XB_LT);
            s.analog[7] = (uint8_t)RX_LEVEL(XB_RT);
            s.lx = stick(d.lx, false);
            s.ly = stick(d.ly, true);
            s.rx = stick(d.rx, false);
            s.ry = stick(d.ry, true);
            nc_pad_set((unsigned)p, &s);
        }
        usleep(4000);
    }
    return NULL;
}

/* boot.txt's first line, or the first image in XPSemu's games folder. */
static bool pick_disc(char *path, size_t n, NcOptions *opt)
{
    /* the dashboard's start: read once and gone, so a crash comes back to the dashboard */
    FILE *f = fopen(RX_DIR "/launch.txt", "r");
    if (f) {
        unlink(RX_DIR "/launch.txt");
    } else if (access(RX_DIR "/nodash", F_OK) == 0) {
        f = fopen(RX_DIR "/boot.txt", "r");
    }
    path[0] = 0;
    if (f) {
        char line[4096];
        bool first = true;
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) {
                continue;
            }
            extern void xog_qm_start_line(const char *line);
            xog_qm_start_line(line);
            if (!strncmp(line, "key=", 4) || !strncmp(line, "name=", 5)) {
                continue;
            }
            if (!strncmp(line, "modavail=", 9)) {
                continue;
            }
            if (!strncmp(line, "mod=", 4)) {
                rx_mod_manifest(line + 4);
                continue;
            }
            if (!strcmp(line, "unlock60")) {
                g_xog_vblank_factor = 2;        /* the game's second vblank is 60 a second */
                nc_log("xog", "60 FPS patch on");
                continue;
            }
            if (first) {
                snprintf(path, n, "%s", line);
                first = false;
            } else if (!strncmp(line, "poke=", 5)) {
                extern void rx_poke_add(const char *text);
                rx_poke_add(line + 5);
            } else if (!strncmp(line, "aspect=", 7)) {
                extern void nc_present_set_aspect(int mode);
                const char *v = line + 7;
                nc_present_set_aspect(!strcmp(v, "16:9") ? 2 : !strcmp(v, "4:3") ? 1 : !strcmp(v, "stretch") ? 3 : 0);
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "volume=", 7)) {
                extern void rx_audio_volume(int tens);
                rx_audio_volume(atoi(line + 7));
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "fps=", 4)) {
                nc_log("xog", "option: %s", line);     /* the overlay's (quick_menu.cc) */
            } else if (!strncmp(line, "sides=", 6)) {
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "framegen=", 9)) {
                extern void hle_present_set_framegen(bool on);
                extern void hle_present_set_framegen_mode(unsigned most);
                extern int g_xog_want_120hz;
                hle_present_set_framegen(true);
                hle_present_set_framegen_mode((unsigned)atoi(line + 9));    /* auto: 0 */
                g_xog_want_120hz = 1;       /* a 60 fps game has room for a picture only at 120 Hz */
                nc_log("xog", "option: frame generation on");
            } else if (!strncmp(line, "xogfix=", 7)) {
                setenv("XOG_FIX", line + 7, 1);     /* this game's own fixes */
                nc_log("xog", "option: %s", line);
            } else if (!strcmp(line, "apudsp")) {
                setenv("NC_APU_DSP", "1", 1);   /* read when the sound chip is made */
                nc_log("xog", "option: full sound processor");
            } else if (!strncmp(line, "fpscap=", 7)) {
                extern void hle_pb_set_fps_cap(int fps);
                hle_pb_set_fps_cap(atoi(line + 7));
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "aa=", 3)) {
                extern void nc_present_set_fxaa(bool on);
                extern void nc_present_set_smaa(bool on);
                nc_present_set_fxaa(!strcmp(line + 3, "fxaa"));
                nc_present_set_smaa(!strcmp(line + 3, "smaa"));
                nc_log("xog", "option: %s", line);
            } else if (!strcmp(line, "upscale=sgsr") || !strcmp(line, "upscale=bilinear")) {
                extern void nc_present_set_upscale(int mode);
                nc_present_set_upscale(line[8] == 's');     /* app.h's NC_UPSCALE_* */
                nc_log("xog", "option: %s", line);
            } else if (nc_option(opt, line)) {
                nc_log("xog", "option: %s", line);
            } else {
                nc_log("xog", "unknown option: %s", line);
            }
        }
        fclose(f);
    }
    if (path[0]) {
        return true;
    }
    if (access(RX_DIR "/nodash", F_OK) != 0) {
        return false;   /* the dashboard chooses */
    }
    static const char dir[] = "/data/xemu/games";
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        const size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".iso")) {
            snprintf(path, n, "%s/%s", dir, e->d_name);
            break;
        }
    }
    closedir(d);
    return path[0] != 0;
}

int main(void)
{
    /* XPSemu's folder was /data/xog in the test builds: moved once, with the
     * hard disk, saves, states and settings in it (patch 169) */
    {
        struct stat st;
        if (stat(RX_DIR, &st) != 0 && stat("/data/xog", &st) == 0) {
            rename("/data/xog", RX_DIR);
        }
    }
    mkdir(RX_DIR, 0777);
    mkdir(RX_DIR "/hdd", 0777);
    /* the dashboard's run has its own log: it mustn't push the game's out */
    const bool dashboard = access(RX_DIR "/launch.txt", F_OK) != 0 && access(RX_DIR "/nodash", F_OK) != 0;
    {
        FILE *st = fopen(RX_DIR "/start-trace.txt", "a");
        if (st) {
            fprintf(st, "%ld %s\n", (long)time(NULL), dashboard ? "dashboard" : "game");
            fclose(st);
        }
    }
    const char *const log_file = dashboard ? RX_DIR "/ui.log" : RX_LOG;
    if (!dashboard) {
        /* every game's log stays, under when it was written */
        struct stat old;
        if (stat(RX_DIR "/game.previous.log", &old) == 0) {
            char kept[128];
            struct tm tm;
            const time_t when = old.st_mtime;
            gmtime_r(&when, &tm);
            mkdir(RX_DIR "/logs", 0777);
            snprintf(kept, sizeof(kept), RX_DIR "/logs/%04d%02d%02d-%02d%02d%02d.log", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
            rename(RX_DIR "/game.previous.log", kept);
        }
    }
    rename(log_file, dashboard ? RX_DIR "/ui.previous.log" : RX_DIR "/game.previous.log");
    g_log_fd = open(log_file, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    freopen(log_file, "a", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);
    nc_log("xog", "XOG starting; main at %p", (void *)main);
    rx_signals_install();

    report_imports();
    mkdir(RX_DIR "/hle", 0777);
    mkdir(RX_DIR "/hle/mesa", 0777);
    rx_log_start();
    setenv("MESA_SHADER_CACHE_DIR", RX_DIR "/hle/mesa", 1);
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "1G", 1);
    setenv("HAKUX_NATIVE_BASE", RX_DIR "/hle", 1);
    setenv("HAKUX_HLE_MODE", "replace", 0);
    if (dashboard) {
        /* no game asked for: the dashboard, and nothing of the emulator running */
        extern void xog_ui_entry(void);
        xog_ui_entry();
        rx_log_drain();
        return 0;
    }
    {   /* a test's debug switches: no file, nothing */
        FILE *dbg = fopen(RX_DIR "/debug.txt", "r");
        char line[256];
        while (dbg && fgets(line, sizeof(line), dbg)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (eq && eq != line) {
                *eq = 0;
                setenv(line, eq + 1, 1);
                nc_log("xog", "debug: %s=%s", line, eq + 1);
            }
        }
        if (dbg) {
            fclose(dbg);
        }
    }
    {
        extern void xog_prof_start(void);
        xog_prof_start();
    }
    NcOptions opt;
    nc_options_init(&opt);
    {
        /* what hakuX's app passes on every run; boot.txt's lines still override them */
        static const char *const defaults[] = {"devtime=1", "avpack=hdtv", "video=480000"};
        for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
            nc_option(&opt, defaults[i]);
        }
    }
    char disc[512];
    if (!pick_disc(disc, sizeof(disc), &opt)) {
        nc_log("xog", "no disc asked for");
        back_to_dashboard(NULL);
        return 0;
    }
    nc_options_apply(&opt);
    if (access(RX_DIR "/vblank2", F_OK) == 0) {
        extern void nc_dev_nv2a_vblank_multiply(unsigned n);
        nc_dev_nv2a_vblank_multiply(2);
        nc_log("xog", "vblank at twice the video mode's rate (%s/vblank2)", RX_DIR);
    }
    nc_io_set_hdd_root(RX_DIR "/hdd");
    nc_log("xog", "disc %s", disc);
    struct stat disc_st;
    const bool folder = stat(disc, &disc_st) == 0 && S_ISDIR(disc_st.st_mode);
    extern void nc_core_set_folder(const char *path);
    extern int rx_cci_open(const char *path);
    extern bool rx_cci_name(const char *path);
    if (folder) {
        nc_core_set_folder(disc);
    }
    const bool no_disc = !strcmp(disc, "none");    /* the Xbox's own dashboard */
    const int fd = folder || no_disc ? -1 : rx_cci_name(disc) ? rx_cci_open(disc) : open(disc, O_RDONLY);
    if (fd < 0 && !folder && !no_disc) {
        nc_log("xog", "can't open it: %s", strerror(errno));
        back_to_dashboard("Couldn't open the game's file");
        return 0;
    }
    if (!nc_core_boot(&g_core, fd)) {
        nc_log("xog", "boot failed%s%s", nc_hle_fatal() ? ": " : "", nc_hle_fatal() ? nc_hle_fatal() : "");
        const char *why = nc_hle_fatal();
        back_to_dashboard(why && strstr(why, "cache file") ? "This save state is from before the game's cache changed: start the game and save a new one"
                          : why && strstr(why, "savestate") ? "The save state couldn't be loaded"
                                                            : "The game couldn't start");
        return 0;
    }
    g_core_started = true;
    if (getenv("XOG_VBLANK_X2")) {
        g_xog_vblank_factor = 2;        /* debug: the 60 unlock on any game (patch 139) */
        nc_log("xog", "debug: vblanks twice as often");
    }
    if (g_xog_vblank_factor > 1) {
        extern void nc_dev_nv2a_vblank_multiply(unsigned n);
        nc_dev_nv2a_vblank_multiply((unsigned)g_xog_vblank_factor);
    }
    pthread_t t;
    pthread_create(&t, NULL, present_thread, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, pad_thread, NULL);
    pthread_detach(t);
    nc_core_run(&g_core);
    nc_log("xog", "the guest stopped: %s", g_core.kernel.stop ? g_core.kernel.stop_reason : "it asked to");
    if (g_core.kernel.relaunch) {
        /* another XBE of the title: it starts again into that one; only if that fails, the dashboard */
        extern void xog_qm_write_relaunch(void);
        extern void rx_dump_stop(void);
        rx_dump_stop();
        xog_qm_write_relaunch();
        rx_restart_game();
        back_to_dashboard("The game couldn't start its next part");
        return 0;
    }
    {
        extern void rx_log_drain(void);
extern void rx_stubs_leave(void);
        extern void rx_dump_stop(void);
        int sceSystemServiceLoadExec(const char *path, char *const argv[]);
        rx_dump_stop();
        back_to_dashboard("The game stopped");
        sceSystemServiceLoadExec("exit", NULL);   /* the system's way out, no crash report */
    }
    return 0;
}
