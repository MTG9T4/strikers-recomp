"""Settings > Saves through the real launcher: the list of emulator saves found on this PC, Use this save, Restart the
game now, and the next start putting the save in place with a backup of the one before. Driven by posted keys and clicks.

  python tools/audit/job38_save_import.py "<install glob under build/phase8>"

The tab and buttons are clicked where the overlay logs them (STRIKERS_UI_TRACE); screenshots of each step land in <install>/save-import-test. Emulator saves are only read. Run the test copy windowed:
the game reads the real keyboard while its window is in front.
"""
import hashlib, json, os, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
import win_post
from job21b_launch import process_snapshot
VK_DOWN, VK_RETURN = 0x28, 0x0D
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()


def game_of(launcher_pid, seconds=120):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        g = [p['pid'] for p in process_snapshot() if p['parent'] == launcher_pid and p['name'].lower() == 'strikers_native.exe']
        if g:
            h = win_post.window(g[0])
            if h:
                return g[0], h
        time.sleep(.3)
    return None, None


def first_frame(log, since, seconds=180):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if log.exists() and log.stat().st_mtime >= since and 'nvn_vk_frame: 1 draws' in log.read_text(errors='replace'):
            return True
        time.sleep(1)
    return False


def main():
    install = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    out = install / 'save-import-test'
    out.mkdir(exist_ok=True)
    user = install / 'userdata'
    log = user / 'last-game.log'
    before = sha(user / 'save/strkrs.save') if (user / 'save/strkrs.save').exists() else None
    env = dict(os.environ, STRIKERS_UPDATE_BASE='http://127.0.0.1:9', STRIKERS_VOLUME='0', STRIKERS_UI_TRACE='1')
    steps = {}
    t0 = time.time()
    launcher = subprocess.Popen([str(install / 'StrikersRecomp.exe')], cwd=install, env=env)
    pid, h = game_of(launcher.pid)
    assert h and first_frame(log, t0), 'the game did not draw'
    time.sleep(6)
    win_post.key(h, VK_DOWN); time.sleep(.4); win_post.key(h, VK_DOWN); time.sleep(.4); win_post.key(h, VK_RETURN); time.sleep(1.5)
    win_post.shot(h, out / '1-settings.png')
    def item(name, seconds=10):
        """Centre of an overlay item from its STRIKERS_UI_TRACE line in the log (the newest one)."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            lines = [l.split() for l in log.read_text(errors='replace').splitlines() if l.startswith('pc-ui-item: ' + name + ' ')]
            if lines:
                x0, y0, x1, y1 = map(int, lines[-1][-4:])
                return (x0 + x1) // 2, (y0 + y1) // 2
            time.sleep(.3)
        raise AssertionError('no position logged for ' + name)
    win_post.click(h, *item('tab Saves')); time.sleep(1)
    win_post.shot(h, out / '2-saves-tab.png')
    win_post.click(h, *item('use 0')); time.sleep(1)
    win_post.shot(h, out / '3-staged.png')
    steps['staged'] = (user / 'save-import/strkrs.save').exists()
    staged = sha(user / 'save-import/strkrs.save') if steps['staged'] else None
    t1 = time.time()
    win_post.click(h, *item('restart'))
    end = time.monotonic() + 120
    new = None
    while time.monotonic() < end and not new:
        g = [p['pid'] for p in process_snapshot() if p['parent'] == launcher.pid and p['name'].lower() == 'strikers_native.exe' and p['pid'] != pid]
        new = g[0] if g else None
        time.sleep(.3)
    steps['started again'] = round(time.time() - t1, 1) if new else None
    steps['first frame'] = first_frame(log, t1)
    text = log.read_text(errors='replace')
    steps['log'] = [l for l in text.splitlines() if l.startswith('save-import')]
    steps['save is the staged one'] = staged is not None and sha(user / 'save/strkrs.save') == staged
    backups = sorted(user.glob('save-before-import-*'))
    steps['backup is the save before'] = bool(backups) and before is not None and sha(backups[-1] / 'strkrs.save') == before
    steps['left behind'] = [n for n in ('save-import', 'restart-requested') if (user / n).exists()]
    time.sleep(6)
    h2 = win_post.window(new) if new else None
    if h2:
        win_post.shot(h2, out / '4-after-restart.png')
        win_post.u.PostMessageW(h2, 0x10, 0, 0)
    try:
        steps['launcher exit'] = launcher.wait(90)
    except subprocess.TimeoutExpired:
        steps['launcher exit'] = None
    (out / 'report.json').write_text(json.dumps(steps, indent=1))
    print(json.dumps(steps, indent=1))


if __name__ == '__main__':
    main()
