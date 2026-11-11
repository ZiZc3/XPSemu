// XOG: what the dashboard asks of the emulator, answered for this one.
// The dashboard runs alone, before a game: launching restarts the title into the game, so no line
// of it runs while one plays. GPL-3.0-or-later.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "padmap_ps5.h"

typedef uint64_t XemuTexture;

// ---- vk_ui.cc
XemuTexture xemu_vk_texture_create(const void *pixels, int width, int height, int channels);
void xemu_vk_texture_destroy(XemuTexture tex);
bool xemu_vk_texture_get_size(XemuTexture tex, int *width, int *height);
void xemu_vk_rebuild_fonts(void);
bool xog_vk_init(void);
void xog_vk_present(struct ImDrawData *draw_data);
const char *pgraph_vk_get_device_name(void);

// ---- settings: what the options of a game's start are made from
enum { CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO, CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE = CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO,
       CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3, CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9 };
enum { CONFIG_DISPLAY_UI_FIT_SCALE, CONFIG_DISPLAY_UI_FIT_STRETCH };
enum { CONFIG_UPSCALE_SMOOTH, CONFIG_UPSCALE_SHARP, CONFIG_UPSCALE_CAS };   // CAS: patch 247
enum { CONFIG_FPS_OFF, CONFIG_FPS_ON, CONFIG_FPS_FULL };
enum { CONFIG_SYS_AVPACK_HDTV, CONFIG_SYS_AVPACK_COMPOSITE, CONFIG_SYS_AVPACK_SVIDEO, CONFIG_SYS_AVPACK_SCART,
       CONFIG_SYS_AVPACK_VGA, CONFIG_SYS_AVPACK_RFU };

struct XogConfig {
    int scale = 4;              // 1920p; heavier and lighter games can have others
    int aspect = CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO;
    int fit = CONFIG_DISPLAY_UI_FIT_SCALE;
    int upscale = CONFIG_UPSCALE_SHARP;
    int aa = 4;     // 0 off, 1 FXAA, 2 SMAA, 3 SSAA, 4 SSAA + SMAA (the default, patch 313)
    int af = 16;    // anisotropic filtering (default 16x, patch 243): 0 off, else 2, 4, 8, 16 (patch 241)
    int cas = 4;        // CAS sharpness 0 to 4 (0.0 to 1.0): default 1.0 (patch 251, user)
    int texsharp = 0;   // patch 247: texture sharpness, 0 off, 1 low, 2 medium, 3 high (LOD bias -0.5 each)
    int crt = 0;        // patch 247: the CRT filter, 0 off, 1 scanlines, 2 CRT
    int ssao = 0;       // patch 367: ambient occlusion for every game, 0 off, 1 low, 2 high (Video)
    int fps_cap = 0;    // 0 off, else frames a second
    int volume = 10;            // tens of percent
    int fps = CONFIG_FPS_ON;
    int avpack = CONFIG_SYS_AVPACK_HDTV;
    int language = 0;           // the Xbox's (1 English...), 0 Auto: the PS5's when the disc has it (patch 178)
    bool pal60 = true;
    bool notifications = true;
    bool autopatch_widescreen = true;
    bool autopatch_60fps = true;
    bool sounds = true, music = true, covers = true, boot_logo = true;
    bool vibration = true;      // patch 234: the controller's rumble in games, for all of them
    bool triggers = true;       // patch 266 (303: On by default again, user): the DualSense's adaptive triggers in games
    bool fastforward = false;   // patch 253: swipe right on the touchpad in a game = x2 (Xbox games only)
    bool sides = false;
    int sides_style = 0;
};
extern XogConfig g_cfg;
void xemu_settings_save(void);
void xemu_settings_load(void);
const char *xemu_settings_get_base_path(void);

// ---- ui_main.cc
void xemu_queue_notification(const char *text);
void xog_start_game(const std::string &lines);  // writes the start's file, restarts into it
void xog_restart_dashboard(void);
void xog_ui_main(void);
void xog_ui_log(const char *fmt, ...);
