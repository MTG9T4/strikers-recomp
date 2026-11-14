"""Strict full-match acceptance for run_online_pair; probe success cannot pass.

Requires authenticated server/session/relay evidence AND visually reviewed
captures from both games. Logs/primitive tests alone never certify a match.
The current server is a probe and intentionally cannot satisfy this checker.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def verify(path, review_path):
    manifest = json.loads(path.read_text())
    rows = manifest['clients']
    assert len(rows) in (2, 4), 'requires two or four game clients'
    assert len({r['principal'] for r in rows}) == len(rows), 'accounts are not distinct'
    for key in ('STRIKERS_USERDATA', 'STRIKERS_CONFIG', 'STRIKERS_REC', 'STRIKERS_ONLINE_TOKEN_FILE'):
        assert len({r['env'][key] for r in rows}) == len(rows), 'shared client state: ' + key
    events = []
    for server in manifest['servers']:
        events += [json.loads(line) for line in (ROOT / server['log']).read_text().splitlines()]
    # Future server integration must emit these events only AFTER verified
    # account login and authoritative membership/forwarding, with no tokens.
    authenticated = {e['principal'] for e in events if e['event']=='authenticated'}
    assert {r['principal'] for r in rows} <= authenticated, 'no verified server authentication for both game accounts'
    assert any(e['event']=='pia-forward' and e.get('bytes', 0)>0 for e in events), 'no Pia relay data evidence'
    assert review_path and review_path.is_file(), 'actual visual review is required'
    reviewed = json.loads(review_path.read_text());scores = [];result = {}
    for index, row in enumerate(rows):
        role, env = row['role'], row['env']
        assert row.get('exit_code') == 0 and not row.get('killed_after_limit'), role + ': failed or killed process'
        log_path = ROOT / row['log'];log = log_path.read_text(errors='replace')
        assert env['STRIKERS_NETWORK']=='online' and env['STRIKERS_ONLINE_SERVER']=='127.0.0.1'
        assert not re.search(r'GUEST TRAP|HOST EXCEPTION|UNTRANSLATED INSTRUCTION|ERROR: AddressSanitizer|^vk-validation:|VUID-', log, re.I|re.M), role + ': runtime diagnostics'
        assert '[online-tls] verified private-CA' in log, role + ': no game TLS verification'
        assert 'TEST 1 virtual SDL gamepads' in log and env['STRIKERS_KEYBOARD_PLAYER']=='0'
        wait = re.search(r'^input wait: satisfied at frame (\d+);', log, re.M)
        assert wait and int(wait[1])>11000, role + ': no late match-end save fence'
        rec = Path(env['STRIKERS_REC']);recorded = rec.read_text()
        direction = 32767 if index%2==0 else -32767
        assert any(int(m[1])==direction for m in re.finditer(r'^a \d+ \d+ [0-9a-f]+ (-?\d+) -?\d+$', recorded, re.M)), role + ': no independent stick input'
        captures = {v['frame']: v for v in json.loads(log_path.with_name(role+'-captures.json').read_text())}
        v = reviewed[role]
        assert len(v['movement_frames'])>=2 and v['results_frame']>int(wait[1])
        assert v['returned_frame']>v['results_frame'] and v['returned_to'] in ('main menu', 'Quick Battle menu', 'matchmaking')
        frames = [*v['movement_frames'], v['results_frame'], v['returned_frame']]
        for f in frames:
            c = captures[f];png = ROOT / c['png']
            assert hashlib.sha256(png.read_bytes()).hexdigest()==c['png_sha256'] and not c['spatially_uniform']
        scores.append(v['score']);result[role] = {'principal': row['principal'], 'frames': frames, 'score': v['score']}
    for i in range(0, len(scores), 2):
        assert scores[i]==scores[i+1], 'paired clients disagree about the final score'
    return {'status': 'PASS', 'instances': result}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('manifest', type=Path)
    ap.add_argument('--review', type=Path)
    ap.add_argument('--out', type=Path)
    a = ap.parse_args()
    try:
        result = verify(a.manifest, a.review)
    except (AssertionError, KeyError, OSError, ValueError) as e:
        result = {'status': 'FAIL', 'reason': str(e)}
    if a.out:
        a.out.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return result['status'] != 'PASS'


if __name__ == '__main__':
    raise SystemExit(main())
