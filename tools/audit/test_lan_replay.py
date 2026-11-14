"""Verify BOTH game logs, independent inputs, transport and reviewed captures.

python tools/audit/test_lan_replay.py build/run/<pair>.json --review <review.json>
Review JSON contains host/station records with score, movement_frames (2+),
results_frame, returned_frame and returned_to. This records visual inspection;
the checker does not pretend that log counts/OCR certify a match result.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
from test_menu_replay import assert_no_unimplemented_imports

ROOT = Path(__file__).resolve().parents[2]


def verify(manifest, review):
    rows = json.loads(manifest.read_text())
    assert (len(rows), frozenset(r['role'] for r in rows)) in (
        (2, frozenset(('host', 'station'))), (3, frozenset(('host', 'station', 'station2'))))
    reviewed = json.loads(review.read_text())
    group=all(r.get('group_member') is True for r in rows)
    def network(row):return row['group_network'] if group else row['env']['STRIKERS_ZEROTIER_NETWORK']
    identities = []; builds = []; ips = []; scores = []; result = {}; zt_nodes = []; zt_addresses = []
    for row in rows:
        role = row['role']; path = ROOT / row['log']; log = path.read_text(errors='replace')
        assert row['exit_code'] == 0 and not row.get('killed_after_limit'), f'{role}: process failed/was killed'
        assert not re.search(r'GUEST TRAP|HOST EXCEPTION|UNTRANSLATED INSTRUCTION|unhandled exception|ERROR: AddressSanitizer|^vk-validation:|VUID-', log, re.I | re.M), f'{role}: runtime diagnostics'
        assert '0 calls of operations that are not implemented' in log
        assert_no_unimplemented_imports(log)
        assert re.search(r'shutdown clean:.*0 Vulkan call failures, 0 validation messages', log)
        env = row['env']
        transport = env['STRIKERS_NETWORK']
        assert transport in ('lan', 'zerotier')
        if transport == 'lan':
            assert env['STRIKERS_LDN_INTERFACE'] == '127.0.0.1'
        assert env['STRIKERS_SDL_TEST_PLAYERS'] == '1' and env['STRIKERS_KEYBOARD_PLAYER'] == '0'
        assert 'TEST 1 virtual SDL gamepads' in log
        interface = r'127\.0\.0\.1' if transport == 'lan' else r'100\.64\.0\.0'
        banner = re.search(r'\[lan\] initialized interface=' + interface + r' .*?identity=([0-9a-f]+) build=([0-9a-f]+)', log)
        assert banner, f'{role}: selected transport absent'
        if transport == 'zerotier':
            zbanner = re.search(r'\[zerotier\] SLDN bound=(.+):\d+ node=([0-9a-f]+) network=' + re.escape(network(row)), log)
            assert zbanner
            zt_addresses.append(zbanner[1]); zt_nodes.append(zbanner[2])
            assert '[zerotier] state=2 Connected.' in log
            if group:
                assert 'STRIKERS_ZT_PEERS' not in env,'Group discovery must use the Worker directory'
                assert row['node']==zbanner[2]
            peers=[r['node'] for r in rows if r['node']!=row['node']] if group else env['STRIKERS_ZT_PEERS'].split(',')
            for peer in peers:
                if env.get('STRIKERS_ZT_BLOCK_DIRECT'):
                    assert f'peer={peer} path=relayed' in log, f'{role}: relay status absent for {peer}'
                    assert f'peer={peer} path=direct' not in log, f'{role}: direct path bypassed test policy'
                    assert '[zt-path-sim] rejected direct path' in log
                else:
                    assert f'peer={peer} path=direct' in log, f'{role}: direct path not observed for {peer}'
        identities.append(banner[1]); builds.append(banner[2])
        tx = [(int(n), int(src,16), int(dst,16), int(size)) for n, src, dst, size in re.findall(
            r'\[lan\] UDP tx=(\d+) src=([0-9a-f]+):\d+ dst=([0-9a-f]+):\d+ bytes=(\d+)', log)]
        assert tx and max(n for n, *_ in tx) >= 8, f'{role}: no Pia datagram stream'
        ips.append(tx[0][1]); assert all(src>>16==0xa9fe and (dst>>16==0xa9fe or dst==0xffffffff) for _, src, dst, _ in tx)
        if role=='host':
            assert 'room-created' in log and 'node-joined id=1' in log
        else:
            assert re.search(r'scan .*found=1', log) and 'connect result=joined' in log
        steps=[s.strip() for s in row['replay'].splitlines() if s.strip() and not s.lstrip().startswith('#')]
        blocks={i+1 for i,s in enumerate(steps) if s=='wait open:StrkrsSave:/strkrs.save'}
        wait=next((m for m in re.finditer(r'^input wait: satisfied at frame (\d+); block (\d+) begins',log,re.M) if int(m[2]) in blocks),None)
        assert wait and int(wait[1]) > 11000, f'{role}: no match-end save fence'
        rec = ROOT / env['STRIKERS_REC']
        # Environment is relative to build/bin, just as the runtime resolves it.
        if not rec.exists():
            rec = (ROOT / 'build/bin' / env['STRIKERS_REC']).resolve()
        recorded = rec.read_text()
        direction = 32767 if role=='host' else -32767
        assert any(int(m[1])==direction for m in re.finditer(r'^a \d+ \d+ [0-9a-f]+ (-?\d+) -?\d+$', recorded, re.M)), f'{role}: independent stick input missing'
        cap = {r['frame']:r for r in json.loads(path.with_name(path.stem+'-captures.json').read_text())}
        visual = reviewed[role]
        frames = [*visual['movement_frames'], visual['results_frame'], visual['returned_frame']]
        assert len(visual['movement_frames']) >= 2 and visual['results_frame'] > int(wait[1])
        assert visual['returned_frame'] > visual['results_frame'] and visual['returned_to'] in ('main menu', 'Quick Battle menu', 'Local Wireless room list', 'matchmaking')
        for frame in frames:
            capture = cap[frame]; png = ROOT / capture['png']
            assert hashlib.sha256(png.read_bytes()).hexdigest() == capture['png_sha256'] and not capture['spatially_uniform']
        scores.append(visual['score'])
        result[role] = {'log':row['log'], 'identity':banner[1], 'build':banner[2], 'virtual_ip':hex(ips[-1]),
            'match_end_save_frame':int(wait[1]), 'reviewed_frames':frames, 'score':visual['score'], 'returned_to':visual['returned_to']}
        if transport == 'zerotier':
            result[role].update(transport=transport, zerotier_node=zbanner[2], zerotier_address=zbanner[1],
                               network=network(row), path='relayed' if env.get('STRIKERS_ZT_BLOCK_DIRECT') else 'direct')
    assert len(set(identities)) == len(rows) and len(set(ips)) == len(rows) and len(set(builds)) == 1
    assert len(set(scores)) == 1, 'instances disagree about the reviewed final score'
    assert len({r['env']['STRIKERS_USERDATA'] for r in rows}) == len(rows)
    assert len({r['env']['STRIKERS_CONFIG'] for r in rows}) == len(rows)
    assert len({r['env']['STRIKERS_NETWORK'] for r in rows}) == 1
    if zt_nodes:
        assert len(set(zt_nodes)) == len(rows) and len(set(zt_addresses)) == len(rows)
        assert len({network(r) for r in rows}) == 1
        for row, node in zip(rows, zt_nodes):
            if not group:assert set(row['env']['STRIKERS_ZT_PEERS'].split(',')) == set(zt_nodes) - {node}
    return {'status':'PASS', 'instances':result}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('manifest', type=Path)
    ap.add_argument('--review', required=True, type=Path)
    args = ap.parse_args()
    result = verify(args.manifest, args.review)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
