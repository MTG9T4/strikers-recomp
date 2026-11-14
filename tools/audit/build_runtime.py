"""Compile the runtime with MSVC 2022 x64; deliberately never link any artifact.

GPL-3.0-or-later. Strikers project code. Run from any directory.
Included Vulkan .c fragments are compiled through nvn_vk.c, as upstream requires.
game_shim_eh.c includes game_shim.c; the alternative non-EH shim is also checked.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FRAGMENTS = {'nvn_vk_draw.c', 'nvn_vk_pipelines.c', 'nvn_vk_watchdog.c'}


def msvc_environment():
    installer = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer'
    vs = subprocess.check_output([str(installer / 'vswhere.exe'), '-version', '[17.0,18.0)',
                                  '-products', '*', '-requires',
                                  'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                  '-property', 'installationPath'], text=True).strip().splitlines()
    if not vs:
        raise RuntimeError('MSVC 2022 Build Tools not found')
    vcvars = Path(vs[-1]) / 'VC/Auxiliary/Build/vcvars64.bat'
    env = dict(os.environ)
    env['PATH'] = str(installer) + os.pathsep + env['PATH']
    command = f'call "{vcvars}" >nul && set'
    output = subprocess.check_output(f'cmd.exe /d /s /c "{command}"', env=env, text=True)
    env.update(line.split('=', 1) for line in output.splitlines() if '=' in line and not line.startswith('='))
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/runtime-check')
    parser.add_argument('--filter', help='compile only matching path names')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Both compilers must see exactly the same ABI declarations; fail on job 03 drift.
    cpu = (ROOT / 'runtime/core/runtime.h').read_text()
    abi = cpu[cpu.index('typedef union {'):cpu.index('/* Guest memory')]
    if abi not in (ROOT / 'runtime/core/host_cpu.h').read_text():
        raise RuntimeError('host_cpu.h ABI differs from protected runtime.h; synchronize the host view')
    env = msvc_environment()
    compiler = str(Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe')
    includes = [str(p) for p in (ROOT / 'runtime').iterdir() if p.is_dir()]
    includes += [str(ROOT / p) for p in ('build/gen', 'deps/SDL3/include',
                  'deps/Vulkan-Headers/include', 'deps/glslang/include', 'deps/imgui')]
    common = ['/nologo', '/c', '/MD', '/Od', '/W3', '/we4013', '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN']
    common += ['/I' + p for p in includes]
    sources = sorted((ROOT / 'runtime').rglob('*.c')) + sorted((ROOT / 'runtime').rglob('*.cpp'))
    sources = [p for p in sources if p.name not in FRAGMENTS and (not args.filter or args.filter in str(p))]

    def compile_one(src):
        rel = src.relative_to(ROOT).as_posix()
        flags = list(common)
        flags += ['/std:c++17'] if src.suffix == '.cpp' else ['/std:c11']
        if src.name not in ('game_shim.c', 'game_shim_eh.c'):
            flags += ['/Dfn_table=(*game_fn_table_p)', '/Dfn_table_count=(*game_fn_table_count_p)',
                      '/Ddyn_syms=(*game_dyn_syms_p)', '/Ddyn_sym_count=(*game_dyn_sym_count_p)']
        obj = out / (rel.replace('/', '_').rsplit('.', 1)[0] + '.obj')
        result = subprocess.run([compiler, *flags, '/Fo' + str(obj), str(src)], env=env,
                                cwd=ROOT, capture_output=True, text=True)
        log = out / (src.name + '.log')
        log.write_text(result.stdout + result.stderr, encoding='utf-8')
        return {'file': rel, 'exit_code': result.returncode, 'log': str(log),
                'object': str(obj), 'source_sha256': hashlib.sha256(src.read_bytes()).hexdigest(),
                'warnings': (result.stdout + result.stderr).count('warning C'),
                'command': [compiler, *flags, '/Fo' + str(obj), str(src)]}

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(compile_one, sources))
    for row in results:
        print(('PASS ' if row['exit_code'] == 0 else 'FAIL ') + row['file'])
        if row['exit_code']:
            lines = Path(row['log']).read_text().splitlines()
            print('\n'.join(line for line in lines if 'error ' in line)[:6000])
    report = {'toolset': env.get('VCToolsVersion'), 'target': env.get('VSCMD_ARG_TGT_ARCH'),
              'compiled': len(results), 'failed': sum(r['exit_code'] != 0 for r in results),
              'warnings': sum(r['warnings'] for r in results),
              'fragments': sorted(FRAGMENTS), 'linked': False, 'results': results}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'MSVC {report["toolset"]} {report["target"]}: {report["compiled"]} units, {report["failed"]} failed; no link')
    return int(report['failed'] != 0)


if __name__ == '__main__':
    raise SystemExit(main())
