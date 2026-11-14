#!/usr/bin/env python3
"""One entry point: from the game files you extracted from your own Switch to a runnable native build of Mario Strikers:
Battle League.

    .venv\\Scripts\\python -I scripts/setup.py --exefs <ExeFS folder> --romfs <RomFS folder> [--jobs N]

Adapted from smo-recomp's scripts/setup.py (GPL-3.0-or-later). The game is Mario Strikers: Battle League 1.3.2
(program 010019401051C000 with update v393216): the ExeFS of that update and the RomFS with it applied, extracted on
your own console (installer/game_files.py checks them). No package, key or firmware is read. Nothing of the game is
stored in the repository: every step derives its files into <repo>/build (ignored by git). Steps run in this order and are resumable: a step whose output
exists is skipped (--redo <step> repeats it, --from <step> repeats it and everything after it); compile, tables and
link always run, and only rebuild what changed.

    deps       check Python packages, clang 9, SDL3, Vulkan headers, glslang
    dump       your folders -> build/dump/update/{exefs,romfs}                          (installer/game_files.py)
    identify   modules -> ELF, build ids, SDK version, control.nacp                     (scripts/identify.py)
    functions  every function of each module, with exact boundaries                    (tools/extract/functions.py)
    imports    what each module imports, by namespace                                   (tools/extract/imports.py)
    lift       AArch64 -> C for every function -> build/lift/main                       (tools/lifter/recomp.py)
    compile    the lifted C -> objects (clang 9, incremental)                           (tools/lifter/build_main.py)
    tables     generated headers and tables                                             (tools/lifter/gen_tables.py)
    sdk        the system-library functions that are lifted too                          (tools/lifter/sdklift.py)
    link       host runtime + game library -> build/bin/strikers_native.exe             (tools/lifter/build_exe.py)
    check      differential test against Unicorn and link check (only with --only check or --check; minutes)

Other commands:  --status (what is done)   --steps (list)   --only <step>
"""
import argparse
import importlib
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

BUILD = T.BUILD
PY = sys.executable
DEPS = os.path.join(REPO, 'deps')


def tool(*parts):
    return os.path.join(REPO, 'tools', *parts)


def exists(*parts):
    return os.path.exists(os.path.join(BUILD, *parts))


def check_deps(a):
    ok = True
    for mod, pip in (('lz4', 'lz4'), ('capstone', 'capstone'), ('unicorn', 'unicorn'), ('numpy', 'numpy'),
                     ('PIL', 'Pillow'), ('Crypto', 'pycryptodome')):
        try:
            importlib.import_module(mod)
        except ImportError:
            print('  missing Python package %s (pip install -r requirements.txt)' % pip)
            ok = False
    llvm = os.environ.get('LLVM_ROOT', os.path.join(DEPS, 'llvm'))
    for what, path in (('clang 9 (LLVM_ROOT or deps/llvm)', os.path.join(llvm, 'bin', 'clang.exe')),
                       ('SDL3 (deps/SDL3)', os.path.join(DEPS, 'SDL3', 'include', 'SDL3', 'SDL.h')),
                       ('Vulkan headers (deps/Vulkan-Headers)', os.path.join(DEPS, 'Vulkan-Headers', 'include', 'vulkan', 'vulkan.h')),
                       ('glslang (deps/glslang)', os.path.join(DEPS, 'glslang', 'include', 'glslang', 'Include', 'glslang_c_interface.h'))):
        found = os.path.exists(path)
        print('  %-40s %s' % (what, path if found else 'NOT FOUND (python -I scripts/fetch_deps.py)'))
        ok &= found
    for label, p in (('--exefs', a.exefs), ('--romfs', a.romfs)):
        if p and not os.path.isdir(p):
            print('  %s %s is not a folder' % (label, p))
            ok = False
    return ok


def dump_commands(a):
    if a.exefs and a.romfs:
        return [[PY, '-I', os.path.join(REPO, 'installer', 'game_files.py'), 'copy', a.exefs, a.romfs, T.DUMP]]
    raise SystemExit('this step reads the game files you extracted: give --exefs <ExeFS folder> --romfs <RomFS folder>')


def identify_commands(a):
    command = [PY, '-I', os.path.join(REPO, 'scripts', 'identify.py')]
    return [command]


# name, what it makes (for --status; in the build folder), done test (None: always runs), commands
STEPS = [
    ('deps', 'tools found', None, None),
    ('dump', 'dump/update/{exefs,romfs}', lambda: exists('dump', 'update', 'exefs', 'main') and exists('dump', 'update', 'romfs'),
     dump_commands),
    ('identify', 'dump/update/elf, identity.json', lambda: os.path.exists(T.elf('main')) and exists('dump', 'update', 'identity.json'),
     identify_commands),
    ('functions', 'functions', lambda: exists('functions', 'main_functions.tsv'),
     lambda a: [[PY, '-I', tool('extract', 'functions.py')]]),
    ('imports', 'dump/update/imports', lambda: exists('dump', 'update', 'imports', 'main_imports.tsv'),
     lambda a: [[PY, '-I', tool('extract', 'imports.py')]]),
    ('lift', 'lift/main', lambda: exists('lift', 'main', 'table.c'),
     lambda a: [[PY, '-I', tool('lifter', 'recomp.py'), 'full']]),
    ('compile', 'obj/main', None, lambda a: [[PY, '-I', tool('lifter', 'build_main.py'), str(a.jobs)]]),
    ('tables', 'gen', None, lambda a: [[PY, '-I', tool('lifter', 'gen_tables.py')]]),
    ('sdk', 'gen/sdklift_gen.c', lambda: exists('gen', 'sdklift_report.txt'),
     lambda a: [[PY, '-I', tool('lifter', 'sdklift.py'), 'gen']]),
    ('link', 'bin/strikers_native.exe', None, lambda a: [[PY, '-I', tool('lifter', 'build_exe.py')]]),
    ('check', 'difftest + linkcheck', None,
     lambda a: [[PY, '-I', tool('lifter', 'difftest.py'), 'all'], [PY, '-I', tool('lifter', 'linkcheck.py')]]),
]
NAMES = [s[0] for s in STEPS]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--exefs', help='the ExeFS folder of update v393216 (1.3.2), extracted from your Switch')
    ap.add_argument('--romfs', help='the RomFS folder with that update applied, extracted from your Switch')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument('--status', action='store_true')
    ap.add_argument('--steps', action='store_true')
    ap.add_argument('--only', choices=NAMES)
    ap.add_argument('--redo', choices=NAMES)
    ap.add_argument('--from', dest='from_step', choices=NAMES)
    ap.add_argument('--check', action='store_true', help='also run the check step at the end')
    a = ap.parse_args()

    if a.steps:
        print('\n'.join(NAMES))
        return
    if a.status:
        print('Mario Strikers: Battle League %s: %s' % (T.dump_version() or '(not dumped yet)', os.path.relpath(BUILD, REPO)))
        for name, made, done, _ in STEPS:
            state = 'runs every time (incremental)' if done is None else ('done' if done() else 'to do')
            print('  %-10s %-32s %s' % (name, made, state if name not in ('deps', 'check') else 'on demand'))
        return

    todo = [a.only] if a.only else [n for n in NAMES if n != 'check' or a.check]
    first = NAMES.index(a.from_step) if a.from_step else None
    for name, made, done, cmds in STEPS:
        if name not in todo:
            continue
        forced = a.only == name or a.redo == name or (first is not None and NAMES.index(name) >= first)
        if name == 'deps':
            print('[deps]')
            if not check_deps(a):
                raise SystemExit('some tools are missing (README.md, Toolchain)')
            continue
        if done is not None and done() and not forced:
            print('[%s] done (%s)' % (name, made))
            continue
        for cmd in cmds(a):
            print('[%s] %s' % (name, ' '.join(os.path.relpath(c, REPO) if c.startswith(REPO) else c for c in cmd[2:])), flush=True)
            r = subprocess.run(cmd, cwd=REPO)
            if r.returncode != 0:
                raise SystemExit('[%s] failed (exit code %d); fix the cause and run setup again, it resumes here' % (name, r.returncode))
    if 'link' in todo:
        print('\nBuilt: %s' % os.path.relpath(T.build('bin', 'strikers_native.exe'), REPO))


if __name__ == '__main__':
    main()
