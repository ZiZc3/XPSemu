#!/usr/bin/env python3
# The emblem without its background: icon-1024.png's white shapes as alpha, for the dashboard
# (ps5/ui/emblem-art.h), and a look at it in place on a screenshot when one is given.
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
icon = Image.open(os.path.join(HERE, "icon-1024.png")).convert("RGB")
blue = icon.split()[2]
alpha = blue.point(lambda v: max(0, min(255, (v - 70) * 255 // 150)))
emblem = Image.new("RGBA", icon.size, (255, 255, 255, 0))
emblem.putalpha(alpha)
small = emblem.resize((640, 640), Image.LANCZOS)
out = os.path.join(HERE, "emblem.png")
small.save(out, optimize=True)
data = open(out, "rb").read()
with open(os.path.join(HERE, "..", "ui", "emblem-art.h"), "w") as f:
    f.write("// Made by ps5/art/make_emblem.py\nstatic const unsigned char kEmblemPng[] = {\n")
    for i in range(0, len(data), 24):
        f.write(",".join(str(b) for b in data[i:i + 24]) + ",\n")
    f.write("};\n")
print("emblem", len(data), "bytes")

if len(sys.argv) > 2:
    shot = Image.open(sys.argv[1]).convert("RGB")
    W, H = shot.size
    # the old ball away: the page's dark, with its grid left out
    clean = shot.copy()
    box = (110, 170, 830, 890)
    patch = shot.crop((800, 780, 1520, 1080)).resize((box[2] - box[0], box[3] - box[1])).filter(ImageFilter.GaussianBlur(30))
    mask = Image.new("L", shot.size, 0)
    ImageDraw.Draw(mask).ellipse(box, fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(22))
    layer = clean.copy()
    layer.paste(patch, box[:2])
    clean = Image.composite(layer, clean, mask)

    def place(base, size, cx, cy, tint=(255, 255, 255), glow=(150, 235, 60), strength=1.0):
        e = emblem.resize((size, size), Image.LANCZOS)
        a = e.split()[3]
        out = base.convert("RGBA")
        for blur, k in ((60, 0.55), (26, 0.65), (9, 0.5)):
            big = Image.new("L", out.size, 0)
            big.paste(a, (cx - size // 2, cy - size // 2))
            big = big.filter(ImageFilter.GaussianBlur(blur)).point(lambda v: int(min(255, v * k * strength)))
            g = Image.new("RGBA", out.size, glow + (0,))
            g.putalpha(big)
            out = Image.alpha_composite(out, g)
        body = Image.new("RGBA", (size, size), tint + (0,))
        body.putalpha(a)
        out.alpha_composite(body, (cx - size // 2, cy - size // 2))
        return out

    # A: the emblem alone, glowing
    a = place(clean, 500, 470, 530)
    a.convert("RGB").save(os.path.join(sys.argv[2], "home-A-emblem.jpg"), quality=90)

    # B: set in a jewel, as the console's own lid has one
    b = clean.convert("RGBA")
    d = Image.new("RGBA", b.size, (0, 0, 0, 0))
    dr = ImageDraw.Draw(d)
    cx, cy, r = 470, 530, 300
    dr.ellipse((cx - r - 16, cy - r - 16, cx + r + 16, cy + r + 16), outline=(150, 235, 60, 90), width=3)
    dr.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(10, 42, 6, 235), outline=(205, 245, 45, 255), width=8)
    dr.ellipse((cx - r + 26, cy - r + 26, cx + r - 26, cy + r - 26), outline=(70, 170, 30, 170), width=3)
    b = Image.alpha_composite(b, d)
    # the light in the jewel
    light = Image.new("L", b.size, 0)
    ImageDraw.Draw(light).ellipse((cx - 170, cy - 170, cx + 170, cy + 170), fill=120)
    light = light.filter(ImageFilter.GaussianBlur(70))
    g = Image.new("RGBA", b.size, (110, 215, 40, 0))
    g.putalpha(light)
    b = Image.alpha_composite(b, g)
    b = place(b.convert("RGB"), 400, cx, cy, strength=0.7)
    b.convert("RGB").save(os.path.join(sys.argv[2], "home-B-jewel.jpg"), quality=90)

    # C: the emblem in lime, big and quiet, cut off by the screen's edge
    c = place(clean, 800, 330, 540, tint=(190, 240, 60), glow=(90, 190, 40), strength=0.6)
    c.convert("RGB").save(os.path.join(sys.argv[2], "home-C-big.jpg"), quality=90)
