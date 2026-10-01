/*
 * xemu UI texture handles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XEMU_TEXTURE_H
#define XEMU_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "config-host.h"

#ifdef CONFIG_OPENGL
#include <epoxy/gl.h>

/* An OpenGL texture name. */
typedef GLuint XemuTexture;

#else

/*
 * Builds without OpenGL (the PS5) draw the UI with Vulkan: a texture is the
 * ImTextureID of the Vulkan UI, 0 when there is none.
 */
typedef uint64_t XemuTexture;

#ifdef __cplusplus
extern "C" {
#endif

/* Pixels are RGBA8 (channels = 4) or RGB8 (channels = 3), top row first. */
XemuTexture xemu_vk_texture_create(const void *pixels, int width, int height,
                                   int channels);
void xemu_vk_texture_destroy(XemuTexture tex);
bool xemu_vk_texture_get_size(XemuTexture tex, int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif

#endif
