"""Kill the game itself at its first prepared save, then boot the prior save."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time
from build_runtime import ROOT


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('save', type=Path)
    p.add_argument('--name', default='job14-game-crash')
    a = p.parse_args()
    assert a.name.replace('-', '').replace('_', '').isalnum()
    out = ROOT / 'build/run' / a.name
    assert not out.exists()
    (out / 'save').mkdir(parents=True)
    original = a.save.read_bytes()
    (out / 'save/strkrs.save').write_bytes(original)
    env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_USERDATA=out.as_posix(), STRIKERS_ROMFS=(ROOT / 'build/dump/update/romfs').as_posix(),
               STRIKERS_VOLUME='0', STRIKERS_FSLOG='1', STRIKERS_SEQ='../../tools/audit/job14_save_verify.seq',
               STRIKERS_FS_TEST_PAUSE='prepared')
    exe = ROOT / 'build/bin/strikers_job14_verified.exe'
    command = [str(exe), str(ROOT / 'build/dump/update/elf/main.elf'), '200']
    logpath = out / 'killed.log'
    with logpath.open('w') as log:
        child = subprocess.Popen(command, cwd=exe.parent, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            until = time.monotonic() + 190
            while 'fs-test: paused prepared' not in logpath.read_text(errors='replace'):
                assert child.poll() is None, 'game exited before save boundary'
                assert time.monotonic() < until, 'game did not reach its save boundary'
                time.sleep(.1)
        finally:
            child.kill()
            child.wait()
    assert (out / 'save/strkrs.save').read_bytes() == original
    assert list(out.glob('save.work.*')) and not (out / 'save.commit').exists()
    env.pop('STRIKERS_FS_TEST_PAUSE')
    # Boot and enter Options with the SAME userdata; stop before changing Music.
    load_seq = out / 'load.seq'
    load_seq.write_text('300+12:Z\n800+8:Z\n1100+8:*\n')
    env['STRIKERS_SEQ'] = load_seq.as_posix()
    with (out / 'reloaded.log').open('w') as log:
        r = subprocess.run(command[:-1] + ['60'], cwd=exe.parent, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=90)
    assert r.returncode == 0
    assert (out / 'save/strkrs.save').read_bytes() == original
    assert 'mode 1 -> ok' in (out / 'reloaded.log').read_text(errors='replace')
    assert not list(out.glob(f'save.work.{child.pid}'))
    result = dict(killed_pid=child.pid, boundary='prepared', save_sha256=hashlib.sha256(original).hexdigest(),
                  old_save_survived=True, restarted_same_userdata=True, restart_exit=r.returncode)
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
