"""Verify generated SDK roots and host-call closure against the target ELF.

GPL-3.0-or-later. No namespace-based coverage credits. A root must be selected
by a current rule, installed, and present at its exact ELF address in sdk_fns.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/extract'))
from functions import Module


def inventory(root=ROOT):
    sdk = Module(str(root / 'build/dump/update/elf/sdk.elf'))
    symbols = {n: (v, size, typ) for n, typ, _, sec, v, size in sdk.symbols() if sec}
    code = (root / 'build/gen/sdklift_gen.c').read_text()
    header = (root / 'build/gen/sdklift_install.h').read_text()
    report = (root / 'build/gen/sdklift_report.txt').read_text()
    summary = re.match(r'roots (\d+), lifted (\d+) functions, (\d+) instructions, untranslated (\d+), functions with svc (\d+)', report)
    if not summary or int(summary[4]):
        raise ValueError('SDK lift has untranslated instructions or an invalid report')
    funcs = {int(a, 16) for a in re.findall(r'\{0x([0-9a-f]+)ULL, f_[0-9a-f]+, "', code)}
    if len(funcs) != int(summary[2]):
        raise ValueError('SDK function table disagrees with report')
    imports = set((root / 'build/gen/imports_all.txt').read_text().splitlines())
    roots, cuts, family, rootall = set(), set(), [], []
    for line in (root / 'tools/lifter/sdklift_rules.txt').read_text().splitlines():
        line = line.split('#')[0].strip()
        if not line:
            continue
        kind, arg = line.split(None, 1)
        if kind == 'root':
            roots.add(arg)
        elif kind == 'cut':
            cuts.add(arg)
        elif kind == 'family':
            family.append(re.compile(arg))
        elif kind == 'rootall':
            rootall.append(re.compile(arg))
    roots.update(n for n, (_, _, t) in symbols.items() if t == 2 and
                 (any(p.search(n) for p in rootall) or n in imports and any(p.search(n) for p in family)))
    roots -= cuts
    if len(roots) != int(summary[1]):
        raise ValueError('SDK roots disagree with current rules; regenerate the lift')
    for name in roots:
        if name not in symbols or symbols[name][2] != 2 or 0x7500000000 + symbols[name][0] not in funcs:
            raise ValueError(f'SDK root absent from generated function table: {name}')
    installed = set(re.findall(r'"([^"\n]+)"', header))
    if installed != roots & imports:
        raise ValueError('SDK install list disagrees with verified roots')
    functions = {n: {'file': 'build/gen/sdklift_gen.c', 'handler': f'lifted SDK +0x{symbols[n][0]:x}'} for n in installed}
    data = {n: {'file': 'runtime/core/sdklift_rt.c', 'handler': f'sdk_prepare_data SDK +0x{v:x}, {size} bytes'}
            for n, (v, size, typ) in symbols.items() if typ == 1 and size}
    host_block = code.split('const char *const sdk_host_names[] = {', 1)[1].split('};', 1)[0]
    hosts = re.findall(r'"([^"\n]+)"', host_block)
    svc_block = report.split('functions with svc:\n', 1)[1].split('untranslated:', 1)[0]
    svc = {int(a, 16) for a in re.findall(r'^  ([0-9a-f]+) ', svc_block, re.M)}
    if svc != {0x3cce58, 0x3cce88, 0x3cce90, 0x3ccea8, 0x3ccec0}:
        raise ValueError('SDK SVC closure changed; review dispatch before accepting')
    return {'functions': functions, 'data': data, 'roots': {n: symbols[n][0] for n in sorted(roots)}, 'hosts': hosts,
            'summary': summary[0], 'svc': sorted(svc)}


def check_host_closure(inv):
    from import_coverage import bindings
    bound = bindings()
    main = Module(str(ROOT / 'build/dump/update/elf/main.elf'))
    game_defined = {n for n, _, _, sec, _, _ in main.symbols() if sec}
    # Overrides are explicit SDK-internal boundaries. Expand the same literal
    # prefix convention used by ImportDef tables (H("...") in nnhid.c).
    overrides = set()
    for p in (ROOT / 'runtime').rglob('*.c'):
        text = p.read_text()
        for macro, prefix in re.findall(r'^#define (\w+)\(\w+\) "([^"\n]*)" \w+\s*$', text, re.M):
            text = re.sub(r'\b' + macro + r'\("([^"\n]*)"\)', lambda m: '"' + prefix + m[1] + '"', text)
        overrides.update(re.findall(r'sdk_host_override\("([^"\n]+)"\s*,', text))
    missing = set(inv['hosts']) - bound.keys() - game_defined - overrides
    if missing:
        raise ValueError('Unbound lifted SDK host callees: ' + ', '.join(sorted(missing)))
    return {n: 'host binding' if n in bound else 'SDK override' if n in overrides else 'main defined symbol'
            for n in inv['hosts']}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--write', action='store_true', help='write verified inventory and annotate roots in the lift report')
    args = p.parse_args()
    inv = inventory()
    inv['host_closure'] = check_host_closure(inv)
    if args.write:
        files = ['tools/lifter/sdklift_rules.txt', 'build/dump/update/elf/sdk.elf', 'build/gen/sdklift_gen.c', 'build/gen/sdklift_install.h']
        inv['sha256'] = {n: hashlib.sha256((ROOT / n).read_bytes()).hexdigest() for n in files}
        (ROOT / 'build/gen/sdklift_inventory.json').write_text(json.dumps(inv, indent=2) + '\n')
        report = ROOT / 'build/gen/sdklift_report.txt'
        text = report.read_text().split('\nverified roots (tools/audit/sdklift_inventory.py):')[0]
        text += '\nverified roots (tools/audit/sdklift_inventory.py):\n'
        text += ''.join(f'  {v:x} {n}\n' for n, v in inv['roots'].items())
        report.write_text(text)
    print(inv['summary'])
    print(f"Verified {len(inv['roots'])} roots, {len(inv['functions'])} installs, {len(inv['hosts'])} host callees, 5 dispatched SVC veneers")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
