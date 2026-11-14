"""Bounded Windows compiler process trees, without taskkill or global changes.

Create suspended, assign to a private kill-on-close Job Object, then resume the
primary thread. Children inherit the job from their first instruction, avoiding
both an assignment race and accidentally touching another worker's compiler.
"""
import ctypes as C
from ctypes import wintypes as W
import subprocess


class BasicLimits(C.Structure):
    _fields_=[('process_time',C.c_int64),('job_time',C.c_int64),('flags',W.DWORD),
              ('min_working_set',C.c_size_t),('max_working_set',C.c_size_t),
              ('active_processes',W.DWORD),('affinity',C.c_size_t),
              ('priority',W.DWORD),('scheduling',W.DWORD)]


class ExtendedLimits(C.Structure):
    _fields_=[('basic',BasicLimits),('io',C.c_uint64*6),('process_memory',C.c_size_t),
              ('job_memory',C.c_size_t),('peak_process',C.c_size_t),('peak_job',C.c_size_t)]


class Thread(C.Structure):
    _fields_=[('size',W.DWORD),('usage',W.DWORD),('id',W.DWORD),('owner',W.DWORD),
              ('base',W.LONG),('delta',W.LONG),('flags',W.DWORD)]


def run_compiler(command,env,cwd,timeout):
    k=C.WinDLL('kernel32',use_last_error=True)
    k.CreateJobObjectW.argtypes=[C.c_void_p,W.LPCWSTR];k.CreateJobObjectW.restype=W.HANDLE
    k.SetInformationJobObject.argtypes=[W.HANDLE,C.c_int,C.c_void_p,W.DWORD]
    k.AssignProcessToJobObject.argtypes=[W.HANDLE,W.HANDLE]
    k.CloseHandle.argtypes=[W.HANDLE]
    k.CreateToolhelp32Snapshot.argtypes=[W.DWORD,W.DWORD];k.CreateToolhelp32Snapshot.restype=W.HANDLE
    k.Thread32First.argtypes=[W.HANDLE,C.POINTER(Thread)]
    k.Thread32Next.argtypes=[W.HANDLE,C.POINTER(Thread)]
    k.OpenThread.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenThread.restype=W.HANDLE
    k.ResumeThread.argtypes=[W.HANDLE];k.ResumeThread.restype=W.DWORD
    job=k.CreateJobObjectW(None,None)
    if not job:raise C.WinError(C.get_last_error())
    p=None
    try:
        limits=ExtendedLimits();limits.basic.flags=0x2000 # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not k.SetInformationJobObject(job,9,C.byref(limits),C.sizeof(limits)):
            raise C.WinError(C.get_last_error())
        p=subprocess.Popen([str(x) for x in command],env=env,cwd=cwd,
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,errors='replace',
                           creationflags=0x4|subprocess.CREATE_NO_WINDOW) # CREATE_SUSPENDED
        if not k.AssignProcessToJobObject(job,int(p._handle)):
            raise C.WinError(C.get_last_error())
        snapshot=k.CreateToolhelp32Snapshot(4,0)
        if snapshot==C.c_void_p(-1).value:raise C.WinError(C.get_last_error())
        thread=Thread();thread.size=C.sizeof(thread);tid=None
        try:
            ok=k.Thread32First(snapshot,C.byref(thread))
            while ok:
                if thread.owner==p.pid:tid=thread.id;break
                ok=k.Thread32Next(snapshot,C.byref(thread))
        finally:k.CloseHandle(snapshot)
        if tid is None:raise RuntimeError('Suspended compiler has no primary thread')
        handle=k.OpenThread(2,False,tid) # THREAD_SUSPEND_RESUME
        if not handle:raise C.WinError(C.get_last_error())
        try:
            if k.ResumeThread(handle)==0xffffffff:raise C.WinError(C.get_last_error())
        finally:k.CloseHandle(handle)
        try:
            stdout,stderr=p.communicate(timeout=timeout)
            return p.returncode,stdout,stderr
        except subprocess.TimeoutExpired:
            k.CloseHandle(job);job=None # Terminates only this job's complete tree.
            stdout,stderr=p.communicate(timeout=30)
            return None,stdout,stderr
    finally:
        if job:k.CloseHandle(job)
        if p is not None:
            if p.poll() is None:p.kill()
            p.wait()
            if p.stdout:p.stdout.close()
            if p.stderr:p.stderr.close()
