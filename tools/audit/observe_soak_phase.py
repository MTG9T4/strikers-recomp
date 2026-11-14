"""Read-only matched-menu checkpoints for a running owned soak process.

Minute samples mix menus, loading and matches. These additional snapshots make
phase-dependent handle/thread/memory changes visible; they do not override the
soak's original pass thresholds or turn a failed run into a pass.
"""
import argparse
import csv
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import re
from types import SimpleNamespace
import sys
import time
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(Path(__file__).resolve().parent))
from run_player_soak import metrics


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('run',type=Path)
    a=p.parse_args();out=a.run.resolve()
    if ROOT/'build/phase8' not in out.parents:raise ValueError('Expected a project-local soak run')
    manifest=json.loads((out/'manifest.json').read_text());pid=manifest['pid']
    k=C.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
    k.QueryFullProcessImageNameW.argtypes=[W.HANDLE,W.DWORD,W.LPWSTR,C.POINTER(W.DWORD)]
    k.GetProcessTimes.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p]
    k.WaitForSingleObject.argtypes=[W.HANDLE,W.DWORD];k.CloseHandle.argtypes=[W.HANDLE]
    handle=k.OpenProcess(0x101000,False,pid) # synchronize + query limited info
    if not handle:raise C.WinError(C.get_last_error())
    try:
        name=C.create_unicode_buffer(32768);size=W.DWORD(len(name))
        if not k.QueryFullProcessImageNameW(handle,0,name,C.byref(size)):raise C.WinError(C.get_last_error())
        if Path(name.value).resolve()!=out/'bin/strikers_native.exe':raise ValueError('Owned process path mismatch')
        created,exited,kernel,user=(C.c_uint64() for _ in range(4))
        if not k.GetProcessTimes(handle,C.byref(created),C.byref(exited),C.byref(kernel),C.byref(user)):
            raise C.WinError(C.get_last_error())
        epoch=created.value/10**7-11644473600
        fields=['seconds','cycle','frame','working_set','private_bytes','handles','threads']
        seen=set();position=0;fences=[];capture=None;buffer=''
        # Do not fabricate checkpoints for captures that happened before this observer.
        initial=(out/'game.log').read_text(errors='replace')
        for frame in re.findall(r'^capture-frame: (\d+)',initial,re.M):seen.add(int(frame))
        with (out/'menu-checkpoints.csv').open('x',newline='') as dest:
            writer=csv.DictWriter(dest,fieldnames=fields);writer.writeheader();dest.flush()
            while k.WaitForSingleObject(handle,0)==0x102:
                with (out/'game.log').open('r',errors='replace') as source:
                    source.seek(position);buffer+=source.read();position=source.tell()
                lines=buffer.split('\n');buffer=lines.pop()
                for line in lines:
                    m=re.match(r'input wait: satisfied at frame (\d+);',line)
                    if m:fences.append(int(m[1]))
                    m=re.match(r'capture-frame: (\d+)',line)
                    if m:capture=int(m[1])
                    if line.startswith('capture: ') and capture is not None and capture not in seen:
                        seen.add(capture)
                        cycles=[i+1 for i,fence in enumerate(fences) if 2459<=capture-fence<=2463]
                        if cycles:
                            row=dict(seconds=round(time.time()-epoch,2),cycle=cycles[-1],frame=capture,
                                     **metrics(SimpleNamespace(pid=pid)))
                            writer.writerow(row);dest.flush();print(json.dumps(row),flush=True)
                time.sleep(.5)
    finally:k.CloseHandle(handle)


if __name__=='__main__':main()
