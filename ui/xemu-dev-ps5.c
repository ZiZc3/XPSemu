/*
 * XPSemu: dev build tools: the crash journal and the stress test's button
 * bot (ui/xemu-dev-ps5.h). Compiled in every build; empty in releases.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "xemu-input.h"
#include "xemu-dev-ps5.h"

volatile int xemu_dev_stress_on;
volatile int xemu_dev_bot_on;

#if XPSEMU_DEV

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char g_build[32];

void xemu_dev_journal_raw(const char *line)
{
    int fd = open(XEMU_DEV_JOURNAL, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        (void)!write(fd, line, strlen(line));
        close(fd);
    }
}

void xemu_dev_journal(char kind, const char *fmt, ...)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "%c %lld ", kind,
                     (long long)time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    /* One line, whatever the details hold */
    for (char *p = line; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    strcat(line, "\n");
    xemu_dev_journal_raw(line);
}

const char *xemu_dev_build(void)
{
    return g_build;
}

void xemu_dev_start(void)
{
    mkdir(XEMU_DEV_DIR, 0777);
    /* ps5/build-dev.sh puts the eboot's id next to it */
    static const char *const paths[] = {
        "/app0/dev-build.txt",
        "/data/homebrew/PPSA97358/dev-build.txt",
    };
    for (size_t i = 0; !g_build[0] && i < ARRAY_SIZE(paths); i++) {
        FILE *f = fopen(paths[i], "r");
        if (f) {
            if (fscanf(f, "%31s", g_build) != 1) {
                g_build[0] = 0;
            }
            fclose(f);
        }
    }
    xemu_dev_journal('S', "%s", g_build[0] ? g_build : "unknown");
    fprintf(stderr, "XPSemu: DEV build %s, journal %s\n",
            g_build[0] ? g_build : "(no dev-build.txt)", XEMU_DEV_JOURNAL);
}

/*
 * The bot: what a curious player does with a pad they don't know. Mostly
 * A and the left stick (menus, walking), sometimes Start, X/Y, the right
 * stick or a trigger, rarely B (it backs out of menus). Every action is
 * held for a while, then the bot waits a little.
 */
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint32_t g_rng = 0x9e3779b9;

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static double rnd_range(double lo, double hi)
{
    return lo + (hi - lo) * (rnd() % 10000) / 10000.0;
}

static struct {
    double until, next;  /* The action ends; the next one starts */
    uint32_t buttons;
    int16_t axis[CONTROLLER_AXIS__COUNT];
} g_bot;

static double g_player_at = -1e9; /* The player last touched the pad */

static void bot_pick(double now)
{
    memset(&g_bot.axis, 0, sizeof(g_bot.axis));
    g_bot.buttons = 0;
    double hold = 0.12;
    int r = rnd() % 100;
    double angle = rnd_range(0, 6.2832);
    int16_t sx = (int16_t)(cos(angle) * 30000), sy = (int16_t)(sin(angle) * 30000);
    if (r < 30) {
        g_bot.buttons = CONTROLLER_BUTTON_A;
    } else if (r < 40) {
        g_bot.buttons = CONTROLLER_BUTTON_START;
    } else if (r < 44) {
        g_bot.buttons = CONTROLLER_BUTTON_B;
    } else if (r < 54) {
        g_bot.buttons = (rnd() & 1) ? CONTROLLER_BUTTON_X : CONTROLLER_BUTTON_Y;
    } else if (r < 79) {
        g_bot.axis[CONTROLLER_AXIS_LSTICK_X] = sx;
        g_bot.axis[CONTROLLER_AXIS_LSTICK_Y] = sy;
        hold = rnd_range(0.5, 3.0);
    } else if (r < 89) {
        g_bot.axis[CONTROLLER_AXIS_RSTICK_X] = sx;
        g_bot.axis[CONTROLLER_AXIS_RSTICK_Y] = sy;
        hold = rnd_range(0.3, 1.5);
    } else if (r < 94) {
        g_bot.axis[(rnd() & 1) ? CONTROLLER_AXIS_LTRIG : CONTROLLER_AXIS_RTRIG] =
            32767;
        hold = rnd_range(0.3, 1.0);
    } else {
        static const uint32_t dpad[] = {
            CONTROLLER_BUTTON_DPAD_UP, CONTROLLER_BUTTON_DPAD_DOWN,
            CONTROLLER_BUTTON_DPAD_LEFT, CONTROLLER_BUTTON_DPAD_RIGHT,
        };
        g_bot.buttons = dpad[rnd() % 4];
    }
    g_bot.until = now + hold;
    g_bot.next = g_bot.until + rnd_range(0.2, 1.0);
}

void xemu_dev_bot(ControllerState *state)
{
    double now = now_s();
    /* The player's own presses come first, and pause the bot */
    bool touched = state->buttons != 0;
    for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
        if (state->axis[i] > 8000 || state->axis[i] < -8000) {
            touched = true;
        }
    }
    if (touched) {
        g_player_at = now;
    }
    if (!xemu_dev_bot_on || now - g_player_at < 10) {
        g_bot.next = 0;
        return;
    }
    if (now >= g_bot.next) {
        bot_pick(now);
    }
    if (now < g_bot.until) {
        state->buttons |= g_bot.buttons;
        for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
            if (g_bot.axis[i]) {
                state->axis[i] = g_bot.axis[i];
            }
        }
    }
}

#else /* !XPSEMU_DEV */

void xemu_dev_journal(char kind, const char *fmt, ...) {}
void xemu_dev_journal_raw(const char *line) {}
void xemu_dev_start(void) {}
const char *xemu_dev_build(void) { return ""; }
void xemu_dev_bot(ControllerState *state) {}

#endif
