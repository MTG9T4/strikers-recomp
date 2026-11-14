"""Read-only movie ABI disassembly and main import call sites from the supplied ELFs."""
import sys
from pathlib import Path
import capstone

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/extract'))
from functions import Module


def main():
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.skipdata = True
    for name in ('subsdk0', 'main'):
        mod = Module(ROOT / f'build/dump/update/elf/{name}.elf')
        plt, _ = mod.plt_entries()
        symbols = {v: n for n, _, _, defined, v, _ in mod.symbols() if defined}
        labels = {**symbols, **plt}
        if len(sys.argv) > 2:
            if name != sys.argv[1]:
                continue
            start, size = int(sys.argv[2], 0), int(sys.argv[3], 0)
            for i in md.disasm(mod.d[0x1000 + start:0x1000 + start + size], start):
                target = int(i.op_str[1:], 16) if i.mnemonic in ('bl', 'b') and i.op_str.startswith('#0x') else 0
                print(f'{name}+{i.address:08x}: {i.mnemonic:8} {i.op_str:38} {labels.get(target, "")}')
        else:
            wanted = {p: n for p, n in plt.items() if any(s in n for s in ('SetNvn', 'movie7DecoderC', 'movie6BufferC', 'SetDataAndCapacity'))}
            for i in md.disasm(mod.d[0x1000:0x1000 + mod.text_end], 0):
                if i.mnemonic in ('bl', 'b') and i.op_str.startswith('#0x'):
                    target = int(i.op_str[1:], 16)
                    if target in wanted:
                        print(f'{name}+{i.address:08x}: {wanted[target]}')


if __name__ == '__main__':
    main()
