/*
 * hakuX native core — the Sega Chihiro base boards (QC and SC, two Cypress
 * AN2131 USB devices) on the first OHCI controller, Chihiro mode only
 * (NC_CHIHIRO). XPSemu phase 3; see chihiro_usb.c for the credits.
 *
 * ohci.c calls these with its controller lock held: the boards' 8051s run
 * on the OHCI frame thread (and in a port reset, on the CPU thread that
 * wrote the port), never two at once.
 */
#ifndef NATIVE_DEV_CHIHIRO_USB_H
#define NATIVE_DEV_CHIHIRO_USB_H

#include "native/dev/usb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NC_CHIHIRO set. */
bool nc_chihiro_usb_enabled(void);
/* Load the firmware dumps, make the boards and run their firmware's init;
 * called once from nc_dev_ohci_init. */
void nc_chihiro_usb_init(void);
/* The guest put the controller into OPERATIONAL. */
void nc_chihiro_usb_bus_start(void);
/* Every 1 ms frame. */
void nc_chihiro_usb_frame(void);
void nc_chihiro_usb_run(void);
/* patch 207: player 1's controller (Xbox layout, as xid.c gets it) to the JVS I/O board */
void nc_chihiro_pad_input(const NcPadState *s);
/* patch 220: the gun games' crosshair, a fraction of the picture (false: none, or off the picture) */
bool nc_chihiro_crosshair(float *x, float *y);
/* patch 227: a mouse for the gun games (the PS5 app reads it): movement counts, buttons left 1 right 2 middle 4 side 8/16 */
void nc_chihiro_mouse(int dx, int dy, uint32_t buttons);
/* patch 209: the drive board's rumble for player 1's motors (0-0xFFFF) */
void nc_chihiro_rumble(uint16_t *left, uint16_t *right);
void nc_ohci_root_restore(unsigned port, NcUsbDev *d);         /* patch 196: outside the OHCI lock */

/* ohci.c: a device into root port 1-4 (NULL: pull it out), as a hot plug:
 * connection change and RHSC. Controller lock held. */
void nc_ohci_root_plug(unsigned port, NcUsbDev *d);

#ifdef __cplusplus
}
#endif

#endif
