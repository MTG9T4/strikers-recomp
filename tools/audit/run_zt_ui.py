"""Capture this job's Online overlay using the renderer's swapchain capture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import ctypes
from PIL import Image
from build_runtime import ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--exe', default='strikers_native.exe')
    ap.add_argument('--network', default='ff61286137000000')
    ap.add_argument('--peer', action='store_true', help='include an isolated native ad-hoc peer for path/latency UI evidence')
    ap.add_argument('--block-direct', action='store_true')
    a = ap.parse_args()
    out = a.out.resolve(); assert out.is_relative_to(ROOT / 'build/run')
    out.mkdir(exist_ok=False)
    config = out / 'strikers.ini'
    config.write_text('window=windowed\nwidth=1280\nheight=900\nresolution_scale=0.5\nvolume=0\n'
                      '[Network]\nmode=zerotier\nzerotier_network=' + a.network + '\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_PC_CONFIG=config.as_posix(), STRIKERS_USERDATA=(out / 'userdata').as_posix(),
               STRIKERS_CONFIG=(out / 'controllers.ini').as_posix(), STRIKERS_SHOW='0',
               STRIKERS_ROMFS=(ROOT / 'build/dump/update/romfs').as_posix(),
               STRIKERS_PC_SHOT='0', STRIKERS_SHOT_DIR=out.as_posix(), STRIKERS_KEYBOARD_PLAYER='0')
    actions = []
    buddy = None
    buddy_file = None
    buddy_id = ''
    if a.peer:
        assert a.network.startswith('ff'), 'peer UI probes use ad-hoc networks only'
        lib = ctypes.CDLL(str(ROOT / 'build/bin/strikers_zt.dll'))
        ids = []
        for profile in ('userdata', 'buddy'):
            folder = out / profile / 'zerotier'; folder.mkdir(parents=True)
            key = ctypes.create_string_buffer(384); size = ctypes.c_uint(384)
            assert lib.zts_id_new(key, ctypes.byref(size)) == 0
            (folder / 'identity.secret').write_bytes(key.value)
            ids.append(key.value.decode().split(':')[0])
        buddy_id = ids[1]
        env['STRIKERS_ZT_PEERS'] = ids[1]
        if a.block_direct:
            env['STRIKERS_ZT_BLOCK_DIRECT'] = ids[1]
        buddy_env = dict(env, STRIKERS_NETWORK='zerotier', STRIKERS_ZEROTIER_NETWORK=a.network,
                         STRIKERS_USERDATA=(out / 'buddy').as_posix(), STRIKERS_ZT_PEERS=ids[0])
        if a.block_direct:
            buddy_env['STRIKERS_ZT_BLOCK_DIRECT'] = ids[0]
        buddy_file = (out / 'buddy.log').open('wb')
        buddy = subprocess.Popen([str(ROOT / 'build/bin/strikers_job18_zt_live.exe'), '60'],
                                 cwd=ROOT / 'build/bin', env=buddy_env, stdout=buddy_file, stderr=subprocess.STDOUT)
    exe = ROOT / 'build/bin' / a.exe
    with (out / 'game.log').open('wb') as log:
        child = subprocess.Popen([str(exe), str(ROOT / 'build/dump/update/elf/main.elf'), '180'],
                                 cwd=ROOT / 'build/bin', env=env, stdout=log, stderr=subprocess.STDOUT)
        def action(name):
            r = subprocess.run([sys.executable, str(ROOT / 'tools/audit/pc_window_probe.py'), str(child.pid), name],
                               capture_output=True, text=True, check=True)
            actions.append(json.loads(r.stdout))
        try:
            deadline = time.monotonic() + 100
            while time.monotonic() < deadline:
                text = (out / 'game.log').read_text(errors='replace')
                expected = f'peer={buddy_id} path={"relayed" if a.block_direct else "direct"}'
                if '[zerotier] state=2 Connected.' in text and 'nvn_vk: frame 60' in text and (not a.peer or expected in text):
                    break
                assert child.poll() is None, 'game exited before overlay'
                time.sleep(.25)
            else:
                raise TimeoutError('node/renderer not ready')
            action('f1'); time.sleep(1); action('capture')
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                images = sorted(out.glob('pc_*.bmp'))
                if images:
                    try:
                        with Image.open(images[-1]) as im:
                            im.load(); im.save(out / 'online-connected.png')
                        break
                    except OSError:
                        pass
                time.sleep(.1)
            else:
                raise TimeoutError('capture missing')
            action('close'); child.wait(timeout=20)
            assert child.returncode == 0
        finally:
            if child.poll() is None:
                child.kill(); child.wait()
            if buddy:
                try:
                    buddy.wait(timeout=65)
                except subprocess.TimeoutExpired:
                    buddy.kill(); buddy.wait()
                buddy_file.close()
            (out / 'run.json').write_text(json.dumps(dict(pid=child.pid, exit=child.returncode,
                exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), actions=actions,
                peer=buddy_id, blocked_direct=a.block_direct, peer_exit=buddy.returncode if buddy else None), indent=2))


if __name__ == '__main__':
    main()
