/*
 * XPSemu: dev build tools (ui/xpsemu-dev.h): the crash journal and the
 * stress test's button bot. In a release build these do nothing.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_DEV_PS5_H
#define XEMU_DEV_PS5_H

#include <stdint.h>
#include "xpsemu-dev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XEMU_DEV_DIR "/data/xemu/dev"
#define XEMU_DEV_JOURNAL XEMU_DEV_DIR "/journal.txt"

/*
 * The journal: one line per event, "<letter> <unix time> <details>", added
 * to and never cleared (delete the file to start counting again):
 *   S  XPSemu started            S <time> <build>
 *   G  a game started            G <time> <title id> <name>
 *   E  the game stopped          E <time> <why>
 *   H  still running (a minute)  H <time> <fps>
 *   F  the game froze            F <time> <seconds without a frame>
 *   W  XPSemu hung (45 s)        W <time> <why>  (stress test watchdog)
 *   Q  XPSemu quit or restarted  Q <time> <restart|exit>
 *   C  XPSemu crashed            C <time> <signal> <eboot|jit|system> <rip
 *                                 offset> <fault address> <eboot return
 *                                 addresses on the stack...>
 * A session (from S) that ends with no Q or C ended without a word: a GPU
 * fault (the PS5 closes the app), a hang the PS5 ended, or the app closed
 * from the PS5's home screen.
 */
void xemu_dev_journal(char kind, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* For the crash handler: one write(), nothing else. `line` ends in \n. */
void xemu_dev_journal_raw(const char *line);
/* At startup: the S line (with the build from dev-build.txt). */
void xemu_dev_start(void);
/* This build's id ("6e4acd668c91"), or "" */
const char *xemu_dev_build(void);

/* The stress test is on: the crash handler starts XPSemu again. */
extern volatile int xemu_dev_stress_on;
/* The bot plays (a game runs, the dashboard is down, stress test on). */
extern volatile int xemu_dev_bot_on;

struct ControllerState;
/* After the pad was read: the bot's buttons and sticks, unless the player
 * pressed something in the last 10 seconds. */
void xemu_dev_bot(struct ControllerState *state);

#ifdef __cplusplus
}
#endif

#endif
