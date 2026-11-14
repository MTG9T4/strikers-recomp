"""Local boot investigation helpers; all evidence is written under build/run.

python -I tools/audit/boot_probe.py disasm main 0x7d3840 0x100
python -I tools/audit/boot_probe.py run boot5 --seconds 40 --env STRIKERS_ICALLS=0,40
"""
import argparse
import os
from pathlib import Path
import subprocess
import struct
import re
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    dis = sub.add_parser('disasm')
    dis.add_argument('module')
    dis.add_argument('offset', type=lambda v: int(v, 0))
    dis.add_argument('size', type=lambda v: int(v, 0))
    probe = sub.add_parser('instrument')
    probe.add_argument('start', type=lambda v: int(v, 0))
    probe.add_argument('end', type=lambda v: int(v, 0))
    probe.add_argument('--all', action='store_true', help='every instruction, rather than call returns')
    sym = sub.add_parser('symbol')
    sym.add_argument('map')
    sym.add_argument('offsets', nargs='+', type=lambda v: int(v, 0))
    run = sub.add_parser('run')
    run.add_argument('name')
    run.add_argument('--seconds', type=int, default=40)
    run.add_argument('--env', action='append', default=[])
    run.add_argument('--asan', action='store_true', help='run the separate ASan host')
    run.add_argument('--exe', help='alternative executable basename in build/bin')
    run.add_argument('--normal', action='store_true', help='no diagnostic environment defaults or inherited STRIKERS overrides')
    args = parser.parse_args()
    if args.command == 'symbol':
        data = Path(args.map).read_text()
        base = int(re.search(r'Preferred load address is ([0-9a-fA-F]+)', data)[1], 16)
        symbols = []
        for line in data.splitlines():
            m = re.match(r'\s+[0-9a-fA-F]{4}:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s', line)
            if m:
                symbols.append((int(m[2], 16) - base, m[1]))
        for offset in args.offsets:
            start, name = max((s for s in symbols if s[0] <= offset), default=(0, '?'))
            print(f'{offset:x}: {name} +0x{offset-start:x}')
        return
    if args.command == 'instrument':
        backup_dir = Path(tempfile.gettempdir()) / 'strikers-boot-probe-originals'
        backup_dir.mkdir(exist_ok=True)
        for path in (ROOT / 'build/lift/main').glob('c_*.c'):
            original = path.read_text()
            lines = original.splitlines(keepends=True)
            changed = False
            prev_call = False
            for i, line in enumerate(lines):
                match = re.match(r'L_([0-9a-f]+): (.*)', line)
                if not match:
                    prev_call = False
                    continue
                addr = int(match[1], 16)
                if args.start <= addr < args.end and (args.all or prev_call):
                    lines[i] = (f'L_{addr:x}: printf("probe {addr:x}: x0=%llx x1=%llx x2=%llx x3=%llx x19=%llx lr=%llx\\n", '
                                'c->x[0], c->x[1], c->x[2], c->x[3], c->x[19], c->x[30]); ' + match[2] + '\n')
                    changed = True
                prev_call = 'c->x[30] = ' in match[2] and ('(c);' in match[2] or 'call_indirect' in match[2])
            if changed:
                backup = backup_dir / path.name
                if not backup.exists():
                    backup.write_text(original)
                path.write_text('extern int printf(const char *, ...);\n' + ''.join(lines))
                print(f'{path.name}: instrumented; original {backup}')
        return
    if args.command == 'disasm':
        import capstone
        with (ROOT / 'build/dump/update/elf' / (args.module + '.elf')).open('rb') as f:
            header = f.read(64)
            phoff = struct.unpack_from('<Q', header, 32)[0]
            phsize, phnum = struct.unpack_from('<HH', header, 54)
            for i in range(phnum):
                f.seek(phoff + i * phsize)
                kind, _, offset, start, _, size, _, _ = struct.unpack('<IIQQQQQQ', f.read(56))
                if kind == 1 and start <= args.offset < start + size:
                    f.seek(offset + args.offset - start)
                    code = f.read(args.size)
                    break
            else:
                raise ValueError('offset outside file-backed ELF segments')
        for insn in capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM).disasm(code, args.offset):
            print(f'{insn.address:08x}: {insn.mnemonic:8} {insn.op_str}')
        return
    env = os.environ.copy()
    if args.normal:
        env = {k: v for k, v in env.items() if not k.startswith('STRIKERS_') and k != 'ASAN_OPTIONS'}
    env.update(STRIKERS_ROMFS=(ROOT / 'build/dump/update/romfs').as_posix(),
               STRIKERS_USERDATA=(ROOT / 'build/run/userdata').as_posix())
    if not args.normal:
        env.update(STRIKERS_TRACE=f'../run/{args.name}-trace.txt', STRIKERS_CRASH_DUMP='0x100')
    for setting in args.env:
        key, value = setting.split('=', 1)
        env[key] = value
    with (ROOT / 'build/run' / (args.name + '.log')).open('wb') as log:
        exe = 'strikers_native_asan.exe' if args.asan else 'strikers_native.exe'
        if args.exe:
            if Path(args.exe).name != args.exe or not args.exe.endswith('.exe'):
                raise ValueError('--exe must be an .exe basename inside build/bin')
            exe = args.exe
        p = subprocess.Popen([str(ROOT / 'build/bin' / exe),
                              str(ROOT / 'build/dump/update/elf/main.elf'), str(args.seconds)],
                             cwd=ROOT / 'build/bin', env=env, stdout=log, stderr=subprocess.STDOUT)
        print(f'pid {p.pid}', flush=True)
        try:
            p.wait(timeout=args.seconds + 20)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            print('Host time limit exceeded; killed this probe process.', flush=True)
        print(f'exit {p.returncode}; build/run/{args.name}.log')


if __name__ == '__main__':
    main()
