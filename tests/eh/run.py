# SPDX-License-Identifier: GPL-3.0-or-later
"""Dump-independent Windows EH integration test. Run: python -I tests/eh/run.py.

Requires deps/llvm and VS 2022 Build Tools. All compiler products are temporary;
the durable summary is build/obj/eh-test.json. No runtime source is modified.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tools/audit'))
from build_runtime import msvc_environment


def elf_sections(data):
    shoff = struct.unpack_from('<Q', data, 40)[0]
    shnum, shstr = struct.unpack_from('<HH', data, 60)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + 64*i) for i in range(shnum)]
    s = sections[shstr]
    names = data[s[4]:s[4]+s[5]]
    return {names[s[0]:names.index(b'\0', s[0])].decode(): s for s in sections}


def fixture_image(linked, temp):
    """Wrap assembled bytes in the lifter's NSO-style flat ELF container.

    Keep linker-produced CFI/header/LSDA bytes intact. Add only a dynamic symbol
    and JUMP_SLOT directory for the assembly's normal AArch64 PLT stubs.
    Deliberately omit an .eh_frame_hdr section header, as in Strikers main.
    """
    data = linked.read_bytes()
    sections = elf_sections(data)
    symbols = {}
    s, strings = sections['.symtab'], sections['.strtab']
    strings = data[strings[4]:strings[4]+strings[5]]
    functions = []
    for at in range(s[4], s[4]+s[5], 24):
        n, info, _, ndx, value, size = struct.unpack_from('<IBBHQQ', data, at)
        name = strings[n:strings.index(b'\0', n)].decode()
        symbols[name] = value
        if info & 15 == 2 and ndx and size:
            functions.append((value, value+size, name))
    allocated = [s for s in sections.values() if s[2] & 2 and s[1] != 8]
    flat = bytearray(max(s[3]+s[5] for s in allocated))
    for s in allocated:
        flat[s[3]:s[3]+s[5]] = data[s[4]:s[4]+s[5]]

    def append(blob, alignment=8):
        flat.extend(b'\0' * (-len(flat) % alignment))
        start = len(flat)
        flat.extend(blob)
        return start

    names = ['__cxa_throw', '__cxa_begin_catch', '__cxa_end_catch', '_Unwind_Resume',
             'cleaned', 'caught', 'unexpected_return']
    slots = ['got_throw', 'got_begin', 'got_end', 'got_resume', 'got_cleaned', 'got_caught', 'got_unexpected']
    strs, syms = bytearray(b'\0'), bytearray(24)
    rels = bytearray()
    for i, (name, slot) in enumerate(zip(names, slots), 1):
        syms.extend(struct.pack('<IBBHQQ', len(strs), 0x12, 0, 0, 0, 0))
        strs.extend(name.encode() + b'\0')
        rels.extend(struct.pack('<QQq', symbols[slot], (i << 32) | 0x402, 0))
    symtab = append(syms)
    strtab = append(strs, 1)  # recomp's dynamic symbol count is (STRTAB-SYMTAB)/24
    jmprel = append(rels)
    dyn = b''.join(struct.pack('<qQ', t, v) for t, v in
                   [(6, symtab), (5, strtab), (7, 0), (8, 0), (23, jmprel), (2, len(rels)), (0, 0)])
    dynamic = append(dyn)
    blob = bytes(flat)
    names = b'\0.text\0.dynamic\0.dynsym\0.dynstr\0.shstrtab\0'
    shnames = append(names)
    flat.extend(b'\0' * (-len(flat) % 8))
    shoff = 0x1000 + len(flat)
    headers = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for name, kind, flags, addr, size, align, entsize in [
            ('.text', 1, 6, 0, sections['.text'][5], 4, 0),
            ('.dynamic', 6, 3, dynamic, len(dyn), 8, 16),
            ('.dynsym', 11, 2, symtab, len(syms), 8, 24),
            ('.dynstr', 3, 2, strtab, len(strs), 1, 0),
            ('.shstrtab', 3, 0, shnames, len(names), 1, 0)]:
        headers.append((names.index(name.encode()+b'\0'), kind, flags, addr, 0x1000+addr,
                        size, 0, 0, align, entsize))
    for header in headers:
        flat.extend(struct.pack('<IIQQQQIIQQ', *header))
    image = bytearray(0x1000) + flat
    image[:16] = b'\x7fELF\x02\x01\x01' + b'\0'*9
    struct.pack_into('<HHIQQQIHHHHHH', image, 16, 3, 183, 1, 0, 0, shoff, 0, 64, 0, 0, 64, len(headers), 5)
    (temp / 'fixture.elf').write_bytes(image)
    (temp / 'fixture.bin').write_bytes(blob)
    (temp / 'functions.tsv').write_text('start\tend\tsize\tsrc\tend_from\tsymbol\n' + ''.join(
        '%x\t%x\t%d\tE\texact\t%s\n' % (a, b, b-a, n) for a, b, n in sorted(functions)))
    (temp / 'fixture.h').write_text('#define TEST_CALLER 0x%xULL\n#define TEST_CLEANUP_PAD 0x%xULL\n' %
                                   (0x7100000000 + symbols['caller'], 0x7100000000 + symbols['cleanup_pad']))
    (temp / 'image.h').write_text('#define GUEST_IMAGE_SIZE 0x%xULL\n' % len(blob))
    return symbols


def parser_checks(temp, symbols):
    """Check rejection paths and the reference's terminate-only classification."""
    sys.path.insert(0, str(ROOT / 'tools/lifter'))
    from recomp_eh import Reader, parse_main_eh
    image = (temp / 'fixture.elf').read_bytes()
    text_size = elf_sections(image)['.text'][5]
    base = 0x7100000000
    ends = {base + int(t[0], 16): base + int(t[1], 16) for t in
            (line.split('\t') for line in (temp / 'functions.tsv').read_text().splitlines()[1:])}
    env = SimpleNamespace(ends=ends, plt_symbol=lambda target: None)
    original = parse_main_eh(image, base, text_size, env)
    assert len(original['catchers']) == 1 and original['terminate_records'] == 0
    checked = []

    def rejected(name, candidate, env=env):
        try:
            parse_main_eh(candidate, base, text_size, env)
        except ValueError:
            checked.append(name)
        else:
            raise AssertionError('malformed metadata accepted: ' + name)

    r = Reader(image)
    mod0 = r.unpack('<I', 4)
    mutated = bytearray(image)
    mutated[0x1000+mod0] = 0
    rejected('missing MOD0 magic', mutated)
    rejected('truncated EH image', image[:0x1000+text_size])
    shortened = dict(ends)
    shortened[base+symbols['caller']] -= 4
    rejected('FDE/TSV disagreement', image, SimpleNamespace(ends=shortened, plt_symbol=lambda target: None))
    q = symbols['caller_lsda'] + 2
    _, q = r.leb(q)  # skip type-table offset
    cs_size, cs_start = r.leb(q + 1)
    cs_end = cs_start + cs_size
    _, q = r.leb(cs_start)
    _, q = r.leb(q)
    _, next_q = r.leb(q)
    assert next_q == q + 1
    mutated = bytearray(image)
    mutated[0x1000+q] = 127
    rejected('landing pad outside owning TSV extent', mutated)
    mutated = bytearray(image)
    mutated[0x1000+cs_end+1] = 127  # next action offset -1 points back to itself
    rejected('cyclic action chain', mutated)
    hdr = mod0 + r.unpack('<i', mod0+0x10)
    mutated = bytearray(image)
    struct.pack_into('<i', mutated, 0x1000+hdr+12, 0)
    rejected('EH header/FDE start disagreement', mutated)

    # Give the first pad's callee the recognized __clang_call_terminate body.
    mutated = bytearray(image)
    helper = symbols['begin_catch']
    for i, target in enumerate((symbols['personality'], symbols['unexpected_return'])):
        struct.pack_into('<I', mutated, 0x1000+helper+4*i,
                         0x94000000 | (((target-helper-4*i)//4) & 0x3ffffff))
    term_names = {base+symbols['personality']: '__cxa_begin_catch',
                  base+symbols['unexpected_return']: '_ZSt9terminatev'}
    terminal = parse_main_eh(mutated, base, text_size, SimpleNamespace(ends=ends, plt_symbol=term_names.get))
    assert terminal['catchers'] == [] and terminal['terminate_records'] == 1
    assert terminal['pads'] == original['pads']  # terminate pads still need resume entries
    checked.append('terminate-only pad retained without catch wrapper')
    print('parser checks: %d passed' % len(checked))
    return checked


def main():
    env = msvc_environment()
    clang = ROOT / 'deps/llvm/bin/clang.exe'
    linker = ROOT / 'deps/llvm/bin/ld.lld.exe'
    compiler = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    temp = Path(tempfile.gettempdir()).resolve() / ('strikers-eh-' + uuid.uuid4().hex)
    temp.mkdir()  # inherit normal ACLs; Python 3.13 TemporaryDirectory blocks compiler access here
    commands, outputs = [], []

    def run(args, extra_env=None):
        args = [str(a) for a in args]
        commands.append(args)
        r = subprocess.run(args, cwd=temp, env=extra_env or env, capture_output=True, text=True, timeout=180)
        outputs.append(r.stdout + r.stderr)
        if r.returncode:
            raise RuntimeError('command failed (%d): %s\n%s' % (r.returncode, args, outputs[-1]))
        return r.stdout

    try:
        run([clang, '--target=aarch64-none-elf', '-c', HERE / 'fixture.s', '-o', 'fixture.o'])
        run([linker, '-T', HERE / 'fixture.ld', '--eh-frame-hdr', 'fixture.o', '-o', 'linked.elf'])
        symbols = fixture_image(temp / 'linked.elf', temp)
        checks = parser_checks(temp, symbols)
        lift_env = dict(env, RECOMP_ELF=str(temp / 'fixture.elf'), RECOMP_FUNCS=str(temp / 'functions.tsv'))
        print(run([sys.executable, '-I', ROOT / 'tools/lifter/recomp.py', 'full', temp / 'lift', '1'], lift_env))
        eh = json.loads((temp / 'lift/eh.json').read_text())
        assert len(eh['functions']) == 2 and len(eh['pads']) == 2 and len(eh['catchers']) == 1
        includes = [arg for folder in ('runtime/core', 'runtime/libc') for arg in ('-I', str(ROOT / folder))]
        dll = temp / 'fixture.dll'
        objects = []
        for source in sorted((temp / 'lift').glob('*.c')):
            obj = temp / (source.stem + '.obj')
            run([clang, '-c', '-O2', '-ffp-contract=off', '-fno-strict-aliasing', *includes,
                 '-I', temp / 'lift', source, '-o', obj])
            objects.append(obj)
        # Match build_exe's shape: all main objects PLUS the EH-generation
        # marker source, which must not duplicate table.c's definitions.
        run([clang, '-shared', '-O1', *includes, '-I', temp / 'lift', *objects,
             temp / 'lift/eh_resume.c', ROOT / 'runtime/core/game_shim_eh.c', '-o', dll])
        defines = ['/D' + a + '=(*game_' + b + '_p)' for a, b in
                   [('fn_table', 'fn_table'), ('fn_table_count', 'fn_table_count'),
                    ('dyn_syms', 'dyn_syms'), ('dyn_sym_count', 'dyn_sym_count')]]
        run([compiler, '/nologo', '/MD', '/Od', '/std:c11', '/W3', '/we4013',
             '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN', *defines,
             '/I' + str(temp), '/I' + str(ROOT / 'runtime/core'), '/I' + str(ROOT / 'runtime/libc'),
             HERE / 'harness.c', ROOT / 'runtime/libc/nnunwind.c', ROOT / 'runtime/libc/eh_resume.c',
             '/Fe:' + str(temp / 'eh-test.exe')])
        output = run([temp / 'eh-test.exe', dll, temp / 'fixture.bin'])
        print(output)
        sources = ['tools/lifter/recomp.py', 'tools/lifter/recomp_eh.py', 'runtime/libc/nncxxrt.c',
                   'runtime/libc/nnunwind.c', 'runtime/libc/eh_resume.c', 'runtime/core/game_shim_eh.c',
                   'runtime/core/game_shim.c', 'tests/eh/fixture.s', 'tests/eh/fixture.ld',
                   'tests/eh/harness.c', 'tests/eh/run.py']
        report = {'passed': True, 'eh': eh, 'parser_checks': checks, 'commands': commands, 'outputs': outputs,
                  'source_sha256': {s: hashlib.sha256((ROOT / s).read_bytes()).hexdigest() for s in sources}}
        out = ROOT / 'build/obj/eh-test.json'
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2) + '\n')
    finally:
        if temp.resolve().parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError('scratch path escaped temporary directory')
        shutil.rmtree(temp)


if __name__ == '__main__':
    main()
