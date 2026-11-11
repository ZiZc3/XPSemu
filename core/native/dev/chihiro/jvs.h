/*
 * Chihiro JVS I/O Board Emulation (Sega 837-13551)
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * XPSemu (hakuX native core, Sega Chihiro phase 3): adapted from tattoohanz's
 * xemu-based Chihiro fork (hw/xbox/chihiro/chihiro-jvs.h): no QEMU, no
 * cabinet outputs (drive boards, card locks) yet, and an input setter.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef NATIVE_DEV_CHIHIRO_JVS_H
#define NATIVE_DEV_CHIHIRO_JVS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JVS_MAX_PLAYERS    2
#define JVS_MAX_COINS      2
#define JVS_MAX_ANALOG     8

#define JVS_SYNC           0xE0
#define JVS_BROADCAST      0xFF
#define JVS_HOST_ADDR      0x00

#define JVS_STATUS_OK      0x01

#define JVS_REPORT_OK      0x01
#define JVS_REPORT_PARAM   0x02

typedef struct {
    uint8_t  device_id;
    uint8_t  sense;                    /* 3=unassigned, 0=addressed */
    uint16_t coin_count[JVS_MAX_COINS];
    uint8_t  reset_count;              /* must reach 2 for actual reset */

    uint8_t  system_switches;          /* test=0x80, tilt1=0x40 ... */
    uint8_t  player_switches[JVS_MAX_PLAYERS][2];
    uint16_t analog[JVS_MAX_ANALOG];   /* 16-bit, 0x8000 = center */
} ChihiroJVSState;

/* The QC's board (the one whose sense line the QC reads), or NULL. */
extern ChihiroJVSState *chihiro_jvs_global;

void chihiro_jvs_init(ChihiroJVSState *s);

/*
 * Process a raw JVS frame (starting with 0xE0 sync).
 * Returns the number of bytes written to out[], or 0 if no response
 * (broadcast commands like Reset produce no response).
 */
int  chihiro_jvs_process(ChihiroJVSState *s,
                          const uint8_t *in, int in_len,
                          uint8_t *out, int out_max);

/* XPSemu: the inputs the board reports, set from any thread (taken into the
 * board at its next switch read). system: test 0x80, tilt 0x40..; player
 * byte 0: start 0x80, service 0x40, up 0x20, down 0x10, left 0x08,
 * right 0x04, push1 0x02, push2 0x01; byte 1: push3 0x80 .. push8 0x04.
 * Coins add to the slot's counter. */
void nc_chihiro_jvs_set(uint8_t system, const uint8_t p1[2], const uint8_t p2[2],
                        const uint16_t analog[JVS_MAX_ANALOG]);
void nc_chihiro_jvs_coin(unsigned slot);

/* XPSemu patch 225 (the fork's): the card drawers' solenoids on an HW210 cabinet, one per slot, as the game last
 * set them (GPO bank 0); the bit that drives each (0: no slot); how many times the game read the switches. */
extern bool chihiro_jvs_card_lock[2];
extern uint8_t chihiro_jvs_card_lock_bits[2];
extern uint32_t chihiro_jvs_switch_reads;

#ifdef __cplusplus
}
#endif

#endif
