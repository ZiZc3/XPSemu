/*
 * hakuX native core — x86 I/O ports.
 *
 * Devices claim ports as the frontier shows them used; the rest read as all
 * ones and drop writes, as unclaimed ports do on the bus. The first accesses
 * to each port are logged.
 *   0x8008  ACPI power-management timer: free-running at 3.375 MHz
 *   0x80C0  MCPX GPIO: bit 5 the video field, odd / even by turns at each
 *           vblank (the other bits as an unclaimed port's)
 */
#include "native/dev/dev.h"
#include "native/core/trainer.h"

#include <time.h>

#define ACPI_PM_TIMER 0x8008u
/* D3D's vblank DPC reads the field from bit 5 of this port into the
 * device (5849 LTCG: 0x4c07bd, [dev+0x1df8]), and GetDisplayFieldStatus
 * reports it. Read as all ones, every field was the same one, and the XMV
 * player, which starts a movie on the other field, never started
 * (Riddick's movies stayed black in push-buffer mode). */
#define MCPX_GPIO_FIELD 0x80C0u
#define FIELD_BIT       0x20u
#define LOGGED_PORTS  64

static struct {
    uint16_t port;
    unsigned count;
} g_seen[LOGGED_PORTS];

static void note(uint16_t port, bool out, unsigned bytes, uint32_t value)
{
    for (int i = 0; i < LOGGED_PORTS; i++) {
        if (g_seen[i].count && g_seen[i].port == port) {
            if (++g_seen[i].count <= 4) {
                break;
            }
            return;
        }
        if (!g_seen[i].count) {
            g_seen[i].port = port;
            g_seen[i].count = 1;
            break;
        }
    }
    nc_log("hakuX-native-io", "port %s %04x %s %0*x (%u)", out ? "out" : "in", port, out ? "<-" : "->",
           (int)(2 * bytes), value, bytes);
}

/* The guest's time (native/kernel/ke.c): it stands still while the
 * emulation is paused and goes on from a savestate's. */
uint64_t nc_ke_now_ns(void);

static uint32_t acpi_timer(void)
{
    const uint64_t ns = nc_ke_now_ns();
    return (uint32_t)((unsigned __int128)ns * 3375000u / 1000000000u);
}

/* Sega Chihiro media board LPC registers (XPSemu patch 193; Cxbx-Reloaded MediaBoard.cpp, GPL-2.0-or-later) */
#include <stdlib.h>
#include "native/core/irq.h"
static uint32_t g_mb_4026;
/* patch 256 (native/kernel/io.c): a Type-3 media board's registers, on for the games that need one */
bool nc_mb_t3_port_in(uint16_t port, unsigned bytes, uint32_t *v);
bool nc_mb_t3_port_out(uint16_t port, unsigned bytes, uint32_t v);

static bool mb_port(uint16_t port)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("NC_CHIHIRO") != NULL;
    }
    return on && port >= 0x4000 && port <= 0x40FF;
}

static uint32_t mb_lpc_read(uint16_t port)
{
    switch (port) {
    case 0x40E0: return 1;          /* alive */
    case 0x401E: return 0x0317;     /* firmware version */
    case 0x4020: return 0x00A0;
    case 0x4022: return 0x4258;     /* "XB" */
    case 0x4024: return 0x4D41;     /* "MA" */
    case 0x40F4: return 0x0003;     /* DIMM size: 1 GB */
    case 0x4026: return g_mb_4026;  /* handshake */
    case 0x40F0: return 0x0000;     /* board type 1 */
    default: return 0;
    }
}

uint32_t nc_port_in(uint16_t port, unsigned bytes)
{
    uint32_t tv;
    if (nc_trainer_port_in(port, bytes, &tv)) {    /* patch 246: a trainer's SMBus reads */
        return tv;
    }
    if (mb_port(port)) {
        uint32_t v;
        if (!nc_mb_t3_port_in(port, bytes, &v)) {   /* patch 256: a Type-3 board's registers (Maximum Tune) */
            v = mb_lpc_read(port);
        }
        if (port != 0x4000) {                       /* the ASIC window's data port: thousands, not logged */
            note(port, false, bytes, v);
        }
        return v;
    }
    uint32_t v;
    switch (port) {
    case ACPI_PM_TIMER:
        return acpi_timer();
    case MCPX_GPIO_FIELD:
        v = (bytes == 4 ? 0xFFFFFFFFu : (1u << (8 * bytes)) - 1) & ~FIELD_BIT;
        return v | ((nc_dev_nv2a_vblanks() & 1) ? FIELD_BIT : 0);
    default:
        v = bytes == 4 ? 0xFFFFFFFFu : (1u << (8 * bytes)) - 1;
        break;
    }
    note(port, false, bytes, v);
    return v;
}

void nc_port_out(uint16_t port, unsigned bytes, uint32_t value)
{
    if (nc_trainer_port_out(port, bytes, value)) {     /* patch 246: the trainer's stub, its SMBus writes */
        return;
    }
    if (mb_port(port) && nc_mb_t3_port_out(port, bytes, value)) {   /* patch 256: a Type-3 board */
        if (port != 0x4000 && port != 0x4004 && port != 0x4008) {
            note(port, true, bytes, value);
        }
        return;
    }
    if (mb_port(port)) {
        if (port == 0x40E1 || port == 0x40E0) {
            nc_irq_set(10, false);  /* IRQ 10 handled (Cxbx: 40E1; the fork: 40E0) */
        } else if (port == 0x4026) {
            g_mb_4026 = value;
        }
    }
    note(port, true, bytes, value);
}
