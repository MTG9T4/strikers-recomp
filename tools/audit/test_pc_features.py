"""Native PC config/input/geometry contracts and pinned FSR integrity, no game state writes."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid
import shutil
import argparse
from build_runtime import ROOT, msvc_environment

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,default=ROOT/'build/run/job16-contracts')
    args=parser.parse_args()
    # Exercise the default build's generator with no existing generated header.
    sys.path.insert(0,str(ROOT/'tools/lifter'))
    import build_exe
    manifest=build_exe.MANIFEST
    required={'runtime/gfx/pc_overlay.cpp','deps/imgui/imgui.cpp','deps/imgui/imgui_draw.cpp',
              'deps/imgui/imgui_tables.cpp','deps/imgui/imgui_widgets.cpp'}
    assert required.issubset(manifest['host_sources'])
    assert len(manifest['host_sources'])==len(set(manifest['host_sources']))
    old_gen=build_exe.HOST_GEN
    # Normal mkdir inherits the workspace runner's usable temp ACL (Python 3.13
    # TemporaryDirectory's private Windows ACL is unusable in this sandbox).
    scratch=Path(tempfile.gettempdir()).resolve()/('strikers-fsr-'+uuid.uuid4().hex)
    scratch.mkdir()
    try:
        build_exe.HOST_GEN=scratch
        build_exe.embed_fsr_sources()
        text=(Path(scratch)/'fsr_sources.h').read_text()
        for symbol,filename in [('fsr_a','ffx_a.h'),('fsr_1','ffx_fsr1.h')]:
            body=text.split(f'static const char {symbol}[] = {{\n',1)[1].split('};',1)[0]
            embedded=bytes(int(v) for v in body.replace('\n','').split(',') if v)
            assert embedded==(ROOT/'runtime/gfx/fsr'/filename).read_bytes()+b'\0'
    finally:
        build_exe.HOST_GEN=old_gen
        if scratch.resolve().parent!=Path(tempfile.gettempdir()).resolve():
            raise RuntimeError('FSR scratch cleanup escaped temp')
        shutil.rmtree(scratch)
    print('Default host manifest and clean FSR embedding: PASS')
    out=args.out.resolve();assert out.is_relative_to(ROOT/'build/run');out.mkdir(parents=True,exist_ok=True)
    env=msvc_environment();cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
    args=[str(cl),'/nologo','/MD','/Od','/std:c11','/D_CRT_SECURE_NO_WARNINGS','/DWIN32_LEAN_AND_MEAN','/I'+str(ROOT/'runtime/gfx'),str(ROOT/'tools/audit/pc_features_probe.c'),'/Fo'+str(out/'probe.obj'),'/Fe'+str(out/'probe.exe'),'/link','user32.lib']
    r=subprocess.run(args,cwd=out,env=env,capture_output=True,text=True);log=r.stdout+r.stderr
    if not r.returncode:
        r=subprocess.run([str(out/'probe.exe'),(out/'strikers.ini').as_posix()],cwd=out,env=env,capture_output=True,text=True);log+=r.stdout+r.stderr
    (out/'contracts.log').write_text(log);print(log)
    if r.returncode:return r.returncode
    manifest=json.loads((ROOT/'runtime/provenance.json').read_text())
    for file in manifest['fsr1']:
        actual=hashlib.sha256((ROOT/file['file']).read_bytes()).hexdigest()
        assert actual==file['sha256'],file['file']
    print('Pinned AMD FSR1 shader/license hashes: PASS')
    return 0

if __name__=='__main__':raise SystemExit(main())
