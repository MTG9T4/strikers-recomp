"""Exercise the actual release launcher with an unhandled native exception."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import uuid
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_runtime import ROOT, msvc_environment


def main():
    out=ROOT/'build/phase8/crash-tests'/uuid.uuid4().hex
    out.mkdir(parents=True,exist_ok=False)
    launcher=ROOT/'build/phase8/apps/StrikersRecomp.exe'
    if not launcher.exists():
        subprocess.run([sys.executable,'-I',str(ROOT/'tools/release/build_apps.py')],check=True)
    source=out/'crash.cpp'
    source.write_text('''#include <windows.h>
#include <stdio.h>
int main() { setvbuf(stdout,0,_IONBF,0); puts("phase8 crash sentinel");
/* the launcher's child must not have the debug heap (docs/audit-07.md): PEB NtGlobalFlag, offset 0xBC on x64 */
printf("NtGlobalFlag=%lx;", *(unsigned long *)((char *)__readgsqword(0x60) + 0xBC));
#ifdef HANDLED_EXCEPTION
__try { RaiseException(0xE0420043, 0, 0, 0); }
__except(EXCEPTION_EXECUTE_HANDLER) { puts("handled exception sentinel"); }
return 0;
#else
RaiseException(0xE0420042, EXCEPTION_NONCONTINUABLE, 0, 0); return 90;
#endif
}
''')
    env=msvc_environment()
    cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
    subprocess.run([str(cl),'/nologo','/MT',str(source),'/Fo'+str(out/'crash.obj'),
                    '/Fe'+str(out/'crash.exe')],cwd=out,env=env,check=True,capture_output=True)
    # A profile without any existing user data; no real game intentionally crashed.
    user=out/'userdata'
    p=subprocess.run([str(launcher),'--crash-test',
                      str(out/'crash.exe'),str(user)],cwd=out,timeout=30,capture_output=True)
    assert p.returncode!=0,'The crash monitor lost the child failure status'
    reports=list((user/'crashes').glob('*.dmp'))
    assert len(reports)==1,'Expected exactly one local minidump'
    dump=reports[0].read_bytes()
    assert dump[:4]==b'MDMP','Invalid minidump signature'
    count,directory=struct.unpack_from('<II',dump,8)
    streams=[struct.unpack_from('<III',dump,directory+i*12) for i in range(count)]
    exception=next(rva for kind,size,rva in streams if kind==6)
    assert struct.unpack_from('<I',dump,exception+8)[0]==0xE0420042,'Wrong exception stream'
    assert b'phase8 crash sentinel' in reports[0].with_suffix('.txt').read_bytes()
    assert b'NtGlobalFlag=0;' in reports[0].with_suffix('.txt').read_bytes(),'The game runs with the debug heap'
    subprocess.run([str(cl),'/nologo','/MT','/DHANDLED_EXCEPTION',str(source),
                    '/Fo'+str(out/'handled.obj'),'/Fe'+str(out/'handled.exe')],
                   cwd=out,env=env,check=True,capture_output=True)
    handled_user=out/'handled-userdata'
    handled=subprocess.run([str(launcher),'--crash-test',str(out/'handled.exe'),str(handled_user)],
                           cwd=out,timeout=30,capture_output=True)
    assert handled.returncode==0,'The monitor prevented a normal handled exception from continuing'
    assert not list(handled_user.rglob('*.dmp')),'A first-chance handled exception was reported as a crash'
    assert b'handled exception sentinel' in (handled_user/'last-game.log').read_bytes()
    print(f'PASS: real unhandled exception, minidump exception stream, local log tail; exit={p.returncode}; {reports[0]}; handled exception continued without a dump')


if __name__=='__main__':main()
