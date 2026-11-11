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
#include <math.h>

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
/* patch 227: a USB/Bluetooth mouse for the Chihiro gun games (the fork's PS5 code: libSceMouse loaded by name,
 * opened for player 1's user in merged mode, its samples summed) */
typedef struct RxMouseOpenParam {
    uint8_t behavior, reserved[7];
} RxMouseOpenParam;
typedef struct RxMouseData {
    uint64_t timestamp;
    bool connected;
    uint32_t buttons;
    int32_t x, y, wheel, tilt;
    uint8_t reserved[8];
} RxMouseData;
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, uint32_t flags, const void *option, int *result);
int sceKernelDlsym(int handle, const char *symbol, void **address);
static int (*rx_mouse_init)(void);
static int (*rx_mouse_open)(int32_t, int32_t, int32_t, const RxMouseOpenParam *);
static int (*rx_mouse_read)(int32_t, RxMouseData *, int32_t);
static int32_t rx_mouse_handle = -1;
extern void nc_chihiro_mouse(int dx, int dy, uint32_t buttons);
extern volatile int g_qm_open;

static void rx_mouse_poll(int32_t user)
{
    static bool tried;
    static double next_open;
    if (!tried) {
        tried = true;
        int status = 0, mod = -1;
        static const char *const paths[] = {"libSceMouse.sprx", "/system/common/lib/libSceMouse.sprx"};
        for (int i = 0; i < 2 && mod < 0; i++) {
            mod = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &status);
        }
        if (mod >= 0) {
            void *a = NULL, *b = NULL, *c = NULL;
            if (sceKernelDlsym(mod, "sceMouseInit", &a) != 0) {
                sceKernelDlsym(mod, "Qs0wWulgl7U", &a);
            }
            if (sceKernelDlsym(mod, "sceMouseOpen", &b) != 0) {
                sceKernelDlsym(mod, "RaqxZIf6DvE", &b);
            }
            if (sceKernelDlsym(mod, "sceMouseRead", &c) != 0) {
                sceKernelDlsym(mod, "x8qnXqh-tiM", &c);
            }
            rx_mouse_init = (int (*)(void))a;
            rx_mouse_open = (int (*)(int32_t, int32_t, int32_t, const RxMouseOpenParam *))b;
            rx_mouse_read = (int (*)(int32_t, RxMouseData *, int32_t))c;
            if (rx_mouse_init) {
                rx_mouse_init();
            }
        }
        nc_log("xog", "mouse: module %d, functions %s", mod, rx_mouse_read && rx_mouse_open ? "found" : "missing");
    }
    if (!rx_mouse_read || !rx_mouse_open || user < 0) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const double now = ts.tv_sec + ts.tv_nsec / 1e9;
    if (rx_mouse_handle < 0) {
        if (now < next_open) {
            return;
        }
        next_open = now + 2.0;
        const RxMouseOpenParam merged = {1, {0}};
        int h = rx_mouse_open(user, 0, 0, &merged);
        if (h < 0) {
            h = rx_mouse_open(user, 0, 0, NULL);
        }
        if (h >= 0) {
            rx_mouse_handle = h;
            nc_log("xog", "mouse: opened (handle %d)", h);
        }
        return;
    }
    RxMouseData d[32];
    const int n = rx_mouse_read(rx_mouse_handle, d, 32);
    if (n <= 0) {
        return;
    }
    int dx = 0, dy = 0;
    uint32_t buttons = 0;
    for (int i = 0; i < n; i++) {
        dx += d[i].x;
        dy += d[i].y;
        buttons = d[i].buttons;
    }
    nc_chihiro_mouse(dx, dy, g_qm_open ? 0 : buttons);
}

/* patch 219: a Chihiro image's files on the core's disc, read in place */
#include "chihiro_ps5.h"
#include "native/core/xdvdfs.h"
extern unsigned (*nc_chihiro_mount_hook)(const char *img, NcDisc *disc);
static void rx_chihiro_add(void *ctx, const char *path, uint32_t size, unsigned id)
{
    nc_disc_overlay_virtual((NcDisc *)ctx, path, size, id);
}
static unsigned rx_chihiro_mount_hook(const char *img, NcDisc *disc)
{
    nc_disc_vread_hook = rx_chihiro_mount_read;
    return rx_chihiro_mount(img, rx_chihiro_add, disc);
}
__attribute__((constructor)) static void rx_chihiro_hooks(void)
{
    nc_chihiro_mount_hook = rx_chihiro_mount_hook;
}
/* patch 209: the DualSense's motors (compatible mode 2, as PS5_RetroArch and the fork), 0-255 each */
typedef struct RxPadVibration {
    uint8_t large_motor, small_motor;
} RxPadVibration;
int scePadSetVibration(int32_t handle, const RxPadVibration *param);
int scePadSetVibrationMode(int32_t handle, int32_t mode);

/* hakuX's rumble path: the core's level for a player (nc_pad_rumble: the Xbox pad's motors, or Chihiro's drive
 * board), sent to that player's controller when it changes and every second while on. Patch 234: every player,
 * every game, unless Settings' Vibration is off (g_rx_vibration, quick_menu.cc). */
extern volatile int g_rx_vibration;
/* HD haptics (haptics_ps5.c): the same levels on the DualSense's voice coils, else false */
int xog_haptics_rumble(unsigned player, int32_t user, int32_t handle, uint16_t large, uint16_t small);
void xog_haptics_stop_all(void);
static void rx_rumble(unsigned player, int32_t user, int32_t handle, bool quiet)
{
    static int32_t mode_handles[4] = {-1, -1, -1, -1};
    static RxPadVibration lasts[4];
    static uint64_t last_mss[4];
    static unsigned sends, fails;
    if (player >= 4) {
        return;
    }
    int32_t *const mode_handle = &mode_handles[player];
    RxPadVibration *const lastp = &lasts[player];
    uint64_t *const last_msp = &last_mss[player];
#define last (*lastp)
#define last_ms (*last_msp)
    uint16_t l = 0, r = 0;
    if (!quiet) {
        nc_pad_rumble(player, &l, &r);
    }
    if (xog_haptics_rumble(player, user, handle, l, r)) {
        *mode_handle = -1;      /* should HD haptics stop, the motors' mode is set again */
        return;
    }
    if (*mode_handle != handle) {
        const int rc = scePadSetVibrationMode(handle, 2);
        *mode_handle = handle;
        memset(&last, 0, sizeof(last));
        last_ms = 0;
        nc_log("xog", "rumble: vibration mode 2 on controller %u: %d", player + 1, rc);
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    RxPadVibration v = {(uint8_t)(l >> 8), (uint8_t)(r >> 8)};
    const bool changed = v.large_motor != last.large_motor || v.small_motor != last.small_motor;
    if (!changed && !((v.large_motor || v.small_motor) && now - last_ms >= 1000)) {
        return;
    }
    const int rc = scePadSetVibration(handle, &v);
    sends++;
    if (rc != 0 && fails++ < 5) {
        nc_log("xog", "rumble: scePadSetVibration %d (sent %u)", rc, sends);
    }
    last = v;
    last_ms = now;
#undef last
#undef last_ms
}

typedef struct RxPadData {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry; /* 0-255, 128 centred, down and right larger */
    uint8_t l2, r2;
    uint8_t pad[2];
    uint8_t rest[256];      /* ScePadData from 12: orientation (4 floats), acceleration (3), angular velocity (3,
                             * rad/s, at rest + 28), then the touch data (rest + 40) */
} RxPadData;

/* Patch 266: the DualSense's extras. Adaptive triggers (L2/R2 resistance by kind of game), gyro aim (added to the
 * right stick, per game) and the light bar in the player's colour. libScePad's functions for them are looked up by
 * name, else by NID, in the loaded module (as the mouse's above): none is an import of the title, so one the
 * console's firmware lacks only turns its feature off (logged), never a call to 0. */
typedef struct RxTriggerCommand {
    uint32_t mode;          /* 0 off, 1 feedback, 2 weapon, 3 vibration, 4 multi-position feedback, 5 slope feedback */
    uint8_t reserved[4];
    uint8_t data[48];       /* the mode's parameters */
} RxTriggerCommand;
typedef struct RxTriggerParam {
    uint8_t mask;           /* 1 L2, 2 R2 */
    uint8_t reserved[7];
    RxTriggerCommand command[2];    /* [0] L2, [1] R2 */
} RxTriggerParam;
_Static_assert(sizeof(RxTriggerParam) == 120, "ScePadTriggerEffectParam is 120 bytes");
typedef struct RxLightBar {
    uint8_t r, g, b, reserved;
} RxLightBar;
static int (*rx_pad_trigger_fn)(int32_t, const RxTriggerParam *);
static int (*rx_pad_lightbar_fn)(int32_t, const RxLightBar *);
static int (*rx_pad_lightbar_reset_fn)(int32_t);
static int (*rx_pad_motion_fn)(int32_t, bool);
static volatile int g_rx_trigger_kind;  /* the start line "triggers=": 0 off, 1 driving, 2 shooter */
/* The quick menu: this game's own choice changed there (it saves it to the game's settings too); the pad
 * thread puts it on the triggers on its next turn, once the menu has closed. */
void xog_set_turbo(int mask)     /* patch 324: the quick menu's Turbo buttons row, at once */
{
    extern int g_rx_turbo_live;
    g_rx_turbo_live = mask & 63;
}

void xog_set_msaa(int samples)    /* patch 326: the quick menu's MSAA row, at once (vk/surf.c applies it between passes) */
{
    extern void hle_vk_msaa_request(uint32_t samples);
    hle_vk_msaa_request(samples < 2 ? 1u : (uint32_t)samples);
}

void xog_set_a2c(int on)          /* patch 354: the quick menu's MSAA smooth cut-outs row, at once (the next draws) */
{
    extern void hle_vk_set_a2c(int on);
    hle_vk_set_a2c(on);
}

/* patch 366: ambient occlusion's level now (0 off) and the last one chosen; L3 + R3's compare removed (patch 368) */
static int g_ssao_now, g_ssao_pick = 2;

void xog_set_ssao(int level)      /* patch 363: the quick menu's ambient occlusion row, at once (the next frame) */
{
    extern void hle_vk_set_ssao(int level);
    hle_vk_set_ssao(level);
    g_ssao_now = level;
    if (level > 0) {
        g_ssao_pick = level;
    }
}

void xog_set_trigger_kind(int kind)
{
    g_rx_trigger_kind = kind < 0 || kind > 2 ? 0 : kind;
}
static int g_rx_gyro;                   /* "gyro=": 0 off, 1 low, 2 medium, 3 high */
int g_rx_turbo_live = -1;                /* patch 324: the quick menu's choice (-1: the start line's) */
static int g_rx_turbo;                  /* patch 317: "turbo=": the Xbox buttons held = pressed again and again (1 A, 2 B, 4 X, 8 Y, 16 RT, 32 LT) */
static XbPadMap g_rx_padmap;            /* patch 269: "padmap=": the game's own button mapping (Xbox games only) */
static bool g_rx_padmap_on;
static volatile int g_rx_pad_leaving;   /* back to the dashboard: nothing of the game's sent any more */
static int32_t g_rx_pad_handles[4] = {-1, -1, -1, -1};
extern volatile int g_rx_triggers;      /* Settings' Adaptive triggers (quick_menu.cc) */

/* patch 267: the console's libScePad exports these (the SDK's stub lists the firmware's exports); looked up by name
 * through sceKernelDlsym, none was found on the console (patch 266's test): now ordinary imports, like
 * scePadSetVibration. */
int scePadSetTriggerEffect(int32_t handle, const RxTriggerParam *param);
int scePadSetLightBar(int32_t handle, const RxLightBar *colour);
int scePadResetLightBar(int32_t handle);
int scePadSetMotionSensorState(int32_t handle, bool on);

static void rx_pad_extras_resolve(void)
{
    static bool tried;
    if (tried) {
        return;
    }
    tried = true;
    rx_pad_trigger_fn = scePadSetTriggerEffect;
    rx_pad_lightbar_fn = scePadSetLightBar;
    rx_pad_lightbar_reset_fn = scePadResetLightBar;
    rx_pad_motion_fn = scePadSetMotionSensorState;
    nc_log("xog-pad", "trigger effect, light bar and motion sensor: libScePad imports (patch 267)");
}

/* L2 and R2 for a kind of game; 0 = off. Driving: R2 the gas, light and rising (slope feedback 1-9, strength
 * 1 to 4), L2 the brake, firm from near its start (feedback at 1, strength 6). Shooter: R2 a gun's trigger, a
 * section that gives way (weapon 4-6, strength 6); L2 free for aiming. */
static int rx_pad_trigger_set(int32_t handle, int kind)
{
    if (!rx_pad_trigger_fn || handle < 0) {
        return -1;
    }
    RxTriggerParam t;
    memset(&t, 0, sizeof(t));
    t.mask = 3;
    if (kind == 1) {
        t.command[0].mode = 1;
        t.command[0].data[0] = 1;   /* position */
        t.command[0].data[1] = 6;   /* strength */
        t.command[1].mode = 1;      /* patch 267: plain feedback (the slope mode's number isn't certain) */
        t.command[1].data[0] = 2;   /* position */
        t.command[1].data[1] = 3;   /* strength */
    } else if (kind == 2) {
        t.command[1].mode = 2;
        t.command[1].data[0] = 4;   /* start position */
        t.command[1].data[1] = 6;   /* end position */
        t.command[1].data[2] = 6;   /* strength */
    }
    int rc = rx_pad_trigger_fn(handle, &t);
    if (rc != 0 && kind == 1) {
        /* a firmware without slope feedback: R2 an even light resistance */
        const int first = rc;
        memset(&t.command[1], 0, sizeof(t.command[1]));
        t.command[1].mode = 1;
        t.command[1].data[0] = 2;
        t.command[1].data[1] = 2;
        rc = rx_pad_trigger_fn(handle, &t);
        nc_log("xog-pad", "triggers: slope feedback refused (%#x), plain feedback for R2: %#x", (unsigned)first, (unsigned)rc);
    }
    return rc;
}

static void rx_pad_lightbar_reset(int32_t handle)
{
    if (rx_pad_lightbar_reset_fn && handle >= 0) {
        const int rc = rx_pad_lightbar_reset_fn(handle);
        if (rc != 0) {
            nc_log("xog-pad", "light bar reset on handle %d: %#x", handle, (unsigned)rc);
        }
    }
}

/* Each turn of the pad thread, each player: the triggers as they should be now (the game's kind while it runs
 * and Settings has them on; off in the quick menu), the light bar once a controller, the motion sensor on for
 * gyro aim. Sent only when something changed. */
static void rx_pad_extras(unsigned player, int32_t handle, bool in_menu)
{
    static int32_t seen[4] = {-1, -1, -1, -1};
    static int kinds[4] = {-1, -1, -1, -1};
    static unsigned fails;
    if (player >= 4 || handle < 0 || g_rx_pad_leaving) {
        return;
    }
    rx_pad_extras_resolve();
    if (seen[player] != handle) {
        seen[player] = handle;
        g_rx_pad_handles[player] = handle;
        kinds[player] = -1;
        static const RxLightBar colours[4] = {{16, 124, 16, 0}, {0, 64, 255, 0}, {255, 0, 0, 0}, {255, 64, 160, 0}};   /* patch 268: player 1 Xbox green (#107C10) */
        static const char *const names[4] = {"Xbox green", "blue", "red", "pink"};
        if (rx_pad_lightbar_fn) {
            const int rc = rx_pad_lightbar_fn(handle, &colours[player]);
            nc_log("xog-pad", "light bar: player %u %s (handle %d): %#x", player + 1, names[player], handle, (unsigned)rc);
        }
        if (g_rx_gyro) {
            const int rc = rx_pad_motion_fn ? rx_pad_motion_fn(handle, true) : -1;
            nc_log("xog-pad", "gyro aim on, sensitivity %s, player %u: motion sensor %s %#x", g_rx_gyro >= 3 ? "high" :
                   g_rx_gyro == 2 ? "medium" : "low", player + 1, rx_pad_motion_fn ? "on" : "function missing,",
                   (unsigned)rc);
        }
    }
    const int want = in_menu || !g_rx_triggers ? 0 : g_rx_trigger_kind;
    if (want == kinds[player]) {
        return;
    }
    const int rc = rx_pad_trigger_set(handle, want);
    static const char *const kind_names[3] = {"off", "driving", "shooter"};
    if (rc == 0 || fails++ < 8) {
        nc_log("xog-pad", "triggers: player %u %s (game kind %s%s%s): %#x", player + 1, kind_names[want],
               kind_names[g_rx_trigger_kind], in_menu ? ", quick menu" : "", g_rx_triggers ? "" : ", Settings off",
               (unsigned)rc);
    }
    kinds[player] = want;   /* a refusal isn't sent again every turn */
}

/* Leaving the game (or restarting it): triggers off and the light bar the system's again, on every controller. */
static void rx_pad_extras_leave(void)
{
    g_rx_pad_leaving = 1;
    usleep(10000);          /* the pad thread past its turn */
    for (int p = 0; p < 4; p++) {
        if (g_rx_pad_handles[p] >= 0) {
            rx_pad_trigger_set(g_rx_pad_handles[p], 0);
            rx_pad_lightbar_reset(g_rx_pad_handles[p]);
        }
    }
    xog_haptics_stop_all();     /* HD haptics: every pad back to the plain motors, still */
    nc_log("xog-pad", "leaving: triggers off, light bars reset, haptics off");
}

/* The dashboard's controller (input-manager.cc): nothing left of a game's, even after a crash. */
void rx_pad_extras_dashboard(int32_t handle)
{
    rx_pad_extras_resolve();
    const int rc = rx_pad_trigger_set(handle, 0);
    rx_pad_lightbar_reset(handle);
    nc_log("xog-pad", "dashboard: triggers off (%#x), light bar reset", (unsigned)rc);
}

/* Gyro aim: the angular velocity (rad/s) as right-stick movement, on top of the stick. Yaw (around the pad's
 * up axis) turns, pitch (around its left-right axis) looks up and down. A slow bias estimate while the pad is
 * still, and a small deadzone, so a still pad doesn't drift; past the deadzone it starts just over the games'
 * own stick deadzone, so small moves count. */
static void rx_gyro_add(unsigned player, const RxPadData *d, NcPadState *s)
{
    static float bias[4][2];
    static unsigned logs;
    if (!g_rx_gyro || player >= 4) {
        return;
    }
    float w[3];
    memcpy(w, d->rest + 28, sizeof(w));
    if (w[0] != w[0] || w[1] != w[1]) {
        return;     /* NaN: no sensor data */
    }
    const float pitch = w[0], yaw = w[1];
    float *const b = bias[player];
    if (fabsf(pitch - b[0]) < 0.06f && fabsf(yaw - b[1]) < 0.06f) {
        b[0] += (pitch - b[0]) * 0.01f;
        b[1] += (yaw - b[1]) * 0.01f;
    }
    static const float gains[4] = {0, 6000, 11000, 18000};     /* stick units per rad/s */
    const float gain = gains[g_rx_gyro > 3 ? 3 : g_rx_gyro], dead = 0.04f, start = 5000;
    const float axes[2] = {-(yaw - b[1]), pitch - b[0]};     /* turning left: stick left; front up: aim up */
    int16_t *const out[2] = {&s->rx, &s->ry};
    for (int i = 0; i < 2; i++) {
        const float v = axes[i], m = fabsf(v);
        if (m < dead) {
            continue;
        }
        const float add = (start + (m - dead) * gain) * (v < 0 ? -1.0f : 1.0f);
        float r = (float)*out[i] + add;
        r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
        *out[i] = (int16_t)r;
    }
    if (logs < 6 && (fabsf(pitch) > 1.0f || fabsf(yaw) > 1.0f)) {
        logs++;
        nc_log("xog-pad", "gyro: player %u pitch %.2f yaw %.2f roll %.2f rad/s -> stick %d %d", player + 1, pitch, yaw,
               w[2], s->rx, s->ry);
    }
}

static NcCore g_core;
extern uint64_t rx_frames_shown;

/* patch 253: fast-forward (ff_ps5.c) */
void nc_ff_allow(bool on);
bool nc_ff_allowed(void);
void nc_ff_set(bool on, const char *why);
void nc_ff_off(const char *why);
void nc_ff_poll(void);
extern volatile int g_xog_ff;
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
    nc_ff_off("leaving or restarting");    /* patch 253 */
    rx_pad_extras_leave();                  /* patch 266: triggers off, light bars the system's */
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
    {
        /* patch 261: the cache drives' written-behind writes (xogfix=cachewrite) on the disk before the process ends */
        extern bool nc_io_writes_drain(uint64_t timeout_ms, const char *why);
        nc_io_writes_drain(30000, "leaving");
    }
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

/* patch 253: the touchpad with fast-forward allowed (Settings > Advanced, ff=1; never in a Chihiro run).
 * A swipe right (a touch that moves right more than 35% of the pad's width within 400 ms of its start) turns
 * fast-forward on or off, at most every 500 ms and once a touch; a swipe left is kept for rewind (logged only).
 * The quick menu opens when the touchpad's press is let go, unless that touch swiped: a click opens it even when
 * the finger rolls a little (patch 254: a 10% "slide" blocked it; a new touch, by its id, starts fresh). ScePadData's touch data is at 0x34 (rest + 40): touchNum,
 * 3 reserved, a reserved word, then touch[0] {u16 x, u16 y, u8 id, 3 reserved} (x 0-1919 on a DualSense). */
__attribute__((unused)) static void rx_touch_gesture(const RxPadData *d, bool pressed, bool was_pressed)
{
    extern uint64_t rx_now_ms(void);
    static bool down, swiped;
    static unsigned id0;
    static uint64_t t0, last_toggle;
    static int x0, xmax = 1919;
    static unsigned raw_logs;
    const uint8_t *td = d->rest + 40;
    const unsigned num = td[0];
    const int x = td[8] | td[9] << 8, y = td[10] | td[11] << 8;
    const bool valid = num >= 1 && num <= 2 && x < 4096;
    const uint64_t now = rx_now_ms();
    if (valid && x > xmax) {
        xmax = x;
    }
    const int width = xmax + 1;
    if (valid && (!down || td[12] != id0)) {
        down = true;
        swiped = false;
        id0 = td[12];
        x0 = x;
        t0 = now;
        if (raw_logs < 4) {
            raw_logs++;
            nc_log("xog-ff", "touch: %u point(s), x %d y %d id %u (bytes %02x %02x %02x %02x | %02x %02x %02x %02x %02x)",
                   num, x, y, td[12], td[0], td[1], td[2], td[3], td[8], td[9], td[10], td[11], td[12]);
        }
    } else if (valid) {
        const int dx = x - x0, adx = dx < 0 ? -dx : dx;
        if (!swiped && now - t0 <= 400 && adx * 100 > width * 35) {
            swiped = true;
            if (dx < 0) {
                nc_log("xog-ff", "swipe left: nothing yet (kept for rewind)");
            } else if (g_qm_open) {
                /* the menu's own */
            } else if (now - last_toggle < 500) {
                nc_log("xog-ff", "swipe right too soon after the last (%u ms): ignored", (unsigned)(now - last_toggle));
            } else {
                last_toggle = now;
                nc_ff_set(!g_xog_ff, "swipe right");
            }
        }
    } else {
        down = false;   /* swiped kept until the next touch: the press may be let go after the finger */
    }
    if (!pressed && was_pressed && !g_qm_open) {
        if (swiped) {
            nc_log("xog-ff", "touchpad press let go after a swipe: no quick menu");
        } else {
            xog_qm_request();
        }
    }
}

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
    /* patch 269: the game's own button mapping, never in a Chihiro run (its games keep their fixed layouts) */
    const bool own_map = g_rx_padmap_on && !getenv("NC_CHIHIRO");
    {
        XbPadMap m;
        char text[512];
        if (own_map) {
            m = g_rx_padmap;
        } else {
            xemu_ps5_padmap_global(&m);
        }
        xemu_ps5_padmap_describe(&m, text, sizeof(text));
        nc_log("xog-pad", "button mapping: %s: %s",
               own_map ? "this game's own" : getenv("NC_CHIHIRO") ? "Chihiro run, your layout (no game mapping)" :
                                             "your layout (Settings > Controller)", text);
    }
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
                    xog_qm_request();   /* patch 255: no swipes (fast-forward is in the menu's Settings) */
                }
                touch = (b & 0x00100000u) != 0;
                if (g_qm_open || mute) {
                    g_qm_buttons = b;
                    g_qm_stick_x = (int)d.lx - 128;
                    g_qm_stick_y = (int)d.ly - 128;
                    mute = g_qm_open || (b != 0);
                }
            }
            rx_rumble((unsigned)p, pl[p].user, pl[p].handle, g_qm_open || !g_rx_vibration);   /* patches 209/234: the game's rumble */
            rx_pad_extras((unsigned)p, pl[p].handle, g_qm_open != 0);       /* patch 266: triggers, light bar */
            if (p == 0 && getenv("NC_CHIHIRO")) {
                rx_mouse_poll(pl[p].user);              /* patch 227: a mouse for the gun games */
            }
            if (g_qm_open || mute) {
                nc_pad_set((unsigned)p, &s);
                continue;
            }
/* patch 269: the game's own mapping or your layout. A source's level: L2/R2 analog, any other button 0 or 255
 * (full press on the Xbox's analog buttons and triggers) */
#define RX_LEVEL(x) (own_map ? xemu_ps5_source_level(g_rx_padmap.src[(x)], b, d.l2, d.r2) : \
                               xemu_ps5_xbox_level((x), b, d.l2, d.r2))
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
            if (g_rx_turbo_live >= 0) {
                g_rx_turbo = g_rx_turbo_live;   /* patch 324: changed in the quick menu */
            }
            if (g_rx_turbo) {
                /* patch 317: a held turbo button reads pressed 33 ms, let go 33 ms: ~15 presses a second */
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                if (((ts.tv_sec * 1000 + ts.tv_nsec / 1000000) / 33) & 1) {
                    static const int slot[6] = { 0, 1, 2, 3, 7, 6 };     /* A B X Y RT LT in analog[] */
                    for (int i = 0; i < 6; i++) {
                        if (g_rx_turbo & (1 << i)) {
                            s.analog[slot[i]] = 0;
                        }
                    }
                }
            }
            {
                /* patch 269: swap sticks, then invert up/down of the Xbox's stick (what the game gets) */
                const int fl = own_map ? g_rx_padmap.flags : 0;
                const bool swap = (fl & PADMAP_SWAP_STICKS) != 0;
                s.lx = stick(swap ? d.rx : d.lx, false);
                s.ly = stick(swap ? d.ry : d.ly, !(fl & PADMAP_INVERT_LY));
                s.rx = stick(swap ? d.lx : d.rx, false);
                s.ry = stick(swap ? d.ly : d.ry, !(fl & PADMAP_INVERT_RY));
            }
            rx_gyro_add((unsigned)p, &d, &s);      /* patch 266: gyro aim (this game's setting) */
            nc_pad_set((unsigned)p, &s);
        }
        nc_ff_poll();       /* patch 253: the guest's speed once a second while fast-forward is on */
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
            if (!strncmp(line, "chihiro=", 8)) {
                /* Sega Chihiro mode (XPSemu patch 185): 128 MB, the arcade flag; the game named for the media board */
                setenv("NC_CHIHIRO", line + 8, 1);
                setenv("NC_RAM_MB", "128", 1);
                nc_log("xog", "Chihiro mode: %s", line + 8);
                continue;
            }
            if (!strncmp(line, "chihiroimg=", 11)) {
                setenv("NC_CHIHIRO_IMG", line + 11, 1);    /* the image read in place (patch 219) */
                continue;
            }
            if (!strncmp(line, "chihirofs=", 10)) {
                setenv("NC_CHIHIRO_FS", line + 10, 1);     /* the media board's mbfs: (patch 187) */
                nc_log("xog", "Chihiro mbfs: %s", line + 10);
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
            } else if (!strncmp(line, "trainer=", 8)) {
                extern void nc_trainer_set(const char *path);      /* native/core/trainer.c (patch 246) */
                nc_trainer_set(line + 8);
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "trainer_opts=", 13)) {
                extern void nc_trainer_set_options(const char *bits);
                nc_trainer_set_options(line + 13);
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "xogfix=", 7)) {
                setenv("XOG_FIX", line + 7, 1);     /* this game's own fixes */
                nc_log("xog", "option: %s", line);
            } else if (!strcmp(line, "apudsp")) {
                setenv("NC_APU_DSP", "1", 1);   /* read when the sound chip is made */
                nc_log("xog", "option: full sound processor");
            } else if (!strncmp(line, "triggers=", 9)) {
                /* patch 266: the adaptive triggers' kind of game (the dashboard's choice); Settings' on/off is read
                 * by the quick menu's start (g_rx_triggers) */
                g_rx_trigger_kind = atoi(line + 9);
                g_rx_trigger_kind = g_rx_trigger_kind < 0 || g_rx_trigger_kind > 2 ? 0 : g_rx_trigger_kind;
                nc_log("xog", "option: %s (adaptive triggers %s)", line,
                       g_rx_trigger_kind == 1 ? "driving" : g_rx_trigger_kind == 2 ? "shooter" : "off");
            } else if (!strncmp(line, "turbo=", 6)) {
                g_rx_turbo = atoi(line + 6) & 63;    /* patch 317: turbo buttons */
                nc_log("xog-pad", "turbo buttons: %s%s%s%s%s%s", g_rx_turbo & 1 ? "A " : "", g_rx_turbo & 2 ? "B " : "",
                       g_rx_turbo & 4 ? "X " : "", g_rx_turbo & 8 ? "Y " : "", g_rx_turbo & 16 ? "RT " : "",
                       g_rx_turbo & 32 ? "LT" : "");
                continue;
            } else if (!strncmp(line, "gyro=", 5)) {
                g_rx_gyro = atoi(line + 5);         /* patch 266: gyro aim, 1 low to 3 high */
                g_rx_gyro = g_rx_gyro < 0 ? 0 : g_rx_gyro > 3 ? 3 : g_rx_gyro;
                nc_log("xog", "option: %s (gyro aim %s)", line,
                       g_rx_gyro == 3 ? "high" : g_rx_gyro == 2 ? "medium" : g_rx_gyro ? "low" : "off");
            } else if (!strncmp(line, "padmap=", 7)) {
                /* patch 269: the game's own button mapping (the dashboard's game settings); Xbox games only:
                 * never used in a Chihiro run (the pad thread checks NC_CHIHIRO) */
                g_rx_padmap_on = xemu_ps5_padmap_parse(line + 7, &g_rx_padmap) != 0;
                nc_log("xog", "option: %s (button mapping %s)", line, g_rx_padmap_on ? "this game's own" : "unreadable, not used");
            } else if (!strncmp(line, "ff=", 3)) {
                /* patch 253: fast-forward allowed (Settings > Advanced); never in a Chihiro run */
                nc_ff_allow(atoi(line + 3) != 0 && !getenv("NC_CHIHIRO"));
            } else if (!strncmp(line, "fpscap=", 7)) {
                extern void hle_pb_set_fps_cap(int fps);
                hle_pb_set_fps_cap(atoi(line + 7));
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "msaa=", 5)) {
                /* patch 325: MSAA on the game's render targets (the game settings' Picture row), read by the
                 * renderer when its surfaces are made: before the game starts, never changed while it runs */
                extern void hle_vk_set_msaa(int samples);
                hle_vk_set_msaa(atoi(line + 5));
                nc_log("xog", "option: %s (MSAA %dx)", line, atoi(line + 5));
            } else if (!strncmp(line, "a2c=", 4)) {
                /* patch 354: MSAA smooth cut-outs (alpha-to-coverage), on unless 0 (the game settings' Picture row) */
                extern void hle_vk_set_a2c(int on);
                hle_vk_set_a2c(atoi(line + 4));
                nc_log("xog", "option: %s (MSAA smooth cut-outs %s)", line, atoi(line + 4) ? "on" : "off");
            } else if (!strncmp(line, "movieup=", 8)) {
                /* patch 355: the movie upscaler, on unless 0 (the game settings' Picture row) */
                extern void hle_vk_set_movieup(int on);
                hle_vk_set_movieup(atoi(line + 8));
                nc_log("xog", "option: %s (movie upscaler %s)", line, atoi(line + 8) ? "on" : "off");
            } else if (!strncmp(line, "ssao=", 5)) {
                /* patch 363: ambient occlusion, 0 off (the default), 1 low, 2 high (the game settings' Picture row) */
                xog_set_ssao(atoi(line + 5));     /* patch 366: through the setter (patch 366) */
                nc_log("xog", "option: %s (ambient occlusion %s)", line,
                       atoi(line + 5) >= 2 ? "high" : atoi(line + 5) == 1 ? "low" : "off");
            } else if (!strncmp(line, "af=", 3)) {
                extern void hle_vk_set_af(int level);      /* vk/draw.c: before the first sampler (patch 241) */
                hle_vk_set_af(atoi(line + 3));
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "aa=", 3)) {
                extern void nc_present_set_fxaa(bool on);
                extern void nc_present_set_smaa(bool on);
                nc_present_set_fxaa(!strcmp(line + 3, "fxaa"));
                nc_present_set_smaa(!strcmp(line + 3, "smaa") || !strcmp(line + 3, "ssaa+smaa"));
                nc_log("xog", "option: %s", line);
            } else if (!strcmp(line, "upscale=sgsr") || !strcmp(line, "upscale=bilinear") || !strcmp(line, "upscale=cas")) {
                extern void nc_present_set_upscale(int mode);
                nc_present_set_upscale(line[8] == 's' ? 1 : line[8] == 'c' ? 2 : 0);     /* app.h's NC_UPSCALE_* (CAS: patch 247) */
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "cas=", 4)) {
                extern void nc_present_set_cas(int level);      /* patch 247: CAS's sharpness, 0 to 4 */
                nc_present_set_cas(atoi(line + 4));
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "crt=", 4)) {
                extern void nc_present_set_crt(int mode);       /* patch 247: 0 off, 1 scanlines, 2 CRT */
                nc_present_set_crt(atoi(line + 4));
                nc_log("xog", "option: %s", line);
            } else if (!strncmp(line, "texsharp=", 9)) {
                extern void hle_vk_set_texsharp(int level);     /* vk/draw.c (patch 247): 0 to 3 */
                hle_vk_set_texsharp(atoi(line + 9));
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
    {   /* a test's debug switches: no file, nothing. A Chihiro start (a "chihiro=" start line) reads
         * /data/xpsemu/chihiro/debug.txt instead, so its tools never slow Xbox games (patch 195) */
        bool chihiro = false;
        FILE *start = fopen(RX_DIR "/launch.txt", "r");
        char line[256];
        while (start && fgets(line, sizeof(line), start)) {
            if (!strncmp(line, "chihiro=", 8)) {
                chihiro = true;
            }
        }
        if (start) {
            fclose(start);
        }
        FILE *dbg = fopen(chihiro ? RX_DIR "/chihiro/debug.txt" : RX_DIR "/debug.txt", "r");
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
    if (no_disc) {
        setenv("XOG_XBOX_DASH", "1", 1);    /* patch 259: the quick menu shows only Resume and Back to home */
    }
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
