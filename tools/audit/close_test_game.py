"""Close the game windows of test copies under build/phase8 (never any other copy, such as the player's own install).

  python tools/audit/close_test_game.py

Asks each window to close (WM_CLOSE, as the window's X does), waits for the process to end and prints what it did.
"""
import ctypes as C
from ctypes import wintypes as W
import json, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = str(ROOT / 'build' / 'phase8').lower()
u = C.WinDLL('user32')
PROC = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
u.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]


def games():
    out = subprocess.run(['powershell', '-NoProfile', '-Command',
                          "Get-CimInstance Win32_Process -Filter \"Name='strikers_native.exe'\" | "
                          "Select-Object ProcessId,ExecutablePath | ConvertTo-Json"], capture_output=True, text=True).stdout
    found = json.loads(out) if out.strip() else []
    return found if isinstance(found, list) else [found]


def main():
    ours = [g for g in games() if (g.get('ExecutablePath') or '').lower().startswith(BUILD)]
    others = [g for g in games() if g not in ours]
    for g in ours:
        def each(h, _):
            pid = W.DWORD(); u.GetWindowThreadProcessId(h, C.byref(pid))
            cls = C.create_unicode_buffer(100); u.GetClassNameW(h, cls, 100)
            if pid.value == g['ProcessId'] and cls.value == 'strikers_native_window':
                u.PostMessageW(h, 0x10, 0, 0)
            return True
        u.EnumWindows(PROC(each), 0)
    end = time.monotonic() + 30
    while time.monotonic() < end and any(g['ProcessId'] in [x['ProcessId'] for x in games()] for g in ours):
        time.sleep(.5)
    left = [g['ProcessId'] for g in ours if g['ProcessId'] in [x['ProcessId'] for x in games()]]
    print(json.dumps(dict(closed=[g['ProcessId'] for g in ours if g['ProcessId'] not in left], still_running=left,
                          other_copies_left_alone=[g['ProcessId'] for g in others])))


if __name__ == '__main__':
    main()
