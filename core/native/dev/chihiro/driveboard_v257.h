/*
 * Namco V257 STR PCB (Maximum Tune's steering and force feedback board), on the SC's MIDI line (UART1).
 *
 * Ported to XPSemu from the tattoohanz xemu fork's hw/xbox/chihiro/chihiro-driveboard-v257.c/.h
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version. This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU Lesser General Public License for more details. You should have received a copy of the GNU Lesser
 * General Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * XPSemu changes: no QEMU (plain C), a short log; the 16 ms service tick comes from chihiro_usb.c (every 16 USB
 * frames), the motor line from jvs.c (GPO bank 0 bit 7), the wheel from the left stick.
 *
 * Host -> board: a 10-byte frame (V322.xbe 0x00055E60): FF FF | pos_lo pos_hi | force_a 00 | force_b 00 | torque 00
 * (pos 0-0x3FF; force_a spring, force_b damping 0-0x7F; torque a short push, direction in bit 7, size complemented
 * when set). Board -> host: 'H' hi lo the wheel position (also the ack for the next frame); 'C01' motor powered,
 * 'C06' unpowered, 'E00' self-test passed, each status sent twice. The boot wheel check raises the motor line,
 * waits for C01, holds 2 s, drops it and waits for C06 (20 s on any step: the game's own E20). The board's
 * firmware (rrv3_str-0a.ic16) gives the timings; the fork's notes have the rest.
 */
#ifndef NATIVE_DEV_CHIHIRO_DRIVEBOARD_V257_H
#define NATIVE_DEV_CHIHIRO_DRIVEBOARD_V257_H

#include <stdbool.h>
#include <stdint.h>

#define V257_CMD_LEN   10
#define V257_MSG_LEN   3
#define V257_MSG_SLOTS 4
#define V257_RESP_SIZE 16

/* The board's own condition, as its firmware steps through it. */
enum {
    V257_BOOTING = 0,   /* just powered: serial off, waits for the motor line */
    V257_SELF_TEST,     /* line up: powering up, then reports E00 */
    V257_BOOT_WAIT_LOW, /* E00 sent, waits for the line to drop, then C06 */
    V257_OFF,           /* main loop, line low: target 0x1FF, no force */
    V257_POWERING,      /* line up: frames stored, not answered */
    V257_POWERED,       /* C01 sent: the forces act */
};

typedef struct V257DriveBoard {
    uint8_t  cmd[V257_CMD_LEN];
    int      cmd_pos;

    uint8_t  pending[V257_MSG_SLOTS][V257_MSG_LEN];
    int      pending_head;
    int      pending_tail;

    uint8_t  resp[V257_RESP_SIZE];
    int      resp_head;
    int      resp_tail;

    uint16_t position;   /* the target the board holds, 0-0x3FF */
    uint16_t wheel;      /* where the wheel actually is, 0-0x3FF */
    bool     motor_on;   /* the cabinet relay, as the JVS output drives it */
    bool     motor_known;
    uint8_t  force_a;    /* the spring, as the board scales it: 0-0x7F */
    uint8_t  force_b;    /* the damping, 0-0x7F */
    int8_t   torque;     /* the push, -0x7F-0x7F */

    bool     pending_twice[V257_MSG_SLOTS];
    uint8_t  state;      /* V257_* */
    int      countdown;  /* service ticks left in a power-up or self-test */
    int      torque_ticks;
    uint32_t logged;     /* XPSemu: log lines so far */
} V257DriveBoard;

/* What the board drives the wheel with: the target, the spring and damping (0-0x7F) and the push (signed, 0
 * between pushes). False while it drives nothing. */
bool    v257_drive(V257DriveBoard *db, int *target, int *spring, int *damping, int *torque);
void    v257_init(V257DriveBoard *db);
/* One step of the board, on the cabinet's 16 ms service tick. */
void    v257_tick(V257DriveBoard *db);
/* The motor line, a JVS general-purpose output. */
void    v257_set_motor(V257DriveBoard *db, bool on);
/* Where the wheel is, 0-0x3FF (a real V257 reads the column's encoder). */
void    v257_set_wheel(V257DriveBoard *db, uint16_t pos);
void    v257_receive_byte(V257DriveBoard *db, uint8_t byte);
bool    v257_has_response(V257DriveBoard *db);
uint8_t v257_get_response(V257DriveBoard *db);

/* Maximum Tune's board, NULL on every other cabinet. */
extern V257DriveBoard *chihiro_v257_global;

#endif
