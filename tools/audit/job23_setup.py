"""Serial fresh real-input setup, preserving contaminated attempts for review."""
import json
import os
import subprocess
import time
import argparse
from pathlib import Path

from job21_setup import ROOT, BASE, UPDATE, KEYS, prepare, script, archive_attempt
from job21b_launch import process_snapshot, close_owned_window


def games(processes):
    return [p for p in processes if p['name'].lower().startswith(
        ('strikers', 'eden', 'yuzu', 'ryujinx'))]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',default='job23')
    parser.add_argument('--host-dir',type=Path,default=ROOT/'build/phase8/job23-release-host')
    args=parser.parse_args()
    if Path(args.prefix).name!=args.prefix:parser.error('Invalid run prefix')
    # Record metadata only, never key contents; originals remain read-only.
    originals = {str(p): (p.stat().st_size, p.stat().st_mtime_ns)
                 for p in (BASE, UPDATE, KEYS)}
    for attempt in range(1, 100):
        quiet = None
        while quiet is None or time.monotonic() - quiet < 120:
            others = games(process_snapshot())
            if others:
                quiet = None
            elif quiet is None:
                quiet = time.monotonic()
            print('Waiting for quiet PC: ' + json.dumps(others), flush=True)
            time.sleep(5)
        out = ROOT / 'build/phase8' / f'{args.prefix}-setup-{attempt:02} Ana’s 試合 لاعب'
        if out.exists():
            continue
        prepare(out, args.host_dir)
        test = script(out)
        started = time.monotonic()
        owned = set()
        contamination = []
        with (out / 'driver.log').open('wb') as output:
            child = subprocess.Popen(
                [str(out / 'StrikersSetup.exe'), '--self-test', str(test)],
                cwd=out, env=dict(os.environ, STRIKERS_OFFLINE='1'),
                stdout=output, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            owned.add(child.pid)
            with (out / 'process-observations.jsonl').open('w') as observations:
                previous = None
                while child.poll() is None:
                    processes = process_snapshot()
                    for _ in range(8):
                        owned.update(p['pid'] for p in processes if p['parent'] in owned)
                    active = games(processes)
                    if active != previous:
                        observations.write(json.dumps(dict(
                            seconds=round(time.monotonic() - started, 3), processes=active)) + '\n')
                        observations.flush()
                        previous = active
                    contamination = [p for p in active if p['pid'] not in owned]
                    if contamination:
                        for process in active:
                            if process['pid'] in owned:
                                close_owned_window(process['pid'])
                        close_owned_window(child.pid)
                        child.wait(60)
                        break
                    time.sleep(.5)
        unchanged = all((p.stat().st_size, p.stat().st_mtime_ns) == originals[str(p)]
                        for p in (BASE, UPDATE, KEYS))
        result = dict(exit=child.returncode, seconds=time.monotonic() - started,
                      valid_timing=not contamination, contamination=contamination,
                      original_metadata_unchanged=unchanged,
                      status='CONTAMINATED' if contamination else
                      'PASS' if child.returncode == 0 and unchanged else 'FAILED')
        (out / 'driver-result.json').write_text(json.dumps(result, indent=2))
        archive_attempt(out, test)
        print(json.dumps(dict(install=str(out), **result)), flush=True)
        if not contamination:
            (ROOT / f'build/phase8/{args.prefix}-setup-result.json').write_text(
                json.dumps(dict(install=str(out), **result), indent=2))
            return 0 if result['status'] == 'PASS' else 1
    raise RuntimeError('No uncontaminated setup attempt')


if __name__ == '__main__':
    raise SystemExit(main())
