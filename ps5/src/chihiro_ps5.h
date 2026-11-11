/* XPSemu: Sega Chihiro game images (chihiro_ps5.c). Our own code. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RxChihiroInfo {
    bool ok;
    char name[40];          /* boot.id's game name */
    char maker[40];
    char game_id[12];       /* boot.id 0x30 */
    char exe[40];           /* the game's XBE in the volume */
    uint8_t regions;        /* bit 1 Japan, bit 2 USA, bit 3 export */
    bool type3;
    bool encrypted;         /* a GD-ROM unlocked with its security key */
    char format[16];        /* "GD-ROM" or "Netboot" */
    char why[128];          /* when not ok */
} RxChihiroInfo;

bool rx_chihiro_info(const char *path, RxChihiroInfo *info);
bool rx_chihiro_unpack(const char *path, const char *dest, char *why, size_t why_cap);
/* patch 219: no unpacking: only boot.id to dest; the core reads the image in place */
bool rx_chihiro_prepare(const char *path, const char *dest, char *why, size_t why_cap);
typedef void (*RxChihiroAdd)(void *ctx, const char *path, uint32_t size, unsigned id);
unsigned rx_chihiro_mount(const char *path, RxChihiroAdd add, void *ctx);
bool rx_chihiro_mount_read(unsigned id, uint64_t off, void *buf, size_t len);

void *rx_chihiro_img_open(const char *path, char *why, size_t why_cap);
bool rx_chihiro_img_read(const void *handle, void *buf, size_t len, uint64_t off);
uint64_t rx_chihiro_img_size(const void *handle);
void rx_chihiro_img_close(void *handle);

#ifdef __cplusplus
}
#endif
