/*
 * Sega 838-13683 drive board (OutRun 2's force feedback board), on the SC's MIDI line.
 *
 * Ported to XPSemu from the tattoohanz xemu fork's hw/xbox/chihiro/chihiro-driveboard-sega838.c
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version. This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU Lesser General Public License for more details. You should have received a copy of the GNU Lesser
 * General Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "native/dev/chihiro/driveboard.h"
#include "native/core/nc.h"

#include <string.h>

#define TAG "hakuX-native-chihiro"

DriveBoardState *chihiro_driveboard_global = NULL;

static void push(DriveBoardState *db, uint8_t val)
{
    const int next = (db->resp_tail + 1) % DRIVEBOARD_RESP_SIZE;
    if (next == db->resp_head) {
        return;     /* the answer FIFO full: dropped */
    }
    db->resp_buf[db->resp_tail] = val;
    db->resp_tail = next;
}

void driveboard_init(DriveBoardState *db)
{
    memset(db, 0, sizeof(*db));
    db->play_pkg = -1;
}

void driveboard_receive_byte(DriveBoardState *db, uint8_t byte)
{
    if (byte & 0x80) {
        db->tx_pos = 0;
    }
    db->tx_buf[db->tx_pos++] = byte;
    if (db->tx_pos < 4) {
        return;
    }
    db->tx_pos = 0;

    const uint8_t cmd = db->tx_buf[0] & 0x7F, p1 = db->tx_buf[1], p2 = db->tx_buf[2];
    if (db->cmds++ < 60) {
        nc_log(TAG, "drive board: command %02x %02x %02x %02x", db->tx_buf[0], p1, p2, db->tx_buf[3]);
    }
    switch (cmd) {
    /* init / handshake: the answer is the board status (bits 0-2, mirrored in 4-6): 1 while it initializes,
     * 0 in service (the fork: OutRun 2 re-sends SET_MODE forever on a 0 after RESET; later a 1 is the ENCODER
     * ALARM). The first poll marks it in service. */
    case 0x7F:  /* 0xFF: motor reset */
        db->initializing = true;
        push(db, 0x11);
        break;
    case 0x01:  /* 0x81: SET_MODE */
        push(db, db->initializing ? 0x11 : 0x00);
        break;
    case 0x7C:  /* 0xFC: start (no answer) */
        db->initializing = false;
        break;
    case 0x7D:  /* 0xFD: poll / watchdog */
        db->initializing = false;
        push(db, 0x00);
        break;
    case 0x00:  /* 0x80: motor power, 00 00 = off */
        db->motor_active = !(p1 == 0 && p2 == 0);
        if (!db->motor_active) {
            db->spring_active = false;
            db->damper_level = db->friction_power = db->vibration_power = db->movement_power = 0;
            db->play_pkg = -1;
        }
        push(db, 0x00);
        break;
    case 0x03:  /* 0x83: global power */
        db->global_power = p1;
        push(db, 0x00);
        break;
    case 0x04:  /* 0x84: movement */
        db->movement_dir = p1;
        db->movement_power = p2;
        push(db, 0x00);
        break;
    case 0x05:  /* 0x85, 0x89: vibration */
    case 0x09:
        db->vibration_power = p2;
        push(db, 0x00);
        break;
    case 0x06:  /* 0x86: constant torque (P2 the force) */
        db->friction_power = p2;
        push(db, 0x00);
        break;
    case 0x07:  /* 0x87: spring (centering) */
        db->spring_active = true;
        push(db, 0x00);
        break;
    case 0x08:  /* 0x88: damper */
        db->damper_level = p2;
        push(db, 0x00);
        break;
    case 0x1D:  /* 0x9D: package upload cursor */
        db->sud_up_idx = p1;
        db->sud_up_pkg = p2;
        push(db, 0x00);
        break;
    case 0x1E:  /* 0x9E: package movement */
        if (db->sud_up_pkg < 16 && db->sud_up_idx < 16) {
            db->sud_pkg[db->sud_up_pkg][db->sud_up_idx] = (uint8_t)((p1 << 7) | (p2 & 0x7F));
        }
        push(db, 0x00);
        break;
    case 0x7B:  /* 0xFB: package playback */
        db->play_pkg = (int8_t)(p2 & 0x0F);
        db->play_pos = db->play_sub = 0;
        db->play_ms = 0;
        push(db, 0x00);
        break;
    default:    /* 0x8B vibration, 0x82, 0x8A, 0xF0, 0xFA...: ack */
        push(db, 0x00);
        break;
    }
}

uint16_t driveboard_rumble(DriveBoardState *db, uint64_t now_ms)
{
    if (!db->motor_active) {
        db->event_pulse = 0;
        return 0;
    }
    uint8_t power = 0;
    if (db->play_pkg >= 0 && db->play_pkg < 16) {
        if (!db->play_ms) {
            db->play_ms = now_ms;
        }
        const uint8_t step = db->sud_pkg[db->play_pkg][db->play_pos & 0x0F];
        power = step & 0x7F;
        if (now_ms - db->play_ms >= 33) {
            db->play_ms = now_ms;
            if (++db->play_pos >= 16) {
                db->play_pkg = -1;      /* the package played out */
                db->play_ms = 0;
            }
        }
    }
    db->event_pulse = power;
    /* a pad can only buzz: the event packages and the explicit vibration (the fork's choice; the road's
     * continuous 0x8B buzz, centring and damping are the wheel's), boosted: power ~0x54 is full */
    uint32_t mag = db->event_pulse, vib = (uint32_t)db->vibration_power * 2;
    if (vib > mag) {
        mag = vib;
    }
    const uint32_t r = mag * 780;
    return (uint16_t)(r > 0xFFFF ? 0xFFFF : r);
}

bool driveboard_has_response(DriveBoardState *db)
{
    return db->resp_head != db->resp_tail;
}

uint8_t driveboard_get_response(DriveBoardState *db)
{
    if (db->resp_head == db->resp_tail) {
        return 0x00;
    }
    const uint8_t v = db->resp_buf[db->resp_head];
    db->resp_head = (db->resp_head + 1) % DRIVEBOARD_RESP_SIZE;
    return v;
}
