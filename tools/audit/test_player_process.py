"""Actually kill an owned compiler-like parent and child via the release helper."""
from pathlib import Path
import ctypes as C
from ctypes import wintypes as W
import json
import os
import sys
import time
import uuid
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/release'))
from windows_process import run_compiler


def main():
    out=ROOT/'build/phase8/process-tests'/uuid.uuid4().hex;out.mkdir(parents=True)
    marker=out/'child.pid'
    script=out/'parent.py'
    script.write_text('import subprocess,sys,time\nfrom pathlib import Path\n'
                      'p=subprocess.Popen([sys.executable,"-c","import time;time.sleep(600)"])\n'
                      'Path(sys.argv[1]).write_text(str(p.pid))\ntime.sleep(600)\n')
    started=time.monotonic()
    code,stdout,stderr=run_compiler([sys.executable,'-I',script,marker],dict(os.environ),out,2)
    assert code is None and time.monotonic()-started<15,'Timeout did not stop promptly'
    pid=int(marker.read_text())
    k=C.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
    k.WaitForSingleObject.argtypes=[W.HANDLE,W.DWORD];k.CloseHandle.argtypes=[W.HANDLE]
    handle=k.OpenProcess(0x100000,False,pid)
    if handle:
        try:assert k.WaitForSingleObject(handle,5000)==0,'The grandchild survived the compiler timeout'
        finally:k.CloseHandle(handle)
    else:assert C.get_last_error()==87,'Unable to verify child termination'
    (out/'result.json').write_text(json.dumps(dict(status='PASS',seconds=time.monotonic()-started,terminated_child=pid)))
    print('PASS: timed out real parent and child; private job killed both; no taskkill')
    # Normal exit/error streams also survive the ownership wrapper.
    code,stdout,stderr=run_compiler([sys.executable,'-I','-c','import sys;print("normal");sys.exit(7)'],dict(os.environ),out,10)
    assert code==7 and stdout.strip()=='normal'


if __name__=='__main__':main()
