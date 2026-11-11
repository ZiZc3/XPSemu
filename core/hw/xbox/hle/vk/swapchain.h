/*
 * hakuX HLE GPU — the window's presenter (vk/swapchain.c): the front frame
 * drawn into an Android window through a Vulkan swapchain.
 */
#ifndef HW_XBOX_HLE_VK_SWAPCHAIN_H
#define HW_XBOX_HLE_VK_SWAPCHAIN_H

#include <stdbool.h>
#include <stdint.h>

struct ANativeWindow;

/* A frame smaller than its place in the window: scaled up bilinearly, by
 * Snapdragon Game Super Resolution, or by AMD FidelityFX CAS (scaled and
 * sharpened in one pass; a larger one is always averaged down). */
enum { HLE_UPSCALE_BILINEAR, HLE_UPSCALE_SGSR, HLE_UPSCALE_CAS };

/* This refresh would put up what win already shows (the same front frame,
 * gamma ramp, window and swapchain, and key: the caller's placement and
 * filters, as at the last present that went through): the caller may skip
 * it, the panel keeping the image. A 30 fps title on a 120 Hz panel drew,
 * submitted and presented each frame four times. */
bool hle_present_unchanged(struct ANativeWindow *win, uint64_t key);

/* Each display refresh, on the window's thread, these three in order.
 * begin: an image of win's swapchain (made or remade as the window comes,
 * goes or changes size), waiting for one: the panel's pace. *w, *h: the
 * window's size. false: nothing to draw into (the HLE GPU not up yet, no
 * window): skip this refresh, no frame or end. */
bool hle_present_begin(struct ANativeWindow *win, uint32_t *w, uint32_t *h);
/* The FPS counter's number, drawn over the picture from the next present. */
void hle_present_set_fps(int fps);
/* frame: the front frame, held until a newer one is taken; its size, false
 * when there is none yet. */
bool hle_present_frame(uint32_t *w, uint32_t *h);
/* end: the frame drawn into the window's middle (sx, sy: its half size in
 * clip space, 1 the whole window), the rest black, through the display's
 * gamma ramp, and presented. fxaa: FXAA over each new frame first; average:
 * false samples a frame larger than its place bilinearly too (comparisons).
 * key: what hle_present_unchanged compares next. */
void hle_present_end(float sx, float sy, bool fxaa, int upscale, bool average, uint64_t key);
/* CAS's sharpness (patch 247): 0 to 4, 0.0 to 1.0 in quarters. */
void hle_present_set_cas(int level);
/* The CRT filter over the picture, last (patch 247): 0 off, 1 scanlines, 2 CRT
 * (scanlines, a slight curve, a vignette and a light shadow mask). */
void hle_present_set_crt(int mode);
/* SMAA 1x over each new frame (ahead of FXAA). */
void hle_present_set_smaa(bool on);
/* Frame generation: pictures between the game's frames, shown before each. */
void hle_present_set_framegen(bool on);
/* The most a game frame is made into, its own counted (2 to 4); 0: as many as fit. */
void hle_present_set_framegen_mode(unsigned most);
/* What's shown a second with the pictures in it, while they're made; -1: not now. */
int hle_present_framegen_shown(void);

#endif
