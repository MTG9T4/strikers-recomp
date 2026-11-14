"""Texture replacement and button pictures (runtime/gfx/texrepl.c): BC4 encoding, keys, badges, the font sheet patch.

  python tools/audit/test_texrepl.py [<dumped extension font sheet .png> <patched .png out>]
"""
import subprocess, sys, time
from pathlib import Path
from build_runtime import ROOT, msvc_environment


def main():
    out = ROOT / 'build/run' / time.strftime('texrepl-%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    env = msvc_environment()
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    exe = out / 'texrepl_probe.exe'
    with (out / 'build.log').open('w') as log:
        r = subprocess.run([str(cl), '/nologo', '/MD', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(ROOT / 'runtime/gfx'),
                            str(ROOT / 'tools/audit/test_texrepl.c'), str(ROOT / 'runtime/gfx/tex_decode.c'), '/Fe' + str(exe),
                            'gdi32.lib', 'user32.lib', 'ole32.lib', 'windowscodecs.lib'],
                           cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT)
    if r.returncode:
        print((out / 'build.log').read_text()[-3000:])
        sys.exit(1)
    sys.exit(subprocess.run([str(exe)] + sys.argv[1:], cwd=out).returncode)


if __name__ == '__main__':
    main()
