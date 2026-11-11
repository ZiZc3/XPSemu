/*
 * hakuX HLE GPU — back-end internals shared by the vk/ sources (CONFIG_VULKAN
 * only).
 */
#ifndef HW_XBOX_HLE_VK_BACK_INT_H
#define HW_XBOX_HLE_VK_BACK_INT_H

#include "hw/xbox/hle/vk/vkdev.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/front/cmd.h"

/* timeline.c */
int hle_tl_init(void (*on_complete)(uint64_t completed));
VkSemaphore hle_tl_sem(void);
void hle_tl_signaled(uint64_t value);
/* Wait for a timeline semaphore's value: the driver's host wait, or with
 * no_sem_wait a poll of the counter in userspace (quirks.c). */
VkResult hle_tl_sem_wait(VkSemaphore sem, uint64_t value, uint64_t timeout_ns);
/* Before each submit: the GPU left idle a moment when the app asked
 * (hle_tl_gap, back.h). */
void hle_tl_gap_serve(void);

/* Resolution scale (the surface_scale setting, 1-6, read at DEVICE_CREATE):
 * every render target and depth surface is rendered at d.w * scale by
 * d.h * scale. Guest-facing values stay in guest pixels (D3D viewports,
 * clear rects, readbacks, visibility counts) and are converted where the
 * image is used. */
extern uint32_t g_hle_rscale;

/* surf.c */
typedef struct HleVkSurf {
    bool alive;
    uint32_t id;
    HleSurfDesc d;
    uint32_t w, h;              /* image extent (views: their level's), scaled */
    uint32_t scale;             /* host pixels per guest pixel (hle_surf_define) */
    VkFormat fmt;
    VkImageAspectFlags aspect;
    uint32_t texel_bytes;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    uint64_t view_gen;          /* new value whenever its views are made: a remade view
                                   can reuse a destroyed one's handle, so the sampling
                                   slots (vk/draw.c surf_slot) compare this */
    VkImageLayout layout;
    uint64_t destroy_after;     /* 0 = live; HLE_SURF_PARKED = next submit */
    uint32_t rec_gen;           /* recording generation that last used it */
    /* Textures with several levels / faces (d.levels, d.faces) and their
     * level / face views (d.parent): one image in GENERAL layout. */
    bool general;
    /* a draw read it as an input attachment (vk/draw.c hle_draw_fb_stages):
     * passes on it render in RENDERING_LOCAL_READ from then on */
    bool local_read;
    uint32_t nlevels, nfaces;   /* owners: the image's levels and layers */
    uint64_t dirty;            /* owners: subresources rendered since the last read barrier */
    uint32_t rendered_levels;   /* owners: levels 0..n-1 sampled (the highest rendered + 1) */
    uint32_t sview_levels;
    VkImageView sview, cube_view;   /* owners: sampled views */
    VkImageView oview;          /* owners: the texels in another byte order
                                   (hle_surf_order_view: oview_ord) */
    uint32_t oview_ord, oview_levels;
    uint64_t oview_gen;         /* view_gen's, for oview */
    VkImage owner_img;          /* views: the owner image the view was made from */
    VkImageView zr_depth, zr_stencil;   /* depth buffers read as colour (zread.c) */
    uint64_t wgen;              /* new value whenever a pass or copy may write it */
    uint64_t zr_gen;            /* zread targets: the source's wgen converted */
    uint32_t zr_src;
    uint8_t zr_map[4];
    /* asynchronous pipeline builds (vk/draw.c target_note): the last swap a
     * draw targeted it and the one before (swap count + 1, 0: never) */
    uint64_t drawn_cur, drawn_prev;
    /* patch 325: MSAA (hle_vk_set_msaa). A multisampled copy of the image
     * the passes render into, resolved into img at each pass's end; img
     * stays what everything else samples, copies and reads back. ms_ok: the
     * copy holds what img does (no write to img since the last pass that
     * rendered the copy, hle_surf_transition), so a pass can load it. */
    uint32_t ms_samples;        /* the copy's samples, 0 = none made yet */
    bool ms_failed;             /* none can be made for it: its passes stay 1x */
    bool ms_ok;
    VkImage ms_img;
    VkDeviceMemory ms_mem;
    VkImageView ms_view;
    VkImageLayout ms_layout;
} HleVkSurf;

/* patch 325: MSAA. The samples asked for (1 off, 2, 4, 8; the "msaa=" start
 * line, before the device is made), and the open pass's (1 when it renders
 * the single-sample images): the draws' pipelines are made for it. */
extern uint32_t g_hle_msaa;
extern uint32_t g_hle_pass_samples;
/* s's multisampled copy for a pass: made on first use; 0 when s can't have
 * one (its format, size or kind, or the device), else its samples. */
uint32_t hle_surf_ms_prepare(HleVkSurf *s);
/* Before a pass renders s's copy: its layout, and the last pass's writes
 * before this one's loads. */
void hle_surf_ms_begin(VkCommandBuffer cb, HleVkSurf *s);
/* patch 326: a count the quick menu asked for (hle_vk_msaa_request), made
 * the one in use: render thread, no pass open (back.c open_pass). */
void hle_surf_ms_apply(void);

/* A pass or copy may write s: its wgen changes (zread.c converts again). */
void hle_surf_written(HleVkSurf *s);

/* A surface the open (unsubmitted) recording may reference is never
 * destroyed directly: it is parked and stamped with the value of the
 * submission that closes the recording (Vulkan: destroying a resource used
 * by a recording command buffer invalidates it). */
#define HLE_SURF_PARKED UINT64_MAX

HleVkSurf *hle_surf_get(uint32_t id);
HleVkSurf *hle_surf_define(const HleSurfDesc *d);
bool hle_surf_ensure(HleVkSurf *s);
void hle_surf_destroy_after(uint32_t id, uint64_t value);
void hle_surf_stamp_parked(uint64_t value);
void hle_surf_reap(uint64_t completed);
/* Caller guarantees no open recording (back.c flush_idle). */
void hle_surf_destroy_all(void);
/* Recording generation tracking (debug check in destroy_now). */
void hle_surf_rec_begin(void);
void hle_surf_rec_end(void);
void hle_img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                     VkImageLayout from, VkImageLayout to);
void hle_surf_transition(VkCommandBuffer cb, HleVkSurf *s, VkImageLayout to);
/* gamma.c: the swap's copy through the gamma ramp of src's pixels sx..sw x
 * sy..sh (sx, sy 0: its top left sw x sh); false = not done (blit) */
bool hle_gamma_pass(VkCommandBuffer cb, unsigned slot, const uint8_t ramp[3][256],
                    HleVkSurf *src, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                    VkImage dst, uint32_t dw, uint32_t dh);
/* gamma.c, patch 337: the video overlay drawn over display image dst (dw x
 * dh, SHADER_READ_ONLY, left so) where the frame's pixel is the colour key
 * (0x00RRGGBB): the frame src's pixels sx..sw x sy..sh fill dst (as the
 * swap's copy); the overlay (ovl: ow x oh, SHADER_READ_ONLY) its src rect
 * s[4] at d[4] (dst pixels). False = not done (the caller blits it). */
bool hle_ovlkey_pass(VkCommandBuffer cb, unsigned slot, HleVkSurf *src, uint32_t sx, uint32_t sy,
                     uint32_t sw, uint32_t sh, VkImageView ovl, uint32_t ow, uint32_t oh,
                     const int32_t s[4], const int32_t d[4], uint32_t key, VkImage dst,
                     uint32_t dw, uint32_t dh);
/* patch 355: the movie upscaler (gamma.c). A set sampling src (an identity
 * view, SHADER_READ_ONLY) for hle_movieup_pass, VK_NULL_HANDLE when there is
 * none (the movie is drawn as before); freed once the GPU is done with it.
 * The pass fills dst (dw x dh, R8G8B8A8, its whole contents new) with src
 * enlarged (Catmull-Rom, deringed), left in layout to. Outside a pass. */
VkDescriptorSet hle_movieup_set_new(VkImageView src);
void hle_movieup_set_free(VkDescriptorSet set);
void hle_movieup_pass(VkCommandBuffer cb, VkDescriptorSet set, VkImage dst, VkImageView dst_view,
                      uint32_t dw, uint32_t dh, VkImageLayout to);
/* draw.c: the "Movie upscaler" setting (movieup= start line), XOG_FIX nomovieup */
bool hle_movieup_on(void);
/* patch 363: ambient occlusion (gamma.c). The setting (ssao= start line, the
 * quick menu at once): 0 off, 1 low, 2 high; hle_ssao_level: 0 as well with
 * XOG_FIX nossao. The pass darkens display image dst (dw x dh,
 * SHADER_READ_ONLY, left so) by depth buffer ds (left DEPTH_STENCIL_READ_ONLY):
 * map takes a display pixel to ds's uv (x, y scale, x, y offset); gw x gh: the
 * picture's guest size (the radius's unit). Outside a pass. False = not done.
 * hle_ssao_regrow: its working images must grow for a ds of that size (made
 * again in the pass): the caller waits for the GPU first. */
extern volatile int g_hle_ssao;
int hle_ssao_level(void);
bool hle_ssao_regrow(uint32_t dw, uint32_t dh);
bool hle_ssao_pass(VkCommandBuffer cb, unsigned slot, int level, HleVkSurf *ds, uint32_t gw,
                   uint32_t gh, const float map[4], VkImage dst, uint32_t dw, uint32_t dh);
/* zread.c: depth-stencil surface src into colour surface dst as its 32-bit
 * words (HleCmdZRead), outside a pass; slot: the frame's command buffer.
 * NULL = done, else why not. */
/* HLE_OP_ZWRITE: colour surface src's words as depth surface dst's depth
 * (outside a render pass); NULL, or why it was skipped. */
const char *hle_zwrite_pass(VkCommandBuffer cb, unsigned slot, HleVkSurf *src, HleVkSurf *dst);
const char *hle_zread_pass(VkCommandBuffer cb, unsigned slot, HleVkSurf *src, HleVkSurf *dst,
                           const uint8_t map[4]);
/* Depth buffer s's depth-aspect view (VK_NULL_HANDLE: none). */
VkImageView hle_zread_depth_view(HleVkSurf *s);
bool hle_surf_read_pending(HleVkSurf *s, uint32_t rt_id);
/* Readback of a scaled surface: subresource (level, layer) of owner o, in
 * layout, blitted down to w x h (guest size) into a scratch image left in
 * TRANSFER_SRC_OPTIMAL. VK_NULL_HANDLE = the format cannot be blitted. */
VkImage hle_surf_down(VkCommandBuffer cb, HleVkSurf *o, VkImageLayout layout,
                      uint32_t level, uint32_t layer, uint32_t w, uint32_t h);
/* A w x h scratch image of fmt (transfer source and destination) for texels
 * on their way up; VK_NULL_HANDLE = none (the format cannot be blitted). */
VkImage hle_surf_scratch(VkFormat fmt, uint32_t w, uint32_t h);
void hle_surf_sample_views(HleVkSurf *s, VkImageView *view, VkImageView *cube);

/* An A8R8G8B8 / X8R8G8B8 target's texels as a 32-bit texture of another
 * byte order over its memory reads them (ord: HLE_DRAW_ORD_*): a view with
 * its channels reordered; VK_NULL_HANDLE when s is no such target. */
VkImageView hle_surf_order_view(HleVkSurf *s, uint32_t ord);
/* s's format has no alpha (X8R8G8B8, X1R5G5B5): sampled, its alpha is 1
 * whatever passes on it wrote there (its sampled views say so). */
bool hle_surf_x_alpha(const HleVkSurf *s);

/* An 8-bit target (NV2A B8, R8 here) stores the colour's blue channel, a
 * G8B8 one (R8G8) blue and green: the fragment shaders write .bgra and clears
 * swap red and blue. */
static inline bool hle_surf_out_bgra(VkFormat f)
{
    return f == VK_FORMAT_R8_UNORM || f == VK_FORMAT_R8G8_UNORM;
}

/* draw.c (P3) */
int hle_draw_init(void);
/* Before the render thread starts (hle_back_start): the title's recorded
 * pipelines built (the "Build cached shaders before starting" setting). */
void hle_draw_prebuild(void);
void hle_draw_cmd_state(const HleCmdState *s);
void hle_draw_cmd_state_part(const HleCmdStatePart *p);
void hle_draw_cmd_tex(const HleCmdTex *c, VkCommandBuffer cb);
void hle_draw_cmd_draw(const HleCmdDraw *c, VkCommandBuffer cb, uint32_t rt_id,
                       uint32_t ds_id, const HleCmdViewport *vp);
/* A recording starts (back.c cb_get, outside any pass). */
void hle_draw_rec_begin(VkCommandBuffer cb);
/* It is frame slot's (its last submission has completed): the per-slot push
 * blocks and fbfetch sets (caps.push / caps.push_desc off) start over. */
void hle_draw_slot(unsigned slot);
/* The command buffer's pipeline / dynamic state is no longer what the draws
 * recorded: a new command buffer, or another pipeline bound into it. */
void hle_draw_state_lost(void);
void hle_draw_ff_diag(void);
/* Retired textures: destroyed once the timeline reaches their last use;
 * parked ones get the value of the submit that closes the recording. */
void hle_draw_tex_reap(uint64_t completed);
void hle_draw_tex_stamp(uint64_t value);
bool hle_draw_tex_needs_prep(const HleCmdDraw *c, uint32_t rt_id);
/* Combiner stages of draw c that sample the colour target rt_id it renders
 * into (the NV2A reads the framebuffer): read as an input attachment. */
unsigned hle_draw_fb_stages(const HleCmdDraw *c, uint32_t rt_id);
/* debug (pixel history): the draw state's alpha test enable, func, ref, blend
 * enable, src << 16 | dst, z enable << 16 | z write, z func, colour write mask */
void hle_draw_rs_brief(uint32_t out[9]);
/* Combiner stages of draw c read the colour target rt_id at coordinates of
 * their own (not its pixel, which fbfetch reads): outside the pass,
 * hle_draw_snap copies it and the draw samples the copy; hle_draw_snap_end
 * after the draw. */
bool hle_draw_snap_needed(const HleCmdDraw *c, uint32_t rt_id);
void hle_draw_snap(VkCommandBuffer cb, HleVkSurf *rt);
void hle_draw_snap_end(void);
/* Draw c tests depth EQUAL without writing it: HLE_FB_ZEQ24 / ZEQ16
 * (shader/ff.h) by depth buffer ds_id's step, its fragments in front of the
 * stored depth discarded by reading it (set 2, binding 1); 0 = not. */
unsigned hle_draw_zeq(const HleCmdDraw *c, uint32_t ds_id);
/* The next draw runs without the driver's LRZ (vk/quirks.h HLE_QUIRK_LRZ_CLEAR). */
bool hle_draw_lrz_off(void);
/* What the next hle_draw_cmd_draw has bound of that (back.c): the bits, or 0. */
extern unsigned g_hle_zeq;
/* In the open pass: rt's view as input attachment 0 and depth view zv as the
 * depth one (set 2; either may be absent), after the earlier draws' writes
 * (the depth's: zbar). */
void hle_draw_fb_bind(VkCommandBuffer cb, HleVkSurf *rt, HleVkSurf *ds, VkImageView zv,
                      bool zbar);
extern bool g_hle_fbbar_off;            /* debug (debug.hakux.fbbar=0): no barrier */
/* debug (debug.hakux.gpucut): 1 (frag) draws scissored to one pixel, 2 (draw)
 * the state recorded and no draw, 3 (pass) neither: the passes alone, 4 (tri)
 * draws of 3 vertices scissored to one pixel */
extern int g_hle_gpucut;
/* The vertices drawn and pipelines bound so far (the pass profile, back.c). */
void hle_draw_counts(uint64_t *verts, uint64_t *pipes);
void hle_draw_forget_tex(uint32_t id);
void hle_draw_forget_surf(uint32_t id);
/* A debug capture is armed (pixel history): pipelines are built in
 * place, no draw of the captured frames skipped (vk/draw.c). */
bool hle_back_capture_armed(void);
/* debug probe (draw.c): an occlusion query of the visibility tests is open,
 * and the submissions so far */
bool hle_back_vis_query_open(void);
uint64_t hle_back_submits(void);
void hle_draw_psframe_poll(uint64_t swap);
void hle_draw_cmd_vsh(const HleCmdVsh *c);
void hle_draw_tex_prep(const HleCmdDraw *c, uint32_t rt_id, VkCommandBuffer cb);

/* display.c (here, not display.h, so that header has no Vulkan types) */
VkImage hle_disp_image(int slot);
bool hle_disp_fill(int slot);
/* The front frame for the window's presenter (vk/swapchain.c): its slot,
 * held until hle_disp_release(_fenced); the image in SHADER_READ_ONLY_OPTIMAL,
 * complete. Between taking it and submitting the work that reads it, the
 * caller is inside hle_disp_use_begin/end: a display resize (hle_disp_init)
 * waits for that before it destroys the images. */
typedef struct HleDispFrame {
    int slot;
    uint32_t gen, swap_id, w, h;
    uint64_t value;             /* the render timeline's (hle_tl_sem) when it was done */
    VkImage img;
    VkImageView view;
} HleDispFrame;
bool hle_disp_acquire(HleDispFrame *f);
/* The slots' generation (made again at a resize): inside use_begin/end. */
uint32_t hle_disp_gen(void);
void hle_disp_use_begin(void);
void hle_disp_use_end(void);

#endif
