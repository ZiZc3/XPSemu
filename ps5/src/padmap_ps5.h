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

/* patch 269: a game's own button mapping (Xbox games only, never Chihiro). Kept in the game's settings file as
 * "button_mapping=" and handed to the game's run as the start line "padmap=", the same text: 16 letters, one per
 * Xbox input in XB_* order ('a' + PS5_SRC_*), then ':' and the flags (a number). */
enum { PADMAP_SWAP_STICKS = 1, PADMAP_INVERT_RY = 2, PADMAP_INVERT_LY = 4 };
typedef struct {
    int8_t src[XB__COUNT];
    int flags;
} XbPadMap;
void xemu_ps5_padmap_global(XbPadMap *m);                   /* your layout (Settings > Controller), no flags */
int xemu_ps5_padmap_parse(const char *text, XbPadMap *m);   /* 1: good */
void xemu_ps5_padmap_format(const XbPadMap *m, char *out, unsigned n);
int xemu_ps5_padmap_same(const XbPadMap *a, const XbPadMap *b);
const char *xemu_ps5_xbox_name(int xbox);
/* "a=Cross b=Circle ... | swap sticks off, invert right stick off, invert left stick off": for the log */
void xemu_ps5_padmap_describe(const XbPadMap *m, char *out, unsigned n);

#ifdef __cplusplus
}
#endif
