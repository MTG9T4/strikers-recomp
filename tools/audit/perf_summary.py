"""Per-window and summary performance numbers of runtime logs (audit 01).

    python -I tools/audit/perf_summary.py build/run/<a>.log [build/run/<b>.log ...] [--from 4200] [--to 11100] [--windows]

For each log: the nvn_perf / nvn_vk_perf windows (300 frames each) whose first frame is in [from, to) are averaged
(frame-weighted): fps, frame time, render-thread CPU, batch replay, draw preparation, descriptor sets, and the final hold
counters ("holds so far" of the last window, and the exit line of nvn_vk_pipelines).
"""
import argparse
import re

R_FR = re.compile(r'nvn_perf: frames (\d+)\.\.(\d+) frame time ms: avg ([\d.]+) min ([\d.]+) median ([\d.]+) p99 ([\d.]+) max ([\d.]+) \| fps avg ([\d.]+)')
R_TH = re.compile(r'nvn_perf: cpu ms/frame by thread \(all ([\d.]+)\):.*?render\(host\)\[(\d+)\] ([\d.]+)')
R_VK = re.compile(r'nvn_vk_perf: frames (\d+)\.\.(\d+) ms/frame: batch replay ([\d.]+) \(of it: dispatch ([\d.]+), draw preparation ([\d.]+), pipelines ([\d.]+), shader modules ([\d.]+)\), '
                  r'wait for the GPU ([\d.]+).*?present ([\d.]+) \| per frame: ([\d.]+) dispatches, ([\d.]+) descriptor sets allocated, ([\d.]+) reused.*?holds so far: (\d+), ([\d.]+) ms, longest ([\d.]+) ms')
R_WAIT = re.compile(r'wait-present ([\d.]+) \(([\d.]+)\) wait-other ([\d.]+) \(([\d.]+)\)')
R_HOLD = re.compile(r'nvn_vk_pipelines: holds of the render thread at a draw: (\d+), (\d+) ms in all, longest ([\d.]+) ms \((.*)\)')
R_GPU = re.compile(r'nvn_vk_gpu: frames (\d+)\.\.(\d+): GPU time ([\d.]+) ms per frame')
R_VBL = re.compile(r'display_vblank(?:_total)?: (\d+) presents: monitor refreshes per frame 0:(\d+) 1:(\d+) 2:(\d+) 3\+:(\d+) \| frames never shown (\d+), shown for other than the game.s refreshes (\d+)')
R_DRAWS = re.compile(r'nvn_vk: frames (\d+)\.\.(\d+): ([\d.]+) fps, (\d+) draws per frame')


def parse(path):
    win = {}
    th_pending = None
    wait_pending = None
    exit_hold = None
    last_hold = None
    global vbl_total
    vbl_total = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = R_TH.search(line)
        if m:
            th_pending = (float(m[1]), float(m[3]))
            continue
        m = R_WAIT.search(line)
        if m and 'nvn_perf: ms/frame' in line:
            wait_pending = float(m[3])
        m = R_FR.search(line)
        if m:
            f0 = int(m[1])
            w = win.setdefault(f0, {})
            w.update(avg=float(m[3]), p99=float(m[6]), mx=float(m[7]), fps=float(m[8]))
            if th_pending:
                w['cpu_all'], w['cpu_render'] = th_pending
                th_pending = None
            continue
        m = R_VK.search(line)
        if m:
            f0 = int(m[1])
            w = win.setdefault(f0, {})
            w.update(replay=float(m[3]), prep=float(m[5]), pipes=float(m[6]), mods=float(m[7]), gpu=float(m[8]), present=float(m[9]),
                     sets=float(m[11]), reused=float(m[12]))
            if wait_pending is not None:
                w['wait_other'] = wait_pending
                wait_pending = None
            last_hold = (int(m[13]), float(m[14]), float(m[15]))
            continue
        m = R_GPU.search(line)
        if m:
            win.setdefault(int(m[1]), {})['gpu_ms'] = float(m[3])
            continue
        m = R_VBL.search(line)
        if m and 'total' in line:
            vbl_total = tuple(int(x) for x in m.groups())
            continue
        m = R_DRAWS.search(line)
        if m:
            win.setdefault(int(m[1]), {})['draws'] = int(m[4])
            continue
        m = R_HOLD.search(line)
        if m:
            exit_hold = (int(m[1]), int(m[2]), float(m[3]), m[4])
    return win, last_hold, exit_hold


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--from', dest='lo', type=int, default=4200)
    ap.add_argument('--to', dest='hi', type=int, default=11100)
    ap.add_argument('--windows', action='store_true')
    ap.add_argument('--maxdraws', type=float, default=0, help='only windows with at most this many draws per frame (active play: ~520-600; cutscenes and replays 650-1350)')
    a = ap.parse_args()
    keys = ['fps', 'avg', 'p99', 'mx', 'cpu_render', 'cpu_all', 'replay', 'prep', 'sets', 'draws', 'wait_other', 'gpu_ms', 'present']
    print('%-28s %6s %7s %7s %7s %7s %7s %7s %6s %6s %7s  %s' % ('log', 'wins', 'fps', 'ms', 'p99', 'rendcpu', 'allcpu', 'replay', 'prep', 'sets', 'draws', 'holds (last window; exit)'))
    for path in a.logs:
        win, last_hold, exit_hold = parse(path)
        sel = [(f, w) for f, w in sorted(win.items()) if a.lo <= f < a.hi and 'avg' in w and (not a.maxdraws or w.get('draws', 1e9) <= a.maxdraws)]
        if a.windows:
            for f, w in sel:
                print('   %6d %s' % (f, ' '.join('%s=%s' % (k, w.get(k)) for k in keys)))
        if not sel:
            print('%-28s no windows' % path.split('/')[-1])
            continue
        avg = {}
        for k in keys:
            vals = [w[k] for _, w in sel if k in w]
            avg[k] = sum(vals) / len(vals) if vals else float('nan')
        ms = avg['avg']
        print('%-28s %6d %7.1f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %6.0f %6.0f  %s; %s' % (
            path.replace('\\', '/').split('/')[-1], len(sel), 1000.0 / ms, ms, avg['p99'], avg['cpu_render'], avg['cpu_all'], avg['replay'], avg['prep'],
            avg['sets'], avg['draws'], last_hold, exit_hold))
        mxs = [w['mx'] for _, w in sel if 'mx' in w]
        print('    worst window max frame %.2f ms, mean window max %.2f ms, mean present %.2f ms, GPU %s ms/frame; monitor (whole run) %s' % (
            max(mxs), sum(mxs) / len(mxs), avg['present'], 'n/a' if avg['gpu_ms'] != avg['gpu_ms'] else '%.2f' % avg['gpu_ms'], vbl_total))


if __name__ == '__main__':
    main()
