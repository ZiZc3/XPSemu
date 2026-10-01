//
// XPSemu: what a game disc image says about itself
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
#include <stdint.h>
#include <string>
#include <vector>

struct XisoInfo {
    std::string title;          // From default.xbe's certificate, UTF-8
    uint32_t title_id = 0;      // E.g. 0x4D530004
    // A full disc dump (redump), not an XISO: xemu wants XISO to play it.
    bool full_disc = false;
    // Where default.xbe is in the image (bytes from its start), and its size.
    uint64_t xbe_offset = 0;
    uint32_t xbe_size = 0;
    // The game's title image ($$XTIMAGE), RGBA8, top row first; empty if
    // it has none (or in a format not read here).
    std::vector<uint8_t> image;
    int image_width = 0, image_height = 0;
};

// Reads default.xbe of an Xbox disc image: an XISO, or a full disc dump
// (redump) with the game partition further in. False if it isn't one.
bool XisoReadInfo(const char *path, XisoInfo *info);

// Decodes an XPR0 texture (as in $$XTIMAGE) to RGBA8. Used by XisoReadInfo.
bool XprDecode(const uint8_t *data, size_t size, std::vector<uint8_t> *rgba,
               int *width, int *height);
