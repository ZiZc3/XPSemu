/*
 * Tamura (SAXA) HW210 IC card reader (Ghost Squad), on the SC's UARTs (see cardreader_hw210.h).
 *
 * Ported to XPSemu from the tattoohanz xemu fork's hw/xbox/chihiro/chihiro-cardreader-hw210.c and the slot
 * logic of its chihiro.c
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version. This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU Lesser General Public License for more details. You should have received a copy of the GNU Lesser
 * General Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "native/dev/chihiro/cardreader_hw210.h"
#include "native/dev/chihiro/jvs.h"
#include "native/core/nc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "hakuX-native-chihiro"

CardReaderState *chihiro_card_reader_global = NULL;
bool chihiro_hw210_enabled;
ChihiroHW210 *chihiro_hw210_global = NULL;

/* Ghost Squad (vsg.xbe): 95 71, type 36 4X, serial 0, version 03 F2 in block 6, read off real cards (the
 * fork's chihiro.c). */
const CardStock chihiro_card_stock_vsg = {
    .header = {0x95, 0x71, 0x36, 0x40, 0, 0, 0, 0,
               0, 0, 0, 0, 0, 0, 0, 0,
               0, 0, 0, 0, 0, 0, 0x03, 0xF2},
    .numbered = false,
};

static unsigned g_logged;

static uint8_t compute_checksum(const uint8_t *buf, int len)
{
    uint8_t chk = 0;
    for (int i = 0; i < len; i++) {
        chk ^= buf[i];
    }
    return chk;
}

static void build_response(CardReaderState *s, uint8_t cmd)
{
    memset(s->tx_buf, 0, sizeof(s->tx_buf));
    s->tx_buf[0] = 0x10;
    s->tx_buf[1] = cmd;

    uint16_t payload_len = 0;

    switch (cmd) {
    case 0x10: /* INIT */
        payload_len = 0x02;
        s->tx_buf[5] = 0x20;
        break;
    case 0x11: /* GET STATUS / RESET */
        payload_len = 0x02;
        s->tx_buf[5] = 0x20;
        break;
    case 0x14: /* reader init, second step (vsg.xbe FUN_000d2a50) */
    case 0x15: /* STANDBY */
    case 0x26:
    case 0x27:
        payload_len = 0x02;
        break;
    case 0x20: /* DETECT CARD */
        payload_len = 0x02;
        if (!s->card_present) {
            s->tx_buf[4] = 0x80;
        }
        break;
    case 0x21: /* GET CARD UID */
        payload_len = 0x0A;
        s->tx_buf[6] = 0x81;
        s->tx_buf[8] = 0x59;
        s->tx_buf[9] = 0xDA;
        break;
    case 0x22: /* GET CARD TYPE */
        payload_len = 0x0A;
        s->tx_buf[7] = 0x08;
        break;
    case 0x33: /* GET CAPACITY */
        payload_len = 0x0A;
        s->tx_buf[6] = 0xFF;
        s->tx_buf[7] = 0xFF;
        break;
    case 0x34: { /* READ BLOCKS */
        const uint16_t block_start = (uint16_t)((s->rx_buf[6] << 8) | s->rx_buf[7]);
        const uint16_t block_count = (uint16_t)((s->rx_buf[8] << 8) | s->rx_buf[9]);
        const uint32_t byte_offset = (uint32_t)block_start * CARD_BLOCK_SIZE;
        const uint32_t byte_count = (uint32_t)block_count * CARD_BLOCK_SIZE;
        if (byte_offset + byte_count <= CARD_TOTAL_SIZE && byte_count + 6 < sizeof(s->tx_buf)) {
            payload_len = (uint16_t)(byte_count + 2);
            memcpy(&s->tx_buf[6], &s->card_data[byte_offset], byte_count);
        } else {
            payload_len = 0x02;
            s->tx_buf[4] = 0x80;
            nc_log(TAG, "HW210: read past the card (block %u, %u blocks)", block_start, block_count);
        }
        break;
    }
    case 0x35: { /* WRITE BLOCKS */
        const uint16_t block_start = (uint16_t)((s->rx_buf[6] << 8) | s->rx_buf[7]);
        const uint16_t block_count = (uint16_t)((s->rx_buf[8] << 8) | s->rx_buf[9]);
        const uint32_t byte_offset = (uint32_t)block_start * CARD_BLOCK_SIZE;
        const uint32_t byte_count = (uint32_t)block_count * CARD_BLOCK_SIZE;
        if (byte_offset + byte_count <= CARD_TOTAL_SIZE && byte_count + 10 <= (uint32_t)sizeof(s->rx_buf)) {
            memcpy(&s->card_data[byte_offset], &s->rx_buf[10], byte_count);
            s->dirty = true;
        }
        payload_len = 0x02;
        break;
    }
    default:
        payload_len = 0x02;
        break;
    }

    s->tx_buf[2] = (uint8_t)(payload_len >> 8);
    s->tx_buf[3] = (uint8_t)(payload_len & 0xFF);
    s->tx_len = payload_len + 5;
    s->tx_pos = 0;
    s->tx_buf[s->tx_len - 1] = compute_checksum(s->tx_buf, s->tx_len - 1);
}

void card_reader_init(CardReaderState *s)
{
    memset(s, 0, sizeof(*s));
}

void card_reader_insert(CardReaderState *s, const char *path, const CardStock *stock)
{
    if (!path || !path[0]) {
        return;
    }
    snprintf(s->card_path, sizeof(s->card_path), "%s", path);

    const long n = chihiro_card_read(path, s->card_data, CARD_TOTAL_SIZE);
    if (n == CARD_TOTAL_SIZE) {
        s->card_present = true;
        s->dirty = false;
        nc_log(TAG, "HW210: card %s in", path);
        return;
    }
    /* Any other size is not a card: taking it in would overwrite it with a 2 KB card. Only an empty file, or
     * none, is a blank. */
    if (n != -1 && n != 0) {
        nc_log(TAG, "HW210: not a card, %ld bytes where a card is %d: %s (left alone)", n, CARD_TOTAL_SIZE, path);
        s->card_path[0] = '\0';
        return;
    }

    /* Missing or empty file: a fresh card from the cabinet's stock, which the game takes as NEW (an all-zero card
     * fails its header check). */
    memset(s->card_data, 0, CARD_TOTAL_SIZE);
    if (stock) {
        memcpy(&s->card_data[CARD_STOCK_OFFSET], stock->header, CARD_STOCK_BYTES);
        if (stock->numbered) {
            /* HEURISTIC (the fork's): the card number is its creation time in microseconds; the game only asks
             * that no two cards share it. */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            const uint64_t number = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
            memcpy(&s->card_data[0], &number, sizeof(number));
        }
    }
    s->card_present = true;
    s->dirty = true;
    card_reader_flush(s);
    nc_log(TAG, "HW210: fresh card written to %s", path);
}

void card_reader_remove(CardReaderState *s)
{
    if (s->dirty) {
        card_reader_flush(s);
    }
    s->card_present = false;
    s->rx_pos = 0;
    s->rx_expected = 0;
    s->tx_len = 0;
    s->tx_pos = 0;
}

void card_reader_write_byte(CardReaderState *s, uint8_t byte)
{
    /* A command opens with 0x00; the reader answers with 0x10. */
    if (s->rx_pos == 0 && byte != 0x00) {
        return;
    }
    if (s->rx_pos == 1 && byte == 0x00) {
        s->rx_pos = 0;
        return;
    }
    if (s->rx_pos >= (int)sizeof(s->rx_buf)) {
        s->rx_pos = 0;
    }
    s->rx_buf[s->rx_pos++] = byte;

    if (s->rx_pos == 4) {
        s->rx_expected = ((s->rx_buf[2] << 8) | s->rx_buf[3]) + 5;
        if (s->rx_expected > (int)sizeof(s->rx_buf)) {
            s->rx_expected = sizeof(s->rx_buf);
        }
    }

    if (s->rx_pos >= 5 && s->rx_pos == s->rx_expected) {
        build_response(s, s->rx_buf[1]);
        if (g_logged++ < 200) {
            nc_log(TAG, "HW210: <- %02x (%d bytes) -> status %02x%02x, %d bytes", s->rx_buf[1], s->rx_expected,
                   s->tx_buf[4], s->tx_buf[5], s->tx_len);
        }
        s->rx_pos = 0;
        s->rx_expected = 0;
        if (s->dirty) {
            card_reader_flush(s);
        }
    }
}

int card_reader_read(CardReaderState *s, uint8_t *buf, int max_len)
{
    const int avail = s->tx_len - s->tx_pos;
    if (avail <= 0) {
        return 0;
    }
    const int n = avail < max_len ? avail : max_len;
    memcpy(buf, &s->tx_buf[s->tx_pos], (size_t)n);
    s->tx_pos += n;
    return n;
}

bool card_reader_has_response(CardReaderState *s)
{
    return s->tx_pos < s->tx_len;
}

bool card_reader_flush(CardReaderState *s)
{
    /* With no card in the slot there is nothing to keep; a card with no file (a refused one) is not written. */
    if (!s->card_path[0]) {
        s->dirty = false;
        return !s->card_present;
    }
    if (!chihiro_card_store(s->card_path, s->card_data, CARD_TOTAL_SIZE)) {
        return false;
    }
    s->dirty = false;
    return true;
}

/* ---- the slots (the fork's chihiro.c) ----
 * Where each slot's card is. Card In pushes the slot's card (a fresh one from the cabinet's stock when the file is
 * new). A game waiting for a card locks it (vsg.xbe FUN_0002da90 state 1) and ejects it by releasing the lock; a
 * card no game waits for springs back after the push. A locked card is never pulled (the games break).
 * HEURISTIC (the fork's): a push lasts 30 of the game's switch reads, half a second. */
enum { HW210_EMPTY, HW210_PUSHED, HW210_LOCKED };
#define HW210_PUSH_READS 30

void hw210_setup(ChihiroHW210 *h, const char *path_p1, const char *path_p2, const CardStock *stock)
{
    memset(h, 0, sizeof(*h));
    card_reader_init(&h->reader[0]);
    card_reader_init(&h->reader[1]);
    h->slot[0] = h->slot[1] = HW210_EMPTY;
    snprintf(h->path[0], sizeof(h->path[0]), "%s", path_p1 ? path_p1 : "");
    snprintf(h->path[1], sizeof(h->path[1]), "%s", path_p2 ? path_p2 : "");
    h->stock = stock;
    chihiro_card_reader_global = h->reader;
    chihiro_hw210_global = h;
}

bool hw210_card_present(const ChihiroHW210 *h, int player)
{
    return h && player >= 0 && player < 2 && h->reader[player].card_present;
}

void hw210_card_key(ChihiroHW210 *h, int player)
{
    if (!chihiro_hw210_enabled || player < 0 || player > 1 || !chihiro_jvs_card_lock_bits[player]) {
        return;
    }
    if (h->slot[player] == HW210_LOCKED) {
        nc_log(TAG, "HW210: P%d card locked, the game is using it", player + 1);
        return;
    }
    if (h->slot[player] == HW210_PUSHED) {
        return;
    }
    card_reader_insert(&h->reader[player], h->path[player], h->stock);
    if (!h->reader[player].card_present) {
        return;
    }
    h->slot[player] = HW210_PUSHED;
    h->pushed_at[player] = chihiro_jvs_switch_reads;
    nc_log(TAG, "HW210: card P%d pushed in: %s", player + 1, h->path[player]);
}

void hw210_tick(ChihiroHW210 *h)
{
    for (int p = 0; p < 2; p++) {
        const uint32_t reads = chihiro_jvs_switch_reads - h->pushed_at[p];
        const bool lock = chihiro_jvs_card_lock[p];
        const char *out = NULL;

        switch (h->slot[p]) {
        case HW210_PUSHED:
            if (lock) {
                h->slot[p] = HW210_LOCKED;
                nc_log(TAG, "HW210: P%d card locked %u switch reads after the push (card in)", p + 1, reads);
            } else if (reads >= HW210_PUSH_READS) {
                out = "not taken";
            }
            break;
        case HW210_LOCKED:
            if (!lock) {
                out = "ejected";
            }
            break;
        default:
            break;
        }
        if (out) {
            card_reader_remove(&h->reader[p]);
            nc_log(TAG, "HW210: card P%d %s", p + 1, out);
            h->slot[p] = HW210_EMPTY;
        }
    }
}
