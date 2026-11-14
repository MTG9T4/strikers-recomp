"""Compile and execute the production decoder ownership/back-pressure regression."""
import ctypes
from pathlib import Path
import subprocess
from build_runtime import ROOT, msvc_environment

def main():
    env = msvc_environment()
    scratch = ROOT / 'build/runtime-check-job11'
    scratch.mkdir(exist_ok=True)
    # Retain the compiled regression artifact with the build evidence.
    dll = scratch / 'queue.dll'
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    cmd = [str(cl), '/nologo', '/LD', '/MD', '/O2', '/Gy', '/std:c11',
           '/DWIN32_LEAN_AND_MEAN', '/D_CRT_SECURE_NO_WARNINGS']
    cmd += ['/I' + str(ROOT / p) for p in ('runtime/core', 'runtime/services', 'runtime/gfx', 'build/gen')]
    cmd += [str(ROOT / p) for p in ('tools/audit/job11_movie_queue.c', 'runtime/gfx/nvn_movie.c')]
    cmd += ['/link', '/OPT:REF', '/OUT:' + str(dll), 'mfplat.lib', 'mfreadwrite.lib', 'mfuuid.lib', 'ole32.lib', 'shlwapi.lib']
    subprocess.run(cmd, cwd=scratch, env=env, check=True)
    loaded = ctypes.CDLL(str(dll))
    try:
        result = loaded.movie_queue_test()
    finally:
        ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(loaded._handle))
    assert result == 0, f'decoder back-pressure regression: {result:#x}'
    print('PASS video/audio output ownership, PTS order, release progress, stop/start')

if __name__ == '__main__':
    main()
