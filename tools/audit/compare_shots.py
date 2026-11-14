"""Compare the frame_<n>.bmp captures (STRIKERS_SHOT_RANGES + STRIKERS_SHOT_DIR) of two runs, frame by frame (audit 01).

    python -I tools/audit/compare_shots.py build/run/<a>-shots build/run/<b>-shots

Per frame present in both folders: identical (byte-for-byte pixels) or the share of differing pixels and the mean absolute
difference. In-match frames depend on the game's refresh-count time step, so only frames whose game state does not depend on
the frame rate (menus at the game's own 30 fps) are expected to be identical; a base-vs-base pair gives the noise floor.
"""
import argparse
import hashlib
import re
from pathlib import Path

from PIL import Image, ImageChops


def frames(folder):
    return {int(re.match(r'frame_(\d+)\.bmp$', p.name)[1]): p for p in Path(folder).glob('frame_*.bmp')}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    a = ap.parse_args()
    fa, fb = frames(a.a), frames(a.b)
    for n in sorted(set(fa) & set(fb)):
        ia, ib = Image.open(fa[n]).convert('RGB'), Image.open(fb[n]).convert('RGB')
        if ia.size != ib.size:
            print('frame %6d: sizes differ %s %s' % (n, ia.size, ib.size))
            continue
        ha, hb = hashlib.sha256(ia.tobytes()).hexdigest()[:16], hashlib.sha256(ib.tobytes()).hexdigest()[:16]
        if ha == hb:
            print('frame %6d: identical (%s)' % (n, ha))
            continue
        d = ImageChops.difference(ia, ib)
        px = d.convert('L').point(lambda v: 255 if v else 0)
        changed = sum(px.histogram()[1:]) / (ia.size[0] * ia.size[1])
        mean = sum(i * c for i, c in enumerate(d.convert('L').histogram())) / (ia.size[0] * ia.size[1])
        print('frame %6d: differs: %.2f%% of pixels, mean abs difference %.2f (%s vs %s)' % (n, 100 * changed, mean, ha, hb))
    only = sorted(set(fa) ^ set(fb))
    if only:
        print('frames in one run only:', only)


if __name__ == '__main__':
    main()
