"""Audit 03: the audio output numbers of finished game logs, one row per log.

python -I tools/audit/audio_summary.py build/run/a03-*.log [--stalls]

Reads the end-of-run lines of nnaudio.c (underruns, filled gaps, starved appends, latency, late appends, explained stretches) and
nnhid.c (input timing). --stalls also lists every 'nn::audio stall' line with the producer's time not on a CPU.
"""
import argparse
import re
from pathlib import Path


def summary(path):
    t = Path(path).read_text(errors='replace')
    r = dict(log=Path(path).name)
    m = re.search(r'underruns \(device ran dry while started, after the first sound\): (\d+), ([\d.]+) ms in all, longest ([\d.]+) ms', t)
    if m:
        r.update(underruns=int(m[1]), underrun_ms=float(m[2]), longest_ms=float(m[3]))
    m = re.search(r'buffers appended, (\d+) released', t)
    m = re.search(r'gaps filled \(.*?low-water mark (\d+) ms, now (\d+) ms after (\d+) late fills\): (\d+) fills, ([\d.]+) ms in all, '
                  r'longest run ([\d.]+) ms, (\d+) faded to silence', t)
    if m:                                                       # concealment (nnaudio.c, 2026-11-21)
        r.update(fills=int(m[4]), fill_ms=float(m[5]), fill_run_ms=float(m[6]), faded=int(m[7]),
                 low_water=f'{m[1]}->{m[2]}ms', late_fills=int(m[3]))
    m = re.search(r'the output had no samples left\): (\d+) of (\d+)', t)
    if m:
        r.update(starved=int(m[1]), appends=int(m[2]))
    m = re.search(r'latency after an append .*?p01/p50/p99 (\d+)/(\d+)/(\d+) ms over \d+ appends; appends more than \w+ buffer periods after the previous one (\d+), '
                  r'longest gap ([\d.]+) ms; late stretches of (\d+) ms or more explained \(STRIKERS_AUDIO_STALL\) (\d+), (\d+) of them with the device dry', t)
    if m:
        r.update(lat=f'{m[1]}/{m[2]}/{m[3]}', late_gaps=int(m[4]), longest_gap_ms=float(m[5]), stalls=int(m[7]), stalls_dry=int(m[8]))
    m = re.search(r'the producer raised to', t)
    r['raised'] = bool(m)
    m = re.search(r'newest read to present p50/p90/p99 ([\d.]+)/([\d.]+)/([\d.]+) ms over (\d+) frames', t)
    if m:
        r['read_to_present'] = f'{m[1]}/{m[2]}/{m[3]}'
    m = re.search(r'reads per presented frame 0:(\d+) 1:(\d+) 2:(\d+) 3:(\d+) 4\+:(\d+)', t)
    if m:
        r['reads_per_frame'] = '/'.join(m.groups())
    m = re.search(r'time between presents: median ([\d.]+) ms, p99 ([\d.]+)', t)
    if m:
        r['present_ms_p50_p99'] = f'{m[1]}/{m[2]}'
    return r, t


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('logs', nargs='+')
    p.add_argument('--stalls', action='store_true')
    a = p.parse_args()
    for path in a.logs:
        r, t = summary(path)
        print(' '.join(f'{k}={v}' for k, v in r.items()))
        if a.stalls:
            for line in t.splitlines():
                if line.startswith('nn::audio stall:'):
                    off = sum(float(x) for x in re.findall(r'not on a CPU\) ([\d.]+) ms', line))
                    wait = sum(float(x) for x in re.findall(r'\(waiting[^)]*\) ([\d.]+) ms', line))
                    head = re.match(r'nn::audio stall: frame (\d+), ([\d.]+) ms between appends( \(the device ran dry\))?', line)
                    tail = line.split('; its processor', 1)[1] if '; its processor' in line else ''
                    print(f'  frame {head[1]} gap {head[2]} ms{" DRY" if head[3] else ""}: waiting-or-ready {wait:.1f} ms, lifted not on CPU {off:.1f} ms; processor{tail}')


if __name__ == '__main__':
    main()
