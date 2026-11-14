# SPDX-License-Identifier: GPL-3.0-or-later
"""Audit the full dump-derived main against its generated landing-pad bodies.

Run after setup --only lift: python -I tests/eh/audit_main.py
This is a structural check; run.py is the dump-independent execution test.
"""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/lifter'))
import recomp
from recomp_eh import parse_main_eh


def main():
    env = recomp.Env()
    env.compute_starts()
    expected = parse_main_eh(recomp.IMG, recomp.BASE, recomp.TEXT_SIZE, env)
    lift = ROOT / 'build/lift/main'
    generated = json.loads((lift / 'eh.json').read_text())
    assert generated == json.loads(json.dumps(expected)), 'metadata differs from current ELF/TSV'
    sources = sorted(lift.glob('c_*.c'))
    bodies, wrappers = {}, set()
    definitions = re.compile(r'^void ([fu])_([0-9a-f]+)\(Ctx \*c(?:, uint64_t rp)?\) \{', re.M)
    for source in sources:
        content = source.read_text()
        functions = list(definitions.finditer(content))
        for i, definition in enumerate(functions):
            end = functions[i+1].start() if i+1 < len(functions) else len(content)
            body = content[definition.start():end]
            entry = int(definition[2], 16)
            if definition[1] == 'u':
                assert entry not in bodies
                bodies[entry] = body
            elif 'nro_catch_call(c,' in body:
                assert 'void f_%x(Ctx *c) { nro_catch_call(c, u_%x); }' % (entry, entry) in body
                wrappers.add(entry)
    assert set(bodies) == set(expected['functions'])
    assert wrappers == set(expected['catchers'])
    for entry, pads in expected['functions'].items():
        body = bodies[entry]
        switch = body.index('if (rp) switch (rp)')
        first_guest = body.index('L_%x:' % entry)
        assert switch < first_guest
        for pad in pads:
            assert 'case 0x%xULL: goto L_%x;' % (pad, pad) in body[switch:first_guest]
            assert '\nL_%x: ' % pad in body[first_guest-1:]
        labels = [int(x, 16) for x in re.findall(r'^L_([0-9a-f]+):', body, re.M)]
        assert labels == list(range(entry, env.ends[entry], 4)), 'body violates TSV extent'
    table = (lift / 'table.c').read_text().split('const MainResume resume_table_main[] = {')[1]
    actual = [(int(p, 16), int(f, 16)) for p, f in re.findall(r'\{ 0x([0-9a-f]+)ULL, u_([0-9a-f]+) \}', table)]
    assert actual == expected['pads']
    assert len(actual) == len({p for p, _ in actual})
    assert 'const int resume_count_main = %d;' % len(actual) in table
    result = {'passed': True, 'lsda_functions': len(bodies), 'unique_pads': len(actual),
              'catch_wrappers': len(wrappers), 'bucket_files': len(sources),
              'unlisted_secondary_entries': sum(1 for a in re.findall(r'void f_([0-9a-f]+)', (lift / 'funcs.h').read_text())
                                                if int(a, 16) not in env.ends),
              'elf_sha256': hashlib.sha256(recomp.IMG).hexdigest(),
              'tsv_sha256': hashlib.sha256(Path(recomp.FUNCS).read_bytes()).hexdigest()}
    out = ROOT / 'build/obj/eh-main-audit.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
