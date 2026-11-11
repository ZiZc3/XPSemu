/*
 * Tamura (SAXA) HW210 IC card reader (Ghost Squad), on the SC's UARTs.
 *
 * Ported to XPSemu from the tattoohanz xemu fork's hw/xbox/chihiro/chihiro-cardreader-hw210.c/.h and the slot
 * logic of its chihiro.c (chihiro_hw210_card_key, chihiro_card_reader_tick)
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version. This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU Lesser General Public License for more details. You should have received a copy of the GNU Lesser
 * General Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * XPSemu changes: no QEMU (plain C); the card files written through chihiro_usb.c's save thread
 * (chihiro_card_store) and read back through it (chihiro_card_read); each slot's card is a fixed file
 * (/data/xpsemu/chihiro/run/<gameid>.card<N>.bin); notifications go to the log.
 *
 * The cabinet's card drawer is the Sega CTF-1150 "IC RW UNIT" (HW210 reader, solenoid, photo interrupter); the
 * DX cabinet carries two. The protocol was read off the SC firmware and the games (the fork's notes).
 */
#ifndef NATIVE_DEV_CHIHIRO_CARDREADER_HW210_H
#define NATIVE_DEV_CHIHIRO_CARDREADER_HW210_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CARD_BLOCK_SIZE 8
#define CARD_TOTAL_SIZE 2048

/* A fresh card carries the factory header the game validates in blocks 4 to 6, which it never writes. */
#define CARD_STOCK_OFFSET 32
#define CARD_STOCK_BYTES  24

typedef struct {
    uint8_t header[CARD_STOCK_BYTES];  /* blocks 4 to 6 of a fresh card */
    bool    numbered;                  /* the maker numbers each card in block 0 (Gundam B.O.S.) */
} CardStock;

typedef struct {
    /* A full-card WRITE: 10 header bytes, 2048 of data and a checksum. */
    uint8_t rx_buf[CARD_TOTAL_SIZE + 32];
    int rx_pos;
    int rx_expected;

    uint8_t tx_buf[2048 + 8];
    int tx_len;
    int tx_pos;

    bool card_present;
    uint8_t card_data[CARD_TOTAL_SIZE];
    bool dirty;
    char card_path[512];
} CardReaderState;

void card_reader_init(CardReaderState *s);
void card_reader_insert(CardReaderState *s, const char *path, const CardStock *stock);
void card_reader_remove(CardReaderState *s);
void card_reader_write_byte(CardReaderState *s, uint8_t byte);
int  card_reader_read(CardReaderState *s, uint8_t *buf, int max_len);
bool card_reader_has_response(CardReaderState *s);
bool card_reader_flush(CardReaderState *s); /* true once the file has it (queued to the save thread) */

/* The two readers, driven by the SC 8051's UARTs: [0] on MIDI/UART1 (player 1), [1] on RS-232C/UART0
 * (player 2). NULL when the cabinet has none. */
extern CardReaderState *chihiro_card_reader_global;
/* This cabinet carries HW210 slots (Ghost Squad): the SC MIDI channel (UART1) belongs to the reader then, not a
 * drive board. */
extern bool chihiro_hw210_enabled;

/* The slots around the readers (the fork's chihiro.c): Card In pushes the slot's card; a game waiting for a card
 * locks it (a JVS output) and ejects it by letting go; a card no game locks springs back after 30 switch reads. */
typedef struct {
    CardReaderState reader[2];
    int32_t  slot[2];           /* HW210_EMPTY / PUSHED / LOCKED */
    uint32_t pushed_at[2];      /* chihiro_jvs_switch_reads at the push */
    char     path[2][512];      /* each slot's card file */
    const CardStock *stock;
} ChihiroHW210;

void hw210_setup(ChihiroHW210 *h, const char *path_p1, const char *path_p2, const CardStock *stock);
void hw210_card_key(ChihiroHW210 *h, int player);
/* The slot's card-insertion microswitch (a JVS input on Ghost Squad: player 1 byte 1 bit 5). */
bool hw210_card_present(const ChihiroHW210 *h, int player);
/* The slots' clock: the cabinet's 16 ms service tick. */
void hw210_tick(ChihiroHW210 *h);
extern ChihiroHW210 *chihiro_hw210_global;
/* Ghost Squad's fresh-card header (the fork's cabinet table, vsg.xbe: read off real cards). */
extern const CardStock chihiro_card_stock_vsg;

/* The card files (chihiro_usb.c): a write is queued to the save thread (the PS5's app writes are slow); a read
 * sees a write still queued. chihiro_card_read: the size, -1 no file, -2 bigger than cap. */
bool chihiro_card_store(const char *path, const uint8_t *data, size_t len);
long chihiro_card_read(const char *path, uint8_t *buf, long cap);

#endif
