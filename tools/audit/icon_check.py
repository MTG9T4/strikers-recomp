"""Ask Windows which icon a file, shortcut or window really shows, and compare it with the icon setup made from the player's
own dump (EXPECTED: <installation>/build/game-art/icon.ico). The project ships no icon of its own."""
import ctypes
from ctypes import wintypes as w
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

ROOT=Path(__file__).resolve().parents[2]
EXPECTED=None          # set by the test once setup has run
user=ctypes.WinDLL('user32',use_last_error=True)
gdi=ctypes.WinDLL('gdi32')
shell=ctypes.WinDLL('shell32')
user.GetClassLongPtrW.restype=ctypes.c_void_p
user.GetClassLongPtrW.argtypes=[w.HWND,ctypes.c_int]
user.DrawIconEx.argtypes=[w.HDC,ctypes.c_int,ctypes.c_int,ctypes.c_void_p,ctypes.c_int,ctypes.c_int,w.UINT,w.HBRUSH,w.UINT]
user.GetDC.restype=w.HDC
user.ReleaseDC.argtypes=[w.HWND,w.HDC]
user.FindWindowW.restype=w.HWND
user.FindWindowW.argtypes=[w.LPCWSTR,w.LPCWSTR]
user.DestroyIcon.argtypes=[ctypes.c_void_p]
gdi.CreateCompatibleDC.restype=w.HDC
gdi.CreateCompatibleDC.argtypes=[w.HDC]
gdi.SelectObject.restype=ctypes.c_void_p
gdi.SelectObject.argtypes=[w.HDC,ctypes.c_void_p]
gdi.DeleteObject.argtypes=[ctypes.c_void_p]
gdi.DeleteDC.argtypes=[w.HDC]
gdi.CreateDIBSection.restype=ctypes.c_void_p
gdi.CreateDIBSection.argtypes=[w.HDC,ctypes.c_void_p,w.UINT,ctypes.POINTER(ctypes.c_void_p),w.HANDLE,w.DWORD]
ctypes.OleDLL('ole32').CoInitialize(None)  # SHGetFileInfo cannot resolve a shortcut without it
GCLP_HICON=-14
SIZE=32


class FileInfo(ctypes.Structure):
    _fields_=[('hIcon',ctypes.c_void_p),('iIcon',ctypes.c_int),('attributes',w.DWORD),
              ('display',w.WCHAR*260),('type',w.WCHAR*80)]


def render(icon):
    header=(ctypes.c_uint32*10)(40,SIZE,(-SIZE)&0xffffffff,1|(32<<16),0,0,0,0,0,0)
    bits=ctypes.c_void_p()
    screen=user.GetDC(None);dc=gdi.CreateCompatibleDC(screen)
    bitmap=gdi.CreateDIBSection(dc,header,0,ctypes.byref(bits),None,0)
    old=gdi.SelectObject(dc,bitmap)
    drawn=user.DrawIconEx(dc,0,0,icon,SIZE,SIZE,0,None,3)
    raw=ctypes.string_at(bits,SIZE*SIZE*4)
    gdi.SelectObject(dc,old);gdi.DeleteObject(bitmap);gdi.DeleteDC(dc);user.ReleaseDC(None,screen)
    if not drawn:raise OSError('DrawIconEx failed')
    return Image.frombuffer('RGBA',(SIZE,SIZE),raw,'raw','BGRA',0,1).copy()


def expected():
    icon=Image.open(EXPECTED);icon.size=(SIZE,SIZE);icon.load()
    return icon.convert('RGBA')


def difference(image,arrow=False):
    # Both on black: DrawIconEx leaves premultiplied colour, so compare what a viewer would see.
    want=expected();flat=Image.new('RGBA',want.size,(0,0,0,255))
    want=Image.alpha_composite(flat,want).convert('RGB');image=image.convert('RGB')
    if arrow:
        # Windows paints its shortcut arrow over the bottom-left corner; compare the rest.
        box=(0,0,SIZE,SIZE*5//8);want=want.crop(box);image=image.crop(box)
    # At a quarter of the size: the game's icon is detailed art, and Explorer and a DPI-aware window resample it a little
    # differently (about 15 per pixel at full size, 5 here); Windows' default icon stays near 95 here.
    want,image=(i.resize((max(1,i.width//4),max(1,i.height//4)),Image.BOX) for i in (want,image))
    return round(sum(ImageStat.Stat(ImageChops.difference(want,image)).mean)/3,2)


def report(label,icon,out,arrow=False,want=True):
    """Capture now, judge later (judge()): the expected icon may not exist yet. want: the game's icon (True) or any other."""
    if not icon:return dict(label=label,icon=False,want=want,matches=False,ok=not want)
    path=Path(out)/f'icon-{label}.png';render(icon).save(path)
    return dict(label=label,icon=True,want=want,arrow=arrow,image=str(path))


def judge(results):
    for r in results:
        if r.get('image'):
            r['difference']=difference(Image.open(r['image']),r['arrow']);r['matches']=r['difference']<12
            r['ok']=r['matches']==r['want']
    return results


def of_path(label,path,out,want=True):
    info=FileInfo()
    # SHGFI_ICON|SHGFI_LARGEICON: what Explorer shows for this file or shortcut.
    if not shell.SHGetFileInfoW(ctypes.c_wchar_p(str(path)),0,ctypes.byref(info),ctypes.sizeof(info),0x100):
        return dict(label=label,icon=False,matches=False)
    try:return report(label,info.hIcon,out,Path(path).suffix.lower()=='.lnk',want)
    finally:user.DestroyIcon(info.hIcon)


def of_window(label,hwnd,out,want=True):
    return report(label,user.GetClassLongPtrW(hwnd,GCLP_HICON),out,want=want)


def of_class(label,name,out,want=True):
    hwnd=user.FindWindowW(name,None)
    return of_window(label,hwnd,out,want) if hwnd else dict(label=label,icon=False,want=want,matches=False,ok=False,note='window not found')
