"""Build/run the Job 08 shader parser regressions with MSVC AddressSanitizer.

Optional arguments are project-local MXD1 compute snapshots from STRIKERS_SHADERDUMP_ALL.
Compiler products live in the system temp directory; evidence goes to build/run.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import uuid

from build_runtime import ROOT, msvc_environment


def main():
    env = msvc_environment()
    bindir = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64'
    base = Path(tempfile.gettempdir()).resolve()
    tmp = base / ('strikers-gfx-contracts-' + uuid.uuid4().hex)
    tmp.mkdir()
    try:
        shutil.copy2(bindir / 'clang_rt.asan_dynamic-x86_64.dll', tmp)
        cmd = [str(bindir / 'cl.exe'), '/nologo', '/MD', '/Od', '/Zi', '/fsanitize=address',
               '/std:c11', '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
               '/I' + str(ROOT / 'runtime/gfx'),
               str(ROOT / 'tools/audit/job08_gfx_contracts.c'), str(ROOT / 'runtime/gfx/mx_translate.c'),
               '/Fe' + str(tmp / 'gfx_contracts.exe'), '/link', '/INCREMENTAL:NO']
        r = subprocess.run(cmd, env=env, cwd=tmp, capture_output=True, text=True)
        log = r.stdout + r.stderr
        if not r.returncode:
            r = subprocess.run([str(tmp / 'gfx_contracts.exe'), *(str(Path(p).resolve()) for p in sys.argv[1:])],
                               cwd=tmp, env=env, capture_output=True, text=True)
            log += r.stdout + r.stderr
        (ROOT / 'build/run/job08-gfx-contracts.log').write_text(log)
        print(log)
        return r.returncode
    finally:
        if tmp.resolve().parent != base:
            raise RuntimeError('scratch cleanup escaped temp directory')
        shutil.rmtree(tmp)


if __name__ == '__main__':
    raise SystemExit(main())
