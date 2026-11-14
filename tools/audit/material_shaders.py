"""Copy captures into local evidence, disassemble and translate with production code.

Usage: python tools/audit/material_shaders.py --out build/run/name capture.mxd ...
For raw .bin stages, use --stage (NVN stage number). Compiler products stay in
the worktree's build/obj/host, including in restricted Windows environments.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from build_runtime import ROOT, msvc_environment


def build_probe():
    env = msvc_environment()
    out = ROOT / 'build/obj/host/job12-probe'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'material_shader_probe.exe'
    sources = [ROOT / 'tools/audit/material_shader_probe.c', ROOT / 'runtime/gfx/mx_translate.c']
    if not exe.exists() or exe.stat().st_mtime < max(p.stat().st_mtime for p in sources + [ROOT / 'runtime/gfx/mx_translate.h']):
        cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
        subprocess.run([str(cl), '/nologo', '/MD', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
                        '/I' + str(ROOT / 'runtime/gfx'), *map(str, sources), '/Fe' + str(exe)],
                       cwd=out, env=env, check=True)
    return exe


def build_texture_probe():
    """material_texture_probe.exe (test_colour_shadow.py): tools/audit/material_texture_probe.c + runtime/gfx/tex_decode.c.
    Audit 03: job 13 built it by hand; rebuilt here whenever a source or header is newer than the executable."""
    env = msvc_environment()
    out = ROOT / 'build/obj/host/job12-probe'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'material_texture_probe.exe'
    sources = [ROOT / 'tools/audit/material_texture_probe.c', ROOT / 'runtime/gfx/tex_decode.c']
    headers = [ROOT / 'runtime/gfx/tex_decode.h', ROOT / 'runtime/gfx/nvn_front.h']
    if not exe.exists() or exe.stat().st_mtime < max(p.stat().st_mtime for p in sources + headers):
        cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
        subprocess.run([str(cl), '/nologo', '/MD', '/O2', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
                        '/I' + str(ROOT / 'runtime/gfx'), '/I' + str(ROOT / 'runtime/core'), '/I' + str(ROOT / 'build/gen'),
                        '/I' + str(ROOT / 'deps/Vulkan-Headers/include'), *map(str, sources), '/Fe' + str(exe)],
                       cwd=out, env=env, check=True)
    return exe


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--stage', type=int, default=1)
    p.add_argument('captures', type=Path, nargs='+')
    args = p.parse_args()
    out = args.out.resolve()
    out.relative_to(ROOT / 'build/run')
    out.mkdir(parents=True, exist_ok=True)
    exe = build_probe()
    records = []
    for src in args.captures:
        dst = out / src.name
        if src.resolve() != dst:
            shutil.copy2(src, dst)
        r = subprocess.run([str(exe), str(dst), str(args.stage)], capture_output=True, text=True)
        print(r.stdout, end='')
        records.append(dict(source=str(src.resolve()), copy=str(dst), sha256=hashlib.sha256(dst.read_bytes()).hexdigest(),
                            exit=r.returncode, result=r.stdout))
    (out / 'manifest.json').write_text(json.dumps(records, indent=2) + '\n')
    return int(any(r['exit'] for r in records))


if __name__ == '__main__':
    raise SystemExit(main())
