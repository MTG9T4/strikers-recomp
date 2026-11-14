"""Send ordinary window/input messages only to an explicitly named test PID.

Examples: pc_window_probe.py 1234 f1; ... mode 1; ... resize 1280 1024.
No desktop mode changes, input injection into other apps, or guest addresses.
"""
import ctypes
from ctypes import wintypes as W
import json
import sys
import time

def main():
    pid=int(sys.argv[1]);action=sys.argv[2]
    u=ctypes.WinDLL('user32',use_last_error=True)
    u.SetThreadDpiAwarenessContext.argtypes=[ctypes.c_void_p];u.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    u.GetWindowThreadProcessId.argtypes=[W.HWND,ctypes.POINTER(W.DWORD)]
    u.GetClientRect.argtypes=[W.HWND,ctypes.POINTER(W.RECT)];u.GetWindowRect.argtypes=u.GetClientRect.argtypes
    u.GetDpiForWindow.argtypes=[W.HWND];u.GetDpiForWindow.restype=W.UINT
    u.IsIconic.argtypes=[W.HWND]
    u.GetForegroundWindow.restype=W.HWND
    u.SetWindowPos.argtypes=[W.HWND,W.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,W.UINT]
    u.ShowWindow.argtypes=[W.HWND,ctypes.c_int];u.SetForegroundWindow.argtypes=[W.HWND]
    u.SendMessageTimeoutW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM,W.UINT,W.UINT,ctypes.POINTER(ctypes.c_size_t)]
    found=[]
    @ctypes.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    def enum(h,l):
        p=W.DWORD();u.GetWindowThreadProcessId(h,ctypes.byref(p))
        name=ctypes.create_unicode_buffer(128);u.GetClassNameW(h,name,128)
        if p.value==pid and name.value=='strikers_native_window':found.append(h)
        return True
    u.EnumWindows(enum,0)
    if len(found)!=1:raise RuntimeError(f'expected exactly one game window for {pid}, got {found}')
    h=found[0]
    def send(m,w=0,l=0):
        result=ctypes.c_size_t()
        if not u.SendMessageTimeoutW(h,m,w,l,2,3000,ctypes.byref(result)):raise ctypes.WinError(ctypes.get_last_error())
    if action=='focus-event':send(7)
    elif action=='capture':send(0x8000+17)
    elif action=='scale-test':send(0x8000+18,int(sys.argv[3]))
    elif action=='f1':send(0x100,0x70,1);send(0x101,0x70,0xc0000001)
    elif action=='alt-enter':send(0x104,13,0x20000001);send(0x105,13,0xe0000001)
    elif action=='mode':send(0x8000+16,int(sys.argv[3]))
    elif action=='resize':
        w,he=map(int,sys.argv[3:5]);rc=W.RECT(0,0,w,he);style=u.GetWindowLongW(h,-16);u.AdjustWindowRect(ctypes.byref(rc),style,False)
        u.SetWindowPos(h,None,40,40,rc.right-rc.left,rc.bottom-rc.top,0x14)
    elif action=='minimize':u.ShowWindow(h,6)
    elif action=='restore':u.ShowWindow(h,9)
    elif action=='focus':u.ShowWindow(h,9);u.SetForegroundWindow(h)
    elif action=='close':send(0x10)
    elif action=='key':
        key=int(sys.argv[3],0);send(0x100,key,1);send(0x101,key,0xc0000001)
    elif action=='text':
        for ch in sys.argv[3]:send(0x102,ord(ch))
    elif action=='click':
        x,y=map(int,sys.argv[3:5]);l=(y<<16)|x;send(0x200,0,l);time.sleep(0.12);send(0x201,1,l);time.sleep(0.12);send(0x202,0,l)
    elif action!='state':raise ValueError(action)
    client=W.RECT();window=W.RECT();u.GetClientRect(h,ctypes.byref(client));u.GetWindowRect(h,ctypes.byref(window))
    print(json.dumps(dict(pid=pid,hwnd=h,action=action,dpi=u.GetDpiForWindow(h),client=[client.right,client.bottom],window=[window.left,window.top,window.right,window.bottom],minimized=bool(u.IsIconic(h)),foreground=u.GetForegroundWindow()==h)))

if __name__=='__main__':main()
