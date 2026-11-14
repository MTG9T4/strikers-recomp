"""Sequential distance-match runs, isolated userdata; no human playability claim.

Review captures and run test_lan_replay separately for each completed match.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from build_runtime import ROOT


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix', required=True)
    p.add_argument('--exe', default='strikers_job18_zt.exe')
    p.add_argument('--seed-save', required=True)
    p.add_argument('--seconds', type=int, default=950)
    p.add_argument('--cases', default='100,200,200loss,250')
    a = p.parse_args()
    cases = {'100': '100,0,0', '200': '200,0,0', '200loss': '200,0,2', '250': '250,0,0'}
    results = []
    for case in a.cases.split(','):
        name = a.prefix + '-' + case
        cmd = [sys.executable, str(ROOT / 'tools/audit/run_lan_pair.py'), '--name', name,
               '--exe', a.exe, '--seconds', str(a.seconds), '--scale', '0.5',
               '--seed-save', a.seed_save, '--net-sim', cases[case]]
        cmd += ['--host-seq', str(ROOT / 'tools/audit/job18_lan_host.seq'),
                '--station-seq', str(ROOT / 'tools/audit/job18_lan_station.seq')]
        with (ROOT / f'build/run/{name}-runner.txt').open('w') as log:
            r = subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        rows = json.loads((ROOT / f'build/run/{name}.json').read_text())
        outcome = {'case': case, 'runner_exit': r.returncode, 'players': []}
        for row in rows:
            text = (ROOT / row['log']).read_text(errors='replace')
            outcome['players'].append({'role': row['role'], 'exit': row['exit_code'],
                'joined': 'connect result=joined' in text or 'node-joined' in text,
                'save_fence': 'input wait: satisfied' in text,
                'signal_lost': 'reason=6' in text,
                'error_viewer': 'nn::err' in text})
        results.append(outcome)
        print(json.dumps(outcome), flush=True)
        (ROOT / f'build/run/{a.prefix}-summary.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
