"""Time a normal start through the launcher: launcher process, game window on screen, game's first frame, start screen."""
import ctypes, json, os, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
import win_post
from job21b_launch import process_snapshot
user = ctypes.WinDLL('user32')
install = next((ROOT / 'build/phase8').glob(sys.argv[1]))
env = dict(os.environ, STRIKERS_VOLUME='0')
if len(sys.argv) > 2: env['STRIKERS_UPDATE_BASE'] = sys.argv[2]
log = install / 'userdata/last-game.log'
t0 = time.monotonic(); marks = {}
launcher = subprocess.Popen([str(install / 'StrikersRecomp.exe')], cwd=install, env=env)
pid = None
while time.monotonic() - t0 < 180:
    now = round(time.monotonic() - t0, 2)
    lw = user.FindWindowW('StrikersPlayerLauncher', None)
    if lw and user.IsWindowVisible(lw) and 'launcher window' not in marks: marks['launcher window'] = now
    if not pid:
        g = [p['pid'] for p in process_snapshot() if p['parent'] == launcher.pid and p['name'].lower() == 'strikers_native.exe']
        if g: pid = g[0]; marks['game process'] = now
    if pid and 'game window' not in marks and win_post.window(pid): marks['game window'] = now
    if 'game window' in marks and 'splash shot' not in marks and now - marks['game window'] > 1.5 and 'first frame' not in marks:
        win_post.shot(win_post.window(pid), install / 'splash.png'); marks['splash shot'] = now
    text = log.read_text(errors='replace') if log.exists() and pid else ''
    if 'nvn_vk_frame: 1 draws' in text and 'first frame' not in marks: marks['first frame'] = now
    if 'menu 0 -> 2' in text and 'start screen' not in marks: marks['start screen'] = now
    if 'first frame' in marks and now - marks['first frame'] > 3: break
    time.sleep(.1)
print(json.dumps(marks, indent=1))
if pid:
    h = win_post.window(pid)
    if h: user.PostMessageW(h, 0x10, 0, 0)
try: launcher.wait(60)
except subprocess.TimeoutExpired: pass
