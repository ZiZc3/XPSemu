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
enum { CONFIG_UPSCALE_SMOOTH, CONFIG_UPSCALE_SHARP };
enum { CONFIG_FPS_OFF, CONFIG_FPS_ON, CONFIG_FPS_FULL };
enum { CONFIG_SYS_AVPACK_HDTV, CONFIG_SYS_AVPACK_COMPOSITE, CONFIG_SYS_AVPACK_SVIDEO, CONFIG_SYS_AVPACK_SCART,
       CONFIG_SYS_AVPACK_VGA, CONFIG_SYS_AVPACK_RFU };

struct XogConfig {
    int scale = 4;              // 1920p; heavier and lighter games can have others
    int aspect = CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO;
    int fit = CONFIG_DISPLAY_UI_FIT_SCALE;
    int upscale = CONFIG_UPSCALE_SHARP;
    int aa = 0;     // 0 off, 1 FXAA, 2 SMAA, 3 SSAA
    int fps_cap = 0;    // 0 off, else frames a second
    int volume = 10;            // tens of percent
    int fps = CONFIG_FPS_ON;
    int avpack = CONFIG_SYS_AVPACK_HDTV;
    int language = 1;           // the Xbox's: 1 English
    bool pal60 = true;
    bool notifications = true;
    bool autopatch_widescreen = true;
    bool autopatch_60fps = true;
    bool sounds = true, music = true, covers = true, boot_logo = true;
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
