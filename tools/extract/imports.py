"""List what each module imports and exports, grouped by namespace, so the runtime's service work can be planned.

    .venv\\Scripts\\python -I tools/extract/imports.py [--dump build/dump/update]

Writes <dump>/imports/<module>_imports.tsv (name, demangled-ish namespace path, binding module) and prints, per module,
the imported namespaces with their counts and which module defines each import.
"""
import argparse
import collections
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
sys.path.insert(0, os.path.join(REPO, 'scripts'))
import target as T              # noqa: E402
from identify import elf_sections  # noqa: E402


def symbols(e):
    secs = elf_sections(e)
    _, off, size = secs['.dynsym']
    _, stroff, _ = secs['.dynstr']
    for i in range(size // 24):
        name_off, info, _, shndx, value, sz = struct.unpack_from('<IBBHQQ', e, off + i * 24)
        name = e[stroff + name_off:e.index(b'\0', stroff + name_off)].decode('latin1')
        if name:
            yield name, info & 0xf, shndx != 0, value, sz


def namespace(name):
    """The leading nested-name components of an Itanium-mangled name, e.g. _ZN2nn7account4InitEv -> nn::account."""
    m = re.match(r'_ZN?K?(?:St)?((?:\d+[A-Za-z_]\w*?)+)', name)
    if not name.startswith('_Z') or not m:
        return '(C) ' + name.split('_')[0] if not name.startswith('_Z') else '(other)'
    parts, s = [], name[2:].lstrip('NK')
    while s and s[0].isdigit():
        n = int(re.match(r'\d+', s).group(0))
        s = s[len(str(n)):]
        parts.append(s[:n])
        s = s[n:]
        if len(parts) == 2:
            break
    return '::'.join(parts[:2]) if parts else '(other)'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump', default=T.DUMP)
    a = ap.parse_args()
    elfdir = os.path.join(a.dump, 'elf')
    outdir = os.path.join(a.dump, 'imports')
    os.makedirs(outdir, exist_ok=True)
    mods = {fn[:-4]: open(os.path.join(elfdir, fn), 'rb').read() for fn in sorted(os.listdir(elfdir))}
    defined = {}
    for m, e in mods.items():
        for name, kind, is_def, value, sz in symbols(e):
            if is_def:
                defined.setdefault(name, m)
    for m, e in mods.items():
        rows = [(n, namespace(n), defined.get(n, '?')) for n, kind, is_def, _, _ in symbols(e) if not is_def]
        with open(os.path.join(outdir, m + '_imports.tsv'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('name\tnamespace\tdefined_in\n')
            for r in sorted(rows):
                f.write('\t'.join(r) + '\n')
        by_ns = collections.Counter((ns, src) for _, ns, src in rows)
        print('== %s imports %d' % (m, len(rows)))
        for (ns, src), c in sorted(by_ns.items(), key=lambda x: (-x[1], x[0])):
            print('   %5d  %-40s from %s' % (c, ns, src))


if __name__ == '__main__':
    main()
