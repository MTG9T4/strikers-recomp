"""Fast-forward (hold Tab; display.c): the same boot twice in a windowed test copy, once with STRIKERS_FF_TEST forcing
fast-forward for a stretch, once without, with pictures at the same moments and the log's refresh counts.

  python tools/audit/job47_fast_forward.py "<install glob under build/phase8>" <name> [<first second>,<seconds>] [shots...]

Only the test copy's own game is driven or closed (matched by its full path). The copy must be windowed: the game reads
the real keyboard while it is in front.
"""
import ctypes, json, os, re, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
import win_post
from job21b_launch import process_snapshot
from job35_update_flow import image


def game(install):
    want = str((install / 'strikers_native.exe').resolve()).lower()
    for p in process_snapshot():
        if p['name'].lower() == 'strikers_native.exe' and image(p['pid']).lower() == want:
            return p['pid']
    return None


def main():
    install = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    name = sys.argv[2]
    ff = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != '-' else ''
    shots = [int(x) for x in sys.argv[4:]] or [15, 30, 45]
    ini = (install / 'strikers.ini').read_text(encoding='utf-8', errors='replace')
    assert re.search(r'^window\s*=\s*windowed', ini, re.M), 'the test copy must be windowed'
    assert game(install) is None, 'the test copy is already running'
    out = install / 'fast-forward' / name
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, STRIKERS_UPDATE_BASE='http://127.0.0.1:9', STRIKERS_VOLUME='0', STRIKERS_CLOCKSTAT='1')
    env.pop('STRIKERS_FF_TEST', None)
    if ff:
        env['STRIKERS_FF_TEST'] = ff
    t0 = time.time()
    subprocess.Popen([str(install / 'StrikersRecomp.exe')], cwd=install, env=env)
    pid = None
    for s in shots:
        while time.time() - t0 < s:
            time.sleep(.2)
        pid = pid or game(install)
        if pid:
            h = win_post.window(pid)
            if h:
                win_post.shot(h, out / f'{s:03d}s.png')
    if pid:
        h = win_post.window(pid)
        if h:
            ctypes.WinDLL('user32').PostMessageW(ctypes.c_void_p(h), 0x10, 0, 0)
        for _ in range(60):
            if game(install) is None:
                break
            time.sleep(.5)
    log = (install / 'userdata/last-game.log').read_text(errors='replace')
    keep = [l for l in log.splitlines() if 'fast-forward' in l or 'STRIKERS_FF_TEST' in l or 'clockstat' in l.lower() or 'refreshes' in l][:40]
    (out / 'log-lines.txt').write_text('\n'.join(keep))
    print(json.dumps(dict(name=name, ff=ff, shots=[f'{s:03d}s.png' for s in shots], still_running=game(install) is not None, log=keep[:14]), indent=1))


if __name__ == '__main__':
    main()
