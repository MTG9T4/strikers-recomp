"""Developer-only build of setup and local crash-monitor applications."""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from common import ROOT, run
sys.path.insert(0,str(ROOT/'tools/audit'))
from build_runtime import msvc_environment


def main():
    import json
    lock=json.loads((ROOT/'installer/toolchain.lock.json').read_text())
    source=(ROOT/'installer/setup_win32.cpp').read_text()
    if lock['python']['sha256'] not in source or '/ftp/python/3.13.9/python-3.13.9-embed-amd64.zip' not in source:
        raise ValueError('Native bootstrap and toolchain lock disagree')
    out=ROOT/'build/phase8/apps'
    out.mkdir(parents=True,exist_ok=True)
    env=msvc_environment()
    cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
    # The programs' own icon (resource 1), drawn by tools/release/app_icon.py: no image file is kept in the repository.
    import shutil
    from app_icon import write_ico
    write_ico(out/'app_icon.ico')
    (out/'app_icon.rc').write_text('1 ICON "app_icon.ico"\n',encoding='ascii')
    rc=shutil.which('rc.exe',path=env.get('PATH') or env.get('Path'))
    if not rc:raise ValueError('rc.exe (Windows SDK) not found')
    run([rc,'/nologo','/fo',out/'app_icon.res',out/'app_icon.rc'],env,out,out/'icon-build.log')
    run([cl,'/nologo','/utf-8','/std:c++17','/EHsc','/O2','/MT','/DUNICODE','/D_UNICODE',
         ROOT/'tools/release/crash_launcher.cpp','/Fo'+str(out/'launcher.obj'),
         '/Fe'+str(out/'StrikersRecomp.exe'),'/link',out/'app_icon.res','/SUBSYSTEM:WINDOWS','/ENTRY:wmainCRTStartup',
         '/DEPENDENTLOADFLAG:0x800',
         '/MANIFEST:EMBED',"/MANIFESTUAC:level='asInvoker' uiAccess='false'",
         'dbghelp.lib','user32.lib','gdi32.lib','gdiplus.lib','ole32.lib','uuid.lib'],env,out,out/'launcher-build.log')
    run([cl,'/nologo','/utf-8','/std:c++17','/EHsc','/O2','/MT','/DUNICODE','/D_UNICODE',
         ROOT/'installer/setup_win32.cpp','/Fo'+str(out/'setup.obj'),'/Fe'+str(out/'StrikersSetup.exe'),
         '/link',out/'app_icon.res','/SUBSYSTEM:WINDOWS','/MANIFEST:EMBED',
         '/DEPENDENTLOADFLAG:0x800',
         '/MANIFESTINPUT:'+str(ROOT/'installer/StrikersSetup.manifest'),
         "/MANIFESTUAC:level='asInvoker' uiAccess='false'",
         'user32.lib','gdi32.lib','comctl32.lib','comdlg32.lib',
         'winhttp.lib','bcrypt.lib','gdiplus.lib','shell32.lib','ole32.lib','uuid.lib'],env,out,out/'setup-build.log')
    run([cl,'/nologo','/utf-8','/std:c++17','/O2','/MT',ROOT/'tools/release/runtime_probe.cpp',
         '/Fo'+str(out/'runtime-probe.obj'),'/Fe'+str(out/'runtime-probe.exe')],env,out,out/'runtime-probe-build.log')
    from common import sha256
    from pe import PE
    import struct
    proof={}
    for name in ('StrikersRecomp.exe','StrikersSetup.exe'):
        path=out/name;pe=PE(path);imports=pe.imports()
        if any(n.lower().startswith(('vcruntime','msvcp','ucrtbase')) for n in imports):
            raise ValueError('Native app unexpectedly requires a dynamic Microsoft CRT')
        clr=struct.unpack_from('<II',pe.data,pe.optional+112+14*8)
        if any(clr):raise ValueError('Native app unexpectedly requires CLR')
        proof[name]=dict(sha256=sha256(path),imports=imports,clr=clr,static_crt=True)
    proof['source_sha256']={str(p.relative_to(ROOT)):sha256(p) for p in
        (ROOT/'installer/setup_win32.cpp',ROOT/'tools/release/crash_launcher.cpp',ROOT/'tools/release/gui_self_test.h',
         ROOT/'tools/release/app_icon.py')}
    proof['icon_sha256']=sha256(out/'app_icon.ico')
    (out/'app-proof.json').write_text(json.dumps(proof,indent=2))
    print(out)


if __name__=='__main__': main()
