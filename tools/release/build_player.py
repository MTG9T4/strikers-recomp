"""Build ONLY the locally generated game DLL, without MSVC or Windows SDK.

This command consumes an isolated pipeline tree (tools/target.py uses its root).
It does not build or distribute the host. Use --root with a fresh player tree.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import ROOT, clean_environment, run, sha256, within
from windows_process import run_compiler


# The game code compiled for the player's CPU (setup picks the level, installer/player_setup.py cpu_level). Lifted AArch64
# code uses fused multiply-add, square root and rounding all the time: for the oldest x86-64 they were calls into ucrtbase
# (fmaf, sqrtf, floorf ...), with these flags they are single instructions. Results are the same bits: FMA is exact on both
# sides, -ffp-contract=off still forbids new fusing, and -fno-math-errno only drops errno, which nothing reads.
CPU_FLAGS = {'base': [],
             'v2': ['-msse4.2', '-mpopcnt', '-mcx16'],
             'v3': ['-msse4.2', '-mpopcnt', '-mcx16', '-mavx', '-mavx2', '-mfma']}      # every AVX2 CPU (Intel Haswell, AMD Excavator, Zen and later) has FMA3


def detect_cpu():
    """The highest level this PC runs, from what Windows reports; STRIKERS_CPU_LEVEL=base|v2|v3 for tests."""
    import ctypes, os
    forced = os.environ.get('STRIKERS_CPU_LEVEL')
    if forced in CPU_FLAGS:
        return forced
    present = ctypes.windll.kernel32.IsProcessorFeaturePresent
    return 'v3' if present(40) else 'v2' if present(38) else 'base'      # PF_AVX2_..., PF_SSE4_2_INSTRUCTIONS_AVAILABLE


def build(root, llvm, output, jobs=4, link_existing=False, cpu='auto'):
    cpu = detect_cpu() if cpu == 'auto' else cpu
    root, llvm, output = Path(root).resolve(), Path(llvm).resolve(), Path(output).resolve()
    if output.exists() and any(output.iterdir()) and not link_existing:
        raise ValueError('Use a new empty output folder for a reproducible build')
    output.mkdir(parents=True, exist_ok=True)
    env = clean_environment(output, llvm)
    clang, link = llvm / 'bin/clang.exe', llvm / 'bin/lld-link.exe'
    version = run([clang, '--version'], env, output)
    if not re.search(r'clang version 9\.0\.0\b', version):
        raise ValueError('This build requires exactly clang 9.0.0')
    # -nostdinc prevents clang registry discovery from importing MSVC/SDK headers.
    resource = llvm / 'lib/clang/9.0.0/include'
    flags = ['-c', '-O2', '-w', '-Werror=implicit-function-declaration', '-ffreestanding', '-fno-strict-aliasing', '-ffp-contract=off',
             '-fno-math-errno', *CPU_FLAGS[cpu],
             '-fno-stack-protector', '-nostdinc', '-isystem', ROOT / 'tools/release/include',
             '-isystem', resource, '-I', root / 'runtime/core', '-I', root / 'build/lift/main']
    sources = sorted((root / 'build/lift/main').glob('*.c'))
    if not sources or not (root / 'build/lift/main/table.c').is_file():
        raise ValueError('The lift is incomplete; no function table was generated')
    manifest = json.loads((root / 'runtime/runtime_sources.json').read_text())
    sources += [root / p for p in manifest['game_dll_sources']]
    if not (root / 'build/lift/main/eh_resume.c').exists():
        raise ValueError('Landing-pad resume source is required; regenerate the full lift')
    sources += [ROOT / 'tools/release/player_crt.c']
    objdir = output / 'obj'
    objdir.mkdir(exist_ok=link_existing)
    started = time.monotonic()
    import hashlib
    shared_inputs=[*list((root/'runtime/core').glob('*.h')),
                   *list((ROOT/'tools/release/include').glob('*.h')),
                   root/'build/lift/main/funcs.h',clang,link]
    shared_digest=hashlib.sha256(json.dumps(dict(flags=[str(x) for x in flags],
        inputs={str(p):sha256(p) for p in shared_inputs}),sort_keys=True).encode()).hexdigest()
    # Keep the developer pipeline's bounded optimization rule, without importing
    # build_exe.py (which imports the MSVC build helper).
    sys.path.insert(0, str(root / 'tools/lifter'))
    from build_main import optnone_header

    def compile_one(item):
        i, src = item
        obj = objdir / f'{i:04}.obj'
        receipt=obj.with_suffix('.receipt.json')
        input_digest=hashlib.sha256((shared_digest+sha256(src)).encode()).hexdigest()
        if link_existing:
            # Interrupted compiler output is never trusted merely for being
            # newer. A receipt is atomically written only after successful exit.
            try:proof=json.loads(receipt.read_text()) if receipt.exists() else {}
            except (OSError,ValueError):proof={}
            if obj.exists() and proof.get('inputs')==input_digest and proof.get('object_sha256')==sha256(obj):
                return dict(source=str(src.relative_to(root)) if root in src.parents else src.name,sha256=sha256(src),object=str(obj),
                            object_sha256=sha256(obj),optimization='verified prior successful compile')
        for opt, limit in (('-O2', 20000), ('-O2', 5000), ('-O2', 1250), ('-O0', 0)):
            hdr, _, _ = optnone_header(str(src), str(obj), limit) if limit else (None, 0, 0)
            command = [clang, *flags, opt, *(['-include', hdr] if hdr else []), src, '-o', obj]
            code,stdout,stderr=run_compiler(command,env,output,180 if limit else 600)
            if code is None:continue
            (objdir / f'{i:04}.log').write_text(stdout + stderr)
            if code:
                raise RuntimeError(f'{src.name}: {stderr[-1800:]}')
            pending_receipt=receipt.with_suffix('.part')
            pending_receipt.write_text(json.dumps(dict(inputs=input_digest,object_sha256=sha256(obj))))
            pending_receipt.replace(receipt)
            return dict(source=str(src.relative_to(root)) if root in src.parents else src.name,
                        sha256=sha256(src), object=str(obj),object_sha256=sha256(obj), optimization=opt, optnone_limit=limit)
        raise RuntimeError(f'{src.name}: compiler time limit exceeded')

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        records = []
        pending=[pool.submit(compile_one,item) for item in enumerate(sources)]
        for future in concurrent.futures.as_completed(pending):
            record=future.result()
            records.append(record)
            if len(records) % 10 == 0:
                print(f'Compiled {len(records)}/{len(sources)} ({time.monotonic()-started:.0f}s)', flush=True)
                percent=60+int(29*len(records)/len(sources))
                minutes=max(1,int((time.monotonic()-started)*(len(sources)-len(records))/len(records)/60))
                print(f'@phase8 {percent}|Building game files ({len(records)}/{len(sources)}); roughly {minutes} minutes remaining (estimate)',flush=True)
        records.sort(key=lambda r:int(Path(r['object']).stem))
    imports = output / 'ucrtbase.lib'
    # LLVM 9's /lib driver cannot consume .def. Its PE linker does create an
    # import library from a .def without any objects or SDK libraries.
    run([link, '/dll', '/noentry', '/nodefaultlib', '/machine:x64',
         '/def:' + str(ROOT / 'tools/release/defs/ucrtbase.def'),
         '/out:' + str(output/'ucrt-import-stub.dll'), '/implib:' + str(imports)],
        env, output, output / 'imports.log')
    kernel = output / 'kernelbase.lib'
    run([link, '/dll', '/noentry', '/nodefaultlib', '/machine:x64',
         '/def:' + str(ROOT/'tools/release/defs/kernelbase.def'),
         '/out:' + str(output/'kernel-import-stub.dll'), '/implib:' + str(kernel)],
        env,output,output/'kernel-imports.log')
    response = output / 'link.rsp'
    dll = output / 'strikers_game.dll'
    response.write_text('\n'.join('"' + str(x) + '"' for x in [
        '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/nxcompat',
        '/out:' + str(dll), *[r['object'] for r in records], str(imports), str(kernel)]))
    run([link, '@' + str(response)], env, output, output / 'link.log')
    # An import library can name a nonexistent procedure. Check every import
    # against Windows before calling a successful link a usable player DLL.
    import ctypes
    from pe import PE
    for library, symbols in PE(dll).imports().items():
        system = ctypes.WinDLL(str(Path(env['SystemRoot'])/'System32'/library))
        for symbol in symbols:
            if symbol.startswith('#') or getattr(system,symbol,None) is None:
                raise ValueError(f'Unsupported Windows import: {library}!{symbol}')
    ctypes.CDLL(str(dll))
    proof = dict(environment=env, compiler=version, sources=records, dll_sha256=sha256(dll),
                 seconds=time.monotonic()-started, recovered_link=link_existing,
                 compile_flags=[str(x) for x in flags], compiler_sha256=sha256(clang), linker_sha256=sha256(link),
                 scope='main DLL; SDK-derived host material still excluded from release')
    proof['cpu_level'] = cpu
    (output / 'proof.json').write_text(json.dumps(proof, indent=2))
    # The launcher's check (installer/update.py wrong_cpu) reads it: a copy moved to an older PC is built again.
    (root / 'build').mkdir(exist_ok=True)
    (root / 'build/cpu-level.txt').write_text(cpu)
    print(f'Built {dll} in {proof["seconds"]:.1f}s', flush=True)
    return dll


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=ROOT)
    p.add_argument('--llvm', type=Path, default=ROOT / 'deps/llvm')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--link-existing',action='store_true',help='recover a link failure in this clean output folder')
    p.add_argument('--cpu', choices=sorted(CPU_FLAGS) + ['auto'], default='auto', help='instruction set level of the game code (auto: this PC)')
    a = p.parse_args()
    if a.jobs < 1 or a.jobs > 16:
        p.error('--jobs must be 1..16')
    within(a.root, a.output)
    build(a.root, a.llvm, a.output, a.jobs, a.link_existing, a.cpu)
