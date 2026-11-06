#!/usr/bin/env python3
# ReXemu's icon, drawn from nothing: the flared four-blade X of the user's reference,
# no words. usage: make_icon.py OUT_DIR
import os, sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

out_dir = sys.argv[1]
SIZE, SS = 1024, 4
S = SIZE * SS


def bez(p0, p1, p2, n=40):
    t = np.linspace(0, 1, n)[:, None]
    return (1 - t) ** 2 * np.array(p0) + 2 * (1 - t) * t * np.array(p1) + t ** 2 * np.array(p2)


def poly(*segments):
    pts = np.vstack(segments)
    return [(float(x) * SS, float(y) * SS) for x, y in pts]


CX, CY = 512, 512
GAP = 34        # the channel around the core


def blades():
    """Top chevron and two lower blades, curved as if cut by one stroke."""
    tl, tr = (150, 158), (874, 158)
    notch = (CX, 330)
    inner_l, inner_r = (392, 438), (632, 438)
    top = poly(
        bez(tl, (330, 232), notch),
        bez(notch, (694, 232), tr),
        bez(tr, (760, 300), inner_r),
        bez(inner_r, (CX, 438), inner_l),
        bez(inner_l, (264, 300), tl),
    )
    left = poly(
        bez((306, 566), (360, 566), (424, 566)),
        bez((424, 566), (300, 720), (84, 878)),
        bez((84, 878), (190, 720), (306, 566)),
    )
    right = [(2 * CX * SS - x, y) for x, y in left]
    return [top, left, right]


def mask_of(polys):
    img = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(img)
    for p in polys:
        d.polygon(p, fill=255)
    return img


def diamond(r, cy=CY + 0):
    return [(CX * SS, (cy - r) * SS), ((CX + r) * SS, cy * SS), (CX * SS, (cy + r) * SS), ((CX - r) * SS, cy * SS)]


yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32)
nx, ny = (xx - CX) / SIZE, (yy - CY) / SIZE
dist = np.sqrt(nx ** 2 + ny ** 2)


def lerp(a, b, t):
    return a[None, None, :] * (1 - t[..., None]) + b[None, None, :] * t[..., None]


# the ground: lit green at the middle, deep at the edges, a soft sheen from above
inner = np.array([0.44, 0.84, 0.13], np.float32)
outer = np.array([0.05, 0.36, 0.09], np.float32)
t = np.clip(dist / 0.72, 0, 1) ** 1.25
rgb = lerp(inner, outer, t)
sheen = np.clip(1 - (yy / SIZE) / 0.55, 0, 1) ** 2 * 0.10
rgb = rgb + sheen[..., None] * np.array([0.7, 1.0, 0.6], np.float32)


def down(img):
    return img.resize((SIZE, SIZE), Image.LANCZOS)


m_hi = mask_of(blades())
m = np.asarray(down(m_hi), np.float32) / 255

# the emblem's shadow on the ground
shadow = np.asarray(down(m_hi).filter(ImageFilter.GaussianBlur(20)), np.float32) / 255
shadow = np.roll(shadow, 14, axis=0)
rgb = rgb * (1 - 0.36 * shadow[..., None])

# light behind the emblem
glow = np.asarray(down(m_hi).filter(ImageFilter.GaussianBlur(44)), np.float32) / 255
rgb = rgb + glow[..., None] * np.array([0.10, 0.20, 0.05], np.float32)

# the blades: white at the top, faint lime at the feet, with a bevel from their own edges
vert = np.clip((yy - 150) / 730, 0, 1)
top_c, low_c = np.array([1.0, 1.0, 1.0], np.float32), np.array([0.90, 0.99, 0.78], np.float32)
blade = lerp(top_c, low_c, vert)
soft = np.asarray(down(m_hi).filter(ImageFilter.GaussianBlur(7)), np.float32) / 255
gy, gx = np.gradient(soft)
light = np.clip(-(gy * 0.80 + gx * 0.28) * 34, -1, 1)
# only a fine bright rim: no dark edge, the blades read as cut clean
blade = blade + np.clip(light, 0, 1)[..., None] * 0.05
rgb = rgb * (1 - m[..., None]) + np.clip(blade, 0, 1) * m[..., None]


# a quiet edge, as the console's tiles have
vign = np.clip((np.maximum(np.abs(nx), np.abs(ny)) - 0.40) / 0.10, 0, 1) ** 2 * 0.22
rgb = rgb * (1 - vign[..., None])

out = Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")
os.makedirs(out_dir, exist_ok=True)
out.save(os.path.join(out_dir, "icon-1024.png"))
out.resize((512, 512), Image.LANCZOS).convert("RGBA").save(os.path.join(out_dir, "icon0.png"))
print("written", out_dir)
