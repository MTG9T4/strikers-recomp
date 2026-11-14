"""A fresh install through the real setup window, in the mode new players get: the fields filled in by setup itself, the
"copy" box left clear, Check files and build pressed. Reports what setup found, how long it took, that the game reads
its data from the packages (installer/packages.txt, no extracted RomFS) and the folder's size.

  python tools/audit/job45_fresh_install.py "<fresh stage copy under build/phase8>"
"""
import ctypes as C
from ctypes import wintypes as W
import json, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/audit'))
import win_post
u = C.WinDLL('user32')
u.GetDlgItem.restype = W.HWND; u.GetDlgItem.argtypes = [W.HWND, C.c_int]
u.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]; u.SendMessageW.restype = C.c_ssize_t
u.GetWindowTextW.argtypes = [W.HWND, C.c_wchar_p, C.c_int]
PROC = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)


def text(h):
    b = C.create_unicode_buffer(1024); u.GetWindowTextW(h, b, 1024); return b.value


def edits(top):
    out = []
    def each(h, _):
        cls = C.create_unicode_buffer(32); u.GetClassNameW(h, cls, 32)
        if cls.value == 'Edit':                                # another process's edit control: WM_GETTEXT, not GetWindowText
            buf = C.create_unicode_buffer(1024); u.SendMessageW(h, 0x000D, 1024, C.cast(buf, C.c_void_p).value or 0); out.append(buf.value)
        return True
    u.EnumChildWindows(top, PROC(each), 0)
    return out


def folder_size(p):
    return sum(f.stat().st_size for f in p.rglob('*') if f.is_file())


def main():
    root = next((ROOT / 'build/phase8').glob(sys.argv[1]))
    t0 = time.time()
    setup = subprocess.Popen([str(root / 'StrikersSetup.exe')], cwd=root)
    h = None
    for _ in range(60):
        time.sleep(.5); h = win_post.window(setup.pid)
        if h: break
    time.sleep(1.5)
    found = edits(h)
    print(json.dumps(dict(fields_filled=[bool(x) for x in found], names=[Path(x).name for x in found]), indent=1), flush=True)
    assert len(found) == 3 and all(found), 'setup did not fill in all three files'
    button = u.GetDlgItem(h, 103)
    u.SendMessageW(button, 0x00F5, 0, 0)                     # BM_CLICK: Check files and build
    log = root / 'installer/setup.log'
    end = time.monotonic() + 4 * 3600
    while time.monotonic() < end and setup.poll() is None:
        time.sleep(15)
        if log.exists():
            last = [l for l in log.read_text(encoding='utf-8', errors='replace').splitlines() if l.strip()][-1:]
            if last and ('Ready' in last[0] or 'failed' in last[0].lower()):
                time.sleep(10)
                break
    took = round((time.time() - t0) / 60, 1)
    lines = log.read_text(encoding='utf-8', errors='replace').splitlines() if log.exists() else []
    if setup.poll() is None:
        win_post.u.PostMessageW(h, 0x10, 0, 0)
        try: setup.wait(30)
        except subprocess.TimeoutExpired: pass
    result = dict(minutes=took, last_lines=lines[-4:], packages_txt=(root / 'installer/packages.txt').exists(),
                  extracted_romfs=(root / 'build/dump/update/romfs').exists(), exefs=(root / 'build/dump/update/exefs/main').exists(),
                  game_dll=(root / 'strikers_game.dll').exists(), shortcut=(root / 'Strikers Recomp.lnk').exists(),
                  folder_gb=round(folder_size(root) / 2**30, 2))
    timings = root / 'build/setup-timings.json'
    if timings.exists():
        result['steps'] = {s['step']: round(s['seconds']) for s in json.loads(timings.read_text())['steps']}
    (root / 'fresh-install-report.json').write_text(json.dumps(result, indent=1))
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
