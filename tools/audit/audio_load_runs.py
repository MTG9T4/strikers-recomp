"""Audit 03: A/B audio runs under controlled CPU load, started only in clean windows.

Each run plays the Training match (job09_training_match.seq) with boot_probe.py. Before a run starts, no other
Strikers game (any process named strikers*.exe that is not this worktree's build, see --mine) may have run for
--quiet seconds; while it runs, other games are sampled every 2 s and their count is written to the run's record,
so a contaminated run can be told apart and repeated. --load N starts N busy-loop processes at normal priority for the
whole run (CPU load generated here, never by another game); they end by themselves and are killed after the run.

python -I tools/audit/audio_load_runs.py --queue build/run/a03-queue.json run-name:exe:load[:ENV=VAL,...] ...

Audit 04: --instances N plays N copies of the run at once (logs <run-name>-i1 .. -iN, each with its own userdata), the
several-games case of the LAN test; they are this runner's own games (--mine), never counted as other games.
"""
import argparse
import csv
import io
import json
import multiprocessing as mp
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def spin(seconds):
    end = time.time() + seconds
    x = 0
    while time.time() < end:
        for _ in range(100000):
            x += 1


def other_games(mine):
    out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    names = [row[0] for row in csv.reader(io.StringIO(out)) if row]
    return [n for n in names if n.lower().startswith('strikers') and not n.lower().startswith(mine)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--queue', type=Path, required=True, help='JSON record of every run (inside build/run)')
    p.add_argument('--seconds', type=int, default=560)
    p.add_argument('--quiet', type=int, default=15)
    p.add_argument('--max-wait', type=int, default=1500)
    p.add_argument('--mine', default='strikers_a03')
    p.add_argument('--seq', default='../../tools/audit/job09_training_match.seq')
    p.add_argument('--instances', type=int, default=1, help='games of the same run played at once (audit 04)')
    p.add_argument('runs', nargs='+')
    a = p.parse_args()
    queue = a.queue.resolve()
    queue.relative_to(ROOT / 'build/run')
    records = json.loads(queue.read_text()) if queue.exists() else []
    run_dir = (ROOT / 'build/run').as_posix()
    for spec in a.runs:
        parts = spec.split(':', 3)
        name, exe, load = parts[0], parts[1], int(parts[2])
        extra = [e for e in parts[3].split(',') if e] if len(parts) > 3 else []
        names = [name] if a.instances == 1 else [f'{name}-i{i + 1}' for i in range(a.instances)]
        for n in names:
            assert not (ROOT / 'build/run' / (n + '.log')).exists(), n
        waited, quiet_since, t0 = 0, None, time.time()
        while True:
            busy = other_games(a.mine)
            now = time.time()
            if busy:
                quiet_since = None
            elif quiet_since is None:
                quiet_since = now
            if quiet_since is not None and now - quiet_since >= a.quiet:
                break
            if now - t0 > a.max_wait:
                break
            time.sleep(2)
        waited = round(time.time() - t0)
        hogs = [mp.Process(target=spin, args=(a.seconds + 40,)) for _ in range(load)]
        for h in hogs:
            h.start()
        start = time.time()
        children = []
        for n in names:
            env = [f'STRIKERS_VOLUME=0', 'STRIKERS_AUDIO_STALL=35', f'STRIKERS_USERDATA={run_dir}/{n}-userdata', f'STRIKERS_SEQ={a.seq}'] + extra
            cmd = [sys.executable, '-I', str(ROOT / 'tools/audit/boot_probe.py'), 'run', n, '--exe', exe, '--normal', '--seconds', str(a.seconds)]
            for e in env:
                cmd += ['--env', e]
            children.append(subprocess.Popen(cmd, cwd=ROOT))
        samples = []
        while any(ch.poll() is None for ch in children):
            samples.append(len(other_games(a.mine)))
            time.sleep(2)
        for h in hogs:
            if h.is_alive():
                h.kill()
            h.join()
        records.append(dict(name=name, exe=exe, load=load, instances=a.instances, logs=names, env=extra, waited_s=waited, started=time.strftime('%H:%M:%S', time.localtime(start)),
                            seconds=round(time.time() - start), exit=[ch.returncode for ch in children] if a.instances > 1 else children[0].returncode,
                            other_game_samples=len(samples), samples_with_other_games=sum(1 for s in samples if s), max_other_games=max(samples, default=0)))
        queue.write_text(json.dumps(records, indent=2) + '\n')
        print(json.dumps(records[-1]), flush=True)


if __name__ == '__main__':
    main()
