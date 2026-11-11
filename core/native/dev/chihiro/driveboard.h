/*
 * Sega 838-13683 drive board (OutRun 2's force feedback board), on the SC's MIDI line.
 *
 * Ported to XPSemu from the tattoohanz xemu fork's hw/xbox/chihiro/chihiro-driveboard-sega838.c/.h
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version. This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU Lesser General Public License for more details. You should have received a copy of the GNU Lesser
 * General Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * XPSemu changes: no QEMU (plain C), the host side (FFB to the DualSense) not ported yet, a short log.
 */
#ifndef NATIVE_DEV_CHIHIRO_DRIVEBOARD_H
#define NATIVE_DEV_CHIHIRO_DRIVEBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRIVEBOARD_RESP_SIZE 8

typedef struct DriveBoardState {
    uint8_t tx_buf[4];
    int tx_pos;
    uint8_t resp_buf[DRIVEBOARD_RESP_SIZE];
    int resp_head, resp_tail;
    bool initializing;      /* from RESET (0xFF) to the first poll (0xFD) or start (0xFC): status 1 */
    bool motor_active;      /* 0x80: 00 00 = off */
    uint8_t global_power;   /* 0x83 */
    bool spring_active;     /* 0x87 */
    uint8_t damper_level;   /* 0x88 P2 */
    uint8_t friction_power; /* 0x86 P2 */
    uint8_t vibration_power;/* 0x85/0x89 P2 */
    uint8_t movement_dir, movement_power;   /* 0x84 */
    uint8_t sud_pkg[16][16];/* effect packages, uploaded by 0x9D/0x9E, played by 0xFB */
    uint8_t sud_up_pkg, sud_up_idx;
    int8_t play_pkg;
    uint8_t play_pos, play_sub;
    uint32_t cmds;          /* XPSemu: commands taken (log) */
    uint8_t event_pulse;    /* the package movement playing now, 0-0x7F */
    uint64_t play_ms;       /* patch 209: when the playback last stepped */
} DriveBoardState;

void driveboard_init(DriveBoardState *db);
void driveboard_receive_byte(DriveBoardState *db, uint8_t byte);
bool driveboard_has_response(DriveBoardState *db);
uint8_t driveboard_get_response(DriveBoardState *db);
/* patch 209: the rumble now, 0-0xFFFF, the package playback stepped by the time (a movement every 33 ms, the
 * fork's two UI frames) */
uint16_t driveboard_rumble(DriveBoardState *db, uint64_t now_ms);

extern DriveBoardState *chihiro_driveboard_global;

#endif
