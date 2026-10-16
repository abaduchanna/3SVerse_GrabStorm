#!/usr/bin/env python3
"""GrabStorm asset generator — 3SVerse brand (orb + spiral from the xfer design system).
Generates: src/res/{logo,spiral,orb}.png (RCDATA embeds), src/res/icon.ico,
extension/icons/icon{16,32,48,128}.png
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root
XFER = "/home/z/my-project/3SVerse_WiFi_Transfer/pc/assets"
RES = os.path.join(ROOT, "src", "res")
EXT = os.path.join(ROOT, "extension", "icons")
os.makedirs(RES, exist_ok=True)
os.makedirs(EXT, exist_ok=True)

# 1) brand art: convert xfer webp -> png (same files the brand system standardizes)
logo = Image.open(os.path.join(XFER, "logo.png")).convert("RGBA")
logo.save(os.path.join(RES, "logo.png"), optimize=True)
print("logo.png", logo.size)

spiral = Image.open(os.path.join(XFER, "spiral.webp")).convert("RGBA")
spiral.thumbnail((618, 640), Image.LANCZOS)   # T192-optimized embed size
spiral.save(os.path.join(RES, "spiral.png"), optimize=True)
print("spiral.png", spiral.size)

orb = Image.open(os.path.join(XFER, "orb.webp")).convert("RGBA")
orb.thumbnail((480, 480), Image.LANCZOS)
orb.save(os.path.join(RES, "orb.png"), optimize=True)
print("orb.png", orb.size)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(4))


def brand_mark(size):
    """Dark rounded tile + glossy cyan->magenta orb + white spiral (3SVerse mark)."""
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # tile
    rad = max(2, int(s * 0.22))
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=rad, fill=(13, 12, 20, 255))
    # orb: radial gradient sphere center, highlight top-left
    cx, cy = s * 0.52, s * 0.54
    r = s * 0.30
    orb_img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    od = ImageDraw.Draw(orb_img)
    steps = 48
    for i in range(steps, 0, -1):
        t = i / steps
        rr = r * t
        # outer -> inner: magenta edge -> cyan core -> white-ish highlight
        if t > 0.85:
            col = lerp((228, 75, 215, 255), (110, 231, 239, 255), (1 - t) / 0.15)
        else:
            col = lerp((110, 231, 239, 255), (246, 250, 255, 255), 1 - t / 0.85)
        od.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=col)
    # highlight gloss
    hg = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hg)
    hx, hy = cx - r * 0.35, cy - r * 0.42
    for i in range(20, 0, -1):
        t = i / 20
        rr = r * 0.34 * t
        hd.ellipse([hx - rr, hy - rr * 0.8, hx + rr, hy + rr * 0.8],
                   fill=(255, 255, 255, int(70 * (1 - t)) ))
    orb_img = Image.alpha_composite(orb_img, hg)
    img = Image.alpha_composite(img, orb_img)
    # spiral (archimedean, 2.2 turns) over the orb
    d = ImageDraw.Draw(img)
    import math
    turns = 2.2
    pts = []
    steps = 160
    for i in range(steps + 1):
        t = i / steps
        ang = t * turns * 2 * math.pi
        rad_t = r * 1.28 * t
        x = cx + rad_t * math.cos(ang)
        y = cy + rad_t * math.sin(ang)
        pts.append((x, y))
    lw = max(1, int(s * 0.028))
    for i in range(len(pts) - 1):
        t = i / (len(pts) - 1)
        col = lerp((255, 255, 255, 235), (110, 231, 239, 235), t)
        d.line([pts[i], pts[i + 1]], fill=col, width=lw)
    return img


# 2) application icon (.ico multi-size)
mark512 = brand_mark(512)
ico_sizes = [256, 128, 64, 48, 32, 24, 16]
mark512.save(os.path.join(RES, "icon.ico"),
             format="ICO", sizes=[(x, x) for x in ico_sizes])
print("icon.ico written")

# 3) extension icons
for x in (128, 48, 32, 16):
    brand_mark(x).save(os.path.join(EXT, f"icon{x}.png"))
print("extension icons written")
