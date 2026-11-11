/*
 * Cypress AN2131 (EZ-USB) register mapping for Chihiro baseboard.
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * XPSemu (hakuX native core, Sega Chihiro phase 3): adapted from tattoohanz's
 * xemu-based Chihiro fork (hw/xbox/chihiro/chihiro-an2131.c). The register
 * layer, I2C, JVS relay, interrupt logic, control-request window and idle
 * skip are the fork's; taken out: QEMU (clocks, spinlocks, GString), the SC's
 * the SC exchange trace and the TBRATE probe. The SC's peers came back as
 * their own files: the Sega 838 drive board (patch 205), the V257 drive
 * board and the HW210 / CRP-1231 card readers (patch 225); a cabinet without
 * them has its UART bytes dropped and TI raised at once, as the fork does.
 * Logging goes to nc_log ("hakuX-native-chihiro").
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
 * Maps XDATA-space USB registers, I2C bus master, and port I/O
 * to the generic 8051 CPU core. Runs the ic10/pc20 firmware natively
 * to handle USB vendor requests, EEPROM I2C, JVS relay and the backup
 * records without per-game HLE.
 *
 * Register addresses from Cypress EZ-USB TRM v1.9/1.10.
 */

#include "native/dev/chihiro/an2131.h"
#include "native/dev/chihiro/jvs.h"
#include "native/core/nc.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#define TAG "hakuX-native-chihiro"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

bool an2131_monitor_31khz;
bool an2131_drive_sega838;

/* XPSemu patch 205: the drive board on the SC's UART1, as the fork (chihiro-an2131.c midi_*): a byte goes out
 * in 1920 cycles (10 bits at 31250 baud, 6 M cycles/s), TI1 then; an answer byte lands 5000 cycles apart.
 * patch 225: the peer is the cabinet's: the Sega 838 (OutRun 2) or the V257 (Maximum Tune); with HW210 card
 * readers (Ghost Squad) the line belongs to player 1's reader (card_rx_deliver). The V257 has its TI1 at once, as
 * the fork (only the Sega 838 has the deferred one). */
#include "native/dev/chihiro/driveboard.h"
#include "native/dev/chihiro/driveboard_v257.h"
#include "native/dev/chihiro/cardreader_hw210.h"
#include "native/dev/chihiro/cardreader_crp1231.h"
#define MIDI_TI_CYCLES 1920

static bool midi_sega838(void)
{
    return an2131_drive_sega838 && chihiro_driveboard_global;
}

/* SC UART1 reaches whichever board the cabinet hangs off it (none: the line unanswered). */
static void midi_peer_send(uint8_t val)
{
    if (chihiro_v257_global) {
        v257_receive_byte(chihiro_v257_global, val);
    } else if (midi_sega838()) {
        driveboard_receive_byte(chihiro_driveboard_global, val);
    }
}

static bool midi_peer_has_response(void)
{
    if (chihiro_v257_global) {
        return v257_has_response(chihiro_v257_global);
    }
    return midi_sega838() && driveboard_has_response(chihiro_driveboard_global);
}

static uint8_t midi_peer_get_response(void)
{
    if (chihiro_v257_global) {
        return v257_get_response(chihiro_v257_global);
    }
    return driveboard_get_response(chihiro_driveboard_global);
}

static void midi_tick(AN2131State *s)
{
    if (s->is_qc || chihiro_hw210_enabled || (!midi_sega838() && !chihiro_v257_global)) {
        return;
    }
    Cpu8051State *cpu = &s->cpu;
    if (s->midi_ti_cycles && s->total_cycles - s->midi_ti_cycles >= MIDI_TI_CYCLES) {
        cpu->events++;
        cpu->sfr[0xC0 - 0x80] |= 0x02;      /* TI1 */
        cpu->irq_recheck = true;
        s->midi_ti_cycles = 0;
    }
    if (!midi_peer_has_response()) {
        return;
    }
    /* A board may speak unasked (the V257 reports its motor line), so pacing starts from what is queued. */
    if (!s->midi_response_ready) {
        s->midi_response_ready = true;
        s->midi_response_set_cycles = s->total_cycles;
    }
    if (cpu->in_interrupt || (cpu->sfr[0xC0 - 0x80] & 0x01)) {
        return;                              /* in an ISR, or RI1 still set */
    }
    if (s->total_cycles - s->midi_response_set_cycles < 5000) {
        return;
    }
    const uint8_t byte = midi_peer_get_response();
    static unsigned logged;
    if (logged++ < 60) {
        nc_log(TAG, "SC uart1 rx %02x (from the drive board)", byte);
    }
    cpu->events++;
    cpu->sfr[0xC1 - 0x80] = byte;
    cpu->sfr[0xC0 - 0x80] |= 0x01;          /* RI1 */
    cpu->irq_recheck = true;
    if (midi_peer_has_response()) {
        s->midi_response_set_cycles = s->total_cycles;
    } else {
        s->midi_response_ready = false;
    }
}

/* ---- patch 225: the card readers on the SC's UARTs (the fork's chihiro-an2131.c card_*) ---- */

/* A card reader on the SC's UARTs with bytes still to give: work pending for the firmware. */
static bool sc_reader_pending(AN2131State *s)
{
    if (s->is_qc) {
        return false;
    }
    if (chihiro_hw210_enabled && chihiro_card_reader_global &&
        (card_reader_has_response(&chihiro_card_reader_global[0]) ||
         card_reader_has_response(&chihiro_card_reader_global[1]))) {
        return true;
    }
    return chihiro_crp1231_enabled && chihiro_crp1231_global && crp1231_has_response(chihiro_crp1231_global);
}

/* HEURISTIC (the fork's): a card reader's turnaround before the first byte of an answer, then a serial byte rate;
 * no HW210 or CRP-1231 datasheet gives them. */
#define CARD_TURNAROUND_CYCLES 60000
#define CARD_BYTE_CYCLES       1500
/* HEURISTIC (the fork's): the delay from an SBUF write to TI (33 us; a byte at 115200 baud would take about 520
 * cycles). */
#define CARD_TI_CYCLES         200

/* A byte from a card reader lands in a SC UART: SBUF, the even parity in RB8 (the 9-bit mode's ninth bit), RI, and
 * the serial interrupt looked at again. */
static void sc_uart_post_rx(AN2131State *s, uint8_t sbuf, uint8_t scon, uint8_t b)
{
    Cpu8051State *cpu = &s->cpu;
    uint8_t par = b;

    cpu->events++;
    cpu->sfr[sbuf - 0x80] = b;
    par ^= par >> 4;
    par ^= par >> 2;
    par ^= par >> 1;
    if (par & 1) {
        cpu->sfr[scon - 0x80] |= 0x04;
    } else {
        cpu->sfr[scon - 0x80] &= ~0x04;
    }
    cpu->sfr[scon - 0x80] |= 0x01;
    cpu->irq_recheck = true;
}

/* One pending card-response byte into a SC UART RX register: a reader round-trip before the first byte, then a
 * real serial byte rate (back-to-back delivery starves the firmware's SUDAV dispatch during long responses), gated
 * on RI so the UART ISR takes each byte before the next. The UARTs run in 9-bit mode: the firmware checks RB8 as
 * the even parity of each byte. */
static void card_rx_deliver_ch(AN2131State *s, int slot, uint8_t sbuf, uint8_t scon, int idx)
{
    Cpu8051State *cpu = &s->cpu;
    CardReaderState *c = &chihiro_card_reader_global[slot];

    if (!card_reader_has_response(c)) {
        s->card_resp_cycles[idx] = 0;               /* nothing pending */
        s->card_delivering[idx] = false;
        return;
    }
    if (s->card_resp_cycles[idx] == 0) {
        s->card_resp_cycles[idx] = s->total_cycles; /* response just became ready */
    }
    if (c->tx_pos == 0 && s->card_delivering[idx]) {
        /* A repeated command replaced the response being delivered (the firmware retransmits frames): the
         * turnaround starts again so the duplicates settle into one clean reply. */
        s->card_resp_cycles[idx] = s->total_cycles;
        s->card_delivering[idx] = false;
    }
    const uint64_t need = c->tx_pos == 0 ? CARD_TURNAROUND_CYCLES : CARD_BYTE_CYCLES;
    if (s->total_cycles - s->card_resp_cycles[idx] < need) {
        return;
    }
    if (cpu->sfr[scon - 0x80] & 0x01) {
        return;                                     /* RI set: previous byte unread */
    }
    uint8_t b;
    if (card_reader_read(c, &b, 1) == 1) {
        s->card_resp_cycles[idx] = s->total_cycles; /* pace the next byte */
        s->card_delivering[idx] = true;
        sc_uart_post_rx(s, sbuf, scon, b);
    }
}

/* The CRP-1231 on the RS-232C line: same wire, framing and pacing. */
static void crp1231_rx_deliver(AN2131State *s)
{
    Cpu8051State *cpu = &s->cpu;
    CRP1231State *r = chihiro_crp1231_global;

    if (!r || !crp1231_has_response(r)) {
        s->card_resp_cycles[0] = 0;
        return;
    }
    if (s->card_resp_cycles[0] == 0) {
        s->card_resp_cycles[0] = s->total_cycles;
    }
    const uint64_t need = crp1231_mid_response(r) ? CARD_BYTE_CYCLES : CARD_TURNAROUND_CYCLES;
    if (s->total_cycles - s->card_resp_cycles[0] < need) {
        return;
    }
    if (cpu->sfr[SFR_SCON - 0x80] & 0x01) {
        return;                                     /* RI set: byte unread */
    }
    uint8_t b;
    if (crp1231_read(r, &b)) {
        s->card_resp_cycles[0] = s->total_cycles;
        sc_uart_post_rx(s, SFR_SBUF, SFR_SCON, b);
    }
}

static void card_rx_deliver(AN2131State *s)
{
    if (s->is_qc) {
        return;
    }
    /* Deferred TX-complete: TI CARD_TI_CYCLES after the SBUF write, as the silicon raises it once the byte is out
     * (UART1 to a drive board has its own, midi_tick). */
    for (int i = 0; i < 2; i++) {
        if (s->card_ti_cycles[i] && s->total_cycles - s->card_ti_cycles[i] >= CARD_TI_CYCLES) {
            s->cpu.events++;
            s->cpu.sfr[(i == 0 ? 0x98 : 0xC0) - 0x80] |= 0x02;
            s->cpu.irq_recheck = true;
            s->card_ti_cycles[i] = 0;
        }
    }
    const bool mt = chihiro_crp1231_enabled && chihiro_crp1231_global;
    if (!mt && (!chihiro_hw210_enabled || !chihiro_card_reader_global)) {
        return;
    }
    /* Hold delivery while a card-drain vendor request (0x1A/0x1B) is still being serviced (reply not yet armed):
     * a serial RX interrupt in the middle of the drain/feeder copy clobbers shared firmware work pointers and the
     * game receives zeros. */
    if (s->in_setup && !s->ep[0].in_armed) {
        return;
    }
    if (s->cpu.in_interrupt) {
        return;
    }
    if (mt) {
        crp1231_rx_deliver(s);
        return;
    }
    card_rx_deliver_ch(s, 1, 0x99, 0x98, 0);   /* RS-232C: SBUF0/SCON0, player 2's reader */
    card_rx_deliver_ch(s, 0, 0xC1, 0xC0, 1);   /* MIDI:    SBUF1/SCON1, player 1's reader */
}

static void an2131_card_pacing_reset(AN2131State *s)
{
    s->in_setup = false;
    memset(s->card_resp_cycles, 0, sizeof(s->card_resp_cycles));
    memset(s->card_delivering, 0, sizeof(s->card_delivering));
    memset(s->card_ti_cycles, 0, sizeof(s->card_ti_cycles));
}

static void an2131_reset(AN2131State *s);
static void an2131_set_cpucs(AN2131State *s, uint8_t val);
static void jvs_rx_deliver(AN2131State *s);

static inline uint16_t canon(uint16_t addr)
{
    if (addr >= 0x1B40 && addr <= 0x1FFF)
        return addr + 0x6000;
    return addr;
}

/* Where an external address lands in the SRAM: in the half the QC's port C
 * bit 3 selects, when the pin is a driven output (see extmem). */
static inline uint32_t extmem_index(AN2131State *s, uint16_t addr)
{
    bool high = s->is_qc && (s->oec & 0x08) && !(s->portccfg & 0x08) &&
                (s->outc & 0x08);

    return (high ? 0x10000u : 0) | addr;
}

/* ── I2C state machine ────────────────────────────────────────────── */

/* XPSemu: one log line per I2C transaction that moved data (first 200). */
static void i2c_log_end(AN2131State *s)
{
    static unsigned logged;
    if (s->i2c.count > 0 && s->i2c.dev7 != 0x32 && logged < 200) {
        logged++;
        nc_log(TAG, "%s i2c: %s %d byte(s) %s eeprom 0x%02x at 0x%04x", s->name,
               s->i2c.reading ? "read" : "wrote", s->i2c.count,
               s->i2c.reading ? "from" : "to", s->i2c.dev7, s->i2c.start);
    }
    s->i2c.count = 0;
}

static void i2c_fire_done(AN2131State *s, bool ack)
{
    s->i2cs = I2CS_DONE | (ack ? I2CS_ACK : 0);
    if (s->i2c_lastrd) s->i2cs |= I2CS_LASTRD;
    s->i2c_irq_pending = true;
    s->cpu.irq_recheck = true;
}

static void i2c_select_device(AN2131State *s, uint8_t addr7)
{
    if (addr7 == 0x50 && s->ic10_eeprom) {
        s->i2c.eeprom = s->ic10_eeprom;
        s->i2c.eeprom_size = s->ic10_size;
        s->i2c.addr_bytes_needed = 2;   /* 24LC64: 2-byte address */
    } else if (addr7 == 0x51 && s->ic10_eeprom) {
        s->i2c.eeprom = s->ic10_eeprom;
        s->i2c.eeprom_size = s->ic10_size;
        s->i2c.addr_bytes_needed = 2;   /* ic10 24LC64: A0=1 on baseboard → addr 0x51 */
    } else if (addr7 == 0x55 && s->ic11_eeprom) {
        s->i2c.eeprom = s->ic11_eeprom;
        s->i2c.eeprom_size = s->ic11_size;
        s->i2c.addr_bytes_needed = 1;   /* ic11, the baseboard settings EEPROM */
    } else if (addr7 == 0x32) {
        time_t now = time(NULL);
        struct tm tmv, *t = localtime_r(&now, &tmv);
        #define TO_BCD(v) (uint8_t)(((v)/10 << 4) | ((v)%10))
        if (t) {
            s->rtc_regs[0] = TO_BCD(t->tm_sec);
            s->rtc_regs[1] = TO_BCD(t->tm_min);
            s->rtc_regs[2] = TO_BCD(t->tm_hour);
            s->rtc_regs[3] = t->tm_wday + 1;
            s->rtc_regs[4] = TO_BCD(t->tm_mday);
            s->rtc_regs[5] = TO_BCD(t->tm_mon + 1);
            s->rtc_regs[6] = TO_BCD(t->tm_year % 100);
        }
        #undef TO_BCD
        s->i2c.eeprom = s->rtc_regs;
        s->i2c.eeprom_size = 16;
        s->i2c.addr_bytes_needed = 1;
    } else {
        s->i2c.eeprom = NULL;
        s->i2c.eeprom_size = 0;
        s->i2c.addr_bytes_needed = 0;
    }
}

static void i2c_dat_write(AN2131State *s, uint8_t val)
{
    switch (s->i2c.phase) {
    case I2C_ADDR: {
        uint8_t addr7 = val >> 1;
        i2c_log_end(s);
        s->i2c.reading = val & 1;
        s->i2c.dev7 = addr7;
        s->i2c.start = s->i2c.mem_addr;
        i2c_select_device(s, addr7);
        s->i2c.addr_bytes_sent = 0;
        if (!s->i2c.eeprom) {
            static unsigned nacks;
            if (nacks++ < 50) {
                nc_log(TAG, "%s i2c: NACK, no device at 0x%02x", s->name, addr7);
            }
            i2c_fire_done(s, false);
            return;
        }
        s->i2c.phase = s->i2c.reading ? I2C_DATA : I2C_MEM_ADDR;
        s->i2c.first_read = s->i2c.reading;
        i2c_fire_done(s, true);
        break;
    }
    case I2C_MEM_ADDR:
        if (s->i2c.addr_bytes_needed == 2 && s->i2c.addr_bytes_sent == 0) {
            s->i2c.mem_addr = (uint16_t)val << 8;
        } else if (s->i2c.addr_bytes_needed == 2) {
            s->i2c.mem_addr |= val;
        } else {
            s->i2c.mem_addr = val;
        }
        s->i2c.addr_bytes_sent++;
        if (s->i2c.addr_bytes_sent >= s->i2c.addr_bytes_needed) {
            s->i2c.phase = I2C_DATA;
            s->i2c.start = s->i2c.mem_addr;
        }
        i2c_fire_done(s, true);
        break;
    case I2C_DATA:
        if (s->i2c.eeprom && s->i2c.mem_addr < (uint16_t)s->i2c.eeprom_size) {
            if (s->i2c.eeprom[s->i2c.mem_addr] != val && s->i2c.eeprom == s->ic11_eeprom) {
                s->ic11_dirty = true;
            }
            s->i2c.eeprom[s->i2c.mem_addr] = val;
            s->backup_dirty = true;
        }
        s->i2c.mem_addr++;
        s->i2c.count++;
        i2c_fire_done(s, true);
        break;
    default:
        break;
    }
}

static uint8_t i2c_dat_read(AN2131State *s)
{
    uint8_t val = 0xFF;
    if (s->i2c.first_read) {
        s->i2c.first_read = false;
        s->i2c.start = s->i2c.mem_addr;
        i2c_fire_done(s, true);
        return val;
    }
    if (s->i2c.reading && s->i2c.eeprom &&
        s->i2c.mem_addr < (uint16_t)s->i2c.eeprom_size) {
        val = s->i2c.eeprom[s->i2c.mem_addr];
        s->i2c.mem_addr++;
        s->i2c.count++;
    }
    if (s->i2c.phase != I2C_IDLE) {
        i2c_fire_done(s, !s->i2c_lastrd);
    }
    return val;
}

/* ── XDATA read callback ─────────────────────────────────────────── */

static uint8_t an2131_xdata_read(Cpu8051State *cpu, uint16_t addr)
{
    AN2131State *s = (AN2131State *)cpu->opaque;

    if (addr < 0x1B40)
        return s->ram[addr];

    uint16_t ca = canon(addr);

    if (ca >= 0x7B40 && ca < 0x7F40)
        return s->ram[ca - 0x6000];

    if (ca >= 0x7F40 && ca <= 0x7FFF) {
        switch (ca) {
        case AN_CPUCS:    return s->cpucs;

        case AN_PORTACFG: return s->portacfg;
        case AN_PORTBCFG: return s->portbcfg;
        case AN_PORTCCFG: return s->portccfg;
        case AN_OUTA:     return s->outa;
        case AN_OUTB:     return s->outb;
        case AN_OUTC:     return s->outc;
        case AN_PINSA: {
            /* Port A: the baseboard DIP switches; bits 1 and 2 are the
             * monitor's scan frequency (15 kHz bit 1, the default). */
            uint8_t freq = an2131_monitor_31khz ? 0x04 : 0x02;
            return (s->pinsa & (uint8_t)~0x06u) | freq;
        }
        case AN_PINSB: {
            /* JVS sense is on bit 0 only. Bit 1 is a separate hardware pin (always high). */
            uint8_t sense = chihiro_jvs_global ? (chihiro_jvs_global->sense & 0x01) : 0x01;
            return (s->pinsb & ~0x01) | sense;
        }
        case AN_PINSC:    return s->pinsc;
        case AN_OEA:      return s->oea;
        case AN_OEB:      return s->oeb;
        case AN_OEC:      return s->oec;

        case AN_I2CS:     return s->i2cs;
        case AN_I2DAT:    cpu->events++; return i2c_dat_read(s);

        case AN_IVEC:     return s->ivec;
        case AN_IN07IRQ:  return s->in07irq;
        case AN_OUT07IRQ: return s->out07irq;
        case AN_USBIRQ:   return s->usbirq;
        case AN_IN07IEN:  return s->in07ien;
        case AN_OUT07IEN: return s->out07ien;
        case AN_USBIEN:   return s->usbien;
        case AN_USBBAV:   return s->usbbav;

        case AN_EP0CS:    return s->ep0cs;
        case AN_IN0BC:    return s->ep[0].bc_in;
        case AN_OUT0BC:   return s->ep[0].bc_out;

        case AN_SUDPTRH:  return (uint8_t)(s->sudptr >> 8);
        case AN_SUDPTRL:  return (uint8_t)s->sudptr;
        case AN_USBCS:    return s->usbcs;
        case AN_TOGCTL:   return s->togctl;
        case AN_FNADDR:   return s->fnaddr;
        case AN_USBPAIR:  return s->usbpair;
        case AN_IN07VAL:  return s->in07val;
        case AN_OUT07VAL: return s->out07val;

        case AN_FASTXFR:  return s->fastxfr;
        case AN_AUTOPTRH: return (uint8_t)(s->autoptr >> 8);
        case AN_AUTOPTRL: return (uint8_t)s->autoptr;
        case AN_AUTODATA: {
            cpu->events++;
            uint8_t v = canon(s->autoptr) == AN_AUTODATA
                            ? 0xFF : an2131_xdata_read(cpu, s->autoptr);
            s->autoptr++;
            return v;
        }

        case AN_SETUPDAT:     case AN_SETUPDAT + 1:
        case AN_SETUPDAT + 2: case AN_SETUPDAT + 3:
        case AN_SETUPDAT + 4: case AN_SETUPDAT + 5:
        case AN_SETUPDAT + 6: case AN_SETUPDAT + 7:
            return s->setupdat[ca - AN_SETUPDAT];

        default:
            break;
        }

        if (ca >= 0x7FB6 && ca <= 0x7FC3) {
            int n = (ca - 0x7FB6) / 2 + 1;
            return ((ca - 0x7FB6) & 1) ? s->ep[n].bc_in : s->ep[n].cs_in;
        }
        if (ca >= 0x7FC6 && ca <= 0x7FD3) {
            int n = (ca - 0x7FC6) / 2 + 1;
            return ((ca - 0x7FC6) & 1) ? s->ep[n].bc_out : s->ep[n].cs_out;
        }
        return 0xFF;
    }

    /* External SRAM: 0x2000-0x7B3F and 0x8000-0xFFFF */
    if (s->extmem)
        return s->extmem[extmem_index(s, addr)];
    return 0xFF;
}

/* ── XDATA write callback ────────────────────────────────────────── */

static void an2131_xdata_write(Cpu8051State *cpu, uint16_t addr, uint8_t val)
{
    AN2131State *s = (AN2131State *)cpu->opaque;

    if (addr < 0x1B40) {
        cpu->events += s->ram[addr] != val;
        s->ram[addr] = val;
        return;
    }

    uint16_t ca = canon(addr);

    if (ca >= 0x7B40 && ca < 0x7F40) {
        cpu->events += s->ram[ca - 0x6000] != val;
        s->ram[ca - 0x6000] = val;
        return;
    }

    if (ca >= 0x7F40 && ca <= 0x7FFF) {
        cpu->events++;
        cpu->irq_recheck = true;
        switch (ca) {
        case AN_CPUCS:    s->cpucs = val; return;

        case AN_PORTACFG: s->portacfg = val; return;
        case AN_PORTBCFG: s->portbcfg = val; return;
        case AN_PORTCCFG: s->portccfg = val; return;
        case AN_OUTA:     s->outa = val; return;
        case AN_OUTB:     s->outb = val; return;
        case AN_OUTC:     s->outc = val; return;
        case AN_OEA:      s->oea = val; return;
        case AN_OEB:      s->oeb = val; return;
        case AN_OEC:      s->oec = val; return;

        case AN_I2CS: {
            if (val & I2CS_START) s->i2c.phase = I2C_ADDR;
            if (val & I2CS_STOP) {
                i2c_log_end(s);
                s->i2c.phase = I2C_IDLE;
                s->i2c_lastrd = false;
            } else {
                s->i2c_lastrd = !!(val & I2CS_LASTRD);
            }
            return;
        }
        case AN_I2DAT:
            i2c_dat_write(s, val);
            return;

        case AN_IVEC:     return;
        case AN_IN07IRQ:  s->in07irq  &= ~val; return;
        case AN_OUT07IRQ: s->out07irq &= ~val; return;
        case AN_USBIRQ:   s->usbirq   &= ~val; return;
        case AN_IN07IEN:  s->in07ien  = val; return;
        case AN_OUT07IEN: s->out07ien = val; return;
        case AN_USBIEN:   s->usbien   = val; return;
        case AN_USBBAV:   s->usbbav   = val; return;

        case AN_EP0CS:
            s->ep0cs = val;
            return;
        case AN_IN0BC:
            s->ep[0].bc_in = val;
            s->ep[0].in_armed = true;
            s->ep0cs |= EP0CS_INBSY;
            return;
        case AN_OUT0BC:
            s->ep[0].bc_out = 0;
            s->ep0cs &= ~EP0CS_OUTBSY;
            return;

        case AN_SUDPTRH: s->sudptr = (s->sudptr & 0x00FF) | ((uint16_t)val << 8); return;
        case AN_SUDPTRL: s->sudptr = (s->sudptr & 0xFF00) | val; return;
        case AN_USBCS:   s->usbcs = val; return;
        case AN_TOGCTL:  s->togctl = val; return;
        case AN_FNADDR:  s->fnaddr = val; return;
        case AN_USBPAIR: s->usbpair = val; return;
        case AN_IN07VAL: s->in07val = val; return;
        case AN_OUT07VAL: s->out07val = val; return;

        case AN_FASTXFR:  s->fastxfr = val; return;
        case AN_AUTOPTRH: s->autoptr = (s->autoptr & 0x00FF) | ((uint16_t)val << 8); return;
        case AN_AUTOPTRL: s->autoptr = (s->autoptr & 0xFF00) | val; return;
        case AN_AUTODATA:
            if (canon(s->autoptr) != AN_AUTODATA) {
                an2131_xdata_write(cpu, s->autoptr, val);
            }
            s->autoptr++;
            return;

        case AN_SETUPDAT:     case AN_SETUPDAT + 1:
        case AN_SETUPDAT + 2: case AN_SETUPDAT + 3:
        case AN_SETUPDAT + 4: case AN_SETUPDAT + 5:
        case AN_SETUPDAT + 6: case AN_SETUPDAT + 7:
            return;

        default:
            break;
        }

        if (ca >= 0x7FB6 && ca <= 0x7FC3) {
            int n = (ca - 0x7FB6) / 2 + 1;
            if ((ca - 0x7FB6) & 1) {
                s->ep[n].bc_in = val;
                s->ep[n].in_armed = true;
                s->ep[n].cs_in |= EPCS_BSY;
            } else {
                s->ep[n].cs_in = val;
            }
            return;
        }
        if (ca >= 0x7FC6 && ca <= 0x7FD3) {
            int n = (ca - 0x7FC6) / 2 + 1;
            if ((ca - 0x7FC6) & 1) {
                s->ep[n].bc_out = 0;
                s->ep[n].cs_out &= ~EPCS_BSY;
            } else {
                s->ep[n].cs_out = val;
            }
            return;
        }
        return;
    }

    /* External SRAM: 0x2000-0x7B3F and 0x8000-0xFFFF. Only the game's
     * backup half is what the save file keeps. */
    if (s->extmem) {
        uint32_t i = extmem_index(s, addr);

        if (s->extmem[i] != val) {
            cpu->events++;
            if (i >= 0x10000)
                s->backup_dirty = true;
        }
        s->extmem[i] = val;
    }
}

/* ── SFR callbacks (AN2131-specific registers) ────────────────────── */

static uint8_t an2131_sfr_read(Cpu8051State *cpu, uint8_t addr)
{
    AN2131State *s = (AN2131State *)cpu->opaque;
    if (addr >= SFR_TL0 && addr <= SFR_TH1) {
        cpu->events++;
    }
    switch (addr) {
    case SFR_EXIF:  return s->exif;
    case SFR_MPAGE: return s->mpage;
    case SFR_DPS:   return s->dps;
    case SFR_EIE:   return s->eie;
    case SFR_EIP:   return s->eip;
    default:        return cpu->sfr[addr - 0x80];
    }
}

/* XPSemu: an SC UART byte with nothing on the line (first 64). */
static void sc_uart_log(AN2131State *s, int uart, uint8_t val)
{
    static unsigned logged;
    if (logged < 64) {
        logged++;
        nc_log(TAG, "%s uart%d tx %02x (no board on this line yet)", s->name, uart, val);
    }
}

/* XPSemu: a JVS frame each way (first 200 exchanges). */
static void jvs_log(const char *what, const uint8_t *b, int n)
{
    static unsigned logged;
    if (logged >= 400) {
        return;
    }
    logged++;
    char hex[3 * 40 + 4] = "";
    int show = MIN(n, 40), o = 0;
    for (int i = 0; i < show; i++) {
        o += snprintf(hex + o, sizeof(hex) - o, " %02x", b[i]);
    }
    if (n > show) {
        snprintf(hex + o, sizeof(hex) - o, " ..");
    }
    nc_log(TAG, "jvs %s (%d):%s", what, n, hex);
}

static void an2131_sfr_write(Cpu8051State *cpu, uint8_t addr, uint8_t val)
{
    AN2131State *s = (AN2131State *)cpu->opaque;
    if ((addr >= SFR_TL0 && addr <= SFR_TH1) ||
        addr == 0x99 || addr == 0xC0 || addr == 0xC1) {
        cpu->events++;
    }
    switch (addr) {
    case SFR_EXIF:
        s->exif = val;
        return;
    case SFR_MPAGE:
        s->mpage = val;
        return;
    case SFR_DPS: {
        uint8_t old = s->dps;
        s->dps = val & 0x01;
        if ((val & 1) != (old & 1)) {
            uint16_t tmp = cpu->dptr;
            cpu->dptr = cpu->dptr_alt;
            cpu->dptr_alt = tmp;
        }
        return;
    }
    case SFR_EIE:
        s->eie = val;
        return;
    case SFR_EIP:
        s->eip = val;
        return;
    case 0x99: /* SBUF0 — UART0 TX (SC: RS-232C card reader) */
        cpu->sfr[addr - 0x80] = val;
        if (!s->is_qc && chihiro_crp1231_enabled && chihiro_crp1231_global) {
            /* patch 225: the Maximum Tune cabinet has its CRP-1231 here; TI deferred (card_rx_deliver) */
            crp1231_receive_byte(chihiro_crp1231_global, val);
            s->card_ti_cycles[0] = s->total_cycles | 1;
            return;
        }
        if (!s->is_qc && chihiro_hw210_enabled && chihiro_card_reader_global) {
            /* patch 225: player 2's HW210 (Ghost Squad). Real UART: TI rises when the byte finishes shifting out.
             * Raised at once, the TX interrupt runs one time too many at the end of a frame and the firmware's
             * byte counter underflows: the pump then retransmits forever (the fork's). */
            card_reader_write_byte(&chihiro_card_reader_global[1], val);
            s->card_ti_cycles[0] = s->total_cycles | 1;
            return;
        }
        if (!s->is_qc) {
            sc_uart_log(s, 0, val);
        }
        cpu->sfr[0x98 - 0x80] |= 0x02;  /* set TI0 */
        return;
    case 0xC0: /* SCON1 — defer next RX byte until after RETI */
    {
        uint8_t old = cpu->sfr[addr - 0x80];
        cpu->sfr[addr - 0x80] = val;
        if ((old & 0x01) && !(val & 0x01)) {
            s->jvs_rx_pending = true;
        }
        return;
    }
    case 0xC1: /* SBUF1 — serial TX (channel 1) */
    {
        if (!s->is_qc && chihiro_hw210_enabled) {
            /* patch 225: player 1's HW210 owns this channel (Ghost Squad); TI deferred as SBUF0 */
            if (chihiro_card_reader_global) {
                card_reader_write_byte(&chihiro_card_reader_global[0], val);
            }
            s->card_ti_cycles[1] = s->total_cycles | 1;
            return;
        }
        if (!s->is_qc && midi_sega838() && !chihiro_v257_global) {
            /* SC: MIDI to the drive board (patch 205); TI1 once the byte is out (midi_tick) */
            s->midi_ti_cycles = s->total_cycles | 1;
            driveboard_receive_byte(chihiro_driveboard_global, val);
            if (!s->midi_response_ready && driveboard_has_response(chihiro_driveboard_global)) {
                s->midi_response_ready = true;
                s->midi_response_set_cycles = s->total_cycles;
            }
            return;
        }
        if (!s->is_qc && chihiro_v257_global) {
            /* patch 225: MIDI to the V257 (Maximum Tune); TI1 at once, as the fork */
            cpu->sfr[0xC0 - 0x80] |= 0x02;
            midi_peer_send(val);
            if (!s->midi_response_ready && midi_peer_has_response()) {
                s->midi_response_ready = true;
                s->midi_response_set_cycles = s->total_cycles;
            }
            return;
        }
        cpu->sfr[0xC0 - 0x80] |= 0x02;  /* set TI1 — byte "sent" */

        if (!s->is_qc) {
            /* SC: MIDI to the cabinet's drive board: none on this cabinet */
            sc_uart_log(s, 1, val);
            return;
        }

        /* QC: RS-485 unescape: firmware escapes 0xE0→{0xD0,0xDF}, 0xD0→{0xD0,0xCF} */
        if (val == 0xE0) {
            if (s->jvs_tx_len > 0) {
                s->jvs_tx_len = 0;
                s->jvs_tx_expected = 0;
            }
            s->jvs_tx_escape = false;
            s->jvs_tx_buf[s->jvs_tx_len++] = val;
            return;
        }
        if (s->jvs_tx_escape) {
            val = val + 1;
            s->jvs_tx_escape = false;
        } else if (val == 0xD0) {
            s->jvs_tx_escape = true;
            return;
        }

        if (s->jvs_tx_len < (int)sizeof(s->jvs_tx_buf)) {
            s->jvs_tx_buf[s->jvs_tx_len++] = val;
        }
        if (s->jvs_tx_len == 3 && s->jvs_tx_buf[0] == 0xE0) {
            s->jvs_tx_expected = 3 + s->jvs_tx_buf[2];
        }

        if (s->jvs_tx_expected > 0 && s->jvs_tx_len >= s->jvs_tx_expected) {
            jvs_log("to the I/O board", s->jvs_tx_buf, s->jvs_tx_expected);
            if (chihiro_jvs_global) {
                uint8_t raw_resp[256];
                int rlen = chihiro_jvs_process(chihiro_jvs_global,
                                               s->jvs_tx_buf, s->jvs_tx_expected,
                                               raw_resp, sizeof(raw_resp));
                if (rlen > 0) {
                    jvs_log("from the I/O board", raw_resp, rlen);
                    /* RS-485 escape: bytes after SYNC that are 0xE0 or 0xD0 */
                    int epos = 0;
                    s->jvs_rx_buf[epos++] = raw_resp[0]; /* SYNC */
                    for (int i = 1; i < rlen && epos < (int)sizeof(s->jvs_rx_buf) - 1; i++) {
                        if (raw_resp[i] == 0xE0 || raw_resp[i] == 0xD0) {
                            s->jvs_rx_buf[epos++] = 0xD0;
                            s->jvs_rx_buf[epos++] = raw_resp[i] - 1;
                        } else {
                            s->jvs_rx_buf[epos++] = raw_resp[i];
                        }
                    }
                    s->jvs_rx_len = epos;
                    s->jvs_rx_pos = 0;
                    s->jvs_response_ready = true;
                    s->jvs_response_set_cycles = s->total_cycles;
                }
            }
            s->jvs_tx_len = 0;
            s->jvs_tx_expected = 0;
        }
        return;
    }
    default:
        cpu->sfr[addr - 0x80] = val;
        return;
    }
}

/* ── Interrupt logic ──────────────────────────────────────────────── */

static bool usb_irq_pending(AN2131State *s)
{
    if (s->usbirq & s->usbien) return true;
    if (s->in07irq & s->in07ien) return true;
    if (s->out07irq & s->out07ien) return true;
    return false;
}

static uint8_t usb_avec(AN2131State *s)
{
    if (s->usbirq & s->usbien & USBIRQ_SUDAV)  return AVEC_SUDAV;
    if (s->usbirq & s->usbien & USBIRQ_SOF)    return AVEC_SOF;
    if (s->usbirq & s->usbien & USBIRQ_SUTOK)  return AVEC_SUTOK;
    if (s->usbirq & s->usbien & USBIRQ_SUSP)   return AVEC_SUSPEND;
    if (s->usbirq & s->usbien & USBIRQ_URES)   return AVEC_USBRESET;

    for (int i = 0; i < 8; i++) {
        if (s->in07irq & s->in07ien & (1 << i))
            return (uint8_t)(AVEC_EP0IN + i * 8);
    }
    for (int i = 0; i < 8; i++) {
        if (s->out07irq & s->out07ien & (1 << i))
            return (uint8_t)(AVEC_EP0OUT + i * 8);
    }
    return 0xFF;
}

static void check_interrupts(AN2131State *s)
{
    Cpu8051State *cpu = &s->cpu;

    if (!(cpu->sfr[SFR_IE - 0x80] & 0x80)) return;
    if (cpu->in_interrupt) return;

    /* Timer 0 overflow (TF0 = TCON.5), enabled by IE.1 (ET0) */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x02) &&
        (cpu->sfr[SFR_TCON - 0x80] & 0x20)) {
        cpu->sfr[SFR_TCON - 0x80] &= ~0x20;
        cpu8051_interrupt(cpu, 0x000B);
        return;
    }

    /* Timer 1 overflow (TF1 = TCON.7), enabled by IE.3 (ET1) */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x08) &&
        (cpu->sfr[SFR_TCON - 0x80] & 0x80)) {
        cpu->sfr[SFR_TCON - 0x80] &= ~0x80;
        cpu8051_interrupt(cpu, 0x001B);
        return;
    }

    /* Serial interrupt (RI|TI in SCON), enabled by IE.4 (ES) */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x10) &&
        (cpu->sfr[SFR_SCON - 0x80] & 0x03)) {
        cpu8051_interrupt(cpu, 0x0023);
        return;
    }

    /* Second serial channel (SFR 0xC0 bits 0-1), enabled by IE.6 */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x40) &&
        (cpu->sfr[0xC0 - 0x80] & 0x03)) {
        cpu8051_interrupt(cpu, 0x003B);
        return;
    }

    /* USB interrupt (INT2), enabled by EIE.0 */
    if ((s->eie & 0x01) && usb_irq_pending(s)) {
        uint8_t avec = usb_avec(s);
        if (avec != 0xFF) {
            s->ivec = avec;
            /* Autovector: patch the LJMP target low byte at code[0x0045] */
            if (s->usbbav & USBBAV_AVEN) {
                s->ram[0x0045] = avec;
            }
            s->exif |= 0x10;
            cpu8051_interrupt(cpu, INT2_VECTOR);
            return;
        }
    }

    /* I2C interrupt (INT3), enabled by EIE.1 */
    if ((s->eie & 0x02) && s->i2c_irq_pending) {
        s->i2c_irq_pending = false;
        s->exif |= 0x20;
        cpu8051_interrupt(cpu, INT3_VECTOR);
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void an2131_init(AN2131State *s)
{
    memset(s, 0, sizeof(*s));
    cpu8051_init(&s->cpu);

    s->cpu.code = s->ram;
    s->cpu.code_size = AN2131_RAM_SIZE;
    s->cpu.xdata_read = an2131_xdata_read;
    s->cpu.xdata_write = an2131_xdata_write;
    s->cpu.sfr_read_cb = an2131_sfr_read;
    s->cpu.sfr_write_cb = an2131_sfr_write;
    s->cpu.opaque = s;
    s->name = "?";

    an2131_reset(s);
}

static void an2131_reset(AN2131State *s)
{
    cpu8051_reset(&s->cpu);
    memset(&s->idle, 0, sizeof(s->idle));

    s->cpu.code = s->ram;
    s->cpu.code_size = AN2131_RAM_SIZE;

    s->cpucs = CPUCS_8051RES;
    s->cpu_running = false;

    s->ep0cs = 0;
    s->usbbav = 0;
    s->usbcs = 0;
    s->usbpair = 0;
    s->in07val = 0x01;
    s->out07val = 0x01;
    s->fnaddr = 0;
    s->sudptr = 0;
    s->autoptr = 0;
    s->fastxfr = 0;
    s->togctl = 0;

    s->usbirq = 0;
    s->in07irq = 0;
    s->out07irq = 0;
    s->usbien = 0;
    s->in07ien = 0;
    s->out07ien = 0;
    s->ivec = 0;

    s->i2cs = I2CS_DONE;
    s->i2c_irq_pending = false;
    s->i2c_lastrd = false;
    memset(&s->i2c, 0, sizeof(s->i2c));
    s->i2c.phase = I2C_IDLE;

    /* Baseboard port defaults; the monitor bits come from AN_PINSA. */
    s->pinsa = 0xC9;
    s->pinsb = 0x52;
    s->pinsc = 0x00;
    s->outa = 0;
    s->outb = 0;
    s->outc = 0;
    s->oea = 0;
    s->oeb = 0;
    s->oec = 0;
    s->portacfg = 0;
    s->portbcfg = 0;
    s->portccfg = 0;

    s->exif = 0;
    s->eie = 0;
    s->eip = 0;
    s->mpage = 0;
    s->dps = 0;

    memset(s->ep, 0, sizeof(s->ep));
    memset(s->setupdat, 0, sizeof(s->setupdat));
}

void an2131_b2_boot(AN2131State *s, const uint8_t *eeprom, int eeprom_size)
{
    if (eeprom_size < 7 || eeprom[0] != 0xB2) {
        nc_log(TAG, "%s: B2 boot skipped, not a B2 EEPROM (first byte %02x)", s->name,
               eeprom_size > 0 ? eeprom[0] : 0);
        return;
    }

    uint16_t vid = eeprom[1] | ((uint16_t)eeprom[2] << 8);
    uint16_t pid = eeprom[3] | ((uint16_t)eeprom[4] << 8);
    uint16_t did = eeprom[5] | ((uint16_t)eeprom[6] << 8);
    nc_log(TAG, "%s: B2 boot: VID %04x PID %04x DID %04x", s->name, vid, pid, did);

    int pos = 7;
    int total_bytes = 0, records = 0;

    while (pos + 4 <= eeprom_size) {
        uint8_t lenh = eeprom[pos];
        uint8_t lenl = eeprom[pos + 1];
        bool last = lenh & 0x80;
        uint16_t len  = ((uint16_t)(lenh & 0x7F) << 8) | lenl;
        uint16_t addr = ((uint16_t)eeprom[pos + 2] << 8) | eeprom[pos + 3];
        pos += 4;

        if (len == 0) break;
        if (pos + len > eeprom_size) break;

        an2131_anchor_load(s, addr, &eeprom[pos], len);
        total_bytes += len;
        records++;
        pos += len;

        if (last)
            break;
    }

    nc_log(TAG, "%s: B2 boot: %d record(s), %d bytes loaded, 8051 %s", s->name, records,
           total_bytes, s->cpu_running ? "running" : "NOT running (no CPUCS release record)");
}

void an2131_anchor_load(AN2131State *s, uint16_t addr,
                        const uint8_t *data, int len)
{
    if (addr == 0x7F92) {
        if (len > 0)
            an2131_set_cpucs(s, data[0]);
        return;
    }
    for (int i = 0; i < len && (addr + i) < AN2131_RAM_SIZE; i++)
        s->ram[addr + i] = data[i];
}

static void an2131_set_cpucs(AN2131State *s, uint8_t val)
{
    bool was_reset = s->cpucs & CPUCS_8051RES;
    s->cpucs = val;

    if (was_reset && !(val & CPUCS_8051RES)) {
        cpu8051_reset(&s->cpu);

        s->total_cycles = 0;
        s->jvs_response_set_cycles = 0;
        s->jvs_response_ready = false;
        s->jvs_rx_pending = false;
        s->jvs_rx_len = 0;
        s->jvs_rx_pos = 0;
        s->jvs_tx_len = 0;
        s->jvs_tx_expected = 0;
        s->jvs_tx_escape = false;

        s->cpu_running = true;
        nc_log(TAG, "%s: 8051 released, running the firmware from 0000", s->name);
        const uint64_t steps0 = s->steps;
        an2131_run(s, 8000000);
        nc_log(TAG, "%s: 8051 init done (8M cycles, %llu instructions run): PC %04x SP %02x IE %02x "
               "EIE %02x USBIEN %02x CKCON %02x", s->name, (unsigned long long)(s->steps - steps0),
               s->cpu.pc, s->cpu.sp, s->cpu.sfr[SFR_IE - 0x80], s->eie, s->usbien,
               s->cpu.sfr[SFR_CKCON - 0x80]);
    } else if (!was_reset && (val & CPUCS_8051RES)) {
        s->cpu_running = false;
        nc_log(TAG, "%s: 8051 held in reset", s->name);
    }
}

int an2131_setup_packet(AN2131State *s, const uint8_t setup[8],
                        const uint8_t *out_data, int out_len,
                        uint8_t *resp_buf, int resp_max)
{
    if (!s->cpu_running) return -1;

    /* XPSemu patch 264: a request that comes while the 8051 is inside an interrupt routine waits for its RETI.
     * Clearing in_interrupt at once (as the fork did) let the next interrupt nest inside the running one: a JVS
     * byte delivered into the serial routine of the byte before it was lost (House of the Dead III's
     * JST_CHKSUM_ERROR, when its backup write (0x1F + EP3) pushed the status poll into the reply's arrival).
     * A routine that has not returned after 20000 cycles is taken as stale and cleared, as before. */
    if (s->cpu.in_interrupt) {
        static unsigned s_waits, s_stale;
        int waited = 0;
        while (s->cpu.in_interrupt && waited < 20000) {
            midi_tick(s);
            card_rx_deliver(s);
            const int c = cpu8051_step(&s->cpu);
            waited += c;
            s->total_cycles += c;
            s->steps++;
        }
        s_waits++;
        const bool stale = s->cpu.in_interrupt;
        if (stale) {
            s_stale++;
            s->cpu.in_interrupt = false;
        }
        if (s_waits <= 3 || (s_waits & (s_waits - 1)) == 0 || (stale && s_stale <= 20)) {
            nc_log(TAG, "%s: request %02x came inside an interrupt routine: waited %d cycles for its RETI "
                   "(%u times, %u stale) (patch 264)", s->name, setup[1], waited, s_waits, s_stale);
        }
    }

    memcpy(s->setupdat, setup, 8);

    /* Pre-load OUT0BUF for OUT control transfers */
    if (out_data && out_len > 0) {
        int copy = MIN(out_len, AN2131_EP_BUFSZ);
        memcpy(&s->ram[AN_OUT0BUF - 0x6000], out_data, copy);
        s->ep[0].bc_out = copy;
        s->ep0cs |= EP0CS_OUTBSY;
        s->out07irq |= 0x01;
        s->cpu.irq_recheck = true;
    }

    s->usbirq |= USBIRQ_SUDAV;
    s->cpu.irq_recheck = true;
    s->ep[0].in_armed = false;
    /* patch 225 (the fork's): hold card RX delivery only while a card drain (0x1A/0x1B) runs. */
    s->in_setup = !s->is_qc && (setup[0] & 0x80) && (setup[1] == 0x1A || setup[1] == 0x1B);

    bool is_in = setup[0] & 0x80;
    bool need_ep4 = (setup[1] == 0x19 && is_in);
    bool need_ep2 = (setup[1] == 0x17 && is_in);
    /* The SC card polls (0x1A/0x1B in, 0x22/0x23 out) take long, like the 0x17 EEPROM read, whichever reader
     * answers them. */
    bool need_card = !s->is_qc && (chihiro_hw210_enabled || chihiro_crp1231_enabled) &&
                     (((setup[1] == 0x1A || setup[1] == 0x1B) && is_in) || setup[1] == 0x22 || setup[1] == 0x23);

    /* On the bus the device NAKs the data stage until the firmware arms EP0
     * IN, however long that takes; here the request runs until the firmware
     * answers. HEURISTIC (the fork's): the cap, 100000 cycles (17 ms of 8051
     * time), past any exchange of the firmware's own (a JVS round trip is
     * under 45000). */
    int cycles = 0;
    int limit = 100000;
    int drain = 0;
    bool mainloop_hit = false;

    while (cycles < limit) {
        jvs_rx_deliver(s);
        midi_tick(s);
        card_rx_deliver(s);
        check_interrupts(s);
        int c = cpu8051_step(&s->cpu);
        cycles += c;
        s->total_cycles += c;
        s->steps++;

        if (drain && s->cpu.pc == 0x0696) mainloop_hit = true;

        if (is_in && s->ep[0].in_armed)
            s->ep0cs &= ~EP0CS_INBSY;

        if (!drain) {
            if (is_in && s->ep[0].in_armed) {
                if ((!need_ep4 || s->ep[4].in_armed) &&
                    (!need_ep2 || s->ep[2].in_armed))
                    drain = cycles;
            }
            if (!is_in && !(s->usbirq & USBIRQ_SUDAV)) drain = cycles;
        }
        if (drain && mainloop_hit && setup[1] == 0x20) break;
        if (drain && mainloop_hit && (cycles - drain) > 2000) break;
        /* Card requests: once the reply is armed the critical work is done (the bulk feeder keeps running from
         * an2131_run); the 20000-cycle drain tail on every poll slows the whole machine (the fork's). */
        if (drain && need_card && (cycles - drain) > 2000) break;
        if (drain && (cycles - drain) > 20000) break;
    }
    s->in_setup = false;
    if (cycles >= limit) {
        s->setup_capped++;
    }

    if (is_in && s->ep[0].in_armed) {
        int bc = s->ep[0].bc_in;
        int copy = MIN(bc, resp_max);
        memcpy(resp_buf, &s->ram[AN_IN0BUF - 0x6000], copy);

        s->ep[0].in_armed = false;
        s->ep0cs &= ~EP0CS_INBSY;
        return copy;
    }

    /* SUDPTR-based EP0 IN (AN2131 silicon feature for standard USB requests):
     * Firmware sets SUDPTRH:SUDPTRL to point to descriptor data in RAM,
     * then sets EP0CS. The silicon reads from SUDPTR automatically.
     * Vendor requests bypass this — they write IN0BUF + IN0BC directly. */
    if (is_in && s->sudptr != 0) {
        uint16_t ptr = s->sudptr;
        if (ptr < AN2131_RAM_SIZE) {
            int desc_len = s->ram[ptr];
            if (ptr + 3 < AN2131_RAM_SIZE && s->ram[ptr + 1] == 0x02) {
                desc_len = s->ram[ptr + 2] | (s->ram[ptr + 3] << 8);
            }
            int wLength = s->setupdat[6] | (s->setupdat[7] << 8);
            if (desc_len > wLength) desc_len = wLength;
            if (desc_len > resp_max) desc_len = resp_max;
            if (ptr + desc_len <= AN2131_RAM_SIZE) {
                memcpy(resp_buf, &s->ram[ptr], desc_len);
                s->sudptr = 0;
                return desc_len;
            }
        }
        s->sudptr = 0;
    }

    return 0;
}

int an2131_ep_in_poll(AN2131State *s, int ep_nr)
{
    if (ep_nr < 0 || ep_nr >= AN2131_EP_COUNT) return 0;
    if (!s->cpu_running) return 0;

    an2131_run(s, 6000);

    return s->ep[ep_nr].in_armed ? s->ep[ep_nr].bc_in : -1;
}

int an2131_ep_in_read(AN2131State *s, int ep_nr,
                      uint8_t *buf, int max_len)
{
    if (ep_nr < 0 || ep_nr >= AN2131_EP_COUNT) return 0;
    if (!s->ep[ep_nr].in_armed) return 0;

    int bc = s->ep[ep_nr].bc_in;
    int copy = MIN(bc, max_len);

    /* INnBUF RAM offset = 0x1F00 - ep_nr * 0x80 */
    uint16_t buf_off = 0x1F00 - (uint16_t)ep_nr * 0x80;
    if (buf_off + copy <= AN2131_RAM_SIZE)
        memcpy(buf, &s->ram[buf_off], copy);

    /* HEURISTIC: a short host read leaves the rest armed for the next one */
    if (copy < bc) {
        memmove(&s->ram[buf_off], &s->ram[buf_off + copy], bc - copy);
        s->ep[ep_nr].bc_in = bc - copy;
        return copy;
    }

    s->ep[ep_nr].in_armed = false;
    s->ep[ep_nr].bc_in = 0;
    s->ep[ep_nr].cs_in &= ~EPCS_BSY;

    /* Notify firmware: IN data was sent to host */
    s->in07irq |= (1 << ep_nr);
    s->cpu.irq_recheck = true;

    return copy;
}

void an2131_ep_out_write(AN2131State *s, int ep_nr,
                         const uint8_t *data, int len)
{
    if (ep_nr < 0 || ep_nr >= AN2131_EP_COUNT) return;
    if (!s->cpu_running) return;

    int copy = MIN(len, AN2131_EP_BUFSZ);

    /* OUTnBUF RAM offset = 0x1EC0 - ep_nr * 0x80 */
    uint16_t buf_off = 0x1EC0 - (uint16_t)ep_nr * 0x80;
    if (buf_off + copy <= AN2131_RAM_SIZE)
        memcpy(&s->ram[buf_off], data, copy);

    s->ep[ep_nr].bc_out = copy;
    s->ep[ep_nr].cs_out |= EPCS_BSY;

    s->out07irq |= (1 << ep_nr);
    s->cpu.irq_recheck = true;

    an2131_run(s, 2000);
}

static void jvs_rx_deliver(AN2131State *s)
{
    Cpu8051State *cpu = &s->cpu;
    if (cpu->in_interrupt) return;

    /* First byte: simulate RS-485 serial latency (~2ms at 115200 baud), in
     * CPU cycles so the delay works during an2131_run bursts. */
    if (s->jvs_response_ready) {
        uint64_t elapsed = s->total_cycles - s->jvs_response_set_cycles;
        if (elapsed < 15000) return;
        if (cpu->sfr[0xC0 - 0x80] & 0x02) return;  /* TI1 still set */
        cpu->events++;
        cpu->sfr[0xC1 - 0x80] = s->jvs_rx_buf[0];
        s->jvs_rx_pos = 1;
        cpu->sfr[0xC0 - 0x80] |= 0x01;  /* set RI1 */
        cpu->irq_recheck = true;
        s->jvs_response_ready = false;
        s->jvs_rx_pending = false;
        return;
    }

    /* Subsequent bytes: deliver after ISR returns */
    if (s->jvs_rx_pending) {
        if (s->jvs_rx_pos < s->jvs_rx_len) {
            uint8_t rxbyte = s->jvs_rx_buf[s->jvs_rx_pos];
            cpu->events++;
            cpu->sfr[0xC1 - 0x80] = rxbyte;
            s->jvs_rx_pos++;
            cpu->sfr[0xC0 - 0x80] |= 0x01;  /* set RI1 */
            cpu->irq_recheck = true;
        }
        s->jvs_rx_pending = false;
    }
}

/* ── The main loop at rest ─────────────────────────────────────────── */

/* Both firmwares idle in a polling loop, never in the 8051's IDLE mode. A
 * turn that leaves the chip as it found it repeats, so such turns are charged
 * at the chip's rate and not run. Exact: the fingerprint (registers, internal
 * RAM, SFRs but the timer counts) must repeat AND nothing outside it moved
 * during the turn (cpu.events). Timers advance by arithmetic, and a skip
 * stops short of an overflow that would raise a flag, of the budget, and of
 * a turn begun in an earlier call. */
static uint64_t an2131_mix(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 8) {
        uint64_t w;

        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}

static uint64_t an2131_idle_hash(AN2131State *s)
{
    Cpu8051State *c = &s->cpu;
    uint8_t sfr[128];
    const uint8_t regs[16] = {
        c->sp, c->acc, c->b, c->psw, c->in_interrupt,
        (uint8_t)c->dptr, (uint8_t)(c->dptr >> 8),
        (uint8_t)c->dptr_alt, (uint8_t)(c->dptr_alt >> 8),
        s->exif, s->eie, s->eip, s->dps, s->mpage,
    };
    uint64_t h = 0x243F6A8885A308D3ull;

    memcpy(sfr, c->sfr, sizeof(sfr));
    sfr[SFR_TL0 - 0x80] = sfr[SFR_TH0 - 0x80] = 0;
    sfr[SFR_TL1 - 0x80] = sfr[SFR_TH1 - 0x80] = 0;
    _Static_assert(sizeof(regs) % 8 == 0 && sizeof(c->iram) % 8 == 0 && sizeof(sfr) % 8 == 0,
                   "an2131_mix takes whole eight-byte words");
    h = an2131_mix(h, regs, sizeof(regs));
    h = an2131_mix(h, c->iram, sizeof(c->iram));
    return an2131_mix(h, sfr, sizeof(sfr));
}

static bool an2131_idle_quiet(AN2131State *s)
{
    Cpu8051State *c = &s->cpu;
    uint8_t ie = c->sfr[SFR_IE - 0x80], tcon = c->sfr[SFR_TCON - 0x80];

    if (c->in_interrupt) {
        return false;
    }
    if (s->jvs_response_ready || s->jvs_rx_pending || s->midi_response_ready || s->midi_ti_cycles ||
        s->card_ti_cycles[0] || s->card_ti_cycles[1] ||
        usb_irq_pending(s) || s->i2c_irq_pending ||
        (s->ep[0].in_armed && (s->ep0cs & EP0CS_INBSY))) {
        return false;
    }
    /* patch 225 (the fork's): a peer on the SC's UARTs with bytes still to give (the drive board's MIDI answer,
     * a card reader's) keeps the turns running. */
    if ((!s->is_qc && !chihiro_hw210_enabled && midi_peer_has_response()) || sc_reader_pending(s)) {
        return false;
    }
    return !((ie & 0x80) &&
             (((ie & 0x02) && (tcon & 0x20)) || ((ie & 0x08) && (tcon & 0x80)) ||
              ((ie & 0x10) && (c->sfr[SFR_SCON - 0x80] & 0x03)) ||
              ((ie & 0x40) && (c->sfr[SFR_SCON1 - 0x80] & 0x03))));
}

static uint64_t an2131_count8(uint8_t *tl, uint8_t th, uint64_t ticks)
{
    uint64_t first = 256 - *tl, period = 256 - th;

    if (ticks < first) {
        *tl += ticks;
        return 0;
    }
    ticks -= first;
    *tl = th + ticks % period;
    return 1 + ticks / period;
}

static uint64_t an2131_count16(uint8_t *tl, uint8_t *th, uint64_t ticks)
{
    uint64_t t = *tl | (*th << 8), first = 0x10000 - t, overflows = 0;

    if (ticks < first) {
        t += ticks;
    } else {
        ticks -= first;
        overflows = 1 + ticks / 0x10000;
        t = ticks % 0x10000;
    }
    *tl = (uint8_t)t;
    *th = (uint8_t)(t >> 8);
    return overflows;
}

static uint64_t an2131_steps_to_overflow(AN2131State *s)
{
    Cpu8051State *c = &s->cpu;
    uint8_t tcon = c->sfr[SFR_TCON - 0x80], tmod = c->sfr[SFR_TMOD - 0x80];
    uint8_t m0 = tmod & 3, m1 = (tmod >> 4) & 3;
    uint64_t limit = UINT64_MAX;

    if ((tcon & 0x10) && !(tcon & 0x20) && (m0 == 1 || m0 == 2)) {
        uint64_t ticks = m0 == 1
            ? 0x10000 - (c->sfr[SFR_TL0 - 0x80] | (c->sfr[SFR_TH0 - 0x80] << 8))
            : 256 - c->sfr[SFR_TL0 - 0x80];
        limit = (c->sfr[SFR_CKCON - 0x80] & 0x08) ? ticks
                                              : 3 * ticks - c->timer0_prescale;
    }
    if ((tcon & 0x40) && !(tcon & 0x80) && !(tmod & 0x40) &&
        (m1 == 1 || m1 == 2)) {
        uint64_t ticks = m1 == 1
            ? 0x10000 - (c->sfr[SFR_TL1 - 0x80] | (c->sfr[SFR_TH1 - 0x80] << 8))
            : 256 - c->sfr[SFR_TL1 - 0x80];
        limit = MIN(limit, ticks);
    }
    return limit;
}

static void an2131_advance_timers(AN2131State *s, uint64_t steps)
{
    Cpu8051State *c = &s->cpu;
    uint8_t *sfr = c->sfr;
    uint8_t tcon = sfr[SFR_TCON - 0x80], tmod = sfr[SFR_TMOD - 0x80];
    uint8_t m0 = tmod & 3, m1 = (tmod >> 4) & 3;
    uint64_t overflows;

    if (tcon & 0x10) {
        uint64_t ticks = steps;
        if (!(sfr[SFR_CKCON - 0x80] & 0x08)) {
            ticks = (c->timer0_prescale + steps) / 3;
            c->timer0_prescale = (c->timer0_prescale + steps) % 3;
        }
        overflows = m0 == 1 ? an2131_count16(&sfr[SFR_TL0 - 0x80],
                                             &sfr[SFR_TH0 - 0x80], ticks)
                  : m0 == 2 ? an2131_count8(&sfr[SFR_TL0 - 0x80],
                                            sfr[SFR_TH0 - 0x80], ticks)
                  : 0;
        if (overflows) {
            tcon |= 0x20;
            c->irq_recheck = true;
        }
    }
    if ((tcon & 0x40) && !(tmod & 0x40)) {
        overflows = m1 == 1 ? an2131_count16(&sfr[SFR_TL1 - 0x80],
                                             &sfr[SFR_TH1 - 0x80], steps)
                  : m1 == 2 ? an2131_count8(&sfr[SFR_TL1 - 0x80],
                                            sfr[SFR_TH1 - 0x80], steps)
                  : 0;
        if (overflows) {
            tcon |= 0x80;
            c->irq_recheck = true;
        }
    }
    sfr[SFR_TCON - 0x80] = tcon;
}

static int an2131_idle_turn(AN2131State *s, int budget_left)
{
    uint64_t hash, turn_c, turn_s, k, limit;
    bool same;

    if (s->cpu.pc != s->idle.pc) {
        if (++s->idle.sample >= 4096) {
            s->idle.sample = 0;
            s->idle.pc = s->cpu.pc;
            s->idle.hash = 0;
        }
        return 0;
    }
    hash = an2131_idle_hash(s);
    turn_c = s->total_cycles - s->idle.cycles;
    turn_s = s->steps - s->idle.steps;
    same = hash == s->idle.hash && s->cpu.events == s->idle.events &&
           turn_c > 0 && turn_s > 0;
    s->idle.hash = hash;
    s->idle.cycles = s->total_cycles;
    s->idle.steps = s->steps;
    s->idle.events = s->cpu.events;
    if (!same || budget_left <= 0 || !an2131_idle_quiet(s)) {
        return 0;
    }
    k = (uint64_t)budget_left / turn_c;
    limit = an2131_steps_to_overflow(s);
    if (limit != UINT64_MAX) {
        k = MIN(k, (limit - 1) / turn_s);
    }
    if (k == 0) {
        return 0;
    }
    an2131_advance_timers(s, k * turn_s);
    s->total_cycles += k * turn_c;
    s->steps += k * turn_s;
    s->idle.cycles = s->total_cycles;
    s->idle.steps = s->steps;
    return (int)(k * turn_c);
}

void an2131_relink(AN2131State *s)
{
    s->cpu.code = s->ram;
    s->cpu.code_size = AN2131_RAM_SIZE;
    s->cpu.xdata_read = an2131_xdata_read;
    s->cpu.xdata_write = an2131_xdata_write;
    s->cpu.sfr_read_cb = an2131_sfr_read;
    s->cpu.sfr_write_cb = an2131_sfr_write;
    s->cpu.opaque = s;
    s->cpu.irq_recheck = true;
    s->i2c.phase = I2C_IDLE;
    s->i2c.eeprom = NULL;
    memset(&s->idle, 0, sizeof(s->idle));
    s->midi_response_ready = false;
    s->midi_response_set_cycles = 0;
    s->midi_ti_cycles = 0;
    /* patch 225: the card pacing stamps count against total_cycles: after a load they start over */
    an2131_card_pacing_reset(s);
}

int an2131_run(AN2131State *s, int max_cycles)
{
    if (!s->cpu_running) return 0;

    int total = 0;
    /* A turn is only trusted if it ran whole within this call. */
    s->idle.hash = 0;
    while (total < max_cycles) {
        if (s->jvs_response_ready || s->jvs_rx_pending) {
            jvs_rx_deliver(s);
        }
        midi_tick(s);
        card_rx_deliver(s);
        if (s->cpu.irq_recheck) {
            s->cpu.irq_recheck = false;
            check_interrupts(s);
        }
        /* A late EP0-IN reply (armed after the setup window returned): the
         * host on silicon always drains EP0, so it is consumed here and the
         * firmware returns to its main loop. */
        if (s->ep[0].in_armed && (s->ep0cs & EP0CS_INBSY)) {
            s->cpu.events++;
            s->ep0cs &= ~EP0CS_INBSY;
            s->ep[0].in_armed = false;
        }
        int c = cpu8051_step(&s->cpu);
        total += c;
        s->total_cycles += c;
        s->steps++;
        int idle = an2131_idle_turn(s, max_cycles - total);
        total += idle;
        s->idle_cycles += (uint64_t)idle;
    }
    return total;
}
