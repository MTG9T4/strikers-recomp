"""Drive an owned game window with posted messages: no real mouse or keyboard, so the desktop stays the user's.

Used to fill in the real F1 form on the exact player host, which has no test driver.
"""
import ctypes as C
from ctypes import wintypes as W
import time

from PIL import Image

u=C.WinDLL('user32',use_last_error=True);g=C.WinDLL('gdi32')
u.SetProcessDpiAwarenessContext(C.c_void_p(-4))
WM_KEYDOWN,WM_KEYUP,WM_CHAR=0x100,0x101,0x102
WM_MOUSEMOVE,WM_LBUTTONDOWN,WM_LBUTTONUP=0x200,0x201,0x202
VK_F1=0x70
PROC=C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
u.PostMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
u.SetWindowPos.argtypes=[W.HWND,W.HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.UINT]
u.GetDpiForWindow.argtypes=[W.HWND]
u.PrintWindow.argtypes=[W.HWND,W.HDC,W.UINT]
u.GetDC.argtypes=[W.HWND];u.GetDC.restype=W.HDC
u.ReleaseDC.argtypes=[W.HWND,W.HDC]
g.CreateCompatibleDC.argtypes=[W.HDC];g.CreateCompatibleDC.restype=W.HDC
g.CreateCompatibleBitmap.argtypes=[W.HDC,C.c_int,C.c_int];g.CreateCompatibleBitmap.restype=W.HBITMAP
g.SelectObject.argtypes=[W.HDC,W.HGDIOBJ];g.SelectObject.restype=W.HGDIOBJ
g.DeleteObject.argtypes=[W.HGDIOBJ];g.DeleteDC.argtypes=[W.HDC]
g.GetDIBits.argtypes=[W.HDC,W.HBITMAP,W.UINT,W.UINT,C.c_void_p,C.c_void_p,W.UINT]


def window(pid):
    """The largest visible top-level window of an owned process, or None."""
    found=[]
    def each(h,_):
        owner=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(owner));r=W.RECT()
        if owner.value==pid and u.IsWindowVisible(h) and u.GetClientRect(h,C.byref(r)):found.append((r.right*r.bottom,h))
        return True
    u.EnumWindows(PROC(each),0)
    return max(found)[1] if found and max(found)[0]>0 else None


def pin(h,x=100,y=100):
    """Move to the primary monitor: the form's coordinates depend on the monitor's scaling."""
    u.SetWindowPos(h,None,x,y,0,0,0x15)  # SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE
    time.sleep(.3)
    r=W.RECT();u.GetClientRect(h,C.byref(r));print('window',r.right,r.bottom,'dpi',u.GetDpiForWindow(h),flush=True)


def key(h,vk):
    u.PostMessageW(h,WM_KEYDOWN,vk,1);time.sleep(.08);u.PostMessageW(h,WM_KEYUP,vk,0xC0000001)


def click(h,x,y):
    at=(y<<16)|x
    u.PostMessageW(h,WM_MOUSEMOVE,0,at);time.sleep(.25)
    u.PostMessageW(h,WM_LBUTTONDOWN,1,at);time.sleep(.2)
    u.PostMessageW(h,WM_LBUTTONUP,0,at);time.sleep(.25)


def text(h,value):
    for ch in value:
        u.PostMessageW(h,WM_CHAR,ord(ch),1);time.sleep(.05)


def shot(h,path):
    """Save the window's client area, even if another window covers it."""
    r=W.RECT();u.GetClientRect(h,C.byref(r));w,t=r.right,r.bottom
    screen=u.GetDC(None);dc=g.CreateCompatibleDC(screen);bmp=g.CreateCompatibleBitmap(screen,w,t);old=g.SelectObject(dc,bmp)
    try:
        assert u.PrintWindow(h,dc,3)
        info=(C.c_uint32*10)(40,w,(-t)&0xffffffff,0x200001,0,0,0,0,0,0)
        data=C.create_string_buffer(w*t*4)
        assert g.GetDIBits(dc,bmp,0,t,data,info,0)==t
        Image.frombuffer('RGBA',(w,t),data,'raw','BGRA',0,1).convert('RGB').save(path)
    finally:
        g.SelectObject(dc,old);g.DeleteObject(bmp);g.DeleteDC(dc);u.ReleaseDC(None,screen)
