"""Include SDK zlib's relocated strategy table in the generated lift closure.

The LAN join reached sdk+3e4a70 through the SDK's ten-entry configuration_table
at caaad0 (16 bytes/entry, code pointer +8). The ordinary direct-call closure
misses its three strategies. Lift original SDK instructions, with no game hook,
host codec substitution, or edits to tools/lifter. SDK 13.3 dump specific.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def prepare():
    sys.path.insert(0, str(ROOT / 'tools/lifter'))
    import sdklift as sdk
    slots = [sdk.SDK_BASE + 0xcaaad8 + 16 * i for i in range(10)]
    targets = {sdk.CODE_SLOTS.get(slot) for slot in slots}
    expected = {sdk.SDK_BASE + a for a in (0x3e3bb0, 0x3e4510, 0x3e4a70)}
    if targets != expected:
        raise RuntimeError('SDK zlib strategy table differs; re-audit this dump before generation')
    original = sdk.Rules.data_roots
    sdk.Rules.data_roots = lambda self: sorted(set(original(self)) | targets)
    root_names = sdk.Rules.root_names
    allocators = [n for n in sdk.syms if 'ZlibAllocator' in n and sdk.syms[n][2] == 2]
    if len(allocators) != 2:
        raise RuntimeError('SDK ZlibAllocator callbacks differ; re-audit this dump')
    sdk.Rules.root_names = lambda self: sorted(set(root_names(self)) | set(allocators))
    argv = sys.argv
    try:
        sys.argv = ['sdklift.py', 'gen']
        sdk.main()
    finally:
        sys.argv = argv
        sdk.Rules.data_roots = original
        sdk.Rules.root_names = root_names
    print('LAN SDK: three relocated zlib strategies and two allocator callbacks included')


if __name__ == '__main__':
    prepare()
