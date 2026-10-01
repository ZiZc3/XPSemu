//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
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
#pragma once
#include <vector>
#include "common.hh"
#include "../xemu-input.h"

#ifdef CONFIG_OPENGL
class Fbo
{
public:
    static GLint vp[4];
    static GLint original_fbo;
    static bool blend;

    int w, h;
    GLuint fbo, tex;

    Fbo(int width, int height);
    ~Fbo();
    inline GLuint Texture() { return tex; }
    void Target();
    void Restore();
};
#else
// The Vulkan UI (vk-helpers.cc) doesn't render these offscreen yet: an Fbo
// shows a fixed texture.
class Fbo
{
public:
    int w, h;
    XemuTexture tex;

    Fbo(int width, int height) : w(width), h(height), tex(0) {}
    inline XemuTexture Texture() { return tex; }
    void Target() {}
    void Restore() {}
};
#endif

extern Fbo *controller_fbo, *xmu_fbo, *logo_fbo;
extern XemuTexture g_icon_tex;

void InitCustomRendering(void);
void RenderLogo(uint32_t time);
void RenderController(float frame_x, float frame_y, uint32_t primary_color,
                      uint32_t secondary_color, ControllerState *state);
void RenderControllerPort(float frame_x, float frame_y, int i,
                          uint32_t port_color);
void RenderXmu(float frame_x, float frame_y, uint32_t primary_color,
               uint32_t secondary_color);
#ifdef CONFIG_OPENGL
void RenderFramebuffer(GLint tex, int width, int height, bool flip);
void RenderFramebuffer(GLint tex, int width, int height, bool flip, float scale[2]);
#else
// Queues the frame on ImGui's background draw list, below the UI.
void RenderFramebuffer(XemuTexture tex, int width, int height, bool flip);
#endif
bool RenderFramebufferToPng(XemuTexture tex, bool flip, std::vector<uint8_t> &png, int max_width = 0, int max_height = 0);
void SaveScreenshot(XemuTexture tex, bool flip);
void ScaleDimensions(int src_width, int src_height, int max_width, int max_height, int *out_width, int *out_height);
