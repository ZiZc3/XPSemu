/* XOG: your button layout (padmap_ps5.c). GPL-3.0-or-later. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS5_SRC_CROSS, PS5_SRC_CIRCLE, PS5_SRC_SQUARE, PS5_SRC_TRIANGLE,
    PS5_SRC_L1, PS5_SRC_R1, PS5_SRC_L2, PS5_SRC_R2, PS5_SRC_L3, PS5_SRC_R3,
    PS5_SRC_OPTIONS, PS5_SRC_CREATE, PS5_SRC_UP, PS5_SRC_DOWN, PS5_SRC_LEFT,
    PS5_SRC_RIGHT, PS5_SRC__COUNT
};
enum {
    XB_A, XB_B, XB_X, XB_Y, XB_BLACK, XB_WHITE, XB_BACK, XB_START, XB_LSTICK,
    XB_RSTICK, XB_UP, XB_DOWN, XB_LEFT, XB_RIGHT, XB_LT, XB_RT, XB__COUNT
};

void xemu_ps5_map_load(void);
int xemu_ps5_map_get(int xbox);
void xemu_ps5_map_set(int xbox, int source);    /* swaps with whoever had it */
void xemu_ps5_map_reset(void);
const char *xemu_ps5_source_name(int source);
int xemu_ps5_source_level(int source, uint32_t buttons, uint8_t l2, uint8_t r2);
int xemu_ps5_xbox_level(int xbox, uint32_t buttons, uint8_t l2, uint8_t r2);
void xemu_ps5_pad_seen(uint32_t buttons, uint8_t l2, uint8_t r2);
int xemu_ps5_pad_pressed_source(void);          /* -1: none */

#ifdef __cplusplus
}
#endif
