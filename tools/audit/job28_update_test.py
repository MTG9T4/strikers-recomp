"""Update a test install through the real windows: launcher check, Update button, StrikersSetup.exe --update,
the launcher again and the game's first frame. The release comes from a local test server (STRIKERS_UPDATE_BASE).

  python tools/audit/job28_update_test.py "<install glob under build/phase8>" http://127.0.0.1:8787
"""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
import win_post
from job21b_launch import process_snapshot

user = ctypes.WinDLL('user32')
user.FindWindowW.restype = ctypes.c_void_p
user.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
user.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
user.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]


def find(cls, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        h = user.FindWindowW(cls, None)
        if h:
            return h
        time.sleep(.25)
    return None


def main():
    install = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    out = install / 'update-test'
    out.mkdir(exist_ok=True)
    env = dict(os.environ, STRIKERS_UPDATE_BASE=sys.argv[2], STRIKERS_VOLUME='0')
    report = dict(steps=[])
    note = lambda s: (report['steps'].append([round(time.monotonic() - t0), s]), print(s, flush=True))
    t0 = time.monotonic()
    launcher = subprocess.Popen([str(install / 'StrikersRecomp.exe')], cwd=install, env=env)
    h = find('StrikersPlayerLauncher', 20)
    assert h, 'launcher window did not appear'
    time.sleep(8)                                    # the check: up to 6 s
    win_post.shot(h, out / '1-launcher-offer.png')
    note('launcher offered the update')
    user.PostMessageW(h, 0x111, 2, 0)                # WM_COMMAND, the Update button
    s = find('StrikersPlayerSetup', 20)
    assert s, 'update window did not appear'
    note('update window open')
    shots = 0
    while user.FindWindowW('StrikersPlayerSetup', None):
        if shots < 6 and time.monotonic() - t0 > 15 * (shots + 1):
            try:
                win_post.shot(s, out / f'2-update-{shots}.png')
            except Exception:
                pass
            shots += 1
        if time.monotonic() - t0 > 1800:
            raise SystemExit('update did not finish in 30 minutes')
        time.sleep(1)
    note('update window closed')
    h2 = find('StrikersPlayerLauncher', 30)
    note('launcher started again' if h2 else 'NO launcher after update')
    pid = None
    end = time.monotonic() + 240
    while time.monotonic() < end and not pid:
        games = [p for p in process_snapshot() if p['name'].lower() == 'strikers_native.exe']
        pid = games[0]['pid'] if games else None
        time.sleep(1)
    note(f'game process {pid}')
    log = install / 'userdata/last-game.log'
    end = time.monotonic() + 240
    while time.monotonic() < end and pid:
        if log.exists() and 'nvn_vk_frame: 1 draws ' in log.read_text(errors='replace'):
            break
        time.sleep(2)
    time.sleep(10)
    if pid:
        gh = win_post.window(pid)
        if gh:
            win_post.shot(gh, out / '3-game.png')
            user.PostMessageW(gh, 0x10, 0, 0)       # WM_CLOSE
        note('game reached its first frame and was closed')
    time.sleep(15)
    report['update_log_tail'] = (install / 'installer/update.log').read_text(errors='replace')[-1500:]
    report['installed'] = json.loads((install / 'installer/installed-release.json').read_text())['version']
    report['pending_left'] = (install / 'installer/update/pending.json').exists()
    (out / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'update_log_tail'}, indent=1))


if __name__ == '__main__':
    main()
