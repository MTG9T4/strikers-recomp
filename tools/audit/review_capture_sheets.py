"""Make labelled lossless contact sheets for inspecting every capture of a log.

Paths are restricted to build/run. Labels are capture ordinals, not guessed
frame numbers; review_captures.py retains exact frame mappings where available.
"""
import argparse
from pathlib import Path
import re
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    args = parser.parse_args()
    out = ROOT / 'build/run/job09-review-sheets'
    out.mkdir(exist_ok=True)
    for log in args.logs:
        paths = re.findall(r'^capture: \d+x\d+ picture saved to (.+)$', log.read_text(errors='replace'), re.M)
        for first in range(0, len(paths), 8):
            group = paths[first:first+8]
            sheet = Image.new('RGB', (1280, ((len(group)+1)//2)*390), 'white')
            draw = ImageDraw.Draw(sheet)
            for i, raw in enumerate(group):
                src = Path(raw.strip()).resolve()
                if not src.is_relative_to(ROOT / 'build/run'):
                    raise ValueError('capture outside build/run')
                with Image.open(src) as im:
                    sheet.paste(im.convert('RGB').resize((640, 360)), (i%2*640, i//2*390+30))
                draw.text((i%2*640+8, i//2*390+6), f'{log.stem} capture {first+i+1}: {src.name}', fill='black')
            dst = out / f'{log.stem}-{first+1:02}.png'
            sheet.save(dst)
            print(dst)


if __name__ == '__main__':
    main()
