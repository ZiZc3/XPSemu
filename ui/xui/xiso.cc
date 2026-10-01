//
// XPSemu: what a game disc image says about itself
//
// The Xbox disc file system (XDVDFS) starts with a volume descriptor in
// sector 32 that points at the root directory, a binary tree of entries.
// default.xbe in it is the game; its certificate holds the title and title
// ID, and its $$XTIMAGE section the title image (an XPR0 texture, usually
// DXT1). Full disc dumps (redump) have a video partition first, and the game
// partition at an offset that depends on the disc generation.
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
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#include "xiso.hh"

static const int kSector = 2048;

static uint16_t Le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t Le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool ReadAt(FILE *f, off_t offset, void *buf, size_t size)
{
    return fseeko(f, offset, SEEK_SET) == 0 && fread(buf, 1, size, f) == size;
}

//
// XPR0 textures
//

static void Rgb565(uint16_t c, uint8_t out[4])
{
    out[0] = ((c >> 11) & 31) * 255 / 31;
    out[1] = ((c >> 5) & 63) * 255 / 63;
    out[2] = (c & 31) * 255 / 31;
    out[3] = 255;
}

// One 4x4 block of DXT colour (8 bytes) into rgba (stride in pixels).
static void DxtColorBlock(const uint8_t *b, uint8_t *rgba, int stride,
                          bool alpha_mode)
{
    uint16_t c0 = Le16(b), c1 = Le16(b + 2);
    uint8_t pal[4][4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    for (int i = 0; i < 3; i++) {
        if (c0 > c1 || !alpha_mode) {
            pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3;
            pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3;
        } else {
            pal[2][i] = (pal[0][i] + pal[1][i]) / 2;
            pal[3][i] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (c0 > c1 || !alpha_mode) ? 255 : 0;
    uint32_t idx = Le32(b + 4);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            memcpy(rgba + (y * stride + x) * 4, pal[idx & 3], 4);
            idx >>= 2;
        }
    }
}

static void DecodeDxt(const uint8_t *src, int w, int h, int format,
                      uint8_t *rgba)
{
    int block = format == 0x0C ? 8 : 16;
    for (int by = 0; by < h; by += 4) {
        for (int bx = 0; bx < w; bx += 4, src += block) {
            uint8_t tile[16 * 4];
            DxtColorBlock(src + block - 8, tile, 4, format == 0x0C);
            if (format == 0x0E) { // DXT3: explicit 4-bit alpha
                for (int i = 0; i < 16; i++) {
                    tile[i * 4 + 3] = ((src[i / 2] >> (i & 1) * 4) & 15) * 17;
                }
            } else if (format == 0x0F) { // DXT5: interpolated alpha
                uint8_t a[8] = { src[0], src[1] };
                for (int i = 2; i < 8; i++) {
                    a[i] = a[0] > a[1] ?
                               ((8 - i) * a[0] + (i - 1) * a[1]) / 7 :
                           i < 6 ? ((6 - i) * a[0] + (i - 1) * a[1]) / 5 :
                                   (i == 6 ? 0 : 255);
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) {
                    bits |= (uint64_t)src[2 + i] << (8 * i);
                }
                for (int i = 0; i < 16; i++) {
                    tile[i * 4 + 3] = a[(bits >> (3 * i)) & 7];
                }
            }
            for (int y = 0; y < 4 && by + y < h; y++) {
                for (int x = 0; x < 4 && bx + x < w; x++) {
                    memcpy(rgba + ((by + y) * w + bx + x) * 4,
                           tile + (y * 4 + x) * 4, 4);
                }
            }
        }
    }
}

// Offset of texel (x, y) in a swizzled (Morton order) power of two texture:
// bits of x and y interleaved, x first, while both have bits left.
static uint32_t Swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t offset = 0, bit = 0;
    for (uint32_t mx = 1, my = 1; mx < w || my < h;) {
        if (mx < w) {
            offset |= (x & mx) ? 1u << bit : 0;
            bit++;
            mx <<= 1;
        }
        if (my < h) {
            offset |= (y & my) ? 1u << bit : 0;
            bit++;
            my <<= 1;
        }
    }
    return offset;
}

bool XprDecode(const uint8_t *data, size_t size, std::vector<uint8_t> *rgba,
               int *width, int *height)
{
    if (size < 32 || memcmp(data, "XPR0", 4) != 0) {
        return false;
    }
    uint32_t header_size = Le32(data + 8);
    uint32_t tex_data = Le32(data + 16);
    uint32_t format = Le32(data + 24);
    uint32_t tex_size = Le32(data + 28);
    int color = (format >> 8) & 0xff;
    bool linear = color == 0x12 || color == 0x1E;
    int w, h;
    if (linear) {
        w = (tex_size & 0xfff) + 1;
        h = ((tex_size >> 12) & 0xfff) + 1;
    } else {
        w = 1 << ((format >> 20) & 15);
        h = 1 << ((format >> 24) & 15);
    }
    if (w > 1024 || h > 1024) {
        return false;
    }

    size_t need;
    switch (color) {
    case 0x0C: need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8; break;
    case 0x0E:
    case 0x0F: need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16; break;
    case 0x06:
    case 0x07:
    case 0x12:
    case 0x1E: need = (size_t)w * h * 4; break;
    default: return false;
    }
    size_t start = (size_t)header_size + tex_data;
    if (start > size || size - start < need) {
        return false;
    }
    const uint8_t *src = data + start;

    rgba->assign((size_t)w * h * 4, 0);
    if (color == 0x0C || color == 0x0E || color == 0x0F) {
        DecodeDxt(src, w, h, color, rgba->data());
    } else {
        bool opaque = color == 0x07 || color == 0x1E;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t i = linear ? (size_t)y * w + x : Swizzle(x, y, w, h);
                const uint8_t *p = src + i * 4; // B G R A
                uint8_t *o = rgba->data() + ((size_t)y * w + x) * 4;
                o[0] = p[2];
                o[1] = p[1];
                o[2] = p[0];
                o[3] = opaque ? 255 : p[3];
            }
        }
    }
    *width = w;
    *height = h;
    return true;
}

//
// XDVDFS and XBE
//

// Finds name in a directory (read whole into dir), in its binary tree.
static bool FindEntry(const std::vector<uint8_t> &dir, uint32_t offset,
                      const char *name, uint32_t *sector, uint32_t *size,
                      int depth = 0)
{
    if (depth > 64 || offset + 14 > dir.size()) {
        return false;
    }
    const uint8_t *e = dir.data() + offset;
    uint16_t left = Le16(e), right = Le16(e + 2);
    if (left == 0xffff) { // Padding / empty directory
        return false;
    }
    uint8_t len = e[13];
    if (offset + 14 + len <= dir.size() && len == strlen(name) &&
        strncasecmp((const char *)e + 14, name, len) == 0) {
        *sector = Le32(e + 4);
        *size = Le32(e + 8);
        return true;
    }
    return (left && FindEntry(dir, left * 4u, name, sector, size, depth + 1)) ||
           (right && FindEntry(dir, right * 4u, name, sector, size, depth + 1));
}

static std::string Utf16ToUtf8(const uint8_t *p, int max_chars)
{
    std::string out;
    for (int i = 0; i < max_chars; i++) {
        uint32_t c = Le16(p + i * 2);
        if (!c) {
            break;
        }
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < max_chars) {
            uint32_t lo = Le16(p + (i + 1) * 2);
            if (lo >= 0xdc00 && lo < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                i++;
            }
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xc0 | c >> 6);
            out += (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += (char)(0xe0 | c >> 12);
            out += (char)(0x80 | ((c >> 6) & 0x3f));
            out += (char)(0x80 | (c & 0x3f));
        } else {
            out += (char)(0xf0 | c >> 18);
            out += (char)(0x80 | ((c >> 12) & 0x3f));
            out += (char)(0x80 | ((c >> 6) & 0x3f));
            out += (char)(0x80 | (c & 0x3f));
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

static bool ReadXbe(FILE *f, off_t xbe, uint32_t xbe_size, XisoInfo *info)
{
    uint8_t head[0x178];
    if (xbe_size < sizeof(head) || !ReadAt(f, xbe, head, sizeof(head)) ||
        memcmp(head, "XBEH", 4) != 0) {
        return false;
    }
    uint32_t base = Le32(head + 0x104);
    uint32_t headers_size = Le32(head + 0x108);
    if (headers_size < sizeof(head) || headers_size > 1024 * 1024 ||
        headers_size > xbe_size) {
        return false;
    }
    std::vector<uint8_t> h(headers_size);
    if (!ReadAt(f, xbe, h.data(), h.size())) {
        return false;
    }
    // Addresses in the headers are virtual, from base.
    auto at = [&](uint32_t addr, uint32_t len) -> const uint8_t * {
        if (addr < base || addr - base > h.size() ||
            h.size() - (addr - base) < len) {
            return nullptr;
        }
        return h.data() + (addr - base);
    };

    const uint8_t *cert = at(Le32(h.data() + 0x118), 0x5c);
    if (cert) {
        info->title_id = Le32(cert + 0x8);
        info->title = Utf16ToUtf8(cert + 0xc, 40);
    }

    uint32_t count = Le32(h.data() + 0x11c);
    const uint8_t *sections = at(Le32(h.data() + 0x120), count * 56);
    for (uint32_t i = 0; sections && i < count && i < 256; i++) {
        const uint8_t *s = sections + i * 56;
        const uint8_t *name = at(Le32(s + 20), 10);
        if (!name || memcmp(name, "$$XTIMAGE", 10) != 0) {
            continue;
        }
        uint32_t raw = Le32(s + 12), raw_size = Le32(s + 16);
        if (raw_size == 0 || raw_size > 1024 * 1024 || raw > xbe_size ||
            xbe_size - raw < raw_size) {
            break;
        }
        std::vector<uint8_t> xpr(raw_size);
        if (ReadAt(f, xbe + raw, xpr.data(), xpr.size()) &&
            !XprDecode(xpr.data(), xpr.size(), &info->image,
                       &info->image_width, &info->image_height)) {
            info->image.clear();
        }
        break;
    }
    return cert != nullptr;
}

bool XisoReadInfo(const char *path, XisoInfo *info)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    // XISO, then redump XGD1, XGD2 and XGD3 game partitions.
    static const off_t partitions[] = { 0, 0x18300000, 0xFD90000, 0x2080000 };
    bool ok = false;
    for (off_t part : partitions) {
        uint8_t vd[28];
        if (!ReadAt(f, part + 32 * kSector, vd, sizeof(vd)) ||
            memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) != 0) {
            continue;
        }
        uint32_t root_sector = Le32(vd + 20), root_size = Le32(vd + 24);
        if (root_size == 0 || root_size > 16 * 1024 * 1024) {
            break;
        }
        std::vector<uint8_t> root(root_size);
        uint32_t sector, size;
        if (ReadAt(f, part + (off_t)root_sector * kSector, root.data(),
                   root.size()) &&
            FindEntry(root, 0, "default.xbe", &sector, &size)) {
            ok = ReadXbe(f, part + (off_t)sector * kSector, size, info);
            info->xbe_offset = part + (uint64_t)sector * kSector;
            info->xbe_size = size;
            info->full_disc = part != 0;
        }
        break;
    }
    fclose(f);
    return ok;
}
