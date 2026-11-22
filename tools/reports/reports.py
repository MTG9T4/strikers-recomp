"""Read the problem reports players chose to send (cloudflare/reports). Standard library only.

  python -I tools/reports/reports.py list              newest last: time, id, version, kind, GPU, CPU, description
  python -I tools/reports/reports.py fetch             download reports not yet downloaded into build/reports/
  python -I tools/reports/reports.py summary           group the downloaded reports and pull the known signs out of their logs
  python -I tools/reports/reports.py delete <key>      delete one report from the service

The service's address comes from STRIKERS_REPORTS_URL (or %APPDATA%\\strikers-reports\\url) and the admin token from
STRIKERS_REPORTS_TOKEN (or %APPDATA%\\strikers-reports\\token). Reports hold players' logs: they stay in build/reports
(git-ignored) and are deleted from the service after 90 days."""
import collections
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
STORE = ROOT / 'build' / 'reports'
CONFIG = Path(os.environ.get('APPDATA', str(Path.home()))) / 'strikers-reports'


def setting(env, name):
    value = os.environ.get(env)
    if not value and (CONFIG / name).exists():
        value = (CONFIG / name).read_text(encoding='utf-8').strip()
    if not value:
        sys.exit(f'Set {env} or write {CONFIG / name}')
    return value


def call(path, method='GET'):
    url = setting('STRIKERS_REPORTS_URL', 'url').rstrip('/') + path
    auth = 'Authorization: Bearer ' + setting('STRIKERS_REPORTS_TOKEN', 'token')
    curl = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'curl.exe'
    if curl.exists():
        # Windows' own TLS (Schannel), as the game's WinHTTP: Python's OpenSSL can pick an expired cross-signed root from the
        # Windows store and refuse a valid certificate. The token goes in on stdin, never on the command line.
        r = subprocess.run([str(curl), '-sS', '--fail-with-body', '--max-time', '60', '-X', method, '-H', '@-', '-H',
                            'User-Agent: strikers-reports/1', url], input=auth.encode() + b'\n', capture_output=True)
        if r.returncode:
            sys.exit(f'{method} {path}: {(r.stdout or r.stderr).decode(errors="replace").strip()[:300]}')
        return r.stdout
    req = urllib.request.Request(url, method=method, headers={'Authorization': auth.split(': ', 1)[1], 'User-Agent': 'strikers-reports/1'})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def all_reports():
    out, cursor = [], ''
    while True:
        page = json.loads(call('/v1/admin/reports' + (f'?cursor={urllib.parse.quote(cursor)}' if cursor else '')))
        out += page['reports']
        cursor = page.get('cursor')
        if not cursor:
            return out


def file_name(key):
    return re.sub(r'[^0-9A-Za-z_.-]', '-', key) + '.zip'


def cmd_list():
    reports = all_reports()
    for r in reports:
        print(f"{r.get('at', '')[:16].replace('T', ' ')}  {r.get('id', ''):9} {r.get('version', ''):8} {r.get('kind', ''):5} "
              f"{r.get('gpu', '')[:28]:28} {r.get('cpu', '')[:26]:26} {r.get('description', '')[:60]}")
    print(f'{len(reports)} reports')


def cmd_fetch():
    STORE.mkdir(parents=True, exist_ok=True)
    new = 0
    for r in all_reports():
        target = STORE / file_name(r['key'])
        if target.exists():
            continue
        target.write_bytes(call('/v1/admin/report?key=' + urllib.parse.quote(r['key'])))
        (target.with_suffix('.json')).write_text(json.dumps(r, indent=2), encoding='utf-8')
        new += 1
    print(f'{new} new reports in {STORE}')


SIGNS = [
    ('crash', re.compile(rb'Exception 0x([0-9a-fA-F]+)')),
    ('audio gaps', re.compile(rb'underruns \(device ran dry while started, after the first sound\): (\d+), ([\d.]+) ms')),
    ('missing NVN called', re.compile(rb'nvn: MISSING at call[^\n]*')),
    ('device lost', re.compile(rb'VK_ERROR_DEVICE_LOST|device lost', re.I)),
]
FPS = re.compile(rb'nvn_perf: frames \d+\.\.\d+ frame time ms:[^|]*\| fps avg ([\d.]+)')


def signs(z):
    found = {}
    for name in z.namelist():
        if not name.endswith(('.log', '.txt')):
            continue
        data = z.read(name)
        for label, pattern in SIGNS:
            m = pattern.search(data)
            if m and label not in found:
                found[label] = m.group(0).decode(errors='replace')[:120]
        if name.endswith('last-game.log'):
            fps = [float(x) for x in FPS.findall(data)]
            if fps:
                found['fps'] = f'median {sorted(fps)[len(fps) // 2]:.1f}, lowest {min(fps):.1f} over {len(fps)} windows'
    return found


def cmd_summary():
    files = sorted(STORE.glob('*.zip'))
    if not files:
        sys.exit(f'No reports in {STORE}: run fetch first')
    by = {k: collections.Counter() for k in ('version', 'gpu vendor', 'kind', 'sign')}
    for f in files:
        meta = json.loads(f.with_suffix('.json').read_text(encoding='utf-8')) if f.with_suffix('.json').exists() else {}
        gpu = meta.get('gpu', '')
        vendor = next((v for v in ('NVIDIA', 'AMD', 'Intel') if v.lower() in gpu.lower()), gpu.split(' ')[0] if gpu else '(unknown)')
        by['version'][meta.get('version', '?')] += 1
        by['gpu vendor'][vendor] += 1
        by['kind'][meta.get('kind', '?')] += 1
        try:
            found = signs(zipfile.ZipFile(io.BytesIO(f.read_bytes())))
        except zipfile.BadZipFile:
            found = {'unreadable zip': f.name}
        for label in found:
            by['sign'][label] += 1
        print(f"{meta.get('id', f.stem):9} {meta.get('version', ''):8} {meta.get('kind', ''):5} {gpu[:30]:30} {meta.get('description', '')[:50]}")
        for label, text in found.items():
            print(f'            {label}: {text}')
    print()
    for k, counter in by.items():
        print(f'{k}: ' + ', '.join(f'{name} {n}' for name, n in counter.most_common()))


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('list', 'fetch', 'summary', 'delete'):
        sys.exit(__doc__)
    if sys.argv[1] == 'delete':
        if len(sys.argv) != 3:
            sys.exit('delete <key>: the key is in the .json beside a fetched zip (for example r/2026-11-21T19:00:00.000Z_R-7K2Q9F)')
        print(call('/v1/admin/report?key=' + urllib.parse.quote(sys.argv[2]), 'DELETE').decode())
    else:
        {'list': cmd_list, 'fetch': cmd_fetch, 'summary': cmd_summary}[sys.argv[1]]()


if __name__ == '__main__':
    main()
