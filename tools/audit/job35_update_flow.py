"""The update offered in the game (1.0.8 flow): hidden launcher, check beside the game, UPDATE on the start screen,
the update window, the game again. The release comes from a local test server (STRIKERS_UPDATE_BASE).

  python tools/audit/job35_update_flow.py "<install glob under build/phase8>" http://127.0.0.1:8787
"""
import ctypes, json, os, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
import win_post
from job21b_launch import process_snapshot
user = ctypes.WinDLL('user32')
user.FindWindowW.restype = ctypes.c_void_p
user.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
VK_DOWN, VK_RETURN = 0x28, 0x0D


INSTALL = None
kernel = ctypes.WinDLL('kernel32')
kernel.OpenProcess.restype = ctypes.c_void_p
kernel.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
kernel.CloseHandle.argtypes = [ctypes.c_void_p]


def image(pid):
    h = kernel.OpenProcess(0x1000, False, pid)               # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ''
    try:
        buf = ctypes.create_unicode_buffer(1024); n = ctypes.c_uint32(1024)
        return buf.value if kernel.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) else ''
    finally:
        kernel.CloseHandle(h)


def game():
    """The test copy's game only: never the player's own copy running elsewhere."""
    want = str((INSTALL / 'strikers_native.exe').resolve()).lower()
    for p in process_snapshot():
        if p['name'].lower() == 'strikers_native.exe' and image(p['pid']).lower() == want:
            return p['pid']
    return None


def wait_frame(log, since, seconds=240):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if log.exists() and log.stat().st_mtime >= since and 'nvn_vk_frame: 1 draws' in log.read_text(errors='replace'):
            return True
        time.sleep(1)
    return False


def main():
    global INSTALL
    install = INSTALL = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    out = install / 'update-flow'
    out.mkdir(exist_ok=True)
    env = dict(os.environ, STRIKERS_UPDATE_BASE=sys.argv[2], STRIKERS_VOLUME='0')
    log = install / 'userdata/last-game.log'
    note = install / 'installer/update/available.json'
    note.unlink(missing_ok=True)
    steps = []
    t0 = time.time()
    subprocess.Popen([str(install / 'StrikersRecomp.exe')], cwd=install, env=env)
    assert wait_frame(log, t0), 'first game did not draw'
    end = time.monotonic() + 30
    while time.monotonic() < end and not note.exists():
        time.sleep(.5)
    steps.append(['available.json', note.read_text() if note.exists() else None])
    time.sleep(4)                                   # the overlay reads it every 2 s
    h = win_post.window(game())
    win_post.shot(h, out / '1-start-with-update.png')
    win_post.key(h, VK_DOWN); time.sleep(.4); win_post.key(h, VK_RETURN)
    s = None
    end = time.monotonic() + 60
    while time.monotonic() < end and not s:
        s = user.FindWindowW('StrikersPlayerSetup', None); time.sleep(.5)
    steps.append(['update window', bool(s)])
    end = time.monotonic() + 1200
    while time.monotonic() < end and user.FindWindowW('StrikersPlayerSetup', None):
        time.sleep(1)
    steps.append(['update window closed', round(time.time() - t0)])
    t1 = time.time()
    steps.append(['game again', wait_frame(log, t1)])
    time.sleep(6)
    pid = game()
    if pid:
        h = win_post.window(pid)
        win_post.shot(h, out / '2-after-update.png')
        user.PostMessageW(ctypes.c_void_p(h), 0x10, 0, 0)
    time.sleep(15)
    installed = json.loads((install / 'installer/installed-release.json').read_text())['version']
    steps.append(['installed', installed, 'note left', note.exists()])
    print(json.dumps(steps, indent=1))
    (out / 'report.json').write_text(json.dumps(steps, indent=1))


if __name__ == '__main__':
    main()
