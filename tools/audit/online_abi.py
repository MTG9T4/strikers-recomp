"""Capture-free online ABI inventory from the user's ELF; never opens a socket.

Outputs stay outside build/dump. Addresses are module-relative, not runtime hooks.
The constant tracker is deliberately local and conservative: its references are
navigation aids; printed instructions are the evidence, not inferred C types.
"""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

import capstone

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/extract'))
from functions import Module


def inventory():
    m = Module(ROOT / 'build/dump/update/elf/main.elf')
    rel = {o: a for o, i, a in m.relocs() if i & 0xffffffff == 0x403}
    ranges = sorted(m.fdes())
    starts = [a for a, b in ranges]
    def owner(pc):
        i = bisect.bisect_right(starts, pc) - 1
        return starts[i] if i >= 0 and pc < ranges[i][1] else pc
    strings = {}
    for s in re.finditer(rb'[\x20-\x7e]{5,}', m.d):
        name = s.group().decode()
        delta = 0
        # RTTI names may immediately follow printable bytes in compact unwind
        # data. Their pointers target N2..., not the regex run's first byte.
        marker = re.search(r'N2nn3(?:nex|pia)', name)
        if marker:
            delta = marker.start();name = name[delta:]
        if any(w in name.lower() for w in ('nintendo.net', 'n2nn3nex', 'n2nn3pia',
                                          'nexmatch', 'nexconnect', 'nexnat', 'pki',
                                          'certificate', 'piahelper', 'prudp', 'nex-', 'sec-websocket')):
            strings[s.start() + delta - 4096] = name
    # RTTI: name-pointer -> typeinfo -> vtable typeinfo slot -> address point.
    tables = {}
    for name_addr, name in strings.items():
        if not name.startswith(('N2nn3nex', 'N2nn3pia')):
            continue
        for slot, target in rel.items():
            if target != name_addr:
                continue
            ti = slot - 8
            for vslot, vtarget in rel.items():
                if vtarget != ti:
                    continue
                funcs = []
                p = vslot + 8
                while p in rel and 0 < rel[p] < m.text_end and rel[p] % 4 == 0:
                    funcs.append(rel[p]); p += 8
                if funcs:
                    tables[vslot + 8] = {'name': name, 'rtti': ti, 'functions': funcs}
    targets = dict(strings)
    targets.update({a: t['name'] + ' [vtable]' for a, t in tables.items()})
    # GOT references usually name vtable header, with add #16 at the use.
    targets.update({a - 16: t['name'] + ' [vtable header]' for a, t in tables.items()})
    plt, _ = m.plt_entries()
    wanted_imports = {a: n for a, n in plt.items() if n.startswith('nnssl') or any(x in n for x in
        ('nn7account', 'nn4nifm', 'nn3ssl', 'nn6socket', 'nn7friends'))}
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.skipdata = True
    calls, refs, imports = [], [], []
    regs = {}
    history = []
    for pc, size, op, args in md.disasm_lite(m.d[4096:4096 + m.text_end], 0):
        line = f'{pc:08x}: {op:8} {args}'
        history.append(line); history = history[-14:]
        parts = args.split(', ')
        dst = parts[0].replace('w', 'x', 1)
        value = None
        if op in ('adrp', 'adr') and len(parts) == 2:
            value = int(parts[1].lstrip('#'), 0)
        elif op in ('add', 'mov'):
            if op == 'mov' and len(parts) == 2:
                value = int(parts[1][1:], 0) if parts[1].startswith('#') else regs.get(parts[1].replace('w', 'x', 1))
            elif op == 'add' and len(parts) == 3 and parts[2].startswith('#'):
                base = regs.get(parts[1].replace('w', 'x', 1))
                if base is not None:
                    value = base + int(parts[2][1:], 0)
        elif op == 'ldr':
            mm = re.fullmatch(r'\w+, \[(x\d+)(?:, #(0x[0-9a-f]+|\d+))?\]', args)
            if mm and mm[1] in regs:
                slot = regs[mm[1]] + int(mm[2] or '0', 0)
                value = rel.get(slot)
        if value is not None:
            regs[dst] = value
            if value in targets:
                refs.append({'pc': pc, 'function': owner(pc), 'target': value,
                             'name': targets[value], 'instructions': history.copy()})
        elif op not in ('str', 'strb', 'strh', 'stp', 'cmp', 'tst', 'cbz', 'cbnz', 'tbz', 'tbnz'):
            regs.pop(dst, None)
        if op in ('bl', 'b') and args.startswith('#0x'):
            target = int(args[1:], 16)
            call = {'pc': pc, 'function': owner(pc), 'target': target}
            calls.append(call)
            if target in wanted_imports:
                imports.append(dict(call, name=wanted_imports[target], instructions=history.copy()))
        if op in ('bl', 'blr', 'b', 'br', 'ret') or op.startswith('b.'):
            regs.clear()
    return {'elf_sha256': hashlib.sha256(m.d).hexdigest(),
            'address_unit': 'main module-relative byte offset', 'strings': strings,
            'vtables': tables, 'references': refs, 'calls': calls, 'imports': imports}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/run/job17-online-abi.json')
    args = ap.parse_args()
    if not args.out.resolve().is_relative_to(ROOT / 'build') or args.out.resolve().is_relative_to(ROOT / 'build/dump'):
        ap.error('output must be under build, outside build/dump')
    result = inventory()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(result["references"])} references, {len(result["vtables"])} vtables, '
          f'{len(result["imports"])} import call sites -> {args.out}')


if __name__ == '__main__':
    main()
