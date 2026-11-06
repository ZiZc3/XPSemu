//
// XPSemu: a small BC7 encoder (mode 6 only: one colour line per 4x4 block,
// 16 steps along it), quick enough to make a 3840x2160 picture on the PS5:
// the home screen's backgrounds (sce_sys/pic0.dds, pic1.dds) are BC7.
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
#include <math.h>
#include <stdint.h>
#include <string.h>

// One opaque 4x4 block (RGB, rows top to bottom) to its 16 bytes.
static inline void Bc7EncodeBlock(const uint8_t px[16][3], uint8_t out[16])
{
    static const int kWeight[16] = { 0,  4,  9,  13, 17, 21, 26, 30,
                                     34, 38, 43, 47, 51, 55, 60, 64 };
    // The line: through the mean, along the colours' main direction.
    float mean[3] = { 0, 0, 0 };
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 3; c++) {
            mean[c] += px[i][c];
        }
    }
    for (int c = 0; c < 3; c++) {
        mean[c] /= 16;
    }
    float cov[3][3] = {};
    for (int i = 0; i < 16; i++) {
        float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                cov[a][b] += d[a] * d[b];
            }
        }
    }
    float axis[3] = { 0.577f, 0.577f, 0.577f };
    for (int it = 0; it < 6; it++) {
        float v[3];
        for (int a = 0; a < 3; a++) {
            v[a] = cov[a][0] * axis[0] + cov[a][1] * axis[1] + cov[a][2] * axis[2];
        }
        float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len < 1e-4f) {
            break; // One colour: any direction
        }
        for (int a = 0; a < 3; a++) {
            axis[a] = v[a] / len;
        }
    }
    float tmin = 1e9f, tmax = -1e9f;
    for (int i = 0; i < 16; i++) {
        float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] +
                  (px[i][2] - mean[2]) * axis[2];
        tmin = t < tmin ? t : tmin;
        tmax = t > tmax ? t : tmax;
    }

    // The ends, in 7 bits and a shared low bit each (the better of 0, 1).
    int q[2][3], pbit[2], e[2][3];
    for (int k = 0; k < 2; k++) {
        float t = k ? tmax : tmin;
        int best_err = 1 << 30;
        for (int p = 0; p < 2; p++) {
            int qq[3], err = 0;
            for (int c = 0; c < 3; c++) {
                float v = mean[c] + axis[c] * t;
                v = v < 0 ? 0 : v > 255 ? 255 : v;
                int q7 = (int)lrintf((v - p) / 2);
                q7 = q7 < 0 ? 0 : q7 > 127 ? 127 : q7;
                qq[c] = q7;
                int d = ((q7 << 1) | p) - (int)lrintf(v);
                err += d * d;
            }
            if (err < best_err) {
                best_err = err;
                pbit[k] = p;
                memcpy(q[k], qq, sizeof(qq));
            }
        }
        for (int c = 0; c < 3; c++) {
            e[k][c] = (q[k][c] << 1) | pbit[k];
        }
    }

    // Each pixel: the nearest of the 16 steps.
    int pal[16][3];
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 3; c++) {
            pal[i][c] = ((64 - kWeight[i]) * e[0][c] + kWeight[i] * e[1][c] + 32) >> 6;
        }
    }
    int idx[16];
    for (int i = 0; i < 16; i++) {
        int best = 0, best_err = 1 << 30;
        for (int s = 0; s < 16; s++) {
            int dr = px[i][0] - pal[s][0], dg = px[i][1] - pal[s][1],
                db = px[i][2] - pal[s][2];
            int err = dr * dr + dg * dg + db * db;
            if (err < best_err) {
                best_err = err;
                best = s;
            }
        }
        idx[i] = best;
    }
    // The first pixel's step has no top bit: under 8, else the ends swap.
    if (idx[0] >= 8) {
        for (int c = 0; c < 3; c++) {
            int t = q[0][c];
            q[0][c] = q[1][c];
            q[1][c] = t;
        }
        int t = pbit[0];
        pbit[0] = pbit[1];
        pbit[1] = t;
        for (int i = 0; i < 16; i++) {
            idx[i] = 15 - idx[i];
        }
    }

    // Mode 6: 7 bits of mode, R0 R1 G0 G1 B0 B1 A0 A1 (7 bits), P0 P1,
    // then the steps (3 bits for the first, 4 for the rest); low bits first.
    uint64_t bits[2] = { 0, 0 };
    int pos = 0;
    auto put = [&](uint32_t v, int n) {
        for (int b = 0; b < n; b++, pos++) {
            if ((v >> b) & 1) {
                bits[pos >> 6] |= 1ull << (pos & 63);
            }
        }
    };
    put(1 << 6, 7);
    for (int c = 0; c < 3; c++) {
        put(q[0][c], 7);
        put(q[1][c], 7);
    }
    put(127, 7); // Opaque
    put(127, 7);
    put(pbit[0], 1);
    put(pbit[1], 1);
    put(idx[0], 3);
    for (int i = 1; i < 16; i++) {
        put(idx[i], 4);
    }
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(bits[0] >> (8 * i));
        out[8 + i] = (uint8_t)(bits[1] >> (8 * i));
    }
}
