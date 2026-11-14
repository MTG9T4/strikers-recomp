"""Project picture for Strikers Recomp from the programs' own icon (tools/release/app_icon.py): writes art/pfp-512.png,
art/pfp-1024.png and art/social-preview-1280x640.png (GitHub's social preview). No Nintendo artwork, logo or font.

  .venv/Scripts/python.exe -I scripts/oneoff/make_pfp.py      (from the project folder)
"""
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/release'))
from app_icon import NAVY, NAVY2, TEAL, gradient, icon

OUT = ROOT / 'art'


def social(icon_img):
    W, H = 1280, 640
    img = gradient((W, H), NAVY, NAVY2).convert('RGBA')
    img.alpha_composite(icon_img.resize((440, 440), Image.LANCZOS), (70, 100))
    d = ImageDraw.Draw(img)
    fonts = Path('C:/Windows/Fonts')
    bold = ImageFont.truetype(str(fonts / 'segoeuib.ttf'), 92)
    reg = ImageFont.truetype(str(fonts / 'segoeui.ttf'), 34)
    d.text((560, 200), 'Strikers', font=bold, fill=(244, 248, 255))
    d.text((560, 300), 'Recomp', font=bold, fill=TEAL)
    d.text((564, 430), 'Native Windows static recompilation', font=reg, fill=(200, 210, 235))
    d.text((564, 476), 'Bring your own game files', font=reg, fill=(255, 160, 70))
    return img


def main():
    OUT.mkdir(exist_ok=True)
    big = icon()
    for n in (1024, 512):
        big.resize((n, n), Image.LANCZOS).save(OUT / f'pfp-{n}.png')
    social(big).convert('RGB').save(OUT / 'social-preview-1280x640.png')
    print('written to', OUT)


if __name__ == '__main__':
    main()
