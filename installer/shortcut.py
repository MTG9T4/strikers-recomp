"""Create a portable .lnk through Windows' existing Shell COM API."""
import ctypes as C
from ctypes import wintypes as W
import uuid


class GUID(C.Structure):
    _fields_=[('bytes',C.c_ubyte*16)]
    def __init__(self,value):
        super().__init__()
        self.bytes[:]=uuid.UUID(value).bytes_le


def create_shortcut(path,target,directory,icon=None):
    # IShellLinkW and IPersistFile: no PowerShell, WScript or registry changes.
    ole=C.OleDLL('ole32')
    ole.CoInitialize.argtypes=[C.c_void_p]
    ole.CoCreateInstance.argtypes=[C.POINTER(GUID),C.c_void_p,W.DWORD,C.POINTER(GUID),C.POINTER(C.c_void_p)]
    ole.CoInitialize(None)
    link=C.c_void_p(); persist=C.c_void_p()
    def method(obj,index,restype,*args):
        table=C.cast(obj,C.POINTER(C.POINTER(C.c_void_p))).contents
        return C.WINFUNCTYPE(restype,C.c_void_p,*args)(table[index])
    def check(result):
        if result<0: raise OSError(f'Windows shortcut operation failed: 0x{result & 0xffffffff:08x}')
    try:
        cls=GUID('00021401-0000-0000-C000-000000000046')
        iid=GUID('000214F9-0000-0000-C000-000000000046')
        check(ole.CoCreateInstance(C.byref(cls),None,1,C.byref(iid),C.byref(link)))
        check(method(link,20,C.c_long,W.LPCWSTR)(link,str(target))) # SetPath
        check(method(link,9,C.c_long,W.LPCWSTR)(link,str(directory))) # SetWorkingDirectory
        if icon:check(method(link,17,C.c_long,W.LPCWSTR,C.c_int)(link,str(icon),0)) # SetIconLocation
        iid=GUID('0000010B-0000-0000-C000-000000000046')
        check(method(link,0,C.c_long,C.POINTER(GUID),C.POINTER(C.c_void_p))(link,C.byref(iid),C.byref(persist)))
        check(method(persist,6,C.c_long,W.LPCWSTR,W.BOOL)(persist,str(path),True))
    finally:
        if persist: method(persist,2,W.ULONG)(persist)
        if link: method(link,2,W.ULONG)(link)
        ole.CoUninitialize()
