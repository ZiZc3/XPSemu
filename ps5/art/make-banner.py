#!/usr/bin/env python3
# XPSemu: the README banner (ps5/art/readme-banner.png) from the app icon:
# the green X emblem on the left, "XPSemu" and a tagline on the right, on the
# dashboard's dark green glow.
#
# Usage: make-banner.py ICON_PNG OUTPUT_PNG
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

icon_path, out = sys.argv[1], sys.argv[2]
W, H = 1600, 480


def font(size, bold=True):
    for path, var in (
        ('/usr/share/fonts/truetype/ubuntu/Ubuntu[wdth,wght].ttf', b'Bold'),
        ('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', None),
    ):
        try:
            f = ImageFont.truetype(path, size)
            if var and bold:
                f.set_variation_by_name(var)
            elif var:
                f.set_variation_by_name(b'Regular')
            return f
        except OSError:
            continue
    return ImageFont.load_default()


# Background: near-black green with a soft glow behind the emblem.
y, x = np.mgrid[0:H, 0:W].astype(np.float32)
d = np.sqrt(((x - 300) / 700) ** 2 + ((y - H / 2) / 420) ** 2)
t = np.clip(d, 0, 1)[..., None] ** 1.2
inner = np.array([18, 58, 14], np.float32)
outer = np.array([4, 10, 4], np.float32)
bg = inner * (1 - t) + outer * t

# Faint perspective grid at the bottom, like the dashboard.
img = Image.fromarray(bg.astype(np.uint8), 'RGB')
grid = Image.new('L', (W, H), 0)
g = ImageDraw.Draw(grid)
horizon = H * 0.62
for i in range(-20, 21):
    g.line([(W / 2 + i * 18, horizon), (W / 2 + i * 260, H)], fill=40, width=1)
for k in range(1, 9):
    yy = horizon + (H - horizon) * (k / 8) ** 1.8
    g.line([(0, yy), (W, yy)], fill=34, width=1)
grid = grid.filter(ImageFilter.GaussianBlur(0.6))
lime = Image.new('RGB', (W, H), (120, 230, 60))
img = Image.composite(lime, img, grid)

# The emblem: "lighten" blend, so the icon's own dark background disappears.
icon = Image.open(icon_path).convert('RGB').resize((440, 440), Image.LANCZOS)
layer = Image.new('RGB', (W, H), (0, 0, 0))
layer.paste(icon, (80, (H - 440) // 2))
img = ImageChops.lighter(img, layer)

# Title with a glow.
title_font = font(190)
sub_font = font(40, bold=False)
tx, ty = 560, 95
glow = Image.new('L', (W, H), 0)
ImageDraw.Draw(glow).text((tx, ty), 'XPSemu', font=title_font, fill=255)
glow = glow.filter(ImageFilter.GaussianBlur(18))
img = Image.composite(Image.new('RGB', (W, H), (90, 200, 40)), img,
                      glow.point(lambda v: int(v * 0.55)))
dr = ImageDraw.Draw(img)
dr.text((tx, ty), 'XPSemu', font=title_font, fill=(196, 255, 140))
dr.text((tx + 8, ty + 235), 'Original Xbox emulator for the PS5',
        font=sub_font, fill=(214, 236, 200))
dr.text((tx + 8, ty + 290), 'native app  ·  based on xemu  ·  Alpha 1',
        font=font(30, bold=False), fill=(130, 175, 115))
img.save(out, optimize=True)
print(out, img.size)
