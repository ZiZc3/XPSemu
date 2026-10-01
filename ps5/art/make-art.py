#!/usr/bin/env python3
# XPSemu: draws the app's launcher art (an original green "X" emblem, not
# the Xbox trademark) as source PNGs for PS5_Vulkan's tools/prepare-assets.sh:
#   icon-source.png        1024x1024 -> sce_sys/icon0.png
#   background-source.png  3840x2160 -> sce_sys/pic0.dds, pic1.dds (no
#                          emblem: just the green glow, grid and light)
# The PS5 draws the app's name and Play button over the left of the
# background, so the emblem sits on the right.
#
# Usage: make-art.py OUTPUT_DIR
import math
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

out = sys.argv[1] if len(sys.argv) > 1 else '.'


def lerp(a, b, t):
    return a + (b - a) * t


def radial(w, h, cx, cy, radius, inner, outer):
    """RGB image fading from inner (at cx, cy) to outer (radius away)."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / radius
    t = np.clip(d, 0, 1)[..., None] ** 1.3
    img = np.array(inner, np.float32) * (1 - t) + np.array(outer, np.float32) * t
    return img


def x_mask(size, scale=4):
    """The emblem's shape: two strokes crossing, slim where they meet and
    flaring towards their ends, the sides gently curved, ends cut square."""
    s = size * scale
    img = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(img)
    c = s / 2
    length = 0.44 * s
    for base in (math.pi / 4, -math.pi / 4):
        ux, uy = math.cos(base), math.sin(base)
        nx, ny = -uy, ux
        left, right = [], []
        steps = 60
        for i in range(steps + 1):
            t = -1 + 2 * i / steps
            hw = lerp(0.058, 0.128, abs(t) ** 1.8) * s
            px, py = c + ux * t * length, c + uy * t * length
            left.append((px + nx * hw, py + ny * hw))
            right.append((px - nx * hw, py - ny * hw))
        d.polygon(left + right[::-1], fill=255)
    return img.resize((size, size), Image.LANCZOS)


def emblem(size):
    """The X in green: bright at the top left to deep green at the bottom
    right, a light rim along its edges, and a glow around it (RGBA)."""
    mask = x_mask(size)
    m = np.asarray(mask, np.float32) / 255

    y, x = np.mgrid[0:size, 0:size].astype(np.float32) / size
    t = np.clip((x + y) / 2, 0, 1)[..., None]
    top = np.array([190, 250, 90], np.float32)
    bottom = np.array([20, 110, 18], np.float32)
    body = top * (1 - t) + bottom * t
    # A soft sheen across the upper arms.
    sheen = np.clip(1 - np.abs((x - y) * 3), 0, 1)[..., None] * \
        np.clip(1.2 - (x + y), 0, 1)[..., None]
    body = body + sheen * 40

    # Rim light: where the mask falls off after a small blur.
    inner = np.asarray(mask.filter(ImageFilter.GaussianBlur(size / 90)),
                       np.float32) / 255
    rim = np.clip((m - inner) * 3, 0, 1)[..., None]
    body = body * (1 - rim) + np.array([235, 255, 170], np.float32) * rim

    rgba = np.zeros((size, size, 4), np.float32)
    rgba[..., :3] = np.clip(body, 0, 255)
    rgba[..., 3] = m * 255
    x_img = Image.fromarray(rgba.astype(np.uint8), 'RGBA')

    glow = mask.filter(ImageFilter.GaussianBlur(size / 18))
    g = np.asarray(glow, np.float32) / 255
    glow_rgba = np.zeros((size, size, 4), np.float32)
    glow_rgba[..., :3] = [90, 220, 40]
    glow_rgba[..., 3] = np.clip(g * 1.1, 0, 1) * 200
    out_img = Image.fromarray(glow_rgba.astype(np.uint8), 'RGBA')
    out_img.alpha_composite(x_img)
    return out_img


def ring(size, radius, width, color, blur):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = size / 2
    d.ellipse((c - radius, c - radius, c + radius, c + radius),
              outline=color, width=width)
    return img.filter(ImageFilter.GaussianBlur(blur)) if blur else img


def make_icon():
    size = 1024
    bg = radial(size, size, size / 2, size / 2, size * 0.72,
                (14, 48, 12), (0, 0, 0))
    icon = Image.fromarray(bg.astype(np.uint8), 'RGB').convert('RGBA')
    icon.alpha_composite(ring(size, size * 0.43, 10, (80, 200, 40, 90), 14))
    e = emblem(int(size * 0.8))
    icon.alpha_composite(e, ((size - e.width) // 2, (size - e.height) // 2))
    icon.convert('RGB').save(f'{out}/icon-source.png')


def make_background():
    w, h = 3840, 2160
    ex, ey = int(w * 0.7), int(h * 0.5)
    bg = radial(w, h, ex, ey, w * 0.62, (16, 58, 14), (1, 6, 1))
    img = Image.fromarray(bg.astype(np.uint8), 'RGB').convert('RGBA')

    # A fine grid bowed like the inside of a sphere, as in the dashboard.
    grid = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(grid)
    step = 160
    for gx in range(-step, w + step, step):
        bow = (gx - w / 2) / w * 120
        pts = [(gx + bow * (1 - 2 * abs(j / 24 - 0.5)) * 2 - bow, h * j / 24)
               for j in range(25)]
        d.line(pts, fill=(60, 150, 30, 55), width=3)
    for gy in range(-step, h + step, step):
        bow = (gy - h / 2) / h * 120
        pts = [(w * j / 32, gy + bow * (1 - 2 * abs(j / 32 - 0.5)) * 2 - bow)
               for j in range(33)]
        d.line(pts, fill=(60, 150, 30, 55), width=3)
    # The grid fades away from the emblem.
    fade = radial(w, h, ex, ey, w * 0.75, (255, 255, 255), (40, 40, 40))[..., 0]
    ga = np.asarray(grid, np.float32)
    ga[..., 3] *= fade / 255
    img.alpha_composite(Image.fromarray(ga.astype(np.uint8), 'RGBA'))

    # Soft beams of light from behind the emblem.
    beams = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(beams)
    for ang in (-60, -25, 10, 40, 75, 120, 160, 200):
        a = math.radians(ang)
        spread = math.radians(5)
        far = 3000
        d.polygon([(ex, ey),
                   (ex + math.cos(a - spread) * far, ey + math.sin(a - spread) * far),
                   (ex + math.cos(a + spread) * far, ey + math.sin(a + spread) * far)],
                  fill=(120, 230, 60, 18))
    img.alpha_composite(beams.filter(ImageFilter.GaussianBlur(40)))

    # Motes of light.
    random.seed(7)
    motes = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(motes)
    for _ in range(260):
        mx, my = random.uniform(0, w), random.uniform(0, h)
        r = random.uniform(2, 6)
        d.ellipse((mx - r, my - r, mx + r, my + r),
                  fill=(190, 250, 110, random.randint(40, 150)))
    img.alpha_composite(motes.filter(ImageFilter.GaussianBlur(1.5)))

    # Darker on the left, where the PS5 puts the name and Play button.
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    shade = np.clip(1 - x / (w * 0.55), 0, 1) ** 1.5 * 0.55
    a = np.asarray(img, np.float32)
    a[..., :3] *= (1 - shade)[..., None]
    Image.fromarray(a.astype(np.uint8), 'RGBA').convert('RGB').save(
        f'{out}/background-source.png')


make_icon()
make_background()
print('art written to', out)
