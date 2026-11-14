"""The start screen and the in-game PC menu through the real launcher, driven by posted keys, with screenshots.

  python tools/audit/job33_menu_test.py "<install glob under build/phase8>"

Start screen -> Settings and Back -> Play -> F1 PC menu -> Resume -> F1 -> Quit to desktop (pressed twice).
"""
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

VK_DOWN, VK_UP, VK_RETURN, VK_ESCAPE = 0x28, 0x26, 0x0D, 0x1B


def main():
    install = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    out = install / 'menu-test'
    out.mkdir(exist_ok=True)
    # STRIKERS_UPDATE_BASE to a closed local port: the update check fails at once and the launcher plays.
    env = dict(os.environ, STRIKERS_UPDATE_BASE='http://127.0.0.1:9', STRIKERS_VOLUME='0')
    steps = []
    log = install / 'userdata/last-game.log'
    launcher = subprocess.Popen([str(install / 'StrikersRecomp.exe')], cwd=install, env=env)
    pid = h = None
    end = time.monotonic() + 120
    while time.monotonic() < end and not h:
        games = [p['pid'] for p in process_snapshot() if p['parent'] == launcher.pid and p['name'].lower() == 'strikers_native.exe']
        if games:
            pid = games[0]
            h = win_post.window(pid)
        time.sleep(.5)
    assert h, 'no game window'
    end = time.monotonic() + 180
    while time.monotonic() < end and 'nvn_vk_frame: 1 draws' not in (log.read_text(errors='replace') if log.exists() else ''):
        time.sleep(1)
    time.sleep(6)

    def shot(name, note):
        time.sleep(1.2)
        win_post.shot(h, out / f'{name}.png')
        steps.append(note)
        print(note, flush=True)

    def press(vk, n=1):
        for _ in range(n):
            win_post.key(h, vk)
            time.sleep(.35)

    shot('1-start', 'start screen')
    press(VK_DOWN, 2); shot('2-start-settings-focused', 'Settings focused')
    press(VK_RETURN); shot('3-settings', 'settings window')
    press(VK_ESCAPE)                       # leaves any text field; Back is a button
    win_post.key(h, win_post.VK_F1); time.sleep(.5); shot('4-back-to-start', 'F1 from settings goes back to the start screen')
    press(VK_UP, 2); press(VK_RETURN); time.sleep(4); shot('5-playing', 'Play: game visible')
    win_post.key(h, win_post.VK_F1); time.sleep(.5); shot('6-pc-menu', 'F1 opens the PC menu')
    press(VK_ESCAPE); time.sleep(1); shot('7-resumed', 'Esc resumes')
    win_post.key(h, win_post.VK_F1); time.sleep(.5)
    press(VK_DOWN, 3); press(VK_RETURN); shot('8-quit-armed', 'Quit asks to be pressed again')
    press(VK_RETURN)
    try:
        code = launcher.wait(90)
    except subprocess.TimeoutExpired:
        code = None
    text = log.read_text(errors='replace')
    report = dict(steps=steps, launcher_exit=code,
                  menu_lines=[l for l in text.splitlines() if l.startswith('pc-settings:')][:40],
                  closed=[l for l in text.splitlines() if 'window closed by the user' in l or 'shutdown clean' in l][:3])
    (out / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
