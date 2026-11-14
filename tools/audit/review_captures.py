"""Save lossless PNG review copies of runtime album captures and pixel statistics.

python tools/audit/review_captures.py build/run/<run>.log --frames 1,30,120
The log's capture order must match the requested frame list.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--frames', help='older logs need the requested frame list; new logs record capture-frame')
    args = parser.parse_args()
    log = args.log.read_text(errors='replace')
    frames = [int(x) for x in args.frames.split(',')] if args.frames else [int(x) for x in re.findall(r'^capture-frame: (\d+)$', log, re.M)]
    captures = re.findall(r'^capture: \d+x\d+ picture saved to (.+)$', log, re.M)
    if len(captures) != len(frames):
        raise SystemExit(f'{len(captures)} captures but {len(frames)} frame numbers; inspect the log')
    report = []
    for frame, source in zip(frames, captures):
        src = Path(source.strip()).resolve()
        if not any(src.is_relative_to(ROOT / folder)
                   for folder in ('build/run', 'build/phase8')):
            raise SystemExit('capture outside project runtime evidence folders: ' + str(src))
        # Nested online-pair runs reuse role names (client1/client2). Keep their
        # review copies beside the log so another run cannot overwrite them.
        dst = args.log.resolve().parent / f'{args.log.stem}-frame-{frame}.png'
        with Image.open(src) as original:
            im = original.convert('RGB')
            rgb = im.split()
            peak = ImageChops.lighter(ImageChops.lighter(rgb[0], rgb[1]), rgb[2])
            extrema = im.getextrema()
            row = {'frame': frame, 'source': str(src.relative_to(ROOT)),
                   'png': str(dst.relative_to(ROOT)), 'size': im.size, 'extrema': extrema,
                   'nonzero_rgb_pixels': im.width * im.height - peak.histogram()[0],
                   'spatially_uniform': all(lo == hi for lo, hi in extrema)}
            # Keep an existing identical review copy; never replace differing evidence.
            if dst.exists():
                with Image.open(dst) as prior:
                    if prior.convert('RGB').tobytes() != im.tobytes():
                        raise SystemExit('different review image already exists: ' + str(dst))
            else:
                im.save(dst)
        row['png_sha256'] = hashlib.sha256(dst.read_bytes()).hexdigest()
        report.append(row)
        print(json.dumps(row))
    manifest = args.log.resolve().with_name(f'{args.log.stem}-captures.json')
    manifest.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
