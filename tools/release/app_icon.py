"""The project's own icon for StrikersSetup.exe and StrikersRecomp.exe, drawn from this code at build time (the
repository holds no image files): a football with circuit traces and speed lines. No Nintendo artwork, logo or font.

  python tools/release/app_icon.py <out.ico>      (build_apps.py calls write_ico)
"""
import math
from pathlib import Path
import sys

NAVY, NAVY2 = (11, 21, 48), (30, 14, 60)
TEAL, ORANGE = (40, 230, 210), (255, 122, 26)
INK = (14, 24, 50)
ICO_SIZES = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (96, 96), (128, 128), (256, 256)]


def gradient(size, top, bottom):
    from PIL import Image
    g = Image.new('RGB', (1, size[1]))
    for y in range(size[1]):
        t = y / (size[1] - 1)
        g.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    return g.resize(size)


def pentagon(cx, cy, r, rot=-90):
    return [(cx + r * math.cos(math.radians(rot + 72 * k)), cy + r * math.sin(math.radians(rot + 72 * k))) for k in range(5)]


def icon(size=2048):
    """The icon as an RGBA image of size x size (drawn on a 512 grid scaled by size/512)."""
    from PIL import Image, ImageDraw, ImageFilter
    u = size / 512
    img = gradient((size, size), NAVY, NAVY2).convert('RGBA')
    mask = Image.new('L', (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=int(110 * u), fill=255)
    cx, cy, R = 280 * u, 256 * u, 150 * u

    d = ImageDraw.Draw(img)                         # speed lines, left of the ball
    for y, x0, w in ((-70, 40, 9), (-25, 18, 12), (20, 30, 12), (65, 55, 9), (105, 85, 7)):
        d.rounded_rectangle((x0 * u, cy + (y - w / 2) * u, cx - 60 * u, cy + (y + w / 2) * u), radius=w / 2 * u,
                            fill=ORANGE if y in (-25, 20) else (255, 160, 70))

    glow = Image.new('RGBA', (size, size), (0, 0, 0, 0))   # glow behind the ball
    ImageDraw.Draw(glow).ellipse((cx - R - 26 * u, cy - R - 26 * u, cx + R + 26 * u, cy + R + 26 * u), fill=TEAL + (150,))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(28 * u)))

    ball = Image.new('RGBA', (size, size), (0, 0, 0, 0))   # white ball, dark pentagons, teal seams, clipped to its circle
    b = ImageDraw.Draw(ball)
    b.ellipse((cx - R, cy - R, cx + R, cy + R), fill=(244, 248, 255))
    shade = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(shade).ellipse((cx - R * 0.55, cy - R * 0.2, cx + R * 1.6, cy + R * 1.8), fill=(120, 140, 190, 110))
    ball.alpha_composite(shade.filter(ImageFilter.GaussianBlur(40 * u)))
    b = ImageDraw.Draw(ball)
    centre = pentagon(cx, cy, 50 * u)
    seam = int(7 * u)
    for k, (x, y) in enumerate(centre):
        a = math.radians(-90 + 72 * k)
        b.line((x, y, cx + 108 * u * math.cos(a), cy + 108 * u * math.sin(a)), fill=TEAL, width=seam)
    for k in range(5):
        a = math.radians(-90 + 36 + 72 * k)
        px, py = cx + 152 * u * math.cos(a), cy + 152 * u * math.sin(a)
        outer = pentagon(px, py, 50 * u, rot=math.degrees(a) + 180)
        b.polygon(outer, fill=INK)
        b.line(outer + outer[:1], fill=TEAL, width=seam)
    b.polygon(centre, fill=INK)
    b.line(centre + centre[:1], fill=TEAL, width=seam)
    clip = Image.new('L', (size, size), 0)
    ImageDraw.Draw(clip).ellipse((cx - R, cy - R, cx + R, cy + R), fill=255)
    ball.putalpha(Image.composite(ball.getchannel('A'), Image.new('L', (size, size), 0), clip))
    img.alpha_composite(ball)
    d = ImageDraw.Draw(img)
    d.ellipse((cx - R, cy - R, cx + R, cy + R), outline=TEAL, width=int(8 * u))

    w = int(8 * u)                                  # circuit traces leaving the ball on the right, ending in nodes
    for dy, run, rise in ((-95, 26, -30), (0, 40, 0), (95, 26, 30)):
        a = math.asin(dy / 150)
        x0, y0 = cx + R * math.cos(a), cy + R * math.sin(a)
        x1 = x0 + run * u
        x2, y2 = x1 + abs(rise) * u, y0 + rise * u
        x3 = 492 * u
        d.line((x0, y0, x1, y0, x2, y2, x3 - 14 * u, y2), fill=TEAL, width=w, joint='curve')
        d.ellipse((x3 - 26 * u, y2 - 12 * u, x3 - 2 * u, y2 + 12 * u), fill=NAVY, outline=TEAL, width=int(6 * u))

    img.putalpha(mask)
    return img


def write_ico(path):
    """A Windows icon with every common size, each scaled down from the 2048 drawing."""
    from PIL import Image
    big = icon()
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    big.resize((256, 256), Image.LANCZOS).save(path, format='ICO', sizes=ICO_SIZES)
    return path


if __name__ == '__main__':
    print(write_ico(sys.argv[1]))
