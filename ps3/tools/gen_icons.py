#!/usr/bin/env python3
"""Generate the XMB artwork for the PS3 package (needs python3-pil).

  ps3/pkgfiles/ICON0.PNG  320x176   icon shown in the XMB
  ps3/pkgfiles/PIC1.PNG   1920x1080 background shown while the icon is selected

Uses the chiaki-ng logo from assets/official-artwork (same AGPL repository).
Run from the repository root: python3 ps3/tools/gen_icons.py
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGO = os.path.join(ROOT, 'assets', 'official-artwork', 'Chiaki-ng logo color.png')
OUT = os.path.join(ROOT, 'ps3', 'pkgfiles')
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'

LIME = (200, 222, 40)
TEAL = (40, 190, 170)


def logo_on_dark(width):
    """chiaki-ng colour logo with the navy parts turned white (for a dark background)."""
    im = Image.open(LOGO).convert('RGBA')
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a < 24:
                px[x, y] = (0, 0, 0, 0)  # the file has faint pixels everywhere: drop them
            elif r < 120 and g < 120:    # navy -> white, lime stays lime
                px[x, y] = (255, 255, 255, a)
    h = round(im.height * width / im.width)
    return im.resize((width, h), Image.LANCZOS)


def background(w, h):
    """Dark XMB-like gradient with a glossy top and soft lime/teal ribbons."""
    img = Image.new('RGB', (w, h))
    top, bottom = (14, 28, 58), (2, 4, 10)
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))

    layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    # ribbons: thick soft sine curves
    for (base, amp, freq, phase, thick, col, alpha) in [
        (0.62, 0.10, 1.3, 0.4, 0.050, TEAL, 70),
        (0.70, 0.08, 1.9, 2.1, 0.030, LIME, 90),
        (0.66, 0.05, 2.7, 4.0, 0.012, (230, 240, 255), 110),
    ]:
        pts = []
        for x in range(0, w + 1, max(1, w // 160)):
            u = x / w
            y = h * (base + amp * math.sin(u * math.pi * 2 * freq * 0.5 + phase))
            pts.append((x, y))
        ld.line(pts, fill=col + (alpha,), width=max(2, round(h * thick)))
    layer = layer.filter(ImageFilter.GaussianBlur(radius=max(1, h * 0.012)))
    img.paste(layer, (0, 0), layer)

    # glossy highlight on the upper part
    gloss = Image.new('L', (w, h), 0)
    gd = ImageDraw.Draw(gloss)
    gd.ellipse([-w * 0.3, -h * 1.15, w * 1.3, h * 0.42], fill=34)
    gloss = gloss.filter(ImageFilter.GaussianBlur(radius=h * 0.05))
    img.paste((255, 255, 255), (0, 0), gloss)
    return img.convert('RGBA')


def glow(logo, radius, color, strength):
    a = logo.split()[3].filter(ImageFilter.GaussianBlur(radius))
    a = a.point(lambda v: min(255, int(v * strength)))
    g = Image.new('RGBA', logo.size, color + (0,))
    g.putalpha(a)
    return g


def badge(text, height):
    font = ImageFont.truetype(FONT, round(height * 0.72))
    tw = font.getlength(text)
    w = round(tw + height * 0.9)
    img = Image.new('RGBA', (w, height), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, w - 1, height - 1], radius=height // 2, fill=LIME + (255,))
    d.text(((w - tw) / 2, height * 0.5), text, font=font, fill=(10, 22, 44), anchor='lm')
    return img


def icon0():
    w, h = 320, 176
    img = background(w, h)
    logo = logo_on_dark(206)
    x, y = 18, 18
    img.alpha_composite(glow(logo, 5, LIME, 0.5), (x, y))
    img.alpha_composite(logo, (x, y))
    b = badge('PS3', 30)
    # right of the wordmark, on its baseline
    img.alpha_composite(b, (w - b.width - 14, y + logo.height - b.height - 2))
    return img


def pic1():
    w, h = 1920, 1080
    img = background(w, h)
    logo = logo_on_dark(980)
    faint = logo.copy()
    faint.putalpha(faint.split()[3].point(lambda v: v * 0.16))
    img.alpha_composite(faint, (w - logo.width - 120, (h - logo.height) // 2 - 60))
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    icon0().convert('RGB').save(os.path.join(OUT, 'ICON0.PNG'), optimize=True)
    pic1().convert('RGB').save(os.path.join(OUT, 'PIC1.PNG'), optimize=True)
    print('wrote', OUT)


if __name__ == '__main__':
    sys.exit(main())
