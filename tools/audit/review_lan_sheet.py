"""Labelled review sheet of existing lossless captures; does not certify a match."""
import argparse
import json
from pathlib import Path
import re
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('name')
    a = p.parse_args()
    assert Path(a.name).name == a.name
    rows = []
    for role in ('host', 'station', 'station2'):
        path = ROOT / f'build/run/{a.name}-{role}-captures.json'
        if not path.exists():
            continue
        captures = json.loads(path.read_text())
        log = (ROOT / f'build/run/{a.name}-{role}.log').read_text(errors='replace')
        fence = re.search(r'^input wait: satisfied at frame (\d+);', log, re.M)
        targets = [10010, 10170, int(fence[1]) + 1080 if fence else 11000, max(c['frame'] for c in captures)]
        chosen = [min(captures, key=lambda c: abs(c['frame'] - f)) for f in targets]
        rows.append((role, chosen))
    sheet = Image.new('RGB', (1920, len(rows) * 300), 'black')
    draw = ImageDraw.Draw(sheet)
    report = {}
    for y, (role, captures) in enumerate(rows):
        report[role] = []
        for x, c in enumerate(captures):
            with Image.open(ROOT / c['png']) as im:
                im.thumbnail((480, 270))
                sheet.paste(im, (x * 480, y * 300))
            draw.text((x * 480 + 5, y * 300 + 275), f'{role}: frame {c["frame"]}', fill='white')
            report[role].append(c['frame'])
    out = ROOT / f'build/run/{a.name}-review-sheet.png'
    assert not out.exists(), 'preserve earlier review sheets; use a distinct copy if more captures arrive'
    sheet.save(out)
    print(json.dumps(report))


if __name__ == '__main__':
    main()
