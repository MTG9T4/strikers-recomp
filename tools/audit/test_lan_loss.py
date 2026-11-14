"""Check both logs of an owned-child, mid-match process-loss test.

Requires a manual capture review alongside the logs; log counters alone cannot
prove that the fault happened during a match or identify the game's response.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def verify(manifest, review):
    rows = json.loads(manifest.read_text())
    visual = json.loads(review.read_text())
    assert len(rows) == 2 and {r['role'] for r in rows} == {'host', 'station'}
    victim = visual['victim_role']
    assert victim in ('host', 'station')
    result = {}
    for row in rows:
        role = row['role']
        path = ROOT / row['log']
        log = path.read_text(errors='replace')
        assert not re.search(r'GUEST TRAP|HOST EXCEPTION|UNTRANSLATED INSTRUCTION|unhandled exception|^vk-validation:|VUID-', log, re.I | re.M)
        assert row['env']['STRIKERS_LDN_INTERFACE'] == '127.0.0.1'
        assert row['env']['STRIKERS_SDL_TEST_PLAYERS'] == '1'
        assert re.search(r'\[lan\] UDP tx=\d+ src=a9fe', log), f'{role}: no Pia traffic'
        captures = {r['frame']: r for r in json.loads(path.with_name(path.stem + '-captures.json').read_text())}

        def capture(frame):
            cap = captures[frame]
            assert not cap['spatially_uniform']
            assert hashlib.sha256((ROOT / cap['png']).read_bytes()).hexdigest() == cap['png_sha256']

        match_frame = visual['match_frames'][role]
        capture(match_frame)
        assert not row.get('killed_after_limit'), f'{role}: test deadline exceeded'
        if role == victim:
            fault = row['intentional_mid_match_termination']['at_capture_frame']
            assert row['exit_code'] != 0 and fault >= match_frame
            result[role] = {'intentional_kill_after_frame': fault, 'reviewed_match_frame': match_frame}
        else:
            assert row['exit_code'] == 0 and not row.get('intentional_mid_match_termination')
            assert '0 calls of operations that are not implemented' in log
            assert 'unimplemented imports reached:\n\n' in log
            assert re.search(r'shutdown clean:.*0 Vulkan call failures, 0 validation messages', log)
            response = visual['survivor']
            assert response['response_frame'] > match_frame
            assert response['progress_frame'] > match_frame + 1800
            assert response['response'] in ('forfeit victory', 'disconnect message and menu')
            capture(response['response_frame']); capture(response['progress_frame'])
            if victim == 'station':
                assert 'node-left id=1' in log
            else:
                assert '[lan] state 5 -> 4 reason=6' in log
            if response['response'] == 'disconnect message and menu':
                assert 'nn::err: Error Code:' in log and '(scripted viewer acknowledged)' in log
            result[role] = {'exit_code': 0, 'reviewed_match_frame': match_frame, **response}
    return {'status': 'PASS', 'victim_role': victim, 'instances': result}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('manifest', type=Path)
    ap.add_argument('--review', required=True, type=Path)
    args = ap.parse_args()
    print(json.dumps(verify(args.manifest, args.review), indent=2))


if __name__ == '__main__':
    main()
