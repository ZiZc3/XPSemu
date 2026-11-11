/*
 * Cypress AN2131 (EZ-USB) register mapping for Chihiro baseboard.
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * XPSemu (hakuX native core, Sega Chihiro phase 3): adapted from tattoohanz's
 * xemu-based Chihiro fork (hw/xbox/chihiro/chihiro-an2131.h): the SC's drive
 * board, card reader and trace parts left out; a few counters for our logs.
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
 * Provides the hardware register layer between the generic 8051 CPU core
 * and the AN2131-specific USB, I2C, and port I/O peripherals.
 * The ic10/pc20 firmware runs on the 8051 and accesses these registers
 * via XDATA (MOVX) and SFR instructions.
 *
 * Register map from Cypress EZ-USB Technical Reference Manual v1.9/1.10.
 */

#ifndef NATIVE_DEV_CHIHIRO_AN2131_H
#define NATIVE_DEV_CHIHIRO_AN2131_H

#include "native/dev/chihiro/i8051.h"

/* ── Memory layout ──────────────────────────────────────────────────── */

#define AN2131_RAM_SIZE     8192    /* 8KB on-chip code/data RAM */
#define AN2131_EP_COUNT     8       /* EP0-EP7 */
#define AN2131_EP_BUFSZ     64      /* Bytes per endpoint buffer */

#define AN_IN0BUF      0x7F00
#define AN_OUT0BUF     0x7EC0

/* ── XDATA register addresses (0x7F40-0x7FFF) ──────────────────────── */

#define AN_CPUCS       0x7F92

#define AN_PORTACFG    0x7F93
#define AN_PORTBCFG    0x7F94
#define AN_PORTCCFG    0x7F95
#define AN_OUTA        0x7F96
#define AN_OUTB        0x7F97
#define AN_OUTC        0x7F98
#define AN_PINSA       0x7F99
#define AN_PINSB       0x7F9A
#define AN_PINSC       0x7F9B
#define AN_OEA         0x7F9C
#define AN_OEB         0x7F9D
#define AN_OEC         0x7F9E

#define AN_I2CS        0x7FA5
#define AN_I2DAT       0x7FA6

#define AN_IVEC        0x7FA8
#define AN_IN07IRQ     0x7FA9
#define AN_OUT07IRQ    0x7FAA
#define AN_USBIRQ      0x7FAB
#define AN_IN07IEN     0x7FAC
#define AN_OUT07IEN    0x7FAD
#define AN_USBIEN      0x7FAE
#define AN_USBBAV      0x7FAF

#define AN_EP0CS       0x7FB4
#define AN_IN0BC       0x7FB5
#define AN_OUT0BC      0x7FC5

#define AN_SUDPTRH     0x7FD4
#define AN_SUDPTRL     0x7FD5
#define AN_USBCS       0x7FD6
#define AN_TOGCTL      0x7FD7
#define AN_FNADDR      0x7FDB
#define AN_USBPAIR     0x7FDD
#define AN_IN07VAL     0x7FDE
#define AN_OUT07VAL    0x7FDF

#define AN_FASTXFR     0x7FE2
#define AN_AUTOPTRH    0x7FE3
#define AN_AUTOPTRL    0x7FE4
#define AN_AUTODATA    0x7FE5

#define AN_SETUPDAT    0x7FE8

/* ── Bit definitions ────────────────────────────────────────────────── */

#define USBIRQ_SUDAV   0x01
#define USBIRQ_SOF     0x02
#define USBIRQ_SUTOK   0x04
#define USBIRQ_SUSP    0x08
#define USBIRQ_URES    0x10

#define I2CS_START     0x80
#define I2CS_STOP      0x40
#define I2CS_LASTRD    0x20
#define I2CS_ACK       0x02
#define I2CS_DONE      0x01

#define EP0CS_OUTBSY   0x08
#define EP0CS_INBSY    0x04

#define EPCS_BSY       0x02

#define USBBAV_AVEN    0x01

#define CPUCS_8051RES  0x01

#define AVEC_SUDAV     0x00
#define AVEC_SOF       0x04
#define AVEC_SUTOK     0x08
#define AVEC_SUSPEND   0x0C
#define AVEC_USBRESET  0x10
#define AVEC_EP0IN     0x18
#define AVEC_EP0OUT    0x1C

#define INT2_VECTOR    0x0043   /* USB (all sources via autovector) */
#define INT3_VECTOR    0x004B   /* I2C DONE */

#define SFR_DPS        0x86
#define SFR_CKCON      0x8E
#define SFR_EXIF       0x91
#define SFR_MPAGE      0x92
#define SFR_SCON1      0xC0
#define SFR_EIE        0xE8
#define SFR_EIP        0xF8

typedef enum {
    I2C_IDLE,
    I2C_ADDR,
    I2C_MEM_ADDR,
    I2C_DATA
} I2CPhase;

typedef struct AN2131State {
    Cpu8051State cpu;

    uint8_t ram[AN2131_RAM_SIZE];

    struct {
        uint8_t cs_in;
        uint8_t bc_in;
        uint8_t cs_out;
        uint8_t bc_out;
        bool    in_armed;
    } ep[AN2131_EP_COUNT];
    uint8_t ep0cs;

    uint8_t ivec;
    uint8_t in07irq;
    uint8_t out07irq;
    uint8_t usbirq;
    uint8_t in07ien;
    uint8_t out07ien;
    uint8_t usbien;
    uint8_t usbbav;

    uint8_t i2cs;
    bool    i2c_irq_pending;
    bool    i2c_lastrd;
    struct {
        I2CPhase phase;
        uint16_t mem_addr;
        bool     reading;
        bool     first_read;
        int      addr_bytes_sent;
        int      addr_bytes_needed;
        uint8_t *eeprom;
        int      eeprom_size;
        /* XPSemu: the transaction for the log (device, start address, bytes) */
        uint8_t  dev7;
        uint16_t start;
        int      count;
    } i2c;

    uint8_t *ic10_eeprom;   /* 8KB: ic10 (QC) or pc20 (SC) */
    int      ic10_size;
    uint8_t *ic11_eeprom;   /* baseboard config: the 128-byte dump, 256-byte window */
    int      ic11_size;

    uint8_t  rtc_regs[16];

    /* The baseboard SRAM, 128 KB: two 64 KB halves; on the QC, port C bit 3
     * picks one (half 0 the firmware's work RAM, half 1 the game's backup). */
    uint8_t *extmem;

    uint8_t  cpucs;
    uint8_t  usbcs;
    uint8_t  fnaddr;
    uint8_t  togctl;
    uint8_t  usbpair;
    uint8_t  in07val;
    uint8_t  out07val;
    uint16_t sudptr;
    uint8_t  fastxfr;
    uint16_t autoptr;
    uint8_t  setupdat[8];

    uint8_t outa, outb, outc;
    uint8_t pinsa, pinsb, pinsc;
    uint8_t oea, oeb, oec;
    uint8_t portacfg, portbcfg, portccfg;

    uint8_t exif;
    uint8_t eie;
    uint8_t eip;
    uint8_t mpage;
    uint8_t dps;

    /* JVS serial (SBUF1 at SFR 0xC1) */
    uint8_t jvs_tx_buf[256];
    int     jvs_tx_len;
    int     jvs_tx_expected;
    bool    jvs_tx_escape;
    uint8_t jvs_rx_buf[512];
    int     jvs_rx_len;
    int     jvs_rx_pos;

    bool is_qc;             /* true = JVS/RS-485, false = MIDI/drive board */
    const char *name;       /* XPSemu: "QC" / "SC" for the log */

    /* The battery-backed half of the SRAM, or the settings EEPROM, holds
     * something written since the save file last did. */
    bool     backup_dirty;
    bool     ic11_dirty;    /* XPSemu: the ic11 window was written */

    bool cpu_running;
    bool jvs_response_ready;
    bool jvs_rx_pending;
    uint64_t total_cycles;
    uint64_t jvs_response_set_cycles;

    struct {
        uint16_t pc;
        int      sample;
        uint64_t hash;
        uint64_t cycles;
        uint64_t steps;
        uint32_t events;
    } idle;
    uint64_t steps;
    uint64_t idle_cycles;   /* XPSemu: cycles charged for idle turns not run */
    bool     midi_response_ready;       /* patch 205 (the fork's): the drive board's answer, paced */
    uint64_t midi_response_set_cycles;
    uint64_t midi_ti_cycles;            /* a MIDI byte's TX-complete pending: the SBUF1 write time (0 none) */
    uint32_t setup_capped;  /* XPSemu: control requests that ran to the cycle cap */
    /* patch 225 (the fork's): the card readers on the SC's UARTs. A card drain request (0x1A/0x1B) is being
     * serviced; per channel ([0] UART0, [1] UART1): when the reader's answer (or its last byte) was paced from,
     * whether a reply is being delivered, a TX-complete pending (the SBUF write time, 0 none). */
    bool     in_setup;
    uint64_t card_resp_cycles[2];
    bool     card_delivering[2];
    uint64_t card_ti_cycles[2];
} AN2131State;

void an2131_init(AN2131State *s);
void an2131_b2_boot(AN2131State *s, const uint8_t *eeprom, int eeprom_size);

void an2131_anchor_load(AN2131State *s, uint16_t addr,
                        const uint8_t *data, int len);

int  an2131_setup_packet(AN2131State *s, const uint8_t setup[8],
                         const uint8_t *out_data, int out_len,
                         uint8_t *resp_buf, int resp_max);

int  an2131_ep_in_poll(AN2131State *s, int ep_nr);
int  an2131_ep_in_read(AN2131State *s, int ep_nr,
                       uint8_t *buf, int max_len);
void an2131_ep_out_write(AN2131State *s, int ep_nr,
                         const uint8_t *data, int len);

int  an2131_run(AN2131State *s, int max_cycles);
/* patch 209 (the fork's): after a savestate's bytes were copied in, the pointers and callbacks back */
void an2131_relink(AN2131State *s);

/* XPSemu: the monitor bit on port A (the cabinet's: 15 kHz bit 1, the
 * default; 31 kHz bit 2). */
extern bool an2131_monitor_31khz;

/* XPSemu patch 205: the cabinet's drive board on the SC's MIDI line (UART1), Sega 838-13683 (OutRun 2) */
extern bool an2131_drive_sega838;
/* patch 225: the other peers on the SC's UARTs come from their own globals: the V257 (Maximum Tune, on UART1:
 * chihiro_v257_global), the HW210 readers (Ghost Squad, UART1 and UART0: chihiro_hw210_enabled) and the CRP-1231
 * (Maximum Tune, UART0: chihiro_crp1231_enabled). */

#endif
