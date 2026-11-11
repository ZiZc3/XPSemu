/*
 * Sanwa CRP-1231LR-10NAB card reader-printer (Maximum Tune 1/2), on the SC's UART0 (see cardreader_crp1231.h).
 *
 * Ported to XPSemu from the tattoohanz xemu fork's hw/xbox/chihiro/chihiro-cardreader-crp1231.c
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version. This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU Lesser General Public License for more details. You should have received a copy of the GNU Lesser
 * General Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "native/dev/chihiro/cardreader_crp1231.h"
#include "native/dev/chihiro/cardreader_hw210.h"   /* chihiro_card_store / chihiro_card_read */
#include "native/core/nc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "hakuX-native-chihiro"

#define STX 0x02
#define ETX 0x03
#define ENQ 0x05
#define ACK 0x06
#define NAK 0x15

#define CRP1231_CMD_CANCEL     0x40  /* V322.xbe 0x00057D20 */
#define CRP1231_CMD_PRINT_TEXT 0x7C  /* 0x0005BAF0 */
#define CRP1231_CMD_RETURN     0x80  /* 0x000586A0, hand the card back */
#define CRP1231_CMD_DISPENSE   0xB0  /* 0x00058A10 */
#define CRP1231_CMD_CARD       0x33  /* 0x00057E70 ('2' take in) and 0x00057F30 ('0' read) */
#define CRP1231_CMD_WRITE      0x53  /* 'S', 0x00058730 */
#define CRP1231_CMD_CLEAN      0xA0  /* 0x00058000 */

#define LOG_MAX 300

/* A card starts with the signature of the game that wrote it: Maximum Tune 2 reads 1's cards, not the reverse;
 * a refused card is handed back and asked for again (the fork's notes). */

CRP1231State *chihiro_crp1231_global = NULL;
bool chihiro_crp1231_enabled;

static void crp1231_respond(CRP1231State *r, uint8_t cmd, uint8_t param);

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_hex(CRP1231State *r, const char *what, const uint8_t *b, int n)
{
    if (r->logged >= LOG_MAX) {
        return;
    }
    r->logged++;
    char hex[3 * 40 + 8];
    int o = 0;
    const int show = n < 40 ? n : 40;
    for (int i = 0; i < show && o < (int)sizeof(hex) - 4; i++) {
        o += snprintf(hex + o, sizeof(hex) - (size_t)o, " %02x", b[i]);
    }
    if (n > show) {
        snprintf(hex + o, sizeof(hex) - (size_t)o, " ..");
    }
    nc_log(TAG, "CRP-1231: %s (%d):%s", what, n, hex);
}

static uint8_t crp1231_sum(const uint8_t *frame, int len)
{
    /* Seeded with LEN, then every byte from the command through the ETX. */
    uint8_t sum = frame[1];
    for (int i = 2; i <= len - 2; i++) {
        sum ^= frame[i];
    }
    return sum;
}

static void crp1231_out(CRP1231State *r, const uint8_t *buf, int len)
{
    if (r->resp_len + len > CRP1231_RESP_MAX) {
        return;
    }
    memcpy(&r->resp[r->resp_len], buf, (size_t)len);
    r->resp_len += len;
}

/* The reader answers the games' commands in one shape; only the echoed command and the three status digits
 * change. */
static void crp1231_answer(CRP1231State *r, uint8_t cmd, uint8_t s3, uint8_t s4, uint8_t s5)
{
    uint8_t f[8] = {STX, 0x06, cmd, s3, s4, s5, ETX, 0};
    f[7] = crp1231_sum(f, 8);
    memcpy(r->last_frame, f, sizeof(f));
    r->last_len = sizeof(f);
    r->have_frame = true;
}

/* The one long answer: the card's 69 bytes. LEN 0x4B is also how the game tells it from the short one. */
static void crp1231_answer_card(CRP1231State *r)
{
    uint8_t f[77];
    f[0] = STX;
    f[1] = 0x4B;
    f[2] = CRP1231_CMD_CARD;
    f[3] = '1';   /* a card is in the reader */
    f[4] = '0';   /* no error */
    f[5] = '0';   /* done */
    memcpy(&f[6], r->card, CRP1231_CARD_BYTES);
    f[75] = ETX;
    f[76] = crp1231_sum(f, sizeof(f));
    memcpy(r->last_frame, f, sizeof(f));
    r->last_len = sizeof(f);
    r->have_frame = true;
}

/* The card has left the reader: it is back in the player's hand, where it is its file again. */
static void crp1231_card_gone(CRP1231State *r)
{
    r->card_pos = CRP1231_CARD_NONE;
    r->card_written = false;
    memset(r->card, 0, sizeof(r->card));
    r->card_path[0] = '\0';
}

/* Take up the card a file holds, a blank one for an empty path or a file that does not exist yet. False when
 * the file is not a card. */
static bool crp1231_load(CRP1231State *r, const char *path)
{
    crp1231_card_gone(r);
    if (!path || !path[0]) {
        return true;
    }
    const long n = chihiro_card_read(path, r->card, CRP1231_CARD_BYTES);
    if (n != -1 && n != 0 && n != CRP1231_CARD_BYTES) {
        nc_log(TAG, "CRP-1231: not a card, %ld bytes where a card is %d: %s (left alone)", n, CRP1231_CARD_BYTES,
               path);
        memset(r->card, 0, sizeof(r->card));
        return false;
    }
    if (n == CRP1231_CARD_BYTES) {
        /* A blank stripe reads as nothing, so a file of zeroes is a blank. */
        for (int i = 0; i < CRP1231_CARD_BYTES; i++) {
            if (r->card[i]) {
                r->card_written = true;
                break;
            }
        }
    }
    if (!r->card_written) {
        memset(r->card, 0, sizeof(r->card));
    }
    snprintf(r->card_path, sizeof(r->card_path), "%s", path);
    return true;
}

bool crp1231_offer(CRP1231State *r, const char *path)
{
    if (!crp1231_load(r, path)) {
        return false;
    }
    r->card_pos = CRP1231_CARD_ENTRY;
    /* A take-in waiting for a card draws it in the moment it covers the mouth sensor, and its answer is ready for
     * the game's next ENQ. */
    if (r->waiting) {
        crp1231_respond(r, r->last_cmd, r->last_param);
    }
    return true;
}

void crp1231_take_back(CRP1231State *r)
{
    if (r->card_pos == CRP1231_CARD_ENTRY) {
        crp1231_card_gone(r);
    }
}

static void crp1231_save(CRP1231State *r)
{
    /* A blank from the dispenser becomes a card of its own the first time the game writes it. */
    if (!r->card_path[0] && !chihiro_crp1231_card_issue(r->card_path, sizeof(r->card_path))) {
        nc_log(TAG, "CRP-1231: no file could be made for the new card: not saved");
        return;
    }
    chihiro_card_store(r->card_path, r->card, CRP1231_CARD_BYTES);
}

/* The weekly clean. SIMPLIFIED (the fork's): there is no cleaning card to push in; the reader cleans as if one had
 * been, 1 s to take it, 3 s to clean, 1 s at the mouth. */
static void crp1231_clean(CRP1231State *r, uint8_t *s3, uint8_t *s5)
{
    const int64_t t = now_ms() - r->clean_start;
    if (t < 1000) {
        *s5 = '4';
    } else if (t < 4000) {
        *s5 = '3';
    } else if (t < 5000) {
        *s3 = '4';
    } else {
        r->cleaning = false;
    }
}

static void crp1231_status(CRP1231State *r, uint8_t cmd, uint8_t param, uint8_t *s3, uint8_t *s4, uint8_t *s5)
{
    const bool in = r->card_pos == CRP1231_CARD_IN;

    switch (cmd) {
    case CRP1231_CMD_CARD:
        if (param != '0' && param != '2') {
            *s5 = '2';       /* a parameter the games never send */
        } else if (r->waiting) {
            *s5 = '4';
        } else if (param == '0' && !r->card_written) {
            *s4 = '1';       /* nothing on the stripe */
        }
        break;
    case CRP1231_CMD_CANCEL:
        if (r->waiting || r->cleaning) {
            r->waiting = false;
            r->cleaning = false;
        } else {
            *s5 = '2';
        }
        break;
    case CRP1231_CMD_CLEAN:
        if (!r->cleaning) {
            r->cleaning = true;
            r->clean_start = now_ms();
        }
        break;
    case CRP1231_CMD_DISPENSE:
        if (in) {
            *s5 = '2';
            break;
        }
        /* SIMPLIFIED (the fork's): a card left in the mouth goes back to the player's hand instead of reading as a
         * jam. */
        if (r->card_pos == CRP1231_CARD_ENTRY) {
            nc_log(TAG, "CRP-1231: card out (the dispenser feeds a new one)");
        }
        crp1231_card_gone(r);
        r->card_pos = CRP1231_CARD_IN;
        break;
    case CRP1231_CMD_RETURN:
        if (r->card_pos != CRP1231_CARD_NONE) {
            r->card_pos = CRP1231_CARD_GATE;
        }
        break;
    case CRP1231_CMD_WRITE:
    case CRP1231_CMD_PRINT_TEXT:
        if (!in) {
            *s5 = '2';
        }
        break;
    default:
        break;
    }

    switch (r->card_pos) {
    case CRP1231_CARD_IN:
        *s3 = '1';
        break;
    case CRP1231_CARD_GATE:
        *s3 = '4';
        break;
    default:
        *s3 = '0';
        break;
    }

    if (r->cleaning) {
        crp1231_clean(r, s3, s5);
    }
}

/* Say where things stand, for a command or for an ENQ asking again. */
static void crp1231_respond(CRP1231State *r, uint8_t cmd, uint8_t param)
{
    /* A card handed back sits at the gate for exactly one answer: by the time the host asks anything else the
     * player has taken it. */
    if (r->card_pos == CRP1231_CARD_GATE) {
        crp1231_card_gone(r);
        nc_log(TAG, "CRP-1231: card out (handed back)");
    }

    r->last_cmd = cmd;
    r->last_param = param;

    /* A take-in draws in the card sitting in the mouth, and waits when there is none; any other command ends the
     * wait but the one that cancels it. */
    if (cmd == CRP1231_CMD_CARD && (param == '0' || param == '2')) {
        if (r->card_pos == CRP1231_CARD_ENTRY) {
            r->card_pos = CRP1231_CARD_IN;
            nc_log(TAG, "CRP-1231: card drawn in (%s)", r->card_path[0] ? r->card_path : "blank");
        }
        r->waiting = r->card_pos == CRP1231_CARD_NONE;
    } else if (cmd != CRP1231_CMD_CANCEL) {
        r->waiting = false;
    }

    /* A read of a written card answers with its 69 bytes. */
    if (cmd == CRP1231_CMD_CARD && param == '0' && r->card_pos == CRP1231_CARD_IN && r->card_written) {
        crp1231_answer_card(r);
        return;
    }

    uint8_t s3 = '0', s4 = '0', s5 = '0';
    crp1231_status(r, cmd, param, &s3, &s4, &s5);
    crp1231_answer(r, cmd, s3, s4, s5);
}

void crp1231_init(CRP1231State *r)
{
    memset(r, 0, sizeof(*r));
}

void crp1231_receive_byte(CRP1231State *r, uint8_t byte)
{
    /* Outside a frame the host only ever sends ENQ, asking where things stand, or opens a frame with STX. */
    if (r->cmd_len == 0) {
        if (byte == ENQ || byte == NAK) {
            /* A valid command gets an ACK only; the answer goes out on the host's ENQ. At rest, ENQ and NAK replay
             * the last answer, except for a card at the gate or a clean under way: worked out again. */
            if (!r->have_frame) {
                return;
            }
            /* Still shifting the previous bytes out: a real reader does not restart a transmission. */
            if (crp1231_has_response(r)) {
                return;
            }
            if (!r->answer_pending && (r->card_pos == CRP1231_CARD_GATE || r->cleaning)) {
                crp1231_respond(r, r->last_cmd, r->last_param);
            }
            r->answer_pending = false;
            crp1231_out(r, r->last_frame, r->last_len);
            return;
        }
        if (byte != STX) {
            return;
        }
    }

    if (r->cmd_len >= CRP1231_CMD_MAX) {
        r->cmd_len = 0;
        r->cmd_expect = 0;
        return;
    }
    r->cmd[r->cmd_len++] = byte;

    if (r->cmd_len == 2) {
        r->cmd_expect = byte + 2;
        if (r->cmd_expect > CRP1231_CMD_MAX || r->cmd_expect < 4) {
            r->cmd_len = 0;
            r->cmd_expect = 0;
        }
        return;
    }
    if (r->cmd_expect == 0 || r->cmd_len < r->cmd_expect) {
        return;
    }

    const int len = r->cmd_expect;
    const uint8_t cmd = r->cmd[2];
    const bool ok = r->cmd[len - 2] == ETX && r->cmd[len - 1] == crp1231_sum(r->cmd, len);

    r->cmd_len = 0;
    r->cmd_expect = 0;

    log_hex(r, ok ? "<- ok" : "<- BAD FRAME", r->cmd, len);

    if (!ok) {
        const uint8_t nak = NAK;
        crp1231_out(r, &nak, 1);
        return;
    }

    /* One answer buffer: a new reply abandons what was left to shift out. */
    r->resp_len = 0;
    r->resp_pos = 0;

    const uint8_t ack = ACK;
    crp1231_out(r, &ack, 1);
    r->answer_pending = true;

    /* The 69 bytes follow the three parameter digits. */
    if (cmd == CRP1231_CMD_WRITE && len >= 9 + CRP1231_CARD_BYTES && r->card_pos == CRP1231_CARD_IN) {
        memcpy(r->card, &r->cmd[9], CRP1231_CARD_BYTES);
        r->card_written = true;
        crp1231_save(r);
        nc_log(TAG, "CRP-1231: card written to %s", r->card_path[0] ? r->card_path : "(no file)");
    }

    crp1231_respond(r, cmd, len >= 7 ? r->cmd[6] : 0);
    log_hex(r, "-> 06, then on ENQ", r->last_frame, r->last_len);
}

bool crp1231_has_response(CRP1231State *r)
{
    return r->resp_pos < r->resp_len;
}

bool crp1231_mid_response(CRP1231State *r)
{
    return r->resp_pos > 0;
}

bool crp1231_read(CRP1231State *r, uint8_t *out)
{
    if (r->resp_pos >= r->resp_len) {
        return false;
    }
    *out = r->resp[r->resp_pos++];
    if (r->resp_pos >= r->resp_len) {
        r->resp_pos = 0;
        r->resp_len = 0;
    }
    return true;
}

void crp1231_card_key(CRP1231State *r, const char *hand)
{
    if (!chihiro_crp1231_enabled || !r) {
        return;
    }
    switch (r->card_pos) {
    case CRP1231_CARD_IN:
        nc_log(TAG, "CRP-1231: card key: the card is in the reader");
        return;
    case CRP1231_CARD_GATE:
        /* Handed back and taken at the game's next look; nothing else goes in meanwhile. */
        nc_log(TAG, "CRP-1231: card key: the card is coming out");
        return;
    case CRP1231_CARD_ENTRY:
        crp1231_take_back(r);
        nc_log(TAG, "CRP-1231: card key: card taken back out of the mouth");
        return;
    default:
        break;
    }
    if (!crp1231_offer(r, hand)) {
        return;
    }
    nc_log(TAG, "CRP-1231: card key: %s card pushed into the mouth (%s)", r->card_written ? "written" : "blank",
           hand && hand[0] ? hand : "no file");
}
