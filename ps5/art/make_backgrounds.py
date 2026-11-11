#!/usr/bin/env python3
# XOG's backgrounds, drawn from nothing in the mood of the user's two references: a burst of
# green light with shards and sparks, and a column of green smoke on black. No text, no logo.
# usage: make_backgrounds.py OUT_DIR
import os, sys

import numpy as np
from PIL import Image, ImageFilter

out_dir = sys.argv[1]
W, H = 3840, 2160
rng = np.random.default_rng(7)


def noise(h, w, octaves, base, gain=0.55, stretch=(1.0, 1.0)):
    """Fractal noise: smooth random fields, finer and fainter at each octave."""
    out = np.zeros((h, w), np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        gw = max(2, int(base * stretch[0] * 2 ** o))
        gh = max(2, int(base * stretch[1] * 2 ** o * h / w))
        g = rng.random((gh + 1, gw + 1), dtype=np.float32)
        img = Image.fromarray(g, "F").resize((w, h), Image.BICUBIC)
        out += np.asarray(img, np.float32) * amp
        total += amp
        amp *= gain
    return out / total


def tone(field):
    """Black through deep green and lime to white: the references' one colour."""
    f = np.clip(field, 0, None)[..., None]
    deep = np.array([0.02, 0.30, 0.03], np.float32)
    lime = np.array([0.38, 0.92, 0.10], np.float32)
    hot = np.array([0.95, 1.0, 0.72], np.float32)
    rgb = deep * np.clip(f * 2.4, 0, 1)
    rgb = rgb + (lime - deep) * np.clip(f * 2.0 - 0.45, 0, 1)
    rgb = rgb + (hot - lime) * np.clip(f * 1.6 - 1.05, 0, 1)
    return rgb


def sparks(canvas, n, cx, cy, spread, seed):
    r = np.random.default_rng(seed)
    layer = np.zeros(canvas.shape[:2], np.float32)
    xs, ys = r.normal(cx, spread[0], n).astype(int), r.normal(cy, spread[1], n).astype(int)
    keep = (xs >= 0) & (xs < canvas.shape[1]) & (ys >= 0) & (ys < canvas.shape[0])
    layer[ys[keep], xs[keep]] = r.random(int(keep.sum()), dtype=np.float32) ** 3 * 255
    a = Image.fromarray(layer.astype(np.uint8))
    fine = np.asarray(a.filter(ImageFilter.GaussianBlur(1.6)), np.float32) / 255 * 5.0
    wide = np.asarray(a.filter(ImageFilter.GaussianBlur(6)), np.float32) / 255 * 9.0
    return canvas + (fine + wide)[..., None] * np.array([0.72, 1.0, 0.42], np.float32)


def save(rgb, name):
    # a little grain, or long gradients band on a TV
    rgb = rgb + (rng.random(rgb.shape[:2], dtype=np.float32)[..., None] - 0.5) / 255
    img = Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")
    img.save(os.path.join(out_dir, name + ".png"))
    img.resize((1280, 720), Image.LANCZOS).save(os.path.join(out_dir, name + "-preview.jpg"), quality=88)
    print("written", name)


yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)


def burst():
    """Light from low on the left, thrown across the frame in long rays and torn shards."""
    cx, cy = W * 0.09, H * 0.62
    dx, dy = xx - cx, yy - cy
    ang = np.arctan2(dy, dx)
    dist = np.sqrt(dx * dx + dy * dy) / W
    # rays: noise read along the angle only, so it streaks away from the source
    a01 = (ang + np.pi) / (2 * np.pi)
    table = noise(1, 4096, 6, 18, 0.62)[0]
    rays = table[(a01 * 4095).astype(int)]
    rays = np.clip((rays - 0.36) * 2.6, 0, 1) ** 1.3
    # the main beam, up and to the right, with a second going down
    beam = np.exp(-((ang + 0.42) / 0.34) ** 2) * 1.0 + np.exp(-((ang - 0.26) / 0.10) ** 2) * 0.6
    fall = 1 / (1 + (dist / 0.52) ** 2.2)
    core = np.exp(-(dist / 0.11) ** 1.1) * 1.9
    # torn light: stretched noise along the beam's direction
    c, s = np.cos(-0.33), np.sin(-0.33)
    u, v = (dx * c + dy * s) / W, (-dx * s + dy * c) / W
    torn = noise(H // 2, W // 2, 5, 7, 0.6, stretch=(0.45, 2.4))
    torn = np.asarray(Image.fromarray(torn, "F").resize((W, H), Image.BICUBIC), np.float32)
    shards = np.clip((torn - 0.50) * 5, 0, 1) ** 1.5
    field = fall * (0.10 + rays * (0.75 + beam * 1.4)) + core
    field = field + shards * 0.62 * fall * (0.30 + beam * 1.3)
    # the right and the far corners go to black, as in the reference
    field = field * (1 - np.clip((xx / W - 0.68) / 0.32, 0, 1) ** 2 * (1 - np.clip(beam * 1.2, 0, 1)) * 0.96)
    rgb = tone(field)
    rgb = sparks(rgb, 3400, W * 0.28, H * 0.42, (W * 0.17, H * 0.27), 11)
    rgb = sparks(rgb, 700, W * 0.62, H * 0.28, (W * 0.17, H * 0.14), 12)
    save(rgb, "background-burst")


def smoke():
    """A column of green smoke rising through the middle of a black frame."""
    n1 = noise(H, W, 7, 5, 0.58)
    n2 = noise(H, W, 7, 7, 0.58)
    # warped: the smoke folds on itself, as it does in the reference
    ix = np.clip(xx + (n1 - 0.5) * W * 0.22, 0, W - 1).astype(int)
    iy = np.clip(yy + (n2 - 0.5) * H * 0.34, 0, H - 1).astype(int)
    base = noise(H, W, 8, 4, 0.60)
    smoke_f = base[iy, ix]
    # veins: thin bright lines where the field crosses a level
    veins = np.clip(1 - np.abs(n2[iy, ix] - 0.5) * 22, 0, 1) ** 2
    u = xx / W
    centre = 0.5 + (yy / H - 0.5) * 0.16 + (n1 - 0.5) * 0.20
    column = np.exp(-((u - centre) / 0.20) ** 2)
    top = np.clip(1.25 - yy / H * 1.05, 0.28, 1.25)
    density = np.clip((smoke_f - 0.34) * 2.5, 0, 1) ** 1.3
    field = column * top * (density * 0.82 + veins * 0.40 * density)
    rgb = tone(field)
    rgb = sparks(rgb, 900, W * 0.5, H * 0.46, (W * 0.15, H * 0.26), 21)
    save(rgb, "background-smoke")


os.makedirs(out_dir, exist_ok=True)
burst()
smoke()
