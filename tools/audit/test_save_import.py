"""Save import (runtime/fs/save_import.c): the file check, staging, the swap at start-up, backups and cancel.

  python tools/audit/test_save_import.py [--save <a strkrs.save of this game>]

Compiles tools/audit/test_save_import.c with the production source and runs it in a fresh folder under build/run.
Without --save it uses the first save found under build/phase8 (a test copy's userdata). The save is only read.
"""
import argparse, subprocess, sys, time
from pathlib import Path
from build_runtime import ROOT, msvc_environment


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--save', type=Path)
    a = p.parse_args()
    save = a.save or next(iter(sorted((ROOT / 'build/phase8').glob('*/userdata/save/strkrs.save'))), None)
    if not save:
        sys.exit('no strkrs.save to start from: pass --save')
    out = ROOT / 'build/run' / time.strftime('save-import-%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    env = msvc_environment()
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    exe = out / 'save_import_probe.exe'
    with (out / 'build.log').open('w') as log:
        subprocess.run([str(cl), '/nologo', '/MD', '/std:c11', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS', str(ROOT / 'tools/audit/test_save_import.c'),
                        '/Fe' + str(exe), 'user32.lib', 'comdlg32.lib'], cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    work = out / 'work'
    work.mkdir()
    r = subprocess.run([str(exe), str(work), str(save)], cwd=out)
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
