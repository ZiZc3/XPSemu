/*
 * XPSemu: saves on the Xbox hard disk.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef HW_XBOX_XEMU_SAVE_REPAIR_H
#define HW_XBOX_XEMU_SAVE_REPAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Xbox's E: partition (saves, title data) on its hard disk */
#define XSR_E_OFFSET 0xABE80000ull
#define XSR_E_SIZE   0x1312D6000ull

typedef struct XsrDisk {
    void *opaque;
    bool (*read)(void *opaque, uint64_t off, void *buf, size_t len);
    bool (*write)(void *opaque, uint64_t off, const void *buf, size_t len);
} XsrDisk;

/*
 * Removes the game's broken saves: folders in E:\UDATA\<title ID> whose
 * only files are empty, with an empty SaveMeta.xbx (a save cut off while
 * it was being made). The game then can't save at all ("save failed") and
 * asks again every time. Every changed sector is first written to @undo
 * (records: 8-byte offset, 4-byte length, the old bytes; may be NULL).
 * The names of the removed folders go to @names. Returns how many were
 * removed, or -1 if the disk doesn't look as expected (nothing written).
 */
int xsr_repair_title(const XsrDisk *disk, uint64_t part_off,
                     uint64_t part_size, uint32_t title_id, FILE *undo,
                     char *names, size_t names_len);

/* On the running Xbox's hard disk (the machine stopped, the BQL held);
 * the undo records go to /data/xemu/save-repair/. */
int xemu_repair_saves(uint32_t title_id);

/* Writes the Xbox has made to its system and save partitions (C:, E:),
 * not the game cache partitions (X:, Y:, Z:) that streaming games write
 * all the time. The IDE disk calls xemu_note_hdd_write for each. */
#define XSR_SAVE_PARTS_FROM 0x8CA80000ull
uint64_t xemu_hdd_write_count(void);
void xemu_note_hdd_write(uint64_t offset);

#ifdef __cplusplus
}
#endif

#endif
