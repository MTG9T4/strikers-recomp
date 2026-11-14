"""Append future frame-script inputs using the latest runtime frame counter.

python tools/audit/drive_input.py <log> <seq> 0+8:K 60+12:Z 200+8:*
Offsets are relative to the latest logged frame plus --ahead (default 600).
"""
import argparse
from pathlib import Path
import re


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('sequence', type=Path)
    p.add_argument('steps', nargs='+')
    p.add_argument('--ahead', type=int, default=600)
    args = p.parse_args()
    frames = [int(x) for x in re.findall(r'nvn_vk_mem: frame (\d+)', args.log.read_text(errors='replace'))]
    if not frames:
        raise SystemExit('No frame counter yet')
    first = max(frames) + args.ahead
    lines = []
    for step in args.steps:
        m = re.fullmatch(r'(\d+)\+(\d+):([A-Za-z!\-*2468]+)', step)
        if not m or int(m[2]) <= 0:
            raise SystemExit('Invalid step: ' + step)
        lines.append(f'{first + int(m[1])}+{m[2]}:{m[3]}')
    with args.sequence.open('a') as f:
        f.write('\n' + '\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
