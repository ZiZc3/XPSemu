#!/usr/bin/env python3
# XPSemu: side art for 4:3 games, after the Xbox 360's "Blades" dashboard
# (original artwork in its style and colours: no Microsoft art). Draws the
# left and right panels (480x2160, a 4K screen's bars) and builds them into
# the app as ui/xui/blades-art.h; with an output folder, saves the PNGs too.
#
# Usage: ps5/art/make-blades.py [PNG_DIR]   (from the xemu tree)
import io
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H, SS = 480, 2160, 2          # Size, supersampling
w, h = W * SS, H * SS
y, x = np.mgrid[0:h, 0:w].astype(np.float32)
u, v = x / w, y / h              # 0..1 across (outer edge = 0), down


def mix(a, b, t):
    t = np.clip(t, 0, 1)[..., None]
    return np.array(a, np.float32) * (1 - t) + np.array(b, np.float32) * t


def band(d, edge):
    """1 inside (d < 0), 0 outside, anti-aliased over `edge` pixels."""
    return np.clip(0.5 - d / edge, 0, 1)


def orange_panel(glow_u):
    """Blades' glossy orange: bright where the light falls, deeper at the
    top, bottom and far side, a sheen across the top."""
    light = np.exp(-((v - 0.5) / 0.42) ** 2) * np.exp(-((u - glow_u) / 0.9) ** 2)
    img = mix((168, 92, 36), (238, 148, 68), light)
    top = np.clip(1 - v / 0.09, 0, 1) ** 2
    img = img * (1 - top[..., None] * 0.35) + np.array([60, 30, 10]) * top[..., None] * 0.35
    sheen = np.exp(-((v - 0.16) / 0.07) ** 2) * 0.22
    img = img + (np.array([255, 225, 190]) - img) * sheen[..., None]
    bottom = np.clip((v - 0.9) / 0.1, 0, 1) ** 1.5
    img = img * (1 - bottom[..., None] * 0.3)
    return img


def finish(img):
    rgb = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')
    return rgb.resize((W, H), Image.LANCZOS)


def left_panel():
    # Edges, as curves down the panel (u at each v): the cream frame bows
    # out towards the screen's edge in the middle, as the Blades did.
    bow = np.sin(np.pi * v)
    frame_l = 0.27 - 0.12 * bow
    frame_r = frame_l + 0.11
    tab_r = frame_r + 0.17
    img = orange_panel(0.75)
    # A soft dark shadow on the orange along the tab, then its rim.
    shadow = np.exp(-np.clip(u - tab_r, 0, None) / 0.06) * (u > tab_r)
    img = img * (1 - 0.45 * shadow[..., None])
    # The orange's dark brown rim along the tab.
    rim = band(np.abs(u - tab_r) - 0.006, 2 * SS / w)
    img = img * (1 - rim[..., None]) + np.array([110, 56, 14]) * rim[..., None]
    # The tab: a deeper orange-brown strip.
    tab = band(u - tab_r, 2 * SS / w)
    tab_col = mix((190, 128, 80), (150, 92, 46), v)
    tab_col = tab_col * (0.85 + 0.15 * np.clip((u - frame_r) / 0.15, 0, 1))[..., None]
    img = img * (1 - tab[..., None]) + tab_col * tab[..., None]
    # The cream frame: glossy, brightest along its middle.
    fr = band(u - frame_r, 2 * SS / w)
    across = np.clip((u - frame_l) / (frame_r - frame_l), 0, 1)
    gloss = np.exp(-((across - 0.4) / 0.28) ** 2)
    frame_col = mix((214, 200, 188), (255, 251, 246), gloss)
    img = img * (1 - fr[..., None]) + frame_col * fr[..., None]
    # The blade behind, out to the edge: taupe grey, darker into the frame.
    back = band(u - frame_l, 2 * SS / w)
    shade = np.clip((frame_l - u) / 0.05, 0, 1)
    back_col = mix((150, 134, 126), (176, 163, 156), v * 0.4 + shade * 0.6)
    back_col = back_col * (0.82 + 0.18 * shade)[..., None]
    img = img * (1 - back[..., None]) + back_col * back[..., None]
    # Fine dark lines either side of the frame.
    for edge in (frame_l, frame_r):
        line = band(np.abs(u - edge) - 0.0025, 1.5 * SS / w) * 0.55
        img = img * (1 - line[..., None]) + np.array([70, 48, 34]) * line[..., None]
    out = finish(img)

    # "xpsemu" down the tab, in dark brown.
    font = None
    for path in ('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
                 'C:/Windows/Fonts/segoeui.ttf', 'C:/Windows/Fonts/arial.ttf'):
        if os.path.exists(path):
            font = ImageFont.truetype(path, 80)
            break
    font = font or ImageFont.load_default(size=80)
    label = Image.new('RGBA', (560, 120), (0, 0, 0, 0))
    ImageDraw.Draw(label).text((8, 6), 'xpsemu', font=font,
                               fill=(78, 42, 16, 235))
    label = label.rotate(-90, expand=True)
    vy = 0.24
    cu = (0.27 - 0.12 * math.sin(math.pi * vy)) + 0.11 + 0.085
    out.paste(label, (int(cu * W - label.width / 2), int(vy * H - 200)), label)
    return out


def right_panel():
    # The silver blade on the outer edge (u = 0 is the screen's edge, so the
    # picture is flipped at the end); a strip of orange towards the game.
    bow = np.sin(np.pi * v)
    blade_r = 0.42 - 0.08 * bow
    img = orange_panel(0.95)
    # The orange ends in a rounded rim, a little shadow past it.
    shadow = np.clip(1 - (u - blade_r) / 0.06, 0, 1) * (u > blade_r)
    img = img * (1 - 0.35 * shadow[..., None])
    rim = band(np.abs(u - blade_r - 0.012) - 0.004, 2 * SS / w)
    img = img * (1 - rim[..., None]) + np.array([110, 56, 14]) * rim[..., None]
    # The blade: silver, lit from above, bright bevel along its edge.
    blade = band(u - blade_r, 2 * SS / w)
    across = np.clip(u / blade_r, 0, 1)
    blade_col = mix((150, 136, 129), (206, 198, 193), across * 0.7 + (1 - v) * 0.3)
    bevel = np.exp(-((u - (blade_r - 0.03)) / 0.018) ** 2)
    blade_col = blade_col + (np.array([250, 248, 246]) - blade_col) * bevel[..., None] * 0.8
    groove = np.exp(-((u - (blade_r - 0.075)) / 0.008) ** 2) * 0.25
    blade_col = blade_col * (1 - groove[..., None])
    img = img * (1 - blade[..., None]) + blade_col * blade[..., None]
    return finish(img).transpose(Image.FLIP_LEFT_RIGHT)


def green(im):
    """The same panels in green: only the orange and brown turn (the cream
    frame, the taupe and the silver stay as they are)."""
    hsv = np.asarray(im.convert('HSV'), np.float32)
    h, s = hsv[..., 0] * 360 / 255, hsv[..., 1] / 255
    warm = np.clip((s - 0.18) / 0.15, 0, 1) * ((h > 5) & (h < 50))
    hsv[..., 0] = np.where(warm > 0, (h + (100 - 28) * warm) * 255 / 360,
                           hsv[..., 0])
    hsv[..., 2] = hsv[..., 2] * (1 - 0.12 * warm)  # Green reads brighter
    return Image.fromarray(np.clip(hsv, 0, 255).astype(np.uint8),
                           'HSV').convert('RGB')


here = os.path.dirname(os.path.abspath(__file__))
root = os.path.normpath(os.path.join(here, '..', '..'))
left, right = left_panel(), right_panel()
left_g, right_g = green(left), green(right)
if len(sys.argv) > 1:
    left.save(os.path.join(sys.argv[1], 'blades-left.png'))
    right.save(os.path.join(sys.argv[1], 'blades-right.png'))
    left_g.save(os.path.join(sys.argv[1], 'blades-green-left.png'))
    right_g.save(os.path.join(sys.argv[1], 'blades-green-right.png'))
out = ['// Generated by ps5/art/make-blades.py: do not edit.', '#pragma once', '']
for name, im in (('Left', left), ('Right', right), ('GreenLeft', left_g),
                 ('GreenRight', right_g)):
    buf = io.BytesIO()
    im.save(buf, 'JPEG', quality=92)
    data = buf.getvalue()
    out.append(f'// The {name.lower()} side panel, {W}x{H} JPEG.')
    out.append(f'static const unsigned char kBlades{name}[{len(data)}] = {{')
    for i in range(0, len(data), 20):
        out.append('    ' + ', '.join(str(b) for b in data[i:i + 20]) + ',')
    out += ['};', '']
with open(os.path.join(root, 'ui', 'xui', 'blades-art.h'), 'w') as f:
    f.write('\n'.join(out))
print('blades art written')
