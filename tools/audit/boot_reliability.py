"""Five sequential 120-second normal boots; retain exact artifact hashes and results.

No diagnostic STRIKERS/ASan overrides. RomFS and userdata are ordinary launch inputs.
Run logs and the JSON ledger are preserved in build/run; existing evidence is never overwritten.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name')
    args = parser.parse_args()
    paths = [ROOT / 'build/run' / f'{args.name}-{i}.log' for i in range(1, 6)]
    ledger = ROOT / 'build/run' / f'{args.name}.json'
    if ledger.exists() or any(p.exists() for p in paths):
        raise SystemExit('choose a new name to preserve prior evidence')
    env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')
           and not k.startswith('ASAN_') and not k.startswith('LSAN_')}
    env.update(STRIKERS_ROMFS=str(ROOT / 'build/dump/update/romfs'),
               STRIKERS_USERDATA=str(ROOT / 'build/run/userdata'))
    artifacts = {name: hashlib.sha256((ROOT / 'build/bin' / name).read_bytes()).hexdigest()
                 for name in ('strikers_native.exe', 'strikers_game.dll', 'strikers_glslang.dll')}
    report = {'seconds_requested': 120, 'environment': {k: v for k, v in env.items() if k.startswith('STRIKERS_')},
              'artifacts_sha256': artifacts, 'runs': []}
    ledger.write_text(json.dumps(report, indent=2) + '\n')
    for i, path in enumerate(paths, 1):
        start = time.monotonic()
        killed = False
        with path.open('xb') as log:
            p = subprocess.Popen([str(ROOT / 'build/bin/strikers_native.exe'),
                                  str(ROOT / 'build/dump/update/elf/main.elf'), '120'],
                                 cwd=ROOT / 'build/bin', env=env, stdout=log, stderr=subprocess.STDOUT)
            print(f'run {i}: pid {p.pid}, {path.name}', flush=True)
            try:
                p.wait(timeout=150)
            except subprocess.TimeoutExpired:
                killed = True
                p.kill()
                p.wait()
        text = path.read_text(errors='replace')
        frames = re.findall(r'display: (\d+) frames presented', text)
        fatal = re.findall(r'^.*(?:HOST EXCEPTION|GUEST TRAP|AddressSanitizer:|UNTRANSLATED INSTRUCTION).*$', text, re.M)
        row = {'run': i, 'log': str(path.relative_to(ROOT)), 'exit_code': p.returncode,
               'elapsed_seconds': round(time.monotonic() - start, 3), 'killed': killed,
               'reached_time_limit': 'still running after 120 s' in text,
               'frames': int(frames[-1]) if frames else 0, 'fatal': fatal}
        row['passed'] = (row['exit_code'] == 0 and row['elapsed_seconds'] >= 120 and
                         row['reached_time_limit'] and row['frames'] > 0 and not fatal and not killed)
        report['runs'].append(row)
        ledger.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(row), flush=True)
        if not row['passed']:
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
