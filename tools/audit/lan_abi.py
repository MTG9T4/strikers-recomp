"""Read-only SDK 13.3 LDN/socket ABI and main import-call disassembly.

python tools/audit/lan_abi.py > build/run/job15-sdk-abi.txt
"""
from pathlib import Path
import sys
import capstone

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/extract'))
from functions import Module


def main():
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.skipdata = True
    sdk = Module(ROOT / 'build/dump/update/elf/sdk.elf')
    plt, _ = sdk.plt_entries()
    labels = {v: n for n, _, _, defined, v, _ in sdk.symbols() if defined}
    labels.update(plt)
    methods = ('MakeIpv4Address', 'GetIpv4Address', 'Scan', 'CreateNetwork',
               'Connect', 'GetLastErrno', 'Bind', 'Fcntl', 'Poll', 'RecvFrom', 'SetSockOpt')
    for name, _, _, defined, addr, size in sdk.symbols():
        if not defined or size > 600 or not any(f'{len(m)}{m}' in name for m in methods):
            continue
        if 'nn3ldn' not in name and 'nn6socket6detail' not in name:
            continue
        print(f'\n{name}: sdk+{addr:x}, size={size}')
        for ins in md.disasm(sdk.d[0x1000+addr:0x1000+addr+size], addr):
            target = int(ins.op_str[1:], 16) if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#0x') else 0
            print(f'{ins.address:08x}: {ins.mnemonic:8} {ins.op_str:38} {labels.get(target, "")}')
    game = Module(ROOT / 'build/dump/update/elf/main.elf')
    plt, _ = game.plt_entries()
    wanted = {p: n for p, n in plt.items() if 'nn3ldn' in n or 'nn6socket' in n or n.startswith('nnsocket')}
    print('\nMain LDN/socket imported call sites:')
    for ins in md.disasm(game.d[0x1000:0x1000+game.text_end], 0):
        if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#0x'):
            target = int(ins.op_str[1:], 16)
            if target in wanted:
                print(f'main+{ins.address:08x}: {wanted[target]}')


if __name__ == '__main__':
    main()
