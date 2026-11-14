"""Read-only thread priority/CPU census for an owned Job 21c process."""
import ctypes as C
from ctypes import wintypes as W


def snapshot(pid):
    k=C.WinDLL('kernel32',use_last_error=True)
    nt=C.WinDLL('ntdll')
    class Thread(C.Structure):
        _fields_=[(n,W.DWORD) for n in ('size','usage','tid','pid')]+[('base',W.LONG),('delta',W.LONG),('flags',W.DWORD)]
    class Module(C.Structure):
        _fields_=[(n,W.DWORD) for n in ('size','id','pid','global_usage','process_usage')]+[('base',C.c_void_p),('bytes',W.DWORD),('module',W.HMODULE),('name',W.WCHAR*256),('path',W.WCHAR*260)]
    k.CreateToolhelp32Snapshot.argtypes=[W.DWORD,W.DWORD];k.CreateToolhelp32Snapshot.restype=W.HANDLE
    k.CloseHandle.argtypes=[W.HANDLE]
    k.Thread32First.argtypes=[W.HANDLE,C.POINTER(Thread)];k.Thread32Next.argtypes=k.Thread32First.argtypes
    k.Module32FirstW.argtypes=[W.HANDLE,C.POINTER(Module)];k.Module32NextW.argtypes=k.Module32FirstW.argtypes
    k.OpenThread.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenThread.restype=W.HANDLE
    k.GetThreadPriority.argtypes=[W.HANDLE];k.GetThreadPriority.restype=C.c_int
    k.GetThreadTimes.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p]
    k.GetThreadDescription.argtypes=[W.HANDLE,C.POINTER(C.c_void_p)]
    k.LocalFree.argtypes=[C.c_void_p]
    nt.NtQueryInformationThread.argtypes=[W.HANDLE,C.c_int,C.c_void_p,W.ULONG,C.c_void_p]
    modules=[];h=k.CreateToolhelp32Snapshot(0x18,pid)
    if h!=C.c_void_p(-1).value:
        m=Module();m.size=C.sizeof(m);ok=k.Module32FirstW(h,C.byref(m))
        while ok:
            modules.append((m.base,m.base+m.bytes,m.name));ok=k.Module32NextW(h,C.byref(m))
        k.CloseHandle(h)
    h=k.CreateToolhelp32Snapshot(4,0);rows=[]
    if h==C.c_void_p(-1).value:return dict(pid=pid,error=C.get_last_error())
    try:
        t=Thread();t.size=C.sizeof(t);ok=k.Thread32First(h,C.byref(t))
        while ok:
            if t.pid==pid:
                row=dict(tid=t.tid,base_priority=t.base)
                th=k.OpenThread(0x840,False,t.tid)
                if th:
                    try:
                        row['relative_priority']=k.GetThreadPriority(th)
                        desc=C.c_void_p()
                        status=k.GetThreadDescription(th,C.byref(desc))
                        row['description_status']=status
                        if status>=0 and desc.value:
                            row['name']=C.wstring_at(desc);k.LocalFree(desc)
                        times=[C.c_uint64() for _ in range(4)]
                        if k.GetThreadTimes(th,*[C.byref(x) for x in times]):
                            row.update(kernel_seconds=times[2].value/1e7,user_seconds=times[3].value/1e7)
                        addr=C.c_void_p()
                        if nt.NtQueryInformationThread(th,9,C.byref(addr),C.sizeof(addr),None)==0:
                            row['start_module']=next((name for lo,hi,name in modules if addr.value and lo<=addr.value<hi),'unknown')
                    finally:k.CloseHandle(th)
                else:row['open_error']=C.get_last_error()
                rows.append(row)
            ok=k.Thread32Next(h,C.byref(t))
    finally:k.CloseHandle(h)
    return dict(pid=pid,threads=rows)
