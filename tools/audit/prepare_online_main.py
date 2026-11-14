"""Add original NEX/libcurl callbacks to an isolated main DLL's lift closure.

The function extractor's heuristic ranges swallowed callbacks following terminal
branches. Online pair probes reached +a8b24, +b0b10, +9b9dc and +9fcac. Discover
ADRP+ADD materialised code pointers in that middleware region, split only its
heuristic ranges, and lift original instructions. No hooks or patched words.
The ordinary generated main sources, objects and strikers_game.dll stay intact.
"""
import bisect
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def prepare():
    sys.path.insert(0, str(ROOT / 'tools/lifter'))
    sys.path.insert(0, str(ROOT / 'tools/extract'))
    import recomp as R
    import build_exe as build
    from functions import Module, sext
    out = ROOT / 'build/online-main';out.mkdir(exist_ok=True)
    mod = Module(ROOT / 'build/dump/update/elf/main.elf')
    existing=ROOT / 'build/bin/strikers_job17_game.dll'
    inputs=[Path(__file__),ROOT / 'build/lift/main/table.c',*[p for p in (ROOT / 'build/obj/main').glob('*.obj')],
            *[Path(build.rel(p)) for p in build.MANIFEST['game_dll_sources']+build.MANIFEST['game_dll_includes']]]
    if existing.exists() and (out / 'roots.json').exists() and existing.stat().st_mtime>=max(p.stat().st_mtime for p in inputs):
        evidence=json.loads((out / 'roots.json').read_text())
        if evidence['elf_sha256']==hashlib.sha256(mod.d).hexdigest() and evidence['dll_sha256']==hashlib.sha256(existing.read_bytes()).hexdigest():
            print('Online main: validated existing isolated callback DLL');return existing.name
    original = (ROOT / 'build/lift/main/table.c').read_text()
    match = re.search(r'const FnEntry fn_table\[\] = \{(.*?)\};\nconst int fn_table_count = (\d+);', original, re.S)
    if not match:
        raise RuntimeError('main table layout changed')
    entries = {int(a, 16): b for a, b in re.findall(r'\{ 0x([0-9a-f]+)ULL, (f_[0-9a-f]+) \}', match[1])}
    if len(entries) != int(match[2]):
        raise RuntimeError('main table count mismatch')
    env = R.Env();env.compute_starts();env.imports = set();env.full = True
    original_starts = env.starts.copy()
    rows = {}
    for line in (ROOT / 'build/functions/main_functions.tsv').read_text().splitlines()[1:]:
        cols = line.split('\t');rows[R.BASE + int(cols[0], 16)] = cols
    targets = {}
    words = mod.words()
    for i, w in enumerate(words[:-1]):
        pc, add = i*4, words[i+1]
        if w & 0x9f000000 != 0x90000000 or add & 0xffc00000 != 0x91000000 or not (w & 31 == add & 31 == (add >> 5) & 31):
            continue
        page = (pc & ~4095) + (sext(((w >> 5 & 0x7ffff) << 2) | (w >> 29 & 3), 21) << 12)
        target = page + (add >> 10 & 4095)
        if 0x90000 <= target < 0xb1000 and target % 4 == 0 and R.BASE + target not in entries:
            targets.setdefault(R.BASE + target, []).append(pc)
    # Exact FDE ranges are not split by this audit. Only the middleware's
    # heuristic assembly ranges containing the observed callbacks qualify.
    for entry in targets:
        owner = original_starts[bisect.bisect_right(original_starts, entry)-1]
        if 'E' in rows[owner][3] or not entry < env.ends[owner]:
            raise RuntimeError(f'candidate {entry:x} is not inside a heuristic range; inspect before lifting')
    all_starts = sorted(set(original_starts) | set(targets))
    for entry in targets:
        owner = original_starts[bisect.bisect_right(original_starts, entry)-1]
        env.ends[entry] = min(env.ends[owner], all_starts[bisect.bisect_right(all_starts, entry)])
    env.starts = all_starts
    chunks, evidence = [], []
    for entry, refs in sorted(targets.items()):
        code, count, bad = R.lift_function(env, entry)
        if bad:
            raise RuntimeError(f'callback {entry:x} has untranslated instructions')
        chunks.append(code);entries[entry] = f'f_{entry:x}'
        start, end = entry-R.BASE, env.ends[entry]-R.BASE
        evidence.append({'entry': hex(start), 'end': hex(end), 'materialisation_sites': [hex(v) for v in refs],
                         'instructions': count, 'sha256': hashlib.sha256(mod.d[4096+start:4096+end]).hexdigest()})
    missing = set(env.wanted) - set(entries)
    if missing:
        raise RuntimeError('callback direct-call closure missing: ' + ','.join(hex(v) for v in sorted(missing)))
    callback = out / 'callbacks.c'
    callback.write_text('#include "runtime.h"\n#include "funcs.h"\n' + '\n'.join(f'void f_{a:x}(Ctx *c);' for a in targets) + '\n' + '\n'.join(chunks) + '\n')
    table = out / 'table.c'
    replacement = ''.join(f'void f_{entry:x}(Ctx *c);\n' for entry in targets) + 'const FnEntry fn_table[] = {\n'
    replacement += ''.join(f'  {{ 0x{a:x}ULL, {fn} }},\n' for a, fn in sorted(entries.items()))
    replacement += f'}};\nconst int fn_table_count = {len(entries)};'
    table.write_text(original[:match.start()] + replacement + original[match.end():])
    flags = ['-c', '-O2', '-w', '-fno-strict-aliasing', '-ffp-contract=off',
             '-I', str(ROOT / 'runtime/core'), '-I', str(ROOT / 'build/lift/main')]
    for src in (callback, table):
        r = subprocess.run([build.CLANG, *flags, str(src), '-o', str(src.with_suffix('.obj'))], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stdout+r.stderr)
    objs = sorted(p for p in (ROOT / 'build/obj/main').glob('*.obj') if p.name != 'table.obj')
    objs += [callback.with_suffix('.obj'), table.with_suffix('.obj')]
    rsp = out / 'game.rsp';rsp.write_text('\n'.join('"'+p.as_posix()+'"' for p in objs))
    dll = ROOT / 'build/bin/strikers_job17_game.dll'
    cmd = [build.CLANG, '-shared', '-O1', '-w', *build.T.runtime_includes(), '-I', build.LIFT, '-I', build.GEN,
           *[build.rel(s) for s in build.MANIFEST['game_dll_sources']], str(ROOT / 'build/lift/main/eh_resume.c'),
           '@'+str(rsp), '-o', str(dll)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (out / 'link.log').write_text(r.stdout+r.stderr)
    if r.returncode:
        raise RuntimeError('isolated main DLL link failed; inspect build/online-main/link.log')
    (out / 'roots.json').write_text(json.dumps({'elf_sha256': hashlib.sha256(mod.d).hexdigest(), 'callbacks': evidence,
        'dll_sha256': hashlib.sha256(dll.read_bytes()).hexdigest()}, indent=2)+'\n')
    print(f'Online main: {len(targets)} original materialised callbacks, {sum(r["instructions"] for r in evidence)} instructions, zero untranslated; isolated {dll.name}')
    return dll.name


if __name__ == '__main__':
    prepare()
