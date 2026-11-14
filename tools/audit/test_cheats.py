"""The cheat interpreter (runtime/core/cheats.c), code by code, on memory reserved at the game's address.

  python tools/audit/test_cheats.py
"""
import subprocess, sys, time
from pathlib import Path
from build_runtime import ROOT, msvc_environment


def main():
    out = ROOT / 'build/run' / time.strftime('cheats-%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    env = msvc_environment()
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    exe = out / 'cheats_probe.exe'
    with (out / 'build.log').open('w') as log:
        subprocess.run([str(cl), '/nologo', '/MD', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS', str(ROOT / 'tools/audit/test_cheats.c'), '/Fe' + str(exe)],
                       cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    sys.exit(subprocess.run([str(exe)], cwd=out).returncode)


if __name__ == '__main__':
    main()
