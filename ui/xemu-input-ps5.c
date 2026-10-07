/*
 * xemu Input: the PS5's controller (DualSense), read with libScePad
 *
 * SDL has no PS5 joystick driver in this build, so the DualSense is its own
 * controller type, mapped to an Xbox controller. The calls and the layout of
 * the pad's state are the ones PS5SX2 uses (ps5/coreorbis/main-boot.cpp).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "xemu-input.h"
#include "xemu-settings.h"

int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void *data);

/* The start of ScePadData; the rest (motion, touch, ...) isn't used. */
typedef struct PadData {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry; /* 0-255, 128 centred, down and right larger */
    uint8_t l2, r2;         /* 0-255 */
    uint8_t pad[2];
    uint8_t rest[256];
} PadData;

enum {
    PAD_L3 = 0x00000002,
    PAD_R3 = 0x00000004,
    PAD_OPTIONS = 0x00000008,
    PAD_UP = 0x00000010,
    PAD_RIGHT = 0x00000020,
    PAD_DOWN = 0x00000040,
    PAD_LEFT = 0x00000080,
    PAD_L1 = 0x00000400,
    PAD_R1 = 0x00000800,
    PAD_TRIANGLE = 0x00001000,
    PAD_CIRCLE = 0x00002000,
    PAD_CROSS = 0x00004000,
    PAD_SQUARE = 0x00008000,
    PAD_TOUCHPAD = 0x00100000,
    PAD_CREATE = 0x00000001, /* PS5SX2 maps it with the touchpad, to Select */
};

static int32_t pad_user = -1;
static int32_t pad_handle = -1;
static int64_t pad_next_open_ms;

/* Opens the first user's controller; again every two seconds until it's there. */
static void pad_open(void)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (pad_handle >= 0 || now < pad_next_open_ms) {
        return;
    }
    pad_next_open_ms = now + 2000;

    static bool initialized;
    if (!initialized) {
        int rc_user = sceUserServiceInitialize(NULL);
        int rc_pad = scePadInit();
        fprintf(stderr, "xemu PS5 pad: user service %#x, pad %#x\n", rc_user,
                rc_pad);
        initialized = true;
    }
    if (pad_user < 0 && sceUserServiceGetInitialUser(&pad_user) != 0) {
        pad_user = -1;
        return;
    }

    pad_handle = scePadOpen(pad_user, 0, 0, NULL);
    if (pad_handle < 0) {
        pad_handle = scePadGetHandle(pad_user, 0, 0);
    }
    fprintf(stderr, "xemu PS5 pad: user %d, handle %d\n", pad_user,
            pad_handle);
}

ControllerState *xemu_input_ps5_pad_create(void)
{
    ControllerState *state = g_new0(ControllerState, 1);
    state->type = INPUT_DEVICE_PS5_PAD;
    state->name = "DualSense";
    state->bound = -1;
    state->peripheral_types[0] = PERIPHERAL_NONE;
    state->peripheral_types[1] = PERIPHERAL_NONE;
    pad_open();
    return state;
}

static int16_t stick(uint8_t v)
{
    int value = ((int)v - 128) * 256;
    return value > 32767 ? 32767 : value;
}

static int16_t trigger(uint8_t v)
{
    return (int16_t)((int)v * 32767 / 255);
}

/*
 * XPSemu: the Xbox controller's buttons and triggers can each come from any
 * of the DualSense's (Settings > Controller), kept in
 * /data/xemu/controller.txt. The touchpad stays the Guide button (it opens
 * the dashboard), the sticks stay the sticks. The dashboard follows the
 * layout too (xemu_ps5_pad_ui_buttons gives the standard one).
 */
static const uint32_t kSourceBit[PS5_SRC__COUNT] = {
    [PS5_SRC_CROSS] = PAD_CROSS,     [PS5_SRC_CIRCLE] = PAD_CIRCLE,
    [PS5_SRC_SQUARE] = PAD_SQUARE,   [PS5_SRC_TRIANGLE] = PAD_TRIANGLE,
    [PS5_SRC_L1] = PAD_L1,           [PS5_SRC_R1] = PAD_R1,
    [PS5_SRC_L3] = PAD_L3,           [PS5_SRC_R3] = PAD_R3,
    [PS5_SRC_OPTIONS] = PAD_OPTIONS, [PS5_SRC_CREATE] = PAD_CREATE,
    [PS5_SRC_UP] = PAD_UP,           [PS5_SRC_DOWN] = PAD_DOWN,
    [PS5_SRC_LEFT] = PAD_LEFT,       [PS5_SRC_RIGHT] = PAD_RIGHT,
    /* L2 and R2 are read as triggers */
};
static const char *const kSourceName[PS5_SRC__COUNT] = {
    "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3",
    "R3", "Options", "Create", "D-pad up", "D-pad down", "D-pad left",
    "D-pad right",
};
static const char *const kSourceKey[PS5_SRC__COUNT] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3",
    "r3", "options", "create", "up", "down", "left", "right",
};
static const uint16_t kXboxBit[XB__COUNT] = {
    [XB_A] = CONTROLLER_BUTTON_A,           [XB_B] = CONTROLLER_BUTTON_B,
    [XB_X] = CONTROLLER_BUTTON_X,           [XB_Y] = CONTROLLER_BUTTON_Y,
    [XB_BLACK] = CONTROLLER_BUTTON_BLACK,   [XB_WHITE] = CONTROLLER_BUTTON_WHITE,
    [XB_BACK] = CONTROLLER_BUTTON_BACK,     [XB_START] = CONTROLLER_BUTTON_START,
    [XB_LSTICK] = CONTROLLER_BUTTON_LSTICK, [XB_RSTICK] = CONTROLLER_BUTTON_RSTICK,
    [XB_UP] = CONTROLLER_BUTTON_DPAD_UP,    [XB_DOWN] = CONTROLLER_BUTTON_DPAD_DOWN,
    [XB_LEFT] = CONTROLLER_BUTTON_DPAD_LEFT, [XB_RIGHT] = CONTROLLER_BUTTON_DPAD_RIGHT,
    /* The triggers are axes */
};
static const char *const kXboxKey[XB__COUNT] = {
    "a", "b", "x", "y", "black", "white", "back", "start", "lstick", "rstick",
    "up", "down", "left", "right", "lt", "rt",
};
static const int8_t kDefaultMap[XB__COUNT] = {
    [XB_A] = PS5_SRC_CROSS,       [XB_B] = PS5_SRC_CIRCLE,
    [XB_X] = PS5_SRC_SQUARE,      [XB_Y] = PS5_SRC_TRIANGLE,
    [XB_BLACK] = PS5_SRC_R1,      [XB_WHITE] = PS5_SRC_L1,
    [XB_BACK] = PS5_SRC_CREATE,   [XB_START] = PS5_SRC_OPTIONS,
    [XB_LSTICK] = PS5_SRC_L3,     [XB_RSTICK] = PS5_SRC_R3,
    [XB_UP] = PS5_SRC_UP,         [XB_DOWN] = PS5_SRC_DOWN,
    [XB_LEFT] = PS5_SRC_LEFT,     [XB_RIGHT] = PS5_SRC_RIGHT,
    [XB_LT] = PS5_SRC_L2,         [XB_RT] = PS5_SRC_R2,
};
static int8_t g_map[XB__COUNT];
static bool g_map_loaded;
static uint32_t g_raw_buttons;    /* The DualSense as last read */
static uint8_t g_raw_l2, g_raw_r2;
static uint32_t g_ui_buttons;     /* In the standard layout */
int g_xemu_ps5_pad_hold;          /* The dashboard is up: no rumble, triggers */

static char *map_file(void)
{
    return g_strdup_printf("%scontroller.txt", xemu_settings_get_base_path());
}

static void map_load(void)
{
    g_map_loaded = true;
    memcpy(g_map, kDefaultMap, sizeof(g_map));
    g_autofree char *path = map_file();
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char xbox[32], src[32];
    while (fscanf(f, " %31[^=]=%31s", xbox, src) == 2) {
        for (int x = 0; x < XB__COUNT; x++) {
            for (int s = 0; s < PS5_SRC__COUNT; s++) {
                if (!strcmp(xbox, kXboxKey[x]) && !strcmp(src, kSourceKey[s])) {
                    g_map[x] = s;
                }
            }
        }
    }
    fclose(f);
    fprintf(stderr, "xemu PS5 pad: layout from %s:", path);
    for (int x = 0; x < XB__COUNT; x++) {
        fprintf(stderr, " %s=%s", kXboxKey[x], kSourceKey[g_map[x]]);
    }
    fprintf(stderr, "\n");
}

static void map_save(void)
{
    g_autofree char *path = map_file();
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    for (int x = 0; x < XB__COUNT; x++) {
        fprintf(f, "%s=%s\n", kXboxKey[x], kSourceKey[g_map[x]]);
    }
    fclose(f);
}

int xemu_ps5_map_get(int xbox)
{
    if (!g_map_loaded) {
        map_load();
    }
    return xbox >= 0 && xbox < XB__COUNT ? g_map[xbox] : -1;
}

/* Gives xbox the source; whatever had it gets xbox's old one (a swap). */
void xemu_ps5_map_set(int xbox, int source)
{
    if (!g_map_loaded) {
        map_load();
    }
    if (xbox < 0 || xbox >= XB__COUNT || source < 0 || source >= PS5_SRC__COUNT) {
        return;
    }
    for (int x = 0; x < XB__COUNT; x++) {
        if (g_map[x] == source) {
            g_map[x] = g_map[xbox];
        }
    }
    g_map[xbox] = source;
    map_save();
    fprintf(stderr, "xemu PS5 pad: Xbox %s now from %s\n", kXboxKey[xbox],
            kSourceName[source]);
}

void xemu_ps5_map_reset(void)
{
    memcpy(g_map, kDefaultMap, sizeof(g_map));
    g_map_loaded = true;
    map_save();
}

const char *xemu_ps5_source_name(int source)
{
    return source >= 0 && source < PS5_SRC__COUNT ? kSourceName[source] : "-";
}

/* How far a source is pressed, 0..1. */
static float source_level(int source, uint32_t buttons, uint8_t l2, uint8_t r2)
{
    if (source == PS5_SRC_L2) {
        return l2 / 255.0f;
    }
    if (source == PS5_SRC_R2) {
        return r2 / 255.0f;
    }
    return source >= 0 && source < PS5_SRC__COUNT &&
           (buttons & kSourceBit[source]) ? 1.0f : 0.0f;
}

/* The first source pressed now (for "press a button"), or -1. */
int xemu_ps5_pad_pressed_source(void)
{
    for (int s = 0; s < PS5_SRC__COUNT; s++) {
        if (source_level(s, g_raw_buttons, g_raw_l2, g_raw_r2) > 0.6f) {
            return s;
        }
    }
    return -1;
}

uint32_t xemu_ps5_pad_ui_buttons(void)
{
    return g_ui_buttons;
}

/* No rumble on the DualSense (a plain controller, by choice). */
void xemu_input_update_ps5_pad_rumble(ControllerState *state)
{
    (void)state;
}

void xemu_input_update_ps5_pad_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    pad_open();
    PadData d;
    if (pad_handle < 0 || scePadReadState(pad_handle, &d) != 0) {
        return;
    }
    if (!g_map_loaded) {
        map_load();
    }
    g_raw_buttons = d.buttons;
    g_raw_l2 = d.l2;
    g_raw_r2 = d.r2;

    /* The games: the buttons as mapped. */
    static uint32_t told; /* Remapped controls already logged */
    for (int x = 0; x < XB_LT; x++) {
        if (source_level(g_map[x], d.buttons, d.l2, d.r2) > 0.5f) {
            state->buttons |= kXboxBit[x];
            if (g_map[x] != kDefaultMap[x] && !(told & (1u << x))) {
                told |= 1u << x;
                fprintf(stderr, "xemu PS5 pad: %s pressed -> Xbox %s\n",
                        kSourceName[g_map[x]], kXboxKey[x]);
            }
        }
    }
    /* The PS button belongs to the system: the touchpad opens xemu's menu */
    if (d.buttons & PAD_TOUCHPAD) {
        state->buttons |= CONTROLLER_BUTTON_GUIDE;
    }
    state->axis[CONTROLLER_AXIS_LTRIG] =
        (int16_t)(source_level(g_map[XB_LT], d.buttons, d.l2, d.r2) * 32767);
    state->axis[CONTROLLER_AXIS_RTRIG] =
        (int16_t)(source_level(g_map[XB_RT], d.buttons, d.l2, d.r2) * 32767);
    /* xemu's Y axes are positive up (xemu_input_update_sdl_controller_state) */
    state->axis[CONTROLLER_AXIS_LSTICK_X] = stick(d.lx);
    state->axis[CONTROLLER_AXIS_LSTICK_Y] = -1 - stick(d.ly);
    state->axis[CONTROLLER_AXIS_RSTICK_X] = stick(d.rx);
    state->axis[CONTROLLER_AXIS_RSTICK_Y] = -1 - stick(d.ry);

    /* The dashboard: the standard layout. */
    uint32_t ui = 0;
    for (int x = 0; x < XB_LT; x++) {
        if (source_level(kDefaultMap[x], d.buttons, d.l2, d.r2) > 0.5f) {
            ui |= kXboxBit[x];
        }
    }
    if (d.buttons & PAD_TOUCHPAD) {
        ui |= CONTROLLER_BUTTON_GUIDE;
    }
    g_ui_buttons = ui;
    (void)trigger;
}

/* XPSemu: the dashboard's controller icon (top right): the pad is open. */
int xemu_ps5_pad_connected(void);
int xemu_ps5_pad_connected(void)
{
    return pad_handle >= 0;
}
