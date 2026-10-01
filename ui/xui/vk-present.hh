//
// xemu User Interface: Vulkan presentation (builds without OpenGL)
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

#include <cstdint>
#include <vector>
#include "../xemu-texture.h"

struct ImDrawData;

// Reads a texture (or the NV2A display image) back as RGBA8, top row first.
// Only on the UI thread, which is the one that holds the display image.
bool xemu_vk_read_texture(XemuTexture tex, std::vector<uint8_t> &rgba,
                          int *width, int *height);

// Draws and presents one UI frame, then waits for it to finish.
void xemu_vk_present_frame(ImDrawData *draw_data);

// Re-uploads the font atlas (on the shared queue, so not from NewFrame).
// Before the device exists this does nothing: initialization uploads it.
void xemu_vk_rebuild_fonts(void);
