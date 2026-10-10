/*
 * XPSemu: dev builds
 *
 * 1 in dev builds (ps5/build-dev.sh sets it while it builds), 0 in releases.
 * A dev build keeps a crash journal (/data/xemu/dev/journal.txt), has a
 * stress test that plays every game in turn with a button bot, and shows
 * how stable it was on the System page. None of it is in a release.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XPSEMU_DEV_H
#define XPSEMU_DEV_H

#define XPSEMU_DEV 0

#endif
