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

void xemu_input_update_ps5_pad_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    pad_open();
    PadData d;
    if (pad_handle < 0 || scePadReadState(pad_handle, &d) != 0) {
        return;
    }

    static const struct {
        uint32_t pad;
        uint16_t xbox;
    } map[] = {
        { PAD_CROSS, CONTROLLER_BUTTON_A },
        { PAD_CIRCLE, CONTROLLER_BUTTON_B },
        { PAD_SQUARE, CONTROLLER_BUTTON_X },
        { PAD_TRIANGLE, CONTROLLER_BUTTON_Y },
        { PAD_LEFT, CONTROLLER_BUTTON_DPAD_LEFT },
        { PAD_UP, CONTROLLER_BUTTON_DPAD_UP },
        { PAD_RIGHT, CONTROLLER_BUTTON_DPAD_RIGHT },
        { PAD_DOWN, CONTROLLER_BUTTON_DPAD_DOWN },
        { PAD_CREATE, CONTROLLER_BUTTON_BACK },
        { PAD_OPTIONS, CONTROLLER_BUTTON_START },
        { PAD_L1, CONTROLLER_BUTTON_WHITE },
        { PAD_R1, CONTROLLER_BUTTON_BLACK },
        { PAD_L3, CONTROLLER_BUTTON_LSTICK },
        { PAD_R3, CONTROLLER_BUTTON_RSTICK },
        /* The PS button belongs to the system: the touchpad opens xemu's menu */
        { PAD_TOUCHPAD, CONTROLLER_BUTTON_GUIDE },
    };
    for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if (d.buttons & map[i].pad) {
            state->buttons |= map[i].xbox;
        }
    }

    state->axis[CONTROLLER_AXIS_LTRIG] = trigger(d.l2);
    state->axis[CONTROLLER_AXIS_RTRIG] = trigger(d.r2);
    /* xemu's Y axes are positive up (xemu_input_update_sdl_controller_state) */
    state->axis[CONTROLLER_AXIS_LSTICK_X] = stick(d.lx);
    state->axis[CONTROLLER_AXIS_LSTICK_Y] = -1 - stick(d.ly);
    state->axis[CONTROLLER_AXIS_RSTICK_X] = stick(d.rx);
    state->axis[CONTROLLER_AXIS_RSTICK_Y] = -1 - stick(d.ry);
}
