/*
 * XOG: the DualSense's buttons to the Xbox controller's, your own layout: /data/xpsemu/controller.txt.
 * Read by the dashboard's controller page and by the game. After XPSemu's. GPL-3.0-or-later.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "padmap_ps5.h"

#define MAP_FILE "/data/xpsemu/controller.txt"

static const uint32_t k_bit[PS5_SRC__COUNT] = {
    [PS5_SRC_CROSS] = 0x4000,    [PS5_SRC_CIRCLE] = 0x2000, [PS5_SRC_SQUARE] = 0x8000, [PS5_SRC_TRIANGLE] = 0x1000,
    [PS5_SRC_L1] = 0x400,        [PS5_SRC_R1] = 0x800,      [PS5_SRC_L3] = 0x2,        [PS5_SRC_R3] = 0x4,
    [PS5_SRC_OPTIONS] = 0x8,     [PS5_SRC_CREATE] = 0x1,    [PS5_SRC_UP] = 0x10,       [PS5_SRC_DOWN] = 0x40,
    [PS5_SRC_LEFT] = 0x80,       [PS5_SRC_RIGHT] = 0x20,
};
static const char *const k_source_key[PS5_SRC__COUNT] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
    "options", "create", "up", "down", "left", "right",
};
static const char *const k_source_name[PS5_SRC__COUNT] = {
    "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3",
    "Options", "Create", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
};
static const char *const k_xbox_key[XB__COUNT] = {
    "a", "b", "x", "y", "black", "white", "back", "start", "lstick", "rstick",
    "up", "down", "left", "right", "lt", "rt",
};
static const int8_t k_default[XB__COUNT] = {
    [XB_A] = PS5_SRC_CROSS,     [XB_B] = PS5_SRC_CIRCLE,   [XB_X] = PS5_SRC_SQUARE,    [XB_Y] = PS5_SRC_TRIANGLE,
    [XB_BLACK] = PS5_SRC_R1,    [XB_WHITE] = PS5_SRC_L1,   [XB_BACK] = PS5_SRC_CREATE, [XB_START] = PS5_SRC_OPTIONS,
    [XB_LSTICK] = PS5_SRC_L3,   [XB_RSTICK] = PS5_SRC_R3,  [XB_UP] = PS5_SRC_UP,       [XB_DOWN] = PS5_SRC_DOWN,
    [XB_LEFT] = PS5_SRC_LEFT,   [XB_RIGHT] = PS5_SRC_RIGHT, [XB_LT] = PS5_SRC_L2,      [XB_RT] = PS5_SRC_R2,
};
static int8_t g_map[XB__COUNT];
static bool g_loaded;
static uint32_t g_raw;
static uint8_t g_raw_l2, g_raw_r2;

void xemu_ps5_map_load(void)
{
    g_loaded = true;
    memcpy(g_map, k_default, sizeof(g_map));
    FILE *f = fopen(MAP_FILE, "r");
    if (!f) {
        return;
    }
    char xbox[32], src[32];
    while (fscanf(f, " %31[^=]=%31s", xbox, src) == 2) {
        for (int x = 0; x < XB__COUNT; x++) {
            for (int s = 0; s < PS5_SRC__COUNT; s++) {
                if (!strcmp(xbox, k_xbox_key[x]) && !strcmp(src, k_source_key[s])) {
                    g_map[x] = (int8_t)s;
                }
            }
        }
    }
    fclose(f);
}

static void map_save(void)
{
    FILE *f = fopen(MAP_FILE, "w");
    if (!f) {
        return;
    }
    for (int x = 0; x < XB__COUNT; x++) {
        fprintf(f, "%s=%s\n", k_xbox_key[x], k_source_key[g_map[x]]);
    }
    fclose(f);
}

int xemu_ps5_map_get(int xbox)
{
    if (!g_loaded) {
        xemu_ps5_map_load();
    }
    return xbox >= 0 && xbox < XB__COUNT ? g_map[xbox] : -1;
}

/* Gives xbox the source; whatever had it gets xbox's old one (a swap). */
void xemu_ps5_map_set(int xbox, int source)
{
    if (!g_loaded) {
        xemu_ps5_map_load();
    }
    if (xbox < 0 || xbox >= XB__COUNT || source < 0 || source >= PS5_SRC__COUNT) {
        return;
    }
    for (int x = 0; x < XB__COUNT; x++) {
        if (g_map[x] == source) {
            g_map[x] = g_map[xbox];
        }
    }
    g_map[xbox] = (int8_t)source;
    map_save();
}

void xemu_ps5_map_reset(void)
{
    memcpy(g_map, k_default, sizeof(g_map));
    g_loaded = true;
    map_save();
}

const char *xemu_ps5_source_name(int source)
{
    return source >= 0 && source < PS5_SRC__COUNT ? k_source_name[source] : "-";
}

/* How far a source is pressed, 0..255. */
int xemu_ps5_source_level(int source, uint32_t buttons, uint8_t l2, uint8_t r2)
{
    if (source == PS5_SRC_L2) {
        return l2;
    }
    if (source == PS5_SRC_R2) {
        return r2;
    }
    return source >= 0 && source < PS5_SRC__COUNT && (buttons & k_bit[source]) ? 255 : 0;
}

int xemu_ps5_xbox_level(int xbox, uint32_t buttons, uint8_t l2, uint8_t r2)
{
    return xemu_ps5_source_level(xemu_ps5_map_get(xbox), buttons, l2, r2);
}

void xemu_ps5_pad_seen(uint32_t buttons, uint8_t l2, uint8_t r2)
{
    g_raw = buttons;
    g_raw_l2 = l2;
    g_raw_r2 = r2;
}

/* The first source pressed now (for "press a button"), or -1. */
int xemu_ps5_pad_pressed_source(void)
{
    for (int s = 0; s < PS5_SRC__COUNT; s++) {
        if (xemu_ps5_source_level(s, g_raw, g_raw_l2, g_raw_r2) > 153) {
            return s;
        }
    }
    return -1;
}

/* patch 269: a game's own button mapping (see padmap_ps5.h). */
void xemu_ps5_padmap_global(XbPadMap *m)
{
    for (int x = 0; x < XB__COUNT; x++) {
        m->src[x] = (int8_t)xemu_ps5_map_get(x);
    }
    m->flags = 0;
}

int xemu_ps5_padmap_parse(const char *text, XbPadMap *m)
{
    XbPadMap t;
    if (!text) {
        return 0;
    }
    for (int x = 0; x < XB__COUNT; x++) {
        const int c = (unsigned char)text[x] - 'a';
        if (c < 0 || c >= PS5_SRC__COUNT) {
            return 0;   /* also a text too short */
        }
        t.src[x] = (int8_t)c;
    }
    t.flags = 0;
    const char *f = text + XB__COUNT;
    if (*f == ':') {
        f++;
        while (*f >= '0' && *f <= '9' && t.flags < 1000) {
            t.flags = t.flags * 10 + (*f++ - '0');
        }
        t.flags &= PADMAP_SWAP_STICKS | PADMAP_INVERT_RY | PADMAP_INVERT_LY;
    }
    if (*f && *f != '\r' && *f != '\n' && *f != ' ') {
        return 0;
    }
    *m = t;
    return 1;
}

void xemu_ps5_padmap_format(const XbPadMap *m, char *out, unsigned n)
{
    char t[XB__COUNT + 1];
    for (int x = 0; x < XB__COUNT; x++) {
        const int s = m->src[x];
        t[x] = (char)('a' + (s >= 0 && s < PS5_SRC__COUNT ? s : k_default[x]));
    }
    t[XB__COUNT] = 0;
    snprintf(out, n, "%s:%d", t, m->flags);
}

int xemu_ps5_padmap_same(const XbPadMap *a, const XbPadMap *b)
{
    return a->flags == b->flags && !memcmp(a->src, b->src, sizeof(a->src));
}

const char *xemu_ps5_xbox_name(int xbox)
{
    static const char *const names[XB__COUNT] = {
        "A", "B", "X", "Y", "Black", "White", "Back", "Start", "Left stick click",
        "Right stick click", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
        "Left trigger", "Right trigger",
    };
    return xbox >= 0 && xbox < XB__COUNT ? names[xbox] : "";
}

void xemu_ps5_padmap_describe(const XbPadMap *m, char *out, unsigned n)
{
    size_t at = 0;
    out[0] = 0;
    for (int x = 0; x < XB__COUNT && at < n; x++) {
        at += (size_t)snprintf(out + at, n - at, "%s=%s ", k_xbox_key[x], xemu_ps5_source_name(m->src[x]));
    }
    if (at < n) {
        snprintf(out + at, n - at, "| swap sticks %s, invert right stick %s, invert left stick %s",
                 m->flags & PADMAP_SWAP_STICKS ? "on" : "off", m->flags & PADMAP_INVERT_RY ? "on" : "off",
                 m->flags & PADMAP_INVERT_LY ? "on" : "off");
    }
}
