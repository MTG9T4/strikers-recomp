"""Run isolated online clients and the local protocol probe; loopback only.

The default replay reaches Online Play. It is NOT a completed-match replay.
Acceptance requires test_online_replay.py and actual reviewed results captures.
Only processes created by this invocation can be stopped or restarted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from server.account_stub import LocalAccountProvider
from server.nex.__main__ import load_config


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name', required=True)
    ap.add_argument('--exe', default='strikers_job17_online.exe')
    ap.add_argument('--seconds', type=int, default=180)
    ap.add_argument('--seed-save', type=Path, required=True)
    ap.add_argument('--pki', type=Path, default=ROOT / 'build/online/pki02')
    ap.add_argument('--server-config', type=Path, default=ROOT / 'server/nex/config.local.json')
    ap.add_argument('--client1-seq', type=Path, default=ROOT / 'tools/audit/job17_online_probe.seq')
    ap.add_argument('--client2-seq', type=Path, default=ROOT / 'tools/audit/job17_online_probe.seq')
    ap.add_argument('--pairs', type=int, choices=(1, 2), default=1)
    ap.add_argument('--no-server', action='store_true')
    ap.add_argument('--restart-after', type=float, help='restart only the owned server after this many seconds')
    ap.add_argument('--quit-client', type=int, choices=(1, 2, 3, 4))
    ap.add_argument('--quit-after', type=float, default=120)
    a = ap.parse_args()
    if a.quit_client and a.quit_client > a.pairs * 2:
        ap.error('--quit-client must identify one of this run\'s clients')
    if Path(a.name).name != a.name or Path(a.exe).name != a.exe or not 1 <= a.seconds <= 3600:
        ap.error('invalid name/executable/time limit')
    cfg = load_config(a.server_config)
    if not cfg['local_only'] or cfg['bind'] != '127.0.0.1' or cfg['advertise'] != '127.0.0.1':
        ap.error('this runner requires numeric loopback exclusively')
    exe = ROOT / 'build/bin' / a.exe
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    game_name='strikers_job17_game.dll' if b'strikers_job17_game.dll\0' in exe.read_bytes() else 'strikers_game.dll'
    game_dll=ROOT / 'build/bin' / game_name
    game_digest=hashlib.sha256(game_dll.read_bytes()).hexdigest()
    seed_digest = hashlib.sha256(a.seed_save.read_bytes()).hexdigest()
    ca = (a.pki.resolve() / 'ca.der')
    if not ca.is_file():
        ap.error('private CA missing; generate test PKI first')
    out = ROOT / 'build/run' / a.name
    out.mkdir()  # exclusive run; no profile/log overwrites
    provider = LocalAccountProvider(secrets.token_bytes(32))
    (out / 'account.key').write_bytes(provider.key)
    cfg['account_key'] = (out / 'account.key').as_posix()
    (out / 'server.json').write_text(json.dumps(cfg, indent=2) + '\n')
    records, clients, servers, handles = [], [], [], []
    manifest = {'status': 'probe; completed game match not established', 'server_config': cfg,
                'executable_sha256': digest, 'game_dll': str(game_dll.relative_to(ROOT)), 'game_dll_sha256': game_digest,
                'private_ca_sha256': hashlib.sha256(ca.read_bytes()).hexdigest(),
                'seed_sha256': seed_digest, 'clients': records,
                'servers': servers, 'faults': []}
    clean_env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_') and not k.upper().endswith('_PROXY')}
    server_processes = []
    def start_server(index):
        log = out / f'server-{index}.jsonl'
        stdout = (out / f'server-{index}.stdout').open('wb');handles.append(stdout)
        p = subprocess.Popen([sys.executable, '-m', 'server.nex', '--config', str(out / 'server.json'),
                              '--log', str(log), '--seconds', str(a.seconds+20)],
                             cwd=ROOT, env=clean_env, stdout=stdout, stderr=subprocess.STDOUT)
        server_processes.append(p);servers.append({'pid': p.pid, 'log': str(log.relative_to(ROOT))})
        until = time.monotonic()+10
        while time.monotonic()<until:
            if p.poll() is not None:
                raise RuntimeError('owned server failed; inspect ' + str(out / f'server-{index}.stdout'))
            if log.exists() and '"event": "ready"' in log.read_text():
                return p
            time.sleep(.05)
        raise RuntimeError('server startup deadline exceeded')
    try:
        if not a.no_server:
            start_server(1)
        for i in range(a.pairs*2):
            name = f'client{i+1}';prefix = out / name
            userdata = out / (name+'-userdata');(userdata / 'save').mkdir(parents=True)
            shutil.copyfile(a.seed_save, userdata / 'save/strkrs.save')
            token = out / (name+'.token');principal = 1701+i
            provider.write_runtime_token(token, principal, lifetime=3600)
            seq = (a.client1_seq if i % 2 == 0 else a.client2_seq).resolve()
            env = dict(clean_env, STRIKERS_NETWORK='online', STRIKERS_ONLINE_SERVER='127.0.0.1',
                STRIKERS_ONLINE_PORTS=','.join(str(cfg[k+'_port']) for k in ('auth', 'secure', 'nat', 'relay'))+','+str(cfg.get('tls_port',443)),
                STRIKERS_ONLINE_CA_FILE=ca.as_posix(), STRIKERS_ONLINE_TOKEN_FILE=token.as_posix(),
                STRIKERS_ONLINE_OPENSSL=(Path(sys.base_prefix) / 'DLLs/libssl-3.dll').as_posix(),
                STRIKERS_ONLINE_TRACE='1',
                STRIKERS_CONFIG=prefix.with_suffix('.ini').as_posix(), STRIKERS_USERDATA=userdata.as_posix(),
                STRIKERS_ROMFS=(ROOT / 'build/dump/update/romfs').as_posix(), STRIKERS_IMPORT_COUNTS='1',
                STRIKERS_NICKNAME='Online'+str(principal), STRIKERS_VOLUME='0',
                STRIKERS_TRACE=prefix.with_suffix('.trace').as_posix(), STRIKERS_REC=prefix.with_suffix('.rec').as_posix(),
                STRIKERS_HIDLOG='1', STRIKERS_KEYBOARD_PLAYER='0', STRIKERS_SDL_TEST_PLAYERS='1',
                STRIKERS_SDL_TEST_SEQ=seq.as_posix())
            log = prefix.with_suffix('.log');handle = log.open('wb');handles.append(handle)
            p = subprocess.Popen([str(exe), str(ROOT / 'build/dump/update/elf/main.elf'), str(a.seconds)],
                                 cwd=ROOT / 'build/bin', env=env, stdout=handle, stderr=subprocess.STDOUT)
            clients.append(p)
            records.append({'role': name, 'pid': p.pid, 'principal': principal, 'log': str(log.relative_to(ROOT)),
                            'env': {k: v for k, v in env.items() if k.startswith('STRIKERS_')},
                            'replay': seq.read_text(), 'replay_sha256': hashlib.sha256(seq.read_bytes()).hexdigest(),
                            'executable': str(exe.relative_to(ROOT))})
            print(f'{name} pid={p.pid} principal={principal}', flush=True)
        started = time.monotonic()
        while any(p.poll() is None for p in clients) and time.monotonic()-started < a.seconds+45:
            elapsed = time.monotonic()-started
            if a.restart_after is not None and elapsed>=a.restart_after and len(server_processes)==1:
                p = server_processes[0];p.terminate();p.wait(timeout=10)
                manifest['faults'].append({'action': 'server-restart', 'elapsed': elapsed, 'pid': p.pid})
                start_server(2)
            if a.quit_client and elapsed>=a.quit_after:
                p = clients[a.quit_client-1]
                if p.poll() is None:
                    p.kill();manifest['faults'].append({'action': 'client-quit', 'elapsed': elapsed, 'pid': p.pid})
                a.quit_client = None
            time.sleep(.25)
    finally:
        for p, record in zip(clients, records):
            if p.poll() is None:
                p.kill();record['killed_after_limit'] = True
            record['exit_code'] = p.wait(timeout=10)
        for p, record in zip(server_processes, servers):
            if p.poll() is None:
                p.terminate()
            record['exit_code'] = p.wait(timeout=10)
        for h in handles:
            h.close()
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Evidence: ' + str(out / 'manifest.json'))
    return int(any(r['exit_code'] != 0 and not any(f['pid']==r['pid'] for f in manifest['faults']) for r in records))


if __name__ == '__main__':
    raise SystemExit(main())
