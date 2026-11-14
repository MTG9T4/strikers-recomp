"""Disassemble target SDK exports to inspect ABI layouts (read-only).

GPL-3.0-or-later. Addresses are module-relative, not reference-game addresses.
"""
import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/extract'))
from functions import Module
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pattern', help='regular expression matching exported symbols')
    p.add_argument('--limit', type=int, default=400)
    args = p.parse_args()
    sdk = Module(str(ROOT / 'build/dump/update/elf/sdk.elf'))
    dis = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    for name, typ, bind, section, value, size in sdk.symbols():
        if section and re.search(args.pattern, name):
            print(f'\n{name} @{value:x} size {size:x} type {typ}')
            if typ == 2:
                for i in dis.disasm(sdk.d[0x1000 + value:0x1000 + value + min(size, args.limit * 4)], value):
                    print(f'  {i.address:x}: {i.mnemonic} {i.op_str}')
            else:
                print(sdk.d[0x1000 + value:0x1000 + value + min(size, 64)].hex())


if __name__ == '__main__':
    main()
