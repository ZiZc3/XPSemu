/*
 * Chihiro JVS I/O Board Emulation (Sega 837-13551)
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * XPSemu (hakuX native core, Sega Chihiro phase 3): adapted from tattoohanz's
 * xemu-based Chihiro fork (hw/xbox/chihiro/chihiro-jvs.c): the protocol as
 * there; QEMU, the cabinet outputs (steering motor relay, card drawer locks)
 * and the fork's log macros taken out; inputs come from nc_chihiro_jvs_set.
 * Cxbx-Reloaded's JvsIo (GPL-2.0-or-later, Luke Usher and the Cxbx-Reloaded
 * team) was read as a second reference.
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
 *
 * JVS (JAMMA Video Standard) I/O board used by Chihiro, Naomi, Triforce.
 * Protocol references: MAME jvsdev.cpp, Flycast maple_jvs.cpp,
 * Lindbergh Loader jvs.c, Dolphin JVSIO.cpp.
 */

#include "native/dev/chihiro/jvs.h"
#include "native/core/nc.h"

#include <pthread.h>
#include <string.h>

#define TAG "hakuX-native-chihiro"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

ChihiroJVSState *chihiro_jvs_global = NULL;

/* XPSemu patch 225 (the fork's): the cabinet outputs on GPO bank 0. The card drawers' solenoids, as the game last
 * set them, and which bit drives each (chihiro_usb.c sets them for Ghost Squad: 0x20 P1, 0x01 P2; 0 elsewhere);
 * how many times the game read the switches (the card slots count time in it). The steering motor relay (bank 0
 * bit 7) goes to the V257 when the cabinet has one (Maximum Tune). */
#include "native/dev/chihiro/driveboard_v257.h"
bool chihiro_jvs_card_lock[2];
uint8_t chihiro_jvs_card_lock_bits[2];
uint32_t chihiro_jvs_switch_reads;

static const char board_id[] =
    "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

static const uint8_t capabilities[] = {
    0x01, 0x02, 0x0D, 0x00,   /* Switch: 2 players, 13 buttons each */
    0x02, 0x02, 0x00, 0x00,   /* Coin: 2 slots */
    0x03, 0x08, 0x10, 0x00,   /* Analog: 8 channels, 16-bit */
    0x12, 0x06, 0x00, 0x00,   /* GP output: 6 channels */
    0x15, 0x00, 0x00, 0x00,   /* Backup data */
    0x00                       /* Terminator */
};

/* The inputs from outside, taken in at each switch read. */
static pthread_mutex_t g_in_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool set;
    uint8_t system;
    uint8_t player[JVS_MAX_PLAYERS][2];
    uint16_t analog[JVS_MAX_ANALOG];
    uint16_t coins[JVS_MAX_COINS];
} g_in;

void nc_chihiro_jvs_set(uint8_t system, const uint8_t p1[2], const uint8_t p2[2],
                        const uint16_t analog[JVS_MAX_ANALOG])
{
    pthread_mutex_lock(&g_in_lock);
    g_in.set = true;
    g_in.system = system;
    if (p1) {
        memcpy(g_in.player[0], p1, 2);
    }
    if (p2) {
        memcpy(g_in.player[1], p2, 2);
    }
    if (analog) {
        memcpy(g_in.analog, analog, sizeof(g_in.analog));
    }
    pthread_mutex_unlock(&g_in_lock);
}

void nc_chihiro_jvs_coin(unsigned slot)
{
    if (slot < JVS_MAX_COINS) {
        pthread_mutex_lock(&g_in_lock);
        g_in.coins[slot]++;
        pthread_mutex_unlock(&g_in_lock);
    }
}

static void take_inputs(ChihiroJVSState *s)
{
    pthread_mutex_lock(&g_in_lock);
    if (g_in.set) {
        s->system_switches = g_in.system;
        memcpy(s->player_switches, g_in.player, sizeof(s->player_switches));
        memcpy(s->analog, g_in.analog, sizeof(s->analog));
    }
    for (int i = 0; i < JVS_MAX_COINS; i++) {
        if (g_in.coins[i]) {
            s->coin_count[i] = (uint16_t)((s->coin_count[i] + g_in.coins[i]) & 0x3FFF);
            nc_log(TAG, "jvs: coin slot %d -> %u", i + 1, s->coin_count[i]);
            g_in.coins[i] = 0;
        }
    }
    pthread_mutex_unlock(&g_in_lock);
}

void chihiro_jvs_init(ChihiroJVSState *s)
{
    memset(s, 0, sizeof(*s));
    s->sense = 3;
    for (int i = 0; i < JVS_MAX_ANALOG; i++) {
        s->analog[i] = 0x8000;
    }
    pthread_mutex_lock(&g_in_lock);
    for (int i = 0; i < JVS_MAX_ANALOG; i++) {
        g_in.analog[i] = 0x8000;
    }
    pthread_mutex_unlock(&g_in_lock);
}

/*
 * Handle a single JVS command from the unescaped data stream.
 * Returns the number of input bytes consumed (command + params).
 * Appends report bytes to resp[*rpos].
 */
static int jvs_handle_command(ChihiroJVSState *s,
                               const uint8_t *cmd, int cmd_len,
                               uint8_t *resp, int *rpos, int rmax)
{
    if (cmd_len < 1) return 0;
    int rp = *rpos;

#define PUT(b) do { if (rp < rmax) resp[rp++] = (b); } while(0)

    switch (cmd[0]) {
    case 0xF0: /* Reset */
        if (cmd_len < 2) return 1;
        s->reset_count++;
        if (s->reset_count >= 2) {
            s->device_id = 0;
            s->sense = 3;
            s->reset_count = 0;
            /* A reset drops the board's outputs with its address: the card locks, and the steering motor's
             * relay (the fork's). */
            chihiro_jvs_card_lock[0] = chihiro_jvs_card_lock[1] = false;
            if (chihiro_v257_global) {
                v257_set_motor(chihiro_v257_global, false);
            }
            nc_log(TAG, "jvs: RESET applied (sense 3, id 0)");
        }
        *rpos = rp;
        return 2;

    case 0xF1: /* Set Device ID */
        if (cmd_len < 2) return 1;
        if (s->sense == 0) {
            *rpos = rp;
            return 2;
        }
        nc_log(TAG, "jvs: SET_ID %d (sense %d -> 0)", cmd[1], s->sense);
        s->device_id = cmd[1];
        s->sense = 0;
        s->reset_count = 0;
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 2;

    case 0x10: /* Request Board ID */
        PUT(JVS_REPORT_OK);
        for (int i = 0; i <= (int)strlen(board_id); i++) {
            PUT(board_id[i]);
        }
        *rpos = rp;
        return 1;

    case 0x11: /* Command Format Version */
        PUT(JVS_REPORT_OK);
        PUT(0x11);
        *rpos = rp;
        return 1;

    case 0x12: /* JVS Revision */
        PUT(JVS_REPORT_OK);
        PUT(0x20);
        *rpos = rp;
        return 1;

    case 0x13: /* Communication Version */
        PUT(JVS_REPORT_OK);
        PUT(0x10);
        *rpos = rp;
        return 1;

    case 0x14: /* Feature Check */
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < (int)sizeof(capabilities); i++) {
            PUT(capabilities[i]);
        }
        *rpos = rp;
        return 1;

    case 0x15: { /* Convey Main Board ID */
        int consumed = 1;
        while (consumed < cmd_len && cmd[consumed] != '\0') {
            consumed++;
        }
        if (consumed < cmd_len) consumed++; /* skip the NUL */
        nc_log(TAG, "jvs: main board id \"%.*s\"", consumed - 1 > 0 ? consumed - 2 : 0, (const char *)cmd + 1);
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return consumed;
    }

    case 0x20: { /* Read Switch Inputs */
        if (cmd_len < 3) return 1;
        int players = cmd[1];
        int bytes_per = cmd[2];
        chihiro_jvs_switch_reads++;
        take_inputs(s);
        PUT(JVS_REPORT_OK);
        PUT(s->system_switches);
        for (int p = 0; p < players && p < JVS_MAX_PLAYERS; p++) {
            for (int b = 0; b < bytes_per && b < 2; b++) {
                PUT(s->player_switches[p][b]);
            }
        }
        *rpos = rp;
        return 3;
    }

    case 0x21: { /* Read Coin Inputs */
        if (cmd_len < 2) return 1;
        int slots = cmd[1];
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < slots && i < JVS_MAX_COINS; i++) {
            PUT((s->coin_count[i] >> 8) & 0x3F);
            PUT(s->coin_count[i] & 0xFF);
        }
        *rpos = rp;
        return 2;
    }

    case 0x22: { /* Read Analog Inputs */
        if (cmd_len < 2) return 1;
        int channels = cmd[1];
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < channels && i < JVS_MAX_ANALOG; i++) {
            PUT((s->analog[i] >> 8) & 0xFF);
            PUT(s->analog[i] & 0xFF);
        }
        *rpos = rp;
        return 2;
    }

    case 0x26: { /* Read Misc Switch Inputs */
        if (cmd_len < 2) return 1;
        int nbytes = cmd[1];
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < nbytes; i++) PUT(0x00);
        *rpos = rp;
        return 2;
    }

    case 0x2E: { /* Read Payout Hopper Status */
        if (cmd_len < 2) return 1;
        int slots = cmd[1];
        PUT(JVS_REPORT_OK);
        for (int i = 0; i < slots; i++) {
            PUT(0); PUT(0); PUT(0); PUT(0);
        }
        *rpos = rp;
        return 2;
    }

    case 0x2F: /* Retransmit Data */
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 1;

    case 0x30:
    case 0x31: { /* Coin Decrease/Increase */
        if (cmd_len < 4) return 1;
        int slot = cmd[1] - 1;
        uint16_t count = (cmd[2] << 8) | cmd[3];
        if (slot >= 0 && slot < JVS_MAX_COINS) {
            if (cmd[0] == 0x30) {
                if (s->coin_count[slot] >= count)
                    s->coin_count[slot] -= count;
            } else {
                s->coin_count[slot] += count;
            }
        }
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 4;
    }

    case 0x32: { /* General Purpose Output */
        if (cmd_len < 2) return 1;
        int banks = cmd[1];
        int consumed = 2 + banks;
        if (consumed > cmd_len) consumed = cmd_len;
        /* patch 225 (the fork's): bank 0 bit 7 is the steering motor relay in a Maximum Tune cabinet (V322.xbe
         * 0x000558C0); the board has to see it: its answer is what the boot wheel check waits for. */
        if (banks >= 1 && cmd_len >= 3 && chihiro_v257_global) {
            v257_set_motor(chihiro_v257_global, (cmd[2] & 0x80) != 0);
        }
        /* The card drawers' solenoids, one output per slot (Ghost Squad): the game holds a card it finds in a
         * slot and lets go to eject it. */
        if (banks >= 1 && cmd_len >= 3) {
            for (int p = 0; p < 2; p++) {
                const uint8_t bit = chihiro_jvs_card_lock_bits[p];
                const bool now = bit && (cmd[2] & bit);
                if (now != chihiro_jvs_card_lock[p]) {
                    chihiro_jvs_card_lock[p] = now;
                    nc_log(TAG, "jvs: P%d card lock %s", p + 1, now ? "on" : "off");
                }
            }
        }
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return consumed;
    }

    case 0x33: { /* Analog Output */
        if (cmd_len < 2) return 1;
        int channels = cmd[1];
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 2 + channels * 2;
    }

    case 0x34: { /* Character Output */
        if (cmd_len < 2) return 1;
        int nbytes = cmd[1];
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 2 + nbytes;
    }

    case 0x36: /* Payout Subtraction Output */
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 4;

    case 0x37: /* General Purpose Output 2 */
        PUT(JVS_REPORT_OK);
        *rpos = rp;
        return 3;

    default:
        {
            static unsigned s_unknown;
            if (s_unknown++ < 50) {
                nc_log(TAG, "jvs: unknown command %02x", cmd[0]);
            }
        }
        PUT(JVS_REPORT_PARAM);
        *rpos = rp;
        return 1;
    }
#undef PUT
}

int chihiro_jvs_process(ChihiroJVSState *s,
                         const uint8_t *in, int in_len,
                         uint8_t *out, int out_max)
{
    if (in_len < 3 || in[0] != JVS_SYNC) return 0;

    uint8_t target = in[1];
    int escaped_count = in[2];

    /* The caller (the QC's UART1, an2131.c) removes and restores the 0xD0
     * escapes, and hands over whole frames: the bytes here are raw, and
     * raw_len is escaped_count. */
    const uint8_t *raw = in + 3;
    int raw_len = in_len - 3;

    /* Verify checksum: sum of (target + count + all_data_bytes) & 0xFF */
    int data_len = MIN(raw_len, escaped_count) - 1;
    if (data_len < 0) data_len = 0;

    uint8_t csum = target + escaped_count;
    for (int i = 0; i < data_len; i++) csum += raw[i];
    if (escaped_count >= 1 && raw_len >= escaped_count &&
        raw[escaped_count - 1] != (csum & 0xFF)) {
        nc_log(TAG, "jvs: checksum error (got %02x, expected %02x)",
               raw[escaped_count - 1], csum & 0xFF);
    }

    /* Broadcast: Reset gets no response, but Set ID does (claiming device responds) */
    if (target == JVS_BROADCAST) {
        if (data_len >= 1 && raw[0] == 0xF1) {
            /* Set ID on broadcast — fall through to addressed handler so we respond */
        } else {
            const uint8_t *cmd = raw;
            int remaining = data_len;
            while (remaining > 0) {
                int dummy = 0;
                int consumed = jvs_handle_command(s, cmd, remaining,
                                                  NULL, &dummy, 0);
                if (consumed <= 0) break;
                cmd += consumed;
                remaining -= consumed;
            }
            return 0;
        }
    }

    /* Addressed packet — must match our device_id (broadcast Set ID also passes) */
    if (target != JVS_BROADCAST && target != s->device_id && s->device_id != 0) {
        return 0;
    }

    /* Process commands and build response payload */
    uint8_t payload[256];
    int ppos = 0;
    payload[ppos++] = JVS_STATUS_OK;

    const uint8_t *cmd = raw;
    int remaining = data_len;
    while (remaining > 0) {
        int consumed = jvs_handle_command(s, cmd, remaining,
                                          payload, &ppos, sizeof(payload));
        if (consumed <= 0) break;
        cmd += consumed;
        remaining -= consumed;
    }

    /* Build framed response: SYNC + host_addr + count + payload + checksum,
     * raw: the caller escapes it. */
    uint8_t frame[sizeof(payload) + 4];     /* SYNC, address, count, sum */
    int fpos = 0;
    uint8_t resp_count = ppos + 1; /* payload + checksum */

    uint8_t resp_csum = JVS_HOST_ADDR + resp_count;
    for (int i = 0; i < ppos; i++) resp_csum += payload[i];

    frame[fpos++] = JVS_SYNC;
    frame[fpos++] = JVS_HOST_ADDR;
    frame[fpos++] = resp_count;
    memcpy(frame + fpos, payload, ppos);
    fpos += ppos;
    frame[fpos++] = resp_csum & 0xFF;

    int out_len = (fpos < out_max) ? fpos : out_max;
    memcpy(out, frame, out_len);

    return out_len;
}
