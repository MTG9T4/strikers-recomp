"""Extract NEX request method constants and direct callers from online_abi.

This is a linked-client inventory, not a claim every method executes in Quick
Battle. Protocol association is based on the observed constructors/subobjects.
Inline auth/secure writers are separately enumerated and disassembled.
"""
import bisect
import json
from pathlib import Path
import re
import sys
import capstone

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/extract'))
from functions import Module

REGIONS = ((0xd888, 0x1511d, 109, 'Matchmake Extension'),
           (0x22268, 0x23421, 120, 'Matchmake Referee'),
           (0x27c0c, 0x27e15, 50, 'Match Making Extension'),
           (0x28134, 0x28135, 21, 'Match Making'),
           (0x28318, 0x28319, 50, 'Match Making Extension'),
           (0x285a8, 0x28a79, 21, 'Match Making'),
           (0xdb634, 0xdba79, 122, 'Ranking2'),
           (0xdd438, 0xdd739, 112, 'Ranking'),
           (0xfafd4, 0xfb4d5, 121, 'Subscriber'),
           (0xfd898, 0xfdea5, 110, 'Utility'),
           (0x1cbd20, 0x1cc415, 3, 'NAT Traversal'))


def main():
    data = json.loads((ROOT / 'build/run/job17-online-abi.json').read_text())
    mod = Module(ROOT / 'build/dump/update/elf/main.elf')
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    rows = []
    def callers(function):
        # Include the tail thunk immediately preceding a request builder.
        sites = [c['pc'] for c in data['calls'] if c['target'] == function]
        thunks = {c['function'] for c in data['calls'] if c['target'] == function and 0 < function-c['function'] < 64}
        sites += [c['pc'] for c in data['calls'] if c['target'] in thunks]
        return [hex(v) for v in sorted(set(sites))]
    for call in data['calls']:
        if call['target'] != 0xe150c:
            continue
        function, pc = call['function'], call['pc']
        for lo, hi, protocol, name in REGIONS:
            if lo <= function < hi:
                instructions = [f'{i.address:x}: {i.mnemonic} {i.op_str}' for i in md.disasm(mod.d[4096+pc-12:4096+pc+4], pc-12)]
                match = re.search(r'mov w1, #(0x[0-9a-f]+|\d+)', '\n'.join(instructions))
                if not match:
                    raise RuntimeError(f'method at {pc:x} is not an immediate; inspect manually')
                rows.append({'protocol': protocol, 'family': name, 'method': int(match[1], 0),
                             'builder': hex(function), 'writer_call': hex(pc), 'call_sites': callers(function),
                             'instructions': instructions})
                break
    # These request builders inline the u32 writer rather than calling e150c.
    inline = ((10, 'Authentication / Ticket Granting', 3, 0xe75fc, 0xe76e8),
              (10, 'Authentication / Ticket Granting', 6, 0xe7b40, 0xe7c88),
              (11, 'Secure Connection', 1, 0xe5144, 0xe5260),
              (11, 'Secure Connection', 2, 0xe53e4, 0xe5500),
              (11, 'Secure Connection', 4, 0xe5684, 0xe5798),
              (11, 'Secure Connection', 5, 0xe5908, 0xe5a6c),
              (11, 'Secure Connection', 7, 0xe5adc, 0xe5c5c),
              (11, 'Secure Connection', 8, 0xe5d78, 0xe5ec4))
    fdes = sorted(mod.fdes());starts = [a for a, b in fdes]
    for protocol, family, method, builder, pc in inline:
        owner = starts[bisect.bisect_right(starts, pc)-1]
        instructions = [f'{i.address:x}: {i.mnemonic} {i.op_str}' for i in md.disasm(mod.d[4096+pc:4096+pc+24], pc)]
        if not any(re.fullmatch(rf'{pc:x}: mov w\d+, #(0x{method:x}|{method})', s) for s in instructions):
            raise RuntimeError(f'inline method constant at {pc:x} differs; inspect')
        rows.append({'protocol': protocol, 'family': family, 'method': method, 'builder': hex(owner),
                     'writer_call': hex(pc), 'call_sites': callers(owner), 'instructions': instructions})
    rows.sort(key=lambda r: (r['protocol'], r['method']))
    (ROOT / 'build/run/job17-online-methods.json').write_text(json.dumps({'elf_sha256': data['elf_sha256'],
        'scope': 'linked client request builders; Quick Battle use must be established separately', 'methods': rows}, indent=2)+'\n')
    print('| Protocol | Method | Builder | Constant writer | Direct call sites (including thunk callers) |')
    print('| --- | --- | --- | --- | --- |')
    for row in rows:
        print(f'| {row["protocol"]} {row["family"]} | {row["method"]} | `+{row["builder"][2:]}` | `+{row["writer_call"][2:]}` | '+', '.join('`+'+s[2:]+'`' for s in row['call_sites'])+' |')


if __name__ == '__main__':
    main()
