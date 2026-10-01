/*
 * XPSemu: the Xbox's own settings in its EEPROM (hw/xbox/smbus_storage.c)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef HW_XBOX_XEMU_EEPROM_H
#define HW_XBOX_XEMU_EEPROM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The video settings the Xbox dashboard sets (Settings > Video), which
 * games read when they start: what they may use, not what they must. */
#define XBOX_VIDEO_WIDESCREEN 0x00010000
#define XBOX_VIDEO_720P       0x00020000
#define XBOX_VIDEO_1080I      0x00040000
#define XBOX_VIDEO_480P       0x00080000
#define XBOX_VIDEO_LETTERBOX  0x00100000

/* The EEPROM's video flags. Setting them updates the running Xbox's EEPROM
 * and its file (with the user section's checksum); the Xbox reads them
 * when it boots, so they apply from the next game started. Need the
 * machine lock (the UI holds it). */
bool xemu_eeprom_get_video_flags(uint32_t *flags);
bool xemu_eeprom_set_video_flags(uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif
