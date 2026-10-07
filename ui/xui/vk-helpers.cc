//
// xemu User Interface: Vulkan counterparts of gl-helpers.cc
//
// Builds without OpenGL (the PS5) draw the game frame through ImGui, and read
// it back for screenshots and snapshot thumbnails. The custom GL shaders
// (animated logo, controller and XMU diagrams, gamma and DAC palette on the
// frame) are not ported yet: the diagrams show their masks and the logo shows
// the icon.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "ui/xemu-widescreen.h"
#include "gl-helpers.hh"
#include "common.hh"
#ifdef __PROSPERO__
#include "dashboard.hh"
#endif
#include "data/controller_mask.png.h"
#include "data/xemu_64x64.png.h"
#include "data/xmu_mask.png.h"
#include "notifications.hh"
#include "vk-present.hh"
#include "stb_image.h"
#include <fpng.h>
#include <math.h>
#include <algorithm>

Fbo *controller_fbo, *xmu_fbo, *logo_fbo;
XemuTexture g_icon_tex;

// flip: bottom row first, as the GL UI keeps FBO images (drawn with V flipped)
static XemuTexture LoadTextureFromMemory(const unsigned char *buf,
                                         unsigned int size, bool flip)
{
    stbi_set_flip_vertically_on_load(flip);

    int width, height, channels = 0;
    unsigned char *data =
        stbi_load_from_memory(buf, size, &width, &height, &channels, 4);
    assert(data != NULL);

    XemuTexture tex = xemu_vk_texture_create(data, width, height, 4);
    stbi_image_free(data);
    return tex;
}

// Called by vk-present.cc once the Vulkan device exists.
void InitCustomRendering(void)
{
    controller_fbo = new Fbo(512, 512);
    controller_fbo->tex =
        LoadTextureFromMemory(controller_mask_data, controller_mask_size, true);

    xmu_fbo = new Fbo(512, 256);
    xmu_fbo->tex = LoadTextureFromMemory(xmu_mask_data, xmu_mask_size, true);

    logo_fbo = new Fbo(512, 512);
    logo_fbo->tex =
        LoadTextureFromMemory(xemu_64x64_data, xemu_64x64_size, true);

    g_icon_tex =
        LoadTextureFromMemory(xemu_64x64_data, xemu_64x64_size, false);
}

void RenderLogo(uint32_t time)
{
    (void)time;
}

void RenderController(float frame_x, float frame_y, uint32_t primary_color,
                      uint32_t secondary_color, ControllerState *state)
{
    (void)frame_x;
    (void)frame_y;
    (void)primary_color;
    (void)secondary_color;
    (void)state;
}

void RenderControllerPort(float frame_x, float frame_y, int i,
                          uint32_t port_color)
{
    (void)frame_x;
    (void)frame_y;
    (void)i;
    (void)port_color;
}

void RenderXmu(float frame_x, float frame_y, uint32_t primary_color,
               uint32_t secondary_color)
{
    (void)frame_x;
    (void)frame_y;
    (void)primary_color;
    (void)secondary_color;
}

// Scale <src> proportionally to fit in <max>
void ScaleDimensions(int src_width, int src_height, int max_width, int max_height, int *out_width, int *out_height)
{
    float w_ratio = (float)max_width/(float)max_height;
    float t_ratio = (float)src_width/(float)src_height;

    if (w_ratio >= t_ratio) {
        *out_width = (float)max_width * t_ratio/w_ratio;
        *out_height = max_height;
    } else {
        *out_width = max_width;
        *out_height = (float)max_height * w_ratio/t_ratio;
    }
}

static float GetDisplayAspectRatio(int width, int height)
{
    switch (g_config.display.ui.aspect_ratio) {
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE:
        return (float)width/(float)height;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9:
        return 16.0f/9.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3:
        return 4.0f/3.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO:
    default:
        return xemu_get_widescreen() ? 16.0f/9.0f : 4.0f/3.0f;
    }
}

// flip: the image is bottom row first (the NV2A display image, made for GL).
void RenderFramebuffer(XemuTexture tex, int width, int height, bool flip)
{
    int tw, th;
#ifdef __PROSPERO__
    // The side art stays where it was while the game shows nothing.
    static ImVec2 last0, last1;
    static bool have_last;
#endif
    if (!tex || nv2a_get_screen_off() ||
        !xemu_vk_texture_get_size(tex, &tw, &th)) {
#ifdef __PROSPERO__
        if (have_last) {
            DrawBladeSides(ImGui::GetBackgroundDrawList(), last0, last1);
        }
#endif
        return;
    }

    // Same fit modes as the GL UI's scale factors, as a destination rect.
    float scale[2];
    if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_STRETCH) {
        scale[0] = 1.0;
        scale[1] = 1.0;
    } else if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_CENTER) {
        float t_ratio = GetDisplayAspectRatio(tw, th);
        scale[0] = t_ratio*(float)th/(float)width;
        scale[1] = (float)th/(float)height;
    } else {
        float t_ratio = GetDisplayAspectRatio(tw, th);
        float w_ratio = (float)width/(float)height;
        if (w_ratio >= t_ratio) {
            scale[0] = t_ratio/w_ratio;
            scale[1] = 1.0;
        } else {
            scale[0] = 1.0;
            scale[1] = w_ratio/t_ratio;
        }
    }

    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 size(display.x * scale[0], display.y * scale[1]);
    ImVec2 p0((display.x - size.x) / 2, (display.y - size.y) / 2);
    ImGui::GetBackgroundDrawList()->AddImage(
        (ImTextureID)tex, p0, ImVec2(p0.x + size.x, p0.y + size.y),
        ImVec2(0, flip ? 1 : 0), ImVec2(1, flip ? 0 : 1));
#ifdef __PROSPERO__
    last0 = p0;
    last1 = ImVec2(p0.x + size.x, p0.y + size.y);
    have_last = true;
    DrawBladeSides(ImGui::GetBackgroundDrawList(), last0, last1);
#endif
}

bool RenderFramebufferToPng(XemuTexture tex, bool flip, std::vector<uint8_t> &png, int max_width, int max_height)
{
    std::vector<uint8_t> rgba;
    int src_w, src_h;
    if (!xemu_vk_read_texture(tex, rgba, &src_w, &src_h)) {
        return false;
    }
    if (flip) { // Bottom row first: put the top row first for the PNG
        size_t row = (size_t)src_w * 4;
        for (int y = 0; y < src_h / 2; y++) {
            std::swap_ranges(rgba.begin() + y * row,
                             rgba.begin() + (y + 1) * row,
                             rgba.begin() + (src_h - 1 - y) * row);
        }
    }

    // Same size as the GL UI: the display aspect ratio, within the maximum.
    int width = src_h * GetDisplayAspectRatio(src_w, src_h);
    int height = src_h;
    if (!max_width) max_width = width;
    if (!max_height) max_height = height;
    ScaleDimensions(width, height, max_width, max_height, &width, &height);

    // Bilinear resample to RGB8.
    std::vector<uint8_t> pixels(width * height * 3);
    for (int y = 0; y < height; y++) {
        float fy = fmaxf((y + 0.5f) * src_h / height - 0.5f, 0);
        int y0 = (int)fy, y1 = y0 + 1 < src_h ? y0 + 1 : y0;
        float ty = fy - y0;
        for (int x = 0; x < width; x++) {
            float fx = fmaxf((x + 0.5f) * src_w / width - 0.5f, 0);
            int x0 = (int)fx, x1 = x0 + 1 < src_w ? x0 + 1 : x0;
            float tx = fx - x0;
            for (int c = 0; c < 3; c++) {
                float a = rgba[(y0 * src_w + x0) * 4 + c];
                float b = rgba[(y0 * src_w + x1) * 4 + c];
                float d = rgba[(y1 * src_w + x0) * 4 + c];
                float e = rgba[(y1 * src_w + x1) * 4 + c];
                float top = a + (b - a) * tx, bottom = d + (e - d) * tx;
                pixels[(y * width + x) * 3 + c] =
                    (uint8_t)(top + (bottom - top) * ty + 0.5f);
            }
        }
    }

    return fpng::fpng_encode_image_to_memory(pixels.data(), width, height, 3, png);
}

void SaveScreenshot(XemuTexture tex, bool flip)
{
    Error *err = NULL;
    char fname[128];
    std::vector<uint8_t> png;

    if (RenderFramebufferToPng(tex, flip, png)) {
        time_t t = time(NULL);
        struct tm *tmp = localtime(&t);
        if (tmp) {
            strftime(fname, sizeof(fname), "xemu-%Y-%m-%d-%H-%M-%S.png", tmp);
        } else {
            strcpy(fname, "xemu.png");
        }

        const char *output_dir = g_config.general.screenshot_dir;
        if (!strlen(output_dir)) {
            output_dir = xemu_settings_get_base_path();
        }
        char *path = g_strdup_printf("%s/%s", output_dir, fname);
        FILE *fd = qemu_fopen(path, "wb");
        if (fd) {
            int s = fwrite(png.data(), png.size(), 1, fd);
            if (s != 1) {
                error_setg(&err, "Failed to write %s", path);
            }
            fclose(fd);
        } else {
            error_setg(&err, "Failed to open %s for writing", path);
        }
        g_free(path);
    } else {
        error_setg(&err, "Failed to encode PNG image");
    }

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_report_err(err);
    } else {
        char *msg = g_strdup_printf("Screenshot Saved: %s", fname);
        xemu_queue_notification(msg);
        free(msg);
    }
}
