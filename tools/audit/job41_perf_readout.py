"""The performance readout (F10) through the real launcher: shown over the start screen and the game, hidden again.

  python tools/audit/job41_perf_readout.py "<install glob under build/phase8>"

Run the test copy windowed (the game reads the real keyboard while its window is in front). Screenshots in
<install>/perf-readout-test: 1-start (readout on), 2-playing (after PLAY), 3-hidden (F10 again).
"""
import json, os, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
import win_post
from job21b_launch import process_snapshot
VK_F10, VK_RETURN = 0x79, 0x0D


def main():
    install = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    out = install / 'perf-readout-test'
    out.mkdir(exist_ok=True)
    log = install / 'userdata/last-game.log'
    env = dict(os.environ, STRIKERS_UPDATE_BASE='http://127.0.0.1:9', STRIKERS_VOLUME='0')
    t0 = time.time()
    launcher = subprocess.Popen([str(install / 'StrikersRecomp.exe')], cwd=install, env=env)
    h = None
    end = time.monotonic() + 120
    while time.monotonic() < end and not h:
        g = [p['pid'] for p in process_snapshot() if p['parent'] == launcher.pid and p['name'].lower() == 'strikers_native.exe']
        if g:
            h = win_post.window(g[0])
        time.sleep(.3)
    end = time.monotonic() + 180
    while time.monotonic() < end and not (log.exists() and log.stat().st_mtime >= t0 and 'nvn_vk_frame: 1 draws' in log.read_text(errors='replace')):
        time.sleep(1)
    time.sleep(4)
    win_post.u.PostMessageW(h, 0x104, VK_F10, 1); time.sleep(.1); win_post.u.PostMessageW(h, 0x105, VK_F10, 0xC0000001)   # WM_SYSKEYDOWN/UP, as Windows sends F10
    time.sleep(2.5)
    win_post.shot(h, out / '1-start.png')
    win_post.key(h, VK_RETURN)
    time.sleep(12)
    win_post.shot(h, out / '2-playing.png')
    win_post.u.PostMessageW(h, 0x104, VK_F10, 1); time.sleep(.1); win_post.u.PostMessageW(h, 0x105, VK_F10, 0xC0000001)
    time.sleep(1.5)
    win_post.shot(h, out / '3-hidden.png')
    gpu = [l for l in log.read_text(errors='replace').splitlines() if l.startswith('nvn_vk_gpu:')][-1:]
    win_post.u.PostMessageW(h, 0x10, 0, 0)
    try:
        code = launcher.wait(90)
    except subprocess.TimeoutExpired:
        code = None
    print(json.dumps(dict(gpu_line=gpu, launcher_exit=code), indent=1))


if __name__ == '__main__':
    main()
