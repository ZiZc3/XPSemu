/*
 * Sanwa CRP-1231LR-10NAB card reader-printer (Maximum Tune 1/2), on the SC's UART0 (RS-232C).
 *
 * Ported to XPSemu from the tattoohanz xemu fork's hw/xbox/chihiro/chihiro-cardreader-crp1231.c/.h and the card
 * key of its chihiro.c (chihiro_crp1231_card_key)
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version. This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU Lesser General Public License for more details. You should have received a copy of the GNU Lesser
 * General Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * XPSemu changes: no QEMU (plain C); the clean's clock is CLOCK_MONOTONIC; the card files go through
 * chihiro_usb.c (chihiro_card_store / chihiro_card_read / chihiro_crp1231_card_issue); notifications to the log.
 * A high-level model of the reader's answers: its firmware ROM (crp1231lr10_ver0110.ic2) is not run, the fork
 * only read it to learn the answers.
 *
 * The protocol (the fork's notes, V322.xbe and the reader's firmware): ASCII framed by STX and ETX:
 *   host -> reader   02 | LEN | CMD | params... | 03 | SUM
 *   reader -> host   06 (ACK), then on the host's ENQ: 02 | 06 | CMD | s3 | s4 | s5 | 03 | SUM
 * LEN is the frame length minus two; SUM is LEN XOR every byte from CMD through ETX; a bad frame gets 15 (NAK).
 * s3 where the card is, s4 the error digit, s5 '3' busy / '4' waiting. A card is 69 bytes both ways: the write
 * command (0x53) carries them after three parameter digits; the 77-byte answer to 0x33 '0' carries them.
 */
#ifndef NATIVE_DEV_CHIHIRO_CARDREADER_CRP1231_H
#define NATIVE_DEV_CHIHIRO_CARDREADER_CRP1231_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRP1231_CMD_MAX    257
#define CRP1231_RESP_MAX   128
#define CRP1231_CARD_BYTES 69

/* Where the card is, as the reader's path sensors see it: '0' nowhere or just pushed into the mouth, '1' drawn
 * in, '4' handed back and waiting at the mouth to be taken. */
enum {
    CRP1231_CARD_NONE = 0,
    CRP1231_CARD_IN,
    CRP1231_CARD_GATE,
    CRP1231_CARD_ENTRY,     /* pushed into the mouth by the player */
};

typedef struct CRP1231State {
    uint8_t cmd[CRP1231_CMD_MAX];       /* the frame, whole until the next STX */
    int     cmd_len;
    int     cmd_expect;

    uint8_t resp[CRP1231_RESP_MAX];
    int     resp_len;
    int     resp_pos;

    uint8_t last_frame[80];             /* the answer, sent when the host asks with ENQ */
    int     last_len;
    bool    have_frame;
    bool    answer_pending;

    uint8_t card[CRP1231_CARD_BYTES];
    int     card_pos;                   /* CRP1231_CARD_* */
    bool    card_written;
    char    card_path[512];
    bool    waiting;                    /* a take-in command is waiting for a card */

    uint8_t last_cmd;
    uint8_t last_param;
    bool    cleaning;
    int64_t clean_start;                /* ms, CLOCK_MONOTONIC */
    uint32_t logged;                    /* XPSemu: log lines so far */
} CRP1231State;

void crp1231_init(CRP1231State *r);
/* The player pushes the card from this file (a blank one when the file is new or empty) into the reader's mouth;
 * false, with nothing pushed, when the file is not a card. */
bool crp1231_offer(CRP1231State *r, const char *path);
/* The player takes back the card still sitting in the mouth. */
void crp1231_take_back(CRP1231State *r);
void crp1231_receive_byte(CRP1231State *r, uint8_t byte);
bool crp1231_has_response(CRP1231State *r);
bool crp1231_read(CRP1231State *r, uint8_t *out);
/* True once the reader has started sending the answer it owes. */
bool crp1231_mid_response(CRP1231State *r);
/* The Card In key (the fork's chihiro_crp1231_card_key): the card in hand goes into the mouth; a second press
 * takes back a card still in the mouth. */
void crp1231_card_key(CRP1231State *r, const char *hand);

/* This cabinet has a CRP-1231 (Maximum Tune) on SC UART0. */
extern bool chihiro_crp1231_enabled;
extern CRP1231State *chihiro_crp1231_global;

/* chihiro_usb.c: a file for a blank card that has none (one the dispenser fed); the card in hand becomes it. */
bool chihiro_crp1231_card_issue(char *out, size_t out_len);

#endif
