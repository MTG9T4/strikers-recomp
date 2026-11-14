"""Start two isolated game instances on loopback; never touches inherited processes.

python tools/audit/run_lan_pair.py --name job15-pair01 --seconds 700
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import shutil
import re
import hashlib
import sys
from build_runtime import ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name', required=True)
    ap.add_argument('--seconds', type=int, default=700)
    ap.add_argument('--exe', default='strikers_job15_lan.exe')
    ap.add_argument('--port', type=int, default=24872)
    ap.add_argument('--seed-save', type=Path, required=True, help='intro-complete test save; copied into separate userdata')
    ap.add_argument('--host-seq', type=Path, default=ROOT / 'tools/audit/job15_lan_host.seq')
    ap.add_argument('--station-seq', type=Path, default=ROOT / 'tools/audit/job15_lan_station.seq')
    ap.add_argument('--terminate-role', choices=('host', 'station'), help='fault test: kill only this run\'s selected child')
    ap.add_argument('--terminate-after-frame', type=int, default=8371)
    ap.add_argument('--station-build', help='mismatch test only: override station protocol build id')
    ap.add_argument('--network', choices=('lan', 'zerotier'), default='lan')
    ap.add_argument('--zerotier-network', default='ff61286137000000')
    ap.add_argument('--net-sim', help='one-way delay_ms,jitter_ms,loss_pct')
    ap.add_argument('--scale', type=float, default=1, help='test-only internal resolution scale')
    ap.add_argument('--block-direct', action='store_true')
    ap.add_argument('--instances', type=int, choices=(2, 3), default=2)
    ap.add_argument('--third-seq', type=Path, default=ROOT / 'tools/audit/job18_lan_third.seq')
    args = ap.parse_args()
    assert Path(args.name).name == args.name and Path(args.exe).name == args.exe
    executable = ROOT / 'build/bin' / args.exe
    executable_sha256 = hashlib.sha256(executable.read_bytes()).hexdigest()
    dependency_sha256 = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in (ROOT / 'build/bin').glob('*.dll')
                         if p.name in ('strikers_game.dll', 'strikers_zt.dll', 'libopus-0.dll', 'SDL3.dll')}
    records = []; processes = []; handles = []
    zt_keys = []
    if args.network == 'zerotier':
        import ctypes
        lib = ctypes.CDLL(str(ROOT / 'build/bin/strikers_zt.dll'))
        for i in range(args.instances):
            key = ctypes.create_string_buffer(384); length = ctypes.c_uint(384)
            assert lib.zts_id_new(key, ctypes.byref(length)) == 0
            zt_keys.append(key.value)
    try:
        for i, role in enumerate(('host', 'station', 'station2')[:args.instances]):
            prefix = ROOT / 'build/run' / (args.name + '-' + role)
            log = prefix.with_suffix('.log')
            assert not log.exists(), 'use an unused run name'
            userdata = Path(prefix.as_posix() + '-userdata')
            assert not userdata.exists()
            (userdata / 'save').mkdir(parents=True)
            shutil.copyfile(args.seed_save, userdata / 'save/strkrs.save')
            if zt_keys:
                (userdata / 'zerotier').mkdir()
                (userdata / 'zerotier/identity.secret').write_bytes(zt_keys[i])
            env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')}
            seq = args.host_seq if role=='host' else args.station_seq if role=='station' else args.third_seq
            env.update(STRIKERS_NETWORK='lan', STRIKERS_LDN_INTERFACE='127.0.0.1',
                STRIKERS_LDN_TRACE='1', STRIKERS_IMPORT_COUNTS='1',
                STRIKERS_TRACE=f'../run/{args.name}-{role}-trace.txt',
                STRIKERS_REC=f'../run/{args.name}-{role}.rec',
                STRIKERS_HIDLOG='1',
                STRIKERS_LDN_PORT=str(args.port), STRIKERS_LDN_ID=f'{0x1500+i+1:x}',
                STRIKERS_VOLUME='0', STRIKERS_KEYBOARD_PLAYER='0', STRIKERS_SDL_TEST_PLAYERS='1',
                STRIKERS_SDL_TEST_SEQ=seq.resolve().as_posix(),
                STRIKERS_CONFIG=prefix.with_suffix('.ini').as_posix(),
                STRIKERS_USERDATA=prefix.as_posix() + '-userdata',
                STRIKERS_ROMFS=(ROOT / 'build/dump/update/romfs').as_posix())
            if role=='station' and args.station_build:
                env['STRIKERS_LDN_BUILD'] = args.station_build
            if args.net_sim:
                env['STRIKERS_NET_SIM'] = args.net_sim
            env['STRIKERS_VK_SCALE'] = str(args.scale)
            if zt_keys:
                env.update(STRIKERS_NETWORK='zerotier', STRIKERS_ZEROTIER_NETWORK=args.zerotier_network,
                           STRIKERS_ZT_PEERS=','.join(k.decode().split(':')[0] for j,k in enumerate(zt_keys) if j!=i))
                if args.block_direct:
                    env['STRIKERS_ZT_BLOCK_DIRECT'] = env['STRIKERS_ZT_PEERS']
            handle = log.open('wb'); handles.append(handle)
            p = subprocess.Popen([str(executable),
                str(ROOT / 'build/dump/update/elf/main.elf'), str(args.seconds)],
                cwd=ROOT / 'build/bin', env=env, stdout=handle, stderr=subprocess.STDOUT)
            processes.append(p)
            records.append({'role': role, 'pid': p.pid, 'log': str(log.relative_to(ROOT)), 'env': env,
                'executable': str(executable.relative_to(ROOT)), 'executable_sha256': executable_sha256,
                'dependency_sha256': dependency_sha256,
                'replay': seq.read_text()})
            print(f'{role}: pid {p.pid}; {log.relative_to(ROOT)}', flush=True)
        deadline = time.monotonic() + args.seconds + 45
        while any(p.poll() is None for p in processes) and time.monotonic() < deadline:
            # These standard full-match scripts finish with a capture at +2460
            # after the game's save fence. Close only when every live participant
            # has completed it, retaining real captures for independent review.
            if not args.terminate_role and args.host_seq.name in ('job15_lan_host.seq','job18_lan_host.seq') and args.station_seq.name in ('job15_lan_station.seq','job18_lan_station.seq'):
                finished = []
                for record, child in zip(records, processes):
                    text = (ROOT / record['log']).read_text(errors='replace')
                    fence = re.search(r'^input wait: satisfied at frame (\d+);', text, re.M)
                    complete = len(re.findall(r'^capture: \d+x\d+ picture saved to ', text, re.M))
                    frames = [int(v) for v in re.findall(r'^capture-frame: (\d+)$', text, re.M)][:complete]
                    finished.append(child.poll() is not None or bool(fence and frames and max(frames) >= int(fence[1]) + 2460))
                if all(finished):
                    for record, child in zip(records, processes):
                        if child.poll() is None and not record.get('closed_after_replay'):
                            close = subprocess.run([sys.executable, str(ROOT / 'tools/audit/pc_window_probe.py'), str(child.pid), 'close'],
                                                   capture_output=True, timeout=10)
                            if close.returncode == 0 or child.poll() is not None:
                                record['closed_after_replay'] = True
            if args.terminate_role:
                index = 0 if args.terminate_role=='host' else 1
                record, victim = records[index], processes[index]
                if victim.poll() is None and not record.get('intentional_mid_match_termination'):
                    log = (ROOT / record['log']).read_text(errors='replace')
                    # Wait for the completed-save line as well as the marker:
                    # killing while the BMP is being written would ruin evidence.
                    complete = len(re.findall(r'^capture: \d+x\d+ picture saved to ', log, re.M))
                    frames = [int(v) for v in re.findall(r'^capture-frame: (\d+)$', log, re.M)][:complete]
                    if frames and max(frames) >= args.terminate_after_frame:
                        record['intentional_mid_match_termination'] = {'at_capture_frame': max(frames)}
                        victim.kill()
                        print(f"fault test: killed only this run's {args.terminate_role} pid {victim.pid} after capture {max(frames)}", flush=True)
            time.sleep(1)
    finally:
        for record, p in zip(records, processes):
            if p.poll() is None:
                p.kill(); record['killed_after_limit'] = True
            p.wait(); record['exit_code'] = p.returncode
        for handle in handles:
            handle.close()
        for record in records:
            record['env'] = {k: v for k, v in record['env'].items() if k.startswith('STRIKERS_')}
        (ROOT / 'build/run' / (args.name + '.json')).write_text(json.dumps(records, indent=2) + '\n')
    print([(r['role'], r['exit_code']) for r in records])


if __name__ == '__main__':
    main()
