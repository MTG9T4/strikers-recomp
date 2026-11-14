"""F9 bug report through the real launcher: the system line at the top of the log, the report zip and what is in it.

  python tools/audit/job39_bug_report.py "<install glob under build/phase8>"

Run the test copy windowed (the game reads the real keyboard while its window is in front); F9 is a posted key, which
the game window handles itself. Checks that the zip holds a screenshot, system.txt, the log, the settings without the
invite code and controllers.ini, and that the note was drawn (screenshot in <install>/bug-report-test).
"""
import json, os, subprocess, sys, time, zipfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
import win_post
from job21b_launch import process_snapshot
VK_F9 = 0x78


def main():
    install = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    out = install / 'bug-report-test'
    out.mkdir(exist_ok=True)
    user = install / 'userdata'
    log = user / 'last-game.log'
    before = set((user / 'reports').glob('*.zip')) if (user / 'reports').exists() else set()
    env = dict(os.environ, STRIKERS_UPDATE_BASE='http://127.0.0.1:9', STRIKERS_VOLUME='0')
    t0 = time.time()
    launcher = subprocess.Popen([str(install / 'StrikersRecomp.exe')], cwd=install, env=env)
    h = pid = None
    end = time.monotonic() + 120
    while time.monotonic() < end and not h:
        g = [p['pid'] for p in process_snapshot() if p['parent'] == launcher.pid and p['name'].lower() == 'strikers_native.exe']
        if g:
            pid, h = g[0], win_post.window(g[0])
        time.sleep(.3)
    end = time.monotonic() + 180
    while time.monotonic() < end and not (log.exists() and log.stat().st_mtime >= t0 and 'nvn_vk_frame: 1 draws' in log.read_text(errors='replace')):
        time.sleep(1)
    time.sleep(5)
    result = {}
    text = log.read_text(errors='replace')
    result['system line'] = next((l for l in text.splitlines() if l.startswith('system: ')), None)
    win_post.key(h, VK_F9)
    end = time.monotonic() + 60
    zips = []
    while time.monotonic() < end and not zips:
        zips = sorted(set((user / 'reports').glob('*.zip')) - before) if (user / 'reports').exists() else []
        time.sleep(.5)
    time.sleep(1.5)
    win_post.shot(h, out / 'note.png')
    result['zip'] = zips[-1].name if zips else None
    if zips:
        z = zipfile.ZipFile(zips[-1])
        names = [n.split('/', 1)[-1] for n in z.namelist() if not n.endswith('/')]
        result['files'] = sorted(names)
        result['bytes'] = zips[-1].stat().st_size
        ini = next((z.read(n).decode(errors='replace') for n in z.namelist() if n.endswith('strikers.ini')), '')
        result['invite code removed'] = all('removed from the report' in l for l in ini.splitlines() if l.strip().startswith('group_code'))
        shot = next((n for n in z.namelist() if n.endswith('screenshot.jpg')), None)
        if shot:
            (out / 'screenshot.jpg').write_bytes(z.read(shot))
            from PIL import Image
            result['screenshot size'] = Image.open(out / 'screenshot.jpg').size
        result['system.txt'] = next((z.read(n).decode(errors='replace').splitlines()[0] for n in z.namelist() if n.endswith('system.txt')), None)
        result['folder left'] = [p.name for p in (user / 'reports').iterdir() if p.is_dir()]
    result['log lines'] = [l for l in log.read_text(errors='replace').splitlines() if l.startswith('bug-report:')]
    win_post.u.PostMessageW(h, 0x10, 0, 0)
    try:
        result['launcher exit'] = launcher.wait(90)
    except subprocess.TimeoutExpired:
        result['launcher exit'] = None
    (out / 'report.json').write_text(json.dumps(result, indent=1))
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
